#include "estimator.h"

#include <algorithm>
#include <cmath>

#include "irsdk.h"

namespace {

constexpr int kMaxCands = 4;
constexpr float kNeighbourhood = 60.0f;  // cars further along the track are not tracked, m
constexpr float kUnplaceable = 99.0f;    // sigma for cars whose orientation relative to us is unknown

struct Occupancy {
  int minL, maxL, minR, maxR;
};

Occupancy occupancyFor(int status) {
  switch (status) {
    case irsdk::kLRCarLeft: return {1, 1, 0, 0};
    case irsdk::kLRCarRight: return {0, 0, 1, 1};
    case irsdk::kLRCarLeftRight: return {1, 3, 1, 3};
    case irsdk::kLR2CarsLeft: return {2, 3, 0, 0};
    case irsdk::kLR2CarsRight: return {0, 0, 2, 3};
    default: return {0, 0, 0, 0};
  }
}

float sigmoid(float t) { return 1.0f / (1.0f + std::exp(-t)); }

// -1 marks a car outside the world, values just past 1 occur at the line and wrap like any other
bool validPct(float p) { return p > -0.5f && p < 1.5f; }

}  // namespace

void Estimator::setGeometry(const SessionGeometry& g) {
  geo_ = g;
  if (cars_.size() < geo_.cars.size()) cars_.resize(geo_.cars.size());
}

void Estimator::reset() {
  for (auto& c : cars_) c = {};
  diag_.clear();
  hasLast_ = false;
  ++resets_;
}

const CarGeometry& Estimator::geom(int i) const {
  static const CarGeometry fallback;
  return i >= 0 && i < (int)geo_.cars.size() ? geo_.cars[i] : fallback;
}

void Estimator::trackCar(int i, float z, double now) {
  CarState& s = cars_[i];
  double dt = now - s.t;
  if (!s.valid || dt < 0 || dt > 0.5) {
    int side = s.side;
    s = {};
    s.valid = true;
    s.side = side;
    s.t = now;
    s.x = s.z = z;
    return;
  }
  s.t = now;
  if (dt > 0) {
    float xp = s.x + s.v * (float)dt;
    if (std::fabs(z - xp) > cfg_.jumpReset) {
      // a discontinuity such as a tow, reset or network correction restarts the track
      s.valid = false;
      s.locked = false;
      trackCar(i, z, now);
      return;
    }
    switch (cfg_.filter) {
      case FilterKind::kRaw:
        s.v = (z - s.z) / (float)dt;
        s.x = z;
        break;
      case FilterKind::kAlphaBeta: {
        float r = z - xp;
        s.x = xp + cfg_.alpha * r;
        s.v += cfg_.beta * r / (float)dt;
        break;
      }
      case FilterKind::kKalman: {
        float t = (float)dt, q = cfg_.accelNoise * cfg_.accelNoise;
        float pxx = s.pxx + 2 * t * s.pxv + t * t * s.pvv + q * t * t * t * t / 4;
        float pxv = s.pxv + t * s.pvv + q * t * t * t / 2;
        float pvv = s.pvv + q * t * t;
        float sk = pxx + cfg_.measSigma * cfg_.measSigma;
        float kx = pxx / sk, kv = pxv / sk, r = z - xp;
        s.x = xp + kx * r;
        s.v += kv * r;
        s.pxx = (1 - kx) * pxx;
        s.pxv = (1 - kx) * pxv;
        s.pvv = pvv - kv * pxv;
        break;
      }
    }
  } else {
    // paused or duplicate sim time: take the observation, keep the velocity
    s.x = z;
  }
  s.z = z;
}

float Estimator::geometrySigma(int i, float x, const Frame& f) const {
  // lap distance is measured along the track reference, not along our car's axis
  float w = cfg_.lateralSeparation;
  if (f.velX < -0.5f) return kUnplaceable;  // reversing
  float speed = std::max(f.speed, 1.0f);
  float slip = f.speed > 3 ? std::atan2(std::fabs(f.velY), std::max(f.velX, 0.1f)) : 0.0f;
  if (slip > 0.5f) return kUnplaceable;  // sliding or spinning, our axis no longer follows the track
  float kappa = std::fabs(f.yawRate) / std::max(speed, 5.0f);
  float curve = std::fabs(x) * kappa * w;
  float heading = w * std::sin(slip);
  // a car crawling or going backwards along the track may be at any angle
  float otherSpeed = f.velX + cars_[i].v;
  if (f.speed > 8 && otherSpeed < 2) return kUnplaceable;
  float cross = geom(i).model != geom(me_).model || geom(i).model < 0 ? cfg_.crossModelSigma : 0.0f;
  return std::sqrt(curve * curve + heading * heading + cross * cross);
}

void Estimator::update(const Frame& f) {
  if (f.sessionNum != lastSessionNum_ || f.sessionUid != lastSessionUid_ || f.playerIdx != lastPlayer_) {
    reset();
    lastSessionNum_ = f.sessionNum;
    lastSessionUid_ = f.sessionUid;
    lastPlayer_ = f.playerIdx;
  }
  double dt = hasLast_ ? f.sessionTime - lastSim_ : 0;
  if (hasLast_ && (dt < 0 || dt > 1.0)) reset();  // replay jump or long stall, history no longer describes the present
  hasLast_ = true;
  lastSim_ = f.sessionTime;
  lastHost_ = f.hostTime;
  diag_.clear();
  view_ = {};

  me_ = f.playerIdx;
  int n = (int)f.lapDistPct.size();
  if (!f.isOnTrack || f.isReplay) return;  // not driving, nothing to spot
  view_.feed = Feed::kUnavailable;
  if (me_ < 0 || me_ >= n || geo_.trackLength <= 0) return;
  view_.myLength = geom(me_).length;
  float myPct = f.lapDistPct[me_];
  if (!validPct(myPct)) return;
  if (f.carLeftRight < irsdk::kLRClear || f.carLeftRight > irsdk::kLR2CarsRight) return;
  view_.feed = Feed::kLive;
  if ((int)cars_.size() < n) cars_.resize(n);

  std::vector<Cand>& cands = cands_;
  cands.clear();
  for (int i = 0; i < n; ++i) {
    float pct = f.lapDistPct[i];
    bool usable = i != me_ && validPct(pct) && f.trackSurface[i] != irsdk::kNotInWorld && !geom(i).ignore;
    double d = pct - myPct;
    d -= std::floor(d + 0.5);
    float z = (float)(d * geo_.trackLength) - (geom(i).refOffset - geom(me_).refOffset);
    if (!usable || std::fabs(z) > kNeighbourhood) {
      cars_[i] = {};
      continue;
    }
    trackCar(i, z, f.sessionTime);
    const CarState& s = cars_[i];
    float x = s.x + std::clamp(s.v * cfg_.predict, -0.5f, 0.5f);
    float half = 0.5f * (geom(me_).length + geom(i).length) + cfg_.padding;
    float margin = half - std::fabs(x);
    if (margin < -cfg_.gateMargin) {
      cars_[i].locked = false;
      continue;
    }
    float track = cfg_.filter == FilterKind::kKalman ? std::sqrt(std::max(s.pxx, 0.0f)) : cfg_.measSigma;
    float g = geometrySigma(i, x, f);
    // a pit lane running beside the track puts parked and pitting cars level with us
    bool inPits = f.onPitRoad[i] || f.trackSurface[i] == irsdk::kInPitStall;
    bool meInPits = f.onPitRoad[me_] || f.trackSurface[me_] == irsdk::kInPitStall;
    float context = inPits != meInPits ? cfg_.pPitMismatch : 1.0f;
    cands.push_back({i, x, half, std::sqrt(track * track + g * g), margin, context});
  }
  // keep the cars the spotter could plausibly mean when more than kMaxCands are within reach
  std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
    return a.context != b.context ? a.context > b.context : a.margin > b.margin;
  });
  if (cands.size() > kMaxCands) cands.resize(kMaxCands);
  associate(cands, f.carLeftRight);
}

void Estimator::associate(const std::vector<Cand>& cands, int status) {
  const int k = (int)cands.size();
  const Occupancy occ = occupancyFor(status);
  const float logPhantom = std::log(cfg_.pPhantom);

  // per candidate log likelihood of being left, right or not alongside
  float term[kMaxCands][3];
  for (int c = 0; c < k; ++c) {
    const Cand& cd = cands[c];
    const CarState& s = cars_[cd.car];
    float soft = std::sqrt(cfg_.edgeSigma * cfg_.edgeSigma + std::min(cd.sigma, 2.0f) * std::min(cd.sigma, 2.0f));
    float along = cd.context * sigmoid(cd.margin / soft);
    term[c][0] = std::log(std::max(1e-9f, 1 - (1 - cfg_.pFar) * along));
    for (int side = 1; side <= 2; ++side) {
      float prior = 0;
      if (s.side != 0 && s.side != side) prior = std::log(s.locked ? cfg_.pSwitchLocked : cfg_.pSwitchFree);
      term[c][side] = std::log(std::max(1e-9f, along)) + prior;
    }
  }

  static_assert(kMaxCands <= 4, "Hyp holds four assignments");
  std::vector<Hyp>& hyps = hyps_;
  hyps.clear();
  int total = 1;
  for (int c = 0; c < k; ++c) total *= 3;
  for (int code = 0; code < total; ++code) {
    Hyp h{};
    int count[3] = {0, 0, 0};
    for (int c = 0, rest = code; c < k; ++c, rest /= 3) {
      h.assign[c] = rest % 3;
      ++count[h.assign[c]];
      h.score += term[c][h.assign[c]];
    }
    if (count[1] > occ.maxL || count[2] > occ.maxR) continue;
    h.phantom[1] = std::max(0, occ.minL - count[1]);
    h.phantom[2] = std::max(0, occ.minR - count[2]);
    h.score += (h.phantom[1] + h.phantom[2]) * logPhantom;
    hyps.push_back(h);
  }

  float best = -INFINITY;
  for (const auto& h : hyps) best = std::max(best, h.score);
  float norm = 0;
  for (auto& h : hyps) norm += (h.score = std::exp(h.score - best));
  for (auto& h : hyps) h.score /= norm;

  float marg[kMaxCands][3] = {};
  for (const auto& h : hyps)
    for (int c = 0; c < k; ++c) marg[c][h.assign[c]] += h.score;

  for (int side = 1; side <= 2; ++side) {
    SideView& out = side == 1 ? view_.left : view_.right;
    // the displayed assignment is the most probable set of cars on this side
    std::vector<Group>& groups = groups_;
    groups.clear();
    for (const auto& h : hyps) {
      int mask = 0;
      for (int c = 0; c < k; ++c)
        if (h.assign[c] == side) mask |= 1 << c;
      auto it = std::find_if(groups.begin(), groups.end(),
                             [&](const Group& g) { return g.mask == mask && g.phantoms == h.phantom[side]; });
      if (it == groups.end()) groups.push_back({mask, h.phantom[side], h.score});
      else it->p += h.score;
    }
    if (groups.empty()) continue;
    const Group& g = *std::max_element(groups.begin(), groups.end(), [](auto& a, auto& b) { return a.p < b.p; });
    int cars = __builtin_popcount(g.mask) + g.phantoms;
    int minCars = side == 1 ? occ.minL : occ.minR;
    out.reported = std::max(cars, minCars);
    out.confidence = g.p;
    if (cars == 0) continue;
    bool precise = g.p >= cfg_.minConfidence && g.phantoms == 0;
    float rear = INFINITY, front = -INFINITY, sigma = 0;
    for (int c = 0; c < k; ++c) {
      if (!(g.mask & (1 << c))) continue;
      float half = 0.5f * geom(cands[c].car).length;
      rear = std::min(rear, cands[c].x - half);
      front = std::max(front, cands[c].x + half);
      sigma = std::max(sigma, cands[c].sigma);
      if (out.carIdx < 0) out.carIdx = cands[c].car;
    }
    out.sigma = sigma;
    out.positioned = precise && sigma <= cfg_.maxSigma;
    if (g.mask) {
      out.rear = rear;
      out.front = front;
    }
  }

  for (int c = 0; c < k; ++c) {
    CarState& s = cars_[cands[c].car];
    if (marg[c][1] > 0.95f || marg[c][2] > 0.95f) {
      s.side = marg[c][1] > marg[c][2] ? 1 : 2;
      s.locked = true;
    } else if (marg[c][0] > 0.95f) {
      s.locked = false;
    }
    diag_.push_back({cands[c].car, s.z, cands[c].x, s.v, cands[c].sigma, cands[c].margin, marg[c][1], marg[c][2]});
  }
}

View Estimator::view(double hostNow) const {
  View v = view_;
  if (v.feed == Feed::kLive && hostNow - lastHost_ > cfg_.staleAfter) {
    v.feed = Feed::kUnavailable;
    v.left = v.right = {};
  }
  return v;
}
