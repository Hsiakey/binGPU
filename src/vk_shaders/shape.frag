#version 450
// bingpu shape.frag —— 圆角矩形的**解析**抗锯齿（ABI 4）
//
// 这是"AA 数学在 GPU 上算"的那一份：没有覆盖率模板、没有纹理采样，
// 每个像素从自己的局部坐标求一个有符号距离，再换算成覆盖率。
//
//   q   = abs(p) - half + r
//   d   = length(max(q, 0)) + min(max(q.x, q.y), 0) - r
//   cov = clamp(0.5 - d, 0, 1)                     实心
//   cov = clamp(0.5 - |d + t/2| + t/2, 0, 1)       环带（厚度 t）
//
// 为什么 0.5 - d 就是覆盖率（而不是要除以梯度长度）：
//   轴对齐、1:1 的映射下，局部坐标的 1 个单位 = 屏幕的 1 个像素，圆角矩形距离场
//   是 1-Lipschitz 且 |grad d| 恒等于 1。一个像素内 d 恰好跨过 1，覆盖率随 d
//   线性地从 1 降到 0，中点落在 d = 0 的边界上 —— 于是 0.5 - d 是**精确解析**值。
//   所以这里**没有** fwidth：导数在 2×2 quad 上是分块常数，高曲率处（小半径的弧）
//   会退化成方块；而这里的梯度本来就是常数，解析式更准，也让 CPU 参考后端
//   （bingpu_cpu.cpp 的 shape_coverage，逐字同一套式子）能逐位对上。
//
// 与文档的关系：docs/raster_rules.md §5/§6 的"片元颜色/采样"在这里不适用 ——
// 解析圆角不采样纹理。顶点色是直通的，输出预乘（与 GL / D3D11 三端同一口径）。

layout(push_constant) uniform PC {
    vec4 vp;
    vec2 target;
    vec2 flags;
    vec4 xf0;
    vec4 xf1;
    vec4 lightDir;
    vec4 lightColor;
    vec4 lightAmb;
} pc;

noperspective layout(location = 0) in vec2 vLocal;
flat layout(location = 1) in vec2 vHalf;
flat layout(location = 2) in vec2 vRT;
noperspective layout(location = 3) in vec4 vColor;

layout(location = 0) out vec4 outColor;

void main() {
    float r = max(vRT.x, 0.0);
    float th = vRT.y;
    vec2 q = abs(vLocal) - vHalf + vec2(r);
    float d = length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - r;
    float cov;
    if (th > 0.0) {
        float half_t = th * 0.5;
        cov = 0.5 - abs(d + half_t) + half_t;
    } else {
        cov = 0.5 - d;
    }
    cov = clamp(cov, 0.0, 1.0);
    float a = vColor.a * cov;
    outColor = vec4(vColor.rgb * a, a);   // 顶点色直通 ⇒ 输出预乘
}
