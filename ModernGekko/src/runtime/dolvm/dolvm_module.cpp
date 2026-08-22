// SPDX-License-Identifier: GPL-3.0-or-later
//
// Presents a .dvm to the chassis as an ordinary recompiled module.
//
// The container already holds everything the module ABI asks for; what is left
// is arranging it the way the ABI wants. Two points are worth knowing:
//
//   Regions are chunks. A region is one call to the IR builder -- the unit the
//   recompiler lowered in one go -- so it is also the natural granule for the
//   chassis's self-modifying-code guard, which retires a chunk to the
//   interpreter when guest RAM stops matching the code it was built from.
//
//   Code ranges are regions merged. The ABI wants coverage as few ranges as
//   possible and chunks tiling them exactly, and consecutive regions from one
//   section abut, so merging is just a walk.

#include "moderngekko/dolvm_module.hpp"

#include "dolvm_bridge.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace moderngekko
{
namespace
{
bool s_open = false;
ModernGekkoModuleDesc s_descriptor{};
std::vector<ModernGekkoRange> s_code_ranges;
std::vector<ModernGekkoRange> s_chunk_ranges;
std::vector<ModernGekkoRange> s_smc_ranges;
std::vector<std::uint64_t> s_chunk_hashes;

std::vector<ModernGekkoRange> MergeAdjacent(const DolVMBridgeRange* ranges, std::uint32_t count)
{
  std::vector<ModernGekkoRange> merged;
  for (std::uint32_t i = 0; i < count; ++i)
  {
    if (!merged.empty() && merged.back().end == ranges[i].start)
    {
      merged.back().end = ranges[i].end;
      continue;
    }
    merged.push_back({ranges[i].start, ranges[i].end});
  }
  return merged;
}
}  // namespace

bool DolVMModule::IsBytecodePath(const std::filesystem::path& path)
{
  std::string extension = path.extension().string();
  std::ranges::transform(extension, extension.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return extension == ".dvm";
}

bool DolVMModule::Open(const std::filesystem::path& path, const std::string& disc_id,
                       std::string* error)
{
  Close();

  DolVMBridgeInfo info{};
  char message[512] = {};
  if (!dolvm_bridge_open(path.string().c_str(), &info, message, sizeof(message)))
  {
    if (error)
      *error = message[0] ? message : "cannot load bytecode module";
    return false;
  }
  if (info.region_count == 0)
  {
    dolvm_bridge_close();
    if (error)
      *error = "bytecode module covers no code";
    return false;
  }

  s_chunk_ranges.clear();
  s_chunk_ranges.reserve(info.region_count);
  for (std::uint32_t i = 0; i < info.region_count; ++i)
    s_chunk_ranges.push_back({info.regions[i].start, info.regions[i].end});
  s_chunk_hashes.assign(info.region_hashes, info.region_hashes + info.region_count);
  s_code_ranges = MergeAdjacent(info.regions, info.region_count);
  s_smc_ranges.clear();
  s_smc_ranges.reserve(info.smc_count);
  for (std::uint32_t i = 0; i < info.smc_count; ++i)
    s_smc_ranges.push_back({info.smc_ranges[i].start, info.smc_ranges[i].end});

  s_descriptor = {};
  s_descriptor.abi_version = MODERNGEKKO_MODULE_ABI_VERSION;
  s_descriptor.cpu_abi_version = MODERNGEKKO_CPU_ABI_VERSION;
  s_descriptor.cpu_state_size = static_cast<std::uint32_t>(sizeof(CPUState));
  // A module built before the container carried a game id still runs; it just
  // cannot be the thing that catches a module paired with the wrong disc.
  const std::string game_id = info.game_id && info.game_id[0] ? info.game_id : disc_id;
  if (game_id.empty() || game_id.size() >= sizeof(s_descriptor.game_id))
  {
    dolvm_bridge_close();
    if (error)
      *error = "bytecode module has no usable game id";
    return false;
  }
  std::memcpy(s_descriptor.game_id, game_id.c_str(), game_id.size());
  s_descriptor.entry_point = info.entry_point;
  s_descriptor.dispatch = &dolvm_bridge_dispatch;
  s_descriptor.on_state_loaded = &dolvm_bridge_on_state_loaded;
  s_descriptor.code_ranges = s_code_ranges.data();
  s_descriptor.num_code_ranges = static_cast<std::uint32_t>(s_code_ranges.size());
  s_descriptor.smc_ranges = s_smc_ranges.empty() ? nullptr : s_smc_ranges.data();
  s_descriptor.num_smc_ranges = static_cast<std::uint32_t>(s_smc_ranges.size());
  s_descriptor.chunk_ranges = s_chunk_ranges.data();
  s_descriptor.num_chunk_ranges = static_cast<std::uint32_t>(s_chunk_ranges.size());
  s_descriptor.chunk_hashes = s_chunk_hashes.data();
  s_descriptor.rel_modules = nullptr;
  s_descriptor.num_rel_modules = 0;

  s_open = true;
  return true;
}

void DolVMModule::Close()
{
  if (s_open)
    dolvm_bridge_close();
  s_open = false;
  s_descriptor = {};
  s_code_ranges.clear();
  s_chunk_ranges.clear();
  s_smc_ranges.clear();
  s_chunk_hashes.clear();
}

const ModernGekkoModuleDesc* DolVMModule::Descriptor()
{
  return s_open ? &s_descriptor : nullptr;
}

void DolVMModule::PublishGate(const StaticRecompDispatchGate* gate, void*)
{
  if (s_open)
    dolvm_bridge_set_gate(gate);
}
}
