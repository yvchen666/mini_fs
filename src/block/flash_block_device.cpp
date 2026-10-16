#include "flash_block_device.hpp"

#include <cstring>

namespace minifs {

FlashBlockDevice::FlashBlockDevice() : driver_(controller_), ftl_(driver_) {}

FlashBlockDevice::~FlashBlockDevice() {
  close();
}

Error FlashBlockDevice::create(const std::string& path, uint32_t block_count) {
  close();
  const Error created = controller_.create_image(path, block_count);
  if (created != Error::Ok) {
    return created;
  }
  driver_.reset_counters();
  const Error scanned = ftl_.scan(controller_.logical_blocks(), controller_.physical_blocks());
  if (scanned != Error::Ok) {
    close();
    return scanned;
  }
  open_ = true;
  return Error::Ok;
}

Error FlashBlockDevice::open(const std::string& path) {
  close();
  const Error opened = controller_.open_image(path);
  if (opened != Error::Ok) {
    return opened;
  }
  driver_.reset_counters();
  const Error scanned = ftl_.scan(controller_.logical_blocks(), controller_.physical_blocks());
  if (scanned != Error::Ok) {
    close();
    return scanned;
  }
  open_ = true;
  return Error::Ok;
}

void FlashBlockDevice::close() {
  if (controller_.is_open()) {
    static_cast<void>(controller_.flush());
  }
  controller_.close();
  driver_.reset_counters();
  open_ = false;
}

bool FlashBlockDevice::is_open() const {
  return open_;
}

uint32_t FlashBlockDevice::block_count() const {
  return controller_.logical_blocks();
}

Error FlashBlockDevice::read_block(uint32_t index, void* buffer) const {
  if (!open_) {
    return Error::Io;
  }
  if (controller_.power_failed()) {
    return Error::PowerLoss;
  }
  return ftl_.read(index, static_cast<uint8_t*>(buffer));
}

Error FlashBlockDevice::write_block(uint32_t index, const void* buffer) {
  if (!open_) {
    return Error::Io;
  }
  if (controller_.power_failed()) {
    return Error::PowerLoss;
  }
  return ftl_.write(index, static_cast<const uint8_t*>(buffer));
}

Error FlashBlockDevice::sync() {
  if (!open_) {
    return Error::Io;
  }
  return controller_.flush();
}

Error FlashBlockDevice::storage_info(StorageInfo& out) const {
  if (!open_) {
    return Error::Io;
  }
  out = ftl_.info();
  return Error::Ok;
}

Error FlashBlockDevice::inject_fault(InjectedFault fault) {
  if (!open_) {
    return Error::Io;
  }
  controller_.arm_fault(fault);
  return Error::Ok;
}

}  // namespace minifs
