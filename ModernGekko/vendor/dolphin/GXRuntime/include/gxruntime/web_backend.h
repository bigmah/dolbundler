// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef GXRUNTIME_WEB_BACKEND_H
#define GXRUNTIME_WEB_BACKEND_H

// The browser backend: a third implementation of DolPlatformOps, beside
// backends/aurora (native wgpu) and src/headless_backend.c (nothing).
//
// It owns no GPU objects. gxcore already turns GX register writes into a
// self-contained DrawPlan -- decoded vertices, POD uniform blocks, a pipeline
// key and WGSL -- so this backend's whole job is to serialise a frame's worth
// of plans into flat arenas in WASM linear memory and let the JavaScript side
// walk them once per frame. The JS/WASM boundary is crossed at FRAME
// granularity, never per draw.
//
// See backends/web/web_backend.cpp for the arena layout, and web/dolweb.js for
// the reader on the other side. The two must agree on the record strides, which
// are published through dolweb_*_stride() rather than hardcoded.

#include "gxruntime/platform.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DolWebBackendConfig {
    const char* app_name;
    // Logical framebuffer the guest renders into before configure_vi lands.
    // GameCube NTSC interlaced defaults; configure_vi overrides both.
    unsigned efb_width;
    unsigned efb_height;
    bool graphics_logging;
} DolWebBackendConfig;

bool dol_web_initialize(const DolWebBackendConfig* config);
void dol_web_shutdown(void);

// Marks the run loop as finished; the client's step function returns and the
// page stops scheduling frames.
void dol_web_request_quit(void);

// True once the guest has called present() since the last dol_web_begin_frame.
// The client's bounded run slice polls this to hand control back to the browser
// on a frame boundary rather than after a fixed block budget.
bool dol_web_frame_ready(void);

// Clears the frame arenas. Called by the client at the top of each run slice,
// after JavaScript has consumed the previous frame.
void dol_web_begin_frame(void);

#ifdef __cplusplus
}
#endif

#endif
