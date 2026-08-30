// Pro AudioLab - Electron main process.
// Owns the window, native menus and file dialogs. The audio engine itself is
// a native N-API addon loaded by the preload script inside the renderer.
'use strict';

const { app, BrowserWindow, Menu, dialog, ipcMain, shell } = require('electron');
const path = require('path');

const APP_NAME = 'Pro AudioLab';

let mainWindow = null;

function createWindow() {
  mainWindow = new BrowserWindow({
    width: 1440,
    height: 920,
    minWidth: 1024,
    minHeight: 640,
    backgroundColor: '#0b0e14',
    show: false,
    title: APP_NAME,
    webPreferences: {
      preload: path.join(__dirname, '..', 'preload', 'preload.js'),
      contextIsolation: true,
      nodeIntegration: false,
      // Required so the preload can require() the native C++ addon.
      // contextIsolation stays ON: the renderer only sees the whitelisted
      // API exposed through the contextBridge.
      sandbox: false,
      spellcheck: false,
    },
  });

  mainWindow.once('ready-to-show', () => mainWindow.show());

  // Block accidental window.open / navigation away from the app.
  mainWindow.webContents.setWindowOpenHandler(() => ({ action: 'deny' }));
  mainWindow.webContents.on('will-navigate', (e) => e.preventDefault());

  mainWindow.on('closed', () => { mainWindow = null; });

  mainWindow.loadFile(path.join(__dirname, '..', 'renderer', 'index.html'));
  return mainWindow;
}

function send(action, payload) {
  if (mainWindow && !mainWindow.isDestroyed()) {
    mainWindow.webContents.send('menu-action', { action, payload });
  }
}

function buildMenu() {
  const isMac = process.platform === 'darwin';
  const template = [
    ...(isMac ? [{ role: 'appMenu' }] : []),
    {
      label: 'File',
      submenu: [
        {
          label: 'Open Audio File…',
          accelerator: 'CmdOrCtrl+O',
          click: () => send('open-file'),
        },
        {
          label: 'Export / Save As…',
          accelerator: 'CmdOrCtrl+S',
          click: () => send('save-file'),
        },
        { type: 'separator' },
        isMac ? { role: 'close' } : { role: 'quit' },
      ],
    },
    {
      label: 'Edit',
      submenu: [
        { role: 'undo' },
        { role: 'redo' },
        { type: 'separator' },
        { role: 'cut' },
        { role: 'copy' },
        { role: 'paste' },
        { role: 'selectAll' },
      ],
    },
    {
      label: 'View',
      submenu: [
        { role: 'reload' },
        { role: 'forceReload' },
        { role: 'toggleDevTools' },
        { type: 'separator' },
        { role: 'resetZoom' },
        { role: 'zoomIn' },
        { role: 'zoomOut' },
        { role: 'togglefullscreen' },
        { type: 'separator' },
        {
          label: 'C++ Engine Self-Test…',
          click: () => openSelftestWindow(),
        },
      ],
    },
    {
      label: 'Help',
      submenu: [
        {
          label: 'About Pro AudioLab',
          click: () => send('about'),
        },
      ],
    },
  ];
  Menu.setApplicationMenu(Menu.buildFromTemplate(template));
}

// ---- IPC: native file dialogs ---------------------------------------------
const AUDIO_FILTERS = [
  { name: 'Audio Files', extensions: ['wav', 'wave', 'pcm', 'raw', 'dat'] },
  { name: 'All Files', extensions: ['*'] },
];
const EXPORT_FILTERS = [
  { name: 'WAV File', extensions: ['wav'] },
];

ipcMain.handle('dialog:open-audio', async () => {
  const r = await dialog.showOpenDialog(mainWindow, {
    title: 'Open Audio File',
    properties: ['openFile'],
    filters: AUDIO_FILTERS,
  });
  return r.canceled || r.filePaths.length === 0 ? null : r.filePaths[0];
});

ipcMain.handle('dialog:save-audio', async (_e, suggestedName) => {
  const r = await dialog.showSaveDialog(mainWindow, {
    title: 'Export Audio',
    defaultPath:
      typeof suggestedName === 'string' && suggestedName
        ? suggestedName
        : 'proaudiolab-export.wav',
    filters: EXPORT_FILTERS,
  });
  return r.canceled ? null : r.filePath;
});

// Child window with the native-engine diagnostic page.
function openSelftestWindow() {
  const win = new BrowserWindow({
    width: 1100,
    height: 860,
    title: 'Pro AudioLab — Engine Self-Test',
    backgroundColor: '#0b0e14',
    parent: mainWindow || undefined,
    webPreferences: {
      preload: path.join(__dirname, '..', 'preload', 'preload.js'),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: false,
      spellcheck: false,
    },
  });
  win.setMenu(null);
  win.loadFile(path.join(__dirname, '..', 'renderer', 'selftest.html'));
}

ipcMain.handle('window:open-selftest', () => openSelftestWindow());

ipcMain.handle('app:info', () => ({
  name: APP_NAME,
  version: app.getVersion(),
  electron: process.versions.electron,
  node: process.versions.node,
  chrome: process.versions.chrome,
  platform: `${process.platform}-${process.arch}`,
}));

ipcMain.handle('shell:show-in-folder', async (_e, filePath) => {
  if (typeof filePath === 'string' && filePath) shell.showItemInFolder(filePath);
});

// ---- lifecycle --------------------------------------------------------------
const gotLock = app.requestSingleInstanceLock();
if (!gotLock) {
  app.quit();
} else {
  app.on('second-instance', () => {
    if (mainWindow) {
      if (mainWindow.isMinimized()) mainWindow.restore();
      mainWindow.focus();
    }
  });

  app.whenReady().then(() => {
    buildMenu();
    createWindow();
    app.on('activate', () => {
      if (BrowserWindow.getAllWindows().length === 0) createWindow();
    });
  });

  app.on('window-all-closed', () => {
    if (process.platform !== 'darwin') app.quit();
  });
}
