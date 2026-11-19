// bingpu quad.hlsl —— D3D11 后端的固定功能着色器对（2D quad / 网格 两个 VS + 一个 PS）
//
// 数学与 src/bingpu_cpu.cpp 的 shade_general **逐位同构**：
//   · 顶点色按屏幕空间线性插值（CPU 也是 w0*v0 + w1*v1 + w2*v2，不除 iw）；
//   · UV 与法线在 MESH 布局下按 1/w 透视校正：interp(attr*iw) / interp(iw)；
//   · 光照在**直通空间**乘、clamp 到 [0,1]，最后才乘 alpha 变回预乘。
// 三端（CPU / D3D11 / Vulkan / GL）给出同样像素，所以这里不重排任何运算顺序。
//
// 为什么有两个 VS：D3D11 的输入布局必须覆盖 VS 声明的**全部**输入语义。
// 2D 顶点（BgpuVertex，24 字节）没有法线和 w，如果硬让它共用网格 VS，就只能把
// 缺的两个语义指向别的字段（读垃圾值）—— 那等于给 2D 路径塞了无意义的数据。
// 拆成 vs / vs_mesh 之后，2D 的顶点流一个字节没动（v0.5.2 的逐像素结果不变）。
//
// 顶点属性布局由输入布局对象决定，着色器只认语义：
//   POSITION  float3   2D/网格共用（目标像素坐标，z 直接进深度）
//   TEXCOORD0 float2   UV
//   COLOR     float4   打包色（内存 0xAARRGGBB，按 R8G8B8A8 读进来是 (B,G,R,A)）
//   NORMAL    float3   世界空间法线（仅网格）
//   TEXCOORD1 float    裁剪空间原始 w（仅网格；0 视为 1）
//
// 编译（构建不需要这一步，产物已入库成 src/bingpu_d3d11_shaders.h）：
//   fxc /nologo /T vs_4_0 /E vs      /O3 /Fo quad.vs.cso      quad.hlsl
//   fxc /nologo /T vs_4_0 /E vs_mesh /O3 /Fo quad.vs_mesh.cso quad.hlsl
//   fxc /nologo /T ps_4_0 /E ps      /O3 /Fo quad.ps.cso      quad.hlsl
//   python tools/gen_dxbc_header.py
// 选 vs_4_0/ps_4_0（而不是 5_0）是为了让 feature level 10_0 的机器也能跑 —— 这个
// 着色器没有用到任何 SM5 独有的东西。

// 与 C++ 侧的 PushConstants 逐字段对应（128 字节，16 字节对齐打包）
cbuffer Push : register(b0) {
    float4 vp;          //   0：viewport x, y, w, h（目标像素）
    float2 target;      //  16：目标尺寸 w, h
    float2 pad;         //  24：16 字节对齐的填充，恒 0
    float4 xf0;         //  32：仿射第 0 行 a, c, e；w = 是否打开（>0.5）
    float4 xf1;         //  48：仿射第 1 行 b, d, f
    float4 lightDir;    //  64：xyz = 指向光源（已归一化）；w = 是否开光照（>0.5）
    float4 lightColor;  //  80：rgb = 光色（可 >1 过曝）
    float4 lightAmb;    //  96：rgb = 环境光
    float4 flags;       // 112：x = 是否透视校正（网格布局 = 1，2D = 0）
                        //      y = 是否采样纹理（BGPU_PIPE_TEXTURED）
};

Texture2D    tex : register(t0);
SamplerState smp : register(s0);

struct VSIn2D {
    float3 pos : POSITION;
    float2 uv  : TEXCOORD0;
    float4 col : COLOR0;
};

struct VSInMesh {
    float3 pos : POSITION;
    float3 nrm : NORMAL;
    float2 uv  : TEXCOORD0;
    float4 col : COLOR0;
    float  w   : TEXCOORD1;
};

// 两个 VS 的输出结构必须一致（共用一个 PS）
struct PSIn {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;   // 已乘 iw
    float4 col : COLOR0;      // 直通顶点色（屏幕空间线性插值）
    float3 nrm : NORMAL;      // 已乘 iw（2D 恒 0，LIGHTING 在 2D 上被 API 层挡掉）
    float  iw  : TEXCOORD1;   // 1/w（2D 恒 1）
};

// 顶点 → 屏幕坐标（docs/raster_rules.md §1）：
//   sx = vp.x + (x / target_w) * vp.w
//   sy = vp.y + (y / target_h) * vp.h
//   打开仿射后对 (sx, sy) 再作用一次 2x3 矩阵（屏幕空间，viewport 之后）
float4 ToClip(float3 pos) {
    float2 s = float2(vp.x + (pos.x / target.x) * vp.z,
                      vp.y + (pos.y / target.y) * vp.w);
    if (xf0.w > 0.5) {
        s = float2(xf0.x * s.x + xf0.y * s.y + xf0.z,
                   xf1.x * s.x + xf1.y * s.y + xf1.z);
    }
    // D3D 的 NDC y 向上：目标坐标（y 向下）要把 y 翻过来
    return float4(s.x / target.x * 2.0 - 1.0,
                  1.0 - s.y / target.y * 2.0,
                  pos.z, 1.0);
}

// UNORM8 顶点色 → float：**必须**是 k/255.0（精确除），不能直接用输入布局转出来的值。
// D3D 的 layout 转换是 k*(1/255)，255 得到 0.99999994；而 CPU 参考实现
// （bingpu_cpu.cpp 的 vcol）是 k/255.0f，255 就是 1.0。差一个 ulp 会在
// 0.1*255 = 25.5 这种半点上分出 25 / 26 —— [lighting] 用例抓的就是这个。
// 先把字节取回来（乘 255 再截断，误差远小于 0.5），再做一次精确除法。
float4 UnpackColor(float4 c) {
    return floor(c * 255.0 + 0.5) / 255.0;
}

PSIn vs(VSIn2D i) {
    PSIn o;
    o.pos = ToClip(i.pos);
    o.uv = i.uv;
    o.col = UnpackColor(i.col.zyxw);   // 内存 (B,G,R,A) → (R,G,B,A)
    o.nrm = float3(0.0, 0.0, 0.0);
    o.iw = 1.0;
    return o;
}

PSIn vs_mesh(VSInMesh i) {
    // w = 裁剪空间原始 w；0 视为 1（正交投影，与 CPU 的 `w > 1e-9 ? 1/w : 1` 同判据）
    const float iw = (i.w > 1e-9) ? (1.0 / i.w) : 1.0;
    PSIn o;
    o.pos = ToClip(i.pos);
    o.uv = i.uv * iw;
    o.col = UnpackColor(i.col.zyxw);
    o.nrm = i.nrm * iw;
    o.iw = iw;
    return o;
}

float4 ps(PSIn i) : SV_Target {
    // 透视校正：attr = interp(attr·iw) / interp(iw)。
    // 2D 布局没有 w，flags.x = 0 时**原样通过**（不做那次除法，逐位等于旧路径）。
    float2 uv = i.uv;
    float3 n = i.nrm;
    if (flags.x > 0.5) {
        const float invIw = 1.0 / i.iw;
        uv *= invIw;
        n *= invIw;
    }

    // 采样纹理：不采样时**必须**是精确的 (1,1,1,1)，不能"绑一张白纹理凑数"。
    // UNORM 白纹素采样回来是 255*(1/255) = 0.99999994（差一个 ulp），而 CPU 的
    // 非纹理路径根本不乘纹理 → 光照里 0.1*255 = 25.5 会被这个 ulp 压到 25.4999，
    // 舍入成 25 而 CPU 是 26（[lighting] 用例实打实抓到的差 1）。
    const float4 t = (flags.y > 0.5) ? tex.Sample(smp, uv) : float4(1.0, 1.0, 1.0, 1.0);
    const float a = i.col.a;
    // CPU 的 frag（预乘）：rgb = tex.rgb * vcol.rgb * vcol.a，a = tex.a * vcol.a
    const float3 fragRgb = t.rgb * i.col.rgb * a;
    const float outA = t.a * a;

    if (lightDir.w > 0.5) {
        // ---- 固定功能 Lambert（ABI 3）----
        // CPU（bingpu_cpu.cpp 的 shade_general）先把 frag 还原成直通、乘光照、
        // clamp 到 [0,1]，**最后才乘 alpha**。预乘空间 clamp 会差一个 alpha 因子。
        const float nlen2 = dot(n, n);
        float ndl = 0.0;
        if (nlen2 > 1e-12) {
            ndl = max(dot(n * (1.0 / sqrt(nlen2)), lightDir.xyz), 0.0);
        }
        const float3 lit = lightAmb.rgb + lightColor.rgb * ndl;
        const float invA = (outA > 1e-6) ? (1.0 / outA) : 0.0;
        const float3 straight = saturate(fragRgb * invA * lit);
        // 末段与 CPU 的 q8 对齐：CPU 是 (uint8)(v*255 + 0.5)（**.5 向上**），
        // 而 RTV 的 float→UNORM 实测是 **.5 向下**（字面量 0.1/0.3/0.5 写进
        // R8G8B8A8_UNORM 得到 25/76/127），恰好半格的值（0.1*255=25.5 就是典型）
        // 会与 CPU 差 1。这里先量化成 CPU 的那个字节，再加 1/4 格输出 ——
        // 两次舍入（无论哪种规则）都落在同一个整数上。
        const float3 prem = straight * outA;
        const float3 q = floor(prem * 255.0 + 0.5);
        const float qa = floor(outA * 255.0 + 0.5);
        return float4((q + 0.25) / 255.0, (qa + 0.25) / 255.0);
    }
    return float4(fragRgb, outA);
}
