#version 450
// bingpu quad.frag —— 固定功能批处理的片元着色器
//
// 公式逐字照 docs/raster_rules.md §5（片元颜色）与 §6（采样）：
//   有纹理：frag.rgb = tex.rgb * vcol.rgb * vcol.a;  frag.a = tex.a * vcol.a
//   无纹理：frag.rgb = vcol.rgb * vcol.a;            frag.a = vcol.a
// 纹理存储是**预乘**的（上传时已经乘过 alpha，见 raster_rules §9），顶点色是**直通**的；
// 两者在片元里相遇，结果直接进预乘域的混合 —— 于是下面的混合因子只需要五种固定组合。

layout(location = 0) in vec2 vUv;
layout(location = 1) in vec4 vColor;  // 直通（已插值）

layout(set = 0, binding = 0) uniform sampler2D tex;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 t = texture(tex, vUv);  // 预乘
    outColor = vec4(t.rgb * vColor.rgb * vColor.a, t.a * vColor.a);
}
