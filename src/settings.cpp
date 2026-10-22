#include "settings.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdlib>
#include <vector>

namespace {

std::wstring widen(const std::string& s) {
  std::wstring w(MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), (int)w.size());
  return w;
}

std::string narrow(const std::wstring& w) {
  std::string s(WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), (int)s.size(), nullptr, nullptr);
  return s;
}

std::wstring readStr(const std::wstring& path, const wchar_t* section, const wchar_t* key, const wchar_t* def) {
  wchar_t buf[256];
  GetPrivateProfileStringW(section, key, def, buf, 256, path.c_str());
  return buf;
}

int readInt(const std::wstring& path, const wchar_t* section, const wchar_t* key, int def) {
  std::wstring s = readStr(path, section, key, L"");
  return s.empty() ? def : _wtoi(s.c_str());
}

uint32_t readColor(const std::wstring& path, const wchar_t* key, uint32_t def) {
  std::wstring s = readStr(path, L"colors", key, L"");
  if (!s.empty() && s[0] == L'#') s.erase(0, 1);
  return s.size() == 6 ? (uint32_t)wcstoul(s.c_str(), nullptr, 16) : def;
}

void write(const std::wstring& path, const wchar_t* section, const wchar_t* key, const std::wstring& v) {
  WritePrivateProfileStringW(section, key, v.c_str(), path.c_str());
}

std::wstring hex(uint32_t c) {
  wchar_t buf[16];
  swprintf(buf, 16, L"#%06X", c & 0xFFFFFF);
  return buf;
}

}  // namespace

void Settings::load(const std::wstring& path) {
  x = readInt(path, L"window", L"x", x);
  y = readInt(path, L"window", L"y", y);
  width = readInt(path, L"window", L"width", width);
  height = readInt(path, L"window", L"height", height);
  opacity = readInt(path, L"window", L"opacity", opacity);
  barColor = readColor(path, L"bar", barColor);
  referenceColor = readColor(path, L"reference", referenceColor);
  unavailableColor = readColor(path, L"unavailable", unavailableColor);
  fps = readInt(path, L"behaviour", L"fps", fps);

  std::vector<wchar_t> buf(8192);
  DWORD n = GetPrivateProfileSectionW(L"cars", buf.data(), (DWORD)buf.size(), path.c_str());
  for (const wchar_t* p = buf.data(); p < buf.data() + n && *p; p += wcslen(p) + 1) {
    std::wstring kv = p;
    size_t eq = kv.find(L'=');
    if (eq != std::wstring::npos) carLengths[narrow(kv.substr(0, eq))] = (float)_wtof(kv.c_str() + eq + 1);
  }
}

void Settings::save(const std::wstring& path) const {
  write(path, L"window", L"x", std::to_wstring(x));
  write(path, L"window", L"y", std::to_wstring(y));
  write(path, L"window", L"width", std::to_wstring(width));
  write(path, L"window", L"height", std::to_wstring(height));
  write(path, L"window", L"opacity", std::to_wstring(opacity));
  write(path, L"colors", L"bar", hex(barColor));
  write(path, L"colors", L"reference", hex(referenceColor));
  write(path, L"colors", L"unavailable", hex(unavailableColor));
  write(path, L"behaviour", L"fps", std::to_wstring(fps));
  for (const auto& [carPath, length] : carLengths) write(path, L"cars", widen(carPath).c_str(), std::to_wstring(length));
}

float spotterPadding() {
  PWSTR docs = nullptr;
  float pad = 0.25f;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
    std::wstring ini = std::wstring(docs) + L"\\iRacing\\app.ini";
    std::wstring v = readStr(ini, L"SPCC", L"carLowHiPadding", L"");
    if (!v.empty()) pad = (float)_wtof(v.c_str());
  }
  CoTaskMemFree(docs);
  return pad >= 0 && pad < 5 ? pad : 0.25f;
}
