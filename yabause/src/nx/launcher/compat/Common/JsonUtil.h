// Stand-in for the parts of Dolphin's Common/JsonUtil.h the launcher uses
#pragma once

#include <string>

#include <picojson.h>

#include "Common/FileUtil.h"

inline bool JsonFromFile(const std::string& filename, picojson::value* root, std::string* error)
{
  std::string json_data;
  if (!File::ReadFileToString(filename, json_data))
    return false;
  *error = picojson::parse(*root, json_data);
  return error->empty();
}
