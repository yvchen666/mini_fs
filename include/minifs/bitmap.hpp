#pragma once

#include <cstdint>

// 一块 4KB 位图能管 32768 个对象。第 index 位落在第 index/8 个字节的低位起第 index%8 位。

namespace minifs::bitmap {

inline bool test(const uint8_t* bits, uint32_t index) {
  return (bits[index / 8] >> (index % 8)) & 0x1u;
}

inline void set(uint8_t* bits, uint32_t index) {
  bits[index / 8] = static_cast<uint8_t>(bits[index / 8] | (1u << (index % 8)));
}

inline void clear(uint8_t* bits, uint32_t index) {
  bits[index / 8] = static_cast<uint8_t>(bits[index / 8] & ~(1u << (index % 8)));
}

inline bool find_clear(const uint8_t* bits, uint32_t begin, uint32_t end, uint32_t& index) {
  for (uint32_t i = begin; i < end; ++i) {
    if (!test(bits, i)) {
      index = i;
      return true;
    }
  }
  return false;
}

inline uint32_t count_clear(const uint8_t* bits, uint32_t begin, uint32_t end) {
  uint32_t count = 0;
  for (uint32_t i = begin; i < end; ++i) {
    if (!test(bits, i)) {
      ++count;
    }
  }
  return count;
}

}  // namespace minifs::bitmap
