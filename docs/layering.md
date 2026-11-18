# 分层（layering）：binGPU 在第几层，四家后端怎么接

> 写于 2026-11-18。来源是 binUI 三个 GPU 后端（GL / VK / D3D11）跑通"逐像素一致"之后的复盘：
> 哪些重复**该收上来**、哪些收进这一层会**把抽象做坏**。每个数字都能在 binUI 仓库的
> `CHANGELOG.md` / `docs/startup.md` 里查到。
>
> 这篇**不改 ABI**（ABI 1 照旧），它只定义这一层的位置、边界和验收口径；改动清单在 §6，
> 标的是 **ABI 2**。

---

## 0. 三句话

1. binGPU = **设备与呈现层** + **固定功能批处理指令面**。它统一的是**语义**，不是图形 API 的指令集。
2. 往上有**两条**一致性承诺（§2），必须分开验收；混在一起测会得出完全相反的错误结论。
3. 往下四家分成**两族**（§4）：VK / D3D12 / Metal 同族（PSO + 命令缓冲），GL / D3D11 异族
   （即时状态）。同族原生映射，异族做**状态映射**，两边都**不需要翻译层**。

---

## 1. 为什么这一层值得存在（用 binUI 的血换的）

binUI 现在的 GPU 侧是这么分的：

| 文件 | 行数 | 职责 |
|---|---|---|
| `binui_vk.cpp` | 4276 | Vulkan：设备/交换链/MSAA/滤镜/呈现 |
| `binui_d3d11.cpp` | 2270 | D3D11：同上 |
| `binui_gl.cpp` | 2040 | GL：同上 |
| `binui_app.h` | 1836 | 窗口外壳（三端共用同一个模板） |
| `binui_draw_nodes.h` | 1788 | **三端共用**的节点树遍历 |
| `binui_glyph_atlas.h` | 1169 | **三端共用**的字形图集 |

即：**绘制语义早就统一了**（共用遍历 + 共用图集 + 7 个原语），重复的是**设备与呈现**那一段 ——
交换链、resize 策略、present 路径、离屏目标池、MSAA 解析，每个后端各写一遍。

而 bug 全部长在这一段：

| 版本 | bug | 长在哪 |
|---|---|---|
| v0.19.0 | D3D11 `ResizeBuffers` 永远返回 `DXGI_ERROR_INVALID_CALL`（RTV 还绑在管线上），交换链停在旧尺寸，画面被系统拉伸 | 设备/交换链 |
| v0.19.0 | D3D11 resize 后视口没换（`RSSetViewports` 只在建状态时设过一次），内容放大 `1100/820≈1.34` 倍并裁掉右下 | 设备/交换链 |
| v0.19.0 | VK 每次 resize 重建**两份**交换链（缓存的 `currentExtent` 过期）：26.6 ms → 4.7 ms | 设备/交换链 |
| v0.19.1 | VK 窗口铺满显示器后冻在上一帧 + 黑带：flipping 窗口下 GDI 贴屏"成功返回"但被合成器忽略 | 呈现策略 |
| v0.19.2 | 试"拖拽期不重建交换链"：resize 3.95 → 1.58 ms，但 present 11.4 → 13.6 ms，整帧 16.0 ms 不变 → **回滚** | 呈现策略 |

三条结论：

- **该收的是"设备 + 呈现"**，不是绘制原语（那个已经收过了）。
- 五个里有三个是"**某一端的行为差异**"。这类 bug 只有在"三份实现 vs 一个定义"的对比下才会
  稳定暴露 —— 这就是 binGPU 存在的操作性理由，比"少写代码"硬得多。
- **收益不是性能**。VK resize 的 4.66 ms 里有 2.8–3.3 ms 是 `vkCreateSwapchainKHR` 本身
  （驱动内部要重建 DXGI flip 交换链），抽象层一分钱也省不下来。收益是
  **少写第四份实现 + 一套门禁管四端**。

---

## 2. 往上的两条承诺（必须分开验收）

### P1 — binGPU 承诺：同一串命令 → 同样的像素

输入只有 `BgpuVertex`（位置/UV/颜色）+ 六个状态（viewport / scissor / pipeline / texture /
sampler / affine）。规则由 [`raster_rules.md`](raster_rules.md) 定义，**裁判是 CPU 参考后端**。

- 验收：`tests/test_cpu_render.c` 里每条规则，在 VK / D3D11 / GL 各跑一遍，**逐像素 0 差**。
- 已知边界：**MSAA 会破坏 0 差**（GPU 的 resolve 位置、子采样模式与 CPU 不同）。两个选择，
  写清楚而不是含糊：
  - (a) CPU 后端也做 4× 参考 —— 能维持 0 差，但 CPU 慢 4 倍、实现多一份子采样逻辑；
  - (b) MSAA 帧的 P1 降级为"最大通道差 ≤ 2 / 差异像素 ≤ 0.1%"，单独出一张容差表。
  - **当前选 (b)**：MSAA 只影响边缘，而 binUI 的三端一致在 0.16.x 就是靠"容差 0 + 通道阈值
    128"做到的。等 bin3d 的深度/网格进来再评估 (a)。

### P2 — 上层承诺：同一份内容 → 同一串命令

binUI / bin2d / bin3d 自己保证"同一份 UI/场景生成同一串 `BgpuVertex`"。裁判是上层自己的基准
（binUI：18 张 GDI 基准 + `pixels-vk`（容差 0）+ `pixels-d3d11`）。

**为什么必须分开**：v0.19.0 的三个 bug 没有一个在"命令"层。只做 P1 → 三个 bug 全漏；只做 P2 →
每个上层都要重新解决一遍跨端一致（binUI 已经用 1788 + 1169 行解决了，bin2d 和 bin3d 得再来一遍）。

---

## 3. 边界判据：什么进 binGPU，什么留在上层

**一句话**：这条信息**是否与具体图形 API 有关**？有关 → binGPU；无关 → 上层。

| 该进 binGPU | binUI 现在几份 |
|---|---|
| 设备 / 交换链 / 帧模型 | 3 |
| present 路径 + 守卫判定（§6.3） | 3（VK 那份出过 flipping bug） |
| resize 合并策略（每帧至多一次重建） | 3 |
| MSAA 解析 / 离屏目标池 / 图层 | 3 |
| 5 种混合、采样器、纹理上传（含脏矩形上传） | 3 |
| 深度附件（bin3d 阶段） | 0（还没有） |

| 不该进 binGPU | 为什么 |
|---|---|
| 布局 / flex / 遍历顺序 | 与图形 API 无关；`binui_draw_nodes.h` 就是这条边界的实现 |
| 字形光栅化 / 图集 | 同一份 CPU 位图，三端共用 `binui_glyph_atlas.h` 才逐像素一致 |
| 路径展平 / 九宫格圆角几何 | 几何在 CPU 侧已经算成四边形；binGPU 只负责"忠实地画四边形" |
| 用户着色器 / 任意管线状态 / descriptor | 见 [`contracts.md`](contracts.md) §1；**它同时是分层判据** |

反面教材一句话：**binGPU 长成 RHI 的那天，就是四端一致性失效的那天。**
一旦暴露任意管线/着色器，GL 和 Metal 的实现就变成翻译器，P1 的 0 差立刻不再可证。

---

## 4. 四家的地形：三家同族，两家异族

| 后端 | 模型 | 在 binGPU 里的形状 | 主要风险 |
|---|---|---|---|
| Vulkan | PSO + command buffer + 显式同步 | 原生映射 | 同步与生命周期（最挑 ABI，所以先做） |
| D3D12 | 同上 + 根签名 / 描述符堆 | 原生映射 | 样板多、信息量低（与 VK 同学一遍） |
| Metal | 同上（`MTLRenderPipelineState` + `MTLCommandBuffer` + encoder） | 原生映射 | **成本不在 GPU 层**：窗口外壳（Cocoa + `CAMetalLayer`）、字体（CoreText）、IME / 拖拽 / 像素基准全套要重来 |
| GL | 即时状态（全局状态 + FBO/纹理绑定） | **状态映射**：`BgpuCmd` 退化成"当前状态 + 立即提交" | 状态泄漏；scissor/viewport 的左下原点差异 |
| D3D11 | immediate context | 同上 | 无命令缓冲，但 `contracts.md` §6 已规定单线程记录，所以**不需要** Deferred Context |

关键推论：**异族后端不需要模拟命令缓冲。** `BgpuCmd` 只是一个"记录器"抽象；在 GL/D3D11 里它
退化为一个状态结构体 + 立即提交，语义完全不变 —— 因为 §6 已经禁止跨线程记录、§2 已经禁止
交换链局部重绘。

**这条推论修正了 [`backends.md`](backends.md) 里"GL 为什么不迁"的担心**：当时担心"翻译层会
变成半个 GL 封装"。在固定功能批处理面下，GL 的适配是 3–4 处状态映射 + 一次 `glDrawArrays`，
量级和 binUI `binui_gl.cpp` 里 present/状态那 300 行相当，不是封装整个 GL。**仍然排在最后**，
但理由是"没有用户"，不是"技术上不行"。

---

## 5. 落地顺序与删除条件

| 顺序 | 后端 | 为什么在这个位置 | 通过标准 | 完成后删掉 |
|---|---|---|---|---|
| 1 | VK | 最显式，会把 ABI 的不足立刻顶出来 | `raster_rules.md` 每条规则 vs CPU **0 差**；binUI 的 `pixels-vk` + `resize-bench` + `vk-window` 三关 | `binui_vk.cpp`（4276 行）→ 薄适配层 |
| 2 | D3D11 | **异族模型的代表**，且 binUI 有现成实现（2270 行）可对拍；能反向验证"ABI 是不是太现代了" | 同上 + `pixels-d3d11` | `binui_d3d11.cpp` → 薄适配层 |
| 3 | GL | 同为异族；旧机器兜底 | 同上 + `pixels`（18 张 GDI 基准） | `binui_gl.cpp`（2040 行）→ 薄适配层 |
| 4 | Metal | 同族，实现最省力；但开的是**新平台** | 平台外壳 + 一套新建的像素基准 | — |
| 待定 | D3D12 | 与 VK 同族，学不到新东西；目前没有用户 | — | — |

**这条排期改动了 [`backends.md`](backends.md)**：原来写"VK → D3D12 → GL"，现在把 **D3D11
提到 D3D12 之前**。理由：异族模型能验证"这套 ABI 对上一代 API 也成立"，而 D3D12 与 VK 同族，
验证价值最低却要重写 2270 行。D3D12 不删除，只往后放。

**替换，不并存**（这条比 ABI 更重要）：VK 后端通过就把 `binui_vk.cpp` 删掉。一旦"binUI 自己
那套 VK"和"binGPU 的 VK"同时存在，就有两个真相，像素门禁立刻失去意义。

---

## 6. 要动 ABI 的三处（ABI 2；ABI 1 不动）

### 6.1 句柄不能走哈希表

一帧穿过 ABI 的次数：binUI 现在 **103 个原语 / ≈938 个四边形 / 109.9 KB**每帧，节点遍历
**16.6 µs/帧**；每个原语都要 `set_texture` / `set_pipeline` / `draw_quads`。如果每次调用都查
对象表（几十 ns 级），光查表就能把遍历翻倍。

建议：

- 句柄内部直接是**指针 + 魔数**（`uint64_t`：高 16 位魔数、低 48 位指针）；
- 深度校验（代数 / 所属设备 / 是否已销毁）只在 `BGPU_DEVICE_VALIDATE` 打开时做，
  发布版靠魔数 + 指针合法性；
- 可量化的验收线：**binUI 迁移后 `make bench-draw` 的遍历时间退化不超过 20%**（基线 16.6 µs/帧）。

### 6.2 冗余状态剔除是后端契约，不是优化

上层每帧会重复 `set_pipeline`（同一个管线画几百个四边形）、`set_texture`。现在 binUI 自己
做去重（记住上一次管线/纹理），搬进 binGPU 后这条责任必须落在**后端**：

> 后端必须记住上一次状态，并在 draw 时按 `(pipeline, texture, scissor)` 切批；
> 上层不做去重，也不该被要求做。

写进头文件注释 + `backends.md` 的后端清单。否则四份实现各自决定在哪儿去重，行为又开始分叉。

### 6.3 present 是一等公民：三态 + 守卫

binUI 现在有四条呈现路径，**分散在各后端里**：

| 路径 | 触发条件 | 实现（GL / VK 同构） |
|---|---|---|
| 交换链 present | 常规 | `SwapBuffers` / `vkQueuePresentKHR` |
| 读回 + GDI 贴屏 | 拖拽 resize 期、远程会话、强制 | `SetDIBitsToDevice`（1:1，不缩放） |
| 分层窗口 | 透明窗口 | `UpdateLayeredWindow` |
| （曾错误地合并） | 客户区铺满显示器 | flipping 窗口下前两种都不生效 → v0.19.1 |

建议 ABI 2 显式化：

```c
typedef enum BgpuPresentMode {
    BGPU_PRESENT_SWAPCHAIN = 0,  /* 正常：交换链呈现，可能阻塞垂直同步 */
    BGPU_PRESENT_READBACK  = 1,  /* 读回 + 平台贴屏（拖拽期 / 远程会话 / 驱动已知崩溃路径） */
    BGPU_PRESENT_LAYERED   = 2   /* 分层窗口（透明窗） */
} BgpuPresentMode;

BgpuResult bingpu_present(BgpuSwapchain sc, BgpuPresentMode mode);
```

**守卫判定收进 binGPU**（"客户区是否盖满显示器 / 是否远程会话 / 是否拖拽中"）：它在 Windows 上
是同一套判据，三端各实现了一遍；VK 那次翻车正是因为判定散在外面、GDI 路径没被排除。

另外把两条已经用血换来的规则写成契约：

- **每帧至多一次交换链重建**（binUI 的 `applyPendingResize` 合并语义）。resize 期间每帧重建
  要付一次 4.66 ms；重复重建直接翻倍（v0.19.0 就是每帧两份）。
- **拖拽期不重建交换链不划算**：实测 resize 省 3 ms、present 多付 2.2 ms，整帧 16.0 ms 不变。
  写进文档，省得第四份实现再试一遍。

---

## 7. 明确不做

| 不做 | 理由 |
|---|---|
| 用户着色器 / SPIR-V | [`contracts.md`](contracts.md) §1；同时是 §3 的分层判据 |
| 多线程记录 | `contracts.md` §6；上层要并行就把每帧拆给多个 cmd，不进这一层 |
| 交换链局部重绘 | `contracts.md` §2 |
| 布局 / 遍历 / 图集 / 路径几何 | §3 |
| 反向依赖（binGPU 认识 binUI 的类型） | 方向必须单向：上层只**消费** `bingpu.h` |
| 把 binGPU 做成"引擎" | §3 的判据就是为此写的 |

---

## 8. 迁移台账（binUI 现状对照）

| binUI 资产 | 行数 | 迁移后 |
|---|---|---|
| `src/backends/binui_vk.cpp` | 4276 | **删**（换成 binGPU 的 VK 后端 + 薄适配） |
| `src/backends/binui_d3d11.cpp` | 2270 | **删**（同上） |
| `src/backends/binui_gl.cpp` | 2040 | **删**（同上） |
| `src/backends/binui_app.h`（窗口外壳） | 1836 | **留**（Win32 消息循环 / IME / 拖拽不属于 GPU 层） |
| `src/backends/binui_draw_nodes.h` | 1788 | **留**（P2 的席位） |
| `src/backends/binui_glyph_atlas.h` | 1169 | **留**（P2 的席位，bin2d 将来共用） |
| `include/binui_path.h` | 1112 | **留**（几何，将来属 bin2d） |

迁移完成的判据不是"代码变少"，而是：**binUI 三个后端加起来 < 800 行**（只剩"把
`IBinUIGfx` 调用翻译成 `bingpu_cmd_*`"），且 `pixels` / `pixels-vk` / `pixels-d3d11` 三关全绿。

上层的两份对应文档（同一批写的，互相引用）：

| 文档 | 讲什么 |
|---|---|
| `../bin2D/docs/layering.md` | 2D 绘制核：认不认节点这条分界、与 binGPU 的 P1/P2 落点、对 UI 库的迁移台账、**对 ABI 2 没有新增原语诉求** |
| `../bin3D/docs/layering.md` | 3D 库：软光栅是像素权威、3D 为什么只能统计一致、**对 ABI 2 要"光照状态"而不是自定义着色器** |

---

## 9. 一句话总结

binGPU 买到的不是性能（VK 那 4.66 ms 是驱动成本），是**"一个定义 + 四份实现 + 一套门禁"**。
它成立的前提只有一条：**指令面停在"固定功能批处理"上**。这条一破，四端一致就得靠每个上层
自己再解决一遍。
