/* bingpu_null.c —— NULL 后端：只校验、只计数、不产生像素
 *
 * 为什么需要一个"什么都不画"的后端：
 *   ① 它是**调用序列的回归夹具**：上层（bin2d/bin3d/binUI）接进来时，先跑 NULL
 *      就能把"顺序错了 / 句柄用错了 / struct_size 填错了"这类 bug 从像素问题里
 *      摘出来 —— 排查"画面不对"时最贵的一步就是分离"调用错"和"画错"；
 *   ② 无 GPU、无 framebuffer 的 CI 里它永远可用（AUTO 的最后兜底）；
 *   ③ 它是"后端契约"的可执行版本：同样的误用在这里得到的结果码，真实后端也必须给。
 *
 * 所以它把状态机完整实现一遍（谁在帧里、谁在 render 里、管线设没设），只是
 * 不分配任何像素。**故意不共享 CPU 后端的代码**：两个独立实现互为对照，
 * 哪天 CPU 的校验悄悄松了，测试会从 NULL 这边看出来。
 */
#include "bingpu_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct NullDevice NullDevice;

typedef struct NullTexture {
    BgpuObjectBase base;
    NullDevice* dev;
    uint32_t w, h;
    BgpuFormat fmt;
    uint32_t tex_flags;
    uint32_t mip_levels;
    uint64_t bytes;
} NullTexture;

typedef struct NullSwapchain {
    BgpuObjectBase base;
    NullDevice* dev;
    uint32_t w, h;
    BgpuFormat fmt;
    uint32_t flags;
    BgpuTexture target;      /* 借给 frame_begin 的目标纹理（borrowed） */
    uint64_t frames;
} NullSwapchain;

typedef struct NullBuffer {
    BgpuObjectBase base;
    NullDevice* dev;
    uint32_t size;
    uint32_t usage;
    uint64_t bytes;
} NullBuffer;

typedef struct NullSampler {
    BgpuObjectBase base;
    NullDevice* dev;
    BgpuSamplerDesc desc;
} NullSampler;

typedef struct NullPipeline {
    BgpuObjectBase base;
    NullDevice* dev;
    BgpuPipelineDesc desc;
} NullPipeline;

typedef struct NullCmd {
    BgpuObjectBase base;
    NullDevice* dev;
    int in_frame;
    NullSwapchain* frame;
    int in_render;
    BgpuTexture target;
    BgpuTexture depth;
    NullPipeline* pipe;
    float vp[4];
    int32_t sc[4];
    int affine_on;
    uint32_t quads_in_draw;
} NullCmd;

struct NullDevice {
    BgpuObjectBase base;
    BgpuDevice self;   /* 自己的句柄：资源 owner 用它，不能用 base.device（那是 0） */
    uint32_t flags;
    uint32_t w, h;
    BgpuFormat surface_format;
    /* 累计统计（跨帧；与 BgpuStats 的语义一致，见 docs/abi.md） */
    uint64_t frames_presented, draws, quads, tris;
    uint64_t bytes_uploaded, bytes_live;
    uint32_t frame_index;
};

/* ---------------------------------------------------------------
 * 析构：只还内存（NULL 后端没有像素）
 * --------------------------------------------------------------- */
static void null_device_dtor(void* p) { free(p); }
static void null_swapchain_dtor(void* p) { free(p); }
static void null_texture_dtor(void* p) { free(p); }
static void null_buffer_dtor(void* p) { free(p); }
static void null_sampler_dtor(void* p) { free(p); }
static void null_pipeline_dtor(void* p) { free(p); }
static void null_cmd_dtor(void* p) { free(p); }

/* ---------------------------------------------------------------
 * 设备
 * --------------------------------------------------------------- */
static int null_available(void) { return 1; }

static BgpuResult null_device_create(const BgpuDeviceDesc* desc, BgpuDevice* out_device) {
    const BgpuDevice handle = bgpu_obj_new(BGPU_OBJ_DEVICE, BGPU_BACKEND_NULL, BGPU_INVALID,
                                           sizeof(NullDevice), null_device_dtor);
    if (handle == BGPU_INVALID) {
        bgpu_set_reason("对象表分配失败（内存不足）");
        return BGPU_ERR_OUT_OF_MEMORY;
    }
    NullDevice* d = (NullDevice*)bgpu_obj_get(handle, BGPU_OBJ_DEVICE);
    d->self = handle;
    d->flags = desc->flags;
    d->w = desc->width;
    d->h = desc->height;
    d->surface_format = BGPU_FORMAT_RGBA8;
    *out_device = handle;
    return BGPU_OK;
}

static void null_device_info(void* dev, BgpuDeviceInfo* out) {
    NullDevice* d = (NullDevice*)dev;
    out->backend = BGPU_BACKEND_NULL;
    out->surface_format = d->surface_format;
    out->api_version = 1;   /* "NULL 后端第 1 版"，仅上报用 */
    snprintf(out->adapter_name, sizeof(out->adapter_name), "%s", "binGPU NULL (validation only)");
    snprintf(out->driver_name, sizeof(out->driver_name), "%s", "none");
}

static void null_device_caps(void* dev, BgpuCaps* out) {
    (void)dev;
    /* 如实上报：NULL 画不出任何东西，所以能力全 0。 */
    out->flags = 0;
    out->max_texture_size = BGPU_MAX_TEXTURE_SIZE;
    out->max_frames_in_flight = 1;
    out->msaa_samples = 1;
    out->max_quads_per_draw = 65536u;
}

static void null_device_stats(void* dev, BgpuStats* out) {
    NullDevice* d = (NullDevice*)dev;
    out->frame_index = d->frame_index;
    out->frames_presented = d->frames_presented;
    out->draws = d->draws;
    out->quads = d->quads;
    out->triangles = d->tris;
    out->bytes_uploaded = d->bytes_uploaded;
    out->bytes_live = d->bytes_live;
}

static int null_device_lost(void* dev) {
    (void)dev;
    return 0;
}

/* ---------------------------------------------------------------
 * 交换链 / 帧
 * --------------------------------------------------------------- */
static BgpuResult null_swapchain_create(void* dev, const BgpuSwapchainDesc* desc,
                                        BgpuSwapchain* out) {
    NullDevice* d = (NullDevice*)dev;
    const BgpuSwapchain handle = bgpu_obj_new(BGPU_OBJ_SWAPCHAIN, BGPU_BACKEND_NULL,
                                              d->self, sizeof(NullSwapchain),
                                              null_swapchain_dtor);
    if (handle == BGPU_INVALID) return BGPU_ERR_OUT_OF_MEMORY;
    NullSwapchain* sc = (NullSwapchain*)bgpu_obj_get(handle, BGPU_OBJ_SWAPCHAIN);
    sc->dev = d;
    sc->w = desc->width;
    sc->h = desc->height;
    sc->fmt = desc->format;
    sc->flags = desc->flags;
    /* 借出用的目标纹理：句柄必须是真的（调用方会拿它去 begin_render），
     * 但没有像素，所以只是登记一个带 BORROWED 标志的纹理对象。 */
    const BgpuTexture t = bgpu_obj_new(BGPU_OBJ_TEXTURE, BGPU_BACKEND_NULL, d->self,
                                       sizeof(NullTexture), null_texture_dtor);
    if (t == BGPU_INVALID) {
        bgpu_obj_destroy(handle);
        return BGPU_ERR_OUT_OF_MEMORY;
    }
    NullTexture* nt = (NullTexture*)bgpu_obj_get(t, BGPU_OBJ_TEXTURE);
    nt->dev = d;
    nt->w = desc->width;
    nt->h = desc->height;
    nt->fmt = desc->format;
    nt->tex_flags = BGPU_TEXTURE_RENDER_TARGET;
    nt->mip_levels = 1;
    nt->base.obj_flags |= BGPU_OBJ_F_BORROWED;
    sc->target = t;
    *out = handle;
    return BGPU_OK;
}

static BgpuResult null_swapchain_resize(void* sc, uint32_t w, uint32_t h) {
    NullSwapchain* s = (NullSwapchain*)sc;
    s->w = w;
    s->h = h;
    NullTexture* t = (NullTexture*)bgpu_obj_get(s->target, BGPU_OBJ_TEXTURE);
    if (t) {
        t->w = w;
        t->h = h;
    }
    return BGPU_OK;
}

static void null_swapchain_info(void* sc, BgpuSwapchainInfo* out) {
    NullSwapchain* s = (NullSwapchain*)sc;
    out->width = s->w;
    out->height = s->h;
    out->format = s->fmt;
    out->image_count = 1;
    out->msaa_samples = 1;
    out->present_paces_frames = 0;
}

static BgpuResult null_frame_begin(void* sc, void* cmd, BgpuTexture* out_target) {
    NullSwapchain* s = (NullSwapchain*)sc;
    NullCmd* c = (NullCmd*)cmd;
    if (c->in_frame) {
        bgpu_set_reason("cmd 已经在一个帧里（先 frame_end 再 begin）");
        return BGPU_ERR_STATE;
    }
    c->in_frame = 1;
    c->frame = s;
    *out_target = s->target;
    return BGPU_OK;
}

static BgpuResult null_frame_end(void* sc, void* cmd) {
    NullSwapchain* s = (NullSwapchain*)sc;
    NullCmd* c = (NullCmd*)cmd;
    if (!c->in_frame || c->frame != s) {
        bgpu_set_reason("这个 cmd 没抓着这个交换链的帧（frame_begin 呢？）");
        return BGPU_ERR_STATE;
    }
    if (c->in_render) {
        bgpu_set_reason("还在 begin_render/end_render 之间，先 end_render");
        return BGPU_ERR_STATE;
    }
    c->in_frame = 0;
    c->frame = NULL;
    s->frames++;
    s->dev->frames_presented++;
    s->dev->frame_index++;
    return BGPU_OK;
}

static BgpuResult null_frame_read(void* sc, void* dst, size_t dst_size, uint32_t* out_stride) {
    (void)sc;
    (void)dst;
    (void)dst_size;
    (void)out_stride;
    bgpu_set_reason("NULL 后端不保存像素，frame_read 不可用（换 cpu 后端出图）");
    return BGPU_ERR_UNSUPPORTED;
}

/* ---------------------------------------------------------------
 * 纹理
 * --------------------------------------------------------------- */
static BgpuResult null_texture_create(void* dev, const BgpuTextureDesc* desc,
                                      BgpuTexture* out) {
    NullDevice* d = (NullDevice*)dev;
    const BgpuTexture handle = bgpu_obj_new(BGPU_OBJ_TEXTURE, BGPU_BACKEND_NULL,
                                            d->self, sizeof(NullTexture),
                                            null_texture_dtor);
    if (handle == BGPU_INVALID) return BGPU_ERR_OUT_OF_MEMORY;
    NullTexture* t = (NullTexture*)bgpu_obj_get(handle, BGPU_OBJ_TEXTURE);
    t->dev = d;
    t->w = desc->width;
    t->h = desc->height;
    t->fmt = desc->format;
    t->tex_flags = desc->flags;
    t->mip_levels = desc->mip_levels ? desc->mip_levels : 1;
    t->bytes = (uint64_t)t->w * t->h * bingpu_format_bytes(t->fmt);
    d->bytes_live += t->bytes;
    *out = handle;
    return BGPU_OK;
}

static void null_texture_info(void* tex, BgpuTextureInfo* out) {
    NullTexture* t = (NullTexture*)tex;
    out->width = t->w;
    out->height = t->h;
    out->format = t->fmt;
    out->flags = t->tex_flags;
    out->mip_levels = t->mip_levels;
}

static BgpuResult null_texture_upload(void* tex, const BgpuBox* box, const void* pixels,
                                      uint32_t stride) {
    NullTexture* t = (NullTexture*)tex;
    const uint32_t x = box ? box->x : 0;
    const uint32_t y = box ? box->y : 0;
    const uint32_t w = box ? box->w : t->w;
    const uint32_t h = box ? box->h : t->h;
    if (w == 0 || h == 0 || x + w > t->w || y + h > t->h) {
        bgpu_set_reason("上传区域 %u,%u %ux%u 超出纹理 %ux%u", x, y, w, h, t->w, t->h);
        return BGPU_ERR_INVALID_ARGUMENT;
    }
    const uint32_t bpp = bingpu_format_bytes(t->fmt);
    const uint32_t row = stride ? stride : w * bpp;
    if (row < w * bpp) {
        bgpu_set_reason("stride=%u 小于一行 %u 字节", row, w * bpp);
        return BGPU_ERR_INVALID_ARGUMENT;
    }
    (void)pixels;
    t->dev->bytes_uploaded += (uint64_t)row * h;
    return BGPU_OK;
}

static BgpuResult null_texture_read(void* tex, void* dst, size_t dst_size,
                                    uint32_t* out_stride) {
    (void)tex;
    (void)dst;
    (void)dst_size;
    (void)out_stride;
    bgpu_set_reason("NULL 后端不保存像素，texture_read 不可用（换 cpu 后端）");
    return BGPU_ERR_UNSUPPORTED;
}

/* ---------------------------------------------------------------
 * 缓冲 / 采样器 / 管线
 * --------------------------------------------------------------- */
static BgpuResult null_buffer_create(void* dev, const BgpuBufferDesc* desc, BgpuBuffer* out) {
    NullDevice* d = (NullDevice*)dev;
    const BgpuBuffer handle = bgpu_obj_new(BGPU_OBJ_BUFFER, BGPU_BACKEND_NULL, d->self,
                                           sizeof(NullBuffer), null_buffer_dtor);
    if (handle == BGPU_INVALID) return BGPU_ERR_OUT_OF_MEMORY;
    NullBuffer* b = (NullBuffer*)bgpu_obj_get(handle, BGPU_OBJ_BUFFER);
    b->dev = d;
    b->size = desc->size;
    b->usage = desc->usage;
    b->bytes = desc->size;
    d->bytes_live += b->bytes;
    *out = handle;
    return BGPU_OK;
}

static BgpuResult null_buffer_update(void* buf, uint32_t offset, const void* data,
                                     uint32_t size) {
    NullBuffer* b = (NullBuffer*)buf;
    if ((uint64_t)offset + size > b->size) {
        bgpu_set_reason("写入区间 %u+%u 超出缓冲 %u 字节", offset, size, b->size);
        return BGPU_ERR_INVALID_ARGUMENT;
    }
    (void)data;
    b->dev->bytes_uploaded += size;
    return BGPU_OK;
}

static BgpuResult null_sampler_create(void* dev, const BgpuSamplerDesc* desc, BgpuSampler* out) {
    NullDevice* d = (NullDevice*)dev;
    const BgpuSampler handle = bgpu_obj_new(BGPU_OBJ_SAMPLER, BGPU_BACKEND_NULL,
                                            d->self, sizeof(NullSampler),
                                            null_sampler_dtor);
    if (handle == BGPU_INVALID) return BGPU_ERR_OUT_OF_MEMORY;
    NullSampler* s = (NullSampler*)bgpu_obj_get(handle, BGPU_OBJ_SAMPLER);
    s->dev = d;
    s->desc = *desc;
    *out = handle;
    return BGPU_OK;
}

static BgpuResult null_pipeline_create(void* dev, const BgpuPipelineDesc* desc,
                                       BgpuPipeline* out) {
    NullDevice* d = (NullDevice*)dev;
    const BgpuPipeline handle = bgpu_obj_new(BGPU_OBJ_PIPELINE, BGPU_BACKEND_NULL,
                                             d->self, sizeof(NullPipeline),
                                             null_pipeline_dtor);
    if (handle == BGPU_INVALID) return BGPU_ERR_OUT_OF_MEMORY;
    NullPipeline* p = (NullPipeline*)bgpu_obj_get(handle, BGPU_OBJ_PIPELINE);
    p->dev = d;
    p->desc = *desc;
    *out = handle;
    return BGPU_OK;
}

/* ---------------------------------------------------------------
 * 命令缓冲：完整状态机
 * --------------------------------------------------------------- */
static BgpuResult null_cmd_create(void* dev, BgpuCmd* out) {
    NullDevice* d = (NullDevice*)dev;
    const BgpuCmd handle = bgpu_obj_new(BGPU_OBJ_CMD, BGPU_BACKEND_NULL, d->self,
                                        sizeof(NullCmd), null_cmd_dtor);
    if (handle == BGPU_INVALID) return BGPU_ERR_OUT_OF_MEMORY;
    NullCmd* c = (NullCmd*)bgpu_obj_get(handle, BGPU_OBJ_CMD);
    c->dev = d;
    c->vp[2] = (float)d->w;
    c->vp[3] = (float)d->h;
    *out = handle;
    return BGPU_OK;
}

static BgpuResult null_cmd_begin_render(void* cmd, const BgpuRenderBeginDesc* desc) {
    NullCmd* c = (NullCmd*)cmd;
    if (c->in_render) {
        bgpu_set_reason("已经在渲染里（重复 begin_render）");
        return BGPU_ERR_STATE;
    }
    NullTexture* t = (NullTexture*)bgpu_obj_get(desc->target, BGPU_OBJ_TEXTURE);
    if (!t) {
        bgpu_set_reason("target 不是有效的纹理句柄");
        return BGPU_ERR_INVALID_HANDLE;
    }
    if (!(t->tex_flags & BGPU_TEXTURE_RENDER_TARGET) && !(t->base.obj_flags & BGPU_OBJ_F_BORROWED)) {
        bgpu_set_reason("target 纹理没有 BGPU_TEXTURE_RENDER_TARGET 标志");
        return BGPU_ERR_STATE;
    }
    c->in_render = 1;
    c->target = desc->target;
    c->depth = desc->depth_target;
    return BGPU_OK;
}

static BgpuResult null_cmd_end_render(void* cmd) {
    NullCmd* c = (NullCmd*)cmd;
    if (!c->in_render) {
        bgpu_set_reason("没有正在进行的渲染（end_render 前没有 begin_render）");
        return BGPU_ERR_STATE;
    }
    c->in_render = 0;
    c->target = BGPU_INVALID;
    c->depth = BGPU_INVALID;
    return BGPU_OK;
}

static BgpuResult null_cmd_set_viewport(void* cmd, float x, float y, float w, float h) {
    NullCmd* c = (NullCmd*)cmd;
    c->vp[0] = x;
    c->vp[1] = y;
    c->vp[2] = w;
    c->vp[3] = h;
    return BGPU_OK;
}

static BgpuResult null_cmd_set_scissor(void* cmd, int32_t x, int32_t y, int32_t w, int32_t h) {
    NullCmd* c = (NullCmd*)cmd;
    c->sc[0] = x;
    c->sc[1] = y;
    c->sc[2] = w;
    c->sc[3] = h;
    return BGPU_OK;
}

static BgpuResult null_cmd_set_pipeline(void* cmd, void* pipeline) {
    NullCmd* c = (NullCmd*)cmd;
    c->pipe = (NullPipeline*)pipeline;
    return BGPU_OK;
}

static BgpuResult null_cmd_set_texture(void* cmd, void* tex, void* sampler) {
    (void)cmd;
    (void)tex;
    (void)sampler;
    return BGPU_OK;
}

static BgpuResult null_cmd_set_affine(void* cmd, const float xf[6]) {
    NullCmd* c = (NullCmd*)cmd;
    c->affine_on = xf != NULL;
    return BGPU_OK;
}

static BgpuResult null_cmd_draw_quads(void* cmd, const BgpuVertex* vertices, uint32_t count) {
    NullCmd* c = (NullCmd*)cmd;
    if (!c->in_render) {
        bgpu_set_reason("draw 之前没有 begin_render");
        return BGPU_ERR_STATE;
    }
    if (!c->pipe) {
        bgpu_set_reason("draw 之前没有 set_pipeline");
        return BGPU_ERR_STATE;
    }
    if (!vertices && count) {
        bgpu_set_reason("vertices 为空");
        return BGPU_ERR_INVALID_ARGUMENT;
    }
    c->dev->draws++;
    c->dev->quads += count;
    c->dev->tris += (uint64_t)count * 2;
    return BGPU_OK;
}

static BgpuResult null_cmd_draw_indexed(void* cmd, void* vb, uint32_t vertex_offset, void* ib,
                                        uint32_t index_count, uint32_t index_offset) {
    NullCmd* c = (NullCmd*)cmd;
    if (!c->in_render) {
        bgpu_set_reason("draw_indexed 之前没有 begin_render");
        return BGPU_ERR_STATE;
    }
    if (!c->pipe) {
        bgpu_set_reason("draw_indexed 之前没有 set_pipeline");
        return BGPU_ERR_STATE;
    }
    if (c->pipe->desc.vertex_layout != BGPU_VERTEX_MESH) {
        bgpu_set_reason("draw_indexed 需要 BGPU_VERTEX_MESH 管线的顶点布局");
        return BGPU_ERR_STATE;
    }
    NullBuffer* bvb = (NullBuffer*)vb;
    NullBuffer* bib = (NullBuffer*)ib;
    const uint32_t vsize = (uint32_t)sizeof(BgpuMeshVertex);
    if (vertex_offset % vsize != 0) {
        bgpu_set_reason("vertex_offset=%u 不是 %u 的倍数（按字节偏移）", vertex_offset, vsize);
        return BGPU_ERR_INVALID_ARGUMENT;
    }
    if (index_offset % 4u != 0) {
        bgpu_set_reason("index_offset=%u 不是 4 的倍数（32 位索引，按字节偏移）", index_offset);
        return BGPU_ERR_INVALID_ARGUMENT;
    }
    if ((uint64_t)index_offset + (uint64_t)index_count * 4u > bib->size) {
        bgpu_set_reason("索引区间超出索引缓冲（offset=%u count=%u size=%u）", index_offset,
                        index_count, bib->size);
        return BGPU_ERR_INVALID_ARGUMENT;
    }
    if (vertex_offset > bvb->size) {
        bgpu_set_reason("vertex_offset=%u 超出顶点缓冲 %u 字节", vertex_offset, bvb->size);
        return BGPU_ERR_INVALID_ARGUMENT;
    }
    c->dev->draws++;
    c->dev->tris += index_count / 3u;
    return BGPU_OK;
}

/* ---------------------------------------------------------------
 * 注册
 * --------------------------------------------------------------- */
const BgpuVt bgpu_null_vt = {
    BGPU_BACKEND_NULL,
    "null",
    null_available,
    NULL,
    null_device_create,
    null_device_info,
    null_device_caps,
    null_device_stats,
    null_device_lost,
    null_swapchain_create,
    null_swapchain_resize,
    null_swapchain_info,
    null_frame_begin,
    null_frame_end,
    null_frame_read,
    null_texture_create,
    null_texture_info,
    null_texture_upload,
    null_texture_read,
    null_buffer_create,
    null_buffer_update,
    null_sampler_create,
    null_pipeline_create,
    null_cmd_create,
    null_cmd_begin_render,
    null_cmd_end_render,
    null_cmd_set_viewport,
    null_cmd_set_scissor,
    null_cmd_set_pipeline,
    null_cmd_set_texture,
    null_cmd_set_affine,
    null_cmd_draw_quads,
    null_cmd_draw_indexed,
};
