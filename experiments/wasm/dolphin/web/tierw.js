// Worker half of the tiering probe: instantiates tier.wasm directly (its
// imports are all stubbable) and runs the two kernels alternately, posting one
// row per call, so a page can compare the main thread with a Worker on the
// same device -- an emscripten pthread is a Worker, and iOS may schedule it on
// an efficiency core.
onmessage = async (e) => {
  const { seconds, iters } = e.data;
  const t0 = performance.now();
  const mod = await WebAssembly.compileStreaming(fetch('tier.wasm'));
  const importObject = {};
  for (const i of WebAssembly.Module.imports(mod)) {
    importObject[i.module] ??= {};
    if (i.kind === 'memory') importObject[i.module][i.name] = new WebAssembly.Memory({ initial: 256, maximum: 256 });
    else if (i.kind === 'function') importObject[i.module][i.name] = () => 0;
    else if (i.kind === 'table') importObject[i.module][i.name] = new WebAssembly.Table({ initial: 1, element: 'anyfunc' });
    else if (i.kind === 'global') importObject[i.module][i.name] = new WebAssembly.Global({ value: 'i32', mutable: true }, 0);
  }
  const ex = (await WebAssembly.instantiate(mod, importObject)).exports;
  if (ex.__wasm_call_ctors) try { ex.__wasm_call_ctors(); } catch (err) {}
  postMessage({ ready: performance.now() - t0 });
  const end = performance.now() + seconds * 1000;
  let n = 0;
  while (performance.now() < end) {
    let t = performance.now();
    ex.tier_kernel(iters);
    const localsMs = performance.now() - t;
    t = performance.now();
    ex.tier_kernel_mem(iters);
    const memMs = performance.now() - t;
    postMessage({ row: { n: ++n, at: +((performance.now() - t0) / 1000).toFixed(1),
                         locals: +(iters / localsMs / 1000).toFixed(1), mem: +(iters / memMs / 1000).toFixed(1) } });
  }
  postMessage({ done: true });
};
