// The browser half of GXRuntime's web backend.
//
// It reads one frame's worth of flat records out of WASM linear memory and
// turns them into WebGPU work. It contains no GX knowledge at all: every
// decision that needs a GameCube register was already made in C++ by gxcore,
// and what arrives here is pipeline state, byte blobs and an ordered program.
//
// The record layouts are published by the module (dolweb_*_stride) rather than
// hardcoded, and the whole stream carries a version that must match.

'use strict';

const DOLWEB_STREAM_VERSION = 3;

// GX cull modes are Dolphin's CullMode (genMode bits 14-15), already swapped
// relative to the SDK enum. All (3) never reaches here: gxcore drops the draw.
const GPU_CULL = ['none', 'back', 'front', 'none'];

// GX compare, flipped for reversed-Z. Same table as the aurora substrate's
// to_compare() with UseReversedZ = true, which is what gxcore's generated
// vertex shader assumes when it flips clip z.
const GPU_CMP_REVZ = ['never', 'greater', 'equal', 'greater-equal',
                      'less', 'not-equal', 'less-equal', 'always'];

const GPU_SRC_FACTOR = ['zero', 'one', 'dst', 'one-minus-dst', 'src-alpha',
                        'one-minus-src-alpha', 'dst-alpha', 'one-minus-dst-alpha'];
const GPU_DST_FACTOR = ['zero', 'one', 'src', 'one-minus-src', 'src-alpha',
                        'one-minus-src-alpha', 'dst-alpha', 'one-minus-dst-alpha'];

// gxcore GapCounters, in declaration order (gxcore.hpp). The module hands the
// struct over as raw u64s; the names live here so the page can print them.
const GAP_NAMES = [
  'draws_planned', 'draws_skipped', 'cull_all_draws', 'missing_vcd',
  'vertex_decode_failures', 'unsupported_texgen', 'per_vertex_tex_mtx',
  'unresolved_tex_matrix', 'normals_ignored', 'lighting_ignored',
  'tlut_texture', 'alpha_compare_ignored', 'tev_stages_over',
  'tev_multi_texmap', 'efb_copy_ignored', 'efb_copies', 'efb_copy_depth',
  'efb_display_copies', 'fog_ignored', 'indirect_ignored', 'logic_op_ignored',
];

const EFB_FORMAT = 'rgba8unorm';
const DEPTH_FORMAT = 'depth24plus';

const BLIT_WGSL = `
struct VOut { @builtin(position) pos: vec4f, @location(0) uv: vec2f };
@group(0) @binding(0) var src: texture_2d<f32>;
@group(0) @binding(1) var samp: sampler;
@vertex fn vs(@builtin(vertex_index) i: u32) -> VOut {
  var p = array<vec2f, 3>(vec2f(-1.0, -1.0), vec2f(3.0, -1.0), vec2f(-1.0, 3.0));
  var o: VOut;
  o.pos = vec4f(p[i], 0.0, 1.0);
  o.uv = vec2f((p[i].x + 1.0) * 0.5, 1.0 - (p[i].y + 1.0) * 0.5);
  return o;
}
@fragment fn fs(in: VOut) -> @location(0) vec4f {
  return vec4f(textureSample(src, samp, in.uv).rgb, 1.0);
}`;

class DolWebRenderer {
  constructor(module, canvas) {
    this.M = module;
    this.canvas = canvas;
    this.device = null;
    this.pipelines = new Map();      // id -> {pipeline, tev, textured, texMask}
    this.textures = new Map();       // id -> GPUTexture
    this.textureViews = new Map();   // id -> GPUTextureView
    this.texBindGroups = new Map();  // key -> GPUBindGroup
    this.texBGLs = new Map();        // mask -> GPUBindGroupLayout
    this.vtxBuf = null; this.vtxCap = 0;
    this.idxBuf = null; this.idxCap = 0;
    this.uniBuf = null; this.uniCap = 0;
    this.efbColor = null; this.efbDepth = null;
    this.efbWidth = 0; this.efbHeight = 0;
    // Copy-clear carried between frames; see ensurePass().
    this.pendingClear = null;
    this.stats = { frames: 0, draws: 0, skippedDraws: 0, copies: 0,
                   pipelines: 0, textures: 0, unsupportedCopies: 0,
                   gpuMs: 0 };
    this.lastError = null;
  }

  async init() {
    if (!navigator.gpu) throw new Error('WebGPU unavailable (no navigator.gpu)');
    const adapter = await navigator.gpu.requestAdapter();
    if (!adapter) throw new Error('requestAdapter() returned null');
    const device = await adapter.requestDevice({
      requiredLimits: {
        maxBufferSize: Math.min(adapter.limits.maxBufferSize, 512 * 1024 * 1024),
        maxStorageBufferBindingSize: adapter.limits.maxStorageBufferBindingSize,
      },
    });
    this.device = device;
    this.adapterInfo = adapter.info || {};
    device.lost.then((info) => { this.lastError = 'device lost: ' + info.message; });
    device.onuncapturederror = (e) => {
      if (!this.lastError) this.lastError = e.error.message;
      console.error('[webgpu]', e.error.message);
    };

    this.ctx = this.canvas.getContext('webgpu');
    this.canvasFormat = navigator.gpu.getPreferredCanvasFormat();
    this.ctx.configure({ device, format: this.canvasFormat, alphaMode: 'opaque' });

    this.sampler = device.createSampler({
      addressModeU: 'repeat', addressModeV: 'repeat',
      magFilter: 'linear', minFilter: 'linear', mipmapFilter: 'nearest',
    });
    // One uniform bind group layout for both the vertex and the pixel block:
    // they are different structs but both bind at binding 0 with a dynamic
    // offset, and leaving minBindingSize unset lets one layout serve both.
    this.uniformBGL = device.createBindGroupLayout({
      entries: [{
        binding: 0,
        visibility: GPUShaderStage.VERTEX | GPUShaderStage.FRAGMENT,
        buffer: { type: 'uniform', hasDynamicOffset: true },
      }],
    });

    const blitModule = device.createShaderModule({ code: BLIT_WGSL });
    this.blitBGL = device.createBindGroupLayout({
      entries: [
        { binding: 0, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: 'float' } },
        { binding: 1, visibility: GPUShaderStage.FRAGMENT, sampler: { type: 'filtering' } },
      ],
    });
    this.blitPipeline = device.createRenderPipeline({
      layout: device.createPipelineLayout({ bindGroupLayouts: [this.blitBGL] }),
      vertex: { module: blitModule, entryPoint: 'vs' },
      fragment: { module: blitModule, entryPoint: 'fs',
                  targets: [{ format: this.canvasFormat }] },
      primitive: { topology: 'triangle-list' },
    });

    this.readLayout();
    return this;
  }

  readLayout() {
    const M = this.M;
    const version = M._dolweb_stream_version();
    if (version !== DOLWEB_STREAM_VERSION) {
      throw new Error(`frame stream version ${version} != ${DOLWEB_STREAM_VERSION}; ` +
                      'rebuild the module or update dolweb.js');
    }
    this.drawWords = M._dolweb_draw_stride() >> 2;
    this.copyWords = M._dolweb_copy_stride() >> 2;
    this.pipeWords = M._dolweb_pipeline_stride() >> 2;
    this.texWords = M._dolweb_texture_stride() >> 2;
    this.vertexStride = M._dolweb_vertex_stride();
    this.vsUniformSize = M._dolweb_vs_uniform_size();
    this.psUniformSize = M._dolweb_ps_uniform_size();
    this.vtxOffsets = {
      pos: M._dolweb_vertex_offset_pos(),
      posmtx: M._dolweb_vertex_offset_posmtx(),
      color0: M._dolweb_vertex_offset_color0(),
      color1: M._dolweb_vertex_offset_color1(),
      uv: M._dolweb_vertex_offset_uv(),
      normal: M._dolweb_vertex_offset_normal(),
      texmtxidx: M._dolweb_vertex_offset_texmtxidx(),
      binormal: M._dolweb_vertex_offset_binormal(),
      tangent: M._dolweb_vertex_offset_tangent(),
    };
  }

  // --- resources ----------------------------------------------------------

  ensureEfb(width, height) {
    if (this.efbWidth === width && this.efbHeight === height) return;
    if (this.efbColor) this.efbColor.destroy();
    if (this.efbDepth) this.efbDepth.destroy();
    const device = this.device;
    this.efbColor = device.createTexture({
      size: [width, height], format: EFB_FORMAT,
      usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC |
             GPUTextureUsage.TEXTURE_BINDING,
    });
    this.efbDepth = device.createTexture({
      size: [width, height], format: DEPTH_FORMAT,
      usage: GPUTextureUsage.RENDER_ATTACHMENT,
    });
    this.efbColorView = this.efbColor.createView();
    this.efbDepthView = this.efbDepth.createView();
    this.efbWidth = width; this.efbHeight = height;
    this.blitBindGroup = device.createBindGroup({
      layout: this.blitBGL,
      entries: [{ binding: 0, resource: this.efbColorView },
                { binding: 1, resource: this.sampler }],
    });
  }

  ensureBuffer(name, capName, bytes, usage) {
    if (this[name] && this[capName] >= bytes) return;
    if (this[name]) this[name].destroy();
    const size = Math.max(1 << 16, 1 << Math.ceil(Math.log2(Math.max(bytes, 1))));
    this[name] = this.device.createBuffer({ size, usage: usage | GPUBufferUsage.COPY_DST });
    this[capName] = size;
  }

  textureBGL(mask) {
    let bgl = this.texBGLs.get(mask);
    if (bgl) return bgl;
    const entries = [];
    if (mask === 1) {
      // Single-texmap: the generated fragment declares tex0/samp0 at binding
      // 0/1 whichever texmap it actually samples.
      entries.push({ binding: 0, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: 'float' } });
      entries.push({ binding: 1, visibility: GPUShaderStage.FRAGMENT, sampler: { type: 'filtering' } });
    } else {
      for (let t = 0; t < 8; t++) {
        if ((mask & (1 << t)) === 0) continue;
        entries.push({ binding: 2 * t, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: 'float' } });
        entries.push({ binding: 2 * t + 1, visibility: GPUShaderStage.FRAGMENT, sampler: { type: 'filtering' } });
      }
    }
    bgl = this.device.createBindGroupLayout({ entries });
    this.texBGLs.set(mask, bgl);
    return bgl;
  }

  vertexAttributes(attrMask) {
    const o = this.vtxOffsets;
    const attrs = [
      { shaderLocation: 0, offset: o.pos, format: 'float32x3' },
      { shaderLocation: 1, offset: o.posmtx, format: 'uint32' },
      { shaderLocation: 2, offset: o.color0, format: 'float32x4' },
      { shaderLocation: 3, offset: o.color1, format: 'float32x4' },
      { shaderLocation: 4, offset: o.uv + 0, format: 'float32x2' },
      { shaderLocation: 5, offset: o.uv + 8, format: 'float32x2' },
      { shaderLocation: 6, offset: o.uv + 16, format: 'float32x2' },
      { shaderLocation: 7, offset: o.uv + 24, format: 'float32x2' },
    ];
    if (attrMask & (1 << 8)) attrs.push({ shaderLocation: 8, offset: o.normal, format: 'float32x3' });
    if (attrMask & (1 << 9)) attrs.push({ shaderLocation: 9, offset: o.texmtxidx, format: 'uint32' });
    if (attrMask & (1 << 10)) attrs.push({ shaderLocation: 10, offset: o.binormal, format: 'float32x3' });
    if (attrMask & (1 << 11)) attrs.push({ shaderLocation: 11, offset: o.tangent, format: 'float32x3' });
    return attrs;
  }

  ingestPipelines() {
    const M = this.M;
    const count = M._dolweb_new_pipeline_count();
    if (count === 0) return;
    const base = M._dolweb_new_pipelines();
    const wgslBase = M._dolweb_wgsl();
    const words = new Uint32Array(M.HEAPU8.buffer, base, count * this.pipeWords);
    for (let i = 0; i < count; i++) {
      const w = words.subarray(i * this.pipeWords, (i + 1) * this.pipeWords);
      const id = w[0];
      const wgsl = M.UTF8ToString(wgslBase + w[1]);
      const tev = w[15] !== 0, textured = w[16] !== 0;
      const texMask = w[14];
      const module = this.device.createShaderModule({ code: wgsl, label: `gxcore ${id}` });

      // Group 0 stays empty: gxcore's WGSL never references it, but the
      // substrate reserves it, so the generated bindings start at group 1.
      const layouts = [this.device.createBindGroupLayout({ entries: [] }),
                       this.uniformBGL];
      if (tev) layouts.push(this.uniformBGL);
      if (textured) layouts.push(this.textureBGL(texMask));

      const blending = w[7] !== 0 || w[8] !== 0;
      let writeMask = 0;
      if (w[11] !== 0) writeMask |= GPUColorWrite.RED | GPUColorWrite.GREEN | GPUColorWrite.BLUE;
      if (w[12] !== 0) writeMask |= GPUColorWrite.ALPHA;
      let blend;
      if (w[8] !== 0) {
        // GC subtract mode forces ONE/ONE with dst - src (Dolphin RenderState).
        const c = { operation: 'reverse-subtract', srcFactor: 'one', dstFactor: 'one' };
        blend = { color: c, alpha: c };
      } else if (blending) {
        const c = { operation: 'add',
                    srcFactor: GPU_SRC_FACTOR[w[9] & 7],
                    dstFactor: GPU_DST_FACTOR[w[10] & 7] };
        blend = { color: c, alpha: c };
      }

      const depthTest = w[4] !== 0;
      let pipeline = null;
      try {
        pipeline = this.device.createRenderPipeline({
          label: `gxcore ${id}`,
          layout: this.device.createPipelineLayout({ bindGroupLayouts: layouts }),
          vertex: {
            module, entryPoint: 'vs_main',
            buffers: [{ arrayStride: this.vertexStride, stepMode: 'vertex',
                        attributes: this.vertexAttributes(w[13]) }],
          },
          fragment: { module, entryPoint: 'fs_main',
                      targets: [{ format: EFB_FORMAT, blend, writeMask }] },
          primitive: {
            topology: 'triangle-list',
            frontFace: 'cw', // substrate winding convention (gx.cpp)
            cullMode: GPU_CULL[w[3] & 3],
          },
          depthStencil: {
            format: DEPTH_FORMAT,
            depthWriteEnabled: depthTest && w[6] !== 0,
            depthCompare: depthTest ? GPU_CMP_REVZ[w[5] & 7] : 'always',
          },
        });
      } catch (e) {
        console.error('[gxcore] pipeline', id, 'failed:', e.message);
        if (!this.lastError) this.lastError = `pipeline ${id}: ${e.message}`;
      }
      this.pipelines.set(id, { pipeline, tev, textured, texMask });
      this.stats.pipelines++;
    }
  }

  ingestTextures() {
    const M = this.M;
    const freedCount = M._dolweb_freed_texture_count();
    if (freedCount > 0) {
      const freed = new Uint32Array(M.HEAPU8.buffer, M._dolweb_freed_textures(), freedCount).slice();
      for (const id of freed) {
        const tex = this.textures.get(id);
        if (tex) tex.destroy();
        this.textures.delete(id);
        this.textureViews.delete(id);
      }
      this.texBindGroups.clear();
    }

    const count = M._dolweb_new_texture_count();
    if (count === 0) return;
    const base = M._dolweb_new_textures();
    const bytesBase = M._dolweb_texbytes();
    const words = new Uint32Array(M.HEAPU8.buffer, base, count * this.texWords);
    for (let i = 0; i < count; i++) {
      const w = words.subarray(i * this.texWords, (i + 1) * this.texWords);
      const id = w[0], width = w[1], height = w[2], off = w[3], size = w[4];
      const old = this.textures.get(id);
      if (old) old.destroy();
      // size 0 means "an EFB copy lands here": a render target, not an upload.
      const usage = size === 0
        ? GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST | GPUTextureUsage.RENDER_ATTACHMENT
        : GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST;
      const tex = this.device.createTexture({
        size: [Math.max(width, 1), Math.max(height, 1)], format: EFB_FORMAT, usage,
      });
      if (size !== 0) {
        this.device.queue.writeTexture(
          { texture: tex },
          new Uint8Array(M.HEAPU8.buffer, bytesBase + off, size),
          { bytesPerRow: width * 4, rowsPerImage: height },
          [width, height]);
      }
      this.textures.set(id, tex);
      this.textureViews.set(id, tex.createView());
      this.stats.textures++;
    }
    this.texBindGroups.clear();
  }

  textureBindGroup(mask, ids) {
    const key = mask + ':' + ids.join(',');
    let bg = this.texBindGroups.get(key);
    if (bg !== undefined) return bg;
    const entries = [];
    if (mask === 1) {
      const view = this.textureViews.get(ids[0]);
      if (!view) { this.texBindGroups.set(key, null); return null; }
      entries.push({ binding: 0, resource: view });
      entries.push({ binding: 1, resource: this.sampler });
    } else {
      for (let t = 0; t < 8; t++) {
        if ((mask & (1 << t)) === 0) continue;
        const view = this.textureViews.get(ids[t]);
        if (!view) { this.texBindGroups.set(key, null); return null; }
        entries.push({ binding: 2 * t, resource: view });
        entries.push({ binding: 2 * t + 1, resource: this.sampler });
      }
    }
    bg = this.device.createBindGroup({ layout: this.textureBGL(mask), entries });
    this.texBindGroups.set(key, bg);
    return bg;
  }

  // --- the frame ----------------------------------------------------------

  renderFrame() {
    const M = this.M, device = this.device;
    const t0 = performance.now();
    this.ensureEfb(M._dolweb_efb_width(), M._dolweb_efb_height());
    this.ingestPipelines();
    this.ingestTextures();

    const progCount = M._dolweb_prog_count();
    const drawCount = M._dolweb_draw_count();
    const copyCount = M._dolweb_copy_count();
    const vtxSize = M._dolweb_vtx_size();
    const idxSize = M._dolweb_idx_size();
    const uniSize = M._dolweb_uni_size();

    // One writeBuffer per arena per frame: the whole point of the flat stream
    // is that the JS/WASM boundary is crossed a fixed number of times, not once
    // per draw.
    if (vtxSize > 0) {
      this.ensureBuffer('vtxBuf', 'vtxCap', vtxSize, GPUBufferUsage.VERTEX);
      device.queue.writeBuffer(this.vtxBuf, 0,
        new Uint8Array(M.HEAPU8.buffer, M._dolweb_vtx(), vtxSize));
    }
    if (idxSize > 0) {
      this.ensureBuffer('idxBuf', 'idxCap', idxSize, GPUBufferUsage.INDEX);
      device.queue.writeBuffer(this.idxBuf, 0,
        new Uint8Array(M.HEAPU8.buffer, M._dolweb_idx(), idxSize));
    }
    if (uniSize > 0) {
      // Room past the last record: a dynamic-offset binding is
      // sizeof(VertexShaderConstants) wide wherever it lands.
      this.ensureBuffer('uniBuf', 'uniCap', uniSize + this.vsUniformSize + 256,
                        GPUBufferUsage.UNIFORM);
      device.queue.writeBuffer(this.uniBuf, 0,
        new Uint8Array(M.HEAPU8.buffer, M._dolweb_uni(), uniSize));
    }
    // The uniform bind group is bound once per frame with a dynamic offset per
    // draw, so it only has to be rebuilt when ensureBuffer reallocated under it.
    if (this.uniBuf && this.uniformBindGroupBuffer !== this.uniBuf) {
      this.uniformBindGroup = device.createBindGroup({
        layout: this.uniformBGL,
        entries: [{ binding: 0, resource: { buffer: this.uniBuf, offset: 0,
                                            size: this.vsUniformSize } }],
      });
      this.uniformBindGroupBuffer = this.uniBuf;
    }
    // Group 0 is declared by every pipeline layout (the substrate reserves it)
    // but referenced by no shader, and WebGPU does not require an unused group
    // to be set -- so there is nothing to bind for it.

    const prog = progCount > 0
      ? new Uint32Array(M.HEAPU8.buffer, M._dolweb_prog(), progCount * 2) : null;
    const draws = drawCount > 0
      ? new Uint32Array(M.HEAPU8.buffer, M._dolweb_draws(), drawCount * this.drawWords) : null;
    const drawsF = drawCount > 0
      ? new Float32Array(M.HEAPU8.buffer, M._dolweb_draws(), drawCount * this.drawWords) : null;
    const copies = copyCount > 0
      ? new Uint32Array(M.HEAPU8.buffer, M._dolweb_copies(), copyCount * this.copyWords) : null;
    const copiesF = copyCount > 0
      ? new Float32Array(M.HEAPU8.buffer, M._dolweb_copies(), copyCount * this.copyWords) : null;

    const encoder = device.createCommandEncoder();
    let pass = null;
    const self = this;

    // Passes are created lazily, and a clear is applied when the NEXT pass
    // opens rather than when it is requested. A GameCube title clears the EFB
    // through the copy-clear on GXCopyDisp -- the copy that says "this frame is
    // finished, show it". Beginning a cleared pass there wipes the frame that
    // was just about to be presented, which is a black screen with a perfectly
    // healthy 40,000-draw frame behind it. this.pendingClear therefore survives
    // across frames until something actually draws.
    function ensurePass() {
      if (pass) return pass;
      const c = self.pendingClear;
      pass = encoder.beginRenderPass({
        colorAttachments: [{
          view: self.efbColorView,
          loadOp: (c && c.color) ? 'clear' : 'load', storeOp: 'store',
          clearValue: {
            r: c ? ((c.rgba >>> 24) & 0xFF) / 255 : 0,
            g: c ? ((c.rgba >>> 16) & 0xFF) / 255 : 0,
            b: c ? ((c.rgba >>> 8) & 0xFF) / 255 : 0,
            a: c ? (c.rgba & 0xFF) / 255 : 1,
          },
        }],
        depthStencilAttachment: {
          view: self.efbDepthView,
          depthLoadOp: (c && c.depth_) ? 'clear' : 'load', depthStoreOp: 'store',
          depthClearValue: c ? c.depth : 0,
        },
      });
      self.pendingClear = null;
      self.boundPipeline = -1;
      return pass;
    }
    function endPass() { if (pass) { pass.end(); pass = null; } }

    let issued = 0, dropped = 0, copiesDone = 0;

    for (let p = 0; p < progCount; p++) {
      const type = prog[p * 2], index = prog[p * 2 + 1];
      if (type === 0) {
        const b = index * this.drawWords;
        const entry = this.pipelines.get(draws[b + 0]);
        if (!entry || !entry.pipeline) { dropped++; continue; }

        let texBG = null;
        if (entry.textured) {
          const mask = draws[b + 7];
          const ids = [];
          for (let t = 0; t < 8; t++) ids.push(draws[b + 8 + t]);
          texBG = this.textureBindGroup(mask === 0 ? 1 : mask, ids);
          if (!texBG) { dropped++; continue; }
        }

        const rp = ensurePass();
        if (draws[b + 16] !== 0) {
          // WebGPU rejects a viewport that leaves the attachment; GC viewports
          // routinely do (the guest scissors instead). Clamp rather than drop.
          let x = drawsF[b + 17], y = drawsF[b + 18];
          let w = drawsF[b + 19], h = drawsF[b + 20];
          if (w < 0) { x += w; w = -w; }
          if (h < 0) { y += h; h = -h; }
          const l = Math.max(0, Math.min(x, this.efbWidth));
          const t = Math.max(0, Math.min(y, this.efbHeight));
          const r = Math.max(l, Math.min(x + w, this.efbWidth));
          const bo = Math.max(t, Math.min(y + h, this.efbHeight));
          if (r - l < 1 || bo - t < 1) { dropped++; continue; }
          let minD = drawsF[b + 21], maxD = drawsF[b + 22];
          minD = Math.min(1, Math.max(0, minD));
          maxD = Math.min(1, Math.max(0, maxD));
          if (minD > maxD) { const sw = minD; minD = maxD; maxD = sw; }
          rp.setViewport(l, t, r - l, bo - t, minD, maxD);
        }

        rp.setPipeline(entry.pipeline);
        const vsOff = draws[b + 5];
        rp.setBindGroup(1, this.uniformBindGroup, [vsOff]);
        if (entry.tev) {
          const psOff = draws[b + 6];
          rp.setBindGroup(2, this.uniformBindGroup, [psOff === 0xFFFFFFFF ? vsOff : psOff]);
          if (texBG) rp.setBindGroup(3, texBG);
        } else if (texBG) {
          rp.setBindGroup(2, texBG);
        }
        rp.setVertexBuffer(0, this.vtxBuf, draws[b + 1], draws[b + 2]);
        rp.setIndexBuffer(this.idxBuf, 'uint16', draws[b + 3], draws[b + 4] * 2);
        rp.drawIndexed(draws[b + 4]);
        issued++;
      } else {
        const c = index * this.copyWords;
        const format = copies[c + 2];
        endPass();
        if (format !== 0xF) {
          const destId = copies[c + 1];
          const dest = this.textures.get(destId);
          // Z-source copies (GXTexFmt bit 0x10) resolve the depth buffer, which
          // is not colour-copyable in WebGPU. Counted, not faked.
          if (dest && (format & 0x10) === 0) {
            const w = Math.min(copies[c + 5], this.efbWidth - Math.min(copies[c + 3], this.efbWidth));
            const h = Math.min(copies[c + 6], this.efbHeight - Math.min(copies[c + 4], this.efbHeight));
            if (w > 0 && h > 0) {
              encoder.copyTextureToTexture(
                { texture: this.efbColor, origin: { x: copies[c + 3], y: copies[c + 4] } },
                { texture: dest },
                [Math.min(w, dest.width), Math.min(h, dest.height)]);
              copiesDone++;
            }
          } else if (dest) {
            this.stats.unsupportedCopies++;
          }
        }
        if (copies[c + 7] !== 0) {
          this.pendingClear = {
            rgba: copies[c + 8], depth: copiesF[c + 9],
            color: copies[c + 10] !== 0 || copies[c + 11] !== 0,
            depth_: copies[c + 12] !== 0,
          };
        }
      }
    }
    endPass();

    // EFB -> canvas. GameCube presents the XFB; this stands in for the VI's
    // copy, scaled to whatever the page sized the canvas to.
    const canvasPass = encoder.beginRenderPass({
      colorAttachments: [{
        view: this.ctx.getCurrentTexture().createView(),
        loadOp: 'clear', storeOp: 'store',
        clearValue: { r: 0, g: 0, b: 0, a: 1 },
      }],
    });
    canvasPass.setPipeline(this.blitPipeline);
    canvasPass.setBindGroup(0, this.blitBindGroup);
    canvasPass.draw(3);
    canvasPass.end();

    device.queue.submit([encoder.finish()]);

    this.stats.frames++;
    this.stats.draws = issued;
    this.stats.skippedDraws = dropped;
    this.stats.copies = copiesDone;
    this.stats.gpuMs = performance.now() - t0;
  }

  gapCounters() {
    const M = this.M;
    const count = M._dolweb_gap_counter_count();
    const base = M._dolweb_gap_counters();
    const view = new BigUint64Array(M.HEAPU8.buffer, base, count);
    const out = {};
    for (let i = 0; i < count; i++) out[GAP_NAMES[i] || `gap_${i}`] = Number(view[i]);
    return out;
  }

  // Render one frame off-screen and read the pixels back. Sampling the canvas
  // races the compositor and will lie; copyTextureToBuffer is queue-ordered.
  async readbackEfb() {
    const device = this.device;
    const bytesPerRow = Math.ceil(this.efbWidth * 4 / 256) * 256;
    const buf = device.createBuffer({
      size: bytesPerRow * this.efbHeight,
      usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ,
    });
    const enc = device.createCommandEncoder();
    enc.copyTextureToBuffer({ texture: this.efbColor }, { buffer: buf, bytesPerRow },
                            [this.efbWidth, this.efbHeight]);
    device.queue.submit([enc.finish()]);
    await buf.mapAsync(GPUMapMode.READ);
    const raw = new Uint8Array(buf.getMappedRange()).slice();
    buf.unmap(); buf.destroy();
    return { width: this.efbWidth, height: this.efbHeight, bytesPerRow, data: raw };
  }
}

if (typeof window !== 'undefined') window.DolWebRenderer = DolWebRenderer;
if (typeof window !== 'undefined') window.DOLWEB_GAP_NAMES = GAP_NAMES;
