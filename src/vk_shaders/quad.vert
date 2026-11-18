#version 450
// bingpu quad.vert —— 固定功能批处理的顶点着色器（2D 与网格共用一份）
//
// 顶点属性布局由管线决定，但**着色器只认三个 location**：
//   location 0：vec3 位置（目标相对坐标，z 直接进深度）
//   location 1：vec2 UV
//   location 2：打包颜色 0xAARRGGBB（内存序 = B,G,R,A，见 fragment 的 .zyxw）
// 2D 顶点（BGPU_VERTEX_2D，24 字节）与网格顶点（BGPU_VERTEX_MESH，36 字节，
// 中间夹一条法线）走的是同一个着色器、不同的 vertex input binding。
//
// 顶点 → 屏幕坐标的规则逐字照 docs/raster_rules.md §1：
//   sx = vp.x + (x / target_w) * vp.w
//   sy = vp.y + (y / target_h) * vp.h
//   打开仿射后对 (sx, sy) 再作用一次 2×3 矩阵（屏幕空间，顺序是 viewport 之后）
// 然后换成 NDC。**Vulkan 的 NDC y 向下**，所以这里不翻 y。

layout(push_constant) uniform PC {
    vec4 vp;      // viewport：x, y, w, h（目标像素）
    vec2 target;  // 目标尺寸：w, h
    vec4 xf0;     // 仿射第 0 行：a, c, e；w = 是否打开（>0.5）
    vec4 xf1;     // 仿射第 1 行：b, d, f；w 未用
} pc;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec2 inUv;
layout(location = 2) in vec4 inColor;

layout(location = 0) out vec2 vUv;
layout(location = 1) out vec4 vColor;  // **直通**颜色（片元里才预乘）

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
    vUv = inUv;
    vColor = inColor.zyxw;  // 内存序 (B,G,R,A) → (R,G,B,A)
}
