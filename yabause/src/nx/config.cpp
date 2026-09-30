/*
This file is part of YabaSanshiro.

        YabaSanshiro is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

YabaSanshiro is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

        You should have received a copy of the GNU General Public License
along with YabaSanshiro; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/

#include "config.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sys/stat.h>
#include <vector>

namespace nx {

static std::string trim(const std::string & s)
{
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

static std::string lower(std::string s)
{
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}

bool IniFile::load(const std::string & path)
{
  std::ifstream in(path);
  if (!in) return false;

  std::string line, section;
  while (std::getline(in, line)) {
    size_t comment = line.find_first_of("#;");
    if (comment != std::string::npos) line.erase(comment);
    line = trim(line);
    if (line.empty()) continue;

    if (line.front() == '[' && line.back() == ']') {
      section = lower(trim(line.substr(1, line.size() - 2)));
      continue;
    }

    size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string key = lower(trim(line.substr(0, eq)));
    if (!section.empty()) key = section + "." + key;
    values_[key] = trim(line.substr(eq + 1));
  }
  return true;
}

std::string IniFile::get(const std::string & key, const std::string & def) const
{
  auto it = values_.find(lower(key));
  return it == values_.end() ? def : it->second;
}

int IniFile::getInt(const std::string & key, int def) const
{
  std::string v = get(key, "");
  if (v.empty()) return def;
  char * end;
  long n = strtol(v.c_str(), &end, 0);
  return *end == '\0' ? (int)n : def;
}

bool IniFile::getBool(const std::string & key, bool def) const
{
  std::string v = lower(get(key, ""));
  if (v == "1" || v == "true" || v == "on" || v == "yes") return true;
  if (v == "0" || v == "false" || v == "off" || v == "no") return false;
  return def;
}

std::string dataPath(const char * name)
{
  return std::string(NX_DATA_DIR "/") + name;
}

void ensureDataDirs()
{
  mkdir("sdmc:/switch", 0777);
  mkdir(NX_DATA_DIR, 0777);
  mkdir(NX_DATA_DIR "/games", 0777);
  mkdir(NX_DATA_DIR "/cache", 0777);
}

static bool pathExists(const std::string & path)
{
  struct stat st;
  return stat(path.c_str(), &st) == 0;
}

static bool isDirectory(const std::string & path)
{
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// Moves the contents of 'from' into 'to' entry by entry, recursing into
// directories that exist on both sides (games/, cache/).
static void moveTree(const std::string & from, const std::string & to)
{
  DIR * d = opendir(from.c_str());
  if (!d) return;

  std::vector<std::string> names;
  while (struct dirent * e = readdir(d)) {
    if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0)
      names.push_back(e->d_name);
  }
  closedir(d);

  for (const std::string & name : names) {
    std::string src = from + "/" + name;
    std::string dst = to + "/" + name;
    if (!pathExists(dst)) {
      if (rename(src.c_str(), dst.c_str()) == 0)
        printf("Moved %s -> %s\n", src.c_str(), dst.c_str());
      else
        printf("Could not move %s -> %s\n", src.c_str(), dst.c_str());
    } else if (isDirectory(src) && isDirectory(dst)) {
      moveTree(src, dst);
    } else {
      printf("Kept %s: %s already exists\n", src.c_str(), dst.c_str());
    }
  }

  // Only succeeds once everything has been moved out
  rmdir(from.c_str());
}

void migrateOldDataDir()
{
  if (!isDirectory(NX_OLD_DATA_DIR)) return;
  printf("Migrating %s to %s\n", NX_OLD_DATA_DIR, NX_DATA_DIR);
  moveTree(NX_OLD_DATA_DIR, NX_DATA_DIR);
}

static const char * kDefaultSettings =
  "# YabaSanshiro NX settings. Delete this file to restore the defaults.\n"
  "\n"
  "[emulation]\n"
  "# SH2 CPU core: dynarec (fast) or interpreter (slow, for debugging)\n"
  "sh2_core = dynarec\n"
  "# Emulate the SH2 cache. Needed by a few games; small speed cost\n"
  "sh2_cache = on\n"
  "# 0 = 60Hz, 1 = unlimited, 2 = 120Hz\n"
  "frame_limit = 0\n"
  "# Skip drawing frames when emulation falls behind\n"
  "frame_skip = off\n"
  "# 0 = none, 6 = 1MB RAM cart, 7 = 4MB RAM cart\n"
  "cartridge = 0\n"
  "# Sound CPU timing: 0 = locked to emulated CPU time, 1 = real time\n"
  "sound_sync = 0\n"
  "\n"
  "[video]\n"
  "# 0 = native, 1 = 4x, 2 = 2x, 3 = original, 4 = 720p, 5 = 1080p\n"
  "resolution = 0\n"
  "# Rotating background resolution: 0 = original, 1 = 2x, 2 = 720p, 3 = 1080p, 4 = fit\n"
  "rbg_resolution = 0\n"
  "rbg_compute_shader = on\n"
  "# 0 = perspective correction, 1 = CPU tessellation, 2 = GPU tessellation\n"
  "polygon_mode = 0\n"
  "# 0 = original aspect ratio, 1 = stretch\n"
  "aspect = 0\n"
  "# Wait for the display refresh when presenting. The emulator already paces\n"
  "# itself to 60Hz; with both on, a frame occasionally loses its background\n"
  "vsync = off\n"
  "# Hold emulation until the background layers of each frame are drawn.\n"
  "# Turning this off is slightly faster but layers can flash black\n"
  "sync_render = on\n";

Settings loadSettings()
{
  std::string path = dataPath("settings.ini");
  IniFile ini;
  if (!ini.load(path)) {
    FILE * fp = fopen(path.c_str(), "w");
    if (fp) {
      fputs(kDefaultSettings, fp);
      fclose(fp);
    }
  }

  Settings s;
  s.dynarec = lower(ini.get("emulation.sh2_core", "dynarec")) != "interpreter";
  s.sh2_cache = ini.getBool("emulation.sh2_cache", s.sh2_cache);
  s.framelimit = ini.getInt("emulation.frame_limit", s.framelimit);
  s.frameskip = ini.getBool("emulation.frame_skip", s.frameskip);
  s.cart = ini.getInt("emulation.cartridge", s.cart);
  s.scsp_main_mode = ini.getInt("emulation.sound_sync", s.scsp_main_mode);
  s.resolution_mode = ini.getInt("video.resolution", s.resolution_mode);
  s.rbg_resolution_mode = ini.getInt("video.rbg_resolution", s.rbg_resolution_mode);
  s.rbg_compute_shader = ini.getBool("video.rbg_compute_shader", s.rbg_compute_shader);
  s.polygon_mode = ini.getInt("video.polygon_mode", s.polygon_mode);
  s.aspect_mode = ini.getInt("video.aspect", s.aspect_mode);
  s.vsync = ini.getBool("video.vsync", s.vsync);
  s.sync_render = ini.getBool("video.sync_render", s.sync_render);
  return s;
}

} // namespace nx
