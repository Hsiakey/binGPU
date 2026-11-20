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

    printf("\n通过 %d 项，失败 %d 项\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

#endif  // BINGPU_GPU_PARITY_H
