#include "fs_impl.hpp"

#include <cstring>
#include <vector>

namespace minifs {
namespace {

Error parse_path(std::string_view path, std::vector<std::string>& parts) {
  parts.clear();
  if (path.empty() || path.front() != '/') {
    return Error::InvalidPath;
  }
  if (path.find('\\') != std::string_view::npos) {
    return Error::InvalidPath;
  }
  if (path.size() > 1 && path.back() == '/') {
    path.remove_suffix(1);
  }
  if (path == "/") {
    return Error::Ok;
  }

  std::size_t begin = 1;
  while (begin <= path.size()) {
    const std::size_t end = path.find('/', begin);
    const std::size_t stop = end == std::string_view::npos ? path.size() : end;
    if (stop == begin) {
      return Error::InvalidPath;
    }
    const std::string_view part = path.substr(begin, stop - begin);
    if (part != "." && part != "..") {
      const Error error = check_file_name(part);
      if (error != Error::Ok) {
        return error;
      }
    }
    parts.emplace_back(part);
    if (end == std::string_view::npos) {
      break;
    }
    begin = end + 1;
  }
  return Error::Ok;
}

}  // namespace

Error FileSystem::mkdir(std::string_view path) {
  if (!impl_->mounted) {
    return Error::NotMounted;
  }

  uint32_t parent = 0;
  std::string name;
  Error error = impl_->walk_parent(path, parent, name);
  if (error != Error::Ok) {
    return error;
  }

  uint32_t existing = 0;
  error = impl_->lookup(parent, name, existing);
  if (error == Error::Ok) {
    return Error::AlreadyExists;
  }
  if (error != Error::NotFound) {
    return error;
  }

  uint32_t inode_index = 0;
  error = impl_->alloc_inode(inode_index);
  if (error != Error::Ok) {
    return error;
  }

  uint32_t block = 0;
  error = impl_->alloc_block(block);
  if (error != Error::Ok) {
    static_cast<void>(impl_->release_inode(inode_index));
    return error;
  }

  DirEntry entries[2]{};
  entries[0].inode = inode_index;
  set_entry_name(entries[0], ".");
  entries[1].inode = parent;
  set_entry_name(entries[1], "..");

  std::vector<uint8_t> raw(kBlockSize, 0);
  std::memcpy(raw.data(), entries, sizeof(entries));
  error = impl_->device.write_block(block, raw.data());
  if (error != Error::Ok) {
    static_cast<void>(impl_->free_block(block));
    static_cast<void>(impl_->release_inode(inode_index));
    return error;
  }

  Inode inode{};
  inode.type = static_cast<uint32_t>(FileType::Directory);
  inode.size = static_cast<uint32_t>(sizeof(entries));
  inode.link_count = 1;
  inode.direct[0] = block;
  error = impl_->write_inode(inode_index, inode);
  if (error != Error::Ok) {
    static_cast<void>(impl_->free_block(block));
    static_cast<void>(impl_->release_inode(inode_index));
    return error;
  }
  error = impl_->device.sync();
  if (error != Error::Ok) {
    static_cast<void>(impl_->release_inode(inode_index));
    return error;
  }

  error = impl_->add_entry(parent, name, inode_index);
  if (error != Error::Ok) {
    static_cast<void>(impl_->release_inode(inode_index));
    return error;
  }
  return Error::Ok;
}

Error FileSystem::create_file(std::string_view path) {
  if (!impl_->mounted) {
    return Error::NotMounted;
  }

  uint32_t parent = 0;
  std::string name;
  Error error = impl_->walk_parent(path, parent, name);
  if (error != Error::Ok) {
    return error;
  }

  uint32_t existing = 0;
  error = impl_->lookup(parent, name, existing);
  if (error == Error::Ok) {
    return Error::AlreadyExists;
  }
  if (error != Error::NotFound) {
    return error;
  }

  uint32_t inode_index = 0;
  error = impl_->alloc_inode(inode_index);
  if (error != Error::Ok) {
    return error;
  }

  Inode inode{};
  inode.type = static_cast<uint32_t>(FileType::Regular);
  inode.link_count = 1;
  error = impl_->write_inode(inode_index, inode);
  if (error != Error::Ok) {
    static_cast<void>(impl_->release_inode(inode_index));
    return error;
  }
  error = impl_->device.sync();
  if (error != Error::Ok) {
    static_cast<void>(impl_->release_inode(inode_index));
    return error;
  }

  error = impl_->add_entry(parent, name, inode_index);
  if (error != Error::Ok) {
    static_cast<void>(impl_->release_inode(inode_index));
    return error;
  }
  return Error::Ok;
}

Error FileSystem::unlink(std::string_view path) {
  if (!impl_->mounted) {
    return Error::NotMounted;
  }

  uint32_t parent = 0;
  std::string name;
  Error error = impl_->walk_parent(path, parent, name);
  if (error != Error::Ok) {
    return error;
  }

  uint32_t child = 0;
  error = impl_->lookup(parent, name, child);
  if (error != Error::Ok) {
    return error;
  }

  Inode inode{};
  error = impl_->read_inode(child, inode);
  if (error != Error::Ok) {
    return error;
  }
  if (inode.type == static_cast<uint32_t>(FileType::Directory)) {
    return Error::IsDirectory;
  }
  if (inode.type != static_cast<uint32_t>(FileType::Regular)) {
    return Error::Corrupt;
  }
  if (impl_->inode_is_open(child)) {
    return Error::Busy;
  }

  error = impl_->remove_entry(parent, name, child);
  if (error != Error::Ok) {
    return error;
  }
  return impl_->release_inode(child);
}

Error FileSystem::rmdir(std::string_view path) {
  if (!impl_->mounted) {
    return Error::NotMounted;
  }

  uint32_t parent = 0;
  std::string name;
  Error error = impl_->walk_parent(path, parent, name);
  if (error != Error::Ok) {
    return error;
  }

  uint32_t child = 0;
  error = impl_->lookup(parent, name, child);
  if (error != Error::Ok) {
    return error;
  }
  if (child == impl_->super.root_inode) {
    return Error::InvalidArgument;
  }

  Inode inode{};
  error = impl_->read_inode(child, inode);
  if (error != Error::Ok) {
    return error;
  }
  if (inode.type != static_cast<uint32_t>(FileType::Directory)) {
    return Error::NotDirectory;
  }

  bool empty = false;
  error = impl_->is_directory_empty(child, empty);
  if (error != Error::Ok) {
    return error;
  }
  if (!empty) {
    return Error::NotEmpty;
  }

  error = impl_->remove_entry(parent, name, child);
  if (error != Error::Ok) {
    return error;
  }
  return impl_->release_inode(child);
}

Error FileSystem::stat(std::string_view path, FileStat& out) const {
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
  if (inode.type != static_cast<uint32_t>(FileType::Regular) &&
      inode.type != static_cast<uint32_t>(FileType::Directory)) {
    return Error::Corrupt;
  }

  out.type = static_cast<FileType>(inode.type);
  out.inode = index;
  out.size = inode.size;
  out.data_blocks = 0;
  for (uint32_t pointer : inode.direct) {
    if (pointer != 0) {
      ++out.data_blocks;
    }
  }
  return Error::Ok;
}

Error FileSystem::list(std::string_view path, std::vector<DirItem>& out) const {
  if (!impl_->mounted) {
    return Error::NotMounted;
  }

  uint32_t dir_index = 0;
  Error error = impl_->resolve(path, dir_index);
  if (error != Error::Ok) {
    return error;
  }

  Inode dir{};
  error = impl_->load_directory(dir_index, dir);
  if (error != Error::Ok) {
    return error;
  }

  out.clear();
  for (uint32_t offset = 0; offset < dir.size; offset += static_cast<uint32_t>(sizeof(DirEntry))) {
    DirEntry entry{};
    uint32_t transferred = 0;
    error = impl_->read_at(dir, offset, &entry, static_cast<uint32_t>(sizeof(entry)), transferred);
    if (error != Error::Ok) {
      return error;
    }
    if (transferred != sizeof(entry)) {
      return Error::Corrupt;
    }
    if (entry.inode == kInvalidInode) {
      continue;
    }

    const std::string name = entry_name(entry);
    if (is_dot_or_dotdot(name)) {
      continue;
    }

    Inode child{};
    error = impl_->read_inode(entry.inode, child);
    if (error != Error::Ok) {
      return error;
    }
    if (child.type != static_cast<uint32_t>(FileType::Regular) &&
        child.type != static_cast<uint32_t>(FileType::Directory)) {
      return Error::Corrupt;
    }

    DirItem item;
    item.name = name;
    item.type = static_cast<FileType>(child.type);
    item.inode = entry.inode;
    item.size = child.size;
    out.push_back(item);
  }
  return Error::Ok;
}

Error FileSystem::Impl::load_directory(uint32_t index, Inode& inode) const {
  const Error error = read_inode(index, inode);
  if (error != Error::Ok) {
    return error;
  }
  if (inode.type != static_cast<uint32_t>(FileType::Directory)) {
    return Error::NotDirectory;
  }
  if (inode.size % sizeof(DirEntry) != 0) {
    return Error::Corrupt;
  }
  return Error::Ok;
}

Error FileSystem::Impl::lookup(uint32_t dir_index, std::string_view name, uint32_t& child) const {
  Inode dir{};
  Error error = load_directory(dir_index, dir);
  if (error != Error::Ok) {
    return error;
  }

  for (uint32_t offset = 0; offset < dir.size; offset += static_cast<uint32_t>(sizeof(DirEntry))) {
    DirEntry entry{};
    uint32_t transferred = 0;
    error = read_at(dir, offset, &entry, static_cast<uint32_t>(sizeof(entry)), transferred);
    if (error != Error::Ok) {
      return error;
    }
    if (transferred != sizeof(entry)) {
      return Error::Corrupt;
    }
    if (entry.inode == kInvalidInode) {
      continue;
    }
    if (entry_name(entry) == name) {
      child = entry.inode;
      return Error::Ok;
    }
  }
  return Error::NotFound;
}

Error FileSystem::Impl::add_entry(uint32_t dir_index, std::string_view name, uint32_t child) {
  const Error name_error = check_file_name(name);
  if (name_error != Error::Ok) {
    return name_error;
  }

  Inode dir{};
  Error error = load_directory(dir_index, dir);
  if (error != Error::Ok) {
    return error;
  }

  bool found_hole = false;
  uint32_t hole = 0;
  for (uint32_t offset = 0; offset < dir.size; offset += static_cast<uint32_t>(sizeof(DirEntry))) {
    DirEntry entry{};
    uint32_t transferred = 0;
    error = read_at(dir, offset, &entry, static_cast<uint32_t>(sizeof(entry)), transferred);
    if (error != Error::Ok) {
      return error;
    }
    if (transferred != sizeof(entry)) {
      return Error::Corrupt;
    }
    if (entry.inode == kInvalidInode) {
      if (!found_hole) {
        found_hole = true;
        hole = offset;
      }
      continue;
    }
    if (entry_name(entry) == name) {
      return Error::AlreadyExists;
    }
  }

  DirEntry created{};
  created.inode = child;
  set_entry_name(created, name);
  const uint32_t at = found_hole ? hole : dir.size;
  uint32_t transferred = 0;
  error = write_at(dir_index, at, &created, static_cast<uint32_t>(sizeof(created)), transferred);
  if (error != Error::Ok) {
    return error;
  }
  if (transferred != sizeof(created)) {
    return Error::Corrupt;
  }
  return Error::Ok;
}

Error FileSystem::Impl::remove_entry(uint32_t dir_index, std::string_view name, uint32_t& child) {
  if (name == "." || name == "..") {
    return Error::InvalidArgument;
  }

  Inode dir{};
  Error error = load_directory(dir_index, dir);
  if (error != Error::Ok) {
    return error;
  }

  for (uint32_t offset = 0; offset < dir.size; offset += static_cast<uint32_t>(sizeof(DirEntry))) {
    DirEntry entry{};
    uint32_t transferred = 0;
    error = read_at(dir, offset, &entry, static_cast<uint32_t>(sizeof(entry)), transferred);
    if (error != Error::Ok) {
      return error;
    }
    if (transferred != sizeof(entry) || entry.inode == kInvalidInode) {
      if (transferred != sizeof(entry)) {
        return Error::Corrupt;
      }
      continue;
    }
    if (entry_name(entry) != name) {
      continue;
    }

    child = entry.inode;
    DirEntry hole{};
    hole.inode = kInvalidInode;
    return write_at(dir_index, offset, &hole, static_cast<uint32_t>(sizeof(hole)), transferred);
  }
  return Error::NotFound;
}

Error FileSystem::Impl::is_directory_empty(uint32_t dir_index, bool& empty) const {
  Inode dir{};
  Error error = load_directory(dir_index, dir);
  if (error != Error::Ok) {
    return error;
  }

  empty = true;
  for (uint32_t offset = 0; offset < dir.size; offset += static_cast<uint32_t>(sizeof(DirEntry))) {
    DirEntry entry{};
    uint32_t transferred = 0;
    error = read_at(dir, offset, &entry, static_cast<uint32_t>(sizeof(entry)), transferred);
    if (error != Error::Ok) {
      return error;
    }
    if (transferred != sizeof(entry)) {
      return Error::Corrupt;
    }
    if (entry.inode == kInvalidInode) {
      continue;
    }
    if (!is_dot_or_dotdot(entry_name(entry))) {
      empty = false;
      break;
    }
  }
  return Error::Ok;
}

Error FileSystem::Impl::resolve(std::string_view path, uint32_t& inode) const {
  std::vector<std::string> parts;
  const Error error = parse_path(path, parts);
  if (error != Error::Ok) {
    return error;
  }
  return walk(parts, inode);
}

Error FileSystem::Impl::walk(const std::vector<std::string>& parts, uint32_t& inode) const {
  uint32_t current = super.root_inode;
  for (const std::string& part : parts) {
    uint32_t child = 0;
    const Error error = lookup(current, part, child);
    if (error != Error::Ok) {
      return error;
    }
    current = child;
  }
  inode = current;
  return Error::Ok;
}

Error FileSystem::Impl::walk_parent(std::string_view path, uint32_t& parent, std::string& name) const {
  std::vector<std::string> parts;
  Error error = parse_path(path, parts);
  if (error != Error::Ok) {
    return error;
  }
  if (parts.empty()) {
    return Error::InvalidPath;
  }
  name = parts.back();
  error = check_file_name(name);
  if (error != Error::Ok) {
    return error;
  }
  parts.pop_back();
  return walk(parts, parent);
}

}  // namespace minifs
