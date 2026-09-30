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

// Per-game cheats: Action Replay codes kept in RetroArch .cht files under
// <data dir>/cheats, fed to the core's cheat engine (cheat.c), which writes them to
// memory every VBLANK-IN. Only used from the main thread, which also runs the emulation.

#pragma once

#include <string>
#include <vector>

namespace nx {
namespace cheats {

struct Cheat {
  std::string desc;
  std::string code;       // as written in the file
  bool enabled = false;
  bool supported = true;  // false when a code line isn't one the engine can apply
};

// Finds the game's cheat file (by product code, then by disc image name) and applies the
// cheats it enables. Call after YabauseInit.
void load(const std::string & game_path, const char * game_code);
void unload();

const std::vector<Cheat> & list();
// The cheat file in use, or the one adding a cheat would create
std::string filePath();
// The file names looked for, for telling the player where to put one
std::vector<std::string> fileNames();

// Whether 'code' is Action Replay codes the engine can apply; 'error' says why not
bool check(const std::string & code, std::string * error);

// These apply the change at once and write the cheat file
bool setEnabled(int index, bool enabled);
// 'error' says what's wrong with a code that can't be used
bool add(const std::string & desc, const std::string & code, std::string * error);
bool remove(int index);

}  // namespace cheats
}  // namespace nx
