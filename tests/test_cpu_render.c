/* tests/test_cpu_render.c —— CPU 参考实现的像素级测试（纯 C11）
 *
 * 这份测试是 CPU 后端的**规格书**：每一条断言都对应 docs/raster_rules.md 里的一条
 * 规则。GPU 后端将来拿 CPU 的出图当基准，所以这里的期望值必须是手算出来的、
 * 而不是"跑一遍看看等于几"抄下来的 —— 每条期望值旁边都写了算式。
 *
 * 覆盖面：clear / 覆盖范围 / top-left 填充规则 / 五种混合模式 / scissor / viewport /
 * 仿射 / 采样器（最近+repeat、线性+clamp）/ 离屏目标回采 / 深度 / 反预乘回读 /
 * R8 遮罩 / 网格索引绘制 / stats 计数。
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bingpu.h"

#define W 8u
#define H 8u

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

static void expect_px(int line, uint32_t x, uint32_t y, uint32_t argb) {
    const uint32_t got = dev_px(x, y);
    if (got == argb) {
        g_pass++;
    } else {
        g_fail++;
        printf("  FAIL %s:%d: 像素(%u,%u) = %08X，期望 %08X\n", __FILE__, line, x, y, got, argb);
    }
}
#define EXPECT_PX(x, y, argb) expect_px(__LINE__, (x), (y), (argb))

static void env_init(uint32_t w, uint32_t h, BgpuFormat fmt) {
    BgpuDeviceDesc dd;
    memset(&dd, 0, sizeof(dd));
    dd.struct_size = (uint32_t)sizeof(dd);
    dd.backend = BGPU_BACKEND_CPU;
    dd.width = w;
    dd.height = h;
    dd.flags = BGPU_DEVICE_HEADLESS;
    if (bingpu_device_create(&dd, &g_dev) != BGPU_OK) {
        printf("  FATAL device_create: %s\n", bingpu_last_error());
        g_fail++;
        return;
    }
    BgpuSwapchainDesc sd;
    memset(&sd, 0, sizeof(sd));
    sd.struct_size = (uint32_t)sizeof(sd);
    sd.width = w;
    sd.height = h;
    sd.format = fmt;
    if (bingpu_swapchain_create(g_dev, &sd, &g_sc) != BGPU_OK) {
        printf("  FATAL swapchain_create: %s\n", bingpu_last_error());
        g_fail++;
    }
    if (bingpu_cmd_create(g_dev, &g_cmd) != BGPU_OK) {
        printf("  FATAL cmd_create: %s\n", bingpu_last_error());
        g_fail++;
    }
    g_target = BGPU_INVALID;
}

static void env_done(void) {
    if (g_cmd != BGPU_INVALID) bingpu_cmd_destroy(g_dev, g_cmd);
    if (g_sc != BGPU_INVALID) bingpu_swapchain_destroy(g_dev, g_sc);
    if (g_dev != BGPU_INVALID) bingpu_device_destroy(g_dev);
    g_cmd = BGPU_INVALID;
    g_sc = BGPU_INVALID;
    g_dev = BGPU_INVALID;
}

/* ---- 绘制小工具 ---- */
static BgpuVertex g_q[4];

static void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
                 uint32_t color, float z) {
    const BgpuVertex tl = {x0, y0, z, u0, v0, color};
    const BgpuVertex tr = {x1, y0, z, u1, v0, color};
    const BgpuVertex bl = {x0, y1, z, u0, v1, color};
    const BgpuVertex br = {x1, y1, z, u1, v1, color};
    g_q[0] = tl;
    g_q[1] = tr;
    g_q[2] = bl;
    g_q[3] = br;
}

static BgpuPipeline make_pipe(BgpuBlend blend, uint32_t flags, BgpuVertexLayout layout) {
    BgpuPipelineDesc pd;
    memset(&pd, 0, sizeof(pd));
    pd.struct_size = (uint32_t)sizeof(pd);
    pd.vertex_layout = layout;
    pd.blend = blend;
    pd.flags = flags;
    BgpuPipeline p = BGPU_INVALID;
    if (bingpu_pipeline_create(g_dev, &pd, &p) != BGPU_OK)
        printf("  FATAL pipeline_create: %s\n", bingpu_last_error());
    return p;
}

static BgpuSampler make_sampler(BgpuFilter filter, BgpuWrap wrap_u, BgpuWrap wrap_v) {
    BgpuSamplerDesc sd;
    memset(&sd, 0, sizeof(sd));
    sd.struct_size = (uint32_t)sizeof(sd);
    sd.min_filter = filter;
    sd.mag_filter = filter;
    sd.wrap_u = wrap_u;
    sd.wrap_v = wrap_v;
    BgpuSampler s = BGPU_INVALID;
    if (bingpu_sampler_create(g_dev, &sd, &s) != BGPU_OK)
        printf("  FATAL sampler_create: %s\n", bingpu_last_error());
    return s;
}

/* 开始一个帧并拿到交换链目标 */
static void begin_frame(void) {
    if (bingpu_frame_begin(g_sc, g_cmd, &g_target) != BGPU_OK)
        printf("  FATAL frame_begin: %s\n", bingpu_last_error());
}

static void begin_render_on(BgpuTexture target, BgpuTexture depth, int clear_color,
                            uint32_t color, int clear_depth, float depth_value) {
    BgpuRenderBeginDesc rd;
    memset(&rd, 0, sizeof(rd));
    rd.struct_size = (uint32_t)sizeof(rd);
    rd.target = target;
    rd.depth_target = depth;
    rd.flags = (clear_color ? BGPU_RENDER_CLEAR_COLOR : 0u) |
               (clear_depth ? BGPU_RENDER_CLEAR_DEPTH : 0u);
    rd.clear_color = color;
    rd.clear_depth = depth_value;
    if (bingpu_cmd_begin_render(g_cmd, &rd) != BGPU_OK)
        printf("  FATAL begin_render: %s\n", bingpu_last_error());
}

static void end_frame_read(void) {
    if (bingpu_frame_end(g_sc, g_cmd) != BGPU_OK)
        printf("  FATAL frame_end: %s\n", bingpu_last_error());
    memset(g_shot, 0xCD, sizeof(g_shot));   /* 先填脏值，确保 read 真的写了 */
    g_stride = 0;
    if (bingpu_frame_read(g_sc, g_shot, sizeof(g_shot), &g_stride) != BGPU_OK)
        printf("  FATAL frame_read: %s\n", bingpu_last_error());
}

/* ============================================================
 * 1) clear：逐像素值
 * ============================================================ */
static void test_clear(void) {
    printf("[clear]\n");
    env_init(W, H, BGPU_FORMAT_RGBA8);
    begin_frame();
    begin_render_on(g_target, BGPU_INVALID, 1, 0xFF3366CCu, 0, 0.0f);
    bingpu_cmd_end_render(g_cmd);
    end_frame_read();
    CHECK_EQ(g_stride, W * 4u);
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++) {
            /* a=255 时不预乘，读回就是原值 */
            if (dev_px(x, y) != 0xFF3366CCu) {
                EXPECT_PX(x, y, 0xFF3366CCu);
                env_done();
                return;
            }
            g_pass++;
        }
    CHECK_EQ(dev_px(0, 0), 0xFF3366CCu);
    env_done();
}

static void test_clear_alpha(void) {
    printf("[clear_alpha]\n");
    env_init(W, H, BGPU_FORMAT_RGBA8);
    begin_frame();
    /* 0x80 FF 80 40：预乘后 (128,64,32,128)，反预乘回来正好是 (255,128,64,128) */
    begin_render_on(g_target, BGPU_INVALID, 1, 0x80FF8040u, 0, 0.0f);
    bingpu_cmd_end_render(g_cmd);
    end_frame_read();
    EXPECT_PX(0, 0, 0x80FF8040u);
    EXPECT_PX(W - 1, H - 1, 0x80FF8040u);
    EXPECT_PX(3, 5, 0x80FF8040u);
    env_done();
}

/* ============================================================
 * 2) 四边形覆盖范围（像素中心 + top-left 规则）
 * ============================================================ */
static void test_quad_coverage(void) {
    printf("[quad_coverage]\n");
    env_init(W, H, BGPU_FORMAT_RGBA8);
    const BgpuPipeline p = make_pipe(BGPU_BLEND_PREMULTIPLIED, 0, BGPU_VERTEX_2D);
    begin_frame();
    begin_render_on(g_target, BGPU_INVALID, 1, 0x00000000u, 0, 0.0f);
    bingpu_cmd_set_pipeline(g_cmd, p);
    quad(2, 2, 5, 5, 0, 0, 1, 1, 0xFFFFFFFFu, 0.0f);   /* 像素中心 2.5..4.5 在 [2,5) 内 */
    CHECK_EQ(bingpu_cmd_draw_quads(g_cmd, g_q, 1), BGPU_OK);
    bingpu_cmd_end_render(g_cmd);
    end_frame_read();
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++) {
            const int inside = (x >= 2 && x < 5 && y >= 2 && y < 5);
            (void)inside;
            EXPECT_PX(x, y, inside ? 0xFFFFFFFFu : 0x00000000u);
        }
    env_done();
}

static void test_quad_fractional(void) {
    printf("[quad_fractional]\n");
    env_init(W, H, BGPU_FORMAT_RGBA8);
    const BgpuPipeline p = make_pipe(BGPU_BLEND_PREMULTIPLIED, 0, BGPU_VERTEX_2D);
    begin_frame();
    begin_render_on(g_target, BGPU_INVALID, 1, 0x00000000u, 0, 0.0f);
    bingpu_cmd_set_pipeline(g_cmd, p);
    /* 2.3..5.7：像素中心 2.5/3.5/4.5/5.5 都落在里面 → 4x4 块，没有半覆盖 */
    quad(2.3f, 2.3f, 5.7f, 5.7f, 0, 0, 1, 1, 0xFFFFFFFFu, 0.0f);
    bingpu_cmd_draw_quads(g_cmd, g_q, 1);
    bingpu_cmd_end_render(g_cmd);
    end_frame_read();
    EXPECT_PX(2, 2, 0xFFFFFFFFu);
    EXPECT_PX(5, 5, 0xFFFFFFFFu);
    EXPECT_PX(1, 2, 0x00000000u);   /* 中心 1.5 < 2.3 */
    EXPECT_PX(6, 5, 0x00000000u);   /* 中心 6.5 > 5.7 */
    env_done();
}

/* 相邻四边形的共享边只被覆盖一次（top-left 规则）：
 * 两个 50% 白的方块并排，若共享边被画两次，那一列的 alpha 会变成 191 而不是 128。 */
static void test_seam_top_left(void) {
    printf("[seam_top_left]\n");
    env_init(W, H, BGPU_FORMAT_RGBA8);
    const BgpuPipeline p = make_pipe(BGPU_BLEND_PREMULTIPLIED, 0, BGPU_VERTEX_2D);
    begin_frame();
    begin_render_on(g_target, BGPU_INVALID, 1, 0x00000000u, 0, 0.0f);
    bingpu_cmd_set_pipeline(g_cmd, p);
    quad(0, 0, 4, 8, 0, 0, 1, 1, 0x80FFFFFFu, 0.0f);
    bingpu_cmd_draw_quads(g_cmd, g_q, 1);
    quad(4, 0, 8, 8, 0, 0, 1, 1, 0x80FFFFFFu, 0.0f);
    bingpu_cmd_draw_quads(g_cmd, g_q, 1);
    bingpu_cmd_end_render(g_cmd);
    end_frame_read();
    for (uint32_t y = 0; y < H; y++) {
        EXPECT_PX(3, y, 0x80FFFFFFu);   /* 左块最后一列：一次 */
        EXPECT_PX(4, y, 0x80FFFFFFu);   /* 右块第一列：一次 */
    }
    EXPECT_PX(0, 0, 0x80FFFFFFu);
    EXPECT_PX(7, 7, 0x80FFFFFFu);
    env_done();
}

/* ============================================================
 * 3) 五种混合模式（期望值手算，见注释）
 *
 *     dst 清成不透明中灰 0xFF808080 → 存储（预乘）= (128,128,128,255)
 *     src 顶点色 0x80FF0000 → 直通 (1,0,0,0.50196) → 预乘 (0.50196,0,0,0.50196)
 *     sa = 0.50196
 *
 *     **注意：混合在预乘域算，frame_read 再把结果反预乘回直通** ——
 *     所以期望值要按"存储 → 反预乘"两步算，直接在预乘值上断言会错。
 *
 *     PREMULTIPLIED: 存储 src + dst*(1-sa) = (0.75196,0.25,0.25,1.0)
 *                    → (192,64,64,255)，a=255 反预乘不变 → FF C0 40 40
 *     REPLACE:       存储 src = (0.50196,0,0,0.50196) → (128,0,0,128)
 *                    → 反预乘 (255,0,0,128) → 80 FF 00 00
 *     KEEP:          存储 dst*sa = (0.25196,0.25196,0.25196,0.50196) → (64,64,64,128)
 *                    → 反预乘 (128,128,128,128) → 80 80 80 80
 *     ERASE:         存储 dst*(1-sa) = (0.25,0.25,0.25,0.49804) → (64,64,64,127)
 *                    → 反预乘 round(64*255/127)=129 → 7F 81 81 81
 *     ADDITIVE:      存储 dst+src = (1.0,0.50196,0.50196,1.0) → (255,128,128,255)
 *                    → 反预乘不变（a=255）→ FF FF 80 80
 * ============================================================ */
static void blend_case(BgpuBlend mode, uint32_t expect) {
    const BgpuPipeline p = make_pipe(mode, 0, BGPU_VERTEX_2D);
    begin_frame();
    begin_render_on(g_target, BGPU_INVALID, 1, 0xFF808080u, 0, 0.0f);
    bingpu_cmd_set_pipeline(g_cmd, p);
    quad(0, 0, W, H, 0, 0, 1, 1, 0x80FF0000u, 0.0f);
    bingpu_cmd_draw_quads(g_cmd, g_q, 1);
    bingpu_cmd_end_render(g_cmd);
    end_frame_read();
    EXPECT_PX(0, 0, expect);
    EXPECT_PX(W - 1, H - 1, expect);
    bingpu_pipeline_destroy(g_dev, p);
}

static void test_blend_modes(void) {
    printf("[blend_modes]\n");
    env_init(W, H, BGPU_FORMAT_RGBA8);
    blend_case(BGPU_BLEND_PREMULTIPLIED, 0xFFC04040u);
    blend_case(BGPU_BLEND_REPLACE, 0x80000000u | 0x00FF0000u);
    blend_case(BGPU_BLEND_KEEP, 0x80808080u);
    blend_case(BGPU_BLEND_ERASE, 0x7F818181u);
    blend_case(BGPU_BLEND_ADDITIVE, 0xFFFF8080u);
    env_done();
}

/* ============================================================
 * 4) 采样器
 * ============================================================ */
static BgpuTexture make_2x2_rgba(void) {
    BgpuTextureDesc td;
    memset(&td, 0, sizeof(td));
    td.struct_size = (uint32_t)sizeof(td);
    td.width = 2;
    td.height = 2;
    td.format = BGPU_FORMAT_RGBA8;
    td.flags = BGPU_TEXTURE_CPU_READ;
    BgpuTexture t = BGPU_INVALID;
    if (bingpu_texture_create(g_dev, &td, &t) != BGPU_OK) return t;
    /* 直通顺序：红 绿 / 蓝 白（行主序，第 0 行在上） */
    const uint8_t px[16] = {
        255, 0,   0,   255,   0,   255, 0,   255,
        0,   0,   255, 255,   255, 255, 255, 255,
    };
    if (bingpu_texture_upload(t, NULL, px, 8) != BGPU_OK) return t;
    return t;
}

static void test_sampler_nearest_repeat(void) {
    printf("[sampler_nearest_repeat]\n");
    env_init(W, H, BGPU_FORMAT_RGBA8);
    const BgpuTexture tex = make_2x2_rgba();
    const BgpuPipeline p = make_pipe(BGPU_BLEND_PREMULTIPLIED, BGPU_PIPE_TEXTURED,
                                     BGPU_VERTEX_2D);
    /* UV 1.0..3.0：像素中心的 u = 1.25 / 1.75 / 2.25 / 2.75
     *   repeat → 0.25, 0.75, 0.25, 0.75 → 纹素 0,1,0,1（红绿红绿）
     *   clamp  → 全部夹到 1.0 → 纹素 floor(2)→夹到 1 = 绿 */
    for (int pass = 0; pass < 2; pass++) {
        const BgpuSampler s = pass == 0 ? make_sampler(BGPU_FILTER_NEAREST, BGPU_WRAP_REPEAT,
                                                       BGPU_WRAP_REPEAT)
                                        : make_sampler(BGPU_FILTER_NEAREST, BGPU_WRAP_CLAMP,
                                                       BGPU_WRAP_CLAMP);
        begin_frame();
        begin_render_on(g_target, BGPU_INVALID, 1, 0x00000000u, 0, 0.0f);
        bingpu_cmd_set_pipeline(g_cmd, p);
        bingpu_cmd_set_texture(g_cmd, tex, s);
        quad(0, 0, 4, 4, 1.0f, 0.0f, 3.0f, 1.0f, 0xFFFFFFFFu, 0.0f);
        bingpu_cmd_draw_quads(g_cmd, g_q, 1);
        bingpu_cmd_end_render(g_cmd);
        end_frame_read();
        if (pass == 0) {
            EXPECT_PX(0, 0, 0xFFFF0000u);
            EXPECT_PX(1, 0, 0xFF00FF00u);
            EXPECT_PX(2, 0, 0xFFFF0000u);
            EXPECT_PX(3, 0, 0xFF00FF00u);
        } else {
            EXPECT_PX(0, 0, 0xFF00FF00u);
            EXPECT_PX(1, 0, 0xFF00FF00u);
            EXPECT_PX(2, 0, 0xFF00FF00u);
            EXPECT_PX(3, 0, 0xFF00FF00u);
        }
        bingpu_sampler_destroy(g_dev, s);
    }
    bingpu_texture_destroy(g_dev, tex);
    bingpu_pipeline_destroy(g_dev, p);
    env_done();
}

static void test_sampler_bilinear(void) {
    printf("[sampler_bilinear]\n");
    env_init(W, H, BGPU_FORMAT_RGBA8);
    const BgpuTexture tex = make_2x2_rgba();
    const BgpuSampler s = make_sampler(BGPU_FILTER_LINEAR, BGPU_WRAP_CLAMP, BGPU_WRAP_CLAMP);
    const BgpuPipeline p = make_pipe(BGPU_BLEND_PREMULTIPLIED, BGPU_PIPE_TEXTURED,
                                     BGPU_VERTEX_2D);
    begin_frame();
    begin_render_on(g_target, BGPU_INVALID, 1, 0x00000000u, 0, 0.0f);
    bingpu_cmd_set_pipeline(g_cmd, p);
    bingpu_cmd_set_texture(g_cmd, tex, s);
    quad(0, 0, 4, 4, 0, 0, 1, 1, 0xFFFFFFFFu, 0.0f);
    bingpu_cmd_draw_quads(g_cmd, g_q, 1);
    bingpu_cmd_end_render(g_cmd);
    end_frame_read();
    /* (1,0)：u=0.375 → x=0.25；v=0.125 → y=-0.25（夹到第 0 行）
     *        = 红*0.75 + 绿*0.25 = (0.75,0.25,0) → (191,64,0) */
    EXPECT_PX(1, 0, 0xFFBF4000u);
    /* (2,1)：u=0.625 → x=0.75；v=0.375 → y=0.25
     *        上排 = (0.25,0.75,0)，下排 = (0.75,0.75,1)
     *        结果 = (0.375,0.75,0.25) → (96,191,64) */
    EXPECT_PX(2, 1, 0xFF60BF40u);
    bingpu_sampler_destroy(g_dev, s);
    bingpu_pipeline_destroy(g_dev, p);
    bingpu_texture_destroy(g_dev, tex);
    env_done();
}

/* ============================================================
 * 5) scissor / viewport / affine
 * ============================================================ */
static void test_scissor(void) {
    printf("[scissor]\n");
    env_init(W, H, BGPU_FORMAT_RGBA8);
    const BgpuPipeline p = make_pipe(BGPU_BLEND_PREMULTIPLIED, 0, BGPU_VERTEX_2D);
    begin_frame();
    begin_render_on(g_target, BGPU_INVALID, 1, 0x00000000u, 0, 0.0f);
    bingpu_cmd_set_pipeline(g_cmd, p);
    CHECK_EQ(bingpu_cmd_set_scissor(g_cmd, 2, 2, 2, 2), BGPU_OK);
    quad(0, 0, W, H, 0, 0, 1, 1, 0xFFFFFFFFu, 0.0f);
    bingpu_cmd_draw_quads(g_cmd, g_q, 1);
    bingpu_cmd_end_render(g_cmd);
    end_frame_read();
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++) {
            const int inside = (x >= 2 && x < 4 && y >= 2 && y < 4);
            EXPECT_PX(x, y, inside ? 0xFFFFFFFFu : 0x00000000u);
        }
    /* 负数尺寸 = 关掉裁剪 */
    begin_frame();
    begin_render_on(g_target, BGPU_INVALID, 1, 0x00000000u, 0, 0.0f);
    bingpu_cmd_set_pipeline(g_cmd, p);
    CHECK_EQ(bingpu_cmd_set_scissor(g_cmd, 0, 0, -1, -1), BGPU_OK);
    quad(0, 0, W, H, 0, 0, 1, 1, 0xFFFFFFFFu, 0.0f);
    bingpu_cmd_draw_quads(g_cmd, g_q, 1);
    bingpu_cmd_end_render(g_cmd);
    end_frame_read();
    EXPECT_PX(0, 0, 0xFFFFFFFFu);
    EXPECT_PX(W - 1, H - 1, 0xFFFFFFFFu);
    bingpu_pipeline_destroy(g_dev, p);
    env_done();
}

static void test_viewport(void) {
    printf("[viewport]\n");
    env_init(W, H, BGPU_FORMAT_RGBA8);
    const BgpuPipeline p = make_pipe(BGPU_BLEND_PREMULTIPLIED, 0, BGPU_VERTEX_2D);
    begin_frame();
    begin_render_on(g_target, BGPU_INVALID, 1, 0x00000000u, 0, 0.0f);
    bingpu_cmd_set_pipeline(g_cmd, p);
    /* viewport 取目标左半：顶点 (0..8) 映射到 x = 0 + (x/8)*4 → 只覆盖左半 */
    CHECK_EQ(bingpu_cmd_set_viewport(g_cmd, 0, 0, 4, 8), BGPU_OK);
    quad(0, 0, 8, 8, 0, 0, 1, 1, 0xFFFFFFFFu, 0.0f);
    bingpu_cmd_draw_quads(g_cmd, g_q, 1);
    bingpu_cmd_end_render(g_cmd);
    end_frame_read();
    EXPECT_PX(0, 0, 0xFFFFFFFFu);
    EXPECT_PX(3, 7, 0xFFFFFFFFu);
    EXPECT_PX(4, 0, 0x00000000u);
    EXPECT_PX(7, 7, 0x00000000u);
    bingpu_pipeline_destroy(g_dev, p);
    env_done();
}

static void test_affine(void) {
    printf("[affine]\n");
    env_init(W, H, BGPU_FORMAT_RGBA8);
    const BgpuPipeline p = make_pipe(BGPU_BLEND_PREMULTIPLIED, 0, BGPU_VERTEX_2D);

    /* 平移 (2,0)：局部 (0,0)-(2,2) → 屏幕 (2,0)-(4,2) */
    const float translate[6] = {1, 0, 2, 0, 1, 0};
    begin_frame();
    begin_render_on(g_target, BGPU_INVALID, 1, 0x00000000u, 0, 0.0f);
    bingpu_cmd_set_pipeline(g_cmd, p);
    CHECK_EQ(bingpu_cmd_set_affine(g_cmd, translate), BGPU_OK);
    quad(0, 0, 2, 2, 0, 0, 1, 1, 0xFFFFFFFFu, 0.0f);
    bingpu_cmd_draw_quads(g_cmd, g_q, 1);
    bingpu_cmd_end_render(g_cmd);
    end_frame_read();
    EXPECT_PX(2, 0, 0xFFFFFFFFu);
    EXPECT_PX(3, 1, 0xFFFFFFFFu);
    EXPECT_PX(0, 0, 0x00000000u);
    EXPECT_PX(4, 0, 0x00000000u);

    /* 缩放 2x：局部 (1,1)-(2,2) → 屏幕 (2,2)-(4,4) */
    const float scale[6] = {2, 0, 0, 0, 2, 0};
    begin_frame();
    begin_render_on(g_target, BGPU_INVALID, 1, 0x00000000u, 0, 0.0f);
    bingpu_cmd_set_pipeline(g_cmd, p);
    CHECK_EQ(bingpu_cmd_set_affine(g_cmd, scale), BGPU_OK);
    quad(1, 1, 2, 2, 0, 0, 1, 1, 0xFFFFFFFFu, 0.0f);
    bingpu_cmd_draw_quads(g_cmd, g_q, 1);
    bingpu_cmd_end_render(g_cmd);
    end_frame_read();
    EXPECT_PX(2, 2, 0xFFFFFFFFu);
    EXPECT_PX(3, 3, 0xFFFFFFFFu);
    EXPECT_PX(1, 1, 0x00000000u);
    EXPECT_PX(4, 2, 0x00000000u);

    /* 传 NULL 复位成单位阵 */
    begin_frame();
    begin_render_on(g_target, BGPU_INVALID, 1, 0x00000000u, 0, 0.0f);
    bingpu_cmd_set_pipeline(g_cmd, p);
    CHECK_EQ(bingpu_cmd_set_affine(g_cmd, NULL), BGPU_OK);
    quad(0, 0, 2, 2, 0, 0, 1, 1, 0xFFFFFFFFu, 0.0f);
    bingpu_cmd_draw_quads(g_cmd, g_q, 1);
    bingpu_cmd_end_render(g_cmd);
    end_frame_read();
    EXPECT_PX(0, 0, 0xFFFFFFFFu);
    EXPECT_PX(1, 1, 0xFFFFFFFFu);
    EXPECT_PX(2, 0, 0x00000000u);
    bingpu_pipeline_destroy(g_dev, p);
    env_done();
}

/* ============================================================
 * 6) 离屏目标 → 回采 → 当纹理用
 * ============================================================ */
static void test_offscreen(void) {
    printf("[offscreen]\n");
    env_init(W, H, BGPU_FORMAT_RGBA8);
    BgpuTextureDesc td;
    memset(&td, 0, sizeof(td));
    td.struct_size = (uint32_t)sizeof(td);
    td.width = 4;
    td.height = 4;
    td.format = BGPU_FORMAT_RGBA8;
    td.flags = BGPU_TEXTURE_RENDER_TARGET | BGPU_TEXTURE_CPU_READ;
    BgpuTexture rt = BGPU_INVALID;
    CHECK(bingpu_texture_create(g_dev, &td, &rt) == BGPU_OK);

    /* 离屏渲染（不需要 frame）：清成不透明绿 */
    BgpuPipeline p = make_pipe(BGPU_BLEND_REPLACE, 0, BGPU_VERTEX_2D);
    begin_render_on(rt, BGPU_INVALID, 1, 0xFF00FF00u, 0, 0.0f);
    bingpu_cmd_end_render(g_cmd);
    /* 回读离屏纹理：预乘存储反预乘后仍是 (0,255,0,255) */
    uint8_t rtpx[4 * 4 * 4];
    uint32_t rtstride = 0;
    CHECK(bingpu_texture_read(rt, rtpx, sizeof(rtpx), &rtstride) == BGPU_OK);
    CHECK_EQ(rtstride, 16u);
    CHECK_EQ(rtpx[0], 0);
    CHECK_EQ(rtpx[1], 255);
    CHECK_EQ(rtpx[2], 0);
    CHECK_EQ(rtpx[3], 255);

    /* 把离屏纹理当纹理画到交换链上（最近采样，避免插值的期望值讨论） */
    const BgpuSampler s = make_sampler(BGPU_FILTER_NEAREST, BGPU_WRAP_CLAMP, BGPU_WRAP_CLAMP);
    p = make_pipe(BGPU_BLEND_PREMULTIPLIED, BGPU_PIPE_TEXTURED, BGPU_VERTEX_2D);
    begin_frame();
    begin_render_on(g_target, BGPU_INVALID, 1, 0x00000000u, 0, 0.0f);
    bingpu_cmd_set_pipeline(g_cmd, p);
    bingpu_cmd_set_texture(g_cmd, rt, s);
    quad(0, 0, W, H, 0, 0, 1, 1, 0xFFFFFFFFu, 0.0f);
    bingpu_cmd_draw_quads(g_cmd, g_q, 1);
    bingpu_cmd_end_render(g_cmd);
    end_frame_read();
    EXPECT_PX(0, 0, 0xFF00FF00u);
    EXPECT_PX(7, 7, 0xFF00FF00u);
    bingpu_sampler_destroy(g_dev, s);
    bingpu_pipeline_destroy(g_dev, p);
    bingpu_texture_destroy(g_dev, rt);
    env_done();
}

/* ============================================================
 * 7) 深度：LESS + 深度写
 * ============================================================ */
static void test_depth(void) {
    printf("[depth]\n");
    env_init(W, H, BGPU_FORMAT_RGBA8);
    BgpuTextureDesc td;
    memset(&td, 0, sizeof(td));
    td.struct_size = (uint32_t)sizeof(td);
    td.width = W;
    td.height = H;
    td.format = BGPU_FORMAT_RGBA8;
    td.flags = BGPU_TEXTURE_RENDER_TARGET;
    BgpuTexture depth = BGPU_INVALID;
    CHECK(bingpu_texture_create(g_dev, &td, &depth) == BGPU_OK);
    const BgpuPipeline p = make_pipe(BGPU_BLEND_REPLACE,
                                     BGPU_PIPE_DEPTH_TEST | BGPU_PIPE_DEPTH_WRITE,
                                     BGPU_VERTEX_2D);

    begin_frame();
    begin_render_on(g_target, depth, 1, 0xFF000000u, 1, 1.0f);
    bingpu_cmd_set_pipeline(g_cmd, p);
    quad(0, 0, W, H, 0, 0, 1, 1, 0xFFFF0000u, 0.5f);     /* 红 z=0.5，先写 */
    bingpu_cmd_draw_quads(g_cmd, g_q, 1);
    quad(0, 0, W, H, 0, 0, 1, 1, 0xFF0000FFu, 0.25f);    /* 蓝 z=0.25，更近 → 赢 */
    bingpu_cmd_draw_quads(g_cmd, g_q, 1);
    quad(0, 0, W, H, 0, 0, 1, 1, 0xFF00FF00u, 0.75f);    /* 绿 z=0.75，更远 → 输 */
    bingpu_cmd_draw_quads(g_cmd, g_q, 1);
    bingpu_cmd_end_render(g_cmd);
    end_frame_read();
    EXPECT_PX(0, 0, 0xFF0000FFu);
    EXPECT_PX(W - 1, H - 1, 0xFF0000FFu);

    /* 只测不写：先红 z=0.5（写），再蓝 z=0.4（通过但不写），再绿 z=0.45（仍比 0.5 近 → 通过）
     * 因为深度没被 0.4 覆盖，绿能过；最终绿。 */
    const BgpuPipeline p_nowrite = make_pipe(BGPU_BLEND_REPLACE, BGPU_PIPE_DEPTH_TEST,
                                             BGPU_VERTEX_2D);
    begin_frame();
    begin_render_on(g_target, depth, 1, 0xFF000000u, 1, 1.0f);
    bingpu_cmd_set_pipeline(g_cmd, p);
    quad(0, 0, W, H, 0, 0, 1, 1, 0xFFFF0000u, 0.5f);
    bingpu_cmd_draw_quads(g_cmd, g_q, 1);
    bingpu_cmd_set_pipeline(g_cmd, p_nowrite);
    quad(0, 0, W, H, 0, 0, 1, 1, 0xFF0000FFu, 0.4f);
    bingpu_cmd_draw_quads(g_cmd, g_q, 1);
    quad(0, 0, W, H, 0, 0, 1, 1, 0xFF00FF00u, 0.45f);
    bingpu_cmd_draw_quads(g_cmd, g_q, 1);
    bingpu_cmd_end_render(g_cmd);
    end_frame_read();
    EXPECT_PX(0, 0, 0xFF00FF00u);
    bingpu_pipeline_destroy(g_dev, p_nowrite);
    bingpu_pipeline_destroy(g_dev, p);
    bingpu_texture_destroy(g_dev, depth);
    env_done();
}

/* ============================================================
 * 8) 上传 / 回读（反预乘）
 * ============================================================ */
static void test_texture_roundtrip(void) {
    printf("[texture_roundtrip]\n");
    env_init(W, H, BGPU_FORMAT_RGBA8);
    BgpuTextureDesc td;
    memset(&td, 0, sizeof(td));
    td.struct_size = (uint32_t)sizeof(td);
    td.width = 4;
    td.height = 4;
    td.format = BGPU_FORMAT_RGBA8;
    td.flags = BGPU_TEXTURE_CPU_READ;
    BgpuTexture t = BGPU_INVALID;
    CHECK(bingpu_texture_create(g_dev, &td, &t) == BGPU_OK);

    /* 只写 (1,1) 一个像素：直通 (255,128,64,128) → 预乘 (128,64,32,128)
     * → 反预乘回 (255,128,64,128)，正好可逆（选的就是能整除的值） */
    const uint8_t one[4] = {255, 128, 64, 128};
    const BgpuBox box = {1, 1, 1, 1};
    CHECK(bingpu_texture_upload(t, &box, one, 4) == BGPU_OK);

    uint8_t out[4 * 4 * 4];
    uint32_t stride = 0;
    CHECK(bingpu_texture_read(t, out, sizeof(out), &stride) == BGPU_OK);
    CHECK_EQ(stride, 16u);
    CHECK_EQ(out[(1 * 4 + 1) * 4 + 0], 255);
    CHECK_EQ(out[(1 * 4 + 1) * 4 + 1], 128);
    CHECK_EQ(out[(1 * 4 + 1) * 4 + 2], 64);
    CHECK_EQ(out[(1 * 4 + 1) * 4 + 3], 128);
    /* 没写过的像素：预乘全 0 → 反预乘也是 0（alpha=0 不产生颜色） */
    CHECK_EQ(out[0], 0);
    CHECK_EQ(out[3], 0);

    /* 行距大于 w*4 也要支持 */
    const uint8_t two_rows[16] = {
        1, 2, 3, 255, 4, 5, 6, 255, 0, 0, 0, 0, 0, 0, 0, 0,
    };
    const BgpuBox box2 = {0, 0, 2, 1};
    CHECK(bingpu_texture_upload(t, &box2, two_rows, 8) == BGPU_OK);
    CHECK(bingpu_texture_read(t, out, sizeof(out), &stride) == BGPU_OK);
    CHECK_EQ(out[0], 1);
    CHECK_EQ(out[4], 4);

    /* 没有 CPU_READ / RENDER_TARGET 的纹理不许回读 */
    td.flags = 0;
    BgpuTexture plain = BGPU_INVALID;
    CHECK(bingpu_texture_create(g_dev, &td, &plain) == BGPU_OK);
    CHECK(bingpu_texture_read(plain, out, sizeof(out), &stride) == BGPU_ERR_UNSUPPORTED);
    bingpu_texture_destroy(g_dev, plain);
    bingpu_texture_destroy(g_dev, t);
    env_done();
}

static void test_r8_mask(void) {
    printf("[r8_mask]\n");
    env_init(W, H, BGPU_FORMAT_RGBA8);
    BgpuTextureDesc td;
    memset(&td, 0, sizeof(td));
    td.struct_size = (uint32_t)sizeof(td);
    td.width = 2;
    td.height = 2;
    td.format = BGPU_FORMAT_R8;
    td.flags = BGPU_TEXTURE_CPU_READ;
    BgpuTexture t = BGPU_INVALID;
    CHECK(bingpu_texture_create(g_dev, &td, &t) == BGPU_OK);
    const uint8_t mask[4] = {255, 0, 0, 255};   /* (0,0)=1,(1,0)=0,(0,1)=0,(1,1)=1 */
    CHECK(bingpu_texture_upload(t, NULL, mask, 2) == BGPU_OK);

    const BgpuSampler s = make_sampler(BGPU_FILTER_NEAREST, BGPU_WRAP_CLAMP, BGPU_WRAP_CLAMP);
    const BgpuPipeline p = make_pipe(BGPU_BLEND_PREMULTIPLIED, BGPU_PIPE_TEXTURED,
                                     BGPU_VERTEX_2D);
    begin_frame();
    begin_render_on(g_target, BGPU_INVALID, 1, 0x00000000u, 0, 0.0f);
    bingpu_cmd_set_pipeline(g_cmd, p);
    bingpu_cmd_set_texture(g_cmd, t, s);
    quad(0, 0, 2, 2, 0, 0, 1, 1, 0xFFFFFFFFu, 0.0f);
    bingpu_cmd_draw_quads(g_cmd, g_q, 1);
    bingpu_cmd_end_render(g_cmd);
    end_frame_read();
    EXPECT_PX(0, 0, 0xFFFFFFFFu);
    EXPECT_PX(1, 0, 0x00000000u);
    EXPECT_PX(0, 1, 0x00000000u);
    EXPECT_PX(1, 1, 0xFFFFFFFFu);
    bingpu_sampler_destroy(g_dev, s);
    bingpu_pipeline_destroy(g_dev, p);
    bingpu_texture_destroy(g_dev, t);
    env_done();
}

/* ============================================================
 * 9) 网格（draw_indexed）
 * ============================================================ */
static void test_mesh(void) {
    printf("[mesh]\n");
    env_init(W, H, BGPU_FORMAT_RGBA8);
    BgpuBufferDesc bd;
    memset(&bd, 0, sizeof(bd));
    bd.struct_size = (uint32_t)sizeof(bd);
    bd.size = (uint32_t)sizeof(BgpuMeshVertex) * 3u;
    bd.usage = BGPU_BUFFER_VERTEX;
    BgpuBuffer vb = BGPU_INVALID;
    CHECK(bingpu_buffer_create(g_dev, &bd, &vb) == BGPU_OK);
    BgpuMeshVertex verts[3];
    memset(verts, 0, sizeof(verts));
    /* 三角形 (0,0)-(8,0)-(0,8)：覆盖目标左上半个三角 */
    verts[0].x = 0;
    verts[0].y = 0;
    verts[0].color = 0xFFFFFFFFu;
    verts[1].x = 8;
    verts[1].color = 0xFFFFFFFFu;
    verts[2].y = 8;
    verts[2].color = 0xFFFFFFFFu;
    CHECK(bingpu_buffer_update(vb, 0, verts, sizeof(verts)) == BGPU_OK);

    bd.size = 3u * 4u;
    bd.usage = BGPU_BUFFER_INDEX;
    BgpuBuffer ib = BGPU_INVALID;
    CHECK(bingpu_buffer_create(g_dev, &bd, &ib) == BGPU_OK);
    const uint32_t idx[3] = {0, 1, 2};
    CHECK(bingpu_buffer_update(ib, 0, idx, sizeof(idx)) == BGPU_OK);

    const BgpuPipeline p = make_pipe(BGPU_BLEND_PREMULTIPLIED, 0, BGPU_VERTEX_MESH);
    begin_frame();
    begin_render_on(g_target, BGPU_INVALID, 1, 0x00000000u, 0, 0.0f);
    bingpu_cmd_set_pipeline(g_cmd, p);
    CHECK_EQ(bingpu_cmd_draw_indexed(g_cmd, vb, 0, ib, 3, 0), BGPU_OK);
    bingpu_cmd_end_render(g_cmd);
    end_frame_read();
    EXPECT_PX(1, 1, 0xFFFFFFFFu);   /* 三角形内部 */
    EXPECT_PX(6, 6, 0x00000000u);   /* 斜边外侧 */
    EXPECT_PX(0, 3, 0xFFFFFFFFu);   /* 左边内侧（避开斜边上的像素中心） */
    bingpu_pipeline_destroy(g_dev, p);
    bingpu_buffer_destroy(g_dev, vb);
    bingpu_buffer_destroy(g_dev, ib);
    env_done();
}

/* ============================================================
 * 9b) 固定功能光照 + 透视校正（ABI 3）
 *
 * 这两条是 bin3d 的 GPU 阶段能不能用的前提，所以判据必须是**手算的数值**，
 * 不能只看"画出来不是黑的"：
 *   · 光照：颜色 = base·vcol·tex × (ambient + lightColor·max(dot(n,dir),0))，
 *     在**直通空间** clamp，最后乘 alpha。
 *   · 透视校正：属性按 Σ(attr/w·λ) / Σ(1/w·λ) 插值。
 * ============================================================ */
static void test_lighting(void) {
    printf("[lighting]\n");
    struct Case {
        float nx, ny, nz;
        uint8_t expect;   /* 直通 RGB（灰度）：0.1 → 26，1.1 clamp 到 1.0 → 255 */
        const char* name;
    };
    const struct Case cases[] = {
        {0.0f, 1.0f, 0.0f, 255, "法线朝光源：ndl=1 → clamp(0.1+1.0)=1.0"},
        {0.0f, -1.0f, 0.0f, 26, "法线背光：ndl=0 → 只剩 ambient 0.1"},
        /* 45°：ndl = cos45 = 0.70710678；0.1 + 0.7071 = 0.8071 → 0.8071*255 = 205.8 → 206 */
        {-0.70710678f, 0.70710678f, 0.0f, 206, "法线斜 45°：ndl=0.7071 → 206"},
    };
    for (size_t ci = 0; ci < sizeof(cases) / sizeof(cases[0]); ci++) {
        env_init(W, H, BGPU_FORMAT_RGBA8);

        BgpuBuffer vb = BGPU_INVALID, ib = BGPU_INVALID;
        BgpuBufferDesc bd;
        memset(&bd, 0, sizeof(bd));
        bd.struct_size = (uint32_t)sizeof(bd);
        bd.size = (uint32_t)sizeof(BgpuMeshVertex) * 4u;
        bd.usage = BGPU_BUFFER_VERTEX;
        CHECK(bingpu_buffer_create(g_dev, &bd, &vb) == BGPU_OK);
        bd.size = 6u * 4u;
        bd.usage = BGPU_BUFFER_INDEX;
        CHECK(bingpu_buffer_create(g_dev, &bd, &ib) == BGPU_OK);
        const uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
        CHECK(bingpu_buffer_update(ib, 0, idx, sizeof(idx)) == BGPU_OK);

        BgpuMeshVertex verts[4];
        memset(verts, 0, sizeof(verts));
        const float xs[4] = {0, 8, 8, 0}, ys[4] = {0, 0, 8, 8};
        for (int k = 0; k < 4; k++) {
            verts[k].x = xs[k];
            verts[k].y = ys[k];
            verts[k].color = 0xFFFFFFFFu;      /* 白色 → 结果直接反映光照 */
            verts[k].nx = cases[ci].nx;
            verts[k].ny = cases[ci].ny;
            verts[k].nz = cases[ci].nz;
            verts[k].w = 1.0f;
        }
        CHECK(bingpu_buffer_update(vb, 0, verts, sizeof(verts)) == BGPU_OK);

        const BgpuPipeline p =
            make_pipe(BGPU_BLEND_PREMULTIPLIED, BGPU_PIPE_LIGHTING, BGPU_VERTEX_MESH);
        BgpuLight light;
        memset(&light, 0, sizeof(light));
        light.dir[1] = 1.0f;          /* 光照方向朝 +y（屏幕向下），法线朝上时 ndl=1 */
        light.color[0] = light.color[1] = light.color[2] = 1.0f;
        light.ambient[0] = light.ambient[1] = light.ambient[2] = 0.1f;

        begin_frame();
        begin_render_on(g_target, BGPU_INVALID, 1, 0x00000000u, 0, 0.0f);
        bingpu_cmd_set_pipeline(g_cmd, p);
        CHECK_EQ(bingpu_cmd_set_light(g_cmd, &light), BGPU_OK);
        CHECK_EQ(bingpu_cmd_draw_indexed(g_cmd, vb, 0, ib, 6, 0), BGPU_OK);
        bingpu_cmd_end_render(g_cmd);
        end_frame_read();
        const uint32_t want = 0xFF000000u | ((uint32_t)cases[ci].expect << 16) |
                              ((uint32_t)cases[ci].expect << 8) | cases[ci].expect;
        EXPECT_PX(6, 2, want);    /* 右上三角形内部（避开 x==y 的斜边） */
        EXPECT_PX(2, 6, want);    /* 左下三角形内部（避开斜边） */

        /* 2D 管线开 LIGHTING 必须被挡掉（2D 顶点没有法线） */
        BgpuPipelineDesc pd;
        memset(&pd, 0, sizeof(pd));
        pd.struct_size = (uint32_t)sizeof(pd);
        pd.vertex_layout = BGPU_VERTEX_2D;
        pd.flags = BGPU_PIPE_LIGHTING;
        BgpuPipeline bad = BGPU_INVALID;
        CHECK_EQ(bingpu_pipeline_create(g_dev, &pd, &bad), BGPU_ERR_INVALID_ARGUMENT);

        /* 非法光照方向要被拒绝（后端不做归一化，方向不是单位长度就没有确定语义） */
        BgpuLight badLight;
        memset(&badLight, 0, sizeof(badLight));
        badLight.dir[0] = 3.0f;   /* 长度平方 9 > 4 */
        CHECK_EQ(bingpu_cmd_set_light(g_cmd, &badLight), BGPU_ERR_INVALID_ARGUMENT);
        /* NULL = 复位默认 */
        CHECK_EQ(bingpu_cmd_set_light(g_cmd, NULL), BGPU_OK);

        bingpu_pipeline_destroy(g_dev, p);
        bingpu_buffer_destroy(g_dev, vb);
        bingpu_buffer_destroy(g_dev, ib);
        env_done();
        printf("  %s\n", cases[ci].name);
    }
}

/* 透视校正：两个顶点 w 不同时，中间像素的 UV 必须按 1/w 加权。
 *
 * 构造：一条水平线段从 x=0 到 x=8，左端 w=1（近）、右端 w=4（远）。
 * 纹理是 2x1 的 {黑, 白}。屏幕空间线性插值会在 x=4 处取到 u=0.5，
 * 透视校正后 u = (0.5·1/1) / (0.5·1/1 + 0.5·1/4) = 0.5/0.625 = 0.8 —— 明显不同。
 * 所以"x=4 处是黑还是白"就能分辨走的是哪条公式。 */
static void test_perspective(void) {
    printf("[perspective]\n");
    env_init(W, H, BGPU_FORMAT_RGBA8);
    BgpuBuffer vb = BGPU_INVALID, ib = BGPU_INVALID;
    BgpuBufferDesc bd;
    memset(&bd, 0, sizeof(bd));
    bd.struct_size = (uint32_t)sizeof(bd);
    bd.size = (uint32_t)sizeof(BgpuMeshVertex) * 4u;
    bd.usage = BGPU_BUFFER_VERTEX;
    CHECK(bingpu_buffer_create(g_dev, &bd, &vb) == BGPU_OK);
    bd.size = 6u * 4u;
    bd.usage = BGPU_BUFFER_INDEX;
    CHECK(bingpu_buffer_create(g_dev, &bd, &ib) == BGPU_OK);
    const uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
    CHECK(bingpu_buffer_update(ib, 0, idx, sizeof(idx)) == BGPU_OK);

    /* 16x16 的目标，纹理 2x1：左半黑、右半白，NEAREST 采样（避免双线性把边界糊开） */
    BgpuTextureDesc td;
    memset(&td, 0, sizeof(td));
    td.struct_size = (uint32_t)sizeof(td);
    td.width = 2;
    td.height = 1;
    td.format = BGPU_FORMAT_RGBA8;
    BgpuTexture tex = BGPU_INVALID;
    CHECK(bingpu_texture_create(g_dev, &td, &tex) == BGPU_OK);
    const uint8_t px[8] = {0, 0, 0, 255, 255, 255, 255, 255};
    CHECK(bingpu_texture_upload(tex, NULL, px, 8) == BGPU_OK);
    BgpuSamplerDesc sd;
    memset(&sd, 0, sizeof(sd));
    sd.struct_size = (uint32_t)sizeof(sd);
    sd.min_filter = BGPU_FILTER_NEAREST;
    sd.mag_filter = BGPU_FILTER_NEAREST;
    sd.wrap_u = BGPU_WRAP_CLAMP;
    sd.wrap_v = BGPU_WRAP_CLAMP;
    BgpuSampler samp = BGPU_INVALID;
    CHECK(bingpu_sampler_create(g_dev, &sd, &samp) == BGPU_OK);

    BgpuMeshVertex verts[4];
    memset(verts, 0, sizeof(verts));
    const float xs[4] = {0, 8, 8, 0}, ys[4] = {0, 0, 8, 8};
    const float us[4] = {0, 1, 1, 0};
    for (int k = 0; k < 4; k++) {
        verts[k].x = xs[k];
        verts[k].y = ys[k];
        verts[k].u = us[k];
        verts[k].color = 0xFFFFFFFFu;
        verts[k].w = (k == 0 || k == 3) ? 1.0f : 4.0f;   /* 左边近、右边远 */
    }
    CHECK(bingpu_buffer_update(vb, 0, verts, sizeof(verts)) == BGPU_OK);

    const BgpuPipeline p =
        make_pipe(BGPU_BLEND_PREMULTIPLIED, BGPU_PIPE_TEXTURED, BGPU_VERTEX_MESH);
    begin_frame();
    begin_render_on(g_target, BGPU_INVALID, 1, 0x00000000u, 0, 0.0f);
    bingpu_cmd_set_pipeline(g_cmd, p);
    bingpu_cmd_set_texture(g_cmd, tex, samp);
    CHECK_EQ(bingpu_cmd_draw_indexed(g_cmd, vb, 0, ib, 6, 0), BGPU_OK);
    bingpu_cmd_end_render(g_cmd);
    end_frame_read();
    /* 左端远小于 0.8 → 仍取左（黑）纹素；右端接近 1 → 取右（白）纹素。
     * 关键是中间：屏幕线性会取到 u≈0.5（黑），透视校正取到 u≈0.8（白）。 */
    EXPECT_PX(4, 1, 0xFF000000u);    /* 判据点：线性 u=0.5625(白) vs 校正 u=0.2432(黑) */
    EXPECT_PX(7, 1, 0xFFFFFFFFu);    /* 右端：两种插值都 > 0.5 → 白 */
    EXPECT_PX(1, 2, 0xFF000000u);    /* 左端：u≈0.19 → 黑 */
    bingpu_pipeline_destroy(g_dev, p);
    bingpu_sampler_destroy(g_dev, samp);
    bingpu_texture_destroy(g_dev, tex);
    bingpu_buffer_destroy(g_dev, vb);
    bingpu_buffer_destroy(g_dev, ib);
    env_done();
}

/* ============================================================
 * 10) 交换链格式 BGRA8 + stats
 * ============================================================ */
static void test_bgra_swapchain(void) {
    printf("[bgra_swapchain]\n");
    env_init(W, H, BGPU_FORMAT_BGRA8);
    BgpuSwapchainInfo si;
    memset(&si, 0, sizeof(si));
    si.struct_size = (uint32_t)sizeof(si);
    CHECK(bingpu_swapchain_info(g_sc, &si) == BGPU_OK);
    CHECK(si.format == BGPU_FORMAT_BGRA8);
    begin_frame();
    begin_render_on(g_target, BGPU_INVALID, 1, 0xFF112233u, 0, 0.0f);
    bingpu_cmd_end_render(g_cmd);
    end_frame_read();
    /* frame_read 的输出永远是 RGBA 顺序，与交换链内部格式无关 */
    EXPECT_PX(0, 0, 0xFF112233u);
    env_done();
}

static void test_stats(void) {
    printf("[stats]\n");
    env_init(W, H, BGPU_FORMAT_RGBA8);
    BgpuStats st;
    memset(&st, 0, sizeof(st));
    st.struct_size = (uint32_t)sizeof(st);
    CHECK(bingpu_device_stats(g_dev, &st) == BGPU_OK);
    const uint64_t live0 = st.bytes_live;
    CHECK(live0 > 0);   /* 交换链的两块位图已经算进去了 */

    const BgpuPipeline p = make_pipe(BGPU_BLEND_PREMULTIPLIED, 0, BGPU_VERTEX_2D);
    begin_frame();
    begin_render_on(g_target, BGPU_INVALID, 1, 0x00000000u, 0, 0.0f);
    bingpu_cmd_set_pipeline(g_cmd, p);
    quad(0, 0, 4, 4, 0, 0, 1, 1, 0xFFFFFFFFu, 0.0f);
    bingpu_cmd_draw_quads(g_cmd, g_q, 3);
    bingpu_cmd_end_render(g_cmd);
    /* 往借来的帧目标传 1 个像素：bytes_uploaded 必须有数（它统计的是真的拷进去的字节） */
    const uint8_t one[4] = {1, 2, 3, 255};
    const BgpuBox b1 = {0, 0, 1, 1};
    CHECK(bingpu_texture_upload(g_target, &b1, one, 4) == BGPU_OK);
    end_frame_read();

    memset(&st, 0, sizeof(st));
    st.struct_size = (uint32_t)sizeof(st);
    CHECK(bingpu_device_stats(g_dev, &st) == BGPU_OK);
    CHECK_EQ(st.frames_presented, 1u);
    CHECK_EQ(st.frame_index, 1u);
    CHECK_EQ(st.draws, 1u);
    CHECK_EQ(st.quads, 3u);
    CHECK_EQ(st.triangles, 6u);
    CHECK(st.bytes_uploaded > 0);

    /* bytes_live 跟着资源生死走 */
    BgpuTextureDesc td;
    memset(&td, 0, sizeof(td));
    td.struct_size = (uint32_t)sizeof(td);
    td.width = 16;
    td.height = 16;
    td.format = BGPU_FORMAT_RGBA8;
    BgpuTexture t = BGPU_INVALID;
    CHECK(bingpu_texture_create(g_dev, &td, &t) == BGPU_OK);
    BgpuStats st2;
    memset(&st2, 0, sizeof(st2));
    st2.struct_size = (uint32_t)sizeof(st2);
    CHECK(bingpu_device_stats(g_dev, &st2) == BGPU_OK);
    CHECK(st2.bytes_live >= st.bytes_live + 16u * 16u * 4u);
    bingpu_texture_destroy(g_dev, t);
    memset(&st2, 0, sizeof(st2));
    st2.struct_size = (uint32_t)sizeof(st2);
    CHECK(bingpu_device_stats(g_dev, &st2) == BGPU_OK);
    CHECK_EQ(st2.bytes_live, st.bytes_live);

    bingpu_pipeline_destroy(g_dev, p);
    env_done();
}

/* ============================================================
 * 11) 交换链 resize 后照常画
 * ============================================================ */
static void test_resize(void) {
    printf("[resize]\n");
    env_init(W, H, BGPU_FORMAT_RGBA8);
    CHECK(bingpu_swapchain_resize(g_sc, 16, 16) == BGPU_OK);
    BgpuSwapchainInfo si;
    memset(&si, 0, sizeof(si));
    si.struct_size = (uint32_t)sizeof(si);
    CHECK(bingpu_swapchain_info(g_sc, &si) == BGPU_OK);
    CHECK_EQ(si.width, 16u);
    CHECK_EQ(si.height, 16u);
    begin_frame();
    begin_render_on(g_target, BGPU_INVALID, 1, 0xFF445566u, 0, 0.0f);
    bingpu_cmd_end_render(g_cmd);
    if (bingpu_frame_end(g_sc, g_cmd) == BGPU_OK) {
        uint8_t big[16 * 16 * 4];
        uint32_t stride = 0;
        CHECK(bingpu_frame_read(g_sc, big, sizeof(big), &stride) == BGPU_OK);
        CHECK_EQ(stride, 64u);
        CHECK_EQ(big[0], 0x44);
        CHECK_EQ(big[1], 0x55);
        CHECK_EQ(big[2], 0x66);
        CHECK_EQ(big[3], 0xFF);
    } else {
        CHECK(0);
    }
    env_done();
}

int main(void) {
    printf("binGPU CPU 参考实现像素测试（%ux%u）\n", W, H);
    test_clear();
    test_clear_alpha();
    test_quad_coverage();
    test_quad_fractional();
    test_seam_top_left();
    test_blend_modes();
    test_sampler_nearest_repeat();
    test_sampler_bilinear();
    test_scissor();
    test_viewport();
    test_affine();
    test_offscreen();
    test_depth();
    test_texture_roundtrip();
    test_r8_mask();
    test_mesh();
    test_lighting();
    test_perspective();
    test_bgra_swapchain();
    test_stats();
    test_resize();
    printf("通过 %d 项，失败 %d 项\n", g_pass, g_fail);
    bingpu_shutdown();
    return g_fail == 0 ? 0 : 1;
}
