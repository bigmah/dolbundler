#ifndef STRIKERSRECOMP_GAME_G4QE01_H
#define STRIKERSRECOMP_GAME_G4QE01_H

// Super Mario Strikers (G4QE01, NTSC). The title this client was developed
// against; everything here came from the smstrikers-decomp symbol map.

#define GAME_ID_CODE            "G4QE"
#define GAME_ID_COMPANY         "01"
// One past the last byte of guest code. Sizes the HLE dispatch table, so it
// must cover the DOL's text and no more than necessary.
#define GAME_HLE_CODE_LIMIT     0x8028D260u

// Where the SDK keeps its __GXData pointer, and the dirty-state word inside it.
// The April 2004 SDK reaches it through r2.
#define GAME_GX_DATA_BASE_REG      2u
#define GAME_GX_DATA_SDA_OFFSET    (-20664)
#define GAME_GX_DIRTY_STATE_OFFSET 1452u

// Functions that are neither SDK symbols nor named by any database.
#define GAME_ADDR_OS_INIT_AUDIO_SYSTEM 0x80254718u
#define GAME_ADDR_AI_SRC_INIT          0x8023B610u
#define GAME_ADDR_SAL_INIT_DSP         0x80282B98u
#define GAME_ADDR_ARAM_UPLOAD_DATA     0x80281BCCu
#define GAME_ADDR_CARD_FORMAT_ASYNC    0x802429FCu

// The game's own functions the host policy watches: MusyX entry points, the
// front-end state machine, the THP video player. These were inline constants in
// hle.c, which meant a build for any other title quietly hooked whatever
// happened to live at Strikers' addresses.
#define GAME_ADDR_SND_FX_START_PARA_INFO 0x80268A08u
#define GAME_ADDR_SND_STREAM_ACTIVATE    0x8026C518u
#define GAME_ADDR_SND_PUSH_GROUP         0x80277274u
#define GAME_ADDR_SND_SEQ_PLAY_EX        0x80277BA4u
#define GAME_ADDR_TASK_SET_NEXT_STATE    0x801D2908u
#define GAME_ADDR_TASK_STARTUP           0x801D2B28u
#define GAME_ADDR_MAIN_MENU_UPDATE       0x800A9A5Cu
#define GAME_ADDR_CHOOSE_SIDE_CHECK      0x800C3C64u
#define GAME_ADDR_CHOOSE_SIDE_POSITION   0x800C36E8u
#define GAME_ADDR_MOVIE_PLAY             0x801CB7F8u
#define GAME_ADDR_MOVIE_STOP             0x801CB8E0u
#define GAME_ADDR_MOVIE_START            0x801CBA48u
#define GAME_ADDR_THP_SIMPLE_DECODE      0x801CC5E8u
#define GAME_ADDR_THP_SIMPLE_DECODE_RET  0x801CB868u
#define GAME_ADDR_THP_VIDEO_DECODE_RET   0x801CC6C4u
#define GAME_ADDR_THP_VIDEO_DECODE_RET2  0x801CC7F0u
#define GAME_ADDR_THP_SIMPLE_PRELOAD     0x801CCB00u
#define GAME_ADDR_THP_SIMPLE_SET_BUFFER  0x801CCDD0u
#define GAME_ADDR_THP_SIMPLE_OPEN        0x801CD2C8u
#define GAME_ADDR_THP_VIDEO_DECODE       0x802857ACu

// Game-specific and SDK-specific guest memory offsets for Strikers HLE registry
#define STRIKERS_THP_SIMPLE_CONTROL         0x8032F000u
#define STRIKERS_GX_PROJ_MATRIX             0x8032C090u
#define STRIKERS_GX_MODELVIEW_MATRIX        0x8032C0D0u
#define STRIKERS_GX_MVIEW_MATRIX            0x8032C060u
#define STRIKERS_OS_CONTEXT_POINTER         0x800000D4u
#define STRIKERS_BALL_DRAW_FUN_START        0x8011DF10u
#define STRIKERS_BALL_DRAW_FUN_END          0x8011E38Cu
#define STRIKERS_RENDER_WORLD_GLOBAL        0x8037273Cu
#define STRIKERS_GBALL_GLOBAL               0x80373664u
#define STRIKERS_BALL_VTABLE                0x802AFA18u
#define STRIKERS_DISPATCH_INTERRUPT_ADDR    0x80256FD0u
#define STRIKERS_MUSYX_DSP_DONE_ADDR        0x80374A98u
#define STRIKERS_GX_DIRTY_STATE_HELPER_ADDR 0x8024DCA0u
#define STRIKERS_GX_FLUSH_PRIM_HELPER_ADDR  0x8024DDF0u

#define STRIKERS_CBALL_UPDATE_ORIENTATION   0x80009E00u
#define STRIKERS_CBALL_POST_PHYSICS_UPDATE  0x8000B828u
#define STRIKERS_PHYSICS_UPDATE             0x80132B10u
#define STRIKERS_PHYSICS_AI_BALL_POST_UPDATE 0x801343C8u
#define STRIKERS_PHYSICS_WORLD_UPDATE       0x8020199Cu
#define STRIKERS_PHYSICS_WORLD_PRE_UPDATE   0x80201A8Cu
#define STRIKERS_DWORLD_QUICK_STEP          0x80220894u
#define STRIKERS_DBODY_SET_FORCE            0x802214E8u
#define STRIKERS_DBODY_ADD_FORCE            0x80221530u
#define STRIKERS_DBODY_SET_ANGULAR_VEL      0x8022160Cu
#define STRIKERS_DBODY_SET_LINEAR_VEL       0x8022161Cu
#define STRIKERS_DBODY_SET_ROTATION         0x8022162Cu
#define STRIKERS_DBODY_SET_POSITION         0x802216B4u
#define STRIKERS_SOR_LCP                    0x80222DC0u
#define STRIKERS_SOR_LCP_RETURN             0x80222994u
#define STRIKERS_DX_STEP_BODY               0x80223F30u

// Game diagnostic guest memory offsets for main.c
#define STRIKERS_TASK_MANAGER               0x803742B8u
#define STRIKERS_TRANSITION                 0x80373DA0u
#define STRIKERS_LOADING_GLOBAL             0x80373DE4u
#define STRIKERS_GAME_SCENE_MANAGER         0x80373840u
#define STRIKERS_FE_RESOURCE_MANAGER        0x80374448u
#define STRIKERS_FE_SCENE_MANAGER           0x80374450u
#define STRIKERS_FE_INPUT                   0x80374458u
#define STRIKERS_VIEW_BASE                  0x80336FC0u
#define STRIKERS_PENDING_RESOURCE           0x80343610u
#define STRIKERS_CURRENT_RESOURCE           0x80374434u
#define STRIKERS_RESOURCE_CONTEXT           0x80374438u
#define STRIKERS_PAD_CURRENT                0x80372FF0u
#define STRIKERS_PAD_NEXT                   0x80372FF4u
#define STRIKERS_PAD_INTERNAL               0x80372FF8u
#define STRIKERS_PUSHPOP_HEAD               0x80343660u
#define STRIKERS_BUS_CLOCK                  0x800000F8u
#define STRIKERS_UPTIME                     0x80373D78u
#define STRIKERS_RESET_MODE                 0x80373DB0u
#define STRIKERS_RESET_STATE                0x80373DB4u
#define STRIKERS_AUDIO_INIT                 0x80373DB8u
#define STRIKERS_RESET_PRESSED              0x80373DB9u
#define STRIKERS_GAME_PAUSED                0x80373DBAu
#define STRIKERS_CHECK_CARD                 0x80373DBBu
#define STRIKERS_RESET_HOLD_BASE            0x802C16B0u
#define STRIKERS_VIEW_ENABLED_BASE          0x80302050u

#endif /* STRIKERSRECOMP_GAME_G4QE01_H */
