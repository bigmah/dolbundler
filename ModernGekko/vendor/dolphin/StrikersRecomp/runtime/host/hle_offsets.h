#ifndef STRIKERSRECOMP_HLE_OFFSETS_H
#define STRIKERSRECOMP_HLE_OFFSETS_H

// Which game this build is for.
//
// Everything the host policy knows that is not an SDK function lives in one
// header per title: guest globals, the addresses of the game's own functions we
// hook, and the handful of SDK internals (the __GXData pointer, the dirty-state
// offset) that no symbol table names. SDK *functions* do not appear here -- they
// come from generated/sdk_symbols.inc, by name.
//
// Set with -DSTRIKERSRECOMP_GAME=GEXE52 at configure time; the CMake option
// defines STRIKERSRECOMP_GAME_HEADER for us.
#ifndef STRIKERSRECOMP_GAME_HEADER
#define STRIKERSRECOMP_GAME_HEADER "host/game_G4QE01.h"
#endif
#include STRIKERSRECOMP_GAME_HEADER

// A title that does not have one of these gets zero, and every use is guarded:
// a hook at address zero is never installed (it is outside the dispatch range)
// and a guest write to address zero would corrupt the OS globals.
#ifndef GAME_ADDR_OS_INIT_AUDIO_SYSTEM
#define GAME_ADDR_OS_INIT_AUDIO_SYSTEM 0u
#endif
#ifndef GAME_ADDR_AI_SRC_INIT
#define GAME_ADDR_AI_SRC_INIT 0u
#endif
#ifndef GAME_ADDR_SAL_INIT_DSP
#define GAME_ADDR_SAL_INIT_DSP 0u
#endif
#ifndef GAME_ADDR_ARAM_UPLOAD_DATA
#define GAME_ADDR_ARAM_UPLOAD_DATA 0u
#endif
#ifndef GAME_ADDR_CARD_FORMAT_ASYNC
#define GAME_ADDR_CARD_FORMAT_ASYNC 0u
#endif
#ifndef GAME_ADDR_SND_FX_START_PARA_INFO
#define GAME_ADDR_SND_FX_START_PARA_INFO 0u
#endif
#ifndef GAME_ADDR_SND_STREAM_ACTIVATE
#define GAME_ADDR_SND_STREAM_ACTIVATE 0u
#endif
#ifndef GAME_ADDR_SND_PUSH_GROUP
#define GAME_ADDR_SND_PUSH_GROUP 0u
#endif
#ifndef GAME_ADDR_SND_SEQ_PLAY_EX
#define GAME_ADDR_SND_SEQ_PLAY_EX 0u
#endif
#ifndef GAME_ADDR_TASK_SET_NEXT_STATE
#define GAME_ADDR_TASK_SET_NEXT_STATE 0u
#endif
#ifndef GAME_ADDR_TASK_STARTUP
#define GAME_ADDR_TASK_STARTUP 0u
#endif
#ifndef GAME_ADDR_MAIN_MENU_UPDATE
#define GAME_ADDR_MAIN_MENU_UPDATE 0u
#endif
#ifndef GAME_ADDR_CHOOSE_SIDE_CHECK
#define GAME_ADDR_CHOOSE_SIDE_CHECK 0u
#endif
#ifndef GAME_ADDR_CHOOSE_SIDE_POSITION
#define GAME_ADDR_CHOOSE_SIDE_POSITION 0u
#endif
#ifndef GAME_ADDR_MOVIE_PLAY
#define GAME_ADDR_MOVIE_PLAY 0u
#endif
#ifndef GAME_ADDR_MOVIE_STOP
#define GAME_ADDR_MOVIE_STOP 0u
#endif
#ifndef GAME_ADDR_MOVIE_START
#define GAME_ADDR_MOVIE_START 0u
#endif
#ifndef GAME_ADDR_THP_SIMPLE_DECODE
#define GAME_ADDR_THP_SIMPLE_DECODE 0u
#endif
#ifndef GAME_ADDR_THP_SIMPLE_DECODE_RET
#define GAME_ADDR_THP_SIMPLE_DECODE_RET 0u
#endif
#ifndef GAME_ADDR_THP_VIDEO_DECODE_RET
#define GAME_ADDR_THP_VIDEO_DECODE_RET 0u
#endif
#ifndef GAME_ADDR_THP_VIDEO_DECODE_RET2
#define GAME_ADDR_THP_VIDEO_DECODE_RET2 0u
#endif
#ifndef GAME_ADDR_THP_SIMPLE_PRELOAD
#define GAME_ADDR_THP_SIMPLE_PRELOAD 0u
#endif
#ifndef GAME_ADDR_THP_SIMPLE_SET_BUFFER
#define GAME_ADDR_THP_SIMPLE_SET_BUFFER 0u
#endif
#ifndef GAME_ADDR_THP_SIMPLE_OPEN
#define GAME_ADDR_THP_SIMPLE_OPEN 0u
#endif
#ifndef GAME_ADDR_THP_VIDEO_DECODE
#define GAME_ADDR_THP_VIDEO_DECODE 0u
#endif

#endif /* STRIKERSRECOMP_HLE_OFFSETS_H */
