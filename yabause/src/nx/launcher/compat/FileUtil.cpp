// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/FileUtil.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

namespace File
{
const std::string& GetUserPath(unsigned int index)
{
  static const std::array<std::string, NUM_PATH_INDICES> paths = {
      "sdmc:/switch/yabasanshiro/",               // D_USER_IDX
      "sdmc:/switch/yabasanshiro/",               // D_CONFIG_IDX
      "sdmc:/switch/yabasanshiro/GameSettings/",  // D_GAMESETTINGS_IDX
      "sdmc:/switch/yabasanshiro/cache/",         // D_CACHE_IDX
      "sdmc:/switch/yabasanshiro/cache/",         // D_SHADERCACHE_IDX
  };
  static const std::string empty;
  return index < paths.size() ? paths[index] : empty;
}

bool Exists(const std::string& path)
{
  struct stat info{};
  return ::stat(path.c_str(), &info) == 0;
}

bool IsDirectory(const std::string& path)
{
  struct stat info{};
  return ::stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool CreateFullPath(const std::string& path)
{
  // Everything up to the last '/' is a directory
  std::size_t position = path.find(":/");
  position = position == std::string::npos ? 0 : position + 2;
  while ((position = path.find('/', position)) != std::string::npos)
  {
    const std::string directory = path.substr(0, position);
    if (::mkdir(directory.c_str(), 0777) != 0 && errno != EEXIST)
      return false;
    ++position;
  }
  return true;
}

bool Rename(const std::string& source, const std::string& destination)
{
  return std::rename(source.c_str(), destination.c_str()) == 0;
}

bool ReadFileToString(const std::string& path, std::string& contents)
{
  std::ifstream file(path, std::ios::binary);
  if (!file)
    return false;
  std::ostringstream buffer;
  buffer << file.rdbuf();
  contents = buffer.str();
  return true;
}
}  // namespace File
