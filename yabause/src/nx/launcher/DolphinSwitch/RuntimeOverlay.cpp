// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// In-game quick menu, adapted for YabaSanshiro from NaGa's Dolphin NX frontend
// (Source/Core/DolphinSwitch/RuntimeOverlay.cpp). Kept from the original: the page and
// selection model, controller navigation and the ImGui drawing and style. Removed: the
// GameCube/Wii pages (achievements, controller modes, frame generation, VBI skip) and
// Dolphin's asynchronous state handling; YabaSanshiro saves and loads while the game is paused.
// The cheats page is YabaSanshiro's own (Action Replay codes, see nx/cheats.h).

#include "DolphinSwitch/RuntimeOverlay.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string_view>
#include <sys/stat.h>

#include <switch.h>

#include "imgui.h"

namespace DolphinSwitch::RuntimeOverlay
{
namespace
{
enum class Page
{
  Main,
  SaveStates,
  LoadStates,
  Cheats,
  Disc,
  DiscBrowser,
  Alert,
};

struct BrowserEntry
{
  std::string name;
  std::string path;
  bool directory = false;
};

constexpr std::uint64_t MENU_CHORD = HidNpadButton_Minus | HidNpadButton_Plus;
constexpr int NUM_STATES = 10;
constexpr int MAIN_ITEM_COUNT = 9;
constexpr int MAIN_SHOW_FPS = 5;
constexpr int MAIN_ASPECT = 6;
constexpr int DISC_ITEM_COUNT = 3;

std::mutex s_mutex;
bool s_visible = false;
bool s_capture_until_release = false;
bool s_initialized = false;
bool s_show_fps = false;
std::string s_aspect;
Page s_page = Page::Main;
int s_selection = 0;
std::deque<Action> s_actions;
std::string s_game_path;
std::string s_state_directory;
std::string s_state_prefix;
std::array<std::string, NUM_STATES> s_state_info;
std::string s_browser_directory;
std::vector<BrowserEntry> s_browser_entries;
std::string s_browser_error;
std::string s_status;
std::chrono::steady_clock::time_point s_status_until{};
std::string s_alert_caption;
std::string s_alert_message;
std::vector<CheatEntry> s_cheats;
std::string s_cheat_file;
std::vector<std::string> s_cheat_file_names;
int s_delete_armed = -1;  // cheat that the next X press deletes

std::string Lower(std::string value)
{
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

std::string Shorten(std::string value, std::size_t limit)
{
  if (value.size() <= limit)
    return value;
  if (limit <= 3)
    return value.substr(0, limit);
  return value.substr(0, limit - 3) + "...";
}

bool IsDiscImage(const std::filesystem::path& path)
{
  static constexpr std::array<std::string_view, 5> extensions = {".cue", ".chd", ".iso", ".ccd",
                                                                 ".mds"};
  const std::string extension = Lower(path.extension().string());
  return std::find(extensions.begin(), extensions.end(), extension) != extensions.end();
}

void QueueAction(Action action)
{
  s_actions.push_back(std::move(action));
}

void SetStatusLocked(std::string message)
{
  s_status = std::move(message);
  s_status_until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
}

void SetPage(Page page)
{
  s_page = page;
  s_selection = 0;
  s_delete_armed = -1;
}

void OpenMenu()
{
  SetPage(Page::Main);
  s_visible = true;
}

void CloseMenu()
{
  s_visible = false;
  s_capture_until_release = true;
}

void RefreshStateInfoLocked()
{
  for (int slot = 0; slot < NUM_STATES; ++slot)
  {
    char path[640];
    std::snprintf(path, sizeof(path), "%s/%s_%03d.yss", s_state_directory.c_str(),
                  s_state_prefix.c_str(), slot + 1);
    struct stat info{};
    if (s_state_prefix.empty() || ::stat(path, &info) != 0)
    {
      s_state_info[slot] = "Empty";
      continue;
    }
    char when[64];
    const std::time_t modified = info.st_mtime;
    std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M", std::localtime(&modified));
    s_state_info[slot] = when;
  }
}

void ScanBrowserDirectory(const std::string& directory)
{
  s_browser_directory = directory;
  s_browser_entries.clear();
  s_browser_error.clear();
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(directory, error))
  {
    const bool is_directory = entry.is_directory(error);
    if (!is_directory && !IsDiscImage(entry.path()))
      continue;
    s_browser_entries.push_back(
        {entry.path().filename().string(), entry.path().string(), is_directory});
  }
  if (error)
    s_browser_error = "The folder could not be read";
  std::sort(s_browser_entries.begin(), s_browser_entries.end(),
            [](const BrowserEntry& a, const BrowserEntry& b) {
              if (a.directory != b.directory)
                return a.directory;
              return Lower(a.name) < Lower(b.name);
            });
  s_selection = 0;
}

void EnterDiscBrowser()
{
  SetPage(Page::DiscBrowser);
  const std::filesystem::path game(s_game_path);
  ScanBrowserDirectory(game.has_parent_path() ? game.parent_path().string() : std::string("sdmc:/"));
}

int ItemCount()
{
  switch (s_page)
  {
  case Page::Main:
    return MAIN_ITEM_COUNT;
  case Page::SaveStates:
  case Page::LoadStates:
    return NUM_STATES + 1;
  case Page::Cheats:
    return static_cast<int>(s_cheats.size()) + 2;  // + Add cheat code, Back
  case Page::Disc:
    return DISC_ITEM_COUNT;
  case Page::DiscBrowser:
    return std::max(1, static_cast<int>(s_browser_entries.size()));
  case Page::Alert:
    return 1;
  }
  return 1;
}

void HandleBack()
{
  switch (s_page)
  {
  case Page::Main:
  case Page::Alert:
    CloseMenu();
    return;
  case Page::DiscBrowser:
  {
    const std::filesystem::path current(s_browser_directory);
    const std::filesystem::path parent = current.parent_path();
    if (!parent.empty() && parent != current)
      ScanBrowserDirectory(parent.string());
    else
      SetPage(Page::Disc);
    return;
  }
  default:
    SetPage(Page::Main);
    return;
  }
}

void ActivateSelection()
{
  switch (s_page)
  {
  case Page::Main:
    switch (s_selection)
    {
    case 0:
      CloseMenu();
      break;
    case 1:
      SetPage(Page::SaveStates);
      break;
    case 2:
      SetPage(Page::LoadStates);
      break;
    case 3:
      SetPage(Page::Cheats);
      break;
    case 4:
      SetPage(Page::Disc);
      break;
    case MAIN_SHOW_FPS:
      QueueAction({ActionType::ToggleFPS});
      break;
    case MAIN_ASPECT:
      QueueAction({ActionType::CycleAspect, 1});
      break;
    case 7:
      QueueAction({ActionType::Reset});
      CloseMenu();
      break;
    case 8:
      QueueAction({ActionType::StopToLauncher});
      CloseMenu();
      break;
    }
    break;
  case Page::SaveStates:
  case Page::LoadStates:
    if (s_selection == NUM_STATES)
    {
      SetPage(Page::Main);
    }
    else if (s_page == Page::LoadStates && s_state_info[s_selection] == "Empty")
    {
      SetStatusLocked("Slot " + std::to_string(s_selection + 1) + " is empty");
    }
    else
    {
      QueueAction({s_page == Page::SaveStates ? ActionType::SaveState : ActionType::LoadState,
                   s_selection + 1});
    }
    break;
  case Page::Cheats:
  {
    const int count = static_cast<int>(s_cheats.size());
    if (s_selection < count)
    {
      if (s_cheats[s_selection].supported)
        QueueAction({ActionType::ToggleCheat, s_selection});
      else
        SetStatusLocked("This cheat uses codes the emulator can't apply");
    }
    else if (s_selection == count)
    {
      QueueAction({ActionType::AddCheat});
    }
    else
    {
      SetPage(Page::Main);
    }
    break;
  }
  case Page::Disc:
    if (s_selection == 0)
    {
      QueueAction({ActionType::EjectDisc});
      CloseMenu();
    }
    else if (s_selection == 1)
    {
      EnterDiscBrowser();
    }
    else
    {
      SetPage(Page::Main);
    }
    break;
  case Page::DiscBrowser:
    if (s_browser_entries.empty())
    {
      SetPage(Page::Disc);
      break;
    }
    if (const BrowserEntry selected = s_browser_entries[s_selection]; selected.directory)
    {
      ScanBrowserDirectory(selected.path);
    }
    else
    {
      QueueAction({ActionType::ChangeDisc, 0, selected.path});
      CloseMenu();
    }
    break;
  case Page::Alert:
    CloseMenu();
    break;
  }
}

void CenteredText(std::string_view text)
{
  const float width = ImGui::CalcTextSize(text.data(), text.data() + text.size()).x;
  ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(),
                                (ImGui::GetWindowWidth() - width) * 0.5f));
  ImGui::TextUnformatted(text.data(), text.data() + text.size());
}

void SelectableRow(const std::string& label, int index, float height = 48.0f)
{
  const bool selected = s_selection == index;
  ImGui::Selectable((label + "##row" + std::to_string(index)).c_str(), selected,
                    ImGuiSelectableFlags_None, {0.0f, height});
  if (selected)
    ImGui::SetScrollHereY(0.5f);
}

void RenderMainPage()
{
  SelectableRow("Resume game", 0);
  SelectableRow("Save state                                      >", 1);
  SelectableRow("Load state                                      >", 2);
  SelectableRow("Cheats                                          >", 3);
  SelectableRow("Disc management                                 >", 4);
  SelectableRow(std::string("Show FPS                         <  ") +
                    (s_show_fps ? "Enabled" : "Disabled") + "  >",
                MAIN_SHOW_FPS);
  SelectableRow("Aspect ratio                     <  " + s_aspect + "  >", MAIN_ASPECT);
  SelectableRow("Reset console", 7);
  SelectableRow("Return to launcher", 8);
}

void RenderCheatsPage()
{
  const int count = static_cast<int>(s_cheats.size());
  const float child_height = std::max(220.0f, ImGui::GetContentRegionAvail().y - 96.0f);
  ImGui::BeginChild("cheats", {0.0f, child_height}, false, ImGuiWindowFlags_NoInputs);
  if (count == 0)
  {
    ImGui::PushTextWrapPos(ImGui::GetWindowWidth() - 10.0f);
    std::string names;
    for (const std::string& name : s_cheat_file_names)
      names += (names.empty() ? "" : " or ") + name;
    ImGui::TextDisabled("No cheats for this game. Add Action Replay codes with Y, or copy a "
                        "RetroArch cheat file to the cheats folder named %s.",
                        names.empty() ? "after the game" : names.c_str());
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
  }
  for (int index = 0; index < count; ++index)
  {
    const CheatEntry& cheat = s_cheats[index];
    const char* state = !cheat.supported     ? "[ n/a ]  " :
                        cheat.enabled        ? "[ ON  ]  " :
                                               "[ off ]  ";
    std::string label = std::string(state) + Shorten(cheat.name, 64);
    if (s_delete_armed == index)
      label = "Press X again to delete:  " + Shorten(cheat.name, 46);
    if (!cheat.supported)
      ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    else if (cheat.enabled)
      ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(140, 235, 150, 255));
    SelectableRow(label, index);
    if (!cheat.supported || cheat.enabled)
      ImGui::PopStyleColor();
  }
  SelectableRow("Add cheat code...", count);
  SelectableRow("Back", count + 1);
  ImGui::EndChild();
  ImGui::Separator();
  ImGui::TextDisabled("%s", Shorten(s_cheat_file, 80).c_str());
  CenteredText("A  Toggle     Y  Add code     X  Delete     B  Back");
}

void RenderStatePage(bool saving)
{
  const float child_height = std::max(220.0f, ImGui::GetContentRegionAvail().y - 58.0f);
  ImGui::BeginChild("state slots", {0.0f, child_height}, false, ImGuiWindowFlags_NoInputs);
  for (int slot = 0; slot < NUM_STATES; ++slot)
    SelectableRow("Slot " + std::to_string(slot + 1) + "    " + Shorten(s_state_info[slot], 66),
                  slot);
  SelectableRow("Back", NUM_STATES);
  ImGui::EndChild();
  ImGui::Separator();
  CenteredText(saving ? "A  Save     B  Back" : "A  Load     B  Back");
}

void RenderDiscPage()
{
  SelectableRow("Eject current disc", 0, 64.0f);
  SelectableRow("Change disc from current game folder           >", 1, 64.0f);
  SelectableRow("Back", 2, 64.0f);
  ImGui::Separator();
  CenteredText("Disc images: CUE, CHD, ISO, CCD and MDS");
}

void RenderDiscBrowser()
{
  ImGui::TextUnformatted(Shorten(s_browser_directory, 88).c_str());
  ImGui::Separator();
  const float child_height = std::max(200.0f, ImGui::GetContentRegionAvail().y - 58.0f);
  ImGui::BeginChild("disc files", {0.0f, child_height}, false, ImGuiWindowFlags_NoInputs);
  if (s_browser_entries.empty())
  {
    SelectableRow(s_browser_error.empty() ? "No disc images in this folder" : s_browser_error, 0,
                  64.0f);
  }
  else
  {
    for (int index = 0; index < static_cast<int>(s_browser_entries.size()); ++index)
    {
      const BrowserEntry& entry = s_browser_entries[index];
      SelectableRow(std::string(entry.directory ? "[Folder]  " : "[Disc]    ") +
                        Shorten(entry.name, 70),
                    index);
    }
  }
  ImGui::EndChild();
  ImGui::Separator();
  CenteredText("A  Open / insert     B  Parent folder");
}

void RenderAlert()
{
  ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 205, 116, 255));
  CenteredText(s_alert_caption.empty() ? "YabaSanshiro message" : s_alert_caption);
  ImGui::PopStyleColor();
  ImGui::Spacing();
  ImGui::PushTextWrapPos(ImGui::GetWindowWidth() - 30.0f);
  ImGui::TextWrapped("%s", s_alert_message.c_str());
  ImGui::PopTextWrapPos();
  ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 86.0f);
  ImGui::Separator();
  CenteredText("A / B  Close");
}
}  // namespace

void BeginSession(std::string game_path, std::string state_directory, std::string state_prefix,
                  bool show_fps)
{
  std::lock_guard lock{s_mutex};
  s_game_path = std::move(game_path);
  s_state_directory = std::move(state_directory);
  s_state_prefix = std::move(state_prefix);
  s_visible = false;
  s_capture_until_release = false;
  s_page = Page::Main;
  s_selection = 0;
  s_actions.clear();
  s_show_fps = show_fps;
  s_browser_directory.clear();
  s_browser_entries.clear();
  s_browser_error.clear();
  s_status.clear();
  s_alert_caption.clear();
  s_alert_message.clear();
  RefreshStateInfoLocked();
  s_initialized = true;
}

void EndSession()
{
  std::lock_guard lock{s_mutex};
  s_initialized = false;
  s_visible = false;
  s_capture_until_release = false;
  s_actions.clear();
  s_browser_entries.clear();
  s_cheats.clear();
}

void UpdateInput(std::uint64_t down, std::uint64_t held)
{
  std::lock_guard lock{s_mutex};
  if (!s_initialized)
    return;

  if (s_capture_until_release)
  {
    if (held != 0)
      return;
    s_capture_until_release = false;
  }

  if ((held & MENU_CHORD) == MENU_CHORD && (down & MENU_CHORD) != 0)
  {
    if (s_visible)
      CloseMenu();
    else
      OpenMenu();
    return;
  }

  if (!s_visible)
    return;
  const int count = std::max(1, ItemCount());
  if (down & (HidNpadButton_Up | HidNpadButton_StickLUp))
    s_selection = (s_selection + count - 1) % count;
  else if (down & (HidNpadButton_Down | HidNpadButton_StickLDown))
    s_selection = (s_selection + 1) % count;

  if (down & (HidNpadButton_Up | HidNpadButton_StickLUp | HidNpadButton_Down |
              HidNpadButton_StickLDown | HidNpadButton_B | HidNpadButton_A | HidNpadButton_Y))
    s_delete_armed = -1;

  if (s_page == Page::Cheats)
  {
    const int count = static_cast<int>(s_cheats.size());
    if (down & HidNpadButton_Y)
    {
      QueueAction({ActionType::AddCheat});
      return;
    }
    if ((down & HidNpadButton_X) && s_selection < count)
    {
      // Twice, so a stray press doesn't lose a code typed by hand
      if (s_delete_armed == s_selection)
      {
        s_delete_armed = -1;
        QueueAction({ActionType::DeleteCheat, s_selection});
      }
      else
      {
        s_delete_armed = s_selection;
      }
      return;
    }
    if ((down & (HidNpadButton_Left | HidNpadButton_Right)) && s_selection < count &&
        s_cheats[s_selection].supported)
    {
      QueueAction({ActionType::ToggleCheat, s_selection});
      return;
    }
  }

  // Left/right flips the Show FPS and aspect ratio options in place, as on NaGa's menu
  if (s_page == Page::Main && s_selection == MAIN_SHOW_FPS &&
      (down & (HidNpadButton_Left | HidNpadButton_Right)))
  {
    QueueAction({ActionType::ToggleFPS});
    return;
  }
  if (s_page == Page::Main && s_selection == MAIN_ASPECT &&
      (down & (HidNpadButton_Left | HidNpadButton_Right)))
  {
    QueueAction({ActionType::CycleAspect, (down & HidNpadButton_Left) ? -1 : 1});
    return;
  }

  if (down & HidNpadButton_B)
  {
    HandleBack();
    return;
  }
  if (down & HidNpadButton_A)
    ActivateSelection();
}

std::vector<Action> TakeActions()
{
  std::lock_guard lock{s_mutex};
  std::vector<Action> result(s_actions.begin(), s_actions.end());
  s_actions.clear();
  return result;
}

bool IsVisible()
{
  std::lock_guard lock{s_mutex};
  return s_visible;
}

void Close()
{
  std::lock_guard lock{s_mutex};
  if (s_visible)
    CloseMenu();
}

bool IsInputCaptured()
{
  std::lock_guard lock{s_mutex};
  return s_visible || s_capture_until_release;
}

bool ShowFPS()
{
  std::lock_guard lock{s_mutex};
  return s_show_fps;
}

void SetShowFPS(bool show)
{
  std::lock_guard lock{s_mutex};
  s_show_fps = show;
}

void SetAspect(std::string name)
{
  std::lock_guard lock{s_mutex};
  s_aspect = std::move(name);
}

void SetStatus(std::string message)
{
  std::lock_guard lock{s_mutex};
  SetStatusLocked(std::move(message));
}

void SetCheats(std::vector<CheatEntry> cheats, std::string file, std::vector<std::string> file_names)
{
  std::lock_guard lock{s_mutex};
  s_cheats = std::move(cheats);
  s_cheat_file = std::move(file);
  s_cheat_file_names = std::move(file_names);
  s_delete_armed = -1;
  if (s_page == Page::Cheats)
    s_selection = std::min(s_selection, static_cast<int>(s_cheats.size()) + 1);
}

std::string CurrentStatus()
{
  std::lock_guard lock{s_mutex};
  if (s_status.empty() || std::chrono::steady_clock::now() >= s_status_until)
    return {};
  return s_status;
}

void ShowAlert(std::string caption, std::string message)
{
  std::lock_guard lock{s_mutex};
  if (!s_initialized)
    return;
  s_alert_caption = std::move(caption);
  s_alert_message = std::move(message);
  SetPage(Page::Alert);
  s_visible = true;
}

void RefreshStateInfo()
{
  std::lock_guard lock{s_mutex};
  RefreshStateInfoLocked();
}

void Draw()
{
  std::lock_guard lock{s_mutex};
  if (!s_initialized || !s_visible)
    return;

  const ImGuiIO& io = ImGui::GetIO();
  ImGui::GetBackgroundDrawList()->AddRectFilled({0.0f, 0.0f}, io.DisplaySize,
                                                IM_COL32(0, 0, 0, 132));
  const float scale = std::clamp(io.DisplaySize.y / 1080.0f, 0.72f, 1.0f);
  ImGui::SetNextWindowPos({io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f}, ImGuiCond_Always,
                          {0.5f, 0.5f});
  ImGui::SetNextWindowSize({880.0f * scale, 760.0f * scale}, ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.96f);

  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 16.0f * scale);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {30.0f * scale, 22.0f * scale});
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {10.0f * scale, 8.0f * scale});
  ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(7, 28, 44, 248));
  ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(71, 190, 235, 255));
  ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(15, 89, 126, 245));
  ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(15, 89, 126, 245));
  ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(15, 89, 126, 245));
  ImGui::PushFont(nullptr, 25.0f * scale);

  constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                     ImGuiWindowFlags_NoResize |
                                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                     ImGuiWindowFlags_NoInputs;
  if (ImGui::Begin("YabaSanshiro quick menu", nullptr, flags))
  {
    switch (s_page)
    {
    case Page::Main:
      CenteredText("Quick Menu");
      break;
    case Page::SaveStates:
      CenteredText("Save State");
      break;
    case Page::LoadStates:
      CenteredText("Load State");
      break;
    case Page::Cheats:
      CenteredText("Cheats");
      break;
    case Page::Disc:
    case Page::DiscBrowser:
      CenteredText("Disc Management");
      break;
    case Page::Alert:
      break;
    }
    ImGui::Separator();

    switch (s_page)
    {
    case Page::Main:
      RenderMainPage();
      ImGui::Separator();
      CenteredText("D-Pad  Navigate     A  Select     B  Close     Minus + Plus  Toggle");
      break;
    case Page::SaveStates:
      RenderStatePage(true);
      break;
    case Page::LoadStates:
      RenderStatePage(false);
      break;
    case Page::Cheats:
      RenderCheatsPage();
      break;
    case Page::Disc:
      RenderDiscPage();
      break;
    case Page::DiscBrowser:
      RenderDiscBrowser();
      break;
    case Page::Alert:
      RenderAlert();
      break;
    }

    if (!s_status.empty() && std::chrono::steady_clock::now() < s_status_until)
    {
      ImGui::SetCursorPosY(18.0f * scale);
      ImGui::SetCursorPosX(20.0f * scale);
      ImGui::TextColored({0.55f, 0.92f, 1.0f, 1.0f}, "%s", s_status.c_str());
    }
  }
  ImGui::End();
  ImGui::PopFont();
  ImGui::PopStyleColor(5);
  ImGui::PopStyleVar(3);
}
}  // namespace DolphinSwitch::RuntimeOverlay
