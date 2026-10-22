// The few session string fields the overlay needs, read without a full YAML parser
#pragma once
#include <map>
#include <string>
#include <vector>

#include "estimator.h"

struct DriverEntry {
  int carIdx = -1;
  std::string carPath;
  bool paceCar = false;
  bool spectator = false;
};

struct SessionInfo {
  double trackLength = 0;  // m
  std::string trackName;
  std::string trackDirection;
  int subSessionId = -1;  // changes when the sim moves to another event without restarting
  int driverCarIdx = -1;
  std::vector<DriverEntry> drivers;
};

SessionInfo parseSessionInfo(const std::string& yaml);

// Session string with only track and car fields, no names, account or session ids
std::string formatSessionInfo(const SessionInfo& info);

// Nominal bumper to bumper length for a CarPath, overrides take precedence
float nominalCarLength(const std::string& carPath, const std::map<std::string, float>& overrides);

SessionGeometry makeGeometry(const SessionInfo& info, const std::map<std::string, float>& overrides);
