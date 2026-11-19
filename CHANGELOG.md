# 变更记录

格式照 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)；版本号遵循
[语义化版本](https://semver.org/lang/zh-CN/)。`ABI` 指的是 `BINGPU_ABI_VERSION`
（结构体布局 / 函数签名 / 枚举值一变就 +1，与语义化版本无关）。

## [0.6.0] - 2026-11-19

**ABI 3：固定功能光照 + 透视校正**。这两条是 bin3d 的 GPU 阶段能不能开的前提，
所以连同"为什么之前做不了"一起记在这里。

### Added

- **`BgpuMeshVertex` 加 `float w`**（36 → 40 字节）：裁剪空间的**原始 w**。顶点位置
  送进来时已经除过 w，属性如果按屏幕空间线性插值，"近处三角形被拉长、UV 却按屏幕
  比例走"—— 纹理在斜面上会明显滑动。带下原始 w 之后后端能做 `Σ(attr/w·λ)/Σ(1/w·λ)`。
  2D 路径（`BgpuVertex` / `draw_quads`）**一个字节没动**。
- **固定功能 Lambert**：`BGPU_PIPE_LIGHTING` + `BgpuLight` + `bingpu_cmd_set_light` +
  `BGPU_CAP_LIGHTING`。公式与 bin3d 的软光栅逐字一致：

  ```
  片元 = base·vcol·tex × (ambient + lightColor·max(dot(n, dir), 0))
  ```

  `n` 是世界空间法线的透视校正插值再归一化；光照在**直通空间** clamp 到 [0,1]
  之后才乘 alpha（预乘空间里 clamp 会差一个 alpha 因子）。
  方向必须单位长度（API 层校验长度平方 ∈ [1e-8, 4]），后端不做归一化。
- `BGPU_CAP_PERSPECTIVE`：后端自报"网格属性做了透视校正"。

### Fixed / 坑

- **CPU 后端的"均匀色快路径"绕过了着色函数**：`raster_tri` 在顶点色一致时会把整片
  颜色在三角形外算好直接填（2D 的主战场 —— 文字/图标几千个小四边形全靠它），
  而光照与透视校正都是**逐像素**的。第一版忘了排除，表现为"开了 LIGHTING 但光照
  完全没生效"（三个法线方向画出同一个颜色）。现在快路径带 `plain2d` 判据。
- API 层挡掉 `BGPU_VERTEX_2D + BGPU_PIPE_LIGHTING`（2D 顶点没有法线）——
  否则后端会画出一片黑，那种错要花半天找。

### 测试

- `test_cpu_render` 新增 `[lighting]` / `[perspective]`：判据是**手算的数值**，
  不是"看起来对"。
  - 光照三例：法线朝光源 ndl=1 → clamp(0.1+1.0)=**255**；背光 ndl=0 → ambient **26**；
    斜 45° → 0.1+0.7071=0.8071 → **206**。
  - 透视：一条水平线段，左端 w=1、右端 w=4，纹理 2×1 黑白。x=4 处屏幕线性 u=0.5625
    （白）而透视校正 u=0.2432（黑）—— 两者结果相反，一点就能分辨走的哪条公式。
- `test_abi` 247 项、`test_cpu_render` 378 项全过；VK / D3D11 / GL 各 13 项照旧全过。

### 还没做（下一步）

- VK / D3D11 / GL 三个后端的光照与透视：shader 要重编（`fxc` / `glslc` 本机都有），
  目前它们**自报不支持** `BGPU_CAP_LIGHTING` / `BGPU_CAP_PERSPECTIVE`，行为与 0.5.2
  完全一致（网格照旧不校正、不光照）。
- bin3d 的 `GpuRenderer` 接上 binGPU（`../bin3D/src/gpu.cpp` 现在还是纯桩）。

## [0.5.2] - 2026-11-18

**两条性能/语义的坑，都是 bin2d 的图层合成基准逼出来的**（`../bin2D/tools/bin2d_bench_gpu.cpp`
把 1280×720×16 层的三种合成路线摆在一张表上）。**ABI 不变**（ABI 2）。

### Fixed

1. **D3D11：纹理上传走的是 D3D11 最慢的一条路**。第一版是"建一张 b.w×b.h 的 STAGING 纹理
   → Map → 拷行 → Unmap → `CopySubresourceRegion` 到 DEFAULT 纹理"：功能对，但每次上传
   都要新建一张暂存纹理，而 STAGING → DEFAULT 的拷贝在驱动里是同步的。实测 56 MB/帧
   （1280×720 的 16 个图层）要 **5942 ms**，比 VK（115 ms）和 GL（106 ms）的同等工作量
   差两个数量级 —— "动态纹理"这条路等于废了（动画、视频帧、虚拟列表都会踩）。
   现在改成先在普通内存里把这段行连续排好并预乘（thread_local 缓冲），再**一次
   `UpdateSubresource`** 写进目标区域：同一基准 **5942 ms → 100 ms**（59 倍）。
   局部上传（`box`）的语义不变，`box` 用例仍 0 差。
2. **`bingpu_device_wait_idle()` 是个谎**。它在 `bingpu_api.cpp` 里直接 `return BGPU_OK`，
   注释写着"真实后端在这里等队列"，但三个真后端一个都没实现 —— 调用方以为 GPU 画完了，
   其实只把命令推给了驱动。表现是基准量到的是**命令录制时间**（0.011 ms 假装画完 16 个
   全屏四边形），截图/回读还可能读到半张图。现在各后端真的等：
   VK = `vkDeviceWaitIdle`、D3D11 = `D3D11_QUERY_EVENT` + `Flush` + 轮询、GL = `glFinish`
   （切到该设备自己的 context 再 `glFinish`，完成后恢复）。同一基准 0.011 ms → 0.21 ms，
   这才是 16 个全屏图层的真实 GPU 时长。

### Changed

- `src/bingpu_backend.h` 的 `Backend` 接口新增虚函数 `deviceWaitIdle()`
  （**内部 C++ 接口**，不是 C ABI，`BINGPU_ABI_VERSION` 仍为 2）。
- `docs/backends.md` 新增"性能：两条被基准抓出来的路径（0.5.2）"。

## [0.5.1] - 2026-11-18

**两个 bug，都是"交换链回读"这条路上才有的**（`frame_begin → … → frame_end → frame_read`）。
它们被 bin2d 的 GPU 桥先撞出来 —— 那份代码在 binGPU 这三个后端之前就写好了，一行没改地
驱动 D3D11 / GL，于是当场暴露了**12 个 `texture_read` 用例一个都没抓到**的两条回读路径差异。

### Fixed

1. **D3D11：HEADLESS 交换链借出的帧目标漏了 `res_w/res_h`**。借出对象只赋了 `w/h/fmt/dxgi`，
   而 `begin_render` 的 viewport、`updateConstants` 的 `target`、`applyScissor` 的裁剪矩形
   全按 `res_w/res_h` 算 —— 于是 viewport 0×0、裁剪矩形为空，**一笔都画不出来**，读回来只有
   清屏色（bin2d 那边逐像素差 255）。窗口路径没这个毛病（那条分支赋了 `res_w/res_h`），
   所以 `make smoke` 一直是绿的。
2. **GL：FBO 回读多翻了一次行**。渲染到 FBO 时 `uFlip=0`（`s.y=0 → NDC -1 → 窗口 y=0`），
   而 `glReadPixels` 正是从窗口 y=0 往上吐 —— 第一条就是图像顶行，不该再翻。多翻一次的结果是
   整幅**上下镜像**（bin2d 用一张 45° 渐变框看出来的，通道差 38）。

### Added

- `tests/gpu_parity.h` 新增 `swapchain(frame_read 满幅纹理)` 用例（96×64，坐标色纹理 +
  NEAREST + REPLACE + 全透明清屏）：它走 `frame_*` 而不是 `texture_read`，因此能钉住
  "两条回读路径必须给出同一幅图"。三个真后端现在各 13 个用例、全部 0 差（双线性那条仍是
  容差用例）。
- `docs/backends.md` 补一条：**回读有两条路径**（离屏目标 / 交换链借出），验收必须两条都覆盖。

### 版本

- `0.5.1`（PATCH：只修 bug，没有新能力）；`BINGPU_ABI_VERSION` 仍是 **2**。

## [0.5.0] - 2026-11-18

**OpenGL 后端落地**（`src/bingpu_gl.cpp`，`GlBackend`）：`docs/layering.md` §5 排的顺序里
第三个、也是**最后一个异族**后端。GL 把"异族"推到极端：没有命令缓冲、没有 PSO、资源是
绑定点上的全局状态，连"深度附件"都不是对象而是 framebuffer 的一个挂点 —— 而
`tests/gpu_parity.h` 那份 12 个用例的规格**一个字没改**就跑出了与 CPU 逐像素 0 差。

三个真实后端现在互相**逐位相同**：同一张复合场景（五种混合 + 接缝 + 重复纹理 + 旋转仿射 +
深度 + 网格），GL 输出的 PNG 与 D3D11 输出的 PNG **sha256 都一样**。

| 用例 | GL vs CPU |
|---|---|
| clear / 覆盖 / 五种混合 / 接缝 / scissor / viewport / affine / 最近+重复 / 局部上传 / 深度 / 网格 | **0 像素差** |
| 双线性取样 | 12.158% 像素、最大通道差 **1**（与 VK/D3D11 完全同一组数字） |

### Added

- **`GlBackend`**：设备（WGL + **OpenGL 3.3 core**，无窗口时用一个隐藏窗口的 DC）/
  交换链（窗口 = 默认帧缓冲 + `SwapBuffers`；HEADLESS = 离屏纹理挂 FBO）/
  `frame_read`（窗口在 `SwapBuffers` 前读一份并翻行；离屏直接读 FBO）/
  纹理上传与回读（内部一律 RGBA8，BGRA8 只在边界换）/ 缓冲（VBO/IBO）/
  采样器对象 / 五种混合 + 深度 + 剔除 / `draw_quads` + `draw_indexed` / 仿射 / 深度附件。
- **零 GL 开发包**：不 include `<GL/gl.h>`（它只有 1.1 的枚举），类型与常量自带；
  函数一律 `wglGetProcAddress` 优先、`opengl32.dll` 兜底；**不链接** `opengl32.lib`。
- 状态剔除（`layering.md` §6.2）：混合/深度/剔除/纹理/采样器/裁剪矩形都只在真的变了才发 GL 调用。
- **`tools/gpu_window_smoke.c` + `make smoke`**：把"窗口路径"从临时程序升成仓库工具 ——
  三个真后端各开一个真窗口跑 3 帧 clear 往返 + 四边形位置 + resize 后的客户区尺寸。
  这一项要桌面会话，所以不进 `make test`，但每个真后端落地都该跑一次。
- 三个真后端的**复合场景出图逐位相同**：同一份场景（五种混合 + 接缝 + 重复纹理 + 旋转仿射 +
  深度 + 网格）经 VK / D3D11 / GL 渲出 320×240，三个 BMP 的 MD5 完全一致；与 CPU 参考后端比
  只差双线性/斜边那 1.699% 像素、最大通道差 2。

### Fixed（落地时踩到的）

1. **`GL_SCISSOR_TEST` 的裁剪框默认值是建上下文那个窗口的大小**（离屏设备上是隐藏窗口的
   8×8）。不每趟清成整目标的话，`clear` 只清左上角 8×8，其余像素是未初始化内容 —— clear
   用例当场 98% 差异、其余涉及清屏的用例跟着红。现在 `begin_render` 里无条件
   `glScissor(0,0,w,h)`（与 D3D11 每趟 `RSSetScissorRects` 对称）。
2. **GL 的窗口原点在左下**：渲染到 FBO 时 `ndc.y = 2*s.y/h - 1`（存储行 0 = 图像顶行，
   `glReadPixels` 自下而上正好对上，回读不用翻），渲染到默认帧缓冲时 `ndc.y = 1 - 2*s.y/h`
   且读回要翻行。用一个 `uFlip` uniform + 一处 `flipRows` 收口，**用例表里看不出来**。

### 明确没做（写进 `docs/backends.md` 的欠账）

- MSAA / mip 链 / 透明窗口 / 多帧飞行（与 VK/D3D11 一致的清单）。
- 一个 HWND 只能绑一个 GL 设备（WGL 的上下文绑在 DC 上，`contracts.md` §4 也是这么规定的）。

### 版本

- `0.5.0`（MINOR：新后端 = 新能力）；`BINGPU_ABI_VERSION` 仍是 **2**（GL 的枚举值 0.1.0 就在）。

## [0.4.0] - 2026-11-18

**D3D11 后端落地**（`src/bingpu_d3d11.cpp`，`D3d11Backend`）：binGPU 有了第二个真后端，
而且是**异族模型**（即时上下文、无命令缓冲、无 PSO）的代表 —— `docs/layering.md` §4 的
关键推论"异族后端不需要模拟命令缓冲"这次被实测证实：`BgpuCmd` 在 D3D11 里退化成
"一个状态结构体 + 立即提交"，**用法例表一个字都没改**就跑通了逐像素对照。

验收口径照 [`docs/layering.md`](docs/layering.md) §2 的 P1（同一串命令 → 同样的像素），
裁判仍然是 CPU 参考后端：

| 用例 | VK vs CPU | D3D11 vs CPU |
|---|---|---|
| clear / 覆盖 / 五种混合 / top-left 接缝 / scissor / viewport / affine / 最近+重复 / 局部上传 / 深度 / 网格 | **0 像素差** | **0 像素差** |
| 双线性取样 | 12.158% 像素、最大通道差 **1** | 同上（两个真后端**互相也逐位相同**） |

### Added

- **`D3d11Backend`**：设备（feature level 10.0 起，`D3D11_CREATE_DEVICE_DEBUG` 失败会自动
  退回普通设备）/ 交换链（`IDXGIFactory::CreateSwapChain`，blt 模型 + `DXGI_SWAP_EFFECT_DISCARD`，
  vsync 走 `Present(1,0)`）/ `frame_read`（present 时拷进 staging，与 VK 同一份语义）/
  纹理上传与回读 / 缓冲（`DEFAULT` + `UpdateSubresource` 带 box，局部更新是原生语义）/
  采样器 / 五种混合状态 / 4 种深度状态 / 2 种光栅化状态 / `draw_quads`（顶点流 + 32 位索引
  展开 TL,TR,BL / BL,TR,BR）/ `draw_indexed` / 仿射 / 离屏目标 / D32 深度附件。
- **`src/d3d11_shaders/quad.hlsl`**：与 `quad.vert`/`quad.frag` 逐行同构的 HLSL 版（差异只有
  D3D 的 NDC y 向上 → 着色器里翻一次，以及正面判定跟着翻）。DXBC 由 `fxc` 编出后以数组入库
  （`src/bingpu_d3d11_shaders.h` + `tools/gen_dxbc_header.py`）：**构建不需要 Windows SDK**。
- **`tests/gpu_parity.h`**：把 VK 那 11 个用例抽成**两个 GPU 后端共用的规格**，并新增第 12 个
  用例 `texture(box 局部上传)`。`tests/test_vk_render.c` / `tests/test_d3d11_render.c` 现在各只有
  一行 `main` —— 加第四个后端只需再写一行，用例表不用动。
- **ABI 2**：`BGPU_BACKEND_D3D11 = 6` 追加在枚举末尾（既有枚举值 / 结构体 / 函数签名全部不变），
  `BINGPU_ABI_VERSION` 1 → 2。注册表顺序变成 `VULKAN → D3D11 → D3D12 → GL → CPU → NULL`。
- 窗口路径烟测（临时程序，不入库）：两个真后端各跑 3 帧 clear 往返**逐字节相等**
  （`FF102030` / `FF30A050` / `FF2030E0`），白色四边形落在 (100,100)，改窗口尺寸后
  `resize` 到新的客户区（`384x221`）两边报同一个值。
- **交换链尺寸听窗口的**（两个真后端统一）：窗口模式下 `resize(w, h)` 的请求值只在取不到
  客户区时兜底。理由：尺寸不同的后台缓冲会被 DXGI 拉伸上屏、被 VK 忽略，两个后端的
  `swapchain_info` 就会分叉。

### Fixed

- **VK 后端的局部上传没有预乘**（0.3.0 的写法：`if (!box)` 才预乘）。CPU 后端是**按区域**
  预乘的，所以"局部更新的半透明贴图"在 VK 上会偏亮。新增的 `texture(box)` 用例当场抓到
  （差异像素 25%、最大通道差 99），现在两个 GPU 后端都是 0 差。
- **D3D11 的 `CopySubresourceRegion` 参数搞反**：最后一个参数是**源** box，目的区域要由
  `DstX/DstY` 给。第一版把 box 当源传，局部上传整体挪到了 (0,0)（用例里 31% 像素差）。
- **R8 遮罩的采样广播**：MinGW 的 `D3D11_SHADER_RESOURCE_VIEW_DESC` 没有
  `Shader4ComponentMapping` 字段，所以 D3D11 只能拿到 `(r,0,0,1)`，而 CPU 当作 `(v,v,v,v)`。
  这条差异与 VK 后端**完全一致**（两个 GPU 后端之间是齐的），记进 `docs/backends.md` 的欠账。

### 明确没做（写进 `docs/backends.md` 的欠账）

- **多帧飞行**：`max_frames_in_flight == 1`（Present 之后不等 GPU，但也不做流水线）。
- **MSAA** / **mip 链** / **透明窗口**（分层窗口 + premultiplied composite）。
- **Deferred Context**：`contracts.md` §6 已经禁止跨线程记录，用不上。

### 版本

- `0.4.0`（MINOR：新后端 = 新能力）；`BINGPU_ABI_VERSION` **2**（新增后端枚举值）。

## [0.3.0] - 2026-11-18

**Vulkan 后端落地**（`src/bingpu_vk.cpp`，`VkBackend`）：binGPU 从"只有 CPU/NULL"变成
"有真后端"。`BINGPU_ABI_VERSION` 仍是 **1**。

验收口径照 [`docs/layering.md`](docs/layering.md) §2 的 P1：**同一串命令 → 同样的像素**，
裁判是 CPU 参考后端。结果：

| 用例 | 差异 |
|---|---|
| clear / quad 覆盖 / 五种混合 / top-left 接缝 / scissor / viewport / affine / 深度 / 网格 / nearest 取样 | **0 像素差** |
| 双线性取样（`texture(linear/clamp)`） | 12.2% 像素、最大通道差 **1**（CPU 用 double 累加、GPU 用 float 插值，两条数值路径的固有差异；容差写死在用例里） |

### Added

- `VkBackend`：设备（Vulkan 1.3 + `dynamicRendering`）/ 交换链（`VK_KHR_swapchain`，
  FIFO=vsync 或 IMMEDIATE）/ present / 纹理上传与回读 / 缓冲 / 采样器 / 固定管线
  （2 种顶点布局 × 5 种混合 + 深度测试写入 + 剔除）/ 离屏目标 / 深度附件 /
  `draw_quads`（顶点流 + 32 位索引展开 TL,TR,BL / BL,TR,BR）/ `draw_indexed` / 仿射。
- `src/vk_shaders/quad.{vert,frag}` + `src/bingpu_vk_shaders.h`：固定功能的两个着色器与
  入库的 SPIR-V（`tools/gen_spv_header.py` 生成；本机没有 Vulkan SDK，用 Android NDK 的
  `glslc` 编，和 binUI 同一套做法）。
- `tests/test_vk_render.c`：**Vulkan vs CPU 逐像素对照**，11 个用例按 `raster_rules.md`
  的章节顺序排；没有 Vulkan 的机器整份跳过（exit 0），CI 不会因为没显卡变红。
- `scripts/fetch-deps.sh` + `third_party/README.md`：Vulkan 头不入库，拉一次即可；
  **没有头也能构建** —— `__has_include` 让后端退化成"登记但不可用"的桩。
- **HEADLESS 交换链**：没有窗口的设备也能建交换链 —— 语义不变（"帧的载体 + `frame_read`
  出图"，`contracts.md` 的帧模型），实现退化成一张离屏纹理 + "拷到回读缓冲"代替 present。
  bin2d 的 GPU 桥就是这种用法。
- 状态剔除（`layering.md` §6.2 的契约在 VK 侧先落地）：管线/描述符/scissor 都只在真的
  变了才发 `vkCmd*`；顶点流按需增长，一帧内只绑一次。

### 落地时踩到并修掉的 6 个坑（都留了注释）

1. **`createCmd` 复用了 `BackendBase::createCmd`** → 造出来的是基类 `Cmd`，再
   `static_cast<VkCmdObj&>` 就是 UB（症状：`vkCmdPipelineBarrier: Invalid commandBuffer`）。
   现在后端自己 `makeObject<VkCmdObj>`。
2. **默认采样器做成了 `Sampler` 对象** → `destroyOwnedObjects` 先销毁它，设备析构再销毁一次
   = double free。现在默认采样器由设备直接持有。
3. **clear color 的通道序错了两次**：先是把顺序写死成 `B,G,R`（RGBA8 目标上红蓝对调）；
   后来改成"BGRA 时交换分量"也不对 —— `clearValue.color.float32` 是 **R,G,B 分量序**，
   字节序由 `VkFormat` 决定。现在只按分量写，字节序交给格式。
4. **push constant 的 std430 对齐**：`vec2` 之后下一个 `vec4` 要从 32 开始，C++ 结构体
   必须补 8 字节（第一版 `xf0` 落在 24，为什么"仿射坏了"查了半天）。
5. **仿射矩阵被按列主序重排**：`cmd.xf` 的 6 个数本来就是 `[a c e | b d f]`，直接搬；
   重排之后仿射用例整块画不出来。
6. **交换链借出的图像被当成自有资源销毁**：`frame_begin` 的 target 是**借用**句柄
   （`contracts.md` §2），第一版给它建了自己的 `VkImage`，`frame_begin` 又把 image 指过去，
   退出时销毁交换链图像 → 窗口路径必崩。现在借用对象 `own_image = false`。
7. **HEADLESS 交换链的 views 被二次销毁**：那几张 view 属于离屏纹理，交换链析构里再销毁
   一次就是 double free。现在 `owns_images` 只在真交换链时为真。

### 明确没做（写进 `docs/backends.md` 的欠账）

- **多帧飞行**：现在每帧 `vkQueueSubmit` + 等 fence，`max_frames_in_flight == 1`。
  先要正确、再要重叠；上层（binUI）目前也是单帧。
- **MSAA**：`msaa_samples == 1`，与 CPU 参考后端一致 —— 这是"0 差验收"的前提；
  开 MSAA 时容差怎么定见 `layering.md` §2。
- **mip 链**：`createTexture` 带 `BGPU_TEXTURE_MIPMAPS` 直接返回 `UNSUPPORTED`。
- **透明窗口**：`compositeAlpha = OPAQUE`；透明窗要 premultiplied composite + 分层窗口，
  留给后面（binUI 的 VK 后端已经有这套，可参考）。

### 版本

- `0.3.0`（MINOR：新后端 = 新能力）；`BINGPU_ABI_VERSION` 仍是 **1**。

## [0.2.2] - 2026-11-18

**只有文档**：把"这一层在哪、四家后端怎么接"写成 [`docs/layering.md`](docs/layering.md)。
`BINGPU_ABI_VERSION` 仍是 **1**，`include/bingpu.h` 的功能面一个字未动（只有版本宏）。

来源是 binUI 三个 GPU 后端（GL / VK / D3D11）跑通"逐像素一致"之后的复盘，回答了三个问题：

1. **该收什么**：binUI 的绘制语义早就统一了（共用遍历 1860 行 + 共用字形图集 1169 行 +
   7 个原语），重复的是**设备与呈现**（交换链、resize 策略、present 路径、离屏池、MSAA 解析，
   每端各一份）。五个已知 bug 里三个长在这一段，两个长在呈现策略。
2. **往上的承诺必须分成两条**：P1 = "同一串 `BgpuVertex` → 同样的像素"（binGPU 自己承诺，
   裁判是 CPU 参考后端）；P2 = "同一份内容 → 同一串 `BgpuVertex`"（上层承诺，裁判是各自基准）。
   混在一起验收会得出完全相反的错误结论。MSAA 破坏 P1 的 0 差，选"通道差 ≤2 / 差异像素
   ≤0.1%"的容差表，而不是让 CPU 也做 4× 参考（理由写在 §2）。
3. **四家分两族**：VK / D3D12 / Metal 同族（PSO + 命令缓冲，原生映射）；GL / D3D11 异族
   （即时状态，做**状态映射**，不需要模拟命令缓冲 —— `contracts.md` §6 本来就禁止跨线程记录）。

### Changed（排期修订，详见 `layering.md` §5）

- **后端顺序改为 VK → D3D11 → GL → Metal，D3D12 待定**。原来写的是 VK → D3D12 → GL；
  理由：D3D12 与 VK 同族、验证价值最低却要重写约 2416 行；而 D3D11 是异族模型，能反向
  验证"这套 ABI 对上一代 API 也成立"。
- **GL 从"不迁"改为"最后迁"**：原理由是"翻译层会变成半个 GL 封装"，但 binGPU 的指令面是
  固定功能批处理（不是 RHI），适配量级约等于 binUI 的 300 行状态/present 代码。排在最后
  的理由改为"没有用户"。

### Documented（给 ABI 2 的三条提案，本次不动 ABI）

- **6.1 句柄不走哈希表**：binUI 一帧 103 原语 / 938 四边形、遍历 16.6 µs；建议句柄 = 指针 +
  魔数，深度校验只在 `BGPU_DEVICE_VALIDATE` 下做。验收线：迁移后 `make bench-draw` 的遍历
  退化不超过 20%。
- **6.2 冗余状态剔除写成后端契约**：上层重复 `set_pipeline` / `set_texture` 是常态，
  按 `(pipeline, texture, scissor)` 切批的责任在后端，不在上层。
- **6.3 present 升格为一等公民**：`BGPU_PRESENT_SWAPCHAIN / READBACK / LAYERED` 三态 +
  守卫判定（铺满显示器 / 远程会话 / 拖拽中）收进 binGPU；并把"每帧至多一次交换链重建"、
  "拖拽期不重建交换链不划算（resize 省 3 ms，present 多付 2.2 ms，整帧不变）"写成契约。

### Fixed

- **Makefile 从来没有 include 过 `-MMD -MP` 生成的 `.d`**：改 `include/bingpu.h`（结构体、版本宏、
  枚举值）之后目标文件不重编，`make` 报 "Nothing to be done"，链出来的还是旧库。这次改版本宏
  当场踩到（测试打印 `0.2.1`、库是新的）。修法：`-include $(wildcard $(OBJ_DIR)/*.d
  $(OBJ_DIR)/*/*.d)`。
- 版本号 `0.2.2`（`include/bingpu.h` 宏 + `tests/test_abi.c` 的断言）。

### 同时

- `docs/backends.md`：加排期修订、GL 段落改写、AUTO 顺序的重排建议（新增后端枚举 = ABI 变化，
  留到 ABI 2）。

## [0.2.1] - 2026-11-15

CPU 参考后端的吞吐优化轮：**四边形填充 4.6–7.3×、裁剪内小批 6.5×**，纹理路径 1.6–2.3×，
**像素逐位不变**（`make bench` 的 13 个校验和与 0.2.0 一致，239 + 334 断言全绿）。
`BINGPU_ABI_VERSION` 仍是 **1**。

### Added

- `tools/bingpu_bench.c` + `make bench`：六类固定场景（纯色五种混合 / 纹理四种采样 /
  scatter 小批 / 索引网格 / 离屏合成 / 回读）的吞吐基准，每项 5 次取最好，并打印 13 个
  FNV-1a 像素校验和 —— 优化前后必须逐位相同。
- `docs/perf.md`：前后数字表、每条优化的收益、未做项与原因、复现方式。

### Optimized

- 光栅内核按"是否纹理 / 是否有深度"分循环（不纹理不算 UV、不测深度不插 Z）；
  混合模式变模板参数（switch 提到三角形外）；REPLACE 与"预乘 + 源 alpha 满"不读目标。
- 均匀色三角形：整条片元计算提到循环外；同色同混合时用 **4×256 混合查表**（表项与
  逐像素路径同一条公式），KEEP/ERASE/ADDITIVE 从 ~78 提到 ~250–290 MPix/s。
- 采样器：`TexState` 把尺寸/格式/filter/wrap 提到循环外；内联 floor；REPEAT 取模快路径；
  4 字节格式按 `uint32` 取纹素。
- 字节↔float 用 256 项查表；反预乘用 64 KiB `(p,a)` 查表；clear 预乘量化一次后按
  `uint32` 铺。
- `src/bingpu_cpu.cpp` 单独用 `-O3`（其余文件仍 `-O2`；量过：UV 采样路径再快 ~20%）。

### Fixed

- **R8 纹理当渲染目标会写越界**：`px_store`/clear 恒按 4 字节写，而 R8 是每像素 1 字节。
  0.2.1 起 `texture_create` 直接拒绝 `R8 + RENDER_TARGET`（R8 仍可作采样遮罩）。

### Kept

- `include/bingpu.h` 只改 `BINGPU_VERSION_PATCH`/`BINGPU_VERSION_STRING` 与文件头注释，
  签名/枚举/结构体布局一个字节未动；ABI 仍是 1。
- 断言数 239 + 334 不变；零警告；LF。

## [0.2.0] - 2026-11-15

**实现语言从 C11 换成 C++17，二进制 ABI 一位未变（仍是 ABI 1）。**
公开头 `include/bingpu.h` 的签名 / 枚举 / 结构体布局全部保持，只把文件头里"为什么用 C"
改写成了"为什么实现用 C++ 而 ABI 仍保持 C 兼容"。

### Changed

- `src/*.c` → `src/*.cpp`。内部改成 C++ 抽象：`src/bingpu_backend.h` 的 `class Backend`
  （虚函数：创建设备/交换链/纹理/缓冲/采样器/管线/命令、begin/end render、draw、readback），
  `NullBackend` / `CpuBackend` 继承它；两个后端共用的命令状态机、参数校验、stats 计数
  收进 `BackendBase`（0.1.0 是两套手写实现，现在结构上不可能不一致）。
- 对象表：`std::vector<std::unique_ptr<ObjectBase>>` + RAII。资源释放在析构里 ——
  显式 `destroy` 只负责"从表里摘掉"，设备销毁连带清理复用同一份代码。
- 错误槽：`_Thread_local char[512]` + `snprintf` → `thread_local std::string`，
  错误串用 `std::string` 拼接（不截断）。语义不变：每次失败都写、成功不清空、格式仍是
  `<函数>: <原因> (file:line)`，`file:line` 现在指向真正的失败位置（后端 .cpp）。
- 像素/缓冲：`std::vector<uint8_t>`；字符串 `std::string`。
- **异常绝不越过 ABI**：每个 `extern "C"` 入口顶层 try/catch —— `bad_alloc` →
  `BGPU_ERR_OUT_OF_MEMORY`，其它 `std::exception` → `BGPU_ERR_BACKEND_FAILED`
  （`what()` 进错误串），未知异常同样兜住。（公共头是冻结的 ABI，没有 `noexcept`；
  C++ 要求声明与定义的异常规格一致，所以不能只在定义上补 —— 等价保证由 try/catch 提供。）
- 构建：`CXX ?= g++` / `-std=c++17`；测试与工具**仍是 C 源码**（同时是"C 能消费这个 ABI"
  的证明），用 `gcc` 编、用 `g++` 链接（`-static-libgcc -static-libstdc++`）。
  `CMakeLists.txt` 改 `project(binGPU C CXX)`。
- `make abi-check` 增加第三条冒烟：C 目标文件 + C++ 静态库 → `g++` 链接 → 跑起来。
  第二条 grep 改为"先剥注释再查"（头文件的说明文字里现在会正常出现 `std::vector` 这类词，
  要拦的是声明里出现 C++ 设施）。

### Fixed（旧的 C 实现里存在、C++ 版按新写法修掉的行为差异）

- **NULL 后端的 `stats.bytes_live` 只加不减**（0.1.0 的析构忘了回写）。现在纹理/缓冲/交换链
  的析构都回写记账，`bytes_live` 真正表示"当前活着的字节数"。
- **帧中途销毁管线/纹理会 use-after-free**（cmd 的状态里存着裸指针）。现在销毁前会通知
  所有活对象把指向它的指针清空，之后的使用退化成 `BGPU_ERR_STATE`，不再是 UB。
- **`draw_indexed` 的索引越界可能"画一半再报错"**（0.1.0 的 CPU 版边画边查）。现在先整段
  校验所有索引，再光栅化：要么全画、要么一个像素都不动。
- **`draw_indexed` 的 `vertex_offset` 对齐检查只在不存在的 NULL 分支里**。现在两个后端统一：
  `vertex_offset` 必须是 `sizeof(BgpuMeshVertex)` 的倍数。
- 交换链被销毁时，借出的目标纹理现在一起失效（0.1.0 会留下一个指向已释放内存的句柄）。

### Kept

- 常量：版本 `0.2.0`；`BINGPU_ABI_VERSION` 仍是 **1**。
- 断言数：`test_abi` **239** 条、`test_cpu_render` **334** 条，全绿；零警告。
- 除上面"Fixed"里列的误用路径外，**能观察到的行为一位未变**：句柄编码、销毁后失效、
  借用目标不许销毁、stats 计数、caps 上报、错误串语义、像素规则全部保持。

### Not done

- **Vulkan / D3D12 后端仍只是登记状态**（`device_create` 返回 `BGPU_ERR_UNSUPPORTED` +
  原因）；GL 不迁。路线与欠账见 `docs/backends.md`。
- 没有 MSAA / mip 采样 / 用户着色器 / 网格光照；CPU 后端仍不呈现窗口。

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

[0.2.1]: https://example.invalid/bingpu/releases/tag/v0.2.1
[0.2.0]: https://example.invalid/bingpu/releases/tag/v0.2.0
[0.1.0]: https://example.invalid/bingpu/releases/tag/v0.1.0
