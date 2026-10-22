#include "sdk_live.h"

#include <atomic>

bool LiveSdk::open() {
  if (base_) return true;
  map_ = OpenFileMappingW(FILE_MAP_READ, FALSE, irsdk::kMemMapName);
  if (!map_) return false;
  base_ = static_cast<const uint8_t*>(MapViewOfFile(map_, FILE_MAP_READ, 0, 0, 0));
  event_ = OpenEventW(SYNCHRONIZE, FALSE, irsdk::kDataValidEventName);
  if (!base_ || !event_) {
    close();
    return false;
  }
  boundNumVars_ = boundBufLen_ = boundVarOffset_ = -1;
  lastTick_ = INT32_MIN;
  return true;
}

void LiveSdk::close() {
  if (base_) UnmapViewOfFile(base_);
  if (map_) CloseHandle(map_);
  if (event_) CloseHandle(event_);
  base_ = nullptr;
  map_ = event_ = nullptr;
  schema_ = {};
}

void LiveSdk::bindSchema() {
  auto* h = hdr();
  boundNumVars_ = h->numVars;
  boundBufLen_ = h->bufLen;
  boundVarOffset_ = h->varHeaderOffset;
  schema_.bufLen = boundBufLen_;
  schema_.vars.resize(boundNumVars_ > 0 ? boundNumVars_ : 0);
  if (!schema_.vars.empty())
    memcpy(schema_.vars.data(), base_ + boundVarOffset_, schema_.vars.size() * sizeof(irsdk::VarHeader));
  schemaChanged_ = true;
}

bool LiveSdk::readLatest(std::vector<uint8_t>& row, int& tick) {
  if (!connected()) return false;
  auto* h = hdr();
  if (h->numVars != boundNumVars_ || h->bufLen != boundBufLen_ || h->varHeaderOffset != boundVarOffset_) bindSchema();
  for (int attempt = 0; attempt < 3; ++attempt) {
    int latest = 0;
    int numBuf = h->numBuf;
    for (int i = 1; i < numBuf && i < irsdk::kMaxBufs; ++i)
      if (h->varBuf[i].tickCount > h->varBuf[latest].tickCount) latest = i;
    int tc = h->varBuf[latest].tickCount;
    if (tc == lastTick_) return false;
    row.resize(boundBufLen_);
    memcpy(row.data(), base_ + h->varBuf[latest].bufOffset, boundBufLen_);
    std::atomic_thread_fence(std::memory_order_acquire);
    // a changed tick means the sim rewrote this buffer while we copied it
    if (tc == h->varBuf[latest].tickCount) {
      lastTick_ = tc;
      tick = tc;
      return true;
    }
    ++torn_;
  }
  return false;
}

std::string LiveSdk::sessionInfo() const {
  if (!base_) return {};
  auto* h = hdr();
  std::string s;
  for (int attempt = 0; attempt < 3; ++attempt) {
    int update = h->sessionInfoUpdate;
    const char* p = reinterpret_cast<const char*>(base_ + h->sessionInfoOffset);
    s.assign(p, strnlen(p, h->sessionInfoLen));
    std::atomic_thread_fence(std::memory_order_acquire);
    if (update == h->sessionInfoUpdate) break;
  }
  return s;
}
