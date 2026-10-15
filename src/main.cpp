#include "self_test.hpp"
#include "shell.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <iostream>
#include <string>

int main(int argc, char** argv) {
#ifdef _WIN32
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
#endif

  if (argc >= 2) {
    const std::string command = argv[1];
    if (command == "check") {
      return run_self_test();
    }
    if (command == "help" || command == "--help" || command == "-h") {
      print_help();
      return 0;
    }
    std::cerr << "未知参数。直接运行进入命令行，或执行 minifs check 做自检。\n";
    print_help();
    return 2;
  }

  return run_shell();
}
