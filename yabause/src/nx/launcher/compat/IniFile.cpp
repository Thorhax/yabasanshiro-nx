// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/IniFile.h"

#include <algorithm>
#include <fstream>
#include <utility>

namespace Common
{
namespace
{
std::string Trim(const std::string& text)
{
  const std::size_t begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos)
    return {};
  return text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
}
}  // namespace

bool IniFile::Section::Get(std::string_view key, std::string* value,
                           const std::string& default_value) const
{
  const auto found = m_values.find(key);
  if (found == m_values.end())
  {
    if (value)
      *value = default_value;
    return false;
  }
  if (value)
    *value = found->second;
  return true;
}

void IniFile::Section::Set(const std::string& key, std::string value)
{
  if (!Exists(key))
    m_order.push_back(key);
  m_values.insert_or_assign(key, std::move(value));
}

bool IniFile::Section::Delete(std::string_view key)
{
  const auto found = m_values.find(key);
  if (found == m_values.end())
    return false;
  m_order.remove(found->first);
  m_values.erase(found);
  return true;
}

bool IniFile::Load(const std::string& path)
{
  m_sections.clear();
  std::ifstream file(path);
  if (!file)
    return false;
  Section* section = nullptr;
  std::string line;
  while (std::getline(file, line))
  {
    line = Trim(line);
    if (line.empty() || line.front() == '#' || line.front() == ';')
      continue;
    if (line.front() == '[' && line.back() == ']')
    {
      section = GetOrCreateSection(Trim(line.substr(1, line.size() - 2)));
      continue;
    }
    const std::size_t equals = line.find('=');
    if (section && equals != std::string::npos)
      section->Set(Trim(line.substr(0, equals)), Trim(line.substr(equals + 1)));
  }
  return true;
}

bool IniFile::Save(const std::string& path)
{
  const std::string temporary = path + ".tmp";
  {
    std::ofstream file(temporary, std::ios::trunc);
    if (!file)
      return false;
    for (const Section& section : m_sections)
    {
      if (section.m_values.empty())
        continue;
      file << '[' << section.m_name << "]\n";
      for (const std::string& key : section.m_order)
        file << key << " = " << section.m_values.at(key) << '\n';
    }
    if (!file)
      return false;
  }
  std::remove(path.c_str());
  return std::rename(temporary.c_str(), path.c_str()) == 0;
}

const IniFile::Section* IniFile::GetSection(std::string_view name) const
{
  const auto found = std::find_if(m_sections.begin(), m_sections.end(),
                                  [&](const Section& section) { return section.m_name == name; });
  return found == m_sections.end() ? nullptr : &*found;
}

IniFile::Section* IniFile::GetSection(std::string_view name)
{
  return const_cast<Section*>(std::as_const(*this).GetSection(name));
}

IniFile::Section* IniFile::GetOrCreateSection(std::string_view name)
{
  if (Section* section = GetSection(name))
    return section;
  return &m_sections.emplace_back(std::string(name));
}

bool IniFile::DeleteKey(std::string_view section_name, std::string_view key)
{
  Section* section = GetSection(section_name);
  return section && section->Delete(key);
}
}  // namespace Common
