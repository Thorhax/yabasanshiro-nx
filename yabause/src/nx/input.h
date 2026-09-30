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

  // Reads the Switch controllers and forwards the result to the emulator.
  void update();

  // Buttons held on player 1's controller this frame (for hotkeys).
  u64 held(int player) const { return held_[player]; }

private:
  PadState pads_[kMaxPlayers];
  ButtonMap maps_[kMaxPlayers];
  u64 held_[kMaxPlayers] = {};
  u32 saturn_prev_[kMaxPlayers] = {};
};

} // namespace nx
