#include "frame.h"

const FrameDecoder::VarSpec FrameDecoder::kVars[kVarCount] = {
    {"SessionTime", true},      {"SessionNum", false},         {"SessionUniqueID", false}, {"PlayerCarIdx", true},
    {"CamCarIdx", false},       {"CarLeftRight", true},        {"IsOnTrack", true},        {"IsReplayPlaying", false},
    {"Speed", false},           {"YawRate", false},            {"VelocityX", false},       {"VelocityY", false},
    {"CarIdxLapDistPct", true}, {"CarIdxTrackSurface", false}, {"CarIdxOnPitRoad", false},
};

void FrameDecoder::bind(const irsdk::Schema& s) {
  missing_.clear();
  for (int k = 0; k < kVarCount; ++k) {
    vars_[k] = {};
    if (const auto* h = s.find(kVars[k].name)) vars_[k] = {*h, true};
    else if (kVars[k].required) missing_.push_back(kVars[k].name);
  }
}

void FrameDecoder::decode(const uint8_t* row, Frame& f) const {
  auto scalar = [&](VarId id, double fallback) {
    return vars_[id].ok ? irsdk::readAsDouble(row, vars_[id].h, 0) : fallback;
  };
  f.sessionTime = scalar(kSessionTime, 0);
  f.sessionNum = (int)scalar(kSessionNum, -1);
  f.sessionUid = (int)scalar(kSessionUid, -1);
  f.playerIdx = (int)scalar(kPlayerIdx, -1);
  f.camCarIdx = (int)scalar(kCamCarIdx, -1);
  f.carLeftRight = (int)scalar(kCarLeftRight, -1);
  f.isOnTrack = scalar(kIsOnTrack, 0) != 0;
  f.isReplay = scalar(kIsReplay, 0) != 0;
  f.speed = (float)scalar(kSpeed, 0);
  f.yawRate = (float)scalar(kYawRate, 0);
  f.velX = (float)scalar(kVelX, 0);
  f.velY = (float)scalar(kVelY, 0);

  const Var& pct = vars_[kLapDistPct];
  const Var& surface = vars_[kTrackSurface];
  const Var& pit = vars_[kOnPitRoad];
  int n = pct.ok ? pct.h.count : 0;
  f.lapDistPct.resize(n);
  f.trackSurface.assign(n, irsdk::kOnTrack);
  f.onPitRoad.assign(n, 0);
  for (int i = 0; i < n; ++i) {
    f.lapDistPct[i] = (float)irsdk::readAsDouble(row, pct.h, i);
    if (surface.ok && i < surface.h.count) f.trackSurface[i] = (int)irsdk::readAsDouble(row, surface.h, i);
    if (pit.ok && i < pit.h.count) f.onPitRoad[i] = irsdk::readAsDouble(row, pit.h, i) != 0;
  }
}
