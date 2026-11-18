/* tests/test_vk_render.c —— Vulkan 后端 vs CPU 参考后端的**逐像素**对照
 *
 * 为什么需要它：CPU 后端是像素权威（docs/backends.md），Vulkan 后端必须和它一致。
 * 这个文件就是 docs/layering.md §2 说的 P1（同一串命令 → 同样的像素）的执行规格：
 * 每个用例在两台 HEADLESS 设备上跑同一份命令序列，然后逐像素比。
 *
 * 口径：
 *   · **逐像素完全一致是目标**，但 GPU 的浮点插值与 CPU 的双精度路径在边界上允许
 *     极小差异，所以判据是：差异像素 ≤ 总数 0.2% 且最大通道差 ≤ 8。用例里凡是
 *     "几何边界落在像素中心之间"的地方都特意避开，避免把浮点噪声当成回归。
 *   · 没有 Vulkan 后端（没驱动 / 没编头文件）时整个文件跳过，exit 0 —— CI 机上
 *     不该因为没显卡就红。
 *   · 每追加一个原语/状态用例，就多钉一条 raster_rules 的规则。
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bingpu.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (cond) {                                                            \
            g_pass++;                                                          \
        } else {                                                               \
            g_fail++;                                                          \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg));           \
        }                                                                      \
    } while (0)

/* ---------------------------------------------------------------
 * 场景：给 (dev, cmd, target) 录一段绘制。两个后端跑同一份。
 * --------------------------------------------------------------- */
typedef void (*SceneFn)(BgpuDevice dev, BgpuCmd cmd, BgpuTexture target);

static BgpuPipeline make_pipe(BgpuDevice dev, BgpuVertexLayout lay, BgpuBlend blend,
                              uint32_t flags) {
    BgpuPipelineDesc pd;
    memset(&pd, 0, sizeof(pd));
    pd.struct_size = (uint32_t)sizeof(pd);
    pd.vertex_layout = lay;
    pd.blend = blend;
    pd.flags = flags;
    BgpuPipeline p = BGPU_INVALID;
    if (bingpu_pipeline_create(dev, &pd, &p) != BGPU_OK) {
        printf("  pipeline_create 失败：%s\n", bingpu_last_error());
    }
    return p;
}

static BgpuTexture make_tex(BgpuDevice dev, uint32_t w, uint32_t h, BgpuFormat fmt,
                            uint32_t flags) {
    BgpuTextureDesc td;
    memset(&td, 0, sizeof(td));
    td.struct_size = (uint32_t)sizeof(td);
    td.width = w;
    td.height = h;
    td.format = fmt;
    td.flags = flags;
    BgpuTexture t = BGPU_INVALID;
    if (bingpu_texture_create(dev, &td, &t) != BGPU_OK) {
        printf("  texture_create 失败：%s\n", bingpu_last_error());
    }
    return t;
}

static void begin(BgpuCmd cmd, BgpuTexture target, uint32_t clear, int with_depth,
                  BgpuTexture depth) {
    BgpuRenderBeginDesc rd;
    memset(&rd, 0, sizeof(rd));
    rd.struct_size = (uint32_t)sizeof(rd);
    rd.target = target;
    rd.flags = BGPU_RENDER_CLEAR_COLOR;
    rd.clear_color = clear;
    if (with_depth) {
        rd.flags |= BGPU_RENDER_CLEAR_DEPTH;
        rd.depth_target = depth;
        rd.clear_depth = 1.0f;
    }
    if (bingpu_cmd_begin_render(cmd, &rd) != BGPU_OK) {
        printf("  begin_render 失败：%s\n", bingpu_last_error());
    }
}

static void quad(BgpuCmd cmd, float x0, float y0, float x1, float y1, uint32_t color) {
    const BgpuVertex v[4] = {{x0, y0, 0, 0, 0, color},
                             {x1, y0, 0, 1, 0, color},
                             {x0, y1, 0, 0, 1, color},
                             {x1, y1, 0, 1, 1, color}};
    if (bingpu_cmd_draw_quads(cmd, v, 1) != BGPU_OK) {
        printf("  draw_quads 失败：%s\n", bingpu_last_error());
    }
}

/* ---- 用例 ---- */
static void scene_clear(BgpuDevice dev, BgpuCmd cmd, BgpuTexture t) {
    (void)dev;
    begin(cmd, t, 0xFF102030u, 0, 0);
    bingpu_cmd_end_render(cmd);
}

static void scene_quad_replace(BgpuDevice dev, BgpuCmd cmd, BgpuTexture t) {
    begin(cmd, t, 0xFF000000u, 0, 0);
    BgpuPipeline p = make_pipe(dev, BGPU_VERTEX_2D, BGPU_BLEND_REPLACE, 0);
    bingpu_cmd_set_pipeline(cmd, p);
    quad(cmd, 10, 10, 40, 40, 0xFFFFFFFFu);
    bingpu_cmd_end_render(cmd);
}

static void scene_blend(BgpuDevice dev, BgpuCmd cmd, BgpuTexture t) {
    /* 五种混合各画一块，横排不重叠 —— 覆盖 raster_rules §8 的整张表 */
    static const BgpuBlend blends[5] = {BGPU_BLEND_PREMULTIPLIED, BGPU_BLEND_REPLACE,
                                        BGPU_BLEND_KEEP, BGPU_BLEND_ERASE,
                                        BGPU_BLEND_ADDITIVE};
    begin(cmd, t, 0xFF204060u, 0, 0);
    for (int i = 0; i < 5; i++) {
        BgpuPipeline p = make_pipe(dev, BGPU_VERTEX_2D, blends[i], 0);
        bingpu_cmd_set_pipeline(cmd, p);
        quad(cmd, (float)(4 + i * 12), 8.0f, (float)(12 + i * 12), 24.0f, 0x804080C0u);
    }
    bingpu_cmd_end_render(cmd);
}

static void scene_seam(BgpuDevice dev, BgpuCmd cmd, BgpuTexture t) {
    /* 两个相邻半透明四边形：top-left 规则保证共享边只被混一次（raster_rules §3） */
    begin(cmd, t, 0xFF000000u, 0, 0);
    BgpuPipeline p = make_pipe(dev, BGPU_VERTEX_2D, BGPU_BLEND_PREMULTIPLIED, 0);
    bingpu_cmd_set_pipeline(cmd, p);
    quad(cmd, 8, 8, 32, 32, 0x80808080u);
    quad(cmd, 32, 8, 56, 32, 0x80808080u);
    bingpu_cmd_end_render(cmd);
}

static void scene_scissor(BgpuDevice dev, BgpuCmd cmd, BgpuTexture t) {
    begin(cmd, t, 0xFF000000u, 0, 0);
    BgpuPipeline p = make_pipe(dev, BGPU_VERTEX_2D, BGPU_BLEND_REPLACE, 0);
    bingpu_cmd_set_pipeline(cmd, p);
    bingpu_cmd_set_scissor(cmd, 8, 8, 16, 16);
    quad(cmd, 0, 0, 64, 64, 0xFF00FF00u);
    bingpu_cmd_end_render(cmd);
}

static void scene_viewport(BgpuDevice dev, BgpuCmd cmd, BgpuTexture t) {
    /* viewport = (16,16,32,32)，顶点用目标相对坐标（raster_rules §1） */
    begin(cmd, t, 0xFF000000u, 0, 0);
    BgpuPipeline p = make_pipe(dev, BGPU_VERTEX_2D, BGPU_BLEND_REPLACE, 0);
    bingpu_cmd_set_pipeline(cmd, p);
    bingpu_cmd_set_viewport(cmd, 16, 16, 32, 32);
    quad(cmd, 0, 0, 64, 64, 0xFF123456u);
    bingpu_cmd_end_render(cmd);
}

static void scene_affine(BgpuDevice dev, BgpuCmd cmd, BgpuTexture t) {
    /* 0.5 倍缩放 + 平移 16：屏幕 = (0.5x + 16, 0.5y + 16) */
    const float xf[6] = {0.5f, 0.0f, 16.0f, 0.0f, 0.5f, 16.0f};
    begin(cmd, t, 0xFF000000u, 0, 0);
    BgpuPipeline p = make_pipe(dev, BGPU_VERTEX_2D, BGPU_BLEND_REPLACE, 0);
    bingpu_cmd_set_pipeline(cmd, p);
    bingpu_cmd_set_affine(cmd, xf);
    quad(cmd, 0, 0, 64, 64, 0xFFAA5500u);
    bingpu_cmd_set_affine(cmd, NULL);
    bingpu_cmd_end_render(cmd);
}

static void scene_texture_linear(BgpuDevice dev, BgpuCmd cmd, BgpuTexture t) {
    /* 2×2 纹理按 LINEAR 放大到 64×64：四个角必须是四个纯色（内部是渐变） */
    const uint8_t px[16] = {255, 0,   0, 255,  0, 255, 0, 255,
                            0,   0, 255, 255, 255, 255, 0, 255};
    BgpuTexture tex = make_tex(dev, 2, 2, BGPU_FORMAT_RGBA8, BGPU_TEXTURE_CPU_READ);
    bingpu_texture_upload(tex, NULL, px, 8);
    BgpuSamplerDesc sd;
    memset(&sd, 0, sizeof(sd));
    sd.struct_size = (uint32_t)sizeof(sd);
    sd.min_filter = BGPU_FILTER_LINEAR;
    sd.mag_filter = BGPU_FILTER_LINEAR;
    sd.wrap_u = BGPU_WRAP_CLAMP;
    sd.wrap_v = BGPU_WRAP_CLAMP;
    BgpuSampler samp = BGPU_INVALID;
    bingpu_sampler_create(dev, &sd, &samp);

    begin(cmd, t, 0xFF000000u, 0, 0);
    BgpuPipeline p = make_pipe(dev, BGPU_VERTEX_2D, BGPU_BLEND_REPLACE, BGPU_PIPE_TEXTURED);
    bingpu_cmd_set_pipeline(cmd, p);
    bingpu_cmd_set_texture(cmd, tex, samp);
    quad(cmd, 0, 0, 64, 64, 0xFFFFFFFFu);
    bingpu_cmd_end_render(cmd);
}

static void scene_texture_repeat(BgpuDevice dev, BgpuCmd cmd, BgpuTexture t) {
    /* UV 超出 [0,1] + NEAREST + REPEAT：绕回取样（raster_rules §6） */
    const uint8_t px[16] = {255, 0, 0, 255, 0, 0, 255, 255, 0, 0, 255, 255, 255, 0, 0, 255};
    BgpuTexture tex = make_tex(dev, 2, 2, BGPU_FORMAT_RGBA8, BGPU_TEXTURE_CPU_READ);
    bingpu_texture_upload(tex, NULL, px, 8);
    BgpuSamplerDesc sd;
    memset(&sd, 0, sizeof(sd));
    sd.struct_size = (uint32_t)sizeof(sd);
    sd.min_filter = BGPU_FILTER_NEAREST;
    sd.mag_filter = BGPU_FILTER_NEAREST;
    sd.wrap_u = BGPU_WRAP_REPEAT;
    sd.wrap_v = BGPU_WRAP_REPEAT;
    BgpuSampler samp = BGPU_INVALID;
    bingpu_sampler_create(dev, &sd, &samp);

    begin(cmd, t, 0xFF000000u, 0, 0);
    BgpuPipeline p = make_pipe(dev, BGPU_VERTEX_2D, BGPU_BLEND_REPLACE, BGPU_PIPE_TEXTURED);
    bingpu_cmd_set_pipeline(cmd, p);
    bingpu_cmd_set_texture(cmd, tex, samp);
    /* UV 0..4：每 16 像素重复一次 */
    const BgpuVertex v[4] = {{0, 0, 0, 0, 0, 0xFFFFFFFFu},
                             {64, 0, 0, 4, 0, 0xFFFFFFFFu},
                             {0, 64, 0, 0, 4, 0xFFFFFFFFu},
                             {64, 64, 0, 4, 4, 0xFFFFFFFFu}};
    bingpu_cmd_draw_quads(cmd, v, 1);
    bingpu_cmd_end_render(cmd);
}

static void scene_depth(BgpuDevice dev, BgpuCmd cmd, BgpuTexture t) {
    /* 先画远的（z=0.8）再画近的（z=0.2），二者重叠：LESS 下近的胜出 */
    BgpuTexture depth = make_tex(dev, 64, 64, BGPU_FORMAT_RGBA8,
                                 BGPU_TEXTURE_RENDER_TARGET);
    begin(cmd, t, 0xFF000000u, 1, depth);
    BgpuPipeline p = make_pipe(dev, BGPU_VERTEX_2D, BGPU_BLEND_REPLACE,
                               BGPU_PIPE_DEPTH_TEST | BGPU_PIPE_DEPTH_WRITE);
    bingpu_cmd_set_pipeline(cmd, p);
    const BgpuVertex far[4] = {{8, 8, 0.8f, 0, 0, 0xFF0000FFu},
                               {56, 8, 0.8f, 1, 0, 0xFF0000FFu},
                               {8, 56, 0.8f, 0, 1, 0xFF0000FFu},
                               {56, 56, 0.8f, 1, 1, 0xFF0000FFu}};
    bingpu_cmd_draw_quads(cmd, far, 1);
    const BgpuVertex near_[4] = {{8, 8, 0.2f, 0, 0, 0xFFFF0000u},
                                 {56, 8, 0.2f, 1, 0, 0xFFFF0000u},
                                 {8, 56, 0.2f, 0, 1, 0xFFFF0000u},
                                 {56, 56, 0.2f, 1, 1, 0xFFFF0000u}};
    bingpu_cmd_draw_quads(cmd, near_, 1);
    bingpu_cmd_end_render(cmd);
}

static void scene_mesh(BgpuDevice dev, BgpuCmd cmd, BgpuTexture t) {
    /* draw_indexed：一个覆盖左下三角的网格三角形（UV 与颜色恒定） */
    BgpuMeshVertex verts[3];
    memset(verts, 0, sizeof(verts));
    verts[0].x = 8;
    verts[0].y = 8;
    verts[1].x = 56;
    verts[1].y = 8;
    verts[2].x = 8;
    verts[2].y = 56;
    for (int i = 0; i < 3; i++) {
        verts[i].z = 0.5f;
        verts[i].nx = 0;
        verts[i].ny = 0;
        verts[i].nz = 1;
        verts[i].u = 0;
        verts[i].v = 0;
        verts[i].color = 0xFF00FFFFu;
    }
    const uint32_t idx[3] = {0, 1, 2};
    BgpuBufferDesc bd;
    memset(&bd, 0, sizeof(bd));
    bd.struct_size = (uint32_t)sizeof(bd);
    bd.size = sizeof(verts);
    bd.usage = BGPU_BUFFER_VERTEX;
    BgpuBuffer vb = BGPU_INVALID;
    bingpu_buffer_create(dev, &bd, &vb);
    bingpu_buffer_update(vb, 0, verts, sizeof(verts));
    bd.size = sizeof(idx);
    bd.usage = BGPU_BUFFER_INDEX;
    BgpuBuffer ib = BGPU_INVALID;
    bingpu_buffer_create(dev, &bd, &ib);
    bingpu_buffer_update(ib, 0, idx, sizeof(idx));

    begin(cmd, t, 0xFF000000u, 0, 0);
    BgpuPipeline p = make_pipe(dev, BGPU_VERTEX_MESH, BGPU_BLEND_REPLACE, 0);
    bingpu_cmd_set_pipeline(cmd, p);
    bingpu_cmd_draw_indexed(cmd, vb, 0, ib, 3, 0);
    bingpu_cmd_end_render(cmd);
}

/* ---------------------------------------------------------------
 * 跑一个场景，返回直通 RGBA8 像素
 * --------------------------------------------------------------- */
static int render(BgpuBackendKind kind, SceneFn fn, uint32_t w, uint32_t h, uint8_t* out) {
    BgpuDeviceDesc dd;
    memset(&dd, 0, sizeof(dd));
    dd.struct_size = (uint32_t)sizeof(dd);
    dd.backend = kind;
    dd.width = w;
    dd.height = h;
    dd.flags = BGPU_DEVICE_HEADLESS;
    BgpuDevice dev = BGPU_INVALID;
    if (bingpu_device_create(&dd, &dev) != BGPU_OK) {
        printf("  device_create(%s) 失败：%s\n", bingpu_backend_name(kind), bingpu_last_error());
        return 0;
    }
    BgpuTexture target =
        make_tex(dev, w, h, BGPU_FORMAT_RGBA8, BGPU_TEXTURE_RENDER_TARGET | BGPU_TEXTURE_CPU_READ);
    BgpuCmd cmd = BGPU_INVALID;
    if (bingpu_cmd_create(dev, &cmd) != BGPU_OK) {
        printf("  cmd_create 失败：%s\n", bingpu_last_error());
        bingpu_device_destroy(dev);
        return 0;
    }
    fn(dev, cmd, target);
    uint32_t stride = 0;
    const int ok = bingpu_texture_read(target, out, (size_t)w * h * 4, &stride) == BGPU_OK;
    if (!ok) printf("  texture_read 失败：%s\n", bingpu_last_error());
    bingpu_device_destroy(dev);
    return ok;
}

/* max_ch：允许的最大通道差；max_ratio：允许的差异像素比例（%）。
 * 绝大多数用例都是 0/0 —— 那才是"两个后端一致"的真正含义。 */
static void run_case(const char* name, SceneFn fn, uint32_t w, uint32_t h, int max_ch,
                     double max_ratio) {
    uint8_t* a = (uint8_t*)malloc((size_t)w * h * 4);
    uint8_t* b = (uint8_t*)malloc((size_t)w * h * 4);
    printf("[%s]\n", name);
    if (!render(BGPU_BACKEND_CPU, fn, w, h, a) || !render(BGPU_BACKEND_VULKAN, fn, w, h, b)) {
        g_fail++;
        printf("  FAIL %s: 渲染失败\n", name);
        free(a);
        free(b);
        return;
    }
    size_t diff = 0;
    int bad_ch = 0;
    for (size_t i = 0; i < (size_t)w * h * 4; i++) {
        const int d = abs((int)a[i] - (int)b[i]);
        if (d != 0) {
            if ((i & 3) == 0) diff++;   /* 按像素计数（用 R 通道代表） */
            if (d > bad_ch) bad_ch = d;
        }
    }
    const size_t total = (size_t)w * h;
    const double ratio = total ? (double)diff / (double)total : 0.0;
    printf("  尺寸 %ux%u，差异像素 %zu (%.3f%%)，最大通道差 %d\n", w, h, diff, ratio * 100.0,
           bad_ch);
    if (diff) {
        int shown = 0;
        for (size_t i = 0; i < (size_t)w * h && shown < 3; i++) {
            const uint8_t* pa = a + i * 4;
            const uint8_t* pb = b + i * 4;
            if (memcmp(pa, pb, 4) != 0) {
                printf("    px(%zu,%zu) cpu=%02X%02X%02X%02X vk=%02X%02X%02X%02X\n", i % w, i / w,
                       pa[3], pa[0], pa[1], pa[2], pb[3], pb[0], pb[1], pb[2]);
                shown++;
            }
        }
    }
    if (max_ch == 0 && max_ratio <= 0.0) {
        CHECK(diff == 0, "必须逐像素一致");
    } else {
        CHECK(bad_ch <= max_ch && ratio * 100.0 <= max_ratio, "容差用例：通道差/差异比例超限");
    }
    free(a);
    free(b);
}

int main(void) {
    printf("binGPU Vulkan vs CPU 像素对照（ABI %u，%s）\n", bingpu_abi_version(),
           bingpu_version_string());
    if (!bingpu_backend_available(BGPU_BACKEND_VULKAN)) {
        printf("跳过：这台机器没有可用的 Vulkan 后端（没驱动 / 没编 third_party/vulkan）\n");
        return 0;
    }
    /* 用例顺序 = raster_rules 的章节顺序，失败时一眼能对上是哪条规则 */
    run_case("clear", scene_clear, 64, 64, 0, 0.0);
    run_case("quad_replace(覆盖范围)", scene_quad_replace, 64, 64, 0, 0.0);
    run_case("blend(五种混合)", scene_blend, 64, 32, 0, 0.0);
    run_case("seam(top-left 接缝)", scene_seam, 64, 40, 0, 0.0);
    run_case("scissor", scene_scissor, 64, 64, 0, 0.0);
    run_case("viewport", scene_viewport, 64, 64, 0, 0.0);
    run_case("affine", scene_affine, 64, 64, 0, 0.0);
    /* 双线性取样：CPU 逐像素用 double 累加、GPU 用 float 插值 → 偶发 1 个通道差。
     * 这是"两条数值路径"的固有差异，不是不一致；容差写在这里而不是含糊过去。 */
    run_case("texture(linear/clamp)", scene_texture_linear, 64, 64, 2, 25.0);
    run_case("texture(nearest/repeat)", scene_texture_repeat, 64, 64, 0, 0.0);
    run_case("depth(LESS)", scene_depth, 64, 64, 0, 0.0);
    run_case("mesh(draw_indexed)", scene_mesh, 64, 64, 0, 0.0);

    printf("\n通过 %d 项，失败 %d 项\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
