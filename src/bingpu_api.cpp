/* bingpu_api.cpp —— 公共 C ABI 的唯一入口层
 *
 * 这一层只做四件事，一行像素都不碰：
 *   ① 参数校验（空指针 / struct_size / 枚举范围 / 尺寸上限）；
 *   ② 句柄校验（种类、是否已销毁、是否属于当前设备）；
 *   ③ 把句柄解析成对象，转发给 `obj->be->xxx(...)`；
 *   ④ 把实现层的失败翻译成 `<函数>: <原因> (file:line)` 写进线程本地错误槽。
 *
 * **异常绝不越过 ABI**：每个入口都是顶层 try/catch ——
 *   std::bad_alloc  → BGPU_ERR_OUT_OF_MEMORY
 *   其它 std::exception → BGPU_ERR_BACKEND_FAILED（what() 进错误串）
 *   未知异常        → 同上（错误串写"未知异常"）
 * 注意：这些函数在**公共头里没有 noexcept**（头是冻结的 C ABI，不能动），而 C++ 要求
 * 声明与定义的异常规格一致，所以不能在这边的定义上补 noexcept。等价的保证由 try/catch
 * 提供：没有任何异常能穿出这一层。这条写进了 docs/abi.md。
 *
 * 为什么校验放在这里而不是每个后端各写一遍：
 *   ABI 的行为必须**后端无关** —— 换后端时同样的误用必须得到同样的结果码。参数/句柄
 *   这类"与像素无关"的规则放一处，后端只剩"这台机器能不能画、画成什么样"。
 */
#include "bingpu_backend.h"

#include <cstring>

namespace {

using namespace bgpu;

/* 句柄 → 对象；失败写错误（以当前公共函数名开头）并返回 nullptr */
ObjectBase* take(uint64_t handle, uint32_t kind, const char* what, const char* file, int line) {
    ObjectBase* o = lookupObject(handle, kind);
    if (!o) {
        if (handle == BGPU_INVALID) {
            fail(BGPU_ERR_INVALID_HANDLE, file, line, cat(what, " 句柄是 0"));
        } else {
            fail(BGPU_ERR_INVALID_HANDLE, file, line,
                 cat(what, " 句柄无效（已销毁或不属于当前设备）"));
        }
    }
    return o;
}

Device* takeDevice(BgpuDevice handle, const char* file, int line) {
    return static_cast<Device*>(take(handle, kObjDevice, "device", file, line));
}

/* 输出结构体：必须有 struct_size 且不小于当前已知布局（未来加字段是 ABI 兼容的） */
BgpuResult checkOut(uint32_t structSize, size_t want, const char* what, const char* file,
                    int line) {
    if (structSize < want) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, file, line,
                    cat(what, ".struct_size=", structSize, " 太小（至少要 ",
                        static_cast<unsigned>(want), "）"));
    }
    return BGPU_OK;
}

bool sameDevice(const ObjectBase* o, BgpuDevice dev) noexcept { return o->device == dev; }

}  // namespace

/* 顶层异常兜底（见文件头） */
#define BGPU_API_TRY try {
#define BGPU_API_CATCH_RESULT()                                                     \
    }                                                                               \
    catch (const std::bad_alloc&) { return bgpu::failOom(__FILE__, __LINE__); }      \
    catch (const std::exception& e) {                                               \
        return bgpu::failExc(__FILE__, __LINE__, e.what());                         \
    }                                                                               \
    catch (...) { return bgpu::failUnknown(__FILE__, __LINE__); }
#define BGPU_API_CATCH_VOID()                                                       \
    }                                                                               \
    catch (...) { bgpu::failUnknown(__FILE__, __LINE__); }
#define BGPU_API_CATCH_INT()                                                        \
    }                                                                               \
    catch (...) {                                                                   \
        bgpu::failUnknown(__FILE__, __LINE__);                                      \
        return 0;                                                                   \
    }

/* ============================================================
 * 设备
 * ============================================================ */
BgpuResult bingpu_device_create(const BgpuDeviceDesc* desc, BgpuDevice* out_device) {
    ApiScope scope("bingpu_device_create");
    const unsigned s0 = errorSeq();
    BGPU_API_TRY
    if (!desc || !out_device) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__, "desc/out_device 为空");
    }
    *out_device = BGPU_INVALID;
    BgpuResult r = checkOut(desc->struct_size, sizeof(BgpuDeviceDesc), "desc", __FILE__,
                            __LINE__);
    if (r != BGPU_OK) return r;

    Backend* be = (desc->backend == BGPU_BACKEND_AUTO) ? backendAuto() : backendOf(desc->backend);
    if (!be) {
        return fail(BGPU_ERR_UNSUPPORTED, __FILE__, __LINE__,
                    cat(backendName(desc->backend), "：", backendUnavailableReason(desc->backend)));
    }
    if (!be->available()) {
        return fail(BGPU_ERR_UNSUPPORTED, __FILE__, __LINE__,
                    cat(be->name(), "：", backendUnavailableReason(be->kind())));
    }
    if (desc->width == 0 || desc->height == 0) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__,
                    cat("尺寸 ", desc->width, "x", desc->height, " 非法（宽高必须 > 0）"));
    }
    if (desc->width > BGPU_MAX_TEXTURE_SIZE || desc->height > BGPU_MAX_TEXTURE_SIZE) {
        return fail(BGPU_ERR_LIMIT, __FILE__, __LINE__,
                    cat("尺寸 ", desc->width, "x", desc->height, " 超出上限 ",
                        BGPU_MAX_TEXTURE_SIZE));
    }
    if (desc->msaa_samples != 0 && desc->msaa_samples != 1 && desc->msaa_samples != 4 &&
        desc->msaa_samples != 8) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__,
                    cat("msaa_samples=", desc->msaa_samples,
                        " 非法（只支持 1/4/8，0 视为 1）"));
    }
    return settle(be->createDevice(*desc, out_device), __FILE__, __LINE__, s0);
    BGPU_API_CATCH_RESULT()
}

void bingpu_device_destroy(BgpuDevice device) {
    ApiScope scope("bingpu_device_destroy");
    BGPU_API_TRY
    Device* dev = static_cast<Device*>(lookupObject(device, kObjDevice));
    if (!dev) return;                      /* 幂等：无效句柄什么也不做 */
    destroyOwnedObjects(device);           /* 最后一道保险：连子资源一起收 */
    destroyObject(device);
    BGPU_API_CATCH_VOID()
}

BgpuResult bingpu_device_info(BgpuDevice device, BgpuDeviceInfo* out_info) {
    ApiScope scope("bingpu_device_info");
    const unsigned s0 = errorSeq();
    BGPU_API_TRY
    Device* dev = takeDevice(device, __FILE__, __LINE__);
    if (!dev) return BGPU_ERR_INVALID_HANDLE;
    if (!out_info) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__, "out_info 为空");
    }
    BgpuResult r = checkOut(out_info->struct_size, sizeof(*out_info), "out_info", __FILE__,
                            __LINE__);
    if (r != BGPU_OK) return r;
    dev->be->deviceInfo(*dev, *out_info);
    return settle(BGPU_OK, __FILE__, __LINE__, s0);
    BGPU_API_CATCH_RESULT()
}

BgpuResult bingpu_device_caps(BgpuDevice device, BgpuCaps* out_caps) {
    ApiScope scope("bingpu_device_caps");
    BGPU_API_TRY
    Device* dev = takeDevice(device, __FILE__, __LINE__);
    if (!dev) return BGPU_ERR_INVALID_HANDLE;
    if (!out_caps) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__, "out_caps 为空");
    }
    BgpuResult r = checkOut(out_caps->struct_size, sizeof(*out_caps), "out_caps", __FILE__,
                            __LINE__);
    if (r != BGPU_OK) return r;
    dev->be->deviceCaps(*dev, *out_caps);
    return BGPU_OK;
    BGPU_API_CATCH_RESULT()
}

BgpuResult bingpu_device_stats(BgpuDevice device, BgpuStats* out_stats) {
    ApiScope scope("bingpu_device_stats");
    BGPU_API_TRY
    Device* dev = takeDevice(device, __FILE__, __LINE__);
    if (!dev) return BGPU_ERR_INVALID_HANDLE;
    if (!out_stats) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__, "out_stats 为空");
    }
    BgpuResult r = checkOut(out_stats->struct_size, sizeof(*out_stats), "out_stats", __FILE__,
                            __LINE__);
    if (r != BGPU_OK) return r;
    dev->be->deviceStats(*dev, *out_stats);
    return BGPU_OK;
    BGPU_API_CATCH_RESULT()
}

int bingpu_device_lost(BgpuDevice device) {
    ApiScope scope("bingpu_device_lost");
    BGPU_API_TRY
    Device* dev = static_cast<Device*>(lookupObject(device, kObjDevice));
    if (!dev) return 0;
    return dev->be->deviceLost(*dev) ? 1 : 0;
    BGPU_API_CATCH_INT()
}

BgpuResult bingpu_device_wait_idle(BgpuDevice device) {
    ApiScope scope("bingpu_device_wait_idle");
    BGPU_API_TRY
    Device* dev = static_cast<Device*>(lookupObject(device, kObjDevice));
    if (!dev) {
        return fail(BGPU_ERR_INVALID_HANDLE, __FILE__, __LINE__, "device 句柄无效");
    }
    /* CPU/NULL 同步执行，"等空闲"天然成立；真实后端在这里等队列（各自实现）。 */
    return dev->be->deviceWaitIdle(*dev);
    BGPU_API_CATCH_RESULT()
}

BgpuResult bingpu_poll(BgpuDevice device, BgpuEvent* out_event) {
    ApiScope scope("bingpu_poll");
    BGPU_API_TRY
    if (!lookupObject(device, kObjDevice)) {
        return fail(BGPU_ERR_INVALID_HANDLE, __FILE__, __LINE__, "device 句柄无效");
    }
    if (!out_event) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__, "out_event 为空");
    }
    BgpuResult r = checkOut(out_event->struct_size, sizeof(*out_event), "out_event", __FILE__,
                            __LINE__);
    if (r != BGPU_OK) return r;
    /* 唯一的取事件通道：没有事件时也必须填一个 NONE，调用方按它判断"队列空"。
     * CPU/NULL 后端不产生事件（没有设备丢失、没有交换链重建）。 */
    out_event->kind = BGPU_EVENT_NONE;
    out_event->a = 0;
    out_event->b = 0;
    return BGPU_OK;
    BGPU_API_CATCH_RESULT()
}

/* ============================================================
 * 交换链 / 帧
 * ============================================================ */
BgpuResult bingpu_swapchain_create(BgpuDevice device, const BgpuSwapchainDesc* desc,
                                   BgpuSwapchain* out_swapchain) {
    ApiScope scope("bingpu_swapchain_create");
    const unsigned s0 = errorSeq();
    BGPU_API_TRY
    Device* dev = takeDevice(device, __FILE__, __LINE__);
    if (!dev) return BGPU_ERR_INVALID_HANDLE;
    if (!desc || !out_swapchain) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__, "desc/out_swapchain 为空");
    }
    *out_swapchain = BGPU_INVALID;
    BgpuResult r = checkOut(desc->struct_size, sizeof(BgpuSwapchainDesc), "desc", __FILE__,
                            __LINE__);
    if (r != BGPU_OK) return r;
    if (desc->width == 0 || desc->height == 0) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__,
                    cat("尺寸 ", desc->width, "x", desc->height, " 非法"));
    }
    if (desc->width > BGPU_MAX_TEXTURE_SIZE || desc->height > BGPU_MAX_TEXTURE_SIZE) {
        return fail(BGPU_ERR_LIMIT, __FILE__, __LINE__,
                    cat("尺寸 ", desc->width, "x", desc->height, " 超出上限 ",
                        BGPU_MAX_TEXTURE_SIZE));
    }
    if (bingpu_format_bytes(desc->format) == 0) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__,
                    cat("format=", static_cast<int>(desc->format), " 非法"));
    }
    return settle(dev->be->createSwapchain(*dev, *desc, out_swapchain), __FILE__, __LINE__, s0);
    BGPU_API_CATCH_RESULT()
}

void bingpu_swapchain_destroy(BgpuDevice device, BgpuSwapchain swapchain) {
    ApiScope scope("bingpu_swapchain_destroy");
    BGPU_API_TRY
    Swapchain* sc = static_cast<Swapchain*>(lookupObject(swapchain, kObjSwapchain));
    if (!sc || !sameDevice(sc, device)) return;
    destroyObject(swapchain);
    BGPU_API_CATCH_VOID()
}

BgpuResult bingpu_swapchain_resize(BgpuSwapchain swapchain, uint32_t width, uint32_t height) {
    ApiScope scope("bingpu_swapchain_resize");
    const unsigned s0 = errorSeq();
    BGPU_API_TRY
    Swapchain* sc = static_cast<Swapchain*>(
        take(swapchain, kObjSwapchain, "swapchain", __FILE__, __LINE__));
    if (!sc) return BGPU_ERR_INVALID_HANDLE;
    if (width == 0 || height == 0) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__,
                    cat("尺寸 ", width, "x", height, " 非法"));
    }
    if (width > BGPU_MAX_TEXTURE_SIZE || height > BGPU_MAX_TEXTURE_SIZE) {
        return fail(BGPU_ERR_LIMIT, __FILE__, __LINE__,
                    cat("尺寸 ", width, "x", height, " 超出上限 ", BGPU_MAX_TEXTURE_SIZE));
    }
    return settle(sc->be->resizeSwapchain(*sc, width, height), __FILE__, __LINE__, s0);
    BGPU_API_CATCH_RESULT()
}

BgpuResult bingpu_swapchain_info(BgpuSwapchain swapchain, BgpuSwapchainInfo* out_info) {
    ApiScope scope("bingpu_swapchain_info");
    BGPU_API_TRY
    Swapchain* sc = static_cast<Swapchain*>(
        take(swapchain, kObjSwapchain, "swapchain", __FILE__, __LINE__));
    if (!sc) return BGPU_ERR_INVALID_HANDLE;
    if (!out_info) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__, "out_info 为空");
    }
    BgpuResult r = checkOut(out_info->struct_size, sizeof(*out_info), "out_info", __FILE__,
                            __LINE__);
    if (r != BGPU_OK) return r;
    sc->be->swapchainInfo(*sc, *out_info);
    return BGPU_OK;
    BGPU_API_CATCH_RESULT()
}

int bingpu_present_paces_frames(BgpuSwapchain swapchain) {
    ApiScope scope("bingpu_present_paces_frames");
    BGPU_API_TRY
    Swapchain* sc = static_cast<Swapchain*>(lookupObject(swapchain, kObjSwapchain));
    if (!sc) return 0;
    BgpuSwapchainInfo info;
    std::memset(&info, 0, sizeof(info));
    info.struct_size = static_cast<uint32_t>(sizeof(info));
    sc->be->swapchainInfo(*sc, info);
    return info.present_paces_frames ? 1 : 0;
    BGPU_API_CATCH_INT()
}

BgpuResult bingpu_frame_begin(BgpuSwapchain swapchain, BgpuCmd cmd, BgpuTexture* out_target) {
    ApiScope scope("bingpu_frame_begin");
    const unsigned s0 = errorSeq();
    BGPU_API_TRY
    Swapchain* sc = static_cast<Swapchain*>(
        take(swapchain, kObjSwapchain, "swapchain", __FILE__, __LINE__));
    if (!sc) return BGPU_ERR_INVALID_HANDLE;
    Cmd* c = static_cast<Cmd*>(take(cmd, kObjCmd, "cmd", __FILE__, __LINE__));
    if (!c) return BGPU_ERR_INVALID_HANDLE;
    if (!out_target) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__, "out_target 为空");
    }
    if (!sameDevice(sc, c->device)) {
        return fail(BGPU_ERR_INVALID_HANDLE, __FILE__, __LINE__,
                    "swapchain 与 cmd 不属于同一个设备");
    }
    *out_target = BGPU_INVALID;
    return settle(sc->be->frameBegin(*sc, *c, out_target), __FILE__, __LINE__, s0);
    BGPU_API_CATCH_RESULT()
}

BgpuResult bingpu_frame_end(BgpuSwapchain swapchain, BgpuCmd cmd) {
    ApiScope scope("bingpu_frame_end");
    const unsigned s0 = errorSeq();
    BGPU_API_TRY
    Swapchain* sc = static_cast<Swapchain*>(
        take(swapchain, kObjSwapchain, "swapchain", __FILE__, __LINE__));
    if (!sc) return BGPU_ERR_INVALID_HANDLE;
    Cmd* c = static_cast<Cmd*>(take(cmd, kObjCmd, "cmd", __FILE__, __LINE__));
    if (!c) return BGPU_ERR_INVALID_HANDLE;
    if (!sameDevice(sc, c->device)) {
        return fail(BGPU_ERR_INVALID_HANDLE, __FILE__, __LINE__,
                    "swapchain 与 cmd 不属于同一个设备");
    }
    return settle(sc->be->frameEnd(*sc, *c), __FILE__, __LINE__, s0);
    BGPU_API_CATCH_RESULT()
}

BgpuResult bingpu_frame_read(BgpuSwapchain swapchain, void* dst, size_t dst_size,
                             uint32_t* out_stride) {
    ApiScope scope("bingpu_frame_read");
    const unsigned s0 = errorSeq();
    BGPU_API_TRY
    Swapchain* sc = static_cast<Swapchain*>(
        take(swapchain, kObjSwapchain, "swapchain", __FILE__, __LINE__));
    if (!sc) return BGPU_ERR_INVALID_HANDLE;
    if (!dst || dst_size == 0) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__, "dst 为空 / dst_size=0");
    }
    return settle(sc->be->frameRead(*sc, dst, dst_size, out_stride), __FILE__, __LINE__, s0);
    BGPU_API_CATCH_RESULT()
}

/* ============================================================
 * 纹理
 * ============================================================ */
BgpuResult bingpu_texture_create(BgpuDevice device, const BgpuTextureDesc* desc,
                                 BgpuTexture* out_texture) {
    ApiScope scope("bingpu_texture_create");
    const unsigned s0 = errorSeq();
    BGPU_API_TRY
    Device* dev = takeDevice(device, __FILE__, __LINE__);
    if (!dev) return BGPU_ERR_INVALID_HANDLE;
    if (!desc || !out_texture) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__, "desc/out_texture 为空");
    }
    *out_texture = BGPU_INVALID;
    BgpuResult r = checkOut(desc->struct_size, sizeof(BgpuTextureDesc), "desc", __FILE__,
                            __LINE__);
    if (r != BGPU_OK) return r;
    if (desc->width == 0 || desc->height == 0) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__,
                    cat("尺寸 ", desc->width, "x", desc->height, " 非法"));
    }
    if (desc->width > BGPU_MAX_TEXTURE_SIZE || desc->height > BGPU_MAX_TEXTURE_SIZE) {
        return fail(BGPU_ERR_LIMIT, __FILE__, __LINE__,
                    cat("尺寸 ", desc->width, "x", desc->height, " 超出上限 ",
                        BGPU_MAX_TEXTURE_SIZE));
    }
    if (bingpu_format_bytes(desc->format) == 0) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__,
                    cat("format=", static_cast<int>(desc->format), " 非法"));
    }
    if (desc->mip_levels > 16) {
        return fail(BGPU_ERR_LIMIT, __FILE__, __LINE__,
                    cat("mip_levels=", desc->mip_levels, " 超出上限 16"));
    }
    return settle(dev->be->createTexture(*dev, *desc, out_texture), __FILE__, __LINE__, s0);
    BGPU_API_CATCH_RESULT()
}

void bingpu_texture_destroy(BgpuDevice device, BgpuTexture texture) {
    ApiScope scope("bingpu_texture_destroy");
    BGPU_API_TRY
    Texture* tex = static_cast<Texture*>(lookupObject(texture, kObjTexture));
    if (!tex || !sameDevice(tex, device)) return;
    if (tex->flags & kObjFlagBorrowed) {
        fail(BGPU_ERR_STATE, __FILE__, __LINE__,
             "交换链借出的目标纹理不许 destroy（有效期到 frame_end）");
        return;
    }
    destroyObject(texture);
    BGPU_API_CATCH_VOID()
}

BgpuResult bingpu_texture_info(BgpuTexture texture, BgpuTextureInfo* out_info) {
    ApiScope scope("bingpu_texture_info");
    BGPU_API_TRY
    Texture* tex = static_cast<Texture*>(
        take(texture, kObjTexture, "texture", __FILE__, __LINE__));
    if (!tex) return BGPU_ERR_INVALID_HANDLE;
    if (!out_info) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__, "out_info 为空");
    }
    BgpuResult r = checkOut(out_info->struct_size, sizeof(*out_info), "out_info", __FILE__,
                            __LINE__);
    if (r != BGPU_OK) return r;
    tex->be->textureInfo(*tex, *out_info);
    return BGPU_OK;
    BGPU_API_CATCH_RESULT()
}

BgpuResult bingpu_texture_upload(BgpuTexture texture, const BgpuBox* box, const void* pixels,
                                 uint32_t stride) {
    ApiScope scope("bingpu_texture_upload");
    const unsigned s0 = errorSeq();
    BGPU_API_TRY
    Texture* tex = static_cast<Texture*>(
        take(texture, kObjTexture, "texture", __FILE__, __LINE__));
    if (!tex) return BGPU_ERR_INVALID_HANDLE;
    if (!pixels) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__, "pixels 为空");
    }
    return settle(tex->be->textureUpload(*tex, box, pixels, stride), __FILE__, __LINE__, s0);
    BGPU_API_CATCH_RESULT()
}

BgpuResult bingpu_texture_read(BgpuTexture texture, void* dst, size_t dst_size,
                               uint32_t* out_stride) {
    ApiScope scope("bingpu_texture_read");
    const unsigned s0 = errorSeq();
    BGPU_API_TRY
    Texture* tex = static_cast<Texture*>(
        take(texture, kObjTexture, "texture", __FILE__, __LINE__));
    if (!tex) return BGPU_ERR_INVALID_HANDLE;
    if (!dst || dst_size == 0) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__, "dst 为空 / dst_size=0");
    }
    return settle(tex->be->textureRead(*tex, dst, dst_size, out_stride), __FILE__, __LINE__, s0);
    BGPU_API_CATCH_RESULT()
}

/* ============================================================
 * 缓冲
 * ============================================================ */
BgpuResult bingpu_buffer_create(BgpuDevice device, const BgpuBufferDesc* desc,
                                BgpuBuffer* out_buffer) {
    ApiScope scope("bingpu_buffer_create");
    const unsigned s0 = errorSeq();
    BGPU_API_TRY
    Device* dev = takeDevice(device, __FILE__, __LINE__);
    if (!dev) return BGPU_ERR_INVALID_HANDLE;
    if (!desc || !out_buffer) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__, "desc/out_buffer 为空");
    }
    *out_buffer = BGPU_INVALID;
    BgpuResult r = checkOut(desc->struct_size, sizeof(BgpuBufferDesc), "desc", __FILE__,
                            __LINE__);
    if (r != BGPU_OK) return r;
    if (desc->size == 0) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__,
                    "size=0（缓冲至少 1 字节）");
    }
    return settle(dev->be->createBuffer(*dev, *desc, out_buffer), __FILE__, __LINE__, s0);
    BGPU_API_CATCH_RESULT()
}

void bingpu_buffer_destroy(BgpuDevice device, BgpuBuffer buffer) {
    ApiScope scope("bingpu_buffer_destroy");
    BGPU_API_TRY
    Buffer* buf = static_cast<Buffer*>(lookupObject(buffer, kObjBuffer));
    if (!buf || !sameDevice(buf, device)) return;
    destroyObject(buffer);
    BGPU_API_CATCH_VOID()
}

BgpuResult bingpu_buffer_update(BgpuBuffer buffer, uint32_t offset, const void* data,
                                uint32_t size) {
    ApiScope scope("bingpu_buffer_update");
    const unsigned s0 = errorSeq();
    BGPU_API_TRY
    Buffer* buf = static_cast<Buffer*>(take(buffer, kObjBuffer, "buffer", __FILE__, __LINE__));
    if (!buf) return BGPU_ERR_INVALID_HANDLE;
    if (!data || size == 0) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__, "data 为空 / size=0");
    }
    return settle(buf->be->bufferUpdate(*buf, offset, data, size), __FILE__, __LINE__, s0);
    BGPU_API_CATCH_RESULT()
}

/* ============================================================
 * 采样器 / 管线
 * ============================================================ */
BgpuResult bingpu_sampler_create(BgpuDevice device, const BgpuSamplerDesc* desc,
                                 BgpuSampler* out_sampler) {
    ApiScope scope("bingpu_sampler_create");
    const unsigned s0 = errorSeq();
    BGPU_API_TRY
    Device* dev = takeDevice(device, __FILE__, __LINE__);
    if (!dev) return BGPU_ERR_INVALID_HANDLE;
    if (!desc || !out_sampler) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__, "desc/out_sampler 为空");
    }
    *out_sampler = BGPU_INVALID;
    BgpuResult r = checkOut(desc->struct_size, sizeof(BgpuSamplerDesc), "desc", __FILE__,
                            __LINE__);
    if (r != BGPU_OK) return r;
    const int mnf = static_cast<int>(desc->min_filter);
    const int mgf = static_cast<int>(desc->mag_filter);
    const int wu = static_cast<int>(desc->wrap_u);
    const int wv = static_cast<int>(desc->wrap_v);
    if (mnf < 0 || mnf > 1 || mgf < 0 || mgf > 1 || wu < 0 || wu > 1 || wv < 0 || wv > 1) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__,
                    "filter/wrap 取值非法（filter 0..1，wrap 0..1）");
    }
    return settle(dev->be->createSampler(*dev, *desc, out_sampler), __FILE__, __LINE__, s0);
    BGPU_API_CATCH_RESULT()
}

void bingpu_sampler_destroy(BgpuDevice device, BgpuSampler sampler) {
    ApiScope scope("bingpu_sampler_destroy");
    BGPU_API_TRY
    Sampler* s = static_cast<Sampler*>(lookupObject(sampler, kObjSampler));
    if (!s || !sameDevice(s, device)) return;
    destroyObject(sampler);
    BGPU_API_CATCH_VOID()
}

BgpuResult bingpu_pipeline_create(BgpuDevice device, const BgpuPipelineDesc* desc,
                                  BgpuPipeline* out_pipeline) {
    ApiScope scope("bingpu_pipeline_create");
    const unsigned s0 = errorSeq();
    BGPU_API_TRY
    Device* dev = takeDevice(device, __FILE__, __LINE__);
    if (!dev) return BGPU_ERR_INVALID_HANDLE;
    if (!desc || !out_pipeline) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__, "desc/out_pipeline 为空");
    }
    *out_pipeline = BGPU_INVALID;
    BgpuResult r = checkOut(desc->struct_size, sizeof(BgpuPipelineDesc), "desc", __FILE__,
                            __LINE__);
    if (r != BGPU_OK) return r;
    const int layout = static_cast<int>(desc->vertex_layout);
    const int blend = static_cast<int>(desc->blend);
    if (layout < 0 || layout > 1) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__,
                    cat("vertex_layout=", layout, " 非法"));
    }
    if (blend < 0 || blend > 4) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__,
                    cat("blend=", blend, " 非法（0..4）"));
    }
    /* ABI 3：光照需要法线，而 2D 顶点（BGPU_VERTEX_2D）没有法线字段。
     * 在 API 层就挡掉，而不是让后端画出一片黑 —— 那种错会花掉半天找。 */
    if ((desc->flags & BGPU_PIPE_LIGHTING) && desc->vertex_layout != BGPU_VERTEX_MESH) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__,
                    "BGPU_PIPE_LIGHTING 需要 BGPU_VERTEX_MESH 顶点（2D 顶点没有法线）");
    }
    return settle(dev->be->createPipeline(*dev, *desc, out_pipeline), __FILE__, __LINE__, s0);
    BGPU_API_CATCH_RESULT()
}

void bingpu_pipeline_destroy(BgpuDevice device, BgpuPipeline pipeline) {
    ApiScope scope("bingpu_pipeline_destroy");
    BGPU_API_TRY
    Pipeline* p = static_cast<Pipeline*>(lookupObject(pipeline, kObjPipeline));
    if (!p || !sameDevice(p, device)) return;
    destroyObject(pipeline);
    BGPU_API_CATCH_VOID()
}

/* ============================================================
 * 命令缓冲
 * ============================================================ */
BgpuResult bingpu_cmd_create(BgpuDevice device, BgpuCmd* out_cmd) {
    ApiScope scope("bingpu_cmd_create");
    const unsigned s0 = errorSeq();
    BGPU_API_TRY
    Device* dev = takeDevice(device, __FILE__, __LINE__);
    if (!dev) return BGPU_ERR_INVALID_HANDLE;
    if (!out_cmd) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__, "out_cmd 为空");
    }
    *out_cmd = BGPU_INVALID;
    return settle(dev->be->createCmd(*dev, out_cmd), __FILE__, __LINE__, s0);
    BGPU_API_CATCH_RESULT()
}

void bingpu_cmd_destroy(BgpuDevice device, BgpuCmd cmd) {
    ApiScope scope("bingpu_cmd_destroy");
    BGPU_API_TRY
    Cmd* c = static_cast<Cmd*>(lookupObject(cmd, kObjCmd));
    if (!c || !sameDevice(c, device)) return;
    destroyObject(cmd);
    BGPU_API_CATCH_VOID()
}

BgpuResult bingpu_cmd_begin_render(BgpuCmd cmd, const BgpuRenderBeginDesc* desc) {
    ApiScope scope("bingpu_cmd_begin_render");
    const unsigned s0 = errorSeq();
    BGPU_API_TRY
    Cmd* c = static_cast<Cmd*>(take(cmd, kObjCmd, "cmd", __FILE__, __LINE__));
    if (!c) return BGPU_ERR_INVALID_HANDLE;
    if (!desc) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__, "desc 为空");
    }
    BgpuResult r = checkOut(desc->struct_size, sizeof(BgpuRenderBeginDesc), "desc", __FILE__,
                            __LINE__);
    if (r != BGPU_OK) return r;
    if (desc->target == BGPU_INVALID) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__,
                    "target 为空（渲染必须有一个目标纹理）");
    }
    Texture* target = static_cast<Texture*>(lookupObject(desc->target, kObjTexture));
    if (!target) {
        return fail(BGPU_ERR_INVALID_HANDLE, __FILE__, __LINE__, "target 纹理句柄无效");
    }
    if (!sameDevice(target, c->device)) {
        return fail(BGPU_ERR_INVALID_HANDLE, __FILE__, __LINE__,
                    "target 纹理不属于当前设备");
    }
    Texture* depth = nullptr;
    if (desc->depth_target != BGPU_INVALID) {
        depth = static_cast<Texture*>(lookupObject(desc->depth_target, kObjTexture));
        if (!depth) {
            return fail(BGPU_ERR_INVALID_HANDLE, __FILE__, __LINE__,
                        "depth_target 纹理句柄无效");
        }
        if (!sameDevice(depth, c->device)) {
            return fail(BGPU_ERR_INVALID_HANDLE, __FILE__, __LINE__,
                        "depth_target 不属于当前设备");
        }
    }
    return settle(c->be->beginRender(*c, *target, depth, *desc), __FILE__, __LINE__, s0);
    BGPU_API_CATCH_RESULT()
}

BgpuResult bingpu_cmd_end_render(BgpuCmd cmd) {
    ApiScope scope("bingpu_cmd_end_render");
    const unsigned s0 = errorSeq();
    BGPU_API_TRY
    Cmd* c = static_cast<Cmd*>(take(cmd, kObjCmd, "cmd", __FILE__, __LINE__));
    if (!c) return BGPU_ERR_INVALID_HANDLE;
    return settle(c->be->endRender(*c), __FILE__, __LINE__, s0);
    BGPU_API_CATCH_RESULT()
}

BgpuResult bingpu_cmd_set_viewport(BgpuCmd cmd, float x, float y, float w, float h) {
    ApiScope scope("bingpu_cmd_set_viewport");
    const unsigned s0 = errorSeq();
    BGPU_API_TRY
    Cmd* c = static_cast<Cmd*>(take(cmd, kObjCmd, "cmd", __FILE__, __LINE__));
    if (!c) return BGPU_ERR_INVALID_HANDLE;
    if (!(w > 0.0f) || !(h > 0.0f)) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__,
                    cat("viewport 尺寸 ", w, "x", h, " 非法（必须 > 0）"));
    }
    return settle(c->be->setViewport(*c, x, y, w, h), __FILE__, __LINE__, s0);
    BGPU_API_CATCH_RESULT()
}

BgpuResult bingpu_cmd_set_scissor(BgpuCmd cmd, int32_t x, int32_t y, int32_t w, int32_t h) {
    ApiScope scope("bingpu_cmd_set_scissor");
    const unsigned s0 = errorSeq();
    BGPU_API_TRY
    Cmd* c = static_cast<Cmd*>(take(cmd, kObjCmd, "cmd", __FILE__, __LINE__));
    if (!c) return BGPU_ERR_INVALID_HANDLE;
    return settle(c->be->setScissor(*c, x, y, w, h), __FILE__, __LINE__, s0);
    BGPU_API_CATCH_RESULT()
}

BgpuResult bingpu_cmd_set_pipeline(BgpuCmd cmd, BgpuPipeline pipeline) {
    ApiScope scope("bingpu_cmd_set_pipeline");
    const unsigned s0 = errorSeq();
    BGPU_API_TRY
    Cmd* c = static_cast<Cmd*>(take(cmd, kObjCmd, "cmd", __FILE__, __LINE__));
    if (!c) return BGPU_ERR_INVALID_HANDLE;
    Pipeline* p = static_cast<Pipeline*>(
        take(pipeline, kObjPipeline, "pipeline", __FILE__, __LINE__));
    if (!p) return BGPU_ERR_INVALID_HANDLE;
    if (!sameDevice(p, c->device)) {
        return fail(BGPU_ERR_INVALID_HANDLE, __FILE__, __LINE__,
                    "pipeline 不属于当前设备");
    }
    return settle(c->be->setPipeline(*c, *p), __FILE__, __LINE__, s0);
    BGPU_API_CATCH_RESULT()
}

BgpuResult bingpu_cmd_set_texture(BgpuCmd cmd, BgpuTexture texture, BgpuSampler sampler) {
    ApiScope scope("bingpu_cmd_set_texture");
    const unsigned s0 = errorSeq();
    BGPU_API_TRY
    Cmd* c = static_cast<Cmd*>(take(cmd, kObjCmd, "cmd", __FILE__, __LINE__));
    if (!c) return BGPU_ERR_INVALID_HANDLE;
    Texture* tex = nullptr;
    Sampler* samp = nullptr;
    if (texture != BGPU_INVALID) {
        tex = static_cast<Texture*>(take(texture, kObjTexture, "texture", __FILE__, __LINE__));
        if (!tex) return BGPU_ERR_INVALID_HANDLE;
        if (!sameDevice(tex, c->device)) {
            return fail(BGPU_ERR_INVALID_HANDLE, __FILE__, __LINE__,
                        "texture 不属于当前设备");
        }
    }
    if (sampler != BGPU_INVALID) {
        samp = static_cast<Sampler*>(take(sampler, kObjSampler, "sampler", __FILE__, __LINE__));
        if (!samp) return BGPU_ERR_INVALID_HANDLE;
        if (!sameDevice(samp, c->device)) {
            return fail(BGPU_ERR_INVALID_HANDLE, __FILE__, __LINE__,
                        "sampler 不属于当前设备");
        }
    }
    return settle(c->be->setTexture(*c, tex, samp), __FILE__, __LINE__, s0);
    BGPU_API_CATCH_RESULT()
}

BgpuResult bingpu_cmd_set_affine(BgpuCmd cmd, const float xf[6]) {
    ApiScope scope("bingpu_cmd_set_affine");
    const unsigned s0 = errorSeq();
    BGPU_API_TRY
    Cmd* c = static_cast<Cmd*>(take(cmd, kObjCmd, "cmd", __FILE__, __LINE__));
    if (!c) return BGPU_ERR_INVALID_HANDLE;
    return settle(c->be->setAffine(*c, xf), __FILE__, __LINE__, s0);
    BGPU_API_CATCH_RESULT()
}

BgpuResult bingpu_cmd_set_light(BgpuCmd cmd, const BgpuLight* light) {
    ApiScope scope("bingpu_cmd_set_light");
    const unsigned s0 = errorSeq();
    BGPU_API_TRY
    Cmd* c = static_cast<Cmd*>(take(cmd, kObjCmd, "cmd", __FILE__, __LINE__));
    if (!c) return BGPU_ERR_INVALID_HANDLE;
    /* NULL = 复位成默认。默认值必须与 bin3d 的 ShadeCtx 一致（见 bingpu.h 的注释），
     * 否则"没设光照就跑"的那一帧软光栅与 GPU 两边对不上。 */
    static const BgpuLight kDefault = {
        {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, {0.1f, 0.1f, 0.1f}};
    const BgpuLight& l = light ? *light : kDefault;
    /* 方向必须是单位向量：后端不做归一化（每帧归一化一次是白给的乘法，
     * 而"方向不是单位长度"会让 Lambert 的亮度差一个常数，那种错很难看出来） */
    const float len2 = l.dir[0] * l.dir[0] + l.dir[1] * l.dir[1] + l.dir[2] * l.dir[2];
    if (!(len2 > 1e-8f) || !(len2 < 4.0f)) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__,
                    "light.dir 必须是单位向量（长度平方应在 1e-8..4 之间）");
    }
    return settle(c->be->setLight(*c, l), __FILE__, __LINE__, s0);
    BGPU_API_CATCH_RESULT()
}

BgpuResult bingpu_cmd_draw_quads(BgpuCmd cmd, const BgpuVertex* vertices, uint32_t quad_count) {
    ApiScope scope("bingpu_cmd_draw_quads");
    const unsigned s0 = errorSeq();
    BGPU_API_TRY
    Cmd* c = static_cast<Cmd*>(take(cmd, kObjCmd, "cmd", __FILE__, __LINE__));
    if (!c) return BGPU_ERR_INVALID_HANDLE;
    if (quad_count == 0) return BGPU_OK;   /* 空绘制是合法的 no-op */
    if (!vertices) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__,
                    cat("vertices 为空（quad_count=", quad_count, "）"));
    }
    if (quad_count > BGPU_MAX_QUADS_PER_DRAW) {
        return fail(BGPU_ERR_LIMIT, __FILE__, __LINE__,
                    cat("quad_count=", quad_count, " 超出单次上限 ", BGPU_MAX_QUADS_PER_DRAW));
    }
    return settle(c->be->drawQuads(*c, vertices, quad_count), __FILE__, __LINE__, s0);
    BGPU_API_CATCH_RESULT()
}

BgpuResult bingpu_cmd_draw_indexed(BgpuCmd cmd, BgpuBuffer vertex_buffer, uint32_t vertex_offset,
                                   BgpuBuffer index_buffer, uint32_t index_count,
                                   uint32_t index_offset) {
    ApiScope scope("bingpu_cmd_draw_indexed");
    const unsigned s0 = errorSeq();
    BGPU_API_TRY
    Cmd* c = static_cast<Cmd*>(take(cmd, kObjCmd, "cmd", __FILE__, __LINE__));
    if (!c) return BGPU_ERR_INVALID_HANDLE;
    Buffer* vb = static_cast<Buffer*>(
        take(vertex_buffer, kObjBuffer, "vertex_buffer", __FILE__, __LINE__));
    if (!vb) return BGPU_ERR_INVALID_HANDLE;
    Buffer* ib = static_cast<Buffer*>(
        take(index_buffer, kObjBuffer, "index_buffer", __FILE__, __LINE__));
    if (!ib) return BGPU_ERR_INVALID_HANDLE;
    if (!sameDevice(vb, c->device) || !sameDevice(ib, c->device)) {
        return fail(BGPU_ERR_INVALID_HANDLE, __FILE__, __LINE__,
                    "顶点/索引缓冲不属于当前设备");
    }
    if (index_count == 0) return BGPU_OK;
    return settle(c->be->drawIndexed(*c, *vb, vertex_offset, *ib, index_count, index_offset),
                  __FILE__, __LINE__, s0);
    BGPU_API_CATCH_RESULT()
}
