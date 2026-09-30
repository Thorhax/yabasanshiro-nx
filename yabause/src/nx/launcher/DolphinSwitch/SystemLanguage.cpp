// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinSwitch/SystemLanguage.h"

#include <switch.h>

#include "Common/ScopeGuard.h"

namespace DolphinSwitch
{
namespace
{
SystemLanguageDefaults MapLanguage(SetLanguage language)
{
  switch (language)
  {
  case SetLanguage_JA:
    return {"Japanese", "ja-JP"};
  case SetLanguage_ENUS:
    return {"English (United States)", "en-US"};
  case SetLanguage_FR:
    return {"French", "fr-FR"};
  case SetLanguage_DE:
    return {"German", "de-DE"};
  case SetLanguage_IT:
    return {"Italian", "it-IT"};
  case SetLanguage_ES:
    return {"Spanish", "es-ES"};
  case SetLanguage_ZHCN:
  case SetLanguage_ZHHANS:
    return {"Chinese (Simplified)", "zh-Hans-CN"};
  case SetLanguage_KO:
    return {"Korean", "ko-KR"};
  case SetLanguage_NL:
    return {"Dutch", "nl-NL"};
  case SetLanguage_PT:
    return {"Portuguese", "pt-PT"};
  case SetLanguage_RU:
    return {"Russian", "ru-RU"};
  case SetLanguage_ZHTW:
  case SetLanguage_ZHHANT:
    return {"Chinese (Traditional)", "zh-Hant-TW"};
  case SetLanguage_ENGB:
    return {"English (United Kingdom)", "en-GB"};
  case SetLanguage_FRCA:
    return {"French (Canada)", "fr-CA"};
  case SetLanguage_ES419:
    return {"Spanish (Latin America)", "es-419"};
  case SetLanguage_PTBR:
    return {"Portuguese (Brazil)", "pt-BR"};
  case SetLanguage_Total:
    break;
  }

  return {};
}

SystemLanguageDefaults QuerySystemLanguage()
{
  if (R_FAILED(setInitialize()))
    return {};
  Common::ScopeGuard set_guard([] { setExit(); });

  u64 language_code = 0;
  SetLanguage language = SetLanguage_ENUS;
  if (R_FAILED(setGetSystemLanguage(&language_code)) ||
      R_FAILED(setMakeLanguage(language_code, &language)))
  {
    return {};
  }

  return MapLanguage(language);
}
}  // namespace

const SystemLanguageDefaults& GetSystemLanguageDefaults()
{
  static const SystemLanguageDefaults defaults = QuerySystemLanguage();
  return defaults;
}

std::vector<std::string> GetSystemPreferredLocales()
{
  return {GetSystemLanguageDefaults().locale};
}

}  // namespace DolphinSwitch
