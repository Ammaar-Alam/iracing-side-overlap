// Antialiased shape drawing into a premultiplied BGRA buffer
#pragma once
#include <cstdint>

struct Color {
  float r, g, b, a;  // straight alpha, 0..1
};

inline Color rgb(uint32_t hex, float a = 1.0f) {
  return {((hex >> 16) & 255) / 255.0f, ((hex >> 8) & 255) / 255.0f, (hex & 255) / 255.0f, a};
}

class Canvas {
 public:
  void attach(uint32_t* pixels, int w, int h) {
    px_ = pixels;
    w_ = w;
    h_ = h;
  }
  int width() const { return w_; }
  int height() const { return h_; }
  void clear();
  void fillRoundRect(float x0, float y0, float x1, float y1, float r, Color c);
  void strokeRoundRect(float x0, float y0, float x1, float y1, float r, float width, Color c);
  void line(float ax, float ay, float bx, float by, float width, Color c);
  // diagonal stripes clipped to a rounded rectangle
  void fillRoundRectStriped(float x0, float y0, float x1, float y1, float r, float period, Color c);
  // tiny 5x7 pixel font, only the glyphs the overlay labels use
  void text(const char* s, float x, float y, float scale, Color c);

 private:
  void blend(int x, int y, Color c, float coverage);
  template <class Coverage>
  void shade(float x0, float y0, float x1, float y1, Color c, Coverage cov);

  uint32_t* px_ = nullptr;
  int w_ = 0, h_ = 0;
};
