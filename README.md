# Pro AudioLab — Electron + C++ Windows Application

A professional audio workstation desktop app for **Windows**: an **Electron** UI on top of a
**native C++ DSP engine** compiled as an N-API addon. All heavy audio work — WAV parsing,
mixing, effects, analysis, export — runs in compiled C++, not JavaScript.

```
┌────────────────────────────────────────────┐
│  Renderer (UI)                             │
│  Pro-Audio.html interface                  │
│  window.proaudio.engine.*  ← contextBridge │
├────────────────────────────────────────────┤      ┌──────────────────────────┐
│  Preload (isolated, whitelisted API)       │──────▶  native/audio_engine    │
├────────────────────────────────────────────┤      │  C++17 · N-API addon    │
│  Main process                              │      │  WAV I/O · DSP · Mix    │
│  window · menus · file dialogs             │      │  analysis · generators  │
└────────────────────────────────────────────┘      └──────────────────────────┘
```

- **UI / app shell** — Electron (`src/main`, `src/preload`, `src/renderer`)
- **Audio engine** — C++ (`native/src`), compiled with node-gyp into `audio_engine.node`
- **Renderer ↔ engine** — secure `contextBridge` API (contextIsolation ON, nodeIntegration OFF)
- **Packaging** — electron-builder → NSIS installer + portable `.exe` for Windows

The C++ addon is a pure **N-API** module, so one build works in both plain Node (for the
test suite) and Electron (ABI-stable across versions — no rebuild needed for Electron).

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
npm run test:engine  # 35-test C++ engine suite (runs under plain Node)
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
| `src/main/main.js` | Electron main process: window, native menu, file dialogs, IPC |
| `src/preload/preload.js` | `contextBridge` whitelist — the only API the UI can touch |
| `src/renderer/` | UI. `index.html` is the current self-test console; the original `Pro-Audio.html` interface is mounted here during integration |
| `native/src/buffer.*` | Planar float audio buffer, PCM conversion, handle registry |
| `native/src/wav.*` | RIFF/RF64 WAV reader/writer (8/16/24/32-bit int, 32/64-bit float, WAVE_FORMAT_EXTENSIBLE, Unicode paths) |
| `native/src/dsp.*` | Biquad EQ (RBJ), compressor (soft-knee), noise gate, Freeverb-style reverb, feedback delay, gain/fades/normalize/pan/reverse, resampling, channel conversion, silence tools |
| `native/src/analysis.*` | Peak/RMS/DC metering, waveform peaks for rendering |
| `native/src/mixer.*` | Multitrack mixdown (auto resample + channel conversion + pan) |
| `native/src/generator.*` | Sine/square/saw/triangle/noise/silence/impulse generators |
| `native/src/napi_bindings.cpp` | N-API glue: all engine functions exposed to JS |
| `scripts/test-engine.js` | 35-test headless engine suite |
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
