// Stand-in for Dolphin's UICommon/GameFile.h, describing a Saturn disc image. Metadata comes
// from the disc's system header (the first 256 bytes of the data track).
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "DiscIO/Enums.h"

namespace UICommon
{
struct GameCover
{
  std::vector<std::uint8_t> buffer;
  bool empty() const { return buffer.empty(); }
};

class GameFile final
{
public:
  enum class Variant
  {
    ShortAndNotCustom,
    ShortAndPossiblyCustom,
    LongAndNotCustom,
    LongAndPossiblyCustom,
  };

  explicit GameFile(std::string path);

  bool IsValid() const { return m_valid; }
  const std::string& GetFilePath() const { return m_file_path; }
  const std::string& GetFileName() const { return m_file_name; }
  // The file name without its extension: dump sets name files after the game ("Daytona USA -
  // Circuit Edition (Japan)"), which reads better than the header's upper-case title.
  const std::string& GetName(Variant) const { return m_name; }
  // Title from the disc header
  const std::string& GetInternalName() const { return m_internal_name; }
  // Product number, e.g. "GS-9100"
  const std::string& GetGameID() const { return m_game_id; }
  const std::string& GetGameTDBID() const { return m_game_id; }
  const std::string& GetMaker() const { return m_maker; }
  std::uint64_t GetTitleID() const { return 0; }
  std::uint16_t GetRevision() const { return m_revision; }
  // 0-based, like Dolphin
  std::uint8_t GetDiscNumber() const { return m_disc_number; }
  DiscIO::Platform GetPlatform() const { return DiscIO::Platform::SaturnDisc; }
  DiscIO::Region GetRegion() const { return m_region; }
  std::uint64_t GetFileSize() const { return m_file_size; }
  std::int64_t GetModifiedTime() const { return m_modified; }
  std::array<std::uint8_t, 20> GetSyncHash() const { return {}; }
  const GameCover& GetCoverImage() const { return m_cover; }

private:
  std::string m_file_path;
  std::string m_file_name;
  std::string m_name;
  std::string m_internal_name;
  std::string m_game_id;
  std::string m_maker;
  std::uint16_t m_revision = 0;
  std::uint8_t m_disc_number = 0;
  DiscIO::Region m_region = DiscIO::Region::Unknown;
  std::uint64_t m_file_size = 0;
  std::int64_t m_modified = 0;
  GameCover m_cover;
  bool m_valid = false;
};
}  // namespace UICommon
