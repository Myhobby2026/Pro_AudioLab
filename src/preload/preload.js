// Pro AudioLab - preload script.
// Bridges the renderer (your UI) to the native C++ engine and the Electron
// main process. The renderer gets exactly this whitelist via window.proaudio:
// no direct Node access, contextIsolation stays enabled.
'use strict';

const { contextBridge, ipcRenderer, webUtils } = require('electron');

// The native C++ audio engine (N-API addon, ABI-stable across Node/Electron).
let engine;
try {
  engine = require('../../native');
} catch (err) {
  // Expose a stub that rethrows with a helpful message so the UI can show
  // build instructions instead of crashing.
  engine = new Proxy(
    {},
    {
      get() {
        throw err;
      },
    }
  );
}

const engineApi = {
  version: engine.version.bind(engine),
  // lifecycle
  createBuffer: engine.createBuffer.bind(engine),
  freeBuffer: engine.freeBuffer.bind(engine),
  info: engine.info.bind(engine),
  stats: engine.stats.bind(engine),
  // file i/o
  loadWav: engine.loadWav.bind(engine),
  writeWav: engine.writeWav.bind(engine),
  // raw pcm bridge
  toPCM: engine.toPCM.bind(engine),
  fromPCM: engine.fromPCM.bind(engine),
  // analysis
  getPeaks: engine.getPeaks.bind(engine),
  analyze: engine.analyze.bind(engine),
  detectSilence: engine.detectSilence.bind(engine),
  // processing (each returns a NEW buffer handle)
  gain: engine.gain.bind(engine),
  normalize: engine.normalize.bind(engine),
  fade: engine.fade.bind(engine),
  pan: engine.pan.bind(engine),
  reverse: engine.reverse.bind(engine),
  biquad: engine.biquad.bind(engine),
  compressor: engine.compressor.bind(engine),
  limiter: engine.limiter.bind(engine),
  clip: engine.clip.bind(engine),
  gate: engine.gate.bind(engine),
  reverb: engine.reverb.bind(engine),
  delay: engine.delay.bind(engine),
  setChannels: engine.setChannels.bind(engine),
  resample: engine.resample.bind(engine),
  removeSilence: engine.removeSilence.bind(engine),
  mix: engine.mix.bind(engine),
  // generators
  generateTone: engine.generateTone.bind(engine),
};

contextBridge.exposeInMainWorld('proaudio', {
  engine: engineApi,

  dialog: {
    /** Ask the user for an audio file to open. Returns a path or null. */
    openAudio: () => ipcRenderer.invoke('dialog:open-audio'),
    /** Ask the user where to save (optionally pre-filling `suggestedName`). */
    saveAudio: (suggestedName) => ipcRenderer.invoke('dialog:save-audio', suggestedName),
  },

  fs: {
    /** Real filesystem path of a File chosen via <input type=file> or drag&drop. */
    getPathForFile: (file) => webUtils.getPathForFile(file),
  },

  app: {
    info: () => ipcRenderer.invoke('app:info'),
    showInFolder: (p) => ipcRenderer.invoke('shell:show-in-folder', p),
    /** Subscribe to native menu actions: cb({action, payload}) */
    onMenuAction: (cb) => {
      const listener = (_e, data) => cb(data);
      ipcRenderer.on('menu-action', listener);
      return () => ipcRenderer.removeListener('menu-action', listener);
    },
  },
});
