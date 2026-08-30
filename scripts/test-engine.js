// Pro AudioLab - C++ engine test suite (runs under plain Node, no Electron).
// Usage: npm run test:engine
'use strict';

const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');

const engine = require('../native');

let passed = 0;
let failed = 0;
const failures = [];

function test(name, fn) {
  try {
    fn();
    passed++;
    console.log(`  ok   ${name}`);
  } catch (err) {
    failed++;
    failures.push({ name, err });
    console.log(`  FAIL ${name}: ${err.message}`);
  }
}

function approx(actual, expected, tol, label) {
  const ok =
    Math.abs((Number.isNaN(actual) ? Infinity : actual) - expected) <= tol;
  assert.ok(
    ok,
    `${label || 'value'}: expected ~${expected}, got ${actual} (tol ${tol})`
  );
}

function db(lin) {
  return 20 * Math.log10(lin);
}

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'pal-engine-'));
const wavPath = (n) => path.join(tmp, n);

// Helpers -------------------------------------------------------------------
function toneHandle(opts) {
  return engine.generateTone(Object.assign({ wave: 'sine', freqHz: 440, durationSec: 1, sampleRate: 44100, channels: 1, amplitude: 0.5 }, opts));
}
function pcmView(handle, fmt) {
  const ab = engine.toPCM(handle, fmt || 'f32');
  return new Float32Array(ab);
}
function countNonZero(handle) {
  const v = pcmView(handle);
  let n = 0;
  for (let i = 0; i < v.length; i++) if (Math.abs(v[i]) > 1e-9) n++;
  return n;
}

console.log(`\nengine: ${engine.version()}\n`);

// --- basics ---------------------------------------------------------------
test('version() returns a string', () => {
  assert.equal(typeof engine.version(), 'string');
});

test('generateTone creates a correct 1 kHz stereo sine', () => {
  const h = toneHandle({ freqHz: 1000, durationSec: 1, channels: 2, amplitude: 0.5 });
  const info = engine.info(h);
  assert.equal(info.channels, 2);
  assert.equal(info.sampleRate, 44100);
  assert.equal(info.frames, 44100);
  approx(info.durationSec, 1.0, 1e-9, 'duration');
  const a = engine.analyze(h);
  approx(a.peakDb, db(0.5), 0.1, 'peak');
  approx(a.rmsDb, db(0.5 / Math.SQRT2), 0.15, 'rms');
  // stereo PCM is interleaved: v[2*i] = L[i]. 1 kHz @ 44.1k -> 11 samples = quarter period
  const v = pcmView(h);
  approx(v[0], 0, 1e-4, 'sine starts at 0');
  approx(v[22], 0.5, 0.01, 'L[11] = quarter period = +amplitude');
  approx(v[23], 0.5, 0.01, 'R[11] identical');
  engine.freeBuffer(h);
});

test('stats() tracks live buffers', () => {
  const before = engine.stats().liveBuffers;
  const h = toneHandle({ durationSec: 0.1 });
  assert.equal(engine.stats().liveBuffers, before + 1);
  assert.ok(engine.freeBuffer(h));
  assert.equal(engine.stats().liveBuffers, before);
  assert.equal(engine.freeBuffer(h), false, 'double free returns false');
});

test('invalid handle throws', () => {
  assert.throws(() => engine.info(999999), /invalid buffer handle/i);
});

test('generateTone validates arguments', () => {
  assert.throws(() => toneHandle({ freqHz: 40000 }), /freqHz/);
  assert.throws(() => toneHandle({ amplitude: 2 }), /amplitude/);
  assert.throws(() => toneHandle({ wave: 'bogus' }), /unknown wave/i);
});

// --- gain / normalize / fade / reverse / pan ------------------------------
test('gain +6 dB ~ doubles amplitude', () => {
  const h = toneHandle({ amplitude: 0.25 });
  const g = engine.gain(h, 6);
  approx(engine.analyze(g).peakDb, db(0.25 * Math.pow(10, 6 / 20)), 0.05, 'peak after +6dB');
  engine.freeBuffer(h); engine.freeBuffer(g);
});

test('gain with fade-in starts silent and ramps up', () => {
  const h = toneHandle({ wave: 'square', durationSec: 1, amplitude: 0.5 });
  const g = engine.gain(h, 0, { fadeInSec: 1, curve: 'smooth' });
  const v = pcmView(g);
  approx(Math.abs(v[0]), 0, 0.005, 'first sample ~ 0');
  // square wave has constant magnitude, so |v| traces the fade envelope
  approx(Math.abs(v[22050]), 0.5 * Math.pow(22051 / 44100, 3), 0.01, 'midpoint = cubic envelope');
  approx(Math.abs(v[44100 - 1]), 0.5, 0.01, 'end ~ full amplitude');
  const lin = engine.gain(h, 0, { fadeInSec: 1, curve: 'lin' });
  approx(Math.abs(pcmView(lin)[22050]), 0.25, 0.01, 'linear midpoint = half');
  engine.freeBuffer(h); engine.freeBuffer(g); engine.freeBuffer(lin);
});

test('normalize hits the target peak', () => {
  const h = toneHandle({ amplitude: 0.05 });
  const n = engine.normalize(h, -1);
  approx(engine.analyze(n).peakDb, -1, 0.05, 'peak');
  engine.freeBuffer(h); engine.freeBuffer(n);
});

test('normalize on silence returns silence', () => {
  const h = toneHandle({ wave: 'silence', amplitude: 0 });
  const n = engine.normalize(h, -1);
  assert.equal(countNonZero(n), 0);
  engine.freeBuffer(h); engine.freeBuffer(n);
});

test('fade in/out works over a region', () => {
  const h = toneHandle({ wave: 'square', amplitude: 0.5 });
  const f = engine.fade(h, 0, 0.5, 'in', 'lin');
  const v = pcmView(f);
  approx(Math.abs(v[0]), 0, 0.005, 'region start silent');
  approx(Math.abs(v[11025]), 0.25, 0.02, 'region midpoint half');
  approx(Math.abs(v[22500]), 0.5, 0.01, 'after region full');
  const fo = engine.fade(h, 0, 0.5, 'out', 'lin');
  approx(Math.abs(pcmView(fo)[0]), 0.5, 0.01, 'fade-out starts full');
  approx(Math.abs(pcmView(fo)[22049]), 0, 0.005, 'fade-out region end silent');
  engine.freeBuffer(h); engine.freeBuffer(f); engine.freeBuffer(fo);
});

test('reverse mirrors the samples', () => {
  const h = toneHandle({ durationSec: 0.1 });
  const r = engine.reverse(h);
  const a = pcmView(h);
  const b = pcmView(r);
  const n = a.length;
  for (let i = 0; i < n; i += 997) approx(b[i], a[n - 1 - i], 1e-6, 'reversed sample');
  engine.freeBuffer(h); engine.freeBuffer(r);
});

test('pan: mono -> stereo equal power, pan 0 = -3dB per side', () => {
  const h = toneHandle({ amplitude: 0.5 });
  const p = engine.pan(h, 0);
  const info = engine.info(p);
  assert.equal(info.channels, 2);
  const a = engine.analyze(p);
  approx(a.perChannel[0].peakDb, db(0.5) - 3.01, 0.1, 'L peak');
  approx(a.perChannel[1].peakDb, db(0.5) - 3.01, 0.1, 'R peak');
  engine.freeBuffer(h); engine.freeBuffer(p);
});

test('pan hard left silences the right channel', () => {
  const h = toneHandle({ amplitude: 0.5 });
  const p = engine.pan(h, -1);
  const a = engine.analyze(p);
  assert.strictEqual(a.perChannel[1].peakDb, -Infinity, 'R silent (-Infinity dB)');
  approx(a.perChannel[0].peakDb, db(0.5), 0.1, 'L full');
  engine.freeBuffer(h); engine.freeBuffer(p);
});

// --- EQ --------------------------------------------------------------------
test('lowpass removes highs from noise', () => {
  const h = toneHandle({ wave: 'noise', amplitude: 0.5, durationSec: 2 });
  const before = engine.analyze(h).rmsDb;
  const lp = engine.biquad(h, { type: 'lowpass', freqHz: 200, q: 0.7071 });
  const after = engine.analyze(lp).rmsDb;
  assert.ok(after < before - 10, `lowpass should cut >10 dB (before ${before}, after ${after})`);
  engine.freeBuffer(h); engine.freeBuffer(lp);
});

test('highpass kills a low sine almost completely', () => {
  const h = toneHandle({ freqHz: 100, amplitude: 0.5, durationSec: 1 });
  const hp = engine.biquad(h, { type: 'highpass', freqHz: 5000, q: 0.7071 });
  const a = engine.analyze(hp);
  assert.ok(a.peakDb < -35, `highpass should kill 100 Hz sine (peak ${a.peakDb} dB)`);
  engine.freeBuffer(h); engine.freeBuffer(hp);
});

test('peaking +12 dB at 1 kHz boosts a 1 kHz sine', () => {
  const h = toneHandle({ freqHz: 1000, amplitude: 0.1, durationSec: 2 });
  const pk = engine.biquad(h, { type: 'peaking', freqHz: 1000, q: 1, gainDb: 12 });
  const a = engine.analyze(pk);
  approx(a.peakDb, db(0.1) + 12, 1.0, 'boosted peak');
  engine.freeBuffer(h); engine.freeBuffer(pk);
});

test('biquad rejects bad type/frequency', () => {
  const h = toneHandle({ durationSec: 0.1 });
  assert.throws(() => engine.biquad(h, { type: 'wah' }), /type must be/i);
  assert.throws(() => engine.biquad(h, { type: 'lowpass', freqHz: 99999 }), /freqHz/);
  engine.freeBuffer(h);
});

// --- dynamics --------------------------------------------------------------
// Builds a mono 44.1k buffer by concatenating segments: [{wave, amp, durSec}]
function signalBuffer(segments) {
  const fs = 44100;
  const parts = [];
  for (const s of segments) {
    const n = Math.round(s.durSec * fs);
    const seg = new Float32Array(n);
    if (s.wave === 'sine') {
      for (let i = 0; i < n; i++) seg[i] = s.amp * Math.sin((2 * Math.PI * 440 * i) / fs);
    } else if (s.wave === 'noise') {
      for (let i = 0; i < n; i++) seg[i] = s.amp * (Math.random() * 2 - 1);
    } // else silence
    parts.push(seg);
  }
  const all = new Float32Array(parts.reduce((a, p) => a + p.length, 0));
  let o = 0;
  for (const p of parts) { all.set(p, o); o += p.length; }
  return engine.fromPCM(all.buffer, 'f32', 1, fs);
}

function loudQuietBuffer() {
  // 0.5 s sine at 0.9, then 0.5 s sine at 0.005 (-46 dB)
  return signalBuffer([
    { wave: 'sine', amp: 0.9, durSec: 0.5 },
    { wave: 'sine', amp: 0.005, durSec: 0.5 },
  ]);
}

test('compressor reduces the loud/quiet dynamic range', () => {
  const m = loudQuietBuffer();
  const vin = pcmView(m);
  const measure = (v) => {
    let loud = 0, quiet = 0;
    for (let i = 0; i < v.length / 2; i++) loud = Math.max(loud, Math.abs(v[i]));
    for (let i = Math.floor(v.length / 2); i < v.length; i++) quiet = Math.max(quiet, Math.abs(v[i]));
    return { loud, quiet };
  };
  const before = measure(vin);
  const c = engine.compressor(m, { thresholdDb: -20, ratio: 8, attackMs: 5, releaseMs: 200, kneeDb: 3 });
  const after = measure(pcmView(c));
  const rangeBefore = db(before.loud / before.quiet);
  const rangeAfter = db(after.loud / after.quiet);
  assert.ok(rangeAfter < rangeBefore - 8,
    `dynamics reduced: before ${rangeBefore.toFixed(1)} dB, after ${rangeAfter.toFixed(1)} dB`);
  engine.freeBuffer(m); engine.freeBuffer(c);
});

test('gate attenuates quiet passages to the floor', () => {
  const m = loudQuietBuffer();
  const g = engine.gate(m, { thresholdDb: -30, floorDb: -80, attackMs: 1, releaseMs: 50 });
  const v = pcmView(g);
  let quietPeak = 0;
  for (let i = Math.floor(v.length / 2); i < v.length; i++) quietPeak = Math.max(quietPeak, Math.abs(v[i]));
  assert.ok(quietPeak < 0.012, `quiet half gated (peak ${quietPeak})`);
  engine.freeBuffer(m); engine.freeBuffer(g);
});

// --- reverb / delay --------------------------------------------------------
test('reverb renders stereo with decay tail', () => {
  const h = toneHandle({ durationSec: 0.5, amplitude: 0.5 });
  const r = engine.reverb(h, { roomSize: 0.8, wetLevel: 0.4, dryLevel: 0.6, tailSec: 1.5 });
  const info = engine.info(r);
  assert.equal(info.channels, 2, 'reverb output is stereo');
  approx(info.durationSec, 2.0, 0.1, 'input + tail duration');
  const v = pcmView(r); // interleaved stereo
  // energy after the input ends (tail) must exist
  const tailStart = Math.floor(0.6 / (info.frames / 44100)) * 2;
  let tailEnergy = 0;
  for (let i = tailStart; i < v.length; i++) tailEnergy += v[i] * v[i];
  assert.ok(tailEnergy > 1e-4, `reverb tail has energy (${tailEnergy})`);
  engine.freeBuffer(h); engine.freeBuffer(r);
});

test('delay produces an echo at the delay time (impulse test)', () => {
  const fs = 44100;
  const data = new Float32Array(fs); // 1 s
  data[0] = 1.0;
  const h = engine.fromPCM(data.buffer, 'f32', 1, fs);
  const d = engine.delay(h, { delayMs: 250, feedback: 0.5, mix: 1.0 });
  const v = pcmView(d);
  const idx = Math.round(0.25 * fs);
  approx(v[idx], 1.0, 0.05, 'first echo at 250 ms');
  const idx2 = Math.round(0.5 * fs);
  approx(v[idx2], 0.5, 0.05, 'second echo = feedback * first');
  engine.freeBuffer(h); engine.freeBuffer(d);
});

// --- channels / resample ---------------------------------------------------
test('setChannels 1->2 duplicates, 2->1 averages', () => {
  const mono = toneHandle({ amplitude: 0.5, durationSec: 0.1 });
  const st = engine.setChannels(mono, 2);
  assert.equal(engine.info(st).channels, 2);
  const back = engine.setChannels(st, 1);
  const a = engine.analyze(back);
  approx(a.peakDb, db(0.5), 0.05, 'mono restored');
  engine.freeBuffer(mono); engine.freeBuffer(st); engine.freeBuffer(back);
});

test('resample 44.1k -> 48k preserves duration', () => {
  const h = toneHandle({ durationSec: 1 });
  const r = engine.resample(h, 48000, 1);
  const info = engine.info(r);
  assert.equal(info.sampleRate, 48000);
  approx(info.frames, 48000, 2, 'frame count');
  approx(engine.analyze(r).rmsDb, engine.analyze(h).rmsDb, 0.2, 'rms preserved');
  engine.freeBuffer(h); engine.freeBuffer(r);
});

// --- silence tools ---------------------------------------------------------
function gappyBuffer() {
  // 1 s tone + 1 s silence + 1 s tone at 44.1k mono
  return signalBuffer([
    { wave: 'sine', amp: 0.5, durSec: 1 },
    { wave: 'silence', amp: 0, durSec: 1 },
    { wave: 'sine', amp: 0.5, durSec: 1 },
  ]);
}

test('detectSilence finds the middle gap', () => {
  const m = gappyBuffer();
  const regions = engine.detectSilence(m, -45, 0.3);
  assert.equal(regions.length, 1, `one region expected, got ${JSON.stringify(regions)}`);
  approx(regions[0].startSec, 1.0, 0.05, 'gap start');
  approx(regions[0].endSec, 2.0, 0.05, 'gap end');
  engine.freeBuffer(m);
});

test('removeSilence compacts the buffer', () => {
  const m = gappyBuffer();
  const cleaned = engine.removeSilence(m, { thresholdDb: -45, minSilenceSec: 0.3, padMs: 10 });
  const info = engine.info(cleaned);
  approx(info.durationSec, 2.05, 0.2, 'duration after removing 1 s gap');
  const nonzero = countNonZero(cleaned);
  assert.ok(nonzero > 40000, `audio preserved (${nonzero} non-zero samples)`);
  engine.freeBuffer(m); engine.freeBuffer(cleaned);
});

// --- PCM bridge ------------------------------------------------------------
test('toPCM/fromPCM roundtrip (f32, s16, s32)', () => {
  const h = toneHandle({ durationSec: 0.25 });
  for (const fmt of ['f32', 's16', 's32']) {
    const back = engine.fromPCM(engine.toPCM(h, fmt), fmt, 1, 44100);
    approx(engine.analyze(back).peakDb, db(0.5), 0.01, `roundtrip ${fmt} peak`);
    engine.freeBuffer(back);
  }
  engine.freeBuffer(h);
});

test('fromPCM requires matching PCM format names', () => {
  assert.throws(() => engine.fromPCM(new Float32Array(8).buffer, 'mp3', 1, 44100), /format/);
});

// --- WAV I/O ---------------------------------------------------------------
test('WAV roundtrip 16-bit / 24-bit / float32', () => {
  const h = toneHandle({ durationSec: 0.5, channels: 2, amplitude: 0.5 });
  for (const spec of [{ bits: 16, float: false }, { bits: 24, float: false }, { bits: 32, float: true }]) {
    const p = wavPath(`t-${spec.bits}${spec.float ? 'f' : 'i'}.wav`);
    const bytes = engine.writeWav(h, p, { bits: spec.bits, float: !!spec.float });
    assert.ok(bytes > 44, `wrote ${bytes} bytes`);
    const loaded = engine.loadWav(p);
    assert.equal(loaded.channels, 2, 'channels');
    assert.equal(loaded.sampleRate, 44100, 'rate');
    assert.equal(loaded.frames, 22050, 'frames');
    assert.equal(loaded.bits, spec.bits, 'bits');
    assert.equal(loaded.isFloat, !!spec.float, 'float flag');
    approx(engine.analyze(loaded.handle).peakDb, db(0.5), 0.01, `roundtrip ${spec.bits}-bit peak`);
    engine.freeBuffer(loaded.handle);
  }
  engine.freeBuffer(h);
});

test('WAV parser tolerates extra chunks before data', () => {
  // Hand-build: RIFF/WAVE + LIST chunk + fmt + odd-sized chunk + data
  const sr = 44100, frames = 100;
  const pcm = Buffer.alloc(frames * 2);
  for (let i = 0; i < frames; i++) pcm.writeInt16LE(Math.round(Math.sin((i / frames) * 6.28) * 8000), i * 2);
  const fmt = Buffer.alloc(16);
  fmt.writeUInt16LE(1, 0); fmt.writeUInt16LE(1, 2); fmt.writeUInt32LE(sr, 4);
  fmt.writeUInt32LE(sr * 2, 8); fmt.writeUInt16LE(2, 12); fmt.writeUInt16LE(16, 14);
  const list = Buffer.from('INFOISFT Pro AudioLab test');
  const odd = Buffer.from('abc'); // 3 bytes -> padded to 4
  const chunks = [];
  const push = (id, body) => {
    const head = Buffer.alloc(8);
    head.write(id, 0, 'ascii');
    head.writeUInt32LE(body.length, 4);
    chunks.push(head, body);
    if (body.length % 2) chunks.push(Buffer.from([0]));
  };
  push('LIST', list);
  push('fmt ', fmt);
  push('odd ', odd);
  push('data', pcm);
  const body = Buffer.concat(chunks);
  const riff = Buffer.alloc(12);
  riff.write('RIFF', 0, 'ascii');
  riff.writeUInt32LE(4 + body.length, 4);
  riff.write('WAVE', 8, 'ascii');
  const p = wavPath('chunky.wav');
  fs.writeFileSync(p, Buffer.concat([riff, body]));
  const loaded = engine.loadWav(p);
  assert.equal(loaded.frames, 100, 'frames with junk chunks');
  assert.equal(loaded.sampleRate, sr, 'rate');
  engine.freeBuffer(loaded.handle);
});

test('fromPCM u8 (unsigned 8-bit) maps correctly', () => {
  // u8 data: silence=128, +0.5 = 192
  const u8 = Buffer.alloc(8);
  for (let i = 0; i < 8; i++) u8[i] = i < 4 ? 128 : 192;
  const from = engine.fromPCM(u8, 'u8', 1, 8000);
  const v = pcmView(from);
  approx(v[0], 0, 1e-6, 'u8 silence');
  approx(v[4], 0.5, 0.01, 'u8 +0.5');
  engine.freeBuffer(from);
});

test('loadWav errors are descriptive', () => {
  assert.throws(() => engine.loadWav(path.join(tmp, 'missing.wav')), /cannot open/i);
  const bad = wavPath('bad.wav');
  fs.writeFileSync(bad, Buffer.from('this is not a wav file at all..'));
  assert.throws(() => engine.loadWav(bad), /RIFF|WAV/i);
});

// --- mixer -----------------------------------------------------------------
test('mix two coherent tones at -6 dB each equals the original', () => {
  const h = toneHandle({ amplitude: 0.5, durationSec: 0.5 });
  const m = engine.mix([
    { handle: h, gainDb: -6.0206 },
    { handle: h, gainDb: -6.0206 },
  ], 44100, 1);
  approx(engine.analyze(m).peakDb, db(0.5), 0.05, 'coherent mix peak');
  engine.freeBuffer(h); engine.freeBuffer(m);
});

test('mix resamples and converts mismatched tracks', () => {
  const a = toneHandle({ durationSec: 1, sampleRate: 44100 });
  const b = toneHandle({ durationSec: 1, sampleRate: 48000, channels: 2, amplitude: 0.2 });
  const m = engine.mix([{ handle: a }, { handle: b }]);
  const info = engine.info(m);
  assert.equal(info.sampleRate, 44100, 'first track rate wins');
  assert.equal(info.channels, 2, 'widest channel count wins');
  engine.freeBuffer(a); engine.freeBuffer(b); engine.freeBuffer(m);
});

test('mix with no tracks throws', () => {
  assert.throws(() => engine.mix([]), /at least one/i);
});

// --- handle lifecycle ------------------------------------------------------
test('freeBuffer + reuse of handle fails cleanly', () => {
  const h = toneHandle({ durationSec: 0.05 });
  engine.freeBuffer(h);
  assert.throws(() => engine.analyze(h), /invalid buffer handle/i);
});

// ---------------------------------------------------------------------------
console.log(`\n${passed} passed, ${failed} failed (${passed + failed} total)\n`);
if (failed > 0) {
  for (const f of failures) console.log(`FAILED: ${f.name}\n  ${f.err.stack}\n`);
  process.exit(1);
}
console.log('All engine tests passed.');
