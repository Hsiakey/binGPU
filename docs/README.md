# binGPU 文档索引

| 文档 | 讲什么 | 主要读者 |
|---|---|---|
| [contracts.md](contracts.md) | **六条硬契约** + 颜色/帧/像素三个模型的关系 | 所有人（先读这篇） |
| [abi.md](abi.md) | 句柄 / `struct_size` / 错误 / 线程与所有权 / 生命周期 / 为什么没有回调 | 绑定作者、上层作者 |
| [raster_rules.md](raster_rules.md) | 光栅规则：顶点变换、top-left、插值、采样、混合、深度、回读 | 写后端的、写像素测试的 |
| [backends.md](backends.md) | 后端现状表、CPU 的取舍、VK/D3D11 已落地的细节、D3D12 与 GL 的排期、欠账 | 规划的人、写新后端的人 |
| [perf.md](perf.md) | CPU 后端的吞吐基线、每条优化的收益、未做项与原因、校验和 | 要动内层循环的人 |
| [layering.md](layering.md) | **这一层在第几层、往上的两条一致性承诺、进出边界判据、四家后端地形与落地顺序、ABI 2 的三处修订、binUI 迁移台账** | 规划的人、做 VK/D3D11/GL/Metal 后端的人 |

## 读的顺序

- **只想用起来**：根目录 [README.md](../README.md) 的"构建/测试"两节 → 需要的 API 直接看
  `include/bingpu.h`（头文件注释是完整契约）。
- **要绑到别的语言**：`abi.md` → `include/bingpu.h`。重点是句柄语义、`struct_size`、
  错误槽是线程本地的、`*_destroy` 幂等。
- **要写新后端**：`contracts.md` → `raster_rules.md` → `backends.md` → `layering.md`（顺序与验收线）。
  新后端 = 继承 `BackendBase` 实现 5 个钩子 + 资源分配，然后拿 `tests/test_cpu_render.c` 当规格：
  每条断言就是一条必须对齐的规则。
- **要改这一层的设计**：先读 `contracts.md` 的六条，任何改动都要回答"破坏哪一条、为什么值得"。
  再读 `layering.md` §2/§3：两条一致性承诺和进出边界是这一层的**位置定义**，比单条契约更靠上。

## 源码地图（`src/`）

| 文件（C++17） | 职责 |
|---|---|
| `bingpu_api.cpp` | 公共 C ABI 入口：参数/句柄/顺序校验 → 转发后端 → 顶层 try/catch（异常不越过 ABI） |
| `bingpu_backend.h` | `class Backend`（后端接口）+ `BackendBase`（共享状态机）+ 对象类型 |
| `bingpu_backend.cpp` | 对象构造 + 两个后端共用的命令状态机 / 参数校验 / stats 计数 |
| `bingpu_error.cpp` | 线程本地错误槽、`result_name`、版本、格式、**后端注册表**、`shutdown` |
| `bingpu_object.cpp` | 句柄 → 对象 的表（`unique_ptr` + RAII，代数失效、按设备连带销毁） |
| `bingpu_null.cpp` | `NullBackend`：只校验 + 计数 + 完整状态机 |
| `bingpu_cpu.cpp` | `CpuBackend`：软件光栅、深度、采样、混合、回读 |
| `bingpu_vk.cpp` | `VkBackend`：Vulkan 1.3（动态渲染）+ 固定管线 + 批量四边形；`__has_include` 缺头时退化成桩 |
| `bingpu_gl.cpp` | `GlBackend`：WGL + OpenGL 3.3 core（全局绑定点状态）；自带 GL 类型/常量，不 include `<GL/gl.h>` |
| `bingpu_d3d11.cpp` | `D3d11Backend`：即时上下文（无命令缓冲）+ 同一套固定管线；运行期 `LoadLibrary(d3d11.dll)`，不链接 d3d11.lib |
| `bingpu_vk_shaders.h` | quad.vert/frag 的 SPIR-V 数组（`tools/gen_spv_header.py` 生成） |
| `bingpu_d3d11_shaders.h` | quad.hlsl 的 DXBC 数组（`tools/gen_dxbc_header.py` 生成） |
| `bingpu_internal.h` | 内部契约（句柄编码 / 对象表 / 错误槽），**不进 `include/`** |

测试：`tests/test_abi.c`（契约）、`tests/test_cpu_render.c`（CPU 像素）、
`tests/gpu_parity.h`（**GPU 后端 vs CPU 的共用用例表**）、`tests/test_{vk,d3d11,gl}_render.c`
（各一行 `main`，没有对应后端时整份跳过）；工具：`tools/bingpu_info.c`。
