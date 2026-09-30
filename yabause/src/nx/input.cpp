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

#include "input.h"
#include "config.h"

#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>

extern "C" {
#include "peripheral.h"
}

namespace nx {

// The emulator identifies inputs by an arbitrary u32; encode player + button.
static u32 keyCode(int player, int saturn_button)
{
  return ((u32)player << 24) | (u32)saturn_button;
}

struct NamedButton {
  const char * name;
  u64 bit;
};

static const NamedButton kSwitchButtons[] = {
  { "A", HidNpadButton_A },
  { "B", HidNpadButton_B },
  { "X", HidNpadButton_X },
  { "Y", HidNpadButton_Y },
  { "L", HidNpadButton_L },
  { "R", HidNpadButton_R },
  { "ZL", HidNpadButton_ZL },
  { "ZR", HidNpadButton_ZR },
  { "PLUS", HidNpadButton_Plus },
  { "MINUS", HidNpadButton_Minus },
  { "LSTICK", HidNpadButton_StickL },
  { "RSTICK", HidNpadButton_StickR },
  { "DUP", HidNpadButton_Up },
  { "DDOWN", HidNpadButton_Down },
  { "DLEFT", HidNpadButton_Left },
  { "DRIGHT", HidNpadButton_Right },
  { "LS_UP", HidNpadButton_StickLUp },
  { "LS_DOWN", HidNpadButton_StickLDown },
  { "LS_LEFT", HidNpadButton_StickLLeft },
  { "LS_RIGHT", HidNpadButton_StickLRight },
  { "RS_UP", HidNpadButton_StickRUp },
  { "RS_DOWN", HidNpadButton_StickRDown },
  { "RS_LEFT", HidNpadButton_StickRLeft },
  { "RS_RIGHT", HidNpadButton_StickRRight },
};

static u64 parseBinds(const std::string & value, const char * where)
{
  u64 bits = 0;
  std::istringstream words(value);
  std::string word;
  while (words >> word) {
    for (char & c : word) c = toupper((unsigned char)c);
    bool found = false;
    for (const NamedButton & b : kSwitchButtons) {
      if (word == b.name) {
        bits |= b.bit;
        found = true;
        break;
      }
    }
    if (!found)
      printf("input.ini: unknown Switch button '%s' for %s\n", word.c_str(), where);
  }
  return bits;
}

static void writeDefaultFile(const std::string & path)
{
  FILE * fp = fopen(path.c_str(), "w");
  if (!fp) return;

  fputs("# YabaSanshiro NX controls. Delete this file to restore the defaults.\n"
        "#\n"
        "# Each line binds a Saturn button to one or more Switch buttons:\n"
        "#   SATURN_BUTTON = SWITCH_BUTTON [SWITCH_BUTTON ...]\n"
        "# Leave the right side empty to unbind a button.\n"
        "#\n"
        "# Saturn buttons: UP DOWN LEFT RIGHT START A B C X Y Z L R\n"
        "# Switch buttons: A B X Y L R ZL ZR PLUS MINUS LSTICK RSTICK\n"
        "#                 DUP DDOWN DLEFT DRIGHT\n"
        "#                 LS_UP LS_DOWN LS_LEFT LS_RIGHT (left stick)\n"
        "#                 RS_UP RS_DOWN RS_LEFT RS_RIGHT (right stick)\n"
        "#\n"
        "# 'type' picks the controller: pad (Saturn pad), 3dpad (3D pad, analog: the\n"
        "# left stick and ZL/ZR are analog), twinstick (Virtual On Twin Stick) or\n"
        "# none (nothing plugged into that port). The\n"
        "# 3D pad and Twin Stick keep their own bindings in [playerN_3dpad] and\n"
        "# [playerN_twinstick]; missing ones use their defaults.\n", fp);

  for (int p = 0; p < kMaxPlayers; p++) {
    fprintf(fp, "\n[player%d]\ntype  = pad\n", p + 1);
    for (const SaturnButton & button : kSaturnButtonInfo)
      fprintf(fp, "%-5s = %.*s\n", std::string(button.key).c_str(),
              (int)button.defaults.size(), button.defaults.data());
  }
  fclose(fp);
}

// Switch stick position (-32767 .. 32767, up positive) as a Saturn axis byte (0 .. 255,
// up / left at 0)
static u8 axisByte(s32 value, bool invert)
{
  if (invert) value = -value;
  int byte = (value + 32768) >> 8;
  return (u8)(byte < 0 ? 0 : byte > 255 ? 255 : byte);
}

void Input::init(const std::vector<std::string> & game_inis)
{
  std::string path = dataPath("input.ini");
  LayeredIni ini;
  ini.load(path, game_inis);
  if (!ini.globalLoaded()) {
    writeDefaultFile(path);
    ini.load(path, game_inis);
  }

  for (int p = 0; p < kMaxPlayers; p++) {
    char type_key[32];
    snprintf(type_key, sizeof(type_key), "player%d.type", p + 1);
    std::string type_name = ini.get(type_key, "pad");
    for (char & c : type_name) c = tolower((unsigned char)c);
    const ControllerTypeInfo & type = controllerTypeInfo(type_name);
    types_[p] = type.type;
    printf("Player %d: %.*s\n", p + 1, (int)type.label.size(), type.label.data());

    const auto & buttons = buttonInfo(type.type);
    for (int i = 0; i < kSaturnButtons; i++) {
      const SaturnButton & button = buttons[i];
      char key[48];
      snprintf(key, sizeof(key), "player%d%.*s.%.*s", p + 1, (int)type.section_suffix.size(),
               type.section_suffix.data(), (int)button.key.size(), button.key.data());
      maps_[p].bind[i] = parseBinds(ini.get(key, std::string(button.defaults)), key);
    }
  }

  padConfigureInput(kMaxPlayers, HidNpadStyleSet_NpadStandard);
  // Player 1 also accepts the handheld (attached Joy-Con) controller.
  padInitialize(&pads_[0], HidNpadIdType_No1, HidNpadIdType_Handheld);
  padInitialize(&pads_[1], HidNpadIdType_No2);

  PerPortReset();
  PortData_struct * ports[kMaxPlayers] = { &PORTDATA1, &PORTDATA2 };
  for (int p = 0; p < kMaxPlayers; p++) {
    saturn_prev_[p] = 0;
    analog_[p] = nullptr;
    // An empty port: nothing is added, so the game sees no controller
    if (types_[p] == ControllerType::None)
      continue;
    // The Twin Stick is a digital pad as far as the Saturn is concerned
    void * controller;
    if (types_[p] == ControllerType::Pad3D) {
      PerAnalog_struct * analog = Per3DPadAdd(ports[p]);
      analog_[p] = analog;
      controller = analog;
    } else {
      analog_[p] = nullptr;
      controller = PerPadAdd(ports[p]);
    }
    for (int i = 0; i < kSaturnButtons; i++)
      PerSetKey(keyCode(p, i), i, controller);
    // Centred stick, released triggers
    axes_prev_[p][0] = axes_prev_[p][1] = 0x7F;
    axes_prev_[p][2] = axes_prev_[p][3] = 0;
    if (analog_[p]) {
      PerAnalog_struct * analog = (PerAnalog_struct *)analog_[p];
      PerAxis1Value(analog, 0x7F);
      PerAxis2Value(analog, 0x7F);
      PerAxis3Value(analog, 0);
      PerAxis4Value(analog, 0);
    }
  }
}

void Input::poll()
{
  for (int p = 0; p < kMaxPlayers; p++) {
    padUpdate(&pads_[p]);
    const u64 previous = held_[p];
    held_[p] = padGetButtons(&pads_[p]);
    down_[p] = held_[p] & ~previous;
  }
}

void Input::apply(bool forward)
{
  for (int p = 0; p < kMaxPlayers; p++) {
    if (types_[p] == ControllerType::None)
      continue;
    u32 saturn = 0;
    if (forward) {
      for (int i = 0; i < kSaturnButtons; i++)
        if (held_[p] & maps_[p].bind[i]) saturn |= 1u << i;
    }

    // Only tell the emulator about changes
    u32 changed = saturn ^ saturn_prev_[p];
    for (int i = 0; i < kSaturnButtons; i++) {
      if (!(changed & (1u << i))) continue;
      if (saturn & (1u << i)) PerKeyDown(keyCode(p, i));
      else PerKeyUp(keyCode(p, i));
    }
    saturn_prev_[p] = saturn;

    // 3D pad: the left stick is its analog stick, and whatever presses Saturn L / R pulls
    // the analog triggers fully (the Switch's triggers are digital)
    if (analog_[p]) {
      PerAnalog_struct * analog = (PerAnalog_struct *)analog_[p];
      u8 axes[4] = { 0x7F, 0x7F, 0, 0 };
      if (forward) {
        const HidAnalogStickState stick = padGetStickPos(&pads_[p], 0);
        axes[0] = axisByte(stick.x, false);
        axes[1] = axisByte(stick.y, true);
        axes[2] = (saturn & (1u << PERPAD_RIGHT_TRIGGER)) ? 0xFF : 0;
        axes[3] = (saturn & (1u << PERPAD_LEFT_TRIGGER)) ? 0xFF : 0;
      }
      if (axes[0] != axes_prev_[p][0]) PerAxis1Value(analog, axes[0]);
      if (axes[1] != axes_prev_[p][1]) PerAxis2Value(analog, axes[1]);
      // Right trigger, then left, as the 3D pad reports them
      if (axes[2] != axes_prev_[p][2]) PerAxis3Value(analog, axes[2]);
      if (axes[3] != axes_prev_[p][3]) PerAxis4Value(analog, axes[3]);
      memcpy(axes_prev_[p], axes, sizeof(axes));
    }
  }
}

} // namespace nx
