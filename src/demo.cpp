#include "demo.h"

#include <algorithm>
#include <cmath>

#include "irsdk.h"

namespace {

constexpr double kTrack = 4000.0;
constexpr float kLength = 4.0f;
constexpr double kCycle = 26.0;

struct Car {
  double x;  // metres ahead of us, NAN when absent
  int side;  // 1 left, 2 right
};

double ramp(double t, double t0, double t1, double a, double b) {
  double u = std::clamp((t - t0) / (t1 - t0), 0.0, 1.0);
  return a + (b - a) * u;
}

// three opponents following a fixed script, sides are ground truth the estimator never sees
void script(double t, Car cars[3]) {
  for (int i = 0; i < 3; ++i) cars[i] = {NAN, 0};
  if (t < 6) cars[0] = {ramp(t, 0, 6, -9, 9), 2};                // passes us on the right
  else if (t < 11) cars[0] = {ramp(t, 6, 11, 7, -9), 1};         // falls back on the left
  else if (t < 15) {                                             // alongside on both sides
    cars[0] = {ramp(t, 11, 15, -7, 1), 1};
    cars[1] = {ramp(t, 11, 15, 6, -1.5), 2};
  } else if (t < 18) {                                           // two cars on the left
    cars[0] = {ramp(t, 15, 18, -6, -2), 1};
    cars[1] = {ramp(t, 15, 18, 7, 2), 1};
  } else if (t < 21) {                                           // two cars arrive together, sides unknowable
    cars[1] = {ramp(t, 18, 21, -2.5, 0.5), 1};
    cars[2] = {ramp(t, 18, 21, -2.4, 0.6), 2};
  }
}

}  // namespace

SessionGeometry demoGeometry() {
  SessionGeometry g;
  g.trackLength = kTrack;
  g.cars.assign(4, CarGeometry{kLength, 0, 0, false});
  return g;
}

bool demoFrame(double t, Frame& f) {
  double u = std::fmod(t, kCycle);
  if (u >= 24) return false;  // feed drops out for the last two seconds
  Car cars[3];
  script(u, cars);
  double me = std::fmod(t * 40.0, kTrack) / kTrack;
  f.sessionTime = t;
  f.sessionNum = 0;
  f.sessionUid = 1;
  f.playerIdx = 0;
  f.isOnTrack = true;
  f.isReplay = false;
  f.speed = f.velX = 40;
  f.velY = f.yawRate = 0;
  f.lapDistPct.assign(4, -1);
  f.trackSurface.assign(4, irsdk::kNotInWorld);
  f.onPitRoad.assign(4, 0);
  f.lapDistPct[0] = (float)me;
  f.trackSurface[0] = irsdk::kOnTrack;
  int left = 0, right = 0;
  for (int i = 0; i < 3; ++i) {
    if (std::isnan(cars[i].x)) continue;
    double p = me + cars[i].x / kTrack;
    f.lapDistPct[i + 1] = (float)(p - std::floor(p));
    f.trackSurface[i + 1] = irsdk::kOnTrack;
    if (std::fabs(cars[i].x) < kLength + 0.25) ++(cars[i].side == 1 ? left : right);
  }
  if (left && right) f.carLeftRight = irsdk::kLRCarLeftRight;
  else if (left) f.carLeftRight = left > 1 ? irsdk::kLR2CarsLeft : irsdk::kLRCarLeft;
  else if (right) f.carLeftRight = right > 1 ? irsdk::kLR2CarsRight : irsdk::kLRCarRight;
  else f.carLeftRight = irsdk::kLRClear;
  return true;
}
