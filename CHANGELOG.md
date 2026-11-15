# 变更记录

格式照 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)；版本号遵循
[语义化版本](https://semver.org/lang/zh-CN/)。`ABI` 指的是 `BINGPU_ABI_VERSION`
（结构体布局 / 函数签名 / 枚举值一变就 +1，与语义化版本无关）。

## [0.1.0] - 2026-11-15

第一个可用版本。目标是"把设备与呈现这一层定下来，并且有一个**可复现的像素裁判**"。

### Added

- **公共 C ABI**（`include/bingpu.h`，ABI 1）：7 种 `uint64_t` 句柄、`struct_size` 前缀的
  描述结构体、10 个结果码、线程本地错误串（`<函数>: <原因> (file:line)`）、无回调的
  `bingpu_poll()` 事件通道。
- **设备 / 交换链 / 帧模型**：`frame_begin → cmd_begin_render → 记录 → cmd_end_render →
  frame_end`，`frame_read` 回读"上一帧"；交换链目标以**借用句柄**形式给出。
- **资源**：纹理（`RGBA8` / `BGRA8` / `R8`，`RENDER_TARGET` / `CPU_READ` / `DYNAMIC` /
  `MIPMAPS` 标志）、缓冲（顶点 / 索引 / uniform）、采样器（线性/最近 × 钳制/重复）、
  管线（2D / 网格顶点布局 + 五种混合 + 深度/剔背/纹理标志）。
- **命令**：viewport、scissor（覆盖语义、负数关闭）、管线/纹理/采样器绑定、2×3 仿射状态、
  `draw_quads`（每 quad 4 顶点 TL/TR/BL/BR 展开两个三角形）、`draw_indexed`（32 位索引，
  字节偏移）。
- **CPU 参考实现**（`src/bingpu_cpu.c`）：预乘 RGBA8 存储、像素中心 + top-left 填充规则、
  屏幕空间线性插值、五种混合严格按预乘公式、双线性/最近采样 + 钳制/重复、float32 深度
  （LESS）、离屏渲染、反预乘回读、网格绘制、有意义的 stats。
- **NULL 后端**（`src/bingpu_null.c`）：完整状态机 + 参数/句柄校验 + 绘制计数，不产生像素。
- **后端注册表**：`NULL` / `CPU` 可用，`VULKAN` / `D3D12` / `GL` 登记但不可用（明确原因）；
  `AUTO` 的挑选顺序为 真实后端 → CPU → NULL。
- **测试**：`tests/test_abi.c`（239 断言，含双线程错误槽隔离）、`tests/test_cpu_render.c`
  （334 断言，逐像素手算期望值）。
- **工具**：`tools/bingpu_info`（版本 / 后端可用性 / caps / stats / 最小调用序列）。
- **构建**：`Makefile`（`all` / `test` / `abi-check` / `clean`）与 `CMakeLists.txt`
  （静态库 + 两个 ctest 用例）。
- **文档**：`docs/contracts.md`（六条契约）、`docs/abi.md`、`docs/raster_rules.md`、
  `docs/backends.md`、`docs/README.md`、`tests/README.md`。

### Known limitations

- 没有可用的 GPU 后端：`VULKAN` / `D3D12` 的 `device_create` 返回 `BGPU_ERR_UNSUPPORTED`，
  `GL` 不迁（理由见 `docs/backends.md`）。
- CPU 后端只做 HEADLESS（带窗口返回 `UNSUPPORTED`）；无 MSAA；无 mip 采样；网格法线不参与
  着色（没有光源状态）。
- `draw_indexed` 的两个偏移是**字节**偏移（名字没体现单位，0.2.0 再改名 —— 属 ABI 变化）。
- `depth_target` 用 `RENDER_TARGET` 纹理表示（没有专门的深度格式）。

[0.1.0]: https://example.invalid/bingpu/releases/tag/v0.1.0
