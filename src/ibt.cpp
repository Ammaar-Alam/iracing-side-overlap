#include "ibt.h"

#include <windows.h>

#include <ctime>

namespace {

FILE* openUtf8(const std::string& path, const wchar_t* mode) {
  std::wstring w(MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, w.data(), (int)w.size());
  return _wfopen(w.c_str(), mode);
}

irsdk::VarHeader makeVar(const char* name, int32_t type, int32_t offset, const char* unit, const char* desc) {
  irsdk::VarHeader v{};
  v.type = type;
  v.offset = offset;
  v.count = 1;
  strncpy(v.name, name, sizeof v.name - 1);
  strncpy(v.unit, unit, sizeof v.unit - 1);
  strncpy(v.desc, desc, sizeof v.desc - 1);
  return v;
}

}  // namespace

bool IbtReader::open(const std::string& path) {
  close();
  f_ = openUtf8(path, L"rb");
  if (!f_) return false;
  irsdk::DiskSubHeader sub{};
  if (fread(&header_, sizeof header_, 1, f_) != 1 || fread(&sub, sizeof sub, 1, f_) != 1 || header_.numVars <= 0 ||
      header_.bufLen <= 0) {
    close();
    return false;
  }
  schema_.bufLen = header_.bufLen;
  schema_.vars.resize(header_.numVars);
  _fseeki64(f_, header_.varHeaderOffset, SEEK_SET);
  if (fread(schema_.vars.data(), sizeof(irsdk::VarHeader), schema_.vars.size(), f_) != schema_.vars.size()) {
    close();
    return false;
  }
  sessionInfo_.assign(header_.sessionInfoLen, '\0');
  _fseeki64(f_, header_.sessionInfoOffset, SEEK_SET);
  size_t got = fread(sessionInfo_.data(), 1, sessionInfo_.size(), f_);
  sessionInfo_.resize(strnlen(sessionInfo_.c_str(), got));
  if (sessionInfo_.empty() && path.size() > 4) {
    // unfinished captures keep their session string in a sidecar file
    if (FILE* y = openUtf8(path.substr(0, path.size() - 4) + ".yaml", L"rb")) {
      char buf[65536];
      for (size_t n; (n = fread(buf, 1, sizeof buf, y)) > 0;) sessionInfo_.append(buf, n);
      fclose(y);
    }
  }
  // trust the file size over the record count, which is only written on a clean close
  _fseeki64(f_, 0, SEEK_END);
  long long dataBytes = _ftelli64(f_) - header_.varBuf[0].bufOffset;
  if (header_.sessionInfoOffset > header_.varBuf[0].bufOffset)
    dataBytes = header_.sessionInfoOffset - header_.varBuf[0].bufOffset;
  records_ = (int)(dataBytes / header_.bufLen);
  return rewind();
}

bool IbtReader::rewind() { return f_ && _fseeki64(f_, header_.varBuf[0].bufOffset, SEEK_SET) == 0; }

bool IbtReader::next(std::vector<uint8_t>& row) {
  if (!f_) return false;
  row.resize(header_.bufLen);
  if (_ftelli64(f_) + header_.bufLen > header_.varBuf[0].bufOffset + (long long)records_ * header_.bufLen) return false;
  return fread(row.data(), 1, row.size(), f_) == row.size();
}

void IbtReader::close() {
  if (f_) fclose(f_);
  f_ = nullptr;
}

bool IbtWriter::open(const std::string& path, const irsdk::Schema& source, int tickRate) {
  f_ = openUtf8(path, L"wb");
  if (!f_) return false;
  setvbuf(f_, nullptr, _IOFBF, 1 << 20);
  sourceLen_ = source.bufLen;
  std::vector<irsdk::VarHeader> vars = source.vars;
  vars.push_back(makeVar("CapHostTime", irsdk::kDouble, sourceLen_, "s", "Capture host monotonic time"));
  vars.push_back(makeVar("CapTickCount", irsdk::kInt, sourceLen_ + 8, "", "SDK buffer tick count"));
  vars.push_back(makeVar("CapSessionInfoUpdate", irsdk::kInt, sourceLen_ + 12, "", "Session info update counter"));
  if (auto* st = source.find("SessionTime"); st && st->type == irsdk::kDouble) {
    sessionTimeVar_ = *st;
    hasSessionTime_ = true;
  }

  header_ = {};
  header_.ver = 2;
  header_.status = irsdk::kStatusConnected;
  header_.tickRate = tickRate;
  header_.numVars = (int32_t)vars.size();
  header_.varHeaderOffset = sizeof(irsdk::Header) + sizeof(irsdk::DiskSubHeader);
  header_.numBuf = 1;
  header_.bufLen = sourceLen_ + (int32_t)sizeof(CaptureExtras);
  header_.varBuf[0].bufOffset = header_.varHeaderOffset + header_.numVars * (int32_t)sizeof(irsdk::VarHeader);
  startDate_ = (int64_t)time(nullptr);
  irsdk::DiskSubHeader sub{};
  fwrite(&header_, sizeof header_, 1, f_);
  fwrite(&sub, sizeof sub, 1, f_);
  fwrite(vars.data(), sizeof(irsdk::VarHeader), vars.size(), f_);
  scratch_.resize(header_.bufLen);
  records_ = 0;
  return true;
}

void IbtWriter::write(const uint8_t* row, const CaptureExtras& extras) {
  if (!f_) return;
  memcpy(scratch_.data(), row, sourceLen_);
  memcpy(scratch_.data() + sourceLen_, &extras, sizeof extras);
  fwrite(scratch_.data(), 1, scratch_.size(), f_);
  if (hasSessionTime_) {
    lastTime_ = irsdk::readAsDouble(row, sessionTimeVar_, 0);
    if (records_ == 0) firstTime_ = lastTime_;
  }
  header_.varBuf[0].tickCount = extras.tickCount;
  ++records_;
}

void IbtWriter::close(const std::string& sessionInfo) {
  if (!f_) return;
  header_.sessionInfoOffset = header_.varBuf[0].bufOffset + records_ * header_.bufLen;
  header_.sessionInfoLen = (int32_t)sessionInfo.size() + 1;
  fwrite(sessionInfo.c_str(), 1, sessionInfo.size() + 1, f_);
  irsdk::DiskSubHeader sub{};
  sub.sessionStartDate = startDate_;
  sub.sessionStartTime = firstTime_;
  sub.sessionEndTime = lastTime_;
  sub.sessionRecordCount = records_;
  _fseeki64(f_, 0, SEEK_SET);
  fwrite(&header_, sizeof header_, 1, f_);
  fwrite(&sub, sizeof sub, 1, f_);
  fclose(f_);
  f_ = nullptr;
}
