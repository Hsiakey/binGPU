# ABI 说明（abi）

> 面向"要绑定 binGPU 的人"（Python / Go / Rust / 其他 C 调用方）与"要写新后端的人"。
> 公共头是 `include/bingpu.h`；这里只讲**头文件没写清楚或者写了但容易漏**的部分。

---

## 0. 语言：实现是 C++17，ABI 仍是 C（2026-11-15 定）

这两件事**故意分开**：

| | 选什么 | 为什么 |
|---|---|---|
| 对外 ABI | **C 兼容**：定长 POD + `uint64_t` 句柄 + 扁平函数，纯 C 编译器能单独编过头文件 | 看的是"**谁** include 它"：`bindings/` 里 Python / Go / Rust 只吃 C 接口；C ABI 意味着它们想画可以直接绑，不必等上层扩 C 接口。C++ 写、导出 C 符号是 Vulkan loader / HarfBuzz / bgfx / Skia 的常规做法 |
| 实现 | **C++17**：`std::vector` / `std::string` / `std::unique_ptr` / 类层次 / 内部异常 | 看的是**表达力**：这一层要的是句柄表、容器、生命周期、错误串拼接。用 C 写全是手写样板（手动 realloc + 容量字段、两条销毁路线各写一遍析构），收益为零、维护成本压在每一处 |

### "C 会不会更快"——实测过，不会

binUI 的 [`docs/future.md`](../../binUI/docs/future.md) 有实测：同一份 alpha 混合内核分别交给
`gcc -O2` 与 `g++ -O2`（MinGW 15.1.0，`-S -masm=intel`），生成的 `.text` **逐行相同**，
唯一差异是符号名。语言本身不改变汇编。

真差别不是语言，是**能不能把每行的取样器内联掉**（200×1.6M 像素，取三次最好）：

| 窄条带（4px × 40 万行） | 耗时 | 相对纯铺 |
|---|---|---|
| 纯铺一个颜色（下限） | 46.2 ms | — |
| 取样器经**函数指针**传入（C 的唯一写法） | 51.3 ms | **+11.0%** |
| 取样器作**模板参数**（可内联） | 46.9 ms | **+1.5%** |
| 取样器走 `std::function` | 110.1 ms | **+138.4%** |

也就是说：C++ 在这类代码上不是"更慢的选择"，反而是"能把热路径内联掉"的那一侧。

### 异常策略：绝不越过 ABI

- 每个 `extern "C"` 入口都是**顶层 try/catch**：
  `std::bad_alloc` → `BGPU_ERR_OUT_OF_MEMORY`；其它 `std::exception` →
  `BGPU_ERR_BACKEND_FAILED` 并把 `what()` 写进 `bingpu_last_error()`；未知异常同样兜住
  （错误串写"未知异常"）。**没有任何异常能穿出这一层。**
- 注意：公共头里这些函数**没有** `noexcept`（头是冻结的 ABI，不动；而 C++ 要求声明与
  定义的异常规格一致，所以也不能只在定义上补）。等价保证由 try/catch 提供 ——
  绑定的作者可以当它们是 noexcept 的。
- 后端内部（C++ 层）该怎么抛就怎么抛：容器分配失败直接让 `bad_alloc` 冒到入口兜住，
  比自己到处写 `if (!ptr) return OOM;` 可靠得多。

### 唯一的对外代价：静态库带 C++ 运行时

`libbingpu.a` 里是 C++ 符号，**链接时必须带 C++ 运行时**：

- 用 C++ 驱动链接（`g++ … libbingpu.a`）最省事 —— 我们的 `Makefile`/`CMakeLists` 就是这么做的
  （`-static-libgcc -static-libstdc++` 静态链进去，产物自包含）；
- 用 C 驱动（`gcc … libbingpu.a`）或给别的语言绑时，要自己加 `-lstdc++`（或让它把运行时
  静态链进去）。`make abi-check` 的第三条就是这个场景的冒烟检查：
  C 目标文件 + C++ 静态库 → `g++` 链接 → 跑起来。
- 这条代价是"实现用 C++"换来的、也是唯一一条；头文件本身仍然没有任何 C++ 依赖，
  `gcc -x c` 编它不需要 libstdc++。

---

## 1. 句柄：`uint64_t`，0 永远无效

- 7 种句柄：`BgpuDevice / BgpuSwapchain / BgpuTexture / BgpuBuffer / BgpuSampler /
  BgpuPipeline / BgpuCmd`。它们在 ABI 上都是 `uint64_t`（**不是指针**）。
- 内部编码（实现细节，绑定不要依赖）：`[63:60]` 种类、`[59:32]` 代数、`[31:0]` 槽位 + 1。
  销毁时代数 +1，于是**旧句柄立刻失效**，复用的槽也不会让旧句柄"复活"。
- 任何拿 `0` 调用的函数返回 `BGPU_ERR_INVALID_HANDLE`；拿已销毁的句柄、拿别的设备的
  句柄也一样。**种类串用**（把纹理句柄当设备传）同样返回 `BGPU_ERR_INVALID_HANDLE`。
- `*_destroy` 是 `void` 且幂等；对无效句柄是 no-op（清理路径永远不打断程序）。

## 2. `struct_size`：每个描述结构体的第一个字段

规则（所有 `typedef struct Bgpu*` 的输入/输出都适用）：

- 输入结构体：填 `sizeof(你的结构体)`。后端按这个字段判断你认识哪些尾巴：
  `struct_size < 后端已知布局` → `BGPU_ERR_INVALID_ARGUMENT`（**拒绝**，不是越界写）；
  `> 已知布局` → 接受，多出来的尾巴忽略。
- 输出结构体：调用前把 `struct_size` 填成 `sizeof(...)`；后端只写它认识的前缀。
  `struct_size` 比后端认识的小 → `BGPU_ERR_INVALID_ARGUMENT`。
- 加字段是**ABI 兼容**的：`BINGPU_ABI_VERSION` 只在"布局/签名/枚举值变化"时 +1。

**为什么不用 `version` 号而用 `struct_size`**：结构体可能在不同调用点按不同节奏演进
（比如 `BgpuSwapchainDesc` 先加字段、`BgpuTextureDesc` 后加）。一个全局版本号会强迫
调用方整体升级；`struct_size` 是逐结构体的、天然向后兼容的、绑定侧也只需要填一次。

## 3. 错误

- `bingpu_last_error()` 返回 `<函数>: <原因> (file:line)`。**每次失败都会写**；
  成功**不清空**（GetLastError 语义 —— 失败后紧接一个查询不该把原因弄丢）。
- 槽是 `_Thread_local`：渲染线程和资源线程各看各的，不会串台（`test_abi` 用两个线程
  验证：主线程的 `bingpu_cmd_set_viewport` 错误在工作线程失败之后仍然在）。
- 返回的指针有效期到**本线程下一次 bingpu_* 调用**为止：不要存，要留就自己复制。
- `bingpu_clear_error()` 显式清空。

### 结果码怎么选（后端实现时对号入座）

| 码 | 什么时候用 |
|---|---|
| `INVALID_ARGUMENT` | 空指针、`struct_size` 太小、宽度/高度为 0、枚举越界、区域越界 |
| `INVALID_HANDLE` | 0 / 已销毁 / 种类不符 / 属于别的设备 |
| `UNSUPPORTED` | 这个后端/这台机器没这个能力（CPU 后端开窗口、NULL 后端回读） |
| `OUT_OF_MEMORY` | 分配失败 |
| `DEVICE_LOST` | 拔卡 / 驱动重启 / TDR（只有真实后端会报） |
| `STATE` | 调用顺序不对（未 `begin_render` 就 draw、重复 begin、帧未开就 end） |
| `BACKEND_FAILED` | 底层 API 直接报错（原因里要带后端原话） |
| `NOT_READY` | 设备还在异步初始化 / 已经关闭 |
| `LIMIT` | 超出后端资源上限（纹理尺寸、单次四边形数） |

## 4. 线程与所有权

- **单渲染线程**：所有调用（含上传/回读）都在同一个线程。句柄表不加锁。
- 错误槽是线程本地的，所以"多个线程各自读错误"是安全的 —— 但**不要**在多线程里建/删资源。
- 资源所有权是**设备**：`bingpu_device_destroy` 会连带销毁该设备名下所有还活着的资源
  （C 没有析构，这是最后一道保险）。正确用法仍然是"先子后父"：
  纹理 → 缓冲 → 采样器 → 管线 → cmd → 交换链 → 设备。
- 交换链借出的帧目标（`frame_begin` 的 `out_target`）**只是借用**：不要 destroy，
  不要跨帧保存；`frame_end` 之后它的内容就不保证还是你画的那一帧。

## 5. 生命周期顺序

```
device_create
  ├── texture_create / buffer_create / sampler_create / pipeline_create / cmd_create
  ├── swapchain_create
  │     └── frame_begin → cmd_begin_render(target) → draw… → cmd_end_render → frame_end
  │           （frame_read 读"上一帧"）
  └── …
destroy（子 → 父）
```

- 一个 `cmd` 同一时刻只能被**一个帧**抓在手里（没有多线程记录）。
- `frame_begin` 之后、`frame_end` 之前可以反复 `begin_render/end_render`（多趟渲染）。
- 离屏渲染**不需要**帧：直接 `cmd_begin_render({target = 你的 RENDER_TARGET 纹理})` 就行。
  帧只属于交换链。
- `frame_end` 要求当前不在 `begin_render` 里；`frame_begin` 要求不在帧里。

## 6. 为什么没有回调（以及怎么替代）

设备丢失 / 交换链失效 / present 失败都走 `bingpu_poll()`：

```c
BgpuEvent ev = { .struct_size = sizeof(ev) };
while (bingpu_poll(dev, &ev) == BGPU_OK && ev.kind != BGPU_EVENT_NONE) { ... }
```

队列空时返回 `BGPU_OK` + `BGPU_EVENT_NONE`。**唯一的取事件通道**，外壳每帧调一次即可。

理由见 `docs/contracts.md` §5：回调会引入"回调里再进 GPU 层"的可重入问题，而在 C ABI 上
这类 bug 的代价远高于"多一次 poll"。绑定的目标语言也可以把 poll 包装成"每帧取一批事件"
的迭代器，用起来并不会更麻烦。

## 7. 已知的 ABI 粗糙处（0.1.0 记录；0.2.0 只换实现语言，ABI 未动，留给 ABI 2）

1. **`draw_indexed` 的两个偏移是字节偏移**（与 `buffer_update` 一致），但名字看不出单位。
   绑定作者很容易传成"索引个数/顶点个数"。建议改名 `vertex_byte_offset` /
   `index_byte_offset`（改名属于 ABI 变化，要 +1 —— 所以先记录，不乱动）。
2. **没有深度纹理格式**：`depth_target` 用 `RENDER_TARGET` 纹理表示，后端自己解释成深度
   （CPU = float32）。建议加 `BGPU_TEXTURE_DEPTH` 标志或 `BGPU_FORMAT_D32F`。
3. **`BgpuStats` 的计数是累计值**（设备生命周期内），不是"本帧"。语义已写在这里；
   要"本帧"就在上层做差。
4. **`bingpu_device_lost` 返回 `int`**（0/1），与 `BgpuResult` 风格不一致 —— 它是查询，
   不是操作，保持 `int` 是为了"没有错误槽污染"（每帧都调，不该刷错误）。
