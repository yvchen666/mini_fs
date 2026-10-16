#pragma once

#include "minifs/types.hpp"

#include <cstdint>
#include <string>

namespace minifs {

// 文件系统只依赖这个接口：按逻辑块号读写 4KB。
// 下面可以是模拟 NOR，也可以以后换成别的介质，调用方不用改。
class BlockDevice {
 public:
  virtual ~BlockDevice() = default;

  virtual Error create(const std::string& path, uint32_t block_count) = 0;
  virtual Error open(const std::string& path) = 0;
  virtual void close() = 0;

  virtual bool is_open() const = 0;
  virtual uint32_t block_count() const = 0;

  virtual Error read_block(uint32_t index, void* buffer) const = 0;
  virtual Error write_block(uint32_t index, const void* buffer) = 0;
  virtual Error sync() = 0;

  virtual Error storage_info(StorageInfo& out) const = 0;
  virtual Error inject_fault(InjectedFault fault) = 0;
};

}  // namespace minifs
