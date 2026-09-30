// SPDX-License-Identifier: GPL-2.0-or-later
//
// YabaSanshiro settings pages for the launcher: emulation, video and controls, each globally
// (settings.ini / input.ini) and per game (the launcher's per-game settings file, whose keys
// override the global ones when that game starts; see nx/config.cpp and nx/input.cpp).
//
// Included by DolphinSwitch/Launcher.cpp inside its anonymous namespace, so the Launcher
// class and its helpers are in scope. Kept apart from NaGa's file to keep that close to
// upstream.

// ---------------------------------------------------------------------------------------
// Option tables. Keys and defaults must match nx/config.cpp (loadSettings).

struct SaturnChoice
{
  std::string_view value;
  std::string_view label;
};

struct SaturnOption
{
  std::string_view section;
  std::string_view key;
  std::string_view label;
  std::string_view default_value;
  std::span<const SaturnChoice> choices;
  std::string_view description;
};

constexpr SaturnChoice SATURN_ON_OFF[] = {{"on", "On"}, {"off", "Off"}};
constexpr SaturnChoice SATURN_SH2_CORES[] = {{"dynarec", "Dynarec (fast)"},
                                             {"interpreter", "Interpreter (slow)"}};
constexpr SaturnChoice SATURN_FRAME_LIMITS[] = {{"0", "60 Hz"}, {"2", "120 Hz"}, {"1", "Unlimited"}};
constexpr SaturnChoice SATURN_CARTRIDGES[] = {{"0", "None"}, {"6", "1 MB RAM"}, {"7", "4 MB RAM"}};
constexpr SaturnChoice SATURN_SOUND_SYNC[] = {{"0", "Locked to CPU time"}, {"1", "Real time"}};
constexpr SaturnChoice SATURN_RESOLUTIONS[] = {{"0", "Native (screen)"}, {"3", "Original"},
                                               {"2", "2x"},              {"1", "4x"},
                                               {"4", "720p"},            {"5", "1080p"}};
constexpr SaturnChoice SATURN_RBG_RESOLUTIONS[] = {
    {"0", "Original"}, {"1", "2x"}, {"2", "720p"}, {"3", "1080p"}, {"4", "Fit to emulation"}};
constexpr SaturnChoice SATURN_POLYGON_MODES[] = {{"0", "Perspective correction"},
                                                 {"1", "CPU tessellation"},
                                                 {"2", "GPU tessellation"}};
// Values are the renderer's ASPECT_RATE_MODE (ygl.h)
constexpr SaturnChoice SATURN_ASPECTS[] = {
    {"1", "4:3 (TV)"}, {"0", "Square pixels"}, {"2", "16:9"}, {"3", "Stretch"}};

constexpr SaturnOption SATURN_EMULATION_OPTIONS[] = {
    {"emulation", "sh2_core", "CPU core", "dynarec", SATURN_SH2_CORES,
     "How the Saturn's two SH-2 CPUs are emulated. The dynarec translates their code for the "
     "Switch's CPU and is what lets games run at full speed. The interpreter is much slower and "
     "only useful when testing."},
    {"emulation", "sh2_cache", "SH-2 cache", "on", SATURN_ON_OFF,
     "Emulates the SH-2 CPUs' cache. A few games rely on it; it costs a little speed."},
    {"emulation", "frame_limit", "Speed limit", "0", SATURN_FRAME_LIMITS,
     "60 Hz is normal speed. 120 Hz and Unlimited fast-forward."},
    {"emulation", "frame_skip", "Frame skip", "off", SATURN_ON_OFF,
     "Skips drawing frames when emulation falls behind, keeping the game at full speed at the "
     "cost of smoothness."},
    {"emulation", "cartridge", "Expansion cartridge", "0", SATURN_CARTRIDGES,
     "The cartridge in the Saturn's expansion slot. Some Capcom and SNK fighting games need the "
     "1 MB or 4 MB RAM cartridge."},
    {"emulation", "sound_sync", "Sound timing", "0", SATURN_SOUND_SYNC,
     "How the sound CPU keeps pace with the main CPUs. Locked keeps them exactly in step, which "
     "is the most accurate. Real time can help if a game's sound stutters."},
};

constexpr SaturnOption SATURN_VIDEO_OPTIONS[] = {
    {"video", "resolution", "Internal resolution", "0", SATURN_RESOLUTIONS,
     "The resolution games are drawn at. Native uses the screen's resolution; Original is the "
     "Saturn's own. Higher resolutions look sharper and cost GPU time."},
    {"video", "rbg_resolution", "Rotating background", "0", SATURN_RBG_RESOLUTIONS,
     "Resolution of the Saturn's rotating background layer, used for 3D floors, roads and "
     "skies in many games."},
    {"video", "rbg_compute_shader", "Rotating background on GPU", "on", SATURN_ON_OFF,
     "Draws the rotating background layer with a GPU compute shader instead of the CPU. Faster; "
     "turn it off if a game's floors or roads look wrong."},
    {"video", "polygon_mode", "Polygon drawing", "0", SATURN_POLYGON_MODES,
     "How the Saturn's four-cornered polygons are drawn. Perspective correction is fastest; "
     "tessellation splits them up for more accurate textures at a GPU cost."},
    {"video", "aspect", "Aspect ratio", "1", SATURN_ASPECTS,
     "4:3 shows the picture as a TV did, with borders at the sides. Square pixels uses the "
     "game's resolution as is, slightly wider. 16:9 and Stretch widen the picture."},
    {"video", "vsync", "VSync", "off", SATURN_ON_OFF,
     "Waits for the display to refresh before showing a frame. The emulator already paces itself "
     "to 60 Hz, so this is off by default."},
    {"video", "sync_render", "Sync rendering", "on", SATURN_ON_OFF,
     "Holds emulation until each frame's background layers are drawn, so a game changing them "
     "early in the next frame can't disturb the picture. Slightly slower; turn it off only when "
     "testing."},
};

// Display order for the controls page (the stored order is PERPAD_*)
constexpr int SATURN_BUTTON_ORDER[] = {0, 2, 3, 1, 7, 8, 9, 10, 11, 12, 5, 4, 6};


std::string NormalizeChoice(const SaturnOption& option, std::string_view raw)
{
  const std::string value = Lower(Trim(std::string(raw)));
  if (option.choices.data() == SATURN_ON_OFF)
  {
    if (value == "on" || value == "true" || value == "1" || value == "yes")
      return "on";
    if (value == "off" || value == "false" || value == "0" || value == "no")
      return "off";
  }
  for (const SaturnChoice& choice : option.choices)
  {
    if (choice.value == value)
      return value;
  }
  return std::string(option.default_value);
}

int ChoiceIndex(const SaturnOption& option, std::string_view value)
{
  for (std::size_t index = 0; index < option.choices.size(); ++index)
  {
    if (option.choices[index].value == value)
      return static_cast<int>(index);
  }
  return 0;
}

std::string_view ChoiceLabel(const SaturnOption& option, std::string_view value)
{
  return option.choices[ChoiceIndex(option, value)].label;
}

std::optional<SettingHelpInfo> SaturnSettingHelp(std::string_view label)
{
  for (const auto options : {std::span<const SaturnOption>(SATURN_EMULATION_OPTIONS),
                             std::span<const SaturnOption>(SATURN_VIDEO_OPTIONS)})
  {
    for (const SaturnOption& option : options)
    {
      if (option.label == label)
        return SettingHelpInfo{option.section == "video" ? "Video setting" : "Emulation setting",
                               std::string(option.description)};
    }
  }
  if (label == "Emulation")
    return SettingHelpInfo{"Settings group",
                           "CPU core, speed limit, expansion cartridge and sound timing."};
  if (label == "Video")
    return SettingHelpInfo{"Settings group",
                           "Resolution, the rotating background layer, polygons, aspect ratio "
                           "and frame pacing."};
  if (label == "Controls")
    return SettingHelpInfo{"Settings group",
                           "Which Switch buttons press each Saturn pad button, for players 1 "
                           "and 2."};
  if (label == "Player 1" || label == "Player 2")
    return SettingHelpInfo{"Controller",
                           "Opens this player's Saturn pad mapping. Player 1 also accepts the "
                           "handheld Joy-Con."};
  if (label == "Clear game settings")
    return SettingHelpInfo{"Management action",
                           "Removes this game's own settings and controls, so it uses the "
                           "global ones again."};
  return std::nullopt;
}

// ---------------------------------------------------------------------------------------
// Global settings files

std::string SaturnGlobalPath(bool input)
{
  return std::string(DATA_DIRECTORY) + (input ? "/input.ini" : "/settings.ini");
}

Common::IniFile& Launcher::SaturnGlobalFile(bool input)
{
  Common::IniFile& file = input ? m_saturn_input : m_saturn_settings;
  bool& loaded = input ? m_saturn_input_loaded : m_saturn_settings_loaded;
  if (!loaded)
  {
    file.Load(SaturnGlobalPath(input));
    loaded = true;
  }
  return file;
}

std::optional<std::string> Launcher::GetSaturnGlobal(bool input, std::string_view section,
                                                     std::string_view key)
{
  const Common::IniFile::Section* ini_section = SaturnGlobalFile(input).GetSection(section);
  std::string value;
  if (!ini_section || !ini_section->Get(key, &value))
    return std::nullopt;
  return value;
}

void Launcher::SetSaturnGlobal(bool input, std::string_view section, std::string_view key,
                               const std::optional<std::string>& value)
{
  Common::IniFile& file = SaturnGlobalFile(input);
  if (value)
    file.GetOrCreateSection(section)->Set(std::string(key), *value);
  else
    file.DeleteKey(section, key);
  if (!file.Save(SaturnGlobalPath(input)))
    Toast("The settings could not be saved", 1200);
}

// ---------------------------------------------------------------------------------------
// Emulation and video

void Launcher::SaturnOptionsPage(std::string_view title, std::span<const SaturnOption> options,
                                 Game* game)
{
  const bool per_game = game != nullptr;
  const auto global_value = [&](const SaturnOption& option) {
    const std::optional<std::string> raw = GetSaturnGlobal(false, option.section, option.key);
    return raw ? NormalizeChoice(option, *raw) : std::string(option.default_value);
  };
  const auto game_value = [&](const SaturnOption& option) -> std::optional<std::string> {
    if (!per_game)
      return std::nullopt;
    const std::optional<std::string> raw = GetGameSetting(*game, option.section, option.key);
    return raw ? std::optional<std::string>{NormalizeChoice(option, *raw)} : std::nullopt;
  };
  const auto after_game_change = [&] {
    game->has_game_config = RegularFileExists(GameIniPath(*game));
  };

  RunRows(
      title, per_game ? game->title : std::string{},
      [&] {
        std::vector<Row> rows;
        for (const SaturnOption& option : options)
        {
          const std::optional<std::string> local = game_value(option);
          const std::string_view global_label = ChoiceLabel(option, global_value(option));
          rows.push_back({std::string(option.label),
                          local ? std::string(ChoiceLabel(option, *local)) :
                          per_game ? GlobalValueLabel(global_label) :
                                     std::string(global_label)});
        }
        return rows;
      },
      [&](int index, int delta) {
        const SaturnOption& option = options[index];
        const std::string global = global_value(option);
        const std::optional<std::string> local = game_value(option);

        // Per game, the first choice is "use the global value"
        std::vector<std::string> choices;
        if (per_game)
          choices.push_back(UseGlobalValueLabel(ChoiceLabel(option, global)));
        for (const SaturnChoice& choice : option.choices)
          choices.emplace_back(choice.label);
        const int offset = per_game ? 1 : 0;
        int selected = per_game ? (local ? ChoiceIndex(option, *local) + 1 : 0) :
                                  ChoiceIndex(option, global);

        if (delta == 0)
          selected = Dropdown(option.label, choices, selected);
        else
          selected = (selected + (delta < 0 ? -1 : 1) + static_cast<int>(choices.size())) %
                     static_cast<int>(choices.size());
        if (selected < 0)
          return false;

        if (per_game)
        {
          SetGameSetting(*game, option.section, option.key,
                         selected == 0 ? std::nullopt :
                                         std::optional<std::string>{std::string(
                                             option.choices[selected - offset].value)});
          after_game_change();
        }
        else
        {
          SetSaturnGlobal(false, option.section, option.key,
                          std::string(option.choices[selected].value));
        }
        return false;
      },
      false,
      [&](int index) {
        const SaturnOption& option = options[index];
        if (per_game)
        {
          SetGameSetting(*game, option.section, option.key, std::nullopt);
          after_game_change();
        }
        else
        {
          SetSaturnGlobal(false, option.section, option.key, std::string(option.default_value));
        }
        return true;
      },
      [](int) { return true; });
}

// ---------------------------------------------------------------------------------------
// Controls

std::string SaturnPlayerSection(int player)
{
  return "player" + std::to_string(player + 1);
}

// "DUP LS_UP" -> "D-Pad up, Left stick up"
std::string BindingLabel(std::string_view binding)
{
  std::string label;
  std::size_t position = 0;
  while (position < binding.size())
  {
    const std::size_t start = binding.find_first_not_of(" \t", position);
    if (start == std::string_view::npos)
      break;
    const std::size_t end = std::min(binding.find_first_of(" \t", start), binding.size());
    if (!label.empty())
      label += ", ";
    label += nx::switchButtonLabel(binding.substr(start, end - start));
    position = end;
  }
  return label.empty() ? std::string("Not bound") : label;
}

// The Switch button SDL reports, named as input.ini names it. SDL uses Xbox positions:
// its "B" is the Switch's A (right), its "A" the Switch's B (bottom), and so on.
std::optional<std::string_view> SaturnNameForButton(Uint8 button)
{
  switch (button)
  {
  case SDL_CONTROLLER_BUTTON_B:
    return "A";
  case SDL_CONTROLLER_BUTTON_A:
    return "B";
  case SDL_CONTROLLER_BUTTON_Y:
    return "X";
  case SDL_CONTROLLER_BUTTON_X:
    return "Y";
  case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
    return "L";
  case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
    return "R";
  case SDL_CONTROLLER_BUTTON_START:
    return "PLUS";
  case SDL_CONTROLLER_BUTTON_BACK:
    return "MINUS";
  case SDL_CONTROLLER_BUTTON_LEFTSTICK:
    return "LSTICK";
  case SDL_CONTROLLER_BUTTON_RIGHTSTICK:
    return "RSTICK";
  case SDL_CONTROLLER_BUTTON_DPAD_UP:
    return "DUP";
  case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
    return "DDOWN";
  case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
    return "DLEFT";
  case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
    return "DRIGHT";
  default:
    return std::nullopt;
  }
}

std::optional<std::string_view> SaturnNameForAxis(Uint8 axis, Sint16 value)
{
  if (std::abs(static_cast<int>(value)) < 20000)
    return std::nullopt;
  switch (axis)
  {
  case SDL_CONTROLLER_AXIS_LEFTX:
    return value < 0 ? "LS_LEFT" : "LS_RIGHT";
  case SDL_CONTROLLER_AXIS_LEFTY:
    return value < 0 ? "LS_UP" : "LS_DOWN";
  case SDL_CONTROLLER_AXIS_RIGHTX:
    return value < 0 ? "RS_LEFT" : "RS_RIGHT";
  case SDL_CONTROLLER_AXIS_RIGHTY:
    return value < 0 ? "RS_UP" : "RS_DOWN";
  case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
    return "ZL";
  case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
    return "ZR";
  default:
    return std::nullopt;
  }
}

// Adapted from NaGa's RenderControllerCapture
void Launcher::RenderSaturnCapture(std::string_view label, int position, int count,
                                   bool releasing, std::string_view current,
                                   std::string_view status)
{
  ClearBackground();
  DrawHeader("Control mapping", "Press-to-bind");
  const int panel_width = std::min(900, m_width - 180);
  const int panel_height = 330;
  const int panel_x = (m_width - panel_width) / 2;
  const int panel_y = (m_height - panel_height) / 2 - 20;
  GlassPanel(panel_x, panel_y, panel_width, panel_height);
  DrawTextCentered(m_font_small, m_width / 2, panel_y + 36,
                   std::string(m_localization.Translate("Control")) + " " +
                       std::to_string(position + 1) + " " +
                       std::string(m_localization.Translate("of")) + " " + std::to_string(count),
                   m_dim);
  DrawTextCentered(m_font_large, m_width / 2, panel_y + 94,
                   std::string(m_localization.Translate("Saturn")) + " " + std::string(label),
                   m_value);
  DrawTextCentered(m_font, m_width / 2, panel_y + 180,
                   m_localization.Translate(releasing ?
                                                "Release the current control" :
                                                "Press a button, trigger or stick direction"),
                   m_text);
  const std::string current_line =
      std::string(m_localization.Translate("Current: ")) + std::string(current);
  DrawTextCentered(m_font_small, m_width / 2, panel_y + 238,
                   status.empty() ? current_line : std::string(m_localization.Translate(status)),
                   status.empty() ? m_dim : m_highlight);
  constexpr int hint_band = 100;
  const int hint_step = FontHeight(m_font_small) + 10;
  const int hint_y =
      m_height - hint_band + (hint_band - hint_step - FontHeight(m_font_small)) / 2;
  DrawFooterBand(hint_band);
  DrawTextCentered(m_font_small, m_width / 2, hint_y,
                   m_localization.Translate("Hold Plus to clear       Hold Minus to cancel"),
                   m_dim);
  DrawTextCentered(m_font_small, m_width / 2, hint_y + hint_step,
                   m_localization.Translate("Touch left to clear       Touch right to cancel"),
                   m_dim);
  SDL_RenderPresent(m_renderer);
}

// Adapted from NaGa's CaptureControllerInput. Returns the Switch button name, "" to clear the
// binding, or nothing when cancelled.
std::optional<std::string> Launcher::CaptureSaturnButton(std::string_view label, int position,
                                                         int count, std::string_view current)
{
  // Wait for the button that opened the capture to be released
  while (BeginFrame())
  {
    SDL_GameControllerUpdate();
    bool held = false;
    if (m_controller && SDL_GameControllerGetAttached(m_controller))
    {
      for (int button = 0; button < SDL_CONTROLLER_BUTTON_MAX; ++button)
        held |= SDL_GameControllerGetButton(m_controller,
                                            static_cast<SDL_GameControllerButton>(button)) != 0;
      for (int axis = 0; axis < SDL_CONTROLLER_AXIS_MAX; ++axis)
        held |= std::abs(static_cast<int>(SDL_GameControllerGetAxis(
                    m_controller, static_cast<SDL_GameControllerAxis>(axis)))) > 16000;
    }
    SDL_Event event{};
    while (PollEvent(&event))
    {
      int touch_x = 0;
      int touch_y = 0;
      if (FeedTouch(event, &touch_x, &touch_y) == TouchKind::Tap && touch_y >= m_height - 100)
        return std::nullopt;
      if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE)
        return std::nullopt;
    }
    if (!held)
      break;
    RenderSaturnCapture(label, position, count, true, current);
    WaitForNextFrame();
  }

  // Plus and Minus can be bound too: a tap binds them, holding them clears or cancels
  int held_special = -1;
  Uint32 held_since = 0;
  while (BeginFrame())
  {
    if (!m_controller || !SDL_GameControllerGetAttached(m_controller))
      return std::nullopt;
    SDL_Event event{};
    while (PollEvent(&event))
    {
      int touch_x = 0;
      int touch_y = 0;
      const TouchKind touch = FeedTouch(event, &touch_x, &touch_y);
      if (touch == TouchKind::Tap && touch_y >= m_height - 100)
        return touch_x < m_width / 2 ? std::optional<std::string>{std::string{}} : std::nullopt;
      if (event.type == SDL_CONTROLLERBUTTONDOWN)
      {
        if (event.cbutton.button == SDL_CONTROLLER_BUTTON_START ||
            event.cbutton.button == SDL_CONTROLLER_BUTTON_BACK)
        {
          held_special = event.cbutton.button;
          held_since = SDL_GetTicks();
        }
        else if (const auto name = SaturnNameForButton(event.cbutton.button))
        {
          return std::string(*name);
        }
      }
      else if (event.type == SDL_CONTROLLERBUTTONUP && event.cbutton.button == held_special)
      {
        return std::string(*SaturnNameForButton(static_cast<Uint8>(held_special)));
      }
      else if (event.type == SDL_CONTROLLERAXISMOTION)
      {
        if (const auto name = SaturnNameForAxis(event.caxis.axis, event.caxis.value))
          return std::string(*name);
      }
      else if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE)
      {
        return std::nullopt;
      }
    }
    std::string status;
    if (held_special >= 0)
    {
      const Uint32 held_for = SDL_GetTicks() - held_since;
      if (held_for >= 800)
      {
        if (held_special == SDL_CONTROLLER_BUTTON_START)
          return std::string{};
        return std::nullopt;
      }
      status = held_special == SDL_CONTROLLER_BUTTON_START ? "Keep holding Plus to clear" :
                                                             "Keep holding Minus to cancel";
    }
    RenderSaturnCapture(label, position, count, false, current, status);
    WaitForNextFrame();
  }
  return std::nullopt;
}

// Adapted from NaGa's ControllerMappingSettings
void Launcher::SaturnMappingPage(int player, Game* game)
{
  const bool per_game = game != nullptr;
  const std::string section = SaturnPlayerSection(player);
  const std::string title = per_game ? "Game controls" : "Controls";
  const std::string context = std::string(m_localization.Translate("Player")) + " " +
                              std::to_string(player + 1) +
                              (per_game ? " - " + game->title : std::string{});
  constexpr int count = static_cast<int>(std::size(SATURN_BUTTON_ORDER));

  const auto button_at = [](int index) -> const nx::SaturnButton& {
    return nx::kSaturnButtonInfo[SATURN_BUTTON_ORDER[index]];
  };
  const auto global_binding = [&](const nx::SaturnButton& button) {
    const std::optional<std::string> value = GetSaturnGlobal(true, section, button.key);
    return value ? *value : std::string(button.defaults);
  };
  const auto game_binding = [&](const nx::SaturnButton& button) -> std::optional<std::string> {
    return per_game ? GetGameSetting(*game, section, button.key) : std::nullopt;
  };
  const auto effective_binding = [&](const nx::SaturnButton& button) {
    const std::optional<std::string> local = game_binding(button);
    return local ? *local : global_binding(button);
  };
  const auto store = [&](const nx::SaturnButton& button, const std::optional<std::string>& value) {
    if (per_game)
    {
      SetGameSetting(*game, section, button.key, value);
      game->has_game_config = RegularFileExists(GameIniPath(*game));
    }
    else
    {
      SetSaturnGlobal(true, section, button.key,
                      value ? value : std::optional<std::string>{std::string(button.defaults)});
    }
  };

  auto& saved = m_row_positions[title + '\n' + context];
  int selection = std::clamp(saved.first, 0, count - 1);
  int top = std::max(0, saved.second);
  const int row_height = SettingsRowHeight();
  const int list_top = SettingsListY();
  const int visible = std::max(1, (m_height - list_top - SettingsFooterReserve()) / row_height);
  const auto finish = [&] { saved = {selection, top}; };

  // 'add' keeps the current buttons and adds the pressed one
  const auto assign_selected = [&](bool add) {
    if (!m_controller || !SDL_GameControllerGetAttached(m_controller))
    {
      RenderMessage("Control mapping", std::array<std::string, 1>{"No controller is connected."},
                    true);
      BeginScreenFx();
      return;
    }
    const nx::SaturnButton& button = button_at(selection);
    const std::string current = effective_binding(button);
    const std::optional<std::string> pressed =
        CaptureSaturnButton(button.label, selection, count, BindingLabel(current));
    if (pressed)
    {
      std::string binding = *pressed;
      if (add && !pressed->empty())
      {
        const std::string padded = " " + current + " ";
        binding = padded.find(" " + *pressed + " ") != std::string::npos ?
                      current :
                      Trim(current + " " + *pressed);
      }
      store(button, binding);
      Toast(binding.empty() ? "Control cleared" : "Control assigned", 650);
    }
    BeginScreenFx();
  };
  const auto reset_selected = [&] {
    const nx::SaturnButton& button = button_at(selection);
    store(button, std::nullopt);
    Toast(per_game ? "Control uses the global mapping" : "Setting reset to default", 650);
  };
  const auto show_selected_info = [&] {
    const nx::SaturnButton& button = button_at(selection);
    ShowInfoCard(title, std::string("Saturn ") + std::string(button.label), "Controller mapping",
                 "Which Switch buttons press this Saturn button. Press A, then the Switch button, "
                 "trigger or stick direction to use; Plus adds another one instead of replacing. "
                 "While binding, hold Plus to clear or Minus to cancel. Y resets the control.",
                 BindingLabel(effective_binding(button)),
                 per_game ? "Per-game setting" : "Global setting", true, false);
    BeginScreenFx();
  };

  BeginScreenFx();
  while (BeginFrame())
  {
    SDL_Event event{};
    while (PollEvent(&event))
    {
      int touch_x = 0;
      int touch_y = 0;
      const TouchKind touch = FeedTouch(event, &touch_x, &touch_y);
      if (TouchScrollList(touch, &selection, &top, count, visible))
        continue;
      if (touch == TouchKind::Tap)
      {
        if (touch_y < TopBarHeight() || touch_y >= m_height - 40)
        {
          finish();
          return;
        }
        const int column_width = std::min(980, m_width - 180);
        const int column_x = (m_width - column_width) / 2;
        for (int row = 0; row < visible && top + row < count; ++row)
        {
          const int y = list_top + row * row_height;
          if (touch_x >= column_x && touch_x < column_x + column_width && touch_y >= y &&
              touch_y < y + row_height)
          {
            selection = top + row;
            assign_selected(false);
            break;
          }
        }
        continue;
      }
      const int direction = EventNavigation(event);
      if (direction)
        selection = (selection + direction + count) % count;
      if (event.type == SDL_CONTROLLERBUTTONDOWN)
      {
        if (event.cbutton.button == BUTTON_CONFIRM)
          assign_selected(false);
        else if (event.cbutton.button == SDL_CONTROLLER_BUTTON_START)
          assign_selected(true);
        else if (event.cbutton.button == SDL_CONTROLLER_BUTTON_X)
          reset_selected();
        else if (event.cbutton.button == BUTTON_SETTINGS)
          show_selected_info();
        else if (event.cbutton.button == BUTTON_CANCEL)
        {
          finish();
          return;
        }
      }
      else if (event.type == SDL_KEYDOWN)
      {
        if (event.key.keysym.sym == SDLK_RETURN)
          assign_selected(false);
        else if (event.key.keysym.sym == SDLK_x)
          show_selected_info();
        else if (event.key.keysym.sym == SDLK_ESCAPE)
        {
          finish();
          return;
        }
      }
      if (selection < top)
        top = selection;
      if (selection >= top + visible)
        top = selection - visible + 1;
    }

    ClearBackground();
    DrawHeader(title, context);
    const int column_width = std::min(980, m_width - 180);
    const int column_x = (m_width - column_width) / 2;
    const int label_x = column_x + 40;
    const int value_x = column_x + column_width - 40;
    GlassPanel(column_x - 12, list_top - 10, column_width + 24,
               std::min(visible, count - top) * row_height + 18);
    const float target = static_cast<float>(list_top + (selection - top) * row_height + 1);
    m_highlight_y = (!m_animations || m_highlight_y < 0.0f) ?
                        target :
                        m_highlight_y + (target - m_highlight_y) * 0.30f;
    DrawRowHighlight(column_x, static_cast<int>(m_highlight_y), column_width, row_height - 2);
    for (int row = 0; row < visible && top + row < count; ++row)
    {
      const int index = top + row;
      const bool current = index == selection;
      const nx::SaturnButton& button = button_at(index);
      const std::optional<std::string> local = game_binding(button);
      const std::string value = local ? BindingLabel(*local) :
                                per_game ? GlobalValueLabel(BindingLabel(global_binding(button))) :
                                           BindingLabel(global_binding(button));
      DrawSettingsRowText(std::string("Saturn ") + std::string(button.label), value,
                          list_top + row * row_height, column_width, label_x, value_x, current,
                          current ? m_value : m_text, current ? m_value : m_dim, false,
                          row_height);
    }
    if (count > visible)
    {
      const int track_height = visible * row_height;
      const int track_x = column_x + column_width + 16;
      const int track_y = list_top - 2;
      FillRect(track_x, track_y, 4, track_height, SDL_Color{40, 44, 54, 255});
      const int thumb_height = track_height * visible / count;
      const int denominator = std::max(1, count - visible);
      FillRect(track_x, track_y + (track_height - thumb_height) * top / denominator, 4,
               thumb_height, m_selection);
    }
    DrawSettingsFooter("A  Assign     +  Add     Y  Reset     X  Info     B  Back");
    DrawFadeIn();
    SDL_RenderPresent(m_renderer);
    WaitForNextFrame();
  }
  finish();
}

void Launcher::SaturnControlsRoot(Game* game)
{
  RunRows(
      game ? "Game controls" : "Controls", game ? game->title : std::string{},
      [&] {
        return std::vector<Row>{
            Row{"Player 1", ">", true, false, false},
            Row{"Player 2", ">", true, false, false},
        };
      },
      [&](int index, int) {
        SaturnMappingPage(index, game);
        BeginScreenFx();
        return false;
      },
      true);
}

// ---------------------------------------------------------------------------------------
// Per-game settings root ("Game settings" in the game menu)

void Launcher::SaturnGameSettingsRoot(Game* game)
{
  if (!game)
    return;
  if (game->game_id.empty() && game->config_override_path.empty())
  {
    RenderMessage("Game settings",
                  std::array<std::string, 1>{
                      "This game's disc has no product number, so it can't have its own "
                      "settings."},
                  true);
    return;
  }
  RunRows(
      "Game settings", game->title,
      [&] {
        return std::vector<Row>{
            Row{"Emulation", ">", true, false, false},
            Row{"Video", ">", true, false, false},
            Row{"Controls", ">", true, false, false},
        };
      },
      [&](int index, int) {
        if (index == 0)
          SaturnOptionsPage("Game emulation", SATURN_EMULATION_OPTIONS, game);
        else if (index == 1)
          SaturnOptionsPage("Game video", SATURN_VIDEO_OPTIONS, game);
        else
          SaturnControlsRoot(game);
        BeginScreenFx();
        return false;
      },
      true);
}
