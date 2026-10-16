#include "ftl/ftl.hpp"

#include <cstring>

namespace minifs {
namespace {

using nor::PageSpare;
using nor::kBlockBytes;
using nor::kInvalidPhys;
using nor::kPageBytes;
using nor::kPageSize;
using nor::kPagesPerBlock;
using nor::kStateFree;
using nor::kStateUsed;

PageSpare spare_from_bytes(const FlashOp& op) {
  PageSpare spare{};
  std::memcpy(&spare, op.bytes.data() + kPageSize, sizeof(spare));
  return spare;
}

bool spare_is_blank(const PageSpare& spare) {
  return spare.logical_block == kInvalidPhys && spare.sequence == kInvalidPhys &&
         spare.checksum == kInvalidPhys && spare.state == kStateFree;
}

}  // namespace

Ftl::Ftl(NorDriver& driver) : driver_(driver) {}

Error Ftl::scan(uint32_t logical_blocks, uint32_t physical_blocks) {
  logical_blocks_ = logical_blocks;
  physical_blocks_ = physical_blocks;
  next_sequence_ = 1;
  map_.assign(logical_blocks_, kInvalidPhys);
  erase_count_.assign(physical_blocks_, 0);
  kind_.assign(physical_blocks_, PhysKind::Garbage);

  struct Best {
    uint32_t phys = kInvalidPhys;
    uint32_t sequence = 0;
  };
  std::vector<Best> best(logical_blocks_);

  for (uint32_t phys = 0; phys < physical_blocks_; ++phys) {
    std::vector<uint8_t> data(kBlockSize, 0);
    PageSpare spare{};
    bool data_blank = true;
    for (uint32_t page = 0; page < kPagesPerBlock; ++page) {
      uint8_t page_data[kPageSize];
      PageSpare page_spare{};
      const Error error = read_page(phys, page, page_data, page_spare);
      if (error != Error::Ok) {
        return error;
      }
      std::memcpy(data.data() + static_cast<std::size_t>(page) * kPageSize, page_data, kPageSize);
      if (!nor::all_ones(page_data, kPageSize)) {
        data_blank = false;
      }
      if (page == 0) {
        spare = page_spare;
      }
    }

    erase_count_[phys] = spare.erase_count;
    const bool valid = spare.state == kStateUsed && spare.logical_block < logical_blocks_ &&
                       spare.checksum == nor::checksum_of(data.data(), kBlockSize);
    if (valid) {
      Best& chosen = best[spare.logical_block];
      if (chosen.phys != kInvalidPhys && spare.sequence < chosen.sequence) {
        kind_[phys] = PhysKind::Garbage;
        continue;
      }
      if (chosen.phys != kInvalidPhys) {
        kind_[chosen.phys] = PhysKind::Garbage;
      }
      chosen.phys = phys;
      chosen.sequence = spare.sequence;
      kind_[phys] = PhysKind::Valid;
      if (spare.sequence >= next_sequence_ && spare.sequence != kInvalidPhys) {
        next_sequence_ = spare.sequence + 1;
      }
      continue;
    }

    if (spare_is_blank(spare) && data_blank) {
      kind_[phys] = PhysKind::Free;
      continue;
    }
    kind_[phys] = PhysKind::Garbage;
  }

  for (uint32_t lba = 0; lba < logical_blocks_; ++lba) {
    map_[lba] = best[lba].phys;
  }
  return Error::Ok;
}

Error Ftl::read(uint32_t lba, uint8_t* destination) {
  if (lba >= logical_blocks_ || destination == nullptr) {
    return Error::InvalidArgument;
  }
  const uint32_t phys = map_[lba];
  if (phys == kInvalidPhys) {
    // 擦除态是 0xFF。没写过的逻辑块对文件系统呈现为全 0。
    std::memset(destination, 0, kBlockSize);
    return Error::Ok;
  }

  for (uint32_t page = 0; page < kPagesPerBlock; ++page) {
    PageSpare spare{};
    const Error error = read_page(phys, page, destination + static_cast<std::size_t>(page) * kPageSize, spare);
    if (error != Error::Ok) {
      return error;
    }
  }
  return Error::Ok;
}

// 文件系统的一次 4KB 覆盖写。闪存不能原地改，所以写到新的擦除块上。
Error Ftl::write(uint32_t lba, const uint8_t* source) {
  if (lba >= logical_blocks_ || source == nullptr) {
    return Error::InvalidArgument;
  }

  uint32_t phys = kInvalidPhys;
  Error error = allocate(phys);
  if (error != Error::Ok) {
    return error;
  }

  const uint32_t sequence = next_sequence_++;
  const uint32_t sum = nor::checksum_of(source, kBlockSize);
  std::vector<FlashOp> ops;
  ops.reserve(kPagesPerBlock + 4);

  // 先写 16 页数据。spare 仍是 0xFF，重新挂载时还不会承认这块。
  for (uint32_t page = 0; page < kPagesPerBlock; ++page) {
    PageSpare untouched{};
    std::memset(&untouched, 0xFF, sizeof(untouched));
    ops.push_back(make_program(phys, page, source + static_cast<std::size_t>(page) * kPageSize, untouched));
  }

  PageSpare commit{};
  std::memset(&commit, 0xFF, sizeof(commit));
  commit.erase_count = erase_count_[phys];
  commit.logical_block = lba;
  commit.sequence = sequence;
  commit.checksum = sum;
  commit.state = kStateUsed;
  // 提交点。这条 spare 写成功后，扫描才会把新数据算进映射。
  ops.push_back(make_program(phys, 0, nullptr, commit));

  const uint32_t old = map_[lba];
  // 旧块先标作废再擦除。擦除会清掉磨损计数，所以擦完要把加一后的次数写回去。
  if (old != kInvalidPhys) {
    PageSpare obsolete{};
    std::memset(&obsolete, 0xFF, sizeof(obsolete));
    obsolete.state = nor::kStateObsolete;
    ops.push_back(make_program(old, 0, nullptr, obsolete));
    ops.push_back(make_erase(old));

    PageSpare freed{};
    std::memset(&freed, 0xFF, sizeof(freed));
    freed.erase_count = erase_count_[old] + 1;
    freed.state = kStateFree;
    ops.push_back(make_program(old, 0, nullptr, freed));
  }

  error = driver_.execute(ops);
  if (error != Error::Ok) {
    return error;
  }

  kind_[phys] = PhysKind::Valid;
  // 命令都成功后才改内存映射。断电后以闪存扫描为准，不看这张表。
  map_[lba] = phys;
  if (old != kInvalidPhys) {
    kind_[old] = PhysKind::Free;
    erase_count_[old] += 1;
  }
  return Error::Ok;
}

StorageInfo Ftl::info() const {
  StorageInfo out;
  out.logical_blocks = logical_blocks_;
  out.physical_blocks = physical_blocks_;
  out.page_reads = driver_.page_reads();
  out.page_programs = driver_.page_programs();
  out.block_erases = driver_.block_erases();
  out.interrupts = driver_.interrupts();
  out.wear_min = 0;
  out.wear_max = 0;
  if (erase_count_.empty()) {
    return out;
  }

  out.wear_min = erase_count_.front();
  for (uint32_t phys = 0; phys < physical_blocks_; ++phys) {
    if (kind_[phys] == PhysKind::Free) {
      ++out.free_physical;
    } else if (kind_[phys] == PhysKind::Garbage) {
      ++out.garbage_physical;
    }
    if (erase_count_[phys] < out.wear_min) {
      out.wear_min = erase_count_[phys];
    }
    if (erase_count_[phys] > out.wear_max) {
      out.wear_max = erase_count_[phys];
    }
  }
  return out;
}

Error Ftl::read_page(uint32_t block, uint32_t page, uint8_t* data, PageSpare& spare) {
  FlashOp op;
  op.command = nor::kCmdRead;
  op.address = block * kBlockBytes + page * kPageBytes;
  std::vector<FlashOp> batch{op};
  const Error error = driver_.execute(batch);
  if (error != Error::Ok) {
    return error;
  }
  std::memcpy(data, batch[0].bytes.data(), kPageSize);
  spare = spare_from_bytes(batch[0]);
  return Error::Ok;
}

Error Ftl::allocate(uint32_t& phys) {
  const uint32_t free = find_kind(PhysKind::Free);
  if (free != kInvalidPhys) {
    kind_[free] = PhysKind::Garbage;
    phys = free;
    return Error::Ok;
  }
  return reclaim_one(phys);
}

Error Ftl::reclaim_one(uint32_t& phys) {
  const uint32_t garbage = find_kind(PhysKind::Garbage);
  if (garbage == kInvalidPhys) {
    return Error::NoSpace;
  }

  std::vector<FlashOp> ops;
  ops.push_back(make_erase(garbage));
  PageSpare freed{};
  std::memset(&freed, 0xFF, sizeof(freed));
  freed.erase_count = erase_count_[garbage] + 1;
  freed.state = kStateFree;
  ops.push_back(make_program(garbage, 0, nullptr, freed));
  const Error error = driver_.execute(ops);
  if (error != Error::Ok) {
    return error;
  }

  erase_count_[garbage] += 1;
  kind_[garbage] = PhysKind::Garbage;
  phys = garbage;
  return Error::Ok;
}

uint32_t Ftl::find_kind(PhysKind kind) const {
  uint32_t chosen = kInvalidPhys;
  uint32_t wear = 0xffffffffu;
  for (uint32_t phys = 0; phys < physical_blocks_; ++phys) {
    if (kind_[phys] == kind && erase_count_[phys] < wear) {
      chosen = phys;
      wear = erase_count_[phys];
    }
  }
  return chosen;
}

FlashOp Ftl::make_program(uint32_t block, uint32_t page, const uint8_t* data, const PageSpare& spare) const {
  FlashOp op;
  op.command = nor::kCmdProgram;
  op.address = block * kBlockBytes + page * kPageBytes;
  std::memset(op.bytes.data(), 0xFF, op.bytes.size());
  if (data != nullptr) {
    std::memcpy(op.bytes.data(), data, kPageSize);
  }
  std::memcpy(op.bytes.data() + kPageSize, &spare, sizeof(spare));
  return op;
}

FlashOp Ftl::make_erase(uint32_t block) const {
  FlashOp op;
  op.command = nor::kCmdErase;
  op.address = block * kBlockBytes;
  return op;
}

}  // namespace minifs
