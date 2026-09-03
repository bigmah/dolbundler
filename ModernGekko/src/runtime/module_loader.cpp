#include "moderngekko/module_loader.hpp"

#if defined(MODERNGEKKO_ENABLE_DYNAMIC_MODULES)
#include "Common/DynamicLibrary.h"
#endif

namespace moderngekko
{
struct ModuleLibrary::Impl
{
#if defined(MODERNGEKKO_ENABLE_DYNAMIC_MODULES)
  Common::DynamicLibrary library;
#endif
  const ModernGekkoModuleDesc* descriptor = nullptr;
};

ModuleLibrary::ModuleLibrary() : m_impl(std::make_unique<Impl>())
{
}
ModuleLibrary::~ModuleLibrary() = default;

ModuleLoadResult ModuleLibrary::Open(
    const std::string& path, const ModernGekkoModuleRequirements& requirements)
{
  Close();

#if defined(MODERNGEKKO_ENABLE_DYNAMIC_MODULES)
  if (!m_impl->library.Open(path.c_str()))
    return {ModuleLoadStatus::LibraryOpenFailed, MODERNGEKKO_MODULE_OK};

  // A module built to share a binary with other games exports its getter
  // under a per-game prefix -- g<GAMEID>_staticrecomp_get_module -- so that
  // two of them can be linked into one iPhone app. The same objects linked
  // into a dylib for this machine keep that name, which is how a phone's
  // module gets played here for a profile; the requirements say which game.
  ModernGekkoGetModuleFn get_module = nullptr;
  if (!m_impl->library.GetSymbol(MODERNGEKKO_GET_MODULE_SYMBOL, &get_module) &&
      requirements.game_id != nullptr)
  {
    const std::string prefixed =
        "g" + std::string(requirements.game_id) + "_" MODERNGEKKO_GET_MODULE_SYMBOL;
    m_impl->library.GetSymbol(prefixed.c_str(), &get_module);
  }
  if (!get_module)
  {
    Close();
    return {ModuleLoadStatus::EntryPointMissing, MODERNGEKKO_MODULE_OK};
  }

  const ModernGekkoModuleDesc* descriptor = get_module();
  const ModernGekkoModuleStatus validation =
      moderngekko_validate_module(descriptor, &requirements);
  if (validation != MODERNGEKKO_MODULE_OK)
  {
    Close();
    return {ModuleLoadStatus::DescriptorRejected, validation};
  }

  m_impl->descriptor = descriptor;
  return {ModuleLoadStatus::Ok, MODERNGEKKO_MODULE_OK};
#else
  (void)path;
  (void)requirements;
  return {ModuleLoadStatus::DynamicLoadingUnavailable, MODERNGEKKO_MODULE_OK};
#endif
}

ModuleLoadResult ModuleLibrary::Attach(
    const ModernGekkoModuleDesc* descriptor,
    const ModernGekkoModuleRequirements& requirements)
{
  Close();
  const ModernGekkoModuleStatus validation =
      moderngekko_validate_module(descriptor, &requirements);
  if (validation != MODERNGEKKO_MODULE_OK)
    return {ModuleLoadStatus::DescriptorRejected, validation};
  m_impl->descriptor = descriptor;
  return {ModuleLoadStatus::Ok, MODERNGEKKO_MODULE_OK};
}

void ModuleLibrary::Close()
{
  m_impl->descriptor = nullptr;
#if defined(MODERNGEKKO_ENABLE_DYNAMIC_MODULES)
  m_impl->library.Close();
#endif
}

bool ModuleLibrary::IsOpen() const
{
  return m_impl->descriptor != nullptr;
}

const ModernGekkoModuleDesc* ModuleLibrary::GetDescriptor() const
{
  return m_impl->descriptor;
}
}
