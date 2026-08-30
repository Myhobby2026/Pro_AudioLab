# Pro AudioLab — Native C++ Engine API

The engine is a C++17 N-API addon (`native/`). The renderer reaches it through
`window.proaudio.engine` (see `src/preload/preload.js`). All functions are
synchronous and run in-process — no IPC serialization for sample data.

## Conventions

- **Handles**: audio lives engine-side. Every buffer is referenced by an integer
  handle (`>= 1`). Every processing function returns a **new** handle and leaves
  the input untouched (undo-friendly). Call `freeBuffer(h)` when done.
- **Levels**: amplitudes are float `[-1, 1]`. dB values use FS reference;
  silence reports `-Infinity` dB.
- **Errors**: invalid arguments / files throw JS `Error` with a descriptive message.
- **PCM bridge**: `toPCM`/`fromPCM` move **interleaved** samples. Stereo
  `Float32Array` order is `[L0, R0, L1, R1, …]`.

---

## Lifecycle

| Function | Returns | Notes |
|---|---|---|
| `version()` | `string` | Engine build id |
| `stats()` | `{liveBuffers, totalSamples}` | Handle registry stats |
| `createBuffer(frames, channels, sampleRate)` | `handle` | Zero-filled buffer |
| `freeBuffer(handle)` | `boolean` | `false` if already freed |
| `info(handle)` | `{channels, sampleRate, frames, durationSec}` | |

## File I/O

| Function | Returns | Notes |
|---|---|---|
| `loadWav(path)` | `{handle, channels, sampleRate, frames, durationSec, bits, isFloat, format}` | PCM 8/16/24/32-bit & IEEE float 32/64; WAVE_FORMAT_EXTENSIBLE; RF64; extra chunks skipped; Unicode paths on Windows |
| `writeWav(handle, path, {bits=16, float=false})` | `bytes` | `bits`: 8/16/24/32 int, or 32/64 with `float:true` |

Compressed WAV encodings (MP3/AAC-in-WAV) are rejected — convert to standard WAV first.

## Raw PCM bridge

| Function | Returns | Notes |
|---|---|---|
| `toPCM(handle, format='f32')` | `ArrayBuffer` | Interleaved; formats `u8, s16, s24, s32, f32, f64` |
| `fromPCM(data, format, channels, sampleRate)` | `handle` | `data`: `ArrayBuffer` or any `TypedArray` view |

## Analysis

| Function | Returns | Notes |
|---|---|---|
| `analyze(handle)` | `{peakDb, rmsDb, durationSec, perChannel:[{peak, peakDb, rms, rmsDb, dcOffset}]}` | |
| `getPeaks(handle, buckets, channel=0)` | `Float32Array` | `[min0, max0, min1, max1, …]` — one min/max pair per bucket, ideal for waveform rendering |
| `detectSilence(handle, thresholdDb, minSilenceSec)` | `[{startSec, endSec}]` | 10 ms block resolution |

## Gain staging & utility

| Function | Notes |
|---|---|
| `gain(handle, gainDb, {fadeInSec=0, fadeOutSec=0, curve='lin'})` | `curve`: `'lin'` or `'smooth'` (cubic) |
| `normalize(handle, targetDbPeak=-0.1)` | Scales to target peak; silence passes through |
| `fade(handle, startSec, durSec, direction, curve)` | `direction`: `'in'`/`'out'`; `curve`: `'lin'`/`'smooth'` |
| `pan(handle, pan)` | `-1..1`. Mono→stereo uses equal-power; stereo uses balance law (no centre dip) |
| `reverse(handle)` | Sample-exact reversal |
| `setChannels(handle, channels)` | Mono↔stereo↔N (mix down / duplicate / grouped) |
| `resample(handle, targetRate, quality=1)` | `quality`: `0` linear, `1` Hermite cubic |
| `removeSilence(handle, {thresholdDb=-45, minSilenceSec=0.3, padMs=20})` | Drops silence runs, keeps `padMs` context |

## Effects (each returns a new handle)

### `biquad(handle, {type, freqHz, q=0.7071, gainDb=0})`

RBJ audio-EQ-cookbook biquad, transposed direct form II. `type`:
`lowpass · highpass · bandpass · notch · allpass · peaking · lowshelf · highshelf`
(`gainDb` applies to peaking/shelf types).

### `compressor(handle, {thresholdDb=-20, ratio=4, attackMs=10, releaseMs=100, kneeDb=6, makeupDb=0, link=false})`

RMS-style envelope follower with soft knee and smooth attack/release.
`link: true` derives one shared envelope from the loudest channel and applies
identical gain to all channels (mastering-friendly, keeps the stereo image stable).

### `limiter(handle, {thresholdDb=-1, releaseMs=100, lookaheadMs=1})`

True look-ahead **brickwall peak limiter**. The signal is delayed by `lookaheadMs`
and gain drops anticipate peaks, so the output **never** exceeds `thresholdDb`.
Channels share one linked gain.

### `clip(handle, ceilingDb=-0.1)`

Hard clip at an absolute output ceiling — the final safety stage of a mastering chain.

### `gate(handle, {thresholdDb=-45, attackMs=1, releaseMs=60, floorDb=-96})`

Noise gate with 6 dB hysteresis to avoid chatter. `floorDb <= -119` is hard silence.

### `reverb(handle, {roomSize=0.7, damping=0.35, wetLevel=0.3, dryLevel=0.7, width=1, tailSec=1})`

Freeverb-style Schroeder reverb (8 combs + 4 allpasses per side, 23-sample stereo
spread). **Always outputs stereo** (mono/stereo input). `tailSec` of decay is
appended after the input ends.

### `delay(handle, {delayMs=300, feedback=0.35, mix=0.35, stereoSpreadMs=0})`

Feedback delay line per channel; `stereoSpreadMs` offsets the right channel for
Haas-style width.

## Mixing

```js
mix(tracks, outRate = 0, outChannels = 0)
// tracks: [{ handle, gainDb = 0, pan = 0 }, ...]
```

Mixes any number of tracks. `outRate` defaults to the first track's rate;
`outChannels` defaults to the widest track (any stereo track → stereo out).
Tracks are Hermite-resampled and channel-converted automatically. Summing is
64-bit with a final clamp to ±1.

## Generators

```js
generateTone({
  wave: 'sine',        // sine | square | saw | triangle | noise | silence | impulse
  freqHz: 440,
  durationSec: 1,
  sampleRate: 44100,
  channels: 1,
  amplitude: 0.5,
})
```

---

## Integration notes (how Pro-Audio.html is wired)

The original single-file UI is mounted as the renderer at `src/renderer/index.html`,
already wired to the engine:

- **WAV files** → `proaudio.fs.getPathForFile(file)` (real path) → `engine.loadWav(path)`
  — decoded entirely by the C++ parser; non-WAV formats still use Chromium's decoder and
  are handed to the engine via `engine.fromPCM`.
- **Render & Export page** → the full chain (`AudioRenderer.render` in the page) runs
  resample → 10× biquad EQ → bass/treble shelves → reverb → linked compressor → master
  gain → look-ahead limiter → stereo → ceiling clip, entirely in C++.
- **Export** → native save dialog (`proaudio.dialog.saveAudio(name)`) → `engine.writeWav`.
- **Waveform preview** → `engine.getPeaks()`.
- Playback/monitoring keeps the Web Audio graph (Chromium's native audio thread).

If you extend the UI, follow the same pattern:

| Web pattern | Pro AudioLab pattern |
|---|---|
| `<input type=file>` + `FileReader` | `proaudio.dialog.openAudio()` → `engine.loadWav(path)` |
| `decodeAudioData` / `AudioBuffer` | `engine.loadWav()` / `engine.fromPCM()` handles |
| JS DSP loops (slow) | `engine.biquad/compressor/reverb/…` (C++) |
| `<a download>` export | `proaudio.dialog.saveAudio()` → `engine.writeWav(handle, path, {bits})` |
| Canvas waveform from PCM | `engine.getPeaks(handle, canvasWidth, channel)` |

Playback stays in the browser layer: `engine.toPCM(handle, 'f32')` → split into
`AudioContext` channels (see the demo in `src/renderer/index.html`).
