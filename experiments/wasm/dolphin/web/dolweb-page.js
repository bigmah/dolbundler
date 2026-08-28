// The page half of the wasm build: pick a disc, start the emulator, show what
// it says. Everything below the canvas is Dolphin; this file only supplies the
// three things a browser owns -- the canvas, the keyboard, and the log.
'use strict';

const params = new URLSearchParams(location.search);
const GAME = params.get('game') || 'game';
const BACKEND = params.get('backend') || 'OGL';
const SECONDS = params.get('seconds') || '0';
// ?env=KEY=VALUE&env=KEY2=VALUE2 -- every knob in the emulator is read with
// getenv(), and a browser has no environment, so they ride in on argv.
const ENV = params.getAll('env');

const logEl = document.getElementById('log');
const statusEl = document.getElementById('status');
const startBtn = document.getElementById('start');
const canvas = document.getElementById('canvas');

let lines = [];
function log(text) {
  lines.push(text);
  // A Dolphin boot is thousands of lines and a phone will not thank us for
  // keeping them all in the DOM.
  if (lines.length > 400) lines = lines.slice(-400);
  logEl.textContent = lines.join('\n');
  logEl.scrollTop = logEl.scrollHeight;
  console.log(text);
}

function status(text) {
  statusEl.textContent = text;
}

document.getElementById('logtoggle').onclick = () => logEl.classList.toggle('hidden');

// --- input ------------------------------------------------------------------
//
// Dolphin's own touch overrider, the same one the iOS app drives. Control IDs
// come from InputCommon/ControllerInterface/Touch/InputOverrider.h.
const CONTROL = {
  BUTTON_A: 0, BUTTON_B: 1, BUTTON_START: 2, BUTTON_X: 3, BUTTON_Y: 4,
  BUTTON_Z: 5, BUTTON_UP: 6, BUTTON_DOWN: 7, BUTTON_LEFT: 8, BUTTON_RIGHT: 9,
  STICK_UP: 10, STICK_DOWN: 11, STICK_LEFT: 12, STICK_RIGHT: 13,
  C_STICK_UP: 14, C_STICK_DOWN: 15, C_STICK_LEFT: 16, C_STICK_RIGHT: 17,
  TRIGGER_L: 18, TRIGGER_R: 19,
};
const KEYS = {
  KeyJ: CONTROL.BUTTON_A, KeyK: CONTROL.BUTTON_B,
  KeyU: CONTROL.BUTTON_X, KeyI: CONTROL.BUTTON_Y,
  KeyQ: CONTROL.BUTTON_Z, Enter: CONTROL.BUTTON_START,
  KeyE: CONTROL.TRIGGER_L, KeyR: CONTROL.TRIGGER_R,
  ArrowUp: CONTROL.BUTTON_UP, ArrowDown: CONTROL.BUTTON_DOWN,
  ArrowLeft: CONTROL.BUTTON_LEFT, ArrowRight: CONTROL.BUTTON_RIGHT,
  KeyW: CONTROL.STICK_UP, KeyS: CONTROL.STICK_DOWN,
  KeyA: CONTROL.STICK_LEFT, KeyD: CONTROL.STICK_RIGHT,
};

let setControl = null;

function wireKeyboard(module) {
  setControl = module.cwrap('dolweb_set_control', null, ['number', 'number']);
  const press = (e, value) => {
    const control = KEYS[e.code];
    if (control === undefined || !setControl) return;
    e.preventDefault();
    setControl(control, value);
  };
  addEventListener('keydown', (e) => { if (!e.repeat) press(e, 1); });
  addEventListener('keyup', (e) => press(e, 0));
}

// --- boot -------------------------------------------------------------------

startBtn.onclick = async () => {
  startBtn.disabled = true;
  status('loading module');

  if (!crossOriginIsolated) {
    log('[page] crossOriginIsolated is false: SharedArrayBuffer is unavailable ' +
        'and the emulator cannot start its threads. Serve with COOP/COEP.');
    status('not cross-origin isolated');
    return;
  }

  const module = await createDolWeb({
    canvas,
    arguments: [GAME, '/user', BACKEND, SECONDS, ...ENV],
    print: log,
    printErr: log,
    onTitle: (t) => { document.title = t; status(t); },
  });
  wireKeyboard(module);
  // Handy from the console and from drive-dolphin.mjs: the user directory is
  // in linear memory, so this is the only way to read what Dolphin wrote there.
  window.dolweb = module;
  window.cat = module.cwrap('dolweb_cat', null, ['string']);
  status('running');
};
