# minifs

一个从 0 到 1 的文件系统加存储系统 demo。磁盘就是一个普通文件，程序把它切成 4KB 的块，在上面做分配、目录和文件读写。进程正常退出后再挂载，内容还在。

第一版只有单进程、直接块和正常关机后的持久化。没有日志、权限、缓存和并发。

## 目录

```
include/minifs/types.hpp          磁盘上的结构、错误码、容量上限
include/minifs/bitmap.hpp         空闲位图
include/minifs/block_device.hpp   块设备接口
include/minifs/filesystem.hpp     给 shell 和自检用的文件系统接口
src/block_device.cpp              用一个文件模拟磁盘
src/fs_impl.hpp                   内部状态，标出每个 cpp 负责什么
src/fs_image.cpp                  格式化、挂载、inode 和块分配
src/fs_path.cpp                   路径、目录、创建和删除
src/fs_file.cpp                   文件字节、描述符、截断
src/shell.cpp                     命令行
src/self_test.cpp                 不依赖手工输入的自检
docs/design.md                    布局、写入顺序和刻意不做的事
```

阅读代码时按这个顺序：`types.hpp` → `block_device` → `fs_image.cpp` → `fs_path.cpp` → `fs_file.cpp` → `shell.cpp`。

## 构建

使用 CMake 和 Visual Studio 2019 x64。在 `D:\CppProject\minifs` 下执行：

```bat
cmake -S . -B build -G "Visual Studio 16 2019" -A x64
cmake --build build --config Release
```

自检：

```bat
build\Release\minifs.exe check
```

也可以在 `build` 目录里执行 `ctest -C Release`。

## 命令行

```bat
build\Release\minifs.exe
```

```text
minifs> format
minifs> mkdir /docs
minifs> write /docs/hello.txt 你好
minifs> ls /docs
minifs> cat /docs/hello.txt
minifs> df
minifs> exit
minifs> mount
minifs> cat /docs/hello.txt
```

`format` 默认创建当前目录下的 `disk.img`（256 块，1 MiB）并直接挂载。`format D:\data\disk.img 128` 可以指定路径和块数。路径在文件系统内部以 `/` 开头；命令行参数本身不要带空格。

`exit` 会先把元数据写回镜像。下次 `mount` 之后，`cat` 应该读到同一份内容。

## 边界

- 单个文件最大 48KB（12 个直接块）。
- 默认 64 个 inode。
- 没有 seek，打开文件后从偏移 0 读或写。
- 删除目录项会留下空槽，下次在同一目录创建时复用。
- 正在打开的文件不能删除。
- 详细的磁盘布局和崩溃时的取舍写在 [docs/design.md](docs/design.md)。
