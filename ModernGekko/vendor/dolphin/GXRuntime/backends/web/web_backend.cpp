// SPDX-License-Identifier: GPL-3.0-or-later
//
// DolPlatformOps against the browser. Structurally this is
// backends/aurora/aurora_graphics.cpp with the live Aurora GX path deleted:
// gxcore is the only consumer, so every GX write goes frontend -> GxCoreSink ->
// DrawPlan, and web_gfx.cpp turns the plan into bytes for JavaScript.

#include "web_backend.hpp"

#include "gxruntime/gx_recomp.h"

#include <cstdio>
#include <cstring>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
// Everything the page calls must survive wasm-ld's dead-code elimination: these
// symbols have no C caller, only a JavaScript one.
#define DOLWEB_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define DOLWEB_EXPORT
#endif

namespace gx_web {

State& state() {
    static State s;
    return s;
}

namespace {

// GXAttr (graphics/aurora/include/dolphin/gx/GXEnum.h) -> CP array index.
// Spelled with literals rather than the Aurora enum: the browser build does not
// compile Aurora at all.
constexpr std::uint32_t kGxVaPos = 9;
constexpr std::uint32_t kGxVaNrm = 10;
constexpr std::uint32_t kGxVaNbt = 25;

bool gx_attr_to_cp_array(std::uint32_t attr, std::uint8_t* out) {
    if (out == nullptr)
        return false;
    if (attr == kGxVaNbt)
        attr = kGxVaNrm;
    if (attr < kGxVaPos)
        return false;
    const std::uint32_t index = attr - kGxVaPos;
    if (index >= DOL_GX_RECOMP_CP_ARRAY_COUNT)
        return false;
    *out = static_cast<std::uint8_t>(index);
    return true;
}

// Bridge the platform-level resolver (a raw pointer + available size) to the
// frontend's DolGuestResolvedRange shape. Identical in intent to
// frontend_guest_address_resolver_bridge in the Aurora backend.
bool frontend_resolver_bridge(void*, u32 address, u32 size,
                              DolGuestAddressSpace space,
                              DolGuestResourceKind resource,
                              DolGuestResolvedRange* out) {
    State& s = state();
    if (s.guest_resolver == nullptr || out == nullptr)
        return false;
    const void* data = nullptr;
    u32 available = 0;
    if (!s.guest_resolver(s.guest_resolver_user, address, size, space, resource,
                          &data, &available) ||
        data == nullptr || available < size)
        return false;
    *out = {
        .data = const_cast<void*>(data),
        .address = address,
        .size = size,
        .available = available,
        .space = space,
        .resource = resource,
    };
    return true;
}

void report_frontend_failure(const char* what) {
    State& s = state();
    if (s.frontend_failed)
        return;
    s.frontend_failed = true;
    std::fprintf(stderr,
                 "[web-gx] frontend rejected %s after %llu FIFO byte(s): %s "
                 "(opcode=0x%02X offset=%llu); sink=%s\n",
                 what, s.fifo_bytes,
                 s.frontend.last_error() ? s.frontend.last_error() : "none",
                 static_cast<unsigned>(s.frontend.last_error_opcode()),
                 static_cast<unsigned long long>(s.frontend.last_error_offset()),
                 s.sink.failure_reason() ? s.sink.failure_reason() : "none");
}

} // namespace
} // namespace gx_web

extern "C" {

// --- lifecycle --------------------------------------------------------------

DOLWEB_EXPORT bool dol_web_initialize(const DolWebBackendConfig* config);
DOLWEB_EXPORT void dol_web_shutdown(void);

static bool web_should_quit(void) { return gx_web::state().should_quit; }

static void web_present(void) {
    gx_web::State& s = gx_web::state();
    if (!s.initialized)
        return;
    if (!s.frontend_failed)
        s.sink.flush_frame();
    // WebGPU's writeBuffer takes a size that must be a multiple of 4, and the
    // page uploads each arena whole. Index blocks start 4-aligned but a draw
    // with an odd index count leaves the arena three bytes short of that, so
    // pad here rather than have the reader silently truncate the last draw.
    s.vtx.resize((s.vtx.size() + 3u) & ~std::size_t(3));
    s.idx.resize((s.idx.size() + 3u) & ~std::size_t(3));
    s.uni.resize((s.uni.size() + 3u) & ~std::size_t(3));
    ++s.present_count;
    s.frame_ready = true;
}

static void web_mark_gx_begin(void) {
    // Only the live Aurora GX decoder needed this hint; gxcore reconstructs
    // draw boundaries from the FIFO stream itself.
}

static void web_gx_write(u64 value, u8 size) {
    gx_web::State& s = gx_web::state();
    if (!s.initialized || s.frontend_failed)
        return;
    s.fifo_bytes += size;
    std::uint8_t bytes[8] = {};
    switch (size) {
    case 1:
        bytes[0] = static_cast<std::uint8_t>(value);
        break;
    case 2:
        bytes[0] = static_cast<std::uint8_t>(value >> 8u);
        bytes[1] = static_cast<std::uint8_t>(value);
        break;
    case 4:
        bytes[0] = static_cast<std::uint8_t>(value >> 24u);
        bytes[1] = static_cast<std::uint8_t>(value >> 16u);
        bytes[2] = static_cast<std::uint8_t>(value >> 8u);
        bytes[3] = static_cast<std::uint8_t>(value);
        break;
    case 8:
        for (unsigned i = 0; i < 8; ++i)
            bytes[i] = static_cast<std::uint8_t>(value >> ((7u - i) * 8u));
        break;
    default:
        return;
    }
    const std::span<const std::uint8_t> fragment(bytes, size);
    if (!s.frontend.write_fifo(fragment) || !s.frontend.flush(&s.sink))
        gx_web::report_frontend_failure("FIFO");
}

static void web_call_display_list(const void* data, u32 size) {
    gx_web::State& s = gx_web::state();
    if (!s.initialized || s.frontend_failed || data == nullptr || size == 0)
        return;
    s.fifo_bytes += size;
    const std::span<const std::uint8_t> bytes(
        static_cast<const std::uint8_t*>(data), size);
    if (!s.frontend.write_display_list(bytes, &s.sink))
        gx_web::report_frontend_failure("HLE display list");
}

static void web_set_array_guest(u32 attr, u32 guest_address, const void* data,
                                u32 size, u8 stride) {
    (void)data;
    (void)size;
    gx_web::State& s = gx_web::state();
    if (!s.initialized || s.frontend_failed || guest_address == 0u)
        return;
    std::uint8_t cp_attr = 0;
    if (!gx_web::gx_attr_to_cp_array(attr, &cp_attr)) {
        std::fprintf(stderr, "[web-gx] unsupported GX array attribute %u\n", attr);
        return;
    }
    const u32 physical = dol_gx_recomp_guest_to_physical(guest_address);
    if (!s.frontend.set_cp_array(cp_attr, physical, stride))
        std::fprintf(stderr, "[web-gx] failed to mirror CP array %u\n", attr);
}

static void web_set_array(u32 attr, const void* data, u32 size, u8 stride) {
    // No guest address, so the frontend cannot re-resolve the array later.
    // The Aurora backend has the same hole and covers it with its live GX
    // decoder; here the guest-address form is the only one that works.
    web_set_array_guest(attr, 0u, data, size, stride);
}

// Textures, TLUTs and the copy destination all reach gxcore through the
// frontend's own BP-register decode plus the guest resolver, so the metadata
// hooks the live Aurora decoder needs are no-ops here.
static void web_load_texture(u8, const void*, u32, u32, u32, u32, bool, u32, u32) {}
static void web_load_texture_guest(u8, u32, const void*, u32, u32, u32, u32,
                                   bool, u32, u32) {}
static void web_load_tlut(u8, const void*, u32, u16, u32, u32) {}
static void web_load_tlut_guest(u8, u32, const void*, u32, u16, u32, u32) {}
static void web_set_copy_destination(const void*) {}
static void web_set_copy_destination_guest(u32, const void*) {}

static void web_set_guest_address_resolver(
    DolPlatformGuestAddressResolverFn resolve, void* user) {
    gx_web::State& s = gx_web::state();
    s.guest_resolver = resolve;
    s.guest_resolver_user = user;
    if (resolve != nullptr) {
        DolGuestAddressResolver bridge;
        dol_guest_address_resolver_init_callback(
            &bridge, gx_web::frontend_resolver_bridge, nullptr);
        s.frontend.reset(&bridge);
        s.sink.set_guest_resolver(&s.frontend.state().resolver);
        s.frontend_failed = false;
    } else {
        s.frontend.reset(nullptr);
        s.sink.set_guest_resolver(nullptr);
    }
}

static void web_configure_vi(u32 tv_mode, u16 fb_width, u16 efb_height,
                             u16 xfb_height, u16 vi_width, u16 vi_height) {
    (void)tv_mode;
    (void)xfb_height;
    gx_web::State& s = gx_web::state();
    if (fb_width != 0)
        s.efb_width = fb_width;
    if (efb_height != 0)
        s.efb_height = efb_height;
    if (vi_width != 0)
        s.vi_width = vi_width;
    if (vi_height != 0)
        s.vi_height = vi_height;
}

// --- input ------------------------------------------------------------------
//
// The page writes into g_pads through dolweb_set_pad(); pad_read just copies
// the current snapshot. There is no polling to do: a browser delivers input
// events, it is not asked for them.

constexpr s8 kPadErrNoController = -1; // PAD_ERR_NO_CONTROLLER

static DolPadState g_pads[4];
static u32 g_pad_connected_mask = 1u; // port 1 is always "plugged in"

static bool web_pad_init(void) { return true; }

static u32 web_pad_read(DolPadState state[4]) {
    if (state == nullptr)
        return 0;
    for (int i = 0; i < 4; ++i) {
        state[i] = g_pads[i];
        state[i].error = (g_pad_connected_mask & (1u << i)) ? 0 : kPadErrNoController;
    }
    // PADRead returns the channels asking to be reset, and nothing here ever
    // does. Reporting a connection mask instead would make the guest call
    // PADReset every single frame.
    return 0;
}

static bool web_pad_reset(u32) { return true; }
static bool web_pad_recalibrate(u32) { return true; }
static void web_pad_control_motor(u32, u32) {}
static void web_pad_set_spec(u32) {}

// --- audio ------------------------------------------------------------------
//
// A lock-free-by-construction ring: the guest (WASM, single-threaded) writes,
// and JS drains it from the same thread between run slices, so no atomics are
// needed. Sized for ~0.5 s at 48 kHz stereo, which absorbs a slow frame
// without the AudioWorklet starving.

constexpr unsigned kAudioRingFrames = 1u << 15; // 32768 stereo frames
static s16 g_audio_ring[kAudioRingFrames * 2u];
static unsigned g_audio_write = 0;
static unsigned g_audio_read = 0;
static u32 g_audio_sample_rate = 32000;
static unsigned long long g_audio_dropped = 0;

static void web_audio_set_sample_rate(u32 sample_rate) {
    g_audio_sample_rate = (sample_rate == 48000u) ? 48000u : 32000u;
}

static void web_audio_push(const s16* samples, u32 frames) {
    if (samples == nullptr || frames == 0)
        return;
    for (u32 i = 0; i < frames; ++i) {
        const unsigned next = (g_audio_write + 1u) % kAudioRingFrames;
        if (next == g_audio_read) {
            // Full: drop the rest rather than block. The guest cannot be
            // stalled here -- there is no other thread to drain the ring.
            g_audio_dropped += frames - i;
            return;
        }
        g_audio_ring[g_audio_write * 2u] = samples[i * 2u];
        g_audio_ring[g_audio_write * 2u + 1u] = samples[i * 2u + 1u];
        g_audio_write = next;
    }
}

DOLWEB_EXPORT bool dol_web_initialize(const DolWebBackendConfig* config) {
    gx_web::State& s = gx_web::state();
    if (s.initialized)
        return true;

    const DolWebBackendConfig defaults = {
        .app_name = "GXRuntime",
        .efb_width = 640,
        .efb_height = 528,
        .graphics_logging = false,
    };
    if (config == nullptr)
        config = &defaults;
    if (config->efb_width != 0)
        s.efb_width = config->efb_width;
    if (config->efb_height != 0)
        s.efb_height = config->efb_height;
    s.graphics_logging = config->graphics_logging;

    s.frontend.set_packet_drain_enabled(true);
    s.sink.set_plan_observer(gx_web::plan_observer, nullptr);
    s.sink.set_copy_observer(gx_web::copy_observer, nullptr);

    const DolPlatformOps ops = {
        .should_quit = web_should_quit,
        .present = web_present,
        .mark_gx_begin = web_mark_gx_begin,
        .gx_write = web_gx_write,
        .call_display_list = web_call_display_list,
        .set_array = web_set_array,
        .set_array_guest = web_set_array_guest,
        .load_texture = web_load_texture,
        .load_texture_guest = web_load_texture_guest,
        .load_tlut = web_load_tlut,
        .load_tlut_guest = web_load_tlut_guest,
        .set_copy_destination = web_set_copy_destination,
        .set_copy_destination_guest = web_set_copy_destination_guest,
        .set_guest_address_resolver = web_set_guest_address_resolver,
        .configure_vi = web_configure_vi,

        .pad_init = web_pad_init,
        .pad_read = web_pad_read,
        .pad_reset = web_pad_reset,
        .pad_recalibrate = web_pad_recalibrate,
        .pad_control_motor = web_pad_control_motor,
        .pad_set_spec = web_pad_set_spec,

        .audio_set_sample_rate = web_audio_set_sample_rate,
        .audio_push = web_audio_push,
    };
    dol_platform_install(&ops);
    s.initialized = true;
    return true;
}

DOLWEB_EXPORT void dol_web_shutdown(void) {
    gx_web::State& s = gx_web::state();
    if (!s.initialized)
        return;
    dol_platform_reset();
    s.initialized = false;
}

DOLWEB_EXPORT void dol_web_request_quit(void) { gx_web::state().should_quit = true; }

DOLWEB_EXPORT bool dol_web_frame_ready(void) { return gx_web::state().frame_ready; }

DOLWEB_EXPORT void dol_web_begin_frame(void) {
    gx_web::State& s = gx_web::state();
    s.frame_ready = false;
    s.prog.clear();
    s.draws.clear();
    s.copies.clear();
    s.vtx.clear();
    s.idx.clear();
    s.uni.clear();
    s.new_pipelines.clear();
    s.wgsl.clear();
    s.new_textures.clear();
    s.texbytes.clear();
    s.freed_textures.clear();
    s.last_frame_draws = 0;
    s.last_frame_skipped = 0;
}

// --- the JavaScript-facing surface -----------------------------------------
//
// Pointers into WASM linear memory plus lengths. Every one of these is only
// valid until the next dol_web_begin_frame; the JS reader copies or uploads
// within the same turn, which it does because it runs synchronously between
// run slices.

DOLWEB_EXPORT u32 dolweb_stream_version(void) { return gx_web::kStreamVersion; }

DOLWEB_EXPORT const void* dolweb_prog(void) { return gx_web::state().prog.data(); }
DOLWEB_EXPORT u32 dolweb_prog_count(void) {
    return static_cast<u32>(gx_web::state().prog.size());
}

DOLWEB_EXPORT const void* dolweb_draws(void) { return gx_web::state().draws.data(); }
DOLWEB_EXPORT u32 dolweb_draw_count(void) {
    return static_cast<u32>(gx_web::state().draws.size());
}
DOLWEB_EXPORT u32 dolweb_draw_stride(void) { return sizeof(gx_web::DrawRecord); }

DOLWEB_EXPORT const void* dolweb_copies(void) { return gx_web::state().copies.data(); }
DOLWEB_EXPORT u32 dolweb_copy_count(void) {
    return static_cast<u32>(gx_web::state().copies.size());
}
DOLWEB_EXPORT u32 dolweb_copy_stride(void) { return sizeof(gx_web::CopyRecord); }

DOLWEB_EXPORT const void* dolweb_vtx(void) { return gx_web::state().vtx.data(); }
DOLWEB_EXPORT u32 dolweb_vtx_size(void) {
    return static_cast<u32>(gx_web::state().vtx.size());
}
DOLWEB_EXPORT const void* dolweb_idx(void) { return gx_web::state().idx.data(); }
DOLWEB_EXPORT u32 dolweb_idx_size(void) {
    return static_cast<u32>(gx_web::state().idx.size());
}
DOLWEB_EXPORT const void* dolweb_uni(void) { return gx_web::state().uni.data(); }
DOLWEB_EXPORT u32 dolweb_uni_size(void) {
    return static_cast<u32>(gx_web::state().uni.size());
}

DOLWEB_EXPORT const void* dolweb_new_pipelines(void) {
    return gx_web::state().new_pipelines.data();
}
DOLWEB_EXPORT u32 dolweb_new_pipeline_count(void) {
    return static_cast<u32>(gx_web::state().new_pipelines.size());
}
DOLWEB_EXPORT u32 dolweb_pipeline_stride(void) { return sizeof(gx_web::PipelineRecord); }
DOLWEB_EXPORT const void* dolweb_wgsl(void) { return gx_web::state().wgsl.data(); }
DOLWEB_EXPORT u32 dolweb_wgsl_size(void) {
    return static_cast<u32>(gx_web::state().wgsl.size());
}

DOLWEB_EXPORT const void* dolweb_new_textures(void) {
    return gx_web::state().new_textures.data();
}
DOLWEB_EXPORT u32 dolweb_new_texture_count(void) {
    return static_cast<u32>(gx_web::state().new_textures.size());
}
DOLWEB_EXPORT u32 dolweb_texture_stride(void) { return sizeof(gx_web::TextureRecord); }
DOLWEB_EXPORT const void* dolweb_texbytes(void) { return gx_web::state().texbytes.data(); }
DOLWEB_EXPORT u32 dolweb_texbytes_size(void) {
    return static_cast<u32>(gx_web::state().texbytes.size());
}
DOLWEB_EXPORT const void* dolweb_freed_textures(void) {
    return gx_web::state().freed_textures.data();
}
DOLWEB_EXPORT u32 dolweb_freed_texture_count(void) {
    return static_cast<u32>(gx_web::state().freed_textures.size());
}

DOLWEB_EXPORT u32 dolweb_efb_width(void) { return gx_web::state().efb_width; }
DOLWEB_EXPORT u32 dolweb_efb_height(void) { return gx_web::state().efb_height; }
DOLWEB_EXPORT u32 dolweb_vi_width(void) { return gx_web::state().vi_width; }
DOLWEB_EXPORT u32 dolweb_vi_height(void) { return gx_web::state().vi_height; }
DOLWEB_EXPORT u32 dolweb_clear_rgba(void) { return gx_web::state().clear_rgba; }
DOLWEB_EXPORT float dolweb_clear_depth(void) { return gx_web::state().clear_depth; }

DOLWEB_EXPORT u32 dolweb_vertex_stride(void) {
    return gxruntime::gxcore::kVertexStrideBytes;
}
DOLWEB_EXPORT u32 dolweb_vs_uniform_size(void) {
    return sizeof(gxruntime::gxcore::VertexShaderConstants);
}
DOLWEB_EXPORT u32 dolweb_ps_uniform_size(void) {
    return sizeof(gxruntime::gxcore::PixelShaderConstants);
}
DOLWEB_EXPORT u32 dolweb_vertex_offset_pos(void) { return gxruntime::gxcore::kVertexPosOffset; }
DOLWEB_EXPORT u32 dolweb_vertex_offset_posmtx(void) {
    return gxruntime::gxcore::kVertexPosMtxOffset;
}
DOLWEB_EXPORT u32 dolweb_vertex_offset_color0(void) {
    return gxruntime::gxcore::kVertexColor0Offset;
}
DOLWEB_EXPORT u32 dolweb_vertex_offset_color1(void) {
    return gxruntime::gxcore::kVertexColor1Offset;
}
DOLWEB_EXPORT u32 dolweb_vertex_offset_uv(void) { return gxruntime::gxcore::kVertexUvOffset; }
DOLWEB_EXPORT u32 dolweb_vertex_offset_normal(void) {
    return gxruntime::gxcore::kVertexNormalOffset;
}
DOLWEB_EXPORT u32 dolweb_vertex_offset_texmtxidx(void) {
    return gxruntime::gxcore::kVertexTexMtxIdxOffset;
}
DOLWEB_EXPORT u32 dolweb_vertex_offset_binormal(void) {
    return gxruntime::gxcore::kVertexBinormalOffset;
}
DOLWEB_EXPORT u32 dolweb_vertex_offset_tangent(void) {
    return gxruntime::gxcore::kVertexTangentOffset;
}

// Doubles, not u64: a u64 arrives in JavaScript as a BigInt, and one BigInt
// anywhere in a payload makes JSON.stringify throw. A frame counter has decades
// of headroom in a double.
DOLWEB_EXPORT double dolweb_present_count(void) {
    return static_cast<double>(gx_web::state().present_count);
}
DOLWEB_EXPORT u32 dolweb_frame_draws(void) { return gx_web::state().last_frame_draws; }
DOLWEB_EXPORT u32 dolweb_frame_skipped(void) { return gx_web::state().last_frame_skipped; }
// Texture-cache telemetry, by index: 0 hits, 1 uploads, 2 address evictions,
// 3 cap evictions, 4 EFB-copy binds, 5 undecodable. Doubles, so the page can
// read them without BigInt.
DOLWEB_EXPORT double dolweb_texture_stat(u32 which) {
    const gx_web::State& s = gx_web::state();
    switch (which) {
    case 0: return static_cast<double>(s.tex_hits);
    case 1: return static_cast<double>(s.tex_uploads);
    case 2: return static_cast<double>(s.tex_evict_address);
    case 3: return static_cast<double>(s.tex_evict_cap);
    case 4: return static_cast<double>(s.tex_efb_hits);
    case 5: return static_cast<double>(s.tex_undecodable);
    default: return -1.0;
    }
}

DOLWEB_EXPORT u32 dolweb_frontend_failed(void) {
    return gx_web::state().frontend_failed ? 1u : 0u;
}

// gxcore's whole gap inventory, in declaration order, as u64s. The page prints
// it with the names in dolweb.js -- see GapCounters in gxcore.hpp.
DOLWEB_EXPORT const void* dolweb_gap_counters(void) { return &gx_web::state().sink.counters(); }
DOLWEB_EXPORT u32 dolweb_gap_counter_count(void) {
    return sizeof(gxruntime::gxcore::GapCounters) /
           sizeof(unsigned long long);
}

// --- input / audio, called from the page ------------------------------------

DOLWEB_EXPORT void dolweb_set_pad(u32 channel, u32 buttons, int stick_x, int stick_y,
                    int substick_x, int substick_y, u32 trigger_left,
                    u32 trigger_right) {
    if (channel >= 4u)
        return;
    DolPadState& pad = g_pads[channel];
    pad.button = static_cast<u16>(buttons);
    pad.stick_x = static_cast<s8>(stick_x);
    pad.stick_y = static_cast<s8>(stick_y);
    pad.substick_x = static_cast<s8>(substick_x);
    pad.substick_y = static_cast<s8>(substick_y);
    pad.trigger_left = static_cast<u8>(trigger_left);
    pad.trigger_right = static_cast<u8>(trigger_right);
    pad.analog_a = 0;
    pad.analog_b = 0;
    pad.error = 0;
}

DOLWEB_EXPORT void dolweb_set_pad_connected(u32 mask) { g_pad_connected_mask = mask & 0xFu; }

DOLWEB_EXPORT u32 dolweb_audio_sample_rate(void) { return g_audio_sample_rate; }
DOLWEB_EXPORT u32 dolweb_audio_available(void) {
    return (g_audio_write + kAudioRingFrames - g_audio_read) % kAudioRingFrames;
}
// Copy up to `frames` stereo frames into `out` (an s16 pointer the page owns in
// WASM memory). Returns the number actually copied.
DOLWEB_EXPORT u32 dolweb_audio_pull(s16* out, u32 frames) {
    if (out == nullptr)
        return 0;
    u32 copied = 0;
    while (copied < frames && g_audio_read != g_audio_write) {
        out[copied * 2u] = g_audio_ring[g_audio_read * 2u];
        out[copied * 2u + 1u] = g_audio_ring[g_audio_read * 2u + 1u];
        g_audio_read = (g_audio_read + 1u) % kAudioRingFrames;
        ++copied;
    }
    return copied;
}
DOLWEB_EXPORT double dolweb_audio_dropped(void) {
    return static_cast<double>(g_audio_dropped);
}

} // extern "C"
