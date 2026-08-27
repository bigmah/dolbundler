// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Shared state for the browser backend. The record layouts below are the
// contract with web/dolweb.js; every field is a u32 or an f32 so the JS side
// reads them as one Uint32Array/Float32Array view over WASM memory with no
// per-field marshalling. Never reorder a field without bumping
// kDolWebStreamVersion.

#include "gxruntime/gxcore/gxcore.hpp"
#include "gxruntime/aurora_recomp/retail_gx_frontend.hpp"
#include "gxruntime/web_backend.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace gx_web {

// Bumped when any record layout in this header changes. dolweb.js refuses to
// run against a mismatched module rather than reading garbage offsets.
inline constexpr std::uint32_t kStreamVersion = 3;

// One entry in the ordered frame program. Draws and EFB copies must execute in
// stream order (a copy ends the render pass), so the order lives here and the
// payloads live in the two arrays below.
enum class CmdType : std::uint32_t { Draw = 0, EfbCopy = 1 };

struct ProgEntry {
    std::uint32_t type;  // CmdType
    std::uint32_t index; // into draws_ or copies_
};

// 0xFFFFFFFF in a *_offset field means "absent".
inline constexpr std::uint32_t kNone = 0xFFFFFFFFu;

struct DrawRecord {
    std::uint32_t pipeline_id;
    std::uint32_t vtx_offset; // byte offset into the vertex arena
    std::uint32_t vtx_size;
    std::uint32_t idx_offset; // byte offset into the index arena
    std::uint32_t idx_count;
    std::uint32_t vsu_offset; // byte offset into the uniform arena
    std::uint32_t psu_offset; // kNone when the draw is not on the TEV path
    std::uint32_t texmap_mask;
    std::uint32_t tex_ids[8]; // host texture id per set bit of texmap_mask
    std::uint32_t viewport_valid;
    float viewport[6]; // left, top, width, height, min_depth, max_depth
};

struct CopyRecord {
    std::uint32_t dest_address;
    std::uint32_t texture_id; // host texture the EFB region resolves into
    std::uint32_t format;     // GXTexFmt; 0xF = display copy (no destination)
    std::uint32_t src_x, src_y, width, height;
    std::uint32_t clear;
    std::uint32_t clear_rgba;  // packed r<<24|g<<16|b<<8|a
    float clear_depth;         // already reversed-Z mapped
    std::uint32_t color_update, alpha_update, depth_update;
};

// Published once, the first time a pipeline key is seen. The JS side compiles
// the WGSL, builds the pipeline and keys it by id forever after.
struct PipelineRecord {
    std::uint32_t id;
    std::uint32_t wgsl_offset; // byte offset into the WGSL arena
    std::uint32_t wgsl_size;
    std::uint32_t cull_mode;
    std::uint32_t depth_test, depth_func, depth_update;
    std::uint32_t blend_enable, blend_subtract, src_factor, dst_factor;
    std::uint32_t color_update, alpha_update;
    // Bit n set => the vertex layout declares @location(n). Locations 0..7 are
    // always present; 8/9/10/11 mirror generate_wgsl's VertexIn exactly.
    std::uint32_t attr_mask;
    std::uint32_t tex_mask; // used texmaps -> texture bind group layout
    std::uint32_t tev;      // TEV path: PS uniform at group 2, texture group 3
    std::uint32_t textured;
};

struct TextureRecord {
    std::uint32_t id;
    std::uint32_t width, height;
    std::uint32_t data_offset; // byte offset into the texture-byte arena
    std::uint32_t data_size;   // width*height*4, tightly packed RGBA8
};

// Guest-identity + content hash, exactly the aurora substrate's TextureKey
// (gxcore_draw.cpp): the same guest buffer is reused for different images
// across screens, so identity alone would bind a stale decode.
struct TextureKey {
    std::uint32_t address, size, format, width, height;
    std::uint32_t tlut_address, tlut_format, tlut_entries;
    std::uint64_t content_hash;
    bool operator==(const TextureKey& o) const {
        return address == o.address && size == o.size && format == o.format &&
               width == o.width && height == o.height &&
               tlut_address == o.tlut_address && tlut_format == o.tlut_format &&
               tlut_entries == o.tlut_entries && content_hash == o.content_hash;
    }
};

struct TextureKeyHash {
    std::size_t operator()(const TextureKey& k) const noexcept;
};

struct State {
    bool initialized = false;
    bool should_quit = false;
    bool frame_ready = false;
    bool graphics_logging = false;
    bool frontend_failed = false;

    std::uint32_t efb_width = 640;
    std::uint32_t efb_height = 528;
    std::uint32_t vi_width = 640;
    std::uint32_t vi_height = 480;

    unsigned long long present_count = 0;
    unsigned long long fifo_bytes = 0;

    gxruntime::aurora_recomp::RetailGxFrontend frontend;
    gxruntime::gxcore::GxCoreSink sink;

    DolPlatformGuestAddressResolverFn guest_resolver = nullptr;
    void* guest_resolver_user = nullptr;

    // --- per-frame arenas, cleared by dol_web_begin_frame ------------------
    std::vector<ProgEntry> prog;
    std::vector<DrawRecord> draws;
    std::vector<CopyRecord> copies;
    std::vector<std::uint8_t> vtx;
    std::vector<std::uint8_t> idx;
    std::vector<std::uint8_t> uni;
    std::vector<PipelineRecord> new_pipelines;
    std::vector<std::uint8_t> wgsl;
    std::vector<TextureRecord> new_textures;
    std::vector<std::uint8_t> texbytes;
    std::vector<std::uint32_t> freed_textures;

    // --- caches that persist across frames --------------------------------
    std::unordered_map<std::string, std::uint32_t> pipeline_ids; // key bytes
    std::uint32_t next_pipeline_id = 1;
    std::unordered_map<TextureKey, std::uint32_t, TextureKeyHash> texture_ids;
    std::unordered_map<std::uint32_t, TextureKey> texture_addr_key;
    std::unordered_map<std::uint32_t, std::uint32_t> efb_copy_textures; // dest -> id
    std::uint32_t next_texture_id = 1;

    // EFB clear state carried between frames (BP 0x4F-0x51 at the last copy).
    std::uint32_t clear_rgba = 0x000000FFu;
    float clear_depth = 0.0f; // reversed-Z far

    // Frame telemetry the page shows without a rebuild.
    std::uint32_t last_frame_draws = 0;
    std::uint32_t last_frame_skipped = 0;

    // Texture-cache telemetry. `uploads` is decode+publish work, `hits` is
    // reuse; a scene where uploads keep pace with binds is thrashing, and
    // decoding a texture per draw is the most expensive thing this backend can
    // accidentally do.
    unsigned long long tex_hits = 0;
    unsigned long long tex_uploads = 0;
    unsigned long long tex_evict_address = 0; // new content at a live address
    unsigned long long tex_evict_cap = 0;     // cache bound reached
    unsigned long long tex_efb_hits = 0;      // resolved to an EFB copy target
    unsigned long long tex_undecodable = 0;   // magenta stand-in
};

State& state();

void plan_observer(const gxruntime::gxcore::DrawPlan& plan, void* user);
void copy_observer(const gxruntime::gxcore::EfbCopyCommand& cmd, void* user);

} // namespace gx_web
