// sideeval: replays .ibt captures through the estimator and measures it
//   sideeval FILE.ibt [FILE.ibt ...] [--filter raw|alphabeta|kalman] [--predict S] [--csv OUT.csv] [--session N] [--edges]
//
// The forward pass is causal: every estimate sees only earlier frames, exactly as live.
// A second pass runs the same estimator backwards in time. Where that hindsight pass is
// confident it labels which car was really on each side, which checks the live assignments
// without letting the future leak into them. Both passes use the same signals, so the
// labels measure consistency, not independent physical truth.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "estimator.h"
#include "frame.h"
#include "ibt.h"
#include "irsdk.h"
#include "session_info.h"

namespace {

struct Summary {
  std::vector<float> values;
  void add(float v) { values.push_back(v); }
  float pct(double p) const {
    if (values.empty()) return NAN;
    std::vector<float> v = values;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, (size_t)(p * (v.size() - 1) + 0.5))];
  }
  void print(const char* name, const char* unit) const {
    printf("  %-36s n=%-6zu p05=%7.3f p25=%7.3f p50=%7.3f p75=%7.3f p95=%7.3f %s\n", name, values.size(), pct(.05),
           pct(.25), pct(.5), pct(.75), pct(.95), unit);
  }
};

struct Options {
  std::vector<std::string> files;
  EstimatorConfig cfg;
  std::string csv;
  bool listEdges = false;
  int session = -1;  // only score this SessionNum, -1 for all
};

// metres of track between two lap fractions, positive when b is ahead of a
double trackDelta(float a, float b, double length) {
  double d = (double)b - a;
  d -= std::floor(d + 0.5);
  return d * length;
}

struct Edge {
  bool enter;
  float offset, expected, speed, kappa, slip;
};

struct SideRec {
  int reported = 0, carIdx = -1;
  bool positioned = false;
  float centre = 0, confidence = 0;
};

struct Eval {
  std::map<int, int> statusTicks;
  int transitions = 0, shortRuns = 0;
  int drivingTicks = 0, liveTicks = 0, unavailableTicks = 0;
  int sideOccupied = 0, sidePositioned = 0, sideAmbiguous = 0, sideImprecise = 0, sideUnmatched = 0, sideMulti = 0;
  int identitySwitches = 0;
  // hindsight comparison, in side-ticks
  int hindsightConfident = 0, hindsightAgree = 0, hindsightDisagree = 0, fallbackResolvable = 0;
  // lap distance overlap that the spotter does not report, and the reverse
  int quietOnTrack = 0, quietOffTrack = 0, quietPit = 0, occupiedNoCandidate = 0;
  std::vector<Edge> edges;
  Summary stepChange;
  int frozenTicks = 0, movingTicks = 0;
  Summary lag, roughness, nextTickError;
  Summary hostDt;
  int tickGaps = 0, missedTicks = 0;
};

struct Loaded {
  std::vector<Frame> frames;
  SessionInfo info;
  SessionGeometry geo;
};

bool load(const std::string& path, Loaded& out, Eval& ev) {
  IbtReader r;
  if (!r.open(path)) {
    fprintf(stderr, "cannot read %s\n", path.c_str());
    return false;
  }
  out.info = parseSessionInfo(r.sessionInfo());
  if (out.info.trackLength <= 0) {
    fprintf(stderr, "%s: no session info, skipped\n", path.c_str());
    return false;
  }
  out.geo = makeGeometry(out.info, {});
  FrameDecoder dec;
  dec.bind(r.schema());
  for (const auto& m : dec.missing()) fprintf(stderr, "%s: missing %s\n", path.c_str(), m.c_str());
  const irsdk::VarHeader* hostVar = r.schema().find("CapHostTime");
  const irsdk::VarHeader* tickVar = r.schema().find("CapTickCount");
  std::vector<uint8_t> row;
  out.frames.reserve(r.recordCount());
  int index = 0;
  while (r.next(row)) {
    Frame& f = out.frames.emplace_back();
    dec.decode(row.data(), f);
    f.hostTime = hostVar ? irsdk::readAsDouble(row.data(), *hostVar, 0) : index / 60.0;
    f.tick = tickVar ? (int)irsdk::readAsDouble(row.data(), *tickVar, 0) : index;
    if (index > 0 && hostVar) {
      const Frame& p = out.frames[index - 1];
      ev.hostDt.add((float)((f.hostTime - p.hostTime) * 1e3));
      if (f.tick - p.tick > 1) {
        ++ev.tickGaps;
        ev.missedTicks += f.tick - p.tick - 1;
      }
    }
    ++index;
  }
  printf("%s: %zu rows, %s, %.1f m, %zu entries\n", path.c_str(), out.frames.size(), out.info.trackName.c_str(),
         out.info.trackLength, out.info.drivers.size());
  return true;
}

SideRec record(const SideView& sv) {
  return {sv.reported, sv.carIdx, sv.positioned, 0.5f * (sv.rear + sv.front), sv.confidence};
}

// runs the estimator over the frames, backwards in time when reverse is set
void pass(const Loaded& d, const EstimatorConfig& cfg, bool reverse, std::vector<View>& views) {
  Estimator est(cfg);
  est.setGeometry(d.geo);
  int n = (int)d.frames.size();
  views.assign(n, {});
  for (int k = 0; k < n; ++k) {
    int i = reverse ? n - 1 - k : k;
    Frame f = d.frames[i];
    if (reverse) {
      f.sessionTime = -f.sessionTime;
      f.hostTime = -f.hostTime;
    }
    est.update(f);
    views[i] = est.view(f.hostTime);
  }
}

void evaluate(const Loaded& d, const Options& opt, Eval& ev, FILE* csv) {
  std::vector<View> fwd, bwd;
  pass(d, opt.cfg, false, fwd);
  pass(d, opt.cfg, true, bwd);
  const double len = d.info.trackLength;
  int runStatus = -2, runLength = 0;
  int prevCar[3] = {-1, -1, -1};
  float prevCentre[3] = {NAN, NAN, NAN}, prevDelta[3] = {NAN, NAN, NAN};
  std::vector<float> prevStep;

  for (size_t i = 0; i < d.frames.size(); ++i) {
    const Frame& f = d.frames[i];
    const View& v = fwd[i];
    if (!f.isOnTrack || f.isReplay || (opt.session >= 0 && f.sessionNum != opt.session)) {
      runStatus = -2;
      continue;
    }
    ++ev.drivingTicks;
    ++ev.statusTicks[f.carLeftRight];
    if (f.carLeftRight != runStatus) {
      if (runStatus >= irsdk::kLRCarLeft && runLength <= 3) ++ev.shortRuns;  // occupied for 50 ms or less
      if (runStatus >= 0) ++ev.transitions;
      runStatus = f.carLeftRight;
      runLength = 0;
    }
    ++runLength;
    if (v.feed == Feed::kUnavailable) ++ev.unavailableTicks;
    if (v.feed != Feed::kLive) continue;
    ++ev.liveTicks;
    const Frame* next = i + 1 < d.frames.size() ? &d.frames[i + 1] : nullptr;
    const Frame* prev = i > 0 ? &d.frames[i - 1] : nullptr;
    int me = f.playerIdx, n = (int)f.lapDistPct.size();

    for (int side = 1; side <= 2; ++side) {
      SideRec a = record(side == 1 ? v.left : v.right);
      SideRec b = record(side == 1 ? bwd[i].left : bwd[i].right);
      if (a.reported == 0) {
        prevCar[side] = -1;
        continue;
      }
      ++ev.sideOccupied;
      if (a.reported > 1) ++ev.sideMulti;
      bool hindsight = b.positioned && b.reported == 1;
      if (hindsight) ++ev.hindsightConfident;
      if (a.positioned) {
        ++ev.sidePositioned;
        if (prevCar[side] >= 0 && a.carIdx != prevCar[side]) ++ev.identitySwitches;
        if (hindsight && a.reported == 1) (a.carIdx == b.carIdx ? ev.hindsightAgree : ev.hindsightDisagree)++;
        if (a.reported == 1 && a.carIdx == prevCar[side]) {
          double raw = trackDelta(f.lapDistPct[me], f.lapDistPct[a.carIdx], len);
          ev.lag.add(std::fabs(a.centre - (float)raw));
          if (next && next->carLeftRight == f.carLeftRight && next->playerIdx == me)
            ev.nextTickError.add(
                std::fabs(a.centre - (float)trackDelta(next->lapDistPct[me], next->lapDistPct[a.carIdx], len)));
          float delta = a.centre - prevCentre[side];
          if (!std::isnan(prevDelta[side])) ev.roughness.add(std::fabs(delta - prevDelta[side]));
          prevDelta[side] = delta;
        } else {
          prevDelta[side] = NAN;
        }
        prevCentre[side] = a.centre;
        prevCar[side] = a.carIdx;
      } else {
        prevCar[side] = -1;
        prevDelta[side] = NAN;
        if (a.carIdx < 0) ++ev.sideUnmatched;
        else if (a.confidence < opt.cfg.minConfidence) ++ev.sideAmbiguous;
        else ++ev.sideImprecise;
        if (hindsight) ++ev.fallbackResolvable;
      }
    }

    // raw offsets of nearby cars, independent of any filtering
    std::vector<float> z(n, NAN);
    bool anyCandidate = false;
    for (int c = 0; c < n; ++c) {
      if (c == me || f.lapDistPct[c] < 0 || f.trackSurface[c] == irsdk::kNotInWorld) continue;
      float dz = (float)trackDelta(f.lapDistPct[me], f.lapDistPct[c], len);
      if (std::fabs(dz) >= 60) continue;
      z[c] = dz;
      float half = 0.5f * (d.geo.cars[me].length + d.geo.cars[c].length);
      if (std::fabs(dz) < half + opt.cfg.padding + opt.cfg.gateMargin) anyCandidate = true;
      if (f.carLeftRight == irsdk::kLRClear && std::fabs(dz) < half - 0.5f) {
        if (f.onPitRoad[c] || f.onPitRoad[me]) ++ev.quietPit;
        else if (f.trackSurface[c] != irsdk::kOnTrack || f.trackSurface[me] != irsdk::kOnTrack) ++ev.quietOffTrack;
        else ++ev.quietOnTrack;
      }
    }
    if (f.carLeftRight >= irsdk::kLRCarLeft && !anyCandidate) ++ev.occupiedNoCandidate;

    if (prev && prev->isOnTrack && prev->playerIdx == me && prev->lapDistPct.size() == f.lapDistPct.size()) {
      double dt = f.sessionTime - prev->sessionTime;
      float mine = (float)trackDelta(prev->lapDistPct[me], f.lapDistPct[me], len);
      if (prevStep.size() != z.size()) prevStep.assign(z.size(), NAN);
      for (int c = 0; c < n && dt > 0; ++c) {
        // parked and pit lane cars legitimately stand still
        if (std::isnan(z[c]) || std::fabs(z[c]) > 30 || f.trackSurface[c] != irsdk::kOnTrack || f.onPitRoad[c] ||
            prev->lapDistPct[c] < 0 || mine < 0.2f) {
          prevStep[c] = NAN;
          continue;
        }
        float step = (float)trackDelta(prev->lapDistPct[c], f.lapDistPct[c], len);
        ++ev.movingTicks;
        if (step == 0) ++ev.frozenTicks;
        if (!std::isnan(prevStep[c])) ev.stepChange.add(std::fabs(step - prevStep[c]));
        prevStep[c] = step;
      }

      // the spotter flips while exactly one car is anywhere near overlap
      int a = prev->carLeftRight, b = f.carLeftRight;
      bool enter = a == irsdk::kLRClear && (b == irsdk::kLRCarLeft || b == irsdk::kLRCarRight);
      bool exit = (a == irsdk::kLRCarLeft || a == irsdk::kLRCarRight) && b == irsdk::kLRClear;
      if (enter || exit) {
        int near = -1, count = 0;
        for (int c = 0; c < n; ++c)
          if (!std::isnan(z[c]) && std::fabs(z[c]) < 8.0f && prev->lapDistPct[c] >= 0) {
            near = c;
            ++count;
          }
        if (count == 1) {
          float before = (float)trackDelta(prev->lapDistPct[me], prev->lapDistPct[near], len);
          float edge = 0.5f * (z[near] + before);  // the crossing happened between the two ticks
          float speed = (float)((z[near] - before) / std::max(dt, 1e-3));
          float half = 0.5f * (d.geo.cars[me].length + d.geo.cars[near].length);
          float kappa = std::fabs(f.yawRate) / std::max(f.speed, 5.0f);
          float slip = std::atan2(std::fabs(f.velY), std::max(f.velX, 0.1f));
          ev.edges.push_back({enter, edge, enter ? half : half + opt.cfg.padding, speed, kappa, slip});
        }
      }
    }

    if (csv) {
      fprintf(csv, "%.4f,%.4f,%d,%d,%d,%d,%.3f,%d,%.3f,%d,%d,%.3f,%d,%.3f\n", f.hostTime, f.sessionTime,
              f.carLeftRight, (int)v.feed, v.left.reported, v.left.positioned, 0.5f * (v.left.rear + v.left.front),
              v.left.carIdx, v.left.confidence, v.right.reported, v.right.positioned,
              0.5f * (v.right.rear + v.right.front), v.right.carIdx, v.right.confidence);
    }
  }
}

void report(const Eval& ev, const Options& opt) {
  printf("\nticks driving=%d live=%d unavailable=%d\n", ev.drivingTicks, ev.liveTicks, ev.unavailableTicks);
  printf("CarLeftRight ticks:");
  for (auto [s, n] : ev.statusTicks) printf(" [%d]=%d", s, n);
  printf("\n  transitions=%d, occupied runs of 3 ticks or less=%d\n", ev.transitions, ev.shortRuns);
  double occ = std::max(1, ev.sideOccupied);
  printf("occupied side-ticks=%d: positioned %.2f%%, fallback %.2f%% (ambiguous %.2f%%, imprecise %.2f%%, unmatched "
         "%.2f%%), multi-car %d\n",
         ev.sideOccupied, 100 * ev.sidePositioned / occ,
         100 * (ev.sideAmbiguous + ev.sideImprecise + ev.sideUnmatched) / occ, 100 * ev.sideAmbiguous / occ,
         100 * ev.sideImprecise / occ, 100 * ev.sideUnmatched / occ, ev.sideMulti);
  printf("identity switches while continuously positioned=%d\n", ev.identitySwitches);
  printf("hindsight labels: confident on %d side-ticks; live positioned agrees %d, disagrees %d; fallback ticks hindsight "
         "could resolve %d\n",
         ev.hindsightConfident, ev.hindsightAgree, ev.hindsightDisagree, ev.fallbackResolvable);
  printf("car-ticks where the spotter says clear but lap distance overlaps by >0.5 m: both on track %d, one off track "
         "%d, pit lane %d\n",
         ev.quietOnTrack, ev.quietOffTrack, ev.quietPit);
  printf("ticks occupied with no car within reach of lap distance: %d\n", ev.occupiedNoCandidate);

  printf("spotter edges against lap distance (isolated single car); expected = touching on enter, +padding on exit:\n");
  Summary enterErr, exitErr, straightErr, curveErr;
  for (const auto& e : ev.edges) {
    float err = std::fabs(e.offset) - e.expected;
    (e.enter ? enterErr : exitErr).add(err);
    (e.kappa < 0.005f ? straightErr : curveErr).add(std::fabs(err));
    if (opt.listEdges)
      printf("  %-5s %-6s offset=%7.3f expected=%6.3f error=%+6.3f speed=%+6.2f m/s radius=%6.0f m slip=%5.2f deg\n",
             e.enter ? "enter" : "exit", e.offset > 0 ? "ahead" : "behind", e.offset, e.expected, err, e.speed,
             e.kappa > 1e-4f ? 1 / e.kappa : 9999.0f, e.slip * 57.2958f);
  }
  enterErr.print("enter error (signed)", "m");
  exitErr.print("exit error (signed)", "m");
  straightErr.print("|error| radius > 200 m", "m");
  curveErr.print("|error| radius <= 200 m", "m");

  printf("remote position freshness (cars driving on track within 30 m):\n");
  printf("  frozen ticks=%d of %d (%.3f%%)\n", ev.frozenTicks, ev.movingTicks,
         100.0 * ev.frozenTicks / std::max(1, ev.movingTicks));
  ev.stepChange.print("tick-to-tick step change", "m");
  printf("  step change p99.9=%.4f max=%.4f m\n", ev.stepChange.pct(.999), ev.stepChange.pct(1));
  printf("displayed bar (single car):\n");
  ev.lag.print("|centre - raw offset same tick|", "m");
  ev.nextTickError.print("|centre - raw offset next tick|", "m");
  ev.roughness.print("|second difference of centre|", "m");
  if (!ev.hostDt.values.empty()) {
    ev.hostDt.print("host interval between ticks", "ms");
    printf("  tick gaps=%d missed ticks=%d\n", ev.tickGaps, ev.missedTicks);
  }
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--filter" && i + 1 < argc) {
      std::string k = argv[++i];
      opt.cfg.filter = k == "kalman" ? FilterKind::kKalman : k == "alphabeta" ? FilterKind::kAlphaBeta : FilterKind::kRaw;
    } else if (a == "--predict" && i + 1 < argc) {
      opt.cfg.predict = (float)atof(argv[++i]);
    } else if (a == "--csv" && i + 1 < argc) {
      opt.csv = argv[++i];
    } else if (a == "--session" && i + 1 < argc) {
      opt.session = atoi(argv[++i]);
    } else if (a == "--edges") {
      opt.listEdges = true;
    } else {
      opt.files.push_back(a);
    }
  }
  if (opt.files.empty()) {
    fprintf(stderr,
            "usage: sideeval FILE.ibt [...] [--filter raw|alphabeta|kalman] [--predict S] [--csv OUT] [--edges]\n");
    return 2;
  }
  FILE* csv = opt.csv.empty() ? nullptr : fopen(opt.csv.c_str(), "w");
  if (csv) fprintf(csv, "host,sim,clr,feed,l_n,l_pos,l_centre,l_car,l_conf,r_n,r_pos,r_centre,r_car,r_conf\n");
  Eval ev;
  for (const auto& path : opt.files) {
    Loaded d;
    if (load(path, d, ev)) evaluate(d, opt, ev, csv);
  }
  if (csv) fclose(csv);
  report(ev, opt);
  return 0;
}
