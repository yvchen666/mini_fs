#include "block/flash_block_device.hpp"
#include "fs_impl.hpp"

#include <cstring>
#include <vector>

namespace minifs {
namespace {

Error validate_super(const Superblock& super, uint32_t image_blocks) {
  if (super.magic != kMagic || super.version != kVersion) {
    return Error::Corrupt;
  }
  if (super.block_size != kBlockSize || super.block_count != image_blocks) {
    return Error::Corrupt;
  }
  if (super.block_count == 0 || super.block_count > kBlockSize * 8) {
    return Error::Corrupt;
  }
  if (super.inode_count == 0 || super.inode_count > kBlockSize * 8) {
    return Error::Corrupt;
  }
  if (super.block_bitmap_block != 1 || super.inode_bitmap_block != 2) {
    return Error::Corrupt;
  }
  if (super.inode_table_block != 3 || super.inode_table_blocks == 0) {
    return Error::Corrupt;
  }

  const uint32_t inode_bytes = super.inode_count * static_cast<uint32_t>(sizeof(Inode));
  if (inode_bytes > super.inode_table_blocks * kBlockSize) {
    return Error::Corrupt;
  }
  if (super.data_block_start != 3 + super.inode_table_blocks) {
    return Error::Corrupt;
  }
  if (super.data_block_start >= super.block_count) {
    return Error::Corrupt;
  }
  if (super.root_inode != 0) {
    return Error::Corrupt;
  }
  if (super.free_blocks > super.block_count || super.free_inodes > super.inode_count) {
    return Error::Corrupt;
  }
  return Error::Ok;
}

}  // namespace

FileSystem::FileSystem() : impl_(std::make_unique<Impl>()) {}

FileSystem::~FileSystem() {
  if (impl_ && impl_->mounted) {
    static_cast<void>(impl_->commit_metadata());
    impl_->clear_open_files();
    if (impl_->device) {
      impl_->device->close();
    }
    impl_->mounted = false;
  }
}

Error FileSystem::format(const std::string& image_path, uint32_t block_count) {
  if (impl_->mounted) {
    const Error error = unmount();
    if (error != Error::Ok) {
      return error;
    }
  }
  const Error error = impl_->format_image(image_path, block_count);
  if (error != Error::Ok) {
    return error;
  }
  return mount(image_path);
}

Error FileSystem::mount(const std::string& image_path) {
  if (impl_->mounted) {
    const Error error = unmount();
    if (error != Error::Ok) {
      return error;
    }
  }
  return impl_->mount_image(image_path);
}

Error FileSystem::unmount() {
  if (!impl_->mounted) {
    return Error::Ok;
  }
  const Error error = sync();
  if (error != Error::Ok) {
    return error;
  }
  impl_->clear_open_files();
  if (impl_->device) {
    impl_->device->close();
  }
  impl_->mounted = false;
  return Error::Ok;
}

Error FileSystem::sync() {
  if (!impl_->mounted) {
    return Error::NotMounted;
  }
  return impl_->commit_metadata();
}

bool FileSystem::mounted() const {
  return impl_->mounted;
}

Error FileSystem::disk_info(DiskInfo& out) const {
  if (!impl_->mounted) {
    return Error::NotMounted;
  }
  out.block_size = impl_->super.block_size;
  out.block_count = impl_->super.block_count;
  out.free_blocks = impl_->super.free_blocks;
  out.inode_count = impl_->super.inode_count;
  out.free_inodes = impl_->super.free_inodes;
  out.data_block_start = impl_->super.data_block_start;
  return Error::Ok;
}

Error FileSystem::storage_info(StorageInfo& out) const {
  if (!impl_->mounted || !impl_->device) {
    return Error::NotMounted;
  }
  return impl_->device->storage_info(out);
}

Error FileSystem::inject_fault(InjectedFault fault) {
  if (!impl_->mounted || !impl_->device) {
    return Error::NotMounted;
  }
  return impl_->device->inject_fault(fault);
}

Error FileSystem::Impl::format_image(const std::string& image_path, uint32_t block_count) {
  const uint32_t inode_count = kDefaultInodeCount;
  const uint32_t inode_bytes = inode_count * static_cast<uint32_t>(sizeof(Inode));
  const uint32_t inode_blocks = (inode_bytes + kBlockSize - 1) / kBlockSize;
  const uint32_t data_start = 3 + inode_blocks;
  if (block_count <= data_start || block_count > kBlockSize * 8 || inode_count > kBlockSize * 8) {
    return Error::InvalidArgument;
  }

  device = std::make_unique<FlashBlockDevice>();
  Error error = device->create(image_path, block_count);
  if (error != Error::Ok) {
    device.reset();
    return error;
  }

  // 中途失败也要把文件句柄放开，否则 Windows 上随后删不掉镜像。
  struct CloseOnExit {
    std::unique_ptr<BlockDevice>& device;
    ~CloseOnExit() {
      if (device) {
        device->close();
      }
    }
  } close_on_exit{device};

  super = Superblock{};
  super.magic = kMagic;
  super.version = kVersion;
  super.block_size = kBlockSize;
  super.block_count = block_count;
  super.inode_count = inode_count;
  super.block_bitmap_block = 1;
  super.inode_bitmap_block = 2;
  super.inode_table_block = 3;
  super.inode_table_blocks = inode_blocks;
  super.data_block_start = data_start;
  super.root_inode = 0;
  super.free_blocks = block_count - data_start;
  super.free_inodes = inode_count - 1;

  block_bitmap.assign(kBlockSize, 0);
  inode_bitmap.assign(kBlockSize, 0);
  for (uint32_t block = 0; block < data_start; ++block) {
    bitmap::set(block_bitmap.data(), block);
  }
  bitmap::set(inode_bitmap.data(), 0);

  error = commit_metadata();
  if (error != Error::Ok) {
    return error;
  }

  uint32_t root_block = 0;
  error = alloc_block(root_block);
  if (error != Error::Ok) {
    return error;
  }

  DirEntry entries[2]{};
  entries[0].inode = 0;
  set_entry_name(entries[0], ".");
  entries[1].inode = 0;
  set_entry_name(entries[1], "..");

  std::vector<uint8_t> block(kBlockSize, 0);
  std::memcpy(block.data(), entries, sizeof(entries));
  error = device->write_block(root_block, block.data());
  if (error != Error::Ok) {
    return error;
  }

  Inode root{};
  root.type = static_cast<uint32_t>(FileType::Directory);
  root.size = static_cast<uint32_t>(sizeof(entries));
  root.link_count = 1;
  root.direct[0] = root_block;
  error = write_inode(0, root);
  if (error != Error::Ok) {
    return error;
  }
  error = device->sync();
  if (error != Error::Ok) {
    return error;
  }
  return check_allocation();
}

Error FileSystem::Impl::mount_image(const std::string& image_path) {
  if (!device) {
    device = std::make_unique<FlashBlockDevice>();
  }
  Error error = device->open(image_path);
  if (error != Error::Ok) {
    return error;
  }

  struct CloseOnFailure {
    Impl& self;
    bool failed = true;
    ~CloseOnFailure() {
      if (failed && self.device) {
        self.device->close();
        self.mounted = false;
      }
    }
  } guard{*this};

  std::vector<uint8_t> block(kBlockSize, 0);
  error = device->read_block(0, block.data());
  if (error != Error::Ok) {
    return error;
  }
  std::memcpy(&super, block.data(), sizeof(super));
  error = validate_super(super, device->block_count());
  if (error != Error::Ok) {
    return error;
  }

  block_bitmap.assign(kBlockSize, 0);
  inode_bitmap.assign(kBlockSize, 0);
  error = device->read_block(super.block_bitmap_block, block_bitmap.data());
  if (error != Error::Ok) {
    return error;
  }
  error = device->read_block(super.inode_bitmap_block, inode_bitmap.data());
  if (error != Error::Ok) {
    return error;
  }
  error = check_allocation();
  if (error != Error::Ok) {
    return error;
  }

  Inode root{};
  error = read_inode(super.root_inode, root);
  if (error != Error::Ok) {
    return error;
  }
  if (root.type != static_cast<uint32_t>(FileType::Directory) || root.direct[0] == 0) {
    return Error::Corrupt;
  }
  if (root.size < sizeof(DirEntry) * 2 || root.size % sizeof(DirEntry) != 0) {
    return Error::Corrupt;
  }

  clear_open_files();
  mounted = true;
  guard.failed = false;
  return Error::Ok;
}

Error FileSystem::Impl::commit_metadata() {
  // 先写两张位图，最后写超级块里的空闲计数。进程若在中间退出，
  // 最坏情况是下一次挂载发现计数对不上，而不是把仍在使用的块再分出去。
  Error error = device->write_block(super.block_bitmap_block, block_bitmap.data());
  if (error != Error::Ok) {
    return error;
  }
  error = device->write_block(super.inode_bitmap_block, inode_bitmap.data());
  if (error != Error::Ok) {
    return error;
  }

  std::vector<uint8_t> block(kBlockSize, 0);
  std::memcpy(block.data(), &super, sizeof(super));
  error = device->write_block(0, block.data());
  if (error != Error::Ok) {
    return error;
  }
  return device->sync();
}

Error FileSystem::Impl::check_allocation() const {
  if (block_bitmap.size() != kBlockSize || inode_bitmap.size() != kBlockSize) {
    return Error::Corrupt;
  }
  for (uint32_t block = 0; block < super.data_block_start; ++block) {
    if (!bitmap::test(block_bitmap.data(), block)) {
      return Error::Corrupt;
    }
  }
  const uint32_t free_blocks =
      bitmap::count_clear(block_bitmap.data(), super.data_block_start, super.block_count);
  const uint32_t free_inodes = bitmap::count_clear(inode_bitmap.data(), 0, super.inode_count);
  if (free_blocks != super.free_blocks || free_inodes != super.free_inodes) {
    return Error::Corrupt;
  }
  return Error::Ok;
}

Error FileSystem::Impl::read_inode(uint32_t index, Inode& out) const {
  if (index >= super.inode_count) {
    return Error::InvalidArgument;
  }
  const uint32_t byte = index * static_cast<uint32_t>(sizeof(Inode));
  const uint32_t block = super.inode_table_block + byte / kBlockSize;
  const uint32_t offset = byte % kBlockSize;

  std::vector<uint8_t> raw(kBlockSize, 0);
  const Error error = device->read_block(block, raw.data());
  if (error != Error::Ok) {
    return error;
  }
  std::memcpy(&out, raw.data() + offset, sizeof(out));
  return Error::Ok;
}

Error FileSystem::Impl::write_inode(uint32_t index, const Inode& inode) {
  if (index >= super.inode_count) {
    return Error::InvalidArgument;
  }
  const uint32_t byte = index * static_cast<uint32_t>(sizeof(Inode));
  const uint32_t block = super.inode_table_block + byte / kBlockSize;
  const uint32_t offset = byte % kBlockSize;

  std::vector<uint8_t> raw(kBlockSize, 0);
  Error error = device->read_block(block, raw.data());
  if (error != Error::Ok) {
    return error;
  }
  std::memcpy(raw.data() + offset, &inode, sizeof(inode));
  return device->write_block(block, raw.data());
}

Error FileSystem::Impl::alloc_block(uint32_t& block) {
  uint32_t found = 0;
  if (!bitmap::find_clear(block_bitmap.data(), super.data_block_start, super.block_count, found)) {
    return Error::NoSpace;
  }
  if (super.free_blocks == 0) {
    return Error::Corrupt;
  }

  // 位图先落盘，再让 inode 指向这块。中途退出只会泄漏空块，不会把这块再分给别人。
  bitmap::set(block_bitmap.data(), found);
  --super.free_blocks;
  Error error = commit_metadata();
  if (error != Error::Ok) {
    bitmap::clear(block_bitmap.data(), found);
    ++super.free_blocks;
    return error;
  }

  const std::vector<uint8_t> zeros(kBlockSize, 0);
  error = device->write_block(found, zeros.data());
  if (error != Error::Ok) {
    bitmap::clear(block_bitmap.data(), found);
    ++super.free_blocks;
    const Error rollback = commit_metadata();
    if (rollback != Error::Ok) {
      return rollback;
    }
    return error;
  }

  block = found;
  return Error::Ok;
}

Error FileSystem::Impl::free_block(uint32_t block) {
  if (block < super.data_block_start || block >= super.block_count) {
    return Error::InvalidArgument;
  }
  if (!bitmap::test(block_bitmap.data(), block)) {
    return Error::Corrupt;
  }
  bitmap::clear(block_bitmap.data(), block);
  ++super.free_blocks;
  return commit_metadata();
}

Error FileSystem::Impl::alloc_inode(uint32_t& index) {
  uint32_t found = 0;
  if (!bitmap::find_clear(inode_bitmap.data(), 0, super.inode_count, found)) {
    return Error::NoSpace;
  }
  if (super.free_inodes == 0) {
    return Error::Corrupt;
  }

  bitmap::set(inode_bitmap.data(), found);
  --super.free_inodes;
  Error error = commit_metadata();
  if (error != Error::Ok) {
    bitmap::clear(inode_bitmap.data(), found);
    ++super.free_inodes;
    return error;
  }

  const Inode blank{};
  error = write_inode(found, blank);
  if (error != Error::Ok) {
    bitmap::clear(inode_bitmap.data(), found);
    ++super.free_inodes;
    const Error rollback = commit_metadata();
    if (rollback != Error::Ok) {
      return rollback;
    }
    return error;
  }

  index = found;
  return Error::Ok;
}

Error FileSystem::Impl::free_inode_slot(uint32_t index) {
  if (index >= super.inode_count) {
    return Error::InvalidArgument;
  }
  if (!bitmap::test(inode_bitmap.data(), index)) {
    return Error::Corrupt;
  }
  bitmap::clear(inode_bitmap.data(), index);
  ++super.free_inodes;
  return commit_metadata();
}

}  // namespace minifs
