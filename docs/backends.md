# 后端（backends）

> 排期与验收口径的总设计见 [`layering.md`](layering.md)：这一层收什么、四家怎么分族、
> 每个后端的"通过标准 + 完成后删掉哪份旧实现"都在那里。本文讲**每个后端的具体情况**。

## 现状表

| 后端 | 状态（类） | 能做什么 | 不能做什么 |
|---|---|---|---|
| `NULL` | **已实现**：`NullBackend` | 完整状态机、参数/句柄校验、绘制计数 | 不产生像素；回读返回 `UNSUPPORTED` |
| `CPU` | **已实现**：`CpuBackend` | 参考像素实现；离屏目标；深度；采样器；仿射；网格；回读 | 不能开窗口（`UNSUPPORTED`）；无 MSAA；无 mip 采样 |
| `VULKAN` | 登记，未实现 | — | 任何 `device_create` 都返回 `UNSUPPORTED` + 原因 |
| `D3D12` | 登记，未实现 | — | 同上 |
| `GL` | 登记，**不迁** | — | 同上（理由见下） |

`AUTO` 的挑选顺序（注册表的遍历顺序，实现在 `src/bingpu_error.cpp`）：

```
VULKAN → D3D12 → GL → CPU → NULL
```

也就是"真实后端优先，最后才是 CPU 与 NULL"。目前前三个都不可用，所以 `AUTO`
落到 **CPU**。上层不该假设 AUTO 一定选到哪个 —— 拿到设备后用 `bingpu_device_info()`
读 `backend` 字段。

> 加入 D3D11 / Metal 之后这条顺序要重排（建议 `VULKAN → D3D11 → D3D12 → GL → METAL →
> CPU → NULL`：按"能开窗口且最稳"优先，同族里 VK 先于 D3D12）。**新增 `BgpuBackendKind`
> 枚举值 = ABI 变化**，所以这条留给 ABI 2，与 [`layering.md`](layering.md) §6 的三处修订
> 一起做。

---

## 后端在代码里长什么样（0.2.0 起是 C++ 类）

```cpp
class Backend {                       // src/bingpu_backend.h：接口，形状照 binUI 的 IBinUIGfx
    virtual BgpuBackendKind kind() const noexcept = 0;
    virtual const char* name() const noexcept = 0;
    virtual bool available() const noexcept = 0;      // 登记了但没实现 → false
    virtual BgpuResult createDevice(...);             // 设备
    virtual BgpuResult createSwapchain(...);          // 交换链 / 帧
    virtual BgpuResult createTexture(...);            // 资源
    virtual BgpuResult createBuffer(...);
    virtual BgpuResult createSampler(...);
    virtual BgpuResult createPipeline(...);
    virtual BgpuResult createCmd(...);                // 命令
    virtual BgpuResult drawQuads(...);
    virtual BgpuResult drawIndexed(...);
    // … beginRender/endRender/setXxx/readback
};

class BackendBase : public Backend {  // 两个后端**一字不差**的那部分
    // 命令状态机、参数校验、stats 计数，以及 5 个钩子：
protected:
    virtual void        presentFrame(Swapchain&);                 // CPU: back→front
    virtual BgpuResult  prepareDepth(Texture&, Texture&, desc);   // CPU: 懒分配+清除
    virtual BgpuResult  clearColor(Texture&, uint32_t);           // CPU: 写预乘像素
    virtual BgpuResult  rasterQuads(Cmd&, const BgpuVertex*, n);  // CPU: 光栅
    virtual BgpuResult  rasterIndexed(...);                       // CPU: 光栅
};

class NullBackend final : public BackendBase { /* 钩子全用基类空实现 */ };
class CpuBackend  final : public BackendBase { /* 覆盖钩子 + 资源分配 */ };
```

为什么 0.1.0 的"两个后端各写一套状态机"不再需要：
  当时的价值是**交叉验证**校验逻辑。这份价值现在由 `tests/test_abi.c` 固定住了
  （同样的误用 → 同样的结果码，239 条断言）。收成一份之后，"两个后端给出不同结果码"
  在结构上就不可能发生 —— 这比"靠人肉保持一致"可靠。

资源管理一律 RAII：`CpuTexture`/`CpuBuffer`/`CpuSwapchain` 的析构负责释放像素/缓冲/位图
并回写 `stats.bytes_live`；"显式 destroy"和"设备销毁连带清理"走的是同一份析构。

将来加后端 = 新写一个类 + 在注册表里登记一行：

| 顺序 | 后端 | 类 | 覆盖什么 |
|---|---|---|---|
| 1 | Vulkan | `VkBackend` | 全部（原生映射） |
| 2 | D3D11 | `D3D11Backend` | 全部（状态映射；immediate context） |
| 3 | GL | `GlBackend` | 全部（状态映射；WGL） |
| 4 | Metal | `MetalBackend` | 全部（原生映射；要同时开 macOS 外壳） |
| 待定 | D3D12 | `D3D12Backend` | 全部（原生映射；与 VK 同族，验证价值最低） |

顺序与每步的"通过标准 + 删掉哪份旧实现"见 [`layering.md`](layering.md) §5。

共享的 `BackendBase` 不用改 —— 状态机、校验、计数是后端无关的。

---

## CPU：为什么它是"裁判"而不是"备胎"

CPU 后端的定位不是"没有 GPU 时的降级"，而是**像素定义的权威**：

- 它没有驱动、没有硬件差异，同样的命令在任何机器上逐字节相同（整数运算 + IEEE 单精度，
  加法顺序固定）；
- 它能回读（`frame_read` / `texture_read`），所以 CI 无 GPU 也能出图；
- GPU 后端写完之后的验收就是"同一份命令，和 CPU 出的图逐像素比" ——
  binUI 的三端一致（GL/VK/D3D11）当年靠的是"共享一份遍历 + 互相 diff"，
  binGPU 把这条升级成"有一个**代数上的**参考实现"。

它的取舍（写在这里，免得被当成 bug）：

| 取舍 | 说明 |
|---|---|
| 单线程、立即执行 | `draw_quads` 当场光栅化；没有命令队列，`frame_end` 就是 `memcpy` |
| 无 MSAA | `caps.msaa_samples == 1`；`desc.msaa_samples` 接受 4/8 但**降级为 1**（info 上报真实值） |
| 无 mip 采样 | 固定管线没有 LOD 输入，采样永远取第 0 级；`mip_levels` 接受后降级为 1 |
| 无窗口 | CPU 不碰 HWND/DPI/合成器（那些是"不可复现"的来源）；窗口呈现交给 VK/D3D12 |
| 无设备丢失 | `bingpu_device_lost()` 恒 0；`poll()` 永远 `NONE` |
| 法线不参与 | 网格按"顶点色 × 纹理"着色（没有光源状态，见欠账） |

## Vulkan：第一个真实后端（排期在 0.2.0 之后）

目标不是"能画"，而是"**和 CPU 逐字节一致**"。为什么它排第一（最显式、最能把 ABI 的不足
顶出来），见 [`layering.md`](layering.md) §5。开工顺序：

1. 设备 / 交换链 / 帧模型（`begin → record → present`，动态渲染，不挂 render pass）；
2. 管线：五种混合各一条（预乘 / replace / keep / erase / additive），两种顶点布局；
3. `draw_quads`：顶点环 + 按 (pipeline, texture, scissor) 切批；
4. 离屏目标 + 深度（`D32_SFLOAT`），采样器（`NEAREST/LINEAR × CLAMP/REPEAT`）；
5. `draw_indexed`（网格）；
6. 验收：`raster_rules.md` 里每条规则各一个用例，CPU vs VK 逐像素比 = 0 差异。

**开工条件**（不满足就别开始）：

- CPU 后端的像素规则已经稳定（本文 `raster_rules.md` 不再改）—— 否则是在追一个移动的靶子；
- 有一台能跑验证层的机器（`VK_LAYER_KHRONOS_validation` 装好），否则一致性无从谈起；
- 绑定层（bin2d/bin3d）已经有一层 C++ RAII 把句柄包起来 —— 否则 VK 的资源生命周期
  会漏进上层的每个调用点。

## D3D12：`layering.md` 里已排到 VK/D3D11/GL 之后

0.2.2 起排期调整（理由见 [`layering.md`](layering.md) §5）：**D3D11 提到 D3D12 之前**。
原来的理由是"D3D12 显式程度最高，最能验证状态机"，但那条价值已经由 VK 拿到（两者同族：
PSO + 命令缓冲 + 显式同步），D3D12 再写一遍学不到新东西，却要重写约 2270 行。而 **D3D11
是异族模型**（immediate context、无命令缓冲），它能反向验证"这套 ABI 对上一代 API 也
成立" —— 那才是真正的未知数。

D3D12 不删除，只往后放：等 VK 后端把状态机钉死、D3D11 证明异族可用之后，D3D12 主要是
"把同一套规则翻译一遍"，差异集中在根签名、描述符堆、fence 与 flip model 交换链。

## GL：不迁 → **改成"最后迁"**（0.2.2 修订）

原判断是"不迁"，理由是"GL 的状态机与无状态命令差得最远，翻译层会变成半个 GL 封装"。
`layering.md` §4 推翻了这个担心的前提：**binGPU 的指令面是固定功能批处理，不是 RHI**，
所以 GL 的适配是"3~4 处状态映射 + 一次 `glDrawArrays`"，量级和 binUI `binui_gl.cpp` 里
present/状态那 300 行相当，不需要命令缓冲模拟（`contracts.md` §6 本来就禁止跨线程记录）。

结论修订为：**GL 排在最后，但理由是"没有用户"**（Windows 上优先 VK/D3D11），不是技术上
不可行。旧机器兜底、以及 bin2d 需要 GPU 加速时，GL 仍是成本最低的第四条路。

## 欠账（明确不做，记在这里）

1. **用户着色器 / SPIR-V**：bin3d 进 GPU 阶段的前提，排在 VK 后端之后。
2. **深度纹理格式**：现在用 `RENDER_TARGET` 纹理当深度目标（后端自己解释）。
   建议将来加 `BGPU_TEXTURE_DEPTH` 或 `BGPU_FORMAT_D32F`。
3. **MSAA**：`desc.msaa_samples` 已预留，CPU 降级为 1；VK/D3D11 落地时按需打开。
   注意：**MSAA 会让"与 CPU 逐像素一致"变成"边界容差"** —— 容差怎么定、为什么现在选
   "通道差 ≤2 / 差异像素 ≤0.1%"而不选"CPU 也做 4× 参考"，见
   [`layering.md`](layering.md) §2 的 P1。
4. **网格光照**：`BgpuMeshVertex` 带法线，但没有光源/材质状态可以喂给它。
   等 bin3d 定义光照模型后再加（那是 ABI 变化）。
5. **多线程记录 / 多帧并行**：`max_frames_in_flight` 现在恒 1；真实后端落地时再按后端
   自报（CPU/NULL 永远是 1）。
6. **透明窗口 / 垂直同步 / present 节拍**：cap 位已经预留（`TRANSPARENT`/`VSYNC`/
   `PRESENT_PACES`），CPU/NULL 都不报；VK/D3D11 落地时按真实能力填。
   **present 策略本身要升格成 ABI 的一等公民**（交换链 / 读回贴屏 / 分层窗三态 + 守卫判定），
   提案与理由见 [`layering.md`](layering.md) §6.3 —— binUI 三端各写了一遍，VK 那份在
   "窗口铺满显示器"时出过 bug（v0.19.1）。
7. **事件队列**：`poll()` 现在恒 `NONE`；真实后端要把 `DEVICE_LOST`/`RESIZE_REQUIRED`/
   `PRESENT_FAILED` 接到这里。
