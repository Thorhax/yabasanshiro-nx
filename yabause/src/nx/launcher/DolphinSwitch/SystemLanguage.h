// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <vector>


namespace DolphinSwitch
{
struct SystemLanguageDefaults
{
  std::string display_name = "English";
  std::string locale = "en-US";
};

const SystemLanguageDefaults& GetSystemLanguageDefaults();

std::vector<std::string> GetSystemPreferredLocales();
}  // namespace DolphinSwitch
