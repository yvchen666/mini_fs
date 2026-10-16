#pragma once

// FileSystem 的内部状态。只被 src 下的实现文件包含。
//
// 块设备在 src/block、src/ftl、src/driver、src/device。这里只保存接口指针。
//
// fs_image.cpp  格式化、挂载、位图分配、inode 块读写
// fs_path.cpp   路径解析、目录项、mkdir / rmdir / unlink / list / stat
// fs_file.cpp   文件字节读写、truncate、open / read / write / close

#include "minifs/bitmap.hpp"
#include "minifs/block_device.hpp"
#include "minifs/filesystem.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace minifs {

struct OpenFile {
  bool used = false;
  bool writable = false;
  uint32_t inode = kInvalidInode;
  uint32_t offset = 0;
};

inline void set_entry_name(DirEntry& entry, std::string_view name) {
  entry.name[0] = '\0';
  for (std::size_t i = 0; i < sizeof(entry.name); ++i) {
    entry.name[i] = '\0';
  }
  for (std::size_t i = 0; i < name.size() && i < sizeof(entry.name); ++i) {
    entry.name[i] = name[i];
  }
}

inline std::string entry_name(const DirEntry& entry) {
  std::size_t length = 0;
  while (length < sizeof(entry.name) && entry.name[length] != '\0') {
    ++length;
  }
  return std::string(entry.name, length);
}

inline Error check_file_name(std::string_view name) {
  if (name.empty() || name.size() > kMaxNameLen) {
    return Error::InvalidPath;
  }
  if (name == "." || name == "..") {
    return Error::InvalidPath;
  }
  if (name.find('/') != std::string_view::npos || name.find('\\') != std::string_view::npos) {
    return Error::InvalidPath;
  }
  return Error::Ok;
}

inline bool is_dot_or_dotdot(const std::string& name) {
  return name == "." || name == "..";
}

struct FileSystem::Impl {
  std::unique_ptr<BlockDevice> device;
  Superblock super{};
  std::vector<uint8_t> block_bitmap;
  std::vector<uint8_t> inode_bitmap;
  bool mounted = false;
  std::array<OpenFile, kMaxOpenFiles> open_files{};

  Error format_image(const std::string& image_path, uint32_t block_count);
  Error mount_image(const std::string& image_path);
  Error commit_metadata();
  Error check_allocation() const;

  Error read_inode(uint32_t index, Inode& out) const;
  Error write_inode(uint32_t index, const Inode& inode);
  Error alloc_block(uint32_t& block);
  Error free_block(uint32_t block);
  Error alloc_inode(uint32_t& index);
  Error free_inode_slot(uint32_t index);

  Error load_directory(uint32_t index, Inode& inode) const;
  Error lookup(uint32_t dir_index, std::string_view name, uint32_t& child) const;
  Error add_entry(uint32_t dir_index, std::string_view name, uint32_t child);
  Error remove_entry(uint32_t dir_index, std::string_view name, uint32_t& child);
  Error is_directory_empty(uint32_t dir_index, bool& empty) const;

  Error resolve(std::string_view path, uint32_t& inode) const;
  Error walk(const std::vector<std::string>& parts, uint32_t& inode) const;
  Error walk_parent(std::string_view path, uint32_t& parent, std::string& name) const;

  Error read_at(const Inode& inode, uint32_t offset, void* buffer, uint32_t size, uint32_t& transferred) const;
  Error write_at(uint32_t index, uint32_t offset, const void* buffer, uint32_t size, uint32_t& transferred);
  Error ensure_data_block(Inode& inode, uint32_t logical, uint32_t& physical);
  Error truncate_inode(uint32_t index, uint32_t new_size);
  Error release_inode(uint32_t index);

  bool inode_is_open(uint32_t index) const;
  void clear_open_files();
};

}  // namespace minifs
