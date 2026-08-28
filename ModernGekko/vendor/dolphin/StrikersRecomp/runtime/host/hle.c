// SPDX-License-Identifier: GPL-3.0-or-later
#include "hle.h"
#include "gxruntime/hle.h"
#include "gxruntime/hle_abi.h"
#include "host/audio.h"
#include "host/interrupt.h"
#include "host/sdk_map.h"
#include "host/hle_physics.h"
#include "host/hle_input.h"
#include "host/hle_offsets.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef void (*HleHandler)(CPUState* cpu);

typedef struct {
    const char* name;
    HleHandler  fn;
} HleEntry;

typedef struct {
    u32         address;
    HleHandler  fn;
    const char* name;
} HleAddrEntry;

typedef struct {
    HleHandler intercept;
    HleHandler notify;
} HleDispatchEntry;

// Globals for Strikers HLE layer
static bool g_movie_log = false;
static bool g_movie_cadence_log = false;
static bool g_auto_skip_card_prompt = false;
static bool g_state_log = false;
static u64 g_auto_input_last_pulse = ~(u64)0;
static u64 g_auto_input_once_frame = 0;
static u64 g_auto_input_once_sent = ~(u64)0;
static u64 g_auto_skip_card_last_pulse = ~(u64)0;
static u64 g_movie_cadence_present_count;
static u64 g_movie_cadence_texframe_changes;
static double g_movie_cadence_start_time;
static s32 g_movie_cadence_last_texframe;
static bool g_movie_cadence_started;

#ifdef STRIKERSRECOMP_HAS_BACKEND
static u32 g_gx_begin_count;
#endif

// ---------------------------------------------------------------------------
// Strikers specific notify wrappers and logging helpers
// ---------------------------------------------------------------------------

static double host_time_seconds(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static u32 thp_audio_valid_samples(CPUState* cpu, u32* output_valid) {
    const u32 base = STRIKERS_THP_SIMPLE_CONTROL; // THP_SIMPLE_CONTROL
    if (base == 0u) {                 // no THP player identified for this title
        if (output_valid != NULL)
            *output_valid = 0u;
        return 0u;
    }
    u32 total = 0;
    for (u32 i = 0; i < 6u; i++)
        total += mem_read32(cpu, base + 0x16Cu + i * 12u + 8u);

    if (output_valid != NULL) {
        const u32 output_index = mem_read32(cpu, base + 0x1B8u);
        *output_valid = output_index < 6u
            ? mem_read32(cpu, base + 0x16Cu + output_index * 12u + 8u)
            : 0u;
    }
    return total;
}

static void movie_cadence_present(CPUState* cpu) {
    if (!g_movie_cadence_log)
        return;

    const u32 base = STRIKERS_THP_SIMPLE_CONTROL; // THP_SIMPLE_CONTROL
    if (base == 0u || mem_read8(cpu, base + 0x6Cu) == 0u) {
        g_movie_cadence_started = false;
        return;
    }

    const s32 tex_frame = (s32)mem_read32(cpu, base + 0x168u);
    const double now = host_time_seconds();
    if (!g_movie_cadence_started) {
        g_movie_cadence_started = true;
        g_movie_cadence_start_time = now;
        g_movie_cadence_present_count = 0;
        g_movie_cadence_texframe_changes = 0;
        g_movie_cadence_last_texframe = tex_frame;
    }

    g_movie_cadence_present_count++;
    if (tex_frame != g_movie_cadence_last_texframe) {
        g_movie_cadence_texframe_changes++;
        g_movie_cadence_last_texframe = tex_frame;
    }

    const double elapsed = now - g_movie_cadence_start_time;
    if (elapsed >= 1.0) {
        u32 audio_output_valid = 0;
        const u32 audio_valid = thp_audio_valid_samples(cpu, &audio_output_valid);
        fprintf(stderr,
                "[movie-cadence] seconds=%.3f presents=%llu "
                "texframe_changes=%llu last_texframe=%d audio_dec=%u "
                "audio_out=%u audio_valid=%u audio_out_valid=%u\n",
                elapsed,
                (unsigned long long)g_movie_cadence_present_count,
                (unsigned long long)g_movie_cadence_texframe_changes,
                g_movie_cadence_last_texframe,
                mem_read32(cpu, base + 0x1B4u),
                mem_read32(cpu, base + 0x1B8u),
                audio_valid,
                audio_output_valid);
        g_movie_cadence_start_time = now;
        g_movie_cadence_present_count = 0;
        g_movie_cadence_texframe_changes = 0;
    }
}

static bool guest_backchain_contains(CPUState* cpu, u32 start, u32 end) {
    if (cpu->lr >= start && cpu->lr < end)
        return true;
    u32 sp = cpu->gpr[1];
    const u32 ram_end = GC_RAM_BASE + cpu->ram_size;
    for (unsigned frame = 0; frame < 24u; ++frame) {
        if (sp < GC_RAM_BASE || sp + 8u > ram_end)
            break;
        const u32 caller = mem_read32(cpu, sp);
        if (caller <= sp || caller < GC_RAM_BASE || caller + 8u > ram_end)
            break;
        const u32 ret = mem_read32(cpu, caller + 4u);
        if (ret >= start && ret < end)
            return true;
        sp = caller;
    }
    return false;
}

static void matrix_log_floats(CPUState* cpu, const char* name, u32 addr, u32 count) {
    fprintf(stderr, "[matrix] %s addr=0x%08X:\n", name, addr);
    for (u32 row = 0; row < count / 4u; row++) {
        fprintf(stderr, "  % .7g % .7g % .7g % .7g\n",
                (double)guest_read_f32(cpu, addr + row * 16u),
                (double)guest_read_f32(cpu, addr + row * 16u + 4u),
                (double)guest_read_f32(cpu, addr + row * 16u + 8u),
                (double)guest_read_f32(cpu, addr + row * 16u + 12u));
    }
}

static void matrix_log_frame(CPUState* cpu) {
    static int matrix_log_enabled = -1;
    if (matrix_log_enabled < 0)
        matrix_log_enabled = getenv("STRIKERS_MATRIX_LOG") != NULL ? 1 : 0;
    if (!matrix_log_enabled)
        return;

    static unsigned frame_matrix_count = 0;
    if (frame_matrix_count >= 120u)
        return;
    ++frame_matrix_count;
    matrix_log_floats(cpu, "gx_proj     ", STRIKERS_GX_PROJ_MATRIX, 16u);
    matrix_log_floats(cpu, "gx_modelview", STRIKERS_GX_MODELVIEW_MATRIX, 12u);
    matrix_log_floats(cpu, "gx_mview    ", STRIKERS_GX_MVIEW_MATRIX, 12u);
}

// ---------------------------------------------------------------------------
// Custom Notify Hooks
// ---------------------------------------------------------------------------

static void notify_OSSleepThread(CPUState* cpu) {
    if (g_hle_log) {
        fprintf(stderr, "[trace] OSSleepThread queue=0x%08X context=0x%08X\n",
                hle_arg_u32(cpu, 0), mem_read32(cpu, STRIKERS_OS_CONTEXT_POINTER));
    }
}

static void notify_DSPSendMailToDSP(CPUState* cpu) {
    audio_dsp_mail(cpu, hle_arg_u32(cpu, 0));
}

static void notify_audio_api(CPUState* cpu) {
    (void)cpu;
}

static void notify_OSResetSystem(CPUState* cpu) {
    if (g_hle_log) {
        fprintf(stderr, "[hle] OSResetSystem reset=%u forceMenu=%u\n",
                hle_arg_u32(cpu, 0), hle_arg_u32(cpu, 1));
    }
}

static void notify_OSLoadContext(CPUState* cpu) {
    interrupt_restore_fpu_context(cpu, hle_arg_u32(cpu, 0));
}

static void notify_SetNextState(CPUState* cpu) {
    u32 manager = hle_arg_u32(cpu, 0);
    s32 state = (s32)hle_arg_u32(cpu, 1);
    if (g_hle_log)
        fprintf(stderr, "[state] nlTaskManager::SetNextState manager=0x%08X state=%d\n",
                manager, state);
}

static void notify_TaskManagerStartup(CPUState* cpu) {
    u32 manager = hle_arg_u32(cpu, 0);
    if (g_hle_log)
        fprintf(stderr, "[state] nlTaskManager::Startup manager=0x%08X\n", manager);
}

static void notify_SHMainMenuUpdate(CPUState* cpu) {
    u32 menu = hle_arg_u32(cpu, 0);
    if (g_hle_log) {
        static s32 last_state = -1;
        s32 state = (s32)mem_read32(cpu, menu + 0x10u);
        if (state != last_state) {
            fprintf(stderr, "[state] SHMainMenu::Update menu=0x%08X state %d -> %d\n",
                    menu, last_state, state);
            last_state = state;
        }
    }
}

static void notify_IChooseSideCheckControllers(CPUState* cpu) {
    u32 choose_side = hle_arg_u32(cpu, 0);
    if (g_hle_log) {
        static s32 last_state = -1;
        s32 state = (s32)mem_read32(cpu, choose_side + 0x10u);
        if (state != last_state) {
            fprintf(stderr, "[state] IChooseSide::CheckControllers menu=0x%08X state %d -> %d\n",
                    choose_side, last_state, state);
            last_state = state;
        }
    }
}

static void notify_IChooseSidePositionController(CPUState* cpu) {
    u32 choose_side = hle_arg_u32(cpu, 0);
    if (g_hle_log) {
        static s32 last_state = -1;
        s32 state = (s32)mem_read32(cpu, choose_side + 0x10u);
        if (state != last_state) {
            fprintf(stderr, "[state] IChooseSide::PositionController menu=0x%08X state %d -> %d\n",
                    choose_side, last_state, state);
            last_state = state;
        }
    }
}

static void notify_MovieStart(CPUState* cpu) {
    (void)cpu;
    if (g_movie_log)
        fprintf(stderr, "[movie] MovieStart\n");
}

static void notify_MovieStop(CPUState* cpu) {
    (void)cpu;
    if (g_movie_log)
        fprintf(stderr, "[movie] MovieStop\n");
}

static void notify_MoviePlay(CPUState* cpu) {
    (void)cpu;
}

static void notify_THPSimpleOpen(CPUState* cpu) {
    char filename[256];
    hle_read_cstr(cpu, hle_arg_u32(cpu, 0), filename, sizeof filename);
    if (g_movie_log)
        fprintf(stderr, "[movie] THPSimpleOpen filename='%s'\n", filename);
}

static void notify_THPSimpleSetBuffer(CPUState* cpu) {
    if (g_movie_log)
        fprintf(stderr, "[movie] THPSimpleSetBuffer buffer=0x%08X\n", hle_arg_u32(cpu, 0));
}

static void notify_THPSimplePreLoad(CPUState* cpu) {
    if (g_movie_log)
        fprintf(stderr, "[movie] THPSimplePreLoad\n");
}

static void notify_THPSimpleDecode(CPUState* cpu) {
    (void)cpu;
}

static void notify_THPVideoDecode(CPUState* cpu) {
    (void)cpu;
}

static void notify_THPVideoDecodeReturn(CPUState* cpu) {
    (void)cpu;
}

static void notify_THPSimpleDecodeReturn(CPUState* cpu) {
    (void)cpu;
}

static void notify_LCEnable(CPUState* cpu) {
    (void)cpu;
}

static void notify_LCDisable(CPUState* cpu) {
    (void)cpu;
}

static void notify_GXBegin(CPUState* cpu) {
#ifdef STRIKERSRECOMP_HAS_BACKEND
    ++g_gx_begin_count;
    if (ball_state_log_enabled() && guest_backchain_contains(cpu, STRIKERS_BALL_DRAW_FUN_START, STRIKERS_BALL_DRAW_FUN_END)) {
        fprintf(stderr, "[ball-draw] guest-frame=%llu begin=%u prim=%u fmt=%u count=%u lr=0x%08X\n",
                (unsigned long long)(cpu->timebase / 675000ull),
                g_gx_begin_count, hle_arg_u32(cpu, 0), hle_arg_u32(cpu, 1),
                hle_arg_u32(cpu, 2), cpu->lr);
    }
#endif
}

static void notify_GXLoadPosMtxImm(CPUState* cpu) {
    static int matrix_log_enabled = -1;
    if (matrix_log_enabled < 0)
        matrix_log_enabled = getenv("STRIKERS_MATRIX_LOG") != NULL ? 1 : 0;
    const bool ball_load = ball_state_log_enabled() && guest_backchain_contains(cpu, STRIKERS_BALL_DRAW_FUN_START, STRIKERS_BALL_DRAW_FUN_END);
    if (!matrix_log_enabled && !ball_load)
        return;

    const u32 mtx_addr = hle_arg_u32(cpu, 0);
    const u32 pn_idx = hle_arg_u32(cpu, 1);
    u8 raw[48];
    copy_guest_to_host(cpu, mtx_addr, raw, sizeof raw);
    float m[12];
    for (int i = 0; i < 12; i++) {
        u32 b = ((u32)raw[i * 4] << 24) | ((u32)raw[i * 4 + 1] << 16) |
                ((u32)raw[i * 4 + 2] << 8) | (u32)raw[i * 4 + 3];
        memcpy(&m[i], &b, sizeof(float));
    }
    if (ball_load) {
        static unsigned ball_matrix_count = 0;
        ++ball_matrix_count;
        if (ball_matrix_count <= 40u || (ball_matrix_count % 60u) == 0u) {
            fprintf(stderr,
                    "[ball-posmtx] load=%u guest-frame=%llu begin=%u "
                    "addr=0x%08X pn=%u lr=0x%08X\n"
                    "  % .7g % .7g % .7g % .7g\n"
                    "  % .7g % .7g % .7g % .7g\n"
                    "  % .7g % .7g % .7g % .7g\n",
                    ball_matrix_count,
                    (unsigned long long)(cpu->timebase / 675000ull),
#ifdef STRIKERSRECOMP_HAS_BACKEND
                    g_gx_begin_count,
#else
                    0u,
#endif
                    mtx_addr, pn_idx, cpu->lr,
                    (double)m[0], (double)m[1], (double)m[2], (double)m[3],
                    (double)m[4], (double)m[5], (double)m[6], (double)m[7],
                    (double)m[8], (double)m[9], (double)m[10], (double)m[11]);
        }
    }
}

static void notify_GXCopyTex(CPUState* cpu) {
    (void)cpu;
}

static void notify_GXCopyDisp(CPUState* cpu) {
    // DIAG logic
    {
        static int s_cutscene_diag = -1;
        if (s_cutscene_diag < 0)
            s_cutscene_diag = getenv("STRIKERS_CUTSCENE_DIAG") != NULL ? 1 : 0;
        if (s_cutscene_diag) {
            static u64 s_diag_frame = 0;
            u8 render_world = mem_read8(cpu, STRIKERS_RENDER_WORLD_GLOBAL); // g_bRenderWorld
            fprintf(stderr, "[cutscene] frame=%llu g_bRenderWorld=%u\n",
                    (unsigned long long)++s_diag_frame, render_world);
        }
    }
    matrix_log_frame(cpu);
    movie_cadence_present(cpu);
    dol_hle_GXCopyDisp(cpu);
}

static void notify_VIConfigure(CPUState* cpu) {
    (void)cpu;
}

static void notify_VIWaitForRetrace(CPUState* cpu) {
    (void)cpu;
}

static void notify_GXLoadTexObj(CPUState* cpu) {
    (void)cpu;
}

static void notify_GXLoadTlut(CPUState* cpu) {
    (void)cpu;
}

static void strikers_hle_GXBegin(CPUState* cpu) {
    notify_GXBegin(cpu);
    dol_hle_GXBegin(cpu);
}

// ---------------------------------------------------------------------------
// Input Script & Custom PAD Handlers
// ---------------------------------------------------------------------------

static void hle_PADRead(CPUState* cpu) {
    // 1. Call clean standard pad read
    dol_hle_PADRead(cpu);

    // 2. Read what standard pad read wrote to memory
    u32 out = hle_arg_u32(cpu, 0);
    u16 buttons = mem_read16(cpu, out);
    s8 stick_x = (s8)mem_read8(cpu, out + 2u);
    s8 stick_y = (s8)mem_read8(cpu, out + 3u);
    u8 analog_a = mem_read8(cpu, out + 8u);
    bool connected = mem_read8(cpu, out + 10u) == 0;

    // 3. Apply custom test scripts
    if (g_auto_input) {
        const u64 frame = cpu->timebase / 675000ull;
        if (frame >= 135u && ((frame - 135u) % 90u) < 12u) {
            buttons |= 0x0100u; // A
            analog_a = 0xFFu;
            connected = true;
        }
    }
    if (input_script_apply(cpu, &buttons, &stick_x, &stick_y, &analog_a))
        connected = true;

    // Mash to gameplay screens
    mash_to_gameplay_apply(cpu, &buttons, &stick_x, &analog_a);
    if (g_mash_to_gameplay)
        connected = true;

    // 4. Write back to guest memory
    mem_write16(cpu, out, buttons);
    mem_write8(cpu, out + 2u, (u8)stick_x);
    mem_write8(cpu, out + 3u, (u8)stick_y);
    mem_write8(cpu, out + 8u, analog_a);
    mem_write8(cpu, out + 10u, (u8)(connected ? 0 : -1));
}


// ---------------------------------------------------------------------------
// Handler registry
// ---------------------------------------------------------------------------

static const HleEntry kHandlers[] = {
    { "OSReport",          dol_hle_OSReport },
    { "ARInit",            dol_hle_ARInit },
    { "ARGetBaseAddress",  dol_hle_ARGetBaseAddress },
    { "ARGetSize",         dol_hle_ARGetSize },
    { "ARGetDMAStatus",    dol_hle_ARGetDMAStatus },
    { "ARStartDMA",        dol_hle_ARStartDMA },
    { "CARDInit",          dol_hle_CARDInit },
    { "CARDProbe",         dol_hle_CARDProbe },
    { "CARDProbeEx",       dol_hle_CARDProbeEx },
    { "CARDGetResultCode", dol_hle_CARDGetResultCode },
    { "CARDGetFastMode",   dol_hle_CARDGetFastMode },
    { "CARDGetXferredBytes", dol_hle_CARDGetXferredBytes },
    { "CARDMountAsync",    dol_hle_CARDMountAsync },
    { "CARDCheckExAsync",  dol_hle_CARDCheckExAsync },
    { "CARDCheckAsync",    dol_hle_CARDCheckAsync },
    { "CARDFreeBlocks",    dol_hle_CARDFreeBlocks },
    { "CARDOpen",          dol_hle_CARDOpen },
    { "CARDCreateAsync",   dol_hle_CARDCreateAsync },
    { "CARDDeleteAsync",   dol_hle_CARDDeleteAsync },
    { "CARDGetStatus",     dol_hle_CARDGetStatus },
    { "CARDSetStatusAsync", dol_hle_CARDSetStatusAsync },
    { "CARDGetSerialNo",   dol_hle_CARDGetSerialNo },
    { "CARDUnmount",       dol_hle_CARDUnmount },
    { "CARDClose",         dol_hle_CARDClose },
    { "CARDReadAsync",     dol_hle_CARDReadAsync },
    { "CARDWriteAsync",    dol_hle_CARDWriteAsync },
    // These used to be spelled as Strikers addresses in kAddrHandlers even
    // though every one of them is an SDK symbol the generated table already
    // names. By name they follow the disc.
    { "ARQPostRequest",    dol_hle_ARQPostRequest },
    { "DSPCheckMailToDSP", dol_hle_return_zero },
    { "DVDInit",           dol_hle_DVDInit },
    { "DVDConvertPathToEntrynum", dol_hle_DVDConvertPathToEntrynum },
    { "DVDFastOpen",       dol_hle_DVDFastOpen },
    { "DVDClose",          dol_hle_DVDClose },
    { "DVDReadAsyncPrio",  dol_hle_DVDReadAsyncPrio },
    { "DVDGetCommandBlockStatus", dol_hle_DVDGetCommandBlockStatus },
    { "DVDGetDriveStatus", dol_hle_DVDGetDriveStatus },
    { "OSGetResetButtonState", dol_hle_OSGetResetButtonState },
    { "LCStoreBlocks",     dol_hle_LCStoreBlocks },
    { "LCStoreData",       dol_hle_LCStoreData },
    { "LCQueueWait",       dol_hle_noop },
#ifdef STRIKERSRECOMP_HAS_BACKEND
    { "PSMTXConcat",       dol_hle_PSMTXConcat },
    { "GXSetArray",        dol_hle_GXSetArray },
    { "GXBegin",           strikers_hle_GXBegin },
    { "GXCallDisplayList", dol_hle_GXCallDisplayList },
#endif
    { "PADReset",          dol_hle_PADReset },
    { "PADRecalibrate",    dol_hle_PADRecalibrate },
    { "PADInit",           dol_hle_PADInit },
    { "PADRead",           hle_PADRead },
    { "PADControlMotor",   dol_hle_PADControlMotor },
    { "PADSetSpec",        dol_hle_PADSetSpec },
};

// The four functions here have no SDK name and appear in no signature
// database: two are static SDK internals, two belong to the game's own audio
// middleware. A title that has not had them identified leaves them at zero,
// which is outside the dispatch range and therefore installs nothing.
static const HleAddrEntry kAddrHandlers[] = {
    { GAME_ADDR_OS_INIT_AUDIO_SYSTEM, dol_hle_noop,          "__OSInitAudioSystem" },
    { GAME_ADDR_AI_SRC_INIT,          dol_hle_noop,          "__AI_SRC_INIT" },
    { GAME_ADDR_SAL_INIT_DSP,         dol_hle_salInitDsp,    "salInitDsp" },
    { GAME_ADDR_ARAM_UPLOAD_DATA,     dol_hle_aramUploadData, "aramUploadData" },
    { GAME_ADDR_CARD_FORMAT_ASYNC,    dol_hle_CARDFormatAsync, "CARDFormatAsync" },
};

// Notifications on SDK functions, by name.
static const HleEntry kNameNotify[] = {
    { "OSSleepThread",     notify_OSSleepThread },
    { "DSPSendMailToDSP",  notify_DSPSendMailToDSP },
    { "OSResetSystem",     notify_OSResetSystem },
    { "LCEnable",          notify_LCEnable },
    { "LCDisable",         notify_LCDisable },
    { "OSLoadContext",     notify_OSLoadContext },
#ifdef STRIKERSRECOMP_HAS_BACKEND
    { "GXCopyDisp",        notify_GXCopyDisp },
    { "GXCopyTex",         notify_GXCopyTex },
    { "GXLoadTexObj",      notify_GXLoadTexObj },
    { "GXLoadTlut",        notify_GXLoadTlut },
    { "GXLoadPosMtxImm",   notify_GXLoadPosMtxImm },
    { "VIWaitForRetrace",  notify_VIWaitForRetrace },
    { "VIConfigure",       notify_VIConfigure },
#endif
};

// Notifications on the game's own functions. All zero for a title whose
// middleware has not been identified, which disables the feature rather than
// hooking an unrelated address.
static const HleAddrEntry kNotify[] = {
    { GAME_ADDR_SND_FX_START_PARA_INFO, notify_audio_api, "sndFXStartParaInfo" },
    { GAME_ADDR_SND_STREAM_ACTIVATE,    notify_audio_api, "sndStreamActivate" },
    { GAME_ADDR_SND_PUSH_GROUP,         notify_audio_api, "sndPushGroup" },
    { GAME_ADDR_SND_SEQ_PLAY_EX,        notify_audio_api, "sndSeqPlayEx" },
    { GAME_ADDR_TASK_SET_NEXT_STATE,    notify_SetNextState, "nlTaskManager::SetNextState" },
    { GAME_ADDR_TASK_STARTUP,           notify_TaskManagerStartup, "nlTaskManager::Startup" },
    { GAME_ADDR_MAIN_MENU_UPDATE,       notify_SHMainMenuUpdate, "SHMainMenu::Update" },
    { GAME_ADDR_CHOOSE_SIDE_CHECK,      notify_IChooseSideCheckControllers, "IChooseSide::CheckControllers" },
    { GAME_ADDR_CHOOSE_SIDE_POSITION,   notify_IChooseSidePositionController, "IChooseSide::PositionController" },
    { GAME_ADDR_MOVIE_PLAY,             notify_MoviePlay,  "MoviePlay" },
    { GAME_ADDR_MOVIE_STOP,             notify_MovieStop,  "MovieStop" },
    { GAME_ADDR_MOVIE_START,            notify_MovieStart, "MovieStart" },
    { GAME_ADDR_THP_SIMPLE_DECODE,      notify_THPSimpleDecode, "THPSimpleDecode" },
    { GAME_ADDR_THP_SIMPLE_DECODE_RET,  notify_THPSimpleDecodeReturn, "THPSimpleDecode return" },
    { GAME_ADDR_THP_VIDEO_DECODE_RET,   notify_THPVideoDecodeReturn, "THPVideoDecode return" },
    { GAME_ADDR_THP_VIDEO_DECODE_RET2,  notify_THPVideoDecodeReturn, "THPVideoDecode return" },
    { GAME_ADDR_THP_SIMPLE_PRELOAD,     notify_THPSimplePreLoad, "THPSimplePreLoad" },
    { GAME_ADDR_THP_SIMPLE_SET_BUFFER,  notify_THPSimpleSetBuffer, "THPSimpleSetBuffer" },
    { GAME_ADDR_THP_SIMPLE_OPEN,        notify_THPSimpleOpen, "THPSimpleOpen" },
    { GAME_ADDR_THP_VIDEO_DECODE,       notify_THPVideoDecode, "THPVideoDecode" },
};

// ---------------------------------------------------------------------------
// Trace and Dispatch tables
// ---------------------------------------------------------------------------

static const char* const kTrace[] = {
    "ARInit", "ARGetBaseAddress", "ARGetSize", "VIInit", "VIWaitForRetrace",
    "VIConfigure", "VISetNextFrameBuffer", "OSLoadContext", "__OSReschedule",
    "OSResumeThread",
};

#define HLE_CODE_BASE 0x80003000u
#define HLE_CODE_LIMIT GAME_HLE_CODE_LIMIT
#define HLE_DISPATCH_COUNT ((HLE_CODE_LIMIT - HLE_CODE_BASE) / 4u)

static HleDispatchEntry g_hle_dispatch[HLE_DISPATCH_COUNT];

static HleDispatchEntry* hle_dispatch_entry(u32 address) {
    if (address < HLE_CODE_BASE || address >= HLE_CODE_LIMIT || (address & 3u) != 0u)
        return NULL;
    return &g_hle_dispatch[(address - HLE_CODE_BASE) / 4u];
}

static const char* trace_name(u32 address) {
    for (size_t i = 0; i < sizeof kTrace / sizeof kTrace[0]; i++)
        if (sdk_symbol_address(kTrace[i]) == address)
            return kTrace[i];
    return NULL;
}

static bool hle_dispatch(CPUState* cpu, u32 address) {
    if (dol_hle_handle_callback_return(cpu, address))
        return true;

    if (dol_hle_handle_gx_return(cpu, address))
        return true;

    // NULL pointer calls check
    if (address < HLE_CODE_BASE) {
        static unsigned long null_calls = 0;
        if (cpu->lr >= HLE_CODE_BASE) {
            if (g_hle_log || null_calls < 8)
                fprintf(stderr,
                        "[hle] null-pointer call to 0x%08X from lr=0x%08X "
                        "(treating as no-op callback, #%lu)\n",
                        address, cpu->lr, ++null_calls);
            else
                ++null_calls;
            hle_return(cpu);
            return true;
        }
        return false;
    }

    HleDispatchEntry* entry = hle_dispatch_entry(address);
    if (entry == NULL)
        return false;

    if (entry->intercept != NULL) {
        if (g_hle_log) {
            const char* name = sdk_symbol_name(address);
            if (name == NULL) {
                for (size_t i = 0; i < sizeof kAddrHandlers / sizeof kAddrHandlers[0]; i++)
                    if (kAddrHandlers[i].address == address) {
                        name = kAddrHandlers[i].name;
                        break;
                    }
            }
            fprintf(stderr, "[hle] %-28s 0x%08X -> host\n",
                    name != NULL ? name : "(unnamed)", address);
        }
        // Some intercepts tail-call into guest code instead of returning a
        // value -- dol_hle_aramUploadData jumps to the caller's completion
        // routine, which is what its "the host dispatch loop checks cpu->pc"
        // comment means. Returning unconditionally would overwrite that jump
        // with lr and the callback would silently never run, so only return
        // when the intercept left the pc where it found it.
        const u32 entry_pc = cpu->pc;
        entry->intercept(cpu);
        if (cpu->pc == entry_pc)
            hle_return(cpu);
        return true;
    }

    if (entry->notify != NULL)
        entry->notify(cpu);

    if (g_hle_log) {
        const char* tn = trace_name(address);
        if (tn != NULL)
            fprintf(stderr, "[trace] %-22s lr=0x%08X r3=0x%08X r4=0x%08X r5=0x%08X\n",
                    tn, cpu->lr, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
        const char* name = sdk_symbol_name(address);
        if (name != NULL)
            fprintf(stderr, "[hle] %-28s 0x%08X (recompiled)\n", name, address);
    }

    return false;
}

static const HleAddrEntry kPhysicsNotify[] = {
    { STRIKERS_CBALL_UPDATE_ORIENTATION, notify_cBallUpdateOrientation, "cBall::UpdateOrientation" },
    { STRIKERS_CBALL_POST_PHYSICS_UPDATE, notify_cBallPostPhysicsUpdate, "cBall::PostPhysicsUpdate" },
    { STRIKERS_PHYSICS_UPDATE,             notify_PhysicsUpdate,       "PhysicsUpdate" },
    { STRIKERS_PHYSICS_AI_BALL_POST_UPDATE, notify_PhysicsAIBallPostUpdate, "PhysicsAIBall::PostUpdate" },
    { STRIKERS_PHYSICS_WORLD_UPDATE,       notify_PhysicsWorldUpdate,  "PhysicsWorld::Update" },
    { STRIKERS_PHYSICS_WORLD_PRE_UPDATE,   notify_PhysicsWorldPreUpdate, "PhysicsWorld::PreUpdate" },
    { STRIKERS_DWORLD_QUICK_STEP,          notify_dWorldQuickStep,   "dWorldQuickStep" },
    { STRIKERS_DBODY_SET_FORCE,            notify_dBodySetForce,     "dBodySetForce" },
    { STRIKERS_DBODY_ADD_FORCE,            notify_dBodyAddForce,     "dBodyAddForce" },
    { STRIKERS_DBODY_SET_ANGULAR_VEL,      notify_dBodySetAngularVel, "dBodySetAngularVel" },
    { STRIKERS_DBODY_SET_LINEAR_VEL,       notify_dBodySetLinearVel, "dBodySetLinearVel" },
    { STRIKERS_DBODY_SET_ROTATION,          notify_dBodySetRotation,  "dBodySetRotation" },
    { STRIKERS_DBODY_SET_POSITION,          notify_dBodySetPosition,  "dBodySetPosition" },
    { STRIKERS_SOR_LCP,                    notify_SorLcp,             "SOR_LCP" },
    { STRIKERS_SOR_LCP_RETURN,             notify_SorLcpReturn,       "SOR_LCP return" },
    { STRIKERS_DX_STEP_BODY,               notify_dxStepBody,        "dxStepBody" },
};

static void initialize_hle_dispatch(void) {
    memset(g_hle_dispatch, 0, sizeof g_hle_dispatch);

    for (size_t i = 0; i < sizeof kHandlers / sizeof kHandlers[0]; i++) {
        HleDispatchEntry* entry = hle_dispatch_entry(sdk_symbol_address(kHandlers[i].name));
        if (entry != NULL)
            entry->intercept = kHandlers[i].fn;
    }
    for (size_t i = 0; i < sizeof kAddrHandlers / sizeof kAddrHandlers[0]; i++) {
        HleDispatchEntry* entry = hle_dispatch_entry(kAddrHandlers[i].address);
        if (entry != NULL)
            entry->intercept = kAddrHandlers[i].fn;
    }
    for (size_t i = 0; i < sizeof kNameNotify / sizeof kNameNotify[0]; i++) {
        HleDispatchEntry* entry =
            hle_dispatch_entry(sdk_symbol_address(kNameNotify[i].name));
        if (entry != NULL)
            entry->notify = kNameNotify[i].fn;
    }
    for (size_t i = 0; i < sizeof kNotify / sizeof kNotify[0]; i++) {
        HleDispatchEntry* entry = hle_dispatch_entry(kNotify[i].address);
        if (entry != NULL)
            entry->notify = kNotify[i].fn;
    }
    if (ball_state_log_enabled()) {
        for (size_t i = 0; i < sizeof kPhysicsNotify / sizeof kPhysicsNotify[0]; ++i) {
            HleDispatchEntry* entry = hle_dispatch_entry(kPhysicsNotify[i].address);
            if (entry != NULL)
                entry->notify = kPhysicsNotify[i].fn;
        }
    }
}

// ---------------------------------------------------------------------------
// Standard API forwarders
// ---------------------------------------------------------------------------

bool hle_card_open(const char* path) {
    return dol_hle_card_open(path);
}

void hle_card_close(void) {
    dol_hle_card_close();
}

bool hle_poll_callback(CPUState* cpu) {
    return dol_hle_poll_callback(cpu);
}

void hle_install(CPUState* cpu) {
    // 1. Configure the generic HLE config
    DolHleConfig config;
    memset(&config, 0, sizeof config);
    config.code_base = HLE_CODE_BASE;
    config.code_limit = HLE_CODE_LIMIT;
    config.dispatch_interrupt_addr = STRIKERS_DISPATCH_INTERRUPT_ADDR;
    config.musyx_dsp_done_addr = STRIKERS_MUSYX_DSP_DONE_ADDR;
    config.thp_simple_control_addr = STRIKERS_THP_SIMPLE_CONTROL;
    config.gx_dirty_state_helper_addr = STRIKERS_GX_DIRTY_STATE_HELPER_ADDR;
    config.gx_flush_prim_helper_addr = STRIKERS_GX_FLUSH_PRIM_HELPER_ADDR;
    memcpy(config.game_code, GAME_ID_CODE, 4);
    memcpy(config.company, GAME_ID_COMPANY, 2);
    config.gx_data_base_reg = GAME_GX_DATA_BASE_REG;
    config.gx_data_sda_offset = GAME_GX_DATA_SDA_OFFSET;
    config.gx_dirty_state_offset = GAME_GX_DIRTY_STATE_OFFSET;
    dol_hle_init(&config);

    // 2. Local logs and scripting settings
    g_movie_log = getenv("STRIKERS_MOVIE_LOG") != NULL;
    g_movie_cadence_log = getenv("STRIKERS_MOVIE_CADENCE_LOG") != NULL;
    g_auto_skip_card_prompt = getenv("STRIKERS_AUTO_SKIP_CARD_PROMPT") != NULL;
    g_state_log = getenv("STRIKERS_STATE_LOG") != NULL;
    g_auto_input = getenv("STRIKERS_AUTO_INPUT") != NULL;
    g_mash_to_gameplay = getenv("STRIKERS_MASH_TO_GAMEPLAY") != NULL;
    g_mash_side_assigned = false;
    g_mash_prematch = false;
    g_mash_route_complete = false;
    hle_physics_init();

    if (g_mash_to_gameplay) {
        fprintf(stderr, "[input] mashing A + Left + Start (8 frames held, 8 released)\n");
    }

    g_auto_input_last_pulse = ~(u64)0;
    g_auto_skip_card_last_pulse = ~(u64)0;
    g_auto_input_once_sent = ~(u64)0;
    g_auto_input_once_frame = 0;

    g_movie_cadence_present_count = 0;
    g_movie_cadence_texframe_changes = 0;
    g_movie_cadence_start_time = 0.0;
    g_movie_cadence_last_texframe = -1;
    g_movie_cadence_started = false;

    initialize_hle_dispatch();

    const char* once_frame = getenv("STRIKERS_AUTO_INPUT_ONCE");
    if (once_frame != NULL && once_frame[0] != '\0')
        g_auto_input_once_frame = strtoull(once_frame, NULL, 0);

    parse_input_script(getenv("STRIKERS_INPUT_SCRIPT"));

#ifdef STRIKERSRECOMP_HAS_BACKEND
    g_gx_begin_count = 0;
#endif

    cpu->host_call = hle_dispatch;
}
