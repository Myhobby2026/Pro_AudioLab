# Pro AudioLab — Electron + C++ Windows Application

**"Pro Audio Player – Studio Edition"** rebuilt as a Windows desktop app: your original
single-file HTML interface, running in **Electron**, with all audio processing moved into a
**native C++ DSP engine** compiled as an N-API addon.

```
┌──────────────────────────────────────────────┐
│  Renderer — your Pro-Audio.html UI           │
│  Home · Playlist · Effects · Master ·        │
│  Equalizer · Render & Export · Settings      │
│         window.proaudio.*  ← contextBridge   │
├──────────────────────────────────────────────┤       ┌─────────────────────────────┐
│  Preload (isolated, whitelisted API)         │──────▶│  native/audio_engine        │
├──────────────────────────────────────────────┤       │  C++17 · N-API addon       │
│  Main process                                │       │  WAV I/O · 10-band EQ      │
│  window · menus · file dialogs · self-test   │       │  comp · limiter · reverb   │
└──────────────────────────────────────────────┘       │  resample · mix · analyze  │
                                                       └─────────────────────────────┘
```

**What runs in C++ now** (previously JavaScript):

| Original (Web Audio / JS) | Pro AudioLab (native C++) |
|---|---|
| `OfflineAudioContext` render | Full mastering chain in C++: resample → 10-band EQ → bass/treble shelves → reverb → compressor → master gain → limiter → ceiling |
| `DynamicsCompressor` ratio 20 "limiter" | True look-ahead **brickwall limiter** + hard **output ceiling** |
| Hand-rolled JS WAV writer (blob download) | C++ WAV writer at 16/24-bit or 32-bit float, via the **native save dialog** |
| `FileReader` + `decodeAudioData` for WAVs | C++ RIFF/RF64 WAV parser (instant, no decode step) |
| JS min/max loops for the waveform | `getPeaks()` in C++ |
| `Math.random` noise-buffer reverb | Freeverb-style Schroeder reverb (8 combs + 4 allpasses per side) |

Realtime playback/monitoring still uses Chromium's Web Audio graph (that *is* a C++
engine) — the upgrade targets every place your app previously burned JavaScript:
decoding, offline rendering, analysis and export.

Extras that come with the desktop shell: native menus & file dialogs (Ctrl+O / Ctrl+S),
drag & drop files onto the window, bundled Font Awesome (works fully offline), CSP
security headers, and a **C++ Engine Self-Test** window (View menu).

---

## Quick start (Windows)

### Prerequisites (one-time)

1. **Node.js LTS** (20 or 22) — <https://nodejs.org>
2. **Visual Studio 2022 Build Tools** with the *Desktop development with C++* workload:
   ```powershell
   winget install Microsoft.VisualStudio.2022.BuildTools --override "--wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
   ```
3. **Python 3.x** (required by node-gyp):
   ```powershell
   winget install Python.Python.3.12
   ```

### Build & run

```powershell
git clone https://github.com/Myhobby2026/Pro_AudioLab.git
cd Pro_AudioLab
npm install          # fetches Electron + build toolchain
npm run build:native # compiles the C++ engine (audio_engine.node)
npm run test:engine  # 46 tests: C++ engine + UI render-pipeline integration
npm start            # launch the Electron app
```

### Package a Windows installer

```powershell
npm run dist            # builds release/Pro-AudioLab-Setup-1.0.0.exe (NSIS)
npm run dist:portable   # also builds a single portable .exe
```

Output lands in `release/`. No code signing is configured — Windows SmartScreen may
show an "unknown publisher" warning on first launch (click *More info → Run anyway*).

> **No compiler on your machine?** Push to GitHub — the included workflow
> (`.github/workflows/build-windows.yml`) builds the installer automatically and
> uploads it as an artifact under *Actions → Build Windows App*.

---

## Project layout

| Path | What it is |
|---|---|
| `src/main/main.js` | Electron main process: window, native menu, file dialogs, IPC, self-test window |
| `src/preload/preload.js` | `contextBridge` whitelist — the only API the UI can touch |
| `src/renderer/index.html` | **Your Pro-Audio.html UI, integrated with the C++ engine** (render/export/waveform/analysis all native) |
| `src/renderer/selftest.html` | Engine self-test & diagnostic page (View → C++ Engine Self-Test) |
| `src/renderer/vendor/` | Bundled Font Awesome 6.4 (offline, no CDN) |
| `native/src/buffer.*` | Planar float audio buffer, PCM conversion, handle registry |
| `native/src/wav.*` | RIFF/RF64 WAV reader/writer (8/16/24/32-bit int, 32/64-bit float, WAVE_FORMAT_EXTENSIBLE, Unicode paths) |
| `native/src/dsp.*` | Biquad EQ (RBJ), soft-knee compressor (incl. stereo-linked mode), look-ahead brickwall limiter, output ceiling clip, Freeverb-style reverb, feedback delay, gain/fades/normalize/pan/reverse, resampling, channel conversion, silence tools |
| `native/src/analysis.*` | Peak/RMS/DC metering, waveform peaks for rendering |
| `native/src/mixer.*` | Multitrack mixdown (auto resample + channel conversion + pan) |
| `native/src/generator.*` | Sine/square/saw/triangle/noise/silence/impulse generators |
| `native/src/napi_bindings.cpp` | N-API glue: all engine functions exposed to JS |
| `scripts/test-engine.js` | 39-test headless engine suite |
| `scripts/test-render-pipeline.js` | 7-test integration suite: the exact UI→C++ render chain |
| `electron-builder.yml` | Windows packaging config |

Full engine API reference: **[docs/ENGINE_API.md](docs/ENGINE_API.md)**

---

## Using the engine from the UI

The renderer never touches Node directly. Everything goes through `window.proaudio`:

```js
// load a file (native file dialog -> C++ WAV parser)
const path = await proaudio.dialog.openAudio();
const { handle, channels, sampleRate, frames } = proaudio.engine.loadWav(path);

// process entirely in C++ — each call returns a NEW handle (undo-friendly)
const eq   = proaudio.engine.biquad(handle, { type: 'highpass', freqHz: 80, q: 0.72 });
const comp = proaudio.engine.compressor(eq, { thresholdDb: -18, ratio: 4, makeupDb: 3 });
const wet  = proaudio.engine.reverb(comp, { roomSize: 0.75, wetLevel: 0.28, tailSec: 1.5 });

// analyse + export
const stats = proaudio.engine.analyze(wet);           // peak/rms dB per channel
proaudio.engine.writeWav(wet, outPath, { bits: 24 }); // 24-bit PCM WAV

// free what you don't need
[eq, comp, handle].forEach(h => proaudio.engine.freeBuffer(h));
```

## Security model

- `contextIsolation: true`, `nodeIntegration: false` — the page has **no** Node access
- The preload exposes a minimal, explicitly whitelisted surface (`proaudio.engine`, `proaudio.dialog`, `proaudio.app`)
- CSP header set in the renderer; `window.open` and navigation are blocked
- All engine calls are synchronous and in-process — no shell-outs, no network

## Scripts

| Command | Purpose |
|---|---|
| `npm start` | Run the app in dev mode |
| `npm run build:native` | Compile the C++ engine (Release) |
| `npm run build:native:debug` | Compile with debug symbols |
| `npm run test:engine` | Run the C++ engine test suite |
| `npm run dist` | Build the Windows NSIS installer |
| `npm run dist:portable` | Build the portable `.exe` |
| `npm run setup` | install + build + test in one shot |

## Troubleshooting

- **`audio_engine is not built yet`** — run `npm run build:native`.
- **node-gyp can't find Visual Studio** — install the Build Tools workload (see prerequisites)
  and, if you have several versions, `npm config set msvs_version 2022`.
- **node-gyp can't find Python** — `npm config set python "C:\Path\To\python.exe"`.
- **Long path errors during packaging** — `git config --system core.longpaths true` and enable
  long paths in Windows registry, or move the repo close to `C:\`.
- Engine tests fail after editing C++ — make sure you rebuilt: `npm run build:native && npm run test:engine`.

## License

MIT
