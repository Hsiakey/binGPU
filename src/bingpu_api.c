/* bingpu_api.c —— 公共 C ABI 的唯一入口层
 *
 * 这一层只做四件事，一行像素都不碰：
 *   ① 参数校验（空指针 / struct_size / 枚举取值范围 / 尺寸上限）；
 *   ② 句柄校验（是不是这个种类、有没有销毁过、是不是这个设备的）；
 *   ③ 按句柄找到后端，转发到对应的虚表函数；
 *   ④ 把后端的失败翻译成"<函数>: <原因> (file:line)"写进线程本地错误槽。
 *
 * 为什么校验放在这里而不是每个后端各写一遍：
 *   ABI 的行为必须是**后端无关**的 —— 上层换了后端，同样的误用必须得到同样的
 *   结果码。参数/句柄这类"与像素无关"的规则放一处，后端只剩"这台机器能不能
 *   画、画成什么样"的规则。也免得 NULL 后端（它的全部意义就是校验）和 CPU
 *   后端慢慢长出两套不一样的判断。
 *
 * 为什么 destroy 系列是 void 且幂等：
 *   C 里析构失败没有补救手段（设备已经在拆了）。重复销毁 / 无效句柄一律 no-op，
 *   保证"清理路径"永远不会把程序打断 —— 这是 docs/abi.md 的承诺之一。
 */
#include "bingpu_internal.h"

#include <string.h>

/* ---------------------------------------------------------------
 * 小工具
 * --------------------------------------------------------------- */
static const BgpuVt* vt_of(const void* obj) {
    const BgpuObjectBase* b = (const BgpuObjectBase*)obj;
    return bgpu_vt((BgpuBackendKind)b->backend);
}

static int same_device(const void* obj, BgpuDevice device) {
    const BgpuObjectBase* b = (const BgpuObjectBase*)obj;
    return b->device == device;
}

/* 句柄 → 对象；失败时写错误槽（错误串以公共函数名开头） */
static void* take_handle(uint64_t handle, uint32_t kind, const char* what,
                         const char* fn, const char* file, int line) {
    void* obj = bgpu_obj_get(handle, kind);
    if (!obj) {
        if (handle == BGPU_INVALID)
            bgpu_fail(BGPU_ERR_INVALID_HANDLE, fn, file, line, "%s 句柄是 0", what);
        else
            bgpu_fail(BGPU_ERR_INVALID_HANDLE, fn, file, line,
                      "%s 句柄无效（已销毁或不属于当前设备）", what);
    }
    return obj;
}

static BgpuResult settle(BgpuResult r, const char* fn, const char* file, int line) {
    if (r == BGPU_OK) return r;
    return bgpu_fail_from(r, fn, file, line);
}

#define SETTLE(call) settle((call), __func__, __FILE__, __LINE__)

/* 输出结构体：必须有 struct_size 且不小于当前已知布局。
 * 未来加字段是 ABI 兼容的（后端只写它认识的尾巴），但"比我知道的还小"说明
 * 调用方是拿旧头编译的 —— 那种情况下写进去就是越界，必须拒绝。 */
static BgpuResult check_out(uint32_t struct_size, size_t want, const char* what,
                            const char* fn, const char* file, int line) {
    if (!bgpu_size_ok(struct_size, want))
        return bgpu_fail(BGPU_ERR_INVALID_ARGUMENT, fn, file, line,
                         "%s.struct_size=%u 太小（至少要 %u）", what, struct_size,
                         (unsigned)want);
    return BGPU_OK;
}

/* ============================================================
 * 设备
 * ============================================================ */
BgpuResult bingpu_device_create(const BgpuDeviceDesc* desc, BgpuDevice* out_device) {
    if (!desc || !out_device)
        return bgpu_fail(BGPU_ERR_INVALID_ARGUMENT, __func__, __FILE__, __LINE__,
                         "desc/out_device 为空");
    *out_device = BGPU_INVALID;
    if (!bgpu_size_ok(desc->struct_size, sizeof(BgpuDeviceDesc)))
        return bgpu_fail(BGPU_ERR_INVALID_ARGUMENT, __func__, __FILE__, __LINE__,
                         "struct_size=%u 太小（至少要 %u）", desc->struct_size,
                         (unsigned)sizeof(BgpuDeviceDesc));

    const BgpuVt* vt = (desc->backend == BGPU_BACKEND_AUTO) ? bgpu_vt_auto()
                                                           : bgpu_vt(desc->backend);
    if (!vt)
        return bgpu_fail(BGPU_ERR_UNSUPPORTED, __func__, __FILE__, __LINE__, "%s：%s",
                         bingpu_backend_name(desc->backend),
                         bgpu_backend_unavailable_reason(desc->backend));
    if (!vt->available || !vt->available())
        return bgpu_fail(BGPU_ERR_UNSUPPORTED, __func__, __FILE__, __LINE__, "%s：%s",
                         vt->name ? vt->name : "backend",
                         bgpu_backend_unavailable_reason(vt->kind));

    if (desc->width == 0 || desc->height == 0)
        return bgpu_fail(BGPU_ERR_INVALID_ARGUMENT, __func__, __FILE__, __LINE__,
                         "尺寸 %ux%u 非法（宽高必须 > 0）", desc->width, desc->height);
    if (desc->width > BGPU_MAX_TEXTURE_SIZE || desc->height > BGPU_MAX_TEXTURE_SIZE)
        return bgpu_fail(BGPU_ERR_LIMIT, __func__, __FILE__, __LINE__,
                         "尺寸 %ux%u 超出上限 %u", desc->width, desc->height,
                         BGPU_MAX_TEXTURE_SIZE);
    if (desc->msaa_samples != 0 && desc->msaa_samples != 1 && desc->msaa_samples != 4 &&
        desc->msaa_samples != 8)
        return bgpu_fail(BGPU_ERR_INVALID_ARGUMENT, __func__, __FILE__, __LINE__,
                         "msaa_samples=%u 非法（只支持 1/4/8，0 视为 1）",
                         desc->msaa_samples);

    return SETTLE(vt->device_create(desc, out_device));
}

void bingpu_device_destroy(BgpuDevice device) {
    void* dev = bgpu_obj_get(device, BGPU_OBJ_DEVICE);
    if (!dev) return;                       /* 幂等：无效句柄什么也不做 */
    bgpu_obj_destroy_owned(device);         /* 最后一道保险：连子资源一起收 */
    bgpu_obj_destroy(device);
}

BgpuResult bingpu_device_info(BgpuDevice device, BgpuDeviceInfo* out_info) {
    void* dev = take_handle(device, BGPU_OBJ_DEVICE, "device", __func__, __FILE__, __LINE__);
    if (!dev) return BGPU_ERR_INVALID_HANDLE;
    if (!out_info)
        return bgpu_fail(BGPU_ERR_INVALID_ARGUMENT, __func__, __FILE__, __LINE__, "out_info 为空");
    BgpuResult r = check_out(out_info->struct_size, sizeof(*out_info), "out_info", __func__,
                             __FILE__, __LINE__);
    if (r != BGPU_OK) return r;
    vt_of(dev)->device_info(dev, out_info);
    return BGPU_OK;
}

BgpuResult bingpu_device_caps(BgpuDevice device, BgpuCaps* out_caps) {
    void* dev = take_handle(device, BGPU_OBJ_DEVICE, "device", __func__, __FILE__, __LINE__);
    if (!dev) return BGPU_ERR_INVALID_HANDLE;
    if (!out_caps)
        return bgpu_fail(BGPU_ERR_INVALID_ARGUMENT, __func__, __FILE__, __LINE__, "out_caps 为空");
    BgpuResult r = check_out(out_caps->struct_size, sizeof(*out_caps), "out_caps", __func__,
                             __FILE__, __LINE__);
    if (r != BGPU_OK) return r;
    vt_of(dev)->device_caps(dev, out_caps);
    return BGPU_OK;
}

BgpuResult bingpu_device_stats(BgpuDevice device, BgpuStats* out_stats) {
    void* dev = take_handle(device, BGPU_OBJ_DEVICE, "device", __func__, __FILE__, __LINE__);
    if (!dev) return BGPU_ERR_INVALID_HANDLE;
    if (!out_stats)
        return bgpu_fail(BGPU_ERR_INVALID_ARGUMENT, __func__, __FILE__, __LINE__, "out_stats 为空");
    BgpuResult r = check_out(out_stats->struct_size, sizeof(*out_stats), "out_stats",
                             __func__, __FILE__, __LINE__);
    if (r != BGPU_OK) return r;
    vt_of(dev)->device_stats(dev, out_stats);
    return BGPU_OK;
}

int bingpu_device_lost(BgpuDevice device) {
    void* dev = bgpu_obj_get(device, BGPU_OBJ_DEVICE);
    if (!dev) return 0;
    return vt_of(dev)->device_lost(dev) ? 1 : 0;
}

BgpuResult bingpu_device_wait_idle(BgpuDevice device) {
    void* dev = take_handle(device, BGPU_OBJ_DEVICE, "device", __func__, __FILE__, __LINE__);
    if (!dev) return BGPU_ERR_INVALID_HANDLE;
    /* CPU/NULL 都是同步执行，"等空闲"天然成立；真实后端在这里等队列。 */
    return BGPU_OK;
}

BgpuResult bingpu_poll(BgpuDevice device, BgpuEvent* out_event) {
    void* dev = take_handle(device, BGPU_OBJ_DEVICE, "device", __func__, __FILE__, __LINE__);
    if (!dev) return BGPU_ERR_INVALID_HANDLE;
    if (!out_event)
        return bgpu_fail(BGPU_ERR_INVALID_ARGUMENT, __func__, __FILE__, __LINE__, "out_event 为空");
    BgpuResult r = check_out(out_event->struct_size, sizeof(*out_event), "out_event",
                             __func__, __FILE__, __LINE__);
    if (r != BGPU_OK) return r;
    /* 唯一的取事件通道：没有事件时也必须填一个 NONE —— 调用方按它判断"队列空"。
     * CPU/NULL 后端不产生事件（没有设备丢失、没有交换链重建）。 */
    out_event->kind = BGPU_EVENT_NONE;
    out_event->a = 0;
    out_event->b = 0;
    return BGPU_OK;
}

/* ============================================================
 * 交换链 / 帧
 * ============================================================ */
BgpuResult bingpu_swapchain_create(BgpuDevice device, const BgpuSwapchainDesc* desc,
                                   BgpuSwapchain* out_swapchain) {
    void* dev = take_handle(device, BGPU_OBJ_DEVICE, "device", __func__, __FILE__, __LINE__);
    if (!dev) return BGPU_ERR_INVALID_HANDLE;
    if (!desc || !out_swapchain)
        return bgpu_fail(BGPU_ERR_INVALID_ARGUMENT, __func__, __FILE__, __LINE__,
                         "desc/out_swapchain 为空");
    *out_swapchain = BGPU_INVALID;
    if (!bgpu_size_ok(desc->struct_size, sizeof(BgpuSwapchainDesc)))
        return bgpu_fail(BGPU_ERR_INVALID_ARGUMENT, __func__, __FILE__, __LINE__,
                         "struct_size=%u 太小（至少要 %u）", desc->struct_size,
                         (unsigned)sizeof(BgpuSwapchainDesc));
    if (desc->width == 0 || desc->height == 0)
        return bgpu_fail(BGPU_ERR_INVALID_ARGUMENT, __func__, __FILE__, __LINE__,
                         "尺寸 %ux%u 非法", desc->width, desc->height);
    if (desc->width > BGPU_MAX_TEXTURE_SIZE || desc->height > BGPU_MAX_TEXTURE_SIZE)
        return bgpu_fail(BGPU_ERR_LIMIT, __func__, __FILE__, __LINE__,
                         "尺寸 %ux%u 超出上限 %u", desc->width, desc->height,
                         BGPU_MAX_TEXTURE_SIZE);
    if (bingpu_format_bytes(desc->format) == 0)
        return bgpu_fail(BGPU_ERR_INVALID_ARGUMENT, __func__, __FILE__, __LINE__,
                         "format=%d 非法", (int)desc->format);
    return SETTLE(vt_of(dev)->swapchain_create(dev, desc, out_swapchain));
}

void bingpu_swapchain_destroy(BgpuDevice device, BgpuSwapchain swapchain) {
    void* sc = bgpu_obj_get(swapchain, BGPU_OBJ_SWAPCHAIN);
    if (!sc || !same_device(sc, device)) return;
    bgpu_obj_destroy(swapchain);
}

BgpuResult bingpu_swapchain_resize(BgpuSwapchain swapchain, uint32_t width, uint32_t height) {
    void* sc = take_handle(swapchain, BGPU_OBJ_SWAPCHAIN, "swapchain", __func__, __FILE__,
                           __LINE__);
    if (!sc) return BGPU_ERR_INVALID_HANDLE;
    if (width == 0 || height == 0)
        return bgpu_fail(BGPU_ERR_INVALID_ARGUMENT, __func__, __FILE__, __LINE__,
                         "尺寸 %ux%u 非法", width, height);
    if (width > BGPU_MAX_TEXTURE_SIZE || height > BGPU_MAX_TEXTURE_SIZE)
        return bgpu_fail(BGPU_ERR_LIMIT, __func__, __FILE__, __LINE__,
                         "尺寸 %ux%u 超出上限 %u", width, height, BGPU_MAX_TEXTURE_SIZE);
    return SETTLE(vt_of(sc)->swapchain_resize(sc, width, height));
}

BgpuResult bingpu_swapchain_info(BgpuSwapchain swapchain, BgpuSwapchainInfo* out_info) {
    void* sc = take_handle(swapchain, BGPU_OBJ_SWAPCHAIN, "swapchain", __func__, __FILE__,
                           __LINE__);
    if (!sc) return BGPU_ERR_INVALID_HANDLE;
    if (!out_info)
        return bgpu_fail(BGPU_ERR_INVALID_ARGUMENT, __func__, __FILE__, __LINE__, "out_info 为空");
    BgpuResult r = check_out(out_info->struct_size, sizeof(*out_info), "out_info", __func__,
                             __FILE__, __LINE__);
    if (r != BGPU_OK) return r;
    vt_of(sc)->swapchain_info(sc, out_info);
    return BGPU_OK;
}

int bingpu_present_paces_frames(BgpuSwapchain swapchain) {
    void* sc = bgpu_obj_get(swapchain, BGPU_OBJ_SWAPCHAIN);
    if (!sc) return 0;
    BgpuSwapchainInfo info;
    memset(&info, 0, sizeof(info));
    info.struct_size = (uint32_t)sizeof(info);
    vt_of(sc)->swapchain_info(sc, &info);
    return info.present_paces_frames ? 1 : 0;
}

BgpuResult bingpu_frame_begin(BgpuSwapchain swapchain, BgpuCmd cmd, BgpuTexture* out_target) {
    void* sc = take_handle(swapchain, BGPU_OBJ_SWAPCHAIN, "swapchain", __func__, __FILE__,
                           __LINE__);
    if (!sc) return BGPU_ERR_INVALID_HANDLE;
    void* c = take_handle(cmd, BGPU_OBJ_CMD, "cmd", __func__, __FILE__, __LINE__);
    if (!c) return BGPU_ERR_INVALID_HANDLE;
    if (!out_target)
        return bgpu_fail(BGPU_ERR_INVALID_ARGUMENT, __func__, __FILE__, __LINE__, "out_target 为空");
    if (!same_device(sc, ((BgpuObjectBase*)c)->device))
        return bgpu_fail(BGPU_ERR_INVALID_HANDLE, __func__, __FILE__, __LINE__,
                         "swapchain 与 cmd 不属于同一个设备");
    *out_target = BGPU_INVALID;
    return SETTLE(vt_of(sc)->frame_begin(sc, c, out_target));
}

BgpuResult bingpu_frame_end(BgpuSwapchain swapchain, BgpuCmd cmd) {
    void* sc = take_handle(swapchain, BGPU_OBJ_SWAPCHAIN, "swapchain", __func__, __FILE__,
                           __LINE__);
    if (!sc) return BGPU_ERR_INVALID_HANDLE;
    void* c = take_handle(cmd, BGPU_OBJ_CMD, "cmd", __func__, __FILE__, __LINE__);
    if (!c) return BGPU_ERR_INVALID_HANDLE;
    if (!same_device(sc, ((BgpuObjectBase*)c)->device))
        return bgpu_fail(BGPU_ERR_INVALID_HANDLE, __func__, __FILE__, __LINE__,
                         "swapchain 与 cmd 不属于同一个设备");
    return SETTLE(vt_of(sc)->frame_end(sc, c));
}

BgpuResult bingpu_frame_read(BgpuSwapchain swapchain, void* dst, size_t dst_size,
                             uint32_t* out_stride) {
    void* sc = take_handle(swapchain, BGPU_OBJ_SWAPCHAIN, "swapchain", __func__, __FILE__,
                           __LINE__);
    if (!sc) return BGPU_ERR_INVALID_HANDLE;
    if (!dst || dst_size == 0)
        return bgpu_fail(BGPU_ERR_INVALID_ARGUMENT, __func__, __FILE__, __LINE__,
                         "dst 为空 / dst_size=0");
    return SETTLE(vt_of(sc)->frame_read(sc, dst, dst_size, out_stride));
}

/* ============================================================
 * 纹理
 * ============================================================ */
BgpuResult bingpu_texture_create(BgpuDevice device, const BgpuTextureDesc* desc,
                                 BgpuTexture* out_texture) {
    void* dev = take_handle(device, BGPU_OBJ_DEVICE, "device", __func__, __FILE__, __LINE__);
    if (!dev) return BGPU_ERR_INVALID_HANDLE;
    if (!desc || !out_texture)
        return bgpu_fail(BGPU_ERR_INVALID_ARGUMENT, __func__, __FILE__, __LINE__,
                         "desc/out_texture 为空");
    *out_texture = BGPU_INVALID;
    if (!bgpu_size_ok(desc->struct_size, sizeof(BgpuTextureDesc)))
        return bgpu_fail(BGPU_ERR_INVALID_ARGUMENT, __func__, __FILE__, __LINE__,
                         "struct_size=%u 太小（至少要 %u）", desc->struct_size,
                         (unsigned)sizeof(BgpuTextureDesc));
    if (desc->width == 0 || desc->height == 0)
        return bgpu_fail(BGPU_ERR_INVALID_ARGUMENT, __func__, __FILE__, __LINE__,
                         "尺寸 %ux%u 非法", desc->width, desc->height);
    if (desc->width > BGPU_MAX_TEXTURE_SIZE || desc->height > BGPU_MAX_TEXTURE_SIZE)
        return bgpu_fail(BGPU_ERR_LIMIT, __func__, __FILE__, __LINE__,
                         "尺寸 %ux%u 超出上限 %u", desc->width, desc->height,
                         BGPU_MAX_TEXTURE_SIZE);
    if (bingpu_format_bytes(desc->format) == 0)
        return bgpu_fail(BGPU_ERR_INVALID_ARGUMENT, __func__, __FILE__, __LINE__,
                         "format=%d 非法", (int)desc->format);
    if (desc->mip_levels > 16)
        return bgpu_fail(BGPU_ERR_LIMIT, __func__, __FILE__, __LINE__,
                         "mip_levels=%u 超出上限 16", desc->mip_levels);
    return SETTLE(vt_of(dev)->texture_create(dev, desc, out_texture));
}

void bingpu_texture_destroy(BgpuDevice device, BgpuTexture texture) {
    void* tex = bgpu_obj_get(texture, BGPU_OBJ_TEXTURE);
    if (!tex || !same_device(tex, device)) return;
    if (((BgpuObjectBase*)tex)->obj_flags & BGPU_OBJ_F_BORROWED) {
        bgpu_fail(BGPU_ERR_STATE, __func__, __FILE__, __LINE__,
                  "交换链借出的目标纹理不许 destroy（有效期到 frame_end）");
        return;
    }
    bgpu_obj_destroy(texture);
}

BgpuResult bingpu_texture_info(BgpuTexture texture, BgpuTextureInfo* out_info) {
    void* tex = take_handle(texture, BGPU_OBJ_TEXTURE, "texture", __func__, __FILE__, __LINE__);
    if (!tex) return BGPU_ERR_INVALID_HANDLE;
    if (!out_info)
        return bgpu_fail(BGPU_ERR_INVALID_ARGUMENT, __func__, __FILE__, __LINE__, "out_info 为空");
    BgpuResult r = check_out(out_info->struct_size, sizeof(*out_info), "out_info", __func__,
                             __FILE__, __LINE__);
    if (r != BGPU_OK) return r;
    vt_of(tex)->texture_info(tex, out_info);
    return BGPU_OK;
}

BgpuResult bingpu_texture_upload(BgpuTexture texture, const BgpuBox* box, const void* pixels,
                                 uint32_t stride) {
    void* tex = take_handle(texture, BGPU_OBJ_TEXTURE, "texture", __func__, __FILE__, __LINE__);
    if (!tex) return BGPU_ERR_INVALID_HANDLE;
    if (!pixels)
        return bgpu_fail(BGPU_ERR_INVALID_ARGUMENT, __func__, __FILE__, __LINE__, "pixels 为空");
    return SETTLE(vt_of(tex)->texture_upload(tex, box, pixels, stride));
}

BgpuResult bingpu_texture_read(BgpuTexture texture, void* dst, size_t dst_size,
                               uint32_t* out_stride) {
    void* tex = take_handle(texture, BGPU_OBJ_TEXTURE, "texture", __func__, __FILE__, __LINE__);
    if (!tex) return BGPU_ERR_INVALID_HANDLE;
    if (!dst || dst_size == 0)
        return bgpu_fail(BGPU_ERR_INVALID_ARGUMENT, __func__, __FILE__, __LINE__,
                         "dst 为空 / dst_size=0");
    return SETTLE(vt_of(tex)->texture_read(tex, dst, dst_size, out_stride));
}

/* ============================================================
 * 缓冲
 * ============================================================ */
BgpuResult bingpu_buffer_create(BgpuDevice device, const BgpuBufferDesc* desc,
                                BgpuBuffer* out_buffer) {
    void* dev = take_handle(device, BGPU_OBJ_DEVICE, "device", __func__, __FILE__, __LINE__);
    if (!dev) return BGPU_ERR_INVALID_HANDLE;
    if (!desc || !out_buffer)
        return bgpu_fail(BGPU_ERR_INVALID_ARGUMENT, __func__, __FILE__, __LINE__,
                         "desc/out_buffer 为空");
    *out_buffer = BGPU_INVALID;
    if (!bgpu_size_ok(desc->struct_size, sizeof(BgpuBufferDesc)))
        return bgpu_fail(BGPU_ERR_INVALID_ARGUMENT, __func__, __FILE__, __LINE__,
                         "struct_size=%u 太小（至少要 %u）", desc->struct_size,
                         (unsigned)sizeof(BgpuBufferDesc));
    if (desc->size == 0)
        return bgpu_fail(BGPU_ERR_INVALID_ARGUMENT, __func__, __FILE__, __LINE__,
                         "size=0（缓冲至少 1 字节）");
    return SETTLE(vt_of(dev)->buffer_create(dev, desc, out_buffer));
}

void bingpu_buffer_destroy(BgpuDevice device, BgpuBuffer buffer) {
    void* buf = bgpu_obj_get(buffer, BGPU_OBJ_BUFFER);
    if (!buf || !same_device(buf, device)) return;
    bgpu_obj_destroy(buffer);
}

BgpuResult bingpu_buffer_update(BgpuBuffer buffer, uint32_t offset, const void* data,
                                uint32_t size) {
    void* buf = take_handle(buffer, BGPU_OBJ_BUFFER, "buffer", __func__, __FILE__, __LINE__);
    if (!buf) return BGPU_ERR_INVALID_HANDLE;
    if (!data || size == 0)
        return bgpu_fail(BGPU_ERR_INVALID_ARGUMENT, __func__, __FILE__, __LINE__,
                         "data 为空 / size=0");
    return SETTLE(vt_of(buf)->buffer_update(buf, offset, data, size));
}

/* ============================================================
 * 采样器 / 管线
 * ============================================================ */
BgpuResult bingpu_sampler_create(BgpuDevice device, const BgpuSamplerDesc* desc,
                                 BgpuSampler* out_sampler) {
    void* dev = take_handle(device, BGPU_OBJ_DEVICE, "device", __func__, __FILE__, __LINE__);
    if (!dev) return BGPU_ERR_INVALID_HANDLE;
    if (!desc || !out_sampler)
        return bgpu_fail(BGPU_ERR_INVALID_ARGUMENT, __func__, __FILE__, __LINE__,
                         "desc/out_sampler 为空");
    *out_sampler = BGPU_INVALID;
    if (!bgpu_size_ok(desc->struct_size, sizeof(BgpuSamplerDesc)))
        return bgpu_fail(BGPU_ERR_INVALID_ARGUMENT, __func__, __FILE__, __LINE__,
                         "struct_size=%u 太小（至少要 %u）", desc->struct_size,
                         (unsigned)sizeof(BgpuSamplerDesc));
    if ((int)desc->min_filter < 0 || (int)desc->min_filter > 1 ||
        (int)desc->mag_filter < 0 || (int)desc->mag_filter > 1 ||
        (int)desc->wrap_u < 0 || (int)desc->wrap_u > 1 ||
        (int)desc->wrap_v < 0 || (int)desc->wrap_v > 1)
        return bgpu_fail(BGPU_ERR_INVALID_ARGUMENT, __func__, __FILE__, __LINE__,
                         "filter/wrap 取值非法（filter 0..1，wrap 0..1）");
    return SETTLE(vt_of(dev)->sampler_create(dev, desc, out_sampler));
}

void bingpu_sampler_destroy(BgpuDevice device, BgpuSampler sampler) {
    void* s = bgpu_obj_get(sampler, BGPU_OBJ_SAMPLER);
    if (!s || !same_device(s, device)) return;
    bgpu_obj_destroy(sampler);
}

BgpuResult bingpu_pipeline_create(BgpuDevice device, const BgpuPipelineDesc* desc,
                                  BgpuPipeline* out_pipeline) {
    void* dev = take_handle(device, BGPU_OBJ_DEVICE, "device", __func__, __FILE__, __LINE__);
    if (!dev) return BGPU_ERR_INVALID_HANDLE;
    if (!desc || !out_pipeline)
        return bgpu_fail(BGPU_ERR_INVALID_ARGUMENT, __func__, __FILE__, __LINE__,
                         "desc/out_pipeline 为空");
    *out_pipeline = BGPU_INVALID;
    if (!bgpu_size_ok(desc->struct_size, sizeof(BgpuPipelineDesc)))
        return bgpu_fail(BGPU_ERR_INVALID_ARGUMENT, __func__, __FILE__, __LINE__,
                         "struct_size=%u 太小（至少要 %u）", desc->struct_size,
                         (unsigned)sizeof(BgpuPipelineDesc));
    if ((int)desc->vertex_layout < 0 || (int)desc->vertex_layout > 1)
        return bgpu_fail(BGPU_ERR_INVALID_ARGUMENT, __func__, __FILE__, __LINE__,
                         "vertex_layout=%d 非法", (int)desc->vertex_layout);
    if ((int)desc->blend < 0 || (int)desc->blend > 4)
        return bgpu_fail(BGPU_ERR_INVALID_ARGUMENT, __func__, __FILE__, __LINE__,
                         "blend=%d 非法（0..4）", (int)desc->blend);
    return SETTLE(vt_of(dev)->pipeline_create(dev, desc, out_pipeline));
}

void bingpu_pipeline_destroy(BgpuDevice device, BgpuPipeline pipeline) {
    void* p = bgpu_obj_get(pipeline, BGPU_OBJ_PIPELINE);
    if (!p || !same_device(p, device)) return;
    bgpu_obj_destroy(pipeline);
}

/* ============================================================
 * 命令缓冲
 * ============================================================ */
BgpuResult bingpu_cmd_create(BgpuDevice device, BgpuCmd* out_cmd) {
    void* dev = take_handle(device, BGPU_OBJ_DEVICE, "device", __func__, __FILE__, __LINE__);
    if (!dev) return BGPU_ERR_INVALID_HANDLE;
    if (!out_cmd)
        return bgpu_fail(BGPU_ERR_INVALID_ARGUMENT, __func__, __FILE__, __LINE__, "out_cmd 为空");
    *out_cmd = BGPU_INVALID;
    return SETTLE(vt_of(dev)->cmd_create(dev, out_cmd));
}

void bingpu_cmd_destroy(BgpuDevice device, BgpuCmd cmd) {
    void* c = bgpu_obj_get(cmd, BGPU_OBJ_CMD);
    if (!c || !same_device(c, device)) return;
    bgpu_obj_destroy(cmd);
}

static void* cmd_of(BgpuCmd cmd, const char* fn, const char* file, int line) {
    return take_handle(cmd, BGPU_OBJ_CMD, "cmd", fn, file, line);
}

BgpuResult bingpu_cmd_begin_render(BgpuCmd cmd, const BgpuRenderBeginDesc* desc) {
    void* c = cmd_of(cmd, __func__, __FILE__, __LINE__);
    if (!c) return BGPU_ERR_INVALID_HANDLE;
    if (!desc)
        return bgpu_fail(BGPU_ERR_INVALID_ARGUMENT, __func__, __FILE__, __LINE__, "desc 为空");
    if (!bgpu_size_ok(desc->struct_size, sizeof(BgpuRenderBeginDesc)))
        return bgpu_fail(BGPU_ERR_INVALID_ARGUMENT, __func__, __FILE__, __LINE__,
                         "struct_size=%u 太小（至少要 %u）", desc->struct_size,
                         (unsigned)sizeof(BgpuRenderBeginDesc));
    if (desc->target == BGPU_INVALID)
        return bgpu_fail(BGPU_ERR_INVALID_ARGUMENT, __func__, __FILE__, __LINE__,
                         "target 为空（渲染必须有一个目标纹理）");
    void* target = bgpu_obj_get(desc->target, BGPU_OBJ_TEXTURE);
    if (!target)
        return bgpu_fail(BGPU_ERR_INVALID_HANDLE, __func__, __FILE__, __LINE__,
                         "target 纹理句柄无效");
    const BgpuDevice dev = ((BgpuObjectBase*)c)->device;
    if (!same_device(target, dev))
        return bgpu_fail(BGPU_ERR_INVALID_HANDLE, __func__, __FILE__, __LINE__,
                         "target 纹理不属于当前设备");
    if (desc->depth_target != BGPU_INVALID) {
        void* depth = bgpu_obj_get(desc->depth_target, BGPU_OBJ_TEXTURE);
        if (!depth)
            return bgpu_fail(BGPU_ERR_INVALID_HANDLE, __func__, __FILE__, __LINE__,
                             "depth_target 纹理句柄无效");
        if (!same_device(depth, dev))
            return bgpu_fail(BGPU_ERR_INVALID_HANDLE, __func__, __FILE__, __LINE__,
                             "depth_target 不属于当前设备");
    }
    return SETTLE(vt_of(c)->cmd_begin_render(c, desc));
}

BgpuResult bingpu_cmd_end_render(BgpuCmd cmd) {
    void* c = cmd_of(cmd, __func__, __FILE__, __LINE__);
    if (!c) return BGPU_ERR_INVALID_HANDLE;
    return SETTLE(vt_of(c)->cmd_end_render(c));
}

BgpuResult bingpu_cmd_set_viewport(BgpuCmd cmd, float x, float y, float w, float h) {
    void* c = cmd_of(cmd, __func__, __FILE__, __LINE__);
    if (!c) return BGPU_ERR_INVALID_HANDLE;
    if (!(w > 0.0f) || !(h > 0.0f))
        return bgpu_fail(BGPU_ERR_INVALID_ARGUMENT, __func__, __FILE__, __LINE__,
                         "viewport 尺寸 %gx%g 非法（必须 > 0）", (double)w, (double)h);
    return SETTLE(vt_of(c)->cmd_set_viewport(c, x, y, w, h));
}

BgpuResult bingpu_cmd_set_scissor(BgpuCmd cmd, int32_t x, int32_t y, int32_t w, int32_t h) {
    void* c = cmd_of(cmd, __func__, __FILE__, __LINE__);
    if (!c) return BGPU_ERR_INVALID_HANDLE;
    return SETTLE(vt_of(c)->cmd_set_scissor(c, x, y, w, h));
}

BgpuResult bingpu_cmd_set_pipeline(BgpuCmd cmd, BgpuPipeline pipeline) {
    void* c = cmd_of(cmd, __func__, __FILE__, __LINE__);
    if (!c) return BGPU_ERR_INVALID_HANDLE;
    void* p = take_handle(pipeline, BGPU_OBJ_PIPELINE, "pipeline", __func__, __FILE__, __LINE__);
    if (!p) return BGPU_ERR_INVALID_HANDLE;
    if (!same_device(p, ((BgpuObjectBase*)c)->device))
        return bgpu_fail(BGPU_ERR_INVALID_HANDLE, __func__, __FILE__, __LINE__,
                         "pipeline 不属于当前设备");
    return SETTLE(vt_of(c)->cmd_set_pipeline(c, p));
}

BgpuResult bingpu_cmd_set_texture(BgpuCmd cmd, BgpuTexture texture, BgpuSampler sampler) {
    void* c = cmd_of(cmd, __func__, __FILE__, __LINE__);
    if (!c) return BGPU_ERR_INVALID_HANDLE;
    void* tex = NULL;
    void* smp = NULL;
    if (texture != BGPU_INVALID) {
        tex = take_handle(texture, BGPU_OBJ_TEXTURE, "texture", __func__, __FILE__, __LINE__);
        if (!tex) return BGPU_ERR_INVALID_HANDLE;
        if (!same_device(tex, ((BgpuObjectBase*)c)->device))
            return bgpu_fail(BGPU_ERR_INVALID_HANDLE, __func__, __FILE__, __LINE__,
                             "texture 不属于当前设备");
    }
    if (sampler != BGPU_INVALID) {
        smp = take_handle(sampler, BGPU_OBJ_SAMPLER, "sampler", __func__, __FILE__, __LINE__);
        if (!smp) return BGPU_ERR_INVALID_HANDLE;
        if (!same_device(smp, ((BgpuObjectBase*)c)->device))
            return bgpu_fail(BGPU_ERR_INVALID_HANDLE, __func__, __FILE__, __LINE__,
                             "sampler 不属于当前设备");
    }
    return SETTLE(vt_of(c)->cmd_set_texture(c, tex, smp));
}

BgpuResult bingpu_cmd_set_affine(BgpuCmd cmd, const float xf[6]) {
    void* c = cmd_of(cmd, __func__, __FILE__, __LINE__);
    if (!c) return BGPU_ERR_INVALID_HANDLE;
    return SETTLE(vt_of(c)->cmd_set_affine(c, xf));
}

BgpuResult bingpu_cmd_draw_quads(BgpuCmd cmd, const BgpuVertex* vertices, uint32_t quad_count) {
    void* c = cmd_of(cmd, __func__, __FILE__, __LINE__);
    if (!c) return BGPU_ERR_INVALID_HANDLE;
    if (quad_count == 0) return BGPU_OK;                      /* 空绘制是合法的 no-op */
    if (!vertices)
        return bgpu_fail(BGPU_ERR_INVALID_ARGUMENT, __func__, __FILE__, __LINE__,
                         "vertices 为空（quad_count=%u）", quad_count);
    if (quad_count > BGPU_MAX_QUADS_PER_DRAW)
        return bgpu_fail(BGPU_ERR_LIMIT, __func__, __FILE__, __LINE__,
                         "quad_count=%u 超出单次上限 %u", quad_count, BGPU_MAX_QUADS_PER_DRAW);
    return SETTLE(vt_of(c)->cmd_draw_quads(c, vertices, quad_count));
}

BgpuResult bingpu_cmd_draw_indexed(BgpuCmd cmd, BgpuBuffer vertex_buffer, uint32_t vertex_offset,
                                   BgpuBuffer index_buffer, uint32_t index_count,
                                   uint32_t index_offset) {
    void* c = cmd_of(cmd, __func__, __FILE__, __LINE__);
    if (!c) return BGPU_ERR_INVALID_HANDLE;
    void* vb = take_handle(vertex_buffer, BGPU_OBJ_BUFFER, "vertex_buffer", __func__, __FILE__,
                           __LINE__);
    if (!vb) return BGPU_ERR_INVALID_HANDLE;
    void* ib = take_handle(index_buffer, BGPU_OBJ_BUFFER, "index_buffer", __func__, __FILE__,
                           __LINE__);
    if (!ib) return BGPU_ERR_INVALID_HANDLE;
    const BgpuDevice dev = ((BgpuObjectBase*)c)->device;
    if (!same_device(vb, dev) || !same_device(ib, dev))
        return bgpu_fail(BGPU_ERR_INVALID_HANDLE, __func__, __FILE__, __LINE__,
                         "顶点/索引缓冲不属于当前设备");
    if (index_count == 0) return BGPU_OK;
    return SETTLE(vt_of(c)->cmd_draw_indexed(c, vb, vertex_offset, ib, index_count, index_offset));
}
