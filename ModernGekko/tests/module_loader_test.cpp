#include "moderngekko/module_loader.hpp"
#include "core/native_state_layout.h"

#include <cstdint>

namespace
{
int Dispatch(CPUState*, std::uint32_t)
{
  return 0;
}

constexpr ModernGekkoRange ranges[] = {{0x80001000u, 0x80001020u}};
constexpr std::uint64_t hashes[] = {1u};
const ModernGekkoNativeMetadata metadata = {
    "test", "test-target", "test-toolchain", "test-codegen", "test-build",
    dolnative_state_layout_hash()};
const ModernGekkoModuleDesc attached_descriptor = {
    MODERNGEKKO_MODULE_ABI_VERSION,
    MODERNGEKKO_CPU_ABI_VERSION,
    sizeof(CPUState),
    "TEST01",
    0x80001000u,
    Dispatch,
    nullptr,
    ranges,
    1u,
    nullptr,
    0u,
    ranges,
    1u,
    hashes,
    nullptr,
    0u,
    &metadata,
    nullptr,
};
}

int main()
{
  moderngekko::ModuleLibrary library;
  ModernGekkoModuleRequirements requirements = {
      MODERNGEKKO_CPU_ABI_VERSION,
      sizeof(CPUState),
      dolnative_state_layout_hash(),
      "TEST01",
  };

  const moderngekko::ModuleLoadResult loaded =
      library.Open(MODERNGEKKO_TEST_MODULE_PATH, requirements);
  if (loaded.status != moderngekko::ModuleLoadStatus::Ok)
    return 1;
  if (!library.IsOpen() || library.GetDescriptor() == nullptr)
    return 2;
  if (library.GetDescriptor()->entry_point != 0x80003100u)
    return 3;

  library.Close();
  if (library.IsOpen() || library.GetDescriptor() != nullptr)
    return 4;

  requirements.game_id = "OTHER1";
  const moderngekko::ModuleLoadResult rejected =
      library.Open(MODERNGEKKO_TEST_MODULE_PATH, requirements);
  if (rejected.status != moderngekko::ModuleLoadStatus::DescriptorRejected)
    return 5;
  if (rejected.validation_status != MODERNGEKKO_MODULE_GAME_ID_MISMATCH)
    return 6;
  if (library.IsOpen())
    return 7;

  const moderngekko::ModuleLoadResult missing_export =
      library.Open(MODERNGEKKO_MISSING_EXPORT_MODULE_PATH, requirements);
  if (missing_export.status != moderngekko::ModuleLoadStatus::EntryPointMissing ||
      library.IsOpen())
  {
    return 8;
  }

  requirements.game_id = "TEST01";
  const moderngekko::ModuleLoadResult attached =
      library.Attach(&attached_descriptor, requirements);
  if (attached.status != moderngekko::ModuleLoadStatus::Ok || !library.IsOpen() ||
      library.GetDescriptor() != &attached_descriptor)
  {
    return 9;
  }

  return 0;
}
