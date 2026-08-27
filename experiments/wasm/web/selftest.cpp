// End-to-end check of the browser backend, with no game in the picture.
//
// It writes real GameCube GX FIFO bytes through dol_platform_gx_write -- the
// same entry point a recompiled title uses -- so the whole production path runs:
// RetailGxFrontend parses the stream, GxCoreSink builds a DrawPlan, and
// backends/web serialises it into the frame arenas the page reads. If this draws
// and the game does not, the bug is in the guest, not in any of that.
//
// Deliberately not a unit test: the point is that the bytes cross into
// JavaScript and come back as pixels, which only a browser can answer.

#include "gxruntime/gx_recomp.h"
#include "gxruntime/platform.h"
#include "gxruntime/web_backend.h"

#include <emscripten/emscripten.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

std::vector<std::uint8_t> g_fifo;
// A scrap of "guest RAM" so the resolver has something to hand back. The draws
// below use direct vertex attributes, so nothing actually reads it -- but the
// frontend refuses to start without a resolver installed.
std::vector<std::uint8_t> g_ram(0x1000, 0);

void u8v(std::uint8_t v) { g_fifo.push_back(v); }
void u16v(std::uint16_t v) {
    g_fifo.push_back(static_cast<std::uint8_t>(v >> 8));
    g_fifo.push_back(static_cast<std::uint8_t>(v));
}
void u32v(std::uint32_t v) {
    g_fifo.push_back(static_cast<std::uint8_t>(v >> 24));
    g_fifo.push_back(static_cast<std::uint8_t>(v >> 16));
    g_fifo.push_back(static_cast<std::uint8_t>(v >> 8));
    g_fifo.push_back(static_cast<std::uint8_t>(v));
}
std::uint32_t f32b(float f) {
    std::uint32_t bits;
    std::memcpy(&bits, &f, sizeof bits);
    return bits;
}

void cp(std::uint8_t reg, std::uint32_t value) {
    u8v(DOL_GX_CMD_LOAD_CP_REG);
    u8v(reg);
    u32v(value);
}
void bp(std::uint8_t reg, std::uint32_t value) {
    u8v(DOL_GX_CMD_LOAD_BP_REG);
    u32v((static_cast<std::uint32_t>(reg) << 24) | (value & 0x00FFFFFFu));
}
void xf(std::uint16_t base, std::uint32_t value) {
    u8v(DOL_GX_CMD_LOAD_XF_REG);
    u32v(base);
    u32v(value);
}
void xf_block(std::uint16_t base, const std::vector<std::uint32_t>& words) {
    u8v(DOL_GX_CMD_LOAD_XF_REG);
    u32v((static_cast<std::uint32_t>(words.size() - 1u) << 16) | base);
    for (std::uint32_t w : words)
        u32v(w);
}

bool resolve(void* user, u32 address, u32 size, DolGuestAddressSpace,
             DolGuestResourceKind, const void** data, u32* available) {
    (void)user;
    const u32 base = 0x80000000u;
    u32 offset = (address >= base) ? (address - base) : address;
    if (offset >= g_ram.size())
        return false;
    const u32 have = static_cast<u32>(g_ram.size()) - offset;
    if (have < size)
        return false;
    *data = g_ram.data() + offset;
    *available = have;
    return true;
}

// GX vertex attribute formats (GXCompType).
constexpr std::uint32_t kFmtF32 = 4;
constexpr std::uint32_t kFmtRGBA8 = 5;

void emit_state() {
    // genMode: no texgens, cull none, one colour channel.
    bp(DOL_GX_BP_REG_GENMODE, 0u);
    // zmode: test on, LEQUAL, write on.
    bp(0x40u, 0x17u);
    // cmode0: no blend, colour + alpha update.
    bp(0x41u, (1u << 3) | (1u << 4));

    // VCD: position direct (bits 9-10), colour0 direct (bits 13-14).
    cp(DOL_GX_CP_REG_VCD_LO, (1u << 9) | (1u << 13));
    cp(DOL_GX_CP_REG_VCD_HI, 0u);
    // VAT group 0: pos = 3 x f32, colour0 = RGBA / RGBA8.
    cp(DOL_GX_CP_REG_VAT_GRP0,
       1u | (kFmtF32 << 1) | (1u << 13) | (kFmtRGBA8 << 14));
    cp(DOL_GX_CP_REG_VAT_GRP1, 0u);
    cp(DOL_GX_CP_REG_VAT_GRP2, 0u);

    // One colour channel, and both the colour and the alpha LitChannel taking
    // the VERTEX colour rather than the material register. Leaving either at 0
    // renders black / fully transparent -- the two traps the spike hit.
    xf(DOL_GX_XF_CHAN_REG_BASE + 0u, 1u); // numColorChans
    xf(DOL_GX_XF_CHAN_REG_BASE + 5u, 1u); // color0 ctrl: matsource = vertex
    xf(DOL_GX_XF_CHAN_REG_BASE + 7u, 1u); // alpha0 ctrl: matsource = vertex

    // Viewport covering a 640x528 EFB, mapped so the draw lands at the origin.
    xf_block(DOL_GX_XF_VIEWPORT_BASE,
             {f32b(320.0f), f32b(-264.0f), f32b(16777215.0f),
              f32b(340.0f + 320.0f), f32b(340.0f + 264.0f), f32b(16777215.0f)});
    // Orthographic identity projection (row-dot form) + the type word.
    xf_block(DOL_GX_XF_PROJECTION_BASE,
             {f32b(1.0f), f32b(0.0f), f32b(1.0f), f32b(0.0f), f32b(-1.0f),
              f32b(0.0f), 1u});
    // Position/normal matrix index A: PN matrix 0.
    xf(DOL_GX_XF_MATRIX_INDEX_A, 0u);
}

void emit_pn_matrix(float spin) {
    // XF matrix memory rows 0..2: a 3x4 row-major model matrix. Rotating it is
    // how the page can tell a live frame from a stuck one.
    const float c = std::cos(spin), s = std::sin(spin);
    xf_block(0x0000u, {
                          f32b(c), f32b(-s), f32b(0.f), f32b(0.f),
                          f32b(s), f32b(c),  f32b(0.f), f32b(0.f),
                          f32b(0.f), f32b(0.f), f32b(1.f), f32b(0.f),
                      });
}

struct V {
    float x, y, z;
    std::uint32_t rgba;
};

void emit_draw(const V* verts, unsigned count) {
    u8v(0x98u); // GX_DRAW_TRIANGLE_STRIP, vtxfmt 0
    u16v(static_cast<std::uint16_t>(count));
    for (unsigned i = 0; i < count; ++i) {
        u32v(f32b(verts[i].x));
        u32v(f32b(verts[i].y));
        u32v(f32b(verts[i].z));
        u32v(verts[i].rgba);
    }
}

void flush() {
    // Hand the stream over in the same shape a guest would: one gx_write per
    // 32-bit word, so the frontend's partial-command path is exercised too.
    std::size_t i = 0;
    for (; i + 4 <= g_fifo.size(); i += 4) {
        const std::uint64_t word =
            (static_cast<std::uint64_t>(g_fifo[i]) << 24) |
            (static_cast<std::uint64_t>(g_fifo[i + 1]) << 16) |
            (static_cast<std::uint64_t>(g_fifo[i + 2]) << 8) |
            static_cast<std::uint64_t>(g_fifo[i + 3]);
        dol_platform_gx_write(word, 4);
    }
    for (; i < g_fifo.size(); ++i)
        dol_platform_gx_write(g_fifo[i], 1);
    g_fifo.clear();
}

bool g_ready = false;

} // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE
int dolweb_selftest_init(void) {
    const DolWebBackendConfig config = {
        .app_name = "web-selftest",
        .efb_width = 640,
        .efb_height = 528,
        .graphics_logging = false,
    };
    if (!dol_web_initialize(&config))
        return 0;
    dol_platform_set_guest_address_resolver(resolve, nullptr);
    g_ready = true;
    return 1;
}

// Produce exactly one frame. `t` animates the model matrix so a live page is
// distinguishable from a frozen one.
EMSCRIPTEN_KEEPALIVE
int dolweb_selftest_frame(double t) {
    if (!g_ready)
        return 0;
    dol_web_begin_frame();

    emit_state();
    emit_pn_matrix(static_cast<float>(t));

    const V strip[4] = {
        {-0.75f, -0.65f, 0.0f, 0xE23A2BFFu}, // red
        {0.75f, -0.65f, 0.0f, 0x0E7580FFu},  // teal
        {-0.30f, 0.75f, 0.0f, 0xD4A85CFFu},  // amber
        {0.85f, 0.35f, 0.0f, 0x2C6E49FFu},   // green
    };
    emit_draw(strip, 4);

    // A second, smaller strip with depth so the depth test and a second draw
    // in the same frame are both covered.
    const V inner[4] = {
        {-0.25f, -0.25f, 0.2f, 0xFFFFFFFFu},
        {0.25f, -0.25f, 0.2f, 0x202020FFu},
        {-0.25f, 0.25f, 0.2f, 0x808080FFu},
        {0.25f, 0.25f, 0.2f, 0xFFFFFFFFu},
    };
    emit_draw(inner, 4);

    flush();
    dol_platform_present();
    return dol_web_frame_ready() ? 1 : 0;
}

} // extern "C"

int main(void) { return 0; }
