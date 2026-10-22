// Read-only client for the simulator's telemetry memory map
#pragma once
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "irsdk.h"

class LiveSdk {
 public:
  LiveSdk() = default;
  LiveSdk(const LiveSdk&) = delete;
  LiveSdk& operator=(const LiveSdk&) = delete;
  ~LiveSdk() { close(); }

  // Opens the map and event if the sim has created them, cheap to retry
  bool open();
  void close();
  bool isOpen() const { return base_ != nullptr; }
  bool connected() const { return base_ && (hdr()->status & irsdk::kStatusConnected); }
  HANDLE dataEvent() const { return event_; }

  // Copies the newest row when its tick differs from the last one returned
  // Rebinds the schema first if the variable layout changed
  bool readLatest(std::vector<uint8_t>& row, int& tick);

  const irsdk::Schema& schema() const { return schema_; }
  bool schemaChanged() { bool c = schemaChanged_; schemaChanged_ = false; return c; }
  int tickRate() const { return base_ ? hdr()->tickRate : 0; }
  int sessionInfoUpdate() const { return base_ ? hdr()->sessionInfoUpdate : -1; }
  std::string sessionInfo() const;
  int tornReads() const { return torn_; }

 private:
  volatile const irsdk::Header* hdr() const { return reinterpret_cast<volatile const irsdk::Header*>(base_); }
  void bindSchema();

  HANDLE map_ = nullptr;
  HANDLE event_ = nullptr;
  const uint8_t* base_ = nullptr;
  irsdk::Schema schema_;
  int boundNumVars_ = -1, boundBufLen_ = -1, boundVarOffset_ = -1;
  bool schemaChanged_ = false;
  int lastTick_ = INT32_MIN;
  int torn_ = 0;
};
