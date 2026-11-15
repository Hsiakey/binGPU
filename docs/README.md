# binGPU 文档索引

| 文档 | 讲什么 | 主要读者 |
|---|---|---|
| [contracts.md](contracts.md) | **六条硬契约** + 颜色/帧/像素三个模型的关系 | 所有人（先读这篇） |
| [abi.md](abi.md) | 句柄 / `struct_size` / 错误 / 线程与所有权 / 生命周期 / 为什么没有回调 | 绑定作者、上层作者 |
| [raster_rules.md](raster_rules.md) | 光栅规则：顶点变换、top-left、插值、采样、混合、深度、回读 | 写后端的、写像素测试的 |
| [backends.md](backends.md) | 后端现状表、CPU 的取舍、VK/D3D12 的开工顺序与条件、GL 为什么不迁、欠账 | 规划的人、写新后端的人 |

## 读的顺序

- **只想用起来**：根目录 [README.md](../README.md) 的"构建/测试"两节 → 需要的 API 直接看
  `include/bingpu.h`（头文件注释是完整契约）。
- **要绑到别的语言**：`abi.md` → `include/bingpu.h`。重点是句柄语义、`struct_size`、
  错误槽是线程本地的、`*_destroy` 幂等。
- **要写新后端**：`contracts.md` → `raster_rules.md` → `backends.md`。然后拿
  `tests/test_cpu_render.c` 当规格：每条断言就是一条必须对齐的规则。
- **要改这一层的设计**：先读 `contracts.md` 的六条，任何改动都要回答"破坏哪一条、为什么值得"。

## 源码地图（`src/`）

| 文件 | 职责 |
|---|---|
| `bingpu_api.c` | 公共函数入口：参数/句柄/顺序校验 → 转发到后端虚表 |
| `bingpu_error.c` | 线程本地错误槽、`result_name`、版本、格式、**后端注册表**、`shutdown` |
| `bingpu_object.c` | 句柄 → 对象 的表（代数失效、按设备连带销毁） |
| `bingpu_null.c` | NULL 后端：只校验 + 计数 + 完整状态机 |
| `bingpu_cpu.c` | CPU 参考实现：软件光栅、深度、采样、混合、回读 |
| `bingpu_internal.h` | 内部契约（句柄编码 / 对象表 / 错误槽 / 后端虚表），**不进 `include/`** |

测试：`tests/test_abi.c`（契约）、`tests/test_cpu_render.c`（像素）；工具：`tools/bingpu_info.c`。
