// Pro AudioLab - render-pipeline integration test.
// Reproduces, outside Electron, the exact C++ chain that the integrated
// Pro-Audio.html UI runs when you press "Render" on the Render & Export page.
// Usage: node scripts/test-render-pipeline.js
'use strict';

const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');

const engine = require('../native');

let passed = 0, failed = 0;
function test(name, fn) {
  try { fn(); passed++; console.log(`  ok   ${name}`); }
  catch (err) { failed++; console.log(`  FAIL ${name}: ${err.message}`); }
}
const approx = (a, e, t, l) =>
  assert.ok(Math.abs(a - e) <= t, `${l}: expected ~${e}, got ${a}`);

// --- fixture: write a 3-second stereo WAV at 44.1 kHz via the C++ writer -----
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'pal-pipeline-'));
const srcPath = path.join(tmp, 'source.wav');
{
  const tone = engine.generateTone({
    wave: 'sine', freqHz: 440, durationSec: 3, sampleRate: 44100,
    channels: 2, amplitude: 0.7,
  });
  engine.writeWav(tone, srcPath, { bits: 16, float: false });
  engine.freeBuffer(tone);
}

// The app's current effect settings (mirrors AudioEngine.settings defaults,
// with a few effects switched on to exercise the full chain).
const appSettings = {
  bassGain: 5, bassFreq: 200,
  trebleGain: 3, trebleFreq: 3000,
  reverbMix: 0.3, reverbDecay: 2,
  eq: [3, 2, 0, 0, 0, 0, 0, 0, 2, 4],
  masterLevel: -1.5,
  ceiling: -0.5,
  limiterThreshold: -1, limiterRelease: 100,
  compThreshold: -24, compRatio: 4, compAttack: 10, compRelease: 100, compMakeup: 0,
};

// --- this is the chain from AudioRenderer.render() in src/renderer/index.html --
function renderChain(handle, sampleRate, settings, toggles = { reverb: true, comp: true, limiter: true }) {
  const scratch = [];
  const keep = (h) => { scratch.push(h); return h; };
  let h = handle;
  const borrowed = h;

  if (engine.info(h).sampleRate !== sampleRate) h = keep(engine.resample(h, sampleRate, 1));

  const freqs = [32, 64, 125, 250, 500, 1000, 2000, 4000, 8000, 16000];
  for (let i = 0; i < freqs.length; i++) {
    if (settings.eq[i]) h = keep(engine.biquad(h, { type: 'peaking', freqHz: freqs[i], q: 1, gainDb: settings.eq[i] }));
  }
  if (settings.bassGain) h = keep(engine.biquad(h, { type: 'lowshelf', freqHz: settings.bassFreq, gainDb: settings.bassGain }));
  if (settings.trebleGain) h = keep(engine.biquad(h, { type: 'highshelf', freqHz: settings.trebleFreq, gainDb: settings.trebleGain }));

  if (toggles.reverb && settings.reverbMix > 0) {
    h = keep(engine.reverb(h, {
      roomSize: Math.min(0.97, 0.3 + settings.reverbDecay * 0.13),
      damping: 0.4, wetLevel: settings.reverbMix, dryLevel: 1 - settings.reverbMix,
      width: 1, tailSec: Math.min(settings.reverbDecay, 5),
    }));
  }
  if (toggles.comp) {
    h = keep(engine.compressor(h, {
      thresholdDb: settings.compThreshold, ratio: settings.compRatio,
      attackMs: settings.compAttack, releaseMs: settings.compRelease,
      kneeDb: 6, makeupDb: settings.compMakeup, link: true,
    }));
  }
  if (settings.masterLevel) h = keep(engine.gain(h, settings.masterLevel));
  if (toggles.limiter) {
    h = keep(engine.limiter(h, { thresholdDb: settings.limiterThreshold, releaseMs: settings.limiterRelease, lookaheadMs: 2 }));
  }
  if (engine.info(h).channels === 1) h = keep(engine.setChannels(h, 2));
  h = keep(engine.clip(h, settings.ceiling));
  if (h === borrowed) h = keep(engine.gain(h, 0));

  for (const x of scratch) if (x !== h && x !== borrowed) engine.freeBuffer(x);
  return h;
}

console.log(`\nengine: ${engine.version()}\n`);

const loaded = engine.loadWav(srcPath);          // what loadOriginalBuffer() does for WAV files
const sourceHandle = loaded.handle;

test('WAV loads through the native fast path', () => {
  assert.equal(loaded.channels, 2);
  assert.equal(loaded.sampleRate, 44100);
  approx(loaded.durationSec, 3.0, 0.01, 'duration');
});

test('full app chain renders at 48 kHz / 24-bit (Render & Export defaults)', () => {
  const out = renderChain(sourceHandle, 48000, appSettings);
  const info = engine.info(out);
  assert.equal(info.sampleRate, 48000, 'target sample rate');
  assert.equal(info.channels, 2, 'stereo output');
  assert.ok(info.durationSec > 3.2, `reverb tail extends duration (${info.durationSec.toFixed(2)} s)`);
  const a = engine.analyze(out);
  assert.ok(a.peakDb <= appSettings.ceiling + 0.05,
    `ceiling respected: peak ${a.peakDb.toFixed(2)} dB <= ${appSettings.ceiling} dB`);
  assert.ok(a.rmsDb > -30, `signal energy sane (${a.rmsDb.toFixed(1)} dB RMS)`);
  // export at the UI's default 24-bit
  const p = path.join(tmp, 'mastered.wav');
  const bytes = engine.writeWav(out, p, { bits: 24, float: false });
  assert.ok(bytes > 3 * 48000 * 2 * 3, 'file size plausible for 24-bit stereo');
  const back = engine.loadWav(p);
  assert.equal(back.bits, 24);
  approx(back.durationSec, info.durationSec, 0.01, 'round-trip duration');
  engine.freeBuffer(back.handle);
  engine.freeBuffer(out);
});

test('flat chain (all effects off) preserves the source level', () => {
  const flat = {
    bassGain: 0, trebleGain: 0, reverbMix: 0, reverbDecay: 2,
    eq: [0, 0, 0, 0, 0, 0, 0, 0, 0, 0], masterLevel: 0, ceiling: -0.1,
    limiterThreshold: -1, limiterRelease: 100,
    compThreshold: -24, compRatio: 4, compAttack: 10, compRelease: 100, compMakeup: 0,
  };
  // Same chain, every stage skipped: no effects enabled, no gain staging.
  const out = renderChain(sourceHandle, 44100, flat, { reverb: false, comp: false, limiter: false });
  const a = engine.analyze(out);
  const b = engine.analyze(sourceHandle);
  approx(a.rmsDb, b.rmsDb, 0.05, 'flat chain preserves level');
  approx(a.peakDb, b.peakDb, 0.05, 'flat chain preserves peak');
  assert.equal(engine.info(out).frames, engine.info(sourceHandle).frames, 'no length change');
  engine.freeBuffer(out);
});

test('96 kHz export path works (High quality option)', () => {
  const out = renderChain(sourceHandle, 96000, appSettings);
  assert.equal(engine.info(out).sampleRate, 96000);
  engine.freeBuffer(out);
});

test('32-bit float export path works', () => {
  const out = renderChain(sourceHandle, 44100, appSettings);
  const p = path.join(tmp, 'float32.wav');
  engine.writeWav(out, p, { bits: 32, float: true });
  const back = engine.loadWav(p);
  assert.equal(back.bits, 32);
  assert.equal(back.isFloat, true);
  engine.freeBuffer(back.handle);
  engine.freeBuffer(out);
});

test('render from decoded PCM (non-WAV files) works identically', () => {
  // Simulates the mp3 path: Chromium decodes -> AudioBuffer -> fromPCM
  const info = engine.info(sourceHandle);
  const pcm = new Float32Array(engine.toPCM(sourceHandle, 'f32'));
  const h = engine.fromPCM(pcm.buffer, 'f32', info.channels, info.sampleRate);
  const out = renderChain(h, 44100, appSettings);
  const a = engine.analyze(out);
  assert.ok(a.peakDb <= appSettings.ceiling + 0.05, 'ceiling respected');
  engine.freeBuffer(h);
  engine.freeBuffer(out);
});

test('native handle bookkeeping stays clean across renders', () => {
  const before = engine.stats().liveBuffers;
  for (let i = 0; i < 3; i++) {
    const out = renderChain(sourceHandle, 48000, appSettings);
    engine.freeBuffer(out);
  }
  assert.equal(engine.stats().liveBuffers, before, 'no leaks after 3 renders');
});

engine.freeBuffer(sourceHandle);
fs.rmSync(tmp, { recursive: true, force: true });

console.log(`\n${passed} passed, ${failed} failed\n`);
if (failed) process.exit(1);
console.log('Render pipeline (UI -> C++ engine) verified end to end.');
