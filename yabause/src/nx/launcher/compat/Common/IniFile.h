// Stand-in for the parts of Dolphin's Common/IniFile.h the launcher uses
#pragma once

#include <list>
#include <map>
#include <string>
#include <string_view>

namespace Common
{
class IniFile
{
public:
  class Section
  {
  public:
    using SectionMap = std::map<std::string, std::string, std::less<>>;

    explicit Section(std::string name) : m_name(std::move(name)) {}

    const std::string& GetName() const { return m_name; }
    bool Exists(std::string_view key) const { return m_values.find(key) != m_values.end(); }
    bool Get(std::string_view key, std::string* value,
             const std::string& default_value = {}) const;
    void Set(const std::string& key, std::string value);
    bool Delete(std::string_view key);
    const SectionMap& GetValues() const { return m_values; }

  private:
    friend class IniFile;
    std::string m_name;
    SectionMap m_values;
    std::list<std::string> m_order;  // keys in file order, so saving keeps the layout
  };

  bool Load(const std::string& path);
  bool Save(const std::string& path);

  const Section* GetSection(std::string_view name) const;
  Section* GetSection(std::string_view name);
  Section* GetOrCreateSection(std::string_view name);
  bool DeleteKey(std::string_view section, std::string_view key);

private:
  std::list<Section> m_sections;
};
}  // namespace Common
