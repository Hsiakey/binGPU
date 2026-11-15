# 光栅规则（raster rules）

> 这份文档是 **CPU 参考实现的可执行规格**：`tests/test_cpu_render.c` 里每一条断言都对
> 应这里的一条规则。GPU 后端（Vulkan / D3D12）写完之后，验收方式就是"同一份命令，
> 和 CPU 出图逐像素比" —— 所以这些规则必须**先**定下来，而且必须能用手算验证。

本文出现的所有坐标都是**目标像素空间**：左上角 (0,0)，x 向右，y 向下，单位是像素。
像素 `(x, y)` 的中心是 `(x + 0.5, y + 0.5)`。

---

## 1. 顶点 → 屏幕

输入顶点 `BgpuVertex{x, y, z, u, v, color}`，其中 `(x, y)` 是"目标相对坐标"：

```
sx = vp.x + (x / target_w) * vp.w
sy = vp.y + (y / target_h) * vp.h
```

- `viewport = (0, 0, target_w, target_h)`（默认值）时是恒等映射 —— 也就是"顶点坐标就是
  目标像素坐标"。这种恒等是有意的：binUI 那层发出来的就是像素坐标，不需要一层换算。
- viewport 不是恒等时，顶点先按目标尺寸归一化、再缩放/平移进 viewport 矩形 —— 与 GPU 的
  NDC→viewport 是同一个式子（`ndc = p/target*2-1`，`px = (ndc+1)/2*vp.w + vp.x`）。
- **viewport 不裁剪**。几何落在 viewport 外只是被映射到目标边界内/外，真正裁剪像素的是
  scissor（§3）和目标边界。这一点与 GPU 一致，也是"最小惊讶"的写法。

### 仿射

`cmd_set_affine(xf)` 打开后，对**已经过 viewport 映射**的屏幕坐标再作用一次 2×3 矩阵：

```
sx' = xf[0]*sx + xf[1]*sy + xf[2]
sy' = xf[3]*sx + xf[4]*sy + xf[5]
```

顺序（viewport → affine）是 ABI 头里写定的。选这个顺序的原因：binUI 的用法是
"节点局部坐标 + 一个世界矩阵"，viewport 恒等时两者等价；而不是恒等时，先 viewport 再
affine 才能让"整块内容缩放/旋转"（矩阵作用在屏幕空间）自然成立。

z 不参与这两步：`z` 直接进深度测试（§7）。

## 2. 四边形拆三角形

`cmd_draw_quads` 每 4 个顶点为一块，顺序 **TL, TR, BL, BR**（左上、右上、左下、右下）。
展开成两个三角形，索引固定：

```
三角形 A = (0, 1, 2)   TL, TR, BL
三角形 B = (2, 1, 3)   BL, TR, BR
```

共享边是 `TR–BL` 对角线。这个索引顺序与 binUI 的 `emitQuad` 一致（`0,1,2 / 2,1,3`），
所以两边的"共享边归谁"也一致。

## 3. 像素覆盖：中心采样 + top-left 填充规则

对三角形 `(a, b, c)` 先算有向面积（叉积）：

```
area = (b.x-a.x)*(c.y-a.y) - (b.y-a.y)*(c.x-a.x)
```

- `area == 0` → 退化三角形，整块丢弃（不写任何像素）。
- `area < 0` → 交换 `b` 与 `c`，并令 `area = -area`。**归一化之后内部点的三条边函数都 > 0**。

对像素中心 `p = (x+0.5, y+0.5)`：

```
e0 = edge(b, c, p)
e1 = edge(c, a, p)
e2 = edge(a, b, p)

edge(u, v, p) = (v.x-u.x)*(p.y-u.y) - (v.y-u.y)*(p.x-u.x)
```

覆盖判定：

```
inside_i = (e_i > 0) || (e_i == 0 && is_top_left(edge_i))
is_top_left(u, v) = (v.y == u.y && v.x > u.x) || (v.y < u.y)
```

**top-left 规则的作用**：两个相邻四边形共享的边（以及四边形自己的对角线）只被**一个**
三角形计入。少了这条，半透明拼接会在接缝处被混两次 —— 一条 128 的缝变成 191，肉眼可见。
`tests/test_cpu_render.c` 的 `seam_top_left` 用例就是钉这个的。

CPU 后端**没有 MSAA**（`caps.msaa_samples == 1`），所以边缘没有覆盖率渐变：一个像素要么
整块被画，要么完全不画。将来 GPU 后端的 4x MSAA 与 CPU 的逐像素比对，必须在
"几何边界完全落在像素中心之外"的用例上做（或者接受边界像素的差异）。

## 4. 属性插值

用重心坐标（屏幕空间线性，**不做透视校正** —— 这一层只有 2D）：

```
w0 = e0 / area, w1 = e1 / area, w2 = e2 / area
attr = w0*a.attr + w1*b.attr + w2*c.attr
```

参与插值的属性：`u, v, z`，以及**直通**的顶点颜色 `r, g, b, a`。

为什么颜色在直通域插值：GPU 上顶点色属性是直通的，着色器在片元里才预乘；如果这里先
预乘再插值，alpha 边缘的渐变会和不透明几何不一致。

## 5. 片元颜色

```
if (管线有 TEXTURED 且绑定了纹理) {
    tex   = sample(纹理, u, v, sampler)        // 存储是**预乘**的
    vcol  = 插值后的直通顶点色
    frag.rgb = tex.rgb * vcol.rgb * vcol.a     // 直通相乘后再预乘
    frag.a   = tex.a   * vcol.a
} else {
    frag.rgb = vcol.rgb * vcol.a
    frag.a   = vcol.a
}
```

第二行是"纯色四边形"：顶点色直通 → 乘 alpha 变预乘。第一行是同一件事的展开：
`tex.rgb` 已经是预乘纹素（`t.rgb*t.a`），所以再乘 `vcol.rgb * vcol.a` 与
"直通纹理 × 直通顶点色 → 预乘"完全等价。

## 6. 采样

纹理**内部存预乘 RGBA8**（见 §9）。采样在存储域做（预乘域线性插值是正确做法：
`lerp(rgb*a)` 与 `lerp(rgb)*lerp(a)` 在预乘语义下一致，且不会在透明边缘出现黑边）。

坐标处理（`u`、`v` 分别独立）：

| wrap | 处理 |
|---|---|
| `BGPU_WRAP_CLAMP` | 先夹到 `[0, 1]`（边缘纹素被拉伸） |
| `BGPU_WRAP_REPEAT` | `u = u - floor(u)`，绕回 `[0, 1)` |

取样（`w`、`h` 是纹理尺寸）：

| filter | 公式 |
|---|---|
| `NEAREST` | `ix = floor(u*w)`（repeat 时对 `w` 取模；clamp 时夹到 `w-1`） |
| `LINEAR` | `x = u*w - 0.5`，`x0 = floor(x)`，`tx = x - x0`，取 `x0` 与 `x0+1` 两个纹素按 `tx` 插值（两个轴都做，四个纹素双线性） |

`-0.5` 是 GPU 的纹素中心约定：`u = (i+0.5)/w` 时正好取到第 `i` 个纹素的中心。
**不要**在这种公式上再加半个纹素（binUI 的颗粒噪声踩过这个坑：加了之后采样点落在纹素
边界上，NEAREST 取哪个纹素由实现决定，GL 与 D3D11 会整块不一致）。

`BGPU_FORMAT_R8` 的纹理当遮罩用：采样结果是 `(v, v, v, v)`（`v` = 覆盖率的预乘白）。

未绑定纹理时按**白色**处理（`(1,1,1,1)`），与 GPU 后端绑定 1×1 白纹理等价。

## 7. 深度

- 深度附件 = `BGPU_RENDER_CLEAR_DEPTH` 该趟里传入的 `depth_target` 纹理；CPU 后端给它
  懒分配一块 `target_w × target_h` 的 **float32** 深度（初值 1.0）。
- 函数固定为 **LESS**：`frag.z < depth[px]` 才通过。相等**不通过**（与 `D3D11_COMPARISON_LESS`
  和 `GL_LESS` 一致）。
- 通过后，若管线有 `BGPU_PIPE_DEPTH_WRITE` 就写 `depth[px] = frag.z`。
- `BGPU_PIPE_DEPTH_TEST` 关掉时不做比较；此时若开了 `DEPTH_WRITE`，仍然写（与 GPU 的
  独立开关一致）。
- 清深度的默认值：`BgpuRenderBeginDesc.clear_depth`，调用方一般给 `1.0f`。

注意：ABI 里**没有专门的深度纹理格式**，`depth_target` 就是一个 `RENDER_TARGET` 纹理
（见 `docs/backends.md` 的"欠账"一节，建议 0.2.0 加 `BGPU_TEXTURE_DEPTH` 标志）。

## 8. 混合（预乘域）

源颜色已预乘（§5），目标存储也是预乘的。五种模式逐通道（rgb 与 a 用同一个公式）：

| 模式 | 公式 | 典型用途 |
|---|---|---|
| `PREMULTIPLIED` | `dst = src + dst*(1-srcA)` | 普通半透明 |
| `REPLACE` | `dst = src` | 覆盖（图层合成 / 毛玻璃 / 清底色） |
| `KEEP` | `dst = dst*srcA` | SVG `<mask>` 的保留趟 |
| `ERASE` | `dst = dst*(1-srcA)` | clip-path 补集擦除 |
| `ADDITIVE` | `dst = dst + src`（结果夹到 [0,1]） | 高光 / 粒子 |

写回时按通道夹到 `[0,1]` 再量化成 8 位（四舍五入）。

## 9. 存储格式与回读（最容易错的一条）

- 纹理 / 渲染目标 / 交换链位图**内部全部是预乘 RGBA8**（`BGRA8` 只是字节序不同）。
- `texture_upload` 收的是**直通**像素 → 立刻预乘后存。
- `texture_read` / `frame_read` 输出**直通**像素 → 先把预乘反除回去：
  `straight = round(premult * 255 / alpha)`；`alpha == 0` 时输出 0。
- 因此非 255 alpha 的"上传 → 读回"往返可能有 **±1 的量化误差**（8 位存不下两次除法）。
  这是规格的一部分，测试用例挑的是能整除的值（例如 `(255,128,64,128)` 往返无损）。
- `frame_read` 的输出**永远按 RGBA 顺序**，与交换链内部格式无关（`BGRA8` 交换链读出来
  也已经是 R,G,B,A）。

## 10. scissor

- `cmd_set_scissor(x, y, w, h)`：**覆盖**上一次的状态（不做累计裁剪），与目标边界求交。
- `w < 0 || h < 0` → 关掉裁剪（等价于整个目标）。
- `w == 0 || h == 0` → 空矩形，什么也不画。
- 整像素：scissor 是整数矩形，不做亚像素处理。左/上闭、右/下开：
  像素 `(x,y)` 被画到当且仅当 `sc.x <= x < sc.x+sc.w` 且同理 y。

## 11. 网格（`draw_indexed`）

- 顶点格式由管线的 `vertex_layout` 决定；`draw_indexed` 要求 `BGPU_VERTEX_MESH`。
- `index_count` 是索引个数，必须是 3 的倍数语义（多余的尾巴忽略）。
- `vertex_offset` / `index_offset` 是**字节**偏移（与 `buffer_update` 的 `offset` 一致）。
  `vertex_offset` 必须是 `sizeof(BgpuMeshVertex)` 的倍数，`index_offset` 必须是 4 的倍数；
  索引是 **32 位**。越界一律 `BGPU_ERR_INVALID_ARGUMENT`。
- 法线（`nx,ny,nz`）在当前的固定管线里**不参与计算** —— 没有光源状态可以喂给它
  （见 `docs/backends.md` 的欠账）。网格按"顶点色 × 纹理"着色。
