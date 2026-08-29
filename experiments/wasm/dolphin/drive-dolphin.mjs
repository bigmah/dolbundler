#!/usr/bin/env node
// Drive the Dolphin wasm page in headless Chrome: start it, collect its log,
// screenshot the canvas, and report the emulation speed it reached.
//
//   node drive-dolphin.mjs                       # 60 s, OGL, screenshot
//   node drive-dolphin.mjs --seconds 30 --backend Null
//   node drive-dolphin.mjs --shot out.png
//
// Chrome, not Safari: Safari has no headless mode. What Chrome proves here is
// that the emulator runs in a browser at all; the phone is checked by hand.
import { spawn } from 'node:child_process';
import { mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const CHROME = '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const opt = { seconds: 60, backend: 'OGL', shot: 'frame.png', port: 8712, host: 'http://127.0.0.1',
              width: 960, height: 720, headed: false };
for (let i = 2; i < process.argv.length; i++) {
  const a = process.argv[i];
  if (a === '--headed') opt.headed = true;
  else if (a.startsWith('--')) opt[a.slice(2)] = process.argv[++i];
}
opt.seconds = +opt.seconds;

class CDP {
  constructor(ws) {
    this.ws = ws; this.id = 0; this.pending = new Map(); this.handlers = [];
    ws.onmessage = (e) => {
      const m = JSON.parse(e.data);
      if (m.id && this.pending.has(m.id)) {
        const p = this.pending.get(m.id);
        this.pending.delete(m.id);
        m.error ? p.reject(new Error(JSON.stringify(m.error))) : p.resolve(m.result);
      } else if (m.method) {
        for (const h of this.handlers) h(m);
      }
    };
  }
  send(method, params = {}) {
    const id = ++this.id;
    this.ws.send(JSON.stringify({ id, method, params }));
    return new Promise((res, rej) => this.pending.set(id, { resolve: res, reject: rej }));
  }
  on(fn) { this.handlers.push(fn); }
  async eval(expression) {
    const r = await this.send('Runtime.evaluate',
      { expression, returnByValue: true, awaitPromise: true });
    if (r.exceptionDetails) throw new Error(r.exceptionDetails.text);
    return r.result.value;
  }
}

const profile = mkdtempSync(join(tmpdir(), 'dolweb-'));
const dbg = 9333 + (process.pid % 400);
const chrome = spawn(CHROME, [
  opt.headed ? '--new-window' : '--headless=new',
  '--use-angle=metal', '--disable-gpu-sandbox',
  '--disable-background-timer-throttling',
  '--disable-backgrounding-occluded-windows', '--disable-renderer-backgrounding',
  '--disable-gpu-vsync', '--disable-frame-rate-limit',
  '--autoplay-policy=no-user-gesture-required', '--mute-audio',
  '--ignore-certificate-errors', '--no-first-run', '--no-default-browser-check',
  `--user-data-dir=${profile}`, `--remote-debugging-port=${dbg}`, 'about:blank',
], { stdio: 'ignore' });

let target = null;
for (let i = 0; i < 120 && !target; i++) {
  await sleep(100);
  try {
    const list = await (await fetch(`http://127.0.0.1:${dbg}/json/list`)).json();
    target = list.find((t) => t.type === 'page');
  } catch (e) { /* not up yet */ }
}
if (!target) { console.error('Chrome never opened a debugging port'); process.exit(1); }

const ws = new WebSocket(target.webSocketDebuggerUrl);
await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });
const cdp = new CDP(ws);
await cdp.send('Runtime.enable');
await cdp.send('Page.enable');
await cdp.send('Log.enable');

const lines = [];
// Printed as they arrive, not collected and dumped at the end: a run that
// stalls is otherwise indistinguishable from a run that is working, and this
// harness spends minutes at a time with nothing to say.
// The guest's own clock, read off the perf line. Acts can be scheduled against
// it, which is what makes a script survive a build that runs at a twentieth of
// the speed -- an interpreter run reaches the same menu, just twenty minutes
// later, and wall-clock cues would all fire during the logos.
let guestSeconds = 0;
let guestHz = 0;

function note(text) {
  lines.push(text);
  const clk = /guest clock (\d+) Hz/.exec(text);
  if (clk) guestHz = +clk[1];
  const t = /\[perf\].*ticks=(\d+)/.exec(text);
  if (t && guestHz) guestSeconds = +t[1] / guestHz;
  // Every instrument prints under a [tag], so match the shape rather than a
  // list that has to be edited each time one is added -- [tex] and [glerr] were
  // both collected and never shown, which reads exactly like an instrument that
  // found nothing. The list was still a list, and it happened again: [census]
  // ran correctly for a whole afternoon of runs and printed nothing here,
  // which cost a rebuild and three browser runs to chase into the binary
  // before the drop turned out to be on this line. Match ANY bracketed tag.
  if (/^\[[a-z][a-z0-9_-]*\]|exception|error|Failed|audio/i.test(text))
    console.log(text);
  else if (inBlock)
    console.log(text);
  if (/^\[[a-z]+\] ==== /.test(text)) inBlock = !/ end ====/.test(text);
}
// An instrument that prints a *block* -- a shader source, a disassembly -- emits
// body lines with no [tag] on them, and the filter above drops exactly those:
// the first attempt at dumping a fragment shader produced one line per shader
// and a diff of 249 lines against 1. Echo everything between a "[tag] ==== ..."
// opening line and its "==== end ====", so a block instrument survives the trip.
let inBlock = false;

cdp.on((m) => {
  if (m.method === 'Runtime.consoleAPICalled')
    note(m.params.args.map((a) => a.value ?? a.description ?? '').join(' '));
  if (m.method === 'Log.entryAdded')
    note('[browser] ' + m.params.entry.text);
  if (m.method === 'Runtime.exceptionThrown')
    note('[exception] ' + (m.params.exceptionDetails.exception?.description ||
                           m.params.exceptionDetails.text));
});

const env = (opt.env ? String(opt.env).split(';') : [])
  .map((e) => `&env=${encodeURIComponent(e)}`).join('');
// --extra passes query parameters straight through, which is how the page's own
// modes (?ab=N) are reached without teaching this driver about each of them.
const extra = opt.extra ? `&${String(opt.extra).replace(/^&/, '')}` : '';
const url = `${opt.host}:${opt.port}/index.html?backend=${opt.backend}` +
            `&seconds=${opt.seconds}&report=1${env}${opt.pad ? '&pad=1' : ''}${extra}`;
await cdp.send('Emulation.setDeviceMetricsOverride',
  { width: +opt.width, height: +opt.height, deviceScaleFactor: 1, mobile: !!opt.pad });
if (opt.pad)
  await cdp.send('Emulation.setTouchEmulationEnabled', { enabled: true, maxTouchPoints: 5 });
await cdp.send('Page.navigate', { url });
await sleep(1500);
// The page waits for a click so that audio and the module load are user-driven.
await cdp.eval("document.getElementById('start').click(); 'clicked'");

// Screenshots as it goes, not only at the end: a game is somewhere different
// every second and one picture of a fade-to-black says nothing.
// "25:5" taps control 5 at t+25 s of wall clock; "g25:5" waits for guest second
// 25 instead.
const acts = (opt.acts ? String(opt.acts).split(',') : []).map((spec) => {
  const [at, control, hold] = spec.split(':');
  const guest = at.startsWith('g');
  return { at: +(guest ? at.slice(1) : at), guest, control: +control,
           hold: +(hold || 140), done: false };
}).sort((a, b) => a.at - b.at);

// --shotAt g135,g145 screenshots when the *guest* reaches those seconds, and
// names the file after the moment rather than a counter. Wall-clock intervals
// cannot be compared against the desktop build: the two reach a given point in
// the game at different wall times, so "frame 15 here" and "frame 15 there" are
// different scenes, and every cross-build comparison in this project that used
// them had to be thrown away. MODERNGEKKO_SHOT_AT is the desktop's equivalent
// and takes the same guest seconds.
const shotAt = (opt.shotAt ? String(opt.shotAt).split(',') : [])
  .map((v) => ({ at: +String(v).replace(/^g/i, ''), done: false }))
  .sort((a, b) => a.at - b.at);

const every = opt.shotEvery ? +opt.shotEvery * 1000 : 0;
let nextShot = every ? Date.now() + every : Infinity;
let shotIndex = 0;
const startedAt = Date.now();
let audioReported = false;
const deadline = Date.now() + (opt.seconds + 45) * 1000;
let done = false;
while (Date.now() < deadline && !done) {
  await sleep(1000);
  if (Date.now() >= nextShot) {
    nextShot += every;
    const png = await cdp.send('Page.captureScreenshot', { format: 'png' });
    const name = opt.shot.replace(/\.png$/, `-${String(++shotIndex).padStart(2, '0')}.png`);
    writeFileSync(name, Buffer.from(png.data, 'base64'));
    console.log(`[shot] ${name}`);
  }
  for (const sa of shotAt) {
    if (sa.done || guestSeconds < sa.at) continue;
    sa.done = true;
    const png = await cdp.send('Page.captureScreenshot', { format: 'png' });
    const name = opt.shot.replace(/\.png$/, `-g${sa.at}.png`);
    writeFileSync(name, Buffer.from(png.data, 'base64'));
    console.log(`[shot] ${name} at guest ${guestSeconds.toFixed(1)}s`);
  }
  // Hold START (control 5, InputOverrider.h) down for a moment every few
  // seconds, but only while getting through the attract loop: once the game is
  // running, START pauses it, and a paused game draws a black screen that looks
  // exactly like a renderer that stopped working.
  // --acts is a timeline: "22:5,26:0,30:7" taps control 5 at t+22 s, 0 at t+26
  // and 7 at t+30. Holding a button down every second walks straight past any
  // menu that uses it to confirm, which is why reaching a specific screen needs
  // taps at known times rather than a stream of presses.
  if (acts.length) {
    const t = (Date.now() - startedAt) / 1000;
    for (const a of acts) {
      if (a.done || (a.guest ? guestSeconds : t) < a.at) continue;
      a.done = true;
      console.log(`[act] ${a.guest ? 'guest' : 'wall'} ${a.at}s control ${a.control}` +
                  ` (wall ${t.toFixed(0)}s, guest ${guestSeconds.toFixed(0)}s)`);
      await cdp.eval(`(() => { const c = window.dolweb && window.cwrapSetControl;
        if (!c) return 'no'; c(${a.control}, 1);
        setTimeout(() => c(${a.control}, 0), ${a.hold}); return 'ok'; })()`).catch(() => {});
    }
  }
  if (opt.press && Date.now() - startedAt < (+opt.press) * 1000) {
    // START first, then A: START gets past the attract loop and the title, and
    // A is what walks through the menus after it. Alternating is enough to
    // reach a menu screen without knowing the game.
    const control = opt.pressOnly !== undefined ? +opt.pressOnly
                  : (Date.now() - startedAt) < (+opt.press) * 500 ? 5 : 0;
    await cdp.eval(`(() => { const c = window.dolweb && window.cwrapSetControl;
      if (!c) return 'no'; c(${control}, 1); setTimeout(() => c(${control}, 0), 120); return 'ok'; })()`)
      .catch(() => {});
  }
  if (opt.audio && !audioReported &&
      Date.now() - startedAt > (opt.seconds * 1000) * 0.6) {
    audioReported = true;
    // While it is running, not after: the producer stops when the guest does,
    // and a sample taken then measures nothing and looks like silence.
    await reportAudio();
  }
  done = lines.some((l) => l.includes('Run() returned'));
}

async function reportAudio() {
  const a = await cdp.eval("JSON.stringify(window.audioStats ? audioStats() : null)");
  await sleep(3000);
  const b = await cdp.eval("JSON.stringify(window.audioStats ? audioStats() : null)");
  const x = JSON.parse(a), y = JSON.parse(b);
  if (!x || !y) console.log('audio: no stats (the worklet never started)');
  else console.log(`audio: ${y.state}, ${y.contextRate} Hz out / ${y.guestRate} Hz in, ` +
    `produced ${y.written - x.written} frames and consumed ${y.read - x.read} in 3 s ` +
    `(${((y.read - x.read) / 3).toFixed(0)}/s against ${y.guestRate} expected)`);
}

if (opt.cat) {
  try { await cdp.eval(`cat(${JSON.stringify(opt.cat)}); 'ok'`); } catch (e) {
    console.error('cat failed: ' + e.message);
  }
  await sleep(500);
}

const shot = await cdp.send('Page.captureScreenshot', { format: 'png' });
writeFileSync(opt.shot, Buffer.from(shot.data, 'base64'));

// Everything the page said, not just the lines worth printing live: a boot
// narrative is thousands of lines and the one that explains a defect is never
// the one that matched the filter.
if (opt.dumplog) {
  writeFileSync(opt.dumplog, lines.join('\n'));
  console.log(`log: ${opt.dumplog} (${lines.length} lines)`);
}

const perf = lines.filter((l) => l.startsWith('[perf]'));
console.log('---');
console.log(`screenshot: ${opt.shot}`);
if (perf.length) {
  console.log(`last perf: ${perf[perf.length - 1]}`);
  const speeds = perf.map((l) => parseFloat(l.match(/([\d.]+)% speed/)?.[1] ?? '0'));
  const tail = speeds.slice(Math.floor(speeds.length / 2));
  if (tail.length)
    console.log(`median speed over the second half: ` +
      `${tail.sort((a, b) => a - b)[Math.floor(tail.length / 2)].toFixed(0)}%`);
} else {
  console.log('no [perf] lines: the emulator never reached the run loop');
}

try { ws.close(); } catch (e) {}
try { chrome.kill(); } catch (e) {}
// Synchronously, and after waiting for Chrome to actually exit: this used to be
// an unref'd timer, which process.exit never gave a chance to fire. Seventy-two
// abandoned profiles and twenty gigabytes later, it does not do that any more.
for (let i = 0; i < 40 && chrome.exitCode === null && chrome.signalCode === null; i++)
  await sleep(50);
try { rmSync(profile, { recursive: true, force: true }); } catch (e) {}
process.exit(0);
