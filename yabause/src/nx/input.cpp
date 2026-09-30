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
        "#                 RS_UP RS_DOWN RS_LEFT RS_RIGHT (right stick)\n", fp);

  for (int p = 0; p < kMaxPlayers; p++) {
    fprintf(fp, "\n[player%d]\n", p + 1);
    for (const SaturnButton & button : kSaturnButtonInfo)
      fprintf(fp, "%-5s = %.*s\n", std::string(button.key).c_str(),
              (int)button.defaults.size(), button.defaults.data());
  }
  fclose(fp);
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
    for (int i = 0; i < kSaturnButtons; i++) {
      const SaturnButton & button = kSaturnButtonInfo[i];
      char key[32];
      snprintf(key, sizeof(key), "player%d.%.*s", p + 1, (int)button.key.size(), button.key.data());
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
    PerPad_struct * pad = PerPadAdd(ports[p]);
    for (int i = 0; i < kSaturnButtons; i++)
      PerSetKey(keyCode(p, i), i, pad);
  }
}

void Input::update()
{
  for (int p = 0; p < kMaxPlayers; p++) {
    padUpdate(&pads_[p]);
    held_[p] = padGetButtons(&pads_[p]);

    u32 saturn = 0;
    for (int i = 0; i < kSaturnButtons; i++)
      if (held_[p] & maps_[p].bind[i]) saturn |= 1u << i;

    // Only tell the emulator about changes
    u32 changed = saturn ^ saturn_prev_[p];
    for (int i = 0; i < kSaturnButtons; i++) {
      if (!(changed & (1u << i))) continue;
      if (saturn & (1u << i)) PerKeyDown(keyCode(p, i));
      else PerKeyUp(keyCode(p, i));
    }
    saturn_prev_[p] = saturn;
  }
}

} // namespace nx
