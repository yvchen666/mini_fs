#include "minifs/block_device.hpp"

#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

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
namespace {

std::filesystem::path to_path(const std::string& text) {
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

}  // namespace

Error BlockDevice::create(const std::string& path, uint32_t block_count) {
  close();
  if (path.empty() || block_count == 0) {
    return Error::InvalidArgument;
  }

  file_.open(to_path(path), std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
  if (!file_) {
    return Error::Io;
  }

  const std::vector<char> zeros(kBlockSize, 0);
  for (uint32_t index = 0; index < block_count; ++index) {
    file_.write(zeros.data(), static_cast<std::streamsize>(kBlockSize));
    if (!file_) {
      close();
      return Error::Io;
    }
  }
  file_.flush();
  if (!file_) {
    close();
    return Error::Io;
  }

  block_count_ = block_count;
  return Error::Ok;
}

Error BlockDevice::open(const std::string& path) {
  close();
  if (path.empty()) {
    return Error::InvalidArgument;
  }

  const std::filesystem::path file_path = to_path(path);
  std::error_code exists_error;
  if (!std::filesystem::exists(file_path, exists_error) || exists_error) {
    return Error::NotFound;
  }

  std::error_code size_error;
  const auto size = std::filesystem::file_size(file_path, size_error);
  if (size_error) {
    return Error::Io;
  }
  if (size == 0 || size % kBlockSize != 0 || size / kBlockSize > std::numeric_limits<uint32_t>::max()) {
    return Error::Corrupt;
  }

  file_.open(file_path, std::ios::binary | std::ios::in | std::ios::out);
  if (!file_) {
    return Error::Io;
  }
  block_count_ = static_cast<uint32_t>(size / kBlockSize);
  return Error::Ok;
}

void BlockDevice::close() {
  if (file_.is_open()) {
    file_.close();
  }
  file_.clear();
  block_count_ = 0;
}

bool BlockDevice::is_open() const {
  return file_.is_open();
}

uint32_t BlockDevice::block_count() const {
  return block_count_;
}

Error BlockDevice::read_block(uint32_t index, void* buffer) const {
  if (!file_.is_open() || buffer == nullptr) {
    return Error::Io;
  }
  if (index >= block_count_) {
    return Error::InvalidArgument;
  }

  const auto position = static_cast<std::streamoff>(static_cast<uint64_t>(index) * kBlockSize);
  file_.clear();
  file_.seekg(position, std::ios::beg);
  file_.read(static_cast<char*>(buffer), static_cast<std::streamsize>(kBlockSize));
  if (!file_ || file_.gcount() != static_cast<std::streamsize>(kBlockSize)) {
    return Error::Io;
  }
  return Error::Ok;
}

Error BlockDevice::write_block(uint32_t index, const void* buffer) {
  if (!file_.is_open() || buffer == nullptr) {
    return Error::Io;
  }
  if (index >= block_count_) {
    return Error::InvalidArgument;
  }

  const auto position = static_cast<std::streamoff>(static_cast<uint64_t>(index) * kBlockSize);
  file_.clear();
  file_.seekp(position, std::ios::beg);
  file_.write(static_cast<const char*>(buffer), static_cast<std::streamsize>(kBlockSize));
  if (!file_) {
    return Error::Io;
  }
  return Error::Ok;
}

Error BlockDevice::sync() {
  if (!file_.is_open()) {
    return Error::Io;
  }
  file_.flush();
  if (!file_) {
    return Error::Io;
  }
  return Error::Ok;
}

}  // namespace minifs
