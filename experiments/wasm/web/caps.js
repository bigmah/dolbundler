// Everything the browser build depends on, probed once and reported as
// (name, value, good) triples. Shared by caps.html and device.html so the
// device run and the desk run answer exactly the same questions.
'use strict';

async function probeCapabilities(put) {
  put('userAgent', navigator.userAgent);
  put('devicePixelRatio', devicePixelRatio);
  put('screen', `${screen.width}x${screen.height}`);
  put('deviceMemory (GB)', navigator.deviceMemory ?? 'unreported');
  put('hardwareConcurrency', navigator.hardwareConcurrency ?? 'unreported');
  put('crossOriginIsolated', self.crossOriginIsolated, self.crossOriginIsolated);
  put('SharedArrayBuffer', typeof SharedArrayBuffer, typeof SharedArrayBuffer === 'function');
  put('WebAssembly', typeof WebAssembly, typeof WebAssembly === 'object');

  // wasm feature probes: tiny modules that only validate when the feature is on.
  const feat = (name, bytes) => {
    let ok = false;
    try { ok = WebAssembly.validate(new Uint8Array(bytes)); } catch (e) {}
    put('wasm ' + name, ok, ok);
    return ok;
  };
  // (module (func (result v128) i32.const 0 i8x16.splat))
  feat('simd128', [0,97,115,109,1,0,0,0,1,5,1,96,0,1,123,3,2,1,0,10,8,1,6,0,65,0,253,15,11]);
  // (module (func (result i64) i64.const 0))  -- always on; sanity check
  feat('i64', [0,97,115,109,1,0,0,0,1,5,1,96,0,1,126,3,2,1,0,10,6,1,4,0,66,0,11]);

  put('AudioWorklet', typeof AudioWorklet, typeof AudioWorklet === 'function');
  put('OffscreenCanvas', typeof OffscreenCanvas, typeof OffscreenCanvas === 'function');
  put('OPFS', !!(navigator.storage && navigator.storage.getDirectory),
      !!(navigator.storage && navigator.storage.getDirectory));
  try {
    const est = await navigator.storage.estimate();
    put('storage quota (MB)', Math.round((est.quota || 0) / 1048576));
    put('storage used (MB)', Math.round((est.usage || 0) / 1048576));
  } catch (e) { put('storage estimate', 'threw: ' + e.message, false); }

  // Round-trip a file through OPFS: the disc and the memory card both live
  // there, and "the API exists" is not the same as "a write survives a read".
  try {
    const root = await navigator.storage.getDirectory();
    const probe = new Uint8Array(1 << 20);
    for (let i = 0; i < probe.length; i += 4093) probe[i] = i & 0xFF;
    const h = await root.getFileHandle('caps-probe.bin', { create: true });
    const w = await h.createWritable();
    await w.write(probe); await w.close();
    const back = new Uint8Array(await (await h.getFile()).arrayBuffer());
    let same = back.length === probe.length;
    for (let i = 0; same && i < probe.length; i += 4093) same = back[i] === probe[i];
    put('OPFS 1 MB round trip', same ? 'identical' : 'differs', same);
    await root.removeEntry('caps-probe.bin');
  } catch (e) { put('OPFS round trip', 'threw: ' + e.message, false); }

  put('navigator.gpu', !!navigator.gpu, !!navigator.gpu);
  if (navigator.gpu) {
    try {
      const adapter = await navigator.gpu.requestAdapter();
      put('requestAdapter()', adapter ? 'adapter' : 'null', !!adapter);
      if (adapter) {
        put('adapter.info', JSON.stringify(adapter.info || {}));
        put('maxBufferSize (MB)', Math.round(adapter.limits.maxBufferSize / 1048576));
        put('maxTextureDimension2D', adapter.limits.maxTextureDimension2D);
        put('maxUniformBufferBindingSize (KB)',
            Math.round(adapter.limits.maxUniformBufferBindingSize / 1024));
        put('minUniformBufferOffsetAlignment', adapter.limits.minUniformBufferOffsetAlignment);
      }
    } catch (e) { put('requestAdapter()', 'threw: ' + e.message, false); }
  }

  // How much wasm memory this engine will actually hand over, which is the
  // ceiling a whole-game module has to live under.
  let grown = 0;
  try {
    const mem = new WebAssembly.Memory({ initial: 1 });
    for (let mb = 0; mb < 4096; mb += 64) {
      try { mem.grow(64 * 16); grown = mb + 64; } catch (e) { break; }
    }
  } catch (e) {}
  put('wasm memory grown to (MB)', grown, grown >= 512);

}

if (typeof window !== 'undefined') window.probeCapabilities = probeCapabilities;
