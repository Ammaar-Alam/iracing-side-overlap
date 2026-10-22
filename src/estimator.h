// Side occupancy, target association and fore/aft overlap estimation
//
// The spotter's CarLeftRight decides whether a side is occupied. Lap distance only
// proposes which cars could explain that and where they sit along the track, so a
// bar is positioned only when one assignment clearly dominates and its fore/aft
// uncertainty is small. Everything else falls back to a plain occupied-side warning.
#pragma once
#include <vector>

#include "frame.h"

struct CarGeometry {
  float length = 4.5f;      // nominal bumper to bumper length, m
  float refOffset = 0.0f;   // lap distance reference point ahead of the car centre, m
  int model = -1;           // cars of one model share the same reference point
  bool ignore = false;      // spectators and entries that never drive
};

struct SessionGeometry {
  double trackLength = 0;   // m along the lap distance reference
  std::vector<CarGeometry> cars;  // indexed by CarIdx
};

enum class FilterKind { kRaw, kAlphaBeta, kKalman };

struct EstimatorConfig {
  float padding = 0.25f;           // spotter clearance before calling clear, app.ini carLowHiPadding
  float gateMargin = 3.0f;         // neighbourhood beyond the overlap span considered for association, m
  float edgeSigma = 0.35f;         // softness of the alongside likelihood at the span edge, m
  float pFar = 0.03f;              // overlapping car the spotter ignores, eg beyond its lateral range
  float pPitMismatch = 0.02f;      // one car on pit road and the other not, never called in any capture
  float pPhantom = 0.02f;          // occupied side with no matching car in the neighbourhood
  float pSwitchLocked = 1e-4f;     // car changes side while it has stayed alongside
  float pSwitchFree = 0.2f;        // car changes side after dropping out of overlap
  float minConfidence = 0.9f;      // assignment probability needed for a positioned bar
  float maxSigma = 0.6f;           // fore/aft uncertainty above which a bar falls back, m
  float staleAfter = 0.3f;         // host seconds without a new tick before telemetry is unavailable
  float lateralSeparation = 2.2f;  // nominal side by side centre spacing used for geometry uncertainty, m
  float crossModelSigma = 0.3f;    // unknown reference point difference between car models, m
  float measSigma = 0.05f;         // lap distance measurement noise for raw and alpha-beta, m
  float jumpReset = 3.0f;          // innovation that resets a car's filter, m
  FilterKind filter = FilterKind::kRaw;
  float alpha = 0.5f, beta = 0.1f;
  float accelNoise = 8.0f;         // Kalman relative acceleration noise, m/s^2
  float predict = 0.0f;            // forward prediction horizon, s
  bool replayUsesCamCar = false;   // analysis only: treat the camera car as the reference in replays
};

enum class Feed { kInactive, kUnavailable, kLive };

struct SideView {
  int reported = 0;          // cars the spotter places on this side
  bool positioned = false;   // fore/aft span is backed by an unambiguous, precise estimate
  float rear = 0, front = 0; // occupied span relative to my centre, m, positive ahead
  int carIdx = -1;           // primary assigned car, diagnostics only
  float confidence = 0;      // probability of the displayed assignment
  float sigma = 0;           // fore/aft uncertainty of the span, m
};

struct View {
  Feed feed = Feed::kInactive;
  SideView left, right;
  float myLength = 4.5f;
};

struct CandidateDiag {
  int carIdx;
  float z, x, v, sigma, margin;
  float pLeft, pRight;
};

class Estimator {
 public:
  explicit Estimator(const EstimatorConfig& cfg = {}) : cfg_(cfg) {}
  void setGeometry(const SessionGeometry& g);
  void update(const Frame& f);
  View view(double hostNow) const;
  void reset();

  const EstimatorConfig& config() const { return cfg_; }
  const std::vector<CandidateDiag>& candidates() const { return diag_; }
  int resets() const { return resets_; }
  int referenceCar() const { return me_; }

 private:
  struct CarState {
    bool valid = false;
    double t = 0;              // sim time of the last observation
    float z = 0;               // last raw relative position, m
    float x = 0, v = 0;        // filtered relative position and velocity
    float pxx = 1, pxv = 0, pvv = 25;
    int side = 0;              // remembered side, 0 none, 1 left, 2 right
    bool locked = false;       // has stayed alongside on that side since it was assigned
  };
  struct Cand {
    int car;
    float x, half, sigma, margin;
    float context;  // how plausible it is for the spotter to report this car at all
  };
  struct Hyp {
    int assign[4];   // per candidate: 0 none, 1 left, 2 right
    int phantom[3];  // unmatched cars needed per side
    float score;
  };
  struct Group {
    int mask, phantoms;
    float p;
  };

  void trackCar(int i, float z, double now);
  float geometrySigma(int i, float x, const Frame& f) const;
  void associate(const std::vector<Cand>& cands, int status);
  const CarGeometry& geom(int i) const;

  EstimatorConfig cfg_;
  SessionGeometry geo_;
  std::vector<CarState> cars_;
  std::vector<CandidateDiag> diag_;
  // per tick scratch, kept to reuse its capacity
  std::vector<Cand> cands_;
  std::vector<Hyp> hyps_;
  std::vector<Group> groups_;
  View view_;
  int me_ = -1, lastSessionNum_ = -2, lastSessionUid_ = -2, lastPlayer_ = -2;
  double lastSim_ = 0, lastHost_ = -1e9;
  bool hasLast_ = false;
  int resets_ = 0;
};
