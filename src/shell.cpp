#include "shell.hpp"

#include "minifs/filesystem.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::string trim(std::string_view text) {
  std::size_t begin = 0;
  while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
    ++begin;
  }
  std::size_t end = text.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
    --end;
  }
  return std::string(text.substr(begin, end - begin));
}

std::vector<std::string> split(const std::string& text) {
  std::vector<std::string> parts;
  std::size_t index = 0;
  while (index < text.size()) {
    while (index < text.size() && std::isspace(static_cast<unsigned char>(text[index])) != 0) {
      ++index;
    }
    if (index >= text.size()) {
      break;
    }
    const std::size_t start = index;
    while (index < text.size() && std::isspace(static_cast<unsigned char>(text[index])) == 0) {
      ++index;
    }
    parts.push_back(text.substr(start, index - start));
  }
  return parts;
}

bool parse_u32(const std::string& text, uint32_t& value) {
  if (text.empty()) {
    return false;
  }
  uint64_t number = 0;
  for (unsigned char ch : text) {
    if (ch < '0' || ch > '9') {
      return false;
    }
    number = number * 10u + static_cast<uint64_t>(ch - '0');
    if (number > UINT32_MAX) {
      return false;
    }
  }
  value = static_cast<uint32_t>(number);
  return true;
}

const char* file_kind(minifs::FileType type) {
  if (type == minifs::FileType::Directory) {
    return "目录";
  }
  if (type == minifs::FileType::Regular) {
    return "文件";
  }
  return "未知";
}

class Shell {
 public:
  int run() {
    std::cout << "minifs 已启动。输入 help 查看命令，默认镜像是 " << image_ << "。\n";
    std::string line;
    while (true) {
      std::cout << (fs_.mounted() ? "minifs> " : "minifs (未挂载)> ") << std::flush;
      if (!std::getline(std::cin, line)) {
        std::cout << '\n';
        break;
      }
      if (handle(line)) {
        break;
      }
    }
    const minifs::Error error = fs_.unmount();
    if (error != minifs::Error::Ok) {
      std::cout << minifs::error_message(error) << '\n';
      return 1;
    }
    return 0;
  }

 private:
  // 返回 true 表示退出命令行。
  bool handle(const std::string& line) {
    const std::string text = trim(line);
    if (text.empty()) {
      return false;
    }
    const auto gap = text.find_first_of(" \t");
    const std::string command = gap == std::string::npos ? text : text.substr(0, gap);
    const std::string args = gap == std::string::npos ? std::string() : trim(text.substr(gap + 1));

    if (command == "help") {
      print_help();
    } else if (command == "format") {
      format_image(args);
    } else if (command == "mount") {
      mount_image(args);
    } else if (command == "unmount") {
      if (report(fs_.unmount())) {
        std::cout << "已卸载\n";
      }
    } else if (command == "sync") {
      if (report(fs_.sync())) {
        std::cout << "已同步\n";
      }
    } else if (command == "ls") {
      list_path(args);
    } else if (command == "mkdir") {
      make_dir(args);
    } else if (command == "touch") {
      touch_file(args);
    } else if (command == "write") {
      write_file(args);
    } else if (command == "cat") {
      cat_file(args);
    } else if (command == "stat") {
      stat_path(args);
    } else if (command == "rm") {
      remove_file(args);
    } else if (command == "rmdir") {
      remove_dir(args);
    } else if (command == "df") {
      show_disk();
    } else if (command == "flash") {
      show_flash();
    } else if (command == "exit" || command == "quit") {
      return true;
    } else {
      std::cout << "未知命令，输入 help 查看\n";
    }
    return false;
  }

  bool report(minifs::Error error) const {
    if (error == minifs::Error::Ok) {
      return true;
    }
    std::cout << minifs::error_message(error) << '\n';
    return false;
  }

  void format_image(const std::string& args) {
    const std::vector<std::string> tokens = split(args);
    if (tokens.size() > 2) {
      std::cout << "用法: format [镜像路径] [块数]\n";
      return;
    }

    std::string path = image_;
    uint32_t blocks = minifs::kDefaultBlockCount;
    if (tokens.size() == 1) {
      uint32_t value = 0;
      if (parse_u32(tokens[0], value)) {
        blocks = value;
      } else {
        path = tokens[0];
      }
    } else if (tokens.size() == 2) {
      path = tokens[0];
      if (!parse_u32(tokens[1], blocks)) {
        std::cout << "块数必须是正整数\n";
        return;
      }
    }

    if (report(fs_.format(path, blocks))) {
      image_ = path;
      std::cout << "已格式化并挂载 " << image_ << "（" << blocks << " 块）\n";
    }
  }

  void mount_image(const std::string& args) {
    const std::vector<std::string> tokens = split(args);
    if (tokens.size() > 1) {
      std::cout << "用法: mount [镜像路径]\n";
      return;
    }
    const std::string path = tokens.empty() ? image_ : tokens[0];
    if (report(fs_.mount(path))) {
      image_ = path;
      std::cout << "已挂载 " << image_ << '\n';
    }
  }

  void list_path(const std::string& args) {
    const std::vector<std::string> tokens = split(args);
    if (tokens.size() > 1) {
      std::cout << "用法: ls [路径]\n";
      return;
    }
    const std::string path = tokens.empty() ? std::string("/") : tokens[0];
    std::vector<minifs::DirItem> items;
    if (!report(fs_.list(path, items))) {
      return;
    }
    std::sort(items.begin(), items.end(), [](const minifs::DirItem& left, const minifs::DirItem& right) {
      return left.name < right.name;
    });
    if (items.empty()) {
      std::cout << "(空)\n";
      return;
    }
    for (const minifs::DirItem& item : items) {
      std::cout << file_kind(item.type) << "  " << item.size << "  " << item.name;
      if (item.type == minifs::FileType::Directory) {
        std::cout << '/';
      }
      std::cout << '\n';
    }
  }

  void make_dir(const std::string& args) {
    const std::vector<std::string> tokens = split(args);
    if (tokens.size() != 1) {
      std::cout << "用法: mkdir <路径>\n";
      return;
    }
    // mkdir 的入口。后面进入 FileSystem::mkdir。
    static_cast<void>(report(fs_.mkdir(tokens[0])));
  }

  void touch_file(const std::string& args) {
    const std::vector<std::string> tokens = split(args);
    if (tokens.size() != 1) {
      std::cout << "用法: touch <路径>\n";
      return;
    }
    const minifs::Error error = fs_.create_file(tokens[0]);
    if (error == minifs::Error::Ok || error == minifs::Error::AlreadyExists) {
      return;
    }
    std::cout << minifs::error_message(error) << '\n';
  }

  void write_file(const std::string& args) {
    if (args.empty()) {
      std::cout << "用法: write <路径> <文本>\n";
      return;
    }
    std::size_t index = 0;
    while (index < args.size() && std::isspace(static_cast<unsigned char>(args[index])) == 0) {
      ++index;
    }
    const std::string path = args.substr(0, index);
    while (index < args.size() && std::isspace(static_cast<unsigned char>(args[index])) != 0) {
      ++index;
    }
    const std::string content = args.substr(index);
    if (path.empty()) {
      std::cout << "用法: write <路径> <文本>\n";
      return;
    }

    // 文件不存在就先占一个 inode 并挂上名字，内容仍是空的。
    const minifs::Error created = fs_.create_file(path);
    if (created != minifs::Error::Ok && created != minifs::Error::AlreadyExists) {
      std::cout << minifs::error_message(created) << '\n';
      return;
    }

    int fd = -1;
    if (!report(fs_.open(path, minifs::OpenMode::Truncate, fd))) {
      return;
    }
    std::size_t transferred = 0;
    // 文本从这里进入文件系统。fd 里记着目标 inode 和当前偏移。
    const minifs::Error error = fs_.write(fd, content.data(), content.size(), transferred);
    const minifs::Error closed = fs_.close(fd);
    if (error != minifs::Error::Ok) {
      std::cout << minifs::error_message(error) << '\n';
      return;
    }
    if (closed != minifs::Error::Ok) {
      std::cout << minifs::error_message(closed) << '\n';
      return;
    }
    if (transferred != content.size()) {
      std::cout << "写入不完整\n";
    }
  }

  void cat_file(const std::string& args) {
    const std::vector<std::string> tokens = split(args);
    if (tokens.size() != 1) {
      std::cout << "用法: cat <路径>\n";
      return;
    }
    int fd = -1;
    if (!report(fs_.open(tokens[0], minifs::OpenMode::ReadOnly, fd))) {
      return;
    }

    std::string data;
    std::vector<char> buffer(minifs::kBlockSize);
    minifs::Error error = minifs::Error::Ok;
    while (true) {
      std::size_t transferred = 0;
      error = fs_.read(fd, buffer.data(), buffer.size(), transferred);
      if (error != minifs::Error::Ok || transferred == 0) {
        break;
      }
      data.append(buffer.data(), transferred);
    }
    const minifs::Error closed = fs_.close(fd);
    if (error != minifs::Error::Ok) {
      std::cout << minifs::error_message(error) << '\n';
      return;
    }
    if (closed != minifs::Error::Ok) {
      std::cout << minifs::error_message(closed) << '\n';
      return;
    }
    std::cout << data;
    if (data.empty() || data.back() != '\n') {
      std::cout << '\n';
    }
  }

  void stat_path(const std::string& args) {
    const std::vector<std::string> tokens = split(args);
    if (tokens.size() != 1) {
      std::cout << "用法: stat <路径>\n";
      return;
    }
    minifs::FileStat stat{};
    if (!report(fs_.stat(tokens[0], stat))) {
      return;
    }
    std::cout << "类型: " << file_kind(stat.type) << '\n'
              << "inode: " << stat.inode << '\n'
              << "大小: " << stat.size << " 字节\n"
              << "数据块: " << stat.data_blocks << '\n';
  }

  void remove_file(const std::string& args) {
    const std::vector<std::string> tokens = split(args);
    if (tokens.size() != 1) {
      std::cout << "用法: rm <路径>\n";
      return;
    }
    static_cast<void>(report(fs_.unlink(tokens[0])));
  }

  void remove_dir(const std::string& args) {
    const std::vector<std::string> tokens = split(args);
    if (tokens.size() != 1) {
      std::cout << "用法: rmdir <路径>\n";
      return;
    }
    static_cast<void>(report(fs_.rmdir(tokens[0])));
  }

  void show_disk() {
    minifs::DiskInfo info{};
    if (!report(fs_.disk_info(info))) {
      return;
    }
    std::cout << "块大小: " << info.block_size << " 字节\n"
              << "总块数: " << info.block_count << "  空闲: " << info.free_blocks << '\n'
              << "inode: " << info.inode_count << "  空闲: " << info.free_inodes << '\n'
              << "数据区从块 " << info.data_block_start << " 开始\n";
  }

  void show_flash() {
    minifs::StorageInfo info{};
    if (!report(fs_.storage_info(info))) {
      return;
    }
    std::cout << "逻辑块: " << info.logical_blocks << "  物理擦除块: " << info.physical_blocks << '\n'
              << "空闲擦除块: " << info.free_physical << "  待回收: " << info.garbage_physical << '\n'
              << "页读取: " << info.page_reads << "  页编程: " << info.page_programs
              << "  块擦除: " << info.block_erases << '\n'
              << "中断: " << info.interrupts << '\n'
              << "磨损次数: " << info.wear_min << " .. " << info.wear_max << '\n';
  }

  minifs::FileSystem fs_;
  std::string image_ = "disk.img";
};

}  // namespace

void print_help() {
  std::cout
      << "命令:\n"
      << "  help                     显示帮助\n"
      << "  format [镜像] [块数]     创建空磁盘并挂载，默认 disk.img、256 块\n"
      << "  mount [镜像]             挂载已有镜像\n"
      << "  unmount                  同步并卸载\n"
      << "  sync                     把元数据写回镜像\n"
      << "  ls [路径]                列出目录，默认 /\n"
      << "  mkdir <路径>             创建目录\n"
      << "  touch <路径>             创建空文件\n"
      << "  write <路径> <文本>      创建或覆盖写入\n"
      << "  cat <路径>               打印文件内容\n"
      << "  stat <路径>              查看类型、大小和数据块\n"
      << "  rm <路径>                删除文件\n"
      << "  rmdir <路径>             删除空目录\n"
      << "  df                       查看剩余块和 inode\n"
      << "  flash                    查看闪存驱动、中断和磨损计数\n"
      << "  exit                     卸载并退出\n"
      << "\n"
      << "路径以 / 开头，用 / 分隔，例如 /docs/a.txt。命令行里的路径不要带空格。\n"
      << "单个文件最大 48KB。正常退出后再次 mount，内容还在。\n";
}

int run_shell() {
  Shell shell;
  return shell.run();
}
