// Browser entry point.
//
// main.c owns a blocking run loop; a browser cannot have one. This file is the
// same boot sequence split into a boot call and a bounded step call, so the
// page can drive the guest from requestAnimationFrame and hand control back
// every frame. Everything else -- MMIO routing, HLE policy, interrupts -- is
// the same code the native host uses.
#include "generated.h"

#include "gxruntime/boot.h"
#include "gxruntime/dvd.h"
#include "gxruntime/loader.h"
#include "gxruntime/platform.h"
#include "gxruntime/web_backend.h"
#include "host/audio.h"
#include "host/hle.h"
#include "host/interrupt.h"
#include "host/mmio.h"

#include <emscripten.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WEB_DOL_PATH  "/main.dol"
#define WEB_CARD_PATH "/card.dolcard"

static CPUState g_cpu;
static DolLayout g_layout;
static bool g_booted;
static const char* g_stop_reason;
static unsigned long long g_blocks;

// The disc lives in a JavaScript ArrayBuffer (or, later, an OPFS handle), not
// in WASM linear memory: a 650 MB-1.4 GB image inside the module's heap would
// be the single largest thing in the WebContent process. This copies straight
// into the guest RAM pointer the DVD layer hands us.
EM_JS(unsigned, web_js_disc_read, (double offset, unsigned size, unsigned dest), {
    if (typeof Module.discRead !== 'function') return 0;
    return Module.discRead(offset, size, dest);
});

static u32 web_disc_read(void* user, u64 offset, u32 size, void* dest) {
    (void)user;
    return web_js_disc_read((double)offset, size, (unsigned)(uintptr_t)dest);
}

static void instruction_fallback(CPUState* ctx, u32 raw, u32 cia) {
    // Identical policy to the native host: this runtime models no cache and no
    // DMA, so cache ops and unmodeled SPR access are no-ops. See main.c.
    if ((raw >> 26) == 31u) {
        const u32 xo = (raw >> 1) & 0x3FFu;
        if (xo == 982u || xo == 86u || xo == 54u || xo == 470u || xo == 467u) {
            ctx->pc = cia + 4u;
            return;
        }
        if (xo == 339u) {
            ctx->gpr[(raw >> 21) & 31u] = 0;
            ctx->pc = cia + 4u;
            return;
        }
    }
    fprintf(stderr, "[fallback] unhandled instruction 0x%08X at 0x%08X\n", raw, cia);
    ctx->exception |= PPC_EXC_PROGRAM;
}

EMSCRIPTEN_KEEPALIVE
int dolweb_boot(int have_disc) {
    if (g_booted)
        return 1;

    const DolWebBackendConfig backend = {
        .app_name = "StrikersRecomp",
        .efb_width = 640,
        .efb_height = 528,
        .graphics_logging = false,
    };
    if (!dol_web_initialize(&backend)) {
        fprintf(stderr, "[boot] web backend init failed\n");
        return 0;
    }

    if (!cpu_init(&g_cpu)) {
        fprintf(stderr, "[boot] cpu_init failed\n");
        return 0;
    }
    if (!dol_load_into_ram(&g_cpu, WEB_DOL_PATH, &g_layout)) {
        fprintf(stderr, "[boot] could not load %s\n", WEB_DOL_PATH);
        return 0;
    }
    if (g_layout.entry_point != DOLRECOMP_ENTRY_POINT) {
        fprintf(stderr,
                "[boot] DOL entry 0x%08X != recompiled entry 0x%08X "
                "(this DOL does not match this build)\n",
                g_layout.entry_point, (u32)DOLRECOMP_ENTRY_POINT);
        return 0;
    }

    boot_setup_os_globals(&g_cpu, &g_layout);
    if (!mmio_install(&g_cpu)) {
        fprintf(stderr, "[boot] mmio_install failed\n");
        return 0;
    }
    if (!hle_card_open(WEB_CARD_PATH))
        fprintf(stderr, "[card] slot A unavailable; continuing with no card\n");
    hle_install(&g_cpu);

    if (have_disc) {
        if (!dvd_open_reader(web_disc_read, NULL))
            fprintf(stderr, "[boot] disc reader rejected the image\n");
    }
    mmio_set_disc_present(dvd_image_ready());

    g_cpu.instruction_fallback = instruction_fallback;
    // Same FP-context policy as the native host (see main.c): this host
    // restores FP eagerly, so keep execute-regardless semantics.
    ppc_lazy_fp_set_enabled(false);
    g_cpu.pc = g_layout.entry_point;

    g_booted = true;
    g_stop_reason = NULL;
    printf("[run] booting at 0x%08X\n", g_cpu.pc);
    return 1;
}

// Run at most `max_blocks` recompiled blocks. Returns 1 when the guest reached
// a frame boundary (present()), 0 when the budget ran out mid-frame, and a
// negative value when the run stopped for good.
EMSCRIPTEN_KEEPALIVE
int dolweb_step(unsigned max_blocks, double max_ms) {
    if (!g_booted)
        return -1;
    const double deadline = (max_ms > 0.0) ? emscripten_get_now() + max_ms : 0.0;
    // The page has consumed the previous frame by the time it calls back, so
    // clearing here (rather than unconditionally) keeps a frame that spans two
    // budget slices intact.
    if (dol_web_frame_ready())
        dol_web_begin_frame();

    for (unsigned i = 0; i < max_blocks; ++i) {
        if (dol_platform_should_quit()) {
            g_stop_reason = "quit requested";
            return -2;
        }
        interrupt_poll(&g_cpu);
        hle_poll_callback(&g_cpu);

        const u32 pc = g_cpu.pc;
        if (!dolrecomp_call(&g_cpu, pc)) {
            fprintf(stderr,
                    "[run] left recompiled code: pc=0x%08X (lr=0x%08X) after "
                    "%llu blocks\n",
                    g_cpu.pc, g_cpu.lr, g_blocks);
            g_stop_reason = "pc left recompiled code";
            return -3;
        }
        if (g_cpu.exception) {
            // `sc` is only ever a post-cache-op sync barrier on GameCube; the
            // SDK's vector syncs and returns. See main.c.
            if (g_cpu.exception == PPC_EXC_SYSTEM_CALL) {
                g_cpu.exception = 0;
                ppc_rfi(&g_cpu, g_cpu.pc);
                continue;
            }
            fprintf(stderr, "[run] cpu exception 0x%08X at pc=0x%08X\n",
                    g_cpu.exception, g_cpu.pc);
            g_stop_reason = "cpu exception";
            return -4;
        }
        ++g_blocks;
        if (dol_web_frame_ready())
            return 1;
        // A block budget alone is a bad clock -- one recompiled block can be a
        // whole function. Check a wall deadline often enough that a long boot
        // sequence still yields to the browser, rarely enough that the clock
        // read is not the profile.
        if (deadline != 0.0 && (i & 0x3FFu) == 0x3FFu &&
            emscripten_get_now() >= deadline)
            return 0;
    }
    return 0;
}

// Host-side counters the page can read without a stderr channel. Index order is
// the contract with dolboot.js's diag() helper.
extern unsigned long long g_mmio_fifo_writes;
extern unsigned long long g_mmio_bp_writes;
extern unsigned long long g_mmio_pe_finishes;

EMSCRIPTEN_KEEPALIVE
double dolweb_diag(int which) {
    switch (which) {
    case 0: return (double)g_mmio_fifo_writes;
    case 1: return (double)g_mmio_bp_writes;
    case 2: return (double)g_mmio_pe_finishes;
    default: return -1.0;
    }
}

EMSCRIPTEN_KEEPALIVE
const char* dolweb_stop_reason(void) {
    return g_stop_reason != NULL ? g_stop_reason : "";
}

EMSCRIPTEN_KEEPALIVE
double dolweb_blocks(void) { return (double)g_blocks; }

EMSCRIPTEN_KEEPALIVE
unsigned dolweb_guest_pc(void) { return g_cpu.pc; }

// The page allocates the audio staging buffer once and reuses it; malloc/free
// are not otherwise exported.
EMSCRIPTEN_KEEPALIVE
void* dolweb_alloc(unsigned size) { return malloc(size); }
EMSCRIPTEN_KEEPALIVE
void dolweb_free(void* p) { free(p); }

int main(void) {
    // Nothing to do at load time: the page boots explicitly once the user has
    // supplied a disc. -sINVOKE_RUN=0 means this is never called anyway.
    return 0;
}
