# tests

五个测试都是**纯 C 源码**（故意的：它们同时是"C 能消费这个 ABI"的证明），编成可执行文件后
链接 `build-mingw/libbingpu.a`（实现是 C++，所以链接由 `g++` 做，带 libstdc++；见
`docs/abi.md` §0）。没有第三方框架：断言就是 `CHECK` / `CHECK_EQ`（`printf` + 计数），
失败时退出码 1。

```sh
make test                     # 编 + 跑四个，任何失败非 0 退出
build-mingw/bin/tests/test_abi
build-mingw/bin/tests/test_cpu_render
build-mingw/bin/tests/test_vk_render      # 没有可用 Vulkan 时整份跳过（exit 0）
build-mingw/bin/tests/test_d3d11_render   # 没有可用 D3D11 时整份跳过（exit 0）
build-mingw/bin/tests/test_gl_render      # 没有可用 GL 3.3 时整份跳过（exit 0）
```

## `test_abi.c` —— 契约

钉的是"绑定作者会踩的坑"，不碰像素：

- 版本 / ABI 号 / `result_name` / `format_bytes` 的取值；
- `struct_size` 协议：填小了必须返回 `BGPU_ERR_INVALID_ARGUMENT`（不是越界写）；
- 0 句柄 / 已销毁句柄 / 已销毁设备名下的句柄 / 种类串用 / 跨设备的资源；
- 状态机：未 `begin_render` 就 draw、重复 `begin_render`、帧里还在 render 就 `frame_end`、
  `draw_indexed` 用错顶点布局或非对齐偏移；
- NULL 后端的**完整调用序列**（frame / begin_render / draw_quads / draw_indexed）与计数；
- 错误串形状（非空、带函数名、带 `file:line`）、成功不清空错误槽、`clear_error`；
- **线程本地错误槽**：两个线程各失败一个不同函数，互不覆盖（Windows 下用
  `CreateThread` + 事件同步；其他平台跳过这一节并打印 skip）；
- `device_destroy` / `shutdown` 幂等。

## `test_cpu_render.c` —— 像素

每一条断言的期望值都是**手算**的（注释里带算式），不是"跑一遍抄下来"的：

| 用例 | 钉住什么 |
|---|---|
| `clear` / `clear_alpha` | clear 逐像素值；预乘存储 + 反预乘回读的往返 |
| `quad_coverage` / `quad_fractional` | 像素中心覆盖范围（整像素、无 AA） |
| `seam_top_left` | 相邻四边形共享边只覆盖一次（半透明拼接不出 191 的缝） |
| `blend_modes` | 五种混合各一个期望值（含"存储是预乘、读回反预乘"两步推导） |
| `sampler_nearest_repeat` | 最近采样 + REPEAT（UV 1.25 → 纹素 0）与 CLAMP 的差异 |
| `sampler_bilinear` | 双线性在纹素中心约定下的两个具体取值 |
| `scissor` / `viewport` / `affine` | 裁剪、缩放平移、2×3 矩阵（平移/缩放/复位） |
| `offscreen` | 离屏渲染 → `texture_read` → 当纹理画到交换链 |
| `depth` | LESS + 深度写；"只测不写"的语义 |
| `texture_roundtrip` | 直通上传 → 预乘存储 → 反预乘回读（可逆取值 + 行距） |
| `r8_mask` | R8 遮罩纹理采样成 `(v,v,v,v)` |
| `mesh` | `draw_indexed` 光栅化（三角形内部/外侧/贴边） |
| `bgra_swapchain` | BGRA8 交换链的 `frame_read` 仍输出 RGBA 顺序 |
| `stats` | draws / quads / triangles / frames_presented / bytes_uploaded / bytes_live |
| `resize` | 交换链 resize 后照常出图（借出的目标纹理跟着换） |

## `gpu_parity.h` —— GPU 后端 vs CPU 的**共用**用例表

`test_vk_render.c`、`test_d3d11_render.c`、`test_gl_render.c` **各只有一行 `main`**，
用例、判据、容差全在 `gpu_parity.h` 里。加第 N 个 GPU 后端 = 再写一行；用例表不用动 —— 这是
"一个定义 + 四份实现 + 一套门禁"落到实处的地方（`docs/layering.md` §2 的 P1）。

| 用例 | 钉住哪条规则 |
|---|---|
| `clear` | 清屏值 + 预乘存储 + 反预乘回读往返 |
| `quad_replace` | 像素中心覆盖范围 |
| `blend`（五种混合横排） | `raster_rules` §8 的整张混合表 |
| `seam` | top-left 规则下相邻四边形共享边只混一次 |
| `scissor` / `viewport` / `affine` | 裁剪、目标相对坐标 + viewport 映射、屏幕空间 2×3 |
| `texture(linear/clamp)` | 双线性取样（**唯一带容差的用例**：CPU double 累加 vs GPU float 插值） |
| `texture(nearest/repeat)` | 最近取样 + UV 绕回 |
| `texture(box 局部上传)` | 局部上传也要预乘（0.4.0 抓到 VK 的偏差） |
| `depth(LESS)` | 深度测试 + 写入，"相等不通过" |
| `mesh(draw_indexed)` | 网格顶点布局（36 字节，零转换） |

判据：默认**逐像素 0 差**；只有上面那条双线性用例放宽到"差异像素 ≤ 25% 且通道差 ≤ 2"。

## 加新用例的规矩

1. 期望值必须能从 `docs/raster_rules.md` 的公式**手算**出来，注释里写算式；
2. 需要新后端对齐的行为，先改 `raster_rules.md`，再改 CPU，最后加断言；
3. 别用"容差"绕过：CPU 后端是确定性的，差 1 就是规则没对齐；
4. GPU 的用例只往 `gpu_parity.h` 里加（**不要**在某个后端的 `test_*.c` 里单写一份），
   否则"两个 GPU 后端用不同的规格验收"这件事会悄悄发生。
