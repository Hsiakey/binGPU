# 后端（backends）

## 现状表

| 后端 | 状态 | 能做什么 | 不能做什么 |
|---|---|---|---|
| `NULL` | **已实现**（0.1.0） | 完整状态机、参数/句柄校验、绘制计数 | 不产生像素；回读返回 `UNSUPPORTED` |
| `CPU` | **已实现**（0.1.0） | 参考像素实现；离屏目标；深度；采样器；仿射；网格；回读 | 不能开窗口（`UNSUPPORTED`）；无 MSAA；无 mip 采样 |
| `VULKAN` | 登记，未实现 | — | 任何 `device_create` 都返回 `UNSUPPORTED` + 原因 |
| `D3D12` | 登记，未实现 | — | 同上 |
| `GL` | 登记，**不迁** | — | 同上（理由见下） |

`AUTO` 的挑选顺序（`bgpu_vt_auto()` 的遍历顺序）：

```
VULKAN → D3D12 → GL → CPU → NULL
```

也就是"真实后端优先，最后才是 CPU 与 NULL"。0.1.0 里前三个都不可用，所以 `AUTO`
落到 **CPU**。上层不该假设 AUTO 一定选到哪个 —— 拿到设备后用 `bingpu_device_info()`
读 `backend` 字段。

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

## Vulkan：第一个真实后端（0.2.0）

目标不是"能画"，而是"**和 CPU 逐字节一致**"。开工顺序：

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

## D3D12：第二个真实后端

排在 Vulkan 之后，**先把 VK 版把状态机钉死**。理由：D3D12 的显式程度最高（根签名、
descriptor heap、fence、swap chain flip model），它验证"状态机 + 资源生命周期"的能力
最强；但它没有"跑在 Linux 上"的兜底，调试成本也最高。VK 版通过之后，D3D12 主要是
"把同一套规则翻译一遍"，差异集中在 present 与 swap chain 的细节。

## GL：为什么不迁

- binUI 的 GL 后端是**真实在用**的（Windows 上最稳的一条路），它已经跑通了三端一致的
  像素验收。把它迁到 binGPU 的收益是"少一份代码"，成本是"把一条已经验证过的路重新
  暴露给新的状态机"。
- GL 的状态机与 binGPU 的"无状态命令"模型差异最大（全局状态、FBO 绑定、纹理绑定），
  翻译层会变成半个 GL 封装，反而不好维护。
- 真要统一的时机：**bin2d 需要 GPU 加速、且 VK/D3D12 已经稳定**之后。那时 GL 可以作为
  "没有 VK 的旧机器"的兜底后端再进来，而不是现在。

## 欠账（0.1.0 明确不做，记在这里）

1. **用户着色器 / SPIR-V**：bin3d 进 GPU 阶段的前提，0.2.0 之后。
2. **深度纹理格式**：现在用 `RENDER_TARGET` 纹理当深度目标（后端自己解释）。
   建议 0.2.0 加 `BGPU_TEXTURE_DEPTH` 或 `BGPU_FORMAT_D32F`。
3. **MSAA**：`desc.msaa_samples` 已预留，CPU 降级为 1；VK/D3D12 落地时按需打开。
   注意：**MSAA 会让"与 CPU 逐像素一致"变成"边界容忍 1px"**，需要单独设计验收方式。
4. **网格光照**：`BgpuMeshVertex` 带法线，但没有光源/材质状态可以喂给它。
   等 bin3d 定义光照模型后再加（那是 ABI 变化）。
5. **多线程记录 / 多帧并行**：`max_frames_in_flight` 现在恒 1；真实后端落地时再按后端
   自报（CPU/NULL 永远是 1）。
6. **透明窗口 / 垂直同步 / present 节拍**：cap 位已经预留（`TRANSPARENT`/`VSYNC`/
   `PRESENT_PACES`），CPU/NULL 都不报；VK/D3D12 落地时按真实能力填。
7. **事件队列**：`poll()` 现在恒 `NONE`；真实后端要把 `DEVICE_LOST`/`RESIZE_REQUIRED`/
   `PRESENT_FAILED` 接到这里。
