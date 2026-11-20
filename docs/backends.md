# 后端（backends）

> 排期与验收口径的总设计见 [`layering.md`](layering.md)：这一层收什么、四家怎么分族、
> 每个后端的"通过标准 + 完成后删掉哪份旧实现"都在那里。本文讲**每个后端的具体情况**。

## 现状表

| 后端 | 状态（类） | 能做什么 | 不能做什么 |
|---|---|---|---|
| `NULL` | **已实现**：`NullBackend` | 完整状态机、参数/句柄校验、绘制计数 | 不产生像素；回读返回 `UNSUPPORTED` |
| `CPU` | **已实现**：`CpuBackend` | 参考像素实现；离屏目标；深度；采样器；仿射；网格；回读 | 不能开窗口（`UNSUPPORTED`）；**恒为 1× MSAA / 1 级 mip**（它是"同档裁判"，不参与高档） |
| `VULKAN` | **已实现（0.3.0）**：`VkBackend` | 设备 / 交换链 / present / 纹理上传回读 / 缓冲 / 采样器 / 五种混合 / 离屏 + 深度 / 网格（`draw_indexed`）/ 仿射 / **MSAA 4×/8×（resolve）+ 真 mip 链** | 默认仍是 1×/1 级；无透明窗口；单帧飞行；要求 Vulkan 1.3（动态渲染） |
| `D3D11` | **已实现（0.4.0）**：`D3d11Backend` | 同上，走**即时上下文**（无命令缓冲 / 无 PSO） | **无 MSAA、无 mip（这一版明确不做，见"质量档"一节）**；无透明窗口；单帧飞行；无 Deferred Context（不需要） |
| `D3D12` | 登记，未实现 | — | 同上 |
| `GL` | **已实现（0.5.0）**：`GlBackend` | 同上，WGL + OpenGL 3.3 core（全局绑定点状态）；**MSAA 4×/8×（multisample FBO + blit resolve）+ 真 mip 链** | 默认仍是 1×/1 级；无透明窗口；单帧飞行；一个 HWND 只能一个 GL 设备 |

`AUTO` 的挑选顺序（注册表的遍历顺序，实现在 `src/bingpu_error.cpp`）：

```
VULKAN → D3D11 → D3D12 → GL → CPU → NULL
```

也就是"真实后端优先，最后才是 CPU 与 NULL"。顺序按 [`layering.md`](layering.md) §5：
同族里 VK 先于 D3D12，异族 D3D11 紧随 VK。0.4.0 起 `AUTO` 在这台机器上选到 **Vulkan**；
上层不该假设 AUTO 一定选到哪个 —— 拿到设备后用 `bingpu_device_info()` 读 `backend` 字段。

> 这条顺序是 ABI 2（0.4.0）定下的：新增 `BGPU_BACKEND_D3D11 = 6` 枚举值（追加在末尾，
> 既有值不变），`BINGPU_ABI_VERSION` 从 1 涨到 2。Metal 没进枚举 —— 它要同时开 macOS 外壳，
> 等真的做的时候再追加。

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

class BackendBase : public Backend {  // 四个后端**一字不差**的那部分
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
  （同样的误用 → 同样的结果码，246 条断言）。收成一份之后，"两个后端给出不同结果码"
  在结构上就不可能发生 —— 这比"靠人肉保持一致"可靠。

资源管理一律 RAII：`CpuTexture`/`CpuBuffer`/`CpuSwapchain` 的析构负责释放像素/缓冲/位图
并回写 `stats.bytes_live`；"显式 destroy"和"设备销毁连带清理"走的是同一份析构。

将来加后端 = 新写一个类 + 在注册表里登记一行：

| 顺序 | 后端 | 类 | 覆盖什么 |
|---|---|---|---|
| 1 | Vulkan | `VkBackend` | ✅ **已完成（0.3.0）**：动态渲染 + 固定管线 + 批量四边形 + 五种混合 + 深度 + 网格 |
| 2 | D3D11 | `D3d11Backend` | ✅ **已完成（0.4.0）**：即时上下文状态映射 + 同一套固定管线 + 交换链/present |
| 3 | GL | `GlBackend` | ✅ **已完成（0.5.0）**：WGL + 3.3 core 状态映射 + 同一套固定管线 |
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
| 无 MSAA | CPU 参考后端恒为 1（`caps.msaa_samples == 1`）；即使 `desc.msaa_samples = 4/8` 也**降级为 1**，它只能当"同档裁判" |
| 无 mip 采样 | 固定管线没有 LOD 输入，采样永远取第 0 级；`mip_levels` 接受后降级为 1。真要 mip 用 VK / GL |
| 无窗口 | CPU 不碰 HWND/DPI/合成器（那些是"不可复现"的来源）；窗口呈现交给 VK / D3D11 |
| 无设备丢失 | `bingpu_device_lost()` 恒 0；`poll()` 永远 `NONE` |
| 法线不参与 | 网格按"顶点色 × 纹理"着色（没有光源状态，见欠账） |

## Vulkan：第一个真实后端 —— **已落地（0.3.0）**

实现用**动态渲染**（Vulkan 1.3 core，`dynamicRendering` 特性），所以不需要 render pass
对象；管线按 `(布局, 混合, flags, 颜色格式, 深度格式)` 懒建并缓存。五条与 CPU 对照的
验收用例全部 0 差（见 `tests/test_vk_render.c`），唯一带容差的是双线性取样
（CPU 用 double 累加、GPU 用 float 插值，偶发 1 个通道差）。

### 0.13.0：一帧里多目标来回切（**命令缓冲是"先录后执行"的**）

VK 是四个后端里唯一一个把绘制**先录进命令缓冲、帧尾才提交**的（GL 是即时调用、
D3D11 走即时上下文、CPU 是内存里直接算）。凡是有"录的时候能看见、执行的时候已经变了"
的状态，都必须显式处理 —— 0.13.0 修的三处都是这个形状：

1. **顶点/索引流扩容**：扩容当刻，前面那些 draw 已经把旧 buffer 绑进去了。旧 buffer
   **不能当场销毁**（显存会被立刻回收、下一笔分配就拿到同一段），只能"退役"到下趟
   `beginRecording`（上一趟 fence 已等完）再释放。症状极刁：整页渲染一帧几千个四边形，
   一扩容，**扩容之前的内容整块消失**，不报错不崩溃。
2. **描述符**：descriptor set 的内容是**执行期**读的，不是录制期快照。所以不能整个 cmd
   共用一个 set、在两条 draw 之间改写它 —— 那样所有 draw 都采样最后写入的那张纹理。
   现在按笔分配（每池 512 个，帧首整批 `vkResetDescriptorPool` 回收）。
3. **同一目标连开多趟**：动态渲染的 `vkCmdBeginRendering/EndRendering` **不插**依赖，
   而"布局没变"的 barrier 又会被跳过；所以 `begin_render` 对目标**强制**插一条
   颜色附件的 RAW 依赖。采样源则每笔都确保转回 `SHADER_READ_ONLY_OPTIMAL`（布局相同
   时 `transition` 自己跳过，不会白插）。

`tests/gpu_parity.h` 的 `multipass` 用例钉住第 1、3 条（原来 12 个用例每笔只有一两个
四边形，一个都抓不到）。

下面这段是当时的开工计划，留作记录：

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

## D3D11：第二个真后端，也是"异族模型"的代表 —— **已落地（0.4.0）**

实现用的是 **immediate context**：没有命令缓冲、没有 PSO、没有显式同步。`BgpuCmd` 在
VK 里是命令缓冲，在这里退化成**一个状态结构体 + 立即提交**。这正是
[`layering.md`](layering.md) §4 那条推论的实测：

> 异族后端不需要模拟命令缓冲。`BgpuCmd` 只是一个"记录器"抽象。

证据：两个 GPU 后端**共用同一份用例表**（`tests/gpu_parity.h`，12 个用例），D3D11 那边
一行用例都没改就跑出与 CPU **逐像素 0 差**（唯一带容差的是双线性取样，同一份容差）。
`tests/test_d3d11_render.c` 全文只有一行 `main`。

与 VK 后端的实现对照（"同一件事在两种模型里长什么样"）：

| 事 | Vulkan | D3D11 |
|---|---|---|
| 管线状态 | `VkPipeline`（PSO，按 布局×混合×格式 懒建缓存） | 设备上预建 5 个 blend + 4 个 depth + 2 个 rasterizer 状态，draw 时按需绑定 |
| 命令 | `VkCommandBuffer` 记录 → submit → 等 fence | 立即执行；`end_render` 只是清状态 |
| 顶点流 | host-visible buffer + `vkCmdDrawIndexed` | `DEFAULT` buffer + `UpdateSubresource` + `DrawIndexed` |
| 清屏 | `loadOp = CLEAR` | `ClearRenderTargetView`（要先绑 RTV） |
| 深度 | D32 image + `depthCompareOp = LESS` | D32 texture + `DSV` + `D3D11_COMPARISON_LESS` |
| 呈现 | `vkQueuePresentKHR`（FIFO/IMMEDIATE） | `Present(vsync ? 1 : 0, 0)`（blt 模型） |
| 回读 | host buffer + `vkCmdCopyImageToBuffer` | staging texture + `CopyResource` + `Map` |
| 构建依赖 | 头在 `third_party/`，运行期 `LoadLibrary("vulkan-1.dll")` | 头由 MinGW 自带，运行期 `LoadLibrary("d3d11.dll"/"dxgi.dll")` |
| 着色器 | `glslc` → SPIR-V 数组 | `fxc` → DXBC 数组 |
| 借出的帧目标 | 手动 `own_image=false` 防二次销毁 | COM 引用计数天然安全（`share()`） |

两个容易踩的 D3D11 细节（都留了注释）：

1. **NDC 的 y 向上**（Vulkan 向下）：`quad.hlsl` 里 `ndc.y = 1 - 2*s.y/h`；正面判定跟着翻，
   所以 rasterizer 的 `FrontCounterClockwise = FALSE` —— 语义仍然是"TL,TR,BL 算正面"。
2. **`ScissorEnable` 必须在 rasterizer state 里打开**，否则 `RSSetScissorRects` 静默无效；
   另外 `ResizeBuffers` 之前必须放掉后台缓冲/RTV 的引用，否则就是 binUI 当年那个
   `0x887A0001`。

**交换链尺寸听窗口的**（两个真后端一致）：窗口模式下 `resize(w, h)` 的请求值只在
"取不到客户区"时兜底 —— VK 那边 `VK_KHR_surface` 的 `currentExtent` 本来就非
`0xFFFFFFFF`（surface 拥有 extent），D3D11 这边显式 `GetClientRect` 对齐同一语义。
理由：尺寸不同的后台缓冲会被 DXGI 拉伸上屏、被 VK 直接忽略，两个后端就会给出不同的
`swapchain_info`。想低分辨率渲染再放大，就把离屏目标当画布（`contracts.md` §2 已经禁止
交换链局部重绘，那条路本来也不该走）。

D3D11 这一版**没有 MSAA / mip**（这一版明确不做，见"质量档：MSAA 与真 mip"一节的
D3D11 段）：`caps.msaa_samples` 报 1、没有 `BGPU_CAP_MSAA` / `BGPU_CAP_MIPMAPS`，
`desc.msaa_samples = 4/8` 与 `BGPU_TEXTURE_MIPMAPS` 都按 0.13.0 的行为降级。
原因不是技术不可行，而是"这一版先保证默认路径零风险"：D3D11 要补的正是交换链
resolve 那条最容易出窗口级 bug 的路，且本机 VK/GL 已经能提供这两档。

## OpenGL：第三个真后端，异族里最极端的那个 —— **已落地（0.5.0）**

GL 没有命令缓冲、没有 PSO、资源是**绑定点上的全局状态**，深度附件甚至不是对象而是
framebuffer 的一个挂点。按 [`layering.md`](layering.md) §4 的推论，这种模型下 `BgpuCmd`
同样只需要退化成"当前状态 + 立即提交" —— 实测：`tests/gpu_parity.h` 的 12 个用例**一个字
没改**，GL 与 CPU 逐像素 0 差，而且与 VK/D3D11 的差异数字完全同一组（双线性那 498 个像素、
最大通道差 1）。

与另外两个后端的实现对照（同一件事在三种模型里长什么样）：

| 事 | Vulkan | D3D11 | OpenGL |
|---|---|---|---|
| 上下文 | `VkInstance`/`VkDevice` | `ID3D11Device`（immediate） | WGL：隐藏窗口 → 1.1 上下文 → `wglCreateContextAttribsARB` 得 3.3 core |
| 呈现 | `vkQueuePresentKHR` | `Present(vsync?1:0, 0)` | `SwapBuffers(dc)` + `wglSwapIntervalEXT` |
| 离屏 | 自有 image + 视图 | 自有 texture + RTV | 自有 texture 挂 FBO（+ depth renderbuffer） |
| 深度清除 | `loadOp = CLEAR` | `ClearDepthStencilView` | `glClear(DEPTH_BUFFER_BIT)`（窗口目标走像素格式的 24 位深度） |
| 回读 | `vkCmdCopyImageToBuffer` | staging + `CopyResource` | `glReadPixels`（窗口要翻行） |
| 着色器 | SPIR-V（glslc 离线） | DXBC（fxc 离线） | **GLSL 源码字符串，运行时由驱动编译**（不需要离线工具） |
| 构建依赖 | 头在 `third_party/`，`LoadLibrary` | MinGW 自带头，`LoadLibrary` | **完全不 include GL 头**，类型/常量自带，`LoadLibrary` |

**回读有两条路径，验收必须两条都覆盖**（0.5.1 的教训）：`texture_read` 读的是"离屏目标
纹理"，`frame_read` 读的是"交换链借出的帧目标"，在后端里是两段完全独立的实现
（D3D11 = staging copy；GL = FBO 回读 / 默认帧缓冲翻行；VK = 拷回读缓冲）。
`tests/gpu_parity.h` 现在两条都有用例（`texture(box)` 与 `swapchain(...)`）。

两个 GL 特有的坑（都留了注释，也都写进了 CHANGELOG）：

1. **`GL_SCISSOR_TEST` 的裁剪框默认值是"建上下文那个窗口"的大小**。离屏设备上那是个 8×8
   的隐藏窗口 —— 不每趟 `glScissor(0,0,w,h)` 的话 `glClear` 只清左上角 8×8。
2. **窗口原点在左下**：FBO 上"存储行 0 = 图像顶行"所以顶点着色器不翻、读回也不翻；
   默认帧缓冲上要翻顶点（`uFlip=1`）且读回翻行、裁剪矩形镜像成 `h - y - hh`。

## 质量档：MSAA 与真 mip（下一版；默认路径一个字没变）

> 对应代码：`include/bingpu.h` 的 `BGPU_CAP_MIPMAPS` / `caps.msaa_samples` /
> `BgpuDeviceDesc.msaa_samples`；VK 在 `src/bingpu_vk.cpp`，GL 在 `src/bingpu_gl.cpp`。
> 版本号与 ABI 都没动（ABI 仍是 4）：这一版只用了既有字段 + `caps.flags` 的新位。

### 合同怎么改（唯一的措辞变化）

以前是"四端与 CPU 参考后端逐像素相同"。CPU 永远只有 1×/1 级，所以一旦开了 MSAA / mip，
那句话就不再是同一个命题。**新合同**：

> **同档设置下与 CPU 参考后端逐像素相同；高档设置（MSAA > 1、mip 链）是显式选择的
> 质量档，不做逐像素承诺。**

默认档（`msaa_samples` 为 0/1、纹理不带 `BGPU_TEXTURE_MIPMAPS`）**一个像素都没变**：
`tests/` 的 16 条跨端 parity 用例在改动前后逐位相同（含唯一带容差的
`texture(linear/clamp)`：498 个像素 / 最大通道差 1）。高档的设置是**增量能力**，不是
默认路径的重写。

### 能力怎么问（不要靠猜）

| 问题 | 答案 |
|---|---|
| 这台设备最高能给几×？ | `BGPU_CAP_MSAA` 位 + `caps.msaa_samples`（1/4/8；1 = 不支持） |
| 这个交换链实际开了几×？ | `bingpu_swapchain_info().msaa_samples` |
| 这张纹理实际有几级 mip？ | `bingpu_texture_info().mip_levels` |
| 这个后端支持真 mip 吗？ | `BGPU_CAP_MIPMAPS` 位（没有这一位的后端把 `MIPMAPS` 接受后降级为 1 级） |

本机（RTX 4060 Ti / 驱动 610.x）实测：**VK 与 GL 的 `caps.msaa_samples` 都是 8**，
两个后端都带 `MSAA` + `MIPMAPS` 位；D3D11 / CPU / NULL 仍是 1、没有这两个位。

### 档位由谁定：`bingpu_render_plan` 按后端给（2026-11-20）

**"开不开 MSAA"收在设备层**，不在调用方各写一份（以前 binUI 的 `binui_gpu` 与 bin2d 的
`GpuExec` 各有一颗"只有 VK 放开 4×"的钉子，漏抄一次就是画质悄悄停在 1×）。
`bingpu_render_plan()` 返回的 `msaa_samples` 就是**该后端该给的默认档**：

| 后端 | `plan.msaa_samples` | 理由 |
|---|---|---|
| `BGPU_BACKEND_VULKAN` | **4** | 18 页对 `binUI/docs/pixels` 基线 0 像素差异（最大通道差 ≤7），多目标 MSAA 与真 mip 都在 0.15.0 修通 |
| `BGPU_BACKEND_GL` | **1** | **窗口**路径 4× 还会丢内容，见下面欠账 12（照搬 VK 的延迟释放无改善）；headless 用例在 GL 上 4× 是逐像素对的，等窗口用例补上再放开 |
| `BGPU_BACKEND_D3D11` | 1 | D3D11 后端的 MSAA 还没做（欠账 3），caps 也报 1，不谎报 |
| `BGPU_BACKEND_CPU` / `NULL` / 一个都不可用 | 1 | 参考实现恒为 1 |

调用方**直接采信**这一项（`dd.msaa_samples = plan.msaa_samples`），想要别的档再在
`BgpuDeviceDesc` 里显式指定；实际生效值一律读 `bingpu_swapchain_info().msaa_samples`。
行为变化不需要改 ABI：结构体布局没动。

### VK：多采样附件 + pass 末 resolve；mip 逐级 blit

- **MSAA**：设备档 `d.msaa`（按 `desc.msaa_samples` 与
  `framebufferColorSampleCounts & framebufferDepthSampleCounts` 取交集降级）。所有**颜色
  渲染目标**（画布、图层、离屏）在第一次当目标时懒建一个同尺寸/同格式的 N 采样
  `VkImage` companion；`vkCmdBeginRendering` 的颜色附件指向它，
  `resolveMode = AVERAGE`、`resolveImageView = 目标的单采样 image`，`vkCmdEndRendering`
  时自动 resolve。于是：
  - `bingpu_frame_read` / `bingpu_texture_read` / 把该纹理当采样源，拿到的**永远是
    单采样结果**（resolve 之后的 image）；
  - 多个颜色目标（bin2d 的 layer / blur 那些 secondary target）**同样走这条路**，不存在
    "只有画布有 MSAA、别的目标没有"的半截状态；
  - 深度图在 `prepareDepth` 里按**同一个采样数**建（附件采样数不一致会被验证层/驱动拒）；
  - 管线缓存键加上了采样数（`VkPipelineMultisampleStateCreateInfo.rasterizationSamples`
    必须等于附件采样数）。
  - `begin_render` 不带 `CLEAR_COLOR`（LOAD）时，先把单采样 image blit 回多采样附件
    —— 这样"在 pass 之外改过这张图"（`texture_upload`、`clear_color`、上一帧的 resolve）
    的内容也能被 LOAD 接上；多采样附件本身是 `storeOp = STORE` 的，上一趟的样本也还在，
    两条路给的结果一致。companion 因此必须带 `TRANSFER_DST`（它会被当 blit 目标）。
    > 0.16（ABI 6）：companion 里**已经是这张图的最新内容**时（`VkTextureObj::msaa_valid`，
    > 上一趟刚画完 / 刚 resolve）这次回灌**直接跳过**，接着在 companion 上画；只有
    > "pass 之外改过 1×"（`texture_upload` / 换交换链图 / 刚建的 companion）才回灌。
    > 这是纯多余的全窗往返，实测在本机 svg-filter 页占 0.4~0.5 ms/帧（见下面的
    > "延迟 resolve"）。
    > 规范上 `vkCmdBlitImage` 写"多采样目标"是比较敏感的一条（原来的 companion 连
    > `TRANSFER_DST` 都没有）。本机没装 Vulkan SDK / 验证层，没法用 VUID 复核；实测
    > 在 4060 Ti / 610.88 上两趟 LOAD 与"帧中多目标"用例都正确。若要更保守，可以改成
    > 用一笔覆盖整屏的 draw 预置多采样附件（多一次 pass），但那要额外一张临时图。
  - **帧中途销毁目标**：命令缓冲是先录后执行，`destroy_texture` / `destroy_buffer` 在
    "还有命令缓冲在录制"时只把句柄挂进设备的退役表，`submitNow` 等完 fence 之后才真正
    `vkDestroyImage` / `vkFreeMemory`。不这么做的话，录进去的 draw/barrier 会去读一块
    已经还给驱动的显存 —— 1× 时靠同尺寸复用侥幸不炸，4× 时连 companion 一起还回去，
    驱动直接 `VK_ERROR_DEVICE_LOST`（0.15.0 修的正是这条，见欠账 11）。
- **mip**：`BGPU_TEXTURE_MIPMAPS` → `mipLevels = floor(log2(max(w,h)))+1`（上限 16），
  image/view 的 `levelCount` 同步；上传 level 0 之后用 `vkCmdBlitImage`
  （`VK_FILTER_LINEAR`）逐级降采样到 1×1，每一级单独换布局（整张图的 barrier 表达不了
  "0 级已 TRANSFER_SRC、末级仍 TRANSFER_DST"）。采样器 `mipmapMode = LINEAR`
  （`min_filter = NEAREST` 时是 NEAREST），**`maxLod = VK_LOD_CLAMP_NONE`** ——
  零初始化的 `VkSamplerCreateInfo.maxLod` 是 0，不显式设就永远只取 0 级。
- 代价：4× 的颜色附件是单采样的 4 倍显存 + 一次 resolve 带宽；mip 链额外 ~1/3 显存
  （等比级数 1/4+1/16+…）。默认档这些开销**一项都不发生**。本机 VK 实测
  （`bingpu_bench --backend vulkan --msaa 1|4`，1024² 离屏 pass）：
  纯色 / 纹理铺满 **0.070 → 0.150 ms**（约 2.1×，fill-bound）；⑤ 离屏 + 合成（含
  present 拷贝）0.39 → 0.45 ms（+15%）；⑥ `texture_read` 9.3 → 9.7 ms（噪声级）。
  也就是说 MSAA 的代价集中在"被覆盖的像素"上，读回与呈现几乎不变。

### GL：multisample renderbuffer + `glBlitFramebuffer` resolve；`glGenerateMipmap`

- **MSAA**：`GL_MAX_SAMPLES` 与两个入口（`glRenderbufferStorageMultisample` /
  `glBlitFramebuffer`）都在才报 `BGPU_CAP_MSAA`。每个目标懒建
  `GL_RGBA8` 多采样 renderbuffer + 它自己的 FBO（深度用同样采样数的
  `GL_DEPTH_COMPONENT24` renderbuffer），`begin_render` 画进去，`end_render` 用
  `glBlitFramebuffer(COLOR_BUFFER_BIT)` resolve 回目标 FBO（窗口模式 resolve 到默认帧缓冲）。
  两个 FBO 都是 GL 窗口坐标（左下原点），所以 blit 的 rect 一对一、不翻 y；
  `uFlip` / scissor 的既有规则不变。
- **mip**：建纹理时按级 `glTexImage2D`，`GL_TEXTURE_MAX_LEVEL = levels-1`，上传后
  `glGenerateMipmap`。这里有一个 GL 特有的坑：**纹理完整性是按采样器判的** ——
  mipmap min filter 的采样器配 1 级纹理会不完整（采出来全黑）。所以 `createSampler`
  为每个采样器建两个 GL 对象（plain / mipmap），`bindTexture` 按目标纹理有没有链选一个；
  默认采样器同理（`def_samp` / `def_samp_mip`）。
- **延迟 resolve（0.16.0 / ABI 6，与 VK 同一套语义）**：GL 的 msaa FBO 内容在 pass
  之间**天然保留**，所以没有 VK 那条 1×→4× 回灌；能推迟的只有 pass 末那次
  `glBlitFramebuffer`。DEFER 的 pass（或自动策略：整窗目标、一帧第 2 趟起）只把
  `GlTextureObj::msaa_pending` 置上，等被采样（`ensureSampleResolved`）、回读
  （`texture_read` / `frame_read`）、呈现（`presentFrame`）或显式
  `bingpu_cmd_resolve_target` 时再 blit（GL 是即时执行，补完把 FBO 绑回当前目标即可）。
  **注意**：GL 的 4× **窗口**路径仍受欠账 12 限制（`render_plan` 对 GL 只给 1×），
  这条延迟机制只是让"显式要 4× 的调用方"也拿到同样的语义，不改变欠账 12 的判断。
- 代价与 VK 同量级；默认档走的是原来的单采样 FBO / 单级 `glTexImage2D`，没有额外对象。

### 延迟 resolve（0.16.0 / ABI 6）：消掉"每进出一次目标就 1×→4× blit + resolve"的往返

**症状**：binUI 的 `svg-filter` 页（660×470 窗口，14 个图层 + 模糊链）在 4× 下
3.3~3.4 ms。命令流里**同一个整窗目标**（bin2D 的画布）一帧被 begin_render 了
**33 趟**：每离开一次（进图层）pass 末 resolve 一次 4×→1×，每回来一次（LOAD）又
要 1×→4× 回灌一次。两趟都是全窗读+写，而"中间那几趟图层绘制根本不采画布"。

**做法**（`BGPU_RENDER_DEFER_RESOLVE` + 自动策略 + 自愈）：

- 标了 DEFER 的 pass（或自动策略命中的 pass）在 `vkCmdEndRendering` 上**不带**
  `resolveMode`：只画进 4× companion、1× 镜像暂时落后（`resolve_pending`）。
- 下一趟 LOAD 回到同一个目标：companion 本来就有最新内容 ⇒ **不回灌**（`msaa_valid`），
  接着画。
- **自愈**：谁要那张 1×，谁在边界上补一次 resolve —— 采样（`rasterQuads` /
  `rasterIndexed`，必要时把当前渲染实例 endRendering → resolve → 原样 LOAD 开回去）、
  `texture_read`、`frame_read`/`frame_end`、`texture_upload`（先 resolve 再叠上传）、
  以及显式 `bingpu_cmd_resolve_target`。上层**不需要**记住哪趟 defer 了哪张图；
  忘了也只是"晚一次"，不会采到旧内容。
- **自动策略**：只对**整窗尺寸**的目标、且是这一帧里第 2 趟起 defer。层 / blur 目标
  按元素尺寸分配（bin2D 0.22.8），它们刚画完就立刻被父级合成采样 —— defer 一定紧接
  自愈，白拆一次 pass；把策略一直开到这些目标上实测**净收益为 0**（自愈的
  suspend/resume 抵消了省下的 resolve）。`BINGPU_NO_AUTO_DEFER=1` 可关掉自动策略做 A/B。
- **诊断**：`BINGPU_TRACE_RESOLVE=1` 每帧打一行 `blits=…… resolves=……`（次数 + 像素量
  + 带宽估算），自愈时另打 `self-heal WxH`；`BINGPU_TRACE_PASSES=1` 逐 pass 打
  `WxH CLEAR/LOAD msaa=N`（用来核实"一帧到底有几个整窗往返"）。

**实测（本机 4060 Ti / 610.88，`svg-filter` 660×470 窗口，`probe_frame` 100 帧中位）**：

| 版本 | 每帧整窗 LOAD（回灌） | 每帧 resolve（整窗/全部） | 帧时 |
|---|---|---|---|
| 0.15.1（blit + 每趟 resolve） | 32 | 33 / 99 | 3.30~3.42 ms |
| 只跳过 LOAD 回灌 | 0（`msaa_valid` 命中后） | 33 / 99 | 2.93~2.97 ms |
| 0.16.0（回灌 + 延迟 resolve） | 0 | 1 / 69 | **2.69~2.71 ms** |

A/B 分账（同一份代码、只关其中一项）：LOAD 回灌 ≈ **0.50 ms/帧**、全部 resolve
≈ **0.53 ms/帧**、两样都关 ≈ **0.94 ms/帧**（那是不可达上限：层/blur 目标必须
resolve 才能被合成采样）。4× 剩下的 ~0.6 ms 是 4× 的 store/load 与填充率，不在
这条链上 —— 所以"把往返消干净"的上限是 3.35 → 2.5 ms 左右，而不是 1×（1.88 ms）。
像素不变：`pixels-bin2d-gpu` 18 页数字与改动前**逐页相同**（`svg-filter` 0.000% / max 6，
`grad-angle` 0.028% / max 21，`texture` 页 0/1 ...），`pixels-bin2d`（0 容差）与
binGPU 的 `test` / `abi-check` / `smoke` 全绿。

### 测试用什么钉（`tests/gpu_parity.h`，只在能力可达时跑）

| 用例 | 判据（本机实测） |
|---|---|
| `msaa(斜边覆盖率)` | 一条斜边：1× 的 `0<v<255` 中间色像素 **0** 个，4× **72** 个；`frame_read` 路径同样 **72**；两趟 LOAD 的第一趟内容保留=1；MSAA 下 `depth(LESS)` 中心仍 R=255/B=0 |
| `msaa 帧中多目标：画布 → 离屏抓取 → 合成回画布` | 照 binUI `blurLayerContent` 的命令顺序：画布 → 离屏抓 A（抓背景）→ A 第二趟 LOAD → 合成回画布。1× 与 4× **逐像素 0 差异**；4× 下红/绿（抓到的内容）与蓝（第二趟）都在（VK/GL 都是 0/32768） |
| `msaa 帧中多目标：同一目标两趟 + 中途当采样源` | A 上两趟（第二趟 LOAD），中途把 A 当采样源一次；1× vs 4× **0 差异**（0/8192） |
| `msaa 帧中销毁目标` | 一帧里 12 轮"建目标 → 画 → **帧中途销毁**（它还被缓冲引用着）→ 建同尺寸新目标 → 合成"：销毁版 vs 不销毁版 **0 差异**（1× 与 4× 都要）。钉的是欠账 11 那条根因的**合同**（销毁不得影响本帧像素）。注意：headless 下这条在没修的 0.14.1 上也过 —— 驱动在这个尺寸上没炸；真正炸的是 binUI 的 660×470 窗口帧，所以现场判据还是 `make pixels-gpu` 的 `svg-filter` |
| `mip(缩小的棋盘格)` | 256×256 / 2px 棋盘缩小到 60×60：1×（`mip_levels=1`）均值 127.46、**平均绝对偏差 74.11**（摩尔纹）；mip（`mip_levels=9`）均值 127.00、**偏差 0.00**（纯灰） |

前四条 VK 与 GL 给出**同一组数字**（帧中多目标那三条都是 0 差异）。

拿不到能力时两条都打印 `[skip] ...` 并返回，不 fail（D3D11 / CPU / NULL 上就是这样）。

### D3D11：这一版明确不做

判断与代价（不是"忘了"）：

- **MSAA**：要新增每目标的多采样 texture + `ResolveSubresource`，交换链还要单独走
  "多采样离屏 → resolve 到 back buffer"；`prepareDepth` 的 DSV 采样数、RTV/DSV 绑定、
  staging 回读全要跟着改。约 250~350 行，且 swapchain resolve 与 present 的交互是
  最容易出"窗口上一帧黑/一帧旧"的地方，必须开真窗口烟测 + 跨端数值门禁才能收。
- **mip**：要建完整 `D3D11_SUBRESOURCE_DATA` / `GenerateMips`、SRV 的 `MipLevels`，
  并把 `D3D11_SAMPLER_DESC.MaxLOD` 从 0 改到 `FLT_MAX`（现在的 0 会让 mip 永远取 0 级）。
  约 100 行，风险比 MSAA 低。
- 为什么先不做：这一版的目标是"VK 先行、GL 顺手、默认路径零风险"。D3D11 的
  `caps.msaa_samples` 继续报 1、没有 `BGPU_CAP_MSAA` / `BGPU_CAP_MIPMAPS`，上层按能力
  选择时会自动留在默认档；不存在"以为开了其实没开"的谎报。要补的时候按 VK 的同一份
  判据（MSAA 中间色 / mip 摩尔纹）验收即可，不需要改 ABI。

## D3D12：`layering.md` 里已排到 VK/D3D11/GL 之后

0.2.2 起排期调整（理由见 [`layering.md`](layering.md) §5）：**D3D11 提到 D3D12 之前**。
原来的理由是"D3D12 显式程度最高，最能验证状态机"，但那条价值已经由 VK 拿到（两者同族：
PSO + 命令缓冲 + 显式同步），D3D12 再写一遍学不到新东西，却要重写约 2416 行。而 **D3D11
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

## 性能：两条被基准抓出来的路径（0.5.2）

这两条都不是"设计问题"，是**实现选错路**，而且都不是 binGPU 自己的测试能发现的：
功能测试只问"像素对不对"，问不出"每帧上传 56 MB 要 5.9 秒"。发现它们的是上层
（`../bin2D/tools/bin2d_bench_gpu.cpp`：CPU 合成 / 每帧上传 / 纹理常驻 三条路线对比）。

| | 症状 | 根因 | 修法 | 前后 |
|---|---|---|---|---|
| D3D11 纹理上传 | 56 MB/帧要 **5942 ms**（VK 115 ms、GL 106 ms） | 每次上传新建一张 STAGING 纹理，再 `CopySubresourceRegion` 到 DEFAULT —— STAGING→DEFAULT 是驱动里最慢的同步拷贝 | 普通内存里排好行并预乘（thread_local 缓冲），一次 `UpdateSubresource` | **59×**（5942 → 100 ms） |
| `wait_idle()` | 基准量到 0.011 ms"画完" 16 个全屏四边形 | `bingpu_api.cpp` 里直接 `return BGPU_OK`，注释说"真实后端在这里等队列" —— 三个真后端一个都没实现 | VK `vkDeviceWaitIdle` / D3D11 `QUERY_EVENT`+`Flush`+轮询 / GL `glFinish`（切 context 再恢复） | 0.011 → **0.21 ms**（真实时长） |

教训记在这里：**"等 GPU 画完"是 API 语义，不是可选优化**。它一旦说谎，所有基于时间的
结论（基准、帧预算、截图）全部无效，而且不会红任何测试。所以现在这条也有断言：
`bin2d` 的基准用 `waitIdle()` 之后拿到的是 0.2 ms/帧，而不是 0.011 ms。

## 欠账（明确不做，记在这里）

1. **用户着色器 / SPIR-V**：bin3d 进 GPU 阶段的前提。注意：**不是"给上层开着色器"** ——
   bin3d 要的是光照状态（`docs/layering.md` §6 与 `../bin3D/docs/layering.md`）。
2. **深度纹理格式**：现在用 `RENDER_TARGET` 纹理当深度目标（后端自己解释，VK 建 D32
   image、D3D11 建 D32 texture + DSV）。建议将来加 `BGPU_TEXTURE_DEPTH` 或
   `BGPU_FORMAT_D32F`。
3. **D3D11 的 MSAA**：VK 与 GL 已落地（见"质量档"一节）。D3D11 还欠"每目标多采样
   texture + `ResolveSubresource` + 交换链 resolve"这一整套，代价与风险写在同一节；
   这一版**明确不做**，caps 继续报 1，不存在谎报。
4. **网格光照**：`BgpuMeshVertex` 带法线，但没有光源/材质状态可以喂给它。
   等 bin3d 定义光照模型后再加（那是 ABI 变化）。
5. **多帧飞行**：`max_frames_in_flight` 恒 1。VK 是"每帧 submit + 等 fence"，D3D11 是
   "立即执行 + Present 后不等 GPU"，两者都是**先正确、再重叠**。
6. **透明窗口 / 分层呈现**：`BGPU_DEVICE_TRANSPARENT` 在两个真后端都返回 `UNSUPPORTED`。
   **present 策略本身要升格成 ABI 的一等公民**（交换链 / 读回贴屏 / 分层窗三态 + 守卫判定），
   提案与理由见 [`layering.md`](layering.md) §6.3 —— binUI 三端各写了一遍，VK 那份在
   "窗口铺满显示器"时出过 bug（v0.19.1）。
7. **事件队列**：`poll()` 现在恒 `NONE`；两个真后端都只**记** `lost` 标志（VK 的
   `VK_ERROR_DEVICE_LOST`、D3D11 的 `DXGI_ERROR_DEVICE_REMOVED`），还没接到事件队列。
8. **R8 遮罩的采样广播**：CPU 把 R8 采样当作 `(v,v,v,v)`，两个 GPU 后端拿到的是
   `(v,0,0,1)`（D3D11 那边是因为 MinGW 的 `D3D11_SHADER_RESOURCE_VIEW_DESC` 没有
   `Shader4ComponentMapping` 字段）。**两个 GPU 后端之间是一致的**，只是与 CPU 在
   "拿 R8 当彩色纹理采样"时不同 —— 目前没有任何调用方这么用（R8 是遮罩，走的是
   `BGPU_BLEND_KEEP/ERASE` 那条路）。要修就得在 ABI 层支持纹理视图映射，和 9 一起做。
9. **mip 链**：VK 与 GL 已实现真链（见"质量档"一节）；CPU（参考实现，LOD 恒 0）与
   D3D11（这一版明确不做）仍**接受 `BGPU_TEXTURE_MIPMAPS` 但降级为 1 级**，
   `mip_levels` 如实上报 1。设备层的"做不到"必须表现成"降级 + 能力位缺失"，
   不是"建不出来" —— 这条仍然成立：上层问 `BGPU_CAP_MIPMAPS` 就能分流，不需要 try/except。
10. **一个 HWND 只能绑一个 GL 设备**：WGL 的上下文绑在 DC 上，`contracts.md` §4 本来就规定
   "一个 native_window 同一时刻只能绑一个呈现后端"。binUI 的多面板要共用一个设备，不是多个。
11. ~~**MSAA 在"多目标 / 图层"路径上还会丢内容**（0.14.1 记录）~~ → **0.15.0 已修**。
    症状：binUI 的 `svg-filter` 页（图层 + 毛玻璃 + 多次换渲染目标 + 合成）在 4× 下
    **整页黑** —— 对基线 100% 差异、最大通道差 255；同一页 1× 下 0 差异。VK 的
    `vkDeviceWaitIdle` 返回 `VK_ERROR_DEVICE_LOST`（Windows 事件日志里有对应的
    `nvlddmkm` 153），也就是**整帧命令被驱动丢掉了**，不是"画错了"。
    **根因不在 resolve，也不在采样目标**（当初的记录猜错了两处，一并记下来免得以后
    再往那个方向找）：调用方在**帧中途销毁了一张还在命令缓冲里的渲染目标**
    （binUI `ensureBlurLevel` 换尺寸时 `freeTex(旧 level7)` → 立刻建同尺寸新图）。
    命令缓冲是先录后执行，录进去的 `vkCmdBeginRendering` / barrier 引用的是那张图的
    `VkImage` 及其 MSAA companion；旧版析构里直接 `vkDestroyImage` + `vkFreeMemory`，
    1× 时那块显存被同尺寸新图复用（读到"错的但有效"的数据，画面看不太出来），4× 时
    companion 也一起还回去，GPU 当场页错误，整帧全丢 ⇒ 整页黑。
    **修法**（与 0.13.0 "顶点流扩容时旧 buffer 不能当场销毁"同一条规矩）：`VkTextureObj`
    / `VkBufferObj` 析构时若设备还有命令缓冲在录制（`recording_cbs > 0`），只把
    image/view/memory 句柄挂进设备的"退役表"，等本帧 `vkCmdEndCommandBuffer` + 提交 +
    等完 fence（`submitNow`）之后再真正释放。同时给 MSAA companion 补上
    `TRANSFER_SRC | TRANSFER_DST` 用途位（它本来就会被当 blit 目标用）。
    实测（`make pixels-gpu`，18 页对 `docs/pixels` 基线、0.5% 容差）：VK **18 页全部
    0 像素差异**（最大通道差 ≤7；`svg-filter` 从 100% → 0、`image-round` 0.217% → 0、
    `image-mip` 0.118% → 0）。帧时（`probe_cost`，窗口路径、60 帧中位）：1× 与 4× 都在
    1.06–1.16 ms/帧，差在噪声内。
    **GL 后端还有同一形状的欠账**（记在下面第 12 条），所以 binUI 的 `binui_gpu` 只对
    VK 放开 4×，GL 继续钉 1×。
12. **GL 后端的 MSAA 在窗口路径上还会丢内容**（0.15.0 记录，未修）：`binGPU` 的 headless
    用例（`tests/gpu_parity.h`，含帧中多目标 / 两趟 + 中途采样 / 帧中销毁目标）在 GL 上
    与 VK 逐像素一致（0 差异），但 binUI 的窗口路径在 4× 下与基线还对不上 ——
    `circle` 94.6%、`shadow` 95.9%、`svg-filter` 9.1%（同一页 1× 是 0）。
    VK 已修的"帧中途销毁"在 GL 上做同样处理（延迟删除）**没有改善**，说明是 GL
    multisample FBO / `glBlitFramebuffer` resolve 这条链自己的问题（多目标 + 窗口默认
    帧缓冲的组合）。修它要先补一条**窗口路径**的 GL 用例（现在的 headless 用例覆盖不到），
    再按 VK 的同一份判据验收。
13. **D3D11 的延迟 resolve**：这一版**明确不做**。D3D11 没有 MSAA（欠账 3），
    `BGPU_RENDER_DEFER_RESOLVE` 对它是 no-op，`bingpu_cmd_resolve_target` 走
    `Backend::resolveTarget` 的默认实现（直接 `BGPU_OK`）—— "没有多采样就没有挂起的
    resolve"，语义上不是漏做。等补 MSAA 时按 VK 的同一套边界（`endRender` 不 resolve、
    采样/回读/上传/frame_end 前自愈）一起补，验收判据也用同一份。

