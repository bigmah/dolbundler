#!/usr/bin/env node
// Drive the browser build through the DevTools protocol, with a real touch
// screen.
//
// run-headless.sh can start a page and read what it POSTs back, but it cannot
// touch anything: the on-screen pad is the one part of this build that only
// exists under a finger, and "looks right in a screenshot" is not the same as
// "the guest saw the button". This opens a CDP session, emulates a phone
// (viewport, device pixel ratio, touch), synthesises touches at real screen
// coordinates and reads the resulting pad state back out of the page.
//
//   node drive.mjs pad            # exercise every control, assert the bits
//   node drive.mjs shot out.png   # screenshot at the emulated size
//   node drive.mjs eval 'expr'    # evaluate in the page and print the result
//
// Options: --url, --width, --height, --dpr, --landscape, --headed, --wait
import { spawn } from 'node:child_process';
import { mkdtempSync, rmSync } from 'node:fs';
import { writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const CHROME = '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';

function args(argv) {
  const o = { width: 932, height: 430, dpr: 3, headed: false, wait: 4000,
              url: 'http://127.0.0.1:8712/index.html?pad=1&immersive=1' };
  const rest = [];
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === '--headed') o.headed = true;
    else if (a === '--landscape') { o.width = 932; o.height = 430; }
    else if (a === '--portrait') { o.width = 430; o.height = 932; }
    else if (a.startsWith('--')) o[a.slice(2)] = argv[++i];
    else rest.push(a);
  }
  o.width = +o.width; o.height = +o.height; o.dpr = +o.dpr; o.wait = +o.wait;
  return [o, rest];
}

class CDP {
  constructor(ws) { this.ws = ws; this.id = 0; this.pending = new Map();
                    ws.onmessage = (e) => this.onMessage(JSON.parse(e.data)); }
  onMessage(m) {
    const p = this.pending.get(m.id);
    if (!p) return;
    this.pending.delete(m.id);
    m.error ? p.reject(new Error(JSON.stringify(m.error))) : p.resolve(m.result);
  }
  send(method, params = {}) {
    const id = ++this.id;
    this.ws.send(JSON.stringify({ id, method, params }));
    return new Promise((resolve, reject) => this.pending.set(id, { resolve, reject }));
  }
  async eval(expression) {
    const r = await this.send('Runtime.evaluate',
      { expression, returnByValue: true, awaitPromise: true });
    if (r.exceptionDetails) throw new Error(r.exceptionDetails.text + ' ' +
      (r.exceptionDetails.exception?.description || ''));
    return r.result.value;
  }
  // One touch point, in CSS pixels. `type` is touchStart/touchMove/touchEnd.
  touch(type, points) {
    return this.send('Input.dispatchTouchEvent', {
      type, touchPoints: points.map((p) => ({ x: p.x, y: p.y, id: p.id ?? 0,
                                              radiusX: 12, radiusY: 12, force: 1 })),
    });
  }
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function connect(o) {
  const profile = mkdtempSync(join(tmpdir(), 'dolweb-drive-'));
  const port = 9222 + (process.pid % 500);
  const chrome = spawn(CHROME, [
    o.headed ? '--new-window' : '--headless=new',
    '--enable-unsafe-webgpu', '--use-angle=metal', '--enable-features=WebGPU',
    '--disable-gpu-sandbox', '--autoplay-policy=no-user-gesture-required',
    '--mute-audio', '--ignore-certificate-errors', '--no-first-run',
    '--no-default-browser-check', `--user-data-dir=${profile}`,
    `--remote-debugging-port=${port}`, 'about:blank',
  ], { stdio: 'ignore' });

  let target = null;
  for (let i = 0; i < 100 && !target; i++) {
    await sleep(100);
    try {
      const list = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
      target = list.find((t) => t.type === 'page');
    } catch (e) { /* not up yet */ }
  }
  if (!target) throw new Error('Chrome never opened a debugging port');

  const ws = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });
  const cdp = new CDP(ws);
  await cdp.send('Runtime.enable');
  await cdp.send('Page.enable');
  // A real phone: exact viewport, device pixel ratio, and a touch screen --
  // without `mobile: true` the page's `(pointer: coarse)` query is false and it
  // never shows the pad at all.
  await cdp.send('Emulation.setDeviceMetricsOverride', {
    width: o.width, height: o.height, deviceScaleFactor: o.dpr, mobile: true,
  });
  await cdp.send('Emulation.setTouchEmulationEnabled', { enabled: true, maxTouchPoints: 5 });
  await cdp.send('Page.navigate', { url: o.url });
  await sleep(o.wait);
  // Chrome is still writing to the profile when kill() returns, so removing it
  // synchronously raises ENOTEMPTY -- which then masks whatever real error sent
  // us into the finally block in the first place.
  return { cdp, cleanup: () => {
    try { ws.close(); } catch (e) {}
    try { chrome.kill(); } catch (e) {}
    setTimeout(() => { try { rmSync(profile, { recursive: true, force: true }); }
                       catch (e) {} }, 500).unref();
  } };
}

// --- commands --------------------------------------------------------------

async function cmdPad(cdp) {
  // Where the page actually put each control, in CSS pixels.
  const geom = await cdp.eval(`JSON.stringify(controls.filter(c => c.geom).map(
      c => ({ id: c.id, x: c.geom.x, y: c.geom.y, r: c.geom.r })))`);
  const controls = JSON.parse(geom);
  const byId = Object.fromEntries(controls.map((c) => [c.id, c]));
  const PAD = await cdp.eval('JSON.stringify(window.DOLWEB_PAD)').then(JSON.parse);
  // No module is loaded in this mode, so the page has no session to write pad
  // state into. Give it a stand-in with the same shape. Assign to the bare
  // name, not to window.session: a top-level `let` lives in the global lexical
  // environment, which shadows the property of the same name.
  await cdp.eval(`if (!session) session = { touchButtons: 0,
      stickTouch: {x:0,y:0}, cstickTouch: {x:0,y:0}, triggerTouch: {l:0,r:0} };
      'ok'`);
  const state = () => cdp.eval(`JSON.stringify({ b: session.touchButtons,
      s: session.stickTouch, c: session.cstickTouch })`).then(JSON.parse);

  let failures = 0;
  const check = (name, ok, detail) => {
    console.log(`  ${ok ? 'ok  ' : 'FAIL'}  ${name}${detail ? '  ' + detail : ''}`);
    if (!ok) failures++;
  };

  console.log(`layout ${controls.length} controls: ` +
              controls.map((c) => c.id).join(' '));

  for (const id of ['A', 'B', 'X', 'Y', 'START', 'L', 'R', 'Z']) {
    const c = byId[id];
    if (!c) { check(id, false, 'not laid out'); continue; }
    await cdp.touch('touchStart', [{ x: c.x, y: c.y, id: 1 }]);
    const down = await state();
    await cdp.touch('touchEnd', [{ x: c.x, y: c.y, id: 1 }]);
    const up = await state();
    check(`${id} press`, (down.b & PAD[id]) !== 0 && (up.b & PAD[id]) === 0,
          `down=0x${down.b.toString(16)} up=0x${up.b.toString(16)}`);
  }

  // Two fingers at once: the stick held while A is tapped. The old pad captured
  // each pointer on its own element, so this is the case that used to break.
  const st = byId.stick, a = byId.A;
  await cdp.touch('touchStart', [{ x: st.x, y: st.y + st.r * 0.8, id: 1 }]);
  const stickOnly = await state();
  await cdp.touch('touchStart', [{ x: st.x, y: st.y + st.r * 0.8, id: 1 },
                                 { x: a.x, y: a.y, id: 2 }]);
  const both = await state();
  await cdp.touch('touchEnd', [{ x: st.x, y: st.y + st.r * 0.8, id: 1 }]);
  const none = await state();
  await cdp.touch('touchEnd', [{ x: a.x, y: a.y, id: 2 }]);
  check('stick down', stickOnly.s.y < -0.3, `y=${stickOnly.s.y.toFixed(2)}`);
  check('stick + A together', (both.b & PAD.A) !== 0 && both.s.y < -0.3,
        `b=0x${both.b.toString(16)} y=${both.s.y.toFixed(2)}`);
  check('stick releases to centre', none.s.x === 0 && none.s.y === 0,
        `x=${none.s.x} y=${none.s.y}`);

  // The stick also has to emit D-pad bits, because a lot of menus only read
  // those. Push it right and look for RIGHT.
  await cdp.touch('touchStart', [{ x: st.x + st.r * 0.9, y: st.y, id: 3 }]);
  const right = await state();
  await cdp.touch('touchEnd', [{ x: st.x + st.r * 0.9, y: st.y, id: 3 }]);
  const released = await state();
  check('stick emits D-pad RIGHT', (right.b & PAD.RIGHT) !== 0,
        `b=0x${right.b.toString(16)}`);
  check('D-pad clears on release', (released.b & 0x0f) === 0,
        `b=0x${released.b.toString(16)}`);

  // Rolling a thumb from B onto A should end with A held and B released.
  const b = byId.B;
  await cdp.touch('touchStart', [{ x: b.x, y: b.y, id: 4 }]);
  await cdp.touch('touchMove', [{ x: a.x, y: a.y, id: 4 }]);
  const rolled = await state();
  await cdp.touch('touchEnd', [{ x: a.x, y: a.y, id: 4 }]);
  check('roll B -> A', (rolled.b & PAD.A) !== 0 && (rolled.b & PAD.B) === 0,
        `b=0x${rolled.b.toString(16)}`);

  // Nothing on the page may zoom, and nothing may scroll.
  const zoom = await cdp.eval(`JSON.stringify({ vv: visualViewport ? visualViewport.scale : 1,
      sx: scrollX, sy: scrollY, bodyH: document.body.scrollHeight,
      innerH: innerHeight })`).then(JSON.parse);
  check('no zoom', zoom.vv === 1, `scale=${zoom.vv}`);
  check('no page scroll', zoom.sx === 0 && zoom.sy === 0 &&
        zoom.bodyH <= zoom.innerH + 1, JSON.stringify(zoom));

  // And every control has to be inside the viewport, not half off the edge.
  const off = controls.filter((c) => c.x - c.r < -1 || c.y - c.r < -1);
  check('all controls on screen', off.length === 0,
        off.map((c) => c.id).join(',') || '');

  return failures;
}

async function main() {
  const [o, rest] = args(process.argv.slice(2));
  const cmd = rest[0] || 'pad';
  const { cdp, cleanup } = await connect(o);
  let code = 0;
  try {
    if (cmd === 'pad') {
      console.log(`--- pad, ${o.width}x${o.height} @${o.dpr}x ---`);
      code = (await cmdPad(cdp)) ? 1 : 0;
      console.log(code ? 'FAILURES' : 'all checks passed');
    } else if (cmd === 'shot') {
      const r = await cdp.send('Page.captureScreenshot', { format: 'png' });
      writeFileSync(rest[1] || 'shot.png', Buffer.from(r.data, 'base64'));
      console.log('wrote', rest[1] || 'shot.png');
    } else if (cmd === 'eval') {
      console.log(await cdp.eval(rest.slice(1).join(' ')));
    } else {
      console.error('unknown command: ' + cmd);
      code = 2;
    }
  } finally { cleanup(); }
  process.exit(code);
}
main().catch((e) => { console.error(e); process.exit(1); });
