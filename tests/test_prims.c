/* tests/test_prims.c —— 2D 图元的几何展开（纯 C11）
 *
 * 这一层不碰 AA 数学（覆盖率模板由调用方给），所以测试也只钉**几何**：
 *   ① 九宫格必须**刚好铺满矩形**：不留缝、不越界、不叠两层
 *   ② 角片 1:1、中段沿单轴拉伸（用带纹理坐标编码的模板反查）
 *   ③ 半径夹到 min(w,h)/2；半径 <= 0 报错（调用方这时该走直角路径）
 *
 * ①是最狠的一条：模板全 255 时，矩形内每个像素必须**恰好等于** tint，外面一个都不许动。
 * 九宫格算错一格（角片边长不对、中段宽度为负、UV 偏半个纹素）都会在这里露出来。
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bingpu.h"

#define W 64u
#define H 64u

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (cond) {                                                          \
            g_pass++;                                                        \
        } else {                                                             \
            g_fail++;                                                        \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                       \
    do {                                                                     \
        const long long va = (long long)(a), vb = (long long)(b);            \
        if (va == vb) {                                                      \
            g_pass++;                                                        \
        } else {                                                             \
            g_fail++;                                                        \
            printf("  FAIL %s:%d: %s(%lld) != %s(%lld)\n", __FILE__, __LINE__, \
                   #a, va, #b, vb);                                          \
        }                                                                    \
    } while (0)

/* ---- 环境 ---- */
static BgpuDevice g_dev;
static BgpuSwapchain g_sc;
static BgpuCmd g_cmd;
static BgpuTexture g_target;
static uint8_t g_shot[W * H * 4];
static uint32_t g_stride;

static uint32_t dev_px(uint32_t x, uint32_t y) {
    const uint8_t* p = g_shot + ((size_t)y * W + x) * 4u;
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

static int env_init(void) {
    BgpuDeviceDesc dd;
    memset(&dd, 0, sizeof(dd));
    dd.struct_size = (uint32_t)sizeof(dd);
    dd.backend = BGPU_BACKEND_CPU;
    dd.width = W;
    dd.height = H;
    dd.flags = BGPU_DEVICE_HEADLESS;
    if (bingpu_device_create(&dd, &g_dev) != BGPU_OK) {
        printf("  FATAL device_create: %s\n", bingpu_last_error());
        return 0;
    }
    BgpuSwapchainDesc sd;
    memset(&sd, 0, sizeof(sd));
    sd.struct_size = (uint32_t)sizeof(sd);
    sd.width = W;
    sd.height = H;
    sd.format = BGPU_FORMAT_RGBA8;
    if (bingpu_swapchain_create(g_dev, &sd, &g_sc) != BGPU_OK) return 0;
    if (bingpu_cmd_create(g_dev, &g_cmd) != BGPU_OK) return 0;
    return 1;
}

static void env_done(void) {
    if (g_cmd != BGPU_INVALID) bingpu_cmd_destroy(g_dev, g_cmd);
    if (g_sc != BGPU_INVALID) bingpu_swapchain_destroy(g_dev, g_sc);
    if (g_dev != BGPU_INVALID) bingpu_device_destroy(g_dev);
    g_cmd = BGPU_INVALID;
    g_sc = BGPU_INVALID;
    g_dev = BGPU_INVALID;
}

static BgpuPipeline make_textured_pipe(void) {
    BgpuPipelineDesc pd;
    memset(&pd, 0, sizeof(pd));
    pd.struct_size = (uint32_t)sizeof(pd);
    pd.vertex_layout = BGPU_VERTEX_2D;
    pd.blend = BGPU_BLEND_PREMULTIPLIED;
    pd.flags = BGPU_PIPE_TEXTURED;
    BgpuPipeline p = BGPU_INVALID;
    if (bingpu_pipeline_create(g_dev, &pd, &p) != BGPU_OK)
        printf("  FATAL pipeline_create: %s\n", bingpu_last_error());
    return p;
}

static BgpuSampler make_sampler(BgpuFilter f) {
    BgpuSamplerDesc sd;
    memset(&sd, 0, sizeof(sd));
    sd.struct_size = (uint32_t)sizeof(sd);
    sd.min_filter = f;
    sd.mag_filter = f;
    sd.wrap_u = BGPU_WRAP_CLAMP;
    sd.wrap_v = BGPU_WRAP_CLAMP;
    BgpuSampler s = BGPU_INVALID;
    if (bingpu_sampler_create(g_dev, &sd, &s) != BGPU_OK) return BGPU_INVALID;
    return s;
}

/* t×t 模板：value(x,y) 决定四通道（覆盖率模板是"四通道都等于覆盖率"） */
static BgpuTexture make_tile(int t, const uint8_t* rgba) {
    BgpuTextureDesc td;
    memset(&td, 0, sizeof(td));
    td.struct_size = (uint32_t)sizeof(td);
    td.width = (uint32_t)t;
    td.height = (uint32_t)t;
    td.format = BGPU_FORMAT_RGBA8;
    BgpuTexture tex = BGPU_INVALID;
    if (bingpu_texture_create(g_dev, &td, &tex) != BGPU_OK) return BGPU_INVALID;
    if (bingpu_texture_upload(tex, NULL, rgba, 0) != BGPU_OK) return BGPU_INVALID;
    return tex;
}

static void frame_begin_clear(uint32_t clear) {
    if (bingpu_frame_begin(g_sc, g_cmd, &g_target) != BGPU_OK)
        printf("  FATAL frame_begin: %s\n", bingpu_last_error());
    BgpuRenderBeginDesc rd;
    memset(&rd, 0, sizeof(rd));
    rd.struct_size = (uint32_t)sizeof(rd);
    rd.target = g_target;
    rd.flags = BGPU_RENDER_CLEAR_COLOR;
    rd.clear_color = clear;
    if (bingpu_cmd_begin_render(g_cmd, &rd) != BGPU_OK)
        printf("  FATAL begin_render: %s\n", bingpu_last_error());
    (void)bingpu_cmd_set_viewport(g_cmd, 0.0f, 0.0f, (float)W, (float)H);
    (void)bingpu_cmd_set_scissor(g_cmd, 0, 0, (int32_t)W, (int32_t)H);
    (void)bingpu_cmd_set_affine(g_cmd, NULL);
}

static void frame_end_read(void) {
    bingpu_cmd_end_render(g_cmd);
    if (bingpu_frame_end(g_sc, g_cmd) != BGPU_OK)
        printf("  FATAL frame_end: %s\n", bingpu_last_error());
    memset(g_shot, 0xCD, sizeof(g_shot));
    g_stride = 0;
    if (bingpu_frame_read(g_sc, g_shot, sizeof(g_shot), &g_stride) != BGPU_OK)
        printf("  FATAL frame_read: %s\n", bingpu_last_error());
}

/* ============================================================
 * ① 九宫格刚好铺满矩形
 * ============================================================ */
static void test_nine_slice_covers_exactly(void) {
    printf("[nine_slice_covers_exactly]\n");
    if (!env_init()) return;
    BgpuPipeline pipe = make_textured_pipe();
    /* 模板 20×20 全 255（t = 2r+4，r = 8 → 20） */
    uint8_t tile[20 * 20 * 4];
    memset(tile, 255, sizeof(tile));
    BgpuTexture tex = make_tile(20, tile);
    CHECK(tex != BGPU_INVALID);

    const float x = 12.0f, y = 10.0f, w = 40.0f, h = 30.0f, r = 8.0f;
    const uint32_t tint = 0xFF804020u;
    frame_begin_clear(0x00000000u);          /* 透明底 */
    (void)bingpu_cmd_set_pipeline(g_cmd, pipe);
    const BgpuResult rc = bingpu_cmd_draw_rounded_rect(g_cmd, tex, x, y, w, h, r, 20, tint);
    CHECK_EQ(rc, BGPU_OK);
    frame_end_read();

    /* 矩形内每个像素都必须是 tint（模板全白 → 乘出来就是它） */
    int inside_bad = 0, outside_bad = 0;
    for (uint32_t py = 0; py < H; ++py) {
        for (uint32_t px = 0; px < W; ++px) {
            const int inside = (float)px >= x && (float)px < x + w &&
                               (float)py >= y && (float)py < y + h;
            const uint32_t got = dev_px(px, py);
            if (inside && got != tint) ++inside_bad;
            if (!inside && got != 0x00000000u) ++outside_bad;
        }
    }
    CHECK_EQ(inside_bad, 0);    /* 不留缝、不叠暗 */
    CHECK_EQ(outside_bad, 0);   /* 不越界 */
    /* 抽查四个角和中心 */
    CHECK_EQ(dev_px((uint32_t)x, (uint32_t)y), tint);
    CHECK_EQ(dev_px((uint32_t)(x + w) - 1, (uint32_t)(y + h) - 1), tint);
    CHECK_EQ(dev_px((uint32_t)(x + w * 0.5f), (uint32_t)(y + h * 0.5f)), tint);

    bingpu_texture_destroy(g_dev, tex);
    bingpu_pipeline_destroy(g_dev, pipe);
    env_done();
}

/* ============================================================
 * ② 角片 1:1、中段拉伸：用"坐标编码"的模板反查 UV
 *    模板里 R = x * 255/(t-1)、G = y * 255/(t-1)、B = 0、A = 255
 * ============================================================ */
static void test_uv_mapping(void) {
    printf("[uv_mapping]\n");
    if (!env_init()) return;
    BgpuPipeline pipe = make_textured_pipe();
    BgpuSampler near = make_sampler(BGPU_FILTER_NEAREST);
    const int t = 20;
    uint8_t tile[20 * 20 * 4];
    for (int yy = 0; yy < t; ++yy) {
        for (int xx = 0; xx < t; ++xx) {
            uint8_t* p = tile + ((size_t)yy * t + xx) * 4;
            p[0] = (uint8_t)(xx * 255 / (t - 1));
            p[1] = (uint8_t)(yy * 255 / (t - 1));
            p[2] = 0;
            p[3] = 255;
        }
    }
    BgpuTexture tex = make_tile(t, tile);
    CHECK(tex != BGPU_INVALID);

    const float x = 8.0f, y = 8.0f, w = 48.0f, h = 48.0f, r = 8.0f;
    frame_begin_clear(0x00000000u);
    (void)bingpu_cmd_set_pipeline(g_cmd, pipe);
    CHECK_EQ(bingpu_cmd_draw_rounded_rect(g_cmd, tex, x, y, w, h, r, t, 0xFFFFFFFFu), BGPU_OK);
    frame_end_read();

    /* 左上角 1:1：模板 (0,0) 是 (0,0,0) */
    CHECK_EQ(dev_px(8, 8) & 0x00FFFFFFu, 0x00000000u);
    /* 角片右下沿：模板 (r-1, r-1) = (7,7) → R=255*7/19=93, G=93 */
    {
        const uint32_t got = dev_px(8 + 7, 8 + 7) & 0x00FFFFFFu;
        const uint32_t want = (93u << 16) | (93u << 8) | 0u;
        CHECK_EQ(got, want);
    }
    /* 上边中段：UV 的 v 从 0 开始、u 取模板正中 (0.5) → R = 模板 x=10 → 255*10/19=134 */
    {
        const uint32_t px = (uint32_t)(x + w * 0.5f);
        const uint32_t got = dev_px(px, 8) & 0x00FFFFFFu;   /* 顶行 v=0 */
        CHECK_EQ(got & 0x0000FF00u, 0u);                    /* G = 0 说明 v 真的是 0 */
        CHECK_EQ(got >> 16, 134u);                          /* R = 正中那个像素 */
    }
    /* 左边中段：u = 0（模板 x=0），v 取正中 → G = 134 */
    {
        const uint32_t py = (uint32_t)(y + h * 0.5f);
        const uint32_t got = dev_px(8, py) & 0x00FFFFFFu;
        CHECK_EQ(got >> 16, 0u);        /* R = 0 说明 u 真的是 0 */
        CHECK_EQ((got >> 8) & 0xFFu, 134u);
    }
    /* 右下角 1:1：模板 (19,19) → R=G=255、B=0（模板里 B 恒 0） */
    CHECK_EQ(dev_px(8 + 48 - 1, 8 + 48 - 1) & 0x00FFFFFFu, 0x00FFFF00u);

    bingpu_texture_destroy(g_dev, tex);
    if (near != BGPU_INVALID) bingpu_sampler_destroy(g_dev, near);
    bingpu_pipeline_destroy(g_dev, pipe);
    env_done();
}

/* ============================================================
 * ③ 参数边界：radius 夹到 min(w,h)/2；<= 0 或尺寸无效要报错
 * ============================================================ */
static void test_radius_clamp(void) {
    printf("[radius_clamp]\n");
    if (!env_init()) return;
    BgpuPipeline pipe = make_textured_pipe();
    uint8_t tile[20 * 20 * 4];
    memset(tile, 255, sizeof(tile));
    BgpuTexture tex = make_tile(20, tile);

    frame_begin_clear(0x00000000u);
    (void)bingpu_cmd_set_pipeline(g_cmd, pipe);
    /* radius 远大于短边的一半：按 min/2 处理，不报错 */
    CHECK_EQ(bingpu_cmd_draw_rounded_rect(g_cmd, tex, 4.0f, 4.0f, 30.0f, 20.0f, 999.0f, 20,
                                          0xFFFFFFFFu),
             BGPU_OK);
    /* 中心仍然被填满 */
    frame_end_read();
    CHECK_EQ(dev_px(19, 14), 0xFFFFFFFFu);

    /* radius <= 0：调用方该走直角路径，这里必须拒绝 */
    CHECK_EQ(bingpu_cmd_draw_rounded_rect(g_cmd, tex, 4.0f, 4.0f, 30.0f, 20.0f, 0.0f, 20,
                                          0xFFFFFFFFu),
             BGPU_ERR_INVALID_ARGUMENT);
    /* 尺寸无效 */
    CHECK_EQ(bingpu_cmd_draw_rounded_rect(g_cmd, tex, 4.0f, 4.0f, 0.0f, 20.0f, 4.0f, 20,
                                          0xFFFFFFFFu),
             BGPU_ERR_INVALID_ARGUMENT);
    /* 模板缺失 */
    CHECK_EQ(bingpu_cmd_draw_rounded_rect(g_cmd, BGPU_INVALID, 4.0f, 4.0f, 30.0f, 20.0f, 4.0f,
                                          20, 0xFFFFFFFFu),
             BGPU_ERR_INVALID_ARGUMENT);

    bingpu_texture_destroy(g_dev, tex);
    bingpu_pipeline_destroy(g_dev, pipe);
    env_done();
}

/* ============================================================
 * ④ 四角带色（0.11.0）：九宫格 + 四色双线性
 *
 * 这一条要钉的是"渐变在圆角上也是精确解"这个论断，所以判据不是"看起来像"，而是
 *   · 四角同色时，输出必须与平色版**逐字节相同**（同一条几何，只换了顶点色）
 *   · 四角不同色时，每个像素必须等于"四角在盒子上的仿射函数在该像素中心处的值"
 *     （容差 2：顶点色是 8 位整数，三角形内的重心插值比解析式晚一步取整）
 * 像素中心是 +0.5 —— 与上面 UV 那条测试用的是同一个口径。
 * ============================================================ */
static int g_grad_ref[W * H];   /* 平色版的快照 */

static void test_rounded_grad(void) {
    printf("[rounded_grad]\n");
    if (!env_init()) return;
    BgpuPipeline pipe = make_textured_pipe();
    const int t = 20;
    uint8_t tile[20 * 20 * 4];
    memset(tile, 255, sizeof(tile));   /* 全 255：没有 AA，边界像素完全覆盖 */
    BgpuTexture tex = make_tile(t, tile);
    CHECK(tex != BGPU_INVALID);

    const float x = 12.0f, y = 10.0f, w = 40.0f, h = 30.0f, r = 8.0f;
    const uint32_t flat = 0xFF804020u;

    /* A) 四角同色 == 平色版 */
    frame_begin_clear(0x00000000u);
    (void)bingpu_cmd_set_pipeline(g_cmd, pipe);
    CHECK_EQ(bingpu_cmd_draw_rounded_rect(g_cmd, tex, x, y, w, h, r, t, flat), BGPU_OK);
    frame_end_read();
    for (uint32_t i = 0; i < W * H; ++i) {
        const uint8_t* p = g_shot + (size_t)i * 4u;
        g_grad_ref[i] = ((int)p[3] << 24) | ((int)p[0] << 16) | ((int)p[1] << 8) | p[2];
    }

    const uint32_t same[4] = {flat, flat, flat, flat};
    frame_begin_clear(0x00000000u);
    (void)bingpu_cmd_set_pipeline(g_cmd, pipe);
    CHECK_EQ(bingpu_cmd_draw_rounded_rect_grad(g_cmd, tex, x, y, w, h, r, t, same), BGPU_OK);
    frame_end_read();
    int diff = 0;
    for (uint32_t i = 0; i < W * H; ++i) {
        const uint8_t* p = g_shot + (size_t)i * 4u;
        const int got = ((int)p[3] << 24) | ((int)p[0] << 16) | ((int)p[1] << 8) | p[2];
        if (got != g_grad_ref[i]) ++diff;
    }
    CHECK_EQ(diff, 0);   /* 同色时与平色版逐字节相同 */

    /* B) 四角不同色：与盒子上的仿射函数对齐（容差 2）
     *
     * 四角色**必须能由一个仿射函数取出来**（线性渐变的四角一定满足
     * c(TL)+c(BR) == c(TR)+c(BL)）。这是"四角带色能替掉条带逼近"的全部前提：
     * 仿射函数在三角形上是精确的；反过来，随手编的四个角（比如对角同色、另两角同色）
     * 不是任何线性渐变，光栅化出来的双线性**不等于**它 —— 那不是 bug，那是拿错了输入。
     * 这里就按仿射函数 c(u,v) = A + B·u + C·v 造四个角。 */
    const uint32_t corners[4] = {
        0xFF204060u,   /* TL = A                 */
        0xFFD468BAu,   /* TR = A + B             */
        0xFF34B89Cu,   /* BL = A + C             */
        0xFFE8E0F6u,   /* BR = A + B + C         */
    };
    frame_begin_clear(0x00000000u);
    (void)bingpu_cmd_set_pipeline(g_cmd, pipe);
    CHECK_EQ(bingpu_cmd_draw_rounded_rect_grad(g_cmd, tex, x, y, w, h, r, t, corners), BGPU_OK);
    frame_end_read();

    int max_err = 0, bad = 0;
    for (uint32_t py = 11; py <= 38; ++py) {
        for (uint32_t px = 13; px <= 50; ++px) {
            const float u = ((float)px + 0.5f - x) / w;
            const float v = ((float)py + 0.5f - y) / h;
            const int er = (int)(32.0f + 180.0f * u + 20.0f * v + 0.5f);
            const int eg = (int)(64.0f + 40.0f * u + 120.0f * v + 0.5f);
            const int eb = (int)(96.0f + 90.0f * u + 60.0f * v + 0.5f);
            const uint32_t got = dev_px(px, py);
            const int dr = (int)((got >> 16) & 0xFFu) - er;
            const int dg = (int)((got >> 8) & 0xFFu) - eg;
            const int db = (int)(got & 0xFFu) - eb;
            const int da = (int)((got >> 24) & 0xFFu) - 255;
            int e = dr < 0 ? -dr : dr;
            if (dg < 0 ? -dg > e : dg > e) e = dg < 0 ? -dg : dg;
            if (db < 0 ? -db > e : db > e) e = db < 0 ? -db : db;
            if (da < 0 ? -da > e : da > e) e = da < 0 ? -da : da;
            if (e > max_err) max_err = e;
            if (e > 2) ++bad;
        }
    }
    printf("  仿射对拍 max_err=%d bad=%d\n", max_err, bad);
    CHECK_EQ(bad, 0);
    CHECK(max_err <= 2);

    /* 盒子外一个都不许动（渐变不能漏出九宫格） */
    CHECK_EQ(dev_px(4, 4), 0x00000000u);
    CHECK_EQ(dev_px(60, 60), 0x00000000u);

    /* 参数边界 */
    CHECK_EQ(bingpu_cmd_draw_rounded_rect_grad(g_cmd, tex, x, y, w, h, 0.0f, t, corners),
             BGPU_ERR_INVALID_ARGUMENT);
    CHECK_EQ(bingpu_cmd_draw_rounded_rect_grad(g_cmd, tex, x, y, w, h, r, t, NULL),
             BGPU_ERR_INVALID_ARGUMENT);

    bingpu_texture_destroy(g_dev, tex);
    bingpu_pipeline_destroy(g_dev, pipe);
    env_done();
}

int main(void) {
    printf("[test_prims] 2D 图元几何\n");
    test_nine_slice_covers_exactly();
    test_uv_mapping();
    test_radius_clamp();
    test_rounded_grad();
    printf("[test_prims] pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
