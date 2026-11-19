/* bingpu_prims.cpp —— 2D 图元的**几何展开**（0.10.0）
 *
 * 这一层只做一件事：把"一个圆角矩形"变成"最多 9 个带 UV 的四边形"，交给
 * `bingpu_cmd_draw_quads`。**没有着色器、没有逐后端代码**，所以 CPU / VK / GL / D3D11
 * 五个后端自动都支持，像素也天然一致。
 *
 * 为什么 AA 数学不在这里：
 *   圆角边缘长什么样是**绘制语义**（bin2d 的 `RasterizeRoundedTileCoverage` 是唯一一份
 *   解析式）。仓里 GL / VK / D3D11 三份后端曾经各写一份一模一样的距离循环，改一处就差
 *   一圈像素 —— 那种漂移不该在这一层重演。这里只吃"覆盖率模板"这张位图。
 *
 * 为什么粗描边不在这一层循环：
 *   粗描边 = 同心 1px 环叠层，而**每一层的半径不同**（内缩 k、半径减 k），于是每一层要
 *   一张**不同**的覆盖率模板。模板缓存归调用方（只有它知道半径取整与缓存键），所以这里
 *   只画"一层"，多层由调用方连着调。这一层负责的是"怎么把一层变成四边形"。
 *
 * 九宫格为什么是 9 块而不是 1 块：
 *   圆角矩形的角必须**不拉伸**（拉伸圆弧会把 AA 拉成糊），中间必须**拉伸**（否则
 *   大圆角矩形要发几百个 1px 条带）。所以：四角 1:1 取模板的角、四边中段沿一个轴拉伸、
 *   中心两个轴都拉伸。中段的 UV 故意取模板**正中 1 个像素**（t >= 2r+4 时那里一定是
 *   直边段），这样"中点采样"不会把弧线采进来。
 *
 * 数字口径（与 binUI 的 BuildRoundedNineSlice 逐条一致，因为像素要对得上）：
 *   · 角片边长 c = r（四分之一圆弧的包围盒），cu = c / t
 *   · 中段 UV = [0.5, 0.5 + 1/t]
 *   · 中段宽度为 0 时该块**直接跳过**（2*r 已经吃掉整个短边）
 */
#include <cmath>
#include <vector>

#include "bingpu_internal.h"

namespace {

struct Quad {
    float x, y, w, h;
    float u0, v0, u1, v1;
};

/* 把 (x,y,w,h) 按半径 r 拆成最多 9 块。返回块数；0 = 参数无效。 */
int BuildNineSlice(float x, float y, float w, float h, float radius, int t, Quad out[9]) {
    if (!out || w <= 0.0f || h <= 0.0f || t <= 0) return 0;
    const float r = std::min(radius, std::min(w, h) * 0.5f);
    if (r <= 0.0f) return 0;
    const float T = static_cast<float>(t);
    const float c = r;            /* 角片边长 = 四分之一圆弧的包围盒 */
    const float cu = c / T;
    const float s0 = 0.5f;        /* 中段取模板正中 1 个像素 */
    const float s1 = 0.5f + 1.0f / T;

    const float mx = w - 2.0f * c;
    const float my = h - 2.0f * c;

    int n = 0;
    auto push = [&](float qx, float qy, float qw, float qh, float u0, float v0, float u1,
                    float v1) {
        if (qw <= 0.0f || qh <= 0.0f || n >= 9) return;
        out[n++] = Quad{qx, qy, qw, qh, u0, v0, u1, v1};
    };
    push(x, y, c, c, 0, 0, cu, cu);                            /* 左上 */
    push(x + w - c, y, c, c, 1 - cu, 0, 1, cu);                /* 右上 */
    push(x, y + h - c, c, c, 0, 1 - cu, cu, 1);                /* 左下 */
    push(x + w - c, y + h - c, c, c, 1 - cu, 1 - cu, 1, 1);    /* 右下 */
    push(x + c, y, mx, c, s0, 0, s1, cu);                      /* 上 */
    push(x + c, y + h - c, mx, c, s0, 1 - cu, s1, 1);          /* 下 */
    push(x, y + c, c, my, 0, s0, cu, s1);                      /* 左 */
    push(x + w - c, y + c, c, my, 1 - cu, s0, 1, s1);          /* 右 */
    push(x + c, y + c, mx, my, s0, s0, s1, s1);                /* 中 */
    return n;
}

/* 一块 → 4 个顶点（TL, TR, BL, BR，与 draw_quads 的约定一致） */
void QuadToVertices(const Quad& q, uint32_t tint, BgpuVertex* v) {
    v[0] = BgpuVertex{q.x, q.y, 0.0f, q.u0, q.v0, tint};
    v[1] = BgpuVertex{q.x + q.w, q.y, 0.0f, q.u1, q.v0, tint};
    v[2] = BgpuVertex{q.x, q.y + q.h, 0.0f, q.u0, q.v1, tint};
    v[3] = BgpuVertex{q.x + q.w, q.y + q.h, 0.0f, q.u1, q.v1, tint};
}

}  // namespace

extern "C" {

BgpuResult bingpu_cmd_draw_rounded_rect(BgpuCmd cmd, BgpuTexture coverage, float x, float y,
                                        float w, float h, float radius, int32_t tile_size,
                                        uint32_t tint) {
    bgpu::ApiScope api("bingpu_cmd_draw_rounded_rect");
    if (!cmd || !coverage) {
        return bgpu::fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__,
                          "cmd / coverage 为空（覆盖率模板要先上传成纹理）");
    }
    Quad q[9];
    const int n = BuildNineSlice(x, y, w, h, radius, tile_size, q);
    if (n <= 0) {
        return bgpu::fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__,
                          "半径/尺寸无效（调用方要先把 radius 夹到 min(w,h)/2）");
    }
    BgpuResult r = bingpu_cmd_set_texture(cmd, coverage, 0);
    if (r != BGPU_OK) return r;
    BgpuVertex verts[9 * 4];
    for (int i = 0; i < n; ++i) QuadToVertices(q[i], tint, verts + i * 4);
    return bingpu_cmd_draw_quads(cmd, verts, static_cast<uint32_t>(n));
}

}  // extern "C"

/* 数组版：一次绑模板、一次 draw_quads。顶点缓冲按批分配 —— 9 块 × 4 顶点 × N。
 * 不做"跨模板合批"：模板一变就得换 bound texture，那是调用方的分组问题（它才知道
 * 哪些形状用同一个半径），这一层只保证"同一张模板的 N 个形状不会退化成 N 次调用"。 */
extern "C" {

BgpuResult bingpu_cmd_draw_rounded_rects(BgpuCmd cmd, BgpuTexture coverage, int32_t tile_size,
                                         const BgpuRoundedRect* rects, uint32_t count) {
    bgpu::ApiScope api("bingpu_cmd_draw_rounded_rects");
    if (!cmd || !coverage) {
        return bgpu::fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__,
                          "cmd / coverage 为空");
    }
    if (!rects && count) {
        return bgpu::fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__, "rects 为空");
    }
    if (count == 0) return BGPU_OK;

    Quad q[9];
    std::vector<BgpuVertex> verts;
    verts.reserve(static_cast<size_t>(count) * 36u);
    uint32_t quads = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const BgpuRoundedRect& r = rects[i];
        const int n = BuildNineSlice(r.x, r.y, r.w, r.h, r.radius, tile_size, q);
        if (n <= 0) continue;   /* 这一项退化：跳过它，别把整批打掉 */
        for (int k = 0; k < n; ++k) {
            BgpuVertex v[4];
            QuadToVertices(q[k], r.tint, v);
            verts.insert(verts.end(), v, v + 4);
        }
        quads += static_cast<uint32_t>(n);
    }
    if (quads == 0) return BGPU_OK;

    BgpuResult rc = bingpu_cmd_set_texture(cmd, coverage, 0);
    if (rc != BGPU_OK) return rc;
    return bingpu_cmd_draw_quads(cmd, verts.data(), quads);
}

}  // extern "C"
