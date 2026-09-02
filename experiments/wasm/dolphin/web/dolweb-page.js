// The page half of the wasm build: pick a disc, start the emulator, show what
// it says. Everything below the canvas is Dolphin; this file only supplies the
// three things a browser owns -- the canvas, the keyboard, and the log.
'use strict';

const params = new URLSearchParams(location.search);
const GAME = params.get('game') || 'game';
let BACKEND = params.get('backend') || 'OGL';
let SECONDS = params.get('seconds') || '0';
// ?env=KEY=VALUE&env=KEY2=VALUE2 -- every knob in the emulator is read with
// getenv(), and a browser has no environment, so they ride in on argv.
const ENV = params.getAll('env');
// ?auto=1 starts without a click, and ?report=1 posts progress back to serve.py.
// Both exist for the same reason: Safari has no headless mode and no way to
// drive it from outside, so the page has to report on itself.
const AUTO = params.get('auto') === '1';
const REPORT = params.get('report') === '1';
// ?log=N turns Dolphin's own log on: 1 is the boot narrative, 3 is what the
// video backend found, 5 is a line per DVD read.
//
// It used to ride on ?report=1 instead, on the reasoning that reporting is a
// diagnostic mode. That coupling made the flag every measurement here sets
// change what it measured. In a pthread build a printf from the emulator
// thread is a postMessage to the browser's main thread -- the thread WASMFS
// needs to service the disc -- and Dolphin logs hardest exactly where the game
// loads most. Desktop Safari, Null, 150 s wall, guest-anchored:
//
//     ?report=1                        guest 69 s reached,   45.7%
//     ?report=1 + DOLWEB_DEBUG_LOG=0   guest 150 s reached, 105.9%
//
// So every browser figure in OVER-THE-LINE.md before 2026-08-31 is a logging
// figure, and the node harness -- which never set it -- was never comparable.
// Ask for the log deliberately or do not get it.
const LOG = params.get('log') || '';
// ?savestate=level:240 -- see the upload below.
const SAVESTATE = params.get('savestate') || '';

// --- ?acts= ------------------------------------------------------------------
//
// A timeline of button presses, anchored to the *guest's* clock:
//
//     ?acts=g25:5,g40:5,g60:8:400,g110:15:9000
//
// "g25:5" taps control 5 at guest second 25; a third field is how long to hold
// it in milliseconds. drive-dolphin.mjs has had this for a while and it is what
// reaches a named level rather than whatever the attract loop was showing. But
// it drives the page from outside over CDP, and Safari -- on a phone, and in
// the simulator -- cannot be driven from outside at all. Owning the timeline
// here is what makes the same scene reachable in the browser that matters.
//
// Guest seconds and not wall: an interpreter build reaches the same menu, it
// just takes twenty minutes, and wall-clock cues would all fire during the
// logos.
const ACTS = (params.get('acts') || '').split(',').filter(Boolean).map((spec) => {
  const [at, control, hold] = spec.split(':');
  return { at: +String(at).replace(/^g/, ''), control: +control,
           hold: +(hold || 140), done: false };
}).sort((a, b) => a.at - b.at);

// --- ?ab=N -------------------------------------------------------------------
//
// Null against OpenGL on the same scene is the one comparison that says whether
// a build is held back by the CPU or by the renderer, and it has to be run on
// the device: this Mac's answer (they measure the same) says nothing about a
// phone with a different GPU and a different browser. But the two runs need the
// same scene and the same session, and a phone cannot be driven from outside.
//
// So the page runs both itself: Null for N seconds of *running* time -- the
// budget starts at the first frame, not at load, because boot on a phone takes
// longer than the measurement does -- then it reloads into OpenGL and does it
// again. Reports carry `ab` so the two halves separate afterwards.
const AB = params.get('ab') ? +params.get('ab') : 0;
const AB_PHASE = +(params.get('abphase') || '1');
// ?abflip=1 runs OpenGL first. The order matters on a phone: the second half
// always runs on a hotter device, so a fixed order biases every comparison
// against whichever backend goes second. Run it both ways before believing a
// difference.
const AB_FLIP = params.get('abflip') === '1';
const AB_BACKEND = (AB_PHASE === 1) !== AB_FLIP ? 'Null' : 'OGL';
// The window, in guest seconds since boot. It starts well past the logos so
// that both halves measure the game rather than a fade.
const AB_FROM = +(params.get('abfrom') || '45');
const T0 = performance.now();
if (AB) {
  BACKEND = AB_BACKEND;
  SECONDS = '0';  // the page owns the budget here
  // Without this both halves read 100% and the comparison says nothing: a
  // throttled build with headroom is indistinguishable from one with none.
  if (!ENV.some((e) => e.startsWith('MODERNGEKKO_EMULATION_SPEED')))
    ENV.push('MODERNGEKKO_EMULATION_SPEED=0');
}

const logEl = document.getElementById('log');
const statusEl = document.getElementById('status');
const startBtn = document.getElementById('start');
const canvas = document.getElementById('canvas');

let lines = [];
// The first sixty lines, kept forever. They are the boot narrative -- which
// backend, which module, how far the core got -- and on a phone they are the
// only account there is: the tail has scrolled past them by the time anything
// goes wrong, and there is no console to scroll back through.
const head = [];
// ?lobby=1: a run queued from the Mac through lobby.html goes back there when
// its budget is spent, so the next queued run needs nobody at the phone.
const LOBBY = params.get('lobby') === '1';
let lobbyReturning = false;
function lobbyWatch(text) {
  if (!LOBBY || lobbyReturning) return;
  if (!text.includes('[dolweb] budget reached') && !text.includes('Run() returned')) return;
  lobbyReturning = true;
  status('returning to the lobby');
  // A beat for the last reports to leave.
  setTimeout(() => location.replace('/lobby.html'), 6000);
}

function log(text) {
  if (head.length < 140) head.push(text);
  lines.push(text);
  // A Dolphin boot is thousands of lines and a phone will not thank us for
  // keeping them all in the DOM.
  if (lines.length > 400) lines = lines.slice(-400);
  logEl.textContent = lines.join('\n');
  logEl.scrollTop = logEl.scrollHeight;
  console.log(text);
  lobbyWatch(text);
  if (ACTS.length) actsWatch(text);
  if (AB) abWatch(text);
  bootWatch(text);
}

// Narrates the boot on the overlay and clears it on the first frame the game
// actually draws.
//
// "The first frame" is fps > 0 in the [perf] line, not the emulator starting:
// the console boots, loads and plays its logos for a long time before anything
// reaches the canvas, and hiding the overlay when the runtime comes up would
// hand the user back the black screen this exists to explain.
let bootHz = 0;
let bootDone = false;
let buildTag = '';
function bootWatch(text) {
  if (bootDone) return;
  const clk = /guest clock (\d+) Hz/.exec(text);
  if (clk) { bootHz = +clk[1]; return; }
  if (text.includes('mounted /game from')) {
    const n = /\((\d+) files\)/.exec(text);
    loading('Booting the game', n ? `disc mounted, ${n[1]} files` : 'disc mounted');
    return;
  }
  if (text.includes('embedded module attached')) {
    // "... from GEXE52=/abs/path/build-wasm/gexe52-c256/generated" -- keep the
    // directory. It is the only thing in a session that says *which build*
    // produced the numbers, and its absence is how a 4096-chunk module went on
    // being measured as if it were the 256-chunk one.
    const m = / from \S*?([^/\s]+)\/generated/.exec(text);
    if (m) buildTag = m[1];
    loading('Booting the game', 'recompiled code attached');
    return;
  }
  if (text.includes('runtime created; running')) {
    loading('Booting the game', 'starting the console');
    return;
  }
  const m = /^\[perf\]\s+([\d.]+) fps.*?ticks=(\d+)/.exec(text);
  if (!m) return;
  const fps = parseFloat(m[1]);
  if (fps > 0) {
    bootDone = true;
    loading(null);
    return;
  }
  const secs = bootHz ? (+m[2] / bootHz) : 0;
  // The Null backend never draws, so "wait for a frame" would leave the overlay
  // up for the whole run. Give up after a while and say so rather than sit
  // there: by then the emulator is plainly working and the picture is the one
  // thing that is not coming.
  if (secs > 25) {
    bootDone = true;
    loading(null);
    return;
  }
  loading('Booting the game',
          secs >= 1 ? `${secs.toFixed(0)}s in, no picture yet`
                    : 'starting the console');
}

function status(text) {
  statusEl.textContent = text;
}

// --- the loading overlay -------------------------------------------------
// 92 MB of WebAssembly, then a disc streamed over the network, then a boot
// sequence that is legitimately black for a while. On a phone that is a long
// time staring at nothing with no way to tell a slow download from a hang.
const loadingEl = document.getElementById('loading');
const loadingTitle = document.getElementById('loading-title');
const loadingFill = document.getElementById('loading-fill');
const loadingDetail = document.getElementById('loading-detail');

// frac === undefined means "no measurable total": sweep rather than invent a
// percentage.
function loading(title, detail = '', frac) {
  if (!loadingEl) return;
  if (title === null) { loadingEl.classList.remove('on'); return; }
  loadingEl.classList.add('on');
  loadingTitle.textContent = title;
  loadingDetail.textContent = detail;
  if (frac === undefined) {
    loadingFill.classList.add('sweep');
  } else {
    loadingFill.classList.remove('sweep');
    loadingFill.style.width = `${Math.max(0, Math.min(1, frac)) * 100}%`;
  }
}

const mb = (n) => `${(n / 1048576).toFixed(1)} MB`;

// Instantiate the module from a counted stream.
//
// Streaming rather than fetching to a buffer first: the binary is 92 MB and the
// heap already reaches ~614 MB in a level, which is close enough to what iOS
// will reclaim a tab for that holding a second copy is a bad trade for a
// progress bar. A TransformStream counts the bytes on their way through, so the
// download reports real progress and is still handed straight to
// WebAssembly.instantiateStreaming.
// ?build=<name> loads build-wasm/<name>/ (served as builds/<name>/) instead of
// the dolweb.{js,wasm,data} symlinks in web/. Two browsers -- the phone and the
// simulator -- can then run two builds at once, and a queued phone run names
// its build in its URL rather than depending on what use-build.sh last
// pointed the symlinks at; an A/B that shared them was measuring whichever
// build the other harness had switched to.
const BUILD_DIR = params.get('build') ? `builds/${params.get('build')}/` : '';
function loadModuleScript() {
  return new Promise((resolve, reject) => {
    const s = document.createElement('script');
    s.src = `${BUILD_DIR}dolweb.js`;
    s.onload = resolve;
    s.onerror = () => reject(new Error(`could not load ${s.src}`));
    document.head.appendChild(s);
  });
}

function instantiateCounted(imports, receive) {
  (async () => {
    const url = `${BUILD_DIR}dolweb.wasm`;
    try {
      const res = await fetch(url);
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      const total = +res.headers.get('content-length') || 0;
      let got = 0;
      let painted = 0;
      const counted = new Response(
        res.body.pipeThrough(new TransformStream({
          transform(chunk, controller) {
            got += chunk.byteLength;
            const now = performance.now();
            if (now - painted > 80) {
              painted = now;
              loading('Downloading emulator',
                      total ? `${mb(got)} of ${mb(total)}` : mb(got),
                      total ? got / total : undefined);
            }
            controller.enqueue(chunk);
          },
        })),
        { headers: { 'Content-Type': 'application/wasm' } });
      loading('Downloading emulator', total ? `${mb(total)}` : '', total ? 1 : undefined);
      const { instance, module } = await WebAssembly.instantiateStreaming(counted, imports);
      loading('Linking module', 'this takes a moment on a phone');
      receive(wrapMainExports(instance), module);
    } catch (err) {
      // Any failure here falls back to letting emscripten load it the ordinary
      // way. A progress bar must never be the reason the emulator will not run.
      log(`[page] streaming instantiate failed (${err}); falling back`);
      loading('Downloading emulator', 'progress unavailable');
      try {
        const res = await fetch(url);
        const bytes = await res.arrayBuffer();
        const { instance, module } = await WebAssembly.instantiate(bytes, imports);
        receive(wrapMainExports(instance), module);
      } catch (err2) {
        log(`[page] module load failed: ${err2}`);
        loading('Failed to load', String(err2));
      }
    }
  })();
  return {};  // async: receive() is called above
}

// Identity for this page load, so a reader can tell one run's reports from
// another's. Nothing else in a report can: `ms` is the page's own clock, so two
// pages posting at once interleave two unrelated clocks, and a stale tab that
// keeps re-posting its last perf lines forever is indistinguishable from a live
// one. Reconstructing runs without this was tried and produced a confidently
// wrong answer (a renderer cost of 0.62x against a measured 1.24x).
const RUN_ID = Math.random().toString(36).slice(2, 10);

// --- the main thread, measured ---------------------------------------------
//
// In the shipping configuration the WebGL context lives on the browser's main
// thread and the video thread proxies every GL call to it, so the renderer's
// real cost on a phone is main-thread time -- and nothing inside the emulator
// can see that thread. Three probes, all from here:
//
//   mailbox  time inside the wasm export that runs proxied work on this thread
//            (`_emscripten_check_mailbox`), wrapped when the module is
//            instantiated. Two clock reads per batch, so it is free enough to
//            leave on.
//   gl       ?glprobe=1 wraps every WebGL2RenderingContext method to count and
//            time the calls themselves. Two clock reads per call, so it is not
//            free and is only for finding out what a call costs on a device.
//   lag      a MessageChannel ping that measures how long a task waits for
//            this thread: the delay every proxied GL batch pays before it runs.
//
// Each is a delta per report interval; the [main] line is the same figures as
// percentages of wall time.
const mainStats = { mailboxMs: 0, mailboxCalls: 0, glMs: 0, glCalls: 0, lagMs: 0,
                    since: performance.now() };
const GL_PROBE = params.get('glprobe') === '1';
if (GL_PROBE && window.WebGL2RenderingContext) {
  const proto = WebGL2RenderingContext.prototype;
  for (const name of Object.getOwnPropertyNames(proto)) {
    const d = Object.getOwnPropertyDescriptor(proto, name);
    if (!d || typeof d.value !== 'function' || name === 'constructor') continue;
    const orig = d.value;
    proto[name] = function () {
      const t = performance.now();
      try { return orig.apply(this, arguments); }
      finally { mainStats.glMs += performance.now() - t; mainStats.glCalls++; }
    };
  }
}
function wrapMainExports(instance) {
  const src = instance.exports;
  const orig = src['_emscripten_check_mailbox'];
  if (typeof orig !== 'function') return instance;
  const exports = {};
  for (const k of Object.keys(src)) exports[k] = src[k];
  exports['_emscripten_check_mailbox'] = function () {
    const t = performance.now();
    try { return orig.apply(this, arguments); }
    finally { mainStats.mailboxMs += performance.now() - t; mainStats.mailboxCalls++; }
  };
  return { exports };
}
(function pingLoop() {
  const ch = new MessageChannel();
  let sent = 0;
  let floor = Infinity;
  ch.port1.onmessage = () => {
    const wait = performance.now() - sent;
    if (wait < floor) floor = wait;
    mainStats.lagMs += Math.max(0, wait - floor);
    setTimeout(() => { sent = performance.now(); ch.port2.postMessage(0); }, 4);
  };
  sent = performance.now();
  ch.port2.postMessage(0);
})();
function mainSnapshot() {
  const now = performance.now();
  const wall = now - mainStats.since;
  const out = { wallMs: +wall.toFixed(0), mailboxMs: +mainStats.mailboxMs.toFixed(1),
                mailboxCalls: mainStats.mailboxCalls, lagMs: +mainStats.lagMs.toFixed(1),
                ...(GL_PROBE ? { glMs: +mainStats.glMs.toFixed(1), glCalls: mainStats.glCalls } : {}) };
  mainStats.mailboxMs = 0; mainStats.mailboxCalls = 0; mainStats.lagMs = 0;
  mainStats.glMs = 0; mainStats.glCalls = 0; mainStats.since = now;
  const pct = (ms) => (wall > 0 ? (100 * ms / wall).toFixed(0) : '0');
  log(`[main] mailbox ${pct(out.mailboxMs)}% (${(out.mailboxCalls * 1000 / wall).toFixed(0)}/s)` +
      `  lag ${pct(out.lagMs)}%` +
      (GL_PROBE ? `  gl ${pct(out.glMs)}% (${(out.glCalls * 1000 / wall).toFixed(0)} calls/s)` : ''));
  return out;
}

function report(payload) {
  if (!REPORT) return;
  try {
    fetch('/report', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ ua: navigator.userAgent,
                             run: RUN_ID,
                             ms: Math.round(performance.now() - T0),
                             ...(buildTag ? { build: buildTag } : {}),
                             ...(AB ? { ab: AB_BACKEND } : {}), ...payload }),
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
    // The head goes with the first few reports only; after that it has been
    // seen and the tail is what is changing.
    report({ tick: ++ticks, perf, heapMB: +(heapBytes() / 1048576).toFixed(1),
             backend: BACKEND, main: mainSnapshot(), tail: lines.slice(-8),
             ...(ticks <= 6 ? { head } : {}) });
  }, 5000);
}

document.getElementById('logtoggle').onclick = () => logEl.classList.toggle('hidden');

// The A/B run. The window is anchored to the guest's own clock, not to wall
// time: Null reaches a given moment of the attract loop sooner than OpenGL
// does, so equal wall-clock windows measure different scenes and the answer is
// whatever the game happened to be showing. Equal *tick* ranges are the same
// scene by construction, which is the discipline a savestate would normally
// provide -- and savestates do not load on wasm32.
//
// Speed is then computed here rather than read off the emulator: guest seconds
// covered over wall seconds spent is exactly the question, with no smoothing.
let abState = 0;  // 0 before the window, 2 inside it, 3 done
let abTps = 0, abStartTicks = 0, abStartWall = 0;
const abSamples = [];

// The guest's own clock, read off the lines the emulator prints. `abWatch`
// parses the same two lines for its window; this one is separate because the
// acts timeline has to work when ?ab is not in play.
let guestHz = 0;
let guestSeconds = 0;
const actsFired = [];

function actsWatch(text) {
  const clk = /guest clock (\d+) Hz/.exec(text);
  if (clk) { guestHz = +clk[1]; return; }
  const m = /^\[perf\].*cpu=0 ticks=(\d+)/.exec(text);
  if (!m || !guestHz) return;
  guestSeconds = +m[1] / guestHz;
  for (const a of ACTS) {
    if (a.done || guestSeconds < a.at) continue;
    a.done = true;
    // Kept for the ab-result. A guest-anchored window says *when* it opened; it
    // does not say the game is where the timeline meant to put it. Two builds
    // measured at guest 125-155 came back at 84% and 209% -- sample ranges
    // 64-106 and 189-228, the level and the menus -- and nothing in either
    // result said which was which. The wall clock each act fired at is the
    // cheapest evidence there is: it costs nothing and it travels with the
    // number it qualifies.
    actsFired.push({ c: a.control, g: +guestSeconds.toFixed(1),
                     w: +((performance.now() - T0) / 1000).toFixed(1) });
    // Deferred: this runs from inside log(), and logging from there re-enters it.
    const note = `[page] act: control ${a.control} at guest ${guestSeconds.toFixed(0)} s`;
    setTimeout(() => log(note), 0);
    if (!setControl) continue;
    setControl(a.control, 1);
    // Release on the guest clock, not the wall clock. A setTimeout hold lasts
    // the same number of milliseconds whatever speed the emulator is running
    // at, so a build at 60% holds the stick for 60% as many guest frames as one
    // at 100% -- and two builds given the same timeline end up with the skater
    // somewhere different. That is why cross-build screenshots of the "same"
    // guest second were still different scenes.
    a.release = guestSeconds + a.hold / 1000;
    holding.push(a);
  }
  for (let i = holding.length - 1; i >= 0; i--) {
    if (guestSeconds < holding[i].release) continue;
    setControl(holding[i].control, 0);
    holding.splice(i, 1);
  }
}
const holding = [];

function abWatch(text) {
  if (abState === 3) return;
  const clk = /guest clock (\d+) Hz/.exec(text);
  if (clk) { abTps = +clk[1]; return; }
  const m = /^\[perf\].*?(\d+)% speed.*cpu=0 ticks=(\d+)/.exec(text);
  if (!m || !abTps) return;
  const ticks = +m[2];
  const from = abTps * AB_FROM;
  const to = abTps * (AB_FROM + AB);
  if (abState === 0) {
    if (ticks < from) return;
    abState = 2;
    abStartTicks = ticks;
    abStartWall = performance.now();
    log(`[page] ab: ${AB_BACKEND} window open at guest ${(ticks / abTps).toFixed(1)} s`);
    return;
  }
  abSamples.push(+m[1]);
  if (ticks >= to) abFinish(ticks);
}

function abFinish(ticks) {
  abState = 3;
  const guest = (ticks - abStartTicks) / abTps;
  const wall = (performance.now() - abStartWall) / 1000;
  const s = abSamples.slice().sort((a, b) => a - b);
  const at = (q) => (s.length ? s[Math.min(s.length - 1, Math.floor(s.length * q))] : 0);
  const result = {
    phase: 'ab-result', abBackend: AB_BACKEND,
    speed: +(100 * guest / wall).toFixed(1),   // the number: guest seconds per wall second
    guest: +guest.toFixed(1), wall: +wall.toFixed(1),
    window: `${AB_FROM}-${AB_FROM + AB}s`,
    samples: s.length, median: at(0.5), p25: at(0.25), p75: at(0.75),
    min: s[0] ?? 0, max: s[s.length - 1] ?? 0,
    // Which of the timeline's presses actually happened before the window
    // opened, and at what wall second. Two builds can share a guest window and
    // still be in different scenes; this is what says so.
    acts: actsFired.slice(),
  };
  log('[page] ab: ' + JSON.stringify(result));
  report(result);
  if (AB_PHASE === 1) {
    const next = new URL(location.href);
    next.searchParams.set('abphase', '2');
    next.searchParams.set('auto', '1');
    status('ab: reloading for OpenGL');
    // A beat for the report to leave before the page goes away.
    setTimeout(() => location.replace(next.toString()), 1500);
  } else {
    status(`ab done: ${result.speed}%`);
  }
}

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
  // ?savestate=<name>:<seconds> writes a state after N seconds of run time and
  // uploads it next to the disc, so later runs can start from it with
  // ?env=DOLWEB_STATE=/game/<name>.sav instead of booting and driving the game.
  //
  // That is the only way to measure anything small here: a run that boots and
  // presses buttons on a timer ends up with the skater somewhere different every
  // time, and the same build measured 51.2% and 66.0% an hour apart. Three
  // interleaved pairs could not separate a renderer change from that noise.
  log(`[state] savestate param = ${JSON.stringify(SAVESTATE)}`);
  if (SAVESTATE) {
    const [stateName, stateAfter] = SAVESTATE.split(':');
    log(`[state] will poll /user/${stateName}.sav from ${stateAfter}s`);
    const path = `/user/${stateName}.sav`;
    // Poll rather than time it. The emulator's own timer starts when Run()
    // does, which is several seconds after the module exists, so a single
    // timeout here read the file about two seconds before it was written and
    // reported "nothing at /user/level.sav" twice.
    setTimeout(async () => {
      let ptr = 0, size = 0;
      for (let i = 0; i < 60 && !size; i++) {
        try {
          ptr = module.ccall('dolweb_read_file', 'number', ['string'], [path]);
          size = module.ccall('dolweb_last_read_size', 'number', [], []);
        } catch (e) { log(`[state] ccall failed: ${e}`); return; }
        if (!size) await new Promise((r) => setTimeout(r, 2000));
      }
      if (!ptr || !size) { log(`[state] nothing at ${path} after 120 s`); return; }
      // Copy into a plain, non-shared buffer and hand fetch a Blob. The heap
      // here is a SharedArrayBuffer (it has to be: the emulator is threaded),
      // and Safari rejects a body backed by shared memory with a bare
      // "TypeError: Load failed" that says nothing about the cause.
      const bytes = new Uint8Array(size);
      bytes.set(module.HEAPU8.subarray(ptr, ptr + size));
      const body = new Blob([bytes]);
      module.ccall('dolweb_free', null, ['number'], [ptr]);
      try {
        await fetch('/savestate', { method: 'POST', body,
                                    headers: { 'X-State-Name': `${stateName}.sav` } });
        log(`[state] uploaded ${size} bytes as ${stateName}.sav`);
      } catch (e) { log(`[state] upload failed: ${e}`); }
    }, (+stateAfter + 8) * 1000);
  }
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
  let exts = [];
  try {
    const probe = document.createElement('canvas').getContext('webgl2');
    webgl2 = !!probe;
    if (probe) {
      const info = probe.getExtension('WEBGL_debug_renderer_info');
      renderer = info ? probe.getParameter(info.UNMASKED_RENDERER_WEBGL) : 'masked';
      // Which extensions this engine actually offers, because Dolphin decides
      // its whole vertex-upload strategy on one of them and asks for it by a
      // name WebGL never uses. StreamBuffer::Create falls back to "upload
      // everything to offset zero" when bSupportsGLBaseVertex is false, which is
      // the 1197 glBufferData calls a level frame issues for its 466 draws --
      // and that flag is set by looking for GL_ARB/EXT/OES_draw_elements_base_
      // vertex, none of which a WebGL context reports even when
      // WEBGL_draw_instanced_base_vertex_base_instance is right there.
      exts = probe.getSupportedExtensions() || [];
    }
  } catch (e) { /* reported as false */ }
  return {
    exts,
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

// What the browser makes of a ranged probe, which is the single decision that
// says whether the disc is read in pieces or downloaded whole. Emscripten's
// fetch backend asks with a HEAD and then reads three things off the response;
// this reports the same three, because Chrome and Safari did not agree and
// the difference was 62 MB against 1.4 GB per boot.
async function reportRangeSupport() {
  try {
    const url = `${GAME}/sys/main.dol`;
    const r = await fetch(url, { method: 'HEAD', headers: { Range: 'bytes=0-' } });
    const seen = { phase: 'range-probe', status: r.status, ok: r.ok,
                   contentLength: r.headers.get('Content-Length'),
                   acceptRanges: r.headers.get('Accept-Ranges'),
                   contentRange: r.headers.get('Content-Range') };
    log('[page] ' + JSON.stringify(seen));
    report(seen);
  } catch (e) {
    log('[page] range probe failed: ' + e);
  }
}

async function boot() {
  const caps = capabilities();
  log('[page] ' + JSON.stringify(caps));
  report({ phase: 'capabilities', ...caps });
  reportRangeSupport();
  if (!caps.webgl2)
    log('[page] no WebGL2: the OpenGL backend cannot start. Try ?backend=Null.');
  startBtn.disabled = true;
  status('loading module');
  loading('Downloading emulator', 'connecting');
  try {
    await loadModuleScript();
  } catch (e) {
    log(`[page] ${e}`);
    loading('Failed to load', String(e));
    return;
  }

  if (!crossOriginIsolated) {
    log('[page] crossOriginIsolated is false: SharedArrayBuffer is unavailable ' +
        'and the emulator cannot start its threads. Serve with COOP/COEP.');
    status('not cross-origin isolated');
    loading('Cannot start', 'not cross-origin isolated: SharedArrayBuffer is unavailable');
    report({ phase: 'not-isolated' });
    return;
  }

  const module = await createDolWeb({
    canvas,
    // Only ?log= asks for the log. See LOG above for what it cost when
    // ?report=1 asked for it too.
    arguments: [GAME, '/user', BACKEND, SECONDS, ...ENV,
                ...(LOG && !ENV.some((e) => e.startsWith('DOLWEB_DEBUG_LOG'))
                    ? [`DOLWEB_DEBUG_LOG=${LOG}`] : [])],
    print: log,
    printErr: log,
    onTitle: (t) => { document.title = t; status(t); },
    instantiateWasm: instantiateCounted,
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
  // Not hidden yet: the emulator is up but the game has not drawn anything, and
  // the boot sequence is black for a good while. bootWatch() takes it from here
  // and clears the overlay on the first frame the game actually renders.
  loading('Booting the game', 'mounting the disc');
}

startBtn.onclick = boot;
// Straight away if the document is already up: this script is the last thing in
// the body, so waiting for `load` is a race with the module's own fetch.
if (AUTO) {
  if (document.readyState === 'complete') boot();
  else addEventListener('load', boot);
}
