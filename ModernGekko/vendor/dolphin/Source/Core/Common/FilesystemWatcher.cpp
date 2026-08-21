// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/FilesystemWatcher.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE

// The watcher library's only Apple backend is FSEvents, which is not available
// on iOS. Nothing there hot-reloads assets off disk, so the whole class becomes
// a no-op that still satisfies its interface. wtr::watch is completed here only
// so the unique_ptr in the (always empty) member map can be destroyed.
namespace wtr
{
inline namespace watcher
{
class watch
{
};
}  // namespace watcher
}  // namespace wtr

namespace Common
{
FilesystemWatcher::FilesystemWatcher() = default;
FilesystemWatcher::~FilesystemWatcher() = default;
void FilesystemWatcher::Watch(const std::string& path)
{
}
void FilesystemWatcher::Unwatch(const std::string& path)
{
}
}  // namespace Common

#else

#include <wtr/watcher.hpp>

#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"

namespace Common
{
FilesystemWatcher::FilesystemWatcher() = default;
FilesystemWatcher::~FilesystemWatcher() = default;

void FilesystemWatcher::Watch(const std::string& path)
{
  const auto [iter, inserted] = m_watched_paths.try_emplace(path, nullptr);
  if (inserted)
  {
    iter->second = std::make_unique<wtr::watch>(path, [this](wtr::event e) {
      const auto watched_path = PathToString(e.path_name);
      if (e.path_type == wtr::event::path_type::watcher)
      {
        if (watched_path.starts_with('e'))
          ERROR_LOG_FMT(COMMON, "Filesystem watcher: '{}'", watched_path);
        else if (watched_path.starts_with('w'))
          WARN_LOG_FMT(COMMON, "Filesystem watcher: '{}'", watched_path);
        return;
      }

      if (e.effect_type == wtr::event::effect_type::create)
      {
        const auto unified_path = WithUnifiedPathSeparators(watched_path);
        PathAdded(unified_path);
      }
      else if (e.effect_type == wtr::event::effect_type::modify)
      {
        const auto unified_path = WithUnifiedPathSeparators(watched_path);
        PathModified(unified_path);
      }
      else if (e.effect_type == wtr::event::effect_type::rename)
      {
        if (!e.associated)
        {
          WARN_LOG_FMT(COMMON, "Rename on path '{}' seen without association!", watched_path);
          return;
        }

        const auto old_path = WithUnifiedPathSeparators(watched_path);
        const auto new_path = WithUnifiedPathSeparators(PathToString(e.associated->path_name));
        PathRenamed(old_path, new_path);
      }
      else if (e.effect_type == wtr::event::effect_type::destroy)
      {
        const auto unified_path = WithUnifiedPathSeparators(watched_path);
        PathDeleted(unified_path);
      }
    });
  }
}

void FilesystemWatcher::Unwatch(const std::string& path)
{
  m_watched_paths.erase(path);
}
}  // namespace Common

#endif
