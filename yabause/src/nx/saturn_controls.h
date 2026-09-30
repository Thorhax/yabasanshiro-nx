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

// Saturn pad buttons and the names of the Switch buttons they can be bound to, shared by
// the emulator (input.cpp) and the launcher's controls page.
//
// Bindings are stored as space-separated Switch button names, per player, e.g.
//   [player1]
//   UP = DUP LS_UP
// in input.ini (global) or a game's settings file (per game).

#pragma once

#include <array>
#include <string_view>

namespace nx {

constexpr int kMaxPlayers = 2;
constexpr int kSaturnButtons = 13;   // PERPAD_UP .. PERPAD_Z

struct SaturnButton {
  std::string_view key;       // input.ini key
  std::string_view label;     // shown in the launcher
  std::string_view defaults;  // default Switch buttons
};

// Indexed by PERPAD_* (peripheral.h). The Saturn pad has A B C on the bottom row and
// X Y Z on the top row.
inline constexpr std::array<SaturnButton, kSaturnButtons> kSaturnButtonInfo = {{
  { "UP", "D-Pad up", "DUP LS_UP" },
  { "RIGHT", "D-Pad right", "DRIGHT LS_RIGHT" },
  { "DOWN", "D-Pad down", "DDOWN LS_DOWN" },
  { "LEFT", "D-Pad left", "DLEFT LS_LEFT" },
  { "R", "R shoulder", "ZR" },
  { "L", "L shoulder", "ZL" },
  { "START", "Start", "PLUS" },
  { "A", "A", "B" },
  { "B", "B", "A" },
  { "C", "C", "R" },
  { "X", "X", "Y" },
  { "Y", "Y", "X" },
  { "Z", "Z", "L" },
}};

struct SwitchButtonName {
  std::string_view key;
  std::string_view label;
};

inline constexpr std::array<SwitchButtonName, 24> kSwitchButtonNames = {{
  { "A", "A" },
  { "B", "B" },
  { "X", "X" },
  { "Y", "Y" },
  { "L", "L" },
  { "R", "R" },
  { "ZL", "ZL" },
  { "ZR", "ZR" },
  { "PLUS", "Plus" },
  { "MINUS", "Minus" },
  { "LSTICK", "Left stick click" },
  { "RSTICK", "Right stick click" },
  { "DUP", "D-Pad up" },
  { "DDOWN", "D-Pad down" },
  { "DLEFT", "D-Pad left" },
  { "DRIGHT", "D-Pad right" },
  { "LS_UP", "Left stick up" },
  { "LS_DOWN", "Left stick down" },
  { "LS_LEFT", "Left stick left" },
  { "LS_RIGHT", "Left stick right" },
  { "RS_UP", "Right stick up" },
  { "RS_DOWN", "Right stick down" },
  { "RS_LEFT", "Right stick left" },
  { "RS_RIGHT", "Right stick right" },
}};

inline std::string_view switchButtonLabel(std::string_view key)
{
  for (const SwitchButtonName & name : kSwitchButtonNames)
    if (name.key == key) return name.label;
  return key;
}

} // namespace nx
