// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "moderngekko/module_abi.h"

#include <filesystem>
#include <string>

struct StaticRecompDispatchGate;

namespace moderngekko
{
// A recompiled game the chassis interprets instead of jumping into.
//
// `.dvm` is the same recompilation the C and LLVM backends produce, lowered to
// a register machine's instruction stream rather than to host machine code. It
// reaches the chassis through the ordinary module ABI: this class opens the
// container, checks it, and hands back a ModernGekkoModuleDesc whose dispatch
// runs the interpreter. Nothing downstream of the descriptor can tell the
// difference, which is the point -- a host that may not create executable
// pages gets the same coverage, SMC guard and host-call interception as one
// that can.
//
// One per process: the module ABI's dispatch is a bare function pointer with
// no user data, so the loaded module has to be reachable from file scope.
class DolVMModule final
{
public:
  static bool IsBytecodePath(const std::filesystem::path& path);

  // Opens `path`. `disc_id` is only a fallback for a module built before the
  // container carried a game id; a module that names one keeps its own.
  static bool Open(const std::filesystem::path& path, const std::string& disc_id,
                   std::string* error);
  static void Close();

  // Valid until Close(). Null when nothing is open.
  static const ModernGekkoModuleDesc* Descriptor();

  // The chassis's dispatch gate, or null to withdraw it. Shaped to be handed
  // straight to StaticRecompModuleSource::publish_gate.
  static void PublishGate(const StaticRecompDispatchGate* gate, void* user);
};
}
