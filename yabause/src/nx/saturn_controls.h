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

// The controller plugged into each port, stored as "type" in that player's section:
//   pad        Saturn control pad
//   3dpad      Saturn 3D control pad in analog mode: the left stick is its analog stick and
//              ZL / ZR its analog triggers (the D-pad and buttons are bound as for the pad)
//   twinstick  Virtual On Twin Stick: a digital pad whose left lever is the D-pad and whose
//              right lever presses face buttons, so Switch's two sticks steer like the levers
//   none       nothing plugged in (some games wait for player 2 otherwise)
// Each type keeps its own bindings, in "player1", "player1_3dpad" or "player1_twinstick".
enum class ControllerType { Pad, Pad3D, TwinStick, None };

struct ControllerTypeInfo {
  ControllerType type;
  std::string_view key;          // value of "type"
  std::string_view label;        // shown in the launcher
  std::string_view section_suffix;
};

inline constexpr std::array<ControllerTypeInfo, 4> kControllerTypes = {{
  { ControllerType::Pad, "pad", "Saturn pad", "" },
  { ControllerType::Pad3D, "3dpad", "3D pad (analog)", "_3dpad" },
  { ControllerType::TwinStick, "twinstick", "Twin Stick (Virtual On)", "_twinstick" },
  { ControllerType::None, "none", "Not connected", "" },
}};

inline const ControllerTypeInfo & controllerTypeInfo(std::string_view key)
{
  for (const ControllerTypeInfo & info : kControllerTypes)
    if (info.key == key) return info;
  return kControllerTypes[0];
}

// The 3D pad's stick is analog, so the left stick isn't also bound to its D-pad
inline constexpr std::array<SaturnButton, kSaturnButtons> k3DPadButtonInfo = {{
  { "UP", "D-Pad up", "DUP" },
  { "RIGHT", "D-Pad right", "DRIGHT" },
  { "DOWN", "D-Pad down", "DDOWN" },
  { "LEFT", "D-Pad left", "DLEFT" },
  { "R", "R trigger", "ZR" },
  { "L", "L trigger", "ZL" },
  { "START", "Start", "PLUS" },
  { "A", "A", "B" },
  { "B", "B", "A" },
  { "C", "C", "R" },
  { "X", "X", "Y" },
  { "Y", "Y", "X" },
  { "Z", "Z", "L" },
}};

// Virtual On's Twin Stick (HSS-0154) is a standard digital pad inside, wired as:
//   left lever = D-pad, left trigger = L, left thumb = R
//   right lever up / down / left / right = Y / B / X / Z, right trigger = A, right thumb = C
// Triggers fire weapons and thumb buttons dash; both triggers together (L + A) is the centre
// weapon, which Switch Y also presses in one go.
inline constexpr std::array<SaturnButton, kSaturnButtons> kTwinStickButtonInfo = {{
  { "UP", "Left lever up", "LS_UP DUP" },
  { "RIGHT", "Left lever right", "LS_RIGHT DRIGHT" },
  { "DOWN", "Left lever down", "LS_DOWN DDOWN" },
  { "LEFT", "Left lever left", "LS_LEFT DLEFT" },
  { "R", "Left thumb, dash (R)", "ZL" },
  { "L", "Left trigger, weapon (L)", "L Y" },
  { "START", "Start", "PLUS" },
  { "A", "Right trigger, weapon (A)", "R Y" },
  { "B", "Right lever down (B)", "RS_DOWN" },
  { "C", "Right thumb, dash (C)", "ZR" },
  { "X", "Right lever left (X)", "RS_LEFT" },
  { "Y", "Right lever up (Y)", "RS_UP" },
  { "Z", "Right lever right (Z)", "RS_RIGHT" },
}};

inline const std::array<SaturnButton, kSaturnButtons> & buttonInfo(ControllerType type)
{
  switch (type) {
  case ControllerType::Pad3D: return k3DPadButtonInfo;
  case ControllerType::TwinStick: return kTwinStickButtonInfo;
  default: return kSaturnButtonInfo;
  }
}

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
