// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/GL/GLInterface/Emscripten.h"

#include <emscripten/emscripten.h>
#include <emscripten/html5.h>

#include "Common/Logging/Log.h"

GLContextEmscripten::~GLContextEmscripten()
{
  if (m_context)
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
  // Both of these exist for the same reason. WebGL presents implicitly when an
  // event callback returns, which is meaningless for an emulator whose frame is
  // produced on its own thread, so rendering goes to an offscreen back buffer
  // and Swap() commits it. That in turn is what allows the context to be used
  // from a thread that does not own the canvas.
  attributes.explicitSwapControl = true;
  attributes.renderViaOffscreenBackBuffer = true;
  attributes.proxyContextToMainThread = EMSCRIPTEN_WEBGL_CONTEXT_PROXY_ALWAYS;

  m_context = emscripten_webgl_create_context(m_target.c_str(), &attributes);
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

void GLContextEmscripten::Swap()
{
  emscripten_webgl_commit_frame();
}

void GLContextEmscripten::SwapInterval(int interval)
{
  // The browser's compositor owns presentation timing; there is nothing to set.
  (void)interval;
}

void* GLContextEmscripten::GetFuncAddress(const std::string& name)
{
  return emscripten_webgl_get_proc_address(name.c_str());
}
