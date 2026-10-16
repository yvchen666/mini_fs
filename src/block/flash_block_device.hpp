#pragma once

#include "minifs/block_device.hpp"

#include "device/nor_controller.hpp"
#include "driver/nor_driver.hpp"
#include "ftl/ftl.hpp"

namespace minifs {

// 块层：文件系统的一次 4KB 读写，在这里变成驱动队列里的页编程和擦除。
class FlashBlockDevice : public BlockDevice {
 public:
  FlashBlockDevice();
  ~FlashBlockDevice() override;

  Error create(const std::string& path, uint32_t block_count) override;
  Error open(const std::string& path) override;
  void close() override;

  bool is_open() const override;
  uint32_t block_count() const override;

  Error read_block(uint32_t index, void* buffer) const override;
  Error write_block(uint32_t index, const void* buffer) override;
  Error sync() override;

  Error storage_info(StorageInfo& out) const override;
  Error inject_fault(InjectedFault fault) override;

 private:
  mutable NorController controller_;
  mutable NorDriver driver_;
  mutable Ftl ftl_;
  bool open_ = false;
};

}  // namespace minifs
