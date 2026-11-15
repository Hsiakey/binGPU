# tests

两个测试都是**纯 C11 可执行文件**，直接链接 `build-mingw/libbingpu.a`，没有第三方框架：
断言就是 `CHECK` / `CHECK_EQ`（`printf` + 计数），失败时退出码 1。

```sh
make test                     # 编 + 跑两个，任何失败非 0 退出
build-mingw/bin/tests/test_abi
build-mingw/bin/tests/test_cpu_render
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

## 加新用例的规矩

1. 期望值必须能从 `docs/raster_rules.md` 的公式**手算**出来，注释里写算式；
2. 需要新后端对齐的行为，先改 `raster_rules.md`，再改 CPU，最后加断言；
3. 别用"容差"绕过：CPU 后端是确定性的，差 1 就是规则没对齐。
