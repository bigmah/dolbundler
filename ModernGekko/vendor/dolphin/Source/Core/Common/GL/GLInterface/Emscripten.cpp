// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/GL/GLInterface/Emscripten.h"

#include <emscripten/emscripten.h>
#include <emscripten/html5.h>

#include <GLES3/gl3.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "Common/Logging/Log.h"

GLContextEmscripten::~GLContextEmscripten()
{
  if (m_context && !m_proxied)
    emscripten_webgl_destroy_context(m_context);
}

bool GLContextEmscripten::IsHeadless() const
{
  return m_context == 0;
}

bool GLContextEmscripten::Initialize(const WindowSystemInfo& wsi, bool stereo, bool core)
{
  // The host names its canvas with a CSS selector, which is what emscripten's
  // API takes; there is no window handle to pass.
  m_target = wsi.render_surface ? static_cast<const char*>(wsi.render_surface) : "#canvas";

  EmscriptenWebGLContextAttributes attributes;
  emscripten_webgl_init_context_attributes(&attributes);
  attributes.majorVersion = 2;  // WebGL 2 == OpenGL ES 3.0
  attributes.minorVersion = 0;
  attributes.alpha = false;
  // Dolphin renders the game into its own framebuffers and only blits the
  // finished image to the default one, so the default framebuffer needs no
  // depth or stencil and no multisampling. Asking for them costs memory on a
  // phone and makes OGLConfig complain about driver-forced MSAA.
  attributes.depth = false;
  attributes.stencil = false;
  attributes.antialias = false;
  attributes.premultipliedAlpha = false;
  attributes.preserveDrawingBuffer = false;
  attributes.failIfMajorPerformanceCaveat = false;
  attributes.enableExtensionsByDefault = true;
  attributes.powerPreference = EM_WEBGL_POWER_PREFERENCE_HIGH_PERFORMANCE;
  // WebGL presents implicitly when an event callback returns, which is
  // meaningless for an emulator producing frames on its own thread; explicit
  // swap control is what makes Swap() the presentation point instead.
  attributes.explicitSwapControl = true;

  // Two ways to get a context here, and the difference is the whole frame time.
  //
  // If this thread owns the canvas -- Core::Init transfers it to the emulation
  // thread at pthread_create, which is the only moment a browser allows -- the
  // context is local and every GL call runs here. If it does not, emscripten
  // can still give us one by creating it on the browser's main thread and
  // forwarding each call, which works and is very slow: one queued cross-thread
  // call per GL call, measured at 99 ms per frame inside the present alone.
  //
  // So: ask for the local one, and only fall back.
  attributes.renderViaOffscreenBackBuffer = false;
  attributes.proxyContextToMainThread = EMSCRIPTEN_WEBGL_CONTEXT_PROXY_DISALLOW;
  m_context = emscripten_webgl_create_context(m_target.c_str(), &attributes);
  if (!m_context)
  {
    WARN_LOG_FMT(VIDEO,
                 "This thread does not hold {}; falling back to a proxied GL "
                 "context, which is correct but roughly an order of magnitude slower.",
                 m_target);
    attributes.renderViaOffscreenBackBuffer = true;
    attributes.proxyContextToMainThread = EMSCRIPTEN_WEBGL_CONTEXT_PROXY_ALWAYS;
    m_context = emscripten_webgl_create_context(m_target.c_str(), &attributes);
    m_proxied = m_context != 0;
  }
  if (!m_context)
  {
    ERROR_LOG_FMT(VIDEO, "emscripten_webgl_create_context failed for {}", m_target);
    return false;
  }

  if (emscripten_webgl_make_context_current(m_context) != EMSCRIPTEN_RESULT_SUCCESS)
  {
    ERROR_LOG_FMT(VIDEO, "emscripten_webgl_make_context_current failed");
    emscripten_webgl_destroy_context(m_context);
    m_context = 0;
    return false;
  }

  // A browser only has ES; there is no desktop-GL path to fall back from.
  m_opengl_mode = Mode::OpenGLES;
  Update();
  return true;
}

std::unique_ptr<GLContext> GLContextEmscripten::CreateSharedContext()
{
  // WebGL has no shared contexts and no way to fake one: resources live in a
  // single context object. Dolphin treats a null return as "no background
  // shader compilation", which is the correct behaviour here rather than a
  // failure.
  return nullptr;
}

bool GLContextEmscripten::MakeCurrent()
{
  return emscripten_webgl_make_context_current(m_context) == EMSCRIPTEN_RESULT_SUCCESS;
}

bool GLContextEmscripten::ClearCurrent()
{
  return emscripten_webgl_make_context_current(0) == EMSCRIPTEN_RESULT_SUCCESS;
}

void GLContextEmscripten::Update()
{
  int width = 0;
  int height = 0;
  if (emscripten_webgl_get_drawing_buffer_size(m_context, &width, &height) ==
      EMSCRIPTEN_RESULT_SUCCESS)
  {
    m_backbuffer_width = static_cast<u32>(width);
    m_backbuffer_height = static_cast<u32>(height);
  }
}

void GLContextEmscripten::SwapInterval(int interval)
{
  // The browser's compositor owns presentation timing; there is nothing to set.
  (void)interval;
}

// --- where a browser frame actually goes -------------------------------------
//
// Dolphin resolves every GL entry point through this class, which makes this the
// one place a timer can go without touching the backend. WebGL lives in another
// process, so the calls worth timing are the ones that force a round trip to it
// -- a readback, a fence wait, a link-status query -- and none of those looks
// expensive in any source file. The draw calls are here as the control: if they
// are cheap and the frame is not, the frame is not being spent drawing.
//
// DOLWEB_TIME_GL=1 turns it on; the report is one line per hundred presents.
namespace
{
struct GLProfileEntry
{
  const char* name;
  std::atomic<uint64_t> calls;
  std::atomic<uint64_t> nanos;
};

enum GLProfileIndex
{
  GLP_ReadPixels,
  GLP_ClientWaitSync,
  GLP_Finish,
  GLP_GetError,
  GLP_LinkProgram,
  GLP_GetProgramiv,
  GLP_CompileShader,
  GLP_TexSubImage2D,
  GLP_TexImage2D,
  GLP_BufferData,
  GLP_BufferSubData,
  GLP_MapBufferRange,
  GLP_UnmapBuffer,
  GLP_BlitFramebuffer,
  GLP_DrawArrays,
  GLP_DrawElements,
  GLP_COUNT,
};

GLProfileEntry g_gl_profile[GLP_COUNT] = {
    {"glReadPixels"},      {"glClientWaitSync"}, {"glFinish"},
    {"glGetError"},        {"glLinkProgram"},    {"glGetProgramiv"},
    {"glCompileShader"},   {"glTexSubImage2D"},  {"glTexImage2D"},
    {"glBufferData"},      {"glBufferSubData"},  {"glMapBufferRange"},
    {"glUnmapBuffer"},     {"glBlitFramebuffer"},{"glDrawArrays"},
    {"glDrawElements"},
};

struct GLProfileScope
{
  GLProfileIndex index;
  double start;
  explicit GLProfileScope(GLProfileIndex i) : index(i), start(emscripten_get_now()) {}
  ~GLProfileScope()
  {
    g_gl_profile[index].calls.fetch_add(1, std::memory_order_relaxed);
    g_gl_profile[index].nanos.fetch_add(
        static_cast<uint64_t>((emscripten_get_now() - start) * 1e6),
        std::memory_order_relaxed);
  }
};

#define DOLWEB_GL_SHIM(slot) GLProfileScope scope_(slot)

void ProfiledReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type,
                        void* pixels)
{
  DOLWEB_GL_SHIM(GLP_ReadPixels);
  glReadPixels(x, y, w, h, fmt, type, pixels);
}
GLenum ProfiledClientWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout)
{
  DOLWEB_GL_SHIM(GLP_ClientWaitSync);
  return glClientWaitSync(sync, flags, timeout);
}
void ProfiledFinish()
{
  DOLWEB_GL_SHIM(GLP_Finish);
  glFinish();
}
GLenum ProfiledGetError()
{
  DOLWEB_GL_SHIM(GLP_GetError);
  return glGetError();
}
void ProfiledLinkProgram(GLuint program)
{
  DOLWEB_GL_SHIM(GLP_LinkProgram);
  glLinkProgram(program);
}
void ProfiledGetProgramiv(GLuint program, GLenum pname, GLint* params)
{
  DOLWEB_GL_SHIM(GLP_GetProgramiv);
  glGetProgramiv(program, pname, params);
}
void ProfiledCompileShader(GLuint shader)
{
  DOLWEB_GL_SHIM(GLP_CompileShader);
  glCompileShader(shader);
}
void ProfiledTexSubImage2D(GLenum target, GLint level, GLint xoff, GLint yoff, GLsizei w,
                           GLsizei h, GLenum fmt, GLenum type, const void* pixels)
{
  DOLWEB_GL_SHIM(GLP_TexSubImage2D);
  glTexSubImage2D(target, level, xoff, yoff, w, h, fmt, type, pixels);
}
void ProfiledTexImage2D(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h,
                        GLint border, GLenum fmt, GLenum type, const void* pixels)
{
  DOLWEB_GL_SHIM(GLP_TexImage2D);
  glTexImage2D(target, level, internal, w, h, border, fmt, type, pixels);
}
void ProfiledBufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage)
{
  DOLWEB_GL_SHIM(GLP_BufferData);
  glBufferData(target, size, data, usage);
}
void ProfiledBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data)
{
  DOLWEB_GL_SHIM(GLP_BufferSubData);
  glBufferSubData(target, offset, size, data);
}
void* ProfiledMapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length,
                             GLbitfield access)
{
  DOLWEB_GL_SHIM(GLP_MapBufferRange);
  return glMapBufferRange(target, offset, length, access);
}
GLboolean ProfiledUnmapBuffer(GLenum target)
{
  DOLWEB_GL_SHIM(GLP_UnmapBuffer);
  return glUnmapBuffer(target);
}
void ProfiledBlitFramebuffer(GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0,
                             GLint dx1, GLint dy1, GLbitfield mask, GLenum filter)
{
  DOLWEB_GL_SHIM(GLP_BlitFramebuffer);
  glBlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filter);
}
void ProfiledDrawArrays(GLenum mode, GLint first, GLsizei count)
{
  DOLWEB_GL_SHIM(GLP_DrawArrays);
  glDrawArrays(mode, first, count);
}
void ProfiledDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices)
{
  DOLWEB_GL_SHIM(GLP_DrawElements);
  glDrawElements(mode, count, type, indices);
}

const struct
{
  const char* name;
  void* shim;
} kProfiledEntries[] = {
    {"glReadPixels", (void*)&ProfiledReadPixels},
    {"glClientWaitSync", (void*)&ProfiledClientWaitSync},
    {"glFinish", (void*)&ProfiledFinish},
    {"glGetError", (void*)&ProfiledGetError},
    {"glLinkProgram", (void*)&ProfiledLinkProgram},
    {"glGetProgramiv", (void*)&ProfiledGetProgramiv},
    {"glCompileShader", (void*)&ProfiledCompileShader},
    {"glTexSubImage2D", (void*)&ProfiledTexSubImage2D},
    {"glTexImage2D", (void*)&ProfiledTexImage2D},
    {"glBufferData", (void*)&ProfiledBufferData},
    {"glBufferSubData", (void*)&ProfiledBufferSubData},
    {"glMapBufferRange", (void*)&ProfiledMapBufferRange},
    {"glUnmapBuffer", (void*)&ProfiledUnmapBuffer},
    {"glBlitFramebuffer", (void*)&ProfiledBlitFramebuffer},
    {"glDrawArrays", (void*)&ProfiledDrawArrays},
    {"glDrawElements", (void*)&ProfiledDrawElements},
};

bool GLProfilingEnabled()
{
  static const bool enabled = std::getenv("DOLWEB_TIME_GL") != nullptr;
  return enabled;
}

void ReportGLProfile(double frame_ms)
{
  std::printf("[gl] %.1f ms/frame over the last 100:", frame_ms);
  for (GLProfileEntry& entry : g_gl_profile)
  {
    const uint64_t calls = entry.calls.exchange(0, std::memory_order_relaxed);
    const uint64_t nanos = entry.nanos.exchange(0, std::memory_order_relaxed);
    if (nanos / 1000000.0 < 1.0)
      continue;
    std::printf("  %s %.1fms/%llu", entry.name, nanos / 1e6 / 100.0,
                (unsigned long long)(calls / 100));
  }
  std::printf("\n");
  std::fflush(stdout);
}
}  // namespace

void* GLContextEmscripten::GetFuncAddress(const std::string& name)
{
  if (GLProfilingEnabled())
  {
    for (const auto& entry : kProfiledEntries)
    {
      if (name == entry.name)
        return entry.shim;
    }
  }
  return emscripten_webgl_get_proc_address(name.c_str());
}

void GLContextEmscripten::Swap()
{
  // Presenting is the one GL call in a frame that can block on the browser's
  // compositor, and it is proxied on top of that, so it is worth being able to
  // ask what it costs before blaming the renderer for a frame time. Off unless
  // asked for; the report is one line per hundred frames.
  static const bool time_swaps = std::getenv("DOLWEB_TIME_SWAP") != nullptr;
  if (!time_swaps)
  {
    emscripten_webgl_commit_frame();
    return;
  }
  static u64 frames = 0;
  static double total_ms = 0.0;
  static double last_report = emscripten_get_now();
  const double start = emscripten_get_now();
  emscripten_webgl_commit_frame();
  total_ms += emscripten_get_now() - start;
  if (++frames % 100 == 0)
  {
    const double now = emscripten_get_now();
    std::printf("[swap] %.2f ms average over %llu frames\n", total_ms / 100.0,
                (unsigned long long)frames);
    std::fflush(stdout);
    if (GLProfilingEnabled())
      ReportGLProfile((now - last_report) / 100.0);
    last_report = now;
    total_ms = 0.0;
  }
}
