// iRacing telemetry layout shared by the live memory map and .ibt files
// Field layout follows the published irsdk_defines.h
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace irsdk {

constexpr int kMaxBufs = 4;
constexpr int kStatusConnected = 1;
constexpr const wchar_t* kMemMapName = L"Local\\IRSDKMemMapFileName";
constexpr const wchar_t* kDataValidEventName = L"Local\\IRSDKDataValidEvent";

enum VarType : int32_t { kChar = 0, kBool, kInt, kBitField, kFloat, kDouble };

struct VarBuf {
  int32_t tickCount;
  int32_t bufOffset;
  int32_t pad[2];
};

struct Header {
  int32_t ver;
  int32_t status;
  int32_t tickRate;
  int32_t sessionInfoUpdate;
  int32_t sessionInfoLen;
  int32_t sessionInfoOffset;
  int32_t numVars;
  int32_t varHeaderOffset;
  int32_t numBuf;
  int32_t bufLen;
  int32_t pad1[2];
  VarBuf varBuf[kMaxBufs];
};

// only present in .ibt files, directly after Header
struct DiskSubHeader {
  int64_t sessionStartDate;
  double sessionStartTime;
  double sessionEndTime;
  int32_t sessionLapCount;
  int32_t sessionRecordCount;
};

struct VarHeader {
  int32_t type;
  int32_t offset;
  int32_t count;
  bool countAsTime;
  char pad[3];
  char name[32];
  char desc[64];
  char unit[32];
};

static_assert(sizeof(Header) == 112);
static_assert(sizeof(DiskSubHeader) == 32);
static_assert(sizeof(VarHeader) == 144);

enum CarLeftRight : int32_t {
  kLROff = 0,
  kLRClear = 1,
  kLRCarLeft = 2,
  kLRCarRight = 3,
  kLRCarLeftRight = 4,
  kLR2CarsLeft = 5,
  kLR2CarsRight = 6,
};

enum TrkLoc : int32_t {
  kNotInWorld = -1,
  kOffTrack = 0,
  kInPitStall = 1,
  kApproachingPits = 2,
  kOnTrack = 3,
};

inline int typeSize(int32_t t) {
  switch (t) {
    case kChar: case kBool: return 1;
    case kInt: case kBitField: case kFloat: return 4;
    case kDouble: return 8;
    default: return 0;
  }
}

inline const char* typeName(int32_t t) {
  static const char* names[] = {"char", "bool", "int", "bitfield", "float", "double"};
  return t >= 0 && t <= kDouble ? names[t] : "?";
}

inline std::string fixedStr(const char* s, size_t n) { return std::string(s, strnlen(s, n)); }

// Variable descriptors for one telemetry stream
struct Schema {
  std::vector<VarHeader> vars;
  int bufLen = 0;

  const VarHeader* find(std::string_view name) const {
    for (const auto& v : vars)
      if (name == std::string_view(v.name, strnlen(v.name, sizeof v.name))) return &v;
    return nullptr;
  }
};

// Typed element read from a snapshot row, converted to double
inline double readAsDouble(const uint8_t* row, const VarHeader& v, int i) {
  const uint8_t* p = row + v.offset + i * typeSize(v.type);
  switch (v.type) {
    case kChar: return (double)(int8_t)*p;
    case kBool: return *p ? 1.0 : 0.0;
    case kInt: { int32_t x; memcpy(&x, p, 4); return x; }
    case kBitField: { uint32_t x; memcpy(&x, p, 4); return x; }
    case kFloat: { float x; memcpy(&x, p, 4); return x; }
    case kDouble: { double x; memcpy(&x, p, 8); return x; }
    default: return 0.0;
  }
}

}  // namespace irsdk
