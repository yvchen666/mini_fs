#include "self_test.hpp"

#include "minifs/filesystem.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool succeeded(const char* step, minifs::Error error) {
  if (error == minifs::Error::Ok) {
    return true;
  }
  std::cerr << step << " 失败: " << minifs::error_message(error) << '\n';
  return false;
}

bool failed_with(const char* step, minifs::Error error, minifs::Error expected) {
  if (error == expected) {
    return true;
  }
  std::cerr << step << " 失败: 得到 " << minifs::error_message(error) << "，期望 "
            << minifs::error_message(expected) << '\n';
  return false;
}

bool read_all(minifs::FileSystem& fs, const std::string& path, std::string& out) {
  int fd = -1;
  if (!succeeded("打开文件", fs.open(path, minifs::OpenMode::ReadOnly, fd))) {
    return false;
  }
  out.clear();
  std::array<char, 1024> buffer{};
  while (true) {
    std::size_t transferred = 0;
    const minifs::Error error = fs.read(fd, buffer.data(), buffer.size(), transferred);
    if (!succeeded("读取文件", error)) {
      static_cast<void>(fs.close(fd));
      return false;
    }
    if (transferred == 0) {
      break;
    }
    out.append(buffer.data(), transferred);
  }
  return succeeded("关闭文件", fs.close(fd));
}

bool write_all(minifs::FileSystem& fs,
               const std::string& path,
               const std::string& data,
               minifs::Error expected = minifs::Error::Ok) {
  int fd = -1;
  if (!succeeded("打开待写文件", fs.open(path, minifs::OpenMode::Truncate, fd))) {
    return false;
  }
  std::size_t transferred = 0;
  const minifs::Error error = fs.write(fd, data.data(), data.size(), transferred);
  const minifs::Error closed = fs.close(fd);
  if (error != expected) {
    std::cerr << "写入结果不符: 得到 " << minifs::error_message(error) << "，期望 "
              << minifs::error_message(expected) << '\n';
    return false;
  }
  if (expected == minifs::Error::Ok && transferred != data.size()) {
    std::cerr << "写入长度不符\n";
    return false;
  }
  if (expected != minifs::Error::Ok && transferred != 0) {
    std::cerr << "失败的写入不应该留下数据\n";
    return false;
  }
  return succeeded("关闭待写文件", closed);
}

std::string make_payload() {
  const std::string prefix = "你好，minifs\n";
  std::string payload(5000, '\0');
  std::memcpy(payload.data(), prefix.data(), prefix.size());
  for (std::size_t index = prefix.size(); index < payload.size(); ++index) {
    payload[index] = static_cast<char>(static_cast<unsigned char>(index * 17u + 3u));
  }
  return payload;
}

bool contains_name(const std::vector<minifs::DirItem>& items, const std::string& name) {
  for (const minifs::DirItem& item : items) {
    if (item.name == name) {
      return true;
    }
  }
  return false;
}

}  // namespace

int run_self_test() {
  const std::filesystem::path image_path =
      std::filesystem::temp_directory_path() / "minifs-selftest.img";
  const std::string image = image_path.string();

  minifs::FileSystem fs;
  struct Cleanup {
    minifs::FileSystem* filesystem = nullptr;
    std::filesystem::path path;
    ~Cleanup() {
      if (filesystem != nullptr) {
        static_cast<void>(filesystem->unmount());
      }
      std::error_code error;
      std::filesystem::remove(path, error);
    }
  } cleanup{&fs, image_path};

  if (!succeeded("格式化", fs.format(image, 128))) {
    return 1;
  }

  minifs::DiskInfo baseline{};
  if (!succeeded("读取磁盘信息", fs.disk_info(baseline))) {
    return 1;
  }
  if (baseline.block_size != minifs::kBlockSize || baseline.block_count != 128 ||
      baseline.inode_count != minifs::kDefaultInodeCount || baseline.data_block_start != 4) {
    std::cerr << "磁盘布局与约定不符\n";
    return 1;
  }
  if (baseline.free_blocks != baseline.block_count - baseline.data_block_start - 1 ||
      baseline.free_inodes != baseline.inode_count - 1) {
    std::cerr << "格式化后的空闲计数不对\n";
    return 1;
  }

  if (!succeeded("创建目录", fs.mkdir("/docs"))) {
    return 1;
  }
  if (!failed_with("重复创建目录", fs.mkdir("/docs"), minifs::Error::AlreadyExists)) {
    return 1;
  }
  if (!failed_with("缺少父目录", fs.mkdir("/missing/child"), minifs::Error::NotFound)) {
    return 1;
  }

  minifs::DiskInfo after_mkdir{};
  if (!succeeded("创建目录后的磁盘信息", fs.disk_info(after_mkdir))) {
    return 1;
  }
  if (after_mkdir.free_blocks != baseline.free_blocks - 1 ||
      after_mkdir.free_inodes != baseline.free_inodes - 1) {
    std::cerr << "创建目录没有正确消耗一个数据块和一个 inode\n";
    return 1;
  }

  if (!succeeded("创建文件", fs.create_file("/docs/note.txt"))) {
    return 1;
  }
  const std::string payload = make_payload();
  if (!write_all(fs, "/docs/note.txt", payload)) {
    return 1;
  }

  minifs::FileStat stat{};
  if (!succeeded("查看文件", fs.stat("/docs/note.txt", stat))) {
    return 1;
  }
  if (stat.type != minifs::FileType::Regular || stat.size != payload.size() || stat.data_blocks != 2) {
    std::cerr << "跨块写入后的大小或块数不对\n";
    return 1;
  }

  minifs::DiskInfo after_write{};
  if (!succeeded("写入后的磁盘信息", fs.disk_info(after_write))) {
    return 1;
  }
  if (after_write.free_blocks != baseline.free_blocks - 3 ||
      after_write.free_inodes != baseline.free_inodes - 2) {
    std::cerr << "写入后的空闲计数不对\n";
    return 1;
  }

  if (!succeeded("卸载", fs.unmount())) {
    return 1;
  }
  std::vector<minifs::DirItem> ignored;
  if (!failed_with("未挂载时列目录", fs.list("/", ignored), minifs::Error::NotMounted)) {
    return 1;
  }
  if (!succeeded("重新挂载", fs.mount(image))) {
    return 1;
  }

  std::string loaded;
  if (!read_all(fs, "/docs/note.txt", loaded) || loaded != payload) {
    std::cerr << "重新挂载后内容不一致\n";
    return 1;
  }
  if (loaded.size() < minifs::kBlockSize + 1 || loaded[0] != payload[0] ||
      loaded[minifs::kBlockSize - 1] != payload[minifs::kBlockSize - 1] ||
      loaded[minifs::kBlockSize] != payload[minifs::kBlockSize]) {
    std::cerr << "跨块边界的字节不一致\n";
    return 1;
  }

  std::vector<minifs::DirItem> docs;
  if (!succeeded("列出 /docs", fs.list("/docs", docs)) || !contains_name(docs, "note.txt")) {
    std::cerr << "/docs 下找不到 note.txt\n";
    return 1;
  }
  std::vector<minifs::DirItem> parent;
  if (!succeeded("解析 ..", fs.list("/docs/..", parent)) || !contains_name(parent, "docs")) {
    std::cerr << "/docs/.. 没有回到根目录\n";
    return 1;
  }
  minifs::FileStat root_stat{};
  if (!succeeded("查看 /..", fs.stat("/..", root_stat)) || root_stat.inode != 0) {
    std::cerr << "根目录的 .. 应该指向自己\n";
    return 1;
  }

  if (!failed_with("删除目录当成文件", fs.unlink("/docs"), minifs::Error::IsDirectory)) {
    return 1;
  }
  if (!failed_with("删除非空目录", fs.rmdir("/docs"), minifs::Error::NotEmpty)) {
    return 1;
  }
  if (!failed_with("列出文件", fs.list("/docs/note.txt", ignored), minifs::Error::NotDirectory)) {
    return 1;
  }

  int readonly_fd = -1;
  if (!succeeded("只读打开", fs.open("/docs/note.txt", minifs::OpenMode::ReadOnly, readonly_fd))) {
    return 1;
  }
  std::size_t transferred = 0;
  if (!failed_with("只读写入", fs.write(readonly_fd, "x", 1, transferred), minifs::Error::InvalidArgument)) {
    static_cast<void>(fs.close(readonly_fd));
    return 1;
  }
  if (!succeeded("关闭只读描述符", fs.close(readonly_fd))) {
    return 1;
  }

  int busy_fd = -1;
  if (!succeeded("打开占用文件", fs.open("/docs/note.txt", minifs::OpenMode::ReadWrite, busy_fd))) {
    return 1;
  }
  if (!failed_with("删除打开中的文件", fs.unlink("/docs/note.txt"), minifs::Error::Busy)) {
    static_cast<void>(fs.close(busy_fd));
    return 1;
  }
  if (!succeeded("关闭占用描述符", fs.close(busy_fd))) {
    return 1;
  }
  if (!succeeded("删除文件", fs.unlink("/docs/note.txt"))) {
    return 1;
  }
  if (!succeeded("删除空目录", fs.rmdir("/docs"))) {
    return 1;
  }

  minifs::DiskInfo restored{};
  if (!succeeded("删除后的磁盘信息", fs.disk_info(restored))) {
    return 1;
  }
  if (restored.free_blocks != baseline.free_blocks || restored.free_inodes != baseline.free_inodes) {
    std::cerr << "删除后没有把块和 inode 还回去\n";
    return 1;
  }

  if (!succeeded("创建 f1", fs.create_file("/f1")) || !succeeded("创建 f2", fs.create_file("/f2"))) {
    return 1;
  }
  minifs::FileStat before_reuse{};
  if (!succeeded("两个文件时的根目录", fs.stat("/", before_reuse))) {
    return 1;
  }
  if (!succeeded("删除 f1", fs.unlink("/f1")) || !succeeded("创建 f3", fs.create_file("/f3"))) {
    return 1;
  }
  minifs::FileStat after_reuse{};
  if (!succeeded("复用目录槽后的根目录", fs.stat("/", after_reuse))) {
    return 1;
  }
  if (after_reuse.size != before_reuse.size) {
    std::cerr << "删除后的目录槽没有被下一次创建复用\n";
    return 1;
  }
  std::vector<minifs::DirItem> root_items;
  if (!succeeded("列出根目录", fs.list("/", root_items)) || contains_name(root_items, "f1") ||
      !contains_name(root_items, "f2") || !contains_name(root_items, "f3")) {
    std::cerr << "目录项复用后的名字不对\n";
    return 1;
  }
  if (!succeeded("删除 f2", fs.unlink("/f2")) || !succeeded("删除 f3", fs.unlink("/f3"))) {
    return 1;
  }

  if (!succeeded("创建大文件", fs.create_file("/big"))) {
    return 1;
  }
  const std::string too_big(static_cast<std::size_t>(minifs::kMaxFileSize) + 1, 'x');
  if (!write_all(fs, "/big", too_big, minifs::Error::TooLarge)) {
    return 1;
  }
  minifs::FileStat big_stat{};
  if (!succeeded("查看失败写入", fs.stat("/big", big_stat)) || big_stat.size != 0) {
    std::cerr << "超过上限的写入不应该改变文件大小\n";
    return 1;
  }

  std::string full(minifs::kMaxFileSize, static_cast<char>(0xAB));
  full.front() = 'A';
  full[minifs::kBlockSize] = 'M';
  full.back() = 'Z';
  if (!write_all(fs, "/big", full)) {
    return 1;
  }
  if (!succeeded("查看满文件", fs.stat("/big", big_stat)) || big_stat.size != minifs::kMaxFileSize ||
      big_stat.data_blocks != minifs::kDirectPointers) {
    std::cerr << "48KB 文件的大小或直接块数量不对\n";
    return 1;
  }

  minifs::DiskInfo full_disk{};
  if (!succeeded("满文件磁盘信息", fs.disk_info(full_disk))) {
    return 1;
  }
  if (full_disk.free_blocks != baseline.free_blocks - minifs::kDirectPointers ||
      full_disk.free_inodes != baseline.free_inodes - 1) {
    std::cerr << "满文件占用的空间不对\n";
    return 1;
  }

  if (!succeeded("再次卸载", fs.unmount()) || !succeeded("再次挂载", fs.mount(image))) {
    return 1;
  }
  std::string full_loaded;
  if (!read_all(fs, "/big", full_loaded) || full_loaded != full) {
    std::cerr << "满文件重新挂载后内容不一致\n";
    return 1;
  }
  if (!succeeded("删除大文件", fs.unlink("/big"))) {
    return 1;
  }

  minifs::DiskInfo end_state{};
  if (!succeeded("结束时的磁盘信息", fs.disk_info(end_state))) {
    return 1;
  }
  if (end_state.free_blocks != baseline.free_blocks || end_state.free_inodes != baseline.free_inodes) {
    std::cerr << "结束时空间没有回到格式化之后的状态\n";
    return 1;
  }

  std::cout << "自检通过：格式化、跨块读写、重新挂载、目录复用和 48KB 上限都符合预期。\n";
  return 0;
}
