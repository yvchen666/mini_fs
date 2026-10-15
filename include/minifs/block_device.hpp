#pragma once

#include "minifs/types.hpp"

#include <cstdint>
#include <fstream>
#include <string>

namespace minifs {

// 把一个普通文件当成磁盘。上层只按块号读写，不自己计算文件偏移。
class BlockDevice {
 public:
  Error create(const std::string& path, uint32_t block_count);
  Error open(const std::string& path);
  void close();

  bool is_open() const;
  uint32_t block_count() const;

  Error read_block(uint32_t index, void* buffer) const;
  Error write_block(uint32_t index, const void* buffer);
  Error sync();

 private:
  // read_block 需要在 const 路径上移动文件指针。
  mutable std::fstream file_;
  uint32_t block_count_ = 0;
};

}  // namespace minifs
