// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// In-game quick menu, adapted for YabaSanshiro from NaGa's Dolphin NX frontend
// (Source/Core/DolphinSwitch/RuntimeOverlay.h). The menu only decides what the player asked
// for; nx/main.cpp carries the actions out and nx/overlay.cpp puts it on screen.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace DolphinSwitch::RuntimeOverlay
{
enum class ActionType
{
  StopToLauncher,
  SaveState,
  LoadState,
  Reset,
  EjectDisc,
  ChangeDisc,
  ToggleFPS,
};

struct Action
{
  ActionType type{};
  int value = 0;  // save state slot
  std::string path;
};

// 'state_directory' and 'state_prefix' locate the save state files for the slot list
// (YabSaveStateSlot names them <prefix>_<slot, 3 digits>.yss).
void BeginSession(std::string game_path, std::string state_directory, std::string state_prefix,
                  bool show_fps);
void EndSession();

// Player 1's buttons (HidNpadButton bits): pressed this frame and held
void UpdateInput(std::uint64_t down, std::uint64_t held);
std::vector<Action> TakeActions();

bool IsVisible();
void Close();
// True while the menu is open, and after it closes until every button is released, so the
// buttons that closed it don't reach the game.
bool IsInputCaptured();
bool ShowFPS();

void SetStatus(std::string message);
// A status message only while it's recent, for drawing during play
std::string CurrentStatus();
void ShowAlert(std::string caption, std::string message);
void RefreshStateInfo();
void SetShowFPS(bool show);

// Draws the menu with Dear ImGui (between ImGui::NewFrame and ImGui::Render)
void Draw();
}  // namespace DolphinSwitch::RuntimeOverlay
