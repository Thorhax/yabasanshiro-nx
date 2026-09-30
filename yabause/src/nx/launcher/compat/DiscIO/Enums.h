// Stand-in for the parts of Dolphin's DiscIO/Enums.h the launcher uses. The GameCube/Wii
// platforms are kept so the launcher's generic code compiles unchanged; YabaSanshiro only
// ever reports SaturnDisc.
#pragma once

#include <string>

namespace DiscIO
{
enum class Platform
{
  GameCubeDisc = 0,
  WiiDisc,
  WiiWAD,
  ELFOrDOL,
  Triforce,
  SaturnDisc,
  NumberOfPlatforms
};

enum class Region
{
  NTSC_J = 0,
  NTSC_U = 1,
  PAL = 2,
  Unknown = 3,
  NTSC_K = 4
};

inline bool ShouldHideFromGameList(const std::string&)
{
  return false;
}
}  // namespace DiscIO
