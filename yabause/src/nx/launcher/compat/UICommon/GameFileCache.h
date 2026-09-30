// Stand-in for Dolphin's UICommon/GameFileCache.h. Saturn headers are a single sector, so the
// cache only lives in memory and skips files whose size and date haven't changed.
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "UICommon/GameFile.h"

namespace UICommon
{
class GameFileCache
{
public:
  enum class DeleteOnDisk
  {
    No,
    Yes,
  };

  void Clear(DeleteOnDisk);
  bool Load() { return true; }
  bool Save() { return true; }

  // Returns the (possibly cached) entry for 'path'; 'cache_changed' is set when it was (re)read.
  std::shared_ptr<const GameFile> AddOrGet(const std::string& path, bool* cache_changed,
                                           bool = false);

  // Drops entries whose files are no longer in 'all_game_paths'. Returns whether any were.
  bool Update(const std::vector<std::string>& all_game_paths,
              std::function<void(const std::shared_ptr<const GameFile>&)> = {},
              std::function<void(const std::string&)> = {},
              const std::atomic_bool& = std::atomic_bool{false}, bool = false);

  void ForEach(const std::function<void(const std::shared_ptr<const GameFile>&)>& callback) const;

private:
  mutable std::mutex m_mutex;
  std::map<std::string, std::shared_ptr<const GameFile>> m_files;
};
}  // namespace UICommon
