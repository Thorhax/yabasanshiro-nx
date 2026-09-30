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

#include "cheats.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

#include <dirent.h>
#include <strings.h>

#include "config.h"
#include "cheat.h"

namespace nx {
namespace cheats {

namespace {

struct Code {
  int type;
  u32 addr;
  u32 val;
};

std::vector<Cheat> s_cheats;
std::string s_path;                 // file in use, or where a new one goes
std::vector<std::string> s_names;   // file names looked for, best first

std::string directory()
{
  return dataPath("cheats");
}

// Action Replay codes: "1602E8F0 0063", several separated by '+', spaces or new lines.
bool parseCode(const std::string & text, std::vector<Code> * codes, std::string * error)
{
  std::vector<std::string> tokens;
  std::string token;
  for (char c : text) {
    if (std::isxdigit((unsigned char)c)) {
      token += c;
    } else if (!token.empty()) {
      tokens.push_back(token);
      token.clear();
    }
  }
  if (!token.empty())
    tokens.push_back(token);

  codes->clear();
  bool any = false;
  for (size_t i = 0; i < tokens.size(); i++) {
    std::string address, value;
    if (tokens[i].size() == 12) {
      address = tokens[i].substr(0, 8);
      value = tokens[i].substr(8);
    } else if (tokens[i].size() == 8 && i + 1 < tokens.size() && tokens[i + 1].size() <= 4) {
      address = tokens[i];
      value = tokens[++i];
    } else {
      if (error) *error = "\"" + tokens[i] + "\" is not an Action Replay code (XXXXXXXX YYYY)";
      return false;
    }
    any = true;
    const u32 addr = (u32)strtoul(address.c_str(), nullptr, 16);
    const u32 val = (u32)strtoul(value.c_str(), nullptr, 16);
    switch (addr >> 28) {
    case 0x1:
      codes->push_back({CHEATTYPE_WORDWRITE, addr & 0x0FFFFFFF, val & 0xFFFF});
      break;
    case 0x3:
      codes->push_back({CHEATTYPE_BYTEWRITE, addr & 0x0FFFFFFF, val & 0xFF});
      break;
    case 0xD:
      codes->push_back({CHEATTYPE_ENABLE, addr & 0x0FFFFFFF, val & 0xFFFF});
      break;
    case 0xB:
    case 0xF:
      // Master codes hook the real cartridge into the game; not needed here
      break;
    default:
      if (error) *error = "Code type " + address.substr(0, 1) + " (" + address + ") is not supported";
      return false;
    }
  }
  if (!any) {
    if (error) *error = "No code entered";
    return false;
  }
  return true;
}

// Rebuilds the engine's list from the enabled cheats
void apply()
{
  CheatClearCodes();
  int count = 0;
  for (const Cheat & cheat : s_cheats) {
    std::vector<Code> codes;
    if (!cheat.enabled || !parseCode(cheat.code, &codes, nullptr))
      continue;
    for (const Code & code : codes)
      CheatAddCode(code.type, code.addr, code.val);
    count++;
  }
  printf("Cheats: %d enabled\n", count);
}

std::string unquote(std::string value)
{
  while (!value.empty() && std::isspace((unsigned char)value.back()))
    value.pop_back();
  size_t start = 0;
  while (start < value.size() && std::isspace((unsigned char)value[start]))
    start++;
  value = value.substr(start);
  if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
    value = value.substr(1, value.size() - 2);
  return value;
}

// RetroArch's format: cheatN_desc, cheatN_code and cheatN_enable, N counting from 0
bool readFile(const std::string & path)
{
  FILE * fp = fopen(path.c_str(), "r");
  if (!fp)
    return false;
  std::map<int, Cheat> entries;
  char line[2048];
  while (fgets(line, sizeof(line), fp)) {
    int index;
    char key[16];
    int consumed = 0;
    if (sscanf(line, " cheat%d_%15[a-z] =%n", &index, key, &consumed) != 2 || consumed == 0)
      continue;
    const std::string value = unquote(line + consumed);
    Cheat & cheat = entries[index];
    if (!strcmp(key, "desc"))
      cheat.desc = value;
    else if (!strcmp(key, "code"))
      cheat.code = value;
    else if (!strcmp(key, "enable"))
      cheat.enabled = value == "true" || value == "1";
  }
  fclose(fp);

  s_cheats.clear();
  for (auto & entry : entries) {
    Cheat & cheat = entry.second;
    // Newer RetroArch files also hold memory-search cheats, which have no code
    if (cheat.code.empty())
      continue;
    std::vector<Code> codes;
    std::string error;
    cheat.supported = parseCode(cheat.code, &codes, &error);
    if (!cheat.supported) {
      printf("Cheat '%s' not usable: %s\n", cheat.desc.c_str(), error.c_str());
      cheat.enabled = false;
    }
    if (cheat.desc.empty())
      cheat.desc = cheat.code;
    s_cheats.push_back(cheat);
  }
  return true;
}

bool writeFile()
{
  FILE * fp = fopen(s_path.c_str(), "w");
  if (!fp) {
    printf("Could not write %s\n", s_path.c_str());
    return false;
  }
  fprintf(fp, "cheats = %d\n", (int)s_cheats.size());
  for (size_t i = 0; i < s_cheats.size(); i++) {
    std::string desc = s_cheats[i].desc;
    for (char & c : desc)
      if (c == '"') c = '\'';
    fprintf(fp, "\ncheat%d_desc = \"%s\"\n", (int)i, desc.c_str());
    fprintf(fp, "cheat%d_code = \"%s\"\n", (int)i, s_cheats[i].code.c_str());
    fprintf(fp, "cheat%d_enable = %s\n", (int)i, s_cheats[i].enabled ? "true" : "false");
  }
  fclose(fp);
  return true;
}

// The file in 'dir' named 'name', ignoring case (FAT and exFAT don't, but copies from
// libretro-database may differ in case from the disc image)
std::string findFile(const std::string & dir, const std::string & name)
{
  DIR * d = opendir(dir.c_str());
  if (!d)
    return {};
  std::string found;
  while (struct dirent * entry = readdir(d)) {
    if (!strcasecmp(entry->d_name, name.c_str())) {
      found = dir + "/" + entry->d_name;
      break;
    }
  }
  closedir(d);
  return found;
}

}  // namespace

void load(const std::string & game_path, const char * game_code)
{
  unload();

  std::string stem = game_path;
  const size_t slash = stem.find_last_of('/');
  if (slash != std::string::npos)
    stem = stem.substr(slash + 1);
  const size_t dot = stem.find_last_of('.');
  if (dot != std::string::npos && dot > 0)
    stem = stem.substr(0, dot);

  if (game_code && *game_code)
    s_names.push_back(std::string(game_code) + ".cht");
  if (!stem.empty())
    s_names.push_back(stem + ".cht");
  if (s_names.empty())
    return;

  // libretro-database keeps Saturn cheats in a folder of this name
  const std::string dirs[] = {directory(), directory() + "/Sega - Saturn"};
  for (const std::string & name : s_names) {
    for (const std::string & dir : dirs) {
      const std::string path = findFile(dir, name);
      if (!path.empty() && readFile(path)) {
        s_path = path;
        printf("Cheats: %s (%d)\n", path.c_str(), (int)s_cheats.size());
        apply();
        return;
      }
    }
  }
  s_path = directory() + "/" + s_names.front();
  printf("Cheats: none (looked for %s)\n", s_names.front().c_str());
}

void unload()
{
  s_cheats.clear();
  s_path.clear();
  s_names.clear();
  CheatClearCodes();
}

const std::vector<Cheat> & list()
{
  return s_cheats;
}

std::string filePath()
{
  return s_path;
}

std::vector<std::string> fileNames()
{
  return s_names;
}

bool check(const std::string & code, std::string * error)
{
  std::vector<Code> codes;
  return parseCode(code, &codes, error);
}

bool setEnabled(int index, bool enabled)
{
  if (index < 0 || index >= (int)s_cheats.size() || !s_cheats[index].supported)
    return false;
  s_cheats[index].enabled = enabled;
  apply();
  writeFile();
  return true;
}

bool add(const std::string & desc, const std::string & code, std::string * error)
{
  if (s_path.empty()) {
    if (error) *error = "This game has no product code or file name to store cheats under";
    return false;
  }
  std::vector<Code> codes;
  if (!parseCode(code, &codes, error))
    return false;
  Cheat cheat;
  cheat.desc = desc.empty() ? "Cheat " + std::to_string(s_cheats.size() + 1) : desc;
  cheat.code = code;
  cheat.enabled = true;
  s_cheats.push_back(cheat);
  apply();
  if (!writeFile()) {
    if (error) *error = "The cheat is on, but could not be saved to " + s_path;
    return false;
  }
  return true;
}

bool remove(int index)
{
  if (index < 0 || index >= (int)s_cheats.size())
    return false;
  s_cheats.erase(s_cheats.begin() + index);
  apply();
  writeFile();
  return true;
}

}  // namespace cheats
}  // namespace nx
