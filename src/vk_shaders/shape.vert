#version 450
// bingpu shape.vert —— 解析圆角（ABI 4）
//
// 一个 BgpuShapeVertex（36 字节）在缓冲区里的排布：
//   location 0：vec2  位置（目标像素；与 quad.vert 的 inPos 同一坐标系）
//   location 1：vec2  局部坐标（相对盒子中心，顶点处 = ±half）
//   location 2：vec2  盒子半尺寸
//   location 3：float 圆角半径
//   location 4：float 环带厚度（0 = 实心）
//   location 5：vec4  打包颜色 0xAARRGGBB（内存序 B,G,R,A → .zyxw）
//
// 位置变换与 quad.vert **逐字相同**（viewport → 仿射 → NDC）。局部坐标原样送到片元：
//   轴对齐 1:1 时屏幕位移就等于像素位移，于是片元里的 |grad d| 恰好是 1，
//   0.5 - d 是**解析**覆盖率（不需要 fwidth，理由见 shape.frag）。
// 旋转/缩放/透视仍然不精确（局部坐标会被非均匀拉伸而距离场不知道），
// 所以那条路上层继续用覆盖率模板 —— 这是 bin2d 的选路决定，不是这里的缺陷。
//
// vLocal 用 noperspective：w 恒 1 时数学上是空操作，但透视校正那一遍除法会让
// 恒定/线性的属性多舍入一次；CPU 参考后端做的是纯 Σ(λ·attr)，这里对齐它。
// vHalf / vRT 对四个顶点是常量，用 flat（省插值，也免得平面外的边界值漂进来）。

layout(push_constant) uniform PC {
    vec4 vp;          // viewport：x, y, w, h（目标像素）
    vec2 target;      // 目标尺寸：w, h
    vec2 flags;       // 本着色器不用（与 quad.vert 共用同一块 push constant）
    vec4 xf0;         // 仿射第 0 行：a, c, e；w = 是否打开（>0.5）
    vec4 xf1;         // 仿射第 1 行：b, d, f
    vec4 lightDir;    // 不用
    vec4 lightColor;  // 不用
    vec4 lightAmb;    // 不用
} pc;

layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inLocal;
layout(location = 2) in vec2 inHalf;
layout(location = 3) in float inRadius;
layout(location = 4) in float inThick;
layout(location = 5) in vec4 inColor;

noperspective layout(location = 0) out vec2 vLocal;
flat layout(location = 1) out vec2 vHalf;
flat layout(location = 2) out vec2 vRT;      // x = 半径，y = 厚度
noperspective layout(location = 3) out vec4 vColor;

void main() {
    vec2 s = vec2(pc.vp.x + (inPos.x / pc.target.x) * pc.vp.z,
                  pc.vp.y + (inPos.y / pc.target.y) * pc.vp.w);
    if (pc.xf0.w > 0.5) {
        s = vec2(pc.xf0.x * s.x + pc.xf0.y * s.y + pc.xf0.z,
                 pc.xf1.x * s.x + pc.xf1.y * s.y + pc.xf1.z);
    }
    /* Vulkan 的 NDC y 向下，与 quad.vert 一样这里不翻 y */
    vec2 ndc = vec2(s.x / pc.target.x * 2.0 - 1.0,
                    s.y / pc.target.y * 2.0 - 1.0);
    gl_Position = vec4(ndc, 0.0, 1.0);

    vLocal = inLocal;
    vHalf = inHalf;
    vRT = vec2(inRadius, inThick);
    vColor = inColor.zyxw;
}
