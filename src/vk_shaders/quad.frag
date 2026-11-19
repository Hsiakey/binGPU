#version 450
// bingpu quad.frag —— 固定功能批处理的片元着色器
//
// 公式逐字照 docs/raster_rules.md §5（片元颜色）与 §6（采样）：
//   有纹理：frag.rgb = tex.rgb * vcol.rgb * vcol.a;  frag.a = tex.a * vcol.a
//   无纹理：frag.rgb = vcol.rgb * vcol.a;            frag.a = vcol.a
// （无纹理的管线照样绑一张 1×1 白纹理，于是只要一条公式。）
//
// 纹理存储是**预乘**的（上传时已经乘过 alpha，见 raster_rules §9），顶点色是**直通**的；
// 两者在片元里相遇，结果直接进预乘域的混合 —— 于是混合因子只需要五种固定组合。
//
// ABI 3 加了两段，都对着 CPU 的 shade_general 写：
//
// ① 透视校正（flags.x）：顶点着色器送来 Σ(iw·attr) 与 Σ(iw)，这里除一次。
//    uv / 顶点色 / 法线三者都要除。2D 管线 flags.x = 0，**整段不执行**，
//    于是 2D 的像素与 ABI 2 逐位相同（不是"除以 1.0 的近似"，是根本没有那次除法）。
//
// ② 固定功能 Lambert（flags.y）：
//      lit = ambient + lightColor · max(dot(normalize(n), dir), 0)
//      rgb(直通) = clamp(rgb(直通) · lit, 0, 1)，再乘回 alpha
//    必须"还原成直通再 clamp"：纹理与 frag 都是预乘的，先乘光照再 clamp 会让
//    过曝的像素被 alpha 挡掉一部分 —— 与软光栅差一个 alpha 因子（CPU 的注释同一条）。

layout(push_constant) uniform PC {
    vec4 vp;
    vec2 target;
    vec2 flags;       // x = 透视校正；y = 光照
    vec4 xf0;
    vec4 xf1;
    vec4 lightDir;    // 指向光源的方向（单位长）
    vec4 lightColor;  // 光色（线性，可 > 1 用来过曝）
    vec4 lightAmb;    // 环境光
} pc;

// 与顶点着色器一一对应的 noperspective（理由见 quad.vert 的同一段注释）
noperspective layout(location = 0) in vec2 vUv;      // Σ(iw · uv)
noperspective layout(location = 1) in vec4 vColor;   // Σ(iw · 直通色)
noperspective layout(location = 2) in vec3 vNormal;  // Σ(iw · 法线)
noperspective layout(location = 3) in float vIw;     // Σ(iw)

layout(set = 0, binding = 0) uniform sampler2D tex;

layout(location = 0) out vec4 outColor;

void main() {
    vec2 uv = vUv;
    vec4 vcol = vColor;
    vec3 n = vNormal;
    // 透视除法：与 CPU 的 `invIw = (perspective && iwSum > 1e-9f) ? 1/iwSum : 1` 同一条判据。
    // 写**真除法**而不是"算一次倒数再乘"：恒定属性（三个顶点同色/同法线）时
    //   Σ(λ·a·iw) 与 Σ(λ·iw) 是同一个浮点数，x/x 恰好是 1.0；
    //   而 x*(1/x) 会多舍入一次，落到 0.99999994 —— 乘上很小的环境光之后就可能
    //   在 8 位量化上差 1 级。这条是我实现时实测撞出来的（背光那一格 cpu=1A / gpu=19），
    //   修掉"乘倒数"还不够：硬件自己的透视插值也会多除一次，所以顶点侧的
    //   四个插值量都标了 noperspective（见 quad.vert）。
    if (pc.flags.x > 0.5 && vIw > 1e-9) {
        uv = vUv / vIw;
        vcol = vColor / vIw;
        n = vNormal / vIw;
    }

    vec4 t = texture(tex, uv);                                    // 预乘
    vec3 rgb = t.rgb * vcol.rgb * vcol.a;                         // 预乘
    float a = t.a * vcol.a;

    if (pc.flags.y > 0.5) {
        // 法线归一化（长度平方过小就当作"没有方向"，与 CPU 的 1e-12 判据一致）
        float n2 = dot(n, n);
        float ndl = 0.0;
        if (n2 > 1e-12) {
            ndl = max(dot(n * inversesqrt(n2), pc.lightDir.xyz), 0.0);
        }
        vec3 lit = pc.lightAmb.rgb + pc.lightColor.rgb * ndl;
        float invA = (a > 1e-6) ? (1.0 / a) : 0.0;
        // 预乘 → 直通 → 乘光照 → clamp → **先量化到 8 位** → 乘回 alpha
        //
        // 为什么必须自己量化：帧缓冲的 float→UNORM 在 `.5` 上**向下收**（本机实测
        // 0.1/0.3/0.5 写进 RGBA8 得 25/76/127），而 CPU 的 q8 是 `.5` **向上**
        // （0.1*255 = 25.5 → 26）。环境光 0.1 正好踩在这个平局点上，于是同一份
        // 光照公式在两边差 1 级 —— 那不是公式的偏差，是写入路径的舍入规则不同。
        // 在片元里先按 CPU 的口径量化一次，之后写入的误差 < 1/255 LSB，两种舍入
        // 规则都落到同一个整数。（GL 与 D3D11 后端各自做了同一件事，见两份 shader。）
        vec3 straight = clamp(rgb * invA * lit, 0.0, 1.0);
        if (a > 1e-6) straight = floor(straight * 255.0 + 0.5) / 255.0;
        rgb = straight * a;
    }

    outColor = vec4(rgb, a);
}
