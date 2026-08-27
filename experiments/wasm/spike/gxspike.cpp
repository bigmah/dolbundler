// GXRuntime's GX pipeline, compiled to WebAssembly.
//
// Drives GxCoreState with real GameCube GX register writes and a real vertex
// payload, builds a draw plan, and hands the result out to JavaScript: the
// generated WGSL, the decoded vertices, the uniform blocks and the pipeline
// state. Nothing here knows what a browser is -- the WGSL comes out of gxcore
// unchanged, because gxcore already targets WGSL for the wgpu substrate.

#include "gxruntime/gxcore/gxcore.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <emscripten/emscripten.h>

namespace ar = gxruntime::aurora_recomp;
namespace gxc = gxruntime::gxcore;

namespace {

ar::RenderStatePacket bp(std::uint32_t reg, std::uint32_t value) {
  return {.kind = ar::RenderStateKind::BpReg, .index = reg, .value = value};
}

void append_be32(std::vector<std::uint8_t>& out, std::uint32_t v) {
  out.push_back(static_cast<std::uint8_t>(v >> 24));
  out.push_back(static_cast<std::uint8_t>(v >> 16));
  out.push_back(static_cast<std::uint8_t>(v >> 8));
  out.push_back(static_cast<std::uint8_t>(v));
}

void append_be_f32(std::vector<std::uint8_t>& out, float f) {
  std::uint32_t bits;
  std::memcpy(&bits, &f, sizeof bits);
  append_be32(out, bits);
}

// --- the built plan, kept alive for the JS side -------------------------------
gxc::DrawPlan g_plan;
std::string g_wgsl;
std::string g_status;
std::vector<std::uint8_t> g_payload;

// GameCube vertex attribute formats
constexpr std::uint32_t kFmtF32 = 4;
constexpr std::uint32_t kFmtRGBA8 = 5;

} // namespace

extern "C" {

// Build a triangle strip whose vertices carry position + RGBA colour, exactly
// as a GX title would program it: VCD says which attributes are present, VAT
// says how they are encoded, and the payload is big-endian guest bytes.
EMSCRIPTEN_KEEPALIVE
int gxspike_build(void) {
  gxc::GxCoreState state;
  state.reset();

  // genMode: 0 texgens, cull none.
  state.apply(bp(0x00, 0u));
  // zmode: depth test on, func 3 (LEQUAL), depth update on.
  state.apply(bp(0x40, 0x17u));
  // cmode0: no blend, colour + alpha update enabled.
  state.apply(bp(0x41, (1u << 3) | (1u << 4)));

  // VCD lo: position direct (bits 9-10 = 1), colour0 direct (bits 13-14 = 1).
  state.apply({.kind = ar::RenderStateKind::CpVcd, .index = 0,
               .value = (1u << 9) | (1u << 13)});
  // VCD hi: no texture coordinates.
  state.apply({.kind = ar::RenderStateKind::CpVcd, .index = 1, .value = 0u});

  // VAT fmt0: pos 3-element f32, colour0 RGBA / RGBA8.
  state.apply({.kind = ar::RenderStateKind::CpVat, .index = 0,
               .value = 1u | (kFmtF32 << 1) | (1u << 13) | (kFmtRGBA8 << 14),
               .aux0 = 0});
  state.apply({.kind = ar::RenderStateKind::CpVat, .index = 0, .value = 0,
               .aux0 = 1});
  state.apply({.kind = ar::RenderStateKind::CpVat, .index = 0, .value = 0,
               .aux0 = 2});

  ar::ConsumedDraw draw{};
  draw.primitive = 0x98; // GX_DRAW_TRIANGLE_STRIP
  draw.vtx_fmt = 0;
  draw.vertex_count = 4;
  draw.vertex_size = 12 + 4; // pos(3 x f32) + colour(RGBA8)

  struct V { float x, y, z; std::uint32_t rgba; };
  const V verts[4] = {
      {-0.75f, -0.65f, 0.0f, 0xE23A2BFFu}, // red
      { 0.75f, -0.65f, 0.0f, 0x0E7580FFu}, // teal
      {-0.30f,  0.75f, 0.0f, 0xD4A85CFFu}, // amber
      { 0.85f,  0.35f, 0.0f, 0x2C6E49FFu}, // green
  };
  g_payload.clear();
  for (const V& v : verts) {
    append_be_f32(g_payload, v.x);
    append_be_f32(g_payload, v.y);
    append_be_f32(g_payload, v.z);
    append_be32(g_payload, v.rgba);
  }
  draw.vertex_payload = g_payload;

  // Identity projection (orthographic) and an identity position matrix.
  draw.transform_flags = ar::kDrawTransformProjectionValid;
  draw.projection[0] = 1.f;
  draw.projection[2] = 1.f;
  draw.projection[4] = -1.f;
  draw.projection_type = 1; // orthographic
  draw.current_pn_matrix = 0;
  draw.position_matrix_valid_mask = 1u;
  draw.position_matrices[0][0] = 1.f;
  draw.position_matrices[0][5] = 1.f;
  draw.position_matrices[0][10] = 1.f;

  // XF: one colour channel, unlit (vertex colour passes straight through).
  draw.chan_regs[0] = 1u;  // numColorChans
  // colour0 control (LitChannel): matsource@0 = 1 selects the *vertex* colour
  // rather than the material register, enablelighting@1 stays clear. With
  // matsource 0 the channel would take a zeroed material register and the
  // whole draw comes out black -- which is exactly what it did first try.
  draw.chan_regs[5] = 1u;
  // Alpha is its own LitChannel (slot 7), with its own matsource. Leaving it 0
  // takes the material register's zero alpha, and the draw renders fully
  // transparent -- geometry and colour both correct, and nothing visible.
  draw.chan_regs[7] = 1u;
  draw.chan_reg_mask = 0x1FFu;

  gxc::GapCounters counters;
  g_plan = state.build_draw_plan(draw, counters);
  if (!g_plan.ok) {
    g_status = g_plan.skip_reason ? g_plan.skip_reason : "unknown";
    return 0;
  }
  g_wgsl = gxc::generate_wgsl(g_plan.pipeline.shader);
  g_status = "ok";
  return 1;
}

EMSCRIPTEN_KEEPALIVE const char* gxspike_status(void) { return g_status.c_str(); }
EMSCRIPTEN_KEEPALIVE const char* gxspike_wgsl(void) { return g_wgsl.c_str(); }
EMSCRIPTEN_KEEPALIVE int gxspike_wgsl_len(void) { return (int)g_wgsl.size(); }

EMSCRIPTEN_KEEPALIVE const float* gxspike_vertices(void) { return g_plan.vertices.data(); }
EMSCRIPTEN_KEEPALIVE int gxspike_vertex_floats(void) { return (int)gxc::kVertexFloats; }
EMSCRIPTEN_KEEPALIVE int gxspike_vertex_stride(void) { return (int)gxc::kVertexStrideBytes; }
EMSCRIPTEN_KEEPALIVE int gxspike_vertex_count(void) { return (int)g_plan.vertex_count; }
EMSCRIPTEN_KEEPALIVE const std::uint16_t* gxspike_indices(void) { return g_plan.indices.data(); }
EMSCRIPTEN_KEEPALIVE int gxspike_index_count(void) { return (int)g_plan.indices.size(); }

EMSCRIPTEN_KEEPALIVE const void* gxspike_vs_constants(void) { return &g_plan.constants; }
EMSCRIPTEN_KEEPALIVE int gxspike_vs_constants_size(void) { return (int)sizeof(g_plan.constants); }
EMSCRIPTEN_KEEPALIVE const void* gxspike_ps_constants(void) { return &g_plan.pixel_constants; }
EMSCRIPTEN_KEEPALIVE int gxspike_ps_constants_size(void) { return (int)sizeof(g_plan.pixel_constants); }

EMSCRIPTEN_KEEPALIVE int gxspike_cull_mode(void) { return g_plan.pipeline.cull_mode; }
EMSCRIPTEN_KEEPALIVE int gxspike_depth_test(void) { return g_plan.pipeline.depth_test; }
EMSCRIPTEN_KEEPALIVE int gxspike_depth_func(void) { return g_plan.pipeline.depth_func; }
EMSCRIPTEN_KEEPALIVE int gxspike_blend_enable(void) { return g_plan.pipeline.blend_enable; }

// Byte offsets into the fixed decoded-vertex layout, so the JS side can build a
// WebGPU vertex layout matching the WGSL VertexIn struct without hardcoding.
EMSCRIPTEN_KEEPALIVE int gxspike_off_pos(void) { return (int)gxc::kVertexPosOffset; }
EMSCRIPTEN_KEEPALIVE int gxspike_off_posmtx(void) { return (int)gxc::kVertexPosMtxOffset; }
EMSCRIPTEN_KEEPALIVE int gxspike_off_color0(void) { return (int)gxc::kVertexColor0Offset; }
EMSCRIPTEN_KEEPALIVE int gxspike_off_color1(void) { return (int)gxc::kVertexColor1Offset; }
EMSCRIPTEN_KEEPALIVE int gxspike_off_tex0(void) { return (int)gxc::kVertexUvOffset; }

} // extern "C"

int main(void) {
  if (!gxspike_build()) {
    std::printf("plan skipped: %s\n", gxspike_status());
    return 1;
  }
  std::printf("plan ok: %d vertices, %d indices, stride %d bytes\n",
              gxspike_vertex_count(), gxspike_index_count(),
              gxspike_vertex_stride());
  std::printf("cull=%d depth_test=%d depth_func=%d blend=%d\n",
              gxspike_cull_mode(), gxspike_depth_test(),
              gxspike_depth_func(), gxspike_blend_enable());
  std::printf("vs constants %d bytes, ps constants %d bytes\n",
              gxspike_vs_constants_size(), gxspike_ps_constants_size());
  std::printf("offsets: pos=%d posmtx=%d col0=%d col1=%d tex0=%d\n",
              gxspike_off_pos(), gxspike_off_posmtx(), gxspike_off_color0(),
              gxspike_off_color1(), gxspike_off_tex0());
  std::printf("---- WGSL (%d bytes) ----\n%s\n", gxspike_wgsl_len(), gxspike_wgsl());
  return 0;
}
