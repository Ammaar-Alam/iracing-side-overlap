#include "session_info.h"

#include <cstdio>
#include <cstdlib>

namespace {

std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\"");
  size_t b = s.find_last_not_of(" \t\r\"");
  return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// splits "  Key: value" into indent, key and value
bool splitLine(const std::string& line, int& indent, std::string& key, std::string& value) {
  size_t colon = line.find(':');
  if (colon == std::string::npos) return false;
  indent = (int)line.find_first_not_of(' ');
  key = trim(line.substr(0, colon));
  if (!key.empty() && key[0] == '-') key = trim(key.substr(1));
  value = trim(line.substr(colon + 1));
  return true;
}

// published manufacturer lengths of road car counterparts, nominal rather than sim geometry
struct Nominal {
  const char* carPath;
  float length;
};
constexpr Nominal kNominal[] = {
    {"mx5 mx52016", 3.915f},
    {"toyotagr86", 4.265f},
    {"bmwm2g87", 4.580f},
};
constexpr float kDefaultLength = 4.5f;

}  // namespace

SessionInfo parseSessionInfo(const std::string& yaml) {
  SessionInfo info;
  std::string line, section, key, value;
  bool inDrivers = false;
  int indent = 0;
  for (size_t pos = 0; pos < yaml.size();) {
    size_t end = yaml.find('\n', pos);
    if (end == std::string::npos) end = yaml.size();
    line.assign(yaml, pos, end - pos);
    pos = end + 1;
    if (!line.empty() && line[0] != ' ') {
      section = trim(line.substr(0, line.find(':')));
      inDrivers = false;
      continue;
    }
    if (!splitLine(line, indent, key, value)) continue;
    if (section == "WeekendInfo") {
      if (key == "TrackLength") {
        double v = atof(value.c_str());
        info.trackLength = value.find("mi") != std::string::npos ? v * 1609.344 : v * 1000.0;
      } else if (key == "TrackName") {
        info.trackName = value;
      } else if (key == "TrackDirection") {
        info.trackDirection = value;
      } else if (key == "SubSessionID") {
        info.subSessionId = atoi(value.c_str());
      }
    } else if (section == "DriverInfo") {
      bool listItem = line[indent] == '-';
      if (indent == 1 && !listItem) {
        if (key == "DriverCarIdx") info.driverCarIdx = atoi(value.c_str());
        inDrivers = key == "Drivers";
      }
      if (!inDrivers) continue;
      if (listItem && key == "CarIdx") info.drivers.emplace_back().carIdx = atoi(value.c_str());
      if (info.drivers.empty()) continue;
      DriverEntry& d = info.drivers.back();
      if (key == "CarPath") d.carPath = value;
      else if (key == "CarIsPaceCar") d.paceCar = atoi(value.c_str()) != 0;
      else if (key == "IsSpectator") d.spectator = atoi(value.c_str()) != 0;
    }
  }
  return info;
}

float nominalCarLength(const std::string& carPath, const std::map<std::string, float>& overrides) {
  if (auto it = overrides.find(carPath); it != overrides.end() && it->second > 1) return it->second;
  for (const auto& n : kNominal)
    if (carPath == n.carPath) return n.length;
  return kDefaultLength;
}

SessionGeometry makeGeometry(const SessionInfo& info, const std::map<std::string, float>& overrides) {
  SessionGeometry g;
  g.trackLength = info.trackLength;
  std::vector<std::string> models;
  for (const auto& d : info.drivers) {
    if (d.carIdx < 0) continue;
    if ((int)g.cars.size() <= d.carIdx) g.cars.resize(d.carIdx + 1);
    CarGeometry& c = g.cars[d.carIdx];
    c.length = nominalCarLength(d.carPath, overrides);
    c.ignore = d.spectator;
    size_t m = 0;
    while (m < models.size() && models[m] != d.carPath) ++m;
    if (m == models.size()) models.push_back(d.carPath);
    c.model = (int)m;
  }
  return g;
}

std::string formatSessionInfo(const SessionInfo& s) {
  char length[64];
  snprintf(length, sizeof length, "%.4f km", s.trackLength / 1000.0);
  std::string y = "---\nWeekendInfo:\n TrackName: " + s.trackName + "\n TrackLength: " + length +
                  "\n TrackDirection: " + s.trackDirection + "\nDriverInfo:\n DriverCarIdx: " +
                  std::to_string(s.driverCarIdx) + "\n Drivers:\n";
  for (const auto& d : s.drivers) {
    y += " - CarIdx: " + std::to_string(d.carIdx) + "\n   CarPath: " + d.carPath +
         "\n   CarIsPaceCar: " + std::to_string((int)d.paceCar) + "\n   IsSpectator: " + std::to_string((int)d.spectator) +
         "\n";
  }
  return y + "...\n";
}
