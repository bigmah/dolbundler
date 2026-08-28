// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <emscripten/html5_webgl.h>

#include <string>

#include "Common/GL/GLContext.h"

// WebGL2 is OpenGL ES 3.0, which is a target Dolphin's GL backend already
// supports; what a browser does not have is EGL, GLX or WGL. Emscripten's own
// html5 API creates the context instead, and it is the only one of the three
// that understands the constraint that matters here: the WebGL context belongs
// to whichever thread owns the canvas, and Dolphin's video backend runs on a
// thread it created itself. PROXY_ALWAYS routes the GL calls to the canvas's
// thread so any of Dolphin's threads may render.
class GLContextEmscripten final : public GLContext
{
public:
  ~GLContextEmscripten() override;

  bool IsHeadless() const override;

  std::unique_ptr<GLContext> CreateSharedContext() override;

  bool MakeCurrent() override;
  bool ClearCurrent() override;

  void Update() override;

  void Swap() override;
  void SwapInterval(int interval) override;

  void* GetFuncAddress(const std::string& name) override;

protected:
  bool Initialize(const WindowSystemInfo& wsi, bool stereo, bool core) override;

private:
  EMSCRIPTEN_WEBGL_CONTEXT_HANDLE m_context = 0;
  std::string m_target;
};
