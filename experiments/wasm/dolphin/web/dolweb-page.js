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
// InputCommon/ControllerInterface/Touch/InputOverrider.h. The sticks are axes
// with a value in [-1, 1], not four buttons; the earlier version of this table
// was copied from a different pad ABI and pressed X when it meant START.
const CONTROL = {
  A: 0, B: 1, X: 2, Y: 3, Z: 4, START: 5,
  UP: 6, DOWN: 7, LEFT: 8, RIGHT: 9,
  L_DIGITAL: 10, R_DIGITAL: 11, L_ANALOG: 12, R_ANALOG: 13,
  STICK_X: 14, STICK_Y: 15, C_STICK_X: 16, C_STICK_Y: 17,
};
const BUTTONS = {
  KeyJ: CONTROL.A, KeyK: CONTROL.B, KeyU: CONTROL.X, KeyI: CONTROL.Y,
  KeyQ: CONTROL.Z, Enter: CONTROL.START,
  KeyE: CONTROL.L_DIGITAL, KeyR: CONTROL.R_DIGITAL,
  ArrowUp: CONTROL.UP, ArrowDown: CONTROL.DOWN,
  ArrowLeft: CONTROL.LEFT, ArrowRight: CONTROL.RIGHT,
};
const AXES = {
  KeyW: [CONTROL.STICK_Y, 1], KeyS: [CONTROL.STICK_Y, -1],
  KeyA: [CONTROL.STICK_X, -1], KeyD: [CONTROL.STICK_X, 1],
  KeyT: [CONTROL.C_STICK_Y, 1], KeyG: [CONTROL.C_STICK_Y, -1],
  KeyF: [CONTROL.C_STICK_X, -1], KeyH: [CONTROL.C_STICK_X, 1],
};

let setControl = null;

function wireKeyboard(module) {
  setControl = module.cwrap('dolweb_set_control', null, ['number', 'number']);
  const press = (e, down) => {
    const button = BUTTONS[e.code];
    if (button !== undefined) {
      e.preventDefault();
      setControl(button, down ? 1 : 0);
      return;
    }
    const axis = AXES[e.code];
    if (axis) {
      e.preventDefault();
      setControl(axis[0], down ? axis[1] : 0);
    }
  };
  addEventListener('keydown', (e) => { if (!e.repeat) press(e, true); });
  addEventListener('keyup', (e) => press(e, false));
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
  window.cwrapSetControl = setControl;
  status('running');
};
