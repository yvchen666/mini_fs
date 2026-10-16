#include "nor_controller.hpp"

#include "host/utf8_path.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>

#include "nor_layout.hpp"

namespace minifs {

using nor::ImageHeader;
using nor::PageSpare;
using nor::kBlockBytes;
using nor::kImageMagic;
using nor::kImageVersion;
using nor::kPageBytes;
using nor::kPageSize;
using nor::kPagesPerBlock;
using nor::kStateFree;

namespace {

PageSpare blank_spare() {
  PageSpare spare{};
  std::memset(&spare, 0xFF, sizeof(spare));
  spare.erase_count = 0;
  spare.state = kStateFree;
  return spare;
}

}  // namespace

Error NorController::create_image(const std::string& path, uint32_t logical_blocks) {
  close();
  if (path.empty() || logical_blocks == 0) {
    return Error::InvalidArgument;
  }

  header_ = ImageHeader{};
  header_.magic = kImageMagic;
  header_.version = kImageVersion;
  header_.page_size = kPageSize;
  header_.spare_size = nor::kSpareSize;
  header_.pages_per_block = kPagesPerBlock;
  header_.logical_blocks = logical_blocks;
  header_.physical_blocks = nor::physical_blocks_for(logical_blocks);

  flash_.assign(static_cast<std::size_t>(header_.physical_blocks) * kBlockBytes, static_cast<uint8_t>(0xFF));
  const PageSpare spare = blank_spare();
  for (uint32_t block = 0; block < header_.physical_blocks; ++block) {
    const std::size_t spare_at = static_cast<std::size_t>(block) * kBlockBytes + kPageSize;
    std::memcpy(flash_.data() + spare_at, &spare, sizeof(spare));
  }

  buffer_.assign(kPageBytes, static_cast<uint8_t>(0xFF));
  path_ = path;
  power_failed_ = false;
  armed_ = InjectedFault::None;
  status_ = nor::kStatusReady;
  open_ = true;
  return flush();
}

Error NorController::open_image(const std::string& path) {
  close();
  if (path.empty()) {
    return Error::InvalidArgument;
  }

  const std::filesystem::path file_path = utf8_path(path);
  std::error_code exists_error;
  if (!std::filesystem::exists(file_path, exists_error) || exists_error) {
    return Error::NotFound;
  }

  std::ifstream input(file_path, std::ios::binary);
  if (!input) {
    return Error::Io;
  }
  input.read(reinterpret_cast<char*>(&header_), sizeof(header_));
  if (!input) {
    return Error::Corrupt;
  }
  if (header_.magic != kImageMagic || header_.version != kImageVersion) {
    return Error::Corrupt;
  }
  if (header_.page_size != kPageSize || header_.spare_size != nor::kSpareSize ||
      header_.pages_per_block != kPagesPerBlock || header_.logical_blocks == 0 ||
      header_.physical_blocks != nor::physical_blocks_for(header_.logical_blocks)) {
    return Error::Corrupt;
  }

  flash_.assign(static_cast<std::size_t>(header_.physical_blocks) * kBlockBytes, static_cast<uint8_t>(0));
  input.read(reinterpret_cast<char*>(flash_.data()), static_cast<std::streamsize>(flash_.size()));
  if (!input) {
    flash_.clear();
    return Error::Corrupt;
  }

  buffer_.assign(kPageBytes, static_cast<uint8_t>(0xFF));
  path_ = path;
  power_failed_ = false;
  armed_ = InjectedFault::None;
  status_ = nor::kStatusReady;
  open_ = true;
  return Error::Ok;
}

void NorController::close() {
  if (open_) {
    static_cast<void>(flush());
  }
  flash_.clear();
  buffer_.clear();
  path_.clear();
  header_ = ImageHeader{};
  operation_ = Operation{};
  power_failed_ = false;
  armed_ = InjectedFault::None;
  open_ = false;
  status_ = nor::kStatusReady;
}

Error NorController::flush() const {
  if (!open_) {
    return Error::Io;
  }
  std::ofstream output(utf8_path(path_), std::ios::binary | std::ios::trunc);
  if (!output) {
    return Error::Io;
  }
  output.write(reinterpret_cast<const char*>(&header_), sizeof(header_));
  output.write(reinterpret_cast<const char*>(flash_.data()), static_cast<std::streamsize>(flash_.size()));
  if (!output) {
    return Error::Io;
  }
  return Error::Ok;
}

bool NorController::is_open() const {
  return open_;
}

bool NorController::power_failed() const {
  return power_failed_;
}

uint32_t NorController::logical_blocks() const {
  return header_.logical_blocks;
}

uint32_t NorController::physical_blocks() const {
  return header_.physical_blocks;
}

void NorController::set_irq_handler(IrqHandler handler, void* user) {
  irq_ = handler;
  irq_user_ = user;
}

void NorController::write_reg(uint32_t offset, uint32_t value) {
  if (offset == nor::kRegAddress) {
    address_ = value;
    return;
  }
  if (offset == nor::kRegIrqEnable) {
    irq_enable_ = value;
    return;
  }
  if (offset != nor::kRegCommand || !open_ || power_failed_) {
    return;
  }
  if (operation_.active || !address_ok(value, address_)) {
    status_ = nor::kStatusError | nor::kStatusIrq;
    raise_irq();
    return;
  }

  operation_.active = true;
  operation_.command = value;
  operation_.address = address_;
  operation_.cut = (armed_ == InjectedFault::PowerCutOnProgram && value == nor::kCmdProgram) ||
                   (armed_ == InjectedFault::PowerCutOnErase && value == nor::kCmdErase);
  if (operation_.cut) {
    armed_ = InjectedFault::None;
  }
  if (value == nor::kCmdRead) {
    operation_.ticks_total = nor::kReadTicks;
  } else if (value == nor::kCmdProgram) {
    operation_.ticks_total = nor::kProgramTicks;
  } else {
    operation_.ticks_total = nor::kEraseTicks;
  }
  operation_.ticks_left = operation_.ticks_total;
  status_ = nor::kStatusBusy;
}

uint32_t NorController::read_reg(uint32_t offset) const {
  if (offset == nor::kRegAddress) {
    return address_;
  }
  if (offset == nor::kRegStatus) {
    return status_;
  }
  if (offset == nor::kRegIrqEnable) {
    return irq_enable_;
  }
  if (offset == nor::kRegCommand) {
    return operation_.command;
  }
  return 0;
}

void NorController::write_buffer(uint32_t offset, uint8_t value) {
  if (offset < buffer_.size()) {
    buffer_[offset] = value;
  }
}

uint8_t NorController::read_buffer(uint32_t offset) const {
  if (offset >= buffer_.size()) {
    return 0xFF;
  }
  return buffer_[offset];
}

void NorController::clear_irq() {
  status_ &= ~nor::kStatusIrq;
}

void NorController::tick() {
  if (!operation_.active || power_failed_) {
    return;
  }

  const int step = operation_.ticks_total - operation_.ticks_left;
  // 编程按位与，只能把 1 写成 0。擦除把这一片恢复成 0xFF。
  apply_step(step);
  // 模拟断电切在一半拍数上，阵列里会留下半截操作。
  if (operation_.cut && step + 1 == operation_.ticks_total / 2) {
    fail_power();
    return;
  }

  --operation_.ticks_left;
  if (operation_.ticks_left == 0) {
    finish_ok();
  }
}

void NorController::arm_fault(InjectedFault fault) {
  armed_ = fault;
}

void NorController::raise_irq() {
  if (irq_enable_ == 0 || irq_ == nullptr) {
    return;
  }
  irq_(irq_user_);
}

void NorController::fail_power() {
  power_failed_ = true;
  operation_.active = false;
  status_ = nor::kStatusError | nor::kStatusIrq;
  static_cast<void>(flush());
  raise_irq();
}

void NorController::finish_ok() {
  operation_.active = false;
  status_ = nor::kStatusReady | nor::kStatusIrq;
  raise_irq();
}

void NorController::apply_step(int step) {
  if (operation_.command == nor::kCmdRead) {
    if (step == operation_.ticks_total - 1) {
      std::memcpy(buffer_.data(), flash_.data() + operation_.address, kPageBytes);
    }
    return;
  }

  if (operation_.command == nor::kCmdProgram) {
    const uint32_t slice = kPageBytes / static_cast<uint32_t>(operation_.ticks_total);
    const uint32_t begin = static_cast<uint32_t>(step) * slice;
    for (uint32_t i = 0; i < slice; ++i) {
      flash_[operation_.address + begin + i] =
          static_cast<uint8_t>(flash_[operation_.address + begin + i] & buffer_[begin + i]);
    }
    return;
  }

  const uint32_t slice = kBlockBytes / static_cast<uint32_t>(operation_.ticks_total);
  const uint32_t begin = static_cast<uint32_t>(step) * slice;
  std::memset(flash_.data() + operation_.address + begin, 0xFF, slice);
}

bool NorController::address_ok(uint32_t command, uint32_t address) const {
  if (flash_.empty()) {
    return false;
  }
  if (command == nor::kCmdErase) {
    return address % kBlockBytes == 0 && address + kBlockBytes <= flash_.size();
  }
  if (command == nor::kCmdRead || command == nor::kCmdProgram) {
    return address % kPageBytes == 0 && address + kPageBytes <= flash_.size();
  }
  return false;
}

}  // namespace minifs
