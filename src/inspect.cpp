// irinspect: telemetry inventory and capture utility
//   irinspect inventory [--seconds N] [--out DIR]  every live descriptor with sample values and update stats
//   irinspect capture [--seconds N] [--out DIR]    record live telemetry to .ibt until Ctrl+C
//   irinspect ibt FILE [--out DIR]                 same inventory for a recorded .ibt
//   irinspect clip IN OUT --from ROW --to ROW [--yaml FILE]  compact sanitized excerpt for test fixtures
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include "frame.h"
#include "ibt.h"
#include "recorder.h"
#include "sdk_live.h"
#include "session_info.h"
#include "timing.h"

namespace {

std::atomic<bool> g_quit = false;

BOOL WINAPI onCtrl(DWORD) {
  g_quit = true;
  return TRUE;
}

std::string stamp() {
  char buf[32];
  time_t t = time(nullptr);
  strftime(buf, sizeof buf, "%Y%m%d_%H%M%S", localtime(&t));
  return buf;
}

std::string fmtValue(const uint8_t* row, const irsdk::VarHeader& v, int i) {
  char buf[64];
  double x = irsdk::readAsDouble(row, v, i);
  if (v.type == irsdk::kBitField) snprintf(buf, sizeof buf, "0x%08x", (uint32_t)x);
  else if (v.type == irsdk::kFloat || v.type == irsdk::kDouble) snprintf(buf, sizeof buf, "%.7g", x);
  else snprintf(buf, sizeof buf, "%lld", (long long)x);
  return buf;
}

// Per-variable change and range statistics over a run of rows
class Inventory {
 public:
  explicit Inventory(const irsdk::Schema& s) : schema_(s), stats_(s.vars.size()) {
    for (size_t k = 0; k < s.vars.size(); ++k) stats_[k].everChanged.assign(std::max(1, s.vars[k].count), 0);
  }

  void add(const uint8_t* row) {
    for (size_t k = 0; k < schema_.vars.size(); ++k) {
      const auto& v = schema_.vars[k];
      auto& st = stats_[k];
      bool changed = false;
      for (int i = 0; i < v.count; ++i) {
        double x = irsdk::readAsDouble(row, v, i);
        if (!std::isnan(x)) {
          st.mn = std::min(st.mn, x);
          st.mx = std::max(st.mx, x);
        }
        if (v.type == irsdk::kBitField) st.bits |= (uint32_t)x;
        if (!prev_.empty() && irsdk::readAsDouble(prev_.data(), v, i) != x) {
          changed = true;
          st.everChanged[i] = 1;
        }
      }
      st.changedRows += changed;
    }
    prev_.assign(row, row + schema_.bufLen);
    ++rows_;
  }

  void write(FILE* f) const {
    fprintf(f, "name\ttype\tcount\tunit\tchange_frac\tmin\tmax\tactive_elems\tminus_one_elems\tbits_seen\tsample\tdesc\n");
    for (size_t k = 0; k < schema_.vars.size(); ++k) {
      const auto& v = schema_.vars[k];
      const auto& st = stats_[k];
      int active = 0, minusOne = 0;
      for (int i = 0; i < v.count; ++i) {
        active += st.everChanged[i];
        if (!prev_.empty() && irsdk::readAsDouble(prev_.data(), v, i) == -1.0) ++minusOne;
      }
      std::string sample;
      if (!prev_.empty()) {
        if (v.type == irsdk::kChar && v.count > 1) {
          sample = irsdk::fixedStr(reinterpret_cast<const char*>(prev_.data() + v.offset), v.count);
        } else {
          for (int i = 0; i < std::min(v.count, 8); ++i) sample += (i ? "," : "") + fmtValue(prev_.data(), v, i);
          if (v.count > 8) sample += ",...";
        }
      }
      fprintf(f, "%s\t%s\t%d\t%s\t%.4f\t%.7g\t%.7g\t%d\t%d\t0x%08x\t%s\t%s\n", irsdk::fixedStr(v.name, 32).c_str(),
              irsdk::typeName(v.type), v.count, irsdk::fixedStr(v.unit, 32).c_str(),
              rows_ > 1 ? (double)st.changedRows / (rows_ - 1) : 0.0, st.mn, st.mx, active, minusOne, st.bits,
              sample.c_str(), irsdk::fixedStr(v.desc, 64).c_str());
    }
  }

  int rows() const { return rows_; }

 private:
  struct Stat {
    double mn = INFINITY, mx = -INFINITY;
    int changedRows = 0;
    uint32_t bits = 0;
    std::vector<uint8_t> everChanged;
  };
  const irsdk::Schema& schema_;
  std::vector<Stat> stats_;
  std::vector<uint8_t> prev_;
  int rows_ = 0;
};

double percentile(std::vector<double> v, double p) {
  if (v.empty()) return NAN;
  std::sort(v.begin(), v.end());
  return v[std::min(v.size() - 1, (size_t)(p * (v.size() - 1) + 0.5))];
}

bool waitForSim(LiveSdk& sdk) {
  bool announced = false;
  while (!g_quit) {
    if (sdk.open() && sdk.connected()) return true;
    if (!announced) fprintf(stderr, "waiting for the simulator...\n");
    announced = true;
    Sleep(500);
  }
  return false;
}

int runInventory(const std::string& outDir, double seconds) {
  LiveSdk sdk;
  if (!waitForSim(sdk)) return 1;
  std::vector<uint8_t> row;
  int tick = 0;
  std::vector<double> hostDt, simDt;
  int wakes = 0, timeouts = 0, emptyWakes = 0, tickGaps = 0, missedTicks = 0, lastTick = -1;
  double lastHost = 0, lastSim = 0, start = hostSeconds();
  std::unique_ptr<Inventory> inv;
  const irsdk::VarHeader* sessionTime = nullptr;
  while (!g_quit && hostSeconds() - start < seconds && sdk.connected()) {
    DWORD w = WaitForSingleObject(sdk.dataEvent(), 200);
    if (w == WAIT_TIMEOUT) ++timeouts;
    else ++wakes;
    if (!sdk.readLatest(row, tick)) {
      if (w != WAIT_TIMEOUT) ++emptyWakes;
      continue;
    }
    double now = hostSeconds();
    if (sdk.schemaChanged()) {
      inv = std::make_unique<Inventory>(sdk.schema());
      sessionTime = sdk.schema().find("SessionTime");
      lastTick = -1;
    }
    inv->add(row.data());
    double sim = sessionTime ? irsdk::readAsDouble(row.data(), *sessionTime, 0) : 0;
    if (lastTick >= 0) {
      hostDt.push_back((now - lastHost) * 1000);
      simDt.push_back((sim - lastSim) * 1000);
      if (tick - lastTick > 1) {
        ++tickGaps;
        missedTicks += tick - lastTick - 1;
      }
    }
    lastTick = tick;
    lastHost = now;
    lastSim = sim;
  }
  if (!inv) {
    fprintf(stderr, "no telemetry rows received\n");
    return 1;
  }
  std::string base = outDir + "\\inventory_" + stamp();
  FILE* f = fopen((base + ".tsv").c_str(), "w");
  if (!f) return 1;
  inv->write(f);
  fclose(f);
  if (FILE* y = fopen((base + ".yaml").c_str(), "w")) {
    fputs(sdk.sessionInfo().c_str(), y);
    fclose(y);
  }
  if (FILE* s = fopen((base + ".txt").c_str(), "w")) {
    for (FILE* out : {s, stdout}) {
      fprintf(out, "rows=%d vars=%zu bufLen=%d tickRate=%d sessionInfoUpdate=%d tornReads=%d\n", inv->rows(),
              sdk.schema().vars.size(), sdk.schema().bufLen, sdk.tickRate(), sdk.sessionInfoUpdate(), sdk.tornReads());
      fprintf(out, "eventWakes=%d timeouts=%d wakesWithoutNewTick=%d tickGaps=%d missedTicks=%d\n", wakes, timeouts,
              emptyWakes, tickGaps, missedTicks);
      fprintf(out, "hostDtMs p50=%.3f p95=%.3f p99=%.3f max=%.3f\n", percentile(hostDt, .5), percentile(hostDt, .95),
              percentile(hostDt, .99), percentile(hostDt, 1));
      fprintf(out, "simDtMs p50=%.3f p95=%.3f max=%.3f\n", percentile(simDt, .5), percentile(simDt, .95),
              percentile(simDt, 1));
    }
    fclose(s);
  }
  printf("wrote %s.{tsv,yaml,txt}\n", base.c_str());
  return 0;
}

int runCapture(const std::string& outDir, double seconds) {
  LiveSdk sdk;
  Recorder rec;
  std::vector<uint8_t> row;
  std::string yaml;
  int tick = 0, infoUpdate = -1, subSession = -1;
  double start = hostSeconds(), lastReport = start;
  while (!g_quit && hostSeconds() - start < seconds) {
    if (!sdk.connected()) {
      if (rec.active()) {
        rec.stop();
        printf("sim disconnected, closed %s\n", rec.currentPath().c_str());
      }
      infoUpdate = subSession = -1;
      if (!waitForSim(sdk)) break;
    }
    WaitForSingleObject(sdk.dataEvent(), 200);
    if (!sdk.readLatest(row, tick)) continue;
    double now = hostSeconds();
    bool newInfo = sdk.sessionInfoUpdate() != infoUpdate;
    if (newInfo) {
      infoUpdate = sdk.sessionInfoUpdate();
      yaml = sdk.sessionInfo();
      // the sim can move to another event without restarting, keep one event per file
      int sub = parseSessionInfo(yaml).subSessionId;
      if (sub != subSession) rec.stop();
      subSession = sub;
    }
    if (sdk.schemaChanged() || !rec.active()) {
      rec.stop();
      if (!rec.start(outDir + "\\capture_" + stamp(), sdk.schema(), sdk.tickRate())) {
        fprintf(stderr, "cannot create capture file in %s\n", outDir.c_str());
        return 1;
      }
      newInfo = true;
      printf("recording %s\n", rec.currentPath().c_str());
    }
    if (newInfo) rec.setSessionInfo(yaml);
    rec.push(row.data(), {now - start, tick, infoUpdate});
    if (now - lastReport > 5) {
      lastReport = now;
      printf("rows=%d dropped=%d torn=%d\n", rec.written(), rec.dropped(), sdk.tornReads());
    }
  }
  rec.stop();
  printf("done rows=%d dropped=%d\n", rec.written(), rec.dropped());
  return 0;
}

int runIbt(const std::string& path, const std::string& outDir) {
  IbtReader r;
  if (!r.open(path)) {
    fprintf(stderr, "cannot read %s\n", path.c_str());
    return 1;
  }
  Inventory inv(r.schema());
  std::vector<uint8_t> row;
  while (r.next(row)) inv.add(row.data());
  std::string base = outDir + "\\ibt_inventory_" + stamp();
  FILE* f = fopen((base + ".tsv").c_str(), "w");
  if (!f) return 1;
  inv.write(f);
  fclose(f);
  printf("rows=%d vars=%zu bufLen=%d tickRate=%d -> %s.tsv\n", inv.rows(), r.schema().vars.size(), r.schema().bufLen,
         r.tickRate(), base.c_str());
  return 0;
}

// compact, sanitized excerpt of a capture holding only what the estimator reads
int runClip(const std::string& in, const std::string& out, int from, int to, const std::string& yamlPath) {
  IbtReader r;
  if (!r.open(in)) {
    fprintf(stderr, "cannot read %s\n", in.c_str());
    return 1;
  }
  irsdk::Schema compact;
  std::vector<std::pair<irsdk::VarHeader, irsdk::VarHeader>> copies;
  for (const auto& spec : FrameDecoder::kVars) {
    const irsdk::VarHeader* v = r.schema().find(spec.name);
    if (!v) continue;
    irsdk::VarHeader d = *v;
    d.offset = compact.bufLen;
    compact.bufLen += irsdk::typeSize(v->type) * v->count;
    compact.vars.push_back(d);
    copies.push_back({*v, d});
  }
  const irsdk::VarHeader* host = r.schema().find("CapHostTime");
  const irsdk::VarHeader* tick = r.schema().find("CapTickCount");
  const irsdk::VarHeader* info = r.schema().find("CapSessionInfoUpdate");
  IbtWriter w;
  if (!w.open(out, compact, r.tickRate())) {
    fprintf(stderr, "cannot write %s\n", out.c_str());
    return 1;
  }
  std::vector<uint8_t> row, small(compact.bufLen);
  for (int index = 0; index < to && r.next(row); ++index) {
    if (index < from) continue;
    for (const auto& [s, d] : copies) memcpy(small.data() + d.offset, row.data() + s.offset, irsdk::typeSize(s.type) * s.count);
    CaptureExtras ex{host ? irsdk::readAsDouble(row.data(), *host, 0) : index / 60.0,
                     tick ? (int)irsdk::readAsDouble(row.data(), *tick, 0) : index,
                     info ? (int)irsdk::readAsDouble(row.data(), *info, 0) : 0};
    w.write(small.data(), ex);
  }
  int rows = w.records();
  std::string yaml = r.sessionInfo();
  if (!yamlPath.empty()) {
    // a capture spanning two events needs the session string of the clipped one
    yaml.clear();
    if (FILE* y = fopen(yamlPath.c_str(), "rb")) {
      char buf[65536];
      for (size_t n; (n = fread(buf, 1, sizeof buf, y)) > 0;) yaml.append(buf, n);
      fclose(y);
    }
  }
  w.close(formatSessionInfo(parseSessionInfo(yaml)));
  printf("wrote %d rows of %d bytes to %s\n", rows, compact.bufLen, out.c_str());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  SetConsoleCtrlHandler(onCtrl, TRUE);
  std::string mode = argc > 1 ? argv[1] : "";
  std::string outDir = ".", file, file2, yaml;
  int from = 0, to = 0;
  double seconds = mode == "inventory" ? 10 : 1e12;
  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--out" && i + 1 < argc) outDir = argv[++i];
    else if (a == "--seconds" && i + 1 < argc) seconds = atof(argv[++i]);
    else if (a == "--from" && i + 1 < argc) from = atoi(argv[++i]);
    else if (a == "--yaml" && i + 1 < argc) yaml = argv[++i];
    else if (a == "--to" && i + 1 < argc) to = atoi(argv[++i]);
    else (file.empty() ? file : file2) = a;
  }
  if (mode != "clip") CreateDirectoryA(outDir.c_str(), nullptr);
  if (mode == "inventory") return runInventory(outDir, seconds);
  if (mode == "capture") return runCapture(outDir, seconds);
  if (mode == "ibt" && !file.empty()) return runIbt(file, outDir);
  if (mode == "clip" && !file2.empty() && to > from) return runClip(file, file2, from, to, yaml);
  fprintf(stderr,
          "usage:\n  irinspect inventory [--seconds N] [--out DIR]\n  irinspect capture [--seconds N] [--out DIR]\n"
          "  irinspect ibt FILE [--out DIR]\n  irinspect clip IN.ibt OUT.ibt --from ROW --to ROW [--yaml FILE]\n");
  return 2;
}
