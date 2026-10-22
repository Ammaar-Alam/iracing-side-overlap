// Decodes the telemetry subset the estimator needs from one snapshot row
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "irsdk.h"

struct Frame {
  double hostTime = 0;     // monotonic seconds when the row was acquired
  double sessionTime = 0;  // simulator clock, stops while paused
  int tick = 0;
  int sessionNum = -1;
  int sessionUid = -1;
  int playerIdx = -1;
  int camCarIdx = -1;
  int carLeftRight = -1;   // irsdk::CarLeftRight, -1 when the variable is missing
  bool isOnTrack = false;
  bool isReplay = false;
  float speed = 0;         // m/s
  float yawRate = 0;       // rad/s
  float velX = 0, velY = 0;  // body frame m/s
  std::vector<float> lapDistPct;
  std::vector<int> trackSurface;
  std::vector<uint8_t> onPitRoad;
};

class FrameDecoder {
 public:
  enum VarId {
    kSessionTime, kSessionNum, kSessionUid, kPlayerIdx, kCamCarIdx, kCarLeftRight, kIsOnTrack, kIsReplay,
    kSpeed, kYawRate, kVelX, kVelY, kLapDistPct, kTrackSurface, kOnPitRoad, kVarCount
  };
  struct VarSpec {
    const char* name;
    bool required;
  };
  // every variable decode() reads
  static const VarSpec kVars[kVarCount];

  void bind(const irsdk::Schema& schema);
  void decode(const uint8_t* row, Frame& f) const;
  // names of required variables absent from the bound schema
  const std::vector<std::string>& missing() const { return missing_; }

 private:
  struct Var {
    irsdk::VarHeader h{};
    bool ok = false;
  };
  Var vars_[kVarCount];
  std::vector<std::string> missing_;
};
