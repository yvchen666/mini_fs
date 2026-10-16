#pragma once

// 模拟闪存控制器：一组寄存器、一块页缓冲，以及按 NOR 规则变化的存储阵列。
// 驱动只许碰寄存器和页缓冲。阵列的位变化只发生在 tick() 里。

#include "device/nor_layout.hpp"
#include "minifs/types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace minifs {

class NorController {
 public:
  using IrqHandler = void (*)(void* user);

  Error create_image(const std::string& path, uint32_t logical_blocks);
  Error open_image(const std::string& path);
  void close();
  Error flush() const;

  bool is_open() const;
  bool power_failed() const;
  uint32_t logical_blocks() const;
  uint32_t physical_blocks() const;

  void set_irq_handler(IrqHandler handler, void* user);
  void write_reg(uint32_t offset, uint32_t value);
  uint32_t read_reg(uint32_t offset) const;
  void write_buffer(uint32_t offset, uint8_t value);
  uint8_t read_buffer(uint32_t offset) const;
  void clear_irq();

  void tick();
  void arm_fault(InjectedFault fault);

 private:
  struct Operation {
    bool active = false;
    bool cut = false;
    uint32_t command = 0;
    uint32_t address = 0;
    int ticks_left = 0;
    int ticks_total = 0;
  };

  void raise_irq();
  void fail_power();
  void finish_ok();
  void apply_step(int step);
  bool address_ok(uint32_t command, uint32_t address) const;

  std::string path_;
  nor::ImageHeader header_{};
  std::vector<uint8_t> flash_;
  std::vector<uint8_t> buffer_;
  uint32_t address_ = 0;
  uint32_t status_ = nor::kStatusReady;
  uint32_t irq_enable_ = 1;
  Operation operation_{};
  InjectedFault armed_ = InjectedFault::None;
  bool power_failed_ = false;
  bool open_ = false;
  IrqHandler irq_ = nullptr;
  void* irq_user_ = nullptr;
};

}  // namespace minifs
