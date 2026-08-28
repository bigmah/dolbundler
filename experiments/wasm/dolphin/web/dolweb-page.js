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
// ?auto=1 starts without a click, and ?report=1 posts progress back to serve.py.
// Both exist for the same reason: Safari has no headless mode and no way to
// drive it from outside, so the page has to report on itself.
const AUTO = params.get('auto') === '1';
const REPORT = params.get('report') === '1';

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

function report(payload) {
  if (!REPORT) return;
  try {
    fetch('/report', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ ua: navigator.userAgent, ...payload }),
    });
  } catch (e) { /* the run matters more than the report */ }
}

// One report a second is plenty, and the log is the last few lines only: a
// Dolphin boot is thousands and the point is to see where it stopped.
let heapBytes = () => 0;

if (REPORT) {
  let ticks = 0;
  setInterval(() => {
    const perf = lines.filter((l) => l.startsWith('[perf]')).slice(-3);
    // Linear memory is the number that decides whether a phone can run this at
    // all: the module is 91.6 MB of code on top of whatever the heap grows to,
    // and a WebContent process that crosses its jetsam limit just disappears.
    report({ tick: ++ticks, perf, heapMB: +(heapBytes() / 1048576).toFixed(1),
             tail: lines.slice(-8) });
  }, 5000);
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

// --- the on-screen pad -------------------------------------------------------
//
// The same control IDs the keyboard uses, fed the same way. Pointer events
// rather than touch events so a mouse can exercise it during development, and
// pointer capture so a finger that slides off a button still releases it --
// without that a dropped pointerup leaves a button held down forever, which
// reads as a game that stopped responding.

function wirePad() {
  const coarse = matchMedia('(pointer: coarse)').matches;
  if (!coarse && !params.has('pad')) return;
  document.body.classList.add('pad');

  for (const el of document.querySelectorAll('.btn')) {
    const control = +el.dataset.control;
    const set = (down) => {
      el.classList.toggle('down', down);
      if (setControl) setControl(control, down ? 1 : 0);
    };
    el.addEventListener('pointerdown', (e) => {
      e.preventDefault();
      el.setPointerCapture(e.pointerId);
      set(true);
    });
    const up = (e) => { e.preventDefault(); set(false); };
    el.addEventListener('pointerup', up);
    el.addEventListener('pointercancel', up);
  }

  const stick = document.getElementById('stick');
  const knob = document.getElementById('knob');
  let stickPointer = null;
  const moveStick = (e) => {
    const r = stick.getBoundingClientRect();
    // Normalised to the circle, clamped to it: a GameCube stick cannot leave
    // its gate and neither should this.
    let x = (e.clientX - (r.left + r.width / 2)) / (r.width / 2);
    let y = (e.clientY - (r.top + r.height / 2)) / (r.height / 2);
    const len = Math.hypot(x, y);
    if (len > 1) { x /= len; y /= len; }
    knob.style.left = `${33 + x * 33}%`;
    knob.style.top = `${33 + y * 33}%`;
    if (setControl) {
      setControl(CONTROL.STICK_X, x);
      setControl(CONTROL.STICK_Y, -y);  // screen y grows downward, the stick does not
    }
  };
  stick.addEventListener('pointerdown', (e) => {
    e.preventDefault();
    stickPointer = e.pointerId;
    stick.setPointerCapture(e.pointerId);
    moveStick(e);
  });
  stick.addEventListener('pointermove', (e) => {
    if (e.pointerId === stickPointer) { e.preventDefault(); moveStick(e); }
  });
  const release = (e) => {
    if (e.pointerId !== stickPointer) return;
    e.preventDefault();
    stickPointer = null;
    knob.style.left = '33%';
    knob.style.top = '33%';
    if (setControl) { setControl(CONTROL.STICK_X, 0); setControl(CONTROL.STICK_Y, 0); }
  };
  stick.addEventListener('pointerup', release);
  stick.addEventListener('pointercancel', release);
}

// A phone is held in landscape for this, and iOS will not rotate a page that
// does not ask. Fullscreen has to come from a gesture, so it rides on the first
// touch rather than on load.
function wireImmersive() {
  const enter = async () => {
    try {
      if (!document.fullscreenElement && document.documentElement.requestFullscreen)
        await document.documentElement.requestFullscreen({ navigationUI: 'hide' });
      if (screen.orientation && screen.orientation.lock)
        await screen.orientation.lock('landscape');
    } catch (e) { /* iOS Safari refuses both; the page still works */ }
  };
  addEventListener('pointerdown', enter, { once: true });
}

// --- audio -------------------------------------------------------------------
//
// The worklet reads the emulator's ring straight out of wasm memory; all this
// has to do is tell it where. It runs after the module exists because the ring
// is allocated when the audio backend starts, and it is tolerant of failure:
// a game with no sound is worth more than a page that does not start.

async function startAudio(module) {
  try {
    // The ring is allocated when the audio backend starts, which happens on the
    // emulator's own thread some way into Runtime::Create -- after this promise
    // resolved. So wait for it rather than asking once and giving up.
    let ringPtr = 0;
    for (let i = 0; i < 240 && !ringPtr; i++) {
      ringPtr = module._dolweb_audio_ring_ptr();
      if (!ringPtr) await new Promise((r) => setTimeout(r, 250));
    }
    if (!ringPtr) {
      log('[page] no audio ring after 60 s; the emulator is running without sound');
      return;
    }
    const ctx = new AudioContext({ latencyHint: 'interactive' });
    await ctx.audioWorklet.addModule('dolweb-audio.js');
    const node = new AudioWorkletNode(ctx, 'dolweb-audio', {
      numberOfInputs: 0,
      outputChannelCount: [2],
      processorOptions: {
        memory: module.HEAPU8.buffer,
        ringPtr,
        ringFrames: module._dolweb_audio_ring_frames(),
        readPtr: module._dolweb_audio_read_ptr(),
        writePtr: module._dolweb_audio_write_ptr(),
        sampleRate: module._dolweb_audio_sample_rate(),
      },
    });
    node.connect(ctx.destination);
    // Safari starts a context suspended unless it was created inside a gesture,
    // and "suspended" here is silence with no error anywhere.
    if (ctx.state === 'suspended') await ctx.resume();
    // Both indices count frames forever, so "is the sound actually moving" is
    // two reads a second apart. Without this the only way to tell a connected
    // worklet from a working one is to listen.
    const indices = new Int32Array(module.HEAPU8.buffer);
    const readIdx = module._dolweb_audio_read_ptr() >> 2;
    const writeIdx = module._dolweb_audio_write_ptr() >> 2;
    window.audioStats = () => ({
      state: ctx.state,
      contextRate: ctx.sampleRate,
      guestRate: module._dolweb_audio_sample_rate(),
      written: Atomics.load(indices, writeIdx) >>> 0,
      read: Atomics.load(indices, readIdx) >>> 0,
    });
    window.dolwebAudio = { ctx, node };
    log(`[page] audio: ${ctx.state}, context ${ctx.sampleRate} Hz, ` +
        `guest ${module._dolweb_audio_sample_rate()} Hz`);
    // A page that loses the gesture (autoplay policy, a background tab) can be
    // resumed by the next touch.
    const resume = () => ctx.resume();
    addEventListener('pointerdown', resume);
    addEventListener('keydown', resume);
  } catch (e) {
    log('[page] audio failed: ' + e.message);
  }
}

// --- boot -------------------------------------------------------------------

// What this browser can actually do, sent before anything is attempted. On a
// phone there is no console to read and no way to drive Safari from outside, so
// a run that fails has to have already said why it was going to.
function capabilities() {
  let webgl2 = false;
  let renderer = '';
  try {
    const probe = document.createElement('canvas').getContext('webgl2');
    webgl2 = !!probe;
    if (probe) {
      const info = probe.getExtension('WEBGL_debug_renderer_info');
      renderer = info ? probe.getParameter(info.UNMASKED_RENDERER_WEBGL) : 'masked';
    }
  } catch (e) { /* reported as false */ }
  return {
    crossOriginIsolated: !!self.crossOriginIsolated,
    sharedArrayBuffer: typeof SharedArrayBuffer !== 'undefined',
    // A module whose only content is a *shared* memory. Declaring one requires
    // the threads proposal, so validate() answers the question and nothing else
    // has to be got right -- an earlier version of this probe carried an atomic
    // op in a code section, got the encoding wrong, and reported "no threads"
    // on a browser that was already running them.
    wasmThreads: (() => {
      try {
        return WebAssembly.validate(
            new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0, 5, 4, 1, 3, 1, 1]));
      } catch (e) { return false; }
    })(),
    audioWorklet: typeof AudioWorklet !== 'undefined',
    webgl2, renderer,
    cores: navigator.hardwareConcurrency,
    deviceMemoryGB: navigator.deviceMemory ?? null,
    screen: `${screen.width}x${screen.height}@${devicePixelRatio}`,
  };
}

async function boot() {
  const caps = capabilities();
  log('[page] ' + JSON.stringify(caps));
  report({ phase: 'capabilities', ...caps });
  if (!caps.webgl2)
    log('[page] no WebGL2: the OpenGL backend cannot start. Try ?backend=Null.');
  startBtn.disabled = true;
  status('loading module');

  if (!crossOriginIsolated) {
    log('[page] crossOriginIsolated is false: SharedArrayBuffer is unavailable ' +
        'and the emulator cannot start its threads. Serve with COOP/COEP.');
    status('not cross-origin isolated');
    report({ phase: 'not-isolated' });
    return;
  }

  const module = await createDolWeb({
    canvas,
    arguments: [GAME, '/user', BACKEND, SECONDS, ...ENV],
    print: log,
    printErr: log,
    onTitle: (t) => { document.title = t; status(t); },
  });
  heapBytes = () => module.HEAPU8.length;
  report({ phase: 'module-created', heapMB: +(module.HEAPU8.length / 1048576).toFixed(1) });
  wireKeyboard(module);
  wirePad();
  wireImmersive();
  startAudio(module);
  // Handy from the console and from drive-dolphin.mjs: the user directory is
  // in linear memory, so this is the only way to read what Dolphin wrote there.
  window.dolweb = module;
  window.cat = module.cwrap('dolweb_cat', null, ['string']);
  window.cwrapSetControl = setControl;
  status('running');
}

startBtn.onclick = boot;
// Straight away if the document is already up: this script is the last thing in
// the body, so waiting for `load` is a race with the module's own fetch.
if (AUTO) {
  if (document.readyState === 'complete') boot();
  else addEventListener('load', boot);
}
