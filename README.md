# binGPU

**一句话**：`设备 / 交换链 / 资源 / 管线 / 命令 / 呈现` 这一层的 **C++17 库，导出 C 兼容 ABI** ——
把"同一件事"（建面、传纹理、切混合、离屏、present）从每个渲染器里抽出来只写一遍，并附带
一个**软件参考实现**当像素裁判。

它不是渲染器，也不是场景图：不知道节点、布局、文本、场景图 —— 那些在上面三层。

```
bin3d  (C++17) ──► bin2d (C++17)
         │                  │
     binUI (C++17) ─────────┘
                  │
            binGPU (C++17 / C ABI) ← 本仓库
                  │
       Vulkan / D3D11 / GL / D3D12 / CPU（参考实现）
```

## 是什么 / 不是什么

| 是 | 不是 |
|---|---|
| 一个 **C++17 实现 + C 兼容 ABI** 的库：`include/bingpu.h` 是纯 C 头（定长 POD + Uint64 句柄），可被 Python/Go/Rust 直接绑 | 不是 RHI 全家桶：没有用户着色器、没有反射、没有绑定模型 |
| 一套**明确的像素规则**（中心采样、top-left、预乘混合、反预乘回读），由 CPU 后端钉住 | 不是"随便画画"：每个像素行为都能手算验证 |
| 一个有 **NULL 后端**（只校验+计数）与 **CPU 参考后端**（确定性像素、可回读）的库 | 不是"没有 GPU 就用不了"：VK / D3D11 两个真后端已落地，都拿 CPU 当像素裁判 |
| 一个**单线程**、显式生命周期的底层：没有回调、没有异常、没有 RAII | 不是多线程命令记录框架（`max_frames_in_flight = 1`） |

## 目录

```
include/bingpu.h      公共 C ABI（定稿；abi-check 会单独用纯 C 编它）
src/                  C++17 实现
  bingpu_api.cpp      公共入口：校验 + 转发 + 顶层 try/catch（异常不越过 ABI）
  bingpu_backend.h    class Backend（后端接口）+ BackendBase（共享状态机）+ 对象类型
  bingpu_backend.cpp  对象构造 + 四个后端共用的命令状态机/校验/计数
  bingpu_error.cpp    错误槽 / 名字表 / 版本 / 后端注册表 / shutdown
  bingpu_object.cpp   句柄表（unique_ptr + RAII，代数失效、按设备连带销毁）
  bingpu_null.cpp     NullBackend：只校验 + 计数
  bingpu_cpu.cpp      CpuBackend：软件参考实现
  bingpu_vk.cpp       VkBackend：Vulkan 1.3（动态渲染）真后端，运行期 LoadLibrary("vulkan-1.dll")
  bingpu_d3d11.cpp    D3D11Backend：即时上下文真后端，运行期 LoadLibrary("d3d11.dll"/"dxgi.dll")
  bingpu_gl.cpp       GlBackend：WGL + OpenGL 3.3 core 真后端，运行期 LoadLibrary("opengl32.dll")
  bingpu_vk_shaders.h     SPIR-V 数组（由 src/vk_shaders/*.vert/.frag 生成，勿手改）
  bingpu_d3d11_shaders.h  DXBC 数组（由 src/d3d11_shaders/quad.hlsl 生成，勿手改）
  bingpu_internal.h   内部契约（不进 include/）
src/vk_shaders/       quad.vert / quad.frag（固定功能的两个着色器；.spv 由 NDK glslc 生成）
src/d3d11_shaders/    quad.hlsl（同一对固定功能着色器的 HLSL 版；.cso 由 fxc 生成）
tests/
  test_abi.c           ABI 契约测试（257 断言）
  test_cpu_render.c    像素级测试（334 断言）
  test_vk_render.c     Vulkan vs CPU 逐像素（用例表在 gpu_parity.h）
  test_d3d11_render.c  D3D11 vs CPU 逐像素（同一份用例表）
  test_gl_render.c     OpenGL vs CPU 逐像素（同一份用例表）
tools/bingpu_info.c   自查工具：版本 / 后端 / caps / stats
tools/gpu_window_smoke.c 窗口路径烟测（交换链 / present / frame_read / resize，三个真后端各一遍）
docs/                 契约、ABI、光栅规则、后端路线
```

## 构建

MinGW / GCC / Clang，需要一个 **C++17 编译器**（`g++`）和一个 C 编译器（`gcc`，编测试与工具）。
**不需要** Vulkan SDK，也**不需要** Windows SDK 的 fxc：GPU 后端的着色器产物
（SPIR-V / DXBC）都以数组入库，GL 那份是 GLSL 源码字符串（运行时由驱动编，不需要离线工具）。
运行期各自 `LoadLibrary` 显卡 API，链接行里没有 `vulkan-1.lib` / `d3d11.lib` / `dxgi.lib` /
`opengl32.lib`。

为什么测试/工具是 C 源码而库是 C++：测试同时是"C 能消费这个 ABI"的证明，所以
`tests/*.c`、`tools/*.c` 用 `gcc` 编成目标文件，再由 `g++` 链接静态库（带 libstdc++）。
`make abi-check` 的第三条就是这条链路的冒烟检查。

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
build-mingw/bin/tests/test_vk_render(.exe)
build-mingw/bin/tests/test_d3d11_render(.exe)
build-mingw/bin/tests/test_gl_render(.exe)
```

CMake 也可用（`project(binGPU C CXX)`，五个 ctest 用例）：

```sh
cmake -S . -B build && cmake --build build && ctest --test-dir build
```

## 测试

| 命令 | 做什么 |
|---|---|
| `make test` | 编五个测试并逐个跑，任何失败非 0 退出 |
| `make abi-check` | ①把 `include/bingpu.h` 当主文件用**纯 C11 + `-Werror`** 编一遍；②剥掉注释后 grep，头里不许出现 `namespace` / `class ` / `template` / `std::` / `<string>` / `<vector>`；③冒烟：C 目标文件 + C++ 静态库由 `g++` 链接成可执行并跑起来 |
| `make bench` | 后端固定场景吞吐基准 + 13 个像素校验和（数字与解读见 [docs/perf.md](docs/perf.md)；bench 只跑 CPU 参考后端） |
| `make smoke` | **窗口路径**烟测：VK / D3D11 / GL 各开一个真窗口跑 3 帧 + resize，判"clear 往返逐字节相等 + 四边形位置对 + 尺寸跟客户区"。要桌面会话，所以不进 `test` |
| `build-mingw/bin/bingpu_info` | 人看：版本、后端可用性、caps、跑一遍最小调用序列后的 stats |

`test_abi` 钉的是契约（`struct_size`、无效/已销毁句柄、状态机、线程本地错误槽、
destroy 幂等）；`test_cpu_render` 钉的是**像素**（clear、覆盖范围、top-left 接缝、五种
混合、采样器、scissor、viewport、affine、离屏、深度、反预乘回读、R8 遮罩、网格、stats）；
`test_vk_render` / `test_d3d11_render` 钉的是"**同一串命令 → 同样的像素**"：两边共用
`tests/gpu_parity.h` 的同一份 12 个用例（clear / 覆盖 / 五种混合 / 接缝 / scissor /
viewport / affine / 双线性 / 最近+重复 / 局部上传 / 深度 / 网格），逐像素和 CPU 比。

## 状态

| 能力 | 状态 |
|---|---|
| C ABI（句柄 / `struct_size` / 线程本地错误 / 无回调） | ✅ 0.1.0 定稿；0.2.0 只换实现语言（ABI 1），0.4.0 追加一个后端枚举（ABI 2），0.6.0 网格顶点加 w + 光照（ABI 3），0.12.0 解析圆角顶点布局 + 能力位（ABI 4），0.14.0 MSAA/mip 能力位（ABI 5），0.16.0 延迟 resolve 标志 + `bingpu_cmd_resolve_target`（ABI 6；结构体布局一直没动） |
| 设备 / 交换链 / 帧模型 / 命令状态机 | ✅ CPU + NULL + VK + D3D11 |
| 纹理上传回读 / 缓冲 / 采样器 / 管线 / 仿射 / 网格 | ✅ CPU / VK / D3D11 |
| 离屏目标 + 深度（LESS） | ✅ CPU / VK / D3D11 |
| 五种混合（预乘 / 覆盖 / 保留 / 擦除 / 叠加） | ✅ CPU / VK / D3D11（逐像素验证） |
| 窗口呈现 / 交换链 / 垂直同步 | ✅ VK / D3D11 / GL（CPU/NULL 明确 `UNSUPPORTED`） |
| **2D 图元（`bingpu_cmd_draw_rounded_rect`）** | ✅ **0.10.0**：圆角矩形九宫格 —— AA 数学在调用方（bin2d 的覆盖率模板），拆块/合批/后端选择在这一层，所以五个后端零改动就能画；另有 `bingpu_cmd_draw_rounded_rects()` 批量版（同模板 N 个形状一次 draw_quads，D3D11 实测 1.37 → 0.38 ms） |
| **四角带色圆角矩形（`bingpu_cmd_draw_rounded_rect_grad`）** | ✅ **0.11.0**：九宫格 + 四角直通色。线性渐变在屏幕空间是仿射函数，仿射函数被四边形上的线性插值精确复现 ⇒ 圆角底渐变也是精确解（实测 1064 像素最大通道差 1），不要条带、不要 stencil、不要第二次 pass |
| **解析圆角（`bingpu_cmd_draw_shapes`，AA 在片元里）** | ✅ **0.12.0 / ABI 4**：圆角的抗锯齿**数学**从 CPU 搬进像素着色器 —— 每个片元自己求有符号距离场，`cov = clamp(0.5 - d, 0, 1)`（轴对齐 1:1 下 `\|grad d\| ≡ 1`，所以是精确解析值，**不需要 `fwidth`**）。CPU 参考 + GL + VK + D3D11 **四端全部实现**，端间最大通道差 1；精度优于上一版的 8 子采样扫描线模板（最大差 10~13 vs 16~40）。无 `BGPU_CAP_SHAPE` 时调用方退回覆盖率模板 |
| **帧节拍器（`bingpu_pace_*`）** | ✅ **0.9.0**：`standard`/`unlimited`/`240`/`120`/`90`/`60`/`30`，规则是「下一拍 = max(本拍开始 + 目标周期, 本拍结束)」；纯状态机、时间由调用方注入，所以能离线单测（`tests/test_pace.c`）。上层实测：VK `unlimited` 2116–2207 fps、`standard` 114 fps |
| **MSAA / 真 mip / 延迟 resolve** | ✅ **VK + GL**：`BgpuDeviceDesc.msaa_samples = 4/8` → 画布与所有颜色目标走多采样、pass 末 resolve 到单采样（`frame_read` / 采样拿到的仍是单采样图）；`BGPU_TEXTURE_MIPMAPS` → 完整 mip 链（VK 逐级 blit / GL `glGenerateMipmap`），采样器按 LOD 取级。**0.16.0 / ABI 6**：`BGPU_RENDER_DEFER_RESOLVE` 让"被反复进出的目标"不落 1×，LOAD 回灌与 resolve 都省掉，被采样/回读前自愈（binUI `svg-filter` 660×470：3.3~3.4 → 2.7 ms）。默认仍是 1×/1 级，与 CPU 逐像素合同一个字没动。能力查 `BGPU_CAP_MSAA` / `BGPU_CAP_MIPMAPS` 与 `caps.msaa_samples`。⛔ D3D11 这一版明确不做（原因见 [docs/backends.md](docs/backends.md) 的质量档一节）；用户着色器 / 网格光照仍未做 |
| **Vulkan 后端** | ✅ **0.3.0**：设备/交换链/present、纹理/缓冲/采样器、五种混合、离屏+深度、网格、仿射；与 CPU 参考后端逐像素 0 差（`tests/test_vk_render.c` 12 个用例） |
| **D3D11 后端** | ✅ **0.4.0**：同上，走**即时上下文**（无命令缓冲）；同一份用例表逐像素 0 差（`tests/test_d3d11_render.c`） |
| **OpenGL 后端** | ✅ **0.5.0**：WGL + OpenGL 3.3 core，全局状态映射；同一份用例表逐像素 0 差（`tests/test_gl_render.c`），窗口路径与 VK/D3D11 输出逐字节相同 |
| D3D12 / Metal 后端 | ⛔ 登记未实现（D3D12 与 VK 同族、验证价值最低；Metal 要开新平台。排期见 [docs/layering.md](docs/layering.md) §5） |

详细状态与理由：[docs/backends.md](docs/backends.md)。像素规则：[docs/raster_rules.md](docs/raster_rules.md)。
**这一层在哪、四家后端怎么接（VK / D3D11 / GL / Metal 的落地顺序与验收线）**：[docs/layering.md](docs/layering.md)。

## 版本

- 语义化版本 `0.16.1`（0.2.0 = 实现改 C++17、0.2.1 = CPU 性能轮、0.2.2 = 分层设计文档、0.3.0 = **Vulkan 后端**、0.4.0 = **D3D11 后端**、0.5.0 = **OpenGL 后端**、0.5.1 = 交换链回读的两个 bug、0.5.2 = D3D11 上传路径 + `wait_idle` 真的等 GPU、0.6.0/0.6.1 = 光照 + 透视校正、0.7.0/0.8.0 = `bingpu_render_plan` 决策面、**0.9.0 = 帧节拍器 `bingpu_pace_*` + GL 的 `wglSwapIntervalEXT` 装载修复**、0.10.0 = 圆角矩形九宫格图元、0.11.0 = 四角带色圆角矩形、0.11.1 = CPU 参考后端共享边 1px 空洞、**0.12.0 = 解析圆角：AA 数学搬进片元着色器（四端）、0.12.1 = 交错绘制时 VK 顶点流截断 + GL 程序没还原、0.13.0 = VK 一帧内多目标来回切（顶点流扩容销毁已录 buffer / 共享 descriptor set / 目标缺依赖 + mip 改降级）**、0.14.0 = MSAA 与真 mip 质量档（VK/GL）、0.14.1 = 记下 MSAA 多目标欠账、0.15.0 = 帧中途销毁目标改退役表（修 `VK_ERROR_DEVICE_LOST`）、0.15.1 = MSAA 档位收进 `render_plan`、**0.16.1 = 延迟 resolve：消掉每进出一次目标的 1×↔4× 往返（ABI 6）**）。
- **二进制 ABI 号 6**：0.16.0 追加 `BGPU_RENDER_DEFER_RESOLVE`（`BgpuRenderBeginDesc.flags` 的一个位值）与 `bingpu_cmd_resolve_target()`，结构体布局一个字节没动。ABI 5 = 0.14.0 的 MSAA/mip 能力位；ABI 4 = 0.12.0 的 `BGPU_VERTEX_SHAPE` + `BgpuShapeVertex`（36 字节）+ `BGPU_PIPE_SHAPE` / `BGPU_CAP_SHAPE` + `bingpu_cmd_draw_shapes()`；ABI 3 = 0.6.0 给 `BgpuMeshVertex` 加 `float w`；ABI 2 = 0.4.0 新增 `BGPU_BACKEND_D3D11`；ABI 1 的 0.1.0–0.3.0 未动过。
- 变更记录：[CHANGELOG.md](CHANGELOG.md)。

## 许可

MIT，见 [LICENSE](LICENSE)。

