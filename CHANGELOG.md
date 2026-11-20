# 变更记录

格式照 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)；版本号遵循
[语义化版本](https://semver.org/lang/zh-CN/)。`ABI` 指的是 `BINGPU_ABI_VERSION`
（结构体布局 / 函数签名 / 枚举值一变就 +1，与语义化版本无关）。

## [0.16.0] - 2026-11-21 【ABI 5 → 6】

**消掉"每进出一次目标就 1×→4× 回灌 + pass 末 resolve"的往返。** 触发场景是 binUI 的
`svg-filter` 页（660×470 窗口，14 个图层 + 模糊链）：bin2D 的画布一帧被 `begin_render`
了 **33 趟** —— 每离开一次（进图层）resolve 一次 4×→1×，每回来一次（LOAD）再 1×→4×
回灌一次；而中间那些图层绘制根本不采画布。两样都是全窗读+写。

### 改了什么

- **ABI 6**：`BgpuRenderBeginDesc.flags` 新增 `BGPU_RENDER_DEFER_RESOLVE = 1<<2`；
  新增 `bingpu_cmd_resolve_target(cmd, target)`。结构体布局一个字节没动。
- **VK**：
  - `msaa_valid`：companion 里已经是这张图最新内容时，LOAD 的 1×→4× 回灌**跳过**；
    只有 pass 之外改过 1×（上传 / 换交换链图 / 刚建 companion）才回灌。
  - `resolve_pending`：DEFER 的 pass 不在 `vkCmdEndRendering` 上 resolve，1× 暂落后。
  - **自愈**：采样（`rasterQuads`/`rasterIndexed`；必要时 endRendering → resolve →
    原样 LOAD 开回去）、`texture_read`、`frame_end`/`frame_read`、`texture_upload`
    之前自动补一次 resolve；显式入口是 `bingpu_cmd_resolve_target`。
  - **自动策略**：整窗尺寸的目标、一帧第 2 趟起自动 defer（层/blur 目标按元素尺寸分配，
    对它们 defer 一定紧接自愈，实测净收益 0）。`BINGPU_NO_AUTO_DEFER=1` 关掉做 A/B。
  - 诊断：`BINGPU_TRACE_RESOLVE=1` 每帧一行 `blits/resolves` 次数 + 像素量 + 带宽估算，
    自愈时打 `self-heal WxH`；`BINGPU_TRACE_PASSES=1` 逐 pass 打尺寸/LOAD/CLEAR/msaa。
- **GL**：同一套语义（`msaa_pending` + 采样/回读/呈现前自愈）。GL 的 msaa FBO 天然保留
  内容，本来就没有回灌那一步；欠账 12（GL 窗口 4× 丢内容）的判断不变。
- **D3D11**：明确不做（没有 MSAA，就没有"挂起的 resolve"）；`resolve_target` 走默认
  no-op，flag 无效。写成欠账 13。

### 实测（本机 4060 Ti / 610.88，`svg-filter` 660×470 窗口，`probe_frame` 100 帧中位）

| | 每帧整窗回灌 | resolve（整窗/全部） | 帧时 |
|---|---|---|---|
| 0.15.1 | 32 | 33 / 99 | 3.30~3.42 ms |
| 0.16.0 | 0 | 1 / 69 | **2.69~2.71 ms** |

A/B 分账：回灌 ≈0.50 ms/帧、全部 resolve ≈0.53 ms/帧、两样都关 ≈0.94 ms/帧
（不可达上限：层/blur 目标必须 resolve）。4× 剩下的 ~0.6 ms 是 store/load 与填充率，
不在这条链上 —— 所以"消干净往返"的上限是 ~2.5 ms，到不了 1×（1.88 ms）。
`buttonshow` 0.80~0.82 ms、`appshow` 1.11~1.14 ms 不变（同机交错对照）。

### 像素与门禁

- binUI `pixels-bin2d-gpu` 18 页对 `docs/pixels` **逐页数字与改动前相同**
  （`svg-filter` 0.000% / max 6、`grad-angle` 0.028% / max 21、`shadow` 0/7、
  `texture` 诸页 0/1）；`pixels-bin2d`（0 容差）与 `make partial` 全绿。
- binGPU `make test`（含 VK/GL 的 MSAA 多目标 / 两趟 LOAD / 帧中销毁 / mip 用例）、
  `make abi-check`、`make smoke` 全绿。

## [0.16.0] - 2026-11-21 【ABI 6】

**延迟 resolve：这一趟不需要 1× 图时，就不落 1×。** 上层带图层的页面每进出一次图层，
旧版都要对 4× 画布做一次"1×→4× 回灌（LOAD 预置）+ pass 末 resolve"。现在这种往返可以推迟：
渲染只画进 4× 附件，等到**真的有人要那张 1×**（采样它、`texture_read`、`frame_read`、显式入口）
时才解析一次。

### 先给归因（这次的实测又把预估推翻了一次）

| 项 | 估的 | 实测 |
|---|---|---|
| 往返成本 | ~1.55 ms | **0.94 ms**（只关回灌 0.45~0.50、只关 resolve 0.53） |
| 每帧次数 | 14 次整窗往返 | **32 次整窗 LOAD 回灌 + 33 次整窗 resolve**（另有 66 次元素尺寸的层/模糊 pass） |

分账：MSAA 总成本 ~1.55~1.58（1× 时 1.87~1.90），其中只有 0.94 在往返链上，剩下 ~0.6 是
4× 的 store/load 与填充率。所以"把往返消干净"的**理论下限是 ~2.5 ms**，`≤2.2` 到不了 ——
这是这一版最重要的一句实话。

### 新增的接口（ABI 5 → 6）

- `BGPU_RENDER_DEFER_RESOLVE = 1u << 2`（`BgpuRenderBeginDesc.flags`，结构体布局零变化）
- `bingpu_cmd_resolve_target(cmd, target)`：显式解析一次；在 `begin/end_render` 之间调用返回 `STATE`。
- **自愈**：后端在"真的有人要那张 1×"的所有边界（采样、`texture_read`、`texture_upload`、
  `frame_end`）上自动补一次 resolve，所以上层**不需要**记住这件设备层的事。宁可多解析一次，
  不允许漏。用 `BINGPU_TRACE_RESOLVE=1` / `BINGPU_TRACE_PASSES=1` 可以看次数。
- 自动策略：整窗目标从"一帧第 2 趟起"延迟；**层 / blur 目标不延迟**（实测对它们延迟净收益 0，
  而且它们必须 resolve 才能被合成采样）。`BINGPU_NO_AUTO_DEFER=1` 可整体关掉。
- GL 后端同一套语义（GL 没有回灌这一步，只省 resolve）；**D3D11 明确不做**（它没有 MSAA，
  也就没有挂起的 resolve），flag 是 no-op，记成欠账 13。

### 数字

| 项 | 改前 | 改后 |
|---|---|---|
| `svg-filter` 660×470 窗口（VK 4×） | 3.30~3.42 ms | **2.69~2.76 ms**（−19%，p95 3.64~3.72 → 3.08~3.16） |
| `buttonshow` 1020×684 | 0.80~0.82 | 0.81~0.82 |
| `appshow` 1280×800 | 1.11~1.14 | 1.10~1.13 |

像素：`pixels-bin2d-gpu` 18 页**逐页数字与改动前相同**（`svg-filter` 0.000%/6、
`grad-angle` 0.028%/21、`shadow` 0/7，`KNOWN` 只剩 `text-shadow-multi`）；`pixels-bin2d`
0 容差全绿；`make partial` 4/4 绿；binGPU `make test`（257+378+72+32+23+35+16，0 失败）/
`abi-check`（0.16.0 / ABI 6）/ `smoke`（vk / d3d11 / gl）全绿。

新用例：`显式 DEFER + resolve_target` 与"立即 resolve"逐位相同（VK 与 GL 都 0/4096）。

### 教训（写给下一个估这块的人）

今天有三次归因被实测推翻：以为"整窗层目标清屏贵"（实际 0.03 ms）、以为"采样到未 resolve 的图"
（实际是帧中销毁被引用的目标炸驱动）、以为"往返值 1.55 ms"（实际 0.94，且次数是 32+33 而不是 14）。
**每次真相都在次数上，不在单次成本上。下次先数次数，再算单价。**

## [0.15.1] - 2026-11-20 【ABI 5 不变】

**"用不用 MSAA"从调用方各写一份判断，收回设备层。** `bingpu_render_plan` 原来无条件给
`msaa_samples = 4`，于是每个上层都得自己判断"这个后端开了安不安全"—— binUI 就在自己那份
后端里钉了一颗钉子。现在分档写在 `src/bingpu_error.cpp` 的 `msaaFor(kind)` 里：

| 后端 | 建议档位 | 为什么 |
|---|---|---|
| VULKAN | **4×** | 已验：binUI 的 18 个像素基准全 0 差异（0.15.0 修完多目标之后） |
| GL | **1×** | 窗口路径 4× 仍丢内容（`circle` 94.6% / max 255、`shadow` 95.9%），见欠账 12 |
| D3D11 | 1× | 没有这项能力（欠账 3） |
| CPU / NULL / 全都不可用 | 1× | 参考实现 |

上层因此只剩一行 `dd.msaa_samples = plan.msaa_samples;`。`docs/backends.md` 新增
"档位由谁定"一节，把这张表和两条欠账的入口写在一起。

## [0.15.0] - 2026-11-20 【ABI 5 不变】

**修 0.14.1 记下的那条 MSAA 欠账 —— 而根因与当初的两条猜测都不一样**：不是"采样到未
resolve 的单采样图"，也不是"resolve 落到错图"，而是**调用方在帧中途销毁了一张还被命令
缓冲引用着的渲染目标**。VK 后端旧版当场 `vkDestroyImage` + `vkFreeMemory`，4× 下连
MSAA companion 一起还回去，GPU 页错误 ⇒ `VK_ERROR_DEVICE_LOST` ⇒ 驱动把整帧命令丢掉
⇒ 整页黑。

### 证据链（记下来，免得下次又往 resolve 时序上猜）

- `svg-filter` 4× 整页黑（全像素 0），`vkDeviceWaitIdle` 返回 `-4 = VK_ERROR_DEVICE_LOST`，
  Windows 事件日志每跑一次一条 `nvlddmkm` 153；而**帧尾的 fence 等待是成功的** ——
  丢的是帧内命令。
- 逐 pass 二分：≤23 趟正常、第 24 趟起丢设备；第 24 趟**单独提交**时正常 ⇒ 不是某条
  命令非法，是"一帧里累积到某个点"。
- 每趟单独提交后 `svg-filter` **0 差异** ⇒ 像素逻辑本身是对的，问题在资源生命周期。
- 关掉 LOAD 的 blit、关掉 resolve 仍然丢设备 ⇒ 排除两条时序猜测。
- 给纹理析构打点：帧内只有两次销毁（75×68、113×75，都带 companion），来自 binUI
  `ensureBlurLevel` 换尺寸时先拆旧 level 再建同尺寸新图。**让析构不真释放 → 不再丢设备、
  `svg-filter` 0.000%**。定案。

### 修法

`VkDeviceObj` 加**退役表**（image / view / mem 与 buffer 两条队列 + `recording_cbs` +
`drainRetired()`）：`VkTextureObj` / `VkBufferObj` 析构时，只要设备还有命令缓冲在录制，
就只挂表、不释放；`beginRecording`（无别的 cb 在录时）、`submitNow`（fence 等完之后）、
`~VkDeviceObj` 三处排空。另外给 MSAA companion 补 `TRANSFER_SRC|TRANSFER_DST` 用途位
（LOAD 的 blit 拿它当 dst，原来缺这属于用法不合法）。

这条与 0.13.0（顶点流扩容 retire-then-free）、以及 bin2D 0.22.3（贴图缓存淘汰时销毁了
命令流还在引用的纹理）是**同一类错误的第三次**：**释放"命令流还在引用"的东西**。
`include/bingpu.h` 的质量档合同里因此加了一条对象生命周期承诺：帧中途 destroy 不许炸驱动。

### 验证

新用例（VK 与 GL 数字一致）：`画布→离屏抓取→合成回画布` 0/32768；`同一目标两趟 + 中途
当采样源` 0/8192；`帧中销毁目标` 1× 与 4× 都是 0/32768。原有 MSAA/mip 用例不变
（中间色 0→72、frame_read 72、mip 偏差 74.11→0.00）。

上层实测（binUI `pixels-gpu`，VK 4×，18 页对 `docs/pixels`）：**18/18 页 0 像素差异**，
最大通道差 ≤ 7 —— 其中三页是多年旧账一起清掉：`grad-angle` 0.667%/17 → 0/1、
`image-round` 0.217%/120 → 0/1、`image-mip` 0.118%/64 → 0/0；`svg-filter` 从"4× 整页黑"
到 0/6。

代价：`binui_gpu`（VK）0.65/0.71 → 0.93/1.11 ms（+43%/+56%）；bin2d 执行体那条不在这
一版受益（它自己还没申请质量档，见下）。

### 已知缺口（新记）

**GL 窗口路径的 4× 仍然丢内容**（`circle` 94.6% / max 255、`shadow` 95.9%、`svg-filter`
9.1%，同页 1× 都是 0）；把 VK 的延迟释放照搬过去**无改善**，所以不是同一根因，是 GL
multisample FBO / `glBlitFramebuffer` resolve 自己那条链的问题（headless 用例覆盖不到
窗口路径）。binUI 侧因此**按后端解除钉子**：VK 开 4×、GL 仍 1×。这条记在
`docs/backends.md` 欠账 12。

## [0.14.1] - 2026-11-20 【ABI 5 不变】

**把 0.14.0 的 MSAA 欠账记清楚**：接上层的实测，MSAA 在"一帧里多目标 / 图层"的场景会
丢内容 —— binUI 的 `svg-filter` 页（图层 + 模糊 + 多次换渲染目标 + 合成）在 4× 下
**整页空白**（对基线 100% 差异、最大通道差 255），同一页 1× 下 0 差异。

单目标用例全过（斜边中间色 0→72、两趟 LOAD 保内容、depth LESS 正确），所以缺口在
"每次换目标都新建 / 解析一张多采样 companion"这条链上，最可能是**主画布的 resolve 没有
落到那张被 present 采样的单采样 image 上**。能力位与 caps 不谎报（MSAA 确实能开），
但调用方在"一帧多目标"的场景不该默认开高档；修正记录在 `docs/backends.md` 的欠账 §11。

顺带确认了 0.14.0 的 mip 是真收益：同一个 `binui_gpu` 后端不钉 MSAA 的情况下，
`image-mip` 页从 37% / max 191 收到 **0.118% / max 64**，`svg-filter` 从 1.4% / max 34
收到 **0 / max 6** —— 缩小的图片与模糊链不再退化。

## [0.14.0] - 2026-11-20 【ABI 5】

**MSAA 与真正的 mip 采样从"后端吃掉入口"变成"可以显式要的质量档"**。这一版之前，
`BgpuDeviceDesc.msaa_samples` 与 `BGPU_TEXTURE_MIPMAPS` 两个入口一直存在，但四个后端
一个都不实现：device caps 硬写 `msaa_samples = 1`，mip 一律"接受但降级为 1 级"。
现在 VK 与 GL 把它们做成真能力，**默认档一个字节都没变**。

### 为什么这件事重要

上层（binUI 的三份老 GPU 后端）一直提供 GL 4× MSAA 与真 mip，而 binGPU 四端恒 1 ——
于是"把 UI 的绘制全部交给 binGPU"这件事卡在一个地方：删掉老后端就等于把这两项画质
一起扔掉（实测代价 `image-mip` 页 37%、`image-round` 页 0.217% / 最大通道差 120 的
旋转边）。质量档归设备层，是这套分层里最后一块该由 binGPU 决定的东西。

### 合同变了哪一处

- **同档设置下**仍然与 CPU 参考后端逐像素相同（每个后端、每条路径的老用例逐位未变）；
- MSAA / mip 是**显式选择的质量档**，不承诺与 CPU 参考逐像素相同 —— CPU 参考后端
  永远是 1×/1 级，它是"同档裁判"，不是"高质量实现"；
- `BgpuCaps.msaa_samples` 的语义从"实际生效值"改成**"能力上限"**（1/4/8）；实际这
  条链开了几倍看 `BgpuSwapchainInfo.msaa_samples`。只看 caps 的老代码不会因此变大，
  因为默认档仍是 1。

ABI 从 4 升到 **5**：结构体布局与函数签名一个字节没动，只是按本项目的老规矩
（"枚举值一变就 +1"，0.12.0 加 `BGPU_CAP_SHAPE` 时同样升过）给新增的
`BGPU_CAP_MSAA` / `BGPU_CAP_MIPMAPS` 记一次账。

### 实现（VK 先行，GL 同步）

- **VK**：每张颜色渲染目标（画布 / 图层 / 毛玻璃 / 离屏）懒建 N 采样 companion，
  动态渲染用 `resolveMode = AVERAGE`，在 `vkCmdEndRendering` 自动 resolve 回单采样
  image；`LOAD` 的 pass 先把单采样 image blit 回多采样附件（一帧多趟的内容才不丢）；
  深度按同一采样数建；管线缓存键带采样数。mip 用 `vkCmdBlitImage` 逐级 LINEAR
  降采样，采样器 `mipmapMode = LINEAR`、`maxLod = VK_LOD_CLAMP_NONE`。
- **GL**：multisample renderbuffer + 独立 FBO，`end_render` 用 `glBlitFramebuffer`
  resolve（窗口模式 resolve 到默认帧缓冲）；mip 用 `glGenerateMipmap`，并按 GL
  "完整性按采样器判"为每个采样器建 plain / mipmap 两个变体。
- **D3D11 这一版明确不做**：MSAA 要每目标多采样 texture + `ResolveSubresource` +
  交换链 resolve + DSV/RTV/staging 跟着改（约 250~350 行，窗口 present 风险最高），
  mip 约 100 行。它如实报 `1`、不置能力位，上层按 cap 分流的代码会留在默认档 ——
  不存在"以为开了其实没开"。理由与代价写在 `docs/backends.md`。
- CPU 参考后端继续 1×/1 级（它的职责是裁判，不是画得好看）。

### 验收数字

质量用例（VK 与 GL 相同，D3D11 skip）：

| 用例 | 1× / 1 级 | 高档 |
|---|---|---|
| 斜边中间色像素（MSAA） | **0** | **72**（`frame_read` 路径同样 72） |
| 缩小的 2px 棋盘 256→60（mip） | 平均绝对偏差 **74.11** | **0.00** |

`msaa=4` 下"两趟 LOAD 保留内容"与"depth LESS"都正确；本机 VK / GL 的
`msaa_max` 都是 8（要 8 就给 8）。

代价（VK 1024²，`bingpu_bench --backend vulkan --msaa 1|4`）：纯色/纹理铺满
0.070 → 0.150 ms（约 2.1×），离屏+合成 0.390 → 0.447 ms（+15%），读回 9.32 → 9.69 ms
（噪声级）—— MSAA 的代价集中在被覆盖的像素上。

默认路径无回归：`make all` / `make abi-check`（247 项）/ `make test`（cpu 378、vk 27、
d3d11 16、gl 27）全绿；改动前后 `tests/gpu_parity.h` 的 16 条用例逐位对比，三端各
16/16 行完全一致。

## [0.13.0] - 2026-11-20

**修掉 Vulkan 后端在"一帧里多目标来回切换"时的三处缺陷（外加一处能力对齐）—— 全是
"命令缓冲先录后执行"这个模型差异的锅，GL / D3D11 / CPU 都不会踩到。**（ABI 仍是 4，
一个字节没动。）

被 binUI 新加的"设备层建在 binGPU 上"后端（`binui_gpu.cpp`）撞出来的：同一个页面

| 页面 | 用到的特性 | 修前 GL vs VK | 修后 |
|---|---|---|---|
| `circle.html` | 解析圆角 + 文字 | 0 像素不同 | 0 / 0 |
| `opacity.html` | 图层（离屏 → 合成回来） | 0 像素不同 | 0 / 0 |
| `svg-filter.html` | 模糊图层 / 外阴影（多级降采样链） | **86.9%**（最大通道差 244） | 0 / 0 |
| `appshow/main.html` | 整页 + 毛玻璃弹窗 | **13.7%** | 0 / 0 |

### ① 根因：顶点流扩容时把"已经录进命令缓冲的 buffer"销毁了

VK 的顶点流是一条按帧增长的设备缓冲：一帧画得够多（约 682 个四边形）就要扩容。
而命令缓冲是**先录后执行**的 —— 扩容那一刻，前面那些 draw 已经把
`vkCmdBindVertexBuffers(旧 buffer)` 录进去了，旧 buffer 却在这时被
`vkDestroyBuffer/vkFreeMemory` 当场销毁：显存立刻回到池子，紧接着的新 buffer 分配
**大概率拿到同一段显存**（而且被 memset 成 0）。于是扩容之前录好的所有 draw 都变成
"所有顶点都在原点"的退化三角形 —— **整块内容消失，不报错、不崩溃**。索引缓冲同理。

整页渲染（binUI/bin2d 一帧几千个四边形）必然扩容，所以只有"内容多"的页面坏，
`circle`/`opacity` 这种几个四边形的页面反而看不出问题。

修法：扩容时旧 buffer 只**退役**（`VkCmdObj::retired`），真正的
`vkDestroyBuffer/vkFreeMemory` 推迟到下一趟 `beginRecording`（此时上一帧已经
`vkWaitForFences` 等完）。顶点数据不必往新 buffer 抄：每笔 draw 绑的是它录制时
那一个 buffer。

### ② 整个 cmd 共用一个 descriptor set

descriptor set 的内容是**执行期**读的，不是录制期快照。原来 `createCmd` 只分配
**一个** set，两条 draw 之间 `vkUpdateDescriptorSets` 改写它 —— 按规范这是
"已提交的 set 被改写"，所有 draw 都可能看到**最后一次写入**的那张纹理。

修法：按笔分配（`allocDescSet`，每池 512 个、池满再开一个），帧首
`vkResetDescriptorPool` 整批回收（`resetDescSets`），set 数量按"一帧的笔数"走，
不随帧累加。

> 诚实说明：**这一条在 RTX 4060 Ti + 这版驱动上没有改变任何像素**（改前改后
> 同一帧的 PNG 逐字节相同），上面那张表的 86.9% 全由 ① 造成。它是"顺手修掉的、
> 明确违反规范的隐患"，不是本页缺陷的根因 —— 但换一个驱动/后端就可能变成真 bug。

### ③ 同一目标连开多趟渲染时的依赖

动态渲染（`vkCmdBeginRendering/vkCmdEndRendering`）**不插**任何依赖；而
`transition()` 在"布局没变"时会直接跳过 barrier，于是"第一趟写目标、第二趟 load
同一个目标"之间一条依赖都没有。另外采样源原来只在"换纹理"时才转布局，
`last_tex` 缓存命中就漏转（目标刚渲染完布局还是颜色附件）。

修法：`begin_render` 对目标**强制**插一条颜色附件的 RAW 依赖
（`transitionImpl(..., /*force=*/true)`）；采样源每笔都确保在
`SHADER_READ_ONLY_OPTIMAL`（布局相同就跳过，不会白插）。

> 同 ②：**这一条在本机也没改变像素**（动态渲染下 NV 的执行是够"顺"的）。但它按
> 规范是必须的 —— 换一台 GPU / 一个驱动就可能出现"上一趟内容读不到"。

### ④ 顺手：VK 的 mip 从"建不出来"改成"降级"

`BGPU_TEXTURE_MIPMAPS` 以前在 VK 上直接 `UNSUPPORTED`，而 CPU/GL/D3D11 都是
"接受但降级为 1 级"。同一个上层代码（binUI 的贴图）于是在 GL 上正常、在 VK 上连
纹理都建不出来 —— 设备层的"做不到"必须表现成**降级**，不是**失败**。
（真正的 mip 采样仍是欠账，见 `docs/backends.md` §欠账 9。）

### 验收

- `tests/gpu_parity.h` 新增 `multipass(目标来回切 + 顶点流扩容)`：先画一个块、再画
  1200 个四边形把顶点流撑过扩容点、然后把刚渲染完的目标当采样源抓一张、再贴回目标。
  把 ① 的实现改回"当场销毁"，这条用例在 VK 上立刻失败（`texture_read` 都回读不了）；
  GL / D3D11 / CPU 一直是绿的。
- `mingw32-make test`：ABI 247 / CPU 378 / pace 72 / prims 32 / shape 23 /
  VK 16 / D3D11 16 / GL 16，**失败 0**。
- binUI 的四个页面（`circle` / `opacity` / `svg-filter` / `appshow` 整页）GL vs VK
  逐像素 **0 差异、最大通道差 0**。

## [0.12.1] - 2026-11-20

**修掉"形状与 2D 四边形交错"时暴露的两个 bug —— 都是 0.12.0 新代码的锅，都被 bin2d 的
跨执行体对拍抓到。**（ABI 不变。）

### ① Vulkan：顶点流按不同的 stride 共用，`first_vertex` 被截断

VK 的顶点流是一条按帧增长的字节缓冲，2D 顶点 24 字节、形状顶点 36 字节**追加在同一段**
里，而 `first_vertex = vused / stride` 是按本笔的 stride 倒算的。24 与 36 互不整除：

- 先画一个 2D 四边形（96 字节）再画形状 → `96 / 36 = 2`，顶点从第 **72** 字节开始读，
  读到的是上一笔的尾巴 ⇒ **整个形状不见**；
- 反过来，形状画了奇数个（36·n）再画四边形，`n` 为奇数时同样截断。

修法：追加前把游标**向上对齐到本笔的 stride**（`base = roundUp(vused, stride)`），
`first_vertex = base / stride` 于是整除。两条 draw 路径都这么做了。

症状很刁：单独画形状（对齐在 0）永远正常，只有交错时才有，而且是"一整条圆角消失"。
bin2d 的 display_list 跨执行体对拍里是"VK 上上半透明圆角渐变一整块不见了"（2954 像素）。

### ② OpenGL：形状画完没把 quad 的程序装回去

GL 的程序是 `begin_render` 时 `glUseProgram(d.prog)` 一次就不管了的，两条 draw 路径都
假定"当前程序就是 d.prog"。`rasterShapes` 换成 `prog_shape` 之后没有还原，于是下一笔
`draw_quads` 拿形状的程序去解 2D 顶点 —— 驱动报 `INVALID_OPERATION`（0x1282），整笔
失败；就算不报错，像素也是错的。

修法：`rasterShapes` 画完 `glUseProgram(d.prog)`。顺带把 `rasterQuads`/`rasterShapes` 里
`glFail()` 的失败结果**直接返回**（不再包一层 `fail(...)`）：那个消息里带着 `glGetError`
的具体码，外面再包一层就把码覆盖掉了 —— 排查时最想看的恰好是它。

### 验收

- `tests/test_shape.c` 新增 5 项"**混画**"：同一条命令缓冲里 2D 四边形与形状交错
  （4 笔，让 `vused` 在 96/240/336/480 这些非 36 倍数上各过一遍），四端逐像素对齐 ——
  `vulkan`/`d3d11`/`gl` 都是 **差 >1 的通道 0 个，最大 0**；
- 全套 8 个测试通过（test_shape 23 项）；
- bin2d 的 `test_gpu_bridge`（40 项）与 `test_display_list`（45 项）全绿。

## [0.12.0] - 2026-11-20

**解析圆角进设备层：圆角的抗锯齿数学从 CPU 搬到**像素着色器**里算。**（ABI 4）

起因是一句追问：「怎么还在用 CPU，CPU 数学还没改用 GPU 吗」。上一版（bin2D 0.19.x 起）的做法是
CPU 用 8 条子扫描线积分出一张 `RoundedTileSize×RoundedTileSize` 的覆盖率位图，上传成纹理，
GPU 只做九宫格采样 —— AA **数学**在 CPU，GPU 只是搬运。这一版把那 6 行公式挪进片元着色器。

### 新增 ABI（全部是**追加**，既有结构体/枚举值/函数签名一个字节没动）

```c
#define BGPU_VERTEX_SHAPE   2u          /* 第三种顶点布局 */
#define BGPU_PIPE_SHAPE     (1u << 5)   /* 必须与 BGPU_VERTEX_SHAPE 成对出现 */
#define BGPU_CAP_SHAPE      (1u << 12)  /* 这个后端能解析地画圆角 */

typedef struct BgpuShapeVertex {        /* 36 字节，静态断言钉住 */
    float x, y;        /* 目标像素位置 */
    float lx, ly;      /* 相对盒子中心的局部坐标（顶点处 = ±hx, ±hy） */
    float hx, hy;      /* 盒子半尺寸 */
    float radius;      /* 圆角半径 */
    float thick;       /* 0 = 实心；> 0 = 向内环带（边框）的厚度 */
    uint32_t tint;     /* 0xAARRGGBB */
} BgpuShapeVertex;

BgpuResult bingpu_cmd_draw_shapes(BgpuCmd cmd, const BgpuShapeVertex* verts, uint32_t quad_count);
```

顶点顺序沿用 `raster_rules §2`：`TL, TR, BL, BR`，索引 `0,1,2 / 2,1,3`。

### 片元里算的就是这 6 行

```glsl
q   = abs(p) - half + r;
d   = length(max(q, 0)) + min(max(q.x, q.y), 0) - r;
cov = clamp(0.5 - d, 0, 1);                   // 实心
cov = clamp(0.5 - abs(d + t/2) + t/2, 0, 1);  // 环带（厚度 t）
```

**没有 `fwidth`。** 轴对齐 1:1 的映射下，局部坐标 1 个单位 = 屏幕 1 个像素，圆角矩形的距离场
是 1-Lipschitz 且 `|grad d| ≡ 1` —— 一个像素内 `d` 恰好跨过 1，覆盖率随 `d` 线性下降，中点落在
`d = 0` 的边界上，于是 `0.5 - d` 就是**精确解析**覆盖率。加导数不但没好处，还会变差：导数在
2×2 quad 上是分块常数，小半径的弧上会退化成方块；而解析式让 CPU 参考后端能逐位对上。

### 四个后端都实现了（这是这一版的重点）

| 后端 | 做法 | 与 CPU 参考后端 |
|---|---|---|
| CPU（参考实现） | `shape_coverage()` 逐像素同一套式子 | 权威 |
| OpenGL | GLSL 330 运行期编译；编不出来就**不报** `CAP_SHAPE` | 最大 1 级，>1 的通道 0 个 |
| Vulkan | `shape.vert/frag` → glslc → SPIR-V 入库；模块建不出来就不报 `CAP_SHAPE` | 最大 1 级，>1 的通道 0 个 |
| D3D11 | `quad.hlsl` 的 `vs_shape`/`ps_shape` → fxc → DXBC 入库；第三套输入布局 | 最大 1 级，>1 的通道 0 个 |

四个后端都是**能力位门控**：没有 `BGPU_CAP_SHAPE` 时调用方退回覆盖率模板（0.10.0 那条路），
不会半残、不会崩。

### 验收（`tests/test_shape.c`，18 项）

- CPU 参考后端的左上角区 **15×15 逐像素** vs 闭式 SDF 公式：**最大差 0**（含 19 个半覆盖格）；
- 三个 GPU 后端 vs CPU：**差 >1 的通道 0 个，最大 1**；
- 环带（`thick=4`）：中心是洞、四边满覆盖；
- 精度对比（16×16 超采样当参考）：**解析 AA 最大差 10~13、平均 0.1**；上一版 8 子采样扫描线
  模板最大差 16~40、平均 1~3 ⇒ 搬到 GPU 之后不但没变糙，本身也更准。

### 已知边界（写在这里免得日后误用）

- 只对**轴对齐、1:1、整数平移**的圆角矩形是精确的。旋转/缩放时局部坐标会被非均匀拉伸，
  而距离场不知道这件事 ⇒ 那条路上层仍应走覆盖率模板；
- 与 bin2D/binUI 的接线在各自仓库（bin2D 0.21.0 起在有 `CAP_SHAPE` 时发 `draw_shapes`）。

## [0.11.1] - 2026-11-20

**修掉 CPU 参考后端的一个 1px 空洞：共享边上的像素被两个三角形同时丢掉。**
（ABI 不变，纯 bug 修复。）

### 现象与成因

四边形沿对角线切成的两个三角形，在共享边上的像素会**两边都不画**。复现条件很窄但确定：
`168×72` 的视口、`box(8,8,120,48) r=14` 的圆角矩形，第 `(8,21)` 与 `(9,20)` 两个像素全透明。

根因是浮点往返：顶点经过"设备坐标 → 视口除法 → 乘回"（`transform_vert` 里的
`in.x / tgt.w * vp[2]`），`22/168*168` 不精确等于 `22`。于是共享边上的像素算出的边函数是
`±1e-16` 而不是 0，而 top-left 规则的判据是 `e > 0 || (e == 0 && tl)` —— 两个三角形各得
一个"极小负数"，**都**把像素丢掉。换成 2 的幂宽的视口（`32`、`256`）往返精确，就复现不出来。

修法：`edgef()` 把 `|e| < 1e-6`（≈1e-7 px，远小于一个像素）**吸成精确 0**。吸成 0 之后
判据重新是确定的：恰好一侧的 top-left 为真，像素只被覆盖一次 —— 不会变成"两边都画"。

### 验收

- `tests/test_cpu_render.c` 378 项全过（含 `seam_top_left`）；
- bin2d 的跨执行体对拍里，`168`/`256`/`128`/`320` 四种视口宽度 × 半径 4..24 的圆角矩形，
  Vulkan / D3D11 / GL 与 bin2d 的 CPU 光栅**逐像素相同（max 0）**。

## [0.11.0] - 2026-11-20

**新增 `bingpu_cmd_draw_rounded_rect_grad()`：四角带色的圆角矩形。**
（ABI 不变，纯追加 API；渲染目标/顶点格式/管线全部没动。）

```c
BgpuResult bingpu_cmd_draw_rounded_rect_grad(BgpuCmd cmd, BgpuTexture coverage,
                                             float x, float y, float w, float h,
                                             float radius, int32_t tile_size,
                                             const uint32_t corner_tints[4]);  /* TL,TR,BL,BR */
```

### 为什么快路径里需要"四角带色"，而不是画两次

圆角底的渐变不能"先画方渐变、再拿模板抠"（没有 stencil，也没有第二次 pass）。
而线性渐变在屏幕空间就是一个**仿射函数**，仿射函数被四边形上的线性插值精确复现
—— 于是"一个九宫格 + 四角带色"就是**精确解**：九块之间不会出现色阶接缝，
也不需要为了渐变把圆角拆成条带。九宫格的块边界（角片边长 = r、中段 UV 取模板正中
1 个像素）是这一层的知识，所以调用方只描述"四角什么颜色"。

顶点色口径不变：`corner_tints` 是 **0xAARRGGBB 直通**（同上层的 `draw_quads`：
`frag.rgb = tex.rgb · vcol.rgb · vcol.a`）。这一层不认识"渐变"—— 它只做
"四色在矩形上双线性插值"，那是纯几何。**顶点在盒子四角上精确取回传入值。**

注意这条口径的边界：直通域插值只在"四角来自一个仿射函数"时才是那个函数。
随手编的四个角（比如对角同色、另两角同色）不是任何线性渐变，光栅化出来自然对不上
—— 那不是 bug，是拿错了输入。

### 验收（`tests/test_prims.c` 21 → 32 项）

| 判据 | 结果 |
|---|---|
| 四角**同色**时与 `draw_rounded_rect` 逐字节相同 | 0 差异像素（64×64 全图） |
| 四角不同色 vs 盒子上的仿射函数，1064 个内部像素 | 最大通道差 **1** |
| 盒子外一个像素都不许动 | 通过 |
| `radius <= 0` / `corner_tints == NULL` | `BGPU_ERR_INVALID_ARGUMENT` |

## [0.10.0] - 2026-11-20

**新增 `bingpu_cmd_draw_rounded_rect()`：圆角矩形的九宫格几何收进设备层。**
（ABI 不变，纯追加 API。）

### 分工

```c
BgpuResult bingpu_cmd_draw_rounded_rect(BgpuCmd cmd, BgpuTexture coverage,
                                        float x, float y, float w, float h,
                                        float radius, int32_t tile_size, uint32_t tint);
```

- **AA 数学在调用方**：圆角边缘长什么样是绘制语义，所以 `coverage` 是调用方给的
  覆盖率模板（bin2d 的 `RasterizeRoundedTileCoverage` 是唯一一份解析式）。仓里
  GL / VK / D3D11 三份后端曾经各写一份一模一样的距离循环，改一处就差一圈像素。
- **拆几块、怎么合批、走哪个后端在这一层**：一次 `draw_quads` 把 t×t 模板铺成 9 块
  （四角 1:1、四边单轴拉伸、中心双轴拉伸，中段 UV 取模板正中 1 个像素）。
  因此五个后端（CPU / VK / GL / D3D11 / NULL）**零改动**就能画，像素也天然一致。

### 为什么是"一层一层调"而不是一次画完粗描边

粗描边 = 同心 1px 环叠层，**每一层半径不同、要一张不同的模板**；模板缓存归调用方
（只有它知道半径取整与缓存键）。所以这一层只画"一层"，多层由调用方连着调 ——
它负责的是"怎么把一层变成四边形"。

### 批量版：`bingpu_cmd_draw_rounded_rects()`

同一张模板、一次绑定、**一次 draw_quads** 画 N 个圆角矩形（每项自己夹半径，退化的项跳过）。

合批为什么是这一层的事：234 个卡片各调一次就是 234 次绑纹理 + 234 次顶点提交。
实测（1020×684、234 个约 100×32 的圆角矩形，5 次中位）：

| 路径 | 时间 |
|---|---|
| bin2d 的 CPU 直填（参考） | 1.43 ms |
| 本层九宫格 · 逐个调用 · D3D11 | 1.37 ms |
| 本层九宫格 · **批量** · D3D11 | **0.38 ms** |
| 本层九宫格 · 逐个/批量 · **CPU 后端** | 19.9 / 20.2 ms |

两个结论：
- 真 GPU 上批量把 1.37 拉到 0.38（3.6×），所以"合批放这一层"是有收益的；
- **本层的 CPU 后端是参考实现，不是性能执行体**（同负载比 bin2d 的 CPU 光栅慢 14×）。
  这也印证了 layering 文档里那句"CPU 执行模式应该让 bin2d 的光栅来当，而不是在这里
  再写一份快路径"。

### 测试（`tests/test_prims.c`，21 条断言）

钉的都是几何，不碰 AA：
- 模板全 255 时，**矩形内每个像素恰好等于 tint、外面一个都不许动**（九宫格算错一格就露）
- 带坐标编码的模板反查 UV：角片 1:1、中段取直边段、四角/中段/边缘各抽一个点
- 半径夹到 min(w,h)/2；`radius <= 0` / 尺寸为 0 / 模板为空都要报错

## [0.9.0] - 2026-11-20

**新增帧节拍器 `bingpu_pace_*`：把"这一拍该不该画"收进 device 层**；
并修掉 **GL 后端的 `BGPU_DEVICE_VSYNC` 从来是个空操作**（ABI 不变，纯追加 API）。

### 为什么节拍器在这一层

master 2026-11-20：「按理说这种绘制请求 GPU 应该能画个几百上千帧才对。防止使用率过高，
性能钳制才用的节拍器（即达到屏幕或系统刷新率即可）。这个性能钳制在哪做呢？我感觉是在
bin2D/3D 这里，当然做成可调的最好：标准/无限制/240/120/90/60/30 等」，以及
「标准 = 跟着合成帧节拍或屏幕刷新率：路性能不够时尽可能画多点帧，性能超过时钳制在系统合成帧率」。

先量出来的事实是：**帧率不是画出来的上限**。1020×684 一帧的 present+draw 只要 0.5–1.0 ms：

| 后端 | vsync=0 裸跑 | vsync=1 |
|---|---|---|
| Vulkan | 966–991 fps | 120.0 fps |
| D3D11 | 1846–1854 fps | 119.0 fps |
| OpenGL | 120.0 fps（**bug**，见下） | 120.0 fps |

上千帧的算力一直摆在那里，被节拍器掐成了 55/31/100。而掐拍需要的判据全在设备层：
刷新率、present mode、present 有没有阻塞到垂直同步、上一拍 present 真实耗时 ——
上层猜这些就是 0.8.0 里"present 节奏不稳定"的来源（见下）。

所以：**策略在上面（bin2D/bin3D 说"最多多少帧"），闸门在这里。**

### 新增

```c
typedef enum BgpuPacePolicy {          /* standard / unlimited / 240 / 120 / 90 / 60 / 30 */
    BGPU_PACE_STANDARD = 0, BGPU_PACE_UNLIMITED = 1, BGPU_PACE_240 = 2, ...
} BgpuPacePolicy;

void      bingpu_pace_init(BgpuPacer* pacer, void* native_window);  /* 探测刷新率 */
BgpuResult bingpu_pace_set_policy(BgpuPacer*, BgpuPacePolicy);
double    bingpu_pace_frame_ms(const BgpuPacer*);      /* **生效**周期；0 = 不掐拍 */
int64_t   bingpu_pace_wait_us(const BgpuPacer*, int64_t now_us);
void      bingpu_pace_frame_begin(BgpuPacer*, int64_t now_us);
void      bingpu_pace_frame_end(BgpuPacer*, int64_t end_us, double present_ms);
int       bingpu_pace_wants_vsync(const BgpuPacer*);   /* 目标 > 刷新率时必须关 */
int       bingpu_display_refresh_hz(void* native_window);
int       bingpu_pace_policy_parse(const char* text, BgpuPacePolicy* out);
```

`BgpuPacer` 是调用方持有的一块 POD（没有资源、没有生命周期），时间由调用方注入 ——
所以整个节拍逻辑可以离线单测（`tests/test_pace.c`，72 条断言，喂任意时间序列）。

规则一句话：**下一拍 = max(本拍开始 + 目标周期, 本拍结束)**。析出三条：

- present 自己在等刷新时，相位从**本拍结束**起算（`end + (k-1) 格`）——
  它返回的时刻本来就是刷新边界，从这里数才不会和刷新打拍子；
- `标准` **不向上凑整**：画不完就尽力多画，不为了"整数个刷新周期"退成 60 fps；
- 只有明确要 60/30 时才凑整；`90` 落在 120 Hz 屏上实际是 120（报的是生效周期）。

### 修复

- **GL：`wglSwapIntervalEXT` 从来没被装载过**（`bingpu_gl.cpp` 只声明、没进
  `loadGlFunctions` 的装载表），于是 `BGPU_DEVICE_VSYNC` 位是个空操作：
  显式传 `vsync=0` 也仍然每帧等刷新 —— 实测卡在 120.0 fps / 8.335 ms，
  装载之后同一个 context、同一个窗口 **801 fps / 1.206 ms**。
- **`tests/test_abi.c` 里的版本断言写死成 0.6.1**，从 0.7.0 起 `make test` 就一直是红的
  （静默红了三个版本）。改成校验"三个宏与字符串一致"，以后再也不会过期。

### 撤回 0.8.0 的"present 节奏不稳定"诊断

0.8.0 那张表（VK 112/55 两档、D3D11 113/31 两档）把原因记成"交换链在飞帧数没限制"。
量到底之后：**三端在 vsync=1 下裸跑都是 119–120 fps / 8.34 ms**，present 本身没有问题。
真正的两个原因：
1. GL 关不掉 vsync（上面那条）；
2. 上层的外壳和 present **互相等**（present 等一格、外壳再补一格）。

0.8.0 提到的验收标准（"连跑 5 次 present 方差 < 10%"）仍然留着，但现在它由
`bingpu_pace_*` 而不是交换链来保证。
## [0.8.0] - 2026-11-20

**新增 `bingpu_render_plan()`：把"用哪个 API、在哪执行、什么质量档"收进 binGPU。**
（ABI 不变，纯追加 API。）

### 为什么

master 2026-11-20 纠正过一次分层：「应该是 binGPU 来决定怎么绘制、用什么绘制来保证绘制
性能和质量吧？binUI、bin2D/3D 决定的是功能。」所以上层的每一层都不该再自己按
`vk/gl/d3d` 挑后端 —— 该问这一层要一个**计划**：

```c
BgpuRenderPlan p;
bingpu_render_plan(BGPU_BACKEND_AUTO, &p);
/* p.backend = VULKAN / GL / D3D11 / CPU，p.exec = GPU / CPU，p.msaa_samples = 质量档 */
```

- 优先级：**VULKAN → GL → D3D11**，一个都不可用才 `exec = CPU`、`backend = CPU`
  （也就是"没显卡，用 CPU 算"）。
- `want` 可以钉住某个后端（对照构建 / 金标准 / 排查）。
- `exec` 这一轴是为下一版准备的：同一套 2D 图元语义既能在 GPU 上求值，也能在 CPU 上
  用同一份解析式求值，于是"换执行体不换像素"，质量由同一份数学保证。

### 实测（`build-mingw/tmp/plan_probe`）

```
auto  -> backend=vulkan exec=gpu msaa=4
cpu   -> backend=cpu    exec=cpu msaa=1
gl    -> backend=gl     exec=gpu msaa=4
```

**已知问题（下一版修，属于本层职责）**：三个 GPU 后端的**呈现节奏不稳定**。
同一台机器、同一个页面，连续跑出来的 present 耗时会落在不同档上：

| 后端 | present（同页、多次） | 说明 |
|---|---|---|
| GL | 1.42 ~ 1.48 ms 稳定，100 fps | SwapBuffers 不等，节奏交给上层 |
| Vulkan | 8.08 ms（1 拍）**或** 17.7 ms（2 拍），112 / 55 fps 两种 | FIFO 队列深度在 1/2 张图上摆 |
| D3D11 | 0.30 ms（不阻塞）**或** 31.5 ms（3.8 拍），113 / 31 fps 两种 | 同上，队列满时阻塞 |

同一段代码两次跑出 4 倍的差距，说明**队列里在飞几帧没被约束住**。修法方向：
交换链固定"最多 1 帧在飞"（acquire 时等 fence / 用 MAILBOX 或按 present mode
显式协商），并在 present 后回收。这一条的验收是"连续 5 次跑同一个页面，present
耗时的方差 < 10%" —— 现在做不到。

---
## [0.7.0] - 2026-11-20

**上传可以声明"数据已经是预乘"**，省掉一次整幅转换（ABI 不变，仍为 3）。
新增一个 `BgpuTextureDesc::flags` 位，属于"解释方式"的变化，不影响任何结构体布局。

### 起因

bin2d 的 `Surface` 本来就是预乘 RGBA。以前上屏要把它**反预乘**成直通喂给 bingpu，
后端上传时再**预乘**回来（`premultiply()`）——一趟往返，纯浪费且有舍入。1020×684 的一帧：

| | 改前 | 改后 |
|---|---|---|
| 上传（含转换） | 4.79 ms | **0.10 ms** |
| present 的 CPU 工作 | 5.09 ms | **0.59 ms** |

### 改动

- `BGPU_TEXTURE_PREMULTIPLIED = 1u << 4`：纹理内容已是预乘。三个后端在整幅与局部上传时
  都跳过 `premultiply()`；CPU 参照后端同样认这个位（否则 GPU 与参照会分叉）。
- 顺带修掉上传路径的中转缓冲：行紧凑时 GL/VK **不再分配** 2.8 MB 中转再整幅 memcpy，
  D3D11 直接把调用方指针交给 `UpdateSubresource`（连行拷贝都省）。
- `present_paces_frames` / `BGPU_DEVICE_VSYNC` 现在真的被上层用上了（bin2d 0.14.0 的
  `GpuCompositor::createWindow(..., vsync)`），这里只是确认语义：**声明**的是交换链模式，
  驱动是否真的阻塞由 backend 决定，上层不要只凭这一位判断"能否掐拍"。

### 门禁

bin2D `make test` 全绿（test_gpu_bridge 20 项，新增"预乘直传不多预乘"四条，四端用读回值
判定：正确读出 (20,40,60)，多预乘一次会变 (10,20,30)）；binUI `make pixels-bin2d` 18/18
容差 0、`pixels` / `pixels-vk` / `pixels-d3d11` 全绿 —— **像素 ±1 一个都没有**，因为
Surface 的预乘与 `premultiply()` 是同一个整数公式 `(c·a+127)/255`。

---
## [0.6.1] - 2026-11-19

**三个 GPU 后端补上 ABI 3**（0.6.0 只落了 CPU 参考实现）。ABI 不变（ABI 3），
变的只是"自报不支持"变成"真的支持"：VK / D3D11 / GL 的 caps 现在都带
`BGPU_CAP_LIGHTING | BGPU_CAP_PERSPECTIVE`。

### 各后端做了什么

| 后端 | VS | PS/FS | 光照参数 |
|---|---|---|---|
| Vulkan | 5 个输入（+normal@3、w@4），四个插值量标 `noperspective`；`gl_Position.w` 恒 1 | `Σ(attr·iw)/Σ(iw)`；Lambert | push constant 64 → **112 字节** |
| D3D11 | 拆成 `vs`(2D) / `vs_mesh` / `ps` 三个入口（input layout 必须覆盖 VS 全部语义）；`il_mesh` stride 40 | 同上 | cbuffer 64 → 128 字节 |
| OpenGL | 加 attrib 3/4，2D 路径显式 `glDisableVertexAttribArray(3/4)`（取默认 0 → iw=1，与 ABI 2 逐位相同） | 同上 | `glUniform3fv` |

### 一个三端都踩到的坑：GPU 帧缓冲写入的 `.5` 是向下收的

CPU 的 q8 是 `(int)(v·255+0.5)`（`.5` 向上），而三个后端的 float→UNORM8 在平局处
**向下**：实测 0.1/0.3/0.5 写进 RGBA8 得 25/76/127。环境光 0.1 正好踩在平局点上
（`0.1×255 = 25.5`），于是同一份光照公式 CPU 给 26、GPU 给 25。

第一版的处理是"把测试的 ambient 换成 0.12 绕开"——那是回避。真正的修法是让片元在
光照分支里**先按 CPU 的口径量化到 8 位**（`floor(v*255+0.5)/255`）再交给写入路径，
之后误差 < 1/255 LSB，两种舍入规则落到同一个整数。三端都这么改了，测试的 ambient
也回到 0.1 —— 这条断言现在同时盯着"光照公式"和"量化口径"两件事。

另两个同类的小坑：**不采样纹理时不要绑白纹理凑数**（UNORM 白采样回来是 0.99999994，
同样会把 .5 边界压下去），按 `flags.y` 直接返回精确 (1,1,1,1)；**插值量必须标
`noperspective`**，否则硬件先做一遍自己的 1/w，恒定属性插值出 0.99999994。

### 用例收口

`lighting` / `perspective` 两个场景从 VK 的 test 搬进了 `tests/gpu_parity.h` ——
一份场景，三端自动覆盖，0 容差。判据与 `test_cpu_render.c` 的手算值同源
（255 / 26 / 206，u=0.5625 vs 0.2432）。

### 验证

```
test_abi          247 ✓
test_cpu_render   378 ✓（含 [lighting] / [perspective]）
test_vk_render     15 ✓（13 + ABI 3 两项，三端全 0 差异）
test_d3d11_render  15 ✓
test_gl_render     15 ✓
```

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
