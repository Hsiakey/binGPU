/* tools/bingpu_bench.c —— CPU 后端的固定基准（吞吐 + 像素校验和）
 *
 * 为什么要有它：
 *   CPU 参考后端是整条链的"像素裁判"（bin2D 的 GPU 往返、无 GPU 的 CI/出图都走它），
 *   但 0.1.0/0.2.0 从没量过吞吐。没有固定场景、没有固定校验和，"优化"就只是手感。
 *   这个工具把三件事固定下来：
 *     ① 场景固定：六类负载（纯色填充 / 纹理 / 裁剪内小批 / 索引网格 / 离屏合成 / 回读），
 *        参数写死 —— 换机器只换数字，不换题；
 *     ② 计时固定：先预热一次，再跑多次取**最好**一次（排除调度抖动）；
 *     ③ 结果固定：每项额外跑一遍"清 → 画 → 回读"，对回读字节做 FNV-1a 校验和。
 *        优化前后校验和必须逐位相同 —— 这是"行为不变"的机器可查证据。
 *
 * 为什么光栅场景渲到**离屏目标**：交换链的 frame_end 会额外拷一份 back→front
 * （1024×1024×4 = 4 MB），会把光栅开销淹掉。所以 ①~④ 直接渲到 RENDER_TARGET 纹理，
 * 只量 begin_render → draw → end_render；present 的拷贝单独放在 ⑤（离屏合成）里量。
 *
 * 怎么读数字：
 *   MPix/s  = 每秒**实际覆盖**的像素数（场景自己声明分子）。
 *   MQuad/s = 每秒四边形数（小四边形场景才有的看）。
 *
 * 用法：bingpu_bench             跑全部并打印表格
 *       bingpu_bench --checksum  只跑校验和（把"是否逐位不变"写成 CI 可比的数字）
 *       bingpu_bench --backend vulkan [--msaa 4]
 *                                换执行体跑同一套场景（默认仍是 CPU / 1×）。
 *                                MSAA 的代价就是同一条命令在 1× 与 4× 下的帧时差。
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bingpu.h"

/* ---------------- 计时 ---------------- */
#ifdef _WIN32
#include <windows.h>
static double now_ms(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return 1000.0 * (double)t.QuadPart / (double)freq.QuadPart;
}
#else
#include <time.h>
static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}
#endif

/* ---------------- 常量 ---------------- */
#define TARGET_W 1024u
#define TARGET_H 1024u
#define TEX_W 256u
#define TEX_H 256u
#define MESH_CELLS 32u       /* 32×32 个格子 → 1024 quad / 2048 tri */
#define SCISSOR_QUADS 100u   /* 10×10 个小四边形 */
#define SMALL_QUAD 64u       /* 小四边形边长 */
#define ITERATIONS 5

static int g_failures = 0;
/* 默认仍是 CPU / 1× —— 不带参数跑出来的表格与 0.13.0 一字不差。
 * --backend / --msaa 只是让同一套场景能换执行体量 MSAA 的代价。 */
static BgpuBackendKind g_backend = BGPU_BACKEND_CPU;
static uint32_t g_msaa = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

/* ---------------- 环境 ---------------- */
typedef struct {
    BgpuDevice dev;
    BgpuCmd cmd;
    BgpuSwapchain sc;  /* 1024×1024 交换链（合成场景用） */
    BgpuTexture rt;    /* 1024×1024 离屏目标（光栅场景用） */
    BgpuTexture tex;   /* 256×256 图案纹理 */
    BgpuSampler near_rep, near_clamp, lin_rep, lin_clamp;
    BgpuPipeline solid[5]; /* 纯色：五个混合模式 */
    BgpuPipeline tex_pipe;  /* 纹理管线：2D 布局 + TEXTURED */
    BgpuPipeline mesh_pipe; /* 网格管线：MESH 布局 + TEXTURED */
    BgpuBuffer mesh_vb, mesh_ib;
    BgpuVertex quad[4]; /* 铺满目标的四边形 */
    uint8_t* readback;
} Env;

/* FNV-1a 64：够做逐位比较，比 crc 简单，比"人眼看图"可靠。 */
static uint64_t fnv1a(const void* data, size_t n) {
    const uint8_t* p = (const uint8_t*)data;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

static void die(const char* what) {
    printf("FATAL: %s: %s\n", what, bingpu_last_error());
    fflush(stdout);   /* exit() 会跑静态析构，别把没刷的行吞了 */
    exit(2);
}

/* ---------------- 资源构造 ---------------- */
static BgpuTexture make_rt(BgpuDevice dev, uint32_t w, uint32_t h, BgpuFormat fmt) {
    BgpuTextureDesc d;
    memset(&d, 0, sizeof(d));
    d.struct_size = (uint32_t)sizeof(d);
    d.width = w;
    d.height = h;
    d.format = fmt;
    d.flags = BGPU_TEXTURE_RENDER_TARGET | BGPU_TEXTURE_CPU_READ;
    BgpuTexture t = BGPU_INVALID;
    if (bingpu_texture_create(dev, &d, &t) != BGPU_OK) die("texture_create(RT)");
    return t;
}

/* 图案纹理：不随机（要可复现），棋盘 + 渐变让采样器的每条分支都被走到。 */
static BgpuTexture make_pattern(BgpuDevice dev, uint32_t w, uint32_t h) {
    const size_t bytes = (size_t)w * h * 4u;
    uint8_t* px = (uint8_t*)malloc(bytes);
    if (!px) die("malloc(pattern)");
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint8_t* p = px + ((size_t)y * w + x) * 4u;
            p[0] = (uint8_t)((x * 255u) / (w - 1u));                     /* R：水平渐变 */
            p[1] = (uint8_t)((y * 255u) / (h - 1u));                     /* G：垂直渐变 */
            p[2] = (uint8_t)((((x >> 3) ^ (y >> 3)) & 1u) ? 200u : 32u); /* B：8px 棋盘 */
            p[3] = (uint8_t)(((x + y) & 3u) == 0 ? 128u : 255u);          /* A：条纹 */
        }
    }
    BgpuTextureDesc d;
    memset(&d, 0, sizeof(d));
    d.struct_size = (uint32_t)sizeof(d);
    d.width = w;
    d.height = h;
    d.format = BGPU_FORMAT_RGBA8;
    d.flags = BGPU_TEXTURE_CPU_READ;
    BgpuTexture t = BGPU_INVALID;
    if (bingpu_texture_create(dev, &d, &t) != BGPU_OK) die("texture_create(pattern)");
    if (bingpu_texture_upload(t, NULL, px, w * 4u) != BGPU_OK) die("texture_upload");
    free(px);
    return t;
}

static BgpuSampler make_sampler(BgpuDevice dev, BgpuFilter f, BgpuWrap w) {
    BgpuSamplerDesc d;
    memset(&d, 0, sizeof(d));
    d.struct_size = (uint32_t)sizeof(d);
    d.min_filter = f;
    d.mag_filter = f;
    d.wrap_u = w;
    d.wrap_v = w;
    BgpuSampler s = BGPU_INVALID;
    if (bingpu_sampler_create(dev, &d, &s) != BGPU_OK) die("sampler_create");
    return s;
}

static BgpuPipeline make_pipeline(BgpuDevice dev, BgpuVertexLayout layout, BgpuBlend blend,
                                  uint32_t flags) {
    BgpuPipelineDesc d;
    memset(&d, 0, sizeof(d));
    d.struct_size = (uint32_t)sizeof(d);
    d.vertex_layout = layout;
    d.blend = blend;
    d.flags = flags;
    BgpuPipeline p = BGPU_INVALID;
    if (bingpu_pipeline_create(dev, &d, &p) != BGPU_OK) die("pipeline_create");
    return p;
}

static void set_quad(Env* e, float x0, float y0, float x1, float y1, float u1, float v1,
                     uint32_t argb) {
    const float xs[4] = {x0, x1, x0, x1};
    const float ys[4] = {y0, y0, y1, y1};
    const float us[4] = {0.0f, u1, 0.0f, u1};
    const float vs[4] = {0.0f, 0.0f, v1, v1};
    for (int i = 0; i < 4; i++) {
        e->quad[i].x = xs[i];
        e->quad[i].y = ys[i];
        e->quad[i].z = 0.5f;
        e->quad[i].u = us[i];
        e->quad[i].v = vs[i];
        e->quad[i].color = argb;
    }
}

static void env_init(Env* e) {
    memset(e, 0, sizeof(*e));

    BgpuDeviceDesc dd;
    memset(&dd, 0, sizeof(dd));
    dd.struct_size = (uint32_t)sizeof(dd);
    dd.backend = g_backend;
    dd.width = TARGET_W;
    dd.height = TARGET_H;
    dd.flags = BGPU_DEVICE_HEADLESS;
    dd.msaa_samples = g_msaa;
    if (bingpu_device_create(&dd, &e->dev) != BGPU_OK) die("device_create");
    if (bingpu_cmd_create(e->dev, &e->cmd) != BGPU_OK) die("cmd_create");

    BgpuSwapchainDesc sd;
    memset(&sd, 0, sizeof(sd));
    sd.struct_size = (uint32_t)sizeof(sd);
    sd.width = TARGET_W;
    sd.height = TARGET_H;
    sd.format = BGPU_FORMAT_RGBA8;
    if (bingpu_swapchain_create(e->dev, &sd, &e->sc) != BGPU_OK) die("swapchain_create");

    e->rt = make_rt(e->dev, TARGET_W, TARGET_H, BGPU_FORMAT_RGBA8);
    e->tex = make_pattern(e->dev, TEX_W, TEX_H);
    e->near_rep = make_sampler(e->dev, BGPU_FILTER_NEAREST, BGPU_WRAP_REPEAT);
    e->near_clamp = make_sampler(e->dev, BGPU_FILTER_NEAREST, BGPU_WRAP_CLAMP);
    e->lin_rep = make_sampler(e->dev, BGPU_FILTER_LINEAR, BGPU_WRAP_REPEAT);
    e->lin_clamp = make_sampler(e->dev, BGPU_FILTER_LINEAR, BGPU_WRAP_CLAMP);

    set_quad(e, 0.0f, 0.0f, (float)TARGET_W, (float)TARGET_H, 1.0f, 1.0f, 0xFFC04040u);

    static const BgpuBlend kBlends[5] = {BGPU_BLEND_PREMULTIPLIED, BGPU_BLEND_REPLACE,
                                         BGPU_BLEND_KEEP, BGPU_BLEND_ERASE,
                                         BGPU_BLEND_ADDITIVE};
    for (int i = 0; i < 5; i++) {
        e->solid[i] = make_pipeline(e->dev, BGPU_VERTEX_2D, kBlends[i], 0);
    }
    e->tex_pipe = make_pipeline(e->dev, BGPU_VERTEX_2D, BGPU_BLEND_PREMULTIPLIED,
                                BGPU_PIPE_TEXTURED);
    e->mesh_pipe = make_pipeline(e->dev, BGPU_VERTEX_MESH, BGPU_BLEND_PREMULTIPLIED,
                                 BGPU_PIPE_TEXTURED);

    /* 索引网格：33×33 顶点网格，32×32 个格子，每格两个三角形（共享边） */
    const uint32_t vcount = (MESH_CELLS + 1u) * (MESH_CELLS + 1u);
    const uint32_t icount = MESH_CELLS * MESH_CELLS * 6u;
    const uint32_t vsize = (uint32_t)sizeof(BgpuMeshVertex);
    const float step = (float)TARGET_W / (float)MESH_CELLS;

    BgpuMeshVertex* verts = (BgpuMeshVertex*)malloc((size_t)vcount * vsize);
    if (!verts) die("malloc(verts)");
    for (uint32_t y = 0; y <= MESH_CELLS; y++) {
        for (uint32_t x = 0; x <= MESH_CELLS; x++) {
            BgpuMeshVertex v;
            memset(&v, 0, sizeof(v));
            v.x = (float)x * step;
            v.y = (float)y * step;
            v.z = 0.5f;
            v.u = (float)x / (float)MESH_CELLS;
            v.v = (float)y / (float)MESH_CELLS;
            v.color = 0xFFFFFFFFu;
            v.nz = 1.0f;
            verts[y * (MESH_CELLS + 1u) + x] = v;
        }
    }
    uint32_t* idx = (uint32_t*)malloc((size_t)icount * 4u);
    if (!idx) die("malloc(idx)");
    uint32_t k = 0;
    for (uint32_t y = 0; y < MESH_CELLS; y++) {
        for (uint32_t x = 0; x < MESH_CELLS; x++) {
            const uint32_t tl = y * (MESH_CELLS + 1u) + x;
            const uint32_t tr = tl + 1u;
            const uint32_t bl = tl + (MESH_CELLS + 1u);
            const uint32_t br = bl + 1u;
            idx[k++] = tl;
            idx[k++] = tr;
            idx[k++] = bl;
            idx[k++] = bl;
            idx[k++] = tr;
            idx[k++] = br;
        }
    }

    BgpuBufferDesc bd;
    memset(&bd, 0, sizeof(bd));
    bd.struct_size = (uint32_t)sizeof(bd);
    bd.size = vcount * vsize;
    bd.usage = BGPU_BUFFER_VERTEX;
    if (bingpu_buffer_create(e->dev, &bd, &e->mesh_vb) != BGPU_OK) die("buffer_create(vb)");
    if (bingpu_buffer_update(e->mesh_vb, 0, verts, bd.size) != BGPU_OK) die("buffer_update(vb)");
    bd.size = icount * 4u;
    bd.usage = BGPU_BUFFER_INDEX;
    if (bingpu_buffer_create(e->dev, &bd, &e->mesh_ib) != BGPU_OK) die("buffer_create(ib)");
    if (bingpu_buffer_update(e->mesh_ib, 0, idx, bd.size) != BGPU_OK) die("buffer_update(ib)");
    free(verts);
    free(idx);

    e->readback = (uint8_t*)malloc((size_t)TARGET_W * TARGET_H * 4u);
    if (!e->readback) die("malloc(readback)");
}

static void env_shutdown(Env* e) {
    free(e->readback);
    bingpu_cmd_destroy(e->dev, e->cmd);
    bingpu_texture_destroy(e->dev, e->rt);
    bingpu_texture_destroy(e->dev, e->tex);
    bingpu_sampler_destroy(e->dev, e->near_rep);
    bingpu_sampler_destroy(e->dev, e->near_clamp);
    bingpu_sampler_destroy(e->dev, e->lin_rep);
    bingpu_sampler_destroy(e->dev, e->lin_clamp);
    for (int i = 0; i < 5; i++) bingpu_pipeline_destroy(e->dev, e->solid[i]);
    bingpu_pipeline_destroy(e->dev, e->tex_pipe);
    bingpu_pipeline_destroy(e->dev, e->mesh_pipe);
    bingpu_buffer_destroy(e->dev, e->mesh_vb);
    bingpu_buffer_destroy(e->dev, e->mesh_ib);
    bingpu_swapchain_destroy(e->dev, e->sc);
    bingpu_device_destroy(e->dev);
}

/* ---------------- 场景（arg 用来选变体，保持 SceneFn 一种签名） ---------------- */
typedef void (*SceneFn)(Env* e, int arg);

static void begin_rt(Env* e, uint32_t flags, uint32_t clear) {
    BgpuRenderBeginDesc rb;
    memset(&rb, 0, sizeof(rb));
    rb.struct_size = (uint32_t)sizeof(rb);
    rb.target = e->rt;
    rb.flags = flags;
    rb.clear_color = clear;
    if (bingpu_cmd_begin_render(e->cmd, &rb) != BGPU_OK) die("begin_render");
}

/* ① 纯色铺满：五个混合模式各一组 */
static void scene_solid(Env* e, int arg) {
    set_quad(e, 0.0f, 0.0f, (float)TARGET_W, (float)TARGET_H, 1.0f, 1.0f, 0xFFC04040u);
    begin_rt(e, 0, 0);
    if (bingpu_cmd_set_pipeline(e->cmd, e->solid[arg]) != BGPU_OK) die("pipeline");
    if (bingpu_cmd_draw_quads(e->cmd, e->quad, 1) != BGPU_OK) die("draw_quads");
    if (bingpu_cmd_end_render(e->cmd) != BGPU_OK) die("end_render");
}

/* ② 纹理铺满：0=nearest+repeat 1=nearest+clamp 2=linear+repeat 3=linear+clamp */
static BgpuSampler pick_sampler(Env* e, int arg) {
    switch (arg) {
        case 0: return e->near_rep;
        case 1: return e->near_clamp;
        case 2: return e->lin_rep;
        default: return e->lin_clamp;
    }
}

static void scene_textured(Env* e, int arg) {
    /* UV 放大 3 倍：REPEAT 真的绕回、CLAMP 真的夹边；而且 3.0（不是 4.0）能让
     * 像素中心**不**落在纹素中心上 —— 否则 u*256-0.5 正好是整数，双线性的权重
     * 恒为 0，等于退化成最近邻，两种 filter 的 checksum 会一模一样。 */
    set_quad(e, 0.0f, 0.0f, (float)TARGET_W, (float)TARGET_H, 3.0f, 3.0f, 0xFFFFFFFFu);
    begin_rt(e, 0, 0);
    if (bingpu_cmd_set_pipeline(e->cmd, e->tex_pipe) != BGPU_OK) die("pipeline");
    if (bingpu_cmd_set_texture(e->cmd, e->tex, pick_sampler(e, arg)) != BGPU_OK)
        die("set_texture");
    if (bingpu_cmd_draw_quads(e->cmd, e->quad, 1) != BGPU_OK) die("draw_quads");
    if (bingpu_cmd_end_render(e->cmd) != BGPU_OK) die("end_render");
    set_quad(e, 0.0f, 0.0f, (float)TARGET_W, (float)TARGET_H, 1.0f, 1.0f, 0xFFC04040u);
}

/* ③ scissor 内小批：100 个 64×64 四边形，批处理与裁剪路径一起压 */
static void scene_scissor(Env* e, int arg) {
    (void)arg;
    begin_rt(e, 0, 0);
    if (bingpu_cmd_set_scissor(e->cmd, 0, 0, 800, 800) != BGPU_OK) die("scissor");
    if (bingpu_cmd_set_pipeline(e->cmd, e->solid[0]) != BGPU_OK) die("pipeline");
    for (uint32_t i = 0; i < SCISSOR_QUADS; i++) {
        const float x = (float)((i % 10u) * 80u);
        const float y = (float)((i / 10u) * 80u);
        set_quad(e, x, y, x + SMALL_QUAD, y + SMALL_QUAD, 1.0f, 1.0f, 0xFF60C060u);
        if (bingpu_cmd_draw_quads(e->cmd, e->quad, 1) != BGPU_OK) die("draw_quads");
    }
    if (bingpu_cmd_set_scissor(e->cmd, 0, 0, -1, -1) != BGPU_OK) die("scissor(reset)");
    if (bingpu_cmd_end_render(e->cmd) != BGPU_OK) die("end_render");
    set_quad(e, 0.0f, 0.0f, (float)TARGET_W, (float)TARGET_H, 1.0f, 1.0f, 0xFFC04040u);
}

/* ④ 索引网格：1024 quad / 2048 tri，纹理 + 双线性 */
static void scene_mesh(Env* e, int arg) {
    (void)arg;
    begin_rt(e, 0, 0);
    if (bingpu_cmd_set_pipeline(e->cmd, e->mesh_pipe) != BGPU_OK) die("pipeline");
    if (bingpu_cmd_set_texture(e->cmd, e->tex, e->lin_clamp) != BGPU_OK) die("set_texture");
    if (bingpu_cmd_draw_indexed(e->cmd, e->mesh_vb, 0, e->mesh_ib,
                                MESH_CELLS * MESH_CELLS * 6u, 0) != BGPU_OK)
        die("draw_indexed");
    if (bingpu_cmd_end_render(e->cmd) != BGPU_OK) die("end_render");
}

/* ⑤ 离屏 + 合成：把离屏目标当纹理画到交换链（含 frame_end 的 back→front 拷贝） */
static void scene_composite(Env* e, int arg) {
    (void)arg;
    BgpuTexture target = BGPU_INVALID;
    if (bingpu_frame_begin(e->sc, e->cmd, &target) != BGPU_OK) die("frame_begin");
    BgpuRenderBeginDesc rb;
    memset(&rb, 0, sizeof(rb));
    rb.struct_size = (uint32_t)sizeof(rb);
    rb.target = target;
    rb.flags = BGPU_RENDER_CLEAR_COLOR;
    rb.clear_color = 0xFF000000u;
    if (bingpu_cmd_begin_render(e->cmd, &rb) != BGPU_OK) die("begin_render");
    if (bingpu_cmd_set_pipeline(e->cmd, e->tex_pipe) != BGPU_OK) die("pipeline");
    if (bingpu_cmd_set_texture(e->cmd, e->rt, e->near_clamp) != BGPU_OK) die("set_texture");
    set_quad(e, 0.0f, 0.0f, (float)TARGET_W, (float)TARGET_H, 1.0f, 1.0f, 0xFFFFFFFFu);
    if (bingpu_cmd_draw_quads(e->cmd, e->quad, 1) != BGPU_OK) die("draw_quads");
    if (bingpu_cmd_end_render(e->cmd) != BGPU_OK) die("end_render");
    if (bingpu_frame_end(e->sc, e->cmd) != BGPU_OK) die("frame_end");
    set_quad(e, 0.0f, 0.0f, (float)TARGET_W, (float)TARGET_H, 1.0f, 1.0f, 0xFFC04040u);
    (void)target;
}

/* ⑥ 回读：texture_read（含反预乘） */
static void scene_readback(Env* e, int arg) {
    (void)arg;
    uint32_t stride = 0;
    if (bingpu_texture_read(e->rt, e->readback, (size_t)TARGET_W * TARGET_H * 4u, &stride) !=
        BGPU_OK)
        die("texture_read");
    (void)stride;
}

/* ---------------- 计量 ---------------- */
typedef struct {
    char name[96];
    double ms;
    double mpix;
    double mquad;
} Row;

static Row g_rows[32];
static int g_row_count = 0;

static void measure(const char* name, double pix, double quads, SceneFn fn, Env* e, int arg) {
    fn(e, arg); /* 预热：第一次分配/缺页落在计时之外 */
    double best = 1e30;
    for (int i = 0; i < ITERATIONS; i++) {
        const double t0 = now_ms();
        fn(e, arg);
        const double t1 = now_ms();
        if (t1 - t0 < best) best = t1 - t0;
    }
    Row* r = &g_rows[g_row_count++];
    snprintf(r->name, sizeof(r->name), "%s", name);
    r->ms = best;
    r->mpix = best > 0.0 ? pix / (best / 1000.0) / 1e6 : 0.0;
    r->mquad = best > 0.0 ? quads / (best / 1000.0) / 1e6 : 0.0;
}

/* 校验和：清一遍 → 画一遍 → 回读，对回读字节做 FNV-1a。
 * 与计时分开：回读本身有开销，不能混进吞吐数字。 */
static void clear_rt(Env* e) {
    begin_rt(e, BGPU_RENDER_CLEAR_COLOR, 0xFF101010u);
    if (bingpu_cmd_end_render(e->cmd) != BGPU_OK) die("end_render(clear)");
}

static uint64_t checksum_rt(SceneFn fn, Env* e, int arg) {
    clear_rt(e);
    fn(e, arg);
    scene_readback(e, 0);
    return fnv1a(e->readback, (size_t)TARGET_W * TARGET_H * 4u);
}

static uint64_t checksum_sc(SceneFn fn, Env* e, int arg) {
    fn(e, arg);
    if (bingpu_frame_read(e->sc, e->readback, (size_t)TARGET_W * TARGET_H * 4u, NULL) != BGPU_OK)
        die("frame_read");
    return fnv1a(e->readback, (size_t)TARGET_W * TARGET_H * 4u);
}

static void print_checksums(Env* e) {
    static const char* kBlendNames[5] = {"premultiplied", "replace", "keep", "erase",
                                         "additive"};
    static const char* kSampNames[4] = {"nearest+repeat", "nearest+clamp", "linear+repeat",
                                        "linear+clamp"};
    char name[96];
    for (int i = 0; i < 5; i++) {
        snprintf(name, sizeof(name), "solid/%-14s", kBlendNames[i]);
        printf("CHECKSUM %-40s %016llX\n", name,
               (unsigned long long)checksum_rt(scene_solid, e, i));
    }
    for (int i = 0; i < 4; i++) {
        snprintf(name, sizeof(name), "tex/%-16s", kSampNames[i]);
        printf("CHECKSUM %-40s %016llX\n", name,
               (unsigned long long)checksum_rt(scene_textured, e, i));
    }
    printf("CHECKSUM %-40s %016llX\n", "scissor/100x64",
           (unsigned long long)checksum_rt(scene_scissor, e, 0));
    printf("CHECKSUM %-40s %016llX\n", "mesh/32x32",
           (unsigned long long)checksum_rt(scene_mesh, e, 0));
    printf("CHECKSUM %-40s %016llX\n", "composite",
           (unsigned long long)checksum_sc(scene_composite, e, 0));
    printf("CHECKSUM %-40s %016llX\n", "readback",
           (unsigned long long)checksum_rt(scene_solid, e, 0));
}

int main(int argc, char** argv) {
    int only_checksum = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--checksum") == 0) {
            only_checksum = 1;
        } else if (strcmp(argv[i], "--backend") == 0 && i + 1 < argc) {
            const char* want = argv[++i];
            g_backend = BGPU_BACKEND_AUTO;
            for (uint32_t b = 0; b < bingpu_backend_count(); b++) {
                const BgpuBackendKind k = bingpu_backend_at(b);
                if (strcmp(bingpu_backend_name(k), want) == 0) {
                    g_backend = k;
                    break;
                }
            }
            if (g_backend == BGPU_BACKEND_AUTO) {
                printf("未知后端 '%s'（可用：vulkan / gl / d3d11 / cpu / null）\n", want);
                return 2;
            }
        } else if (strcmp(argv[i], "--msaa") == 0 && i + 1 < argc) {
            g_msaa = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else {
            printf("用法：bingpu_bench [--checksum] [--backend vulkan|gl|d3d11|cpu|null]"
                   " [--msaa 1|4|8]\n");
            return 2;
        }
    }

    Env e;
    env_init(&e);

    printf("binGPU 基准（版本 %s，ABI %u，后端 %s，msaa=%u，目标 %ux%u）\n",
           bingpu_version_string(), bingpu_abi_version(), bingpu_backend_name(g_backend),
           g_msaa ? g_msaa : 1, TARGET_W, TARGET_H);

    if (only_checksum) {
        print_checksums(&e);
    } else {
        static const char* kBlendNames[5] = {"premultiplied", "replace", "keep", "erase",
                                             "additive"};
        static const char* kSampNames[4] = {"nearest+repeat", "nearest+clamp", "linear+repeat",
                                            "linear+clamp"};
        char name[96];
        const double full = (double)TARGET_W * TARGET_H;

        printf("\n%-36s %8s %10s %10s\n", "场景", "ms", "MPix/s", "MQuad/s");
        printf("--------------------------------------------------------------------\n");

        for (int i = 0; i < 5; i++) {
            snprintf(name, sizeof(name), "① 纯色 1024² · %s", kBlendNames[i]);
            measure(name, full, 1.0, scene_solid, &e, i);
        }
        for (int i = 0; i < 4; i++) {
            snprintf(name, sizeof(name), "② 纹理 1024² UV×4 · %s", kSampNames[i]);
            measure(name, full, 1.0, scene_textured, &e, i);
        }
        snprintf(name, sizeof(name), "③ scissor 内 %u×%u²", SCISSOR_QUADS, SMALL_QUAD);
        measure(name, (double)SCISSOR_QUADS * SMALL_QUAD * SMALL_QUAD, (double)SCISSOR_QUADS,
                scene_scissor, &e, 0);
        snprintf(name, sizeof(name), "④ 索引网格 %u×%u", MESH_CELLS, MESH_CELLS);
        measure(name, full, (double)MESH_CELLS * MESH_CELLS, scene_mesh, &e, 0);
        measure("⑤ 离屏 + 合成（含 present 拷贝）", full, 1.0, scene_composite, &e, 0);
        measure("⑥ texture_read 1024²", full, 0.0, scene_readback, &e, 0);

        for (int i = 0; i < g_row_count; i++) {
            const Row* r = &g_rows[i];
            printf("%-36s %8.3f %10.1f %10.1f\n", r->name, r->ms, r->mpix, r->mquad);
        }

        printf("\n");
        print_checksums(&e);
    }

    env_shutdown(&e);
    if (g_failures) {
        printf("%d 项失败\n", g_failures);
        return 1;
    }
    return 0;
}
