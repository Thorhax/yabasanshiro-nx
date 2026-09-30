// SPDX-License-Identifier: GPL-2.0-or-later
//
// UICommon::GameFile / GameFileCache for Saturn disc images.
//
// Every Saturn disc begins its data track with a system header ("IP.BIN"):
//   0x00  "SEGA SEGASATURN "   hardware identifier
//   0x10  maker ID             e.g. "SEGA ENTERPRISES"
//   0x20  product number       e.g. "GS-9100   "
//   0x2A  version              e.g. "V1.000"
//   0x30  release date         YYYYMMDD
//   0x38  device info          e.g. "CD-1/1  "
//   0x40  area symbols         J (Japan), T (Asia NTSC), U (Americas), E (Europe/PAL), K...
//   0x60  game title           112 bytes, space padded

#include "UICommon/GameFile.h"
#include "UICommon/GameFileCache.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string_view>
#include <sys/stat.h>

#include <libchdr/chd.h>

namespace UICommon
{
namespace
{
constexpr std::string_view SATURN_SIGNATURE = "SEGA SEGASATURN ";
constexpr std::size_t SEARCH_BYTES = 64 * 1024;
constexpr std::size_t HEADER_SIZE = 0x100;

std::string Lower(std::string text)
{
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

std::string Extension(const std::string& path)
{
  const std::size_t dot = path.find_last_of('.');
  const std::size_t slash = path.find_last_of('/');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
    return {};
  return Lower(path.substr(dot));
}

std::string Directory(const std::string& path)
{
  const std::size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? std::string{} : path.substr(0, slash + 1);
}

std::string WithExtension(const std::string& path, std::string_view extension)
{
  const std::size_t dot = path.find_last_of('.');
  return (dot == std::string::npos ? path : path.substr(0, dot)) + std::string(extension);
}

bool RegularFileExists(const std::string& path)
{
  struct stat info{};
  return ::stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

// Tries the name as given, then with the other letter case for its extension, since dumps
// are moved between case-sensitive and case-insensitive file systems.
std::string FindCompanion(const std::string& path, std::string_view extension)
{
  std::string candidate = WithExtension(path, extension);
  if (RegularFileExists(candidate))
    return candidate;
  std::string upper(extension);
  std::transform(upper.begin(), upper.end(), upper.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  candidate = WithExtension(path, upper);
  return RegularFileExists(candidate) ? candidate : std::string{};
}

std::vector<std::uint8_t> ReadPrefix(const std::string& path, std::size_t bytes)
{
  std::vector<std::uint8_t> data(bytes);
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (!file)
    return {};
  data.resize(std::fread(data.data(), 1, bytes, file));
  std::fclose(file);
  return data;
}

// Paths of the track files a cue sheet names, in order
std::vector<std::string> CueTracks(const std::string& cue_path)
{
  std::vector<std::string> tracks;
  std::ifstream cue(cue_path);
  std::string line;
  while (std::getline(cue, line))
  {
    const std::size_t start = line.find_first_not_of(" \t");
    if (start == std::string::npos || Lower(line.substr(start, 5)) != "file ")
      continue;
    std::string name;
    const std::size_t quote = line.find('"', start);
    if (quote != std::string::npos)
    {
      const std::size_t end = line.find('"', quote + 1);
      if (end == std::string::npos)
        continue;
      name = line.substr(quote + 1, end - quote - 1);
    }
    else
    {
      // FILE name.bin BINARY
      const std::size_t name_start = line.find_first_not_of(" \t", start + 5);
      const std::size_t name_end = line.find_last_of(" \t");
      if (name_start == std::string::npos || name_end == std::string::npos || name_end <= name_start)
        continue;
      name = line.substr(name_start, name_end - name_start);
    }
    tracks.push_back(name.starts_with('/') || name.find(':') != std::string::npos ?
                         name :
                         Directory(cue_path) + name);
  }
  return tracks;
}

std::vector<std::uint8_t> ReadChdPrefix(const std::string& path)
{
  chd_file* chd = nullptr;
  if (chd_open(path.c_str(), CHD_OPEN_READ, nullptr, &chd) != CHDERR_NONE)
    return {};
  const chd_header* header = chd_get_header(chd);
  std::vector<std::uint8_t> data;
  if (header && header->hunkbytes > 0 && header->hunkbytes <= 1024 * 1024)
  {
    data.resize(header->hunkbytes);
    if (chd_read(chd, 0, data.data()) != CHDERR_NONE)
      data.clear();
  }
  chd_close(chd);
  return data;
}

// The start of the disc's data track, in whatever sector layout the image uses
std::vector<std::uint8_t> ReadDataTrackPrefix(const std::string& path)
{
  const std::string extension = Extension(path);
  if (extension == ".chd")
    return ReadChdPrefix(path);
  if (extension == ".cue")
  {
    const std::vector<std::string> tracks = CueTracks(path);
    return tracks.empty() ? std::vector<std::uint8_t>{} : ReadPrefix(tracks.front(), SEARCH_BYTES);
  }
  if (extension == ".ccd")
  {
    const std::string image = FindCompanion(path, ".img");
    return image.empty() ? std::vector<std::uint8_t>{} : ReadPrefix(image, SEARCH_BYTES);
  }
  if (extension == ".mds")
  {
    const std::string image = FindCompanion(path, ".mdf");
    return image.empty() ? std::vector<std::uint8_t>{} : ReadPrefix(image, SEARCH_BYTES);
  }
  return ReadPrefix(path, SEARCH_BYTES);
}

std::string Field(const std::uint8_t* header, std::size_t offset, std::size_t length)
{
  std::string text(reinterpret_cast<const char*>(header + offset), length);
  for (char& c : text)
  {
    if (static_cast<unsigned char>(c) < 0x20)
      c = ' ';
  }
  const std::size_t begin = text.find_first_not_of(' ');
  if (begin == std::string::npos)
    return {};
  return text.substr(begin, text.find_last_not_of(' ') - begin + 1);
}

// "  ABC   DEF " -> "ABC DEF"
std::string CollapseSpaces(const std::string& text)
{
  std::string result;
  for (char c : text)
  {
    if (c == ' ' && (result.empty() || result.back() == ' '))
      continue;
    result += c;
  }
  if (!result.empty() && result.back() == ' ')
    result.pop_back();
  return result;
}
}  // namespace

GameFile::GameFile(std::string path) : m_file_path(std::move(path))
{
  const std::size_t slash = m_file_path.find_last_of('/');
  m_file_name = slash == std::string::npos ? m_file_path : m_file_path.substr(slash + 1);
  const std::size_t dot = m_file_name.find_last_of('.');
  m_name = dot == std::string::npos ? m_file_name : m_file_name.substr(0, dot);

  struct stat info{};
  if (::stat(m_file_path.c_str(), &info) != 0 || !S_ISREG(info.st_mode))
    return;
  m_file_size = static_cast<std::uint64_t>(info.st_size);
  m_modified = static_cast<std::int64_t>(info.st_mtime);

  const std::vector<std::uint8_t> data = ReadDataTrackPrefix(m_file_path);
  const auto signature = std::search(data.begin(), data.end(), SATURN_SIGNATURE.begin(),
                                     SATURN_SIGNATURE.end());
  if (signature == data.end() || static_cast<std::size_t>(data.end() - signature) < HEADER_SIZE)
    return;
  const std::uint8_t* header = &*signature;

  m_maker = Field(header, 0x10, 16);
  m_game_id = Field(header, 0x20, 10);
  m_internal_name = CollapseSpaces(Field(header, 0x60, 112));
  if (m_name.empty())
    m_name = m_internal_name;

  // "V1.002" -> 1002
  const std::string version = Field(header, 0x2A, 6);
  int major = 0, minor = 0;
  if (std::sscanf(version.c_str(), "V%d.%d", &major, &minor) == 2)
    m_revision = static_cast<std::uint16_t>(major * 1000 + minor);

  // "CD-2/3" -> disc 1 (0-based)
  const std::string device = Field(header, 0x38, 8);
  int disc = 0, discs = 0;
  if (std::sscanf(device.c_str(), "CD-%d/%d", &disc, &discs) == 2 && disc > 0)
    m_disc_number = static_cast<std::uint8_t>(disc - 1);

  // The first area symbol is the primary market
  switch (Field(header, 0x40, 10).c_str()[0])
  {
  case 'J':
  case 'T':
    m_region = DiscIO::Region::NTSC_J;
    break;
  case 'U':
  case 'B':
    m_region = DiscIO::Region::NTSC_U;
    break;
  case 'E':
  case 'A':
  case 'L':
    m_region = DiscIO::Region::PAL;
    break;
  case 'K':
    m_region = DiscIO::Region::NTSC_K;
    break;
  default:
    m_region = DiscIO::Region::Unknown;
    break;
  }

  m_valid = true;
}

void GameFileCache::Clear(DeleteOnDisk)
{
  std::lock_guard lock(m_mutex);
  m_files.clear();
}

std::shared_ptr<const GameFile> GameFileCache::AddOrGet(const std::string& path,
                                                        bool* cache_changed, bool)
{
  {
    std::lock_guard lock(m_mutex);
    const auto found = m_files.find(path);
    if (found != m_files.end())
    {
      struct stat info{};
      if (::stat(path.c_str(), &info) == 0 &&
          static_cast<std::uint64_t>(info.st_size) == found->second->GetFileSize() &&
          static_cast<std::int64_t>(info.st_mtime) == found->second->GetModifiedTime())
      {
        return found->second;
      }
    }
  }

  auto file = std::make_shared<const GameFile>(path);
  std::lock_guard lock(m_mutex);
  m_files[path] = file;
  if (cache_changed)
    *cache_changed = true;
  return file;
}

bool GameFileCache::Update(const std::vector<std::string>& all_game_paths,
                           std::function<void(const std::shared_ptr<const GameFile>&)>,
                           std::function<void(const std::string&)> removed,
                           const std::atomic_bool&, bool)
{
  std::lock_guard lock(m_mutex);
  bool changed = false;
  for (auto it = m_files.begin(); it != m_files.end();)
  {
    if (std::find(all_game_paths.begin(), all_game_paths.end(), it->first) == all_game_paths.end())
    {
      if (removed)
        removed(it->first);
      it = m_files.erase(it);
      changed = true;
    }
    else
    {
      ++it;
    }
  }
  return changed;
}

void GameFileCache::ForEach(
    const std::function<void(const std::shared_ptr<const GameFile>&)>& callback) const
{
  std::lock_guard lock(m_mutex);
  for (const auto& [path, file] : m_files)
    callback(file);
}
std::vector<std::string> DiscImageCompanionFiles(const std::string& path)
{
  std::vector<std::string> files;
  const std::string extension = Extension(path);
  if (extension == ".cue")
  {
    // Only tracks next to the sheet (or below it): a sheet pointing elsewhere mustn't make
    // deleting the game remove unrelated files
    const std::string directory = Directory(path);
    for (const std::string& track : CueTracks(path))
    {
      if (track != path && track.starts_with(directory) &&
          track.find("/../") == std::string::npos && RegularFileExists(track) &&
          std::find(files.begin(), files.end(), track) == files.end())
        files.push_back(track);
    }
  }
  else if (extension == ".ccd")
  {
    for (std::string_view companion : {".img", ".sub"})
    {
      const std::string file = FindCompanion(path, companion);
      if (!file.empty())
        files.push_back(file);
    }
  }
  else if (extension == ".mds")
  {
    const std::string file = FindCompanion(path, ".mdf");
    if (!file.empty())
      files.push_back(file);
  }
  return files;
}
}  // namespace UICommon
