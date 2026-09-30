// Stand-in for the parts of Dolphin's Common/IOFile.h the launcher uses
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace File
{
class IOFile
{
public:
  IOFile(const std::string& path, const char* mode) : m_file(std::fopen(path.c_str(), mode)) {}
  ~IOFile()
  {
    if (m_file)
      std::fclose(m_file);
  }
  IOFile(const IOFile&) = delete;
  IOFile& operator=(const IOFile&) = delete;

  explicit operator bool() const { return m_file != nullptr; }

  std::uint64_t GetSize() const
  {
    if (!m_file)
      return 0;
    const long position = std::ftell(m_file);
    std::fseek(m_file, 0, SEEK_END);
    const long size = std::ftell(m_file);
    std::fseek(m_file, position, SEEK_SET);
    return size < 0 ? 0 : static_cast<std::uint64_t>(size);
  }

  bool ReadBytes(void* data, std::size_t length)
  {
    return m_file && std::fread(data, 1, length, m_file) == length;
  }

private:
  std::FILE* m_file;
};
}  // namespace File
