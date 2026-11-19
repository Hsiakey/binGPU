/* tests/test_shape.c —— 解析圆角（ABI 4）：AA 数学跑在 GPU 上（纯 C11）
 *
 * 这一层没有覆盖率模板、没有纹理：片元着色器用有符号距离场算覆盖率。
 * 所以测试只钉三件事：
 *   ① 形状结构：圆角矩形内部实、角落空；环带中间是洞
 *   ② 覆盖率是**连续**的：沿弧扫描，必有半覆盖像素（不是 0/1 硬边）
 *   ③ 四端一致：CPU 参考后端与各 GPU 后端逐像素相同（≤1）
 *
 * 没实现的后端（BGPU_CAP_SHAPE 为 0）跳过，不算失败。
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bingpu.h"

#define W 160u
#define H 100u

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

static uint8_t g_shot[W * H * 4];

/* 一个矩形 + 内圈环带的顶点（TL,TR,BL,BR）。tint 一律不透明白。 */
static void push_shape(BgpuShapeVertex* v, float x, float y, float w, float h, float radius,
                       float thick, uint32_t tint) {
    const float cx = x + w * 0.5f, cy = y + h * 0.5f;
    const float hx = w * 0.5f, hy = h * 0.5f;
    const float xs[4] = {x, x + w, x, x + w};
    const float ys[4] = {y, y, y + h, y + h};
    for (int i = 0; i < 4; ++i) {
        v[i].x = xs[i];
        v[i].y = ys[i];
        v[i].lx = xs[i] - cx;
        v[i].ly = ys[i] - cy;
        v[i].hx = hx;
        v[i].hy = hy;
        v[i].radius = radius;
        v[i].thick = thick;
        v[i].tint = tint;
    }
}

static int alpha_at(uint32_t x, uint32_t y) { return g_shot[((size_t)y * W + x) * 4u + 3]; }

/* 跑一遍某个后端，把像素放进 out。返回 0 = 这个后端不支持解析圆角。 */
static int render(BgpuBackendKind kind, uint8_t* out, int* out_cov_count) {
    BgpuDevice dev = BGPU_INVALID;
    BgpuSwapchain sc = BGPU_INVALID;
    BgpuCmd cmd = BGPU_INVALID;
    BgpuPipeline pipe = BGPU_INVALID;

    BgpuDeviceDesc dd;
    memset(&dd, 0, sizeof(dd));
    dd.struct_size = (uint32_t)sizeof(dd);
    dd.backend = kind;
    dd.width = W;
    dd.height = H;
    dd.flags = BGPU_DEVICE_HEADLESS;
    if (bingpu_device_create(&dd, &dev) != BGPU_OK) return 0;

    BgpuCaps caps;
    memset(&caps, 0, sizeof(caps));
    caps.struct_size = (uint32_t)sizeof(caps);
    if (bingpu_device_caps(dev, &caps) != BGPU_OK || !(caps.flags & BGPU_CAP_SHAPE)) {
        bingpu_device_destroy(dev);
        return 0;
    }

    BgpuSwapchainDesc sd;
    memset(&sd, 0, sizeof(sd));
    sd.struct_size = (uint32_t)sizeof(sd);
    sd.width = W;
    sd.height = H;
    sd.format = BGPU_FORMAT_RGBA8;
    if (bingpu_swapchain_create(dev, &sd, &sc) != BGPU_OK) { bingpu_device_destroy(dev); return 0; }
    if (bingpu_cmd_create(dev, &cmd) != BGPU_OK) { bingpu_device_destroy(dev); return 0; }

    BgpuPipelineDesc pd;
    memset(&pd, 0, sizeof(pd));
    pd.struct_size = (uint32_t)sizeof(pd);
    pd.vertex_layout = BGPU_VERTEX_SHAPE;
    pd.blend = BGPU_BLEND_PREMULTIPLIED;
    pd.flags = BGPU_PIPE_SHAPE;
    if (bingpu_pipeline_create(dev, &pd, &pipe) != BGPU_OK) {
        printf("  pipeline 失败：%s\n", bingpu_last_error());
        bingpu_device_destroy(dev);
        return 0;
    }

    BgpuTexture target = BGPU_INVALID;
    if (bingpu_frame_begin(sc, cmd, &target) != BGPU_OK) return 0;
    BgpuRenderBeginDesc rd;
    memset(&rd, 0, sizeof(rd));
    rd.struct_size = (uint32_t)sizeof(rd);
    rd.target = target;
    rd.flags = BGPU_RENDER_CLEAR_COLOR;
    rd.clear_color = 0x00000000u;
    (void)bingpu_cmd_begin_render(cmd, &rd);
    (void)bingpu_cmd_set_viewport(cmd, 0.0f, 0.0f, (float)W, (float)H);
    (void)bingpu_cmd_set_scissor(cmd, 0, 0, (int32_t)W, (int32_t)H);
    (void)bingpu_cmd_set_affine(cmd, NULL);
    (void)bingpu_cmd_set_pipeline(cmd, pipe);

    BgpuShapeVertex v[8];
    /* ① 实心：box(20,20,80,40) r=12 */
    push_shape(v, 20.0f, 20.0f, 80.0f, 40.0f, 12.0f, 0.0f, 0xFFFFFFFFu);
    /* ② 环带：box(112,16,40,40) r=16 thick=4（圆心那块必须是洞） */
    push_shape(v + 4, 112.0f, 16.0f, 40.0f, 40.0f, 16.0f, 4.0f, 0xFFFFFFFFu);
    const BgpuResult dr = bingpu_cmd_draw_shapes(cmd, v, 2);
    if (dr != BGPU_OK) printf("  draw_shapes 失败：%s\n", bingpu_last_error());
    (void)bingpu_cmd_end_render(cmd);
    (void)bingpu_frame_end(sc, cmd);
    memset(g_shot, 0xCD, sizeof(g_shot));
    uint32_t stride = 0;
    const BgpuResult rr = bingpu_frame_read(sc, g_shot, sizeof(g_shot), &stride);
    if (stride == 0) stride = W * 4u;

    /* stride 归一成紧凑行（read 出来的行距可能是 W*4，也可能更大） */
    if (stride != W * 4u) {
        for (uint32_t y = 0; y < H; ++y)
            memmove(g_shot + (size_t)y * W * 4u, g_shot + (size_t)y * stride, W * 4u);
    }
    if (out) memcpy(out, g_shot, sizeof(g_shot));

    /* 连续覆盖率：整幅图里 0 < a < 255 的像素数 */
    int partial = 0;
    for (uint32_t i = 0; i < W * H; ++i) {
        const int a = g_shot[i * 4u + 3];
        if (a > 0 && a < 255) ++partial;
    }
    if (out_cov_count) *out_cov_count = partial;
    (void)rr;
    (void)dr;

    bingpu_pipeline_destroy(dev, pipe);
    bingpu_cmd_destroy(dev, cmd);
    bingpu_swapchain_destroy(dev, sc);
    bingpu_device_destroy(dev);
    return 1;
}

int main(void) {
    printf("[test_shape] 解析圆角（SDF，AA 在片元里）\n");

    uint8_t cpu_px[W * H * 4];
    int cpu_partial = 0;
    if (!render(BGPU_BACKEND_CPU, cpu_px, &cpu_partial)) {
        printf("  FATAL：CPU 参考后端不支持解析圆角（应该支持）\n");
        return 1;
    }
    memcpy(g_shot, cpu_px, sizeof(g_shot));

    /* ① 实心：中心满覆盖、四角空 */
    CHECK(alpha_at(60, 40) == 255);
    CHECK(alpha_at(20, 20) == 0);          /* 圆角外面 */
    CHECK(alpha_at(21, 21) == 0);
    CHECK(alpha_at(100, 60) == 0);         /* 盒子外面 */
    CHECK(alpha_at(60, 19) == 0);
    /* 整数对齐的直边：像素边界与形状边界重合 ⇒ 边界那一列是满覆盖（不是半覆盖）。
     * 半覆盖只出现在**弧**上 —— 那才是 AA 该起作用的地方。 */
    CHECK(alpha_at(20, 40) == 255);
    CHECK(alpha_at(19, 40) == 0);

    /* ② 环带：中心是洞 */
    CHECK(alpha_at(132, 36) == 0);
    CHECK(alpha_at(132, 17) == 255);        /* 上边带子 */
    CHECK(alpha_at(132, 55) == 255);        /* 下边带子 */
    CHECK(alpha_at(113, 36) == 255);        /* 左边带子 */
    CHECK(alpha_at(151, 36) == 255);        /* 右边带子 */

    /* ③ 覆盖率是连续的（不是 0/1 硬边）：只数**弧上**那些像素 */
    CHECK(cpu_partial > 100);
    printf("  半覆盖像素 %d 个（整幅 %u）\n", cpu_partial, W * H);

    /* ③b CPU 的覆盖率必须等于闭式公式（自己再算一遍，防止"两端一起错"）。
     * 拿**整个左上角区**逐像素对（比抽查一个点狠得多：弧上每一格都在判据里）。 */
    {
        const float cx = 60.0f, cy = 40.0f, hx = 40.0f, hy = 20.0f, r = 12.0f;
        int bad = 0, worst = 0, partial_ref = 0;
        for (int y = 19; y <= 33; ++y) {
            for (int x = 19; x <= 33; ++x) {
                const float px = (float)x + 0.5f - cx;
                const float py = (float)y + 0.5f - cy;
                const float qx = fabsf(px) - hx + r;
                const float qy = fabsf(py) - hy + r;
                const float mx = qx > 0 ? qx : 0.0f, my = qy > 0 ? qy : 0.0f;
                const float m = qx > qy ? qx : qy;
                const float d = sqrtf(mx * mx + my * my) + (m < 0 ? m : 0.0f) - r;
                float cov = 0.5f - d;
                cov = cov < 0 ? 0 : (cov > 1 ? 1 : cov);
                const int want = (int)(cov * 255.0f + 0.5f);
                if (want > 0 && want < 255) ++partial_ref;
                const int got = alpha_at((uint32_t)x, (uint32_t)y);
                const int dv = got > want ? got - want : want - got;
                if (dv > worst) worst = dv;
                if (dv > 1) ++bad;
            }
        }
        printf("  左上角区 %dx%d：与闭式公式最大差 %d，>1 的格 %d（含 %d 个半覆盖格）\n",
               15, 15, worst, bad, partial_ref);
        CHECK(bad == 0);
        CHECK(partial_ref >= 8);   /* 这一片里确实有过渡带，不是"全 0 或全 255"的空判据 */
    }

    /* ④ 四端一致 */
    {
        struct { const char* n; BgpuBackendKind k; } backends[] = {
            {"vulkan", BGPU_BACKEND_VULKAN},
            {"d3d11", BGPU_BACKEND_D3D11},
            {"gl", BGPU_BACKEND_GL},
        };
        for (int i = 0; i < 3; ++i) {
            uint8_t px[W * H * 4];
            int partial = 0;
            if (!render(backends[i].k, px, &partial)) {
                printf("  [%s] 没有 BGPU_CAP_SHAPE，跳过\n", backends[i].n);
                continue;
            }
            int n = 0, mx = 0;
            for (uint32_t k = 0; k < W * H * 4u; ++k) {
                const int dv = abs((int)px[k] - (int)cpu_px[k]);
                if (dv > 1) ++n;
                if (dv > mx) mx = dv;
            }
            printf("  [%s] 与 CPU 参考后端：差>1 的通道 %d 个，最大 %d（半覆盖像素 %d）\n",
                   backends[i].n, n, mx, partial);
            CHECK(mx <= 1);
        }
    }

    printf("[test_shape] pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
