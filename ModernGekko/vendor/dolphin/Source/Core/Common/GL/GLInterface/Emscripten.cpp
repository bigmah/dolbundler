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
  // The state calls. Individually trivial on a desktop; in WebGL every one
  // crosses into JS and is validated, and there are far more of them per frame
  // than there are draws. Counting them is how you tell "the renderer is
  // expensive" from "the renderer is chatty".
  GLP_UseProgram,
  GLP_BindTexture,
  GLP_BindFramebuffer,
  GLP_BindBufferRange,
  GLP_BindSampler,
  GLP_Uniform4fv,
  // Dolphin's textures are 2D *arrays*, so every upload goes through the 3D
  // entry points and none of the 2D ones above are ever called. Wrapping only
  // those made texture uploads invisible in every reading taken before this.
  GLP_TexSubImage3D,
  GLP_CompressedTexSubImage3D,
  GLP_TexImage3D,
  GLP_TexStorage3D,
  GLP_CopyTexSubImage3D,
  GLP_GenerateMipmap,
  // Counted, not just timed. The 28 entries above left 80% of a device
  // frame invisible; a frame with 535 draws issues thousands of these.
  GLP_Enable,
  GLP_Disable,
  GLP_Scissor,
  GLP_Viewport,
  GLP_ActiveTexture,
  GLP_TexParameteri,
  GLP_VertexAttribPointer,
  GLP_VertexAttribIPointer,
  GLP_EnableVertexAttribArray,
  GLP_DisableVertexAttribArray,
  GLP_BindVertexArray,
  GLP_BindBuffer,
  GLP_BlendFunc,
  GLP_BlendEquation,
  GLP_ColorMask,
  GLP_DepthMask,
  GLP_DepthFunc,
  GLP_CullFace,
  GLP_Clear,
  GLP_ClearColor,
  GLP_PixelStorei,
  GLP_Uniform1i,
  GLP_DrawRangeElements,
  GLP_FlushMappedBufferRange,
  GLP_COUNT,
};

GLProfileEntry g_gl_profile[GLP_COUNT] = {
    {"glReadPixels"},      {"glClientWaitSync"}, {"glFinish"},
    {"glGetError"},        {"glLinkProgram"},    {"glGetProgramiv"},
    {"glCompileShader"},   {"glTexSubImage2D"},  {"glTexImage2D"},
    {"glBufferData"},      {"glBufferSubData"},  {"glMapBufferRange"},
    {"glUnmapBuffer"},     {"glBlitFramebuffer"},{"glDrawArrays"},
    {"glDrawElements"},   {"glUseProgram"},     {"glBindTexture"},
    {"glBindFramebuffer"},{"glBindBufferRange"},{"glBindSampler"},
    {"glUniform4fv"},     {"glTexSubImage3D"},  {"glCompressedTexSubImage3D"},
    {"glTexImage3D"},     {"glTexStorage3D"},   {"glCopyTexSubImage3D"},
    {"glGenerateMipmap"},
    {"glEnable"}, {"glDisable"}, {"glScissor"}, {"glViewport"}, {"glActiveTexture"}, {"glTexParameteri"}, {"glVertexAttribPointer"}, {"glVertexAttribIPointer"}, {"glEnableVertexAttribArray"}, {"glDisableVertexAttribArray"}, {"glBindVertexArray"}, {"glBindBuffer"}, {"glBlendFuncSeparate"}, {"glBlendEquationSeparate"}, {"glColorMask"}, {"glDepthMask"}, {"glDepthFunc"}, {"glCullFace"}, {"glClear"}, {"glClearColor"}, {"glPixelStorei"}, {"glUniform1i"}, {"glDrawRangeElements"}, {"glFlushMappedBufferRange"},
};

// Timing has to be free when it is not asked for. DOLWEB_GL_DEDUP installs the
// same shims as DOLWEB_TIME_GL -- that is how the dedup gets to intercept a call
// at all -- so an unconditional timer here charged two emscripten_get_now() to
// every one of a frame's 3795 GL calls even when nothing was being timed. It
// cost more than the dedup saved and turned a win into a 62.2% -> 56.2%
// regression, which is what the measurement caught.
bool g_gl_timing = false;

struct GLProfileScope
{
  GLProfileIndex index;
  double start;
  explicit GLProfileScope(GLProfileIndex i)
      : index(i), start(g_gl_timing ? emscripten_get_now() : 0.0)
  {
  }
  ~GLProfileScope()
  {
    if (!g_gl_timing)
      return;
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

void ProfiledUseProgram(GLuint program)
{
  DOLWEB_GL_SHIM(GLP_UseProgram);
  glUseProgram(program);
}
void ProfiledBindTexture(GLenum target, GLuint texture)
{
  DOLWEB_GL_SHIM(GLP_BindTexture);
  glBindTexture(target, texture);
}
void ProfiledBindFramebuffer(GLenum target, GLuint framebuffer)
{
  DOLWEB_GL_SHIM(GLP_BindFramebuffer);
  glBindFramebuffer(target, framebuffer);
}
void ProfiledBindBufferRange(GLenum target, GLuint index, GLuint buffer, GLintptr offset,
                             GLsizeiptr size)
{
  DOLWEB_GL_SHIM(GLP_BindBufferRange);
  glBindBufferRange(target, index, buffer, offset, size);
}
void ProfiledBindSampler(GLuint unit, GLuint sampler)
{
  DOLWEB_GL_SHIM(GLP_BindSampler);
  glBindSampler(unit, sampler);
}
void ProfiledUniform4fv(GLint location, GLsizei count, const GLfloat* value)
{
  DOLWEB_GL_SHIM(GLP_Uniform4fv);
  glUniform4fv(location, count, value);
}

void ProfiledTexSubImage3D(GLenum target, GLint level, GLint xo, GLint yo, GLint zo,
                           GLsizei w, GLsizei h, GLsizei d, GLenum format, GLenum type,
                           const void* pixels)
{
  DOLWEB_GL_SHIM(GLP_TexSubImage3D);
  glTexSubImage3D(target, level, xo, yo, zo, w, h, d, format, type, pixels);
}
void ProfiledCompressedTexSubImage3D(GLenum target, GLint level, GLint xo, GLint yo, GLint zo,
                                     GLsizei w, GLsizei h, GLsizei d, GLenum format,
                                     GLsizei size, const void* data)
{
  DOLWEB_GL_SHIM(GLP_CompressedTexSubImage3D);
  glCompressedTexSubImage3D(target, level, xo, yo, zo, w, h, d, format, size, data);
}
void ProfiledTexImage3D(GLenum target, GLint level, GLint internalformat, GLsizei w, GLsizei h,
                        GLsizei d, GLint border, GLenum format, GLenum type, const void* pixels)
{
  DOLWEB_GL_SHIM(GLP_TexImage3D);
  glTexImage3D(target, level, internalformat, w, h, d, border, format, type, pixels);
}
void ProfiledTexStorage3D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei w,
                          GLsizei h, GLsizei d)
{
  DOLWEB_GL_SHIM(GLP_TexStorage3D);
  glTexStorage3D(target, levels, internalformat, w, h, d);
}
void ProfiledCopyTexSubImage3D(GLenum target, GLint level, GLint xo, GLint yo, GLint zo, GLint x,
                               GLint y, GLsizei w, GLsizei h)
{
  DOLWEB_GL_SHIM(GLP_CopyTexSubImage3D);
  glCopyTexSubImage3D(target, level, xo, yo, zo, x, y, w, h);
}
void ProfiledGenerateMipmap(GLenum target)
{
  DOLWEB_GL_SHIM(GLP_GenerateMipmap);
  glGenerateMipmap(target);
}


// One wrapper for any entry point, so counting a new call costs a line rather
// than a function. The 28 hand-written ones above predate this; they still work
// and are left alone.
template <GLProfileIndex Idx, typename Sig, Sig* fn>
struct GLCount;
template <GLProfileIndex Idx, typename R, typename... A, R (*fn)(A...)>
struct GLCount<Idx, R(A...), fn>
{
  static R Call(A... args)
  {
    DOLWEB_GL_SHIM(Idx);
    return fn(args...);
  }
};
#define DOLWEB_COUNT(slot, gl) \
  {#gl, (void*)&GLCount<slot, decltype(gl), gl>::Call}


// --- DOLWEB_GL_DEDUP=1: drop calls that set state already set ----------------
//
// A level frame issues 3795 GL calls for 466 draws: 811 glBindBufferRange, 288
// glBindTexture, 288 glActiveTexture, 151 glBindSampler, 118 glUseProgram. Much
// of that re-binds what is already bound. On a desktop a redundant bind is a
// pointer compare; in WebGL every call crosses into JS and is validated, and the
// device pays about 3 us for each one.
//
// Filtering here rather than in the backend keeps it reversible and keeps
// Dolphin's code honest: this layer only ever drops a call whose arguments equal
// the ones it already passed through, so the driver sees the same state either
// way. Anything that could be changed behind our back -- buffer *contents*,
// draws, uploads -- is not deduplicated.
bool GLDedupEnabled()
{
  static const bool enabled = std::getenv("DOLWEB_GL_DEDUP") != nullptr;
  return enabled;
}

struct GLStateCache
{
  GLuint program = 0xFFFFFFFFu;
  GLuint vao = 0xFFFFFFFFu;
  GLenum active_unit = 0xFFFFFFFFu;
  GLuint tex[64] = {};       // by unit, for GL_TEXTURE_2D_ARRAY
  GLuint sampler[64] = {};
  struct Range { GLuint buf; GLintptr off; GLsizeiptr size; };
  Range ubo[8] = {};
  bool tex_valid[64] = {};
  bool sampler_valid[64] = {};
  bool ubo_valid[8] = {};
};
GLStateCache g_state;
std::atomic<uint64_t> g_dedup_dropped{0};
std::atomic<uint64_t> g_dedup_total{0};

void DedupUseProgram(GLuint p)
{
  DOLWEB_GL_SHIM(GLP_UseProgram);
  if (g_gl_timing) g_dedup_total.fetch_add(1, std::memory_order_relaxed);
  if (GLDedupEnabled() && p == g_state.program)
  {
    if (g_gl_timing) g_dedup_dropped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  g_state.program = p;
  glUseProgram(p);
}
void DedupActiveTexture(GLenum unit)
{
  DOLWEB_GL_SHIM(GLP_ActiveTexture);
  if (g_gl_timing) g_dedup_total.fetch_add(1, std::memory_order_relaxed);
  if (GLDedupEnabled() && unit == g_state.active_unit)
  {
    if (g_gl_timing) g_dedup_dropped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  g_state.active_unit = unit;
  glActiveTexture(unit);
}
void DedupBindTexture(GLenum target, GLuint tex)
{
  DOLWEB_GL_SHIM(GLP_BindTexture);
  if (g_gl_timing) g_dedup_total.fetch_add(1, std::memory_order_relaxed);
  const unsigned unit = g_state.active_unit - GL_TEXTURE0;
  // Only the array target is cached; Dolphin binds nothing else per draw, and a
  // cache that guessed about other targets would be a correctness risk.
  if (GLDedupEnabled() && target == GL_TEXTURE_2D_ARRAY && unit < 64 &&
      g_state.tex_valid[unit] && g_state.tex[unit] == tex)
  {
    if (g_gl_timing) g_dedup_dropped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (target == GL_TEXTURE_2D_ARRAY && unit < 64)
  {
    g_state.tex[unit] = tex;
    g_state.tex_valid[unit] = true;
  }
  glBindTexture(target, tex);
}
void DedupBindSampler(GLuint unit, GLuint sampler)
{
  DOLWEB_GL_SHIM(GLP_BindSampler);
  if (g_gl_timing) g_dedup_total.fetch_add(1, std::memory_order_relaxed);
  if (GLDedupEnabled() && unit < 64 && g_state.sampler_valid[unit] &&
      g_state.sampler[unit] == sampler)
  {
    if (g_gl_timing) g_dedup_dropped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (unit < 64)
  {
    g_state.sampler[unit] = sampler;
    g_state.sampler_valid[unit] = true;
  }
  glBindSampler(unit, sampler);
}
void DedupBindVertexArray(GLuint vao)
{
  DOLWEB_GL_SHIM(GLP_BindVertexArray);
  if (g_gl_timing) g_dedup_total.fetch_add(1, std::memory_order_relaxed);
  if (GLDedupEnabled() && vao == g_state.vao)
  {
    if (g_gl_timing) g_dedup_dropped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  g_state.vao = vao;
  glBindVertexArray(vao);
}
void DedupBindBufferRange(GLenum target, GLuint index, GLuint buffer, GLintptr offset,
                          GLsizeiptr size)
{
  DOLWEB_GL_SHIM(GLP_BindBufferRange);
  if (g_gl_timing) g_dedup_total.fetch_add(1, std::memory_order_relaxed);
  if (GLDedupEnabled() && target == GL_UNIFORM_BUFFER && index < 8 &&
      g_state.ubo_valid[index] && g_state.ubo[index].buf == buffer &&
      g_state.ubo[index].off == offset && g_state.ubo[index].size == size)
  {
    if (g_gl_timing) g_dedup_dropped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (target == GL_UNIFORM_BUFFER && index < 8)
  {
    g_state.ubo[index] = {buffer, offset, size};
    g_state.ubo_valid[index] = true;
  }
  glBindBufferRange(target, index, buffer, offset, size);
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
    {"glUseProgram", (void*)&DedupUseProgram},
    {"glBindTexture", (void*)&DedupBindTexture},
    {"glBindFramebuffer", (void*)&ProfiledBindFramebuffer},
    {"glBindBufferRange", (void*)&DedupBindBufferRange},
    {"glBindSampler", (void*)&DedupBindSampler},
    {"glUniform4fv", (void*)&ProfiledUniform4fv},
    {"glTexSubImage3D", (void*)&ProfiledTexSubImage3D},
    {"glCompressedTexSubImage3D", (void*)&ProfiledCompressedTexSubImage3D},
    {"glTexImage3D", (void*)&ProfiledTexImage3D},
    {"glTexStorage3D", (void*)&ProfiledTexStorage3D},
    {"glCopyTexSubImage3D", (void*)&ProfiledCopyTexSubImage3D},
    {"glGenerateMipmap", (void*)&ProfiledGenerateMipmap},
    DOLWEB_COUNT(GLP_Enable, glEnable),
    DOLWEB_COUNT(GLP_Disable, glDisable),
    DOLWEB_COUNT(GLP_Scissor, glScissor),
    DOLWEB_COUNT(GLP_Viewport, glViewport),
    {"glActiveTexture", (void*)&DedupActiveTexture},
    DOLWEB_COUNT(GLP_TexParameteri, glTexParameteri),
    DOLWEB_COUNT(GLP_VertexAttribPointer, glVertexAttribPointer),
    DOLWEB_COUNT(GLP_VertexAttribIPointer, glVertexAttribIPointer),
    DOLWEB_COUNT(GLP_EnableVertexAttribArray, glEnableVertexAttribArray),
    DOLWEB_COUNT(GLP_DisableVertexAttribArray, glDisableVertexAttribArray),
    {"glBindVertexArray", (void*)&DedupBindVertexArray},
    DOLWEB_COUNT(GLP_BindBuffer, glBindBuffer),
    DOLWEB_COUNT(GLP_BlendFunc, glBlendFuncSeparate),
    DOLWEB_COUNT(GLP_BlendEquation, glBlendEquationSeparate),
    DOLWEB_COUNT(GLP_ColorMask, glColorMask),
    DOLWEB_COUNT(GLP_DepthMask, glDepthMask),
    DOLWEB_COUNT(GLP_DepthFunc, glDepthFunc),
    DOLWEB_COUNT(GLP_CullFace, glCullFace),
    DOLWEB_COUNT(GLP_Clear, glClear),
    DOLWEB_COUNT(GLP_ClearColor, glClearColor),
    DOLWEB_COUNT(GLP_PixelStorei, glPixelStorei),
    DOLWEB_COUNT(GLP_Uniform1i, glUniform1i),
    DOLWEB_COUNT(GLP_DrawRangeElements, glDrawRangeElements),
    DOLWEB_COUNT(GLP_FlushMappedBufferRange, glFlushMappedBufferRange),
};

bool GLProfilingEnabled()
{
  static const bool enabled = []() {
    const bool on = std::getenv("DOLWEB_TIME_GL") != nullptr;
    g_gl_timing = on;
    return on;
  }();
  return enabled;
}

void ReportGLProfile(double frame_ms)
{
  std::printf("[gl] %.1f ms/frame over the last 100:", frame_ms);
  for (GLProfileEntry& entry : g_gl_profile)
  {
    const uint64_t calls = entry.calls.exchange(0, std::memory_order_relaxed);
    const uint64_t nanos = entry.nanos.exchange(0, std::memory_order_relaxed);
    // Print anything that was called at all: the question is now how many
    // calls a frame issues, not only which ones are individually slow.
    if (calls == 0)
      continue;
    std::printf("  %s %.1fms/%llu", entry.name, nanos / 1e6 / 100.0,
                (unsigned long long)(calls / 100));
  }
  const uint64_t dropped = g_dedup_dropped.exchange(0, std::memory_order_relaxed);
  const uint64_t total = g_dedup_total.exchange(0, std::memory_order_relaxed);
  if (total)
    std::printf("  [dedup %llu/%llu]", (unsigned long long)(dropped / 100),
                (unsigned long long)(total / 100));
  std::printf("\n");
  std::fflush(stdout);
}
}  // namespace

void* GLContextEmscripten::GetFuncAddress(const std::string& name)
{
  // Either flag installs the shims: DOLWEB_TIME_GL wants the timing, and
  // DOLWEB_GL_DEDUP needs the wrappers to exist at all. Gating only on the
  // former made DOLWEB_GL_DEDUP=1 silently measure the baseline.
  if (GLProfilingEnabled() || GLDedupEnabled())
  {
    for (const auto& entry : kProfiledEntries)
    {
      if (name == entry.name)
        return entry.shim;
    }
  }
  return emscripten_webgl_get_proc_address(name.c_str());
}

// DOLWEB_GL_ERRORS=1 drains glGetError once a frame and says what it found.
// WebGL reports nothing to the console for a call the emulator makes wrongly --
// it drops the call and carries on -- so an upload that fails is invisible, and
// the surface it was for just renders black. One line the first time each code
// appears, then a count, because a broken call is usually broken every frame.
static void ReportGLErrors()
{
  static const bool enabled = std::getenv("DOLWEB_GL_ERRORS") != nullptr;
  if (!enabled)
    return;
  static u64 counts[8] = {};
  static bool announced[8] = {};
  static u64 frames = 0;
  ++frames;
  for (int drained = 0; drained < 16; ++drained)
  {
    const GLenum error = glGetError();
    if (error == GL_NO_ERROR)
      break;
    const unsigned slot = error & 7u;
    ++counts[slot];
    if (!announced[slot])
    {
      announced[slot] = true;
      std::printf("[glerr] 0x%04x first seen at frame %llu\n", (unsigned)error,
                  (unsigned long long)frames);
      std::fflush(stdout);
    }
  }
  if (frames % 600 == 0)
  {
    bool any = false;
    for (unsigned slot = 0; slot < 8; ++slot)
    {
      if (!counts[slot])
        continue;
      if (!any)
        std::printf("[glerr] over %llu frames:", (unsigned long long)frames);
      any = true;
      std::printf(" 0x%04x x%llu", 0x0500u | slot, (unsigned long long)counts[slot]);
    }
    if (any)
    {
      std::printf("\n");
      std::fflush(stdout);
    }
  }
}

void GLContextEmscripten::Swap()
{
  ReportGLErrors();
  // Presenting is the one GL call in a frame that can block on the browser's
  // compositor, and it is proxied on top of that, so it is worth being able to
  // ask what it costs before blaming the renderer for a frame time. Off unless
  // asked for; the report is one line per hundred frames.
  static const bool time_swaps = std::getenv("DOLWEB_TIME_SWAP") != nullptr;
  // Swap is where the frame ends, so it is also where both reports are due --
  // but they are separate questions and either can be asked alone.
  // DOLWEB_TIME_GL used to accumulate its per-call totals and never print them,
  // because the only thing that called ReportGLProfile was the swap timer.
  if (!time_swaps && !GLProfilingEnabled())
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
    if (time_swaps)
    {
      std::printf("[swap] %.2f ms average over %llu frames\n", total_ms / 100.0,
                  (unsigned long long)frames);
      std::fflush(stdout);
    }
    if (GLProfilingEnabled())
      ReportGLProfile((now - last_report) / 100.0);
    last_report = now;
    total_ms = 0.0;
  }
}
