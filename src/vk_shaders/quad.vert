#version 450
// bingpu quad.vert —— 固定功能批处理的顶点着色器（2D 与网格共用一份）
//
// ABI 3 起多两件事：**透视校正**与**光照**的顶点侧准备。
//
// 顶点属性布局由管线决定，着色器认五个 location：
//   location 0：vec3 位置（目标相对坐标，z 直接进深度）
//   location 1：vec2 UV
//   location 2：打包颜色 0xAARRGGBB（内存序 = B,G,R,A，见 fragment 的 .zyxw）
//   location 3：vec3 世界空间法线   ← 只有网格布局有；2D 管线喂占位数据
//   location 4：float 裁剪空间原始 w ← 同上
//
// 为什么 2D 管线也要**提供**这两个属性（而不是让它们缺席）：
//   Vulkan 的验证层有一条硬规则 —— 着色器里声明的顶点输入必须由管线提供
//   （VUID-VkGraphicsPipelineCreateInfo-Input-07904），缺席就是验证错误，
//   真驱动上也可能是未定义值。所以 2D 管线把 location 3/4 指向顶点自己的
//   前 12/4 字节（位置），值是无意义的但**在缓冲区内**；是否使用由 push constant
//   的 flags.x（透视）决定，2D 恒为 0，那两个值不会被用到。
//
// 透视校正为什么要自己做（而不是让硬件按 gl_Position.w 插值）：
//   顶点送进来时已经除过 w（屏幕空间），硬件再除一次就错了。把原来的 w 逆过来
//   送给片元、在片元里做 `Σ(attr·iw)/Σ(iw)`（iw = 1/w）与 CPU 参考实现
//   （src/bingpu_cpu.cpp 的 shade_general）是**同一个公式**，逐像素才对得上。
//   gl_Position.w 恒 1，硬件插值就是普通的线性插值 —— 这正是公式需要的输入。
//
// 顶点 → 屏幕坐标的规则逐字照 docs/raster_rules.md §1：
//   sx = vp.x + (x / target_w) * vp.w
//   sy = vp.y + (y / target_h) * vp.h
//   打开仿射后对 (sx, sy) 再作用一次 2×3 矩阵（屏幕空间，顺序是 viewport 之后）
// 然后换成 NDC。**Vulkan 的 NDC y 向下**，所以这里不翻 y。

layout(push_constant) uniform PC {
    vec4 vp;          // viewport：x, y, w, h（目标像素）
    vec2 target;      // 目标尺寸：w, h
    vec2 flags;       // x = 透视校正（网格布局 1，2D 0）；y = 光照（LIGHTING 管线 1）
    vec4 xf0;         // 仿射第 0 行：a, c, e；w = 是否打开（>0.5）
    vec4 xf1;         // 仿射第 1 行：b, d, f；w 未用
    vec4 lightDir;    // 光照方向 xyz（指向光源，单位长）；w 未用
    vec4 lightColor;  // 光色 rgb；w 未用
    vec4 lightAmb;    // 环境光 rgb；w 未用
} pc;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec2 inUv;
layout(location = 2) in vec4 inColor;
layout(location = 3) in vec3 inNormal;   // 2D：占位（flags.x = 0 时不使用）
layout(location = 4) in float inW;       // 2D：占位（同上）

// 四个插值量全部用 **noperspective**：透视除法我们自己在片元里做（见文件头）。
// 如果不加，硬件会先按它自己的 1/w 做一遍校正 —— w 恒 1 时数学上是空操作，
// 但那一遍除法的舍入会让"恒定属性"插值出 0.99999994 而不是 1.0：
// 乘上 ambient 0.1 之后 0.099999994·255 = 25.4999，被舍成 25，与 CPU 的 26 差 1 级
// （实测就是这么撞出来的）。noperspective 之后插值就是纯 Σ(λ·attr)，
// 恒定属性在 vColor 与 vIw 上取到**同一个数**，相除恰好是 1.0。
noperspective layout(location = 0) out vec2 vUv;      // Σ(iw · uv)
noperspective layout(location = 1) out vec4 vColor;   // **直通**颜色 × iw
noperspective layout(location = 2) out vec3 vNormal;  // 世界空间法线 × iw
noperspective layout(location = 3) out float vIw;     // Σ(iw)

void main() {
    vec2 s = vec2(pc.vp.x + (inPos.x / pc.target.x) * pc.vp.z,
                  pc.vp.y + (inPos.y / pc.target.y) * pc.vp.w);
    if (pc.xf0.w > 0.5) {
        s = vec2(pc.xf0.x * s.x + pc.xf0.y * s.y + pc.xf0.z,
                 pc.xf1.x * s.x + pc.xf1.y * s.y + pc.xf1.z);
    }
    vec2 ndc = vec2(s.x / pc.target.x * 2.0 - 1.0,
                    s.y / pc.target.y * 2.0 - 1.0);
    gl_Position = vec4(ndc, inPos.z, 1.0);

    // iw = 1/w（w <= 0 视为 1 = 正交）。与 CPU 的 `(mv.w > 1e-9f) ? 1/w : 1` 同一条判据。
    float iw = 1.0;
    vec3 n = vec3(0.0, 0.0, 1.0);
    if (pc.flags.x > 0.5) {
        iw = (inW > 1e-9) ? (1.0 / inW) : 1.0;
        n = inNormal;
    }
    vUv = inUv * iw;
    vColor = inColor.zyxw * iw;   // 内存序 (B,G,R,A) → (R,G,B,A)，再乘 iw
    vNormal = n * iw;
    vIw = iw;
}
