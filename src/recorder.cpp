#include "recorder.h"

#include <cstdio>

namespace {
constexpr long long kMaxPartBytes = 1800LL << 20;
}

bool Recorder::start(const std::string& pathBase, const irsdk::Schema& schema, int tickRate, int queueRows) {
  stop();
  base_ = pathBase;
  schema_ = schema;
  tickRate_ = tickRate;
  part_ = 0;
  capacity_ = queueRows;
  rows_.assign((size_t)capacity_ * schema_.bufLen, 0);
  extras_.assign(capacity_, {});
  head_ = size_ = 0;
  written_ = dropped_ = 0;
  stopping_ = false;
  if (!openPart()) return false;
  thread_ = std::thread(&Recorder::run, this);
  return true;
}

bool Recorder::openPart() {
  char suffix[16];
  snprintf(suffix, sizeof suffix, "_%03d.ibt", part_++);
  std::lock_guard lock(mu_);
  path_ = base_ + suffix;
  return writer_.open(path_, schema_, tickRate_);
}

std::string Recorder::currentPath() const {
  std::lock_guard lock(mu_);
  return path_;
}

void Recorder::push(const uint8_t* row, const CaptureExtras& extras) {
  {
    std::lock_guard lock(mu_);
    if (!thread_.joinable() || size_ == capacity_) {
      ++dropped_;
      return;
    }
    int slot = (head_ + size_) % capacity_;
    memcpy(rows_.data() + (size_t)slot * schema_.bufLen, row, schema_.bufLen);
    extras_[slot] = extras;
    ++size_;
  }
  cv_.notify_one();
}

void Recorder::setSessionInfo(const std::string& yaml) {
  {
    std::lock_guard lock(mu_);
    sessionInfo_ = yaml;
    sessionInfoDirty_ = true;
  }
  cv_.notify_one();
}

// a copy of the session string beside the capture survives a crash or forced stop
void Recorder::writeSidecar() {
  std::string info, path;
  {
    std::lock_guard lock(mu_);
    if (!sessionInfoDirty_) return;
    sessionInfoDirty_ = false;
    info = sessionInfo_;
    path = path_.substr(0, path_.size() - 4) + ".yaml";
  }
  if (FILE* f = fopen(path.c_str(), "wb")) {
    fwrite(info.data(), 1, info.size(), f);
    fclose(f);
  }
}

void Recorder::run() {
  std::vector<uint8_t> row(schema_.bufLen);
  for (;;) {
    CaptureExtras ex;
    {
      std::unique_lock lock(mu_);
      cv_.wait(lock, [&] { return size_ > 0 || stopping_ || sessionInfoDirty_; });
      if (size_ == 0 && !stopping_) {
        lock.unlock();
        writeSidecar();
        continue;
      }
      if (size_ == 0) break;
      memcpy(row.data(), rows_.data() + (size_t)head_ * schema_.bufLen, schema_.bufLen);
      ex = extras_[head_];
      head_ = (head_ + 1) % capacity_;
      --size_;
    }
    writer_.write(row.data(), ex);
    ++written_;
    if (writer_.bytes() > kMaxPartBytes) {
      std::string info;
      {
        std::lock_guard lock(mu_);
        info = sessionInfo_;
      }
      writer_.close(info);
      openPart();
      std::lock_guard lock(mu_);
      sessionInfoDirty_ = true;
    }
    writeSidecar();
  }
}

void Recorder::stop() {
  if (!thread_.joinable()) return;
  {
    std::lock_guard lock(mu_);
    stopping_ = true;
  }
  cv_.notify_one();
  thread_.join();
  writer_.close(sessionInfo_);
}
