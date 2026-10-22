// Persistent overlay settings in an ini file next to the executable
#pragma once
#include <climits>
#include <cstdint>
#include <map>
#include <string>

struct Settings {
  int x = INT_MIN, y = INT_MIN;  // window position in physical pixels, INT_MIN means default
  int width = 90, height = 150;  // logical pixels at 96 dpi
  int opacity = 90;              // percent
  uint32_t barColor = 0xFFB300;
  uint32_t referenceColor = 0xFFFFFF;
  uint32_t unavailableColor = 0xA0A0A0;
  int fps = 60;
  std::map<std::string, float> carLengths;  // CarPath -> metres

  void load(const std::wstring& path);
  void save(const std::wstring& path) const;
};

// The iRacing spotter's clearance setting, falls back to its shipped default
float spotterPadding();
