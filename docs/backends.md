# 后端（backends）

> 排期与验收口径的总设计见 [`layering.md`](layering.md)：这一层收什么、四家怎么分族、
> 每个后端的"通过标准 + 完成后删掉哪份旧实现"都在那里。本文讲**每个后端的具体情况**。

## 现状表

| 后端 | 状态（类） | 能做什么 | 不能做什么 |
|---|---|---|---|
| `NULL` | **已实现**：`NullBackend` | 完整状态机、参数/句柄校验、绘制计数 | 不产生像素；回读返回 `UNSUPPORTED` |
| `CPU` | **已实现**：`CpuBackend` | 参考像素实现；离屏目标；深度；采样器；仿射；网格；回读 | 不能开窗口（`UNSUPPORTED`）；无 MSAA；无 mip 采样 |
| `VULKAN` | **已实现（0.3.0）**：`VkBackend` | 设备 / 交换链 / present / 纹理上传回读 / 缓冲 / 采样器 / 五种混合 / 离屏 + 深度 / 网格（`draw_indexed`）/ 仿射 | 无 MSAA（与 CPU 一致，`msaa_samples == 1`）；无 mip；无透明窗口；单帧飞行；要求 Vulkan 1.3（动态渲染） |
| `D3D11` | **已实现（0.4.0）**：`D3d11Backend` | 同上，走**即时上下文**（无命令缓冲 / 无 PSO） | 无 MSAA；无 mip；无透明窗口；单帧飞行；无 Deferred Context（不需要） |
| `D3D12` | 登记，未实现 | — | 同上 |
| `GL` | **已实现（0.5.0）**：`GlBackend` | 同上，WGL + OpenGL 3.3 core（全局绑定点状态） | 无 MSAA；无 mip；无透明窗口；单帧飞行；一个 HWND 只能一个 GL 设备 |

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
| 无 MSAA | `caps.msaa_samples == 1`；`desc.msaa_samples` 接受 4/8 但**降级为 1**（info 上报真实值） |
| 无 mip 采样 | 固定管线没有 LOD 输入，采样永远取第 0 级；`mip_levels` 接受后降级为 1 |
| 无窗口 | CPU 不碰 HWND/DPI/合成器（那些是"不可复现"的来源）；窗口呈现交给 VK / D3D11 |
| 无设备丢失 | `bingpu_device_lost()` 恒 0；`poll()` 永远 `NONE` |
| 法线不参与 | 网格按"顶点色 × 纹理"着色（没有光源状态，见欠账） |

## Vulkan：第一个真实后端 —— **已落地（0.3.0）**

实现用**动态渲染**（Vulkan 1.3 core，`dynamicRendering` 特性），所以不需要 render pass
对象；管线按 `(布局, 混合, flags, 颜色格式, 深度格式)` 懒建并缓存。五条与 CPU 对照的
验收用例全部 0 差（见 `tests/test_vk_render.c`），唯一带容差的是双线性取样
（CPU 用 double 累加、GPU 用 float 插值，偶发 1 个通道差）。

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

两个 GL 特有的坑（都留了注释，也都写进了 CHANGELOG）：

1. **`GL_SCISSOR_TEST` 的裁剪框默认值是"建上下文那个窗口"的大小**。离屏设备上那是个 8×8
   的隐藏窗口 —— 不每趟 `glScissor(0,0,w,h)` 的话 `glClear` 只清左上角 8×8。
2. **窗口原点在左下**：FBO 上"存储行 0 = 图像顶行"所以顶点着色器不翻、读回也不翻；
   默认帧缓冲上要翻顶点（`uFlip=1`）且读回翻行、裁剪矩形镜像成 `h - y - hh`。

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

## 欠账（明确不做，记在这里）

1. **用户着色器 / SPIR-V**：bin3d 进 GPU 阶段的前提。注意：**不是"给上层开着色器"** ——
   bin3d 要的是光照状态（`docs/layering.md` §6 与 `../bin3D/docs/layering.md`）。
2. **深度纹理格式**：现在用 `RENDER_TARGET` 纹理当深度目标（后端自己解释，VK 建 D32
   image、D3D11 建 D32 texture + DSV）。建议将来加 `BGPU_TEXTURE_DEPTH` 或
   `BGPU_FORMAT_D32F`。
3. **MSAA**：`desc.msaa_samples` 已预留，两个真后端都报 1。要让 MSAA 进来，先解决
   "**MSAA 会让与 CPU 逐像素一致变成边界容差**"这个问题 —— 见 [`layering.md`](layering.md) §2 的 P1。
   目前的裁决是 (b) 容差表。
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
9. **mip 链**：VK 直接 `UNSUPPORTED`，D3D11/GL 接受 `BGPU_TEXTURE_MIPMAPS` 但降级为 1 级
   （与 CPU 一致）。真要 mip 就得给固定管线加 LOD 输入 —— 那是"再加一个原语"，不划算。
10. **一个 HWND 只能绑一个 GL 设备**：WGL 的上下文绑在 DC 上，`contracts.md` §4 本来就规定
   "一个 native_window 同一时刻只能绑一个呈现后端"。binUI 的多面板要共用一个设备，不是多个。

