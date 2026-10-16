#include "driver/nor_driver.hpp"

namespace minifs {

NorDriver::NorDriver(NorController& controller) : controller_(controller) {
  controller_.set_irq_handler(&NorDriver::irq_entry, this);
}

NorDriver::~NorDriver() {
  controller_.set_irq_handler(nullptr, nullptr);
  active_ = nullptr;
  queue_.clear();
}

Error NorDriver::execute(std::vector<FlashOp>& ops) {
  if (controller_.power_failed()) {
    return Error::PowerLoss;
  }
  if (ops.empty()) {
    return Error::Ok;
  }

  for (FlashOp& op : ops) {
    op.done = false;
    op.result = Error::Ok;
    enqueue(&op);
  }

  int guard = 0;
  while (active_ != nullptr || !queue_.empty()) {
    if (controller_.power_failed()) {
      if (active_ != nullptr) {
        active_->done = true;
        active_->result = Error::PowerLoss;
        active_ = nullptr;
      }
      for (FlashOp* waiting : queue_) {
        waiting->done = true;
        waiting->result = Error::PowerLoss;
      }
      queue_.clear();
      break;
    }
    controller_.tick();
    if (++guard > 1000000) {
      return Error::Io;
    }
  }

  for (const FlashOp& op : ops) {
    if (op.result != Error::Ok) {
      return op.result;
    }
  }
  return Error::Ok;
}

void NorDriver::reset_counters() {
  reads_ = 0;
  programs_ = 0;
  erases_ = 0;
  interrupts_ = 0;
  active_ = nullptr;
  queue_.clear();
}

uint32_t NorDriver::page_reads() const {
  return reads_;
}

uint32_t NorDriver::page_programs() const {
  return programs_;
}

uint32_t NorDriver::block_erases() const {
  return erases_;
}

uint32_t NorDriver::interrupts() const {
  return interrupts_;
}

void NorDriver::enqueue(FlashOp* op) {
  queue_.push_back(op);
  if (active_ == nullptr) {
    start_next();
  }
}

void NorDriver::start_next() {
  if (active_ != nullptr || queue_.empty() || controller_.power_failed()) {
    return;
  }
  active_ = queue_.front();
  queue_.erase(queue_.begin());

  if (active_->command == nor::kCmdProgram || active_->command == nor::kCmdRead) {
    for (uint32_t i = 0; i < active_->bytes.size(); ++i) {
      controller_.write_buffer(i, active_->bytes[i]);
    }
  }
  controller_.write_reg(nor::kRegAddress, active_->address);
  // 最后写命令寄存器，控制器才进入 BUSY。前面只是在填页缓冲和地址。
  controller_.write_reg(nor::kRegCommand, active_->command);

  if (active_->command == nor::kCmdRead) {
    ++reads_;
  } else if (active_->command == nor::kCmdProgram) {
    ++programs_;
  } else if (active_->command == nor::kCmdErase) {
    ++erases_;
  }
}

void NorDriver::on_irq() {
  // 一条读、编程或擦除做完了。这里只取结果并启动下一条，不碰文件和映射。
  ++interrupts_;
  const uint32_t status = controller_.read_reg(nor::kRegStatus);
  if (active_ != nullptr) {
    if ((status & nor::kStatusError) != 0) {
      active_->result = Error::PowerLoss;
    } else if (active_->command == nor::kCmdRead) {
      for (uint32_t i = 0; i < active_->bytes.size(); ++i) {
        active_->bytes[i] = controller_.read_buffer(i);
      }
      active_->result = Error::Ok;
    } else {
      active_->result = Error::Ok;
    }
    active_->done = true;
    active_ = nullptr;
  }
  controller_.clear_irq();
  if (!controller_.power_failed()) {
    start_next();
  }
}

void NorDriver::irq_entry(void* self) {
  static_cast<NorDriver*>(self)->on_irq();
}

}  // namespace minifs
