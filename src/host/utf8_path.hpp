#pragma once

#include <filesystem>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace minifs {

inline std::filesystem::path utf8_path(const std::string& text) {
#if defined(_WIN32)
  if (text.empty()) {
    return {};
  }
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
  if (size <= 0) {
    return std::filesystem::path(text);
  }
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), size);
  if (!wide.empty() && wide.back() == L'\0') {
    wide.pop_back();
  }
  return std::filesystem::path(wide);
#else
  return std::filesystem::path(text);
#endif
}

}  // namespace minifs
