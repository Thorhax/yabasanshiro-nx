// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinSwitch/Launcher.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <switch.h>

#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <climits>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/HookableEvent.h"
#include "Common/IniFile.h"
#include "Common/ScopeGuard.h"
#include "DiscIO/Enums.h"
#include "DolphinSwitch/CoverDownload.h"
#include "DolphinSwitch/Forwarder.h"
#include "DolphinSwitch/Localization.h"
#include "DolphinSwitch/Storage.h"
#include "DolphinSwitch/SystemLanguage.h"
#include "DolphinSwitch/UiAudio.h"
#include "UICommon/GameFile.h"
#include "UICommon/GameFileCache.h"
#include "nx/saturn_controls.h"

// YabaSanshiro: progress reporting for the app's hang watchdog (nx/main.cpp)
void NxLauncherHeartbeat();
void NxLauncherStep(const char* step);

namespace DolphinSwitch
{
namespace
{
constexpr std::string_view DATA_DIRECTORY = "sdmc:/switch/yabasanshiro";
constexpr std::string_view CONFIG_PATH = "sdmc:/switch/yabasanshiro/launcher.ini";
constexpr std::string_view COVER_DIRECTORY = "sdmc:/switch/yabasanshiro/covers";
constexpr int COVER_CACHE_LIMIT = 64;
constexpr int COVER_REQUEST_BUDGET = 48;
constexpr int COVER_UPLOAD_BUDGET = 2;
constexpr std::size_t COVER_JOB_LIMIT = 96;
constexpr std::size_t COVER_READY_LIMIT = 4;
constexpr std::size_t TEXT_CACHE_LIMIT = 512;
constexpr std::size_t TEXT_CACHE_BYTES = 12 * 1024 * 1024;
constexpr std::size_t METRIC_CACHE_LIMIT = 2048;
constexpr std::size_t ELLIPSIS_CACHE_LIMIT = 512;
constexpr std::size_t BUSY_TASK_STACK_SIZE = 2 * 1024 * 1024;

// SDL's B button is the physical A button on Switch.
constexpr SDL_GameControllerButton BUTTON_CONFIRM = SDL_CONTROLLER_BUTTON_B;
constexpr SDL_GameControllerButton BUTTON_CANCEL = SDL_CONTROLLER_BUTTON_A;
constexpr SDL_GameControllerButton BUTTON_SETTINGS = SDL_CONTROLLER_BUTTON_Y;

// Button glyphs are rendered at three times their display size and downscaled when blitted.
constexpr int GLYPH_SUPERSAMPLE = 3;

constexpr std::array<std::pair<std::string_view, std::string_view>, 8> LIBRARY_FOOTER = {{
    {"A", "Launch"},
    {"Y", "Sort"},
    {"X", "Settings"},
    {"+", "Game Menu"},
    {"-", "Filter"},
    {"L", ""},
    {"R", "Page"},
    {"B", "Quit"},
}};

// The per-game menu, split into the everyday actions and the destructive "Manage game" group.
constexpr int GAME_MENU_COUNT = 8;
constexpr int GAME_MENU_MANAGE_START = 6;
constexpr std::array<std::string_view, GAME_MENU_COUNT> GAME_MENU_ITEMS = {
    "Launch",
    "Game settings",
    "Rename game",
    "Favorite / collections",
    "Cover settings",
    "Create HOME shortcut",
    "Clear game settings",
    "Delete game (remove from storage)",
};

struct BusyTaskThreadContext
{
  const std::function<void()>* task = nullptr;
  std::atomic<bool>* complete = nullptr;
  std::atomic_bool* cancel = nullptr;
};

void BusyTaskThreadEntry(void* userdata)
{
  auto* context = static_cast<BusyTaskThreadContext*>(userdata);
  (*context->task)();
  context->complete->store(true, std::memory_order_release);
  SDL_Event wake{};
  wake.type = SDL_USEREVENT;
  wake.user.code = 0x42555359;  // BUSY: cancellable launcher task completed.
  SDL_PushEvent(&wake);
}

std::string Trim(std::string value)
{
  const std::size_t first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos)
    return {};
  const std::size_t last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

std::string Lower(std::string value)
{
  std::ranges::transform(value, value.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool EnsureDirectory(std::string_view path)
{
  const std::string owned(path);
  if (::mkdir(owned.c_str(), 0777) == 0)
    return true;
  if (errno != EEXIST)
    return false;
  struct stat info{};
  return ::stat(owned.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool RegularFileExists(const std::string& path)
{
  struct stat info{};
  return ::stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

bool QueryRegularFile(const std::string& path, bool* exists)
{
  if (!exists)
    return false;
  struct stat info{};
  if (::stat(path.c_str(), &info) == 0)
  {
    *exists = true;
    return S_ISREG(info.st_mode);
  }
  *exists = false;
  return errno == ENOENT;
}

bool RecoverAtomicFile(const std::string& path)
{
  const std::string temporary = path + ".tmp";
  const std::string backup = path + ".old";
  bool current_exists = false;
  bool backup_exists = false;
  bool temporary_exists = false;
  if (!QueryRegularFile(path, &current_exists) || !QueryRegularFile(backup, &backup_exists) ||
      !QueryRegularFile(temporary, &temporary_exists))
    return false;
  if (!current_exists && backup_exists)
  {
    if (std::rename(backup.c_str(), path.c_str()) != 0)
      return false;
    fsdevCommitDevice("sdmc");
    current_exists = true;
    backup_exists = false;
  }
  if (temporary_exists && std::remove(temporary.c_str()) != 0)
    return false;
  if (current_exists && backup_exists && std::remove(backup.c_str()) != 0)
    return false;
  if (temporary_exists || backup_exists)
    fsdevCommitDevice("sdmc");
  return true;
}

class Store
{
public:
  bool Load(const std::string& path)
  {
    m_values.clear();
    if (!RecoverAtomicFile(path))
      return false;
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file)
      return false;
    char line[4096];
    while (std::fgets(line, sizeof(line), file))
    {
      std::string text = Trim(line);
      if (text.empty() || text.front() == '#' || text.front() == ';' || text.front() == '[')
        continue;
      const std::size_t separator = text.find('=');
      if (separator == std::string::npos)
        continue;
      std::string key = Trim(text.substr(0, separator));
      if (!key.empty())
        m_values[std::move(key)] = Trim(text.substr(separator + 1));
    }
    return std::fclose(file) == 0;
  }

  bool Save(const std::string& path) const
  {
    if (!RecoverAtomicFile(path))
      return false;
    const std::string temporary = path + ".tmp";
    FILE* file = std::fopen(temporary.c_str(), "wb");
    if (!file)
      return false;
    bool ok = std::fputs("# YabaSanshiro NX launcher\n", file) >= 0;
    for (const auto& [key, value] : m_values)
    {
      if (!ok || std::fprintf(file, "%s = %s\n", key.c_str(), value.c_str()) < 0)
      {
        ok = false;
        break;
      }
    }
    if (std::fflush(file) != 0)
      ok = false;
    if (::fsync(::fileno(file)) != 0)
      ok = false;
    if (std::fclose(file) != 0)
      ok = false;
    if (!ok)
    {
      std::remove(temporary.c_str());
      return false;
    }
    const std::string backup = path + ".old";
    std::remove(backup.c_str());
    const bool had_current = RegularFileExists(path);
    if (had_current && std::rename(path.c_str(), backup.c_str()) != 0)
    {
      std::remove(temporary.c_str());
      return false;
    }
    if (std::rename(temporary.c_str(), path.c_str()) != 0)
    {
      if (had_current)
        std::rename(backup.c_str(), path.c_str());
      std::remove(temporary.c_str());
      return false;
    }
    fsdevCommitDevice("sdmc");
    if (had_current)
    {
      if (std::remove(backup.c_str()) == 0)
        fsdevCommitDevice("sdmc");
    }
    return true;
  }

  std::string Get(std::string_view key, std::string_view fallback = {}) const
  {
    const auto iterator = m_values.find(std::string(key));
    return iterator == m_values.end() ? std::string(fallback) : iterator->second;
  }

  int GetInt(std::string_view key, int fallback) const
  {
    const std::string value = Get(key);
    if (value.empty())
      return fallback;
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    return end != value.c_str() && *end == '\0' ? static_cast<int>(parsed) : fallback;
  }

  bool GetBool(std::string_view key, bool fallback) const
  {
    const std::string value = Lower(Get(key));
    if (value == "true" || value == "1" || value == "yes")
      return true;
    if (value == "false" || value == "0" || value == "no")
      return false;
    return fallback;
  }

  void Set(std::string key, std::string value) { m_values[std::move(key)] = std::move(value); }
  void SetInt(std::string key, int value) { Set(std::move(key), std::to_string(value)); }
  void SetBool(std::string key, bool value) { Set(std::move(key), value ? "true" : "false"); }
  void Remove(std::string_view key) { m_values.erase(std::string(key)); }

  void RemovePrefix(std::string_view prefix)
  {
    for (auto iterator = m_values.begin(); iterator != m_values.end();)
    {
      if (iterator->first.starts_with(prefix))
        iterator = m_values.erase(iterator);
      else
        ++iterator;
    }
  }

private:
  std::map<std::string, std::string> m_values;
};

std::string NormalizePath(std::string path)
{
  path = Trim(path);
  std::ranges::replace(path, '\\', '/');
  const std::size_t colon = path.find(':');
  const std::size_t protected_length = colon == std::string::npos ? 1 : colon + 2;
  for (std::size_t index = 1; index < path.size();)
  {
    if (path[index] == '/' && path[index - 1] == '/')
      path.erase(index, 1);
    else
      ++index;
  }
  while (path.size() > protected_length && path.back() == '/')
    path.pop_back();
  if (colon != std::string::npos && path.size() == colon + 1)
    path += '/';
  return path;
}

std::string JoinPath(const std::string& base, std::string_view child)
{
  if (base.empty())
    return std::string(child);
  return base.back() == '/' ? base + std::string(child) : base + "/" + std::string(child);
}

std::string ParentPath(const std::string& input)
{
  const std::string path = NormalizePath(input);
  const std::size_t colon = path.find(':');
  if (colon == std::string::npos)
  {
    if (path == "/")
      return {};
  }
  else
  {
    bool root = true;
    for (std::size_t index = colon + 1; index < path.size(); ++index)
    {
      if (path[index] != '/')
      {
        root = false;
        break;
      }
    }
    if (root)
      return {};
  }
  const std::size_t slash = path.find_last_of('/');
  if (slash == std::string::npos)
    return {};
  if (colon == std::string::npos && slash == 0)
    return "/";
  if (colon != std::string::npos && slash <= colon + 1)
    return path.substr(0, colon + 2);
  return path.substr(0, slash);
}

std::string FileName(const std::string& path)
{
  const std::size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string DeviceName(const std::string& path)
{
  const std::size_t colon = path.find(':');
  return colon == std::string::npos ? std::string{} : Lower(path.substr(0, colon));
}

bool IsUsbStoragePath(const std::string& path)
{
  const std::size_t colon = path.find(':');
  if (colon < 4)
    return false;
  if (std::tolower(static_cast<unsigned char>(path[0])) != 'u' ||
      std::tolower(static_cast<unsigned char>(path[1])) != 'm' ||
      std::tolower(static_cast<unsigned char>(path[2])) != 's')
    return false;
  for (std::size_t index = 3; index < colon; ++index)
  {
    if (!std::isdigit(static_cast<unsigned char>(path[index])))
      return false;
  }
  return true;
}

std::string UnavailableUsbSourcePath(std::string_view id, std::string_view relative)
{
  // A disconnected stable source must not keep its old mutable umsN: alias: another drive can
  // legitimately receive that alias during startup. This non-filesystem placeholder remains
  // unique and is replaced as soon as the bound volume is available again.
  std::string path = "usbsource:" + std::string(id);
  if (!relative.empty())
    path = JoinPath(path, relative);
  return NormalizePath(std::move(path));
}

bool PathAtOrBelow(const std::string& candidate, const std::string& root)
{
  const std::string normalized_candidate = Lower(NormalizePath(candidate));
  const std::string normalized_root = Lower(NormalizePath(root));
  if (normalized_root.empty())
    return false;
  return normalized_candidate == normalized_root ||
         (normalized_candidate.size() > normalized_root.size() &&
          normalized_candidate.starts_with(normalized_root) &&
          (normalized_root.back() == '/' || normalized_candidate[normalized_root.size()] == '/'));
}

bool IsGamePath(std::string_view path)
{
  // Saturn disc images. A .cue's .bin/.img tracks are left out so each game appears once.
  constexpr std::array<std::string_view, 5> extensions = {".cue", ".chd", ".iso", ".ccd", ".mds"};
  const std::size_t dot = path.find_last_of('.');
  if (dot == std::string_view::npos)
    return false;
  const std::string extension = Lower(std::string(path.substr(dot)));
  return std::ranges::contains(extensions, extension);
}

// The generic game-path helper returns one large vector after a complete recursive traversal.
// This Switch-specific walker publishes entries as readdir discovers them and observes
// cancellation between every directory entry. d_type avoids a network stat for normal SMB
// entries; filesystems which report DT_UNKNOWN still get the required correctness fallback.
bool WalkGamePaths(std::string root, const std::atomic_bool& cancel,
                   const std::function<void(std::string)>& found)
{
  root = NormalizePath(std::move(root));
  struct stat root_info{};
  if (::lstat(root.c_str(), &root_info) == 0 && !S_ISDIR(root_info.st_mode))
  {
    if (S_ISREG(root_info.st_mode) && IsGamePath(root))
      found(std::move(root));
    return true;
  }

  std::vector<std::string> pending{std::move(root)};
  while (!pending.empty())
  {
    if (cancel.load(std::memory_order_acquire))
      return false;
    std::string directory_path = std::move(pending.back());
    pending.pop_back();
    DIR* directory = ::opendir(directory_path.c_str());
    if (!directory)
      continue;
    while (dirent* entry = ::readdir(directory))
    {
      if (cancel.load(std::memory_order_acquire))
      {
        ::closedir(directory);
        return false;
      }
      if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0)
        continue;
      std::string path = JoinPath(directory_path, entry->d_name);
      bool is_directory = entry->d_type == DT_DIR;
      bool is_file = entry->d_type == DT_REG;
      if (entry->d_type == DT_UNKNOWN)
      {
        struct stat info{};
        if (::lstat(path.c_str(), &info) != 0)
          continue;
        is_directory = S_ISDIR(info.st_mode);
        is_file = S_ISREG(info.st_mode);
      }
      if (is_directory)
        pending.emplace_back(std::move(path));
      else if (is_file && IsGamePath(path))
        found(std::move(path));
    }
    ::closedir(directory);
  }
  return true;
}

bool IsFilesystemRoot(const std::string& input)
{
  const std::string path = NormalizePath(input);
  if (path.empty() || path == "/")
    return true;
  const std::size_t colon = path.find(':');
  if (colon == std::string::npos)
    return false;
  return std::ranges::all_of(path.substr(colon + 1), [](char value) { return value == '/'; });
}

bool ValidEntryName(std::string_view name)
{
  if (name.empty() || name == "." || name == ".." || name.size() > 255)
    return false;
  return std::ranges::none_of(name, [](unsigned char value) {
    return value < ' ' || value == '/' || value == '\\' || value == ':';
  });
}

std::string HumanBytes(std::uint64_t value)
{
  constexpr std::array<const char*, 5> units = {"B", "KiB", "MiB", "GiB", "TiB"};
  double amount = static_cast<double>(value);
  std::size_t unit = 0;
  while (amount >= 1024.0 && unit + 1 < units.size())
  {
    amount /= 1024.0;
    ++unit;
  }
  char text[64];
  std::snprintf(text, sizeof(text), unit == 0 ? "%.0f %s" : "%.1f %s", amount, units[unit]);
  return text;
}

std::uint64_t HashPath(std::string_view path)
{
  std::uint64_t hash = 1469598103934665603ULL;
  for (const unsigned char value : path)
  {
    hash ^= value;
    hash *= 1099511628211ULL;
  }
  return hash;
}

std::string Hex64(std::uint64_t value)
{
  char text[17]{};
  std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
  return text;
}

std::string StableIdStem(std::string_view game_id, std::string_view fingerprint)
{
  std::string prefix;
  for (const unsigned char character : game_id)
  {
    if (std::isalnum(character))
      prefix += static_cast<char>(std::tolower(character));
  }
  if (prefix.empty())
    prefix = "game";
  return prefix + "-" + Hex64(HashPath(fingerprint));
}

enum class Theme
{
  Xmb,
  Bubbles,
  Glow,
  Classic,
  Oled,
};

enum class SortMode
{
  Alphabetical,
  RecentlyPlayed,
  RecentlyAdded,
};

struct Game
{
  std::shared_ptr<const UICommon::GameFile> metadata;
  std::string path;
  std::string title;
  // Stable launcher identity.  Unlike the legacy key, this never contains the file path and is
  // retained in launcher.ini when an image is renamed or a USB device receives another umsN:.
  std::string key;
  std::string legacy_key;
  std::string fingerprint;
  // A deliberately content-insensitive identity tuple. Patched/repacked images of the same game
  // retain it, while a different title replacing a file at the same path does not inherit state.
  std::string base_identity;
  std::string canonical_path;
  std::string storage_id;
  std::string game_id;
  std::string game_tdb_id;
  std::string platform;
  std::string config_override_path;
  std::uint64_t title_id = 0;
  std::uint16_t revision = 0;
  std::int64_t modified = 0;
  std::int64_t played = 0;
  DiscIO::Region region = DiscIO::Region::Unknown;
  bool has_game_config = false;
  bool has_custom_title = false;
  bool installed_nand = false;
  bool allow_legacy_path_migration = true;
  // True when GameFileCache rebuilt this entry because its source/dependencies changed.  The
  // stable fingerprint is intentionally content-insensitive for normal discs, so this separately
  // invalidates metadata-derived artwork without forcing a full-image hash.
  bool metadata_refreshed = false;
  SDL_Texture* cover = nullptr;
  std::uint64_t cover_use = 0;
  std::uint64_t cover_request = 0;
  Uint32 cover_loaded_at = 0;
  bool cover_attempted = false;
  bool cover_queued = false;
};

struct LibraryIdentityRecord
{
  std::string id;
  std::string fingerprint;
  std::string base_identity;
  std::string canonical_path;
  // Current case-preserving filesystem path.  Forwarders resolve this through the stable record
  // after a rename; canonical_path remains the mount-independent matching key.
  std::string current_path;
  // Exact prior filesystem paths are only used to find forwarders made before stable IDs existed.
  // Keep this bounded: paths may include mutable umsN aliases and are never launch candidates.
  std::vector<std::string> previous_paths;
  // A different title replaced the path formerly owned by this record. Retired records remain
  // available for same-volume fingerprint recovery, but forwarders must never resolve them.
  bool retired = false;
};

constexpr std::size_t MAX_PREVIOUS_LIBRARY_PATHS = 4;

std::string LegacyBaseIdentityFromFingerprint(std::string_view fingerprint)
{
  // Fingerprints written by the first stable-ID implementation were
  // game-id:revision:disc:platform:size:sync-hash.  The prefix is sufficient to migrate those
  // records without treating a patched image as another game.
  std::size_t end = 0;
  for (int separator = 0; separator < 4; ++separator)
  {
    end = fingerprint.find(':', end);
    if (end == std::string_view::npos)
      return {};
    ++end;
  }
  return std::string(fingerprint.substr(0, end - 1));
}

void RememberPreviousLibraryPath(LibraryIdentityRecord* record, std::string_view path)
{
  if (!record)
    return;
  const std::string normalized = NormalizePath(std::string(path));
  if (normalized.empty())
    return;
  std::erase_if(record->previous_paths, [&](const std::string& previous) {
    return Lower(NormalizePath(previous)) == Lower(normalized);
  });
  record->previous_paths.emplace_back(normalized);
  if (record->previous_paths.size() > MAX_PREVIOUS_LIBRARY_PATHS)
    record->previous_paths.erase(record->previous_paths.begin(),
                                 record->previous_paths.end() - MAX_PREVIOUS_LIBRARY_PATHS);
}

std::string LibraryIdentityScope(std::string_view canonical_path)
{
  const std::string canonical = Lower(NormalizePath(std::string(canonical_path)));
  if (canonical.starts_with("usb:") || canonical.starts_with("smb:"))
  {
    const std::size_t slash = canonical.find('/', 4);
    return canonical.substr(0, slash);
  }
  const std::size_t colon = canonical.find(':');
  return colon == std::string::npos ? std::string{} : canonical.substr(0, colon + 1);
}

struct Collection
{
  std::string name;
  std::unordered_set<std::string> members;
};

struct LibraryScanState
{
  std::atomic_bool cancel{false};
  std::atomic_bool complete{false};
  std::mutex mutex;
  std::deque<Game> ready;
  std::atomic<std::size_t> discovered{0};
  std::atomic<std::size_t> processed{0};
  bool full = true;
  bool cache_changed = false;
  std::size_t unsorted_published = 0;
  std::unordered_set<std::string> target_usb_ids;
};

struct CoverDecodeJob
{
  std::string key;
  std::string custom_path;
  std::shared_ptr<const UICommon::GameFile> metadata;
  std::uint64_t request = 0;
  std::uint64_t epoch = 0;
};

struct CoverDecodeResult
{
  std::string key;
  std::uint64_t request = 0;
  std::uint64_t epoch = 0;
  int width = 0;
  int height = 0;
  std::vector<Uint8> pixels;
};

struct SmbAutoMountState
{
  std::atomic_bool cancel{false};
  std::atomic_bool complete{false};
  std::mutex mutex;
  std::deque<std::string> mounted_roots;
};

struct UsbInitializationState
{
  std::atomic_bool complete{false};
  bool success = false;
  std::string error;
};

constexpr SDL_Color STATUS_OK_COLOR{120, 220, 120, 255};
constexpr SDL_Color STATUS_MISSING_COLOR{235, 125, 115, 255};

struct Row
{
  std::string label;
  std::string value;
  bool enabled = true;
  bool destructive = false;
  bool adjustable = true;
  bool localize_label = true;
  bool localize_value = true;
  std::optional<SDL_Color> value_color;
};

struct SettingHelpEntry
{
  std::string_view label;
  std::string_view kind;
  std::string_view description;
};

struct SettingHelpInfo
{
  std::string_view kind;
  std::string description;
};

static constexpr SettingHelpEntry SETTING_HELP[] = {
    {"Launcher", "Settings group",
     "Controls the launcher's theme, game-grid layout, animations and navigation sounds. These "
     "options do not change emulation."},
    {"Language", "Launcher language",
     "Changes the language used by the launcher. System follows the console language. "
     "Translation overrides can be placed in switch/yabasanshiro/i18n on the SD card."},
    {"Library & storage", "Settings group",
     "Manages game folders, USB drives, SMB network shares and cover artwork used by "
     "the launcher."},
    {"Theme", "Launcher appearance",
     "Changes the launcher's background and visual style. It has no effect on the in-game "
     "renderer or performance."},
    {"Games per row", "Library layout",
     "Sets how many game covers appear across each library row. More columns make every cover "
     "smaller."},
    {"Rows per page", "Library layout",
     "Sets how many cover rows are visible on one library page."},
    {"Show game titles", "Library layout",
     "Shows or hides game names below cover artwork in the launcher library."},
    {"Show region flags", "Library layout",
     "Shows or hides the region flag in the top-left corner of each game cover."},
    {"Show custom settings badges", "Library layout",
     "Shows or hides the square badge on games that have per-game settings. The settings "
     "themselves are not changed."},
    {"UI animations", "Launcher appearance",
     "Enables launcher transitions, animated highlights and moving theme elements."},
    {"Sound effects", "Launcher audio",
     "Enables launcher navigation, confirmation and back sound effects."},
    {"SteamGridDB API key", "Artwork service",
     "Edits the API key used to search and download SteamGridDB covers and shortcut icons. Leave "
     "it blank to remove the saved key."},
    {"Download from SteamGridDB", "Artwork service",
     "Searches SteamGridDB for this game and replaces its custom cover with the selected online "
     "artwork."},
    {"Import cover from file", "Local artwork",
     "Imports a PNG, JPEG, WebP or BMP image from SD, USB or SMB storage and stores it as this "
     "game's custom cover."},
    {"Remove custom cover", "Artwork management",
     "Deletes this game's custom cover. The launcher falls back to embedded game artwork when "
     "available."},
    {"Game folders", "Library",
     "The folders the launcher looks for games in, on the SD card, USB drives or SMB shares."},
    {"File manager", "Library",
     "Browses, copies, moves and deletes files on the SD card, USB drives and SMB shares."},
    {"SMB network shares", "Network storage",
     "Adds, edits and connects the network shares games can be played from."},
    {"Download covers", "Artwork service",
     "Downloads a SteamGridDB cover for every game that doesn't have one yet. Needs a "
     "SteamGridDB API key."},
};

std::optional<SettingHelpInfo> SaturnSettingHelp(std::string_view label);

SettingHelpInfo SettingHelpFor(std::string_view title, const Row& row)
{
  if (std::optional<SettingHelpInfo> saturn = SaturnSettingHelp(row.label))
    return *saturn;
  for (const SettingHelpEntry& entry : SETTING_HELP)
  {
    if (entry.label == row.label)
      return {entry.kind, std::string(entry.description)};
  }

  if (row.value == ">")
    return {"Settings group", "Opens this group of settings."};
  if (!row.adjustable)
    return {"Management action",
            "Opens this management action or a file-selection screen."};
  if (!title.empty())
    return {"Setting", "Changes this option. Keep the default value when "
                               "troubleshooting an unexpected game-specific problem."};
  return {"Setting", "Changes this launcher option."};
}

std::string_view SettingScope(std::string_view title, std::string_view context)
{
  (void)context;
  if (title.starts_with("Game ") || title == "Cover settings")
    return "Per-game setting";
  return "Global setting";
}

struct InputBinding
{
  std::string label;
  std::string key;
  std::string default_expression;
};

struct TransferState
{
  std::atomic<std::uint64_t> total{0};
  std::atomic<std::uint64_t> done{0};
  std::atomic<bool> cancelled{false};
  std::atomic<bool> destination_created{false};
  std::string current;
  std::string error;
  std::vector<unsigned char> buffer = std::vector<unsigned char>(256 * 1024);
  std::mutex detail_mutex;
};

void SetTransferDetail(TransferState* state, const std::string& current,
                       const std::string& error = {})
{
  if (!state)
    return;
  std::lock_guard lock(state->detail_mutex);
  if (!current.empty())
    state->current = current;
  if (!error.empty())
    state->error = error;
}

void UsbStatusWake(void*)
{
  SDL_Event wake{};
  wake.type = SDL_USEREVENT;
  wake.user.code = 0x55534248;  // USBH: USB hotplug state changed.
  SDL_PushEvent(&wake);
}

std::string TransferError(TransferState* state)
{
  if (!state)
    return {};
  std::lock_guard lock(state->detail_mutex);
  return state->error;
}

struct TextKey
{
  TTF_Font* font = nullptr;
  Uint32 color = 0;
  std::string text;
  bool operator==(const TextKey&) const = default;
};

struct TextKeyHash
{
  std::size_t operator()(const TextKey& key) const
  {
    std::size_t hash = std::hash<std::string>{}(key.text);
    hash ^= std::hash<TTF_Font*>{}(key.font) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    hash ^= std::hash<Uint32>{}(key.color) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    return hash;
  }
};

struct TextTexture
{
  SDL_Texture* texture = nullptr;
  float width = 0.0f;
  float height = 0.0f;
  std::size_t bytes = 0;
  std::uint64_t use = 0;
};

// Marquee state, keyed by the logical position the text is drawn at.
struct TextScroll
{
  std::string text;
  Uint32 since = 0;
};

struct FooterLayout
{
  std::array<int, 10> item_width{};
  std::array<int, 10> gap_after{};
  std::array<int, 10> row_start{};
  std::array<int, 10> row_end{};
  std::array<int, 10> row_width{};
  int row_count = 0;
  int row_spacing = 0;
  int height = 0;
};

// The cover-plus-content split shared by the per-game detail screens.
struct GameDetailLayout
{
  SDL_Rect preview{};
  SDL_Rect content{};
};

struct GameMenuLayout
{
  GameDetailLayout detail{};
  int start = 0;
  int row_height = 0;
  int destructive_start = 0;
  int RowY(int index) const
  {
    return start + index * row_height + (index >= destructive_start ? 56 : 0);
  }
};

// Shared by the grid renderer and the touch hit test.
struct GridLayout
{
  int columns = 1;
  int rows = 1;
  int cover_width = 0;
  int cover_height = 0;
  int gap_x = 0;
  int gap_y = 0;
  int x0 = 0;
  int y0 = 0;
  int title_height = 0;
  int RowStride() const { return cover_height + (title_height ? title_height + 8 : 0) + gap_y; }
};

struct MetricKey
{
  TTF_Font* font = nullptr;
  std::string text;
  bool operator==(const MetricKey&) const = default;
};

struct MetricKeyHash
{
  std::size_t operator()(const MetricKey& key) const
  {
    std::size_t hash = std::hash<std::string>{}(key.text);
    hash ^= std::hash<TTF_Font*>{}(key.font) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    return hash;
  }
};

struct MetricEntry
{
  int width = 0;
  std::uint64_t use = 0;
};

struct EllipsisKey
{
  TTF_Font* font = nullptr;
  int max_width = 0;
  std::string text;
  bool operator==(const EllipsisKey&) const = default;
};

struct EllipsisKeyHash
{
  std::size_t operator()(const EllipsisKey& key) const
  {
    std::size_t hash = std::hash<std::string>{}(key.text);
    hash ^= std::hash<TTF_Font*>{}(key.font) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    hash ^= std::hash<int>{}(key.max_width) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    return hash;
  }
};

struct EllipsisEntry
{
  std::string text;
  std::uint64_t use = 0;
};

enum class TouchKind
{
  None,
  Tap,
  SwipeLeft,
  SwipeRight,
  ScrollUp,
  ScrollDown,
};

struct TouchGesture
{
  bool active = false;
  bool vertical = false;
  SDL_FingerID finger = 0;
  float start_x = 0.0f;
  float start_y = 0.0f;
  float last_y = 0.0f;
  Uint32 started_at = 0;
};

struct SaturnOption;

class Launcher
{
public:
  Launcher(std::string startup_message, std::string launcher_path)
      : m_startup_message(std::move(startup_message)), m_launcher_path(std::move(launcher_path))
  {
  }
  std::optional<LaunchRequest> Run();
  ~Launcher();

private:
  bool Initialize(bool applet_installer = false);
  void Shutdown();
  bool ConfirmApplicationExit();
  void PrepareApplicationExit();
  void LoadDefaults();
  void MarkStoreDirty();
  void FlushPendingSaves();
  bool LoadFonts();
  void ClearTextCaches();
  void ApplyAppearance();
  void LoadSourcesAndShares();
  void SaveSources();
  void SaveShares();
  void StartAutoMountShares();
  void StopAutoMountShares();
  void PumpAutoMountShares();
  void StartUsbInitialization();
  void StopUsbInitialization();
  void PumpUsbInitialization();
  bool RefreshConfiguredUsbSources();
  void LoadLibraryIdentities();
  void SaveLibraryIdentities();
  std::string CanonicalLibraryPath(std::string_view path) const;
  bool LibraryIdentityPathExists(const LibraryIdentityRecord& record) const;
  std::string GameFingerprint(const UICommon::GameFile& metadata) const;
  std::string GameBaseIdentity(const UICommon::GameFile& metadata) const;
  void AssignStableIdentity(Game* game);
  void MigrateLegacyGameState(Game* game);
  void LoadLibraryOrganization();
  void SaveCollections();
  void RebuildVisibleGames();
  Game* VisibleGame(int index);
  void StartGameScan(std::vector<std::string> sources, bool replace);
  void StopGameScan();
  void PumpGameScan();
  void ScanGames();
  void SortGames();

  void ConfigureLauncherScale();
  int FontMetric(int pixels) const;
  int FontHeight(TTF_Font* font) const;
  void ClearBackground();
  void DrawBubbles(float time);
  void DrawXmb(float time);
  void DrawXmbRibbon(float time, float center, float amplitude, float frequency, float slope,
                     float phase, int half_width, SDL_Color color);
  void DrawXmbFilament(float time, float center, float amplitude, float frequency, float slope,
                       float phase, SDL_Color color);
  void DrawXmbSparkles(float time);
  void EnsureGlowTexture();
  bool HasAnimatedBackground() const;
  SDL_FRect PixelAlignedRect(int x, int y, int width, int height) const;
  void FillRect(int x, int y, int width, int height, SDL_Color color);
  void RoundedRect(int x, int y, int width, int height, int radius, SDL_Color color);
  void Border(int x, int y, int width, int height, int thickness, SDL_Color color);
  void FillCircle(int center_x, int center_y, int radius, SDL_Color color);
  void RoundedPanel(int x, int y, int width, int height, SDL_Color face, SDL_Color edge,
                    int radius = 8, int thickness = 1);
  void GlassPanel(int x, int y, int width, int height);
  void DrawButtonPanel(int x, int y, int width, int height, bool selected);
  void DrawProgressBar(int x, int y, int width, int height, double fraction);
  void DrawRowHighlight(int x, int y, int width, int height);
  SDL_Texture* MakeGlyph(std::string_view label, bool pill);
  SDL_Texture* ButtonGlyph(std::string_view button) const;
  SDL_Texture* MakeFlagTexture(DiscIO::Region region, int width, int height);
  void InitializeUiTextures();
  void DestroyUiTextures();
  void DrawText(TTF_Font* font, int x, int y, std::string_view text, SDL_Color color);
  void DrawTextCentered(TTF_Font* font, int center_x, int y, std::string_view text,
                        SDL_Color color);
  void DrawTextRight(TTF_Font* font, int right_x, int y, std::string_view text, SDL_Color color);
  int TextWidth(TTF_Font* font, std::string_view text);
  std::string Ellipsize(TTF_Font* font, std::string_view text, int max_width);
  int TextScrollOffset(int x, int y, int span, std::string_view text);
  void DrawScrollingTextLeft(TTF_Font* font, int x, int y, int max_width, std::string_view text,
                             SDL_Color color);
  void DrawScrollingTextRight(TTF_Font* font, int right_x, int y, int max_width,
                              std::string_view text, SDL_Color color);
  std::vector<std::string> WrapText(TTF_Font* font, int max_width, int max_lines,
                                    std::string_view text);
  void DrawWrapped(TTF_Font* font, int x, int y, int max_width, int line_height, int max_lines,
                   std::string_view text, SDL_Color color);
  void DrawWrappedCentered(TTF_Font* font, int center_x, int y, int max_width, int line_height,
                           int max_lines, std::string_view text, SDL_Color color);
  void DrawTitleCell(int center_x, int width, int y, const Game& game, bool selected,
                     SDL_Color color);
  int TopBarHeight() const;
  void DrawPageHeader(std::string_view title, std::string_view eyebrow, std::string_view summary,
                      std::string_view detail = {});
  void DrawSectionHeading(std::string_view title, int x, int y, int width);
  void DrawHeader(std::string_view title, std::string_view context = {});
  int SettingsRowHeight() const;
  int SettingsListY() const;
  int SettingsFooterReserve() const;
  void DrawSettingsRowText(std::string_view label, std::string_view value, int slot_y,
                           int column_width, int label_x, int value_x, bool current,
                           SDL_Color label_color, SDL_Color value_color, bool scroll_value = false,
                           int row_height = 0);
  GameDetailLayout ComputeGameDetailLayout() const;
  void DrawArtworkPreview(SDL_Texture* texture, const SDL_Rect& rect, bool selected = false,
                          std::string_view placeholder = "NO COVER");
  void DrawGamePreview(Game* game, const SDL_Rect& rect);
  GameMenuLayout ComputeGameMenuLayout() const;
  void DrawGameMenu(Game* game, int selection);
  FooterLayout MeasureFooter(std::span<const std::pair<std::string_view, std::string_view>> hints);
  void DrawFooterBand(int height);
  void DrawFooter(std::span<const std::pair<std::string_view, std::string_view>> hints,
                  int center_y = -1);
  bool PressFooterButton(int x, int y);
  void DrawButtonHint(int x, int y, std::string_view button, std::string_view label,
                      SDL_Color label_color);
  void DrawSettingsFooter(std::string_view text);
  void BeginScreenFx();
  void DrawFadeIn();
  void ShowInfoCard(std::string_view section, std::string_view title, std::string_view kind,
                    std::string_view description, std::string_view current, std::string_view scope,
                    bool localize_title = true, bool localize_current = false);
  void RenderMessage(std::string_view title, std::span<const std::string> lines,
                     bool localize_lines = false);
  void Toast(std::string message, int milliseconds = 900);
  bool Confirm(std::string_view title, std::span<const std::string> lines,
               bool localize_lines = false);
  int Dropdown(std::string_view title, const std::vector<std::string>& choices, int current,
               bool localize_title = true, bool localize_choices = true);
  int SelectChoice(std::string_view title, std::span<const std::string_view> choices, int current,
                   int delta);
  bool PromptText(std::string_view header, std::string_view initial, std::string* output,
                  bool password = false, bool allow_empty = false, std::string_view subtext = {},
                  std::string_view guide = {});

  bool BeginFrame();
  bool PollEvent(SDL_Event* event);
  void WaitForNextFrame(bool force_animation = false);
  bool FrameNeedsAnimation();
  TouchKind FeedTouch(const SDL_Event& event, int* x, int* y);
  bool TouchScrollList(TouchKind kind, int* selection, int* top, int count, int visible);
  int EventNavigation(const SDL_Event& event) const;
  void QueueNavigationRepeat();
  int RunRows(std::string_view title, std::string_view context,
              const std::function<std::vector<Row>()>& rows,
              const std::function<bool(int, int)>& action, bool touch_activates_full_row = false,
              std::function<bool(int)> reset = {}, std::function<bool(int)> resettable = {});

  CoverDecodeResult DecodeCover(const CoverDecodeJob& job);
  void CoverDecodeThread();
  void StartCoverDecodeWorker();
  void StopCoverDecodeWorker();
  void CancelQueuedCoverDecodes();
  void QueueCoverDecode(Game* game, bool priority);
  void PumpCoverDecodeResults();
  SDL_Texture* UploadCoverTexture(const CoverDecodeResult& result);
  SDL_Texture* LoadScaledTexture(const std::string& path, int width, int height);
  void EnsureCover(Game* game, bool priority = false);
  void ReloadCover(Game* game);
  void EvictCover();
  std::string CoverPath(const Game& game) const;
  void RenderGrid(int selection);
  int GridColumns() const;
  int GridRows() const;
  int GridPageSize() const;
  int GridNavigate(int selection, int dx, int dy) const;
  int GridPage(int selection, int direction) const;
  GridLayout ComputeGridLayout();
  int GridHitTest(int x, int y, int page_start);

  void SettingsRoot();
  // YabaSanshiro settings pages (SaturnPages.inc)
  Common::IniFile& SaturnGlobalFile(bool input);
  std::optional<std::string> GetSaturnGlobal(bool input, std::string_view section,
                                             std::string_view key);
  void SetSaturnGlobal(bool input, std::string_view section, std::string_view key,
                       const std::optional<std::string>& value);
  void SaturnOptionsPage(std::string_view title, std::span<const SaturnOption> options,
                         Game* game);
  void SaturnControlsRoot(Game* game);
  std::string SaturnControllerType(int player, Game* game);
  void SaturnMappingPage(int player, Game* game);
  void SaturnGameSettingsRoot(Game* game);
  void RenderSaturnCapture(std::string_view label, int position, int count, bool releasing,
                           std::string_view current, std::string_view status = {});
  std::optional<std::string> CaptureSaturnButton(std::string_view label, int position, int count,
                                                 std::string_view current);
  void AppearanceSettings();
  void LibrarySettings();
  void LibraryFilterMenu();
  void ManageCollections();
  void EditGameOrganization(Game* game);
  bool EjectUsbLocation(std::string_view stable_id);
  void GameSourcesScreen();
  void FileManager();
  std::string FileBrowser(const std::string& start, bool select_folder, bool select_game,
                          bool manage, std::span<const std::string_view> extensions = {},
                          std::string_view selection_title = {});
  void PerGameMenu(Game* game, bool* launch, bool* rescan);
  void CoverSettings(Game* game);
  void CreateHomeShortcut(Game* game);
  bool ChooseForwarderIcon(Game* game, std::string* output_path);
  void DownloadCovers();
  void NetworkSharesScreen();
  bool EditSmbShare(Storage::SmbShare* share, bool creating);
  void DownloadCover(Game* game);
  int ChooseCoverArtwork(const std::vector<CoverDownload::Artwork>& artwork,
                         std::string_view game_name);
  void ImportCoverFromFile(Game* game);

  bool DeleteTree(const std::string& path, const std::atomic_bool* cancel = nullptr);
  bool MeasureTree(const std::string& path, TransferState* state);
  bool CopyTree(const std::string& source, const std::string& destination, TransferState* state);
  bool RenderTransfer(TransferState* state);
  void RunBusyTask(std::string_view title, std::string_view detail,
                   const std::function<void()>& task, std::atomic_bool* cancel = nullptr);
  bool ExecutePaste(const std::string& folder);
  bool RenamePath(const std::string& path);
  void FileActions(const std::string& path);
  void ReplaceSavedPathPrefix(const std::string& old_path, const std::string& new_path);
  void RemoveSavedPathsBelow(const std::string& root);
  void EnsureSourceMountedAtStartup(const std::string& path);
  std::string GameLocationLabel(const Game& game) const;

  std::string SharedGameIniPath(const Game& game) const;
  std::string EntryGameIniPath(const Game& game) const;
  std::string GameIniPath(const Game& game) const;
  std::optional<std::string> GetGameSetting(const Game& game, std::string_view section,
                                            std::string_view key) const;
  bool SetGameSetting(const Game& game, std::string_view section, std::string_view key,
                      const std::optional<std::string>& value);
  bool
  SetGameSettings(const Game& game,
                  std::initializer_list<
                      std::tuple<std::string_view, std::string_view, std::optional<std::string>>>
                      edits);
  void InvalidateGameSettingCache(const Game& game) const;
  std::string GlobalValueLabel(std::string_view value) const;
  std::string UseGlobalValueLabel(std::string_view value) const;

  Store m_store;
  Common::IniFile m_saturn_settings;
  Common::IniFile m_saturn_input;
  bool m_saturn_settings_loaded = false;
  bool m_saturn_input_loaded = false;
  Localization m_localization;
  std::vector<std::string> m_sources;
  std::vector<Storage::SmbShare> m_shares;
  std::vector<Game> m_games;
  std::vector<std::size_t> m_visible_games;
  std::vector<LibraryIdentityRecord> m_library_identities;
  bool m_library_identities_dirty = false;
  std::unordered_set<std::string> m_claimed_library_ids;
  // Existing canonical paths are reserved during a progressive scan so an earlier, identical
  // renamed image cannot steal their IDs through the fingerprint fallback.
  std::unordered_set<std::string> m_reserved_library_ids;
  std::unordered_set<std::string> m_favorites;
  std::vector<Collection> m_collections;
  std::string m_search_query;
  std::string m_active_collection;
  std::vector<Storage::Location> m_usb_locations;
  // Keyed by the normalized current source path; value is {stable device id, path on device}.
  std::unordered_map<std::string, std::pair<std::string, std::string>> m_usb_source_bindings;
  std::shared_ptr<LibraryScanState> m_library_scan;
  std::thread m_library_scan_thread;
  std::vector<std::string> m_pending_scan_sources;
  bool m_pending_nand_reconciliation = false;
  std::unordered_set<std::string> m_unavailable_usb_ids;
  std::shared_ptr<SmbAutoMountState> m_smb_auto_mount;
  std::thread m_smb_auto_mount_thread;
  std::shared_ptr<UsbInitializationState> m_usb_initialization;
  std::thread m_usb_initialization_thread;
  UICommon::GameFileCache m_game_cache;
  std::optional<LaunchRequest> m_pending_launch;

  SDL_Window* m_window = nullptr;
  SDL_Renderer* m_renderer = nullptr;
  SDL_GameController* m_controller = nullptr;
  TTF_Font* m_font_small = nullptr;
  TTF_Font* m_font = nullptr;
  TTF_Font* m_font_large = nullptr;
  TTF_Font* m_font_caption = nullptr;
  SDL_Texture* m_logo = nullptr;
  SDL_Texture* m_glow = nullptr;
  SDL_Texture* m_round_texture = nullptr;
  std::array<SDL_Texture*, 10> m_glyphs{};
  std::array<SDL_Texture*, 4> m_flags{};
  bool m_sdl_ready = false;
  bool m_ttf_ready = false;
  bool m_image_ready = false;
  bool m_font_service_ready = false;
  bool m_cover_download_ready = false;
  bool m_config_dirty = false;
  bool m_store_dirty = false;
  bool m_library_refresh_requested = false;
  bool m_shutdown = false;
  bool m_running = true;
  bool m_user_exit_requested = false;
  bool m_application_exit_prepared = false;
  int m_width = 1280;
  int m_height = 720;
  int m_output_width = 1280;
  int m_output_height = 720;
  float m_ui_scale = 1.0f;
  float m_font_scale = 1.0f;
  bool m_present_vsync = false;
  Theme m_theme = Theme::Bubbles;
  SortMode m_sort_mode = SortMode::Alphabetical;
  bool m_animations = true;
  bool m_show_titles = true;
  bool m_show_region_flags = true;
  bool m_show_custom_settings_badges = true;
  int m_grid_columns = 5;
  int m_grid_rows = 2;
  int m_cover_decode_budget = 0;
  std::uint64_t m_cover_use = 0;
  std::mutex m_cover_decode_mutex;
  std::condition_variable m_cover_decode_condition;
  std::deque<CoverDecodeJob> m_cover_decode_jobs;
  std::deque<CoverDecodeResult> m_cover_decode_ready;
  std::thread m_cover_decode_thread;
  bool m_cover_decode_started = false;
  bool m_cover_decode_stop = false;
  std::uint64_t m_cover_decode_epoch = 1;
  std::uint64_t m_cover_request_serial = 0;
  std::uint64_t m_usb_generation = 0;
  Uint32 m_usb_refresh_at = 0;
  Uint32 m_screen_fx_start = 0;
  float m_highlight_y = -1.0f;
  float m_last_frame_highlight_y = -1.0f;
  Uint32 m_next_frame_deadline = 0;
  Uint32 m_frame_interval = 0;
  Uint32 m_interaction_animation_until = 0;
  bool m_frame_has_scrolling_text = false;
  std::deque<SDL_Event> m_waited_events;
  int m_navigation_held = 0;
  Uint32 m_navigation_since = 0;
  Uint32 m_navigation_last = 0;
  bool m_stick_x_latched = false;
  bool m_stick_y_latched = false;
  TouchGesture m_touch;
  int m_touch_scroll_steps = 1;
  std::array<SDL_Rect, 10> m_footer_hits{};
  std::array<int, 10> m_footer_buttons{};
  int m_footer_hit_count = 0;
  std::map<std::pair<int, int>, TextScroll> m_text_scroll;
  std::string m_clipboard_path;
  bool m_clipboard_move = false;
  std::string m_startup_message;
  std::string m_launcher_path;
  std::unordered_map<TextKey, TextTexture, TextKeyHash> m_text_cache;
  std::unordered_map<MetricKey, MetricEntry, MetricKeyHash> m_metric_cache;
  std::unordered_map<EllipsisKey, EllipsisEntry, EllipsisKeyHash> m_ellipsis_cache;
  std::unordered_map<std::string, std::pair<int, int>> m_row_positions;
  mutable std::unordered_map<std::string, std::unique_ptr<Common::IniFile>> m_game_ini_cache;
  std::size_t m_text_cache_bytes = 0;
  std::uint64_t m_text_use = 0;

  SDL_Color m_background{0, 8, 16, 255};
  SDL_Color m_text{235, 248, 255, 255};
  SDL_Color m_dim{143, 192, 216, 255};
  SDL_Color m_highlight{118, 222, 255, 255};
  SDL_Color m_value{194, 239, 255, 255};
  SDL_Color m_selection{61, 183, 235, 255};
  SDL_Color m_panel{4, 31, 50, 255};
  SDL_Color m_card{5, 35, 56, 218};
  SDL_Color m_focus{12, 76, 108, 255};
};

#include "SaturnPages.inc"

void Launcher::LoadDefaults()
{
  if (!m_store.Get("Launcher/Initialized").empty())
    return;
  m_store.Set("Launcher/Initialized", "1");
  m_store.Set("Launcher/Theme", "bubbles");
  m_store.Set("Launcher/Language", "system");
  m_store.SetBool("Launcher/Animations", true);
  m_store.SetBool("Launcher/Sounds", true);
  m_store.SetBool("Launcher/ShowTitles", true);
  m_store.SetBool("Launcher/ShowRegionFlags", true);
  m_store.SetBool("Launcher/ShowCustomSettingsBadges", true);
  m_store.SetInt("Launcher/GridColumns", 5);
  m_store.SetInt("Launcher/GridRows", 2);
  m_store.SetInt("Launcher/SortMode", 0);
  m_store.Set("Network/SteamGridDBKey", "");
  // Start with the games folder next to the app; more can be added under Library & storage
  m_sources = {std::string(DATA_DIRECTORY) + "/games"};
  SaveSources();
  MarkStoreDirty();
}

void Launcher::MarkStoreDirty()
{
  m_store_dirty = true;
}

void Launcher::FlushPendingSaves()
{
  m_config_dirty = false;
  if (m_store_dirty)
  {
    if (m_store.Save(std::string(CONFIG_PATH)))
      m_store_dirty = false;
  }
}

void Launcher::ClearTextCaches()
{
  for (auto& [key, value] : m_text_cache)
    SDL_DestroyTexture(value.texture);
  m_text_cache.clear();
  m_metric_cache.clear();
  m_ellipsis_cache.clear();
  m_text_cache_bytes = 0;
  m_text_use = 0;
}

bool Launcher::LoadFonts()
{
  PlSharedFontType font_type = PlSharedFontType_Standard;
  switch (m_localization.GetFontFamily())
  {
  case LauncherFontFamily::SimplifiedChinese:
    font_type = PlSharedFontType_ChineseSimplified;
    break;
  case LauncherFontFamily::TraditionalChinese:
    font_type = PlSharedFontType_ChineseTraditional;
    break;
  case LauncherFontFamily::Korean:
    font_type = PlSharedFontType_KO;
    break;
  case LauncherFontFamily::Standard:
    break;
  }

  PlFontData font_data{};
  if (R_FAILED(plGetSharedFontByType(&font_data, font_type)) || !font_data.address ||
      font_data.size == 0 || font_data.size > INT_MAX)
  {
    return false;
  }

  // Fonts are opened at output resolution; layout stays in 1280x720 space.
  const auto open_font = [&](int size) {
    SDL_RWops* stream = SDL_RWFromConstMem(font_data.address, static_cast<int>(font_data.size));
    return stream ? TTF_OpenFontRW(stream, 1,
                                   static_cast<int>(std::lround(size * m_ui_scale))) :
                    nullptr;
  };
  TTF_Font* const caption = open_font(14);
  TTF_Font* const small = open_font(20);
  TTF_Font* const normal = open_font(26);
  TTF_Font* const large_font = open_font(32);
  if (!caption || !small || !normal || !large_font)
  {
    if (caption)
      TTF_CloseFont(caption);
    if (small)
      TTF_CloseFont(small);
    if (normal)
      TTF_CloseFont(normal);
    if (large_font)
      TTF_CloseFont(large_font);
    return false;
  }

  ClearTextCaches();
  if (m_font_caption)
    TTF_CloseFont(m_font_caption);
  if (m_font_small)
    TTF_CloseFont(m_font_small);
  if (m_font)
    TTF_CloseFont(m_font);
  if (m_font_large)
    TTF_CloseFont(m_font_large);
  m_font_scale = m_ui_scale;
  m_font_caption = caption;
  m_font_small = small;
  m_font = normal;
  m_font_large = large_font;
  return true;
}

void Launcher::ConfigureLauncherScale()
{
  // Lay everything out in a fixed 1280x720 space and let SDL scale it up to the real output.
  m_width = 1280;
  m_height = 720;
  m_ui_scale = std::min(m_output_width / 1280.0f, m_output_height / 720.0f);
  if (!(m_ui_scale > 0.0f))
    m_ui_scale = 1.0f;
  if (!m_renderer)
    return;
  SDL_RenderSetViewport(m_renderer, nullptr);
  SDL_RenderSetScale(m_renderer, m_ui_scale, m_ui_scale);
  SDL_RendererInfo renderer_info{};
  m_present_vsync = SDL_GetRendererInfo(m_renderer, &renderer_info) == 0 &&
                    (renderer_info.flags & SDL_RENDERER_PRESENTVSYNC) != 0;
}

int Launcher::FontMetric(int pixels) const
{
  return static_cast<int>(std::ceil(pixels / m_font_scale));
}

int Launcher::FontHeight(TTF_Font* font) const
{
  return font ? FontMetric(TTF_FontHeight(font)) : 0;
}

bool Launcher::Initialize(bool applet_installer)
{
  constexpr std::array<std::string_view, 2> required_directories = {"sdmc:/switch", DATA_DIRECTORY};
  for (const std::string_view path : required_directories)
  {
    if (!EnsureDirectory(path))
      return false;
  }
  if (!applet_installer && !EnsureDirectory(COVER_DIRECTORY))
  {
    return false;
  }

  (void)m_store.Load(std::string(CONFIG_PATH));
  LoadDefaults();
  m_localization.SetLanguage(m_store.Get("Launcher/Language", "system"));
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0)
  {
    std::fprintf(stderr, "[Launcher] SDL initialization failed: %s\n", SDL_GetError());
    return false;
  }
  m_sdl_ready = true;
  InitializeUiAudio();
  SetUiAudioEnabled(m_store.GetBool("Launcher/Sounds", true));
  if (TTF_Init() != 0)
    return false;
  m_ttf_ready = true;
  const int image_flags = IMG_INIT_PNG | IMG_INIT_JPG | IMG_INIT_WEBP;
  if ((IMG_Init(image_flags) & image_flags) != image_flags)
    return false;
  m_image_ready = true;

  if (appletGetOperationMode() == AppletOperationMode_Console)
  {
    m_width = 1920;
    m_height = 1080;
  }
  m_window = SDL_CreateWindow("YabaSanshiro NX", 0, 0, m_width, m_height, SDL_WINDOW_FULLSCREEN);
  if (!m_window)
  {
    std::fprintf(stderr, "[Launcher] SDL window creation failed: %s\n", SDL_GetError());
    return false;
  }
  m_renderer =
      SDL_CreateRenderer(m_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!m_renderer)
  {
    std::fprintf(stderr, "[Launcher] SDL renderer creation failed: %s\n", SDL_GetError());
    return false;
  }
  SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_BLEND);
  // Must run before LoadFonts, which sizes fonts against m_ui_scale.
  SDL_GetRendererOutputSize(m_renderer, &m_output_width, &m_output_height);
  ConfigureLauncherScale();

  const Result pl_result = plInitialize(PlServiceType_User);
  if (R_FAILED(pl_result))
    return false;
  m_font_service_ready = true;
  if (!LoadFonts())
  {
    // A missing locale-specific shared font must not prevent the launcher from starting. Keep the
    // translations and font family in sync so falling back never renders Chinese as empty boxes.
    m_store.Set("Launcher/Language", "en");
    MarkStoreDirty();
    m_localization.SetLanguage("en");
    if (!LoadFonts())
      return false;
  }

  InitializeUiTextures();

  if (SDL_Surface* surface = IMG_Load("romfs:/Resources/yabasanshiro_logo.png"))
  {
    m_logo = SDL_CreateTextureFromSurface(m_renderer, surface);
    SDL_FreeSurface(surface);
  }
  for (int index = 0; index < SDL_NumJoysticks(); ++index)
  {
    if (SDL_IsGameController(index))
    {
      m_controller = SDL_GameControllerOpen(index);
      break;
    }
  }
  m_cover_download_ready = false;
  ApplyAppearance();
  if (!applet_installer)
  {
    StartCoverDecodeWorker();
    // Present the launcher as soon as SDL, fonts and the theme are ready. Source restoration,
    // network startup and scanning happen after this frame so the user never waits on black.
    ClearBackground();
    DrawHeader("YabaSanshiro NX");
    const int panel_width = std::min(940, m_width - 64);
    constexpr int panel_height = 200;
    GlassPanel((m_width - panel_width) / 2, m_height / 2 - 88, panel_width, panel_height);
    DrawWrappedCentered(m_font_large, m_width / 2, m_height / 2 - 48, panel_width - 64, 48, 2,
                        m_localization.Translate("Loading game library..."), m_value);
    DrawWrappedCentered(
        m_font_small, m_width / 2, m_height / 2 + 26, panel_width - 64, 32, 2,
        m_localization.Translate("The first page will appear as soon as it is ready."), m_dim);
    SDL_RenderPresent(m_renderer);
    LoadSourcesAndShares();
    m_cover_download_ready = CoverDownload::Initialize();
  }
  if (!applet_installer)
    Storage::SetUsbStatusCallback(UsbStatusWake);
  return true;
}

void Launcher::Shutdown()
{
  if (m_shutdown)
    return;
  m_shutdown = true;
  NxLauncherStep("shutdown: workers");
  // The USB callback uses SDL_PushEvent, so fence it before the SDL event subsystem is torn down.
  Storage::SetUsbStatusCallback(nullptr);
  StopGameScan();
  StopUsbInitialization();
  StopAutoMountShares();
  NxLauncherStep("shutdown: cover decoder");
  StopCoverDecodeWorker();
  NxLauncherStep("shutdown: saving");
  FlushPendingSaves();
  NxLauncherStep("shutdown: textures and fonts");

  for (Game& game : m_games)
  {
    if (game.cover)
      SDL_DestroyTexture(game.cover);
    game.cover = nullptr;
  }
  for (auto& [key, value] : m_text_cache)
    SDL_DestroyTexture(value.texture);
  m_text_cache.clear();
  m_metric_cache.clear();
  m_ellipsis_cache.clear();
  m_text_cache_bytes = 0;
  m_text_use = 0;
  DestroyUiTextures();
  if (m_logo)
    SDL_DestroyTexture(m_logo);
  if (m_glow)
    SDL_DestroyTexture(m_glow);
  if (m_round_texture)
    SDL_DestroyTexture(m_round_texture);
  m_logo = m_glow = m_round_texture = nullptr;

  m_games.clear();
  m_game_cache.Clear(UICommon::GameFileCache::DeleteOnDisk::No);

  if (m_font_caption)
    TTF_CloseFont(m_font_caption);
  if (m_font_small)
    TTF_CloseFont(m_font_small);
  if (m_font)
    TTF_CloseFont(m_font);
  if (m_font_large)
    TTF_CloseFont(m_font_large);
  m_font_small = m_font = m_font_large = m_font_caption = nullptr;
  if (m_font_service_ready)
    plExit();
  m_font_service_ready = false;

  NxLauncherStep("shutdown: UI audio");
  ShutdownUiAudio();
  NxLauncherStep("shutdown: SDL");
  if (m_controller)
    SDL_GameControllerClose(m_controller);
  m_controller = nullptr;

  if (m_renderer)
    SDL_DestroyRenderer(m_renderer);
  if (m_window)
    SDL_DestroyWindow(m_window);
  m_renderer = nullptr;
  m_window = nullptr;
  if (m_image_ready)
    IMG_Quit();
  if (m_ttf_ready)
    TTF_Quit();
  if (m_sdl_ready)
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS);
  m_image_ready = m_ttf_ready = m_sdl_ready = false;

  if (m_cover_download_ready)
    CoverDownload::Shutdown();
  m_cover_download_ready = false;
  NxLauncherStep("shutdown: done");
}

bool Launcher::ConfirmApplicationExit()
{
  if (!Confirm("Exit YabaSanshiro NX?",
               std::array<std::string, 2>{
                   std::string(m_localization.Translate(
                       "Active scans and network operations will be cancelled safely.")),
                   std::string(m_localization.Translate("Return to the HOME Menu?"))}))
  {
    BeginScreenFx();
    return false;
  }
  m_user_exit_requested = true;
  m_running = false;
  return true;
}

void Launcher::PrepareApplicationExit()
{
  if (m_application_exit_prepared)
    return;
  m_application_exit_prepared = true;

  // Keep presenting a real frame while cancellable scan/network workers drain. A saved custom
  // collection can become interactive long before a full-library scan is finished; immediately
  // blocking in join() in that state made the Switch compositor fall back to a black frame.
  const auto render_closing = [&] {
    ClearBackground();
    DrawHeader("YabaSanshiro NX");
    const int panel_width = std::min(940, m_width - 64);
    constexpr int panel_height = 200;
    GlassPanel((m_width - panel_width) / 2, m_height / 2 - 88, panel_width, panel_height);
    DrawWrappedCentered(m_font_large, m_width / 2, m_height / 2 - 48, panel_width - 64, 48, 2,
                        m_localization.Translate("Closing YabaSanshiro NX..."), m_value);
    DrawWrappedCentered(m_font_small, m_width / 2, m_height / 2 + 30, panel_width - 64, 32, 2,
                        m_localization.Translate("Finishing background operations safely."), m_dim);
    SDL_RenderPresent(m_renderer);
  };
  render_closing();

  Storage::SetUsbStatusCallback(nullptr);
  if (m_library_scan)
    m_library_scan->cancel.store(true, std::memory_order_release);
  if (m_smb_auto_mount)
    m_smb_auto_mount->cancel.store(true, std::memory_order_release);

  Uint32 next_closing_frame = SDL_GetTicks() + 100;
  while (
      (m_library_scan && !m_library_scan->complete.load(std::memory_order_acquire)) ||
      (m_usb_initialization && !m_usb_initialization->complete.load(std::memory_order_acquire)) ||
      (m_smb_auto_mount && !m_smb_auto_mount->complete.load(std::memory_order_acquire)))
  {
    SDL_PumpEvents();
    const Uint32 now = SDL_GetTicks();
    if (SDL_TICKS_PASSED(now, next_closing_frame))
    {
      render_closing();
      next_closing_frame = now + 100;
    }
    if (!appletMainLoop())
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }
  StopGameScan();
  StopUsbInitialization();
  StopAutoMountShares();
  StopCoverDecodeWorker();
  if (m_cover_download_ready)
  {
    CoverDownload::Shutdown();
    m_cover_download_ready = false;
  }
  FlushPendingSaves();
  // On a game launch storage must stay mounted, but on an explicit application exit it should be
  // retired before SDL disappears so open SMB/USB registrations cannot prolong a black teardown.
  Storage::Shutdown();
}

Launcher::~Launcher()
{
  Shutdown();
}

void Launcher::ApplyAppearance()
{
  const Theme previous_theme = m_theme;
  const std::string theme = Lower(m_store.Get("Launcher/Theme", "bubbles"));
  m_theme = theme == "xmb"     ? Theme::Xmb :
            theme == "glow"    ? Theme::Glow :
            theme == "classic" ? Theme::Classic :
            theme == "oled"    ? Theme::Oled :
                                 Theme::Bubbles;
  m_animations = m_store.GetBool("Launcher/Animations", true);
  m_show_titles = m_store.GetBool("Launcher/ShowTitles", true);
  m_show_region_flags = m_store.GetBool("Launcher/ShowRegionFlags", true);
  m_show_custom_settings_badges = m_store.GetBool("Launcher/ShowCustomSettingsBadges", true);
  m_grid_columns = std::clamp(m_store.GetInt("Launcher/GridColumns", 5), 3, 8);
  m_grid_rows = std::clamp(m_store.GetInt("Launcher/GridRows", 2), 1, 3);
  m_sort_mode = static_cast<SortMode>(std::clamp(m_store.GetInt("Launcher/SortMode", 0), 0, 2));
  if (m_theme == Theme::Xmb)
  {
    m_background = {8, 51, 104, 255};
    m_text = {242, 247, 255, 255};
    m_dim = {180, 204, 227, 255};
    m_highlight = {137, 225, 255, 255};
    m_value = {245, 252, 255, 255};
    m_selection = {118, 213, 255, 255};
    m_panel = {10, 43, 81, 255};
    m_card = {12, 48, 85, 196};
    m_focus = {22, 75, 119, 255};
  }
  else if (m_theme == Theme::Classic)
  {
    m_background = {22, 24, 30, 255};
    m_text = {228, 230, 235, 255};
    m_dim = {150, 155, 165, 255};
    m_highlight = {96, 200, 255, 255};
    m_value = {255, 210, 100, 255};
    m_selection = {255, 170, 0, 255};
    m_panel = {28, 31, 40, 255};
    m_card = {24, 26, 34, 255};
    m_focus = {66, 56, 30, 255};
  }
  else if (m_theme == Theme::Oled)
  {
    m_background = {0, 0, 0, 255};
    m_text = {245, 247, 249, 255};
    m_dim = {145, 151, 158, 255};
    m_highlight = {105, 220, 255, 255};
    m_value = {255, 255, 255, 255};
    m_selection = {0, 210, 190, 255};
    m_panel = {4, 4, 5, 255};
    m_card = {8, 8, 10, 250};
    m_focus = {0, 58, 53, 255};
  }
  else if (m_theme == Theme::Glow)
  {
    m_background = {8, 12, 24, 255};
    m_text = {235, 239, 247, 255};
    m_dim = {151, 163, 184, 255};
    m_highlight = {100, 211, 255, 255};
    m_value = {255, 215, 120, 255};
    m_selection = {116, 200, 255, 255};
    m_panel = {16, 23, 39, 255};
    m_card = {22, 30, 49, 214};
    m_focus = {28, 69, 92, 255};
  }
  else
  {
    m_background = {3, 82, 120, 255};
    m_text = {245, 252, 255, 255};
    m_dim = {187, 229, 243, 255};
    m_highlight = {220, 248, 255, 255};
    m_value = {255, 255, 255, 255};
    m_selection = {111, 224, 249, 255};
    m_panel = {0, 67, 101, 255};
    m_card = {2, 75, 110, 207};
    m_focus = {17, 133, 169, 255};
  }
  if (previous_theme != m_theme && m_renderer)
  {
    for (auto& [key, value] : m_text_cache)
      SDL_DestroyTexture(value.texture);
    m_text_cache.clear();
    m_metric_cache.clear();
    m_ellipsis_cache.clear();
    m_text_cache_bytes = 0;
    m_text_use = 0;
  }
}

SDL_FRect Launcher::PixelAlignedRect(int x, int y, int width, int height) const
{
  // Snap to whole output pixels so hairlines stay one pixel wide.
  float scale_x = 1.0f;
  float scale_y = 1.0f;
  SDL_RenderGetScale(m_renderer, &scale_x, &scale_y);
  if (!(scale_x > 0.0f))
    scale_x = 1.0f;
  if (!(scale_y > 0.0f))
    scale_y = 1.0f;
  const float left = std::round(x * scale_x);
  const float top = std::round(y * scale_y);
  return SDL_FRect{left / scale_x, top / scale_y,
                   (std::round((x + width) * scale_x) - left) / scale_x,
                   (std::round((y + height) * scale_y) - top) / scale_y};
}

void Launcher::FillRect(int x, int y, int width, int height, SDL_Color color)
{
  if (width <= 0 || height <= 0)
    return;
  SDL_SetRenderDrawColor(m_renderer, color.r, color.g, color.b, color.a);
  const SDL_FRect rectangle = PixelAlignedRect(x, y, width, height);
  SDL_RenderFillRectF(m_renderer, &rectangle);
}

void Launcher::RoundedRect(int x, int y, int width, int height, int radius, SDL_Color color)
{
  if (width <= 0 || height <= 0)
    return;
  const int corner = std::min({radius, width / 2, height / 2});
  if (!m_round_texture && m_renderer)
  {
    // One antialiased 32x32 disc, sliced into four 16x16 quadrants, serves every rounded corner.
    SDL_Surface* surface =
        SDL_CreateRGBSurfaceWithFormat(0, 32, 32, 32, SDL_PIXELFORMAT_RGBA32);
    if (surface)
    {
      for (int pixel_y = 0; pixel_y < 32; ++pixel_y)
      {
        auto* row = reinterpret_cast<Uint32*>(static_cast<Uint8*>(surface->pixels) +
                                              pixel_y * surface->pitch);
        for (int pixel_x = 0; pixel_x < 32; ++pixel_x)
        {
          const float dx = pixel_x - 15.5f;
          const float dy = pixel_y - 15.5f;
          const Uint8 alpha = static_cast<Uint8>(
              255.0f * std::clamp(16.0f - std::sqrt(dx * dx + dy * dy), 0.0f, 1.0f));
          row[pixel_x] = SDL_MapRGBA(surface->format, 255, 255, 255, alpha);
        }
      }
      m_round_texture = SDL_CreateTextureFromSurface(m_renderer, surface);
      SDL_FreeSurface(surface);
      if (m_round_texture)
        SDL_SetTextureBlendMode(m_round_texture, SDL_BLENDMODE_BLEND);
    }
  }
  if (!m_round_texture || corner < 1)
  {
    FillRect(x, y, width, height, color);
    return;
  }
  FillRect(x + corner, y, width - corner * 2, height, color);
  FillRect(x, y + corner, corner, height - corner * 2, color);
  FillRect(x + width - corner, y + corner, corner, height - corner * 2, color);
  SDL_SetTextureColorMod(m_round_texture, color.r, color.g, color.b);
  SDL_SetTextureAlphaMod(m_round_texture, color.a);
  for (int quadrant = 0; quadrant < 4; ++quadrant)
  {
    const SDL_Rect source{(quadrant & 1) * 16, (quadrant >> 1) * 16, 16, 16};
    const SDL_FRect destination =
        PixelAlignedRect(x + ((quadrant & 1) ? width - corner : 0),
                         y + ((quadrant >> 1) ? height - corner : 0), corner, corner);
    SDL_RenderCopyF(m_renderer, m_round_texture, &source, &destination);
  }
}

void Launcher::Border(int x, int y, int width, int height, int thickness, SDL_Color color)
{
  SDL_SetRenderDrawColor(m_renderer, color.r, color.g, color.b, color.a);
  for (int index = 0; index < thickness; ++index)
  {
    SDL_Rect rectangle{x - index, y - index, width + index * 2, height + index * 2};
    SDL_RenderDrawRect(m_renderer, &rectangle);
  }
}

bool Launcher::HasAnimatedBackground() const
{
  return m_theme == Theme::Xmb || m_theme == Theme::Bubbles || m_theme == Theme::Glow;
}

void Launcher::FillCircle(int center_x, int center_y, int radius, SDL_Color color)
{
  SDL_SetRenderDrawColor(m_renderer, color.r, color.g, color.b, color.a);
  for (int dy = -radius; dy <= radius; ++dy)
  {
    const int dx =
        static_cast<int>(std::sqrt(static_cast<double>(radius * radius - dy * dy)) + 0.5);
    SDL_RenderDrawLine(m_renderer, center_x - dx, center_y + dy, center_x + dx, center_y + dy);
  }
}

void Launcher::RoundedPanel(int x, int y, int width, int height, SDL_Color face, SDL_Color edge,
                            int radius, int thickness)
{
  RoundedRect(x, y, width, height, radius, edge);
  RoundedRect(x + thickness, y + thickness, width - 2 * thickness, height - 2 * thickness,
              std::max(0, radius - thickness), face);
}

void Launcher::GlassPanel(int x, int y, int width, int height)
{
  RoundedPanel(x, y, width, height, m_panel, SDL_Color{255, 255, 255, 24});
}

void Launcher::DrawButtonPanel(int x, int y, int width, int height, bool selected)
{
  RoundedPanel(x, y, width, height, selected ? m_focus : m_card,
               selected ? m_selection : SDL_Color{255, 255, 255, 28}, 6);
}

void Launcher::DrawProgressBar(int x, int y, int width, int height, double fraction)
{
  // Fully rounded pill; the primitive clamps so callers can hand over a raw ratio.
  RoundedRect(x, y, width, height, height / 2, m_card);
  const int filled = static_cast<int>(width * std::clamp(fraction, 0.0, 1.0));
  if (filled > 0)
    RoundedRect(x, y, filled, height, height / 2, m_selection);
}

void Launcher::DrawRowHighlight(int x, int y, int width, int height)
{
  RoundedRect(x, y, width, height, 4, m_focus);
  FillRect(x, y + height / 5, 3, height * 3 / 5, m_selection);
}

SDL_Texture* Launcher::MakeGlyph(std::string_view label, bool pill)
{
  if (!m_font_small || !m_font_large)
    return nullptr;
  constexpr int supersample = GLYPH_SUPERSAMPLE;
  const int base = FontHeight(m_font_small) + 6;
  const int height = base * supersample;
  const int width = (pill ? base * 8 / 5 : base) * supersample;
  SDL_Texture* texture = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_RGBA8888,
                                           SDL_TEXTUREACCESS_TARGET, width, height);
  if (!texture)
    return nullptr;
  SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
  // Draw in the texture's pixel space, then restore the previous target and scale.
  SDL_Texture* const previous_target = SDL_GetRenderTarget(m_renderer);
  float previous_scale_x = 1.0f;
  float previous_scale_y = 1.0f;
  SDL_RenderGetScale(m_renderer, &previous_scale_x, &previous_scale_y);
  SDL_SetRenderTarget(m_renderer, texture);
  SDL_RenderSetScale(m_renderer, 1.0f, 1.0f);
  SDL_SetRenderDrawColor(m_renderer, 0, 0, 0, 0);
  SDL_RenderClear(m_renderer);
  const SDL_Color edge{14, 16, 22, 255};
  const SDL_Color rim{92, 99, 114, 255};
  const SDL_Color face{52, 57, 68, 255};
  if (pill)
  {
    const int radius = height / 2;
    FillCircle(radius, radius, radius, edge);
    FillCircle(width - radius, radius, radius, edge);
    FillRect(radius, 0, width - radius * 2, height, edge);
    FillCircle(radius, radius, radius - supersample, rim);
    FillCircle(width - radius, radius, radius - supersample, rim);
    FillRect(radius, supersample, width - radius * 2, height - supersample * 2, rim);
    FillCircle(radius, radius, radius - supersample * 2, face);
    FillCircle(width - radius, radius, radius - supersample * 2, face);
    FillRect(radius, supersample * 2, width - radius * 2, height - supersample * 4, face);
  }
  else
  {
    const int radius = height / 2;
    FillCircle(width / 2, height / 2, radius, edge);
    FillCircle(width / 2, height / 2, radius - supersample, rim);
    FillCircle(width / 2, height / 2, radius - supersample * 2, face);
  }
  const std::string owned(label);
  SDL_Surface* surface =
      TTF_RenderUTF8_Blended(m_font_large, owned.c_str(), SDL_Color{246, 248, 252, 255});
  if (surface)
  {
    SDL_Texture* text = SDL_CreateTextureFromSurface(m_renderer, surface);
    int text_width = surface->w;
    int text_height = surface->h;
    const int inner_height = height * 56 / 100;
    if (text_height > 0)
    {
      text_width = text_width * inner_height / text_height;
      text_height = inner_height;
    }
    SDL_Rect destination{(width - text_width) / 2, (height - text_height) / 2, text_width,
                         text_height};
    SDL_FreeSurface(surface);
    if (text)
    {
      SDL_SetTextureBlendMode(text, SDL_BLENDMODE_BLEND);
      SDL_RenderCopy(m_renderer, text, nullptr, &destination);
      SDL_DestroyTexture(text);
    }
  }
  SDL_SetRenderTarget(m_renderer, previous_target);
  SDL_RenderSetScale(m_renderer, previous_scale_x, previous_scale_y);
  return texture;
}

SDL_Texture* Launcher::MakeFlagTexture(DiscIO::Region region, int width, int height)
{
  SDL_Texture* texture = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_RGBA8888,
                                           SDL_TEXTUREACCESS_TARGET, width, height);
  if (!texture)
    return nullptr;
  SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
  SDL_Texture* const previous_target = SDL_GetRenderTarget(m_renderer);
  float previous_scale_x = 1.0f;
  float previous_scale_y = 1.0f;
  SDL_RenderGetScale(m_renderer, &previous_scale_x, &previous_scale_y);
  SDL_SetRenderTarget(m_renderer, texture);
  SDL_RenderSetScale(m_renderer, 1.0f, 1.0f);
  SDL_SetRenderDrawColor(m_renderer, 0, 0, 0, 0);
  SDL_RenderClear(m_renderer);
  if (region == DiscIO::Region::NTSC_J)
  {
    FillRect(0, 0, width, height, SDL_Color{245, 245, 245, 255});
    FillCircle(width / 2, height / 2, height * 30 / 100, SDL_Color{188, 0, 45, 255});
  }
  else if (region == DiscIO::Region::NTSC_U)
  {
    for (int stripe = 0; stripe < 7; ++stripe)
      FillRect(0, stripe * height / 7, width, height / 7 + 1,
               stripe % 2 ? SDL_Color{235, 235, 235, 255} : SDL_Color{178, 34, 52, 255});
    FillRect(0, 0, width * 2 / 5, height * 4 / 7, SDL_Color{45, 50, 110, 255});
    for (int row = 0; row < 2; ++row)
      for (int column = 0; column < 3; ++column)
        FillRect(5 + column * (width * 2 / 5 - 8) / 3, 4 + row * 8, 2, 2,
                 SDL_Color{255, 255, 255, 255});
  }
  else
  {
    FillRect(0, 0, width, height, SDL_Color{0, 51, 153, 255});
    for (int star = 0; star < 12; ++star)
    {
      const double angle = star * 6.28318 / 12.0;
      const int x = width / 2 + static_cast<int>(std::cos(angle) * width * 0.30);
      const int y = height / 2 + static_cast<int>(std::sin(angle) * height * 0.32);
      FillRect(x - 1, y - 1, 2, 2, SDL_Color{255, 204, 0, 255});
    }
  }
  SDL_SetRenderTarget(m_renderer, previous_target);
  SDL_RenderSetScale(m_renderer, previous_scale_x, previous_scale_y);
  return texture;
}

void Launcher::InitializeUiTextures()
{
  m_glyphs[0] = MakeGlyph("A", false);
  m_glyphs[1] = MakeGlyph("B", false);
  m_glyphs[2] = MakeGlyph("X", false);
  m_glyphs[3] = MakeGlyph("Y", false);
  m_glyphs[4] = MakeGlyph("+", false);
  m_glyphs[5] = MakeGlyph("L", true);
  m_glyphs[6] = MakeGlyph("R", true);
  m_glyphs[7] = MakeGlyph("-", false);
  m_glyphs[8] = MakeGlyph("<", false);
  m_glyphs[9] = MakeGlyph(">", false);
  m_flags[1] = MakeFlagTexture(DiscIO::Region::NTSC_U, 36, 24);
  m_flags[2] = MakeFlagTexture(DiscIO::Region::PAL, 36, 24);
  m_flags[3] = MakeFlagTexture(DiscIO::Region::NTSC_J, 36, 24);
}

void Launcher::DestroyUiTextures()
{
  for (SDL_Texture*& texture : m_glyphs)
  {
    if (texture)
      SDL_DestroyTexture(texture);
    texture = nullptr;
  }
  for (SDL_Texture*& texture : m_flags)
  {
    if (texture)
      SDL_DestroyTexture(texture);
    texture = nullptr;
  }
}

void Launcher::EnsureGlowTexture()
{
  if (m_glow || !m_renderer)
    return;
  constexpr int size = 256;
  SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, size, size, 32, SDL_PIXELFORMAT_RGBA32);
  if (!surface)
    return;
  if (SDL_LockSurface(surface) == 0)
  {
    for (int y = 0; y < size; ++y)
    {
      auto* row =
          reinterpret_cast<Uint32*>(static_cast<Uint8*>(surface->pixels) + y * surface->pitch);
      for (int x = 0; x < size; ++x)
      {
        const float dx = (x - (size - 1) * 0.5f) / (size * 0.5f);
        const float dy = (y - (size - 1) * 0.5f) / (size * 0.5f);
        const float distance = std::sqrt(dx * dx + dy * dy);
        const float strength = distance >= 1.0f ? 0.0f : 1.0f - distance;
        row[x] = SDL_MapRGBA(surface->format, 255, 255, 255,
                             static_cast<Uint8>(255.0f * strength * strength));
      }
    }
    SDL_UnlockSurface(surface);
    m_glow = SDL_CreateTextureFromSurface(m_renderer, surface);
    if (m_glow)
      SDL_SetTextureBlendMode(m_glow, SDL_BLENDMODE_BLEND);
  }
  SDL_FreeSurface(surface);
}

void Launcher::DrawBubbles(float time)
{
  const SDL_Color top{70, 198, 229, 255};
  const SDL_Color middle{15, 147, 193, 255};
  const SDL_Color bottom{3, 82, 120, 255};
  const auto blend = [](Uint8 first, Uint8 second, float amount) {
    return static_cast<Uint8>(first + (second - first) * std::clamp(amount, 0.0f, 1.0f));
  };
  constexpr int bands = 56;
  for (int band = 0; band < bands; ++band)
  {
    const float y = (band + 0.5f) / bands;
    SDL_Color color{};
    if (y < 0.58f)
    {
      const float amount = y / 0.58f;
      color = {blend(top.r, middle.r, amount), blend(top.g, middle.g, amount),
               blend(top.b, middle.b, amount), 255};
    }
    else
    {
      const float amount = (y - 0.58f) / 0.42f;
      color = {blend(middle.r, bottom.r, amount), blend(middle.g, bottom.g, amount),
               blend(middle.b, bottom.b, amount), 255};
    }
    const int y0 = band * m_height / bands;
    const int y1 = (band + 1) * m_height / bands;
    FillRect(0, y0, m_width, y1 - y0, color);
  }

  EnsureGlowTexture();
  if (m_glow)
  {
    SDL_SetTextureColorMod(m_glow, 197, 244, 255);
    SDL_SetTextureAlphaMod(m_glow, 96);
    SDL_Rect surface{-m_width / 6, -m_height / 3, m_width * 4 / 3, m_height * 2 / 3};
    SDL_RenderCopy(m_renderer, m_glow, nullptr, &surface);
    for (int ray = 0; ray < 7; ++ray)
    {
      const float sway = std::sin(time * (0.10f + ray * 0.013f) + ray * 1.31f);
      const int width = m_width * (11 + (ray % 3) * 3) / 100;
      const int x =
          m_width * (8 + ray * 14) / 100 + static_cast<int>(sway * m_width * 0.025f) - width / 2;
      SDL_Rect shaft{x, -m_height / 3, width, m_height * 4 / 3};
      SDL_SetTextureAlphaMod(m_glow, static_cast<Uint8>(23 + (ray % 3) * 7));
      SDL_RenderCopyEx(m_renderer, m_glow, nullptr, &shaft, -9.0 + ray * 2.7 + sway * 2.0, nullptr,
                       SDL_FLIP_NONE);
    }
  }

  const auto draw_bubble = [&](int center_x, int center_y, int radius, Uint8 alpha) {
    if (radius < 3 || alpha == 0)
      return;
    if (m_glow)
    {
      SDL_SetTextureColorMod(m_glow, 180, 237, 255);
      SDL_SetTextureAlphaMod(m_glow, static_cast<Uint8>(alpha / 5));
      SDL_Rect glow{center_x - radius * 2, center_y - radius * 2, radius * 4, radius * 4};
      SDL_RenderCopy(m_renderer, m_glow, nullptr, &glow);
    }
    constexpr int segments = 24;
    std::array<SDL_Point, segments + 1> outer{};
    std::array<SDL_Point, segments + 1> inner{};
    for (int segment = 0; segment <= segments; ++segment)
    {
      const float angle = segment * 6.2831853f / segments;
      const float x = std::cos(angle);
      const float y = std::sin(angle);
      outer[segment] = {center_x + static_cast<int>(x * radius),
                        center_y + static_cast<int>(y * radius)};
      inner[segment] = {center_x + static_cast<int>(x * (radius - 1)),
                        center_y + static_cast<int>(y * (radius - 1))};
    }
    SDL_SetRenderDrawColor(m_renderer, 188, 240, 255, alpha);
    SDL_RenderDrawLines(m_renderer, outer.data(), outer.size());
    SDL_RenderDrawLines(m_renderer, inner.data(), inner.size());
    SDL_SetRenderDrawColor(m_renderer, 235, 252, 255,
                           static_cast<Uint8>(std::min(255, static_cast<int>(alpha) + 55)));
    std::array<SDL_Point, 6> highlight{};
    for (int segment = 0; segment < static_cast<int>(highlight.size()); ++segment)
    {
      const float angle = 3.55f + segment * 0.13f;
      highlight[segment] = {center_x + static_cast<int>(std::cos(angle) * radius),
                            center_y + static_cast<int>(std::sin(angle) * radius)};
    }
    SDL_RenderDrawLines(m_renderer, highlight.data(), highlight.size());
  };

  for (int index = 0; index < 18; ++index)
  {
    const float progress =
        std::fmod(index * 0.173f + time * (0.038f + (index % 5) * 0.007f), 1.18f);
    const float y = 1.08f - progress;
    const float x = 0.05f + std::fmod(index * 0.283f, 0.90f) +
                    0.032f * std::sin(time * (0.31f + (index % 4) * 0.04f) + index);
    const float fade = std::min(std::clamp((1.10f - y) * 5.0f, 0.0f, 1.0f),
                                std::clamp((y + 0.12f) * 6.0f, 0.0f, 1.0f));
    int radius = static_cast<int>(m_height * (0.009f + (index % 6) * 0.0042f));
    if (index % 11 == 0)
      radius = radius * 3 / 2;
    draw_bubble(static_cast<int>(x * m_width), static_cast<int>(y * m_height), radius,
                static_cast<Uint8>(fade * (85 + (index % 4) * 24)));
  }
  for (int index = 0; index < 24; ++index)
  {
    const float travel =
        std::fmod(index * 0.371f + time * 0.008f * (0.65f + (index % 5) * 0.11f), 1.12f) - 0.06f;
    const float y =
        std::fmod(index * 0.217f + 0.11f * std::sin(time * 0.29f + index * 1.73f), 1.0f);
    const float pulse = 0.45f + 0.55f * std::sin(time * (0.9f + (index % 4) * 0.17f) + index);
    const Uint8 alpha = static_cast<Uint8>(62 * (0.55f + 0.45f * pulse));
    const int size = index % 9 == 0 ? 3 : 2;
    FillRect(static_cast<int>(travel * m_width), static_cast<int>(y * m_height), size, size,
             SDL_Color{216, 246, 255, alpha});
  }
  if (m_glow)
  {
    SDL_SetTextureColorMod(m_glow, 255, 255, 255);
    SDL_SetTextureAlphaMod(m_glow, 255);
  }
}

void Launcher::DrawXmbRibbon(float time, float center, float amplitude, float frequency,
                             float slope, float phase, int half_width, SDL_Color color)
{
  constexpr int point_count = 121;
  std::array<SDL_Point, point_count> base{};
  std::array<SDL_Point, point_count> points{};
  const auto wave_y = [&](float x) {
    const float primary = std::sin(x * 6.2831853f * frequency + phase + time * 0.115f);
    const float detail =
        std::sin(x * 6.2831853f * (frequency * 2.07f) + phase * 0.61f - time * 0.072f);
    return center + slope * (x - 0.5f) + amplitude * (primary + detail * 0.24f);
  };
  for (int point = 0; point < point_count; ++point)
  {
    const float x = static_cast<float>(point) / (point_count - 1);
    base[point] = {static_cast<int>(x * m_width), static_cast<int>(wave_y(x) * m_height)};
  }
  for (int offset = -half_width; offset <= half_width; ++offset)
  {
    const float distance = half_width ? std::abs(static_cast<float>(offset) / half_width) : 0.0f;
    const Uint8 alpha =
        static_cast<Uint8>(color.a * std::pow(std::max(0.0f, 1.0f - distance), 1.45f));
    if (alpha < 2)
      continue;
    for (int point = 0; point < point_count; ++point)
      points[point] = {base[point].x, base[point].y + offset};
    SDL_SetRenderDrawColor(m_renderer, color.r, color.g, color.b, alpha);
    SDL_RenderDrawLines(m_renderer, points.data(), points.size());
  }
}

void Launcher::DrawXmbFilament(float time, float center, float amplitude, float frequency,
                               float slope, float phase, SDL_Color color)
{
  constexpr int point_count = 161;
  std::array<SDL_Point, point_count> points{};
  for (int point = 0; point < point_count; ++point)
  {
    const float x = static_cast<float>(point) / (point_count - 1);
    const float primary = std::sin(x * 6.2831853f * frequency + phase + time * 0.115f);
    const float detail =
        std::sin(x * 6.2831853f * (frequency * 2.07f) + phase * 0.61f - time * 0.072f);
    const float y = center + slope * (x - 0.5f) + amplitude * (primary + detail * 0.24f);
    points[point] = {static_cast<int>(x * m_width), static_cast<int>(y * m_height)};
  }
  SDL_SetRenderDrawColor(m_renderer, color.r, color.g, color.b, color.a);
  SDL_RenderDrawLines(m_renderer, points.data(), points.size());
}

void Launcher::DrawXmbSparkles(float time)
{
  for (int index = 0; index < 42; ++index)
  {
    const float x =
        std::fmod(index * 0.618034f + time * (0.0022f + (index % 5) * 0.00045f), 1.08f) - 0.04f;
    const float primary = std::sin(x * 6.2831853f * 0.91f + 0.4f + time * 0.115f);
    const float detail = std::sin(x * 6.2831853f * (0.91f * 2.07f) + 0.4f * 0.61f - time * 0.072f);
    const float y = 0.585f + 0.075f * (x - 0.5f) + 0.095f * (primary + detail * 0.24f) +
                    (std::fmod(index * 0.413f, 1.0f) - 0.5f) * 0.31f;
    const float pulse =
        0.5f + 0.5f * std::sin(time * (0.55f + (index % 7) * 0.08f) + index * 1.731f);
    const Uint8 alpha = static_cast<Uint8>(28.0f + pulse * (index % 9 == 0 ? 142.0f : 82.0f));
    const int px = static_cast<int>(x * m_width);
    const int py = static_cast<int>(y * m_height);
    const int size = index % 9 == 0 ? 3 : 2;
    FillRect(px, py, size, size, SDL_Color{220, 246, 255, alpha});
    if (index % 9 == 0 && pulse > 0.55f)
    {
      SDL_SetRenderDrawColor(m_renderer, 235, 251, 255, static_cast<Uint8>(alpha * 0.62f));
      SDL_RenderDrawLine(m_renderer, px - 5, py + 1, px + 7, py + 1);
      SDL_RenderDrawLine(m_renderer, px + 1, py - 5, px + 1, py + 7);
    }
  }
}

void Launcher::DrawXmb(float time)
{
  const SDL_Color top{8, 51, 104, 255};
  const SDL_Color middle{12, 82, 139, 255};
  const SDL_Color bottom{6, 39, 82, 255};
  const auto blend = [](Uint8 first, Uint8 second, float amount) {
    return static_cast<Uint8>(first + (second - first) * std::clamp(amount, 0.0f, 1.0f));
  };
  constexpr int bands = 72;
  for (int band = 0; band < bands; ++band)
  {
    const float y = (band + 0.5f) / bands;
    SDL_Color color{};
    if (y < 0.52f)
    {
      const float amount = y / 0.52f;
      color = {blend(top.r, middle.r, amount), blend(top.g, middle.g, amount),
               blend(top.b, middle.b, amount), 255};
    }
    else
    {
      const float amount = (y - 0.52f) / 0.48f;
      color = {blend(middle.r, bottom.r, amount), blend(middle.g, bottom.g, amount),
               blend(middle.b, bottom.b, amount), 255};
    }
    const int y0 = band * m_height / bands;
    const int y1 = (band + 1) * m_height / bands;
    FillRect(0, y0, m_width, y1 - y0, color);
  }

  EnsureGlowTexture();
  if (m_glow)
  {
    const auto glow = [&](float x, float y, float radius, Uint8 red, Uint8 green, Uint8 blue,
                          Uint8 alpha) {
      const int diameter = static_cast<int>(m_height * radius);
      SDL_Rect destination{static_cast<int>(m_width * x) - diameter / 2,
                           static_cast<int>(m_height * y) - diameter / 2, diameter, diameter};
      SDL_SetTextureColorMod(m_glow, red, green, blue);
      SDL_SetTextureAlphaMod(m_glow, alpha);
      SDL_RenderCopy(m_renderer, m_glow, nullptr, &destination);
    };
    glow(0.10f, 0.43f, 1.18f, 55, 157, 255, 32);
    glow(0.84f, 0.38f, 0.92f, 41, 112, 228, 24);
  }
  DrawXmbRibbon(time, 0.655f, 0.082f, 0.78f, -0.105f, 2.15f, std::max(12, m_height / 18),
                SDL_Color{63, 166, 255, 31});
  DrawXmbRibbon(time, 0.575f, 0.074f, 0.96f, 0.080f, 0.35f, std::max(10, m_height / 25),
                SDL_Color{189, 235, 255, 26});
  DrawXmbRibbon(time, 0.605f, 0.049f, 1.28f, -0.025f, 3.82f, std::max(5, m_height / 54),
                SDL_Color{230, 250, 255, 36});
  for (int trace = 0; trace < 9; ++trace)
  {
    const float offset = (trace - 4) * 0.009f;
    DrawXmbFilament(time, 0.588f + offset, 0.083f + trace * 0.0017f, 0.91f, 0.052f,
                    0.62f + trace * 0.19f,
                    SDL_Color{202, 241, 255, static_cast<Uint8>(18 + trace % 3 * 8)});
  }
  DrawXmbFilament(time, 0.578f, 0.073f, 0.96f, 0.080f, 0.35f, SDL_Color{243, 253, 255, 136});
  DrawXmbSparkles(time);
  if (m_glow)
  {
    SDL_SetTextureColorMod(m_glow, 255, 255, 255);
    SDL_SetTextureAlphaMod(m_glow, 255);
  }
}

void Launcher::ClearBackground()
{
  // Footer hit rects only describe the frame that drew them.
  m_footer_hit_count = 0;
  SDL_RenderSetClipRect(m_renderer, nullptr);
  SDL_SetRenderDrawColor(m_renderer, m_background.r, m_background.g, m_background.b, 255);
  SDL_RenderClear(m_renderer);
  const float time = m_animations ? SDL_GetTicks() / 1000.0f : 0.0f;
  if (m_theme == Theme::Xmb)
  {
    DrawXmb(time);
    return;
  }
  if (m_theme == Theme::Bubbles)
  {
    DrawBubbles(time);
    return;
  }
  if (m_theme != Theme::Glow)
    return;
  EnsureGlowTexture();
  if (!m_glow)
    return;
  struct Glow
  {
    float x, y, radius;
    Uint8 r, g, b, a;
  };
  const std::array<Glow, 4> glows = {
      {{0.10f + 0.13f * std::sin(time * 0.43f), 0.20f + 0.11f * std::cos(time * 0.37f), 0.90f, 45,
        140, 255, 128},
       {0.84f + 0.12f * std::cos(time * 0.34f), 0.34f + 0.10f * std::sin(time * 0.41f), 0.78f, 154,
        75, 255, 112},
       {0.54f + 0.10f * std::sin(time * 0.29f), 0.91f + 0.06f * std::cos(time * 0.33f), 0.94f, 0,
        210, 190, 94},
       {0.42f + 0.08f * std::cos(time * 0.25f), 0.48f + 0.09f * std::sin(time * 0.31f), 0.58f, 64,
        125, 255, 67}}};
  for (const Glow& glow : glows)
  {
    const int diameter = static_cast<int>(m_height * glow.radius);
    SDL_Rect destination{static_cast<int>(m_width * glow.x) - diameter / 2,
                         static_cast<int>(m_height * glow.y) - diameter / 2, diameter, diameter};
    SDL_SetTextureColorMod(m_glow, glow.r, glow.g, glow.b);
    SDL_SetTextureAlphaMod(m_glow, glow.a);
    SDL_RenderCopy(m_renderer, m_glow, nullptr, &destination);
  }
  for (int index = 0; index < 28; ++index)
  {
    const float travel =
        std::fmod(index * 0.371f + time * 0.011f * (0.65f + (index % 5) * 0.11f), 1.12f) - 0.06f;
    const float y =
        std::fmod(index * 0.217f + 0.11f * std::sin(time * 0.29f + index * 1.73f), 1.0f);
    const float pulse = 0.45f + 0.55f * std::sin(time * (0.9f + (index % 4) * 0.17f) + index);
    FillRect(static_cast<int>(travel * m_width), static_cast<int>(y * m_height),
             index % 9 == 0 ? 3 : 2, index % 9 == 0 ? 3 : 2,
             SDL_Color{182, 224, 255, static_cast<Uint8>(88 * (0.55f + 0.45f * pulse))});
  }
  SDL_SetTextureColorMod(m_glow, 255, 255, 255);
  SDL_SetTextureAlphaMod(m_glow, 255);
}

int Launcher::TextWidth(TTF_Font* font, std::string_view text)
{
  if (!font || text.empty())
    return 0;
  MetricKey key{font, std::string(text)};
  const auto found = m_metric_cache.find(key);
  if (found != m_metric_cache.end())
  {
    found->second.use = ++m_text_use;
    return found->second.width;
  }
  int width = 0;
  int height = 0;
  if (TTF_SizeUTF8(font, key.text.c_str(), &width, &height) != 0)
    return 0;
  // The metric cache stores logical widths so centring and ellipsis maths stay in layout space.
  width = FontMetric(width);
  if (m_metric_cache.size() >= METRIC_CACHE_LIMIT)
  {
    auto victim = m_metric_cache.begin();
    for (auto iterator = std::next(m_metric_cache.begin()); iterator != m_metric_cache.end();
         ++iterator)
    {
      if (iterator->second.use < victim->second.use)
        victim = iterator;
    }
    m_metric_cache.erase(victim);
  }
  m_metric_cache.emplace(std::move(key), MetricEntry{width, ++m_text_use});
  return width;
}

void Launcher::DrawText(TTF_Font* font, int x, int y, std::string_view text, SDL_Color color)
{
  if (!font || text.empty())
    return;
  const Uint32 packed = color.r | (static_cast<Uint32>(color.g) << 8) |
                        (static_cast<Uint32>(color.b) << 16) | (static_cast<Uint32>(color.a) << 24);
  TextKey key{font, packed, std::string(text)};
  auto found = m_text_cache.find(key);
  if (found != m_text_cache.end())
  {
    found->second.use = ++m_text_use;
    const SDL_FRect destination{static_cast<float>(x), static_cast<float>(y), found->second.width,
                                found->second.height};
    SDL_RenderCopyF(m_renderer, found->second.texture, nullptr, &destination);
    return;
  }
  SDL_Surface* surface = TTF_RenderUTF8_Blended(font, key.text.c_str(), color);
  if (!surface)
    return;
  SDL_Texture* texture = SDL_CreateTextureFromSurface(m_renderer, surface);
  // The texture keeps the output resolution; the destination rectangle is in logical units.
  const int pixel_width = surface->w;
  const int pixel_height = surface->h;
  const float width = pixel_width / m_font_scale;
  const float height = pixel_height / m_font_scale;
  SDL_FreeSurface(surface);
  if (!texture)
    return;
  const int logical_width = FontMetric(pixel_width);
  MetricKey metric_key{font, key.text};
  const auto metric = m_metric_cache.find(metric_key);
  if (metric != m_metric_cache.end())
  {
    metric->second.width = logical_width;
    metric->second.use = ++m_text_use;
  }
  else
  {
    if (m_metric_cache.size() >= METRIC_CACHE_LIMIT)
    {
      auto victim = m_metric_cache.begin();
      for (auto iterator = std::next(m_metric_cache.begin()); iterator != m_metric_cache.end();
           ++iterator)
      {
        if (iterator->second.use < victim->second.use)
          victim = iterator;
      }
      m_metric_cache.erase(victim);
    }
    m_metric_cache.emplace(std::move(metric_key), MetricEntry{logical_width, ++m_text_use});
  }
  const std::size_t bytes =
      static_cast<std::size_t>(pixel_width) * static_cast<std::size_t>(pixel_height) * 4;
  if (bytes > TEXT_CACHE_BYTES)
  {
    const SDL_FRect destination{static_cast<float>(x), static_cast<float>(y), width, height};
    SDL_RenderCopyF(m_renderer, texture, nullptr, &destination);
    SDL_DestroyTexture(texture);
    return;
  }
  while (!m_text_cache.empty() &&
         (m_text_cache.size() >= TEXT_CACHE_LIMIT || m_text_cache_bytes > TEXT_CACHE_BYTES - bytes))
  {
    auto victim = m_text_cache.begin();
    for (auto iterator = std::next(m_text_cache.begin()); iterator != m_text_cache.end();
         ++iterator)
    {
      if (iterator->second.use < victim->second.use)
        victim = iterator;
    }
    SDL_DestroyTexture(victim->second.texture);
    m_text_cache_bytes -= victim->second.bytes;
    m_text_cache.erase(victim);
  }
  auto [iterator, inserted] = m_text_cache.emplace(
      std::move(key), TextTexture{texture, width, height, bytes, ++m_text_use});
  m_text_cache_bytes += bytes;
  const SDL_FRect destination{static_cast<float>(x), static_cast<float>(y), width, height};
  SDL_RenderCopyF(m_renderer, iterator->second.texture, nullptr, &destination);
}

void Launcher::DrawTextCentered(TTF_Font* font, int center_x, int y, std::string_view text,
                                SDL_Color color)
{
  DrawText(font, center_x - TextWidth(font, text) / 2, y, text, color);
}

void Launcher::DrawTextRight(TTF_Font* font, int right_x, int y, std::string_view text,
                             SDL_Color color)
{
  DrawText(font, right_x - TextWidth(font, text), y, text, color);
}

std::string Launcher::Ellipsize(TTF_Font* font, std::string_view text, int max_width)
{
  if (!font || text.empty() || max_width <= 0)
    return {};
  EllipsisKey key{font, max_width, std::string(text)};
  const auto found = m_ellipsis_cache.find(key);
  if (found != m_ellipsis_cache.end())
  {
    found->second.use = ++m_text_use;
    return found->second.text;
  }

  std::string result;
  if (TextWidth(font, text) <= max_width)
  {
    result = text;
  }
  else
  {
    std::vector<std::size_t> boundaries{0};
    for (std::size_t index = 0; index < text.size();)
    {
      const unsigned char lead = static_cast<unsigned char>(text[index]);
      std::size_t length = lead < 0x80           ? 1 :
                           (lead & 0xe0) == 0xc0 ? 2 :
                           (lead & 0xf0) == 0xe0 ? 3 :
                           (lead & 0xf8) == 0xf0 ? 4 :
                                                   1;
      if (index + length > text.size())
        length = 1;
      for (std::size_t continuation = 1; continuation < length; ++continuation)
      {
        if ((static_cast<unsigned char>(text[index + continuation]) & 0xc0) != 0x80)
        {
          length = 1;
          break;
        }
      }
      index += length;
      boundaries.push_back(index);
    }
    std::size_t low = 0;
    std::size_t high = boundaries.size() - 1;
    while (low < high)
    {
      const std::size_t middle = (low + high + 1) / 2;
      const std::string candidate = key.text.substr(0, boundaries[middle]) + "...";
      if (TextWidth(font, candidate) <= max_width)
        low = middle;
      else
        high = middle - 1;
    }
    result = key.text.substr(0, boundaries[low]) + "...";
  }

  if (m_ellipsis_cache.size() >= ELLIPSIS_CACHE_LIMIT)
  {
    auto victim = m_ellipsis_cache.begin();
    for (auto iterator = std::next(m_ellipsis_cache.begin()); iterator != m_ellipsis_cache.end();
         ++iterator)
    {
      if (iterator->second.use < victim->second.use)
        victim = iterator;
    }
    m_ellipsis_cache.erase(victim);
  }
  m_ellipsis_cache.emplace(std::move(key), EllipsisEntry{result, ++m_text_use});
  return result;
}

void Launcher::DrawWrapped(TTF_Font* font, int x, int y, int max_width, int line_height,
                           int max_lines, std::string_view text, SDL_Color color)
{
  if (!font || text.empty() || max_width <= 0 || max_lines <= 0)
    return;
  std::string line;
  int drawn = 0;
  const auto emit = [&](const std::string& value) {
    if (drawn < max_lines)
      DrawText(font, x, y + drawn++ * line_height, value, color);
  };
  std::size_t index = 0;
  while (index < text.size() && drawn < max_lines)
  {
    std::size_t separator = index;
    while (separator < text.size() && text[separator] != ' ' && text[separator] != '\n')
      ++separator;
    const std::string word{text.substr(index, separator - index)};
    const std::string candidate = line.empty() ? word : line + " " + word;
    if (!line.empty() && TextWidth(font, candidate) > max_width)
    {
      emit(line);
      line = word;
    }
    else
    {
      line = candidate;
    }
    if (separator < text.size() && text[separator] == '\n')
    {
      emit(line);
      line.clear();
    }
    index = separator + 1;
  }
  if (drawn < max_lines && !line.empty())
    emit(line);
}

void Launcher::DrawWrappedCentered(TTF_Font* font, int center_x, int y, int max_width,
                                   int line_height, int max_lines, std::string_view text,
                                   SDL_Color color)
{
  const std::vector<std::string> lines = WrapText(font, max_width, max_lines, text);
  for (std::size_t line_index = 0; line_index < lines.size(); ++line_index)
  {
    DrawTextCentered(font, center_x, y + static_cast<int>(line_index) * line_height,
                     lines[line_index], color);
  }
}

std::vector<std::string> Launcher::WrapText(TTF_Font* font, int max_width, int max_lines,
                                            std::string_view text)
{
  std::vector<std::string> lines;
  if (!font || text.empty() || max_width <= 0 || max_lines <= 0)
    return lines;

  std::string line;
  bool truncated = false;
  const auto emit = [&] {
    if (line.empty())
      return;
    if (static_cast<int>(lines.size()) >= max_lines)
    {
      truncated = true;
      return;
    }
    lines.emplace_back(std::move(line));
    line.clear();
  };

  std::size_t index = 0;
  while (index < text.size())
  {
    while (index < text.size() && text[index] == ' ')
      ++index;
    if (index >= text.size())
      break;
    if (text[index] == '\n')
    {
      emit();
      ++index;
      continue;
    }

    std::size_t separator = index;
    while (separator < text.size() && text[separator] != ' ' && text[separator] != '\n')
      ++separator;
    std::string word{text.substr(index, separator - index)};
    if (TextWidth(font, word) > max_width)
      word = Ellipsize(font, word, max_width);
    const std::string candidate = line.empty() ? word : line + " " + word;
    if (!line.empty() && TextWidth(font, candidate) > max_width)
    {
      emit();
      if (static_cast<int>(lines.size()) >= max_lines)
      {
        truncated = true;
        break;
      }
      line = std::move(word);
    }
    else
    {
      line = candidate;
    }
    if (separator < text.size() && text[separator] == '\n')
      emit();
    index = separator + 1;
  }
  if (!line.empty())
    emit();
  if (truncated && !lines.empty())
    lines.back() = Ellipsize(font, lines.back() + " ...", max_width);
  return lines;
}

int Launcher::TextScrollOffset(int x, int y, int span, std::string_view text)
{
  m_frame_has_scrolling_text = true;
  if (m_text_scroll.size() > 64)
    m_text_scroll.clear();
  TextScroll& scroll = m_text_scroll[{x, y}];
  const Uint32 now = SDL_GetTicks();
  if (std::string_view(scroll.text) != text)
  {
    scroll.text = text;
    scroll.since = now;
  }
  constexpr float speed = 45.0f;
  constexpr float pause = 0.9f;
  const float travel = span / speed;
  const float phase = std::fmod((now - scroll.since) / 1000.0f, 2.0f * (travel + pause));
  if (phase < pause)
    return 0;
  if (phase < pause + travel)
    return static_cast<int>((phase - pause) * speed);
  if (phase < 2.0f * pause + travel)
    return span;
  return std::max(0, span - static_cast<int>((phase - 2.0f * pause - travel) * speed));
}

void Launcher::DrawScrollingTextLeft(TTF_Font* font, int x, int y, int max_width,
                                     std::string_view text, SDL_Color color)
{
  if (!font || text.empty() || max_width <= 0)
    return;
  const int width = TextWidth(font, text);
  if (width <= max_width)
  {
    DrawText(font, x, y, text, color);
    return;
  }
  SDL_Rect clip{x, y - 2, max_width, FontHeight(font) + 6};
  SDL_RenderSetClipRect(m_renderer, &clip);
  DrawText(font, x - TextScrollOffset(x, y, width - max_width, text), y, text, color);
  SDL_RenderSetClipRect(m_renderer, nullptr);
}

void Launcher::DrawScrollingTextRight(TTF_Font* font, int right_x, int y, int max_width,
                                      std::string_view text, SDL_Color color)
{
  if (!font || text.empty() || max_width <= 0)
    return;
  const int width = TextWidth(font, text);
  if (width <= max_width)
  {
    DrawTextRight(font, right_x, y, text, color);
    return;
  }
  const int x = right_x - max_width;
  SDL_Rect clip{x, y - 2, max_width, FontHeight(font) + 6};
  SDL_RenderSetClipRect(m_renderer, &clip);
  DrawText(font, x - TextScrollOffset(x, y, width - max_width, text), y, text, color);
  SDL_RenderSetClipRect(m_renderer, nullptr);
}

void Launcher::DrawTitleCell(int center_x, int width, int y, const Game& game, bool selected,
                             SDL_Color color)
{
  const int text_width = TextWidth(m_font_small, game.title);
  if (text_width <= width)
  {
    DrawTextCentered(m_font_small, center_x, y, game.title, color);
    return;
  }
  if (!selected)
  {
    DrawTextCentered(m_font_small, center_x, y, Ellipsize(m_font_small, game.title, width), color);
    return;
  }
  DrawScrollingTextLeft(m_font_small, center_x - width / 2, y, width, game.title, color);
}

int Launcher::TopBarHeight() const
{
  return 80;
}

void Launcher::DrawPageHeader(std::string_view title, std::string_view eyebrow,
                              std::string_view summary, std::string_view detail)
{
  const int height = TopBarHeight();
  constexpr int margin = 32;
  constexpr int logo_size = 48;
  constexpr int left = margin + logo_size + 20;
  FillRect(0, 0, m_width, height, m_panel);
  FillRect(0, height - 1, m_width, 1, SDL_Color{255, 255, 255, 18});
  if (m_logo)
  {
    SDL_Rect destination{margin, 16, logo_size, logo_size};
    SDL_RenderCopy(m_renderer, m_logo, nullptr, &destination);
  }
  const int metadata_width = std::min(
      m_width / 3, std::max(TextWidth(m_font_small, summary), TextWidth(m_font_small, detail)));
  const int title_width = m_width - left - margin - (metadata_width ? metadata_width + 32 : 0);
  DrawText(m_font_caption, left, 9, Ellipsize(m_font_caption, eyebrow, title_width), m_dim);
  DrawScrollingTextLeft(m_font_large, left, 31, title_width, title, m_text);
  if (!summary.empty())
  {
    DrawTextRight(m_font_small, m_width - margin, 16,
                  Ellipsize(m_font_small, summary, metadata_width), m_text);
  }
  if (!detail.empty())
  {
    DrawTextRight(m_font_small, m_width - margin, 44,
                  Ellipsize(m_font_small, detail, metadata_width), m_dim);
  }
}

void Launcher::DrawSectionHeading(std::string_view title, int x, int y, int width)
{
  const std::string_view localized = m_localization.Translate(title);
  const std::string shown = Ellipsize(m_font_small, localized, width - 24);
  const int height = FontHeight(m_font_small);
  const int text_width = TextWidth(m_font_small, shown);
  FillRect(x, y + 4, 3, height - 8, m_selection);
  DrawText(m_font_small, x + 14, y, shown, m_highlight);
  FillRect(x + text_width + 30, y + height / 2, width - text_width - 30, 1,
           SDL_Color{255, 255, 255, 28});
}

void Launcher::DrawHeader(std::string_view title, std::string_view context)
{
  // Header titles are launcher-owned UI.  Context strings are deliberately left raw because
  // they frequently contain game names, paths, profile names, or remote share names.
  title = m_localization.Translate(title);
  DrawPageHeader(title, "YabaSanshiro NX", context);
}

int Launcher::SettingsRowHeight() const
{
  return 44;
}

int Launcher::SettingsListY() const
{
  return TopBarHeight() + 24;
}

int Launcher::SettingsFooterReserve() const
{
  return 80;
}

void Launcher::DrawSettingsRowText(std::string_view label, std::string_view value, int slot_y,
                                   int column_width, int label_x, int value_x, bool current,
                                   SDL_Color label_color, SDL_Color value_color, bool scroll_value,
                                   int row_height)
{
  const int actual_row_height = row_height > 0 ? row_height : SettingsRowHeight();
  if (!current)
  {
    FillRect(label_x, slot_y + actual_row_height - 1, value_x - label_x, 1,
             SDL_Color{255, 255, 255, 10});
  }
  const bool submenu = value == ">";
  const int value_width =
      submenu ? 16 : std::min(TextWidth(m_font_small, value), column_width / 2 - 32);
  const int label_width = std::max(40, value_x - label_x - value_width - 24);
  const int y = slot_y + (actual_row_height - FontHeight(m_font)) / 2;
  if (current)
    DrawScrollingTextLeft(m_font, label_x, y, label_width, label, label_color);
  else
    DrawText(m_font, label_x, y, Ellipsize(m_font, label, label_width), label_color);
  if (submenu)
  {
    const int center_y = slot_y + actual_row_height / 2;
    SDL_SetRenderDrawColor(m_renderer, value_color.r, value_color.g, value_color.b, value_color.a);
    SDL_RenderDrawLine(m_renderer, value_x - 10, center_y - 6, value_x - 4, center_y);
    SDL_RenderDrawLine(m_renderer, value_x - 4, center_y, value_x - 10, center_y + 6);
    return;
  }
  const int value_y = slot_y + (actual_row_height - FontHeight(m_font_small)) / 2;
  if (scroll_value)
    DrawScrollingTextRight(m_font_small, value_x, value_y, column_width / 2 - 32, value,
                           value_color);
  else
    DrawTextRight(m_font_small, value_x, value_y,
                  Ellipsize(m_font_small, value, column_width / 2 - 32), value_color);
}

GameDetailLayout Launcher::ComputeGameDetailLayout() const
{
  const int top = TopBarHeight() + 24;
  const int bottom = m_height - SettingsFooterReserve() - 12;
  constexpr int width = 260;
  constexpr int height = width * 3 / 2;
  return {SDL_Rect{56, top + (bottom - top - height) / 2, width, height},
          SDL_Rect{360, top, m_width - 416, bottom - top}};
}

void Launcher::DrawArtworkPreview(SDL_Texture* texture, const SDL_Rect& rect, bool selected,
                                  std::string_view placeholder)
{
  if (selected)
    RoundedPanel(rect.x - 10, rect.y - 10, rect.w + 20, rect.h + 20, m_panel, m_selection);
  else
    GlassPanel(rect.x - 10, rect.y - 10, rect.w + 20, rect.h + 20);
  FillRect(rect.x, rect.y, rect.w, rect.h, m_card);
  if (texture)
  {
    SDL_SetTextureAlphaMod(texture, 255);
    SDL_SetTextureColorMod(texture, 255, 255, 255);
    SDL_RenderCopy(m_renderer, texture, nullptr, &rect);
  }
  else if (!placeholder.empty())
  {
    // An empty placeholder means the caller draws its own message.
    DrawTextCentered(m_font_small, rect.x + rect.w / 2,
                     rect.y + (rect.h - FontHeight(m_font_small)) / 2,
                     m_localization.Translate(placeholder), m_dim);
  }
}

void Launcher::DrawGamePreview(Game* game, const SDL_Rect& rect)
{
  m_cover_decode_budget = 1;
  EnsureCover(game);
  DrawArtworkPreview(game ? game->cover : nullptr, rect);
}

void Launcher::DrawButtonHint(int x, int y, std::string_view button, std::string_view label,
                              SDL_Color label_color)
{
  // Footer labels are launcher-owned UI; the button token itself is a controller glyph.
  label = m_localization.Translate(label);
  SDL_Texture* const glyph = ButtonGlyph(button);
  int width = 0;
  int height = 0;
  if (glyph)
  {
    SDL_QueryTexture(glyph, nullptr, nullptr, &width, &height);
    width /= GLYPH_SUPERSAMPLE;
    height /= GLYPH_SUPERSAMPLE;
    SDL_Rect destination{x, y - height / 2, width, height};
    SDL_RenderCopy(m_renderer, glyph, nullptr, &destination);
  }
  else
  {
    width = TextWidth(m_font_small, button) + 14;
    height = FontHeight(m_font_small) + 6;
    Border(x, y - height / 2, width, height, 1, m_dim);
    DrawTextCentered(m_font_small, x + width / 2, y - FontHeight(m_font_small) / 2, button,
                     m_text);
  }
  if (!label.empty())
    DrawText(m_font_small, x + width + 8, y - FontHeight(m_font_small) / 2, label, label_color);
}

SDL_Texture* Launcher::ButtonGlyph(std::string_view button) const
{
  if (button == "A")
    return m_glyphs[0];
  if (button == "B")
    return m_glyphs[1];
  if (button == "X")
    return m_glyphs[2];
  if (button == "Y")
    return m_glyphs[3];
  if (button == "+")
    return m_glyphs[4];
  if (button == "L")
    return m_glyphs[5];
  if (button == "R")
    return m_glyphs[6];
  if (button == "-")
    return m_glyphs[7];
  if (button == "Left")
    return m_glyphs[8];
  if (button == "Right")
    return m_glyphs[9];
  return nullptr;
}

FooterLayout
Launcher::MeasureFooter(std::span<const std::pair<std::string_view, std::string_view>> hints)
{
  constexpr int glyph_gap = 16;
  constexpr int label_gap = 8;
  constexpr int pair_gap = 26;
  constexpr int margin = 40;
  FooterLayout layout;
  const int font_height = FontHeight(m_font_small);
  const int count = std::min(static_cast<int>(hints.size()),
                             static_cast<int>(layout.item_width.size()));
  for (int index = 0; index < count; ++index)
  {
    const std::string_view localized_label = m_localization.Translate(hints[index].second);
    SDL_Texture* const glyph = ButtonGlyph(hints[index].first);
    int width = 0;
    if (glyph)
      SDL_QueryTexture(glyph, nullptr, nullptr, &width, nullptr);
    width = glyph ? width / GLYPH_SUPERSAMPLE :
                    TextWidth(m_font_small, hints[index].first) + 14;
    if (!localized_label.empty())
      width += label_gap + TextWidth(m_font_small, localized_label);
    layout.item_width[index] = width;
    layout.gap_after[index] = localized_label.empty() ? glyph_gap : pair_gap;
  }

  // Wrap greedily so a long hint set (the library grid has eight) stays on screen.
  const int available = std::max(120, m_width - margin * 2);
  int index = 0;
  while (index < count && layout.row_count < static_cast<int>(layout.row_start.size()))
  {
    const int row = layout.row_count++;
    layout.row_start[row] = index;
    int width = layout.item_width[index];
    int next = index + 1;
    while (next < count &&
           width + layout.gap_after[next - 1] + layout.item_width[next] <= available)
    {
      width += layout.gap_after[next - 1] + layout.item_width[next];
      ++next;
    }
    layout.row_end[row] = next;
    layout.row_width[row] = width;
    index = next;
  }
  layout.row_spacing = font_height + 22;
  layout.height = layout.row_count < 1 ?
                      0 :
                      (layout.row_count - 1) * layout.row_spacing + font_height + 32;
  return layout;
}

void Launcher::DrawFooterBand(int height)
{
  if (height <= 0)
    return;
  FillRect(0, m_height - height, m_width, height, m_panel);
  FillRect(0, m_height - height, m_width, 1, SDL_Color{255, 255, 255, 18});
}

void Launcher::DrawFooter(std::span<const std::pair<std::string_view, std::string_view>> hints,
                          int center_y)
{
  const FooterLayout layout = MeasureFooter(hints);
  if (layout.row_count < 1)
  {
    m_footer_hit_count = 0;
    return;
  }
  const int font_height = FontHeight(m_font_small);
  const int y = center_y >= 0 ? center_y : m_height - 26;
  if (y >= m_height - 40)
  {
    // Screen-bottom footers get their own band; footers drawn inside a modal panel do not.
    DrawFooterBand(layout.height);
  }
  const int first_row_y = y - (layout.row_count - 1) * layout.row_spacing;
  m_footer_hit_count = 0;
  for (int row = 0; row < layout.row_count; ++row)
  {
    int x = (m_width - layout.row_width[row]) / 2;
    const int row_y = first_row_y + row * layout.row_spacing;
    for (int index = layout.row_start[row]; index < layout.row_end[row]; ++index)
    {
      const std::string_view button = hints[index].first;
      SDL_Texture* const glyph = ButtonGlyph(button);
      int glyph_height = font_height + 6;
      if (glyph)
      {
        SDL_QueryTexture(glyph, nullptr, nullptr, nullptr, &glyph_height);
        glyph_height /= GLYPH_SUPERSAMPLE;
      }
      // The first hint of a footer is the primary action, so it is not dimmed.
      DrawButtonHint(x, row_y, button, hints[index].second, index == 0 ? m_text : m_dim);
      if (m_footer_hit_count < static_cast<int>(m_footer_hits.size()))
      {
        const int hit_height = std::max(glyph_height, font_height);
        m_footer_hits[m_footer_hit_count] = {x - 4, row_y - hit_height / 2 - 8,
                                             layout.item_width[index] + 8, hit_height + 16};
        m_footer_buttons[m_footer_hit_count] =
            button == "A"     ? static_cast<int>(BUTTON_CONFIRM) :
            button == "B"     ? static_cast<int>(BUTTON_CANCEL) :
            button == "X"     ? static_cast<int>(BUTTON_SETTINGS) :
            button == "Y"     ? static_cast<int>(SDL_CONTROLLER_BUTTON_X) :
            button == "+"     ? static_cast<int>(SDL_CONTROLLER_BUTTON_START) :
            button == "-"     ? static_cast<int>(SDL_CONTROLLER_BUTTON_BACK) :
            button == "L"     ? static_cast<int>(SDL_CONTROLLER_BUTTON_LEFTSHOULDER) :
            button == "R"     ? static_cast<int>(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) :
            button == "Left"  ? static_cast<int>(SDL_CONTROLLER_BUTTON_DPAD_LEFT) :
            button == "Right" ? static_cast<int>(SDL_CONTROLLER_BUTTON_DPAD_RIGHT) :
                                -1;
        ++m_footer_hit_count;
      }
      x += layout.item_width[index] + layout.gap_after[index];
    }
  }
}

bool Launcher::PressFooterButton(int x, int y)
{
  // A tapped footer hint synthesises the controller press it advertises.
  for (int index = 0; index < m_footer_hit_count; ++index)
  {
    const SDL_Rect& hit = m_footer_hits[index];
    if (m_footer_buttons[index] < 0 || x < hit.x || x >= hit.x + hit.w || y < hit.y ||
        y >= hit.y + hit.h)
    {
      continue;
    }
    SDL_Event press{};
    press.type = SDL_CONTROLLERBUTTONDOWN;
    press.cbutton.button = static_cast<Uint8>(m_footer_buttons[index]);
    return SDL_PushEvent(&press) == 1;
  }
  return false;
}

void Launcher::DrawSettingsFooter(std::string_view text)
{
  std::vector<std::pair<std::string_view, std::string_view>> hints;
  std::vector<std::string_view> tokens;
  const auto is_separator = [](char character) { return character == ' '; };
  std::size_t cursor = 0;
  while (cursor < text.size())
  {
    while (cursor < text.size() && is_separator(text[cursor]))
      ++cursor;
    if (cursor >= text.size())
      break;

    const std::size_t token_start = cursor;
    std::size_t token_end = text.size();
    std::size_t separator_end = text.size();
    bool found_separator = false;
    for (std::size_t index = cursor + 1; index < text.size(); ++index)
    {
      if (is_separator(text[index - 1]) && is_separator(text[index]))
      {
        token_end = index - 1;
        separator_end = index;
        found_separator = true;
        while (separator_end < text.size() && is_separator(text[separator_end]))
          ++separator_end;
        break;
      }
    }
    const auto trim_token = [&](std::string_view value) {
      std::size_t left = 0;
      while (left < value.size() && is_separator(value[left]))
        ++left;
      std::size_t right = value.size();
      while (right > left && is_separator(value[right - 1]))
        --right;
      return std::string_view(value.data() + left, right - left);
    };
    tokens.emplace_back(trim_token(text.substr(token_start, token_end - token_start + 1)));
    cursor = found_separator ? separator_end : text.size();
  }
  for (std::size_t index = 0; index + 1 < tokens.size(); index += 2)
  {
    if (tokens[index] == "Left / Right")
    {
      hints.emplace_back("Left", std::string_view{});
      hints.emplace_back("Right", tokens[index + 1]);
    }
    else
    {
      hints.emplace_back(tokens[index], tokens[index + 1]);
    }
  }
  if (hints.empty())
  {
    m_footer_hit_count = 0;
    if (!text.empty())
    {
      DrawFooterBand(FontHeight(m_font_small) + 32);
      DrawTextCentered(m_font_small, m_width / 2, m_height - 38, text, m_dim);
    }
  }
  else
  {
    DrawFooter(hints);
  }
}

void Launcher::BeginScreenFx()
{
  m_screen_fx_start = SDL_GetTicks();
  m_highlight_y = -1.0f;
  m_text_scroll.clear();
  m_footer_hit_count = 0;
}

void Launcher::DrawFadeIn()
{
  if (!m_animations)
    return;
  constexpr int duration = 160;
  const int elapsed = static_cast<int>(SDL_GetTicks() - m_screen_fx_start);
  if (elapsed < duration)
    FillRect(0, 0, m_width, m_height,
             SDL_Color{0, 0, 0, static_cast<Uint8>(200 * (duration - elapsed) / duration)});
}

void Launcher::ShowInfoCard(std::string_view section, std::string_view title, std::string_view kind,
                            std::string_view description, std::string_view current,
                            std::string_view scope, bool localize_title, bool localize_current)
{
  const std::string localized_section = std::string(
      m_localization.Translate(section.empty() ? std::string_view{"Settings"} : section));
  const std::string_view effective_title = title.empty() ? std::string_view{"Setting info"} : title;
  const std::string localized_title = localize_title ?
                                          std::string(m_localization.Translate(effective_title)) :
                                          std::string(effective_title);
  const std::string localized_kind =
      std::string(m_localization.Translate(kind.empty() ? "Setting" : kind));
  const std::string localized_scope =
      scope.empty() ? std::string{} : std::string(m_localization.Translate(scope));
  const std::string localized_description = std::string(m_localization.Translate(description));
  BeginScreenFx();
  while (BeginFrame())
  {
    bool close = false;
    SDL_Event event{};
    while (PollEvent(&event))
    {
      int touch_x = 0;
      int touch_y = 0;
      const TouchKind touch = FeedTouch(event, &touch_x, &touch_y);
      if (touch == TouchKind::Tap)
        close = true;
      if (event.type == SDL_CONTROLLERBUTTONDOWN &&
          (event.cbutton.button == BUTTON_CONFIRM || event.cbutton.button == BUTTON_CANCEL ||
           event.cbutton.button == BUTTON_SETTINGS))
        close = true;
      if (event.type == SDL_KEYDOWN &&
          (event.key.keysym.sym == SDLK_RETURN || event.key.keysym.sym == SDLK_ESCAPE ||
           event.key.keysym.sym == SDLK_x))
        close = true;
    }
    if (close)
      return;

    ClearBackground();
    const int panel_width = std::min(m_width - 120, 1000);
    const int panel_height = std::min(m_height - 96, 500);
    const int panel_x = (m_width - panel_width) / 2;
    const int panel_y = (m_height - panel_height) / 2;
    GlassPanel(panel_x, panel_y, panel_width, panel_height);
    DrawText(m_font_small, panel_x + 40, panel_y + 24, localized_section, m_dim);
    DrawScrollingTextLeft(m_font_large, panel_x + 40, panel_y + 58, panel_width - 80,
                          localized_title, m_value);

    std::string metadata = localized_kind;
    if (!localized_scope.empty())
      metadata += "  |  " + localized_scope;
    DrawScrollingTextLeft(m_font_small, panel_x + 40, panel_y + 114, panel_width - 80, metadata,
                          m_selection);

    int body_y = panel_y + 164;
    if (!current.empty())
    {
      const std::string_view prefix = m_localization.Translate("Current: ");
      DrawText(m_font_small, panel_x + 40, panel_y + 146, prefix, m_dim);
      const std::string_view displayed_current =
          localize_current ? m_localization.Translate(current) : current;
      DrawScrollingTextLeft(m_font_small, panel_x + 40 + TextWidth(m_font_small, prefix),
                            panel_y + 146, panel_width - 80 - TextWidth(m_font_small, prefix),
                            displayed_current, m_text);
      body_y = panel_y + 198;
    }
    FillRect(panel_x + 40, body_y - 18, panel_width - 80, 2, SDL_Color{70, 78, 92, 210});
    const int maximum_lines = std::max(1, (panel_y + panel_height - 70 - body_y) / 32);
    DrawWrapped(m_font, panel_x + 40, body_y, panel_width - 80, 32, maximum_lines,
                localized_description, m_text);
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 1> close_hint = {
        std::pair{"A", "Close"}};
    DrawFooter(close_hint, panel_y + panel_height - 32);
    DrawFadeIn();
    SDL_RenderPresent(m_renderer);
    WaitForNextFrame();
  }
}

bool Launcher::BeginFrame()
{
  NxLauncherHeartbeat();
  if (!m_running || !appletMainLoop())
    return false;
  PumpCoverDecodeResults();
  m_frame_has_scrolling_text = false;
  if (m_controller && !SDL_GameControllerGetAttached(m_controller))
  {
    SDL_GameControllerClose(m_controller);
    m_controller = nullptr;
    m_stick_x_latched = m_stick_y_latched = false;
    m_navigation_held = 0;
    m_navigation_since = m_navigation_last = 0;
  }
  QueueNavigationRepeat();
  return true;
}

bool Launcher::PollEvent(SDL_Event* event)
{
  if (!event)
    return false;
  while (true)
  {
    if (!m_waited_events.empty())
    {
      *event = m_waited_events.front();
      m_waited_events.pop_front();
    }
    else if (!SDL_PollEvent(event))
    {
      return false;
    }
    if (event->type == SDL_QUIT)
    {
      m_running = false;
      continue;
    }
    if (event->type == SDL_WINDOWEVENT && (event->window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                                           event->window.event == SDL_WINDOWEVENT_RESIZED))
    {
      // Docking changes the output size: rescale, reload fonts and rebuild textures.
      int output_width = 0;
      int output_height = 0;
      SDL_GetRendererOutputSize(m_renderer, &output_width, &output_height);
      if (output_width > 0 && output_height > 0 &&
          (output_width != m_output_width || output_height != m_output_height))
      {
        m_output_width = output_width;
        m_output_height = output_height;
        ConfigureLauncherScale();
        if (m_font_scale != m_ui_scale && LoadFonts())
        {
          DestroyUiTextures();
          InitializeUiTextures();
        }
      }
      continue;
    }
    if (event->type == SDL_CONTROLLERDEVICEADDED)
    {
      if (!m_controller && SDL_IsGameController(event->cdevice.which))
        m_controller = SDL_GameControllerOpen(event->cdevice.which);
      continue;
    }
    if (event->type == SDL_CONTROLLERDEVICEREMOVED)
    {
      if (m_controller)
      {
        SDL_Joystick* joystick = SDL_GameControllerGetJoystick(m_controller);
        if (joystick && SDL_JoystickInstanceID(joystick) == event->cdevice.which)
        {
          SDL_GameControllerClose(m_controller);
          m_controller = nullptr;
          m_stick_x_latched = m_stick_y_latched = false;
          m_navigation_held = 0;
          m_navigation_since = m_navigation_last = 0;
        }
      }
      continue;
    }
    if (event->type == SDL_CONTROLLERAXISMOTION)
    {
      constexpr int threshold = 18000;
      constexpr int dead_zone = 8000;
      int direction = -1;
      if (event->caxis.axis == SDL_CONTROLLER_AXIS_LEFTX)
      {
        if (!m_stick_x_latched && event->caxis.value < -threshold)
        {
          m_stick_x_latched = true;
          direction = SDL_CONTROLLER_BUTTON_DPAD_LEFT;
        }
        else if (!m_stick_x_latched && event->caxis.value > threshold)
        {
          m_stick_x_latched = true;
          direction = SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
        }
        else if (event->caxis.value > -dead_zone && event->caxis.value < dead_zone)
        {
          m_stick_x_latched = false;
        }
      }
      else if (event->caxis.axis == SDL_CONTROLLER_AXIS_LEFTY)
      {
        if (!m_stick_y_latched && event->caxis.value < -threshold)
        {
          m_stick_y_latched = true;
          direction = SDL_CONTROLLER_BUTTON_DPAD_UP;
        }
        else if (!m_stick_y_latched && event->caxis.value > threshold)
        {
          m_stick_y_latched = true;
          direction = SDL_CONTROLLER_BUTTON_DPAD_DOWN;
        }
        else if (event->caxis.value > -dead_zone && event->caxis.value < dead_zone)
        {
          m_stick_y_latched = false;
        }
      }
      if (direction >= 0)
      {
        SDL_Event navigation{};
        navigation.type = SDL_CONTROLLERBUTTONDOWN;
        navigation.cbutton.button = static_cast<Uint8>(direction);
        SDL_PushEvent(&navigation);
      }
    }
    if (event->type == SDL_CONTROLLERBUTTONDOWN)
    {
      switch (event->cbutton.button)
      {
      case BUTTON_CONFIRM:
        PlayUiSound(UiSound::Confirm);
        break;
      case BUTTON_CANCEL:
        PlayUiSound(UiSound::Back);
        break;
      case SDL_CONTROLLER_BUTTON_DPAD_UP:
      case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
      case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
      case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
      case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
      case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
        PlayUiSound(UiSound::Navigate);
        break;
      default:
        break;
      }
    }
    switch (event->type)
    {
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP:
    case SDL_KEYDOWN:
    case SDL_KEYUP:
    case SDL_FINGERDOWN:
    case SDL_FINGERUP:
    case SDL_FINGERMOTION:
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
    case SDL_MOUSEMOTION:
    case SDL_MOUSEWHEEL:
      // Keep short selection, cover, and touch transitions smooth after an input wake-up.
      m_interaction_animation_until = SDL_GetTicks() + 220;
      break;
    default:
      break;
    }
    return true;
  }
  return false;
}

bool Launcher::FrameNeedsAnimation()
{
  const Uint32 now = SDL_GetTicks();
  if (m_animations && m_highlight_y >= 0.0f && m_last_frame_highlight_y >= 0.0f &&
      std::abs(m_highlight_y - m_last_frame_highlight_y) > 0.2f)
  {
    // The highlight uses an exponential approach, so allow a few quiet frames for the tail.
    m_interaction_animation_until = now + 96;
  }
  m_last_frame_highlight_y = m_highlight_y;

  const bool fade_active = m_animations && m_screen_fx_start != 0 && now - m_screen_fx_start < 160;
  const bool interaction_active =
      m_interaction_animation_until != 0 && !SDL_TICKS_PASSED(now, m_interaction_animation_until);
  return (m_animations && HasAnimatedBackground()) || fade_active || m_frame_has_scrolling_text ||
         interaction_active || m_navigation_held != 0 || m_touch.active;
}

void Launcher::WaitForNextFrame(bool force_animation)
{
  constexpr Uint32 animated_interval = 16;
  constexpr Uint32 lifecycle_poll_interval = 250;
  const bool animate = force_animation || FrameNeedsAnimation();
  if (animate && m_present_vsync)
  {
    // The vsynced present already paces the frame.
    m_frame_interval = 0;
    m_next_frame_deadline = 0;
    return;
  }
  Uint32 now = SDL_GetTicks();

  // Static screens do not have a frame rate.  Poll applet lifecycle and worker state without
  // returning to the caller (and therefore without rendering) until an event or real timer fires.
  // USB and launcher workers also post SDL user events; the generation/state checks are a safe
  // fallback for a lost or unavailable event bridge.
  if (!animate)
  {
    m_frame_interval = 0;
    m_next_frame_deadline = 0;
    for (;;)
    {
      now = SDL_GetTicks();
      int timeout = static_cast<int>(lifecycle_poll_interval);
      const auto include_deadline = [&](Uint32 deadline) {
        if (deadline == 0)
          return false;
        if (SDL_TICKS_PASSED(now, deadline))
          return true;
        timeout = std::min(timeout, static_cast<int>(deadline - now));
        return false;
      };
      if (include_deadline(m_usb_refresh_at))
        return;

      SDL_Event event{};
      if (SDL_WaitEventTimeout(&event, timeout))
      {
        m_waited_events.push_back(event);
        return;
      }
      if (!m_running || !appletMainLoop())
      {
        m_running = false;
        return;
      }
      if (Storage::UsbStatusGeneration() != m_usb_generation)
        return;
    }
  }

  const Uint32 interval = animated_interval;
  if (m_frame_interval != interval || m_next_frame_deadline == 0)
  {
    m_frame_interval = interval;
    m_next_frame_deadline = now + interval;
  }
  if (!SDL_TICKS_PASSED(now, m_next_frame_deadline))
  {
    SDL_Event event{};
    const int timeout = static_cast<int>(m_next_frame_deadline - now);
    if (SDL_WaitEventTimeout(&event, timeout))
    {
      // Waiting must not consume the input (or a worker wake-up) before the screen loop sees it.
      m_waited_events.push_back(event);
      m_next_frame_deadline = 0;
      return;
    }
    now = SDL_GetTicks();
    if (!SDL_TICKS_PASSED(now, m_next_frame_deadline))
      return;
  }

  const Uint32 following_deadline = m_next_frame_deadline + interval;
  m_next_frame_deadline =
      SDL_TICKS_PASSED(now, following_deadline) ? now + interval : following_deadline;
}

TouchKind Launcher::FeedTouch(const SDL_Event& event, int* x, int* y)
{
  constexpr int tap_move = 26;
  constexpr int swipe_distance = 90;
  constexpr int scroll_step = 30;
  constexpr Uint32 tap_time = 400;
  if (event.type == SDL_FINGERDOWN)
  {
    if (m_touch.active && SDL_GetTicks() - m_touch.started_at < 2000)
      return TouchKind::None;
    m_touch.active = true;
    m_touch.vertical = false;
    m_touch.finger = event.tfinger.fingerId;
    m_touch.start_x = event.tfinger.x * m_width;
    m_touch.start_y = event.tfinger.y * m_height;
    m_touch.last_y = m_touch.start_y;
    m_touch.started_at = SDL_GetTicks();
  }
  else if (event.type == SDL_FINGERMOTION && m_touch.active &&
           event.tfinger.fingerId == m_touch.finger)
  {
    const float current_x = event.tfinger.x * m_width;
    const float current_y = event.tfinger.y * m_height;
    const float dx = current_x - m_touch.start_x;
    const float dy = current_y - m_touch.start_y;
    if (!m_touch.vertical && std::abs(dy) > tap_move && std::abs(dy) > std::abs(dx) * 1.15f)
      m_touch.vertical = true;
    if (m_touch.vertical)
    {
      const float step = current_y - m_touch.last_y;
      if (std::abs(step) >= scroll_step)
      {
        m_touch_scroll_steps = std::clamp(static_cast<int>(std::abs(step) / scroll_step), 1, 6);
        m_touch.last_y = current_y;
        if (x)
          *x = static_cast<int>(current_x);
        if (y)
          *y = static_cast<int>(current_y);
        return step < 0 ? TouchKind::ScrollUp : TouchKind::ScrollDown;
      }
    }
  }
  else if (event.type == SDL_FINGERUP && m_touch.active && event.tfinger.fingerId == m_touch.finger)
  {
    m_touch.active = false;
    const float current_x = event.tfinger.x * m_width;
    const float current_y = event.tfinger.y * m_height;
    const float dx = current_x - m_touch.start_x;
    const float dy = current_y - m_touch.start_y;
    const Uint32 elapsed = SDL_GetTicks() - m_touch.started_at;
    if (x)
      *x = static_cast<int>(current_x);
    if (y)
      *y = static_cast<int>(current_y);
    if (m_touch.vertical || (std::abs(dy) >= 55 && std::abs(dy) > std::abs(dx) * 1.15f))
    {
      const float remaining = current_y - m_touch.last_y;
      if (std::abs(remaining) < 18 && m_touch.vertical)
        return TouchKind::None;
      const float distance = m_touch.vertical ? remaining : dy;
      m_touch_scroll_steps = std::clamp(static_cast<int>(std::abs(distance) / scroll_step), 1, 6);
      return distance < 0 ? TouchKind::ScrollUp : TouchKind::ScrollDown;
    }
    if (std::abs(dx) >= swipe_distance && std::abs(dx) > std::abs(dy) * 1.5f)
      return dx < 0 ? TouchKind::SwipeLeft : TouchKind::SwipeRight;
    if (std::abs(dx) <= tap_move && std::abs(dy) <= tap_move && elapsed <= tap_time)
    {
      return PressFooterButton(static_cast<int>(current_x), static_cast<int>(current_y)) ?
                 TouchKind::None :
                 TouchKind::Tap;
    }
  }
  return TouchKind::None;
}

bool Launcher::TouchScrollList(TouchKind kind, int* selection, int* top, int count, int visible)
{
  if (!selection || !top || count <= 0 ||
      (kind != TouchKind::ScrollUp && kind != TouchKind::ScrollDown))
    return false;
  const int previous = *selection;
  const int delta = (kind == TouchKind::ScrollUp ? 1 : -1) * m_touch_scroll_steps;
  *selection = std::clamp(*selection + delta, 0, count - 1);
  if (*selection < *top)
    *top = *selection;
  if (*selection >= *top + visible)
    *top = *selection - visible + 1;
  *top = std::max(0, *top);
  if (*selection != previous)
    PlayUiSound(UiSound::Navigate);
  return true;
}

void Launcher::QueueNavigationRepeat()
{
  if (!m_controller || !SDL_GameControllerGetAttached(m_controller))
    return;
  constexpr int threshold = 18000;
  int direction = 0;
  if (SDL_GameControllerGetButton(m_controller, SDL_CONTROLLER_BUTTON_DPAD_UP) ||
      SDL_GameControllerGetAxis(m_controller, SDL_CONTROLLER_AXIS_LEFTY) < -threshold)
    direction = SDL_CONTROLLER_BUTTON_DPAD_UP;
  else if (SDL_GameControllerGetButton(m_controller, SDL_CONTROLLER_BUTTON_DPAD_DOWN) ||
           SDL_GameControllerGetAxis(m_controller, SDL_CONTROLLER_AXIS_LEFTY) > threshold)
    direction = SDL_CONTROLLER_BUTTON_DPAD_DOWN;
  else if (SDL_GameControllerGetButton(m_controller, SDL_CONTROLLER_BUTTON_DPAD_LEFT) ||
           SDL_GameControllerGetAxis(m_controller, SDL_CONTROLLER_AXIS_LEFTX) < -threshold)
    direction = SDL_CONTROLLER_BUTTON_DPAD_LEFT;
  else if (SDL_GameControllerGetButton(m_controller, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) ||
           SDL_GameControllerGetAxis(m_controller, SDL_CONTROLLER_AXIS_LEFTX) > threshold)
    direction = SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
  const Uint32 now = SDL_GetTicks();
  if (direction != m_navigation_held)
  {
    m_navigation_held = direction;
    m_navigation_since = now;
    m_navigation_last = now;
    return;
  }
  if (!direction || now - m_navigation_since < 360 || now - m_navigation_last < 85)
    return;
  m_navigation_last = now;
  SDL_Event navigation{};
  navigation.type = SDL_CONTROLLERBUTTONDOWN;
  navigation.cbutton.button = static_cast<Uint8>(direction);
  SDL_PushEvent(&navigation);
}

int Launcher::EventNavigation(const SDL_Event& event) const
{
  if (event.type == SDL_CONTROLLERBUTTONDOWN)
  {
    if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_UP)
      return -1;
    if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)
      return 1;
  }
  if (event.type == SDL_KEYDOWN)
  {
    if (event.key.keysym.sym == SDLK_UP)
      return -1;
    if (event.key.keysym.sym == SDLK_DOWN)
      return 1;
  }
  return 0;
}

bool Launcher::PromptText(std::string_view header, std::string_view initial, std::string* output,
                          bool password, bool allow_empty, std::string_view subtext,
                          std::string_view guide)
{
  if (!output)
    return false;
  SwkbdConfig keyboard{};
  if (R_FAILED(swkbdCreate(&keyboard, 0)))
    return false;
  if (password)
    swkbdConfigMakePresetPassword(&keyboard);
  else
    swkbdConfigMakePresetDefault(&keyboard);
  const std::string header_owned(m_localization.Translate(header));
  const std::string initial_owned(initial);
  const std::string subtext_owned(m_localization.Translate(subtext));
  const std::string guide_owned(m_localization.Translate(guide));
  if (!header.empty())
    swkbdConfigSetHeaderText(&keyboard, header_owned.c_str());
  if (!initial.empty())
    swkbdConfigSetInitialText(&keyboard, initial_owned.c_str());
  if (!subtext.empty())
    swkbdConfigSetSubText(&keyboard, subtext_owned.c_str());
  if (!guide.empty())
    swkbdConfigSetGuideText(&keyboard, guide_owned.c_str());
  std::array<char, 1024> buffer{};
  swkbdConfigSetStringLenMax(&keyboard, buffer.size() - 1);
  const Result result = swkbdShow(&keyboard, buffer.data(), buffer.size());
  swkbdClose(&keyboard);
  if (R_FAILED(result) || (!allow_empty && buffer[0] == '\0'))
    return false;
  *output = buffer.data();
  return true;
}

void Launcher::LoadSourcesAndShares()
{
  m_sources.clear();
  m_usb_source_bindings.clear();
  m_usb_locations = Storage::ListUsbLocations();
  std::unordered_set<std::string> source_identities;
  const int source_count = std::clamp(m_store.GetInt("Library/SourceCount", 0), 0, 16);
  for (int index = 0; index < source_count; ++index)
  {
    const std::string prefix = "Library/Source" + std::to_string(index);
    const std::string stored_path = NormalizePath(m_store.Get(prefix));
    const std::string usb_id = m_store.Get(prefix + "UsbId");
    std::string usb_relative = NormalizePath(m_store.Get(prefix + "UsbPath"));
    while (!usb_relative.empty() && usb_relative.front() == '/')
      usb_relative.erase(usb_relative.begin());
    std::string path = stored_path;
    if (!usb_id.empty())
    {
      const std::string root = Storage::ResolveUsbPath(usb_id);
      if (!root.empty())
        path = NormalizePath(JoinPath(root, usb_relative));
      else
        path = UnavailableUsbSourcePath(usb_id, usb_relative);
    }
    const std::string source_identity =
        usb_id.empty() ? Lower(path) : "usb:" + Lower(usb_id) + "/" + Lower(usb_relative);
    if (!path.empty() && source_identities.insert(source_identity).second)
    {
      m_sources.push_back(path);
      if (!usb_id.empty())
        m_usb_source_bindings.emplace(Lower(path), std::pair{usb_id, usb_relative});
    }
  }

  m_shares.clear();
  std::unordered_set<std::string> share_ids;
  const int share_count = std::clamp(m_store.GetInt("Storage/SmbCount", 0), 0, 8);
  for (int index = 0; index < share_count; ++index)
  {
    const std::string prefix = "Storage/Smb" + std::to_string(index);
    Storage::SmbShare share;
    share.id = m_store.Get(prefix + "Id");
    share.name = m_store.Get(prefix + "Name");
    share.server = m_store.Get(prefix + "Server");
    share.share = m_store.Get(prefix + "Share");
    share.path = m_store.Get(prefix + "Path");
    share.user = m_store.Get(prefix + "User");
    share.password = m_store.Get(prefix + "Password");
    share.domain = m_store.Get(prefix + "Domain");
    share.auto_mount = m_store.GetBool(prefix + "AutoMount", true);
    if (!Storage::SmbRootPath(share.id).empty() && !share.server.empty() && !share.share.empty() &&
        share_ids.insert(share.id).second)
      m_shares.push_back(std::move(share));
  }

  m_usb_generation = Storage::UsbStatusGeneration();
  m_usb_locations = Storage::ListUsbLocations();
  LoadLibraryIdentities();
  LoadLibraryOrganization();
  StartAutoMountShares();
}

void Launcher::StartAutoMountShares()
{
  StopAutoMountShares();
  std::vector<Storage::SmbShare> shares;
  for (const Storage::SmbShare& share : m_shares)
  {
    if (share.auto_mount)
      shares.push_back(share);
  }
  if (shares.empty())
    return;
  auto state = std::make_shared<SmbAutoMountState>();
  m_smb_auto_mount = state;
  m_smb_auto_mount_thread = std::thread([state, shares = std::move(shares)] {
    for (const Storage::SmbShare& share : shares)
    {
      if (state->cancel.load(std::memory_order_acquire))
        break;
      std::string error;
      if (Storage::MountSmb(share, &error, &state->cancel))
      {
        std::lock_guard lock(state->mutex);
        state->mounted_roots.push_back(Storage::SmbRootPath(share.id));
      }
      SDL_Event wake{};
      wake.type = SDL_USEREVENT;
      wake.user.code = 0x534d424d;  // SMBM: SMB mount state changed.
      SDL_PushEvent(&wake);
    }
    state->complete.store(true, std::memory_order_release);
    SDL_Event wake{};
    wake.type = SDL_USEREVENT;
    wake.user.code = 0x534d424d;
    SDL_PushEvent(&wake);
  });
}

void Launcher::StopAutoMountShares()
{
  if (m_smb_auto_mount)
    m_smb_auto_mount->cancel.store(true, std::memory_order_release);
  if (m_smb_auto_mount_thread.joinable())
    m_smb_auto_mount_thread.join();
  m_smb_auto_mount.reset();
}

void Launcher::PumpAutoMountShares()
{
  const std::shared_ptr<SmbAutoMountState> state = m_smb_auto_mount;
  if (!state)
    return;
  std::deque<std::string> roots;
  {
    std::lock_guard lock(state->mutex);
    roots.swap(state->mounted_roots);
  }
  for (const std::string& root : roots)
  {
    for (const std::string& source : m_sources)
    {
      if (PathAtOrBelow(source, root))
        m_pending_scan_sources.push_back(source);
    }
  }
  if (!roots.empty())
  {
    std::ranges::sort(m_pending_scan_sources);
    m_pending_scan_sources.erase(
        std::unique(m_pending_scan_sources.begin(), m_pending_scan_sources.end()),
        m_pending_scan_sources.end());
  }
  if (state->complete.load(std::memory_order_acquire))
  {
    if (m_smb_auto_mount_thread.joinable())
      m_smb_auto_mount_thread.join();
    m_smb_auto_mount.reset();
  }
}

void Launcher::StartUsbInitialization()
{
  if (m_usb_initialization || m_usb_initialization_thread.joinable())
    return;
  auto state = std::make_shared<UsbInitializationState>();
  m_usb_initialization = state;
  m_usb_initialization_thread = std::thread([state] {
    state->success = Storage::InitializeUsb(&state->error);
    state->complete.store(true, std::memory_order_release);
    SDL_Event wake{};
    wake.type = SDL_USEREVENT;
    wake.user.code = 0x55534249;  // USBI: asynchronous USB initialization completed.
    SDL_PushEvent(&wake);
  });
}

void Launcher::StopUsbInitialization()
{
  if (m_usb_initialization_thread.joinable())
    m_usb_initialization_thread.join();
  m_usb_initialization.reset();
}

void Launcher::PumpUsbInitialization()
{
  const std::shared_ptr<UsbInitializationState> state = m_usb_initialization;
  if (!state || !state->complete.load(std::memory_order_acquire))
    return;
  if (m_usb_initialization_thread.joinable())
    m_usb_initialization_thread.join();
  // USB is optional. Keep SD/SMB usable when usb:hs is unavailable instead of interrupting startup
  // with an error dialog; File Manager will simply have no USB roots.
  m_usb_initialization.reset();
}

void Launcher::SaveSources()
{
  m_usb_locations = Storage::ListUsbLocations();
  std::vector<std::string> normalized_sources;
  std::vector<std::pair<std::string, std::string>> normalized_bindings;
  std::unordered_set<std::string> identities;
  for (const std::string& source : m_sources)
  {
    std::string path = NormalizePath(source);
    std::pair<std::string, std::string> binding;
    const auto existing = m_usb_source_bindings.find(Lower(path));
    if (existing != m_usb_source_bindings.end())
      binding = existing->second;
    else
    {
      for (const Storage::Location& location : m_usb_locations)
      {
        if (!PathAtOrBelow(path, location.path))
          continue;
        const std::string normalized_root = NormalizePath(location.path);
        std::string relative = path.substr(normalized_root.size());
        while (!relative.empty() && relative.front() == '/')
          relative.erase(relative.begin());
        binding = {location.id, relative};
        break;
      }
    }
    const std::string identity = binding.first.empty() ?
                                     Lower(path) :
                                     "usb:" + Lower(binding.first) + "/" + Lower(binding.second);
    if (path.empty() || !identities.insert(identity).second || normalized_sources.size() >= 16)
      continue;
    // The runtime map is path-keyed for fast hotplug lookup. If topology churn temporarily gives
    // this stable source the same mutable alias as another record, retain it under its unique
    // unavailable placeholder instead of overwriting either binding.
    if (!binding.first.empty() && std::ranges::any_of(normalized_sources, [&](const auto& saved) {
          return Lower(saved) == Lower(path);
        }))
    {
      path = UnavailableUsbSourcePath(binding.first, binding.second);
    }
    normalized_sources.emplace_back(std::move(path));
    normalized_bindings.emplace_back(std::move(binding));
  }
  m_sources = std::move(normalized_sources);
  m_store.RemovePrefix("Library/Source");
  m_store.SetInt("Library/SourceCount", m_sources.size());
  std::unordered_map<std::string, std::pair<std::string, std::string>> bindings;
  for (std::size_t index = 0; index < m_sources.size(); ++index)
  {
    const std::string prefix = "Library/Source" + std::to_string(index);
    const std::string& source = m_sources[index];
    const auto& binding = normalized_bindings[index];
    m_store.Set(prefix, source);
    if (!binding.first.empty())
    {
      m_store.Set(prefix + "UsbId", binding.first);
      m_store.Set(prefix + "UsbPath", binding.second);
      bindings.emplace(Lower(source), binding);
    }
  }
  m_usb_source_bindings = std::move(bindings);
  MarkStoreDirty();
}

void Launcher::SaveShares()
{
  m_store.RemovePrefix("Storage/Smb");
  m_store.SetInt("Storage/SmbCount", m_shares.size());
  for (std::size_t index = 0; index < m_shares.size(); ++index)
  {
    const Storage::SmbShare& share = m_shares[index];
    const std::string prefix = "Storage/Smb" + std::to_string(index);
    m_store.Set(prefix + "Id", share.id);
    m_store.Set(prefix + "Name", share.name);
    m_store.Set(prefix + "Server", share.server);
    m_store.Set(prefix + "Share", share.share);
    m_store.Set(prefix + "Path", share.path);
    m_store.Set(prefix + "User", share.user);
    m_store.Set(prefix + "Password", share.password);
    m_store.Set(prefix + "Domain", share.domain);
    m_store.SetBool(prefix + "AutoMount", share.auto_mount);
  }
  MarkStoreDirty();
}

void Launcher::ReplaceSavedPathPrefix(const std::string& old_path, const std::string& new_path)
{
  const std::string old_normalized = NormalizePath(old_path);
  const std::string new_normalized = NormalizePath(new_path);
  if (old_normalized.empty() || new_normalized.empty())
    return;
  const std::string old_identity = Lower(old_normalized);
  bool sources_changed = false;
  bool games_changed = false;
  for (std::string& source : m_sources)
  {
    const std::string normalized = NormalizePath(source);
    const std::string identity = Lower(normalized);
    if (identity == old_identity)
    {
      source = new_normalized;
      sources_changed = true;
    }
    else if (identity.size() > old_identity.size() && identity.starts_with(old_identity) &&
             (old_identity.back() == '/' || identity[old_identity.size()] == '/'))
    {
      source = NormalizePath(new_normalized + normalized.substr(old_normalized.size()));
      sources_changed = true;
    }
  }
  if (!m_clipboard_path.empty() && PathAtOrBelow(m_clipboard_path, old_normalized))
  {
    const std::string clipboard = NormalizePath(m_clipboard_path);
    m_clipboard_path = NormalizePath(new_normalized + clipboard.substr(old_normalized.size()));
  }
  for (Game& game : m_games)
  {
    if (game.installed_nand || !PathAtOrBelow(game.path, old_normalized))
      continue;
    const std::string old_game_path = NormalizePath(game.path);
    game.path = NormalizePath(new_normalized + old_game_path.substr(old_normalized.size()));
    game.canonical_path = CanonicalLibraryPath(game.path);
    const auto identity =
        std::ranges::find(m_library_identities, game.key, &LibraryIdentityRecord::id);
    if (identity != m_library_identities.end())
    {
      RememberPreviousLibraryPath(&*identity, old_game_path);
      identity->canonical_path = game.canonical_path;
      identity->current_path = game.path;
    }
    games_changed = true;
  }
  if (sources_changed)
    SaveSources();
  if (games_changed)
  {
    SaveLibraryIdentities();
    m_library_refresh_requested = true;
  }
  if (sources_changed || games_changed)
  {
    FlushPendingSaves();
  }
}

void Launcher::RemoveSavedPathsBelow(const std::string& root)
{
  const std::size_t previous_size = m_sources.size();
  std::erase_if(m_sources, [&](const std::string& path) { return PathAtOrBelow(path, root); });
  if (m_sources.size() != previous_size)
  {
    SaveSources();
    FlushPendingSaves();
  }
  if (!m_clipboard_path.empty() && PathAtOrBelow(m_clipboard_path, root))
  {
    m_clipboard_path.clear();
    m_clipboard_move = false;
  }
}

void Launcher::EnsureSourceMountedAtStartup(const std::string& path)
{
  bool changed = false;
  for (Storage::SmbShare& share : m_shares)
  {
    if (PathAtOrBelow(path, Storage::SmbRootPath(share.id)) && !share.auto_mount)
    {
      share.auto_mount = true;
      changed = true;
    }
  }
  if (changed)
  {
    SaveShares();
    FlushPendingSaves();
  }
}

bool Launcher::RefreshConfiguredUsbSources()
{
  if (m_usb_source_bindings.empty() && !std::ranges::any_of(m_sources, IsUsbStoragePath))
    return false;
  m_usb_locations = Storage::ListUsbLocations();
  const auto previous_bindings = m_usb_source_bindings;
  std::vector<std::pair<std::string, std::string>> stable_bindings(m_sources.size());
  bool changed = false;

  // Phase one snapshots every source's stable identity before changing any spelling. Looking up
  // and updating the path-keyed map in one loop is unsafe when ums aliases swap: inserting A's
  // new ums0 key could overwrite B's old ums0 entry before B has been processed.
  for (std::size_t index = 0; index < m_sources.size(); ++index)
  {
    const std::string path = NormalizePath(m_sources[index]);
    const auto existing = previous_bindings.find(Lower(path));
    if (existing != previous_bindings.end())
    {
      stable_bindings[index] = existing->second;
      continue;
    }
    if (!IsUsbStoragePath(path))
      continue;

    // One-time migration for launcher.ini files written before stable USB identities existed.
    struct stat source_info{};
    if (::stat(path.c_str(), &source_info) == 0 && S_ISDIR(source_info.st_mode))
    {
      for (const Storage::Location& location : m_usb_locations)
      {
        if (!PathAtOrBelow(path, location.path))
          continue;
        const std::string root = NormalizePath(location.path);
        std::string relative = NormalizePath(path).substr(root.size());
        while (!relative.empty() && relative.front() == '/')
          relative.erase(relative.begin());
        stable_bindings[index] = {location.id, std::move(relative)};
        changed = true;
        break;
      }
      continue;
    }
    const std::size_t colon = path.find(':');
    std::string relative = colon == std::string::npos ? std::string{} : path.substr(colon + 1);
    while (!relative.empty() && relative.front() == '/')
      relative.erase(relative.begin());
    std::vector<std::string> matches;
    const Storage::Location* matched_location = nullptr;
    for (const Storage::Location& location : m_usb_locations)
    {
      const std::string candidate = NormalizePath(location.path + relative);
      struct stat candidate_info{};
      if (::stat(candidate.c_str(), &candidate_info) == 0 && S_ISDIR(candidate_info.st_mode))
      {
        matches.push_back(candidate);
        matched_location = &location;
      }
    }
    if (matches.size() == 1 && matched_location)
    {
      stable_bindings[index] = {matched_location->id, std::move(relative)};
      changed = true;
    }
  }

  // Phase two resolves every stable identity against the same USB snapshot, then atomically
  // replaces the source vector and lookup map. A duplicate mutable spelling is represented by a
  // stable placeholder, so both saved records survive until their volumes have distinct aliases.
  std::vector<std::string> refreshed_sources;
  refreshed_sources.reserve(m_sources.size());
  std::unordered_map<std::string, std::pair<std::string, std::string>> refreshed_bindings;
  std::unordered_set<std::string> source_identities;
  std::unordered_set<std::string> occupied_paths;
  for (std::size_t index = 0; index < m_sources.size(); ++index)
  {
    const std::string old_path = NormalizePath(m_sources[index]);
    const auto& binding = stable_bindings[index];
    std::string path = old_path;
    std::string identity = Lower(path);
    if (!binding.first.empty())
    {
      identity = "usb:" + Lower(binding.first) + "/" + Lower(binding.second);
      const std::string root = Storage::ResolveUsbPath(binding.first);
      path = root.empty() ? UnavailableUsbSourcePath(binding.first, binding.second) :
                            NormalizePath(JoinPath(root, binding.second));
      if (occupied_paths.contains(Lower(path)))
        path = UnavailableUsbSourcePath(binding.first, binding.second);
    }
    if (path.empty() || occupied_paths.contains(Lower(path)) ||
        !source_identities.insert(identity).second)
    {
      changed = true;
      continue;
    }
    changed |= Lower(path) != Lower(old_path);
    refreshed_sources.emplace_back(path);
    occupied_paths.insert(Lower(path));
    if (!binding.first.empty())
      refreshed_bindings.emplace(Lower(path), binding);
  }
  changed |= refreshed_sources.size() != m_sources.size();
  m_sources = std::move(refreshed_sources);
  m_usb_source_bindings = std::move(refreshed_bindings);
  if (changed)
  {
    SaveSources();
    FlushPendingSaves();
  }
  return changed;
}

void Launcher::LoadLibraryIdentities()
{
  m_library_identities.clear();
  m_library_identities_dirty = false;
  std::unordered_set<std::string> ids;
  const int count = std::clamp(m_store.GetInt("Library/IdentityCount", 0), 0, 16384);
  for (int index = 0; index < count; ++index)
  {
    const std::string prefix = "Library/Identity" + std::to_string(index);
    LibraryIdentityRecord record;
    record.id = m_store.Get(prefix + "Id");
    record.fingerprint = m_store.Get(prefix + "Fingerprint");
    record.base_identity = m_store.Get(prefix + "BaseIdentity");
    record.canonical_path = m_store.Get(prefix + "Path");
    record.current_path = m_store.Get(prefix + "CurrentPath");
    record.retired = m_store.GetBool(prefix + "Retired", false);
    const int previous_count = std::clamp(m_store.GetInt(prefix + "PreviousPathCount", 0), 0,
                                          static_cast<int>(MAX_PREVIOUS_LIBRARY_PATHS));
    for (int previous = 0; previous < previous_count; ++previous)
    {
      const std::string path =
          NormalizePath(m_store.Get(prefix + "PreviousPath" + std::to_string(previous)));
      if (!path.empty() && std::ranges::none_of(record.previous_paths, [&](const auto& existing) {
            return Lower(existing) == Lower(path);
          }))
      {
        record.previous_paths.emplace_back(path);
      }
    }
    const bool valid_id = !record.id.empty() && record.id.size() <= 96 &&
                          std::ranges::all_of(record.id, [](unsigned char character) {
                            return std::isalnum(character) || character == '-' || character == '_';
                          });
    if (valid_id && !record.fingerprint.empty() && ids.insert(record.id).second)
      m_library_identities.emplace_back(std::move(record));
  }
}

void Launcher::SaveLibraryIdentities()
{
  m_store.RemovePrefix("Library/Identity");
  m_store.SetInt("Library/IdentityCount", static_cast<int>(m_library_identities.size()));
  for (std::size_t index = 0; index < m_library_identities.size(); ++index)
  {
    const std::string prefix = "Library/Identity" + std::to_string(index);
    const LibraryIdentityRecord& record = m_library_identities[index];
    m_store.Set(prefix + "Id", record.id);
    m_store.Set(prefix + "Fingerprint", record.fingerprint);
    m_store.Set(prefix + "BaseIdentity", record.base_identity);
    m_store.Set(prefix + "Path", record.canonical_path);
    m_store.Set(prefix + "CurrentPath", record.current_path);
    m_store.SetBool(prefix + "Retired", record.retired);
    m_store.SetInt(prefix + "PreviousPathCount", static_cast<int>(record.previous_paths.size()));
    for (std::size_t previous = 0; previous < record.previous_paths.size(); ++previous)
      m_store.Set(prefix + "PreviousPath" + std::to_string(previous),
                  record.previous_paths[previous]);
  }
  m_library_identities_dirty = false;
  MarkStoreDirty();
}

std::string Launcher::CanonicalLibraryPath(std::string_view input) const
{
  const std::string path = NormalizePath(std::string(input));
  for (const Storage::Location& location : m_usb_locations)
  {
    if (!PathAtOrBelow(path, location.path))
      continue;
    const std::string root = NormalizePath(location.path);
    std::string relative = path.substr(std::min(path.size(), root.size()));
    while (!relative.empty() && relative.front() == '/')
      relative.erase(relative.begin());
    return "usb:" + location.id + "/" + Lower(relative);
  }
  for (const Storage::SmbShare& share : m_shares)
  {
    const std::string root = Storage::SmbRootPath(share.id);
    if (!PathAtOrBelow(path, root))
      continue;
    std::string relative = path.substr(std::min(path.size(), NormalizePath(root).size()));
    while (!relative.empty() && relative.front() == '/')
      relative.erase(relative.begin());
    return "smb:" + share.id + "/" + Lower(relative);
  }
  return Lower(path);
}

bool Launcher::LibraryIdentityPathExists(const LibraryIdentityRecord& record) const
{
  if (record.retired)
    return false;
  const auto regular_file = [](const std::string& path) {
    struct stat info{};
    return !path.empty() && ::stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
  };
  if (regular_file(record.current_path) &&
      CanonicalLibraryPath(record.current_path) == record.canonical_path)
  {
    return true;
  }

  const std::size_t slash = record.canonical_path.find('/');
  if (slash == std::string::npos)
    return false;
  std::string root;
  if (record.canonical_path.starts_with("usb:"))
    root = Storage::ResolveUsbPath(record.canonical_path.substr(4, slash - 4));
  else if (record.canonical_path.starts_with("smb:"))
    root = Storage::SmbRootPath(record.canonical_path.substr(4, slash - 4));
  if (root.empty())
    return false;

  std::string relative = record.canonical_path.substr(slash + 1);
  const std::size_t current_colon = record.current_path.find(':');
  if (current_colon != std::string::npos)
  {
    std::string current_relative = record.current_path.substr(current_colon + 1);
    while (!current_relative.empty() && current_relative.front() == '/')
      current_relative.erase(current_relative.begin());
    if (Lower(current_relative) == relative)
      relative = std::move(current_relative);
  }
  return regular_file(JoinPath(root, relative));
}

std::string Launcher::GameFingerprint(const UICommon::GameFile& metadata) const
{
  std::string hash;
  // GetSyncHash may read an entire multi-gigabyte disc image.  Stable disc identity already has a
  // game/title ID, platform, disc and revision, and deliberately survives patched/repacked images,
  // so hashing those files only made every first scan dramatically slower.  Anonymous executables
  // still need a content discriminator because they have no reliable title identity.
  if (metadata.GetPlatform() == DiscIO::Platform::ELFOrDOL ||
      (metadata.GetGameID().empty() && metadata.GetTitleID() == 0))
  {
    const auto digest = metadata.GetSyncHash();
    static constexpr char HEX[] = "0123456789abcdef";
    hash.reserve(digest.size() * 2);
    for (const std::uint8_t byte : digest)
    {
      hash += HEX[byte >> 4];
      hash += HEX[byte & 0xf];
    }
  }
  // Keep the legacy tuple prefix stable so existing identity records migrate without a reset.
  return metadata.GetGameID() + ":" + std::to_string(metadata.GetRevision()) + ":" +
         std::to_string(metadata.GetDiscNumber()) + ":" +
         std::to_string(static_cast<int>(metadata.GetPlatform())) + ":" +
         std::to_string(metadata.GetFileSize()) + ":" + hash;
}

std::string Launcher::GameBaseIdentity(const UICommon::GameFile& metadata) const
{
  // The title ID adds precision for WADs, while platform/disc/revision distinguish multi-disc and
  // revision-specific configurations. No filename, size, timestamp, or content hash belongs here:
  // those are expected to change for patched games such as BetterWW.
  if (metadata.GetGameID().empty() && metadata.GetTitleID() == 0)
    return "v2-anonymous:" + Hex64(HashPath(GameFingerprint(metadata)));
  return "v2:" + metadata.GetGameID() + ":" + Hex64(metadata.GetTitleID()) + ":" +
         std::to_string(static_cast<int>(metadata.GetPlatform())) + ":" +
         std::to_string(metadata.GetDiscNumber()) + ":" + std::to_string(metadata.GetRevision());
}

void Launcher::AssignStableIdentity(Game* game)
{
  if (!game || game->installed_nand)
    return;
  if (game->canonical_path.empty())
    game->canonical_path = CanonicalLibraryPath(game->path);
  if (game->fingerprint.empty())
    game->fingerprint = GameFingerprint(*game->metadata);
  if (game->base_identity.empty())
    game->base_identity = GameBaseIdentity(*game->metadata);

  const std::string legacy_base = LegacyBaseIdentityFromFingerprint(game->fingerprint);
  const auto base_compatible = [&](const LibraryIdentityRecord& record) {
    if (!record.base_identity.empty())
      return record.base_identity == game->base_identity;
    if (game->base_identity.starts_with("v2-anonymous:"))
      return record.fingerprint == game->fingerprint;
    // Records from 1.0.3 and earlier did not persist BaseIdentity. Their fingerprint contains the
    // same platform/disc/revision tuple, so migrate it once and never accept path alone.
    const std::string record_legacy = LegacyBaseIdentityFromFingerprint(record.fingerprint);
    return !legacy_base.empty() && record_legacy == legacy_base;
  };

  LibraryIdentityRecord* match = nullptr;
  bool identity_changed = false;
  for (LibraryIdentityRecord& record : m_library_identities)
  {
    if (record.retired || m_claimed_library_ids.contains(record.id) ||
        record.canonical_path != game->canonical_path)
      continue;
    if (base_compatible(record))
    {
      match = &record;
      break;
    }
    // A different title now occupies this path. It must not receive path-derived settings,
    // controls, artwork, or a legacy forwarder identity. Keep the canonical scope for recovery,
    // but retire resolution so an already-installed stable forwarder cannot boot the replacement.
    game->allow_legacy_path_migration = false;
    RememberPreviousLibraryPath(&record, record.current_path);
    record.current_path.clear();
    record.retired = true;
    identity_changed = true;
    m_reserved_library_ids.erase(record.id);
  }
  if (!match)
  {
    const std::string game_scope = LibraryIdentityScope(game->canonical_path);
    for (LibraryIdentityRecord& record : m_library_identities)
    {
      const bool fingerprint_compatible =
          record.fingerprint == game->fingerprint ||
          (!game->fingerprint.empty() && game->fingerprint.back() == ':' &&
           record.fingerprint.starts_with(game->fingerprint));
      if (m_claimed_library_ids.contains(record.id) || game_scope.empty() ||
          LibraryIdentityScope(record.canonical_path) != game_scope || !fingerprint_compatible ||
          !base_compatible(record))
        continue;
      // All live records are reserved without touching the filesystem at scan startup. Only a
      // rare fingerprint-based rename candidate needs a stat; exact canonical matches above are
      // still immediate. This removes one potentially-networked stat per library entry from every
      // launch while keeping identical copies from stealing one another's IDs.
      if (m_reserved_library_ids.contains(record.id))
      {
        if (LibraryIdentityPathExists(record))
          continue;
        m_reserved_library_ids.erase(record.id);
      }
      match = &record;
      break;
    }
  }
  if (!match)
  {
    std::string id = StableIdStem(game->game_id, game->fingerprint);
    const std::string stem = id;
    unsigned collision = 1;
    const auto id_exists = [&](std::string_view candidate) {
      return std::ranges::any_of(m_library_identities, [&](const LibraryIdentityRecord& record) {
        return record.id == candidate;
      });
    };
    while (id_exists(id))
      id = stem + "-" + std::to_string(++collision);
    LibraryIdentityRecord record;
    record.id = std::move(id);
    record.fingerprint = game->fingerprint;
    record.base_identity = game->base_identity;
    record.canonical_path = game->canonical_path;
    record.current_path = NormalizePath(game->path);
    m_library_identities.emplace_back(std::move(record));
    match = &m_library_identities.back();
    identity_changed = true;
  }
  else
  {
    // Patched/repacked content with a compatible base tuple legitimately keeps its identity.
    const std::string current_path = NormalizePath(game->path);
    if (!match->current_path.empty() &&
        Lower(NormalizePath(match->current_path)) != Lower(current_path))
    {
      RememberPreviousLibraryPath(match, match->current_path);
    }
    identity_changed = match->fingerprint != game->fingerprint ||
                       match->base_identity != game->base_identity ||
                       match->canonical_path != game->canonical_path ||
                       match->current_path != current_path || match->retired;
    match->fingerprint = game->fingerprint;
    match->base_identity = game->base_identity;
    match->canonical_path = game->canonical_path;
    match->current_path = current_path;
    match->retired = false;
  }
  // Populate the field when migrating a record written by a launcher version that only stored the
  // canonical key.
  if (match->current_path.empty())
  {
    match->current_path = NormalizePath(game->path);
    identity_changed = true;
  }
  m_library_identities_dirty |= identity_changed;
  game->key = match->id;
  m_reserved_library_ids.erase(game->key);
  m_claimed_library_ids.insert(game->key);
  if (game->canonical_path.starts_with("usb:"))
    game->storage_id = game->canonical_path.substr(0, game->canonical_path.find('/'));
  else if (game->canonical_path.starts_with("smb:"))
    game->storage_id = game->canonical_path.substr(0, game->canonical_path.find('/'));
  else
    game->storage_id = DeviceName(game->path);
}

void Launcher::MigrateLegacyGameState(Game* game)
{
  if (!game || !game->allow_legacy_path_migration || game->legacy_key.empty() ||
      game->legacy_key == game->key)
    return;
  const auto migrate_store_value = [&](std::string_view group) {
    const std::string old_key = std::string(group) + "/" + game->legacy_key;
    const std::string new_key = std::string(group) + "/" + game->key;
    const std::string old_value = m_store.Get(old_key);
    if (m_store.Get(new_key).empty() && !old_value.empty())
      m_store.Set(new_key, old_value);
    if (!old_value.empty())
      m_store.Remove(old_key);
  };
  migrate_store_value("Alias");
  migrate_store_value("Recent");
  migrate_store_value("Favorite");

  const std::string old_cover = std::string(COVER_DIRECTORY) + "/" + game->legacy_key + ".png";
  const std::string new_cover = CoverPath(*game);
  if (!RegularFileExists(new_cover) && RegularFileExists(old_cover))
    File::Rename(old_cover, new_cover);

  const std::string entries = File::GetUserPath(D_GAMESETTINGS_IDX) + "Entries/";
  const std::string old_ini = entries + Hex64(HashPath(Lower(NormalizePath(game->path)))) + ".ini";
  const std::string new_ini = entries + game->key + ".ini";
  if (!RegularFileExists(new_ini) && RegularFileExists(old_ini))
  {
    File::CreateFullPath(new_ini);
    File::Rename(old_ini, new_ini);
  }
  MarkStoreDirty();
}

void Launcher::LoadLibraryOrganization()
{
  // Collection and search are transient views. Always open a fresh launcher on the complete
  // library, while favorites and collection membership themselves remain persistent.
  const bool had_saved_view =
      !m_store.Get("Library/ActiveCollection").empty() || !m_store.Get("Library/Search").empty();
  m_active_collection.clear();
  m_search_query.clear();
  m_store.Remove("Library/ActiveCollection");
  m_store.Remove("Library/Search");
  if (had_saved_view)
    MarkStoreDirty();
  m_favorites.clear();
  const int favorite_count = std::clamp(m_store.GetInt("Library/FavoriteCount", 0), 0, 16384);
  for (int index = 0; index < favorite_count; ++index)
  {
    const std::string id = m_store.Get("Library/Favorite" + std::to_string(index));
    if (!id.empty())
      m_favorites.insert(id);
  }
  m_collections.clear();
  const int collection_count = std::clamp(m_store.GetInt("Library/CollectionCount", 0), 0, 128);
  for (int index = 0; index < collection_count; ++index)
  {
    const std::string prefix = "Library/Collection" + std::to_string(index);
    Collection collection;
    collection.name = m_store.Get(prefix + "Name");
    std::string members = m_store.Get(prefix + "Members");
    for (std::size_t start = 0; start <= members.size();)
    {
      const std::size_t separator = members.find(',', start);
      const std::string member = members.substr(
          start, separator == std::string::npos ? std::string::npos : separator - start);
      if (!member.empty())
        collection.members.insert(member);
      if (separator == std::string::npos)
        break;
      start = separator + 1;
    }
    if (!collection.name.empty())
      m_collections.emplace_back(std::move(collection));
  }
}

void Launcher::SaveCollections()
{
  m_store.RemovePrefix("Library/Favorite");
  m_store.SetInt("Library/FavoriteCount", static_cast<int>(m_favorites.size()));
  std::size_t favorite_index = 0;
  for (const std::string& id : m_favorites)
    m_store.Set("Library/Favorite" + std::to_string(favorite_index++), id);
  m_store.RemovePrefix("Library/Collection");
  m_store.SetInt("Library/CollectionCount", static_cast<int>(m_collections.size()));
  for (std::size_t index = 0; index < m_collections.size(); ++index)
  {
    const std::string prefix = "Library/Collection" + std::to_string(index);
    m_store.Set(prefix + "Name", m_collections[index].name);
    std::string members;
    for (const std::string& id : m_collections[index].members)
    {
      if (!members.empty())
        members += ',';
      members += id;
    }
    m_store.Set(prefix + "Members", std::move(members));
  }
  MarkStoreDirty();
}

void Launcher::RebuildVisibleGames()
{
  m_visible_games.clear();
  const std::string query = Lower(Trim(m_search_query));
  const Collection* active = nullptr;
  if (!m_active_collection.empty() && m_active_collection != "favorites")
  {
    const auto found = std::ranges::find(m_collections, m_active_collection, &Collection::name);
    if (found != m_collections.end())
      active = &*found;
  }
  for (std::size_t index = 0; index < m_games.size(); ++index)
  {
    const Game& game = m_games[index];
    if (m_active_collection == "favorites" && !m_favorites.contains(game.key))
      continue;
    if (active && !active->members.contains(game.key))
      continue;
    if (!query.empty())
    {
      const std::string searchable =
          Lower(game.title + " " + game.game_id + " " + game.platform + " " + game.path);
      if (searchable.find(query) == std::string::npos)
        continue;
    }
    m_visible_games.push_back(index);
  }
}

Game* Launcher::VisibleGame(int index)
{
  return index >= 0 && index < static_cast<int>(m_visible_games.size()) ?
             &m_games[m_visible_games[index]] :
             nullptr;
}

void Launcher::StartGameScan(std::vector<std::string> sources, bool replace)
{
  StopGameScan();
  if (replace)
    CancelQueuedCoverDecodes();
  RefreshConfiguredUsbSources();
  sources = replace ? m_sources : std::move(sources);
  m_usb_locations = Storage::ListUsbLocations();
  m_reserved_library_ids.clear();
  for (const LibraryIdentityRecord& record : m_library_identities)
  {
    if (!record.retired)
      m_reserved_library_ids.insert(record.id);
  }

  std::unordered_set<std::uint64_t> unaffected_wad_titles;
  if (replace)
  {
    for (Game& game : m_games)
    {
      if (game.cover)
        SDL_DestroyTexture(game.cover);
    }
    m_games.clear();
    m_visible_games.clear();
    m_cover_use = 0;
    m_claimed_library_ids.clear();
  }
  else
  {
    m_claimed_library_ids.clear();
    // Records belonging to the roots being refreshed must remain available so incoming entries
    // update their existing IDs. Every unaffected live game is claimed up front; otherwise a new
    // byte-identical file from a partial USB/SMB scan could steal another game's fingerprint ID.
    std::erase_if(m_games, [&](Game& game) {
      if (game.installed_nand)
        return false;
      const bool in_target = std::ranges::any_of(
          sources, [&](const std::string& source) { return PathAtOrBelow(game.path, source); });
      if (in_target && game.cover)
        SDL_DestroyTexture(game.cover);
      return in_target;
    });
    for (const Game& game : m_games)
    {
      if (game.installed_nand)
        continue;
      m_claimed_library_ids.insert(game.key);
      if (game.metadata && game.metadata->GetPlatform() == DiscIO::Platform::WiiWAD &&
          game.title_id != 0)
        unaffected_wad_titles.insert(game.title_id);
    }
    RebuildVisibleGames();
  }

  auto state = std::make_shared<LibraryScanState>();
  state->full = replace;
  if (!replace)
  {
    for (const std::string& source : sources)
    {
      const auto binding = m_usb_source_bindings.find(Lower(NormalizePath(source)));
      if (binding != m_usb_source_bindings.end())
        state->target_usb_ids.insert(binding->second.first);
    }
    for (const std::string& id : state->target_usb_ids)
    {
      if (std::ranges::any_of(m_usb_locations,
                              [&](const Storage::Location& location) { return location.id == id; }))
        m_unavailable_usb_ids.erase(id);
    }
  }
  std::vector<std::pair<std::string, std::string>> usb_roots;
  for (const Storage::Location& location : m_usb_locations)
    usb_roots.emplace_back(location.id, NormalizePath(location.path));
  std::vector<std::pair<std::string, std::string>> smb_roots;
  for (const Storage::SmbShare& share : m_shares)
    smb_roots.emplace_back(share.id, NormalizePath(Storage::SmbRootPath(share.id)));
  std::unordered_multimap<std::string, std::pair<std::string, std::string>> known_fingerprints;
  known_fingerprints.reserve(m_library_identities.size());
  for (const LibraryIdentityRecord& record : m_library_identities)
  {
    if (!record.canonical_path.empty() && !record.fingerprint.empty())
      known_fingerprints.emplace(record.canonical_path,
                                 std::pair{record.base_identity, record.fingerprint});
  }
  // Load entries from the saved collection first. This is especially important when Dolphin was
  // closed while a collection was active: its first page should appear immediately instead of
  // waiting for unrelated games earlier in a large source tree.
  std::unordered_set<std::string> priority_ids;
  if (m_active_collection == "favorites")
  {
    priority_ids = m_favorites;
  }
  else if (!m_active_collection.empty())
  {
    const auto collection =
        std::ranges::find(m_collections, m_active_collection, &Collection::name);
    if (collection != m_collections.end())
      priority_ids = collection->members;
  }
  std::vector<std::string> priority_paths;
  priority_paths.reserve(priority_ids.size());
  for (const LibraryIdentityRecord& record : m_library_identities)
  {
    if (record.retired || record.current_path.empty() || !priority_ids.contains(record.id))
      continue;
    if (std::ranges::any_of(sources, [&](const std::string& source) {
          return PathAtOrBelow(record.current_path, source);
        }))
    {
      priority_paths.push_back(record.current_path);
    }
  }
  const std::size_t first_page_size = static_cast<std::size_t>(std::max(1, GridPageSize()));
  m_library_scan = state;
  m_library_refresh_requested = false;
  m_library_scan_thread = std::thread(
      [this, state, sources = std::move(sources), usb_roots = std::move(usb_roots),
       smb_roots = std::move(smb_roots), known_fingerprints = std::move(known_fingerprints),
       unaffected_wad_titles = std::move(unaffected_wad_titles),
       priority_paths = std::move(priority_paths), first_page_size] {
        const auto canonical_path = [&](std::string_view input) {
          const std::string path = NormalizePath(std::string(input));
          for (const auto& [id, root] : usb_roots)
          {
            if (!PathAtOrBelow(path, root))
              continue;
            std::string relative = path.substr(std::min(path.size(), root.size()));
            while (!relative.empty() && relative.front() == '/')
              relative.erase(relative.begin());
            return "usb:" + id + "/" + Lower(relative);
          }
          for (const auto& [id, root] : smb_roots)
          {
            if (!PathAtOrBelow(path, root))
              continue;
            std::string relative = path.substr(std::min(path.size(), root.size()));
            while (!relative.empty() && relative.front() == '/')
              relative.erase(relative.begin());
            return "smb:" + id + "/" + Lower(relative);
          }
          return Lower(path);
        };
        if (state->full)
        {
          m_game_cache.Clear(UICommon::GameFileCache::DeleteOnDisk::No);
          m_game_cache.Load();
        }

        std::unordered_set<std::uint64_t> source_wad_titles = std::move(unaffected_wad_titles);
        std::vector<std::string> all_paths;
        std::unordered_set<std::string> seen_paths;
        const auto process_path = [&](const std::string& path) {
          if (state->cancel.load(std::memory_order_acquire) || DiscIO::ShouldHideFromGameList(path))
            return;
          bool cache_changed = false;
          std::shared_ptr<const UICommon::GameFile> metadata =
              m_game_cache.AddOrGet(path, &cache_changed, false);
          state->cache_changed |= cache_changed;
          if (!metadata || !metadata->IsValid())
            return;

          Game game;
          game.metadata = metadata;
          game.path = metadata->GetFilePath();
          game.game_id = metadata->GetGameID();
          game.game_tdb_id = metadata->GetGameTDBID();
          game.title_id = metadata->GetTitleID();
          game.revision = metadata->GetRevision();
          game.title = metadata->GetName(UICommon::GameFile::Variant::LongAndPossiblyCustom);
          if (game.title.empty())
            game.title = metadata->GetFileName();
          game.region = metadata->GetRegion();
          game.platform = metadata->GetPlatform() == DiscIO::Platform::SaturnDisc ? "Saturn" :
                                                                                     "Unknown";
          game.legacy_key = (game.game_id.empty() ? "game" : game.game_id) + "-" + [&] {
            char suffix[32];
            std::snprintf(suffix, sizeof(suffix), "%04x-%016llx", game.revision,
                          static_cast<unsigned long long>(HashPath(Lower(game.path))));
            return std::string(suffix);
          }();
          game.canonical_path = canonical_path(game.path);
          game.base_identity = GameBaseIdentity(*metadata);
          game.metadata_refreshed = cache_changed;
          if (!cache_changed)
          {
            const auto [first, last] = known_fingerprints.equal_range(game.canonical_path);
            const auto known = std::find_if(first, last, [&](const auto& entry) {
              if (!entry.second.first.empty())
                return entry.second.first == game.base_identity;
              return LegacyBaseIdentityFromFingerprint(entry.second.second) ==
                     LegacyBaseIdentityFromFingerprint(GameFingerprint(*metadata));
            });
            if (known != last)
              game.fingerprint = known->second.second;
          }
          if (game.fingerprint.empty())
            game.fingerprint = GameFingerprint(*metadata);
          struct stat info{};
          if (::stat(game.path.c_str(), &info) == 0)
            game.modified = info.st_mtime;
          {
            std::lock_guard lock(state->mutex);
            state->ready.emplace_back(std::move(game));
          }
          const std::size_t processed =
              state->processed.fetch_add(1, std::memory_order_acq_rel) + 1;
          // Wake immediately for the first game and first complete page, then coalesce
          // notifications. Sorting/rebuilding the visible list once per file was the dominant cost
          // in large caches.
          if (processed <= first_page_size || processed % 32 == 0)
          {
            SDL_Event wake{};
            wake.type = SDL_USEREVENT;
            wake.user.code = 0x444c5343;  // DLSC: Dolphin library scan changed.
            SDL_PushEvent(&wake);
          }
        };

        for (const std::string& path : priority_paths)
        {
          if (state->cancel.load(std::memory_order_acquire))
            break;
          const std::string normalized = Lower(NormalizePath(path));
          if (normalized.empty() || !seen_paths.insert(normalized).second ||
              DiscIO::ShouldHideFromGameList(path))
          {
            continue;
          }
          all_paths.emplace_back(path);
          state->discovered.store(all_paths.size(), std::memory_order_release);
          process_path(path);
        }

        // Enumerate and publish one source at a time. A slow SMB source must not hold back games
        // already found on SD or USB, while all_paths is still retained for the final cache prune.
        for (const std::string& source : sources)
        {
          if (state->cancel.load(std::memory_order_acquire))
            break;
          WalkGamePaths(source, state->cancel, [&](std::string path) {
            if (DiscIO::ShouldHideFromGameList(path))
              return;
            if (!seen_paths.insert(Lower(NormalizePath(path))).second)
              return;
            all_paths.emplace_back(path);
            state->discovered.store(all_paths.size(), std::memory_order_release);
            process_path(path);
          });
        }

        if (state->full && !state->cancel.load(std::memory_order_acquire))
        {
          // AddOrGet already refreshed every discovered file. The final pass only prunes stale
          // cache paths; re-statting every game (and Riivolution dependency) here doubled scan I/O.
          state->cache_changed |= m_game_cache.Update(all_paths, {}, {}, state->cancel, false);
        }
        if (state->cache_changed && !state->cancel.load(std::memory_order_acquire))
          m_game_cache.Save();

        state->complete.store(true, std::memory_order_release);
        SDL_Event wake{};
        wake.type = SDL_USEREVENT;
        wake.user.code = 0x444c5343;
        SDL_PushEvent(&wake);
      });
}

void Launcher::StopGameScan()
{
  if (m_library_scan)
    m_library_scan->cancel.store(true, std::memory_order_release);
  if (m_library_scan_thread.joinable())
    m_library_scan_thread.join();
  m_library_scan.reset();
  // IDs are assigned as progressive results reach the UI.  A scan can be cancelled because the
  // user launches a game, edits storage, or closes Dolphin before its normal completion path.
  // Persist those already-published records so settings and generated shortcuts never reference
  // an identity which only existed in memory.
  if (m_library_identities_dirty)
  {
    SaveLibraryIdentities();
    FlushPendingSaves();
  }
}

void Launcher::PumpGameScan()
{
  const std::shared_ptr<LibraryScanState> state = m_library_scan;
  if (!state)
    return;
  std::deque<Game> ready;
  {
    std::lock_guard lock(state->mutex);
    // Keep publication bounded so metadata insertion and list maintenance cannot monopolize an
    // SDL frame. The worker wakes each first-page result, so the launcher becomes interactive
    // immediately and fills that page progressively.
    constexpr std::size_t batch_limit = 2;
    const std::size_t count = std::min(state->ready.size(), batch_limit);
    for (std::size_t index = 0; index < count; ++index)
    {
      ready.emplace_back(std::move(state->ready.front()));
      state->ready.pop_front();
    }
  }
  std::size_t published = 0;
  for (Game& game : ready)
  {
    if (game.canonical_path.empty())
      game.canonical_path = CanonicalLibraryPath(game.path);
    if (game.canonical_path.starts_with("usb:"))
    {
      const std::size_t slash = game.canonical_path.find('/');
      const std::string usb_id =
          game.canonical_path.substr(4, slash == std::string::npos ? std::string::npos : slash - 4);
      if (m_unavailable_usb_ids.contains(usb_id))
        continue;
    }
    if (!game.installed_nand)
    {
      if (game.metadata && game.metadata->GetPlatform() == DiscIO::Platform::WiiWAD &&
          game.title_id != 0)
      {
        std::erase_if(m_games, [&](Game& existing) {
          if (!existing.installed_nand || existing.title_id != game.title_id)
            return false;
          if (existing.cover)
            SDL_DestroyTexture(existing.cover);
          return true;
        });
      }
      AssignStableIdentity(&game);
      MigrateLegacyGameState(&game);
      game.config_override_path = EntryGameIniPath(game);
    }
    const std::string alias = m_store.Get("Alias/" + game.key);
    if (!alias.empty())
    {
      game.title = alias;
      game.has_custom_title = true;
    }
    game.played = m_store.GetInt("Recent/" + game.key, 0);
    game.has_game_config = RegularFileExists(GameIniPath(game));

    const auto existing = std::ranges::find(m_games, game.key, &Game::key);
    if (existing == m_games.end())
    {
      m_games.emplace_back(std::move(game));
      ++published;
    }
    else
    {
      const bool metadata_changed =
          game.metadata_refreshed || (!game.fingerprint.empty() && !existing->fingerprint.empty() &&
                                      game.fingerprint != existing->fingerprint);
      if (metadata_changed)
      {
        if (existing->cover)
          SDL_DestroyTexture(existing->cover);
      }
      else
      {
        game.cover = existing->cover;
        game.cover_use = existing->cover_use;
        game.cover_request = existing->cover_request;
        game.cover_loaded_at = existing->cover_loaded_at;
        game.cover_attempted = existing->cover_attempted;
        game.cover_queued = existing->cover_queued;
      }
      *existing = std::move(game);
      ++published;
    }
  }
  if (published != 0)
  {
    state->unsorted_published += published;
    const std::size_t first_page = static_cast<std::size_t>(std::max(1, GridPageSize()));
    if (m_games.size() <= first_page || state->unsorted_published >= 16)
    {
      SortGames();
      state->unsorted_published = 0;
    }
    else
    {
      RebuildVisibleGames();
    }
  }

  bool queue_empty = false;
  {
    std::lock_guard lock(state->mutex);
    queue_empty = state->ready.empty();
  }
  if (!queue_empty)
  {
    SDL_Event wake{};
    wake.type = SDL_USEREVENT;
    wake.user.code = 0x444c5343;
    SDL_PushEvent(&wake);
  }
  if (!state->complete.load(std::memory_order_acquire) || !queue_empty)
    return;
  if (m_library_scan_thread.joinable())
    m_library_scan_thread.join();
  if (!state->cancel.load(std::memory_order_acquire))
  {
    SaveLibraryIdentities();
    SortGames();
  }
  m_library_scan.reset();
  m_library_refresh_requested = false;
}

void Launcher::ScanGames()
{
  StartGameScan(m_sources, true);
}

void Launcher::SortGames()
{
  std::ranges::sort(m_games, [&](const Game& left, const Game& right) {
    if (m_sort_mode == SortMode::RecentlyPlayed && left.played != right.played)
      return left.played > right.played;
    if (m_sort_mode == SortMode::RecentlyAdded && left.modified != right.modified)
      return left.modified > right.modified;
    return Lower(left.title) < Lower(right.title);
  });
  RebuildVisibleGames();
}

void Launcher::RenderMessage(std::string_view title, std::span<const std::string> lines,
                             bool localize_lines)
{
  const std::string localized_title{m_localization.Translate(title)};
  std::string message;
  for (const std::string& line : lines)
  {
    if (!message.empty())
      message += "\n\n";
    const std::string_view displayed_line =
        localize_lines ? m_localization.Translate(line) : std::string_view(line);
    message.append(displayed_line);
  }
  if (message.empty())
    message = std::string(m_localization.Translate("Unknown error"));

  BeginScreenFx();
  while (BeginFrame())
  {
    SDL_Event event{};
    bool close = false;
    while (PollEvent(&event))
    {
      int touch_x = 0;
      int touch_y = 0;
      const TouchKind touch = FeedTouch(event, &touch_x, &touch_y);
      if (event.type == SDL_CONTROLLERBUTTONDOWN &&
          (event.cbutton.button == BUTTON_CONFIRM || event.cbutton.button == BUTTON_CANCEL))
        close = true;
      if (event.type == SDL_KEYDOWN &&
          (event.key.keysym.sym == SDLK_RETURN || event.key.keysym.sym == SDLK_ESCAPE))
        close = true;
      if (touch == TouchKind::Tap)
        close = true;
    }
    if (close)
      return;
    ClearBackground();
    const int panel_width = std::min(m_width - 96, 1080);
    const int maximum_panel_height = m_height - 80;
    const int body_width = panel_width - 96;
    const int line_height = std::max(32, FontHeight(m_font) + 8);
    const int maximum_lines = std::max(1, (maximum_panel_height - 170) / line_height);
    const std::vector<std::string> wrapped = WrapText(m_font, body_width, maximum_lines, message);
    const int body_height = std::max(line_height, static_cast<int>(wrapped.size()) * line_height);
    const int panel_height = std::clamp(170 + body_height, 250, maximum_panel_height);
    const int panel_x = (m_width - panel_width) / 2;
    const int panel_y = (m_height - panel_height) / 2;
    GlassPanel(panel_x, panel_y, panel_width, panel_height);
    DrawText(m_font_large, panel_x + 28, panel_y + 22,
             Ellipsize(m_font_large, localized_title, panel_width - 48), m_text);
    FillRect(panel_x + 28, panel_y + 72, panel_width - 56, 1, SDL_Color{255, 255, 255, 24});

    const int body_y = panel_y + 88;
    const int footer_y = panel_y + panel_height - 30;
    const SDL_Rect body_clip{panel_x + 40, body_y - 4, panel_width - 80,
                             std::max(1, footer_y - body_y - 8)};
    SDL_RenderSetClipRect(m_renderer, &body_clip);
    for (std::size_t index = 0; index < wrapped.size(); ++index)
    {
      DrawTextCentered(m_font, m_width / 2, body_y + static_cast<int>(index) * line_height,
                       wrapped[index], m_text);
    }
    SDL_RenderSetClipRect(m_renderer, nullptr);
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 1> continue_hint =
        {std::pair{"A", "Continue"}};
    DrawFooter(continue_hint, footer_y);
    DrawFadeIn();
    SDL_RenderPresent(m_renderer);
    WaitForNextFrame();
  }
}

void Launcher::Toast(std::string message, int milliseconds)
{
  // Toasts are launcher-owned status messages. Translate here, at the semantic boundary, so
  // arbitrary strings passed to the low-level text renderer are never treated as translation keys.
  message = std::string(m_localization.Translate(message));
  const Uint32 deadline = SDL_GetTicks() + milliseconds;
  do
  {
    ClearBackground();
    const int width = std::min(m_width - 80, std::max(820, TextWidth(m_font, message) + 72));
    const int height = 120;
    const int x = (m_width - width) / 2;
    const int y = (m_height - height) / 2;
    GlassPanel(x, y, width, height);
    DrawTextCentered(m_font, m_width / 2, y + 46, message, m_text);
    SDL_RenderPresent(m_renderer);
    WaitForNextFrame(true);
  } while (m_running && appletMainLoop() && !SDL_TICKS_PASSED(SDL_GetTicks(), deadline));
}

bool Launcher::Confirm(std::string_view title, std::span<const std::string> lines,
                       bool localize_lines)
{
  const std::string_view localized_title = m_localization.Translate(title);
  constexpr std::string_view yes_label = "Yes";
  constexpr std::string_view no_label = "No";
  const int panel_width = std::min(m_width - 96, 1080);
  const int body_width = panel_width - 96;
  const int line_height = std::max(30, FontHeight(m_font) + 5);
  std::vector<std::string> wrapped_lines;
  for (const std::string& line : lines)
  {
    const std::string_view displayed_line =
        localize_lines ? m_localization.Translate(line) : std::string_view(line);
    if (displayed_line.empty())
    {
      wrapped_lines.emplace_back();
      continue;
    }
    std::vector<std::string> wrapped = WrapText(m_font, body_width, 4, displayed_line);
    wrapped_lines.insert(wrapped_lines.end(), std::make_move_iterator(wrapped.begin()),
                         std::make_move_iterator(wrapped.end()));
  }
  const int panel_height =
      std::clamp(218 + static_cast<int>(wrapped_lines.size()) * line_height, 290, m_height - 64);
  const int panel_x = (m_width - panel_width) / 2;
  const int panel_y = (m_height - panel_height) / 2;
  const int button_width = 210;
  const int button_height = 56;
  const int button_y = panel_y + panel_height - button_height - 22;
  const int yes_x = m_width / 2 - button_width - 18;
  const int no_x = m_width / 2 + 18;
  BeginScreenFx();
  while (BeginFrame())
  {
    SDL_Event event{};
    while (PollEvent(&event))
    {
      int touch_x = 0;
      int touch_y = 0;
      const TouchKind touch = FeedTouch(event, &touch_x, &touch_y);
      if (event.type == SDL_CONTROLLERBUTTONDOWN)
      {
        if (event.cbutton.button == BUTTON_CONFIRM)
          return true;
        else if (event.cbutton.button == BUTTON_CANCEL)
          return false;
      }
      if (event.type == SDL_KEYDOWN)
      {
        if (event.key.keysym.sym == SDLK_RETURN)
          return true;
        else if (event.key.keysym.sym == SDLK_ESCAPE)
          return false;
      }
      if (touch == TouchKind::Tap)
      {
        if (touch_y >= button_y && touch_y < button_y + button_height)
        {
          if (touch_x >= yes_x && touch_x < yes_x + button_width)
            return true;
          if (touch_x >= no_x && touch_x < no_x + button_width)
            return false;
        }
      }
    }
    ClearBackground();
    GlassPanel(panel_x, panel_y, panel_width, panel_height);
    DrawText(m_font_large, panel_x + 28, panel_y + 22,
             Ellipsize(m_font_large, localized_title, panel_width - 48),
             SDL_Color{238, 135, 135, 255});
    FillRect(panel_x + 28, panel_y + 72, panel_width - 56, 1, SDL_Color{255, 255, 255, 24});
    int y = panel_y + 88;
    const int body_bottom = button_y - 18;
    SDL_Rect body_clip{panel_x + 36, y - 4, panel_width - 72, std::max(1, body_bottom - y)};
    SDL_RenderSetClipRect(m_renderer, &body_clip);
    for (const std::string& line : wrapped_lines)
    {
      DrawTextCentered(m_font, m_width / 2, y, line, m_text);
      y += line_height;
    }
    SDL_RenderSetClipRect(m_renderer, nullptr);
    const auto draw_choice = [&](int x, std::string_view button, std::string_view label) {
      const std::string_view shown = m_localization.Translate(label);
      SDL_Texture* const glyph = ButtonGlyph(button);
      int glyph_width = 0;
      int glyph_height = 0;
      if (glyph)
      {
        SDL_QueryTexture(glyph, nullptr, nullptr, &glyph_width, &glyph_height);
        glyph_width /= GLYPH_SUPERSAMPLE;
        glyph_height /= GLYPH_SUPERSAMPLE;
      }
      const int text_width = TextWidth(m_font, shown);
      const int left = x + (button_width - glyph_width - 8 - text_width) / 2;
      if (glyph)
      {
        SDL_Rect icon{left, button_y + (button_height - glyph_height) / 2, glyph_width,
                      glyph_height};
        SDL_RenderCopy(m_renderer, glyph, nullptr, &icon);
      }
      DrawText(m_font, left + glyph_width + 8,
               button_y + (button_height - FontHeight(m_font)) / 2, shown, m_text);
    };
    RoundedPanel(yes_x, button_y, button_width, button_height, SDL_Color{115, 44, 51, 255},
                 SDL_Color{235, 125, 125, 255}, 6);
    draw_choice(yes_x, "A", yes_label);
    RoundedPanel(no_x, button_y, button_width, button_height, m_focus, m_selection, 6);
    draw_choice(no_x, "B", no_label);
    DrawFadeIn();
    SDL_RenderPresent(m_renderer);
    WaitForNextFrame();
  }
  return false;
}

int Launcher::Dropdown(std::string_view title, const std::vector<std::string>& choices, int current,
                       bool localize_title, bool localize_choices)
{
  if (choices.empty())
    return -1;
  const int count = static_cast<int>(choices.size());
  int selection = std::clamp(current, 0, count - 1);
  int top = 0;
  const int row_height = SettingsRowHeight() + 8;
  const int visible = std::min(count, std::max(1, (m_height - 240) / row_height));
  const int panel_width = std::min(900, m_width - 64);
  const int panel_height = 140 + visible * row_height;
  const int panel_x = (m_width - panel_width) / 2;
  const int panel_y = (m_height - panel_height) / 2;
  const int list_y = panel_y + 80;
  BeginScreenFx();
  while (BeginFrame())
  {
    SDL_Event event{};
    while (PollEvent(&event))
    {
      int touch_x = 0;
      int touch_y = 0;
      const TouchKind touch = FeedTouch(event, &touch_x, &touch_y);
      if (TouchScrollList(touch, &selection, &top, count, visible))
        continue;
      if (touch == TouchKind::Tap)
      {
        if (touch_x >= panel_x + 8 && touch_x < panel_x + panel_width - 8)
        {
          for (int row = 0; row < visible && top + row < count; ++row)
          {
            const int row_y = list_y + row * row_height;
            if (touch_y >= row_y && touch_y < row_y + row_height)
              return top + row;
          }
        }
        continue;
      }
      const int direction = EventNavigation(event);
      if (direction)
        selection = (selection + direction + count) % count;
      if (event.type == SDL_CONTROLLERBUTTONDOWN)
      {
        if (event.cbutton.button == BUTTON_CONFIRM)
          return selection;
        if (event.cbutton.button == BUTTON_CANCEL)
          return current;
      }
      else if (event.type == SDL_KEYDOWN)
      {
        if (event.key.keysym.sym == SDLK_RETURN)
          return selection;
        if (event.key.keysym.sym == SDLK_ESCAPE)
          return current;
      }
      if (selection < top)
        top = selection;
      if (selection >= top + visible)
        top = selection - visible + 1;
    }

    ClearBackground();
    GlassPanel(panel_x, panel_y, panel_width, panel_height);
    const std::string_view displayed_title =
        localize_title ? m_localization.Translate(title) : title;
    DrawScrollingTextLeft(m_font_large, panel_x + 28, panel_y + 22, panel_width - 56,
                          displayed_title, m_text);
    FillRect(panel_x + 28, panel_y + 66, panel_width - 56, 1, SDL_Color{255, 255, 255, 24});
    for (int row = 0; row < visible && top + row < count; ++row)
    {
      const int index = top + row;
      const int y = list_y + row * row_height;
      const bool selected = index == selection;
      if (selected)
        DrawRowHighlight(panel_x + 8, y, panel_width - 16, row_height - 4);
      const std::string_view displayed_choice =
          localize_choices ? m_localization.Translate(choices[index]) : choices[index];
      const int text_y = y + (row_height - FontHeight(m_font)) / 2;
      if (selected)
        DrawScrollingTextLeft(m_font, panel_x + 32, text_y, panel_width - 76, displayed_choice,
                              m_value);
      else
        DrawText(m_font, panel_x + 32, text_y,
                 Ellipsize(m_font, displayed_choice, panel_width - 76), m_text);
    }
    if (count > visible)
    {
      const int track_height = visible * row_height;
      const int thumb_height = std::max(12, track_height * visible / count);
      FillRect(panel_x + panel_width - 18, list_y, 3, track_height, m_card);
      FillRect(panel_x + panel_width - 18,
               list_y + (track_height - thumb_height) * top / std::max(1, count - visible), 3,
               thumb_height, m_selection);
    }
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 2> footer = {
        std::pair{"A", "Select"}, std::pair{"B", "Back"}};
    DrawFooter(footer, panel_y + panel_height - 28);
    DrawFadeIn();
    SDL_RenderPresent(m_renderer);
    WaitForNextFrame();
  }
  return current;
}

int Launcher::SelectChoice(std::string_view title, std::span<const std::string_view> choices,
                           int current, int delta)
{
  if (choices.empty())
    return -1;
  const int count = static_cast<int>(choices.size());
  current = std::clamp(current, 0, count - 1);
  if (delta != 0 || count <= 2)
    return (current + (delta < 0 ? -1 : 1) + count) % count;

  std::vector<std::string> labels;
  labels.reserve(choices.size());
  for (const std::string_view choice : choices)
    labels.emplace_back(choice);
  return Dropdown(title, labels, current);
}

int Launcher::RunRows(std::string_view title, std::string_view context,
                      const std::function<std::vector<Row>()>& rows_provider,
                      const std::function<bool(int, int)>& action, bool touch_activates_full_row,
                      std::function<bool(int)> reset, std::function<bool(int)> resettable)
{
  const std::string position_key = std::string(title) + '\n' + std::string(context);
  const auto saved_position = m_row_positions.find(position_key);
  int selection = saved_position == m_row_positions.end() ? 0 : saved_position->second.first;
  int top = saved_position == m_row_positions.end() ? 0 : saved_position->second.second;
  const auto finish = [&](int result) {
    m_row_positions[position_key] = {selection, top};
    return result;
  };
  const int row_height = SettingsRowHeight();
  const int list_top = SettingsListY();
  const int visible = std::max(1, (m_height - list_top - SettingsFooterReserve()) / row_height);
  BeginScreenFx();
  while (BeginFrame())
  {
    std::vector<Row> rows = rows_provider();
    if (rows.empty())
      return finish(-1);
    selection = std::clamp(selection, 0, static_cast<int>(rows.size()) - 1);
    SDL_Event event{};
    while (PollEvent(&event))
    {
      int touch_x = 0;
      int touch_y = 0;
      const TouchKind touch = FeedTouch(event, &touch_x, &touch_y);
      if (TouchScrollList(touch, &selection, &top, static_cast<int>(rows.size()), visible))
        continue;
      if ((touch == TouchKind::SwipeLeft || touch == TouchKind::SwipeRight) &&
          rows[selection].enabled && rows[selection].adjustable)
      {
        action(selection, touch == TouchKind::SwipeLeft ? -1 : 1);
        continue;
      }
      if (touch == TouchKind::Tap)
      {
        if (touch_y < TopBarHeight() || touch_y >= m_height - 40)
          return finish(-1);
        const int index = top + (touch_y - list_top) / row_height;
        const int column_width = std::min(980, m_width - 180);
        const int column_x = (m_width - column_width) / 2;
        // Only taps on the visible rows count.
        if (touch_x >= column_x && touch_x < column_x + column_width && touch_y >= list_top &&
            touch_y < list_top + visible * row_height && index >= 0 &&
            index < static_cast<int>(rows.size()))
        {
          selection = index;
          if ((touch_activates_full_row || touch_x >= column_x + column_width / 2) &&
              rows[index].enabled && action(index, 0))
            return finish(index);
        }
        continue;
      }
      const int direction = EventNavigation(event);
      if (direction != 0)
      {
        int next = selection;
        do
        {
          next = (next + direction + static_cast<int>(rows.size())) % rows.size();
        } while (!rows[next].enabled && next != selection);
        if (next != selection)
          selection = next;
      }
      if (event.type == SDL_CONTROLLERBUTTONDOWN)
      {
        if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT && rows[selection].enabled &&
            rows[selection].adjustable)
          action(selection, -1);
        else if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT &&
                 rows[selection].enabled && rows[selection].adjustable)
          action(selection, 1);
        else if (event.cbutton.button == BUTTON_SETTINGS)
        {
          const SettingHelpInfo info = SettingHelpFor(title, rows[selection]);
          const std::string_view current = rows[selection].value == ">" ?
                                               std::string_view{} :
                                               std::string_view(rows[selection].value);
          ShowInfoCard(title, rows[selection].label, info.kind, info.description, current,
                       SettingScope(title, context), rows[selection].localize_label,
                       rows[selection].localize_value);
          BeginScreenFx();
        }
        else if (event.cbutton.button == SDL_CONTROLLER_BUTTON_X && reset &&
                 rows[selection].enabled &&
                 (rows[selection].adjustable || (resettable && resettable(selection))))
        {
          if (reset(selection))
          {
            Toast("Setting reset to default", 550);
            BeginScreenFx();
          }
        }
        else if (event.cbutton.button == BUTTON_CONFIRM && rows[selection].enabled)
        {
          if (action(selection, 0))
            return finish(selection);
        }
        else if (event.cbutton.button == BUTTON_CANCEL)
          return finish(-1);
      }
      else if (event.type == SDL_KEYDOWN)
      {
        if (event.key.keysym.sym == SDLK_LEFT && rows[selection].enabled &&
            rows[selection].adjustable)
          action(selection, -1);
        else if (event.key.keysym.sym == SDLK_RIGHT && rows[selection].enabled &&
                 rows[selection].adjustable)
          action(selection, 1);
        else if (event.key.keysym.sym == SDLK_x)
        {
          const SettingHelpInfo info = SettingHelpFor(title, rows[selection]);
          const std::string_view current = rows[selection].value == ">" ?
                                               std::string_view{} :
                                               std::string_view(rows[selection].value);
          ShowInfoCard(title, rows[selection].label, info.kind, info.description, current,
                       SettingScope(title, context), rows[selection].localize_label,
                       rows[selection].localize_value);
          BeginScreenFx();
        }
        else if ((event.key.keysym.sym == SDLK_y || event.key.keysym.sym == SDLK_DELETE) && reset &&
                 rows[selection].enabled &&
                 (rows[selection].adjustable || (resettable && resettable(selection))))
        {
          if (reset(selection))
          {
            Toast("Setting reset to default", 550);
            BeginScreenFx();
          }
        }
        else if (event.key.keysym.sym == SDLK_RETURN && rows[selection].enabled)
        {
          if (action(selection, 0))
            return finish(selection);
        }
        else if (event.key.keysym.sym == SDLK_ESCAPE)
          return finish(-1);
      }
    }
    if (selection < top)
      top = selection;
    if (selection >= top + visible)
      top = selection - visible + 1;

    ClearBackground();
    DrawHeader(title, context);
    const int column_width = std::min(980, m_width - 180);
    const int column_x = (m_width - column_width) / 2;
    const int label_x = column_x + 40;
    const int value_x = column_x + column_width - 40;
    GlassPanel(column_x - 12, list_top - 10, column_width + 24,
               std::min(visible, static_cast<int>(rows.size()) - top) * row_height + 18);
    const float target_y = static_cast<float>(list_top + (selection - top) * row_height + 1);
    m_highlight_y = (!m_animations || m_highlight_y < 0.0f) ?
                        target_y :
                        m_highlight_y + (target_y - m_highlight_y) * 0.30f;
    DrawRowHighlight(column_x, static_cast<int>(m_highlight_y), column_width, row_height - 2);
    for (int row = 0; row < visible && top + row < static_cast<int>(rows.size()); ++row)
    {
      const int index = top + row;
      const int slot_y = list_top + row * row_height;
      const bool current = index == selection;
      const SDL_Color label_color = !rows[index].enabled    ? m_dim :
                                    rows[index].destructive ? SDL_Color{255, 120, 120, 255} :
                                    current                 ? m_value :
                                                              m_text;
      const SDL_Color value_color = rows[index].value_color ? *rows[index].value_color :
                                    !rows[index].enabled    ? m_dim :
                                    current                 ? m_value :
                                                              m_dim;
      const std::string_view localized_label = rows[index].localize_label ?
                                                   m_localization.Translate(rows[index].label) :
                                                   std::string_view(rows[index].label);
      const std::string_view displayed_value = rows[index].localize_value ?
                                                   m_localization.Translate(rows[index].value) :
                                                   std::string_view(rows[index].value);
      DrawSettingsRowText(localized_label, displayed_value, slot_y, column_width, label_x, value_x,
                          current, label_color, value_color, false, row_height);
    }
    if (static_cast<int>(rows.size()) > visible)
    {
      const int track_height = visible * row_height;
      const int track_x = column_x + column_width + 16;
      const int track_y = list_top - 2;
      FillRect(track_x, track_y, 4, track_height, SDL_Color{40, 44, 54, 255});
      const int thumb_height = std::max(16, track_height * visible / static_cast<int>(rows.size()));
      const int denominator = std::max(1, static_cast<int>(rows.size()) - visible);
      FillRect(track_x, track_y + (track_height - thumb_height) * top / denominator, 4,
               thumb_height, m_selection);
    }
    // Only show hints that apply to the selected row.
    const bool can_adjust = rows[selection].enabled && rows[selection].adjustable;
    const bool can_reset = reset && rows[selection].enabled &&
                           (rows[selection].adjustable || (resettable && resettable(selection)));
    std::array<std::pair<std::string_view, std::string_view>, 6> footer{};
    int hint_count = 0;
    if (can_adjust)
    {
      // The empty label is the deliberate glyph pair: Left and Right share one caption.
      footer[hint_count++] = {"Left", ""};
      footer[hint_count++] = {"Right", "Change"};
    }
    if (rows[selection].enabled)
      footer[hint_count++] = {"A", "Choose"};
    footer[hint_count++] = {"X", "Info"};
    if (can_reset)
      footer[hint_count++] = {"Y", "Reset"};
    footer[hint_count++] = {"B", "Back"};
    DrawFooter(std::span(footer).first(hint_count));
    DrawFadeIn();
    SDL_RenderPresent(m_renderer);
    WaitForNextFrame();
  }
  return finish(-1);
}

std::string Launcher::CoverPath(const Game& game) const
{
  return std::string(COVER_DIRECTORY) + "/" + game.key + ".png";
}

CoverDecodeResult Launcher::DecodeCover(const CoverDecodeJob& job)
{
  CoverDecodeResult result;
  result.key = job.key;
  result.request = job.request;
  result.epoch = job.epoch;
  SDL_Surface* surface = nullptr;
  bool custom_exists = RegularFileExists(job.custom_path);
  // Finish an interrupted atomic import before loading the cover. The normal path performs only
  // the existing cover stat; the backup check is needed solely when the active file is absent.
  if (!custom_exists && RegularFileExists(job.custom_path + ".old"))
  {
    (void)RecoverAtomicFile(job.custom_path);
    custom_exists = RegularFileExists(job.custom_path);
  }
  if (custom_exists)
    surface = IMG_Load(job.custom_path.c_str());
  if (!surface && job.metadata)
  {
    const UICommon::GameCover& cover = job.metadata->GetCoverImage();
    if (!cover.empty() && cover.buffer.size() <= static_cast<std::size_t>(INT_MAX))
    {
      if (SDL_RWops* stream =
              SDL_RWFromConstMem(cover.buffer.data(), static_cast<int>(cover.buffer.size())))
        surface = IMG_Load_RW(stream, 1);
    }
  }
  if (!surface || surface->w < 1 || surface->h < 1 || surface->w > 8192 || surface->h > 8192 ||
      static_cast<std::uint64_t>(surface->w) * static_cast<std::uint64_t>(surface->h) >
          16ULL * 1024 * 1024)
  {
    if (surface)
      SDL_FreeSurface(surface);
    return result;
  }
  constexpr int maximum_width = 360;
  constexpr int maximum_height = 540;
  int width = surface->w;
  int height = surface->h;
  if (width > maximum_width)
  {
    height = static_cast<int>(static_cast<long long>(height) * maximum_width / width);
    width = maximum_width;
  }
  if (height > maximum_height)
  {
    width = static_cast<int>(static_cast<long long>(width) * maximum_height / height);
    height = maximum_height;
  }
  if (width != surface->w || height != surface->h)
  {
    SDL_Surface* scaled = SDL_CreateRGBSurfaceWithFormat(0, std::max(1, width), std::max(1, height),
                                                         32, SDL_PIXELFORMAT_RGBA32);
    if (!scaled)
    {
      SDL_FreeSurface(surface);
      return result;
    }
    SDL_BlendMode blend = SDL_BLENDMODE_NONE;
    SDL_GetSurfaceBlendMode(surface, &blend);
    SDL_SetSurfaceBlendMode(surface, SDL_BLENDMODE_NONE);
    const bool copied = SDL_BlitScaled(surface, nullptr, scaled, nullptr) == 0;
    SDL_SetSurfaceBlendMode(surface, blend);
    SDL_FreeSurface(surface);
    if (!copied)
    {
      SDL_FreeSurface(scaled);
      return result;
    }
    surface = scaled;
  }
  SDL_Surface* rgba = SDL_ConvertSurfaceFormat(surface, SDL_PIXELFORMAT_RGBA32, 0);
  SDL_FreeSurface(surface);
  if (!rgba)
    return result;
  const bool must_lock = SDL_MUSTLOCK(rgba);
  if (must_lock && SDL_LockSurface(rgba) != 0)
  {
    SDL_FreeSurface(rgba);
    return result;
  }
  result.width = rgba->w;
  result.height = rgba->h;
  result.pixels.resize(static_cast<std::size_t>(result.width) * result.height * 4);
  for (int row = 0; row < result.height; ++row)
  {
    std::memcpy(result.pixels.data() + static_cast<std::size_t>(row) * result.width * 4,
                static_cast<const Uint8*>(rgba->pixels) + static_cast<std::size_t>(row) * rgba->pitch,
                static_cast<std::size_t>(result.width) * 4);
  }
  if (must_lock)
    SDL_UnlockSurface(rgba);
  SDL_FreeSurface(rgba);
  return result;
}

void Launcher::CoverDecodeThread()
{
  for (;;)
  {
    CoverDecodeJob job;
    {
      std::unique_lock lock(m_cover_decode_mutex);
      m_cover_decode_condition.wait(lock, [&] {
        return m_cover_decode_stop ||
               (!m_cover_decode_jobs.empty() && m_cover_decode_ready.size() < COVER_READY_LIMIT);
      });
      if (m_cover_decode_stop)
        return;
      job = std::move(m_cover_decode_jobs.front());
      m_cover_decode_jobs.pop_front();
    }

    CoverDecodeResult result = DecodeCover(job);
    bool publish = false;
    {
      std::lock_guard lock(m_cover_decode_mutex);
      if (!m_cover_decode_stop && job.epoch == m_cover_decode_epoch)
      {
        m_cover_decode_ready.emplace_back(std::move(result));
        publish = true;
      }
    }
    if (publish)
    {
      SDL_Event wake{};
      wake.type = SDL_USEREVENT;
      wake.user.code = 0x434f5652;  // COVR: a decoded cover is ready for SDL upload.
      SDL_PushEvent(&wake);
    }
  }
}

void Launcher::StartCoverDecodeWorker()
{
  std::lock_guard lock(m_cover_decode_mutex);
  if (m_cover_decode_started)
    return;
  m_cover_decode_stop = false;
  m_cover_decode_started = true;
  m_cover_decode_thread = std::thread(&Launcher::CoverDecodeThread, this);
}

void Launcher::StopCoverDecodeWorker()
{
  {
    std::lock_guard lock(m_cover_decode_mutex);
    if (!m_cover_decode_started)
      return;
    m_cover_decode_stop = true;
    m_cover_decode_jobs.clear();
    m_cover_decode_ready.clear();
  }
  m_cover_decode_condition.notify_all();
  if (m_cover_decode_thread.joinable())
    m_cover_decode_thread.join();
  std::lock_guard lock(m_cover_decode_mutex);
  m_cover_decode_started = false;
}

void Launcher::CancelQueuedCoverDecodes()
{
  {
    std::lock_guard lock(m_cover_decode_mutex);
    ++m_cover_decode_epoch;
    m_cover_decode_jobs.clear();
    m_cover_decode_ready.clear();
  }
  for (Game& game : m_games)
  {
    game.cover_queued = false;
    game.cover_request = 0;
  }
  m_cover_decode_condition.notify_all();
}

void Launcher::QueueCoverDecode(Game* game, bool priority)
{
  if (!game || game->cover || game->cover_attempted)
    return;
  if (game->cover_queued)
  {
    if (priority)
    {
      std::lock_guard lock(m_cover_decode_mutex);
      const auto found = std::ranges::find(m_cover_decode_jobs, game->cover_request,
                                           &CoverDecodeJob::request);
      if (found != m_cover_decode_jobs.end() && found != m_cover_decode_jobs.begin())
      {
        CoverDecodeJob job = std::move(*found);
        m_cover_decode_jobs.erase(found);
        m_cover_decode_jobs.emplace_front(std::move(job));
        m_cover_decode_condition.notify_one();
      }
    }
    return;
  }
  if (m_cover_decode_budget <= 0)
    return;
  --m_cover_decode_budget;

  CoverDecodeJob job;
  job.key = game->key;
  job.custom_path = CoverPath(*game);
  job.metadata = game->metadata;
  job.request = ++m_cover_request_serial;
  game->cover_request = job.request;
  game->cover_queued = true;

  CoverDecodeJob dropped;
  bool did_drop = false;
  {
    std::lock_guard lock(m_cover_decode_mutex);
    job.epoch = m_cover_decode_epoch;
    if (m_cover_decode_jobs.size() >= COVER_JOB_LIMIT)
    {
      dropped = std::move(m_cover_decode_jobs.back());
      m_cover_decode_jobs.pop_back();
      did_drop = true;
    }
    if (priority)
      m_cover_decode_jobs.emplace_front(std::move(job));
    else
      m_cover_decode_jobs.emplace_back(std::move(job));
  }
  if (did_drop)
  {
    const auto old = std::ranges::find(m_games, dropped.key, &Game::key);
    if (old != m_games.end() && old->cover_request == dropped.request)
    {
      old->cover_queued = false;
      old->cover_request = 0;
    }
  }
  m_cover_decode_condition.notify_one();
}

SDL_Texture* Launcher::UploadCoverTexture(const CoverDecodeResult& result)
{
  if (!m_renderer || result.width < 1 || result.height < 1 || result.pixels.empty())
    return nullptr;
  SDL_Texture* texture = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_RGBA32,
                                           SDL_TEXTUREACCESS_STATIC, result.width, result.height);
  if (texture && SDL_UpdateTexture(texture, nullptr, result.pixels.data(), result.width * 4) != 0)
  {
    SDL_DestroyTexture(texture);
    texture = nullptr;
  }
  if (!texture)
  {
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormatFrom(
        const_cast<Uint8*>(result.pixels.data()), result.width, result.height, 32,
        result.width * 4, SDL_PIXELFORMAT_RGBA32);
    if (surface)
    {
      texture = SDL_CreateTextureFromSurface(m_renderer, surface);
      SDL_FreeSurface(surface);
    }
  }
  if (texture)
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
  return texture;
}

void Launcher::PumpCoverDecodeResults()
{
  int uploads = 0;
  int processed = 0;
  while (processed < 12)
  {
    CoverDecodeResult result;
    {
      std::lock_guard lock(m_cover_decode_mutex);
      if (m_cover_decode_ready.empty())
        break;
      if (!m_cover_decode_ready.front().pixels.empty() && uploads >= COVER_UPLOAD_BUDGET)
        break;
      result = std::move(m_cover_decode_ready.front());
      m_cover_decode_ready.pop_front();
    }
    m_cover_decode_condition.notify_one();
    ++processed;
    const auto game = std::ranges::find(m_games, result.key, &Game::key);
    if (game == m_games.end() || game->cover_request != result.request)
      continue;
    game->cover_queued = false;
    game->cover_attempted = true;
    if (result.pixels.empty())
      continue;
    SDL_Texture* texture = UploadCoverTexture(result);
    ++uploads;
    if (!texture)
      continue;
    if (std::ranges::count_if(m_games, [](const Game& item) { return item.cover != nullptr; }) >=
        COVER_CACHE_LIMIT)
    {
      EvictCover();
    }
    game->cover = texture;
    game->cover_use = ++m_cover_use;
    game->cover_loaded_at = SDL_GetTicks();
  }
}

SDL_Texture* Launcher::LoadScaledTexture(const std::string& path, int width, int height)
{
  SDL_Surface* source = IMG_Load(path.c_str());
  if (!source)
    return nullptr;
  // Decode at output resolution; callers pass logical sizes.
  const int decode_width = std::max(1, static_cast<int>(std::lround(width * m_ui_scale)));
  const int decode_height = std::max(1, static_cast<int>(std::lround(height * m_ui_scale)));
  SDL_Surface* scaled = SDL_CreateRGBSurfaceWithFormat(0, decode_width, decode_height, 32,
                                                       SDL_PIXELFORMAT_RGBA32);
  if (!scaled)
  {
    SDL_FreeSurface(source);
    return nullptr;
  }
  SDL_BlendMode blend = SDL_BLENDMODE_NONE;
  SDL_GetSurfaceBlendMode(source, &blend);
  SDL_SetSurfaceBlendMode(source, SDL_BLENDMODE_NONE);
  const bool copied = SDL_BlitScaled(source, nullptr, scaled, nullptr) == 0;
  SDL_SetSurfaceBlendMode(source, blend);
  SDL_FreeSurface(source);
  if (!copied)
  {
    SDL_FreeSurface(scaled);
    return nullptr;
  }
  SDL_Texture* texture = SDL_CreateTextureFromSurface(m_renderer, scaled);
  SDL_FreeSurface(scaled);
  if (texture)
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
  return texture;
}

void Launcher::EvictCover()
{
  Game* victim = nullptr;
  for (Game& game : m_games)
  {
    if (game.cover && (!victim || game.cover_use < victim->cover_use))
      victim = &game;
  }
  if (!victim)
    return;
  SDL_DestroyTexture(victim->cover);
  victim->cover = nullptr;
  victim->cover_use = 0;
  victim->cover_attempted = false;
}

void Launcher::EnsureCover(Game* game, bool priority)
{
  if (!game)
    return;
  if (game->cover)
  {
    game->cover_use = ++m_cover_use;
    return;
  }
  QueueCoverDecode(game, priority);
}

void Launcher::ReloadCover(Game* game)
{
  if (!game)
    return;
  if (game->cover)
    SDL_DestroyTexture(game->cover);
  game->cover = nullptr;
  game->cover_use = 0;
  game->cover_request = 0;
  game->cover_attempted = false;
  game->cover_queued = false;
  m_cover_decode_budget = 1;
  EnsureCover(game, true);
}

int Launcher::GridColumns() const
{
  return m_grid_columns;
}

int Launcher::GridRows() const
{
  return m_grid_rows;
}

int Launcher::GridPageSize() const
{
  return GridColumns() * GridRows();
}

int Launcher::GridNavigate(int selection, int dx, int dy) const
{
  if (m_visible_games.empty())
    return 0;
  const int columns = GridColumns();
  const int rows = GridRows();
  const int per_page = columns * rows;
  const int page = selection / per_page;
  const int position = selection % per_page;
  const int row = position / columns;
  const int column = position % columns;
  if (dx > 0)
  {
    if (column + 1 < columns && selection + 1 < static_cast<int>(m_visible_games.size()))
      return selection + 1;
    if ((page + 1) * per_page < static_cast<int>(m_visible_games.size()))
      return std::min((page + 1) * per_page + row * columns,
                      static_cast<int>(m_visible_games.size()) - 1);
  }
  else if (dx < 0)
  {
    if (column > 0)
      return selection - 1;
    if (page > 0)
      return std::min((page - 1) * per_page + row * columns + columns - 1,
                      static_cast<int>(m_visible_games.size()) - 1);
  }
  else if (dy > 0 && row + 1 < rows &&
           selection + columns < static_cast<int>(m_visible_games.size()))
  {
    return selection + columns;
  }
  else if (dy < 0 && row > 0)
  {
    return selection - columns;
  }
  return selection;
}

int Launcher::GridPage(int selection, int direction) const
{
  if (m_visible_games.empty())
    return 0;
  const int per_page = GridPageSize();
  const int maximum_page = (static_cast<int>(m_visible_games.size()) - 1) / per_page;
  const int page = std::clamp(selection / per_page + direction, 0, maximum_page);
  return std::min(page * per_page + selection % per_page,
                  static_cast<int>(m_visible_games.size()) - 1);
}

GridLayout Launcher::ComputeGridLayout()
{
  GridLayout layout;
  layout.gap_x = 32;
  layout.gap_y = 20;
  layout.title_height = m_show_titles ? FontHeight(m_font_small) : 0;
  layout.columns = GridColumns();
  layout.rows = GridRows();
  const int top_bar = TopBarHeight() + 18;
  const int footer = MeasureFooter(LIBRARY_FOOTER).height + 12;
  const int available_height = m_height - top_bar - footer;
  const int caption = layout.title_height ? layout.title_height + 8 : 0;
  const int maximum_cover_height = std::max(
      72, (available_height - (layout.rows - 1) * layout.gap_y - layout.rows * caption) /
              layout.rows);
  constexpr int margin = 56;
  const int automatic_width = maximum_cover_height * 2 / 3;
  const int maximum_cover_width =
      (m_width - margin * 2 - (layout.columns - 1) * layout.gap_x) / layout.columns;
  layout.cover_width = std::max(48, std::min(automatic_width, maximum_cover_width));
  layout.cover_height = std::min(maximum_cover_height, layout.cover_width * 3 / 2);
  layout.cover_width = layout.cover_height * 2 / 3;
  const int grid_width =
      layout.columns * layout.cover_width + (layout.columns - 1) * layout.gap_x;
  layout.x0 = (m_width - grid_width) / 2;
  const int grid_height =
      layout.rows * (layout.cover_height + caption) + (layout.rows - 1) * layout.gap_y;
  layout.y0 = top_bar + std::max(0, (available_height - grid_height) / 2);
  return layout;
}

int Launcher::GridHitTest(int x, int y, int page_start)
{
  const GridLayout layout = ComputeGridLayout();
  const int caption = layout.title_height ? layout.title_height + 8 : 0;
  const int row_stride = layout.RowStride();
  for (int row = 0; row < layout.rows; ++row)
  {
    for (int column = 0; column < layout.columns; ++column)
    {
      const int index = page_start + row * layout.columns + column;
      if (index >= static_cast<int>(m_visible_games.size()))
        continue;
      const int cell_x = layout.x0 + column * (layout.cover_width + layout.gap_x);
      const int cell_y = layout.y0 + row * row_stride;
      if (x >= cell_x - 4 && x < cell_x + layout.cover_width + 4 && y >= cell_y - 4 &&
          y < cell_y + layout.cover_height + caption)
        return index;
    }
  }
  return -1;
}

void Launcher::RenderGrid(int selection)
{
  ClearBackground();
  m_cover_decode_budget = COVER_REQUEST_BUDGET;
  if (Game* selected = VisibleGame(selection))
    EnsureCover(selected, true);
  const GridLayout layout = ComputeGridLayout();
  const int row_stride = layout.RowStride();
  const int per_page = GridPageSize();
  const int page_start = m_visible_games.empty() ? 0 : selection / per_page * per_page;
  const int page_count = m_visible_games.empty() ?
                             1 :
                             (static_cast<int>(m_visible_games.size()) + per_page - 1) / per_page;
  const int page = m_visible_games.empty() ? 1 : selection / per_page + 1;

  static constexpr std::array<std::string_view, 3> SORT_NAMES = {"A-Z", "Recently played",
                                                                 "Recently added"};
  const Game* const selected_game = VisibleGame(selection);
  const std::string library(m_localization.Translate("Library"));
  std::string eyebrow = library;
  if (selected_game)
    eyebrow += "  ·  " + GameLocationLabel(*selected_game);
  if (!m_active_collection.empty())
  {
    eyebrow += "  ·  " + (m_active_collection == "favorites" ?
                           std::string(m_localization.Translate("Favorites")) :
                           m_active_collection);
  }
  if (!m_search_query.empty())
    eyebrow += "  ·  " + std::string(m_localization.Translate("Search:")) + " " + m_search_query;
  const std::string summary =
      std::to_string(m_visible_games.empty() ? 0 : selection + 1) + " / " +
      std::to_string(m_visible_games.size()) + "  ·  " +
      std::string(m_localization.Translate("Page")) + " " + std::to_string(page) + " / " +
      std::to_string(page_count);
  const std::string sorting =
      std::string(m_localization.Translate("Sort:")) + " " +
      std::string(m_localization.Translate(SORT_NAMES[static_cast<int>(m_sort_mode)]));
  DrawPageHeader(selected_game ? std::string_view(selected_game->title) :
                                 std::string_view(library),
                 eyebrow, summary, sorting);

  const int cover_width = layout.cover_width;
  const int cover_height = layout.cover_height;
  for (int row = 0; row < layout.rows; ++row)
  {
    for (int column = 0; column < layout.columns; ++column)
    {
      const int index = page_start + row * layout.columns + column;
      if (index >= static_cast<int>(m_visible_games.size()))
        continue;
      Game& game = *VisibleGame(index);
      EnsureCover(&game);
      const int x = layout.x0 + column * (cover_width + layout.gap_x);
      const int y = layout.y0 + row * row_stride;
      const bool current = index == selection;
      FillRect(x + 4, y + 6, cover_width, cover_height, SDL_Color{0, 0, 0, 55});
      FillRect(x + 2, y + 3, cover_width, cover_height, SDL_Color{0, 0, 0, 70});
      if (game.cover)
      {
        Uint8 alpha = 255;
        if (m_animations && SDL_GetTicks() - game.cover_loaded_at < 180)
          alpha = static_cast<Uint8>((SDL_GetTicks() - game.cover_loaded_at) * 255 / 180);
        SDL_SetTextureAlphaMod(game.cover, alpha);
        SDL_SetTextureColorMod(game.cover, current ? 255 : 225, current ? 255 : 225,
                               current ? 255 : 225);
        SDL_Rect destination{x, y, cover_width, cover_height};
        SDL_RenderCopy(m_renderer, game.cover, nullptr, &destination);
      }
      else
      {
        FillRect(x, y, cover_width, cover_height, m_card);
        const std::string_view no_cover = m_localization.Translate("NO COVER");
        const int text_width = cover_width - 16;
        const int line_height = FontHeight(m_font_small) + 4;
        const int center_y = y + cover_height / 2;
        if (TextWidth(m_font_small, no_cover) <= text_width)
          DrawTextCentered(m_font_small, x + cover_width / 2,
                           center_y - FontHeight(m_font_small) / 2, no_cover, m_dim);
        else
          DrawWrappedCentered(m_font_small, x + cover_width / 2, center_y - line_height, text_width,
                              line_height, 2, no_cover, m_dim);
      }
      Border(x, y, cover_width, cover_height, 1, SDL_Color{12, 13, 18, 255});
      FillRect(x, y, cover_width, 1, SDL_Color{255, 255, 255, 26});
      if (current)
        Border(x - 2, y - 2, cover_width + 4, cover_height + 4, 2, m_selection);
      int flag_index = 0;
      if (game.region == DiscIO::Region::NTSC_U)
        flag_index = 1;
      else if (game.region == DiscIO::Region::PAL)
        flag_index = 2;
      else if (game.region == DiscIO::Region::NTSC_J || game.region == DiscIO::Region::NTSC_K)
        flag_index = 3;
      if (m_show_region_flags && flag_index && m_flags[flag_index])
      {
        int flag_width = std::clamp(cover_width * 26 / 100, 16, 30);
        const int flag_height = flag_width * 2 / 3;
        SDL_Rect flag{x + 6, y + 6, flag_width, flag_height};
        SDL_RenderCopy(m_renderer, m_flags[flag_index], nullptr, &flag);
        Border(x + 6, y + 6, flag_width, flag_height, 1, SDL_Color{10, 12, 18, 255});
      }
      if (m_show_custom_settings_badges && game.has_game_config)
      {
        const int size = std::max(12, cover_width / 11);
        FillRect(x + cover_width - size - 8, y + 8, size, size, m_selection);
        Border(x + cover_width - size - 8, y + 8, size, size, 2, SDL_Color{10, 12, 18, 255});
      }
      if (m_favorites.contains(game.key))
        DrawText(m_font_small, x + cover_width - 27, y + 5, "★", m_value);
      if (m_show_titles)
        DrawTitleCell(x + cover_width / 2, cover_width, y + cover_height + 6, game, current,
                      current ? m_value : m_dim);
    }
  }
  // Decode the following page after all visible covers are queued. Page turns therefore avoid
  // the first-visit stall without allowing a large library to flood memory or the SDL upload path.
  const int prefetch_start = page_start + per_page;
  const int prefetch_end =
      std::min(static_cast<int>(m_visible_games.size()), prefetch_start + per_page);
  for (int index = prefetch_start; index < prefetch_end; ++index)
    EnsureCover(VisibleGame(index));
  if (m_visible_games.empty())
  {
    std::string empty_state;
    if (m_library_scan)
    {
      const std::size_t processed = m_library_scan->processed.load(std::memory_order_acquire);
      const std::size_t discovered = m_library_scan->discovered.load(std::memory_order_acquire);
      empty_state =
          discovered ? std::string(m_localization.Translate("Scanning game library...")) + "  " +
                           std::to_string(processed) + " / " + std::to_string(discovered) :
                       std::string(m_localization.Translate("Scanning game folders..."));
    }
    else
    {
      empty_state =
          m_localization.Translate("No games match this view -- press - to change filters");
    }
    const int panel_width = std::min(940, m_width - 64);
    const int panel_height = std::max(140, FontHeight(m_font) + 96);
    GlassPanel((m_width - panel_width) / 2, (m_height - panel_height) / 2, panel_width,
               panel_height);
    DrawTextCentered(m_font, m_width / 2, (m_height - FontHeight(m_font)) / 2,
                     Ellipsize(m_font, empty_state, panel_width - 64), m_dim);
  }
  std::array<std::pair<std::string_view, std::string_view>, LIBRARY_FOOTER.size()> hints{};
  int hint_count = 0;
  const bool has_games = !m_visible_games.empty();
  if (has_games)
    hints[hint_count++] = {"A", "Launch"};
  if (has_games)
    hints[hint_count++] = {"Y", "Sort"};
  hints[hint_count++] = {"X", "Settings"};
  if (has_games)
    hints[hint_count++] = {"+", "Game Menu"};
  hints[hint_count++] = {"-", "Filter"};
  if (page_count > 1)
  {
    hints[hint_count++] = {"L", ""};
    hints[hint_count++] = {"R", "Page"};
  }
  hints[hint_count++] = {"B", "Quit"};
  DrawFooter(std::span(hints).first(hint_count));
  SDL_RenderPresent(m_renderer);
}

std::string Launcher::SharedGameIniPath(const Game& game) const
{
  if (game.game_id.empty())
    return {};
  return File::GetUserPath(D_GAMESETTINGS_IDX) + game.game_id + ".ini";
}

std::string Launcher::EntryGameIniPath(const Game& game) const
{
  if (game.installed_nand || game.key.empty())
    return {};
  return File::GetUserPath(D_GAMESETTINGS_IDX) + "Entries/" + game.key + ".ini";
}

std::string Launcher::GameIniPath(const Game& game) const
{
  return game.config_override_path.empty() ? SharedGameIniPath(game) : game.config_override_path;
}

std::optional<std::string> Launcher::GetGameSetting(const Game& game, std::string_view section,
                                                    std::string_view key) const
{
  const auto read = [&](const std::string& path) -> std::optional<std::string> {
    if (path.empty())
      return std::nullopt;
    auto iterator = m_game_ini_cache.find(path);
    if (iterator == m_game_ini_cache.end())
    {
      auto ini = std::make_unique<Common::IniFile>();
      ini->Load(path);
      iterator = m_game_ini_cache.emplace(path, std::move(ini)).first;
    }
    const Common::IniFile::Section* ini_section = iterator->second->GetSection(section);
    if (!ini_section)
      return std::nullopt;
    std::string value;
    return ini_section->Get(key, &value) ? std::optional<std::string>{value} : std::nullopt;
  };

  if (const std::optional<std::string> value = read(GameIniPath(game)))
    return value;
  if (!game.config_override_path.empty())
    return read(SharedGameIniPath(game));
  return std::nullopt;
}

bool Launcher::SetGameSetting(const Game& game, std::string_view section, std::string_view key,
                              const std::optional<std::string>& value)
{
  return SetGameSettings(game, {{section, key, value}});
}

bool Launcher::SetGameSettings(const Game& game,
                               std::initializer_list<std::tuple<std::string_view, std::string_view,
                                                                std::optional<std::string>>>
                                   edits)
{
  const std::string path = GameIniPath(game);
  if (path.empty())
    return false;
  if (!File::CreateFullPath(path))
    return false;
  Common::IniFile ini;
  ini.Load(path);
  for (const auto& [section, key, value] : edits)
  {
    if (value)
      ini.GetOrCreateSection(section)->Set(std::string(key), *value);
    else
      ini.DeleteKey(section, key);
  }
  const bool saved = ini.Save(path);
  if (saved)
    m_game_ini_cache.erase(path);
  return saved;
}

void Launcher::InvalidateGameSettingCache(const Game& game) const
{
  const std::string path = GameIniPath(game);
  if (!path.empty())
    m_game_ini_cache.erase(path);
  const std::string shared_path = SharedGameIniPath(game);
  if (!shared_path.empty() && shared_path != path)
    m_game_ini_cache.erase(shared_path);
}

std::string Launcher::GlobalValueLabel(std::string_view value) const
{
  return std::string(m_localization.Translate("Global")) + ": " +
         std::string(m_localization.Translate(value));
}

std::string Launcher::UseGlobalValueLabel(std::string_view value) const
{
  return std::string(m_localization.Translate("Use global")) + " (" +
         std::string(m_localization.Translate(value)) + ")";
}

void Launcher::AppearanceSettings()
{
  static constexpr std::array<std::string_view, 5> THEMES = {"XMB (PS3)", "Bubbles", "Glow",
                                                             "Classic", "OLED black"};
  static constexpr std::array<std::string_view, 5> THEME_VALUES = {"xmb", "bubbles", "glow",
                                                                   "classic", "oled"};
  constexpr int option_count = 10;
  constexpr int selection_count = option_count;
  const int row_height = SettingsRowHeight();
  const int list_top = SettingsListY();
  static int saved_selection = 0;
  static int saved_top = 0;

  const auto rows_provider = [&] {
    int theme_index = 1;
    const auto iterator =
        std::ranges::find(THEME_VALUES, std::string_view(m_store.Get("Launcher/Theme")));
    if (iterator != THEME_VALUES.end())
      theme_index = iterator - THEME_VALUES.begin();
    const bool steamgriddb_key_set = !Trim(m_store.Get("Network/SteamGridDBKey")).empty();
    return std::array<Row, option_count>{
        Row{"Theme", std::string(THEMES[theme_index])},
        Row{"Language", m_localization.GetDisplayName(), true, false, true, true, false},
        Row{"Games per row", std::to_string(m_grid_columns)},
        Row{"Rows per page", std::to_string(m_grid_rows)},
        Row{"Show game titles", m_show_titles ? "On" : "Off"},
        Row{"Show region flags", m_show_region_flags ? "On" : "Off"},
        Row{"Show custom settings badges", m_show_custom_settings_badges ? "On" : "Off"},
        Row{"UI animations", m_animations ? "On" : "Off"},
        Row{"Sound effects", m_store.GetBool("Launcher/Sounds", true) ? "On" : "Off"},
        Row{"SteamGridDB API key", steamgriddb_key_set ? "Configured" : "Not set", true, false,
            false},
    };
  };

  const auto apply_option = [&](int index, int delta) {
    if (index == 0)
    {
      int current = 1;
      const auto iterator =
          std::ranges::find(THEME_VALUES, std::string_view(m_store.Get("Launcher/Theme")));
      if (iterator != THEME_VALUES.end())
        current = iterator - THEME_VALUES.begin();
      current = SelectChoice("Theme", THEMES, current, delta);
      m_store.Set("Launcher/Theme", std::string(THEME_VALUES[current]));
    }
    else if (index == 1)
    {
      const auto languages = Localization::GetLanguages();
      std::vector<std::string_view> names;
      names.reserve(languages.size());
      for (const LauncherLanguage& language : languages)
        names.push_back(language.name);
      int current = Localization::FindLanguage(m_store.Get("Launcher/Language", "system"));
      current = SelectChoice("Language", names, current, delta);
      m_store.Set("Launcher/Language", std::string(languages[current].code));
      m_localization.SetLanguage(languages[current].code);
      if (!LoadFonts())
      {
        m_store.Set("Launcher/Language", "en");
        m_localization.SetLanguage("en");
        (void)LoadFonts();
        Toast("The selected language font could not be loaded", 1600);
      }
    }
    else if (index == 2)
    {
      int value = std::clamp(m_grid_columns, 3, 8);
      if (delta == 0)
      {
        const int selected =
            Dropdown("Games per row", {"3", "4", "5", "6", "7", "8"}, value - 3, true, false);
        value = selected + 3;
      }
      else
      {
        value = std::clamp(value + (delta < 0 ? -1 : 1), 3, 8);
      }
      m_store.SetInt("Launcher/GridColumns", value);
    }
    else if (index == 3)
    {
      int value = std::clamp(m_grid_rows, 1, 3);
      if (delta == 0)
        value = Dropdown("Rows per page", {"1", "2", "3"}, value - 1, true, false) + 1;
      else
        value = std::clamp(value + (delta < 0 ? -1 : 1), 1, 3);
      m_store.SetInt("Launcher/GridRows", value);
    }
    else if (index == 4)
    {
      m_store.SetBool("Launcher/ShowTitles", !m_show_titles);
    }
    else if (index == 5)
    {
      m_store.SetBool("Launcher/ShowRegionFlags", !m_show_region_flags);
    }
    else if (index == 6)
    {
      m_store.SetBool("Launcher/ShowCustomSettingsBadges", !m_show_custom_settings_badges);
    }
    else if (index == 7)
    {
      m_store.SetBool("Launcher/Animations", !m_animations);
    }
    else if (index == 8)
    {
      const bool enabled = !m_store.GetBool("Launcher/Sounds", true);
      m_store.SetBool("Launcher/Sounds", enabled);
      SetUiAudioEnabled(enabled);
    }
    else if (index == 9)
    {
      std::string api_key = m_store.Get("Network/SteamGridDBKey");
      if (PromptText("SteamGridDB API key", api_key, &api_key, true, true,
                     "Used for cover and shortcut artwork downloads.",
                     "Leave blank to remove the saved key"))
      {
        api_key = Trim(std::move(api_key));
        m_store.Set("Network/SteamGridDBKey", api_key);
        MarkStoreDirty();
        FlushPendingSaves();
        Toast(api_key.empty() ? "SteamGridDB API key removed" : "SteamGridDB API key updated",
              1100);
      }
      return;
    }
    MarkStoreDirty();
    ApplyAppearance();
  };

  const auto reset_option = [&](int index) {
    switch (index)
    {
    case 0:
      m_store.Set("Launcher/Theme", "bubbles");
      break;
    case 1:
      m_store.Set("Launcher/Language", "system");
      m_localization.SetLanguage("system");
      if (!LoadFonts())
      {
        m_store.Set("Launcher/Language", "en");
        m_localization.SetLanguage("en");
        (void)LoadFonts();
      }
      break;
    case 2:
      m_store.SetInt("Launcher/GridColumns", 5);
      break;
    case 3:
      m_store.SetInt("Launcher/GridRows", 2);
      break;
    case 4:
      m_store.SetBool("Launcher/ShowTitles", true);
      break;
    case 5:
      m_store.SetBool("Launcher/ShowRegionFlags", true);
      break;
    case 6:
      m_store.SetBool("Launcher/ShowCustomSettingsBadges", true);
      break;
    case 7:
      m_store.SetBool("Launcher/Animations", true);
      break;
    case 8:
      m_store.SetBool("Launcher/Sounds", true);
      SetUiAudioEnabled(true);
      break;
    case 9:
      m_store.Set("Network/SteamGridDBKey", "");
      break;
    default:
      return;
    }
    MarkStoreDirty();
    ApplyAppearance();
    Toast("Setting reset to default", 550);
    BeginScreenFx();
  };

  int selection = std::clamp(saved_selection, 0, selection_count - 1);
  int top = std::max(0, saved_top);
  const auto finish = [&] {
    saved_selection = selection;
    saved_top = top;
    FlushPendingSaves();
  };

  BeginScreenFx();
  while (BeginFrame())
  {
    const auto rows = rows_provider();
    const int visible =
        std::min(option_count, std::max(1, (m_height - list_top - 190) / row_height));
    top = std::clamp(top, 0, std::max(0, option_count - visible));
    const int column_width = std::min(980, m_width - 180);
    const int column_x = (m_width - column_width) / 2;

    SDL_Event event{};
    while (PollEvent(&event))
    {
      int touch_x = 0;
      int touch_y = 0;
      const TouchKind touch = FeedTouch(event, &touch_x, &touch_y);
      if (TouchScrollList(touch, &selection, &top, option_count, visible))
        continue;
      if ((touch == TouchKind::SwipeLeft || touch == TouchKind::SwipeRight) &&
          selection < option_count && rows[selection].adjustable)
      {
        apply_option(selection, touch == TouchKind::SwipeLeft ? -1 : 1);
        continue;
      }
      if (touch == TouchKind::Tap)
      {
        if (touch_y < TopBarHeight() || touch_y >= m_height - 40)
        {
          finish();
          return;
        }
        if (touch_x >= column_x && touch_x < column_x + column_width && touch_y >= list_top &&
            touch_y < list_top + visible * row_height)
        {
          const int index = top + (touch_y - list_top) / row_height;
          if (index >= 0 && index < option_count)
          {
            selection = index;
            apply_option(index, 0);
          }
        }
        continue;
      }

      const int direction = EventNavigation(event);
      if (direction != 0)
        selection = (selection + direction + selection_count) % selection_count;

      const bool controller = event.type == SDL_CONTROLLERBUTTONDOWN;
      const bool keyboard = event.type == SDL_KEYDOWN;
      const int button = controller ? event.cbutton.button : -1;
      const SDL_Keycode key = keyboard ? event.key.keysym.sym : SDLK_UNKNOWN;
      if ((button == SDL_CONTROLLER_BUTTON_DPAD_LEFT || key == SDLK_LEFT) &&
          selection < option_count && rows[selection].adjustable)
      {
        apply_option(selection, -1);
      }
      else if ((button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT || key == SDLK_RIGHT) &&
               selection < option_count && rows[selection].adjustable)
      {
        apply_option(selection, 1);
      }
      else if ((button == BUTTON_SETTINGS || key == SDLK_x) && selection < option_count)
      {
        const SettingHelpInfo info = SettingHelpFor("Launcher", rows[selection]);
        const std::string_view current = rows[selection].value == ">" ?
                                             std::string_view{} :
                                             std::string_view(rows[selection].value);
        ShowInfoCard("Launcher", rows[selection].label, info.kind, info.description, current,
                     SettingScope("Launcher", {}), rows[selection].localize_label,
                     rows[selection].localize_value);
        BeginScreenFx();
      }
      else if ((button == SDL_CONTROLLER_BUTTON_X || key == SDLK_y || key == SDLK_DELETE) &&
               selection < option_count)
      {
        reset_option(selection);
      }
      else if (button == BUTTON_CONFIRM || key == SDLK_RETURN)
      {
        apply_option(selection, 0);
      }
      else if (button == BUTTON_CANCEL || key == SDLK_ESCAPE)
      {
        finish();
        return;
      }
    }

    if (selection < option_count)
    {
      if (selection < top)
        top = selection;
      if (selection >= top + visible)
        top = selection - visible + 1;
    }

    ClearBackground();
    DrawHeader("Launcher", {});
    const int label_x = column_x + 40;
    const int value_x = column_x + column_width - 40;
    GlassPanel(column_x - 12, list_top - 10, column_width + 24,
               std::min(visible, option_count - top) * row_height + 18);
    if (selection < option_count)
    {
      const float target_y = static_cast<float>(list_top + (selection - top) * row_height + 1);
      m_highlight_y = (!m_animations || m_highlight_y < 0.0f) ?
                          target_y :
                          m_highlight_y + (target_y - m_highlight_y) * 0.30f;
      DrawRowHighlight(column_x, static_cast<int>(m_highlight_y), column_width, row_height - 2);
    }
    for (int row = 0; row < visible && top + row < option_count; ++row)
    {
      const int index = top + row;
      const bool current = index == selection;
      const std::string_view displayed_label = rows[index].localize_label ?
                                                   m_localization.Translate(rows[index].label) :
                                                   std::string_view(rows[index].label);
      const std::string_view displayed_value = rows[index].localize_value ?
                                                   m_localization.Translate(rows[index].value) :
                                                   std::string_view(rows[index].value);
      DrawSettingsRowText(displayed_label, displayed_value, list_top + row * row_height,
                          column_width, label_x, value_x, current, current ? m_value : m_text,
                          current ? m_value : m_dim, false, row_height);
    }
    if (option_count > visible)
    {
      const int track_height = visible * row_height;
      const int track_x = column_x + column_width + 16;
      const int track_y = list_top - 2;
      FillRect(track_x, track_y, 4, track_height, SDL_Color{40, 44, 54, 255});
      const int thumb_height = std::max(16, track_height * visible / option_count);
      FillRect(track_x,
               track_y + (track_height - thumb_height) * top / std::max(1, option_count - visible),
               4, thumb_height, m_selection);
    }

    std::array<std::pair<std::string_view, std::string_view>, 6> footer{};
    int hint_count = 0;
    if (selection < option_count && rows[selection].adjustable)
    {
      // The empty label is the deliberate glyph pair: Left and Right share one caption.
      footer[hint_count++] = {"Left", ""};
      footer[hint_count++] = {"Right", "Change"};
    }
    footer[hint_count++] = {"A", "Choose"};
    if (selection < option_count)
    {
      footer[hint_count++] = {"X", "Info"};
      footer[hint_count++] = {"Y", "Reset"};
    }
    footer[hint_count++] = {"B", "Back"};
    DrawFooter(std::span(footer).first(hint_count));
    DrawFadeIn();
    SDL_RenderPresent(m_renderer);
    WaitForNextFrame();
  }
  finish();
}

void Launcher::GameSourcesScreen()
{
  int selection = 0;
  int top = 0;
  constexpr int row_height = 50;
  const int list_y = SettingsListY();
  const auto identity = [](const std::string& path) { return Lower(NormalizePath(path)); };
  BeginScreenFx();
  while (BeginFrame())
  {
    const int count = 1 + static_cast<int>(m_sources.size());
    const int visible =
        std::max(1, (m_height - list_y - SettingsFooterReserve()) / row_height);
    selection = std::clamp(selection, 0, count - 1);
    if (selection < top)
      top = selection;
    if (selection >= top + visible)
      top = selection - visible + 1;
    bool rebuild = false;
    SDL_Event event{};
    while (PollEvent(&event))
    {
      int touch_x = 0;
      int touch_y = 0;
      const TouchKind touch = FeedTouch(event, &touch_x, &touch_y);
      if (TouchScrollList(touch, &selection, &top, count, visible))
        continue;
      if (touch == TouchKind::Tap)
      {
        if (touch_y >= m_height - 48)
          return;
        for (int row = 0; row < visible && top + row < count; ++row)
        {
          const int y = list_y + row * row_height;
          if (touch_y >= y && touch_y < y + 46)
          {
            selection = top + row;
            SDL_Event press{};
            press.type = SDL_CONTROLLERBUTTONDOWN;
            press.cbutton.button = BUTTON_CONFIRM;
            SDL_PushEvent(&press);
            break;
          }
        }
        continue;
      }
      const int direction = EventNavigation(event);
      if (direction)
        selection = (selection + direction + count) % count;
      const bool confirm =
          (event.type == SDL_CONTROLLERBUTTONDOWN && event.cbutton.button == BUTTON_CONFIRM) ||
          (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_RETURN);
      const bool cancel =
          (event.type == SDL_CONTROLLERBUTTONDOWN && event.cbutton.button == BUTTON_CANCEL) ||
          (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE);
      if (cancel)
        return;
      if (!confirm)
        continue;
      if (selection == 0)
      {
        if (m_sources.size() >= 16)
        {
          Toast("Maximum of 16 game folders", 1000);
          continue;
        }
        const std::string selected = FileBrowser({}, true, false, false);
        if (!selected.empty())
        {
          const std::string selected_identity = identity(selected);
          if (std::ranges::any_of(m_sources, [&](const std::string& path) {
                return identity(path) == selected_identity;
              }))
          {
            Toast("Folder already added", 900);
          }
          else
          {
            EnsureSourceMountedAtStartup(selected);
            m_sources.push_back(NormalizePath(selected));
            SaveSources();
            FlushPendingSaves();
            selection = static_cast<int>(m_sources.size());
          }
          rebuild = true;
        }
      }
      else
      {
        std::size_t source_index = static_cast<std::size_t>(selection - 1);
        const int choice =
            Dropdown("Game folder", {"Change folder", "Move up", "Move down", "Remove"}, -1);
        if (choice == 0)
        {
          const std::string selected = FileBrowser(m_sources[source_index], true, false, false);
          if (!selected.empty())
          {
            const std::string selected_identity = identity(selected);
            bool duplicate = false;
            for (std::size_t index = 0; index < m_sources.size(); ++index)
            {
              if (index != source_index && identity(m_sources[index]) == selected_identity)
                duplicate = true;
            }
            if (duplicate)
              Toast("Folder already added", 900);
            else
            {
              EnsureSourceMountedAtStartup(selected);
              m_sources[source_index] = NormalizePath(selected);
              SaveSources();
              FlushPendingSaves();
            }
            rebuild = true;
          }
        }
        else if (choice == 1 && source_index > 0)
        {
          std::swap(m_sources[source_index], m_sources[source_index - 1]);
          --selection;
          SaveSources();
          FlushPendingSaves();
          rebuild = true;
        }
        else if (choice == 2 && source_index + 1 < m_sources.size())
        {
          std::swap(m_sources[source_index], m_sources[source_index + 1]);
          ++selection;
          SaveSources();
          FlushPendingSaves();
          rebuild = true;
        }
        else if (choice == 3 &&
                 Confirm("Remove game folder?",
                         std::array<std::string, 3>{
                             m_sources[source_index], "",
                             std::string(m_localization.Translate("No files will be deleted."))}))
        {
          m_sources.erase(m_sources.begin() + source_index);
          selection = std::max(0, selection - 1);
          SaveSources();
          FlushPendingSaves();
          rebuild = true;
        }
      }
      if (rebuild)
        break;
    }
    if (rebuild)
    {
      BeginScreenFx();
      continue;
    }

    ClearBackground();
    const std::string summary =
        std::to_string(m_sources.size()) + " " +
        std::string(m_localization.Translate(m_sources.size() == 1 ? "folder" : "folders"));
    DrawPageHeader(m_localization.Translate("Game folders"), "YabaSanshiro NX", summary,
                   m_localization.Translate("Scanned recursively"));
    GlassPanel(44, list_y - 13, m_width - 88,
               std::min(visible, count - top) * row_height + 18);
    for (int row = 0; row < visible && top + row < count; ++row)
    {
      const int index = top + row;
      const int y = list_y + row * row_height;
      const bool current = index == selection;
      if (current)
      {
        DrawRowHighlight(56, y - 3, m_width - 112, 46);
      }
      const std::string label = index == 0 ?
                                    std::string(m_localization.Translate("[ Add game folder ]")) :
                                    m_sources[index - 1];
      DrawText(m_font, 82, y, Ellipsize(m_font, label, m_width - 170),
               current ? m_value : (index == 0 ? m_highlight : m_text));
    }
    DrawSettingsFooter("A  Select       B  Back");
    SDL_RenderPresent(m_renderer);
    WaitForNextFrame();
  }
}

bool Launcher::DeleteTree(const std::string& path, const std::atomic_bool* cancel)
{
  if (cancel && cancel->load(std::memory_order_relaxed))
    return false;
  if (IsFilesystemRoot(path))
    return false;
  struct stat info{};
  if (::lstat(path.c_str(), &info) != 0)
    return errno == ENOENT;
  if (!S_ISDIR(info.st_mode))
    return std::remove(path.c_str()) == 0;
  DIR* directory = ::opendir(path.c_str());
  if (!directory)
    return false;
  bool ok = true;
  while (dirent* entry = ::readdir(directory))
  {
    if (cancel && cancel->load(std::memory_order_relaxed))
    {
      ok = false;
      break;
    }
    if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0)
      continue;
    if (!DeleteTree(JoinPath(path, entry->d_name), cancel))
      ok = false;
  }
  if (::closedir(directory) != 0)
    ok = false;
  return ok && ::rmdir(path.c_str()) == 0;
}

bool Launcher::MeasureTree(const std::string& path, TransferState* state)
{
  if (!state || state->cancelled.load(std::memory_order_relaxed))
    return false;
  struct stat info{};
  if (::lstat(path.c_str(), &info) != 0)
  {
    SetTransferDetail(state, {}, "Source is no longer available");
    return false;
  }
  if (S_ISREG(info.st_mode))
  {
    state->total.fetch_add(static_cast<std::uint64_t>(info.st_size), std::memory_order_relaxed);
    return true;
  }
  if (!S_ISDIR(info.st_mode))
  {
    SetTransferDetail(state, {}, "Unsupported file type");
    return false;
  }
  DIR* directory = ::opendir(path.c_str());
  if (!directory)
  {
    SetTransferDetail(state, {}, "Could not open a source folder");
    return false;
  }
  bool ok = true;
  while (ok && !state->cancelled.load(std::memory_order_relaxed))
  {
    dirent* entry = ::readdir(directory);
    if (!entry)
      break;
    if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0)
      continue;
    ok = MeasureTree(JoinPath(path, entry->d_name), state);
  }
  if (::closedir(directory) != 0 && ok)
  {
    SetTransferDetail(state, {}, "Could not close a source folder");
    ok = false;
  }
  return ok;
}

bool Launcher::CopyTree(const std::string& source, const std::string& destination,
                        TransferState* state)
{
  struct stat info{};
  if (::lstat(source.c_str(), &info) != 0)
  {
    SetTransferDetail(state, {}, "Source is no longer available");
    return false;
  }
  if (S_ISDIR(info.st_mode))
  {
    if (::mkdir(destination.c_str(), 0777) != 0)
    {
      SetTransferDetail(state, {}, "Could not create a destination folder");
      return false;
    }
    state->destination_created.store(true, std::memory_order_relaxed);
    DIR* directory = ::opendir(source.c_str());
    if (!directory)
    {
      SetTransferDetail(state, {}, "Could not open a source folder");
      return false;
    }
    bool ok = true;
    while (ok && !state->cancelled.load(std::memory_order_relaxed))
    {
      dirent* entry = ::readdir(directory);
      if (!entry)
        break;
      if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0)
        continue;
      ok = CopyTree(JoinPath(source, entry->d_name), JoinPath(destination, entry->d_name), state);
    }
    if (::closedir(directory) != 0 && ok)
    {
      SetTransferDetail(state, {}, "Could not close a source folder");
      ok = false;
    }
    return ok && !state->cancelled.load(std::memory_order_relaxed);
  }
  if (!S_ISREG(info.st_mode))
  {
    SetTransferDetail(state, {}, "Unsupported file type");
    return false;
  }

  SetTransferDetail(state, FileName(source));
  const std::string partial = destination + ".dolphin-part";
  const std::string backup = destination + ".dolphin-old";
  std::remove(partial.c_str());
  FILE* input = std::fopen(source.c_str(), "rb");
  if (!input)
  {
    SetTransferDetail(state, {}, "Could not open the source file");
    return false;
  }
  FILE* output = std::fopen(partial.c_str(), "wb");
  if (!output)
  {
    std::fclose(input);
    SetTransferDetail(state, {}, "Could not create the destination file");
    return false;
  }
  bool ok = true;
  while (ok && !state->cancelled.load(std::memory_order_relaxed))
  {
    const std::size_t count = std::fread(state->buffer.data(), 1, state->buffer.size(), input);
    if (count != 0)
    {
      if (std::fwrite(state->buffer.data(), 1, count, output) != count)
      {
        SetTransferDetail(state, {}, "Write failed; check free space and permissions");
        ok = false;
        break;
      }
      state->done.fetch_add(count, std::memory_order_relaxed);
    }
    if (count < state->buffer.size())
    {
      if (std::ferror(input))
      {
        SetTransferDetail(state, {}, "Read failed");
        ok = false;
      }
      break;
    }
  }
  if (state->cancelled.load(std::memory_order_relaxed))
    ok = false;
  if (ok && std::fflush(output) != 0)
  {
    SetTransferDetail(state, {}, "Could not flush the destination file");
    ok = false;
  }
  if (ok && ::fsync(::fileno(output)) != 0)
  {
    SetTransferDetail(state, {}, "Could not commit the destination file");
    ok = false;
  }
  if (std::fclose(input) != 0 && ok)
  {
    SetTransferDetail(state, {}, "Could not close the source file");
    ok = false;
  }
  if (std::fclose(output) != 0 && ok)
  {
    SetTransferDetail(state, {}, "Could not close the destination file");
    ok = false;
  }
  if (!ok)
  {
    std::remove(partial.c_str());
    return false;
  }

  struct stat destination_info{};
  const bool existed = ::lstat(destination.c_str(), &destination_info) == 0;
  if (existed)
  {
    struct stat backup_info{};
    if (::lstat(backup.c_str(), &backup_info) == 0)
    {
      SetTransferDetail(state, {}, "A previous backup file blocks this operation");
      std::remove(partial.c_str());
      return false;
    }
    if (std::rename(destination.c_str(), backup.c_str()) != 0)
    {
      SetTransferDetail(state, {}, "Could not preserve the existing destination");
      std::remove(partial.c_str());
      return false;
    }
  }
  if (std::rename(partial.c_str(), destination.c_str()) != 0)
  {
    if (existed)
      std::rename(backup.c_str(), destination.c_str());
    SetTransferDetail(state, {}, "Could not finalize the copied file");
    std::remove(partial.c_str());
    return false;
  }
  if (existed)
    std::remove(backup.c_str());
  return true;
}

bool Launcher::RenderTransfer(TransferState* state)
{
  if (!BeginFrame())
    state->cancelled.store(true, std::memory_order_relaxed);
  SDL_Event event{};
  while (PollEvent(&event))
  {
    int touch_x = 0;
    int touch_y = 0;
    const TouchKind touch = FeedTouch(event, &touch_x, &touch_y);
    if ((event.type == SDL_CONTROLLERBUTTONDOWN && event.cbutton.button == BUTTON_CANCEL) ||
        (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) ||
        (touch == TouchKind::Tap && touch_y >= m_height - 100))
      state->cancelled.store(true, std::memory_order_relaxed);
  }
  std::string current;
  {
    std::lock_guard lock(state->detail_mutex);
    current = state->current;
  }
  ClearBackground();
  DrawHeader("File transfer", current);
  const int bar_width = m_width * 2 / 3;
  const int bar_x = (m_width - bar_width) / 2;
  const int bar_y = m_height / 2 - 24;
  const int bar_height = 38;
  GlassPanel(bar_x - 32, bar_y - 56, bar_width + 64, bar_height + 148);
  const std::uint64_t done = state->done.load(std::memory_order_relaxed);
  const std::uint64_t total = state->total.load(std::memory_order_relaxed);
  const std::uint64_t progress = total ? std::min(done, total) : 0;
  DrawProgressBar(bar_x, bar_y, bar_width, bar_height,
                  total ? static_cast<double>(progress) / total : 0.0);
  char text[128];
  const int percent = total ? static_cast<int>(progress * 100 / total) : 0;
  std::snprintf(text, sizeof(text), "%d%%  ·  %.1f / %.1f MiB", percent, done / 1048576.0,
                total / 1048576.0);
  DrawTextCentered(m_font, m_width / 2, bar_y + bar_height + 28, text, m_text);
  if (state->cancelled.load(std::memory_order_relaxed))
  {
    // Same band and baseline as DrawFooter, so the text doesn't jump.
    DrawFooterBand(FontHeight(m_font_small) + 32);
    DrawTextCentered(m_font_small, m_width / 2, m_height - 26 - FontHeight(m_font_small) / 2,
                     m_localization.Translate("Cancelling..."), m_value);
  }
  else
  {
    const std::array<std::pair<std::string_view, std::string_view>, 1> controls = {
        std::pair{"B", "Cancel"}};
    DrawFooter(controls);
  }
  SDL_RenderPresent(m_renderer);
  return !state->cancelled.load(std::memory_order_relaxed);
}

void Launcher::RunBusyTask(std::string_view title, std::string_view detail,
                           const std::function<void()>& task, std::atomic_bool* cancel)
{
  std::atomic<bool> complete{false};
  const std::string owned_title{title};
  const std::string owned_detail{detail};
  (void)appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
  BusyTaskThreadContext context{&task, &complete, cancel};
  Thread worker{};
  const Result create_result =
      threadCreate(&worker, BusyTaskThreadEntry, &context, nullptr, BUSY_TASK_STACK_SIZE, 0x2C, -2);
  const bool worker_created = R_SUCCEEDED(create_result);
  const Result start_result = worker_created ? threadStart(&worker) : create_result;
  const bool worker_started = worker_created && R_SUCCEEDED(start_result);
  if (!worker_started)
  {
    if (worker_created)
      (void)threadClose(&worker);
    task();
    complete.store(true, std::memory_order_release);
  }

  while (!complete.load(std::memory_order_acquire))
  {
    const bool can_render = BeginFrame();
    if (!can_render && cancel)
      cancel->store(true, std::memory_order_release);
    if (can_render)
    {
      SDL_Event event{};
      while (PollEvent(&event))
      {
        if (cancel &&
            ((event.type == SDL_CONTROLLERBUTTONDOWN && event.cbutton.button == BUTTON_CANCEL) ||
             (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE)))
        {
          cancel->store(true, std::memory_order_release);
        }
      }
      ClearBackground();
      DrawHeader(owned_title, owned_detail);
      const int panel_width = std::min(920, m_width - 180);
      const int panel_height = 196;
      const int panel_x = (m_width - panel_width) / 2;
      const int panel_y = (m_height - panel_height) / 2;
      GlassPanel(panel_x, panel_y, panel_width, panel_height);
      const int phase = static_cast<int>((SDL_GetTicks() / 220) % 4);
      std::string message(m_localization.Translate("Working"));
      message.append(static_cast<std::size_t>(phase), '.');
      DrawTextCentered(m_font_large, m_width / 2, panel_y + 44, message,
                       m_value);
      DrawTextCentered(
          m_font_small, m_width / 2, panel_y + 112,
          m_localization.Translate(cancel && cancel->load(std::memory_order_acquire) ?
                                       "Cancelling at the next safe point..." :
                                       "Do not remove the active storage device or close the app."),
          m_dim);
      if (cancel && !cancel->load(std::memory_order_acquire))
      {
        const std::array<std::pair<std::string_view, std::string_view>, 1> hints = {
            std::pair{"B", "Cancel"}};
        DrawFooter(hints);
      }
      SDL_RenderPresent(m_renderer);
      WaitForNextFrame();
    }
    else
    {
      // Nothing was presented, so pace the loop instead of spinning.
      SDL_Delay(16);
    }
  }
  if (worker_started)
  {
    (void)threadWaitForExit(&worker);
    (void)threadClose(&worker);
  }
  (void)appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
}

bool Launcher::ExecutePaste(const std::string& folder)
{
  if (m_clipboard_path.empty())
    return false;
  const std::string source_path = m_clipboard_path;
  struct stat source_info{};
  if (::lstat(m_clipboard_path.c_str(), &source_info) != 0)
  {
    RenderMessage("Paste failed",
                  std::array<std::string, 1>{"The copied item is no longer available."}, true);
    m_clipboard_path.clear();
    m_clipboard_move = false;
    return false;
  }
  const std::string destination = JoinPath(folder, FileName(m_clipboard_path));
  if (NormalizePath(destination) == NormalizePath(m_clipboard_path) ||
      (S_ISDIR(source_info.st_mode) && PathAtOrBelow(destination, m_clipboard_path)))
  {
    RenderMessage("Paste failed",
                  std::array<std::string, 1>{"The destination cannot be inside the source."}, true);
    return false;
  }

  struct stat destination_info{};
  const bool destination_exists = ::lstat(destination.c_str(), &destination_info) == 0;
  if (destination_exists && S_ISDIR(source_info.st_mode))
  {
    RenderMessage(
        "Folder already exists",
        std::array<std::string, 2>{std::string(m_localization.Translate(
                                       "Choose another destination or rename the folder.")),
                                   destination});
    return false;
  }
  if (destination_exists && !S_ISREG(destination_info.st_mode))
  {
    RenderMessage("Paste failed",
                  std::array<std::string, 1>{"The destination is not a regular file."}, true);
    return false;
  }
  if (destination_exists &&
      !Confirm("Replace existing file?",
               std::array<std::string, 2>{
                   FileName(destination),
                   std::string(m_localization.Translate("The existing file will be replaced."))}))
    return false;

  if (m_clipboard_move && DeviceName(m_clipboard_path) == DeviceName(destination))
  {
    const std::string backup = destination + ".dolphin-old";
    bool preserved = false;
    if (destination_exists)
    {
      struct stat backup_info{};
      if (::lstat(backup.c_str(), &backup_info) == 0 ||
          std::rename(destination.c_str(), backup.c_str()) != 0)
      {
        RenderMessage("Move failed",
                      std::array<std::string, 1>{"Could not preserve the existing destination."},
                      true);
        return false;
      }
      preserved = true;
    }
    if (std::rename(m_clipboard_path.c_str(), destination.c_str()) == 0)
    {
      if (preserved)
        std::remove(backup.c_str());
      ReplaceSavedPathPrefix(source_path, destination);
      m_clipboard_path.clear();
      m_clipboard_move = false;
      Toast("Move complete", 800);
      return true;
    }
    if (preserved)
      std::rename(backup.c_str(), destination.c_str());
  }

  TransferState state;
  SetTransferDetail(&state, {}, "Measuring source...");
  bool ok = false;
  bool enough_space = true;
  bool cleanup_ok = true;
  bool move_commit_started = false;
  std::atomic<bool> complete{false};
  appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
  std::thread worker([&] {
    ok = MeasureTree(m_clipboard_path, &state);
    if (ok && !state.cancelled.load(std::memory_order_relaxed))
    {
      struct statvfs free_space{};
      const std::uint64_t total = state.total.load(std::memory_order_relaxed);
      if (::statvfs(folder.c_str(), &free_space) == 0 && free_space.f_frsize != 0 &&
          total > static_cast<std::uint64_t>(free_space.f_bavail) * free_space.f_frsize)
      {
        enough_space = false;
        ok = false;
        SetTransferDetail(&state, {}, "The destination does not have enough available space");
      }
    }
    if (ok && !state.cancelled.load(std::memory_order_relaxed))
    {
      SetTransferDetail(&state, FileName(m_clipboard_path));
      ok = CopyTree(m_clipboard_path, destination, &state);
    }
    // A directory destination did not exist before the transfer, so always remove every partial
    // result after failure or cancellation. Do not let the user's cancellation stop cleanup.
    if (!ok && S_ISDIR(source_info.st_mode) &&
        state.destination_created.load(std::memory_order_relaxed))
      cleanup_ok = DeleteTree(destination);
    if (ok && m_clipboard_move)
    {
      // Once the destination is committed, deleting the source is the non-cancellable commit phase
      // of a cross-device move. Stopping halfway would leave an ambiguous partial move.
      move_commit_started = true;
      SetTransferDetail(&state, FileName(m_clipboard_path), "Removing the original...");
      cleanup_ok = DeleteTree(m_clipboard_path);
      if (!cleanup_ok)
        ok = false;
    }
    complete.store(true, std::memory_order_release);
  });
  while (!complete.load(std::memory_order_acquire))
  {
    RenderTransfer(&state);
    WaitForNextFrame();
  }
  worker.join();
  appletSetCpuBoostMode(ApmCpuBoostMode_Normal);

  if (!enough_space)
  {
    RenderMessage(
        "Not enough free space",
        std::array<std::string, 1>{"The destination does not have enough available space."}, true);
    return false;
  }

  if (ok && m_clipboard_move)
  {
    ReplaceSavedPathPrefix(source_path, destination);
    m_clipboard_path.clear();
    m_clipboard_move = false;
  }
  else if (m_clipboard_move && move_commit_started && !cleanup_ok)
  {
    RenderMessage(
        "Move incomplete",
        std::array<std::string, 2>{"The copy completed, but the original could not be removed.",
                                   "Review both locations before trying again."},
        true);
  }
  if (ok)
    Toast("Transfer complete", 800);
  else if (state.cancelled.load(std::memory_order_relaxed))
    Toast("Transfer cancelled", 800);
  else
  {
    const std::string error = TransferError(&state);
    RenderMessage("Transfer failed",
                  std::array<std::string, 1>{error.empty() ?
                                                 std::string(m_localization.Translate(
                                                     "The file transfer could not be completed.")) :
                                                 std::string(m_localization.Translate(error))});
  }
  return ok;
}

bool Launcher::RenamePath(const std::string& path)
{
  std::string name;
  if (!PromptText("Rename", FileName(path), &name, false, false))
    return false;
  name = Trim(std::move(name));
  if (!ValidEntryName(name))
  {
    RenderMessage(
        "Invalid name",
        std::array<std::string, 1>{"Names cannot contain /, \\, :, or control characters."}, true);
    return false;
  }
  const std::string destination = JoinPath(ParentPath(path), name);
  struct stat destination_info{};
  if (::lstat(destination.c_str(), &destination_info) == 0)
  {
    RenderMessage("Rename failed",
                  std::array<std::string, 1>{"An item with that name already exists."}, true);
    return false;
  }
  if (std::rename(path.c_str(), destination.c_str()) != 0)
  {
    RenderMessage("Rename failed", std::array<std::string, 1>{std::strerror(errno)});
    return false;
  }
  ReplaceSavedPathPrefix(path, destination);
  Toast("Renamed", 700);
  return true;
}

void Launcher::FileActions(const std::string& path)
{
  const int choice = Dropdown("File options", {"Copy", "Move", "Rename"}, -1);
  if (choice == 0 || choice == 1)
  {
    m_clipboard_path = path;
    m_clipboard_move = choice == 1;
    Toast(choice == 1 ? "Move queued" : "Copied to clipboard", 700);
  }
  else if (choice == 2)
    RenamePath(path);
}

std::string Launcher::GameLocationLabel(const Game& game) const
{
  if (game.installed_nand)
  {
    char location[40];
    std::snprintf(location, sizeof(location), "Wii NAND: %016llx",
                  static_cast<unsigned long long>(game.title_id));
    return location;
  }
  const std::string path = NormalizePath(game.path);
  for (const Storage::SmbShare& share : m_shares)
  {
    const std::string root = NormalizePath(Storage::SmbRootPath(share.id));
    if (root.empty() || !PathAtOrBelow(path, root))
      continue;
    std::string relative = path.substr(std::min(path.size(), root.size()));
    while (!relative.empty() && relative.front() == '/')
      relative.erase(relative.begin());
    std::string address = "SMB: smb://" + share.server + "/" + share.share;
    if (!relative.empty())
      address += "/" + relative;
    return address;
  }
  if (path.starts_with("sdmc:"))
    return "SD: " + path;
  if (Lower(path).starts_with("ums"))
    return "USB: " + path;
  return path;
}

std::string Launcher::FileBrowser(const std::string& start, bool select_folder, bool select_game,
                                  bool manage, std::span<const std::string_view> extensions,
                                  std::string_view selection_title)
{
  const auto ui = [&](std::string_view text) {
    return std::string(m_localization.Translate(text));
  };
  enum class Kind
  {
    UseFolder,
    Parent,
    Paste,
    Directory,
    File,
    Location,
    Smb,
    ManageSmb,
  };
  struct Entry
  {
    std::string label;
    std::string path;
    Kind kind = Kind::File;
    std::string value;
    // Present only for USB roots on the Locations page.  This is the physical/volume identity,
    // never the mutable umsN: alias shown in path.
    std::string usb_id;
  };
  const auto game_extension = [&](std::string_view name) {
    const std::string lower = Lower(std::string(name));
    if (!extensions.empty())
    {
      return std::ranges::any_of(extensions, [&](std::string_view extension) {
        return lower.ends_with(Lower(std::string(extension)));
      });
    }
    // Saturn disc images, as the library scan finds them
    static constexpr std::array<std::string_view, 5> EXTENSIONS = {".cue", ".chd", ".iso",
                                                                   ".ccd", ".mds"};
    return std::ranges::any_of(
        EXTENSIONS, [&](std::string_view extension) { return lower.ends_with(extension); });
  };
  std::string current = NormalizePath(start);
  if (!current.empty())
  {
    for (const Storage::SmbShare& share : m_shares)
    {
      if (!PathAtOrBelow(current, Storage::SmbRootPath(share.id)) ||
          Storage::IsSmbMounted(share.id))
        continue;
      std::string error;
      std::atomic_bool cancel_mount{false};
      bool mounted = false;
      RunBusyTask(
          "Connecting SMB share", share.name,
          [&] { mounted = Storage::MountSmb(share, &error, &cancel_mount); }, &cancel_mount);
      if (!mounted)
      {
        if (!cancel_mount.load(std::memory_order_acquire))
          RenderMessage("SMB connection failed", std::array<std::string, 2>{share.name, error});
        current.clear();
      }
      break;
    }
  }
  int selection = 0;
  int top = 0;
  constexpr int row_height = 46;
  const int list_top = SettingsListY();
  const int visible = std::max(1, (m_height - list_top - SettingsFooterReserve()) / row_height);
  std::vector<Entry> entries;
  std::string entries_path;
  std::uint64_t locations_generation = Storage::UsbStatusGeneration();
  bool refresh_entries = true;

  while (BeginFrame())
  {
    const std::uint64_t current_generation = Storage::UsbStatusGeneration();
    if (current != entries_path || (current.empty() && current_generation != locations_generation))
      refresh_entries = true;
    if (refresh_entries)
    {
      entries.clear();
      bool folder_opened = true;
      if (current.empty())
      {
        entries.push_back({ui("SD card"), "sdmc:/", Kind::Location, ui("Internal SD storage")});
        for (const Storage::Location& location : Storage::ListUsbLocations())
        {
          entries.push_back(
              {location.label, location.path, Kind::Location, ui("USB mass storage"), location.id});
        }
        for (const Storage::SmbShare& share : m_shares)
        {
          const bool mounted = Storage::IsSmbMounted(share.id);
          entries.push_back({ui("SMB") + " - " + (share.name.empty() ? share.share : share.name) +
                                 (mounted ? "" : " (" + ui("disconnected") + ")"),
                             Storage::SmbBrowsePath(share), Kind::Smb,
                             ui(mounted ? "SMB · Connected" : "SMB · Connect")});
        }
        entries.push_back(
            {ui("Manage SMB shares"), {}, Kind::ManageSmb, ui("Add / edit / connect")});
      }
      else
      {
        if (select_folder)
          entries.push_back({ui("[ Use this folder ]"), current, Kind::UseFolder, current});
        if (manage && !m_clipboard_path.empty())
          entries.push_back(
              {"[ " + ui("Paste") + " " + ui(m_clipboard_move ? "moved" : "copied") + " " +
                   ui("item here") + " ]",
               current, Kind::Paste,
               ui(m_clipboard_move ? "Move" : "Copy") + " " + FileName(m_clipboard_path)});
        entries.push_back({ui("[ .. locations / parent ]"), ParentPath(current), Kind::Parent,
                           ui("Parent folder")});
        const std::size_t fixed = entries.size();
        DIR* directory = ::opendir(current.c_str());
        if (directory)
        {
          while (dirent* item = ::readdir(directory))
          {
            if (item->d_name[0] == '.' &&
                (std::strcmp(item->d_name, ".") == 0 || std::strcmp(item->d_name, "..") == 0))
              continue;
            const std::string path = JoinPath(current, item->d_name);
            struct stat info{};
            if (::stat(path.c_str(), &info) != 0)
              continue;
            if (S_ISDIR(info.st_mode))
              entries.push_back(
                  {std::string(item->d_name) + "/", path, Kind::Directory, ui("Folder")});
            else if (!select_folder && (!select_game || game_extension(item->d_name)))
              entries.push_back({item->d_name, path, Kind::File, HumanBytes(info.st_size)});
          }
          ::closedir(directory);
        }
        else
        {
          folder_opened = false;
        }
        std::ranges::sort(entries.begin() + std::min(fixed, entries.size()), entries.end(),
                          [](const Entry& left, const Entry& right) {
                            if ((left.kind == Kind::Directory) != (right.kind == Kind::Directory))
                              return left.kind == Kind::Directory;
                            return Lower(left.label) < Lower(right.label);
                          });
      }
      if (!current.empty() && !folder_opened)
      {
        RenderMessage("Folder unavailable",
                      std::array<std::string, 3>{current, "",
                                                 std::string(m_localization.Translate(
                                                     "The device may be disconnected."))});
        current.clear();
        selection = top = 0;
        entries_path.clear();
        refresh_entries = true;
        continue;
      }
      if (entries.empty())
        entries.push_back({ui("No accessible locations"), {}, Kind::File, {}});
      entries_path = current;
      locations_generation = current_generation;
      refresh_entries = false;
    }
    selection = std::clamp(selection, 0, static_cast<int>(entries.size()) - 1);
    bool rebuild = false;
    SDL_Event event{};
    while (PollEvent(&event))
    {
      int touch_x = 0;
      int touch_y = 0;
      const TouchKind touch = FeedTouch(event, &touch_x, &touch_y);
      if (TouchScrollList(touch, &selection, &top, static_cast<int>(entries.size()), visible))
        continue;
      if (touch == TouchKind::Tap)
      {
        if (touch_y >= m_height - 48)
        {
          if (current.empty())
            return {};
          current = ParentPath(current);
          selection = top = 0;
          rebuild = true;
          break;
        }
        for (int row = 0; row < visible && top + row < static_cast<int>(entries.size()); ++row)
        {
          const int y = list_top + row * row_height;
          if (touch_y >= y && touch_y < y + 42)
          {
            selection = top + row;
            SDL_Event confirm{};
            confirm.type = SDL_CONTROLLERBUTTONDOWN;
            confirm.cbutton.button = BUTTON_CONFIRM;
            SDL_PushEvent(&confirm);
            break;
          }
        }
        continue;
      }
      const int direction = EventNavigation(event);
      if (direction)
        selection = (selection + direction + entries.size()) % entries.size();
      if (event.type == SDL_CONTROLLERBUTTONDOWN || event.type == SDL_KEYDOWN)
      {
        const bool confirm = event.type == SDL_CONTROLLERBUTTONDOWN ?
                                 event.cbutton.button == BUTTON_CONFIRM :
                                 event.key.keysym.sym == SDLK_RETURN;
        const bool cancel = event.type == SDL_CONTROLLERBUTTONDOWN ?
                                event.cbutton.button == BUTTON_CANCEL :
                                event.key.keysym.sym == SDLK_ESCAPE;
        const bool options =
            event.type == SDL_CONTROLLERBUTTONDOWN && event.cbutton.button == BUTTON_SETTINGS;
        const bool paste = event.type == SDL_CONTROLLERBUTTONDOWN &&
                           event.cbutton.button == SDL_CONTROLLER_BUTTON_X;
        const bool eject = event.type == SDL_CONTROLLERBUTTONDOWN ?
                               event.cbutton.button == SDL_CONTROLLER_BUTTON_START :
                               event.key.keysym.sym == SDLK_DELETE;
        if (cancel)
        {
          if (!current.empty())
          {
            current = ParentPath(current);
            selection = top = 0;
            rebuild = true;
          }
          else
          {
            return {};
          }
        }
        else if (eject && manage && current.empty() && entries[selection].kind == Kind::Location &&
                 !entries[selection].usb_id.empty())
        {
          if (EjectUsbLocation(entries[selection].usb_id))
          {
            selection = top = 0;
            rebuild = true;
          }
        }
        else if (options && (entries[selection].kind == Kind::Directory ||
                             entries[selection].kind == Kind::File ||
                             entries[selection].kind == Kind::UseFolder))
        {
          if (manage)
            FileActions(entries[selection].path);
          rebuild = true;
        }
        else if (paste && manage && !current.empty() && !m_clipboard_path.empty())
        {
          ExecutePaste(current);
          rebuild = true;
        }
        else if (confirm)
        {
          Entry entry = entries[selection];
          if (entry.kind == Kind::UseFolder)
            return NormalizePath(entry.path);
          if (entry.kind == Kind::Parent)
          {
            current = entry.path;
            selection = top = 0;
            rebuild = true;
          }
          else if (entry.kind == Kind::Directory || entry.kind == Kind::Location)
          {
            current = NormalizePath(entry.path);
            selection = top = 0;
            rebuild = true;
          }
          else if (entry.kind == Kind::Smb)
          {
            const auto iterator =
                std::ranges::find_if(m_shares, [&](const Storage::SmbShare& share) {
                  return Storage::SmbBrowsePath(share) == entry.path;
                });
            if (iterator != m_shares.end() && !Storage::IsSmbMounted(iterator->id))
            {
              std::string error;
              std::atomic_bool cancel_mount{false};
              bool mounted = false;
              RunBusyTask(
                  "Connecting SMB share", iterator->name,
                  [&] { mounted = Storage::MountSmb(*iterator, &error, &cancel_mount); },
                  &cancel_mount);
              if (!mounted)
              {
                if (!cancel_mount.load(std::memory_order_acquire))
                  RenderMessage("SMB connection failed", std::array<std::string, 1>{error});
                rebuild = true;
                continue;
              }
            }
            current = NormalizePath(entry.path);
            selection = top = 0;
            rebuild = true;
          }
          else if (entry.kind == Kind::ManageSmb)
          {
            NetworkSharesScreen();
            rebuild = true;
          }
          else if (entry.kind == Kind::File)
          {
            if (select_game)
              return entry.path;
          }
          else if (entry.kind == Kind::Paste)
          {
            ExecutePaste(current);
            rebuild = true;
          }
        }
      }
      if (rebuild)
        break;
    }
    if (rebuild)
    {
      refresh_entries = true;
      continue;
    }
    if (selection < top)
      top = selection;
    if (selection >= top + visible)
      top = selection - visible + 1;
    ClearBackground();
    const std::string_view screen_title = !selection_title.empty() ? selection_title :
                                          manage                   ? "File manager" :
                                          select_game              ? "Select game" :
                                                                     "Select game folder";
    DrawHeader(screen_title, current.empty() ? m_localization.Translate("Locations") :
                                               std::string_view(current));
    constexpr int x = 54;
    const int width = m_width - 108;
    const int shown_rows =
        std::min(visible, std::max(0, static_cast<int>(entries.size()) - top));
    if (shown_rows > 0)
      GlassPanel(x - 10, list_top - 16, width + 20, shown_rows * row_height + 18);
    for (int row = 0; row < visible && top + row < static_cast<int>(entries.size()); ++row)
    {
      const int index = top + row;
      const int y = list_top + row * row_height;
      if (index == selection)
      {
        DrawRowHighlight(x, y - 3, width, 42);
      }
      const bool action_row =
          entries[index].kind == Kind::UseFolder || entries[index].kind == Kind::Paste;
      const SDL_Color color = action_row                        ? m_highlight :
                              entries[index].kind == Kind::File ? SDL_Color{120, 220, 120, 255} :
                                                                  m_text;
      if (index == selection)
        DrawScrollingTextLeft(m_font, 80, y, m_width - 180, entries[index].label, m_value);
      else
        DrawText(m_font, 80, y, Ellipsize(m_font, entries[index].label, m_width - 180), color);
    }
    const bool usb_root_selected = manage && current.empty() && selection >= 0 &&
                                   selection < static_cast<int>(entries.size()) &&
                                   entries[selection].kind == Kind::Location &&
                                   !entries[selection].usb_id.empty();
    if (manage)
    {
      if (usb_root_selected)
      {
        static constexpr std::array<std::pair<std::string_view, std::string_view>, 3> hints = {
            std::pair{"A", "Open"}, std::pair{"+", "Safely eject"}, std::pair{"B", "Back"}};
        DrawFooter(hints);
      }
      else if (current.empty())
      {
        static constexpr std::array<std::pair<std::string_view, std::string_view>, 2> hints = {
            std::pair{"A", "Open"}, std::pair{"B", "Back"}};
        DrawFooter(hints);
      }
      else
      {
        const Kind kind = entries[selection].kind;
        std::array<std::pair<std::string_view, std::string_view>, 4> hints{};
        int hint_count = 0;
        if (kind == Kind::Paste)
          hints[hint_count++] = {"A", "Paste"};
        else if (kind != Kind::File)
          hints[hint_count++] = {"A", "Open"};
        if (kind == Kind::Directory || kind == Kind::File)
          hints[hint_count++] = {"X", "Actions"};
        if (!m_clipboard_path.empty())
          hints[hint_count++] = {"Y", "Paste"};
        hints[hint_count++] = {"B", "Back"};
        DrawFooter(std::span(hints).first(hint_count));
      }
    }
    else
    {
      DrawSettingsFooter("A  Open / Select       B  Back");
    }
    SDL_RenderPresent(m_renderer);
    WaitForNextFrame();
  }
  return {};
}

void Launcher::FileManager()
{
  FileBrowser({}, false, false, true);
}

void Launcher::LibraryFilterMenu()
{
  std::vector<std::string> choices;
  choices.emplace_back(m_localization.Translate("All games"));
  choices.emplace_back(m_localization.Translate("Favorites"));
  for (const Collection& collection : m_collections)
    choices.push_back(collection.name);
  const int manage_index = static_cast<int>(choices.size());
  choices.emplace_back(m_localization.Translate("Manage collections..."));
  const int search_index = static_cast<int>(choices.size());
  choices.emplace_back(m_localization.Translate("Search..."));
  const int clear_search_index = static_cast<int>(choices.size());
  if (!m_search_query.empty())
    choices.emplace_back(m_localization.Translate("Clear search"));

  int current = 0;
  if (m_active_collection == "favorites")
    current = 1;
  else if (!m_active_collection.empty())
  {
    const auto found = std::ranges::find(m_collections, m_active_collection, &Collection::name);
    if (found != m_collections.end())
      current = 2 + static_cast<int>(found - m_collections.begin());
  }
  const int selected = Dropdown("Library view", choices, current, true, false);
  if (selected < 0)
    return;
  if (selected == 0)
    m_active_collection.clear();
  else if (selected == 1)
    m_active_collection = "favorites";
  else if (selected >= 2 && selected < manage_index)
    m_active_collection = m_collections[selected - 2].name;
  else if (selected == manage_index)
  {
    ManageCollections();
  }
  else if (selected == search_index)
  {
    std::string query;
    if (PromptText("Search games", m_search_query, &query, false, true,
                   "Search title, Game ID, platform, or path"))
      m_search_query = Trim(std::move(query));
  }
  else if (selected == clear_search_index)
  {
    m_search_query.clear();
  }
  RebuildVisibleGames();
}

void Launcher::ManageCollections()
{
  const auto normalize_name = [&](std::string name, std::string_view previous = {}) {
    name = Trim(std::move(name));
    if (name.size() > 64)
      name.resize(64);
    const bool invalid = name.empty() || Lower(name) == "favorites" ||
                         std::ranges::any_of(name, [](unsigned char character) {
                           return character < ' ' || character == ',' || character == '=';
                         });
    const bool duplicate = std::ranges::any_of(m_collections, [&](const Collection& collection) {
      return collection.name != previous && Lower(collection.name) == Lower(name);
    });
    return invalid || duplicate ? std::string{} : name;
  };

  RunRows(
      "Manage collections", {},
      [&] {
        std::vector<Row> rows{{"Create collection...", ">", true, false, false}};
        rows.reserve(m_collections.size() + 1);
        for (const Collection& collection : m_collections)
        {
          rows.push_back({collection.name,
                          std::to_string(collection.members.size()) +
                              (collection.members.size() == 1 ? " game" : " games"),
                          true, false, false, false, false});
        }
        return rows;
      },
      [&](int index, int) {
        if (index == 0)
        {
          std::string entered;
          if (!PromptText("Collection name", {}, &entered, false, false))
            return false;
          entered = normalize_name(std::move(entered));
          if (entered.empty())
          {
            Toast("Invalid or duplicate collection name", 1000);
            return false;
          }
          m_collections.push_back({std::move(entered), {}});
          SaveCollections();
          return false;
        }

        const std::size_t collection_index = static_cast<std::size_t>(index - 1);
        if (collection_index >= m_collections.size())
          return false;
        Collection& collection = m_collections[collection_index];
        const int choice =
            Dropdown(collection.name, {"View collection", "Rename", "Delete"}, -1, false, true);
        if (choice == 0)
        {
          m_active_collection = collection.name;
          return true;
        }
        if (choice == 1)
        {
          const std::string old_name = collection.name;
          std::string entered;
          if (!PromptText("Rename collection", old_name, &entered, false, false))
            return false;
          entered = normalize_name(std::move(entered), old_name);
          if (entered.empty())
          {
            Toast("Invalid or duplicate collection name", 1000);
            return false;
          }
          collection.name = entered;
          if (m_active_collection == old_name)
            m_active_collection = entered;
          SaveCollections();
          return false;
        }
        if (choice == 2 &&
            Confirm("Delete collection?",
                    std::array<std::string, 2>{collection.name,
                                               "Games and save data will not be deleted."}))
        {
          if (m_active_collection == collection.name)
            m_active_collection.clear();
          m_collections.erase(m_collections.begin() + collection_index);
          SaveCollections();
        }
        return false;
      },
      true);
  RebuildVisibleGames();
}

void Launcher::EditGameOrganization(Game* game)
{
  if (!game)
    return;
  RunRows(
      "Favorites & collections", game->title,
      [&] {
        std::vector<Row> rows;
        rows.push_back({"Favorite", m_favorites.contains(game->key) ? "Yes" : "No"});
        rows.push_back({"Create collection...", ">", true, false, false});
        for (const Collection& collection : m_collections)
        {
          rows.push_back({collection.name,
                          collection.members.contains(game->key) ?
                              std::string(m_localization.Translate("Added")) :
                              std::string(m_localization.Translate("Not added")),
                          true, false, true, false});
        }
        return rows;
      },
      [&](int index, int) {
        if (index == 0)
        {
          if (!m_favorites.erase(game->key))
            m_favorites.insert(game->key);
        }
        else if (index == 1)
        {
          std::string name;
          if (!PromptText("Collection name", {}, &name, false, false))
            return false;
          name = Trim(std::move(name));
          if (name.size() > 64)
            name.resize(64);
          if (name.empty() || name == "favorites" ||
              std::ranges::any_of(name, [](unsigned char character) {
                return character < ' ' || character == ',' || character == '=';
              }))
          {
            Toast("Invalid collection name", 1000);
            return false;
          }
          const auto existing = std::ranges::find(m_collections, name, &Collection::name);
          if (existing == m_collections.end())
          {
            Collection collection;
            collection.name = name;
            collection.members.insert(game->key);
            m_collections.emplace_back(std::move(collection));
          }
          else
          {
            existing->members.insert(game->key);
          }
        }
        else
        {
          Collection& collection = m_collections[index - 2];
          if (!collection.members.erase(game->key))
            collection.members.insert(game->key);
        }
        SaveCollections();
        RebuildVisibleGames();
        return false;
      },
      true);
}

bool Launcher::EjectUsbLocation(std::string_view stable_id)
{
  const std::vector<Storage::Location> locations = Storage::GetUsbSnapshot().locations;
  const auto found = std::ranges::find(locations, stable_id, &Storage::Location::id);
  if (found == locations.end())
  {
    Toast("USB drive is no longer connected", 900);
    return false;
  }
  const Storage::Location location = *found;
  if (!Confirm(
          "Safely eject USB drive?",
          std::array<std::string, 3>{location.label, location.mount_alias,
                                     std::string(m_localization.Translate(
                                         "All partitions on this physical drive will unmount."))}))
    return false;
  StopGameScan();
  std::string error;
  bool ejected = false;
  RunBusyTask("Safely ejecting USB storage", location.label,
              [&] { ejected = Storage::SafelyEjectUsb(location.id, &error); });
  if (ejected)
  {
    m_library_refresh_requested = true;
    Toast("USB drive can now be removed", 1400);
    return true;
  }
  RenderMessage("USB eject failed", std::array<std::string, 1>{error});
  return false;
}

void Launcher::LibrarySettings()
{
  constexpr int row_count = 4;
  const int row_height = SettingsRowHeight();
  const int start_y = SettingsListY();
  auto& saved = m_row_positions["Library & storage\n"];
  int selection = std::clamp(saved.first, 0, row_count - 1);
  const auto open_row = [&] {
    if (selection == 0)
      GameSourcesScreen();
    else if (selection == 1)
      FileManager();
    else if (selection == 2)
      NetworkSharesScreen();
    else
      DownloadCovers();
    BeginScreenFx();
  };
  BeginScreenFx();
  while (BeginFrame())
  {
    SDL_Event event{};
    while (PollEvent(&event))
    {
      int touch_x = 0;
      int touch_y = 0;
      const TouchKind touch = FeedTouch(event, &touch_x, &touch_y);
      if (touch == TouchKind::Tap)
      {
        if (touch_y < TopBarHeight() || touch_y >= m_height - 40)
        {
          saved.first = selection;
          return;
        }
        for (int row = 0; row < row_count; ++row)
        {
          const int y = start_y + row * row_height;
          if (touch_y >= y && touch_y < y + row_height)
          {
            selection = row;
            open_row();
            if (m_pending_launch)
              return;
            break;
          }
        }
        continue;
      }
      const int direction = EventNavigation(event);
      if (direction)
        selection = (selection + direction + row_count) % row_count;
      if ((event.type == SDL_CONTROLLERBUTTONDOWN && event.cbutton.button == BUTTON_CONFIRM) ||
          (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_RETURN))
      {
        open_row();
        if (m_pending_launch)
          return;
      }
      else if ((event.type == SDL_CONTROLLERBUTTONDOWN && event.cbutton.button == BUTTON_CANCEL) ||
               (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE))
      {
        saved.first = selection;
        return;
      }
    }

    ClearBackground();
    DrawHeader("Library & storage");
    const int column_width = std::min(980, m_width - 180);
    const int column_x = (m_width - column_width) / 2;
    const int label_x = column_x + 40;
    const int value_x = column_x + column_width - 40;
    GlassPanel(column_x - 12, start_y - 10, column_width + 24, row_count * row_height + 18);
    const float target = static_cast<float>(start_y + selection * row_height + 2);
    m_highlight_y = (!m_animations || m_highlight_y < 0.0f) ?
                        target :
                        m_highlight_y + (target - m_highlight_y) * 0.30f;
    DrawRowHighlight(column_x, static_cast<int>(m_highlight_y), column_width, row_height - 4);

    const int connected = std::ranges::count_if(
        m_shares, [](const Storage::SmbShare& share) { return Storage::IsSmbMounted(share.id); });
    const std::string folder_value =
        std::to_string(m_sources.size()) + " " +
        std::string(m_localization.Translate(m_sources.size() == 1 ? "folder" : "folders"));
    const std::string smb_value = std::to_string(connected) + " / " +
                                  std::to_string(m_shares.size()) + " " +
                                  std::string(m_localization.Translate("connected"));
    const std::array<std::string_view, row_count> labels = {
        "Game folders", "File manager", "SMB network shares", "Download covers"};
    const std::array<std::string, row_count> values = {
        folder_value, std::string(m_localization.Translate("SD / USB / SMB")), smb_value,
        std::string(m_localization.Translate("SteamGridDB batch"))};
    for (int row = 0; row < row_count; ++row)
    {
      const bool current = row == selection;
      DrawSettingsRowText(m_localization.Translate(labels[row]), values[row],
                          start_y + row * row_height, column_width, label_x, value_x, current,
                          current ? m_value : m_text, current ? m_value : m_dim, false,
                          row_height);
    }
    DrawSettingsFooter("A  Open       B  Back");
    DrawFadeIn();
    SDL_RenderPresent(m_renderer);
    WaitForNextFrame();
  }
  saved.first = selection;
}

bool Launcher::ChooseForwarderIcon(Game* game, std::string* output_path)
{
  if (!game || !output_path)
    return false;
  const std::string base = std::string(DATA_DIRECTORY) + "/forwarders";
  const std::string temporary_directory = base + "/iconpick";
  EnsureDirectory(base);
  EnsureDirectory(temporary_directory);
  if (DIR* directory = ::opendir(temporary_directory.c_str()))
  {
    while (dirent* entry = ::readdir(directory))
    {
      const std::string name = entry->d_name;
      if (name.starts_with("gicon_") && name.ends_with(".png"))
        std::remove(JoinPath(temporary_directory, name).c_str());
    }
    ::closedir(directory);
  }

  std::vector<std::string> paths;
  std::atomic_bool cancel{false};
  const std::string cover_path = CoverPath(*game);
  if (RegularFileExists(cover_path))
    paths.push_back(cover_path);
  paths.emplace_back("romfs:/fwd/icon.jpg");

  const std::string api_key = m_store.Get("Network/SteamGridDBKey");
  if (!api_key.empty())
  {
    RunBusyTask(
        "Fetching icons from SteamGridDB", game->title,
        [&] {
          CoverDownload::RequestOptions options{&cancel};
          std::vector<CoverDownload::GameResult> games;
          if (CoverDownload::SearchGames(api_key, game->title, &games, &options) ==
                  CoverDownload::Result::Ok &&
              !games.empty())
          {
            std::vector<CoverDownload::Artwork> icons;
            if (CoverDownload::FetchIcons(api_key, games.front().id, &icons, &options) ==
                CoverDownload::Result::Ok)
            {
              for (std::size_t index = 0;
                   index < icons.size() && index < 14 && !cancel.load(std::memory_order_acquire);
                   ++index)
              {
                const std::string path =
                    JoinPath(temporary_directory, "gicon_" + std::to_string(index) + ".png");
                if (CoverDownload::DownloadImage(icons[index].url, path, &options) ==
                    CoverDownload::Result::Ok)
                  paths.push_back(path);
              }
            }
          }
        },
        &cancel);
  }
  if (paths.empty())
  {
    Toast("No icon found - download a cover first", 1600);
    return false;
  }

  const int count = static_cast<int>(paths.size());
  const int columns = std::max(1, std::min(count, 5));
  const int rows = (count + columns - 1) / columns;
  constexpr int gap = 24;
  const int top = TopBarHeight() + 32;
  const int bottom = m_height - SettingsFooterReserve() - 12;
  const int cell = std::max(48, std::min({200, (m_width - 96 - (columns - 1) * gap) / columns,
                                          (bottom - top - (rows - 1) * gap) / rows}));
  const int x0 = (m_width - (columns * cell + (columns - 1) * gap)) / 2;
  const int y0 = top + std::max(0, (bottom - top - rows * cell - (rows - 1) * gap) / 2);
  std::vector<SDL_Texture*> textures(count, nullptr);
  for (int index = 0; index < count; ++index)
    textures[index] = LoadScaledTexture(paths[index], cell, cell);
  int selection = 0;
  int chosen = -1;
  bool done = false;
  BeginScreenFx();
  while (!done && BeginFrame())
  {
    SDL_Event event{};
    while (PollEvent(&event))
    {
      int touch_x = 0;
      int touch_y = 0;
      const TouchKind touch = FeedTouch(event, &touch_x, &touch_y);
      if (touch == TouchKind::ScrollUp)
      {
        selection = std::min(count - 1, selection + columns);
        continue;
      }
      if (touch == TouchKind::ScrollDown)
      {
        selection = std::max(0, selection - columns);
        continue;
      }
      if (touch == TouchKind::Tap)
      {
        if (touch_y >= m_height - 40)
        {
          done = true;
          continue;
        }
        for (int index = 0; index < count; ++index)
        {
          const int row = index / columns;
          const int column = index % columns;
          const int x = x0 + column * (cell + gap);
          const int y = y0 + row * (cell + gap);
          if (touch_x >= x && touch_x < x + cell && touch_y >= y && touch_y < y + cell)
          {
            selection = index;
            chosen = index;
            done = true;
            break;
          }
        }
        continue;
      }
      if (event.type == SDL_KEYDOWN)
      {
        if (event.key.keysym.sym == SDLK_RIGHT)
          selection = (selection + 1) % count;
        else if (event.key.keysym.sym == SDLK_LEFT)
          selection = (selection + count - 1) % count;
        else if (event.key.keysym.sym == SDLK_DOWN)
          selection = (selection + columns) % count;
        else if (event.key.keysym.sym == SDLK_UP)
          selection = (selection - columns + count) % count;
        else if (event.key.keysym.sym == SDLK_RETURN)
        {
          chosen = selection;
          done = true;
        }
        else if (event.key.keysym.sym == SDLK_ESCAPE)
          done = true;
      }
      if (event.type != SDL_CONTROLLERBUTTONDOWN)
        continue;
      if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT)
        selection = (selection + 1) % count;
      else if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT)
        selection = (selection + count - 1) % count;
      else if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)
        selection = (selection + columns) % count;
      else if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_UP)
        selection = (selection - columns + count) % count;
      else if (event.cbutton.button == BUTTON_CONFIRM)
      {
        chosen = selection;
        done = true;
      }
      else if (event.cbutton.button == BUTTON_CANCEL)
        done = true;
    }

    ClearBackground();
    DrawHeader("Choose an icon", game->title);
    for (int index = 0; index < count; ++index)
    {
      const int row = index / columns;
      const int column = index % columns;
      const int x = x0 + column * (cell + gap);
      const int y = y0 + row * (cell + gap);
      if (index == selection)
        RoundedPanel(x - 8, y - 8, cell + 16, cell + 16, m_panel, m_selection);
      else
        GlassPanel(x - 8, y - 8, cell + 16, cell + 16);
      FillRect(x, y, cell, cell, m_card);
      if (textures[index])
      {
        SDL_Rect destination{x, y, cell, cell};
        SDL_RenderCopy(m_renderer, textures[index], nullptr, &destination);
      }
      else
      {
        DrawTextCentered(m_font_small, x + cell / 2, y + cell / 2, "?", m_dim);
      }
    }
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 2> footer = {
        std::pair{"A", "Use icon"}, std::pair{"B", "Back"}};
    DrawFooter(footer);
    DrawFadeIn();
    SDL_RenderPresent(m_renderer);
    WaitForNextFrame();
  }
  for (SDL_Texture* texture : textures)
  {
    if (texture)
      SDL_DestroyTexture(texture);
  }
  if (chosen < 0 || chosen >= count)
    return false;
  *output_path = paths[chosen];
  return true;
}

void Launcher::CreateHomeShortcut(Game* game)
{
  if (!game)
    return;
  const GameDetailLayout layout = ComputeGameDetailLayout();
  constexpr int icon_size = 260;
  const int icon_x = layout.preview.x + (layout.preview.w - icon_size) / 2;
  const int icon_y = layout.preview.y + (layout.preview.h - icon_size) / 2;
  constexpr int panel_height = 340;
  const SDL_Rect panel{layout.content.x, layout.content.y + (layout.content.h - panel_height) / 2,
                       layout.content.w, panel_height};
  const int right_x = panel.x + 24;
  const int right_width = panel.w - 48;
  const int name_y = panel.y + 32;
  const int author_y = panel.y + 126;
  const int create_y = panel.y + 240;
  constexpr int field_height = 86;
  constexpr int create_height = 68;
  std::string name = game->title;
  std::string author =
      game->metadata ?
          game->metadata->GetMaker() :
          std::string{};
  if (author.empty())
    author = "Thorhax";
  std::string icon_path = CoverPath(*game);
  if (!RegularFileExists(icon_path))
    icon_path = "romfs:/fwd/icon.jpg";
  SDL_Texture* icon = LoadScaledTexture(icon_path, icon_size, icon_size);
  int selection = 0;
  bool done = false;

  const auto edit = [&](std::string_view title, std::string* value) {
    std::string replacement;
    if (PromptText(title, *value, &replacement, false, false) && !replacement.empty())
      *value = std::move(replacement);
  };
  const auto build = [&] {
    if (icon_path.empty())
    {
      Toast("Pick an icon first", 1200);
      return;
    }
    std::array<char, 512> error{};
    bool created = false;
    std::vector<std::string> legacy_game_paths;
    if (!game->installed_nand)
    {
      const auto identity =
          std::ranges::find(m_library_identities, game->key, &LibraryIdentityRecord::id);
      if (identity != m_library_identities.end())
        legacy_game_paths = identity->previous_paths;
      // The installed shortcut immediately depends on this launcher.ini record. Commit it before
      // making the external HOME Menu mutation so a crash or power loss cannot leave a shortcut
      // referring to a progressive-scan identity which only existed in memory.
      if (m_library_identities_dirty)
        SaveLibraryIdentities();
      FlushPendingSaves();
    }
    RunBusyTask("Creating HOME shortcut", game->title, [&] {
      created = game->installed_nand ?
                    Forwarder::CreateNANDTitle(game->title_id, name, author, icon_path, game->key,
                                               error.data(), error.size()) :
                    Forwarder::Create(game->path, name, author, icon_path,
                                      game->config_override_path, game->key, legacy_game_paths,
                                      error.data(), error.size());
    });
    if (created)
    {
      Toast("HOME shortcut installed", 1800);
      done = true;
    }
    else
    {
      RenderMessage("Shortcut failed",
                    std::array<std::string, 1>{error[0] ? error.data() : "Unknown error"});
    }
    BeginScreenFx();
  };
  const auto activate = [&] {
    if (selection == 0)
    {
      std::string selected_path;
      if (ChooseForwarderIcon(game, &selected_path))
      {
        icon_path = std::move(selected_path);
        if (icon)
          SDL_DestroyTexture(icon);
        icon = LoadScaledTexture(icon_path, icon_size, icon_size);
      }
      BeginScreenFx();
    }
    else if (selection == 1)
    {
      edit("Shortcut name", &name);
      BeginScreenFx();
    }
    else if (selection == 2)
    {
      edit("Author", &author);
      BeginScreenFx();
    }
    else
    {
      build();
    }
  };

  BeginScreenFx();
  while (!done && BeginFrame())
  {
    SDL_Event event{};
    while (PollEvent(&event))
    {
      int touch_x = 0;
      int touch_y = 0;
      const TouchKind touch = FeedTouch(event, &touch_x, &touch_y);
      if (touch == TouchKind::Tap)
      {
        if (touch_x >= icon_x && touch_x < icon_x + icon_size && touch_y >= icon_y &&
            touch_y < icon_y + icon_size)
        {
          selection = 0;
          activate();
        }
        else if (touch_x >= right_x - 10 && touch_x < right_x + right_width + 10 &&
                 touch_y >= name_y - 6 && touch_y < name_y - 6 + field_height)
        {
          selection = 1;
          activate();
        }
        else if (touch_x >= right_x - 10 && touch_x < right_x + right_width + 10 &&
                 touch_y >= author_y - 6 && touch_y < author_y - 6 + field_height)
        {
          selection = 2;
          activate();
        }
        else if (touch_x >= right_x - 10 && touch_x < right_x + right_width + 10 &&
                 touch_y >= create_y - 6 && touch_y < create_y - 6 + create_height)
        {
          selection = 3;
          activate();
        }
        else if (touch_y >= m_height - 40)
        {
          done = true;
        }
        continue;
      }
      if (event.type == SDL_KEYDOWN)
      {
        if (event.key.keysym.sym == SDLK_LEFT)
          selection = 0;
        else if (event.key.keysym.sym == SDLK_RIGHT && selection == 0)
          selection = 1;
        else if (event.key.keysym.sym == SDLK_UP)
          selection = selection == 0 ? 3 : (selection == 1 ? 3 : selection - 1);
        else if (event.key.keysym.sym == SDLK_DOWN)
          selection = selection == 0 ? 1 : (selection == 3 ? 1 : selection + 1);
        else if (event.key.keysym.sym == SDLK_RETURN)
          activate();
        else if (event.key.keysym.sym == SDLK_ESCAPE)
          done = true;
      }
      if (event.type != SDL_CONTROLLERBUTTONDOWN)
        continue;
      if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT)
        selection = 0;
      else if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT && selection == 0)
        selection = 1;
      else if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_UP)
        selection = selection == 0 ? 3 : (selection == 1 ? 3 : selection - 1);
      else if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)
        selection = selection == 0 ? 1 : (selection == 3 ? 1 : selection + 1);
      else if (event.cbutton.button == BUTTON_CONFIRM)
        activate();
      else if (event.cbutton.button == BUTTON_CANCEL)
        done = true;
    }

    ClearBackground();
    DrawHeader("Create HOME shortcut", game->title);
    DrawArtworkPreview(icon, SDL_Rect{icon_x, icon_y, icon_size, icon_size}, selection == 0,
                       "(no icon)");
    DrawTextCentered(m_font_small, icon_x + icon_size / 2, icon_y + icon_size + 20,
                     m_localization.Translate("Icon"), selection == 0 ? m_value : m_dim);
    GlassPanel(panel.x, panel.y, panel.w, panel.h);
    const auto field = [&](int index, int y, std::string_view label, std::string_view value) {
      const bool current = selection == index;
      RoundedRect(right_x - 10, y - 6, right_width + 20, field_height, 4, m_card);
      if (current)
        DrawRowHighlight(right_x - 10, y - 6, right_width + 20, field_height);
      DrawText(m_font_small, right_x + 8, y + 4, label, m_highlight);
      DrawScrollingTextLeft(m_font, right_x + 8, y + 34, right_width - 16, value,
                            current ? m_value : m_text);
    };
    field(1, name_y, m_localization.Translate("Name"), name);
    const std::string metadata =
        author + "  |  v" YAB_NX_RELEASE_VERSION;
    field(2, author_y, m_localization.Translate("Author / Version"), metadata);
    const bool create_selected = selection == 3;
    DrawButtonPanel(right_x - 10, create_y - 6, right_width + 20, create_height, create_selected);
    DrawTextCentered(m_font, right_x + right_width / 2,
                     create_y - 6 + (create_height - FontHeight(m_font)) / 2,
                     m_localization.Translate("Create shortcut"),
                     create_selected ? m_value : m_text);
    const std::array<std::pair<std::string_view, std::string_view>, 2> footer = {
        std::pair<std::string_view, std::string_view>{
            "A", create_selected ? "Create shortcut" : "Edit / choose"},
        std::pair<std::string_view, std::string_view>{"B", "Back"}};
    DrawFooter(footer);
    DrawFadeIn();
    SDL_RenderPresent(m_renderer);
    WaitForNextFrame();
  }
  if (icon)
    SDL_DestroyTexture(icon);
}

bool Launcher::EditSmbShare(Storage::SmbShare* share, bool creating)
{
  if (!share)
    return false;
  Storage::SmbShare edited = *share;
  constexpr int field_count = 7;
  constexpr int save_row = 7;
  constexpr int total_rows = 8;
  int selection = 0;
  bool done = false;
  bool saved = false;

  const auto clean_server = [&] {
    edited.server = Trim(edited.server);
    if (Lower(edited.server).starts_with("smb://"))
      edited.server.erase(0, 6);
    while (!edited.server.empty() && edited.server.back() == '/')
      edited.server.pop_back();
  };
  const auto clean_share = [&] {
    std::string combined = Trim(edited.share);
    if (!edited.path.empty())
      combined += "/" + edited.path;
    std::ranges::replace(combined, '\\', '/');
    while (!combined.empty() && combined.front() == '/')
      combined.erase(combined.begin());
    while (!combined.empty() && combined.back() == '/')
      combined.pop_back();
    std::string normalized;
    bool slash = false;
    for (const char value : combined)
    {
      if (value == '/')
      {
        if (slash)
          continue;
        slash = true;
      }
      else
      {
        slash = false;
      }
      normalized += value;
    }
    const std::size_t separator = normalized.find('/');
    edited.share = Trim(normalized.substr(0, separator));
    edited.path =
        separator == std::string::npos ? std::string{} : Trim(normalized.substr(separator + 1));
  };
  const auto shared_folder = [&] {
    return edited.path.empty() ? edited.share : edited.share + "/" + edited.path;
  };
  const auto validate = [&] {
    edited.name = Trim(edited.name);
    clean_server();
    clean_share();
    if (edited.name.empty())
    {
      RenderMessage(
          "Display name required",
          std::array<std::string, 1>{"Enter a name used to identify this share in the launcher."}, true);
      return false;
    }
    if (edited.server.empty() || edited.server.find('/') != std::string::npos ||
        edited.server.find('\\') != std::string::npos)
    {
      RenderMessage("Invalid SMB server",
                    std::array<std::string, 2>{"Enter only a host name or IP address.",
                                               "Example: 192.168.1.20"},
                    true);
      return false;
    }
    bool invalid_path = edited.share.empty() || edited.share.find(':') != std::string::npos;
    std::size_t start = 0;
    while (!invalid_path && start <= edited.path.size())
    {
      const std::size_t slash = edited.path.find('/', start);
      const std::string component = Trim(edited.path.substr(
          start, slash == std::string::npos ? std::string::npos : slash - start));
      if ((!edited.path.empty() && component.empty()) || component == "." || component == ".." ||
          component.find(':') != std::string::npos)
        invalid_path = true;
      if (slash == std::string::npos)
        break;
      start = slash + 1;
    }
    if (invalid_path)
    {
      RenderMessage(
          "Invalid SMB share",
          std::array<std::string, 2>{"Enter a share name, optionally followed by folders.",
                                     "Do not include a drive letter or smb:// prefix."},
          true);
      return false;
    }
    return true;
  };
  const auto edit_field = [&](int index) {
    std::string value;
    bool accepted = false;
    if (index == 0)
      accepted = PromptText("SMB display name", edited.name, &value, false, false,
                            "Friendly name shown in the file browser.",
                            "Example: Living room NAS");
    else if (index == 1)
      accepted = PromptText("Server or IP address", edited.server, &value, false, false,
                            "Host only; do not include smb:// or a folder.",
                            "Example: 192.168.1.20 or NAS.local");
    else if (index == 2)
      accepted = PromptText("Shared folder", shared_folder(), &value, false, false,
                            "Enter the share and an optional folder path inside it.",
                            "Nested folders are supported");
    else if (index == 3)
      accepted = PromptText("Username", edited.user, &value, false, true,
                            "Leave blank for guest access.", "Leave blank for guest");
    else if (index == 4)
      accepted = PromptText("Password", edited.password, &value, true, true,
                            "Stored in launcher.ini; leave blank when not required.",
                            "Leave blank when no password is required");
    else if (index == 5)
      accepted =
          PromptText("Workgroup", edited.domain, &value, false, true,
                     "Usually optional on a home network.", "Example: WORKGROUP, or leave blank");
    if (!accepted)
      return;
    if (index == 0)
      edited.name = value;
    else if (index == 1)
    {
      edited.server = value;
      clean_server();
    }
    else if (index == 2)
    {
      edited.share = value;
      edited.path.clear();
      clean_share();
    }
    else if (index == 3)
      edited.user = value;
    else if (index == 4)
      edited.password = value;
    else if (index == 5)
      edited.domain = value;
    BeginScreenFx();
  };
  const auto activate = [&] {
    if (selection < 6)
    {
      edit_field(selection);
    }
    else if (selection == 6)
    {
      edited.auto_mount = !edited.auto_mount;
    }
    else if (validate())
    {
      if (creating || edited.id.empty())
      {
        std::unordered_set<std::string> ids;
        for (const Storage::SmbShare& existing : m_shares)
          ids.insert(existing.id);
        std::uint64_t seed = armGetSystemTick();
        do
        {
          char id[17];
          std::snprintf(id, sizeof(id), "%08llx",
                        static_cast<unsigned long long>(seed & 0xffffffffULL));
          edited.id = id;
          seed = seed * 6364136223846793005ULL + 1;
        } while (ids.contains(edited.id));
      }
      *share = std::move(edited);
      saved = true;
      done = true;
    }
  };

  BeginScreenFx();
  while (!done && BeginFrame())
  {
    SDL_Event event{};
    while (PollEvent(&event))
    {
      int touch_x = 0;
      int touch_y = 0;
      const TouchKind touch = FeedTouch(event, &touch_x, &touch_y);
      const int scale = 2;
      const int row_height = 27 * scale;
      const int y0 = TopBarHeight() + 26;
      const int margin = 56;
      const int help_width = 420;
      const int gap = 28;
      const int form_width = m_width - margin * 2 - help_width - gap;
      if (touch == TouchKind::Tap)
      {
        if (touch_y >= m_height - 42)
        {
          done = true;
          continue;
        }
        for (int index = 0; index < field_count; ++index)
        {
          if (touch_x >= margin && touch_x < margin + form_width &&
              touch_y >= y0 + index * row_height && touch_y < y0 + (index + 1) * row_height)
          {
            selection = index;
            activate();
            break;
          }
        }
        const int button_y = y0 + field_count * row_height + 10;
        if (touch_x >= margin && touch_x < margin + form_width && touch_y >= button_y &&
            touch_y < button_y + row_height)
        {
          selection = save_row;
          activate();
        }
        continue;
      }
      if (event.type == SDL_KEYDOWN)
      {
        if (event.key.keysym.sym == SDLK_UP)
          selection = (selection + total_rows - 1) % total_rows;
        else if (event.key.keysym.sym == SDLK_DOWN)
          selection = (selection + 1) % total_rows;
        else if ((event.key.keysym.sym == SDLK_LEFT || event.key.keysym.sym == SDLK_RIGHT) &&
                 selection == 6)
          edited.auto_mount = !edited.auto_mount;
        else if (event.key.keysym.sym == SDLK_RETURN)
          activate();
        else if (event.key.keysym.sym == SDLK_ESCAPE)
          done = true;
      }
      if (event.type != SDL_CONTROLLERBUTTONDOWN)
        continue;
      if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_UP)
        selection = (selection + total_rows - 1) % total_rows;
      else if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)
        selection = (selection + 1) % total_rows;
      else if ((event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT ||
                event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) &&
               selection == 6)
        edited.auto_mount = !edited.auto_mount;
      else if (event.cbutton.button == BUTTON_CONFIRM)
        activate();
      else if (event.cbutton.button == BUTTON_CANCEL)
        done = true;
    }

    ClearBackground();
    DrawHeader(creating ? "Add SMB network share" : "Edit SMB network share", edited.name);
    const int scale = 2;
    const int row_height = 27 * scale;
    const int y0 = TopBarHeight() + 26;
    const int margin = 56;
    const int help_width = 420;
    const int gap = 28;
    const int form_width = m_width - margin * 2 - help_width - gap;
    const int help_x = margin + form_width + gap;
    const int panel_height = field_count * row_height + row_height + 30;
    GlassPanel(margin, y0 - 10, form_width, panel_height);
    GlassPanel(help_x, y0 - 10, help_width, panel_height);
    static constexpr std::array<std::string_view, field_count> labels = {
        "Display name", "Server / IP address", "Shared folder",     "Username",
        "Password",     "Workgroup",           "Connect at startup"};
    const std::string password =
        edited.password.empty() ?
            std::string(m_localization.Translate("Not set")) :
            std::string(std::min<std::size_t>(16, edited.password.size()), '*');
    const std::array<std::string, field_count> values = {
        edited.name.empty() ? std::string(m_localization.Translate("Not set")) : edited.name,
        edited.server.empty() ? std::string(m_localization.Translate("Not set")) : edited.server,
        edited.share.empty() ? std::string(m_localization.Translate("Not set")) : shared_folder(),
        edited.user.empty() ? std::string(m_localization.Translate("Guest")) : edited.user,
        password,
        edited.domain.empty() ? std::string(m_localization.Translate("Optional")) : edited.domain,
        std::string(m_localization.Translate(edited.auto_mount ? "On" : "Off"))};
    for (int index = 0; index < field_count; ++index)
    {
      const int y = y0 + index * row_height;
      const bool current = selection == index;
      if (current)
      {
        DrawRowHighlight(margin + 8, y, form_width - 16, row_height - 2);
      }
      DrawText(m_font_small, margin + 30, y + (row_height - FontHeight(m_font_small)) / 2,
               m_localization.Translate(labels[index]), current ? m_value : m_dim);
      DrawScrollingTextRight(m_font, margin + form_width - 24,
                             y + (row_height - FontHeight(m_font)) / 2, form_width / 2 - 30,
                             values[index], current ? m_value : m_text);
    }
    const int button_y = y0 + field_count * row_height + 10;
    const bool button_selected = selection == save_row;
    DrawButtonPanel(margin + 14, button_y, form_width - 28, row_height - 4, button_selected);
    DrawTextCentered(m_font, margin + form_width / 2,
                     button_y + (row_height - FontHeight(m_font)) / 2 - 2,
                     m_localization.Translate(creating ? "Connect and save" : "Save changes"),
                     button_selected ? m_value : m_highlight);

    static constexpr std::array<std::string_view, total_rows> help_titles = {
        "Display name", "Server / IP address", "Shared folder",      "Username",
        "Password",     "Workgroup",           "Connect at startup", "Save share"};
    static constexpr std::array<std::string_view, total_rows> help_line_1 = {
        "A friendly name shown only in the launcher.",
        "The host name or IP of your SMB server.",
        "The share name and optional folder path.",
        "Leave blank when the share allows guests.",
        "The password for the selected account.",
        "Usually optional on home networks.",
        "Reconnect this share when the launcher opens.",
        "Validate the fields and connect to the share."};
    static constexpr std::array<std::string_view, total_rows> help_line_2 = {
        "Example: Living room NAS",
        "Example: 192.168.1.20 or NAS.local",
        "Nested folders are supported.",
        "Use the account configured on your NAS or PC.",
        "The value is masked on this screen.",
        "Example: WORKGROUP",
        "Turn this off for manually connected shares.",
        "Connection errors will be shown after saving."};
    DrawText(m_font_large, help_x + 28, y0 + 22, m_localization.Translate(help_titles[selection]),
             m_highlight);
    const int help_line_height = FontHeight(m_font_small) + 4;
    DrawWrapped(m_font_small, help_x + 28, y0 + 92, help_width - 56, help_line_height, 2,
                m_localization.Translate(help_line_1[selection]), m_text);
    DrawWrapped(m_font_small, help_x + 28, y0 + 156, help_width - 56, help_line_height, 2,
                m_localization.Translate(help_line_2[selection]), m_dim);
    const std::string address =
        "smb://" + (edited.server.empty() ? std::string("server") : edited.server) + "/" +
        (edited.share.empty() ? std::string("share") : shared_folder());
    DrawText(m_font_small, help_x + 28, y0 + 210, m_localization.Translate("Connection preview"),
             m_dim);
    DrawScrollingTextLeft(m_font, help_x + 28, y0 + 244, help_width - 56, address, m_value);
    const std::array<std::pair<std::string_view, std::string_view>, 2> hints = {
        std::pair<std::string_view, std::string_view>{
            "A", button_selected ? (creating ? "Connect and save" : "Save changes") :
                                   "Edit / toggle"},
        std::pair<std::string_view, std::string_view>{"B", "Cancel"}};
    DrawFooter(hints);
    DrawFadeIn();
    SDL_RenderPresent(m_renderer);
    WaitForNextFrame();
  }
  return saved;
}

void Launcher::NetworkSharesScreen()
{
  // Do not let an automatic mount worker replace a devoptab registration while this screen is
  // editing it. The worker is joined once here; individual connect operations below remain
  // asynchronous and keep the UI responsive.
  StopAutoMountShares();
  // Stopping the startup worker here used to permanently abandon every share it had not reached
  // yet.  Re-evaluate the saved auto-mount set on every exit path (including touch/back returns),
  // after any edits made on this screen have been committed.
  Common::ScopeGuard restart_auto_mounts([this] {
    if (!m_shutdown)
      StartAutoMountShares();
  });
  int selection = 0;
  int top = 0;
  const int list_y = SettingsListY();
  constexpr int row_height = 60;
  BeginScreenFx();
  while (BeginFrame())
  {
    const int count = 1 + static_cast<int>(m_shares.size());
    const int visible =
        std::max(1, (m_height - list_y - SettingsFooterReserve()) / row_height);
    selection = std::clamp(selection, 0, count - 1);
    if (selection < top)
      top = selection;
    if (selection >= top + visible)
      top = selection - visible + 1;
    bool rebuild = false;
    SDL_Event event{};
    while (PollEvent(&event))
    {
      int touch_x = 0;
      int touch_y = 0;
      const TouchKind touch = FeedTouch(event, &touch_x, &touch_y);
      if (TouchScrollList(touch, &selection, &top, count, visible))
        continue;
      if (touch == TouchKind::Tap)
      {
        if (touch_y >= m_height - 48)
          return;
        for (int row = 0; row < visible && top + row < count; ++row)
        {
          const int y = list_y + row * row_height;
          if (touch_y >= y && touch_y < y + row_height - 4)
          {
            selection = top + row;
            SDL_Event press{};
            press.type = SDL_CONTROLLERBUTTONDOWN;
            press.cbutton.button = BUTTON_CONFIRM;
            SDL_PushEvent(&press);
            break;
          }
        }
        continue;
      }
      const int direction = EventNavigation(event);
      if (direction)
        selection = (selection + direction + count) % count;
      const bool confirm =
          (event.type == SDL_CONTROLLERBUTTONDOWN && event.cbutton.button == BUTTON_CONFIRM) ||
          (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_RETURN);
      const bool cancel =
          (event.type == SDL_CONTROLLERBUTTONDOWN && event.cbutton.button == BUTTON_CANCEL) ||
          (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE);
      if (cancel)
        return;
      if (!confirm)
        continue;
      if (selection == 0)
      {
        if (m_shares.size() >= 8)
        {
          Toast("Maximum of 8 SMB shares", 1000);
          continue;
        }
        Storage::SmbShare new_share;
        if (EditSmbShare(&new_share, true))
        {
          m_shares.push_back(new_share);
          SaveShares();
          FlushPendingSaves();
          std::string error;
          std::atomic_bool cancel_mount{false};
          bool mounted = false;
          RunBusyTask(
              "Connecting SMB share", new_share.name,
              [&] { mounted = Storage::MountSmb(new_share, &error, &cancel_mount); },
              &cancel_mount);
          if (mounted)
          {
            const std::string root = Storage::SmbRootPath(new_share.id);
            for (const std::string& source : m_sources)
              if (PathAtOrBelow(source, root))
                m_pending_scan_sources.push_back(source);
          }
          if (!mounted && !cancel_mount.load(std::memory_order_acquire))
            RenderMessage("SMB connection failed", std::array<std::string, 1>{error});
          selection = static_cast<int>(m_shares.size());
          rebuild = true;
        }
      }
      else
      {
        const int share_index = selection - 1;
        Storage::SmbShare& selected_share = m_shares[share_index];
        const bool mounted = Storage::IsSmbMounted(selected_share.id);
        const int choice = Dropdown(
            selected_share.name.empty() ? selected_share.share : selected_share.name,
            {mounted ? "Disconnect" : "Connect", "Edit", "Toggle connect at startup", "Remove"}, -1,
            false, true);
        if (choice == 0)
        {
          if (mounted)
          {
            StopGameScan();
            Storage::UnmountSmb(selected_share.id);
            m_library_refresh_requested = true;
          }
          else
          {
            std::string error;
            std::atomic_bool cancel_mount{false};
            bool connected = false;
            RunBusyTask(
                "Connecting SMB share", selected_share.name,
                [&] { connected = Storage::MountSmb(selected_share, &error, &cancel_mount); },
                &cancel_mount);
            if (connected)
            {
              const std::string root = Storage::SmbRootPath(selected_share.id);
              for (const std::string& source : m_sources)
                if (PathAtOrBelow(source, root))
                  m_pending_scan_sources.push_back(source);
            }
            if (!connected && !cancel_mount.load(std::memory_order_acquire))
              RenderMessage("SMB connection failed", std::array<std::string, 1>{error});
          }
          rebuild = true;
        }
        else if (choice == 1)
        {
          Storage::SmbShare edited_share = selected_share;
          if (EditSmbShare(&edited_share, false))
          {
            const bool reconnect = mounted || edited_share.auto_mount;
            StopGameScan();
            Storage::UnmountSmb(selected_share.id);
            m_library_refresh_requested = true;
            selected_share = std::move(edited_share);
            SaveShares();
            FlushPendingSaves();
            if (reconnect)
            {
              std::string error;
              std::atomic_bool cancel_mount{false};
              bool connected = false;
              RunBusyTask(
                  "Reconnecting SMB share", selected_share.name,
                  [&] { connected = Storage::MountSmb(selected_share, &error, &cancel_mount); },
                  &cancel_mount);
              if (connected)
              {
                const std::string root = Storage::SmbRootPath(selected_share.id);
                for (const std::string& source : m_sources)
                  if (PathAtOrBelow(source, root))
                    m_pending_scan_sources.push_back(source);
              }
              if (!connected && !cancel_mount.load(std::memory_order_acquire))
                RenderMessage("SMB connection failed", std::array<std::string, 1>{error});
            }
            rebuild = true;
          }
        }
        else if (choice == 2)
        {
          selected_share.auto_mount = !selected_share.auto_mount;
          SaveShares();
          FlushPendingSaves();
          rebuild = true;
        }
        else if (choice == 3 &&
                 Confirm("Remove SMB share?",
                         std::array<std::string, 3>{
                             selected_share.name, "",
                             std::string(m_localization.Translate(
                                 "Saved folders on this share will also be removed."))}))
        {
          const std::string root = Storage::SmbRootPath(selected_share.id);
          StopGameScan();
          Storage::UnmountSmb(selected_share.id);
          m_library_refresh_requested = true;
          m_shares.erase(m_shares.begin() + share_index);
          SaveShares();
          FlushPendingSaves();
          RemoveSavedPathsBelow(root);
          selection = std::max(0, selection - 1);
          rebuild = true;
        }
      }
      if (rebuild)
        break;
    }
    if (rebuild)
    {
      BeginScreenFx();
      continue;
    }

    ClearBackground();
    const std::string summary = std::to_string(m_shares.size()) + " " +
                                std::string(m_localization.Translate(
                                    m_shares.size() == 1 ? "saved share" : "saved shares"));
    DrawHeader("SMB network shares", summary);
    GlassPanel(44, list_y - 13, m_width - 88,
               std::min(visible, count - top) * row_height + 18);
    for (int row = 0; row < visible && top + row < count; ++row)
    {
      const int index = top + row;
      const int y = list_y + row * row_height;
      const bool current = index == selection;
      if (current)
      {
        DrawRowHighlight(56, y - 3, m_width - 112, row_height - 4);
      }
      if (index == 0)
      {
        DrawText(m_font, 82, y + (row_height - FontHeight(m_font)) / 2 - 2,
                 m_localization.Translate("[ Add SMB share ]"), current ? m_value : m_highlight);
      }
      else
      {
        const Storage::SmbShare& item = m_shares[index - 1];
        const bool mounted = Storage::IsSmbMounted(item.id);
        DrawText(m_font, 82, y, item.name, current ? m_value : m_text);
        const Storage::SmbConnectionState connection_state =
            Storage::GetSmbConnectionState(item.id);
        const std::string status =
            connection_state == Storage::SmbConnectionState::Connecting   ? "Connecting..." :
            connection_state == Storage::SmbConnectionState::Reconnecting ? "Reconnecting..." :
            connection_state == Storage::SmbConnectionState::Failed       ? "Connection failed" :
            mounted                                                       ? "Connected" :
                      (item.auto_mount ? "Disconnected - auto" : "Disconnected");
        DrawTextRight(m_font_small, m_width - 82, y + 4, m_localization.Translate(status),
                      mounted ? SDL_Color{120, 220, 120, 255} : m_dim);
        const std::string address = "smb://" + item.server + "/" + item.share +
                                    (item.path.empty() ? std::string{} : "/" + item.path);
        DrawText(m_font_small, 82, y + 31, Ellipsize(m_font_small, address, m_width - 340), m_dim);
      }
    }
    DrawSettingsFooter("A  Select       B  Back");
    DrawFadeIn();
    SDL_RenderPresent(m_renderer);
    WaitForNextFrame();
  }
}

void Launcher::DownloadCovers()
{
  const std::string api_key = Trim(m_store.Get("Network/SteamGridDBKey"));
  if (!m_cover_download_ready || api_key.empty())
  {
    RenderMessage("Cover download unavailable",
                  std::array<std::string, 2>{"A SteamGridDB API key is required.",
                                             "Configure it in Settings > Launcher."},
                  true);
    return;
  }
  struct MissingCover
  {
    std::string key;
    std::string title;
    std::string path;
  };
  std::vector<MissingCover> missing;
  for (const Game& game : m_games)
  {
    const std::string path = CoverPath(game);
    if (!RegularFileExists(path))
      missing.push_back({game.key, game.title, path});
  }
  if (missing.empty())
  {
    Toast("Every game already has a cover", 1000);
    return;
  }
  if (!Confirm("Download covers?",
               std::array<std::string, 2>{std::to_string(missing.size()) + " " +
                                              std::string(m_localization.Translate("games")),
                                          std::string(m_localization.Translate(
                                              "Press B while downloading to cancel safely."))}))
    return;

  std::atomic_bool cancel{false};
  std::atomic<int> downloaded{0};
  std::atomic<int> failed{0};
  std::vector<std::string> downloaded_keys;
  downloaded_keys.reserve(missing.size());
  const auto task = [&] {
    CoverDownload::RequestOptions options;
    options.cancel = &cancel;
    for (const MissingCover& item : missing)
    {
      if (cancel.load(std::memory_order_acquire))
        break;
      const CoverDownload::Result result =
          CoverDownload::DownloadBestCover(api_key, item.title, item.path, nullptr, &options);
      if (result == CoverDownload::Result::Ok)
      {
        downloaded.fetch_add(1, std::memory_order_relaxed);
        downloaded_keys.emplace_back(item.key);
      }
      else if (result != CoverDownload::Result::Cancelled)
        failed.fetch_add(1, std::memory_order_relaxed);
    }
  };
  RunBusyTask("Downloading covers", std::to_string(missing.size()) + " games queued", task,
              &cancel);
  const std::unordered_set<std::string> downloaded_set(downloaded_keys.begin(),
                                                       downloaded_keys.end());
  for (Game& game : m_games)
  {
    if (!downloaded_set.contains(game.key))
      continue;
    if (game.cover)
      SDL_DestroyTexture(game.cover);
    game.cover = nullptr;
    game.cover_use = 0;
    game.cover_loaded_at = 0;
    game.cover_attempted = false;
  }
  if (cancel.load(std::memory_order_acquire))
    Toast("Cover download cancelled", 1000);
  else
    RenderMessage("Cover download complete",
                  std::array<std::string, 2>{
                      std::to_string(downloaded.load()) + " " +
                          std::string(m_localization.Translate("downloaded")),
                      std::to_string(failed.load()) + " " +
                          std::string(m_localization.Translate("not found or failed"))});
}

int Launcher::ChooseCoverArtwork(const std::vector<CoverDownload::Artwork>& artwork,
                                 std::string_view game_name)
{
  if (artwork.empty())
    return -1;
  const GameDetailLayout layout = ComputeGameDetailLayout();
  const int list_x = layout.content.x;
  const int list_width = layout.content.w;
  const int row_height = SettingsRowHeight() + 8;
  const int start_y = layout.content.y + 44;
  const int preview_width = layout.preview.w;
  const int preview_height = layout.preview.h;
  const int visible = std::max(1, (layout.content.h - 52) / row_height);
  const std::string temporary = std::string(COVER_DIRECTORY) + "/.sgdb-preview.img";
  int selection = 0;
  int top = 0;
  int loaded = -1;
  SDL_Texture* preview = nullptr;
  bool preview_failed = false;
  const auto release_preview = [&] {
    if (preview)
      SDL_DestroyTexture(preview);
    preview = nullptr;
    std::remove(temporary.c_str());
  };
  const auto load_preview = [&](int index) {
    release_preview();
    loaded = index;
    preview_failed = false;
    ClearBackground();
    DrawHeader("Choose cover artwork", game_name);
    DrawArtworkPreview(nullptr, layout.preview, false, std::string_view{});
    DrawTextCentered(m_font, layout.preview.x + layout.preview.w / 2, m_height / 2 - 18,
                     m_localization.Translate("Loading preview..."), m_dim);
    SDL_RenderPresent(m_renderer);
    const std::string& url =
        artwork[index].thumbnail_url.empty() ? artwork[index].url : artwork[index].thumbnail_url;
    std::atomic_bool cancel{false};
    CoverDownload::Result result = CoverDownload::Result::Error;
    CoverDownload::RequestOptions options{&cancel};
    RunBusyTask(
        "Loading cover preview", std::string(game_name),
        [&] { result = CoverDownload::DownloadImage(url, temporary, &options); }, &cancel);
    if (result == CoverDownload::Result::Ok)
      preview = LoadScaledTexture(temporary, preview_width, preview_height);
    preview_failed = preview == nullptr;
    std::remove(temporary.c_str());
    BeginScreenFx();
  };

  EnsureDirectory(COVER_DIRECTORY);
  load_preview(0);
  while (BeginFrame())
  {
    SDL_Event event{};
    while (PollEvent(&event))
    {
      int touch_x = 0;
      int touch_y = 0;
      const TouchKind touch = FeedTouch(event, &touch_x, &touch_y);
      const int previous_touch_selection = selection;
      if (TouchScrollList(touch, &selection, &top, static_cast<int>(artwork.size()), visible))
      {
        if (selection != previous_touch_selection)
          load_preview(selection);
        continue;
      }
      if (touch == TouchKind::Tap)
      {
        if (touch_y >= m_height - 48)
        {
          release_preview();
          return -1;
        }
        if (touch_x >= list_x && touch_x < list_x + list_width)
        {
          for (int row = 0; row < visible && top + row < static_cast<int>(artwork.size()); ++row)
          {
            const int y = start_y + row * row_height;
            if (touch_y >= y && touch_y < y + row_height)
            {
              selection = top + row;
              if (loaded != selection)
                load_preview(selection);
              break;
            }
          }
        }
        continue;
      }
      const int previous = selection;
      const int direction = EventNavigation(event);
      if (direction)
        selection = (selection + direction + static_cast<int>(artwork.size())) % artwork.size();
      if ((event.type == SDL_CONTROLLERBUTTONDOWN && event.cbutton.button == BUTTON_CONFIRM) ||
          (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_RETURN))
      {
        release_preview();
        return selection;
      }
      if ((event.type == SDL_CONTROLLERBUTTONDOWN && event.cbutton.button == BUTTON_CANCEL) ||
          (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE))
      {
        release_preview();
        return -1;
      }
      if (selection < top)
        top = selection;
      if (selection >= top + visible)
        top = selection - visible + 1;
      if (selection != previous)
        load_preview(selection);
    }

    ClearBackground();
    DrawHeader("Choose cover artwork", game_name);
    DrawSectionHeading("Online artwork", list_x, start_y - 44, list_width);
    GlassPanel(list_x - 8, start_y - 8, list_width + 16,
               std::min(visible, static_cast<int>(artwork.size())) * row_height + 16);
    for (int row = 0; row < visible && top + row < static_cast<int>(artwork.size()); ++row)
    {
      const int index = top + row;
      const int y = start_y + row * row_height;
      const int text_y = y + (row_height - FontHeight(m_font)) / 2;
      const bool current = index == selection;
      if (current)
      {
        DrawRowHighlight(list_x, y, list_width, row_height - 3);
      }
      DrawText(m_font, list_x + 26, text_y,
               std::string(m_localization.Translate("Artwork")) + " " + std::to_string(index + 1),
               current ? m_value : m_text);
      if (artwork[index].width > 0 && artwork[index].height > 0)
      {
        const std::string dimensions =
            std::to_string(artwork[index].width) + "x" + std::to_string(artwork[index].height);
        DrawTextRight(m_font_small, list_x + list_width - 20,
                      text_y + (FontHeight(m_font) - FontHeight(m_font_small)) / 2,
                      dimensions, current ? m_value : m_dim);
      }
    }
    const bool show_failure = loaded == selection && preview_failed;
    DrawArtworkPreview(loaded == selection ? preview : nullptr, layout.preview, false,
                       show_failure ? std::string_view{} : std::string_view{"NO COVER"});
    if (show_failure)
    {
      const SDL_Rect& rectangle = layout.preview;
      DrawWrapped(m_font_small, rectangle.x + 16, rectangle.y + rectangle.h / 2 - 30,
                  rectangle.w - 32, FontHeight(m_font_small) + 6, 3,
                  m_localization.Translate("Preview unavailable"), m_dim);
    }
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 2> footer = {
        std::pair{"A", "Use artwork"}, std::pair{"B", "Back"}};
    DrawFooter(footer);
    DrawFadeIn();
    SDL_RenderPresent(m_renderer);
    WaitForNextFrame();
  }
  release_preview();
  return -1;
}

void Launcher::DownloadCover(Game* game)
{
  if (!game)
    return;
  if (!m_cover_download_ready)
  {
    RenderMessage("Cover downloads unavailable",
                  std::array<std::string, 2>{"The network or HTTP client could not be initialized.",
                                             "Check the Switch network connection and try again."},
                  true);
    return;
  }
  const auto save_api_key = [&](std::string api_key) {
    api_key = Trim(std::move(api_key));
    m_store.Set("Network/SteamGridDBKey", api_key);
    MarkStoreDirty();
    FlushPendingSaves();
    return api_key;
  };
  std::string api_key = Trim(m_store.Get("Network/SteamGridDBKey"));
  if (api_key.empty())
  {
    if (!PromptText("Enter your free SteamGridDB API key", {}, &api_key, true, false,
                    "Create a free API key at steamgriddb.com/profile/preferences/api"))
    {
      Toast("A SteamGridDB API key is required", 1200);
      return;
    }
    api_key = save_api_key(std::move(api_key));
  }
  std::string query = game->title;
  CoverDownload::GameResult selected_game;
  while (true)
  {
    std::vector<CoverDownload::GameResult> matches;
    std::atomic_bool cancel{false};
    CoverDownload::Result result = CoverDownload::Result::Error;
    CoverDownload::RequestOptions options{&cancel};
    RunBusyTask(
        "Searching SteamGridDB", query,
        [&] { result = CoverDownload::SearchGames(api_key, query, &matches, &options); }, &cancel);
    if (result == CoverDownload::Result::Cancelled)
      return;
    if (result == CoverDownload::Result::NoKey)
    {
      std::string replacement = api_key;
      if (!PromptText("SteamGridDB API key rejected", replacement, &replacement, true, false,
                      "Enter a valid key to retry the cover search.",
                      "steamgriddb.com/profile/preferences/api"))
        return;
      api_key = save_api_key(std::move(replacement));
      continue;
    }
    if (result != CoverDownload::Result::Ok && result != CoverDownload::Result::NotFound)
    {
      RenderMessage("Cover search failed",
                    std::array<std::string, 1>{CoverDownload::ResultMessage(result)});
      return;
    }
    std::vector<std::string> names{std::string(m_localization.Translate("Custom search..."))};
    names.reserve(matches.size() + 1);
    for (const auto& match : matches)
      names.push_back(match.name);
    const int match_index = Dropdown("Choose matching title", names, -1, true, false);
    if (match_index < 0)
      return;
    if (match_index == 0)
    {
      std::string custom;
      if (!PromptText("Custom SteamGridDB search", query, &custom, false, false))
        continue;
      custom = Trim(std::move(custom));
      if (!custom.empty())
        query = std::move(custom);
      continue;
    }
    if (match_index > static_cast<int>(matches.size()))
      return;
    selected_game = matches[match_index - 1];
    break;
  }

  std::vector<CoverDownload::Artwork> artwork;
  std::atomic_bool artwork_cancel{false};
  CoverDownload::Result artwork_result = CoverDownload::Result::Error;
  CoverDownload::RequestOptions artwork_options{&artwork_cancel};
  RunBusyTask(
      "Loading available artwork", selected_game.name,
      [&] {
        artwork_result =
            CoverDownload::FetchArtwork(api_key, selected_game.id, &artwork, &artwork_options);
      },
      &artwork_cancel);
  if (artwork_result == CoverDownload::Result::Cancelled)
    return;
  if (artwork_result != CoverDownload::Result::Ok)
  {
    RenderMessage("Artwork search failed",
                  std::array<std::string, 1>{CoverDownload::ResultMessage(artwork_result)});
    return;
  }
  const int artwork_index = ChooseCoverArtwork(artwork, selected_game.name);
  if (artwork_index < 0 || artwork_index >= static_cast<int>(artwork.size()))
    return;
  std::atomic_bool download_cancel{false};
  CoverDownload::Result download = CoverDownload::Result::Error;
  CoverDownload::RequestOptions download_options{&download_cancel};
  RunBusyTask(
      "Downloading selected cover", selected_game.name,
      [&] {
        download = CoverDownload::DownloadImage(artwork[artwork_index].url, CoverPath(*game),
                                                &download_options);
      },
      &download_cancel);
  if (download == CoverDownload::Result::Cancelled)
    return;
  if (download == CoverDownload::Result::Ok)
  {
    ReloadCover(game);
    Toast("Cover downloaded", 1200);
  }
  else
  {
    RenderMessage("Cover download failed",
                  std::array<std::string, 1>{CoverDownload::ResultMessage(download)});
  }
}

void Launcher::ImportCoverFromFile(Game* game)
{
  if (!game)
    return;

  static constexpr std::array<std::string_view, 5> IMAGE_EXTENSIONS = {".png", ".jpg", ".jpeg",
                                                                       ".webp", ".bmp"};
  const std::string selected = FileBrowser(ParentPath(game->path), false, true, false,
                                           IMAGE_EXTENSIONS, "Select local cover");
  if (selected.empty())
    return;

  const std::string destination = CoverPath(*game);
  const std::string temporary = destination + ".tmp";
  const std::string backup = destination + ".old";
  std::atomic_bool cancel{false};
  bool imported = false;
  std::string reason;
  std::string detail;
  RunBusyTask(
      "Importing local cover", FileName(selected),
      [&] {
        const auto fail = [&](std::string_view message, std::string_view technical = {}) {
          reason = std::string(message);
          detail = std::string(technical);
          std::remove(temporary.c_str());
        };
        const auto was_cancelled = [&] {
          if (!cancel.load(std::memory_order_acquire))
            return false;
          std::remove(temporary.c_str());
          return true;
        };

        if (was_cancelled())
          return;
        struct stat source_info{};
        constexpr std::uint64_t MAXIMUM_FILE_SIZE = 32ULL * 1024 * 1024;
        if (::stat(selected.c_str(), &source_info) != 0)
        {
          fail("The selected cover file is unavailable.", std::strerror(errno));
          return;
        }
        if (!S_ISREG(source_info.st_mode))
        {
          fail("The selected cover file is unavailable.");
          return;
        }
        if (source_info.st_size < 1 ||
            static_cast<std::uint64_t>(source_info.st_size) > MAXIMUM_FILE_SIZE)
        {
          fail("The selected cover file is too large.");
          return;
        }
        if (!RecoverAtomicFile(destination))
        {
          fail("The launcher could not prepare the cover file safely.", std::strerror(errno));
          return;
        }

        using Surface = std::unique_ptr<SDL_Surface, decltype(&SDL_FreeSurface)>;
        Surface source{IMG_Load(selected.c_str()), SDL_FreeSurface};
        if (!source)
        {
          fail("The selected file is not a supported image.", IMG_GetError());
          return;
        }
        constexpr std::uint64_t MAXIMUM_PIXELS = 16ULL * 1024 * 1024;
        if (source->w <= 0 || source->h <= 0 || source->w > 8192 || source->h > 8192 ||
            static_cast<std::uint64_t>(source->w) * static_cast<std::uint64_t>(source->h) >
                MAXIMUM_PIXELS)
        {
          fail("The selected image dimensions are too large.");
          return;
        }
        if (was_cancelled())
          return;

        Surface converted{SDL_ConvertSurfaceFormat(source.get(), SDL_PIXELFORMAT_RGBA32, 0),
                          SDL_FreeSurface};
        source.reset();
        if (!converted)
        {
          fail("The launcher could not convert the selected image to PNG.", SDL_GetError());
          return;
        }
        if (was_cancelled())
          return;
        if (IMG_SavePNG(converted.get(), temporary.c_str()) != 0)
        {
          fail("The launcher could not convert the selected image to PNG.", IMG_GetError());
          return;
        }
        converted.reset();
        if (was_cancelled())
          return;

        // Re-open the generated PNG before replacing the active cover. This catches truncated or
        // unsupported output while the previous cover is still untouched.
        Surface verification{IMG_Load(temporary.c_str()), SDL_FreeSurface};
        if (!verification || verification->w <= 0 || verification->h <= 0)
        {
          fail("The launcher could not verify the converted cover.", IMG_GetError());
          return;
        }
        verification.reset();
        FILE* saved_file = std::fopen(temporary.c_str(), "rb+");
        if (!saved_file)
        {
          fail("The launcher could not save the converted cover.", std::strerror(errno));
          return;
        }
        const bool synced = ::fsync(::fileno(saved_file)) == 0;
        const bool closed = std::fclose(saved_file) == 0;
        if (!synced || !closed)
        {
          fail("The launcher could not save the converted cover.", std::strerror(errno));
          return;
        }
        if (was_cancelled())
          return;

        const bool had_current = RegularFileExists(destination);
        if (had_current && std::rename(destination.c_str(), backup.c_str()) != 0)
        {
          fail("The launcher could not replace the current cover safely.", std::strerror(errno));
          return;
        }
        if (std::rename(temporary.c_str(), destination.c_str()) != 0)
        {
          const int saved_errno = errno;
          if (had_current)
            std::rename(backup.c_str(), destination.c_str());
          fsdevCommitDevice("sdmc");
          fail("The launcher could not replace the current cover safely.", std::strerror(saved_errno));
          return;
        }
        fsdevCommitDevice("sdmc");
        if (had_current && std::remove(backup.c_str()) == 0)
          fsdevCommitDevice("sdmc");
        imported = true;
      },
      &cancel);

  if (imported)
  {
    ReloadCover(game);
    Toast("Cover imported", 1200);
    return;
  }
  if (cancel.load(std::memory_order_acquire))
    return;
  std::vector<std::string> lines;
  lines.emplace_back(m_localization.Translate(
      reason.empty() ? "The selected cover could not be imported safely." : reason));
  if (!detail.empty())
    lines.emplace_back(std::move(detail));
  RenderMessage("Cover import failed", lines);
}

void Launcher::CoverSettings(Game* game)
{
  if (!game)
    return;

  const std::string cover_path = CoverPath(*game);
  (void)RecoverAtomicFile(cover_path);
  const std::array<Row, 2> actions = {
      Row{"Download from SteamGridDB", "Online artwork", true, false, false},
      Row{"Import cover from file", "Local image", true, false, false},
  };
  int selection = 0;

  const GameDetailLayout layout = ComputeGameDetailLayout();
  constexpr int card_gap = 20;
  const int card_height = (layout.content.h - card_gap) / 2;
  const std::array<SDL_Rect, 2> cards = {
      SDL_Rect{layout.content.x, layout.content.y, layout.content.w, card_height},
      SDL_Rect{layout.content.x, layout.content.y + card_height + card_gap, layout.content.w,
               card_height},
  };
  const auto contains = [](const SDL_Rect& rectangle, int x, int y) {
    return x >= rectangle.x && x < rectangle.x + rectangle.w && y >= rectangle.y &&
           y < rectangle.y + rectangle.h;
  };
  const auto show_info = [&] {
    const Row& action = actions[selection];
    const SettingHelpInfo info = SettingHelpFor("Cover settings", action);
    ShowInfoCard("Cover settings", action.label, info.kind, info.description, action.value,
                 "Per-game setting", true, true);
    BeginScreenFx();
  };
  const auto activate = [&] {
    if (selection == 0)
      DownloadCover(game);
    else
      ImportCoverFromFile(game);
    BeginScreenFx();
  };
  const auto remove_custom_cover = [&] {
    if (!RegularFileExists(cover_path) ||
        !Confirm("Remove custom cover?",
                 std::array<std::string, 2>{
                     "The downloaded or imported cover will be deleted.",
                     "The launcher will use the game's embedded artwork when available."},
                 true))
    {
      BeginScreenFx();
      return;
    }

    int removal_error = 0;
    if (!RecoverAtomicFile(cover_path))
      removal_error = errno != 0 ? errno : EIO;
    else if (std::remove(cover_path.c_str()) != 0 && errno != ENOENT)
      removal_error = errno != 0 ? errno : EIO;
    if (removal_error != 0)
    {
      RenderMessage("Cover removal failed",
                    std::array<std::string, 1>{std::strerror(removal_error)});
    }
    else
    {
      fsdevCommitDevice("sdmc");
      ReloadCover(game);
      Toast("Custom cover removed", 1200);
    }
    BeginScreenFx();
  };

  bool has_custom_cover = RegularFileExists(cover_path);
  BeginScreenFx();
  while (BeginFrame())
  {
    SDL_Event event{};
    while (PollEvent(&event))
    {
      int touch_x = 0;
      int touch_y = 0;
      const TouchKind touch = FeedTouch(event, &touch_x, &touch_y);
      bool choose = false;
      bool info = false;
      bool remove = false;
      bool back = false;

      if (touch == TouchKind::Tap)
      {
        if (contains(cards[0], touch_x, touch_y))
        {
          selection = 0;
          choose = true;
        }
        else if (contains(cards[1], touch_x, touch_y))
        {
          selection = 1;
          choose = true;
        }
        // Footer taps are dispatched by FeedTouch as synthetic controller presses.
      }

      if (event.type == SDL_CONTROLLERBUTTONDOWN)
      {
        if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT)
          selection = 0;
        else if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT)
          selection = 1;
        else if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_UP ||
                 event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)
          selection = 1 - selection;
        else if (event.cbutton.button == BUTTON_CONFIRM)
          choose = true;
        else if (event.cbutton.button == BUTTON_SETTINGS)
          info = true;
        else if (event.cbutton.button == SDL_CONTROLLER_BUTTON_X && has_custom_cover)
          remove = true;
        else if (event.cbutton.button == BUTTON_CANCEL)
          back = true;
      }
      else if (event.type == SDL_KEYDOWN)
      {
        if (event.key.keysym.sym == SDLK_LEFT)
          selection = 0;
        else if (event.key.keysym.sym == SDLK_RIGHT)
          selection = 1;
        else if (event.key.keysym.sym == SDLK_UP || event.key.keysym.sym == SDLK_DOWN)
          selection = 1 - selection;
        else if (event.key.keysym.sym == SDLK_RETURN)
          choose = true;
        else if (event.key.keysym.sym == SDLK_x)
          info = true;
        else if ((event.key.keysym.sym == SDLK_y || event.key.keysym.sym == SDLK_DELETE) &&
                 has_custom_cover)
          remove = true;
        else if (event.key.keysym.sym == SDLK_ESCAPE)
          back = true;
      }

      if (back)
        return;
      if (info)
      {
        show_info();
      }
      else if (remove)
      {
        remove_custom_cover();
        has_custom_cover = RegularFileExists(cover_path);
      }
      else if (choose)
      {
        activate();
        has_custom_cover = RegularFileExists(cover_path);
      }
    }

    ClearBackground();
    DrawHeader("Cover settings", game->title);
    DrawGamePreview(game, layout.preview);
    for (int index = 0; index < static_cast<int>(cards.size()); ++index)
    {
      const SDL_Rect& card = cards[index];
      const bool selected = index == selection;
      GlassPanel(card.x, card.y, card.w, card.h);
      if (selected)
        DrawRowHighlight(card.x + 8, card.y + 8, card.w - 16, card.h - 16);
      const int x = card.x + 28;
      const int width = card.w - 56;
      const SettingHelpInfo info = SettingHelpFor("Cover settings", actions[index]);
      DrawText(m_font_small, x, card.y + 24, m_localization.Translate(info.kind), m_highlight);
      DrawScrollingTextLeft(m_font, x, card.y + 62, width,
                            m_localization.Translate(actions[index].label),
                            selected ? m_value : m_text);
      const int description_line_height = FontHeight(m_font_small) + 7;
      DrawWrapped(m_font_small, x, card.y + 112, width, description_line_height,
                  std::max(1, (card.h - 132) / description_line_height),
                  m_localization.Translate(info.description), m_dim);
    }

    if (has_custom_cover)
    {
      static constexpr std::array<std::pair<std::string_view, std::string_view>, 4> hints = {
          std::pair{"A", "Choose"}, std::pair{"X", "Info"}, std::pair{"Y", "Remove custom cover"},
          std::pair{"B", "Back"}};
      DrawFooter(hints);
    }
    else
    {
      static constexpr std::array<std::pair<std::string_view, std::string_view>, 3> hints = {
          std::pair{"A", "Choose"}, std::pair{"X", "Info"}, std::pair{"B", "Back"}};
      DrawFooter(hints);
    }
    DrawFadeIn();
    SDL_RenderPresent(m_renderer);
    WaitForNextFrame();
  }
}

void Launcher::SettingsRoot()
{
  constexpr int count = 5;
  constexpr int launcher_row = 0;
  constexpr int library_row = 1;
  constexpr int section_start = 2;
  int selection = 0;
  int top = 0;
  const int row_height = SettingsRowHeight();
  const int y0 = SettingsListY() + 40;
  constexpr int section_gap = 56;
  static constexpr std::array<std::string_view, count> labels = {
      "Launcher", "Library & storage", "Emulation", "Video", "Controls"};
  const int visible =
      std::max(1, (m_height - y0 - SettingsFooterReserve() - section_gap) / row_height);
  const auto row_y = [&](int index) {
    return y0 + (index - top) * row_height + (index >= section_start ? section_gap : 0);
  };
  BeginScreenFx();
  while (BeginFrame())
  {
    SDL_Event event{};
    while (PollEvent(&event))
    {
      int touch_x = 0;
      int touch_y = 0;
      const TouchKind touch = FeedTouch(event, &touch_x, &touch_y);
      if (TouchScrollList(touch, &selection, &top, count, visible))
        continue;
      const int direction = EventNavigation(event);
      if (direction)
        selection = (selection + direction + count) % count;
      bool activate = false;
      if (event.type == SDL_CONTROLLERBUTTONDOWN)
      {
        if (event.cbutton.button == BUTTON_SETTINGS)
        {
          const Row row{std::string(labels[selection]), ">", true, false, false};
          const SettingHelpInfo info = SettingHelpFor("Settings", row);
          ShowInfoCard("Settings", row.label, info.kind, info.description, {}, "Global settings");
          BeginScreenFx();
          continue;
        }
        activate = event.cbutton.button == BUTTON_CONFIRM;
        if (event.cbutton.button == BUTTON_CANCEL)
          return;
      }
      else if (event.type == SDL_KEYDOWN)
      {
        if (event.key.keysym.sym == SDLK_x)
        {
          const Row row{std::string(labels[selection]), ">", true, false, false};
          const SettingHelpInfo info = SettingHelpFor("Settings", row);
          ShowInfoCard("Settings", row.label, info.kind, info.description, {}, "Global settings");
          BeginScreenFx();
          continue;
        }
        activate = event.key.keysym.sym == SDLK_RETURN;
        if (event.key.keysym.sym == SDLK_ESCAPE)
          return;
      }
      else if (touch == TouchKind::Tap)
      {
        if (touch_y < TopBarHeight() || touch_y >= m_height - 40)
          return;
        for (int row = 0; row < visible && top + row < count; ++row)
        {
          const int index = top + row;
          if (touch_y >= row_y(index) && touch_y < row_y(index) + row_height)
          {
            selection = index;
            activate = true;
            break;
          }
        }
      }
      if (activate)
      {
        if (selection == launcher_row)
          AppearanceSettings();
        else if (selection == library_row)
          LibrarySettings();
        else if (selection == 2)
          SaturnOptionsPage("Emulation", SATURN_EMULATION_OPTIONS, nullptr);
        else if (selection == 3)
          SaturnOptionsPage("Video", SATURN_VIDEO_OPTIONS, nullptr);
        else if (selection == 4)
          SaturnControlsRoot(nullptr);
        if (m_pending_launch)
          return;
        BeginScreenFx();
      }
      if (selection < top)
        top = selection;
      if (selection >= top + visible)
        top = selection - visible + 1;
    }

    ClearBackground();
    DrawPageHeader(m_localization.Translate("Settings"), "YabaSanshiro",
                   m_localization.Translate("Global settings"));
    const int column_width = std::min(980, m_width - 180);
    const int column_x = (m_width - column_width) / 2;
    const int label_x = column_x + 40;
    const int value_x = column_x + column_width - 40;
    const auto draw_visible_section = [&](std::string_view heading, int begin, int end) {
      const int first = std::max(begin, top);
      const int last = std::min(end, top + visible);
      if (first >= last)
        return;

      const int panel_y = row_y(first) - 10;
      const int panel_bottom = row_y(last - 1) + row_height + 8;
      // The heading only belongs to a section whose real first row is on screen.
      if (first == begin)
        DrawSectionHeading(heading, column_x, panel_y - 34, column_width);
      GlassPanel(column_x - 12, panel_y, column_width + 24, panel_bottom - panel_y);
    };
    draw_visible_section("General", 0, section_start);
    draw_visible_section("Emulator", section_start, count);
    const float target = static_cast<float>(row_y(selection) + 2);
    m_highlight_y = (!m_animations || m_highlight_y < 0.0f) ?
                        target :
                        m_highlight_y + (target - m_highlight_y) * 0.30f;
    DrawRowHighlight(column_x, static_cast<int>(m_highlight_y), column_width, row_height - 4);
    for (int row = 0; row < visible && top + row < count; ++row)
    {
      const int index = top + row;
      const int slot = row_y(index);
      const bool current = index == selection;
      std::string value;
      if (index == launcher_row)
      {
        const std::string theme = Lower(m_store.Get("Launcher/Theme", "bubbles"));
        value = theme == "xmb"     ? "XMB (PS3)" :
                theme == "glow"    ? "Glow" :
                theme == "classic" ? "Classic" :
                theme == "oled"    ? "OLED black" :
                                     "Bubbles";
      }
      else if (index == library_row)
        value = "game folders / files";
      else
        value = ">";
      const std::string_view displayed_value = m_localization.Translate(value);
      DrawSettingsRowText(m_localization.Translate(labels[index]), displayed_value, slot,
                          column_width, label_x, value_x, current, current ? m_value : m_text,
                          current ? m_value : m_dim, false, row_height);
    }
    DrawSettingsFooter("A  Choose       X  Info       B  Back");
    DrawFadeIn();
    SDL_RenderPresent(m_renderer);
    WaitForNextFrame();
  }
}

GameMenuLayout Launcher::ComputeGameMenuLayout() const
{
  const GameDetailLayout detail = ComputeGameDetailLayout();
  const int row_height = SettingsRowHeight();
  const int height = GAME_MENU_COUNT * row_height + 56 + 44 + 8;
  return {detail, detail.content.y + 44 + std::max(0, (detail.content.h - height) / 2), row_height,
          GAME_MENU_MANAGE_START};
}

void Launcher::DrawGameMenu(Game* game, int selection)
{
  if (!game)
    return;
  const GameMenuLayout layout = ComputeGameMenuLayout();
  const SDL_Rect& menu = layout.detail.content;
  ClearBackground();
  const std::string summary =
      game->game_id.empty() ?
          std::string(m_localization.Translate("Game ID unavailable")) :
          std::string(m_localization.Translate("Game ID")) + "  " + game->game_id;
  DrawPageHeader(game->title, m_localization.Translate("Game menu"), summary, game->platform);
  DrawGamePreview(game, layout.detail.preview);
  DrawSectionHeading("General", menu.x, layout.start - 44, menu.w);
  GlassPanel(menu.x - 8, layout.start - 8, menu.w + 16,
             GAME_MENU_MANAGE_START * layout.row_height + 16);
  DrawSectionHeading("Manage game", menu.x, layout.RowY(GAME_MENU_MANAGE_START) - 44, menu.w);
  GlassPanel(menu.x - 8, layout.RowY(GAME_MENU_MANAGE_START) - 8, menu.w + 16,
             (GAME_MENU_COUNT - GAME_MENU_MANAGE_START) * layout.row_height + 16);
  const float target = static_cast<float>(layout.RowY(selection) + 2);
  m_highlight_y = (!m_animations || m_highlight_y < 0.0f) ?
                      target :
                      m_highlight_y + (target - m_highlight_y) * 0.30f;
  DrawRowHighlight(menu.x, static_cast<int>(m_highlight_y), menu.w, layout.row_height - 4);
  for (int index = 0; index < GAME_MENU_COUNT; ++index)
  {
    const bool current = index == selection;
    const bool submenu = index == 1 || index == 3 || index == 4 || index == 5;
    std::string_view label = GAME_MENU_ITEMS[index];
    if (index == 3 && m_favorites.contains(game->key))
      label = "Favorite / collections  ★";
    else if (index == GAME_MENU_COUNT - 1 && game->installed_nand)
      label = "Uninstall WAD (keep save)";
    const SDL_Color color = index == GAME_MENU_COUNT - 1 ? SDL_Color{238, 135, 135, 255} :
                            current                      ? m_value :
                                                           m_text;
    DrawSettingsRowText(m_localization.Translate(label), submenu ? ">" : "", layout.RowY(index),
                        menu.w, menu.x + 24, menu.x + menu.w - 24, current, color,
                        current ? m_value : m_dim, false, layout.row_height);
  }
  const std::array<std::pair<std::string_view, std::string_view>, 2> footer = {
      std::pair<std::string_view, std::string_view>{"A",
                                                    selection == 0 ? "Launch" : "Select"},
      std::pair<std::string_view, std::string_view>{"B", "Back"}};
  DrawFooter(footer);
}

void Launcher::PerGameMenu(Game* game, bool* launch, bool* rescan)
{
  if (!game || !launch || !rescan)
    return;
  constexpr int count = GAME_MENU_COUNT;
  int selection = 0;
  int touch_top = 0;
  BeginScreenFx();
  while (BeginFrame())
  {
    const GameMenuLayout layout = ComputeGameMenuLayout();
    SDL_Event event{};
    while (PollEvent(&event))
    {
      int touch_x = 0;
      int touch_y = 0;
      const TouchKind touch = FeedTouch(event, &touch_x, &touch_y);
      if (TouchScrollList(touch, &selection, &touch_top, count, count))
        continue;
      const int direction = EventNavigation(event);
      if (direction)
        selection = (selection + direction + count) % count;
      bool activate = false;
      if (event.type == SDL_CONTROLLERBUTTONDOWN)
      {
        activate = event.cbutton.button == BUTTON_CONFIRM;
        if (event.cbutton.button == BUTTON_CANCEL)
          return;
      }
      else if (event.type == SDL_KEYDOWN)
      {
        activate = event.key.keysym.sym == SDLK_RETURN;
        if (event.key.keysym.sym == SDLK_ESCAPE)
          return;
      }
      else if (touch == TouchKind::Tap)
      {
        if (touch_y < TopBarHeight() || touch_y >= m_height - 40)
          return;
        const SDL_Rect& menu = layout.detail.content;
        if (touch_x < menu.x || touch_x >= menu.x + menu.w)
          continue;
        for (int index = 0; index < count; ++index)
        {
          const int row_top = layout.RowY(index);
          if (touch_y >= row_top && touch_y < row_top + layout.row_height)
          {
            selection = index;
            activate = true;
            break;
          }
        }
      }
      if (!activate)
        continue;

      if (selection == 0)
      {
        *launch = true;
        return;
      }
      if (selection == 1)
      {
        SaturnGameSettingsRoot(game);
      }
      else if (selection == 2)
      {
        std::string title;
        if (PromptText("Rename game", game->title, &title, false, false))
        {
          game->title = title;
          game->has_custom_title = true;
          if (!game->installed_nand)
            game->config_override_path = EntryGameIniPath(*game);
          m_store.Set("Alias/" + game->key, title);
          MarkStoreDirty();
          game->has_game_config = RegularFileExists(GameIniPath(*game));
        }
      }
      else if (selection == 3)
      {
        EditGameOrganization(game);
      }
      else if (selection == 4)
      {
        CoverSettings(game);
      }
      else if (selection == 5)
      {
        CreateHomeShortcut(game);
      }
      else if (selection == 6)
      {
        if (game->has_game_config)
        {
          std::remove(GameIniPath(*game).c_str());
          InvalidateGameSettingCache(*game);
          game->has_game_config = false;
          Toast("Game settings cleared", 700);
        }
        else
        {
          Toast("No game settings found", 700);
        }
      }
      else
      {
        {
          struct stat info{};
          if (::stat(game->path.c_str(), &info) != 0)
          {
            RenderMessage("Delete failed", std::array<std::string, 1>{"The game no longer exists."},
                          true);
          }
          else if (S_ISDIR(info.st_mode))
          {
            RenderMessage(
                "Folder deletion disabled",
                std::array<std::string, 3>{
                    std::string(m_localization.Translate(
                        "Extracted game folders are not deleted automatically.")),
                    std::string(m_localization.Translate(
                        "Remove this folder manually to avoid deleting unrelated files:")),
                    game->path});
          }
          else
          {
            // A cue sheet's tracks (or a CloneCD/Alcohol image's data files) go with it
            const std::vector<std::string> companions =
                UICommon::DiscImageCompanionFiles(game->path);
            const std::string what =
                companions.empty() ?
                    std::string(m_localization.Translate("This permanently deletes the game file.")) :
                    std::string(m_localization.Translate("This permanently deletes the game file")) +
                        " " + std::string(m_localization.Translate("and its")) + " " +
                        std::to_string(companions.size()) + " " +
                        std::string(m_localization.Translate(
                            companions.size() == 1 ? "track file." : "track files."));
            if (Confirm("Delete game?",
                        std::array<std::string, 4>{
                            game->title, "", what,
                            std::string(m_localization.Translate("This cannot be undone."))}))
            {
              if (std::remove(game->path.c_str()) == 0)
              {
                std::size_t failed = 0;
                for (const std::string& companion : companions)
                {
                  if (std::remove(companion.c_str()) != 0 && errno != ENOENT)
                    ++failed;
                }
                std::remove(CoverPath(*game).c_str());
                std::remove(GameIniPath(*game).c_str());
                *rescan = true;
                if (failed)
                {
                  RenderMessage("Delete incomplete",
                                std::array<std::string, 1>{
                                    std::to_string(failed) + " " +
                                    std::string(m_localization.Translate(
                                        failed == 1 ? "track file could not be removed." :
                                                      "track files could not be removed."))},
                                true);
                  return;
                }
                Toast("Game deleted", 800);
                return;
              }
              RenderMessage("Delete failed",
                            std::array<std::string, 1>{"The game file could not be removed."}, true);
            }
          }
        }
      }
      BeginScreenFx();
    }

    DrawGameMenu(game, selection);
    DrawFadeIn();
    SDL_RenderPresent(m_renderer);
    WaitForNextFrame();
  }
}

std::optional<LaunchRequest> Launcher::Run()
{
  if (!Initialize(false))
  {
    if (m_sdl_ready)
      SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "YabaSanshiro NX",
                               "The full SDL launcher could not be initialized.", m_window);
    return std::nullopt;
  }
  ScanGames();
  BeginScreenFx();
  RenderGrid(0);
  StartUsbInitialization();
  if (!m_startup_message.empty())
  {
    RenderMessage("YabaSanshiro NX", std::array<std::string, 1>{m_startup_message});
    m_startup_message.clear();
    BeginScreenFx();
  }
  int selection = 0;
  bool launch = false;
  // A fresh launcher must stay on page 1 while progressive scan batches are sorted in.  Without
  // this guard, preserving the initially selected game's key can move the selection (and visible
  // page) as games which sort before it arrive.  Stop pinning only after deliberate library
  // navigation; normal in-session selection preservation then resumes.
  bool pin_first_library_page = true;
  std::string desired_selection_key;
  const auto select_key = [&](std::string_view key) {
    if (key.empty())
      return false;
    for (std::size_t index = 0; index < m_visible_games.size(); ++index)
    {
      if (m_games[m_visible_games[index]].key == key)
      {
        selection = static_cast<int>(index);
        return true;
      }
    }
    return false;
  };
  const auto rescan_library = [&] {
    const Game* selected = VisibleGame(selection);
    desired_selection_key = selected ? selected->key : std::string{};
    ScanGames();
    selection = 0;
  };

  while (BeginFrame() && !launch && !m_pending_launch)
  {
    if (m_library_scan)
    {
      const Game* before_pump = pin_first_library_page ? nullptr : VisibleGame(selection);
      const std::string selected_key =
          pin_first_library_page ? std::string{} :
                                   (before_pump ? before_pump->key : desired_selection_key);
      PumpGameScan();
      if (pin_first_library_page)
      {
        selection = 0;
        desired_selection_key.clear();
      }
      else if (!selected_key.empty() && select_key(selected_key))
      {
        desired_selection_key.clear();
      }
    }
    PumpUsbInitialization();
    PumpAutoMountShares();
    selection = m_visible_games.empty() ?
                    0 :
                    std::clamp(selection, 0, static_cast<int>(m_visible_games.size()) - 1);

    if (!m_library_scan && (!m_pending_scan_sources.empty() || m_pending_nand_reconciliation))
    {
      std::vector<std::string> pending = std::move(m_pending_scan_sources);
      m_pending_scan_sources.clear();
      m_pending_nand_reconciliation = false;
      StartGameScan(std::move(pending), false);
    }

    const Uint32 now = SDL_GetTicks();
    const Storage::UsbSnapshot usb_snapshot = Storage::GetUsbSnapshot();
    if (usb_snapshot.generation != m_usb_generation)
    {
      m_usb_generation = usb_snapshot.generation;
      m_usb_refresh_at = now + 350;
    }
    if (m_usb_refresh_at && SDL_TICKS_PASSED(now, m_usb_refresh_at))
    {
      m_usb_refresh_at = 0;
      const Storage::UsbSnapshot current = Storage::GetUsbSnapshot();
      std::unordered_map<std::string, std::string> old_roots;
      std::unordered_map<std::string, std::string> new_roots;
      for (const Storage::Location& location : m_usb_locations)
        old_roots.emplace(location.id, location.path);
      for (const Storage::Location& location : current.locations)
        new_roots.emplace(location.id, location.path);

      std::unordered_set<std::string> changed_ids;
      for (const auto& [id, root] : new_roots)
      {
        const auto old = old_roots.find(id);
        if (old == old_roots.end() ||
            Lower(NormalizePath(old->second)) != Lower(NormalizePath(root)))
          changed_ids.insert(id);
      }
      std::unordered_set<std::string> removed_ids;
      for (const auto& [id, root] : old_roots)
      {
        if (!new_roots.contains(id))
          removed_ids.insert(id);
      }
      for (const auto& [id, root] : new_roots)
        m_unavailable_usb_ids.erase(id);
      for (const std::string& id : removed_ids)
        m_unavailable_usb_ids.insert(id);
      m_usb_locations = current.locations;
      RefreshConfiguredUsbSources();
      // A disconnect/reconnect can return with the same ID and alias. The callback generation
      // still proves device activity, so refresh USB sources even when the topology looks equal.
      if (changed_ids.empty() && removed_ids.empty())
      {
        for (const auto& [source, binding] : m_usb_source_bindings)
          changed_ids.insert(binding.first);
      }
      for (const std::string& id : changed_ids)
        m_unavailable_usb_ids.insert(id);
      if (!removed_ids.empty() || !changed_ids.empty())
      {
        std::erase_if(m_games, [&](Game& game) {
          if (!game.storage_id.starts_with("usb:"))
            return false;
          const std::string id = game.storage_id.substr(4);
          if (!removed_ids.contains(id) && !changed_ids.contains(id))
            return false;
          if (game.cover)
            SDL_DestroyTexture(game.cover);
          return true;
        });
        // A removed volume may have produced a WAD candidate which is still in the worker's ready
        // queue and therefore absent from m_games. Always reconcile NAND after physical removal;
        // this also covers that progressive-scan race without requiring a visible WAD entry.
        m_pending_nand_reconciliation |= !removed_ids.empty();
      }
      for (const std::string& source : m_sources)
      {
        const auto binding = m_usb_source_bindings.find(Lower(source));
        if (binding != m_usb_source_bindings.end() && changed_ids.contains(binding->second.first))
          m_pending_scan_sources.push_back(source);
      }
      std::ranges::sort(m_pending_scan_sources);
      m_pending_scan_sources.erase(
          std::unique(m_pending_scan_sources.begin(), m_pending_scan_sources.end()),
          m_pending_scan_sources.end());
      RebuildVisibleGames();
      selection = pin_first_library_page || m_visible_games.empty() ?
                      0 :
                      std::clamp(selection, 0, static_cast<int>(m_visible_games.size()) - 1);
    }

    SDL_Event event{};
    while (PollEvent(&event))
    {
      int touch_x = 0;
      int touch_y = 0;
      const TouchKind touch = FeedTouch(event, &touch_x, &touch_y);
      if (touch == TouchKind::SwipeLeft || touch == TouchKind::SwipeRight)
      {
        pin_first_library_page = false;
        selection = GridPage(selection, touch == TouchKind::SwipeLeft ? 1 : -1);
        continue;
      }
      if (touch == TouchKind::Tap)
      {
        // Footer taps are dispatched by FeedTouch as synthetic controller presses.
        const int hit = GridHitTest(
            touch_x, touch_y,
            m_visible_games.empty() ? 0 : selection / GridPageSize() * GridPageSize());
        if (hit >= 0)
        {
          pin_first_library_page = false;
          if (hit == selection)
            launch = true;
          else
            selection = hit;
        }
        continue;
      }
      if (event.type != SDL_CONTROLLERBUTTONDOWN && event.type != SDL_KEYDOWN)
        continue;
      const int button = event.type == SDL_CONTROLLERBUTTONDOWN ? event.cbutton.button : -1;
      const SDL_Keycode key = event.type == SDL_KEYDOWN ? event.key.keysym.sym : SDLK_UNKNOWN;
      if (button == SDL_CONTROLLER_BUTTON_DPAD_LEFT || key == SDLK_LEFT)
      {
        pin_first_library_page = false;
        selection = GridNavigate(selection, -1, 0);
      }
      else if (button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT || key == SDLK_RIGHT)
      {
        pin_first_library_page = false;
        selection = GridNavigate(selection, 1, 0);
      }
      else if (button == SDL_CONTROLLER_BUTTON_DPAD_UP || key == SDLK_UP)
      {
        pin_first_library_page = false;
        selection = GridNavigate(selection, 0, -1);
      }
      else if (button == SDL_CONTROLLER_BUTTON_DPAD_DOWN || key == SDLK_DOWN)
      {
        pin_first_library_page = false;
        selection = GridNavigate(selection, 0, 1);
      }
      else if (button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER || key == SDLK_PAGEUP)
      {
        pin_first_library_page = false;
        selection = GridPage(selection, -1);
      }
      else if (button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER || key == SDLK_PAGEDOWN)
      {
        pin_first_library_page = false;
        selection = GridPage(selection, 1);
      }
      else if ((button == BUTTON_CONFIRM || key == SDLK_RETURN) && !m_visible_games.empty())
        launch = true;
      else if ((button == SDL_CONTROLLER_BUTTON_X || key == SDLK_s) && !m_visible_games.empty())
      {
        pin_first_library_page = false;
        Game* const selected_game = VisibleGame(selection);
        if (!selected_game)
          continue;
        const std::string sort_selected_key = selected_game->key;
        m_sort_mode = static_cast<SortMode>((static_cast<int>(m_sort_mode) + 1) % 3);
        m_store.SetInt("Launcher/SortMode", static_cast<int>(m_sort_mode));
        MarkStoreDirty();
        SortGames();
        selection = 0;
        select_key(sort_selected_key);
      }
      else if (button == BUTTON_SETTINGS || key == SDLK_F1)
      {
        const std::vector<std::string> old_sources = m_sources;
        SettingsRoot();
        FlushPendingSaves();
        if (!m_pending_launch && (old_sources != m_sources || m_library_refresh_requested))
          rescan_library();
      }
      else if (button == SDL_CONTROLLER_BUTTON_BACK || key == SDLK_f)
      {
        pin_first_library_page = false;
        const Game* selected = VisibleGame(selection);
        const std::string keep = selected ? selected->key : std::string{};
        LibraryFilterMenu();
        RebuildVisibleGames();
        selection = 0;
        select_key(keep);
        BeginScreenFx();
      }
      else if ((button == SDL_CONTROLLER_BUTTON_START || key == SDLK_SPACE) &&
               !m_visible_games.empty())
      {
        pin_first_library_page = false;
        bool rescan = false;
        PerGameMenu(VisibleGame(selection), &launch, &rescan);
        if (rescan)
          rescan_library();
      }
      else if (button == BUTTON_CANCEL || key == SDLK_ESCAPE)
      {
        if (ConfirmApplicationExit())
          break;
      }
    }
    if (!m_running)
      break;
    RenderGrid(selection);
    // Keep repainting while a scan is streaming games in, so the grid fills in front of the user.
    WaitForNextFrame(m_library_scan != nullptr);
  }

  if (m_user_exit_requested)
  {
    PrepareApplicationExit();
    return std::nullopt;
  }

  if (m_pending_launch)
  {
    if (m_library_identities_dirty)
      SaveLibraryIdentities();
    FlushPendingSaves();
    return std::exchange(m_pending_launch, std::nullopt);
  }
  if (!launch || m_visible_games.empty())
  {
    return std::nullopt;
  }
  Game* const selected_game = VisibleGame(selection);
  if (!selected_game)
    return std::nullopt;
  Game& game = *selected_game;
  NxLauncherStep("launch selected");
  game.played = static_cast<std::int64_t>(std::time(nullptr));
  m_store.Set("Recent/" + game.key, std::to_string(game.played));
  MarkStoreDirty();
  NxLauncherStep("launch: saving library");
  if (m_library_identities_dirty)
    SaveLibraryIdentities();
  FlushPendingSaves();
  NxLauncherStep("launch: closing launcher");
  if (game.installed_nand)
    return LaunchRequest{{}, game.game_id, game.revision, game.title_id};
  LaunchRequest request{game.path, game.game_id, game.revision};
  request.game_config_path = game.config_override_path;
  return request;
}

}  // namespace

std::optional<LaunchRequest> RunLauncher(std::string startup_message, std::string launcher_path)
{
  std::optional<LaunchRequest> request;
  {
    Launcher launcher(std::move(startup_message), std::move(launcher_path));
    request = launcher.Run();
  }
  return request;
}

bool PrepareLaunchStorage(const std::string& path, std::string* resolved_path)
{
  const std::string normalized = NormalizePath(path);
  if (resolved_path)
    *resolved_path = normalized;
  const std::string device = DeviceName(normalized);
  if (device.starts_with("ums"))
  {
    if (!Storage::InitializeUsb())
      return false;
    const auto path_exists = [](const std::string& candidate) {
      struct stat info{};
      return ::stat(candidate.c_str(), &info) == 0 &&
             (S_ISREG(info.st_mode) || S_ISDIR(info.st_mode));
    };
    const std::size_t colon = normalized.find(':');
    std::string relative =
        colon == std::string::npos ? std::string{} : normalized.substr(colon + 1);
    while (!relative.empty() && relative.front() == '/')
      relative.erase(relative.begin());
    const auto resolve_usb_path = [&]() -> std::string {
      if (path_exists(normalized))
        return normalized;
      std::vector<std::string> matches;
      for (const Storage::Location& location : Storage::ListUsbLocations())
      {
        const std::string candidate = NormalizePath(location.path + relative);
        if (path_exists(candidate))
          matches.push_back(candidate);
      }
      return matches.size() == 1 ? matches.front() : std::string{};
    };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    std::string available_path;
    while ((available_path = resolve_usb_path()).empty() &&
           std::chrono::steady_clock::now() < deadline)
    {
      if (!appletMainLoop())
        return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (available_path.empty())
      available_path = resolve_usb_path();
    if (available_path.empty())
      return false;
    if (resolved_path)
      *resolved_path = available_path;
    return true;
  }
  if (!device.starts_with("dsmb_"))
  {
    return true;
  }

  const std::string id = device.substr(5);
  if (Storage::IsSmbMounted(id))
  {
    return true;
  }
  Store store;
  if (!store.Load(std::string(CONFIG_PATH)))
    return false;
  const int share_count = std::clamp(store.GetInt("Storage/SmbCount", 0), 0, 8);
  for (int index = 0; index < share_count; ++index)
  {
    const std::string prefix = "Storage/Smb" + std::to_string(index);
    Storage::SmbShare share;
    share.id = store.Get(prefix + "Id");
    if (Lower(share.id) != id)
      continue;
    share.name = store.Get(prefix + "Name");
    share.server = store.Get(prefix + "Server");
    share.share = store.Get(prefix + "Share");
    share.path = store.Get(prefix + "Path");
    share.user = store.Get(prefix + "User");
    share.password = store.Get(prefix + "Password");
    share.domain = store.Get(prefix + "Domain");
    share.auto_mount = store.GetBool(prefix + "AutoMount", true);
    if (!Storage::MountSmb(share))
      return false;
    struct stat mounted_info{};
    const bool available = ::stat(normalized.c_str(), &mounted_info) == 0 &&
                           (S_ISREG(mounted_info.st_mode) || S_ISDIR(mounted_info.st_mode));
    return available;
  }
  return false;
}

bool ResolveLibraryLaunchPath(std::string_view library_id, const std::string& fallback_path,
                              std::string* resolved_path)
{
  const auto path_exists = [](const std::string& candidate) {
    struct stat info{};
    return ::stat(candidate.c_str(), &info) == 0 &&
           (S_ISREG(info.st_mode) || S_ISDIR(info.st_mode));
  };
  const auto try_path = [&](const std::string& candidate) {
    if (candidate.empty())
      return false;
    std::string prepared;
    if (!PrepareLaunchStorage(candidate, &prepared) || !path_exists(prepared))
      return false;
    if (resolved_path)
      *resolved_path = std::move(prepared);
    return true;
  };

  const bool valid_id = !library_id.empty() && library_id.size() <= 96 &&
                        std::ranges::all_of(library_id, [](unsigned char character) {
                          return std::isalnum(character) || character == '-' || character == '_';
                        });
  Store store;
  std::string canonical_path;
  std::string current_path;
  bool retired = false;
  bool record_found = false;
  if (valid_id && store.Load(std::string(CONFIG_PATH)))
  {
    const int count = std::clamp(store.GetInt("Library/IdentityCount", 0), 0, 16384);
    for (int index = 0; index < count; ++index)
    {
      const std::string prefix = "Library/Identity" + std::to_string(index);
      if (store.Get(prefix + "Id") != library_id)
        continue;
      canonical_path = NormalizePath(store.Get(prefix + "Path"));
      current_path = NormalizePath(store.Get(prefix + "CurrentPath"));
      retired = store.GetBool(prefix + "Retired", false);
      record_found = true;
      break;
    }
  }

  // A stable shortcut must never fall back to its embedded mutable path when the ID is invalid,
  // missing, or retired. In particular, a stale umsN: alias may now belong to another drive.
  if (!library_id.empty() && (!valid_id || !record_found || retired))
    return false;

  if (canonical_path.starts_with("usb:"))
  {
    const std::size_t slash = canonical_path.find('/', 4);
    const std::string volume_id =
        canonical_path.substr(4, slash == std::string::npos ? std::string::npos : slash - 4);
    std::string relative =
        slash == std::string::npos ? std::string{} : canonical_path.substr(slash + 1);

    // CurrentPath preserves case, whereas the canonical identity deliberately does not.  Reuse
    // its path below the old umsN: root when it still describes the same relative name.
    const std::size_t current_colon = current_path.find(':');
    if (current_colon != std::string::npos)
    {
      std::string current_relative = current_path.substr(current_colon + 1);
      while (!current_relative.empty() && current_relative.front() == '/')
        current_relative.erase(current_relative.begin());
      if (Lower(current_relative) == Lower(relative))
        relative = std::move(current_relative);
    }

    // Do not fall back to the mutable embedded umsN: alias when a stable volume is known: another
    // disk can later inherit that alias and contain an identically named file.
    if (volume_id.empty() || relative.starts_with('/') ||
        relative.find("../") != std::string::npos || relative == ".." || !Storage::InitializeUsb())
      return false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    do
    {
      const std::string root = Storage::ResolveUsbPath(volume_id);
      if (!root.empty())
      {
        const std::string candidate = NormalizePath(JoinPath(root, relative));
        if (PathAtOrBelow(candidate, root) && path_exists(candidate))
        {
          if (resolved_path)
            *resolved_path = candidate;
          return true;
        }
      }
      if (!appletMainLoop())
        return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
  }

  if (canonical_path.starts_with("smb:"))
  {
    const std::size_t slash = canonical_path.find('/', 4);
    const std::string share_id =
        canonical_path.substr(4, slash == std::string::npos ? std::string::npos : slash - 4);
    std::string relative =
        slash == std::string::npos ? std::string{} : canonical_path.substr(slash + 1);
    const std::string root = Storage::SmbRootPath(share_id);
    if (root.empty() || relative.starts_with('/') || relative.find("../") != std::string::npos ||
        relative == "..")
      return false;

    // Preserve case from CurrentPath when possible.  The dsmb_<id>: mount name itself is stable.
    const std::size_t current_colon = current_path.find(':');
    if (current_colon != std::string::npos &&
        Lower(DeviceName(current_path)) == Lower(DeviceName(root)))
    {
      std::string current_relative = current_path.substr(current_colon + 1);
      while (!current_relative.empty() && current_relative.front() == '/')
        current_relative.erase(current_relative.begin());
      if (Lower(current_relative) == Lower(relative))
        relative = std::move(current_relative);
    }
    const std::string candidate = NormalizePath(JoinPath(root, relative));
    return PathAtOrBelow(candidate, root) && try_path(candidate);
  }

  // SD paths are stable but can be case-sensitive on non-FAT devoptabs.  Prefer the exact current
  // path, then support launcher.ini records written before CurrentPath was introduced.
  if (try_path(current_path) || try_path(canonical_path))
    return true;
  return try_path(fallback_path);
}

void ShutdownLauncherStorage()
{
  Storage::Shutdown();
}
}  // namespace DolphinSwitch
