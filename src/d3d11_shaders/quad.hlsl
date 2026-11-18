// bingpu quad.hlsl —— D3D11 后端的固定功能着色器对（vs + ps 各一个入口）
//
// 与 src/vk_shaders/quad.{vert,frag} **逐行同构**：两份实现必须给出同样的像素，
// 所以数学不重写、不改顺序，只换语言（GLSL → HLSL）与坐标约定。差异只有两处，
// 都写在下面：
//   ① D3D 的 NDC y 向上，Vulkan 的 y 向下 —— 顶点着色器里 y 要翻（`1.0 - ...`）；
//   ② 顶点色在内存里是 0xAARRGGBB（小端：B,G,R,A），按 R8G8B8A8_UNORM 读进来之后
//      通道序是 (B,G,R,A)，与 GLSL 那边一样用 .zyxw 换回 (R,G,B,A)。
//
// 顶点属性布局由输入布局对象决定，着色器只认三个语义：
//   POSITION：float3 目标相对坐标，z 直接进深度
//   TEXCOORD0：float2 UV
//   COLOR：float4 打包色（见②）
// 2D 顶点（BgpuVertex，24 字节）与网格顶点（BgpuMeshVertex，36 字节）共用这一对
// 着色器，区别只在输入布局的 stride / offset。
//
// 编译（构建不需要这一步，产物已入库成 src/bingpu_d3d11_shaders.h）：
//   fxc /nologo /T vs_4_0 /E vs /O3 /Fo quad.vs.cso quad.hlsl
//   fxc /nologo /T ps_4_0 /E ps /O3 /Fo quad.ps.cso quad.hlsl
//   python3 tools/gen_dxbc_header.py
// 选 vs_4_0/ps_4_0（而不是 5_0）是为了让 feature level 10_0 的机器也能跑 —— 这个
// 着色器没有用到任何 SM5 独有的东西。

// 与 C++ 侧的 PushConstants 逐字段对应（64 字节）
cbuffer Push : register(b0) {
    float4 vp;      // 0：viewport x, y, w, h（目标像素）
    float2 target;  // 16：目标尺寸 w, h
    float2 pad;     // 24：D3D11 的 cbuffer 按 16 字节对齐打包，这里必须补到 32
    float4 xf0;     // 32：仿射第 0 行 a, c, e；w = 是否打开（>0.5）
    float4 xf1;     // 48：仿射第 1 行 b, d, f
};

Texture2D    tex : register(t0);
SamplerState smp : register(s0);

struct VSIn {
    float3 pos : POSITION;
    float2 uv  : TEXCOORD0;
    float4 col : COLOR0;
};

struct PSIn {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
    float4 col : COLOR0;
};

PSIn vs(VSIn i) {
    // 顶点 → 屏幕坐标（docs/raster_rules.md §1）：
    //   sx = vp.x + (x / target_w) * vp.w
    //   sy = vp.y + (y / target_h) * vp.h
    //   打开仿射后对 (sx, sy) 再作用一次 2x3 矩阵（屏幕空间，viewport 之后）
    float2 s = float2(vp.x + (i.pos.x / target.x) * vp.z,
                      vp.y + (i.pos.y / target.y) * vp.w);
    if (xf0.w > 0.5) {
        s = float2(xf0.x * s.x + xf0.y * s.y + xf0.z,
                   xf1.x * s.x + xf1.y * s.y + xf1.z);
    }
    // D3D 的 NDC y 向上：目标坐标（y 向下）要把 y 翻过来
    float2 ndc = float2(s.x / target.x * 2.0 - 1.0,
                        1.0 - s.y / target.y * 2.0);
    PSIn o;
    o.pos = float4(ndc, i.pos.z, 1.0);
    o.uv = i.uv;
    o.col = i.col.zyxw;   // 内存 (B,G,R,A) → (R,G,B,A)
    return o;
}

float4 ps(PSIn i) : SV_Target {
    // docs/raster_rules.md §5/§6：纹理是预乘的，顶点色是直通的，在片元里相遇
    //   frag.rgb = tex.rgb * vcol.rgb * vcol.a
    //   frag.a   = tex.a   * vcol.a
    float4 t = tex.Sample(smp, i.uv);
    float a = i.col.a;
    return float4(t.rgb * i.col.rgb * a, t.a * a);
}
