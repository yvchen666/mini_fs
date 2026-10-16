cmake_minimum_required(VERSION 3.16)

project(minifs VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(minifs_core STATIC
  src/device/nor_controller.cpp
  src/driver/nor_driver.cpp
  src/ftl/ftl.cpp
  src/block/flash_block_device.cpp
  src/fs_image.cpp
  src/fs_path.cpp
  src/fs_file.cpp
)

target_include_directories(minifs_core
  PUBLIC include
  PRIVATE src
)

add_executable(minifs
  src/main.cpp
  src/shell.cpp
  src/self_test.cpp
)

target_link_libraries(minifs PRIVATE minifs_core)
target_include_directories(minifs PRIVATE src)

if(MSVC)
  target_compile_options(minifs_core PRIVATE /W4 /utf-8 /permissive-)
  target_compile_options(minifs PRIVATE /W4 /utf-8 /permissive-)
else()
  target_compile_options(minifs_core PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(minifs PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT minifs)

enable_testing()
add_test(NAME selftest COMMAND minifs check)
