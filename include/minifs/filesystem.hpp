#pragma once

#include "minifs/types.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace minifs {

// 块设备上面的那一层：路径、目录、文件描述符。
// 磁盘布局和分配细节在 src/fs_impl.hpp，不放到这个头里。
class FileSystem {
 public:
  FileSystem();
  ~FileSystem();

  FileSystem(const FileSystem&) = delete;
  FileSystem& operator=(const FileSystem&) = delete;

  // 创建镜像并挂载。已挂载时会先卸载。默认 256 块、64 个 inode。
  [[nodiscard]] Error format(const std::string& image_path,
                             uint32_t block_count = kDefaultBlockCount);
  [[nodiscard]] Error mount(const std::string& image_path);
  [[nodiscard]] Error unmount();
  [[nodiscard]] Error sync();
  [[nodiscard]] bool mounted() const;

  [[nodiscard]] Error mkdir(std::string_view path);
  [[nodiscard]] Error create_file(std::string_view path);
  [[nodiscard]] Error unlink(std::string_view path);
  [[nodiscard]] Error rmdir(std::string_view path);
  [[nodiscard]] Error stat(std::string_view path, FileStat& out) const;
  [[nodiscard]] Error list(std::string_view path, std::vector<DirItem>& out) const;
  [[nodiscard]] Error disk_info(DiskInfo& out) const;

  // 没有 seek。描述符的偏移从 0 开始，read/write 依次往后走。
  [[nodiscard]] Error open(std::string_view path, OpenMode mode, int& fd);
  [[nodiscard]] Error read(int fd, void* buffer, std::size_t size, std::size_t& transferred);
  [[nodiscard]] Error write(int fd, const void* buffer, std::size_t size, std::size_t& transferred);
  [[nodiscard]] Error close(int fd);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace minifs
