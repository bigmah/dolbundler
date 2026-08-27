// SPDX-License-Identifier: GPL-3.0-or-later
//
// DrawPlan -> flat frame stream. This is the browser's answer to the aurora
// substrate's gxcore_draw.cpp: same inputs, same decisions, but instead of
// calling wgpu it appends POD records to arenas that JavaScript reads once per
// frame. Anything that can be decided in C++ is decided here, so the JS side
// never inspects a GX register.

#include "web_backend.hpp"

#include "gxruntime/gxcore/texture_decode.hpp"

#include <algorithm>
#include <cstring>

namespace gx_web {

namespace gxc = gxruntime::gxcore;

namespace {

// Dynamic uniform offsets must be a multiple of the adapter's
// minUniformBufferOffsetAlignment. 256 is the WebGPU maximum guaranteed value,
// so aligning to it here is correct on every adapter without querying one.
constexpr std::uint32_t kUniformAlign = 256;

// Bound the persistent texture cache. Guest titles stream textures through a
// handful of buffers; without a bound the content-hashed cache grows for every
// movie frame. Evicted ids are published so JS can destroy the GPUTexture.
constexpr std::size_t kMaxCachedTextures = 4096;

std::uint64_t hash_bytes(const void* data, std::size_t size,
                         std::uint64_t seed = 0x9E3779B97F4A7C15ull) {
    // A cheap 8-bytes-at-a-time mix. Not cryptographic and not xxh3, but it is
    // only ever compared against itself, and texture content hashing sits on
    // the per-draw path so byte-at-a-time FNV would show up in a profile.
    const auto* p = static_cast<const std::uint8_t*>(data);
    std::uint64_t h = seed ^ (static_cast<std::uint64_t>(size) * 0x9E3779B185EBCA87ull);
    std::size_t i = 0;
    for (; i + 8 <= size; i += 8) {
        std::uint64_t v;
        std::memcpy(&v, p + i, 8);
        h ^= v * 0xC2B2AE3D27D4EB4Full;
        h = (h << 31) | (h >> 33);
        h *= 0x9E3779B185EBCA87ull;
    }
    for (; i < size; ++i) {
        h ^= p[i];
        h *= 0x100000001B3ull;
    }
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDull;
    h ^= h >> 29;
    return h;
}

std::uint32_t append(std::vector<std::uint8_t>& arena, const void* data,
                     std::size_t size, std::uint32_t align = 4) {
    const std::size_t pad = (align - (arena.size() % align)) % align;
    arena.insert(arena.end(), pad, 0u);
    const auto offset = static_cast<std::uint32_t>(arena.size());
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    arena.insert(arena.end(), bytes, bytes + size);
    return offset;
}

// Mirror generate_wgsl's VertexIn exactly. A pipeline whose vertex layout
// declares an attribute the shader does not (or vice versa) is a WebGPU
// validation error, so this must stay in lockstep with gxcore_shader.cpp.
std::uint32_t vertex_attr_mask(const gxc::ShaderKey& key) {
    std::uint32_t mask = 0xFFu; // locations 0..7 are unconditional
    bool emboss = false;
    for (std::uint32_t i = 0; i < key.num_tex_gens; ++i) {
        if (static_cast<gxc::TexGenType>(key.tex_gens[i].texgentype) ==
            gxc::TexGenType::EmbossMap)
            emboss = true;
    }
    if ((key.lit_valid != 0 || emboss) && key.has_vertex_normal != 0)
        mask |= 1u << 8;
    if (key.has_tex_mtx_idx != 0)
        mask |= 1u << 9;
    if (emboss && key.has_vertex_binormal != 0)
        mask |= 1u << 10;
    if (emboss && key.has_vertex_tangent != 0)
        mask |= 1u << 11;
    return mask;
}

std::uint32_t intern_pipeline(const gxc::PipelineKey& key) {
    State& s = state();
    // PipelineKey has a unique object representation, so its bytes are its
    // identity (the same memcmp identity the substrate's pipeline cache uses).
    std::string bytes(reinterpret_cast<const char*>(&key), sizeof(key));
    auto it = s.pipeline_ids.find(bytes);
    if (it != s.pipeline_ids.end())
        return it->second;

    const std::uint32_t id = s.next_pipeline_id++;
    s.pipeline_ids.emplace(std::move(bytes), id);

    const std::string wgsl = gxc::generate_wgsl(key.shader);
    const std::uint32_t wgsl_offset =
        append(s.wgsl, wgsl.data(), wgsl.size() + 1u); // NUL for UTF8ToString

    const bool textured = key.shader.textured != 0;
    PipelineRecord rec{};
    rec.id = id;
    rec.wgsl_offset = wgsl_offset;
    rec.wgsl_size = static_cast<std::uint32_t>(wgsl.size());
    rec.cull_mode = key.cull_mode;
    rec.depth_test = key.depth_test;
    rec.depth_func = key.depth_func;
    rec.depth_update = key.depth_update;
    rec.blend_enable = key.blend_enable;
    rec.blend_subtract = key.blend_subtract;
    rec.src_factor = key.src_factor;
    rec.dst_factor = key.dst_factor;
    rec.color_update = key.color_update;
    rec.alpha_update = key.alpha_update;
    rec.attr_mask = vertex_attr_mask(key.shader);
    // generate_wgsl emits a SINGLE-texmap fragment at binding 0/1 whenever the
    // used set has at most one bit, whatever texmap that bit is -- so the bind
    // group layout has to be binding 0/1 too, not 2t/2t+1. Only the genuine
    // multi-texmap case uses the per-texmap bindings.
    const std::uint32_t used =
        textured ? gxc::used_texmap_mask(key.shader) : 0u;
    rec.tex_mask = (used != 0u && gxc::texmap_popcount(used) <= 1u) ? 1u : used;
    rec.tev = key.shader.tev_valid != 0 ? 1u : 0u;
    rec.textured = textured ? 1u : 0u;
    s.new_pipelines.push_back(rec);
    return id;
}

void evict_one_texture_if_full() {
    State& s = state();
    if (s.texture_ids.size() < kMaxCachedTextures)
        return;
    // Arbitrary eviction: the cache is a working set, not an LRU, and the
    // bound only exists so a streaming title cannot grow it without limit.
    auto victim = s.texture_ids.begin();
    ++s.tex_evict_cap;
    s.freed_textures.push_back(victim->second);
    const std::uint32_t address = victim->first.address;
    auto addr_it = s.texture_addr_key.find(address);
    if (addr_it != s.texture_addr_key.end() && addr_it->second == victim->first)
        s.texture_addr_key.erase(addr_it);
    s.texture_ids.erase(victim);
}

// Decode one texmap's texture and publish it, or reuse a cached upload.
// Returns 0 when the texture cannot be resolved (the draw is then dropped, as
// the substrate does, rather than binding nothing).
std::uint32_t resolve_texture(std::uint32_t address, std::uint32_t tsize,
                              std::uint32_t format, std::uint32_t width,
                              std::uint32_t height, const void* data,
                              std::uint32_t available, bool has_tlut,
                              std::uint32_t tlut_address,
                              std::uint32_t tlut_format,
                              std::uint32_t tlut_entries, const void* tlut_data,
                              std::uint32_t tlut_available) {
    State& s = state();
    // An EFB copy destination shadows whatever the guest memory holds there.
    auto efb = s.efb_copy_textures.find(address);
    if (efb != s.efb_copy_textures.end()) {
        ++s.tex_efb_hits;
        return efb->second;
    }

    if (data == nullptr || width == 0 || height == 0)
        return 0;

    const auto* bytes = static_cast<const std::uint8_t*>(data);
    const std::uint32_t size = std::min(tsize, available);
    std::uint64_t content = hash_bytes(bytes, size);
    // Hash the PALETTE, not everything the resolver could reach from its
    // address. `tlut_available` is how many bytes remain in the guest region
    // holding the TLUT -- up to the rest of MEM1 -- so hashing that much per CI
    // draw both costs megabytes of work per frame and makes the cache key
    // change whenever anything unrelated nearby changes, which turns every bind
    // into a re-decode. A TLUT is tlut_entries 16-bit entries and nothing more.
    if (gxc::is_ci_format(format) && has_tlut && tlut_data != nullptr) {
        const std::uint32_t tlut_bytes =
            std::min(tlut_entries * 2u, tlut_available);
        content = hash_bytes(tlut_data, tlut_bytes, content);
    }

    const TextureKey key{address,      tsize,        format,      width,
                         height,       tlut_address, tlut_format, tlut_entries,
                         content};
    auto it = s.texture_ids.find(key);
    if (it != s.texture_ids.end()) {
        ++s.tex_hits;
        return it->second;
    }

    // New content at this guest address: the old decode is dead, so drop it
    // rather than accumulate one entry per historical frame of a movie.
    auto addr_it = s.texture_addr_key.find(address);
    if (addr_it != s.texture_addr_key.end()) {
        auto stale = s.texture_ids.find(addr_it->second);
        if (stale != s.texture_ids.end()) {
            ++s.tex_evict_address;
            s.freed_textures.push_back(stale->second);
            s.texture_ids.erase(stale);
        }
    }
    evict_one_texture_if_full();

    std::vector<std::uint8_t> decoded;
    if (gxc::is_ci_format(format)) {
        if (has_tlut && tlut_data != nullptr)
            decoded = gxc::decode_ci(format, width, height, bytes, size,
                                     tlut_format, tlut_entries,
                                     static_cast<const std::uint8_t*>(tlut_data),
                                     tlut_available);
    } else {
        decoded = gxc::decode_texture(format, width, height, bytes, size);
    }
    if (decoded.size() != static_cast<std::size_t>(width) * height * 4u) {
        ++s.tex_undecodable;
        // CI without a resolved palette, or a format gxcore does not decode.
        // The substrate uploads the raw GX bytes here; the browser has no GX
        // codecs, so a flat magenta stands in and the gap counter is the
        // signal. Counting it as a texture keeps the bind group valid.
        decoded.assign(static_cast<std::size_t>(width) * height * 4u, 0u);
        for (std::size_t i = 0; i + 3 < decoded.size(); i += 4) {
            decoded[i] = 0xFFu;
            decoded[i + 1] = 0x00u;
            decoded[i + 2] = 0xFFu;
            decoded[i + 3] = 0xFFu;
        }
    }

    ++s.tex_uploads;
    const std::uint32_t id = s.next_texture_id++;
    TextureRecord rec{};
    rec.id = id;
    rec.width = width;
    rec.height = height;
    rec.data_offset = append(s.texbytes, decoded.data(), decoded.size());
    rec.data_size = static_cast<std::uint32_t>(decoded.size());
    s.new_textures.push_back(rec);
    s.texture_ids.emplace(key, id);
    s.texture_addr_key[address] = key;
    return id;
}

} // namespace

std::size_t TextureKeyHash::operator()(const TextureKey& k) const noexcept {
    return static_cast<std::size_t>(hash_bytes(&k, sizeof(k)));
}

void plan_observer(const gxc::DrawPlan& plan, void*) {
    State& s = state();
    if (!plan.ok || plan.vertex_count == 0 || plan.indices.empty()) {
        ++s.last_frame_skipped;
        return;
    }

    DrawRecord rec{};
    rec.texmap_mask = 0;
    for (std::uint32_t t = 0; t < 8u; ++t)
        rec.tex_ids[t] = 0;

    if (plan.pipeline.shader.textured != 0) {
        if (plan.texmap_mask == 0u) {
            if (plan.has_texture) {
                const std::uint32_t id = resolve_texture(
                    plan.tex_address, plan.tex_size, plan.tex_format,
                    plan.tex_width, plan.tex_height, plan.tex_data,
                    plan.tex_available, plan.has_tlut, plan.tlut_address,
                    plan.tlut_format, plan.tlut_entries, plan.tlut_data,
                    plan.tlut_available);
                if (id == 0) {
                    ++s.last_frame_skipped;
                    return;
                }
                rec.texmap_mask = 1u;
                rec.tex_ids[0] = id;
            } else {
                // Shader samples texmap 0 but the plan resolved no texture:
                // the bind group would be incomplete. Drop, as the substrate
                // does by leaving textureBindGroup at 0 and failing validation.
                ++s.last_frame_skipped;
                return;
            }
        } else {
            for (std::uint32_t t = 0; t < 8u; ++t) {
                if ((plan.texmap_mask & (1u << t)) == 0u)
                    continue;
                const gxc::PlanTexture& pt = plan.textures[t];
                const std::uint32_t id =
                    pt.valid ? resolve_texture(pt.address, pt.size, pt.format,
                                               pt.width, pt.height, pt.data,
                                               pt.available, pt.has_tlut,
                                               pt.tlut_address, pt.tlut_format,
                                               pt.tlut_entries, pt.tlut_data,
                                               pt.tlut_available)
                             : 0u;
                if (id == 0) {
                    ++s.last_frame_skipped;
                    return;
                }
                rec.tex_ids[t] = id;
            }
            rec.texmap_mask = plan.texmap_mask;
        }
    }

    rec.pipeline_id = intern_pipeline(plan.pipeline);
    rec.vtx_size =
        static_cast<std::uint32_t>(plan.vertices.size() * sizeof(float));
    rec.vtx_offset = append(s.vtx, plan.vertices.data(), rec.vtx_size, 4);
    rec.idx_count = static_cast<std::uint32_t>(plan.indices.size());
    rec.idx_offset = append(s.idx, plan.indices.data(),
                            plan.indices.size() * sizeof(std::uint16_t), 4);
    rec.vsu_offset = append(s.uni, &plan.constants, sizeof(plan.constants),
                            kUniformAlign);
    rec.psu_offset =
        plan.pipeline.shader.tev_valid != 0
            ? append(s.uni, &plan.pixel_constants, sizeof(plan.pixel_constants),
                     kUniformAlign)
            : kNone;

    rec.viewport_valid = plan.viewport_valid ? 1u : 0u;
    if (plan.viewport_valid) {
        // Raw XF viewport -> logical viewport, the substrate's own formula
        // (gxcore_draw.cpp submit_draw_plan). The render target is the EFB at
        // logical size, so logical == render and no scaling step is needed.
        const float sx = plan.viewport[0];
        const float sy = plan.viewport[1];
        const float sz = plan.viewport[2];
        const float ox = plan.viewport[3];
        const float oy = plan.viewport[4];
        const float oz = plan.viewport[5];
        const float width = sx * 2.0f;
        const float height = -sy * 2.0f;
        const float znear = (oz - sz) / 1.6777215e7f;
        const float zfar = oz / 1.6777215e7f;
        rec.viewport[0] = ox - 340.0f - width / 2.0f;
        rec.viewport[1] = oy - 340.0f - height / 2.0f;
        rec.viewport[2] = width;
        rec.viewport[3] = height;
        // Reversed-Z, matching gx::UseReversedZ in the substrate and the clip-z
        // flip gxcore's generated vertex shader already applies.
        rec.viewport[4] = 1.0f - zfar;
        rec.viewport[5] = 1.0f - znear;
    }

    s.prog.push_back({static_cast<std::uint32_t>(CmdType::Draw),
                      static_cast<std::uint32_t>(s.draws.size())});
    s.draws.push_back(rec);
    ++s.last_frame_draws;
}

void copy_observer(const gxc::EfbCopyCommand& cmd, void*) {
    State& s = state();

    const std::uint32_t rgba = (cmd.clear_r << 24) | (cmd.clear_g << 16) |
                               (cmd.clear_b << 8) | cmd.clear_a;
    float depth = static_cast<float>(cmd.clear_z) / 16777215.f;
    depth = 1.0f - depth; // reversed-Z
    // Carry the copy-clear color/Z forward: it becomes the next frame's EFB
    // clear, which is how a GameCube title clears at all.
    s.clear_rgba = rgba;
    s.clear_depth = depth;

    CopyRecord rec{};
    rec.dest_address = cmd.dest_address;
    rec.format = cmd.format;
    rec.src_x = cmd.src_x;
    rec.src_y = cmd.src_y;
    rec.width = std::max<std::uint32_t>(cmd.width, 1u);
    rec.height = std::max<std::uint32_t>(cmd.height, 1u);
    rec.clear = cmd.clear ? 1u : 0u;
    rec.clear_rgba = rgba;
    rec.clear_depth = depth;
    rec.color_update = cmd.color_update ? 1u : 0u;
    rec.alpha_update = cmd.alpha_update ? 1u : 0u;
    rec.depth_update = cmd.depth_update ? 1u : 0u;

    if (cmd.format == 0xFu) {
        // Display copy (GXCopyDisp): no texture destination. Its clear becomes
        // the next frame's EFB clear, already stored above.
        rec.texture_id = 0;
    } else {
        auto it = s.efb_copy_textures.find(cmd.dest_address);
        if (it == s.efb_copy_textures.end()) {
            const std::uint32_t id = s.next_texture_id++;
            // Published with data_size 0: JS allocates a render-target texture
            // of this size rather than uploading bytes.
            TextureRecord trec{};
            trec.id = id;
            trec.width = rec.width;
            trec.height = rec.height;
            trec.data_offset = 0;
            trec.data_size = 0;
            s.new_textures.push_back(trec);
            it = s.efb_copy_textures.emplace(cmd.dest_address, id).first;
        }
        rec.texture_id = it->second;
    }

    s.prog.push_back({static_cast<std::uint32_t>(CmdType::EfbCopy),
                      static_cast<std::uint32_t>(s.copies.size())});
    s.copies.push_back(rec);
}

} // namespace gx_web
