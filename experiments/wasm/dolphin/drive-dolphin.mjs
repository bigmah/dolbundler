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

const opt = { seconds: 60, backend: 'OGL', shot: 'frame.png', port: 8712,
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
cdp.on((m) => {
  if (m.method === 'Runtime.consoleAPICalled')
    lines.push(m.params.args.map((a) => a.value ?? a.description ?? '').join(' '));
  if (m.method === 'Log.entryAdded')
    lines.push('[browser] ' + m.params.entry.text);
  if (m.method === 'Runtime.exceptionThrown')
    lines.push('[exception] ' + (m.params.exceptionDetails.exception?.description ||
                                 m.params.exceptionDetails.text));
});

const env = (opt.env ? String(opt.env).split(';') : [])
  .map((e) => `&env=${encodeURIComponent(e)}`).join('');
const url = `http://127.0.0.1:${opt.port}/index.html?backend=${opt.backend}` +
            `&seconds=${opt.seconds}${env}`;
await cdp.send('Emulation.setDeviceMetricsOverride',
  { width: +opt.width, height: +opt.height, deviceScaleFactor: 1, mobile: false });
await cdp.send('Page.navigate', { url });
await sleep(1500);
// The page waits for a click so that audio and the module load are user-driven.
await cdp.eval("document.getElementById('start').click(); 'clicked'");

// Screenshots as it goes, not only at the end: a game is somewhere different
// every second and one picture of a fade-to-black says nothing.
const every = opt.shotEvery ? +opt.shotEvery * 1000 : 0;
let nextShot = every ? Date.now() + every : Infinity;
let shotIndex = 0;
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
  done = lines.some((l) => l.includes('Run() returned'));
}

if (opt.cat) {
  try { await cdp.eval(`cat(${JSON.stringify(opt.cat)}); 'ok'`); } catch (e) {
    console.error('cat failed: ' + e.message);
  }
  await sleep(500);
}

const shot = await cdp.send('Page.captureScreenshot', { format: 'png' });
writeFileSync(opt.shot, Buffer.from(shot.data, 'base64'));

const perf = lines.filter((l) => l.startsWith('[perf]'));
console.log(lines.join('\n'));
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
setTimeout(() => { try { rmSync(profile, { recursive: true, force: true }); } catch (e) {} },
           500).unref();
process.exit(0);
