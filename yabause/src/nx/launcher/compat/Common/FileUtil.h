// Stand-in for the parts of Dolphin's Common/FileUtil.h the launcher uses
#pragma once

#include <string>

// User directories, as in Dolphin. All live under the YabaSanshiro data directory.
enum
{
  D_USER_IDX,
  D_CONFIG_IDX,
  D_GAMESETTINGS_IDX,
  D_CACHE_IDX,
  D_SHADERCACHE_IDX,
  NUM_PATH_INDICES
};

namespace File
{
// Directory for 'index', with a trailing slash
const std::string& GetUserPath(unsigned int index);

bool Exists(const std::string& path);
bool IsDirectory(const std::string& path);
// Creates every missing parent directory of 'path' (a trailing '/' makes it a directory)
bool CreateFullPath(const std::string& path);
bool Rename(const std::string& source, const std::string& destination);
bool ReadFileToString(const std::string& path, std::string& contents);
}  // namespace File
