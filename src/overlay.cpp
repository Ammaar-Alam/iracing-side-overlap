// side-overlap: compact always-on-top side overlap overlay for iRacing
//   side-overlap.exe [--edit] [--demo] [--record DIR] [--stats] [--playback FILE.ibt [--from ROW]]
#include <windows.h>
#include <psapi.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "demo.h"
#include "estimator.h"
#include "frame.h"
#include "recorder.h"
#include "render.h"
#include "sdk_live.h"
#include "session_info.h"
#include "settings.h"
#include "timing.h"

namespace {

constexpr wchar_t kOverlayClass[] = L"IracingSideOverlap";
constexpr wchar_t kHelperClass[] = L"IracingSideOverlapTray";
constexpr UINT kTrayMsg = WM_APP + 1;
constexpr int kMinWidth = 40, kMinHeight = 60;

enum Command : UINT {
  kCmdEdit = 1,
  kCmdDemo,
  kCmdReset,
  kCmdSettings,
  kCmdExit,
  kCmdOpacity = 100,
  kCmdColor = 200,
  kCmdFps = 300,
};
constexpr int kOpacities[] = {100, 90, 75, 60, 45};
constexpr struct {
  const wchar_t* name;
  uint32_t rgb;
} kColors[] = {{L"Amber", 0xFFB300}, {L"Yellow", 0xFFE14D}, {L"Orange", 0xFF7A1A},
               {L"Cyan", 0x33D6FF},  {L"Lime", 0x9CFF3D},   {L"White", 0xF2F2F2}};
constexpr int kFpsChoices[] = {30, 60};

// what the last frame put on screen, a redraw happens only when this changes
struct DrawKey {
  int feed = -1, left[4] = {}, right[4] = {}, w = 0, h = 0, flags = 0;
  bool operator==(const DrawKey& o) const {
    return feed == o.feed && std::equal(left, left + 4, o.left) && std::equal(right, right + 4, o.right) &&
           w == o.w && h == o.h && flags == o.flags;
  }
};

struct Stats {
  bool enabled = false;
  double start = 0, firstPresent = -1;
  int ticks = 0, missedTicks = 0, lastTick = -1, presents = 0;
  std::vector<float> latencyMs, updateUs, drawUs;
};

struct App {
  HINSTANCE inst = nullptr;
  HWND hwnd = nullptr, helper = nullptr;
  std::wstring iniPath;
  Settings s;
  bool edit = false, demo = false;
  UINT dpi = 96;

  HDC memDC = nullptr;
  HBITMAP dib = nullptr;
  HGDIOBJ oldBmp = nullptr;
  uint32_t* bits = nullptr;
  int w = 0, h = 0;
  Canvas canvas;
  DrawKey drawn;
  double lastPresent = 0;

  LiveSdk sdk;
  FrameDecoder decoder;
  Estimator est;
  Estimator demoEst;
  Frame frame, demoFrameData;
  std::vector<uint8_t> row;
  int infoUpdate = -1, subSession = -1;
  double demoStart = 0, lastTickTime = 0;

  Recorder rec;

  IbtReader playback;
  const irsdk::VarHeader* playbackClock = nullptr;
  bool playing = false, rowPending = false;
  double playbackStart = 0, firstRowTime = -1, rowTime = 0;
  std::string recordDir;
  double recordStart = 0;

  NOTIFYICONDATAW nid{};
  UINT taskbarCreated = 0;
  HICON icon = nullptr;
  Stats stats;
};
App g;

View currentView(double now);

std::string narrowPath(const std::wstring& w) {
  std::string s(WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), (int)s.size(), nullptr, nullptr);
  s.pop_back();
  return s;
}

EstimatorConfig estimatorConfig() {
  EstimatorConfig c;
  c.padding = spotterPadding();
  return c;
}

// ---------- drawing ----------
// Each side is a capsule track standing for our car, front at the top. The fill marks the part
// of our car the other car covers on that side, so it grows up from the bottom as a car arrives
// at the rear quarter and leaves through the top as it clears ahead.

struct Layout {
  float trackW, leftX, rightX, top, bottom, unit;
};

Layout layoutFor(int w, int h) {
  Layout l;
  l.unit = g.dpi / 96.0f;
  float pad = 2 * l.unit;
  l.trackW = std::max(8.0f, std::round(std::min(w * 0.2f, h * 0.24f)));
  l.leftX = pad;
  l.rightX = w - pad - l.trackW;
  l.top = pad;
  l.bottom = h - pad;
  return l;
}

// vertical pixel span of the covered part of our car, at least a sliver at the end the car is on
bool fillSpan(const SideView& sv, const Layout& l, float myLength, float& y0, float& y1) {
  if (!sv.positioned) return false;
  float half = 0.5f * myLength, height = l.bottom - l.top;
  float front = std::clamp(sv.front, -half, half), rear = std::clamp(sv.rear, -half, half);
  y0 = l.top + (half - front) / myLength * height;
  y1 = l.top + (half - rear) / myLength * height;
  float sliver = std::max(l.trackW, 0.2f * height);
  if (y1 - y0 < sliver) {
    if (sv.front + sv.rear < 0) y0 = std::max(l.top, y1 - sliver);
    else y1 = std::min(l.bottom, y0 + sliver);
    if (y1 - y0 < sliver) (y0 == l.top ? y1 : y0) = y0 == l.top ? l.top + sliver : l.bottom - sliver;
  }
  return true;
}

void sideKey(const SideView& sv, const Layout& l, float myLength, int out[4]) {
  float y0 = 0, y1 = 0;
  out[0] = sv.reported;
  out[1] = fillSpan(sv, l, myLength, y0, y1);
  out[2] = (int)std::lround(y0 * 2);
  out[3] = (int)std::lround(y1 * 2);
}

void drawSide(Canvas& c, const SideView& sv, const Layout& l, float x, float myLength, Color bar, Color track) {
  float w = l.trackW, r = 0.5f * w;
  c.fillRoundRect(x, l.top, x + w, l.bottom, r, rgb(0x0c0f12, 0.6f));
  c.strokeRoundRect(x, l.top, x + w, l.bottom, r, 1.2f * l.unit, track);
  if (sv.reported == 0) return;
  float inset = 2.5f * l.unit, x0 = x + inset, x1 = x + w - inset, ri = r - inset;
  float y0, y1;
  if (fillSpan(sv, l, myLength, y0, y1)) {
    y0 += inset;
    y1 -= inset;
    if (sv.reported > 1) {
      // two cars on this side: the fill splits in two
      float mid = 0.5f * (x0 + x1), gap = std::max(1.5f * l.unit, 0.12f * w);
      c.fillRoundRect(x0, y0, mid - 0.5f * gap, y1, 0.5f * (mid - x0), bar);
      c.fillRoundRect(mid + 0.5f * gap, y0, x1, y1, 0.5f * (mid - x0), bar);
    } else {
      c.fillRoundRect(x0, y0, x1, y1, ri, bar);
    }
    return;
  }
  // occupied but not placeable: the whole track is striped
  c.fillRoundRectStriped(x0, l.top + inset, x1, l.bottom - inset, ri, 7 * l.unit, bar);
}

void present(const View& v, double frameTime, bool force) {
  bool showDemo = g.demo || g.edit || g.playing;
  Layout l = layoutFor(g.w, g.h);
  DrawKey key;
  key.feed = (int)v.feed;
  key.w = g.w;
  key.h = g.h;
  key.flags = (g.edit ? 1 : 0) | (showDemo ? 2 : 0);
  sideKey(v.left, l, v.myLength, key.left);
  sideKey(v.right, l, v.myLength, key.right);
  if (!force && key == g.drawn) return;
  double now = hostSeconds();
  // the margin absorbs tick jitter so a 60 Hz cap never drops a 60 Hz telemetry update
  if (!force && now - g.lastPresent < 0.7 / g.s.fps) return;

  Canvas& c = g.canvas;
  c.clear();
  if (g.edit) {
    c.fillRoundRect(0, 0, (float)g.w, (float)g.h, 6 * l.unit, rgb(0x101418, 0.45f));
    c.strokeRoundRect(0, 0, (float)g.w, (float)g.h, 6 * l.unit, 1.0f * l.unit, rgb(0xFFFFFF, 0.5f));
  }
  if (showDemo) c.text("DEMO", 0.5f * g.w - 17.5f * l.unit, 4 * l.unit, 1.5f * l.unit, rgb(0xFFFFFF, 0.8f));
  if (v.feed == Feed::kUnavailable) {
    Color u = rgb(g.s.unavailableColor, 0.85f);
    for (float x : {l.leftX, l.rightX}) {
      c.strokeRoundRect(x, l.top, x + l.trackW, l.bottom, 0.5f * l.trackW, 1.5f * l.unit, u);
      c.line(x, l.bottom - 0.3f * l.trackW, x + l.trackW, l.top + 0.3f * l.trackW, 1.5f * l.unit, u);
    }
  } else if (v.feed == Feed::kLive) {
    Color bar = rgb(g.s.barColor), track = rgb(g.s.trackColor, 0.35f);
    drawSide(c, v.left, l, l.leftX, v.myLength, bar, track);
    drawSide(c, v.right, l, l.rightX, v.myLength, bar, track);
  }

  POINT src{0, 0};
  SIZE size{g.w, g.h};
  BLENDFUNCTION bf{AC_SRC_OVER, 0, (BYTE)std::clamp(g.s.opacity * 255 / 100, 10, 255), AC_SRC_ALPHA};
  UpdateLayeredWindow(g.hwnd, nullptr, nullptr, &size, g.memDC, &src, 0, &bf, ULW_ALPHA);
  double t1 = hostSeconds();
  g.drawn = key;
  g.lastPresent = t1;
  if (g.stats.enabled) {
    if (g.stats.firstPresent < 0) g.stats.firstPresent = t1;
    ++g.stats.presents;
    g.stats.drawUs.push_back((float)((t1 - now) * 1e6));
    if (frameTime > 0 && g.stats.latencyMs.size() < 2000000) g.stats.latencyMs.push_back((float)((t1 - frameTime) * 1e3));
  }
}

void resizeSurface(int w, int h) {
  if (g.dib && w == g.w && h == g.h) return;
  if (g.memDC) {
    SelectObject(g.memDC, g.oldBmp);
    DeleteObject(g.dib);
  } else {
    g.memDC = CreateCompatibleDC(nullptr);
  }
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof bi.bmiHeader;
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  void* bits = nullptr;
  g.dib = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  g.oldBmp = SelectObject(g.memDC, g.dib);
  g.bits = static_cast<uint32_t*>(bits);
  g.w = w;
  g.h = h;
  g.canvas.attach(g.bits, w, h);
}

// ---------- window placement ----------

RECT defaultRect(UINT dpi) {
  // centred under the virtual mirror on the primary monitor
  MONITORINFO mi{};
  mi.cbSize = sizeof mi;
  GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
  int w = MulDiv(g.s.width, dpi, 96), h = MulDiv(g.s.height, dpi, 96);
  const RECT& wa = mi.rcWork;
  int x = (wa.left + wa.right - w) / 2, y = wa.top + (wa.bottom - wa.top) * 18 / 100;
  return {x, y, x + w, y + h};
}

UINT dpiAt(POINT p) {
  UINT dpi = 96;
  if (HMONITOR m = MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST)) {
    typedef HRESULT(WINAPI * GetDpiForMonitorFn)(HMONITOR, int, UINT*, UINT*);
    static auto fn = (GetDpiForMonitorFn)(void*)GetProcAddress(LoadLibraryW(L"shcore.dll"), "GetDpiForMonitor");
    UINT dx = 96, dy = 96;
    if (fn && SUCCEEDED(fn(m, 0, &dx, &dy))) dpi = dx;
  }
  return dpi;
}

// the window must keep a usable part on some monitor, otherwise it returns to the default spot
bool onScreen(const RECT& r) {
  RECT probe{r.left + (r.right - r.left) / 4, r.top + (r.bottom - r.top) / 4, r.right - (r.right - r.left) / 4,
             r.bottom - (r.bottom - r.top) / 4};
  HMONITOR m = MonitorFromRect(&probe, MONITOR_DEFAULTTONULL);
  if (!m) return false;
  MONITORINFO mi{};
  mi.cbSize = sizeof mi;
  GetMonitorInfoW(m, &mi);
  RECT inter;
  return IntersectRect(&inter, &probe, &mi.rcMonitor) && EqualRect(&inter, &probe);
}

void savePlacement() {
  RECT r;
  GetWindowRect(g.hwnd, &r);
  g.s.x = r.left;
  g.s.y = r.top;
  g.s.width = std::max(kMinWidth, MulDiv(r.right - r.left, 96, g.dpi));
  g.s.height = std::max(kMinHeight, MulDiv(r.bottom - r.top, 96, g.dpi));
  g.s.save(g.iniPath);
}

void place(const RECT& r) {
  SetWindowPos(g.hwnd, HWND_TOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE);
  resizeSurface(r.right - r.left, r.bottom - r.top);
  g.drawn = {};
}

void resetPosition() {
  g.dpi = dpiAt({0, 0});
  place(defaultRect(g.dpi));
  savePlacement();
}

void ensureOnScreen() {
  RECT r;
  GetWindowRect(g.hwnd, &r);
  if (!onScreen(r)) resetPosition();
}

void setEdit(bool on) {
  g.edit = on;
  LONG_PTR ex = GetWindowLongPtrW(g.hwnd, GWL_EXSTYLE);
  ex = on ? ex & ~WS_EX_TRANSPARENT : ex | WS_EX_TRANSPARENT;
  SetWindowLongPtrW(g.hwnd, GWL_EXSTYLE, ex);
  if (!on) savePlacement();
  g.demoStart = hostSeconds();
  g.demoEst.reset();
  g.drawn = {};
}

// ---------- tray ----------

HICON makeIcon() {
  constexpr int n = 32;
  std::vector<uint32_t> px(n * n, 0);
  Canvas c;
  c.attach(px.data(), n, n);
  c.fillRoundRect(11, 6, 21, 26, 3.5f, rgb(0xFFFFFF, 0.55f));
  c.fillRoundRect(2, 11, 8, 29, 3, rgb(0xFFB300));
  c.fillRoundRect(24, 3, 30, 17, 3, rgb(0xFFB300));
  ICONINFO ii{};
  ii.fIcon = TRUE;
  BITMAPINFO bi{};
  bi.bmiHeader = {sizeof(BITMAPINFOHEADER), n, -n, 1, 32, BI_RGB, 0, 0, 0, 0, 0};
  void* bits = nullptr;
  ii.hbmColor = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  memcpy(bits, px.data(), px.size() * 4);
  ii.hbmMask = CreateBitmap(n, n, 1, 1, nullptr);
  HICON icon = CreateIconIndirect(&ii);
  DeleteObject(ii.hbmColor);
  DeleteObject(ii.hbmMask);
  return icon;
}

void addTrayIcon() {
  g.nid = {};
  g.nid.cbSize = sizeof g.nid;
  g.nid.hWnd = g.helper;
  g.nid.uID = 1;
  g.nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  g.nid.uCallbackMessage = kTrayMsg;
  g.nid.hIcon = g.icon;
  wcscpy(g.nid.szTip, L"iRacing Side Overlap");
  Shell_NotifyIconW(NIM_ADD, &g.nid);
}

void showMenu() {
  HMENU opacity = CreatePopupMenu(), color = CreatePopupMenu(), fps = CreatePopupMenu();
  for (int i = 0; i < (int)std::size(kOpacities); ++i) {
    wchar_t label[16];
    swprintf(label, 16, L"%d%%", kOpacities[i]);
    AppendMenuW(opacity, MF_STRING | (g.s.opacity == kOpacities[i] ? MF_CHECKED : 0), kCmdOpacity + i, label);
  }
  for (int i = 0; i < (int)std::size(kColors); ++i)
    AppendMenuW(color, MF_STRING | (g.s.barColor == kColors[i].rgb ? MF_CHECKED : 0), kCmdColor + i, kColors[i].name);
  for (int i = 0; i < (int)std::size(kFpsChoices); ++i) {
    wchar_t label[16];
    swprintf(label, 16, L"%d Hz", kFpsChoices[i]);
    AppendMenuW(fps, MF_STRING | (g.s.fps == kFpsChoices[i] ? MF_CHECKED : 0), kCmdFps + i, label);
  }
  HMENU m = CreatePopupMenu();
  AppendMenuW(m, MF_STRING | (g.edit ? MF_CHECKED : 0), kCmdEdit, L"Edit layout");
  AppendMenuW(m, MF_STRING | (g.demo ? MF_CHECKED : 0), kCmdDemo, L"Demo");
  AppendMenuW(m, MF_STRING, kCmdReset, L"Reset position");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_POPUP, (UINT_PTR)opacity, L"Opacity");
  AppendMenuW(m, MF_POPUP, (UINT_PTR)color, L"Color");
  AppendMenuW(m, MF_POPUP, (UINT_PTR)fps, L"Refresh rate");
  AppendMenuW(m, MF_STRING, kCmdSettings, L"Open settings file");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, kCmdExit, L"Exit");
  POINT p;
  GetCursorPos(&p);
  SetForegroundWindow(g.helper);
  UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, p.x, p.y, 0, g.helper, nullptr);
  PostMessageW(g.helper, WM_NULL, 0, 0);
  DestroyMenu(m);

  if (cmd == kCmdEdit) setEdit(!g.edit);
  else if (cmd == kCmdDemo) {
    g.demo = !g.demo;
    g.demoStart = hostSeconds();
    g.demoEst.reset();
  } else if (cmd == kCmdReset) resetPosition();
  else if (cmd == kCmdSettings) {
    g.s.save(g.iniPath);
    ShellExecuteW(nullptr, L"open", L"notepad.exe", g.iniPath.c_str(), nullptr, SW_SHOWNORMAL);
  } else if (cmd == kCmdExit) PostMessageW(g.hwnd, WM_CLOSE, 0, 0);
  else if (cmd >= kCmdOpacity && cmd < kCmdOpacity + std::size(kOpacities)) g.s.opacity = kOpacities[cmd - kCmdOpacity];
  else if (cmd >= kCmdColor && cmd < kCmdColor + std::size(kColors)) g.s.barColor = kColors[cmd - kCmdColor].rgb;
  else if (cmd >= kCmdFps && cmd < kCmdFps + std::size(kFpsChoices)) g.s.fps = kFpsChoices[cmd - kCmdFps];
  if (cmd >= kCmdOpacity) g.s.save(g.iniPath);
  g.drawn = {};
}

// ---------- telemetry ----------

void startRecording() {
  if (g.recordDir.empty()) return;
  SYSTEMTIME t;
  GetLocalTime(&t);
  char name[64];
  snprintf(name, sizeof name, "\\overlay_%04d%02d%02d_%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
           t.wSecond);
  g.rec.start(g.recordDir + name, g.sdk.schema(), g.sdk.tickRate());
  g.recordStart = hostSeconds();
}

void pollTelemetry(double now) {
  if (!g.sdk.isOpen()) {
    static double nextTry = 0;
    if (now < nextTry) return;
    nextTry = now + 1.0;
    if (!g.sdk.open()) return;
    g.lastTickTime = now;  // give a loading sim time to publish its first tick
  }
  int tick = 0;
  if (!g.sdk.readLatest(g.row, tick)) {
    if (now - g.lastTickTime > 10) {
      // the sim exited or died without clearing its status, reopen fresh when it returns
      g.sdk.close();
      g.rec.stop();
      g.infoUpdate = g.subSession = -1;
    }
    return;
  }
  g.lastTickTime = now;
  double t0 = hostSeconds();
  if (g.sdk.schemaChanged()) {
    g.decoder.bind(g.sdk.schema());
    g.infoUpdate = g.subSession = -1;
    g.rec.stop();
    startRecording();
  }
  if (g.sdk.sessionInfoUpdate() != g.infoUpdate) {
    g.infoUpdate = g.sdk.sessionInfoUpdate();
    std::string yaml = g.sdk.sessionInfo();
    SessionInfo info = parseSessionInfo(yaml);
    g.est.setGeometry(makeGeometry(info, g.s.carLengths));
    if (g.rec.active()) {
      // a new event without a sim restart starts a new recording
      if (g.subSession != -1 && info.subSessionId != g.subSession) {
        g.rec.stop();
        startRecording();
      }
      g.rec.setSessionInfo(yaml);
    }
    g.subSession = info.subSessionId;
  }
  g.decoder.decode(g.row.data(), g.frame);
  g.frame.hostTime = now;
  g.frame.tick = tick;
  g.est.update(g.frame);
  if (g.rec.active()) g.rec.push(g.row.data(), {now - g.recordStart, tick, g.infoUpdate});
  if (g.stats.enabled) {
    if (g.stats.lastTick >= 0 && tick - g.stats.lastTick > 1) g.stats.missedTicks += tick - g.stats.lastTick - 1;
    g.stats.lastTick = tick;
    ++g.stats.ticks;
    if (g.stats.updateUs.size() < 2000000) g.stats.updateUs.push_back((float)((hostSeconds() - t0) * 1e6));
  }
}

void writeStats() {
  if (!g.stats.enabled) return;
  auto pct = [](std::vector<float> v, double p) -> double {
    if (v.empty()) return NAN;
    std::sort(v.begin(), v.end());
    return (double)v[std::min(v.size() - 1, (size_t)(p * (v.size() - 1) + 0.5))];
  };
  FILETIME create, exit, kernel, user;
  GetProcessTimes(GetCurrentProcess(), &create, &exit, &kernel, &user);
  auto ft = [](FILETIME f) { return (double)(((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime) * 1e-7; };
  PROCESS_MEMORY_COUNTERS_EX pm{};
  GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof pm);
  double up = hostSeconds() - g.stats.start;
  std::wstring path = g.iniPath.substr(0, g.iniPath.size() - 4) + L"-stats.txt";
  FILE* f = _wfopen(path.c_str(), L"a");
  if (!f) return;
  fprintf(f, "uptime_s=%.1f ticks=%d missed_ticks=%d presents=%d fps_cap=%d\n", up, g.stats.ticks,
          g.stats.missedTicks, g.stats.presents, g.s.fps);
  fprintf(f, "cpu_s=%.3f cpu_pct_one_core=%.3f peak_working_set_kb=%zu private_kb=%zu\n", ft(kernel) + ft(user),
          100 * (ft(kernel) + ft(user)) / up, pm.PeakWorkingSetSize / 1024, pm.PrivateUsage / 1024);
  fprintf(f, "startup_to_first_present_ms=%.1f\n", (g.stats.firstPresent - g.stats.start) * 1e3);
  fprintf(f, "telemetry_to_present_ms p50=%.3f p95=%.3f p99=%.3f max=%.3f n=%zu\n", pct(g.stats.latencyMs, .5),
          pct(g.stats.latencyMs, .95), pct(g.stats.latencyMs, .99), pct(g.stats.latencyMs, 1), g.stats.latencyMs.size());
  fprintf(f, "update_us p50=%.1f p99=%.1f draw_present_us p50=%.1f p99=%.1f\n\n", pct(g.stats.updateUs, .5),
          pct(g.stats.updateUs, .99), pct(g.stats.drawUs, .5), pct(g.stats.drawUs, .99));
  fclose(f);
}

// ---------- windows ----------

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_NCHITTEST: {
      if (hwnd != g.hwnd || !g.edit) break;
      POINT p{(short)LOWORD(lp), (short)HIWORD(lp)};
      RECT r;
      GetWindowRect(hwnd, &r);
      int e = MulDiv(8, g.dpi, 96);
      bool L = p.x < r.left + e, R = p.x >= r.right - e, T = p.y < r.top + e, B = p.y >= r.bottom - e;
      if (T && L) return HTTOPLEFT;
      if (T && R) return HTTOPRIGHT;
      if (B && L) return HTBOTTOMLEFT;
      if (B && R) return HTBOTTOMRIGHT;
      if (L) return HTLEFT;
      if (R) return HTRIGHT;
      if (T) return HTTOP;
      if (B) return HTBOTTOM;
      return HTCAPTION;
    }
    case WM_GETMINMAXINFO: {
      auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
      mm->ptMinTrackSize = {MulDiv(kMinWidth, g.dpi, 96), MulDiv(kMinHeight, g.dpi, 96)};
      return 0;
    }
    case WM_SIZE:
      if (hwnd == g.hwnd && LOWORD(lp) > 0 && HIWORD(lp) > 0) {
        resizeSurface(LOWORD(lp), HIWORD(lp));
        present(currentView(hostSeconds()), 0, true);
      }
      return 0;
    case WM_EXITSIZEMOVE:
      savePlacement();
      return 0;
    case WM_NCRBUTTONUP:
    case WM_RBUTTONUP:
      showMenu();
      return 0;
    case WM_DPICHANGED: {
      g.dpi = HIWORD(wp);
      place(*reinterpret_cast<RECT*>(lp));
      return 0;
    }
    case WM_DISPLAYCHANGE:
      ensureOnScreen();
      return 0;
    case kTrayMsg:
      if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) showMenu();
      return 0;
    case WM_CLOSE:
      // RaceLab's graceful stop and taskkill both arrive here, for either window
      if (IsWindow(g.hwnd)) {
        savePlacement();
        DestroyWindow(g.hwnd);
      }
      return 0;
    case WM_QUERYENDSESSION:
      return TRUE;
    case WM_ENDSESSION:
      if (wp && IsWindow(g.hwnd)) {
        savePlacement();
        DestroyWindow(g.hwnd);
      }
      return 0;
    case WM_DESTROY:
      if (hwnd == g.hwnd) PostQuitMessage(0);
      return 0;
    default:
      if (msg == g.taskbarCreated && msg != 0) {
        addTrayIcon();
        return 0;
      }
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

// what the window should show right now
View currentView(double now) {
  if (g.demo || g.edit) return g.demoEst.view(now);
  if (g.playing) return g.est.view(now);
  if (!g.sdk.isOpen() || now - g.lastTickTime > 10) return {};
  return g.est.view(now);
}

// feeds recorded rows through the live pipeline at their original pace, looping at the end
void pollPlayback(double now) {
  for (;;) {
    if (!g.rowPending) {
      if (!g.playback.next(g.row)) {
        g.playback.rewind();
        g.est.reset();
        g.playbackStart = now;
        g.firstRowTime = -1;
        return;
      }
      double t = g.playbackClock ? irsdk::readAsDouble(g.row.data(), *g.playbackClock, 0) : 0;
      if (g.firstRowTime < 0) g.firstRowTime = t;
      g.rowTime = t - g.firstRowTime;
      g.rowPending = true;
    }
    if (g.rowTime > now - g.playbackStart) return;
    g.decoder.decode(g.row.data(), g.frame);
    g.frame.hostTime = now;
    g.est.update(g.frame);
    g.rowPending = false;
  }
}

void runLoop() {
  MSG msg;
  for (;;) {
    double now = hostSeconds();
    bool animated = g.demo || g.edit || g.playing;
    DWORD timeout = animated ? 8 : g.sdk.connected() ? 50 : 500;
    HANDLE ev = g.sdk.dataEvent();
    MsgWaitForMultipleObjectsEx(ev ? 1 : 0, ev ? &ev : nullptr, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) return;
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    now = hostSeconds();
    if (g.playing) pollPlayback(now);
    else pollTelemetry(now);
    if (g.demo || g.edit) {
      if (demoFrame(now - g.demoStart, g.demoFrameData)) {
        g.demoFrameData.hostTime = now;
        g.demoEst.update(g.demoFrameData);
      }
    }
    present(currentView(now), animated ? 0 : g.frame.hostTime, false);
  }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
  g.stats.start = hostSeconds();
  HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\IracingSideOverlap");
  if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  bool startEdit = false;
  std::string playbackPath;
  int playbackFrom = 0;
  for (int i = 1; i < argc; ++i) {
    std::wstring a = argv[i];
    if (a == L"--edit") startEdit = true;
    else if (a == L"--demo") g.demo = true;
    else if (a == L"--stats") g.stats.enabled = true;
    else if (a == L"--record" && i + 1 < argc) g.recordDir = narrowPath(argv[++i]);
    else if (a == L"--playback" && i + 1 < argc) playbackPath = narrowPath(argv[++i]);
    else if (a == L"--from" && i + 1 < argc) playbackFrom = _wtoi(argv[++i]);
  }
  LocalFree(argv);
  if (!g.recordDir.empty()) CreateDirectoryA(g.recordDir.c_str(), nullptr);

  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  g.iniPath = exe;
  g.iniPath = g.iniPath.substr(0, g.iniPath.find_last_of(L'.')) + L".ini";
  g.s.load(g.iniPath);
  g.s.fps = std::clamp(g.s.fps, 10, 240);
  g.s.opacity = std::clamp(g.s.opacity, 10, 100);
  g.est = Estimator(estimatorConfig());
  g.demoEst = Estimator(estimatorConfig());
  g.demoEst.setGeometry(demoGeometry());
  if (!playbackPath.empty() && g.playback.open(playbackPath)) {
    g.decoder.bind(g.playback.schema());
    g.est.setGeometry(makeGeometry(parseSessionInfo(g.playback.sessionInfo()), g.s.carLengths));
    g.playbackClock = g.playback.schema().find("CapHostTime");
    if (!g.playbackClock) g.playbackClock = g.playback.schema().find("SessionTime");
    for (int i = 0; i < playbackFrom; ++i) g.playback.next(g.row);
    g.playing = true;
    g.playbackStart = hostSeconds();
  }
  g.inst = inst;
  g.icon = makeIcon();
  g.taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof wc;
  wc.lpfnWndProc = wndProc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(nullptr, IDC_SIZEALL);
  wc.hIcon = g.icon;
  wc.lpszClassName = kOverlayClass;
  RegisterClassExW(&wc);
  wc.lpszClassName = kHelperClass;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  RegisterClassExW(&wc);

  g.helper = CreateWindowExW(WS_EX_TOOLWINDOW, kHelperClass, L"iRacing Side Overlap", WS_POPUP, 0, 0, 0, 0, nullptr,
                             nullptr, inst, nullptr);
  g.dpi = g.s.x == INT_MIN ? dpiAt({0, 0}) : dpiAt({g.s.x, g.s.y});
  RECT r = defaultRect(g.dpi);
  if (g.s.x != INT_MIN) {
    RECT saved{g.s.x, g.s.y, g.s.x + MulDiv(g.s.width, g.dpi, 96), g.s.y + MulDiv(g.s.height, g.dpi, 96)};
    if (onScreen(saved)) r = saved;
  }
  g.hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                           kOverlayClass, L"iRacing Side Overlap", WS_POPUP, r.left, r.top, r.right - r.left,
                           r.bottom - r.top, nullptr, nullptr, inst, nullptr);
  if (!g.hwnd) return 1;
  g.dpi = GetDpiForWindow(g.hwnd);
  resizeSurface(r.right - r.left, r.bottom - r.top);
  present(currentView(hostSeconds()), 0, true);
  ShowWindow(g.hwnd, SW_SHOWNOACTIVATE);
  addTrayIcon();
  if (startEdit) setEdit(true);
  g.demoStart = hostSeconds();

  runLoop();

  Shell_NotifyIconW(NIM_DELETE, &g.nid);
  g.rec.stop();
  writeStats();
  DestroyWindow(g.helper);
  DestroyIcon(g.icon);
  ReleaseMutex(mutex);
  CloseHandle(mutex);
  return 0;
}
