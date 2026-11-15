# binGPU

**一句话**：`设备 / 交换链 / 资源 / 管线 / 命令 / 呈现` 这一层的 C11 库 —— 把"同一件事"
（建面、传纹理、切混合、离屏、present）从每个渲染器里抽出来只写一遍，并附带一个
**软件参考实现**当像素裁判。

它不是渲染器，也不是场景图：不知道节点、布局、文本、场景图 —— 那些在上面三层。

```
bin3d  (C++17) ──► bin2d (C++17)
         │                  │
     binUI (C++17) ─────────┘
                  │
            binGPU (C11 / C ABI)   ← 本仓库
                  │
       GL / Vulkan / D3D12 / CPU（参考实现）
```

## 是什么 / 不是什么

| 是 | 不是 |
|---|---|
| 一个 **C11 库**，导出 C ABI（`include/bingpu.h`），Uint64 句柄，可被 Python/Go/Rust 直接绑 | 不是 RHI 全家桶：没有用户着色器、没有反射、没有绑定模型 |
| 一套**明确的像素规则**（中心采样、top-left、预乘混合、反预乘回读），由 CPU 后端钉住 | 不是"随便画画"：每个像素行为都能手算验证 |
| 一个有 **NULL 后端**（只校验+计数）与 **CPU 参考后端**（确定性像素、可回读）的库 | 0.1.0 里**没有**可用的 GPU 后端（VK/D3D12 登记未实现，GL 不迁） |
| 一个**单线程**、显式生命周期的底层：没有回调、没有异常、没有 RAII | 不是多线程命令记录框架（`max_frames_in_flight = 1`） |

## 目录

```
include/bingpu.h      公共 C ABI（定稿；abi-check 会单独用纯 C 编它）
src/
  bingpu_api.c        公共入口：校验 + 转发
  bingpu_error.c      错误槽 / 名字表 / 版本 / 后端注册表 / shutdown
  bingpu_object.c     句柄表（代数失效、按设备连带销毁）
  bingpu_null.c       NULL 后端
  bingpu_cpu.c        CPU 参考实现
  bingpu_internal.h   内部契约（不进 include/）
tests/
  test_abi.c          ABI 契约测试（239 断言）
  test_cpu_render.c   像素级测试（334 断言）
tools/bingpu_info.c   自查工具：版本 / 后端 / caps / stats
docs/                 契约、ABI、光栅规则、后端路线
```

## 构建

MinGW / GCC / Clang，只需要一个 C11 编译器（**不需要** C++、不需要 Vulkan SDK）：

```sh
# Windows（Git Bash 的 sh 当 SHELL，和 binUI 一致）
mingw32-make SHELL='C:/Program Files/Git/bin/sh.exe' -j8 all
mingw32-make SHELL='C:/Program Files/Git/bin/sh.exe' test
mingw32-make SHELL='C:/Program Files/Git/bin/sh.exe' abi-check

# POSIX
make all test abi-check
```

产物（都在 `build-mingw/`，不进版本库）：

```
build-mingw/libbingpu.a
build-mingw/bin/bingpu_info(.exe)
build-mingw/bin/tests/test_abi(.exe)
build-mingw/bin/tests/test_cpu_render(.exe)
```

CMake 也可用（`project(binGPU C)`，两个 ctest 用例）：

```sh
cmake -S . -B build && cmake --build build && ctest --test-dir build
```

## 测试

| 命令 | 做什么 |
|---|---|
| `make test` | 编两个测试并逐个跑，任何失败非 0 退出 |
| `make abi-check` | ①把 `include/bingpu.h` 当主文件用**纯 C11 + `-Werror`** 编一遍；②grep 头里不许出现 `namespace` / `class ` / `template` / `std::` / `<string>` / `<vector>` |
| `build-mingw/bin/bingpu_info` | 人看：版本、后端可用性、caps、跑一遍最小调用序列后的 stats |

`test_abi` 钉的是契约（`struct_size`、无效/已销毁句柄、状态机、线程本地错误槽、
destroy 幂等）；`test_cpu_render` 钉的是**像素**（clear、覆盖范围、top-left 接缝、五种
混合、采样器、scissor、viewport、affine、离屏、深度、反预乘回读、R8 遮罩、网格、stats）。

## 状态

| 能力 | 状态 |
|---|---|
| C ABI（句柄 / `struct_size` / 线程本地错误 / 无回调） | ✅ 0.1.0 定稿（ABI 1） |
| 设备 / 交换链 / 帧模型 / 命令缓冲状态机 | ✅ CPU + NULL |
| 纹理上传回读 / 缓冲 / 采样器 / 管线 / 仿射 / 网格 | ✅ CPU（NULL 只校验+计数） |
| 离屏目标 + 深度（LESS） | ✅ CPU |
| 五种混合（预乘 / 覆盖 / 保留 / 擦除 / 叠加） | ✅ CPU（逐像素验证） |
| 窗口呈现 / 交换链 / 垂直同步 | ⛔ 留给 VK / D3D12（CPU 明确 `UNSUPPORTED`） |
| MSAA / mip 采样 / 用户着色器 / 网格光照 | ⛔ 未做，见 [docs/backends.md](docs/backends.md) 的欠账 |
| Vulkan / D3D12 后端 | ⛔ 登记未实现（`AUTO` 落到 CPU） |

详细状态与理由：[docs/backends.md](docs/backends.md)。像素规则：[docs/raster_rules.md](docs/raster_rules.md)。

## 版本

- 语义化版本 `0.1.0`；**二进制 ABI 号 1**（结构体布局 / 函数签名 / 枚举值一变就 +1）。
- 变更记录：[CHANGELOG.md](CHANGELOG.md)。

## 许可

MIT，见 [LICENSE](LICENSE)。
