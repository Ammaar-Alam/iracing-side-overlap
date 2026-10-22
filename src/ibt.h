// .ibt telemetry files: iRacing disk logs and our own captures share this format
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "irsdk.h"

class IbtReader {
 public:
  ~IbtReader() { close(); }
  bool open(const std::string& path);
  void close();
  bool next(std::vector<uint8_t>& row);
  bool rewind();

  const irsdk::Schema& schema() const { return schema_; }
  const std::string& sessionInfo() const { return sessionInfo_; }
  int tickRate() const { return header_.tickRate; }
  int recordCount() const { return records_; }

 private:
  FILE* f_ = nullptr;
  irsdk::Header header_{};
  irsdk::Schema schema_;
  std::string sessionInfo_;
  int records_ = 0;
};

// Columns appended to each captured row for timing analysis
struct CaptureExtras {
  double hostTime;         // monotonic seconds since capture start
  int32_t tickCount;       // SDK buffer tick
  int32_t sessionInfoUpdate;
};

class IbtWriter {
 public:
  ~IbtWriter() { close({}); }
  bool open(const std::string& path, const irsdk::Schema& source, int tickRate);
  void write(const uint8_t* row, const CaptureExtras& extras);
  // Session info goes at the end so it can reflect the final driver list
  void close(const std::string& sessionInfo);
  bool isOpen() const { return f_ != nullptr; }
  int records() const { return records_; }
  long long bytes() const { return (long long)records_ * header_.bufLen; }

 private:
  FILE* f_ = nullptr;
  irsdk::Header header_{};
  irsdk::VarHeader sessionTimeVar_{};
  bool hasSessionTime_ = false;
  int sourceLen_ = 0;
  int records_ = 0;
  double firstTime_ = 0, lastTime_ = 0;
  int64_t startDate_ = 0;
  std::vector<uint8_t> scratch_;
};
