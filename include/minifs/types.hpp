#pragma once

#include <cstdint>
#include <string>
#include <type_traits>

// 磁盘上的整数按本机小端直接存放。Windows x64 就是小端，demo 不额外做字节交换。

namespace minifs {

inline constexpr uint32_t kMagic = 0x3153464Du;  // 字节序 'M' 'F' 'S' '1'
inline constexpr uint32_t kVersion = 1;
inline constexpr uint32_t kBlockSize = 4096;
inline constexpr uint32_t kDirectPointers = 12;
inline constexpr uint32_t kMaxFileSize = kDirectPointers * kBlockSize;  // 48 KiB
inline constexpr uint32_t kNameCapacity = 28;
inline constexpr uint32_t kMaxNameLen = kNameCapacity - 1;
inline constexpr uint32_t kDefaultBlockCount = 256;  // 1 MiB
inline constexpr uint32_t kDefaultInodeCount = 64;
inline constexpr uint32_t kMaxOpenFiles = 16;

// 数据块指针用 0 表示“还没分配”。块 0 是超级块，永远不会变成数据块，所以 0 可以当空指针。
// inode 号 0 是根目录，不能再用 0 表示“没有这项”，目录项的空位用 kInvalidInode。
inline constexpr uint32_t kInvalidInode = 0xffffffffu;

enum class FileType : uint32_t {
  Free = 0,
  Regular = 1,
  Directory = 2,
};

enum class OpenMode {
  ReadOnly,
  ReadWrite,
  Truncate,
};

enum class Error {
  Ok = 0,
  Io,
  NotMounted,
  InvalidPath,
  NotFound,
  AlreadyExists,
  NotDirectory,
  IsDirectory,
  NotEmpty,
  NoSpace,
  TooLarge,
  BadFd,
  Busy,
  Corrupt,
  InvalidArgument,
  TooManyOpen,
};

// 块 0 的前一段。其余字节保持 0。
struct Superblock {
  uint32_t magic;
  uint32_t version;
  uint32_t block_size;
  uint32_t block_count;
  uint32_t inode_count;
  uint32_t block_bitmap_block;
  uint32_t inode_bitmap_block;
  uint32_t inode_table_block;
  uint32_t inode_table_blocks;
  uint32_t data_block_start;
  uint32_t root_inode;
  uint32_t free_blocks;
  uint32_t free_inodes;
};

// 64 字节，正好整除 4KB，一个 inode 不会跨到下一块。
// link_count 第一版恒为 1。reserved 只用于把结构补齐到 64 字节。
struct Inode {
  uint32_t type;
  uint32_t size;
  uint32_t link_count;
  uint32_t direct[kDirectPointers];
  uint32_t reserved;
};

// 32 字节。inode == kInvalidInode 的槽位可以再次使用。
struct DirEntry {
  uint32_t inode;
  char name[kNameCapacity];
};

struct FileStat {
  FileType type = FileType::Free;
  uint32_t inode = 0;
  uint32_t size = 0;
  uint32_t data_blocks = 0;
};

struct DirItem {
  std::string name;
  FileType type = FileType::Free;
  uint32_t inode = 0;
  uint32_t size = 0;
};

struct DiskInfo {
  uint32_t block_size = 0;
  uint32_t block_count = 0;
  uint32_t free_blocks = 0;
  uint32_t inode_count = 0;
  uint32_t free_inodes = 0;
  uint32_t data_block_start = 0;
};

static_assert(sizeof(Inode) == 64, "inode 必须是 64 字节");
static_assert(sizeof(DirEntry) == 32, "目录项必须是 32 字节");
static_assert(sizeof(Superblock) <= kBlockSize, "超级块必须放得进块 0");
static_assert(kBlockSize % sizeof(Inode) == 0, "inode 不能跨块");
static_assert(kBlockSize % sizeof(DirEntry) == 0, "目录项不能跨块");
static_assert(std::is_trivially_copyable<Superblock>::value, "超级块按字节拷贝");
static_assert(std::is_trivially_copyable<Inode>::value, "inode 按字节拷贝");
static_assert(std::is_trivially_copyable<DirEntry>::value, "目录项按字节拷贝");

inline const char* error_message(Error error) {
  switch (error) {
    case Error::Ok:
      return "成功";
    case Error::Io:
      return "磁盘读写失败";
    case Error::NotMounted:
      return "还没有挂载磁盘";
    case Error::InvalidPath:
      return "路径无效，请使用以 / 开头的绝对路径";
    case Error::NotFound:
      return "找不到目标";
    case Error::AlreadyExists:
      return "已经存在";
    case Error::NotDirectory:
      return "这不是目录";
    case Error::IsDirectory:
      return "这是目录";
    case Error::NotEmpty:
      return "目录不为空";
    case Error::NoSpace:
      return "磁盘空间或 inode 不足";
    case Error::TooLarge:
      return "文件超过 48KB 上限";
    case Error::BadFd:
      return "无效的文件描述符";
    case Error::Busy:
      return "文件正在打开，不能删除";
    case Error::Corrupt:
      return "磁盘镜像损坏或不是 minifs";
    case Error::InvalidArgument:
      return "参数无效";
    case Error::TooManyOpen:
      return "打开的文件过多";
  }
  return "未知错误";
}

}  // namespace minifs
