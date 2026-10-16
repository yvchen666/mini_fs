#pragma once

// NOR 驱动。中断里只把页缓冲拷走，并给队列里的下一条命令写寄存器。
// 磨损均衡、逻辑块这些事情不在这里做。

#include "device/nor_controller.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace minifs {

struct FlashOp {
  uint32_t command = 0;
  uint32_t address = 0;
  std::array<uint8_t, nor::kPageBytes> bytes{};
  bool done = false;
  Error result = Error::Ok;
};

class NorDriver {
 public:
  explicit NorDriver(NorController& controller);
  ~NorDriver();

  NorDriver(const NorDriver&) = delete;
  NorDriver& operator=(const NorDriver&) = delete;

  Error execute(std::vector<FlashOp>& ops);
  void reset_counters();

  uint32_t page_reads() const;
  uint32_t page_programs() const;
  uint32_t block_erases() const;
  uint32_t interrupts() const;

 private:
  void enqueue(FlashOp* op);
  void start_next();
  void on_irq();
  static void irq_entry(void* self);

  NorController& controller_;
  std::vector<FlashOp*> queue_;
  FlashOp* active_ = nullptr;
  uint32_t reads_ = 0;
  uint32_t programs_ = 0;
  uint32_t erases_ = 0;
  uint32_t interrupts_ = 0;
};

}  // namespace minifs
