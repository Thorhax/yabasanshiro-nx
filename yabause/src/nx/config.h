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

  std::string get(const std::string & key, const std::string & def) const;
  int getInt(const std::string & key, int def) const;
  bool getBool(const std::string & key, bool def) const;

  const std::map<std::string, std::string> & values() const { return values_; }

private:
  std::map<std::string, std::string> values_;
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
  int aspect_mode = 0;            // passed to VIDCore->Resize
  bool vsync = false;             // the emulator's own limiter already paces frames
  bool sync_render = true;        // Vdp2SyncVBlankOut
  int scsp_sync_per_frame = 1;
  int scsp_main_mode = 0;         // 0 .. sound CPU locked to emulated time, 1 .. real time
};

std::string dataPath(const char * name);
void ensureDataDirs();

// Moves data from NX_OLD_DATA_DIR into NX_DATA_DIR, skipping anything that
// already exists at the new location. Logs what it did.
void migrateOldDataDir();

// Loads NX_DATA_DIR/settings.ini, writing a commented default file if missing.
Settings loadSettings();

} // namespace nx
