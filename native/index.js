// Pro AudioLab - native addon loader.
// Loads the compiled C++ engine (N-API addon). N-API addons are ABI-stable,
// so the binary built against plain Node also loads inside Electron.
'use strict';

const path = require('path');

const candidates = [
  path.join(__dirname, 'build', 'Release', 'audio_engine.node'),
  path.join(__dirname, 'build', 'Debug', 'audio_engine.node'),
];

let lastErr = null;
for (const file of candidates) {
  try {
    module.exports = require(file);
    return;
  } catch (err) {
    lastErr = err;
  }
}

throw new Error(
  'Pro AudioLab native engine is not built yet. Run "npm run build:native" ' +
    'first (requires Visual Studio Build Tools + Python on Windows, or ' +
    'build-tools + python3 on Linux/macOS). Tried: ' +
    candidates.join(', ') +
    (lastErr ? ' - last error: ' + lastErr.message : '')
);
