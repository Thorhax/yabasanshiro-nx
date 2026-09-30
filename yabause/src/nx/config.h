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

#pragma once

#include <map>
#include <string>
#include <vector>

// Everything the Switch port keeps on the SD card lives under this directory,
// next to the .nro itself.
#define NX_DATA_DIR "sdmc:/switch/yabasanshiro"

// Where builds before 0.1.0 kept their data; migrated on startup.
#define NX_OLD_DATA_DIR "sdmc:/yabasanshiro"

namespace nx {

// Minimal INI reader: "[section]" headers, "key = value" lines, '#'/';' comments.
// Keys are stored as "section.key", lower-cased; values keep their case.
class IniFile {
public:
  bool load(const std::string & path);

  bool has(const std::string & key) const;
  std::string get(const std::string & key, const std::string & def) const;
  int getInt(const std::string & key, int def) const;
  bool getBool(const std::string & key, bool def) const;

  const std::map<std::string, std::string> & values() const { return values_; }

private:
  std::map<std::string, std::string> values_;
};

// A game's own settings files over the global one: keys a game file sets win, and later
// game files win over earlier ones.
class LayeredIni {
public:
  // A missing file just contributes nothing.
  void load(const std::string & global_path, const std::vector<std::string> & game_paths);

  bool globalLoaded() const { return global_loaded_; }
  std::string get(const std::string & key, const std::string & def) const;
  int getInt(const std::string & key, int def) const;
  bool getBool(const std::string & key, bool def) const;

private:
  const IniFile * find(const std::string & key) const;

  IniFile global_;
  std::vector<IniFile> games_;
  bool global_loaded_ = false;
};

struct Settings {
  bool dynarec = true;
  int resolution_mode = 0;        // RES_* in ygl.h
  int rbg_resolution_mode = 0;    // RBG_RES_* in ygl.h
  bool rbg_compute_shader = true;
  int polygon_mode = 0;           // PERSPECTIVE_CORRECTION / CPU_ / GPU_TESSERATION
  bool frameskip = false;
  int framelimit = 0;             // 0 .. 60Hz, 1 .. no limit, 2 .. 120Hz
  bool sh2_cache = true;
  int cart = 0;                   // CART_* in cs0.h
  int aspect_mode = 1;            // ASPECT_RATE_MODE (ygl.h): 1 .. 4:3, passed to VIDCore->Resize
  bool vsync = false;             // the emulator's own limiter already paces frames
  bool sync_render = true;        // Vdp2SyncVBlankOut
  int scsp_sync_per_frame = 1;
  int scsp_main_mode = 0;         // 0 .. sound CPU locked to emulated time, 1 .. real time
  std::string bios = "auto";      // auto (by disc region), jp, us or hle (emulated BIOS)
};

std::string dataPath(const char * name);
void ensureDataDirs();

// Moves data from NX_OLD_DATA_DIR into NX_DATA_DIR, skipping anything that
// already exists at the new location. Logs what it did.
void migrateOldDataDir();

// Loads NX_DATA_DIR/settings.ini, writing a commented default file if missing, with the
// game's own settings file (if any) over it. The launcher's settings pages use the same keys
// and defaults (launcher/SaturnPages.inc).
Settings loadSettings(const std::vector<std::string> & game_inis = {});

} // namespace nx
