#pragma once
#include <windows.h>

// Monotonic seconds from QueryPerformanceCounter
inline double hostSeconds() {
  static const double inv = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return 1.0 / (double)f.QuadPart;
  }();
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return (double)c.QuadPart * inv;
}
