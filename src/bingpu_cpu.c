/* bingpu_cpu.c —— CPU 软件参考实现（整个项目的像素裁判）
 *
 * 定位：它是"同一份绘制指令应该得到什么像素"的**唯一权威**。GPU 后端写完之后
 * 拿它的输出做逐像素比，差异就是 GPU 后端的 bug 或"规则没对齐"。所以这里的三条
 * 目标是按优先级排的：
 *   1. 确定性：同样的输入在任何机器/任何编译优化级别下逐字节相同 ——
 *      所以像素运算用整数（预乘/反预乘）或 IEEE 单精度（插值/混合），不用
 *      平台相关的浮点技巧，加法顺序固定；
 *   2. 规则明确：每条规则都写进 docs/raster_rules.md，GPU 后端逐条对齐；
 *   3. 简单：一个 for 循环扫三角形，没有 tile、没有 SIMD、没有线程 ——
 *      慢一点无所谓（它是出图和 CI 用的，不是实时窗口）。
 *
 * 存储模型（最容易错的地方，先钉死）：
 *   · 纹理与渲染目标**内部都存预乘 alpha**的 RGBA8（与 GPU 的 framebuffer 一致）；
 *   · upload 进来的是**直通**像素 → 立刻预乘；readback 出去一律反预乘（直通）；
 *   · 帧缓冲不清成"透明黑"以外的任何东西都由调用方决定（CLEAR_COLOR 才清）。
 *   因此非 255 alpha 的"上传→读回"往返会有 ±1 的量化误差（8 位存不下两次除法），
 *   这是规格的一部分，不是 bug —— 见 docs/abi.md。
 */
#include "bingpu_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct CpuDevice CpuDevice;

/* ---------------------------------------------------------------
 * 对象
 * --------------------------------------------------------------- */
typedef struct CpuTexture {
    BgpuObjectBase base;
    CpuDevice* dev;
    uint32_t w, h;
    BgpuFormat fmt;
    uint32_t tex_flags;
    uint32_t mip_levels;     /* CPU 后端固定 1（采样不用 mip，见 docs/backends.md） */
    uint8_t* px;             /* 预乘存储；借用的帧目标指向交换链 back，**不拥有** */
    int borrowed;
    /* 作为深度目标时懒分配的 32 位浮点深度（尺寸跟着当次渲染目标）； */
    float* depth;
    uint32_t depth_w, depth_h;
    uint64_t bytes;
} CpuTexture;

typedef struct CpuSwapchain {
    BgpuObjectBase base;
    CpuDevice* dev;
    uint32_t w, h;
    BgpuFormat fmt;
    uint32_t flags;
    uint8_t* back;           /* 当前帧的目标（frame_begin 借出的纹理别名它） */
    uint8_t* front;          /* 上一帧呈现结果（frame_read 读它） */
    BgpuTexture target;
    uint64_t frames;
} CpuSwapchain;

typedef struct CpuBuffer {
    BgpuObjectBase base;
    CpuDevice* dev;
    uint8_t* data;
    uint32_t size;
    uint32_t usage;
    uint64_t bytes;
} CpuBuffer;

typedef struct CpuSampler {
    BgpuObjectBase base;
    CpuDevice* dev;
    BgpuSamplerDesc desc;
} CpuSampler;

typedef struct CpuPipeline {
    BgpuObjectBase base;
    CpuDevice* dev;
    BgpuVertexLayout layout;
    BgpuBlend blend;
    uint32_t pipe_flags;
} CpuPipeline;

typedef struct CpuCmd {
    BgpuObjectBase base;
    CpuDevice* dev;
    int in_frame;
    CpuSwapchain* frame;
    int in_render;
    CpuTexture* target;
    CpuTexture* depth;
    CpuPipeline* pipe;
    CpuTexture* tex;
    CpuSampler* samp;
    float vp[4];
    int scissor_on;
    int32_t sc[4];
    int affine_on;
    float xf[6];
} CpuCmd;

struct CpuDevice {
    BgpuObjectBase base;
    BgpuDevice self;   /* 自己的句柄：资源 owner 用它，不能用 base.device（那是 0） */
    uint32_t flags;
    uint32_t w, h;
    BgpuFormat surface_format;
    uint64_t frames_presented, draws, quads, tris;
    uint64_t bytes_uploaded, bytes_live;
    uint32_t frame_index;
};

/* ---------------------------------------------------------------
 * 8 位像素小工具：预乘/反预乘用整数（可复现），插值/混合用 float
 * --------------------------------------------------------------- */
static inline uint8_t clamp_u8(int v) {
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

/* 乘 alpha：round(c*a/255) —— 用 +127 的整数四舍五入，跨平台一致 */
static inline uint8_t premul8(uint32_t c, uint32_t a) {
    return (uint8_t)((c * a + 127u) / 255u);
}

/* 反预乘：round(p*255/a)；a==0 时按 0 处理（透明像素没有颜色信息） */
static inline uint8_t unpremul8(uint32_t p, uint32_t a) {
    if (a == 0) return 0;
    const uint32_t v = (p * 255u + a / 2u) / a;
    return (uint8_t)(v > 255u ? 255u : v);
}

static inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline uint8_t q8(float v) {
    const int i = (int)(v * 255.0f + 0.5f);
    return (uint8_t)(i < 0 ? 0 : (i > 255 ? 255 : i));
}

/* 通道序：RGBA8 内存里就是 R,G,B,A；BGRA8 是 B,G,R,A。
 * 内部一律按"逻辑 r,g,b,a"读写，换序只在这里发生 —— 少一处手写字节序的地方。 */
static inline void px_store(uint8_t* p, BgpuFormat fmt, const float c[4]) {
    const uint8_t r = q8(c[0]), g = q8(c[1]), b = q8(c[2]), a = q8(c[3]);
    if (fmt == BGPU_FORMAT_BGRA8) {
        p[0] = b;
        p[1] = g;
        p[2] = r;
        p[3] = a;
    } else {
        p[0] = r;
        p[1] = g;
        p[2] = b;
        p[3] = a;
    }
}

static inline void px_load(const uint8_t* p, BgpuFormat fmt, float c[4]) {
    if (fmt == BGPU_FORMAT_BGRA8) {
        c[0] = p[2] / 255.0f;
        c[1] = p[1] / 255.0f;
        c[2] = p[0] / 255.0f;
        c[3] = p[3] / 255.0f;
    } else {
        c[0] = p[0] / 255.0f;
        c[1] = p[1] / 255.0f;
        c[2] = p[2] / 255.0f;
        c[3] = p[3] / 255.0f;
    }
}

/* 把直通颜色写进预乘存储（清除/上传共用） */
static inline void store_straight(uint8_t* p, BgpuFormat fmt, uint32_t argb) {
    const uint32_t a = (argb >> 24) & 0xFFu;
    float c[4];
    c[0] = premul8((argb >> 16) & 0xFFu, a) / 255.0f;
    c[1] = premul8((argb >> 8) & 0xFFu, a) / 255.0f;
    c[2] = premul8(argb & 0xFFu, a) / 255.0f;
    c[3] = a / 255.0f;
    px_store(p, fmt, c);
}

static inline uint32_t bytes_per_px(BgpuFormat fmt) { return bingpu_format_bytes(fmt); }

/* ---------------------------------------------------------------
 * 析构
 * --------------------------------------------------------------- */
static void cpu_device_dtor(void* p) { free(p); }

static void cpu_texture_dtor(void* p) {
    CpuTexture* t = (CpuTexture*)p;
    if (t->dev) t->dev->bytes_live -= t->bytes;
    if (!t->borrowed) free(t->px);
    free(t->depth);
    free(t);
}

static void cpu_buffer_dtor(void* p) {
    CpuBuffer* b = (CpuBuffer*)p;
    if (b->dev) b->dev->bytes_live -= b->bytes;
    free(b->data);
    free(b);
}

static void cpu_swapchain_dtor(void* p) {
    CpuSwapchain* s = (CpuSwapchain*)p;
    if (s->dev) s->dev->bytes_live -= (uint64_t)s->w * s->h * bytes_per_px(s->fmt) * 2u;
    free(s->back);
    free(s->front);
    free(s);
}

static void cpu_sampler_dtor(void* p) { free(p); }
static void cpu_pipeline_dtor(void* p) { free(p); }
static void cpu_cmd_dtor(void* p) { free(p); }

/* ---------------------------------------------------------------
 * 设备
 * --------------------------------------------------------------- */
static int cpu_available(void) { return 1; }

static BgpuResult cpu_device_create(const BgpuDeviceDesc* desc, BgpuDevice* out_device) {
    const BgpuDevice handle = bgpu_obj_new(BGPU_OBJ_DEVICE, BGPU_BACKEND_CPU, BGPU_INVALID,
                                           sizeof(CpuDevice), cpu_device_dtor);
    if (handle == BGPU_INVALID) {
        bgpu_set_reason("对象表分配失败（内存不足）");
        return BGPU_ERR_OUT_OF_MEMORY;
    }
    CpuDevice* d = (CpuDevice*)bgpu_obj_get(handle, BGPU_OBJ_DEVICE);
    d->self = handle;
    d->flags = desc->flags;
    d->w = desc->width;
    d->h = desc->height;
    d->surface_format = BGPU_FORMAT_RGBA8;
    (void)desc->native_window;   /* 窗口由 VK/D3D12 负责；CPU 只在 HEADLESS 下工作 */
    *out_device = handle;
    return BGPU_OK;
}

static void cpu_device_info(void* dev, BgpuDeviceInfo* out) {
    CpuDevice* d = (CpuDevice*)dev;
    out->backend = BGPU_BACKEND_CPU;
    out->surface_format = d->surface_format;
    out->api_version = 1;   /* CPU 参考实现第 1 版，仅上报 */
    snprintf(out->adapter_name, sizeof(out->adapter_name), "%s", "binGPU CPU reference");
    snprintf(out->driver_name, sizeof(out->driver_name), "%s", "scalar C11");
}

static void cpu_device_caps(void* dev, BgpuCaps* out) {
    (void)dev;
    /* 如实上报：CPU 后端能做深度/离屏/回读/采样器/仿射/网格，但没有 MSAA、
     * 没有垂直同步、不呈现到窗口 —— 所以那三个 cap 位故意不置。 */
    out->flags = BGPU_CAP_DEPTH | BGPU_CAP_OFFSCREEN | BGPU_CAP_READBACK |
                 BGPU_CAP_SAMPLERS | BGPU_CAP_AFFINE | BGPU_CAP_MESH;
    out->max_texture_size = BGPU_MAX_TEXTURE_SIZE;
    out->max_frames_in_flight = 1;
    out->msaa_samples = 1;
    out->max_quads_per_draw = BGPU_MAX_QUADS_PER_DRAW;
}

static void cpu_device_stats(void* dev, BgpuStats* out) {
    CpuDevice* d = (CpuDevice*)dev;
    out->frame_index = d->frame_index;
    out->frames_presented = d->frames_presented;
    out->draws = d->draws;
    out->quads = d->quads;
    out->triangles = d->tris;
    out->bytes_uploaded = d->bytes_uploaded;
    out->bytes_live = d->bytes_live;
}

static int cpu_device_lost(void* dev) {
    (void)dev;
    return 0;   /* 软件实现没有"设备丢了"这回事 */
}

/* ---------------------------------------------------------------
 * 交换链 / 帧
 * --------------------------------------------------------------- */
static BgpuFormat pick_surface_format(BgpuFormat want) {
    /* 只支持两种 8 位格式；其它（R8）退回 RGBA8 —— 交换链是画布，不是遮罩。 */
    return (want == BGPU_FORMAT_BGRA8) ? BGPU_FORMAT_BGRA8 : BGPU_FORMAT_RGBA8;
}

static uint8_t* alloc_bitmap(uint32_t w, uint32_t h, BgpuFormat fmt) {
    return (uint8_t*)calloc((size_t)w * h * bytes_per_px(fmt), 1);
}

static BgpuResult cpu_swapchain_create(void* dev, const BgpuSwapchainDesc* desc,
                                       BgpuSwapchain* out) {
    CpuDevice* d = (CpuDevice*)dev;
    if (desc->native_window != NULL) {
        /* 真窗口留给 VK/D3D12：CPU 后端要往 HWND 上贴一帧就得自己走 GDI，
         * 那会把"参考实现"和"窗口/DPI/合成器"这些不可复现的东西绑在一起。 */
        bgpu_set_reason("CPU 后端只做 HEADLESS（native_window 必须为 NULL）；"
                        "窗口呈现请用 Vulkan/D3D12 后端");
        return BGPU_ERR_UNSUPPORTED;
    }
    const BgpuSwapchain handle = bgpu_obj_new(BGPU_OBJ_SWAPCHAIN, BGPU_BACKEND_CPU,
                                              d->self, sizeof(CpuSwapchain),
                                              cpu_swapchain_dtor);
    if (handle == BGPU_INVALID) return BGPU_ERR_OUT_OF_MEMORY;
    CpuSwapchain* sc = (CpuSwapchain*)bgpu_obj_get(handle, BGPU_OBJ_SWAPCHAIN);
    sc->dev = d;
    sc->w = desc->width;
    sc->h = desc->height;
    sc->fmt = pick_surface_format(desc->format);
    sc->flags = desc->flags;
    sc->back = alloc_bitmap(sc->w, sc->h, sc->fmt);
    sc->front = alloc_bitmap(sc->w, sc->h, sc->fmt);
    if (!sc->back || !sc->front) {
        bgpu_obj_destroy(handle);
        return BGPU_ERR_OUT_OF_MEMORY;
    }
    d->bytes_live += (uint64_t)sc->w * sc->h * bytes_per_px(sc->fmt) * 2u;

    const BgpuTexture t = bgpu_obj_new(BGPU_OBJ_TEXTURE, BGPU_BACKEND_CPU, d->self,
                                       sizeof(CpuTexture), cpu_texture_dtor);
    if (t == BGPU_INVALID) {
        bgpu_obj_destroy(handle);
        return BGPU_ERR_OUT_OF_MEMORY;
    }
    CpuTexture* nt = (CpuTexture*)bgpu_obj_get(t, BGPU_OBJ_TEXTURE);
    nt->dev = d;
    nt->w = sc->w;
    nt->h = sc->h;
    nt->fmt = sc->fmt;
    nt->tex_flags = BGPU_TEXTURE_RENDER_TARGET;
    nt->mip_levels = 1;
    nt->px = sc->back;      /* 别名：不拥有，换尺寸时由 resize 重新指向 */
    nt->borrowed = 1;
    nt->base.obj_flags |= BGPU_OBJ_F_BORROWED;
    sc->target = t;
    *out = handle;
    return BGPU_OK;
}

static BgpuResult cpu_swapchain_resize(void* sc_, uint32_t w, uint32_t h) {
    CpuSwapchain* sc = (CpuSwapchain*)sc_;
    CpuDevice* d = sc->dev;
    uint8_t* nb = alloc_bitmap(w, h, sc->fmt);
    uint8_t* nf = alloc_bitmap(w, h, sc->fmt);
    if (!nb || !nf) {
        free(nb);
        free(nf);
        bgpu_set_reason("交换链 resize 分配 %ux%u 失败", w, h);
        return BGPU_ERR_OUT_OF_MEMORY;
    }
    d->bytes_live -= (uint64_t)sc->w * sc->h * bytes_per_px(sc->fmt) * 2u;
    free(sc->back);
    free(sc->front);
    sc->back = nb;
    sc->front = nf;
    sc->w = w;
    sc->h = h;
    d->bytes_live += (uint64_t)w * h * bytes_per_px(sc->fmt) * 2u;

    CpuTexture* t = (CpuTexture*)bgpu_obj_get(sc->target, BGPU_OBJ_TEXTURE);
    if (t) {
        t->w = w;
        t->h = h;
        t->px = sc->back;
    }
    return BGPU_OK;
}

static void cpu_swapchain_info(void* sc_, BgpuSwapchainInfo* out) {
    CpuSwapchain* sc = (CpuSwapchain*)sc_;
    out->width = sc->w;
    out->height = sc->h;
    out->format = sc->fmt;
    out->image_count = 1;          /* 单缓冲：frame_end 就是 memcpy，没有队列 */
    out->msaa_samples = 1;
    out->present_paces_frames = 0; /* 不阻塞；外壳自己按刷新率节流 */
}

static BgpuResult cpu_frame_begin(void* sc_, void* cmd_, BgpuTexture* out_target) {
    CpuSwapchain* sc = (CpuSwapchain*)sc_;
    CpuCmd* c = (CpuCmd*)cmd_;
    if (c->in_frame) {
        bgpu_set_reason("cmd 已经在一个帧里（先 frame_end）");
        return BGPU_ERR_STATE;
    }
    c->in_frame = 1;
    c->frame = sc;
    *out_target = sc->target;
    return BGPU_OK;
}

static BgpuResult cpu_frame_end(void* sc_, void* cmd_) {
    CpuSwapchain* sc = (CpuSwapchain*)sc_;
    CpuCmd* c = (CpuCmd*)cmd_;
    if (!c->in_frame || c->frame != sc) {
        bgpu_set_reason("这个 cmd 没抓着这个交换链的帧（frame_begin 呢？）");
        return BGPU_ERR_STATE;
    }
    if (c->in_render) {
        bgpu_set_reason("还在 begin_render/end_render 之间，先 end_render");
        return BGPU_ERR_STATE;
    }
    c->in_frame = 0;
    c->frame = NULL;
    /* "呈现"= back 拷进 front。单缓冲 CPU 后端没有队列，这一步是同步的；
     * frame_read 读 front，所以"上一帧"的语义与 GPU 后端一致。 */
    memcpy(sc->front, sc->back, (size_t)sc->w * sc->h * bytes_per_px(sc->fmt));
    sc->frames++;
    sc->dev->frames_presented++;
    sc->dev->frame_index++;
    return BGPU_OK;
}

/* 反预乘到直通 RGBA8（frame_read 的输出格式永远是 RGBA8，与交换链格式无关） */
static void copy_out_straight_rgba8(const uint8_t* src, BgpuFormat fmt, uint32_t w, uint32_t h,
                                    uint8_t* dst) {
    const uint32_t bpp = bytes_per_px(fmt);
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t* p = src + ((size_t)y * w + x) * bpp;
            uint8_t r, g, b, a;
            if (fmt == BGPU_FORMAT_BGRA8) {
                b = p[0];
                g = p[1];
                r = p[2];
                a = p[3];
            } else {
                r = p[0];
                g = p[1];
                b = p[2];
                a = p[3];
            }
            uint8_t* o = dst + ((size_t)y * w + x) * 4;
            o[0] = unpremul8(r, a);
            o[1] = unpremul8(g, a);
            o[2] = unpremul8(b, a);
            o[3] = a;
        }
    }
}

static BgpuResult cpu_frame_read(void* sc_, void* dst, size_t dst_size, uint32_t* out_stride) {
    CpuSwapchain* sc = (CpuSwapchain*)sc_;
    const uint64_t need = (uint64_t)sc->w * sc->h * 4u;
    if (dst_size < need) {
        bgpu_set_reason("dst_size=%zu 不够（%ux%u RGBA8 需要 %llu 字节）", dst_size, sc->w,
                        sc->h, (unsigned long long)need);
        return BGPU_ERR_INVALID_ARGUMENT;
    }
    copy_out_straight_rgba8(sc->front, sc->fmt, sc->w, sc->h, (uint8_t*)dst);
    if (out_stride) *out_stride = sc->w * 4u;
    return BGPU_OK;
}

/* ---------------------------------------------------------------
 * 纹理
 * --------------------------------------------------------------- */
static BgpuResult cpu_texture_create(void* dev_, const BgpuTextureDesc* desc,
                                     BgpuTexture* out) {
    CpuDevice* d = (CpuDevice*)dev_;
    const BgpuTexture handle = bgpu_obj_new(BGPU_OBJ_TEXTURE, BGPU_BACKEND_CPU,
                                            d->self, sizeof(CpuTexture),
                                            cpu_texture_dtor);
    if (handle == BGPU_INVALID) return BGPU_ERR_OUT_OF_MEMORY;
    CpuTexture* t = (CpuTexture*)bgpu_obj_get(handle, BGPU_OBJ_TEXTURE);
    t->dev = d;
    t->w = desc->width;
    t->h = desc->height;
    t->fmt = desc->format;
    t->tex_flags = desc->flags;
    /* mip：0.1.0 的固定管线没有 LOD 输入，采样永远取第 0 级，所以 CPU 只存一张图。
     * 调用方要 MIPMAPS 时**接受但降级**（与 MSAA 降级同一套说法），info 上报真实值。 */
    t->mip_levels = 1;
    t->bytes = (uint64_t)t->w * t->h * bytes_per_px(t->fmt);
    t->px = (uint8_t*)calloc((size_t)t->bytes, 1);
    if (!t->px) {
        bgpu_obj_destroy(handle);
        return BGPU_ERR_OUT_OF_MEMORY;
    }
    d->bytes_live += t->bytes;
    *out = handle;
    return BGPU_OK;
}

static void cpu_texture_info(void* tex_, BgpuTextureInfo* out) {
    CpuTexture* t = (CpuTexture*)tex_;
    out->width = t->w;
    out->height = t->h;
    out->format = t->fmt;
    out->flags = t->tex_flags;
    out->mip_levels = t->mip_levels;
}

static BgpuResult cpu_texture_upload(void* tex_, const BgpuBox* box, const void* pixels,
                                     uint32_t stride) {
    CpuTexture* t = (CpuTexture*)tex_;
    const uint32_t bpp = bytes_per_px(t->fmt);
    const uint32_t x0 = box ? box->x : 0u;
    const uint32_t y0 = box ? box->y : 0u;
    const uint32_t w = box ? box->w : t->w;
    const uint32_t h = box ? box->h : t->h;
    if (w == 0 || h == 0 || x0 + w > t->w || y0 + h > t->h) {
        bgpu_set_reason("上传区域 %u,%u %ux%u 超出纹理 %ux%u", x0, y0, w, h, t->w, t->h);
        return BGPU_ERR_INVALID_ARGUMENT;
    }
    const uint32_t row = stride ? stride : w * bpp;
    if (row < w * bpp) {
        bgpu_set_reason("stride=%u 小于一行 %u 字节", row, w * bpp);
        return BGPU_ERR_INVALID_ARGUMENT;
    }
    const uint8_t* src = (const uint8_t*)pixels;
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t* srow = src + (size_t)y * row;
        uint8_t* drow = t->px + ((size_t)(y0 + y) * t->w + x0) * bpp;
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t* s = srow + (size_t)x * bpp;
            uint8_t* d = drow + (size_t)x * bpp;
            if (t->fmt == BGPU_FORMAT_R8) {
                d[0] = s[0];   /* 遮罩：原样存，采样时当作 (v,v,v,v) */
            } else if (t->fmt == BGPU_FORMAT_BGRA8) {
                const uint32_t a = s[3];
                d[0] = premul8(s[0], a);
                d[1] = premul8(s[1], a);
                d[2] = premul8(s[2], a);
                d[3] = (uint8_t)a;
            } else {
                const uint32_t a = s[3];
                d[0] = premul8(s[0], a);
                d[1] = premul8(s[1], a);
                d[2] = premul8(s[2], a);
                d[3] = (uint8_t)a;
            }
        }
    }
    t->dev->bytes_uploaded += (uint64_t)row * h;
    return BGPU_OK;
}

static BgpuResult cpu_texture_read(void* tex_, void* dst, size_t dst_size,
                                   uint32_t* out_stride) {
    CpuTexture* t = (CpuTexture*)tex_;
    if (!(t->tex_flags & (BGPU_TEXTURE_CPU_READ | BGPU_TEXTURE_RENDER_TARGET))) {
        bgpu_set_reason("纹理没有 BGPU_TEXTURE_CPU_READ / RENDER_TARGET 标志，不能回读");
        return BGPU_ERR_UNSUPPORTED;
    }
    const uint32_t bpp = bytes_per_px(t->fmt);
    const uint64_t need = (uint64_t)t->w * t->h * bpp;
    if (dst_size < need) {
        bgpu_set_reason("dst_size=%zu 不够（%ux%u 需要 %llu 字节）", dst_size, t->w, t->h,
                        (unsigned long long)need);
        return BGPU_ERR_INVALID_ARGUMENT;
    }
    uint8_t* out = (uint8_t*)dst;
    if (t->fmt == BGPU_FORMAT_R8) {
        memcpy(out, t->px, (size_t)need);
        if (out_stride) *out_stride = t->w;
        return BGPU_OK;
    }
    for (uint32_t y = 0; y < t->h; y++) {
        for (uint32_t x = 0; x < t->w; x++) {
            const uint8_t* p = t->px + ((size_t)y * t->w + x) * bpp;
            uint8_t* o = out + ((size_t)y * t->w + x) * bpp;
            uint8_t r, g, b, a;
            if (t->fmt == BGPU_FORMAT_BGRA8) {
                b = p[0];
                g = p[1];
                r = p[2];
                a = p[3];
            } else {
                r = p[0];
                g = p[1];
                b = p[2];
                a = p[3];
            }
            const uint8_t sr = unpremul8(r, a), sg = unpremul8(g, a), sb = unpremul8(b, a);
            if (t->fmt == BGPU_FORMAT_BGRA8) {
                o[0] = sb;
                o[1] = sg;
                o[2] = sr;
            } else {
                o[0] = sr;
                o[1] = sg;
                o[2] = sb;
            }
            o[3] = a;
        }
    }
    if (out_stride) *out_stride = t->w * bpp;
    return BGPU_OK;
}

/* ---------------------------------------------------------------
 * 缓冲 / 采样器 / 管线
 * --------------------------------------------------------------- */
static BgpuResult cpu_buffer_create(void* dev_, const BgpuBufferDesc* desc, BgpuBuffer* out) {
    CpuDevice* d = (CpuDevice*)dev_;
    const BgpuBuffer handle = bgpu_obj_new(BGPU_OBJ_BUFFER, BGPU_BACKEND_CPU, d->self,
                                           sizeof(CpuBuffer), cpu_buffer_dtor);
    if (handle == BGPU_INVALID) return BGPU_ERR_OUT_OF_MEMORY;
    CpuBuffer* b = (CpuBuffer*)bgpu_obj_get(handle, BGPU_OBJ_BUFFER);
    b->dev = d;
    b->size = desc->size;
    b->usage = desc->usage;
    b->bytes = desc->size;
    b->data = (uint8_t*)calloc(desc->size, 1);
    if (!b->data) {
        bgpu_obj_destroy(handle);
        return BGPU_ERR_OUT_OF_MEMORY;
    }
    d->bytes_live += b->bytes;
    *out = handle;
    return BGPU_OK;
}

static BgpuResult cpu_buffer_update(void* buf_, uint32_t offset, const void* data,
                                    uint32_t size) {
    CpuBuffer* b = (CpuBuffer*)buf_;
    if ((uint64_t)offset + size > b->size) {
        bgpu_set_reason("写入区间 %u+%u 超出缓冲 %u 字节", offset, size, b->size);
        return BGPU_ERR_INVALID_ARGUMENT;
    }
    memcpy(b->data + offset, data, size);
    b->dev->bytes_uploaded += size;
    return BGPU_OK;
}

static BgpuResult cpu_sampler_create(void* dev_, const BgpuSamplerDesc* desc, BgpuSampler* out) {
    CpuDevice* d = (CpuDevice*)dev_;
    const BgpuSampler handle = bgpu_obj_new(BGPU_OBJ_SAMPLER, BGPU_BACKEND_CPU, d->self,
                                            sizeof(CpuSampler), cpu_sampler_dtor);
    if (handle == BGPU_INVALID) return BGPU_ERR_OUT_OF_MEMORY;
    CpuSampler* s = (CpuSampler*)bgpu_obj_get(handle, BGPU_OBJ_SAMPLER);
    s->dev = d;
    s->desc = *desc;
    *out = handle;
    return BGPU_OK;
}

static BgpuResult cpu_pipeline_create(void* dev_, const BgpuPipelineDesc* desc,
                                      BgpuPipeline* out) {
    CpuDevice* d = (CpuDevice*)dev_;
    const BgpuPipeline handle = bgpu_obj_new(BGPU_OBJ_PIPELINE, BGPU_BACKEND_CPU,
                                             d->self, sizeof(CpuPipeline),
                                             cpu_pipeline_dtor);
    if (handle == BGPU_INVALID) return BGPU_ERR_OUT_OF_MEMORY;
    CpuPipeline* p = (CpuPipeline*)bgpu_obj_get(handle, BGPU_OBJ_PIPELINE);
    p->dev = d;
    p->layout = desc->vertex_layout;
    p->blend = desc->blend;
    p->pipe_flags = desc->flags;
    *out = handle;
    return BGPU_OK;
}

static BgpuResult cpu_cmd_create(void* dev_, BgpuCmd* out) {
    CpuDevice* d = (CpuDevice*)dev_;
    const BgpuCmd handle = bgpu_obj_new(BGPU_OBJ_CMD, BGPU_BACKEND_CPU, d->self,
                                        sizeof(CpuCmd), cpu_cmd_dtor);
    if (handle == BGPU_INVALID) return BGPU_ERR_OUT_OF_MEMORY;
    CpuCmd* c = (CpuCmd*)bgpu_obj_get(handle, BGPU_OBJ_CMD);
    c->dev = d;
    c->samp = NULL;
    c->tex = NULL;
    c->pipe = NULL;
    c->vp[0] = 0.0f;
    c->vp[1] = 0.0f;
    c->vp[2] = (float)d->w;
    c->vp[3] = (float)d->h;
    *out = handle;
    return BGPU_OK;
}

/* ---------------------------------------------------------------
 * 采样
 * --------------------------------------------------------------- */
static inline uint32_t wrap_index(int64_t i, uint32_t n, int repeat) {
    if (!repeat) {
        if (i < 0) return 0;
        return (uint32_t)i >= n ? n - 1u : (uint32_t)i;
    }
    int64_t m = i % (int64_t)n;
    if (m < 0) m += (int64_t)n;
    return (uint32_t)m;
}

static void texel_at(const CpuTexture* t, uint32_t x, uint32_t y, float out[4]) {
    if (t->fmt == BGPU_FORMAT_R8) {
        const float v = t->px[(size_t)y * t->w + x] / 255.0f;
        out[0] = out[1] = out[2] = out[3] = v;
        return;
    }
    px_load(t->px + ((size_t)y * t->w + x) * 4u, t->fmt, out);
}

/* 采样规则见 docs/raster_rules.md §6：
 *   clamp：先把 u,v 夹到 [0,1]（边缘纹素被拉伸）；
 *   repeat：把 u,v 绕回 [0,1)；
 *   nearest：floor(u*w)；
 *   linear：x = u*w - 0.5，取左右两个纹素按小数部分线性插值（GPU 的纹素中心约定）。 */
static void cpu_sample(const CpuTexture* t, float u, float v, const CpuSampler* s, float out[4]) {
    const int rep_u = s && s->desc.wrap_u == BGPU_WRAP_REPEAT;
    const int rep_v = s && s->desc.wrap_v == BGPU_WRAP_REPEAT;
    const int linear = s ? (s->desc.mag_filter == BGPU_FILTER_LINEAR) : 1;

    if (rep_u) {
        u = u - floorf(u);
    } else {
        u = clampf(u, 0.0f, 1.0f);
    }
    if (rep_v) {
        v = v - floorf(v);
    } else {
        v = clampf(v, 0.0f, 1.0f);
    }

    const float fw = (float)t->w;
    const float fh = (float)t->h;
    if (!linear) {
        const uint32_t ix = wrap_index((int64_t)floorf(u * fw), t->w, rep_u);
        const uint32_t iy = wrap_index((int64_t)floorf(v * fh), t->h, rep_v);
        texel_at(t, ix, iy, out);
        return;
    }
    const float fx = u * fw - 0.5f;
    const float fy = v * fh - 0.5f;
    const float x0f = floorf(fx);
    const float y0f = floorf(fy);
    const float tx = fx - x0f;
    const float ty = fy - y0f;
    const int64_t x0 = (int64_t)x0f;
    const int64_t y0 = (int64_t)y0f;
    const uint32_t ix0 = wrap_index(x0, t->w, rep_u);
    const uint32_t ix1 = wrap_index(x0 + 1, t->w, rep_u);
    const uint32_t iy0 = wrap_index(y0, t->h, rep_v);
    const uint32_t iy1 = wrap_index(y0 + 1, t->h, rep_v);
    float c00[4], c10[4], c01[4], c11[4];
    texel_at(t, ix0, iy0, c00);
    texel_at(t, ix1, iy0, c10);
    texel_at(t, ix0, iy1, c01);
    texel_at(t, ix1, iy1, c11);
    for (int i = 0; i < 4; i++) {
        const float a = c00[i] + (c10[i] - c00[i]) * tx;
        const float b = c01[i] + (c11[i] - c01[i]) * tx;
        out[i] = a + (b - a) * ty;
    }
}

/* ---------------------------------------------------------------
 * 混合
 * --------------------------------------------------------------- */
static void blend_premul(float dst[4], const float src[4], BgpuBlend mode) {
    const float sa = src[3];
    switch (mode) {
        case BGPU_BLEND_REPLACE:
            for (int i = 0; i < 4; i++) dst[i] = src[i];
            return;
        case BGPU_BLEND_KEEP:      /* dst *= srcA（mask 保留趟） */
            for (int i = 0; i < 4; i++) dst[i] *= sa;
            return;
        case BGPU_BLEND_ERASE:     /* dst *= (1-srcA)（裁剪补集擦除） */
            for (int i = 0; i < 4; i++) dst[i] *= (1.0f - sa);
            return;
        case BGPU_BLEND_ADDITIVE:  /* src + dst */
            for (int i = 0; i < 4; i++) dst[i] = clampf(dst[i] + src[i], 0.0f, 1.0f);
            return;
        case BGPU_BLEND_PREMULTIPLIED:
        default:
            for (int i = 0; i < 4; i++)
                dst[i] = clampf(src[i] + dst[i] * (1.0f - sa), 0.0f, 1.0f);
            return;
    }
}

/* ---------------------------------------------------------------
 * 光栅
 * --------------------------------------------------------------- */
typedef struct RVert {
    float x, y, z;
    float u, v;
    float r, g, b, a;   /* 直通色（插值在直通域做，与 GPU 的顶点色一致） */
} RVert;

typedef struct RasterCtx {
    CpuCmd* cmd;
    CpuTexture* tgt;
    CpuTexture* depth;
    int l, t, r, b;     /* scissor ∩ 目标 的整数像素范围（右/下开区间） */
} RasterCtx;

static inline float edgef(const RVert* a, const RVert* b, float px, float py) {
    return (b->x - a->x) * (py - a->y) - (b->y - a->y) * (px - a->x);
}

/* top-left 填充规则（y 向下、area>0 归一化之后）：
 *   · 水平且向右的边算 top（dy==0 && dx>0）；
 *   · 向下的边算 left（dy<0，因为归一化后内部在 e>0 一侧）。
 * 作用：相邻三角形共享的边只被**一个**三角形覆盖，半透明拼接不会出现一条缝。 */
static inline int is_top_left(const RVert* a, const RVert* b) {
    const float dx = b->x - a->x;
    const float dy = b->y - a->y;
    return (dy == 0.0f && dx > 0.0f) || (dy < 0.0f);
}

/* 一个片元的着色 + 深度 + 混合。src 是**预乘**颜色。 */
static void shade_pixel(RasterCtx* rc, float px, float py, const RVert* v0, const RVert* v1,
                        const RVert* v2, float w0, float w1, float w2) {
    CpuCmd* c = rc->cmd;
    CpuTexture* tgt = rc->tgt;

    const float u = w0 * v0->u + w1 * v1->u + w2 * v2->u;
    const float v = w0 * v0->v + w1 * v1->v + w2 * v2->v;
    const float z = w0 * v0->z + w1 * v1->z + w2 * v2->z;

    const uint32_t xi = (uint32_t)px;
    const uint32_t yi = (uint32_t)py;

    /* 深度：LESS，只写不测也可以（与 D3D/GL 的独立开关一致） */
    if (rc->depth) {
        CpuTexture* dt = rc->depth;
        if (dt->depth && xi < dt->depth_w && yi < dt->depth_h) {
            float* d = &dt->depth[(size_t)yi * dt->depth_w + xi];
            if ((c->pipe->pipe_flags & BGPU_PIPE_DEPTH_TEST) && !(z < *d)) return;
            if (c->pipe->pipe_flags & BGPU_PIPE_DEPTH_WRITE) *d = z;
        }
    }

    float frag[4];
    if ((c->pipe->pipe_flags & BGPU_PIPE_TEXTURED) && c->tex) {
        float tex[4];
        cpu_sample(c->tex, u, v, c->samp, tex);
        /* 纹理存的是预乘色，顶点色是直通色：按直通语义合成 ——
         * straight = tex_straight * vcol；再预乘 ⇒
         *   rgb = tex_pre.rgb * vcol.rgb * vcol.a, a = tex_pre.a * vcol.a */
        const float vr = w0 * v0->r + w1 * v1->r + w2 * v2->r;
        const float vg = w0 * v0->g + w1 * v1->g + w2 * v2->g;
        const float vb = w0 * v0->b + w1 * v1->b + w2 * v2->b;
        const float va = w0 * v0->a + w1 * v1->a + w2 * v2->a;
        frag[0] = tex[0] * vr * va;
        frag[1] = tex[1] * vg * va;
        frag[2] = tex[2] * vb * va;
        frag[3] = tex[3] * va;
    } else {
        const float vr = w0 * v0->r + w1 * v1->r + w2 * v2->r;
        const float vg = w0 * v0->g + w1 * v1->g + w2 * v2->g;
        const float vb = w0 * v0->b + w1 * v1->b + w2 * v2->b;
        const float va = w0 * v0->a + w1 * v1->a + w2 * v2->a;
        frag[0] = vr * va;
        frag[1] = vg * va;
        frag[2] = vb * va;
        frag[3] = va;
    }

    uint8_t* p = tgt->px + ((size_t)yi * tgt->w + xi) * bytes_per_px(tgt->fmt);
    float dst[4];
    px_load(p, tgt->fmt, dst);
    blend_premul(dst, frag, c->pipe->blend);
    px_store(p, tgt->fmt, dst);
}

static void raster_tri(RasterCtx* rc, RVert* a, RVert* b, RVert* d) {
    float area = (b->x - a->x) * (d->y - a->y) - (b->y - a->y) * (d->x - a->x);
    if (area == 0.0f) return;
    if (area < 0.0f) {
        RVert tmp = *b;
        *b = *d;
        *d = tmp;
        area = -area;
    }
    /* 像素包围盒（右/下闭区间换算成开区间） */
    const float fx0 = fminf(a->x, fminf(b->x, d->x));
    const float fx1 = fmaxf(a->x, fmaxf(b->x, d->x));
    const float fy0 = fminf(a->y, fminf(b->y, d->y));
    const float fy1 = fmaxf(a->y, fmaxf(b->y, d->y));
    int x0 = (int)floorf(fx0);
    int x1 = (int)ceilf(fx1);
    int y0 = (int)floorf(fy0);
    int y1 = (int)ceilf(fy1);
    if (x1 <= x0 || y1 <= y0) return;
    if (x0 < rc->l) x0 = rc->l;
    if (y0 < rc->t) y0 = rc->t;
    if (x1 > rc->r) x1 = rc->r;
    if (y1 > rc->b) y1 = rc->b;
    if (x1 <= x0 || y1 <= y0) return;

    const int tl0 = is_top_left(b, d);
    const int tl1 = is_top_left(d, a);
    const int tl2 = is_top_left(a, b);
    const float inv = 1.0f / area;

    for (int y = y0; y < y1; y++) {
        const float py = (float)y + 0.5f;
        for (int x = x0; x < x1; x++) {
            const float px = (float)x + 0.5f;
            const float e0 = edgef(b, d, px, py);
            const float e1 = edgef(d, a, px, py);
            const float e2 = edgef(a, b, px, py);
            const int in0 = (e0 > 0.0f) || (e0 == 0.0f && tl0);
            const int in1 = (e1 > 0.0f) || (e1 == 0.0f && tl1);
            const int in2 = (e2 > 0.0f) || (e2 == 0.0f && tl2);
            if (!in0 || !in1 || !in2) continue;
            shade_pixel(rc, px, py, a, b, d, e0 * inv, e1 * inv, e2 * inv);
        }
    }
}

/* ---------------------------------------------------------------
 * 命令：渲染状态
 * --------------------------------------------------------------- */
static void ensure_depth_storage(CpuTexture* dt, uint32_t w, uint32_t h) {
    if (dt->depth && dt->depth_w == w && dt->depth_h == h) return;
    free(dt->depth);
    dt->depth = (float*)malloc((size_t)w * h * sizeof(float));
    dt->depth_w = dt->depth ? w : 0;
    dt->depth_h = dt->depth ? h : 0;
    if (dt->depth) {
        for (size_t i = 0; i < (size_t)w * h; i++) dt->depth[i] = 1.0f;
    }
}

static BgpuResult cpu_cmd_begin_render(void* cmd_, const BgpuRenderBeginDesc* desc) {
    CpuCmd* c = (CpuCmd*)cmd_;
    if (c->in_render) {
        bgpu_set_reason("已经在渲染里（重复 begin_render）");
        return BGPU_ERR_STATE;
    }
    CpuTexture* t = (CpuTexture*)bgpu_obj_get(desc->target, BGPU_OBJ_TEXTURE);
    if (!t) {
        bgpu_set_reason("target 不是有效的纹理句柄");
        return BGPU_ERR_INVALID_HANDLE;
    }
    if (!(t->tex_flags & BGPU_TEXTURE_RENDER_TARGET) && !t->borrowed) {
        bgpu_set_reason("target 纹理没有 BGPU_TEXTURE_RENDER_TARGET 标志，不能当渲染目标");
        return BGPU_ERR_STATE;
    }
    CpuTexture* dt = NULL;
    if (desc->depth_target != BGPU_INVALID) {
        dt = (CpuTexture*)bgpu_obj_get(desc->depth_target, BGPU_OBJ_TEXTURE);
        if (!dt) {
            bgpu_set_reason("depth_target 不是有效的纹理句柄");
            return BGPU_ERR_INVALID_HANDLE;
        }
        ensure_depth_storage(dt, t->w, t->h);
        if (!dt->depth) {
            bgpu_set_reason("深度缓冲分配失败（%ux%u）", t->w, t->h);
            return BGPU_ERR_OUT_OF_MEMORY;
        }
        if (desc->flags & BGPU_RENDER_CLEAR_DEPTH) {
            for (size_t i = 0; i < (size_t)dt->depth_w * dt->depth_h; i++)
                dt->depth[i] = desc->clear_depth;
        }
    }
    if (desc->flags & BGPU_RENDER_CLEAR_COLOR) {
        uint8_t* p = t->px;
        const uint32_t n = t->w * t->h;
        const uint32_t bpp = bytes_per_px(t->fmt);
        for (uint32_t i = 0; i < n; i++) {
            store_straight(p + (size_t)i * bpp, t->fmt, desc->clear_color);
        }
    }
    c->in_render = 1;
    c->target = t;
    c->depth = dt;
    /* 每次 begin_render 把状态复位到"整目标 + 单位仿射 + 线性钳制"：
     * 与 GPU 后端的 pass 起点一致，免得上一趟的 scissor 漏到下一趟。 */
    c->vp[0] = 0.0f;
    c->vp[1] = 0.0f;
    c->vp[2] = (float)t->w;
    c->vp[3] = (float)t->h;
    c->scissor_on = 0;
    c->sc[0] = 0;
    c->sc[1] = 0;
    c->sc[2] = (int32_t)t->w;
    c->sc[3] = (int32_t)t->h;
    c->affine_on = 0;
    return BGPU_OK;
}

static BgpuResult cpu_cmd_end_render(void* cmd_) {
    CpuCmd* c = (CpuCmd*)cmd_;
    if (!c->in_render) {
        bgpu_set_reason("没有正在进行的渲染（end_render 前没有 begin_render）");
        return BGPU_ERR_STATE;
    }
    c->in_render = 0;
    c->target = NULL;
    c->depth = NULL;
    return BGPU_OK;
}

static BgpuResult cpu_cmd_set_viewport(void* cmd_, float x, float y, float w, float h) {
    CpuCmd* c = (CpuCmd*)cmd_;
    c->vp[0] = x;
    c->vp[1] = y;
    c->vp[2] = w;
    c->vp[3] = h;
    return BGPU_OK;
}

static BgpuResult cpu_cmd_set_scissor(void* cmd_, int32_t x, int32_t y, int32_t w, int32_t h) {
    CpuCmd* c = (CpuCmd*)cmd_;
    if (w < 0 || h < 0) {
        c->scissor_on = 0;   /* 负数 = 关掉裁剪（整目标） */
        return BGPU_OK;
    }
    c->scissor_on = 1;
    c->sc[0] = x;
    c->sc[1] = y;
    c->sc[2] = w;
    c->sc[3] = h;
    return BGPU_OK;
}

static BgpuResult cpu_cmd_set_pipeline(void* cmd_, void* pipeline_) {
    CpuCmd* c = (CpuCmd*)cmd_;
    c->pipe = (CpuPipeline*)pipeline_;
    return BGPU_OK;
}

static BgpuResult cpu_cmd_set_texture(void* cmd_, void* tex_, void* sampler_) {
    CpuCmd* c = (CpuCmd*)cmd_;
    c->tex = (CpuTexture*)tex_;
    c->samp = (CpuSampler*)sampler_;
    return BGPU_OK;
}

static BgpuResult cpu_cmd_set_affine(void* cmd_, const float xf[6]) {
    CpuCmd* c = (CpuCmd*)cmd_;
    if (!xf) {
        c->affine_on = 0;
        return BGPU_OK;
    }
    for (int i = 0; i < 6; i++) c->xf[i] = xf[i];
    c->affine_on = 1;
    return BGPU_OK;
}

/* ---------------------------------------------------------------
 * 命令：绘制
 * --------------------------------------------------------------- */
static void transform_vert(const CpuCmd* c, const CpuTexture* tgt, const BgpuVertex* in,
                           RVert* out) {
    float sx = c->vp[0] + (in->x / (float)tgt->w) * c->vp[2];
    float sy = c->vp[1] + (in->y / (float)tgt->h) * c->vp[3];
    if (c->affine_on) {
        const float* m = c->xf;
        const float nx = m[0] * sx + m[1] * sy + m[2];
        const float ny = m[3] * sx + m[4] * sy + m[5];
        sx = nx;
        sy = ny;
    }
    out->x = sx;
    out->y = sy;
    out->z = in->z;
    out->u = in->u;
    out->v = in->v;
    out->r = ((in->color >> 16) & 0xFFu) / 255.0f;
    out->g = ((in->color >> 8) & 0xFFu) / 255.0f;
    out->b = (in->color & 0xFFu) / 255.0f;
    out->a = ((in->color >> 24) & 0xFFu) / 255.0f;
}

/* 组装一次绘制的公共上下文（scissor ∩ 目标边界） */
static void make_raster_ctx(CpuCmd* c, RasterCtx* rc) {
    CpuTexture* tgt = c->target;
    rc->cmd = c;
    rc->tgt = tgt;
    rc->depth = c->depth;
    int l = 0, t = 0, r = (int)tgt->w, b = (int)tgt->h;
    if (c->scissor_on) {
        const int sl = c->sc[0], st = c->sc[1];
        const int sr = c->sc[0] + c->sc[2], sb = c->sc[1] + c->sc[3];
        if (sl > l) l = sl;
        if (st > t) t = st;
        if (sr < r) r = sr;
        if (sb < b) b = sb;
    }
    rc->l = l;
    rc->t = t;
    rc->r = r;
    rc->b = b;
}

static BgpuResult cpu_cmd_draw_quads(void* cmd_, const BgpuVertex* vertices,
                                     uint32_t quad_count) {
    CpuCmd* c = (CpuCmd*)cmd_;
    if (!c->in_render) {
        bgpu_set_reason("draw 之前没有 begin_render");
        return BGPU_ERR_STATE;
    }
    if (!c->pipe) {
        bgpu_set_reason("draw 之前没有 set_pipeline");
        return BGPU_ERR_STATE;
    }
    if (c->pipe->layout != BGPU_VERTEX_2D) {
        bgpu_set_reason("draw_quads 需要 BGPU_VERTEX_2D 管线的顶点布局");
        return BGPU_ERR_STATE;
    }
    if (!vertices || quad_count == 0) return BGPU_OK;

    RasterCtx rc;
    make_raster_ctx(c, &rc);
    c->dev->draws++;
    c->dev->quads += quad_count;
    c->dev->tris += (uint64_t)quad_count * 2;

    for (uint32_t q = 0; q < quad_count; q++) {
        const BgpuVertex* v = vertices + (size_t)q * 4u;
        RVert t[4];
        for (int i = 0; i < 4; i++) transform_vert(c, rc.tgt, &v[i], &t[i]);
        /* TL,TR,BL,BR → 三角形 (0,1,2) 与 (2,1,3)：与 binUI 的 emitQuad 同一套索引，
         * 保证共享的对角线在 top-left 规则下只被一个三角形覆盖。 */
        raster_tri(&rc, &t[0], &t[1], &t[2]);
        raster_tri(&rc, &t[2], &t[1], &t[3]);
    }
    return BGPU_OK;
}

static BgpuResult cpu_cmd_draw_indexed(void* cmd_, void* vb_, uint32_t vertex_offset,
                                       void* ib_, uint32_t index_count, uint32_t index_offset) {
    CpuCmd* c = (CpuCmd*)cmd_;
    if (!c->in_render) {
        bgpu_set_reason("draw_indexed 之前没有 begin_render");
        return BGPU_ERR_STATE;
    }
    if (!c->pipe) {
        bgpu_set_reason("draw_indexed 之前没有 set_pipeline");
        return BGPU_ERR_STATE;
    }
    if (c->pipe->layout != BGPU_VERTEX_MESH) {
        bgpu_set_reason("draw_indexed 需要 BGPU_VERTEX_MESH 管线的顶点布局");
        return BGPU_ERR_STATE;
    }
    CpuBuffer* vb = (CpuBuffer*)vb_;
    CpuBuffer* ib = (CpuBuffer*)ib_;
    if (index_count == 0) return BGPU_OK;
    /* 偏移是**字节**（与 buffer_update 一致），见 docs/raster_rules.md §8 */
    if (index_offset % 4u != 0) {
        bgpu_set_reason("index_offset=%u 不是 4 的倍数（32 位索引，按字节偏移）", index_offset);
        return BGPU_ERR_INVALID_ARGUMENT;
    }
    if ((uint64_t)index_offset + (uint64_t)index_count * 4u > ib->size) {
        bgpu_set_reason("索引区间超出索引缓冲（offset=%u count=%u size=%u）", index_offset,
                        index_count, ib->size);
        return BGPU_ERR_INVALID_ARGUMENT;
    }

    RasterCtx rc;
    make_raster_ctx(c, &rc);
    c->dev->draws++;
    c->dev->tris += index_count / 3u;

    const uint32_t* idx = (const uint32_t*)(ib->data + index_offset);
    for (uint32_t i = 0; i + 2 < index_count; i += 3) {
        RVert tri[3];
        int bad = 0;
        for (int k = 0; k < 3; k++) {
            const uint32_t vi = idx[i + (uint32_t)k];
            const uint64_t off = (uint64_t)vertex_offset + (uint64_t)vi * sizeof(BgpuMeshVertex);
            if (off + sizeof(BgpuMeshVertex) > vb->size) {
                bgpu_set_reason("索引 %u 指向顶点 %u，超出顶点缓冲（offset=%u size=%u）", i + (uint32_t)k,
                                vi, vertex_offset, vb->size);
                return BGPU_ERR_INVALID_ARGUMENT;
            }
            BgpuMeshVertex mv;
            memcpy(&mv, vb->data + off, sizeof(mv));
            const BgpuVertex v2 = {mv.x, mv.y, mv.z, mv.u, mv.v, mv.color};
            transform_vert(c, rc.tgt, &v2, &tri[k]);
            bad = 0;
        }
        (void)bad;
        raster_tri(&rc, &tri[0], &tri[1], &tri[2]);
    }
    return BGPU_OK;
}

/* ---------------------------------------------------------------
 * 注册
 * --------------------------------------------------------------- */
const BgpuVt bgpu_cpu_vt = {
    BGPU_BACKEND_CPU,
    "cpu",
    cpu_available,
    NULL,
    cpu_device_create,
    cpu_device_info,
    cpu_device_caps,
    cpu_device_stats,
    cpu_device_lost,
    cpu_swapchain_create,
    cpu_swapchain_resize,
    cpu_swapchain_info,
    cpu_frame_begin,
    cpu_frame_end,
    cpu_frame_read,
    cpu_texture_create,
    cpu_texture_info,
    cpu_texture_upload,
    cpu_texture_read,
    cpu_buffer_create,
    cpu_buffer_update,
    cpu_sampler_create,
    cpu_pipeline_create,
    cpu_cmd_create,
    cpu_cmd_begin_render,
    cpu_cmd_end_render,
    cpu_cmd_set_viewport,
    cpu_cmd_set_scissor,
    cpu_cmd_set_pipeline,
    cpu_cmd_set_texture,
    cpu_cmd_set_affine,
    cpu_cmd_draw_quads,
    cpu_cmd_draw_indexed,
};
