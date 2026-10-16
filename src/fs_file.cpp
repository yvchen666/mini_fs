#include "fs_impl.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <vector>

namespace minifs {

Error FileSystem::open(std::string_view path, OpenMode mode, int& fd) {
  fd = -1;
  if (!impl_->mounted) {
    return Error::NotMounted;
  }

  uint32_t index = 0;
  Error error = impl_->resolve(path, index);
  if (error != Error::Ok) {
    return error;
  }

  Inode inode{};
  error = impl_->read_inode(index, inode);
  if (error != Error::Ok) {
    return error;
  }
  if (inode.type == static_cast<uint32_t>(FileType::Directory)) {
    return Error::IsDirectory;
  }
  if (inode.type != static_cast<uint32_t>(FileType::Regular)) {
    return Error::Corrupt;
  }

  if (mode == OpenMode::Truncate) {
    error = impl_->truncate_inode(index, 0);
    if (error != Error::Ok) {
      return error;
    }
  }

  int slot = -1;
  for (int i = 0; i < static_cast<int>(kMaxOpenFiles); ++i) {
    if (!impl_->open_files[static_cast<std::size_t>(i)].used) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    return Error::TooManyOpen;
  }

  OpenFile& file = impl_->open_files[static_cast<std::size_t>(slot)];
  file = OpenFile{};
  file.used = true;
  file.writable = mode != OpenMode::ReadOnly;
  file.inode = index;
  file.offset = 0;
  fd = slot;
  return Error::Ok;
}

Error FileSystem::read(int fd, void* buffer, std::size_t size, std::size_t& transferred) {
  transferred = 0;
  if (!impl_->mounted) {
    return Error::NotMounted;
  }
  if (fd < 0 || static_cast<std::size_t>(fd) >= kMaxOpenFiles) {
    return Error::BadFd;
  }
  OpenFile& file = impl_->open_files[static_cast<std::size_t>(fd)];
  if (!file.used) {
    return Error::BadFd;
  }
  if (buffer == nullptr && size != 0) {
    return Error::InvalidArgument;
  }
  if (size > std::numeric_limits<uint32_t>::max()) {
    return Error::InvalidArgument;
  }

  Inode inode{};
  const Error error = impl_->read_inode(file.inode, inode);
  if (error != Error::Ok) {
    return error;
  }

  uint32_t count = 0;
  const Error read_error =
      impl_->read_at(inode, file.offset, buffer, static_cast<uint32_t>(size), count);
  file.offset += count;
  transferred = count;
  return read_error;
}

Error FileSystem::write(int fd, const void* buffer, std::size_t size, std::size_t& transferred) {
  transferred = 0;
  if (!impl_->mounted) {
    return Error::NotMounted;
  }
  if (fd < 0 || static_cast<std::size_t>(fd) >= kMaxOpenFiles) {
    return Error::BadFd;
  }
  OpenFile& file = impl_->open_files[static_cast<std::size_t>(fd)];
  if (!file.used) {
    return Error::BadFd;
  }
  if (!file.writable) {
    return Error::InvalidArgument;
  }
  if (buffer == nullptr && size != 0) {
    return Error::InvalidArgument;
  }
  if (size > std::numeric_limits<uint32_t>::max()) {
    return Error::InvalidArgument;
  }

  uint32_t count = 0;
  // fd 只是打开表的下标。真正写字节的是这个 inode 和它的当前偏移。
  const Error error =
      impl_->write_at(file.inode, file.offset, buffer, static_cast<uint32_t>(size), count);
  file.offset += count;
  transferred = count;
  return error;
}

Error FileSystem::close(int fd) {
  if (!impl_->mounted) {
    return Error::NotMounted;
  }
  if (fd < 0 || static_cast<std::size_t>(fd) >= kMaxOpenFiles) {
    return Error::BadFd;
  }
  OpenFile& file = impl_->open_files[static_cast<std::size_t>(fd)];
  if (!file.used) {
    return Error::BadFd;
  }
  file = OpenFile{};
  return Error::Ok;
}

Error FileSystem::Impl::read_at(const Inode& inode,
                                uint32_t offset,
                                void* buffer,
                                uint32_t size,
                                uint32_t& transferred) const {
  transferred = 0;
  if (size == 0 || offset >= inode.size) {
    return Error::Ok;
  }

  const uint32_t readable = std::min(size, inode.size - offset);
  auto* out = static_cast<uint8_t*>(buffer);
  std::vector<uint8_t> block(kBlockSize, 0);

  while (transferred < readable) {
    const uint32_t position = offset + transferred;
    const uint32_t logical = position / kBlockSize;
    const uint32_t inner = position % kBlockSize;
    if (logical >= kDirectPointers) {
      return Error::Corrupt;
    }

    const uint32_t chunk = std::min(readable - transferred, kBlockSize - inner);
    const uint32_t physical = inode.direct[logical];
    if (physical == 0) {
      std::memset(out + transferred, 0, chunk);
    } else {
      if (physical < super.data_block_start || physical >= super.block_count) {
        return Error::Corrupt;
      }
      if (!bitmap::test(block_bitmap.data(), physical)) {
        return Error::Corrupt;
      }
      const Error error = device->read_block(physical, block.data());
      if (error != Error::Ok) {
        return error;
      }
      std::memcpy(out + transferred, block.data() + inner, chunk);
    }
    transferred += chunk;
  }
  return Error::Ok;
}

Error FileSystem::Impl::ensure_data_block(Inode& inode, uint32_t logical, uint32_t& physical) {
  if (logical >= kDirectPointers) {
    return Error::TooLarge;
  }

  const uint32_t existing = inode.direct[logical];
  if (existing != 0) {
    if (existing < super.data_block_start || existing >= super.block_count) {
      return Error::Corrupt;
    }
    if (!bitmap::test(block_bitmap.data(), existing)) {
      return Error::Corrupt;
    }
    physical = existing;
    return Error::Ok;
  }

  uint32_t block = 0;
  const Error error = alloc_block(block);
  if (error != Error::Ok) {
    return error;
  }
  inode.direct[logical] = block;
  physical = block;
  return Error::Ok;
}

Error FileSystem::Impl::write_at(uint32_t index,
                                 uint32_t offset,
                                 const void* buffer,
                                 uint32_t size,
                                 uint32_t& transferred) {
  transferred = 0;
  Inode inode{};
  Error error = read_inode(index, inode);
  if (error != Error::Ok) {
    return error;
  }
  if (inode.type != static_cast<uint32_t>(FileType::Regular) &&
      inode.type != static_cast<uint32_t>(FileType::Directory)) {
    return Error::Corrupt;
  }
  if (size == 0) {
    return Error::Ok;
  }
  if (offset > kMaxFileSize || size > kMaxFileSize - offset) {
    return Error::TooLarge;
  }

  const auto* input = static_cast<const uint8_t*>(buffer);
  std::vector<uint8_t> block(kBlockSize, 0);
  Error write_error = Error::Ok;

  // 文件偏移换成“第几个 4KB 逻辑块”和块内位置。
  while (transferred < size) {
    const uint32_t position = offset + transferred;
    const uint32_t logical = position / kBlockSize;
    const uint32_t inner = position % kBlockSize;
    uint32_t physical = 0;
    write_error = ensure_data_block(inode, logical, physical);
    if (write_error != Error::Ok) {
      break;
    }

    write_error = device->read_block(physical, block.data());
    if (write_error != Error::Ok) {
      break;
    }
    const uint32_t chunk = std::min(size - transferred, kBlockSize - inner);
    std::memcpy(block.data() + inner, input + transferred, chunk);
    // 块设备只能整块写。这里把改过的 4KB 交给 FTL。
    write_error = device->write_block(physical, block.data());
    if (write_error != Error::Ok) {
      break;
    }
    transferred += chunk;
  }

  if (offset + transferred > inode.size) {
    inode.size = offset + transferred;
  }
  // 即使后面的写入失败，也把已经分配的块记进 inode，避免位图占用但 inode 丢失指针。
  const Error save_error = write_inode(index, inode);
  const Error sync_error = device->sync();
  if (write_error != Error::Ok) {
    return write_error;
  }
  if (save_error != Error::Ok) {
    return save_error;
  }
  return sync_error;
}

Error FileSystem::Impl::truncate_inode(uint32_t index, uint32_t new_size) {
  Inode inode{};
  Error error = read_inode(index, inode);
  if (error != Error::Ok) {
    return error;
  }
  if (new_size > inode.size) {
    return Error::InvalidArgument;
  }

  const uint32_t keep = new_size == 0 ? 0 : (new_size + kBlockSize - 1) / kBlockSize;
  std::vector<uint32_t> released;
  for (uint32_t pointer = keep; pointer < kDirectPointers; ++pointer) {
    if (inode.direct[pointer] != 0) {
      released.push_back(inode.direct[pointer]);
      inode.direct[pointer] = 0;
    }
  }
  inode.size = new_size;

  // 先把“不再指向这些块”写回 inode，再归还位图。这样不会把还挂在文件上的块分出去。
  error = write_inode(index, inode);
  if (error != Error::Ok) {
    return error;
  }
  error = device->sync();
  if (error != Error::Ok) {
    return error;
  }
  for (uint32_t block : released) {
    error = free_block(block);
    if (error != Error::Ok) {
      return error;
    }
  }
  return Error::Ok;
}

Error FileSystem::Impl::release_inode(uint32_t index) {
  if (index == super.root_inode) {
    return Error::InvalidArgument;
  }
  Error error = truncate_inode(index, 0);
  if (error != Error::Ok) {
    return error;
  }

  const Inode blank{};
  error = write_inode(index, blank);
  if (error != Error::Ok) {
    return error;
  }
  error = device->sync();
  if (error != Error::Ok) {
    return error;
  }
  return free_inode_slot(index);
}

bool FileSystem::Impl::inode_is_open(uint32_t index) const {
  for (const OpenFile& file : open_files) {
    if (file.used && file.inode == index) {
      return true;
    }
  }
  return false;
}

void FileSystem::Impl::clear_open_files() {
  open_files.fill(OpenFile{});
}

}  // namespace minifs
