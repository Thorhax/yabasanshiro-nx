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

#include <string>
#include <vector>

#include <switch.h>

#include "saturn_controls.h"

namespace nx {

// For each Saturn button, the set of Switch buttons (HidNpadButton bits)
// that press it. Any one of them being held presses the Saturn button.
struct ButtonMap {
  u64 bind[kSaturnButtons];
};

class Input {
public:
  // Loads NX_DATA_DIR/input.ini (writing the defaults if missing), with the
  // game's own bindings (if any) over it, and attaches a standard Saturn pad
  // to each port.
  void init(const std::vector<std::string> & game_inis = {});

  // Reads the Switch controllers.
  void poll();
  // Sends what poll() read to the emulated pads; with forward false (a menu has the
  // controllers) every Saturn button is released instead.
  void apply(bool forward);
  void update() { poll(); apply(true); }

  // Switch buttons held / newly pressed this frame (for hotkeys and menus).
  u64 held(int player) const { return held_[player]; }
  u64 down(int player) const { return down_[player]; }

private:
  PadState pads_[kMaxPlayers];
  ButtonMap maps_[kMaxPlayers];
  u64 held_[kMaxPlayers] = {};
  u64 down_[kMaxPlayers] = {};
  u32 saturn_prev_[kMaxPlayers] = {};
};

} // namespace nx
