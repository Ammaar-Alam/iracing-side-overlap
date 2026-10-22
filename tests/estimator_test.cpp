// Estimator and session parsing checks on synthetic frames and recorded fixtures
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "estimator.h"
#include "frame.h"
#include "ibt.h"
#include "irsdk.h"
#include "session_info.h"

static int g_failures = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      ++g_failures;                                                \
      fprintf(stderr, "%s:%d CHECK(%s)\n", __FILE__, __LINE__, #cond); \
    }                                                              \
  } while (0)
#define CHECK_NEAR(a, b, tol) CHECK(std::fabs((a) - (b)) <= (tol))

namespace {

constexpr double kTrack = 4000.0;

SessionGeometry geometry(int cars, float length = 4.0f) {
  SessionGeometry g;
  g.trackLength = kTrack;
  g.cars.assign(cars, CarGeometry{length, 0, 0, false});
  return g;
}

// driving frame with the player (car 0) at myPct and others placed by metres ahead
struct Builder {
  Frame f;
  double t = 0;
  int tick = 0;
  explicit Builder(int cars) {
    f.lapDistPct.assign(cars, -1);
    f.trackSurface.assign(cars, irsdk::kNotInWorld);
    f.onPitRoad.assign(cars, 0);
    f.playerIdx = 0;
    f.isOnTrack = true;
    f.sessionNum = 0;
    f.sessionUid = 1;
    f.speed = f.velX = 40;
  }
  Frame& at(double myPct, std::initializer_list<std::pair<int, double>> ahead, int status) {
    t += 1.0 / 60;
    f.sessionTime = t;
    f.hostTime = t;
    f.tick = ++tick;
    f.lapDistPct[0] = (float)myPct;
    f.trackSurface[0] = irsdk::kOnTrack;
    for (auto [car, metres] : ahead) {
      double p = myPct + metres / kTrack;
      f.lapDistPct[car] = (float)(p - std::floor(p));
      f.trackSurface[car] = irsdk::kOnTrack;
    }
    f.carLeftRight = status;
    return f;
  }
};

void testWraparound() {
  Estimator e;
  e.setGeometry(geometry(2));
  Builder b(2);
  e.update(b.at(0.9995, {{1, 3.0}}, irsdk::kLRCarLeft));  // other car is across the line, 3 m ahead
  View v = e.view(b.t);
  CHECK(v.feed == Feed::kLive);
  CHECK(v.left.positioned);
  CHECK_NEAR(0.5f * (v.left.rear + v.left.front), 3.0f, 0.01f);
  e.update(b.at(0.0002, {{1, -2.0}}, irsdk::kLRCarLeft));
  v = e.view(b.t);
  CHECK_NEAR(0.5f * (v.left.rear + v.left.front), -2.0f, 0.01f);
  // the sim briefly reports fractions just past 1 at the line
  Frame& f = b.at(0.0002, {{1, -2.0}}, irsdk::kLRCarLeft);
  f.lapDistPct[0] = 1.0000001f;
  f.lapDistPct[1] = 0.9995f;
  e.update(f);
  v = e.view(b.t);
  CHECK(v.feed == Feed::kLive && v.left.positioned);
  CHECK_NEAR(0.5f * (v.left.rear + v.left.front), -2.0f, 0.01f);
}

void testPassOnRight() {
  Estimator e;
  e.setGeometry(geometry(2));
  Builder b(2);
  int positioned = 0;
  for (int k = 0; k <= 160; ++k) {
    double x = -8.0 + k * 0.1;  // car 1 overtakes from 8 m behind to 8 m ahead
    bool alongside = std::fabs(x) < 4.0 + 0.25;
    e.update(b.at(0.5, {{1, x}}, alongside ? irsdk::kLRCarRight : irsdk::kLRClear));
    View v = e.view(b.t);
    CHECK(v.left.reported == 0);
    CHECK(v.right.reported == (alongside ? 1 : 0));
    if (alongside && v.right.positioned) {
      ++positioned;
      CHECK_NEAR(v.right.rear, x - 2.0, 0.01);
      CHECK_NEAR(v.right.front, x + 2.0, 0.01);
      CHECK(v.right.carIdx == 1);
    }
  }
  CHECK(positioned >= 80);
}

void testSpotterClearWins() {
  Estimator e;
  e.setGeometry(geometry(2));
  Builder b(2);
  e.update(b.at(0.3, {{1, 0.5}}, irsdk::kLRClear));  // lap distance overlaps but the spotter sees nobody alongside
  View v = e.view(b.t);
  CHECK(v.feed == Feed::kLive);
  CHECK(v.left.reported == 0 && v.right.reported == 0);
}

void testFeedStates() {
  Estimator e;
  e.setGeometry(geometry(2));
  Builder b(2);
  e.update(b.at(0.3, {{1, 0.5}}, irsdk::kLROff));
  CHECK(e.view(b.t).feed == Feed::kUnavailable);
  e.update(b.at(0.3, {{1, 0.5}}, -1));  // variable missing
  CHECK(e.view(b.t).feed == Feed::kUnavailable);
  e.update(b.at(0.3, {{1, 0.5}}, irsdk::kLRCarLeft));
  CHECK(e.view(b.t).feed == Feed::kLive);
  CHECK(e.view(b.t + 1.0).feed == Feed::kUnavailable);  // no new ticks for a second
  b.f.isOnTrack = false;
  e.update(b.at(0.3, {{1, 0.5}}, irsdk::kLROff));
  CHECK(e.view(b.t).feed == Feed::kInactive);
}

void testThreeWideAmbiguous() {
  Estimator e;
  e.setGeometry(geometry(3));
  Builder b(3);
  // both cars arrive alongside together with nothing to tell them apart
  for (int k = 0; k < 30; ++k) e.update(b.at(0.4, {{1, 0.3}, {2, 0.4}}, irsdk::kLRCarLeftRight));
  View v = e.view(b.t);
  CHECK(v.left.reported == 1 && v.right.reported == 1);
  CHECK(!v.left.positioned && !v.right.positioned);
}

void testThreeWideWithHistory() {
  Estimator e;
  e.setGeometry(geometry(3));
  Builder b(3);
  for (int k = 0; k < 30; ++k) e.update(b.at(0.4, {{1, 0.5}, {2, -12.0}}, irsdk::kLRCarLeft));
  // car 2 arrives on the other side, car 1 cannot have swapped sides while alongside
  for (int k = 0; k < 30; ++k) e.update(b.at(0.4, {{1, 0.5}, {2, -1.0}}, irsdk::kLRCarLeftRight));
  View v = e.view(b.t);
  CHECK(v.left.positioned && v.right.positioned);
  CHECK(v.left.carIdx == 1 && v.right.carIdx == 2);
  CHECK_NEAR(0.5f * (v.right.rear + v.right.front), -1.0f, 0.01f);
}

void testPhantom() {
  Estimator e;
  e.setGeometry(geometry(2));
  Builder b(2);
  e.update(b.at(0.2, {{1, 40.0}}, irsdk::kLRCarLeft));  // spotter sees a car we cannot place
  View v = e.view(b.t);
  CHECK(v.left.reported == 1);
  CHECK(!v.left.positioned);
}

void testParkedPitCarIgnored() {
  Estimator e;
  e.setGeometry(geometry(3));
  Builder b(3);
  // a car parked in a pit stall beside the straight sits level with us by lap distance
  for (int k = 0; k < 20; ++k) {
    Frame& f = b.at(0.5, {{1, -1.5}, {2, 0.2}}, irsdk::kLRCarLeft);
    f.onPitRoad[2] = 1;
    f.trackSurface[2] = irsdk::kInPitStall;
    e.update(f);
  }
  View v = e.view(b.t);
  CHECK(v.left.positioned);
  CHECK(v.left.carIdx == 1);
}

void testTwoCarsLeft() {
  Estimator e;
  e.setGeometry(geometry(3));
  Builder b(3);
  e.update(b.at(0.6, {{1, -2.5}, {2, 1.5}}, irsdk::kLR2CarsLeft));
  View v = e.view(b.t);
  CHECK(v.left.reported == 2 && v.right.reported == 0);
  CHECK(v.left.positioned);
  CHECK_NEAR(v.left.rear, -4.5f, 0.01f);
  CHECK_NEAR(v.left.front, 3.5f, 0.01f);
}

void testSpinFallsBack() {
  Estimator e;
  e.setGeometry(geometry(2));
  Builder b(2);
  b.f.velY = 30;  // sliding sideways
  e.update(b.at(0.3, {{1, 1.0}}, irsdk::kLRCarLeft));
  View v = e.view(b.t);
  CHECK(v.left.reported == 1 && !v.left.positioned);
  b.f.velY = 0;
  b.f.velX = -3;  // reversing
  e.update(b.at(0.3, {{1, 1.0}}, irsdk::kLRCarLeft));
  CHECK(!e.view(b.t).left.positioned);
}

void testCrossModelUncertainty() {
  Estimator e;
  SessionGeometry g = geometry(2);
  g.cars[1].model = 1;  // different car model, reference points unknown
  e.setGeometry(g);
  Builder b(2);
  e.update(b.at(0.3, {{1, 1.0}}, irsdk::kLRCarLeft));
  View v = e.view(b.t);
  CHECK(v.left.positioned);
  CHECK_NEAR(v.left.sigma, 0.3f, 0.06f);
}

void testDiscontinuityAndSessionReset() {
  EstimatorConfig c;
  c.filter = FilterKind::kKalman;
  Estimator e(c);
  e.setGeometry(geometry(2));
  Builder b(2);
  for (int k = 0; k < 20; ++k) e.update(b.at(0.3, {{1, 1.0}}, irsdk::kLRCarLeft));
  e.update(b.at(0.3, {{1, -2.5}}, irsdk::kLRCarLeft));  // jumps 3.5 m in one tick
  View v = e.view(b.t);
  CHECK_NEAR(0.5f * (v.left.rear + v.left.front), -2.5f, 0.01f);
  int before = e.resets();
  b.f.sessionNum = 1;
  e.update(b.at(0.3, {{1, 1.0}}, irsdk::kLRCarLeft));
  CHECK(e.resets() == before + 1);
}

void testFiltersFollowConstantClosing() {
  for (FilterKind k : {FilterKind::kRaw, FilterKind::kAlphaBeta, FilterKind::kKalman}) {
    EstimatorConfig c;
    c.filter = k;
    Estimator e(c);
    e.setGeometry(geometry(2));
    Builder b(2);
    float err = 0;
    for (int i = 0; i < 120; ++i) {
      double x = -4.0 + i * (6.0 / 60);  // closing at 6 m/s
      e.update(b.at(0.3, {{1, x}}, std::fabs(x) < 4.25 ? irsdk::kLRCarLeft : irsdk::kLRClear));
      if (i > 30 && !e.candidates().empty()) err = std::max(err, std::fabs(e.candidates()[0].x - (float)x));
    }
    CHECK(err < 0.05f);
  }
}

void testSessionInfo() {
  const char* yaml =
      "---\nWeekendInfo:\n TrackName: test oval\n TrackLength: 3.8874 km\n TrackDirection: neutral\n"
      "DriverInfo:\n DriverCarIdx: 2\n DriverUserID: 7\n Drivers:\n - CarIdx: 0\n   UserName: Pace Car\n"
      "   CarPath: safety pcporsche911cup\n   CarIsPaceCar: 1\n   IsSpectator: 0\n - CarIdx: 2\n"
      "   UserName: A\n   CarPath: mx5 mx52016\n   CarIsPaceCar: 0\n   IsSpectator: 0\n - CarIdx: 5\n"
      "   CarPath: toyotagr86\n   IsSpectator: 1\nSplitTimeInfo:\n Sectors:\n - SectorNum: 0\n";
  SessionInfo s = parseSessionInfo(yaml);
  CHECK_NEAR(s.trackLength, 3887.4, 0.01);
  CHECK(s.trackName == "test oval");
  CHECK(s.driverCarIdx == 2);
  CHECK(s.drivers.size() == 3);
  CHECK(s.drivers[0].paceCar && !s.drivers[1].paceCar && s.drivers[2].spectator);
  CHECK(s.drivers[1].carPath == "mx5 mx52016");
  SessionGeometry g = makeGeometry(s, {{"toyotagr86", 4.3f}});
  CHECK(g.cars.size() == 6);
  CHECK_NEAR(g.cars[2].length, 3.915f, 1e-4f);
  CHECK_NEAR(g.cars[5].length, 4.3f, 1e-4f);
  CHECK(g.cars[5].ignore);
  CHECK(g.cars[0].model != g.cars[2].model);
  CHECK_NEAR(parseSessionInfo("WeekendInfo:\n TrackLength: 2.50 mi\n").trackLength, 4023.36, 0.01);
}

// replays a recorded fixture and checks invariants that must hold on real telemetry
void testFixture(const std::string& path) {
  IbtReader r;
  CHECK(r.open(path));
  SessionInfo info = parseSessionInfo(r.sessionInfo());
  CHECK(info.trackLength > 0);
  SessionGeometry geo = makeGeometry(info, {});
  FrameDecoder dec;
  dec.bind(r.schema());
  CHECK(dec.missing().empty());
  const irsdk::VarHeader* host = r.schema().find("CapHostTime");
  Estimator e;
  e.setGeometry(geo);
  std::vector<uint8_t> row;
  Frame f;
  int occupied = 0, positioned = 0, switches = 0, prevCar[3] = {-1, -1, -1};
  while (r.next(row)) {
    dec.decode(row.data(), f);
    f.hostTime = host ? irsdk::readAsDouble(row.data(), *host, 0) : f.sessionTime;
    e.update(f);
    View v = e.view(f.hostTime);
    if (!f.isOnTrack || v.feed != Feed::kLive) continue;
    int s = f.carLeftRight;
    bool left = s == irsdk::kLRCarLeft || s == irsdk::kLRCarLeftRight || s == irsdk::kLR2CarsLeft;
    bool right = s == irsdk::kLRCarRight || s == irsdk::kLRCarLeftRight || s == irsdk::kLR2CarsRight;
    CHECK((v.left.reported > 0) == left);
    CHECK((v.right.reported > 0) == right);
    for (int side = 1; side <= 2; ++side) {
      const SideView& sv = side == 1 ? v.left : v.right;
      if (!sv.reported) {
        prevCar[side] = -1;
        continue;
      }
      ++occupied;
      if (!sv.positioned) {
        prevCar[side] = -1;
        continue;
      }
      ++positioned;
      if (prevCar[side] >= 0 && prevCar[side] != sv.carIdx) ++switches;
      prevCar[side] = sv.carIdx;
      if (sv.reported == 1) {
        double d = f.lapDistPct[sv.carIdx] - f.lapDistPct[f.playerIdx];
        d = (d - std::floor(d + 0.5)) * info.trackLength;
        CHECK_NEAR(0.5 * (sv.rear + sv.front), d, 0.01);
      }
    }
  }
  CHECK(occupied > 0);
  CHECK(switches == 0);
  CHECK(positioned >= 0.9 * occupied);
  printf("fixture %s: %d occupied side-ticks, %d positioned\n", path.c_str(), occupied, positioned);
}

}  // namespace

int main(int argc, char** argv) {
  testWraparound();
  testPassOnRight();
  testSpotterClearWins();
  testFeedStates();
  testThreeWideAmbiguous();
  testThreeWideWithHistory();
  testPhantom();
  testParkedPitCarIgnored();
  testTwoCarsLeft();
  testSpinFallsBack();
  testCrossModelUncertainty();
  testDiscontinuityAndSessionReset();
  testFiltersFollowConstantClosing();
  testSessionInfo();
  if (argc > 1) {
    WIN32_FIND_DATAA fd;
    std::string dir = argv[1];
    HANDLE h = FindFirstFileA((dir + "\\*.ibt").c_str(), &fd);
    for (bool more = h != INVALID_HANDLE_VALUE; more; more = FindNextFileA(h, &fd)) testFixture(dir + "\\" + fd.cFileName);
    if (h != INVALID_HANDLE_VALUE) FindClose(h);
  }
  if (g_failures) fprintf(stderr, "%d check(s) failed\n", g_failures);
  else printf("all estimator tests passed\n");
  return g_failures ? 1 : 0;
}
