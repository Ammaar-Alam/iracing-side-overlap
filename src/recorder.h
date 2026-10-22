// Background .ibt recording with a bounded queue so disk stalls never block the caller
#pragma once
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ibt.h"

class Recorder {
 public:
  ~Recorder() { stop(); }
  // pathBase gets _NNN.ibt appended, new parts start before the format's 2 GB offset limit
  bool start(const std::string& pathBase, const irsdk::Schema& schema, int tickRate, int queueRows = 600);
  // Copies the row into the queue, drops it if the writer has fallen behind
  void push(const uint8_t* row, const CaptureExtras& extras);
  void setSessionInfo(const std::string& yaml);
  void stop();
  bool active() const { return thread_.joinable(); }
  int written() const { return written_; }
  int dropped() const { return dropped_; }
  std::string currentPath() const;

 private:
  void run();
  bool openPart();
  void writeSidecar();

  std::string base_;
  irsdk::Schema schema_;
  int tickRate_ = 60;
  int part_ = 0;
  IbtWriter writer_;

  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::thread thread_;
  std::vector<uint8_t> rows_;
  std::vector<CaptureExtras> extras_;
  int capacity_ = 0, head_ = 0, size_ = 0;
  bool stopping_ = false;
  std::string sessionInfo_;
  bool sessionInfoDirty_ = false;
  std::string path_;
  std::atomic<int> written_ = 0, dropped_ = 0;
};
