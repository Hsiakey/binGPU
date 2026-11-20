/* tests/gpu_parity.h —— GPU 后端 vs CPU 参考后端的**逐像素**对照（VK 与 D3D11 共用）
 *
 * 为什么抽成一份：CPU 后端是像素权威（docs/backends.md），每个真实后端都必须和它
 * 一致；"一致的判据"不该每加一个后端就抄一遍 —— 抄一遍就会漂一点。这个头文件就是
 * docs/layering.md §2 说的 P1（同一串命令 → 同样的像素）的**唯一**执行规格：
 * tests/test_vk_render.c 与 tests/test_d3d11_render.c 各只有一行 main。
 *
 * 口径：
 *   · **逐像素完全一致是目标**，但 GPU 的浮点插值与 CPU 的双精度路径在边界上允许
 *     极小差异，所以判据是：差异像素 ≤ 总数 0.2% 且最大通道差 ≤ 8。用例里凡是
 *     "几何边界落在像素中心之间"的地方都特意避开，避免把浮点噪声当成回归。
 *   · 没有这个后端（没驱动 / 没编头文件 / 机器不支持）时整份跳过，exit 0 ——
 *     CI 机上不该因为没显卡就红。
 *   · 每追加一个原语/状态用例，就多钉一条 raster_rules 的规则。
 */
#ifndef BINGPU_GPU_PARITY_H
#define BINGPU_GPU_PARITY_H

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bingpu.h"

static int g_pass = 0;
static int g_fail = 0;

/* 被测后端：入口 parity_run() 填一次，run_case() 用它跑 GPU 那一侧 */
static BgpuBackendKind g_kind = BGPU_BACKEND_NULL;

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

/* begin_render 但**不清**（load）：钉"同一个目标连开多趟时上一趟的内容还在"。
 * begin() 总是带 CLEAR，所以这条得单独写。 */
static void begin_load(BgpuCmd cmd, BgpuTexture target) {
    BgpuRenderBeginDesc rd;
    memset(&rd, 0, sizeof(rd));
    rd.struct_size = (uint32_t)sizeof(rd);
    rd.target = target;
    rd.flags = 0;
    if (bingpu_cmd_begin_render(cmd, &rd) != BGPU_OK) {
        printf("  begin_render(load) 失败：%s\n", bingpu_last_error());
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

static BgpuSampler make_sampler(BgpuDevice dev, BgpuFilter f, BgpuWrap wrap) {
    BgpuSamplerDesc sd;
    memset(&sd, 0, sizeof(sd));
    sd.struct_size = (uint32_t)sizeof(sd);
    sd.min_filter = f;
    sd.mag_filter = f;
    sd.wrap_u = wrap;
    sd.wrap_v = wrap;
    BgpuSampler samp = BGPU_INVALID;
    bingpu_sampler_create(dev, &sd, &samp);
    return samp;
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

/* 一帧里"渲染目标 ⇄ 采样源"来回切 + 把顶点流撑过扩容点。
 *
 * 钉的是 0.13.0 修掉的那个 VK 缺陷：命令缓冲**先录后执行**，顶点流扩容时
 * 前面那些 draw 已经绑了旧 buffer，旧内容必须抄进新 buffer、旧 buffer 也不能
 * 当场销毁。否则一帧几千个四边形的页面会从扩容那一刻开始整块消失 ——
 * 既不出错也不崩溃。bin2d/binUI 的整页渲染必然踩到，而原有的 12 个用例
 * （每笔只有一两个四边形）一个都抓不到。
 *
 * 场景：目标先清底色 + 一个红块（扩容**之前**画），再画 kQuads 个绿点把顶点流
 * 撑过 64KB，然后把目标当采样源抓一张离屏图、再贴回目标左上 32×32。 */
static void scene_multipass(BgpuDevice dev, BgpuCmd cmd, BgpuTexture t) {
    enum { kQuads = 1200 };
    static BgpuVertex many[kQuads * 4];
    const BgpuPipeline solid = make_pipe(dev, BGPU_VERTEX_2D, BGPU_BLEND_REPLACE, 0);
    const BgpuPipeline textured =
        make_pipe(dev, BGPU_VERTEX_2D, BGPU_BLEND_PREMULTIPLIED, BGPU_PIPE_TEXTURED);
    const BgpuSampler samp = make_sampler(dev, BGPU_FILTER_NEAREST, BGPU_WRAP_CLAMP);
    int i;

    begin(cmd, t, 0xFF202020u, 0, 0);
    bingpu_cmd_set_pipeline(cmd, solid);
    bingpu_cmd_set_scissor(cmd, 0, 0, 16, 16);
    quad(cmd, 0, 0, 16, 16, 0xFFFF0000u);   /* 扩容前的一笔：丢了就露馅 */
    bingpu_cmd_set_scissor(cmd, 0, 0, -1, -1);
    for (i = 0; i < kQuads; i++) {
        const float x = (float)(16 + (i % 32));
        const float y = (float)(16 + ((i / 32) % 16));
        BgpuVertex* v = many + i * 4;
        memset(v, 0, sizeof(BgpuVertex) * 4);
        v[0].x = x;      v[0].y = y;      v[0].color = 0xFF00C000u;
        v[1].x = x + 1;  v[1].y = y;      v[1].color = 0xFF00C000u;
        v[2].x = x;      v[2].y = y + 1;  v[2].color = 0xFF00C000u;
        v[3].x = x + 1;  v[3].y = y + 1;  v[3].color = 0xFF00C000u;
    }
    bingpu_cmd_draw_quads(cmd, many, kQuads);
    bingpu_cmd_end_render(cmd);

    /* 目标刚渲染完就是采样源（布局 颜色附件 → 只读 来回横跳） */
    BgpuTexture a = make_tex(dev, 64, 64, BGPU_FORMAT_RGBA8, BGPU_TEXTURE_RENDER_TARGET);
    begin(cmd, a, 0x00000000u, 0, 0);
    bingpu_cmd_set_pipeline(cmd, textured);
    bingpu_cmd_set_texture(cmd, t, samp);
    quad(cmd, 0, 0, 64, 64, 0xFFFFFFFFu);
    bingpu_cmd_end_render(cmd);

    /* 同一帧里再回到目标（load，不清）：左上 32×32 盖回来 */
    begin_load(cmd, t);
    bingpu_cmd_set_pipeline(cmd, textured);
    bingpu_cmd_set_texture(cmd, a, samp);
    bingpu_cmd_set_scissor(cmd, 0, 0, 32, 32);
    quad(cmd, 0, 0, 32, 32, 0xFFFFFFFFu);
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
    BgpuSampler samp = make_sampler(dev, BGPU_FILTER_LINEAR, BGPU_WRAP_CLAMP);

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
    BgpuSampler samp = make_sampler(dev, BGPU_FILTER_NEAREST, BGPU_WRAP_REPEAT);

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

static void scene_texture_box(BgpuDevice dev, BgpuCmd cmd, BgpuTexture t) {
    /* 局部上传（box）：先铺一张不透明的 4×4，再用半透明色更新 (1,1)-(2,2)。
     * **局部上传同样要预乘**（CPU 就是按区域预乘的）—— 这条用例钉住
     * "只在整张上传时预乘"这个偏差（VK 后端 0.3.0 的写法）。 */
    const uint8_t base[4 * 4 * 4] = {
        0x40, 0x80, 0xC0, 0xFF, 0x40, 0x80, 0xC0, 0xFF, 0x40, 0x80, 0xC0, 0xFF, 0x40, 0x80, 0xC0,
        0xFF, 0x40, 0x80, 0xC0, 0xFF, 0x40, 0x80, 0xC0, 0xFF, 0x40, 0x80, 0xC0, 0xFF, 0x40, 0x80,
        0xC0, 0xFF, 0x40, 0x80, 0xC0, 0xFF, 0x40, 0x80, 0xC0, 0xFF, 0x40, 0x80, 0xC0, 0xFF, 0x40,
        0x80, 0xC0, 0xFF, 0x40, 0x80, 0xC0, 0xFF, 0x40, 0x80, 0xC0, 0xFF, 0x40, 0x80, 0xC0, 0xFF,
        0x40, 0x80, 0xC0, 0xFF};
    const uint8_t patch[2 * 2 * 4] = {200, 100, 50, 128, 200, 100, 50, 128,
                                      200, 100, 50, 128, 200, 100, 50, 128};
    BgpuTexture tex = make_tex(dev, 4, 4, BGPU_FORMAT_RGBA8, BGPU_TEXTURE_CPU_READ);
    bingpu_texture_upload(tex, NULL, base, 16);
    BgpuBox box = {1, 1, 2, 2};
    if (bingpu_texture_upload(tex, &box, patch, 8) != BGPU_OK) {
        printf("  局部上传失败：%s\n", bingpu_last_error());
    }
    BgpuSampler samp = make_sampler(dev, BGPU_FILTER_NEAREST, BGPU_WRAP_CLAMP);

    begin(cmd, t, 0xFF000000u, 0, 0);
    BgpuPipeline p = make_pipe(dev, BGPU_VERTEX_2D, BGPU_BLEND_REPLACE, BGPU_PIPE_TEXTURED);
    bingpu_cmd_set_pipeline(cmd, p);
    bingpu_cmd_set_texture(cmd, tex, samp);
    quad(cmd, 0, 0, 64, 64, 0xFFFFFFFFu);
    bingpu_cmd_end_render(cmd);
}

static void scene_depth(BgpuDevice dev, BgpuCmd cmd, BgpuTexture t) {
    /* 先画远的（z=0.8）再画近的（z=0.2），二者重叠：LESS 下近的胜出 */
    BgpuTexture depth =
        make_tex(dev, 64, 64, BGPU_FORMAT_RGBA8, BGPU_TEXTURE_RENDER_TARGET);
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
    if (!render(BGPU_BACKEND_CPU, fn, w, h, a) || !render(g_kind, fn, w, h, b)) {
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
                printf("    px(%zu,%zu) cpu=%02X%02X%02X%02X gpu=%02X%02X%02X%02X\n", i % w, i / w,
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

/* ---------------------------------------------------------------
 * 交换链路径（frame_read）—— 与 texture_read 是**两条**回读代码路径
 *
 * 为什么必须单独有这一组：`texture_read` 读的是"离屏目标纹理"，`frame_read` 读的是
 * "交换链借出的帧目标"，两边在后端里完全是两段实现（D3D11：staging copy；GL：FBO
 * readback / 默认帧缓冲翻行）。0.5.0 落地时两个后端在这条路上各有一个 bug：
 *   · D3D11 的借出对象漏了 res_w/res_h → viewport 0×0，一笔都画不出来；
 *   · GL 的 FBO 回读多翻了一次行 → 整幅上下镜像。
 * 12 个 texture_read 用例**一个都没抓到**，是 bin2d 的 GPU 桥先撞出来的。所以把它
 * 补成规格的一部分：同样的画法，走 frame_* 一遍，也必须逐像素一致。
 * --------------------------------------------------------------- */
static int render_via_swapchain(BgpuBackendKind kind, uint32_t w, uint32_t h, uint8_t* out) {
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
    BgpuSwapchainDesc sd;
    memset(&sd, 0, sizeof(sd));
    sd.struct_size = (uint32_t)sizeof(sd);
    sd.width = w;
    sd.height = h;
    sd.format = BGPU_FORMAT_RGBA8;
    BgpuSwapchain sc = BGPU_INVALID;
    if (bingpu_swapchain_create(dev, &sd, &sc) != BGPU_OK) {
        printf("  swapchain_create 失败：%s\n", bingpu_last_error());
        bingpu_device_destroy(dev);
        return 0;
    }
    BgpuCmd cmd = BGPU_INVALID;
    bingpu_cmd_create(dev, &cmd);

    /* 一张 w×h 的"坐标色"纹理：两个轴上都强烈变化，上下镜像 / 不画 都会立刻露馅 */
    uint8_t* px = (uint8_t*)malloc((size_t)w * h * 4);
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint8_t* p = px + ((size_t)y * w + x) * 4;
            p[0] = (uint8_t)(x * 255u / (w > 1 ? w - 1 : 1));
            p[1] = (uint8_t)(y * 255u / (h > 1 ? h - 1 : 1));
            p[2] = (uint8_t)(((x ^ y) & 1) ? 200 : 40);
            p[3] = 255;
        }
    }
    BgpuTexture tex = make_tex(dev, w, h, BGPU_FORMAT_RGBA8, BGPU_TEXTURE_CPU_READ);
    bingpu_texture_upload(tex, NULL, px, w * 4);
    free(px);
    BgpuSampler samp = make_sampler(dev, BGPU_FILTER_NEAREST, BGPU_WRAP_CLAMP);
    BgpuPipeline pipe = make_pipe(dev, BGPU_VERTEX_2D, BGPU_BLEND_REPLACE, BGPU_PIPE_TEXTURED);

    BgpuTexture target = BGPU_INVALID;
    int ok = bingpu_frame_begin(sc, cmd, &target) == BGPU_OK;
    if (ok) {
        BgpuRenderBeginDesc rd;
        memset(&rd, 0, sizeof(rd));
        rd.struct_size = (uint32_t)sizeof(rd);
        rd.target = target;
        rd.flags = BGPU_RENDER_CLEAR_COLOR;
        rd.clear_color = 0x00000000u;   /* 全透明：没画上的地方一眼看出来 */
        ok = bingpu_cmd_begin_render(cmd, &rd) == BGPU_OK;
        if (ok) {
            bingpu_cmd_set_viewport(cmd, 0, 0, (float)w, (float)h);
            bingpu_cmd_set_scissor(cmd, 0, 0, (int32_t)w, (int32_t)h);
            bingpu_cmd_set_pipeline(cmd, pipe);
            bingpu_cmd_set_texture(cmd, tex, samp);
            const BgpuVertex v[4] = {{0, 0, 0, 0, 0, 0xFFFFFFFFu},
                                     {(float)w, 0, 0, 1, 0, 0xFFFFFFFFu},
                                     {0, (float)h, 0, 0, 1, 0xFFFFFFFFu},
                                     {(float)w, (float)h, 0, 1, 1, 0xFFFFFFFFu}};
            ok = bingpu_cmd_draw_quads(cmd, v, 1) == BGPU_OK &&
                 bingpu_cmd_end_render(cmd) == BGPU_OK && bingpu_frame_end(sc, cmd) == BGPU_OK;
        }
        if (ok) {
            uint32_t stride = 0;
            ok = bingpu_frame_read(sc, out, (size_t)w * h * 4, &stride) == BGPU_OK;
        }
        if (!ok) printf("  交换链路径失败：%s\n", bingpu_last_error());
    }
    bingpu_cmd_destroy(dev, cmd);
    bingpu_swapchain_destroy(dev, sc);
    bingpu_device_destroy(dev);
    return ok;
}

static void run_swapchain_case(const char* name, uint32_t w, uint32_t h, int max_ch,
                               double max_ratio) {
    uint8_t* a = (uint8_t*)malloc((size_t)w * h * 4);
    uint8_t* b = (uint8_t*)malloc((size_t)w * h * 4);
    printf("[%s]\n", name);
    if (!render_via_swapchain(BGPU_BACKEND_CPU, w, h, a) ||
        !render_via_swapchain(g_kind, w, h, b)) {
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
            if ((i & 3) == 0) diff++;
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
                printf("    px(%zu,%zu) cpu=%02X%02X%02X%02X gpu=%02X%02X%02X%02X\n", i % w, i / w,
                       pa[3], pa[0], pa[1], pa[2], pb[3], pb[0], pb[1], pb[2]);
                shown++;
            }
        }
    }
    if (max_ch == 0 && max_ratio <= 0.0) {
        CHECK(diff == 0, "交换链路径也必须逐像素一致");
    } else {
        CHECK(bad_ch <= max_ch && ratio * 100.0 <= max_ratio, "交换链路径：容差超限");
    }
    free(a);
    free(b);
}

/* ---------------------------------------------------------------
 * 入口：各 test_*.c 调一次
 * --------------------------------------------------------------- */
/* ============================================================
 * ABI 3：固定功能 Lambert + 透视校正
 *
 * 这两条是 bin3d 的 GPU 阶段能不能用的前提，判据是**手算的数值**（与
 * test_cpu_render.c 的 [lighting] / [perspective] 同源），不是"看着不黑"。
 *
 * 为什么放在共享 harness 里而不是各后端的 test 里：三个后端算的是同一个公式，
 * 一份场景 + 0 容差就是最硬的验收；分开写三份只会慢慢漂移（0.25.0 的 VK
 * strokeRounded 就是这么漏的）。
 * ============================================================ */
static void scene_lighting(BgpuDevice dev, BgpuCmd cmd, BgpuTexture t) {
    static const float kNormals[3][3] = {
        {0.0f, 1.0f, 0.0f},                      /* 朝光源：ndl=1 → 1.1 → clamp 1.0 → 255 */
        {0.0f, -1.0f, 0.0f},                     /* 背光：ndl=0 → ambient 0.1 → 26         */
        {-0.70710678f, 0.70710678f, 0.0f},       /* 斜 45°：ndl=0.7071 → 0.8071 → 206      */
    };
    BgpuMeshVertex verts[12];
    memset(verts, 0, sizeof(verts));
    uint32_t idx[18];
    for (int q = 0; q < 3; q++) {
        const float x0 = 8.0f * (float)q;
        const float xs[4] = {x0, x0 + 8.0f, x0, x0 + 8.0f};   /* TL,TR,BL,BR */
        const float ys[4] = {0.0f, 0.0f, 8.0f, 8.0f};
        for (int k = 0; k < 4; k++) {
            BgpuMeshVertex* v = &verts[q * 4 + k];
            v->x = xs[k];
            v->y = ys[k];
            v->z = 0.5f;
            v->u = 0.0f;
            v->v = 0.0f;
            v->color = 0xFFFFFFFFu;              /* 白：结果 = 光照系数 */
            v->nx = kNormals[q][0];
            v->ny = kNormals[q][1];
            v->nz = kNormals[q][2];
            v->w = 1.0f;                         /* 正交：iw=1，走普通插值 */
        }
        const uint32_t base = (uint32_t)(q * 4);
        idx[q * 6 + 0] = base + 0;
        idx[q * 6 + 1] = base + 1;
        idx[q * 6 + 2] = base + 2;
        idx[q * 6 + 3] = base + 2;
        idx[q * 6 + 4] = base + 1;
        idx[q * 6 + 5] = base + 3;
    }

    BgpuBufferDesc bd;
    memset(&bd, 0, sizeof(bd));
    bd.struct_size = (uint32_t)sizeof(bd);
    bd.size = (uint32_t)sizeof(verts);
    bd.usage = BGPU_BUFFER_VERTEX;
    BgpuBuffer vb = BGPU_INVALID;
    if (bingpu_buffer_create(dev, &bd, &vb) != BGPU_OK) return;
    if (bingpu_buffer_update(vb, 0, verts, (uint32_t)sizeof(verts)) != BGPU_OK) return;
    bd.size = (uint32_t)sizeof(idx);
    bd.usage = BGPU_BUFFER_INDEX;
    BgpuBuffer ib = BGPU_INVALID;
    if (bingpu_buffer_create(dev, &bd, &ib) != BGPU_OK) return;
    if (bingpu_buffer_update(ib, 0, idx, (uint32_t)sizeof(idx)) != BGPU_OK) return;

    const BgpuPipeline p =
        make_pipe(dev, BGPU_VERTEX_MESH, BGPU_BLEND_PREMULTIPLIED, BGPU_PIPE_LIGHTING);
    BgpuLight light;
    memset(&light, 0, sizeof(light));
    light.dir[1] = 1.0f;
    light.color[0] = light.color[1] = light.color[2] = 1.0f;
    /* ambient 故意取 0.1：0.1·255 = 25.5 是 8 位量化的**平局点** —— CPU 的 q8 向上
     * 收敛给 26，而 GPU 的 float→UNORM8 向下收敛给 25。三个后端都在光照分支里先按
     * CPU 的口径量化了一次（见各自的 shader 注释），所以这里能钉 0 容差。
     * 换成 0.12 就绕开了这个点，这条断言也就测不到量化口径了。 */
    light.ambient[0] = light.ambient[1] = light.ambient[2] = 0.1f;

    begin(cmd, t, 0xFF000000u, 0, 0);
    bingpu_cmd_set_pipeline(cmd, p);
    if (bingpu_cmd_set_light(cmd, &light) != BGPU_OK)
        printf("  set_light 失败：%s\n", bingpu_last_error());
    if (bingpu_cmd_draw_indexed(cmd, vb, 0, ib, 18, 0) != BGPU_OK)
        printf("  draw_indexed 失败：%s\n", bingpu_last_error());
    bingpu_cmd_end_render(cmd);

    bingpu_pipeline_destroy(dev, p);
    bingpu_buffer_destroy(dev, vb);
    bingpu_buffer_destroy(dev, ib);
}

/* 透视校正：一条水平四边形从 x=0 到 x=8，左端 w=1（近）、右端 w=4（远），UV 0→1。
 * 纹理是 2×1 的 {黑, 白}，NEAREST 采样。
 *
 *   屏幕空间线性：x=4 处 u = 0.5625 → 白
 *   透视校正：    x=4 处 u = 0.2432 → 黑
 *
 * 所以"x=4 那个像素是黑还是白"就是判据。 */
static void scene_perspective(BgpuDevice dev, BgpuCmd cmd, BgpuTexture t) {
    BgpuMeshVertex verts[4];
    memset(verts, 0, sizeof(verts));
    const float xs[4] = {0.0f, 8.0f, 0.0f, 8.0f};   /* TL,TR,BL,BR */
    const float ys[4] = {0.0f, 0.0f, 8.0f, 8.0f};
    const float us[4] = {0.0f, 1.0f, 0.0f, 1.0f};
    for (int k = 0; k < 4; k++) {
        BgpuMeshVertex* v = &verts[k];
        v->x = xs[k];
        v->y = ys[k];
        v->z = 0.5f;
        v->u = us[k];
        v->v = 0.0f;
        v->color = 0xFFFFFFFFu;
        v->nx = 0.0f;
        v->ny = 0.0f;
        v->nz = 1.0f;
        v->w = (k == 0 || k == 2) ? 1.0f : 4.0f;     /* TL/BL 近，TR/BR 远 */
    }
    const uint32_t idx[6] = {0, 1, 2, 2, 1, 3};

    BgpuBufferDesc bd;
    memset(&bd, 0, sizeof(bd));
    bd.struct_size = (uint32_t)sizeof(bd);
    bd.size = (uint32_t)sizeof(verts);
    bd.usage = BGPU_BUFFER_VERTEX;
    BgpuBuffer vb = BGPU_INVALID;
    if (bingpu_buffer_create(dev, &bd, &vb) != BGPU_OK) return;
    if (bingpu_buffer_update(vb, 0, verts, (uint32_t)sizeof(verts)) != BGPU_OK) return;
    bd.size = (uint32_t)sizeof(idx);
    bd.usage = BGPU_BUFFER_INDEX;
    BgpuBuffer ib = BGPU_INVALID;
    if (bingpu_buffer_create(dev, &bd, &ib) != BGPU_OK) return;
    if (bingpu_buffer_update(ib, 0, idx, (uint32_t)sizeof(idx)) != BGPU_OK) return;

    /* 2×1 纹理：左黑右白 */
    const BgpuTexture tex = make_tex(dev, 2, 1, BGPU_FORMAT_RGBA8, 0);
    const uint8_t px[8] = {0, 0, 0, 255, 255, 255, 255, 255};
    if (bingpu_texture_upload(tex, NULL, px, 8) != BGPU_OK) return;
    const BgpuSampler samp = make_sampler(dev, BGPU_FILTER_NEAREST, BGPU_WRAP_CLAMP);

    const BgpuPipeline p =
        make_pipe(dev, BGPU_VERTEX_MESH, BGPU_BLEND_PREMULTIPLIED, BGPU_PIPE_TEXTURED);

    begin(cmd, t, 0xFF000000u, 0, 0);
    bingpu_cmd_set_pipeline(cmd, p);
    bingpu_cmd_set_texture(cmd, tex, samp);
    if (bingpu_cmd_draw_indexed(cmd, vb, 0, ib, 6, 0) != BGPU_OK)
        printf("  draw_indexed 失败：%s\n", bingpu_last_error());
    bingpu_cmd_end_render(cmd);

    bingpu_pipeline_destroy(dev, p);
    bingpu_sampler_destroy(dev, samp);
    bingpu_texture_destroy(dev, tex);
    bingpu_buffer_destroy(dev, vb);
    bingpu_buffer_destroy(dev, ib);
}

/* 后端自报的能力位：给 ABI 3 的 두 用例决定"跑"还是"skip"。
 * 单独建设备读一次 caps —— run_case 内部每次自己建/销设备，没有可复用的句柄。 */
static int backend_has_cap(BgpuBackendKind kind, uint32_t cap) {
    BgpuDeviceDesc dd;
    memset(&dd, 0, sizeof(dd));
    dd.struct_size = (uint32_t)sizeof(dd);
    dd.backend = kind;
    dd.width = 8;
    dd.height = 8;
    dd.flags = BGPU_DEVICE_HEADLESS | BGPU_DEVICE_VALIDATE;
    BgpuDevice dev = BGPU_INVALID;
    if (bingpu_device_create(&dd, &dev) != BGPU_OK) return 0;
    BgpuCaps caps;
    memset(&caps, 0, sizeof(caps));
    caps.struct_size = (uint32_t)sizeof(caps);
    const int has = (bingpu_device_caps(dev, &caps) == BGPU_OK) && ((caps.flags & cap) != 0);
    bingpu_device_destroy(dev);
    return has;
}

/* ---------------------------------------------------------------
 * 下一版：质量档（MSAA / mip）—— **只在能力可达时运行**
 *
 * 这两条不是 parity：高档设置是显式选择的质量档，合同是"同档与 CPU 相同"，
 * 而不是"高档也必须和 CPU 逐像素相同"（CPU 参考后端恒为 1×/1 级）。
 * 所以判据是**可观测的画质指标**：
 *   · MSAA：斜边的中间色像素个数（0<v<255）必须明显多于 1×；
 *   · mip ：缩小的棋盘格在 1× 下有摩尔纹（偏离均值的幅度大），mip 后接近纯灰。
 * 拿不到能力（没有 CAP 位 / caps.msaa_samples < 4）就 skip 并打印原因，不 fail。
 * --------------------------------------------------------------- */
static uint32_t caps_msaa_max(BgpuBackendKind kind, int* has_cap) {
    BgpuDeviceDesc dd;
    memset(&dd, 0, sizeof(dd));
    dd.struct_size = (uint32_t)sizeof(dd);
    dd.backend = kind;
    dd.width = 8;
    dd.height = 8;
    dd.flags = BGPU_DEVICE_HEADLESS;
    BgpuDevice dev = BGPU_INVALID;
    if (bingpu_device_create(&dd, &dev) != BGPU_OK) return 1;
    BgpuCaps caps;
    memset(&caps, 0, sizeof(caps));
    caps.struct_size = (uint32_t)sizeof(caps);
    const int ok = bingpu_device_caps(dev, &caps) == BGPU_OK;
    bingpu_device_destroy(dev);
    if (has_cap) *has_cap = ok && (caps.flags & BGPU_CAP_MSAA) != 0;
    return ok ? caps.msaa_samples : 1;
}

/* 建带 msaa 档的 headless 设备，录一段场景到离屏目标，再 texture_read 出来。 */
static int render_msaa(BgpuBackendKind kind, uint32_t msaa, SceneFn fn, uint32_t w, uint32_t h,
                       uint8_t* out) {
    BgpuDeviceDesc dd;
    memset(&dd, 0, sizeof(dd));
    dd.struct_size = (uint32_t)sizeof(dd);
    dd.backend = kind;
    dd.width = w;
    dd.height = h;
    dd.flags = BGPU_DEVICE_HEADLESS;
    dd.msaa_samples = msaa;
    BgpuDevice dev = BGPU_INVALID;
    if (bingpu_device_create(&dd, &dev) != BGPU_OK) {
        printf("  device_create(msaa=%u) 失败：%s\n", msaa, bingpu_last_error());
        return 0;
    }
    BgpuTexture target =
        make_tex(dev, w, h, BGPU_FORMAT_RGBA8, BGPU_TEXTURE_RENDER_TARGET | BGPU_TEXTURE_CPU_READ);
    BgpuCmd cmd = BGPU_INVALID;
    if (bingpu_cmd_create(dev, &cmd) != BGPU_OK) {
        bingpu_device_destroy(dev);
        return 0;
    }
    fn(dev, cmd, target);
    uint32_t stride = 0;
    const int ok = bingpu_texture_read(target, out, (size_t)w * h * 4, &stride) == BGPU_OK;
    if (!ok) printf("  texture_read(msaa) 失败：%s\n", bingpu_last_error());
    bingpu_device_destroy(dev);
    return ok;
}

/* 交换链路径也要能开 MSAA：frame_read 必须拿到 resolve 之后的单采样结果。 */
static int render_swapchain_msaa(BgpuBackendKind kind, uint32_t msaa, SceneFn fn, uint32_t w,
                                 uint32_t h, uint8_t* out) {
    BgpuDeviceDesc dd;
    memset(&dd, 0, sizeof(dd));
    dd.struct_size = (uint32_t)sizeof(dd);
    dd.backend = kind;
    dd.width = w;
    dd.height = h;
    dd.flags = BGPU_DEVICE_HEADLESS;
    dd.msaa_samples = msaa;
    BgpuDevice dev = BGPU_INVALID;
    if (bingpu_device_create(&dd, &dev) != BGPU_OK) return 0;
    BgpuSwapchainDesc sd;
    memset(&sd, 0, sizeof(sd));
    sd.struct_size = (uint32_t)sizeof(sd);
    sd.width = w;
    sd.height = h;
    sd.format = BGPU_FORMAT_RGBA8;
    BgpuSwapchain sc = BGPU_INVALID;
    if (bingpu_swapchain_create(dev, &sd, &sc) != BGPU_OK) {
        bingpu_device_destroy(dev);
        return 0;
    }
    BgpuCmd cmd = BGPU_INVALID;
    bingpu_cmd_create(dev, &cmd);
    BgpuTexture target = BGPU_INVALID;
    int ok = bingpu_frame_begin(sc, cmd, &target) == BGPU_OK;
    if (ok) {
        fn(dev, cmd, target);
        ok = bingpu_frame_end(sc, cmd) == BGPU_OK;
    }
    uint32_t stride = 0;
    if (ok) ok = bingpu_frame_read(sc, out, (size_t)w * h * 4, &stride) == BGPU_OK;
    if (!ok) printf("  交换链 MSAA 路径失败：%s\n", bingpu_last_error());
    bingpu_cmd_destroy(dev, cmd);
    bingpu_swapchain_destroy(dev, sc);
    bingpu_device_destroy(dev);
    return ok;
}

/* 一条斜边：REPLACE 白四边形盖在黑底上。1× 的边缘只有 0/255 两档，
 * N× 的 resolve 会把边缘像素平均出中间色 —— 数 0<v<255 的个数即可。 */
static void scene_diagonal(BgpuDevice dev, BgpuCmd cmd, BgpuTexture t) {
    begin(cmd, t, 0xFF000000u, 0, 0);
    const BgpuPipeline p = make_pipe(dev, BGPU_VERTEX_2D, BGPU_BLEND_REPLACE, 0);
    bingpu_cmd_set_pipeline(cmd, p);
    const BgpuVertex v[4] = {{6.0f, 4.0f, 0, 0, 0, 0xFFFFFFFFu},
                             {58.0f, 16.0f, 0, 1, 0, 0xFFFFFFFFu},
                             {6.0f, 52.0f, 0, 0, 1, 0xFFFFFFFFu},
                             {58.0f, 64.0f, 0, 1, 1, 0xFFFFFFFFu}};
    bingpu_cmd_draw_quads(cmd, v, 1);
    bingpu_cmd_end_render(cmd);
}

static size_t count_midtones(const uint8_t* px, uint32_t w, uint32_t h) {
    size_t n = 0;
    for (size_t i = 0; i < (size_t)w * h; i++) {
        const uint8_t v = px[i * 4];
        if (v > 0 && v < 255) n++;
    }
    return n;
}

/* 两趟 pass：第二趟 begin_render 不带 CLEAR（LOAD）。VK 的 MSAA 附件是独立的，
 * 内容在上一趟已经 resolve 到单采样镜像 —— 第二趟必须先把它搬回多采样附件，
 * 否则"上一趟画的东西"会凭空消失（GL 的 msaa FBO 天然保留，这条仍要钉）。 */
static void scene_two_pass(BgpuDevice dev, BgpuCmd cmd, BgpuTexture t) {
    const BgpuPipeline p = make_pipe(dev, BGPU_VERTEX_2D, BGPU_BLEND_REPLACE, 0);
    begin(cmd, t, 0xFF000000u, 0, 0);
    bingpu_cmd_set_pipeline(cmd, p);
    quad(cmd, 8, 8, 24, 24, 0xFFFFFFFFu);
    bingpu_cmd_end_render(cmd);

    begin_load(cmd, t);
    bingpu_cmd_set_pipeline(cmd, p);
    quad(cmd, 40, 40, 56, 56, 0xFFFFFFFFu);
    bingpu_cmd_end_render(cmd);
}

static int region_is_white(const uint8_t* px, uint32_t w, uint32_t x0, uint32_t y0, uint32_t x1,
                           uint32_t y1) {
    for (uint32_t y = y0; y < y1; y++) {
        for (uint32_t x = x0; x < x1; x++) {
            if (px[((size_t)y * w + x) * 4] < 250) return 0;
        }
    }
    return 1;
}

/* ---------------------------------------------------------------
 * MSAA × 多目标：「帧中把刚渲染过的目标当采样源」
 *
 * 这是 binUI `svg-filter` 页（图层 + 毛玻璃 + 合成）的形状，也是 0.14.0 漏掉的那条：
 * 0.14.0 的 MSAA 用例只有"单目标、画完就 texture_read"，一帧里多目标来回切的
 * 命令流一个都没跑过。
 *
 * 判据：同一串命令流在 msaa=1 与 msaa=4 下必须**逐像素相同**（用例里所有四边形都
 * 落在整数像素边界上，全采样覆盖 ⇒ resolve 就是恒等；任何差异都是缺陷）。
 * --------------------------------------------------------------- */

/* 两个缓冲逐像素比。返回差异像素数，*maxdiff 给最大通道差。 */
static size_t pixel_diff(const uint8_t* a, const uint8_t* b, size_t px, int* maxdiff) {
    size_t n = 0;
    int m = 0;
    for (size_t i = 0; i < px; i++) {
        int d = 0;
        for (int k = 0; k < 4; k++) {
            const int v = (int)a[i * 4 + k] - (int)b[i * 4 + k];
            const int av = v < 0 ? -v : v;
            if (av > d) d = av;
        }
        if (d > 0) n++;
        if (d > m) m = d;
    }
    if (maxdiff) *maxdiff = m;
    return n;
}

static int pixel_is(const uint8_t* px, uint32_t w, uint32_t x, uint32_t y, uint8_t r, uint8_t g,
                    uint8_t b, int tol) {
    const uint8_t* p = px + ((size_t)y * w + x) * 4;
    return (int)p[0] - (int)r <= tol && (int)r - (int)p[0] <= tol &&
           (int)p[1] - (int)g <= tol && (int)g - (int)p[1] <= tol &&
           (int)p[2] - (int)b <= tol && (int)b - (int)p[2] <= tol;
}

/* 场景一（照 binUI `blurLayerContent` 的顺序改写，纯 binGPU）：
 *   ① 画布（= binUI 的 canvas）：清底色 + 两块实心色（"内容"）；
 *   ② A（128×128 离屏）：清空，**把画布当纹理**抓进右上象限（"抓当前画布当背景"）；
 *   ③ A：LOAD 再画一块蓝（"图层里再画一笔"，第二趟不带 CLEAR）；
 *   ④ 画布：LOAD，**把 A 当纹理**贴回右上象限（"把模糊结果合成回整页"）；
 * 抓空白 ⇒ ④ 把空白盖回画布 ⇒ 整页空白，正是 svg-filter 在 4× 下的样子。 */
static void scene_capture_mid_frame(BgpuDevice dev, BgpuCmd cmd, BgpuTexture canvas) {
    const BgpuPipeline solid = make_pipe(dev, BGPU_VERTEX_2D, BGPU_BLEND_REPLACE, 0);
    const BgpuPipeline textured =
        make_pipe(dev, BGPU_VERTEX_2D, BGPU_BLEND_REPLACE, BGPU_PIPE_TEXTURED);
    const BgpuSampler samp = make_sampler(dev, BGPU_FILTER_NEAREST, BGPU_WRAP_CLAMP);
    const BgpuTexture a =
        make_tex(dev, 128, 128, BGPU_FORMAT_RGBA8, BGPU_TEXTURE_RENDER_TARGET);

    begin(cmd, canvas, 0xFF202020u, 0, 0);
    bingpu_cmd_set_pipeline(cmd, solid);
    quad(cmd, 136, 8, 184, 56, 0xFFFF0000u);
    quad(cmd, 144, 72, 176, 104, 0xFF00C000u);
    bingpu_cmd_end_render(cmd);

    /* ② 画布刚渲染完就是采样源；A = 画布的右半（x 128..256，y 全高），1:1 + NEAREST */
    begin(cmd, a, 0xFF000000u, 0, 0);
    bingpu_cmd_set_pipeline(cmd, textured);
    bingpu_cmd_set_texture(cmd, canvas, samp);
    const BgpuVertex cap[4] = {{0, 0, 0, 0.5f, 0, 0xFFFFFFFFu},
                               {128, 0, 0, 1.0f, 0, 0xFFFFFFFFu},
                               {0, 128, 0, 0.5f, 1.0f, 0xFFFFFFFFu},
                               {128, 128, 0, 1.0f, 1.0f, 0xFFFFFFFFu}};
    bingpu_cmd_draw_quads(cmd, cap, 1);
    bingpu_cmd_end_render(cmd);

    /* ③ 同一个目标第二趟（LOAD）：上一趟的内容必须还在 */
    begin_load(cmd, a);
    bingpu_cmd_set_pipeline(cmd, solid);
    quad(cmd, 72, 72, 120, 120, 0xFF0000FFu);
    bingpu_cmd_end_render(cmd);

    /* ④ 把 A 合成回画布右上象限（1:1） */
    begin_load(cmd, canvas);
    bingpu_cmd_set_pipeline(cmd, textured);
    bingpu_cmd_set_texture(cmd, a, samp);
    quad(cmd, 128, 0, 256, 128, 0xFFFFFFFFu);
    bingpu_cmd_end_render(cmd);
    /* 这里**不**销毁纹理/管线：命令缓冲是先录后执行，帧尾才提交（见
     * scene_multipass 的文件头），录完就毁对象等于拿悬垂句柄去提交。 */
}

/* 场景二（更狠一拍）：A 上两趟（第二趟 LOAD），**中途**把 A 当采样源一次。
 *   ① A：清空 + 红块；② B（64×64 离屏）：采样 A；③ A：LOAD + 绿块；
 *   ④ 画布：清空，左半采样 B、右半采样 A。
 * 判据：B 里必须有红（说明①的 resolve 在第一趟结束就完成了）；A 的右半必须有
 * 红**和**绿（说明③之后采样到的是最新内容，两趟都得在）。 */
static void scene_capture_between_passes(BgpuDevice dev, BgpuCmd cmd, BgpuTexture canvas) {
    const BgpuPipeline solid = make_pipe(dev, BGPU_VERTEX_2D, BGPU_BLEND_REPLACE, 0);
    const BgpuPipeline textured =
        make_pipe(dev, BGPU_VERTEX_2D, BGPU_BLEND_REPLACE, BGPU_PIPE_TEXTURED);
    const BgpuSampler samp = make_sampler(dev, BGPU_FILTER_NEAREST, BGPU_WRAP_CLAMP);
    const BgpuTexture a =
        make_tex(dev, 64, 64, BGPU_FORMAT_RGBA8, BGPU_TEXTURE_RENDER_TARGET);
    const BgpuTexture b =
        make_tex(dev, 64, 64, BGPU_FORMAT_RGBA8, BGPU_TEXTURE_RENDER_TARGET);

    begin(cmd, a, 0xFF000000u, 0, 0);
    bingpu_cmd_set_pipeline(cmd, solid);
    quad(cmd, 8, 8, 40, 40, 0xFFFF0000u);
    bingpu_cmd_end_render(cmd);

    /* 中途采样：A 是"刚渲染完的目标" */
    begin(cmd, b, 0xFF000000u, 0, 0);
    bingpu_cmd_set_pipeline(cmd, textured);
    bingpu_cmd_set_texture(cmd, a, samp);
    quad(cmd, 0, 0, 64, 64, 0xFFFFFFFFu);
    bingpu_cmd_end_render(cmd);

    begin_load(cmd, a);
    bingpu_cmd_set_pipeline(cmd, solid);
    quad(cmd, 40, 40, 56, 56, 0xFF00FF00u);
    bingpu_cmd_end_render(cmd);

    begin(cmd, canvas, 0xFF202020u, 0, 0);
    bingpu_cmd_set_pipeline(cmd, textured);
    bingpu_cmd_set_texture(cmd, b, samp);
    quad(cmd, 0, 0, 64, 64, 0xFFFFFFFFu);
    bingpu_cmd_set_texture(cmd, a, samp);
    quad(cmd, 64, 0, 128, 64, 0xFFFFFFFFu);
    bingpu_cmd_end_render(cmd);
}

/* 帧中途销毁"还在命令缓冲里的目标"—— binUI 的模糊链换尺寸时就是这么干的
 * （`ensureBlurLevel` 里 freeTex(旧 level7) 紧接着建同尺寸的新图）。
 *
 * 命令缓冲是**先录后执行**：录进去的 draw / barrier 引用的是那张图的显存。销毁
 * 那一刻就把显存还给驱动 == 让还没执行的命令去读一块已经被下一次分配复用的内存。
 * 1× 时复用的还是同尺寸的新图，读出来"是错的但有效"，看不出大问题；0.15.0 之前
 * 4× 时被释放的还有 MSAA companion，GPU 当场页错误 → 驱动报 VK_ERROR_DEVICE_LOST
 * → 整帧命令全丢（binUI 的 svg-filter 页在 4× 下整页黑）。
 *
 * 判据：销毁版与不销毁版的像素必须逐位相同（同一个 msaa 档内比）。
 * g_destroy_mid 控制销毁那一步，两遍跑的是同一条命令流。 */
static int g_destroy_mid = 0;

static void scene_mid_destroy(BgpuDevice dev, BgpuCmd cmd, BgpuTexture canvas) {
    const BgpuPipeline solid = make_pipe(dev, BGPU_VERTEX_2D, BGPU_BLEND_REPLACE, 0);
    const BgpuPipeline textured =
        make_pipe(dev, BGPU_VERTEX_2D, BGPU_BLEND_REPLACE, BGPU_PIPE_TEXTURED);
    const BgpuSampler samp = make_sampler(dev, BGPU_FILTER_NEAREST, BGPU_WRAP_CLAMP);
    BgpuTexture last = BGPU_INVALID;

    begin(cmd, canvas, 0xFF202020u, 0, 0);
    bingpu_cmd_set_pipeline(cmd, solid);
    quad(cmd, 8, 8, 232, 232, 0xFFFF0000u);
    bingpu_cmd_end_render(cmd);

    /* 一轮 = 建一张离屏 → 画 → 帧中途销毁（它还被刚录进去的 draw 引用着）
     *        → 立刻建一张同尺寸的（驱动多半复用刚还回去的显存）→ 画 → 合成回画布。
     * 与 binUI 模糊链"换尺寸就把旧 level 释放掉"的命令顺序一致。 */
    for (int i = 0; i < 12; i++) {
        BgpuTexture a = make_tex(dev, 226, 151, BGPU_FORMAT_RGBA8, BGPU_TEXTURE_RENDER_TARGET);
        begin(cmd, a, 0xFF000000u, 0, 0);
        bingpu_cmd_set_pipeline(cmd, solid);
        quad(cmd, 4, 4, 222, 147, 0xFF00C000u);
        quad(cmd, 40, 20, 180, 130, 0xFF3366CCu);
        bingpu_cmd_end_render(cmd);
        if (g_destroy_mid) bingpu_texture_destroy(dev, a);

        const BgpuTexture b = make_tex(dev, 226, 151, BGPU_FORMAT_RGBA8,
                                       BGPU_TEXTURE_RENDER_TARGET);
        begin(cmd, b, 0xFF000080u, 0, 0);
        bingpu_cmd_set_pipeline(cmd, textured);
        bingpu_cmd_set_texture(cmd, canvas, samp);
        quad(cmd, 0, 0, 226, 151, 0xFFFFFFFFu);
        bingpu_cmd_end_render(cmd);
        last = b;

        /* 合成回画布（半透明，叠着画，别盖住左边那块红） */
        begin_load(cmd, canvas);
        bingpu_cmd_set_pipeline(cmd, textured);
        bingpu_cmd_set_texture(cmd, last, samp);
        quad(cmd, 232, 4, 458, 155, 0x80FFFFFFu);
        bingpu_cmd_end_render(cmd);
    }
}

/* 同一条场景在 msaa=1 与 msaa=4 下各跑一遍（都走 frame_read），逐像素比。 */
static void run_msaa_capture(BgpuBackendKind kind, const char* name, SceneFn fn, uint32_t w,
                             uint32_t h) {
    const size_t bytes = (size_t)w * h * 4;
    uint8_t* one = (uint8_t*)malloc(bytes);
    uint8_t* four = (uint8_t*)malloc(bytes);
    printf("[msaa 帧中多目标：%s]\n", name);
    if (!one || !four) {
        printf("  [skip] 分配失败\n");
        free(one);
        free(four);
        return;
    }
    const int ok1 = render_swapchain_msaa(kind, 1, fn, w, h, one);
    const int ok4 = render_swapchain_msaa(kind, 4, fn, w, h, four);
    if (!ok1 || !ok4) {
        g_fail++;
        printf("  FAIL %s: 渲染失败\n", name);
        free(one);
        free(four);
        return;
    }
    int maxdiff = 0;
    const size_t ndiff = pixel_diff(one, four, (size_t)w * h, &maxdiff);
    printf("  msaa=1 vs msaa=4：差异像素 %zu/%zu（%.2f%%），最大通道差 %d\n", ndiff,
           (size_t)w * h, 100.0 * (double)ndiff / (double)((size_t)w * h), maxdiff);
    CHECK(ndiff == 0, "同一串命令流在 msaa=1 / msaa=4 下必须逐像素相同（帧中多目标）");
    free(one);
    free(four);
}

static void run_msaa_case(BgpuBackendKind kind) {
    int has_cap = 0;
    const uint32_t maxs = caps_msaa_max(kind, &has_cap);
    printf("[msaa(斜边覆盖率)]\n");
    if (!has_cap || maxs < 4) {
        printf("  [skip] %s 没有可用的 4× MSAA（BGPU_CAP_MSAA=%d，caps.msaa_samples=%u）\n",
               bingpu_backend_name(kind), has_cap, maxs);
        return;
    }
    uint8_t* a = (uint8_t*)malloc(64 * 64 * 4);
    uint8_t* b = (uint8_t*)malloc(64 * 64 * 4);
    if (!a || !b) {
        printf("  [skip] 分配失败\n");
        free(a);
        free(b);
        return;
    }
    const int ok1 = render_msaa(kind, 0, scene_diagonal, 64, 64, a);
    const int ok4 = render_msaa(kind, 4, scene_diagonal, 64, 64, b);
    if (!ok1 || !ok4) {
        g_fail++;
        printf("  FAIL msaa: 渲染失败\n");
        free(a);
        free(b);
        return;
    }
    const size_t mid1 = count_midtones(a, 64, 64);
    const size_t mid4 = count_midtones(b, 64, 64);
    printf("  msaa=1 中间色像素 %zu，msaa=4 中间色像素 %zu（差 %zu）\n", mid1, mid4,
           mid4 > mid1 ? mid4 - mid1 : 0);
    CHECK(mid4 > mid1 + 40, "4× 的斜边中间色必须明显多于一截（期望 > +40）");
    CHECK(mid4 >= mid1 * 3, "4× 的中间色个数至少是 1× 的三倍");

    /* 交换链路径（frame_read）：同一场景、同一档位，结果必须是 resolve 后的单采样图 */
    uint8_t* c = (uint8_t*)malloc(64 * 64 * 4);
    if (c && render_swapchain_msaa(kind, 4, scene_diagonal, 64, 64, c)) {
        const size_t midf = count_midtones(c, 64, 64);
        printf("  frame_read(msaa=4) 中间色像素 %zu\n", midf);
        CHECK(midf > mid1 + 40, "frame_read 也必须拿到 resolve 后的抗锯齿边缘");
    } else {
        g_fail++;
        printf("  FAIL msaa: 交换链路径渲染失败\n");
    }
    free(c);

    /* LOAD 的 pass 必须保留上一趟内容（VK 需要先把单采样镜像搬回多采样附件） */
    uint8_t* d = (uint8_t*)malloc(64 * 64 * 4);
    if (d && render_msaa(kind, 4, scene_two_pass, 64, 64, d)) {
        const int first = region_is_white(d, 64, 12, 12, 20, 20);
        const int second = region_is_white(d, 64, 44, 44, 52, 52);
        printf("  msaa=4 两趟 LOAD：第一趟内容保留=%d，第二趟内容=%d\n", first, second);
        CHECK(first && second, "MSAA 下 begin_render(load) 必须保留上一趟的内容");
    } else {
        g_fail++;
        printf("  FAIL msaa: 两趟 LOAD 渲染失败\n");
    }
    free(d);

    /* 帧中多目标 / 多趟：msaa=1 与 msaa=4 必须逐像素相同。
     * 0.14.0 的 MSAA 用例全是"单目标、画完就读"，这条命令流一帧里换了四次目标、
     * 而且把"刚画过的目标"当采样源 —— binUI 的毛玻璃就是它。 */
    run_msaa_capture(kind, "画布 → 离屏抓取 → 合成回画布", scene_capture_mid_frame, 256, 128);
    run_msaa_capture(kind, "同一目标两趟 + 中途当采样源", scene_capture_between_passes, 128, 64);

    /* 帧中销毁仍在命令缓冲里的目标：1× / 4× 都必须与"不销毁"逐位相同。
     * 这条钉的是 0.15.0 的根因（立即 vkFreeMemory ⇒ 4× 下设备丢），而 0.14.x 的
     * "单目标、画完就读"用例一个都碰不到它。 */
    {
        const uint32_t w = 256, h = 128;
        uint8_t* keep = (uint8_t*)malloc((size_t)w * h * 4);
        uint8_t* gone = (uint8_t*)malloc((size_t)w * h * 4);
        printf("[msaa 帧中销毁目标]\n");
        for (int ms = 0; ms < 2; ms++) {
            const uint32_t samples = ms ? 4 : 1;
            g_destroy_mid = 0;
            const int k1 = render_swapchain_msaa(kind, samples, scene_mid_destroy, w, h, keep);
            g_destroy_mid = 1;
            const int k2 = render_swapchain_msaa(kind, samples, scene_mid_destroy, w, h, gone);
            if (!k1 || !k2) {
                g_fail++;
                printf("  FAIL msaa=%u：帧中销毁目标的场景渲染失败（设备丢了？）\n", samples);
                continue;
            }
            int maxdiff = 0;
            const size_t ndiff = pixel_diff(keep, gone, (size_t)w * h, &maxdiff);
            printf("  msaa=%u 销毁版 vs 不销毁版：差异像素 %zu/%zu，最大通道差 %d\n", samples,
                   ndiff, (size_t)w * h, maxdiff);
            CHECK(ndiff == 0, "帧中销毁仍在缓冲里的目标不能影响本帧像素（1×/4× 都要）");
        }
        g_destroy_mid = 0;
        free(keep);
        free(gone);
    }

    /* 语义检查：4× 下抓取/合成真的搬到了内容（不只是"两边一样地坏"） */
    uint8_t* cap = (uint8_t*)malloc(256 * 128 * 4);
    if (cap && render_swapchain_msaa(kind, 4, scene_capture_mid_frame, 256, 128, cap)) {
        const int red_kept = pixel_is(cap, 256, 150, 20, 255, 0, 0, 8);
        const int green_kept = pixel_is(cap, 256, 155, 85, 0, 192, 0, 8);
        const int blue_new = pixel_is(cap, 256, 210, 90, 0, 0, 255, 8);
        const int bg_kept = pixel_is(cap, 256, 8, 8, 32, 32, 32, 8);
        printf("  4× 帧中抓取：红=%d 绿=%d 第二趟蓝=%d 画布左半背景=%d\n", red_kept, green_kept,
               blue_new, bg_kept);
        CHECK(red_kept && green_kept, "4×：抓到的画布内容必须回到画布上（不能是空白）");
        CHECK(blue_new, "4×：LOAD 的第二趟内容也必须一起搬回来");
        CHECK(bg_kept, "4×：抓取/合成之外的画布内容必须原样保留");
    } else {
        g_fail++;
        printf("  FAIL msaa: 帧中多目标渲染失败\n");
    }
    free(cap);

    /* 深度附件也必须按同一采样数建：MSAA 下 depth(LESS) 仍要让近处红色胜出 */
    uint8_t* z = (uint8_t*)malloc(64 * 64 * 4);
    if (z && render_msaa(kind, 4, scene_depth, 64, 64, z)) {
        const uint8_t* c0 = z + ((size_t)32 * 64 + 32) * 4;
        printf("  msaa=4 depth(LESS) 中心像素 R=%u B=%u\n", c0[0], c0[2]);
        CHECK(c0[0] > 200 && c0[2] < 40, "MSAA 下深度测试必须仍然让近处（红）胜出");
    } else {
        g_fail++;
        printf("  FAIL msaa: 深度路径渲染失败\n");
    }
    free(z);
    free(a);
    free(b);
}

/* 棋盘格纹理（2 像素一格）放大到整幅目标；目标 64×64、纹理 256×256 ⇒ 缩小 4×，
 * 隐式 LOD ≈ 2。mip 链存在时 LOD 2 正好是"每级 4×4 平均"，棋盘格化成纯灰；
 * 没有 mip 时双线性只在 2 级纹素内插值，输出一圈明显的摩尔纹。 */
static BgpuTexture make_checker(BgpuDevice dev, uint32_t size, uint32_t block, uint32_t flags) {
    uint8_t* px = (uint8_t*)malloc((size_t)size * size * 4);
    if (!px) return BGPU_INVALID;
    for (uint32_t y = 0; y < size; y++) {
        for (uint32_t x = 0; x < size; x++) {
            const uint8_t v = (uint8_t)((((x / block) + (y / block)) & 1) ? 255 : 0);
            uint8_t* p = px + ((size_t)y * size + x) * 4;
            p[0] = p[1] = p[2] = v;
            p[3] = 255;
        }
    }
    BgpuTextureDesc td;
    memset(&td, 0, sizeof(td));
    td.struct_size = (uint32_t)sizeof(td);
    td.width = size;
    td.height = size;
    td.format = BGPU_FORMAT_RGBA8;
    td.flags = flags;
    BgpuTexture tex = BGPU_INVALID;
    if (bingpu_texture_create(dev, &td, &tex) != BGPU_OK) {
        free(px);
        return BGPU_INVALID;
    }
    if (bingpu_texture_upload(tex, NULL, px, size * 4) != BGPU_OK) {
        printf("  棋盘格上传失败：%s\n", bingpu_last_error());
        bingpu_texture_destroy(dev, tex);
        free(px);
        return BGPU_INVALID;
    }
    free(px);
    return tex;
}

static int render_mip_scene(BgpuBackendKind kind, uint32_t tex_flags, uint32_t* out_levels,
                            uint8_t* out) {
    /* 目标 60 而非 64：256/60 = 4.27 的**非整数**缩小才会让 1× 的双线性采样相位在
     * 棋盘格上滑动（64 时 4× 整数倍刚好每次落在格心，连 1× 都是纯灰，测不出摩尔纹）。 */
    const uint32_t w = 60, h = 60;
    BgpuDeviceDesc dd;
    memset(&dd, 0, sizeof(dd));
    dd.struct_size = (uint32_t)sizeof(dd);
    dd.backend = kind;
    dd.width = w;
    dd.height = h;
    dd.flags = BGPU_DEVICE_HEADLESS;
    BgpuDevice dev = BGPU_INVALID;
    if (bingpu_device_create(&dd, &dev) != BGPU_OK) return 0;
    BgpuTexture target =
        make_tex(dev, w, h, BGPU_FORMAT_RGBA8, BGPU_TEXTURE_RENDER_TARGET | BGPU_TEXTURE_CPU_READ);
    BgpuTexture tex = make_checker(dev, 256, 2, tex_flags);
    if (tex == BGPU_INVALID) {
        bingpu_device_destroy(dev);
        return 0;
    }
    BgpuTextureInfo ti;
    memset(&ti, 0, sizeof(ti));
    ti.struct_size = (uint32_t)sizeof(ti);
    if (bingpu_texture_info(tex, &ti) == BGPU_OK && out_levels) *out_levels = ti.mip_levels;

    BgpuCmd cmd = BGPU_INVALID;
    bingpu_cmd_create(dev, &cmd);
    const BgpuSampler samp = make_sampler(dev, BGPU_FILTER_LINEAR, BGPU_WRAP_CLAMP);
    const BgpuPipeline p = make_pipe(dev, BGPU_VERTEX_2D, BGPU_BLEND_REPLACE, BGPU_PIPE_TEXTURED);
    begin(cmd, target, 0xFF000000u, 0, 0);
    bingpu_cmd_set_pipeline(cmd, p);
    bingpu_cmd_set_texture(cmd, tex, samp);
    quad(cmd, 0, 0, (float)w, (float)h, 0xFFFFFFFFu);
    bingpu_cmd_end_render(cmd);
    uint32_t stride = 0;
    const int ok = bingpu_texture_read(target, out, (size_t)w * h * 4, &stride) == BGPU_OK;
    if (!ok) printf("  棋盘格回读失败：%s\n", bingpu_last_error());
    bingpu_sampler_destroy(dev, samp);
    bingpu_pipeline_destroy(dev, p);
    bingpu_texture_destroy(dev, tex);
    bingpu_cmd_destroy(dev, cmd);
    bingpu_device_destroy(dev);
    return ok;
}

/* 去掉边缘 8 像素后，R 通道的平均值 / 平均绝对偏差（摩尔纹指标）。 */
static void gray_stats(const uint8_t* px, uint32_t w, uint32_t h, double* mean, double* mad) {
    double sum = 0.0;
    size_t n = 0;
    for (uint32_t y = 8; y < h - 8; y++) {
        for (uint32_t x = 8; x < w - 8; x++) {
            sum += px[((size_t)y * w + x) * 4];
            n++;
        }
    }
    const double m = n ? sum / (double)n : 0.0;
    double dev = 0.0;
    for (uint32_t y = 8; y < h - 8; y++) {
        for (uint32_t x = 8; x < w - 8; x++) {
            dev += fabs((double)px[((size_t)y * w + x) * 4] - m);
        }
    }
    *mean = m;
    *mad = n ? dev / (double)n : 0.0;
}

static void run_mip_case(BgpuBackendKind kind) {
    printf("[mip(缩小的棋盘格)]\n");
    BgpuDeviceDesc dd;
    memset(&dd, 0, sizeof(dd));
    dd.struct_size = (uint32_t)sizeof(dd);
    dd.backend = kind;
    dd.width = 8;
    dd.height = 8;
    dd.flags = BGPU_DEVICE_HEADLESS;
    BgpuDevice probe = BGPU_INVALID;
    int has_cap = 0;
    if (bingpu_device_create(&dd, &probe) == BGPU_OK) {
        BgpuCaps caps;
        memset(&caps, 0, sizeof(caps));
        caps.struct_size = (uint32_t)sizeof(caps);
        has_cap = bingpu_device_caps(probe, &caps) == BGPU_OK &&
                  (caps.flags & BGPU_CAP_MIPMAPS) != 0;
        bingpu_device_destroy(probe);
    }
    if (!has_cap) {
        printf("  [skip] %s 没有 BGPU_CAP_MIPMAPS（MIPMAPS 仍被接受但降级为 1 级）\n",
               bingpu_backend_name(kind));
        return;
    }
    uint8_t* a = (uint8_t*)malloc(60 * 60 * 4);
    uint8_t* b = (uint8_t*)malloc(60 * 60 * 4);
    uint32_t lv0 = 0, lv1 = 0;
    const int ok0 = render_mip_scene(kind, 0, &lv0, a);
    const int ok1 = render_mip_scene(kind, BGPU_TEXTURE_MIPMAPS, &lv1, b);
    if (!ok0 || !ok1) {
        g_fail++;
        printf("  FAIL mip: 渲染失败\n");
        free(a);
        free(b);
        return;
    }
    double m0 = 0.0, d0 = 0.0, m1 = 0.0, d1 = 0.0;
    gray_stats(a, 60, 60, &m0, &d0);
    gray_stats(b, 60, 60, &m1, &d1);
    printf("  1×（mip_levels=%u）：均值 %.2f，平均绝对偏差 %.2f\n", lv0, m0, d0);
    printf("  mip（mip_levels=%u）：均值 %.2f，平均绝对偏差 %.2f\n", lv1, m1, d1);
    CHECK(lv0 == 1, "不带 MIPMAPS 的纹理必须仍是 1 级（默认路径不变）");
    CHECK(lv1 >= 8, "256×256 的完整 mip 链至少 8 级（floor(log2(256))+1 = 9）");
    CHECK(d0 > 20.0, "1× 缩小的棋盘格必须能看到明显的摩尔纹（MAD > 20）");
    CHECK(d1 < 8.0, "mip 后应接近纯灰（MAD < 8）");
    CHECK(d1 * 4.0 < d0, "mip 的偏差必须比 1× 小一个数量级方向（< 1/4）");
    CHECK(m1 > 90.0 && m1 < 170.0, "mip 后的灰度应接近棋盘格的平均（约 128）");
    free(a);
    free(b);
}

static void run_quality_cases(BgpuBackendKind kind) {
    run_msaa_case(kind);
    run_mip_case(kind);
}

static int parity_run(const char* label, BgpuBackendKind kind) {
    g_kind = kind;
    printf("%s vs CPU 像素对照（binGPU %s，ABI %u）\n", label, bingpu_version_string(),
           bingpu_abi_version());
    if (!bingpu_backend_available(kind)) {
        printf("跳过：这台机器没有可用的 %s 后端（没驱动 / 没编对应头文件 / 系统不支持）\n",
               bingpu_backend_name(kind));
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
    /* 一帧多趟 + 顶点流扩容（0.13.0 的 VK 缺陷）：整页渲染才会踩到的那条路 */
    run_case("multipass(目标来回切 + 顶点流扩容)", scene_multipass, 64, 64, 0, 0.0);
    /* 双线性取样：CPU 逐像素用 double 累加、GPU 用 float 插值（D3D11 还有定点
     * 子纹素精度）→ 偶发 1 个通道差。这是"两条数值路径"的固有差异，不是不一致；
     * 容差写在这里而不是含糊过去。 */
    run_case("texture(linear/clamp)", scene_texture_linear, 64, 64, 2, 25.0);
    run_case("texture(nearest/repeat)", scene_texture_repeat, 64, 64, 0, 0.0);
    run_case("texture(box 局部上传)", scene_texture_box, 64, 64, 0, 0.0);
    run_case("depth(LESS)", scene_depth, 64, 64, 0, 0.0);
    run_case("mesh(draw_indexed)", scene_mesh, 64, 64, 0, 0.0);
    /* 交换链路径（frame_begin → … → frame_end → frame_read）：与上面的 texture_read
     * 是两条回读实现。bin2d 的 GPU 桥就长这样，0.5.0 的两个后端 bug 都只在这条路上。 */
    run_swapchain_case("swapchain(frame_read 满幅纹理)", 96, 64, 0, 0.0);

    /* ABI 3：两个能力都要真的实现才置 cap 位。没实现的末端会自报不支持，
     * 于是这两条会以"必失败"的形式暴露出来，而不是悄悄跳过。 */
    if (backend_has_cap(kind, BGPU_CAP_LIGHTING))
        run_case("lighting(Lambert：255/26/206)", scene_lighting, 24, 8, 0, 0.0);
    else
        printf("  [skip] lighting：这个后端自报不支持 BGPU_CAP_LIGHTING\n");
    if (backend_has_cap(kind, BGPU_CAP_PERSPECTIVE))
        run_case("perspective(u=0.5625 vs 0.2432)", scene_perspective, 16, 16, 0, 0.0);
    else
        printf("  [skip] perspective：这个后端自报不支持 BGPU_CAP_PERSPECTIVE\n");

    /* 下一版的质量档：只在能力可达时跑，拿不到就 skip（不 fail）。 */
    run_quality_cases(kind);

    printf("\n通过 %d 项，失败 %d 项\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

#endif  // BINGPU_GPU_PARITY_H
