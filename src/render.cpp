#include "render.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

// signed distance from (px, py) to a rounded rectangle, negative inside
float roundRectDistance(float px, float py, float x0, float y0, float x1, float y1, float r) {
  float cx = 0.5f * (x0 + x1), cy = 0.5f * (y0 + y1);
  float hx = 0.5f * (x1 - x0) - r, hy = 0.5f * (y1 - y0) - r;
  float qx = std::fabs(px - cx) - hx, qy = std::fabs(py - cy) - hy;
  float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
  return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - r;
}

struct Glyph {
  char ch;
  uint8_t rows[7];
};
constexpr Glyph kFont[] = {
    {'D', {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
};

}  // namespace

void Canvas::clear() {
  if (px_) memset(px_, 0, (size_t)w_ * h_ * 4);
}

void Canvas::blend(int x, int y, Color c, float coverage) {
  float a = c.a * std::clamp(coverage, 0.0f, 1.0f);
  if (a <= 0) return;
  uint32_t& d = px_[y * w_ + x];
  float inv = 1 - a;
  auto ch = [&](int shift, float s) {
    float v = s * a * 255.0f + ((d >> shift) & 255) * inv;
    return (uint32_t)std::min(255.0f, v + 0.5f) << shift;
  };
  d = ch(24, 1.0f) | ch(16, c.r) | ch(8, c.g) | ch(0, c.b);
}

template <class Coverage>
void Canvas::shade(float x0, float y0, float x1, float y1, Color c, Coverage cov) {
  int ix0 = std::max(0, (int)std::floor(x0) - 1), iy0 = std::max(0, (int)std::floor(y0) - 1);
  int ix1 = std::min(w_, (int)std::ceil(x1) + 1), iy1 = std::min(h_, (int)std::ceil(y1) + 1);
  for (int y = iy0; y < iy1; ++y)
    for (int x = ix0; x < ix1; ++x) blend(x, y, c, cov(x + 0.5f, y + 0.5f));
}

void Canvas::fillRoundRect(float x0, float y0, float x1, float y1, float r, Color c) {
  r = std::min({r, 0.5f * (x1 - x0), 0.5f * (y1 - y0)});
  shade(x0, y0, x1, y1, c, [&](float px, float py) { return 0.5f - roundRectDistance(px, py, x0, y0, x1, y1, r); });
}

void Canvas::fillRoundRectStriped(float x0, float y0, float x1, float y1, float r, float period, Color c) {
  r = std::min({r, 0.5f * (x1 - x0), 0.5f * (y1 - y0)});
  shade(x0, y0, x1, y1, c, [&](float px, float py) {
    float inside = 0.5f - roundRectDistance(px, py, x0, y0, x1, y1, r);
    float s = std::fmod(px + py, period);
    float stripe = std::clamp(std::min(s, period - s) - 0.25f * period + 0.5f, 0.0f, 1.0f);
    return std::min(inside, stripe);
  });
}

void Canvas::strokeRoundRect(float x0, float y0, float x1, float y1, float r, float width, Color c) {
  r = std::min({r, 0.5f * (x1 - x0), 0.5f * (y1 - y0)});
  float h = 0.5f * width;
  shade(x0, y0, x1, y1, c, [&](float px, float py) {
    float d = roundRectDistance(px, py, x0, y0, x1, y1, r);
    return h + 0.5f - std::fabs(d + h);
  });
}

void Canvas::line(float ax, float ay, float bx, float by, float width, Color c) {
  float dx = bx - ax, dy = by - ay, len2 = std::max(dx * dx + dy * dy, 1e-6f), h = 0.5f * width;
  shade(std::min(ax, bx) - h, std::min(ay, by) - h, std::max(ax, bx) + h, std::max(ay, by) + h, c,
        [&](float px, float py) {
          float t = std::clamp(((px - ax) * dx + (py - ay) * dy) / len2, 0.0f, 1.0f);
          float ex = px - (ax + t * dx), ey = py - (ay + t * dy);
          return h + 0.5f - std::sqrt(ex * ex + ey * ey);
        });
}

void Canvas::text(const char* s, float x, float y, float scale, Color c) {
  for (; *s; ++s, x += 6 * scale) {
    const Glyph* g = std::find_if(std::begin(kFont), std::end(kFont), [&](const Glyph& g) { return g.ch == *s; });
    if (g == std::end(kFont)) continue;
    for (int row = 0; row < 7; ++row)
      for (int col = 0; col < 5; ++col)
        if (g->rows[row] & (0x10 >> col))
          fillRoundRect(x + col * scale, y + row * scale, x + (col + 1) * scale, y + (row + 1) * scale, 0, c);
  }
}
