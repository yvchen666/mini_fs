#pragma once

// 把「可以覆盖写的逻辑块」变成 NOR 上的「写到新擦除块，再废掉旧块」。
// 映射表只在内存里，挂载时靠扫描 spare 重建。

#include "driver/nor_driver.hpp"

#include <cstdint>
#include <vector>

namespace minifs {

class Ftl {
 public:
  explicit Ftl(NorDriver& driver);

  Error scan(uint32_t logical_blocks, uint32_t physical_blocks);
  Error read(uint32_t lba, uint8_t* destination);
  Error write(uint32_t lba, const uint8_t* source);
  StorageInfo info() const;

 private:
  enum class PhysKind { Free, Valid, Garbage };

  Error read_page(uint32_t block, uint32_t page, uint8_t* data, nor::PageSpare& spare);
  Error allocate(uint32_t& phys);
  Error reclaim_one(uint32_t& phys);
  uint32_t find_kind(PhysKind kind) const;
  FlashOp make_program(uint32_t block, uint32_t page, const uint8_t* data, const nor::PageSpare& spare) const;
  FlashOp make_erase(uint32_t block) const;

  NorDriver& driver_;
  uint32_t logical_blocks_ = 0;
  uint32_t physical_blocks_ = 0;
  uint32_t next_sequence_ = 1;
  std::vector<uint32_t> map_;
  std::vector<uint32_t> erase_count_;
  std::vector<PhysKind> kind_;
};

}  // namespace minifs
