#pragma once

// 模拟 SPI NOR 的几何参数。文件系统看不到这些数。
// 编程单位是 256 字节的页，擦除单位是 16 页（正好 4KB 数据）。
// 每页后面有 32 字节 spare，用来放 FTL 的提交信息。

#include "minifs/types.hpp"

#include <cstdint>

namespace minifs::nor {

inline constexpr uint32_t kImageMagic = 0x31524F4Eu;  // 字节 'N' 'O' 'R' '1'
inline constexpr uint32_t kImageVersion = 1;
inline constexpr uint32_t kPageSize = 256;
inline constexpr uint32_t kSpareSize = 32;
inline constexpr uint32_t kPagesPerBlock = kBlockSize / kPageSize;
inline constexpr uint32_t kPageBytes = kPageSize + kSpareSize;
inline constexpr uint32_t kBlockBytes = kPagesPerBlock * kPageBytes;

inline constexpr int kReadTicks = 2;
inline constexpr int kProgramTicks = 8;
inline constexpr int kEraseTicks = 32;

inline constexpr uint8_t kStateFree = 0xFFu;
inline constexpr uint8_t kStateUsed = 0xFEu;
inline constexpr uint8_t kStateObsolete = 0x00u;

inline constexpr uint32_t kCmdRead = 1;
inline constexpr uint32_t kCmdProgram = 2;
inline constexpr uint32_t kCmdErase = 3;

inline constexpr uint32_t kRegCommand = 0x00;
inline constexpr uint32_t kRegAddress = 0x04;
inline constexpr uint32_t kRegStatus = 0x08;
inline constexpr uint32_t kRegIrqEnable = 0x0C;

inline constexpr uint32_t kStatusBusy = 1u << 0;
inline constexpr uint32_t kStatusError = 1u << 1;
inline constexpr uint32_t kStatusReady = 1u << 2;
inline constexpr uint32_t kStatusIrq = 1u << 3;

inline constexpr uint32_t kInvalidPhys = 0xffffffffu;

struct ImageHeader {
  uint32_t magic;
  uint32_t version;
  uint32_t page_size;
  uint32_t spare_size;
  uint32_t pages_per_block;
  uint32_t physical_blocks;
  uint32_t logical_blocks;
};

// 只放在每个擦除块的第 0 页 spare 里。位只能从 1 写成 0。
struct PageSpare {
  uint32_t erase_count;
  uint32_t logical_block;
  uint32_t sequence;
  uint32_t checksum;
  uint8_t state;
  uint8_t pad[15];
};

static_assert(kPagesPerBlock == 16, "擦除块的数据区必须等于文件系统块大小");
static_assert(sizeof(PageSpare) == kSpareSize, "spare 必须是 32 字节");
static_assert(kPageBytes % kProgramTicks == 0, "编程要能按 tick 切成整片");
static_assert(kBlockBytes % kEraseTicks == 0, "擦除要能按 tick 切成整片");

inline uint32_t physical_blocks_for(uint32_t logical_blocks) {
  return logical_blocks * 2;
}

inline uint32_t checksum_of(const uint8_t* data, uint32_t size) {
  uint32_t hash = 2166136261u;
  for (uint32_t i = 0; i < size; ++i) {
    hash ^= data[i];
    hash *= 16777619u;
  }
  return hash;
}

inline bool all_ones(const uint8_t* data, uint32_t size) {
  for (uint32_t i = 0; i < size; ++i) {
    if (data[i] != 0xFFu) {
      return false;
    }
  }
  return true;
}

}  // namespace minifs::nor
