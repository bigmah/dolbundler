#ifndef STRIKERSRECOMP_GAME_GEXE52_H
#define STRIKERSRECOMP_GAME_GEXE52_H

// Disney's Extreme Skate Adventure (GEXE52, NTSC).
//
// There is no decomp for this title, so the SDK function table in
// generated-GEXE52/sdk_symbols.inc comes from tools/sdk_signatures.py. What is
// here is the rest: the SDK internals the HLE reaches through no symbol, all
// confirmed by reading the disassembly against Strikers'.
//
// The game's *own* hooks -- the audio middleware, the video player, the
// front-end state machine -- are all zero. Those are per-title reverse
// engineering with no shortcut, and every use of them is guarded, so the value
// of leaving them at zero is that the feature is simply absent rather than
// firing on an unrelated address.

#define GAME_ID_CODE            "GEXE"
#define GAME_ID_COMPANY         "52"
// Text runs 0x80003100..0x801D29A0.
#define GAME_HLE_CODE_LIMIT     0x801D2A00u

// The September 2002 SDK reaches __GXData through r13, not r2, and its
// dirty-state word sits 184 bytes earlier in the structure.
#define GAME_GX_DATA_BASE_REG      13u
#define GAME_GX_DATA_SDA_OFFSET    (-24448)
#define GAME_GX_DIRTY_STATE_OFFSET 1268u

// __GXSetDirtyState and __GXSendFlushPrim: the functions immediately before
// and after GXBegin, in both games.
#define STRIKERS_GX_DIRTY_STATE_HELPER_ADDR 0x8019D988u
#define STRIKERS_GX_FLUSH_PRIM_HELPER_ADDR  0x8019DB18u

// Two SDK internals with no exported name, found by asking
// tools/sdk_signatures.py --where for the addresses Strikers' equivalents
// transferred to. Both are 1.00 matches at identical sizes.
//
// __OSInitAudioSystem resets the DSP and then waits for its boot ROM to answer
// on the mailbox. There is no DSP here to answer, so it must not run: this is
// where the first Skate boot stopped, spinning at 0x8016CC54 for 6.2 billion
// guest blocks.
#define GAME_ADDR_OS_INIT_AUDIO_SYSTEM 0x8016CB00u
#define GAME_ADDR_AI_SRC_INIT          0x80189B3Cu
#define GAME_ADDR_CARD_FORMAT_ASYNC    0x80195960u

// __OSDispatchInterrupt, where the host hands the guest an interrupt.
#define STRIKERS_DISPATCH_INTERRUPT_ADDR    0x8016EE98u

// __OSCurrentContext: an OS global, fixed by the SDK at the same address in
// every GameCube game.
#define STRIKERS_OS_CONTEXT_POINTER         0x800000D4u

// salInitDsp and aramUploadData are MusyX, and MusyX does not transfer -- this
// game does not appear to use it. Left at zero, which means the host's DSP
// mixer stand-in is never installed and audio will be silent until this
// title's own middleware is identified.
//
// Everything below is likewise not identified for this title. Each one
// disables a feature rather than misfiring: no MusyX completion flag, no THP
// video player, no ball-physics stand-ins, no front-end scripting.
#define STRIKERS_MUSYX_DSP_DONE_ADDR        0u
#define STRIKERS_THP_SIMPLE_CONTROL         0u
#define STRIKERS_GX_PROJ_MATRIX             0u
#define STRIKERS_GX_MODELVIEW_MATRIX        0u
#define STRIKERS_GX_MVIEW_MATRIX            0u
#define STRIKERS_BALL_DRAW_FUN_START        0u
#define STRIKERS_BALL_DRAW_FUN_END          0u
#define STRIKERS_RENDER_WORLD_GLOBAL        0u
#define STRIKERS_GBALL_GLOBAL               0u
#define STRIKERS_BALL_VTABLE                0u

#define STRIKERS_CBALL_UPDATE_ORIENTATION   0u
#define STRIKERS_CBALL_POST_PHYSICS_UPDATE  0u
#define STRIKERS_PHYSICS_UPDATE             0u
#define STRIKERS_PHYSICS_AI_BALL_POST_UPDATE 0u
#define STRIKERS_PHYSICS_WORLD_UPDATE       0u
#define STRIKERS_PHYSICS_WORLD_PRE_UPDATE   0u
#define STRIKERS_DWORLD_QUICK_STEP          0u
#define STRIKERS_DBODY_SET_FORCE            0u
#define STRIKERS_DBODY_ADD_FORCE            0u
#define STRIKERS_DBODY_SET_ANGULAR_VEL      0u
#define STRIKERS_DBODY_SET_LINEAR_VEL       0u
#define STRIKERS_DBODY_SET_ROTATION         0u
#define STRIKERS_DBODY_SET_POSITION         0u
#define STRIKERS_SOR_LCP                    0u
#define STRIKERS_SOR_LCP_RETURN             0u
#define STRIKERS_DX_STEP_BODY               0u

#define STRIKERS_TASK_MANAGER               0u
#define STRIKERS_TRANSITION                 0u
#define STRIKERS_LOADING_GLOBAL             0u
#define STRIKERS_GAME_SCENE_MANAGER         0u
#define STRIKERS_FE_RESOURCE_MANAGER        0u
#define STRIKERS_FE_SCENE_MANAGER           0u
#define STRIKERS_FE_INPUT                   0u
#define STRIKERS_VIEW_BASE                  0u
#define STRIKERS_PENDING_RESOURCE           0u
#define STRIKERS_CURRENT_RESOURCE           0u
#define STRIKERS_RESOURCE_CONTEXT           0u
#define STRIKERS_PAD_CURRENT                0u
#define STRIKERS_PAD_NEXT                   0u
#define STRIKERS_PAD_INTERNAL               0u
#define STRIKERS_PUSHPOP_HEAD               0u
#define STRIKERS_BUS_CLOCK                  0x800000F8u
#define STRIKERS_UPTIME                     0u
#define STRIKERS_RESET_MODE                 0u
#define STRIKERS_RESET_STATE                0u
#define STRIKERS_AUDIO_INIT                 0u
#define STRIKERS_RESET_PRESSED              0u
#define STRIKERS_GAME_PAUSED                0u
#define STRIKERS_CHECK_CARD                 0u
#define STRIKERS_RESET_HOLD_BASE            0u
#define STRIKERS_VIEW_ENABLED_BASE          0u

#endif /* STRIKERSRECOMP_GAME_GEXE52_H */
