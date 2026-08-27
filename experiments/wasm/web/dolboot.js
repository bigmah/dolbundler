// Driving the guest from a browser: disc, boot, run loop, input, audio.
//
// GXRuntime's Run() blocks and a browser cannot, so the loop here calls
// dolweb_step() with a wall-clock budget and renders whenever the guest reaches
// present(). Everything the guest needs from the host that is not a draw --
// disc bytes, pad state, audio sink -- is wired up in this file.

'use strict';

// PAD button bits (graphics/aurora/include/dolphin/pad.h).
const PAD = {
  LEFT: 0x0001, RIGHT: 0x0002, DOWN: 0x0004, UP: 0x0008,
  Z: 0x0010, R: 0x0020, L: 0x0040,
  A: 0x0100, B: 0x0200, X: 0x0400, Y: 0x0800, START: 0x1000,
};

const KEYMAP = {
  ArrowLeft: PAD.LEFT, ArrowRight: PAD.RIGHT, ArrowDown: PAD.DOWN, ArrowUp: PAD.UP,
  KeyJ: PAD.A, KeyK: PAD.B, KeyU: PAD.X, KeyI: PAD.Y,
  KeyQ: PAD.Z, KeyE: PAD.L, KeyR: PAD.R, Enter: PAD.START,
};
const AXISMAP = {
  KeyA: ['x', -1], KeyD: ['x', 1], KeyW: ['y', 1], KeyS: ['y', -1],
  KeyF: ['cx', -1], KeyH: ['cx', 1], KeyT: ['cy', 1], KeyG: ['cy', -1],
};

// --- persistence ------------------------------------------------------------
//
// Origin-private storage, so a phone does not have to be handed a 1.4 GB file
// every time the page loads, and so a save survives a reload. Everything here
// degrades to "no persistence" rather than failing: a private window has no
// OPFS, and the emulator must still run.

const OPFS_DISC = 'disc.iso';
const OPFS_CARD = 'card.dolcard';

async function opfsRoot() {
  if (!navigator.storage || !navigator.storage.getDirectory) return null;
  try { return await navigator.storage.getDirectory(); } catch (e) { return null; }
}

async function opfsRead(name) {
  const root = await opfsRoot();
  if (!root) return null;
  try {
    const handle = await root.getFileHandle(name);
    const file = await handle.getFile();
    return new Uint8Array(await file.arrayBuffer());
  } catch (e) { return null; }  // absent is the common case, not an error
}

async function opfsWrite(name, bytes) {
  const root = await opfsRoot();
  if (!root) return false;
  try {
    const handle = await root.getFileHandle(name, { create: true });
    const w = await handle.createWritable();
    await w.write(bytes);
    await w.close();
    return true;
  } catch (e) { return false; }
}

// Copy a picked File straight into OPFS without ever holding the whole thing in
// a JS array: a 1.4 GB disc read with arrayBuffer() is a 1.4 GB spike on a
// device that has no room for it.
async function opfsStoreFile(name, file, onProgress) {
  const root = await opfsRoot();
  if (!root) return false;
  const handle = await root.getFileHandle(name, { create: true });
  const w = await handle.createWritable();
  const reader = file.stream().getReader();
  let done = 0;
  for (;;) {
    const chunk = await reader.read();
    if (chunk.done) break;
    await w.write(chunk.value);
    done += chunk.value.length;
    if (onProgress) onProgress(done, file.size);
  }
  await w.close();
  return true;
}

// A GameCube disc, wherever its bytes live. The emulator only ever asks for a
// synchronous copy into WASM memory, so anything that can answer that
// synchronously can back a disc -- an ArrayBuffer today, an OPFS sync access
// handle in a worker later.
class ArrayBufferDisc {
  constructor(bytes) { this.bytes = bytes; }
  get size() { return this.bytes.length; }
  read(offset, size, heap, dest) {
    if (offset >= this.bytes.length) return 0;
    const end = Math.min(offset + size, this.bytes.length);
    heap.set(this.bytes.subarray(offset, end), dest);
    return end - offset;
  }
}

function be32(u8, off) {
  return ((u8[off] << 24) | (u8[off + 1] << 16) | (u8[off + 2] << 8) | u8[off + 3]) >>> 0;
}

// Pull main.dol out of a GameCube disc image. boot.bin's word at 0x420 is the
// DOL's disc offset; the DOL's own header gives its extent.
// A tiny stand-in for the WASM heap, so the same DiscSource.read() serves both
// the emulator (copying into linear memory) and this parse.
function readInto(disc, offset, size) {
  const buf = new Uint8Array(size);
  const got = disc.read(offset, size, { set: (src, dst) => buf.set(src, dst) }, 0);
  if (got < size) throw new Error(`short disc read at ${offset}: ${got}/${size}`);
  return buf;
}

function extractDol(disc) {
  // boot.bin is 0x440 bytes and the DOL offset lives at 0x420 -- reading only a
  // 0x400 header puts that word past the end and silently yields NaN.
  const head = readInto(disc, 0, 0x440);
  if (be32(head, 0x1C) !== 0xC2339F3D) throw new Error('not a GameCube disc image');
  const dolOffset = be32(head, 0x420);
  if (!dolOffset || dolOffset >= disc.size)
    throw new Error(`bad main.dol offset 0x${dolOffset.toString(16)}`);

  const dolHeader = readInto(disc, dolOffset, 0x100);
  let extent = 0x100;
  for (let i = 0; i < 18; i++) {
    const off = be32(dolHeader, i * 4);
    const size = be32(dolHeader, 0x90 + i * 4);
    if (off !== 0 && size !== 0) extent = Math.max(extent, off + size);
  }
  const dol = readInto(disc, dolOffset, extent);
  return { dol, gameId: String.fromCharCode(...head.subarray(0, 6)),
           title: String.fromCharCode(...head.subarray(0x20, 0x60)).replace(/\0.*$/, '') };
}

class DolWebSession {
  constructor(opts) {
    this.canvas = opts.canvas;
    this.log = opts.log || (() => {});
    this.onStats = opts.onStats || (() => {});
    this.blockBudget = opts.blockBudget || 20000000;
    this.msBudget = opts.msBudget || 12;
    this.running = false;
    this.frames = 0;
    this.startTime = 0;
    this.pad = { buttons: 0, x: 0, y: 0, cx: 0, cy: 0, l: 0, r: 0 };
    this.touchButtons = 0;
    this.keyButtons = 0;
    this.keyAxes = { x: 0, y: 0, cx: 0, cy: 0 };
    this.stickTouch = { x: 0, y: 0 };
  }

  // `allowNoRenderer` keeps the session usable where WebGPU is absent -- the iOS
  // Simulator exposes navigator.gpu and then returns null from requestAdapter,
  // so it is the only place to measure guest throughput under real iOS WebKit.
  async load(moduleFactory, allowNoRenderer) {
    this.M = await moduleFactory();
    this.renderer = new DolWebRenderer(this.M, this.canvas);
    try {
      await this.renderer.init();
      this.log(`WebGPU ready (${this.renderer.adapterInfo.vendor || 'adapter'} ` +
               `${this.renderer.adapterInfo.architecture || ''})`, 'ok');
    } catch (e) {
      if (!allowNoRenderer) throw e;
      this.renderer = null;
      this.log('no WebGPU: ' + e.message + ' - running headless', 'bad');
    }
    return this;
  }

  attachDisc(disc) {
    this.disc = disc;
    const M = this.M;
    // The C side calls this synchronously through EM_JS with a raw heap
    // pointer; copying straight into HEAPU8 keeps the disc out of the module's
    // own memory, which is the thing a phone cannot spare.
    M.discRead = (offset, size, dest) => disc.read(offset, size, M.HEAPU8, dest);
    const info = extractDol(disc);
    M.FS.writeFile('/main.dol', info.dol);
    this.discInfo = info;
    this.log(`disc: ${info.gameId} "${info.title}" (${(disc.size / 1048576) | 0} MB), ` +
             `main.dol ${(info.dol.length / 1024) | 0} KB`, 'ok');
    return info;
  }

  // Restore a saved memory card into MEMFS before boot, if one is stored.
  // Smaller than any real memory-card image (a 4 Mbit card is 512 KB), so this
  // rejects the stub the runtime leaves behind when the guest never saved --
  // handing that back to hle_card_open is worse than handing it nothing.
  static get MIN_CARD_BYTES() { return 64 * 1024; }

  async restoreCard() {
    const bytes = await opfsRead(OPFS_CARD);
    if (!bytes || bytes.length < DolWebSession.MIN_CARD_BYTES) return false;
    try {
      this.M.FS.writeFile('/card.dolcard', bytes);
      this.log(`memory card restored (${bytes.length} bytes)`, 'ok');
      return true;
    } catch (e) { return false; }
  }

  // The card lives in MEMFS while the game runs; copy it out periodically so a
  // save is not lost with the tab. Cheap: a 4 Mbit card is 512 KB.
  async persistCard() {
    try {
      const bytes = this.M.FS.readFile('/card.dolcard');
      if (!bytes || bytes.length < DolWebSession.MIN_CARD_BYTES)
        return false;  // a stub, not a save
      return await opfsWrite(OPFS_CARD, bytes);
    } catch (e) { return false; }
  }

  boot() {
    if (!this.M._dolweb_boot(this.disc ? 1 : 0)) throw new Error('boot failed');
    this.log('booted', 'ok');
    this.startTime = performance.now();
    this.running = true;
  }

  async startAudio() {
    if (this.audioCtx) return;
    const rate = this.M._dolweb_audio_sample_rate();
    const ctx = new AudioContext({ sampleRate: rate });
    await ctx.audioWorklet.addModule('audio-worklet.js');
    const node = new AudioWorkletNode(ctx, 'dol-audio', { outputChannelCount: [2] });
    node.connect(ctx.destination);
    await ctx.resume();
    this.audioCtx = ctx;
    this.audioNode = node;
    this.audioStage = this.M._dolweb_alloc(4096 * 4); // s16 stereo frames
    this.log(`audio: ${rate} Hz worklet`, 'ok');
  }

  pumpAudio() {
    if (!this.audioNode) return;
    const M = this.M;
    let avail = M._dolweb_audio_available();
    while (avail > 0) {
      const want = Math.min(avail, 4096);
      const got = M._dolweb_audio_pull(this.audioStage, want);
      if (got === 0) break;
      const src = new Int16Array(M.HEAPU8.buffer, this.audioStage, got * 2);
      const f32 = new Float32Array(got * 2);
      for (let i = 0; i < got * 2; i++) f32[i] = src[i] / 32768;
      this.audioNode.port.postMessage({ samples: f32 }, [f32.buffer]);
      avail -= got;
    }
  }

  pushPad() {
    const buttons = this.keyButtons | this.touchButtons;
    const x = Math.max(-1, Math.min(1, this.keyAxes.x + this.stickTouch.x));
    const y = Math.max(-1, Math.min(1, this.keyAxes.y + this.stickTouch.y));
    const clamp = (v) => Math.max(-128, Math.min(127, Math.round(v * 100)));
    this.M._dolweb_set_pad(0, buttons, clamp(x), clamp(y),
                           clamp(this.keyAxes.cx), clamp(this.keyAxes.cy),
                           (buttons & PAD.L) ? 255 : 0, (buttons & PAD.R) ? 255 : 0);
  }

  step() {
    if (!this.running) return false;
    this.pushPad();
    const r = this.M._dolweb_step(this.blockBudget, this.msBudget);
    if (r === 1) {
      if (this.renderer) this.renderer.renderFrame();
      else this.M._dol_web_begin_frame();  // nobody consumed it; drop the arenas
      this.frames++;
      this.pumpAudio();
    } else if (r < 0) {
      this.running = false;
      this.log('stopped: ' + this.M.UTF8ToString(this.M._dolweb_stop_reason()), 'bad');
    }
    return this.running;
  }

  stats() {
    const elapsed = (performance.now() - this.startTime) / 1000;
    // Counters the module keeps as u64 arrive as BigInt (emscripten links with
    // WASM_BIGINT). JSON.stringify throws on those, which silently killed the
    // whole run loop the first time this reported anything.
    const num = (v) => (typeof v === 'bigint' ? Number(v) : v);
    return {
      frames: this.frames,
      fps: elapsed > 0 ? this.frames / elapsed : 0,
      blocks: num(this.M._dolweb_blocks()),
      pc: this.M._dolweb_guest_pc() >>> 0,
      presents: num(this.M._dolweb_present_count()),
      planned: this.M._dolweb_frame_draws(),
      planSkipped: this.M._dolweb_frame_skipped(),
      frontendFailed: this.M._dolweb_frontend_failed() !== 0,
      audio: {
        rate: this.M._dolweb_audio_sample_rate(),
        queued: this.M._dolweb_audio_available(),
        dropped: num(this.M._dolweb_audio_dropped()),
        started: !!this.audioCtx,
      },
      gpu: this.renderer ? Object.assign({}, this.renderer.stats) : null,
      texcache: this.M._dolweb_texture_stat ? {
        hits: this.M._dolweb_texture_stat(0),
        uploads: this.M._dolweb_texture_stat(1),
        evictAddress: this.M._dolweb_texture_stat(2),
        evictCap: this.M._dolweb_texture_stat(3),
        efbBinds: this.M._dolweb_texture_stat(4),
        undecodable: this.M._dolweb_texture_stat(5),
      } : null,
      gaps: this.renderer ? this.renderer.gapCounters() : null,
      rendererError: this.renderer ? this.renderer.lastError : 'no WebGPU adapter',
    };
  }

  bindKeyboard(target) {
    const down = (e) => {
      if (KEYMAP[e.code] !== undefined) { this.keyButtons |= KEYMAP[e.code]; e.preventDefault(); }
      const ax = AXISMAP[e.code];
      if (ax) { this.keyAxes[ax[0]] = ax[1]; e.preventDefault(); }
    };
    const up = (e) => {
      if (KEYMAP[e.code] !== undefined) this.keyButtons &= ~KEYMAP[e.code];
      const ax = AXISMAP[e.code];
      if (ax && this.keyAxes[ax[0]] === ax[1]) this.keyAxes[ax[0]] = 0;
    };
    target.addEventListener('keydown', down);
    target.addEventListener('keyup', up);
  }

  // The Gamepad API covers a real controller; it is polled, not evented.
  pollGamepad() {
    const pads = navigator.getGamepads ? navigator.getGamepads() : [];
    const gp = pads && pads[0];
    if (!gp) return;
    let b = 0;
    const p = (i) => gp.buttons[i] && gp.buttons[i].pressed;
    if (p(0)) b |= PAD.A;
    if (p(1)) b |= PAD.B;
    if (p(2)) b |= PAD.X;
    if (p(3)) b |= PAD.Y;
    if (p(4)) b |= PAD.L;
    if (p(5)) b |= PAD.R;
    if (p(7)) b |= PAD.Z;
    if (p(9)) b |= PAD.START;
    if (p(12)) b |= PAD.UP;
    if (p(13)) b |= PAD.DOWN;
    if (p(14)) b |= PAD.LEFT;
    if (p(15)) b |= PAD.RIGHT;
    this.touchButtons = (this.touchButtons & ~0xFFFF) | b;
    if (gp.axes.length >= 2) {
      this.stickTouch.x = Math.abs(gp.axes[0]) > 0.15 ? gp.axes[0] : 0;
      this.stickTouch.y = Math.abs(gp.axes[1]) > 0.15 ? -gp.axes[1] : 0;
    }
  }
}

if (typeof window !== 'undefined') {
  window.DolWebSession = DolWebSession;
  window.ArrayBufferDisc = ArrayBufferDisc;
  window.DOLWEB_PAD = PAD;
  window.DOLWEB_OPFS = { read: opfsRead, write: opfsWrite, storeFile: opfsStoreFile,
                         DISC: OPFS_DISC, CARD: OPFS_CARD };
}
