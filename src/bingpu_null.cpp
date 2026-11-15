/* bingpu_null.cpp —— NULL 后端：只校验、只计数、不产生像素
 *
 * 为什么需要它（0.1.0 的理由，C++ 版不变）：
 *   ① 它是**调用序列的回归夹具**：上层接进来时先跑 NULL，就能把"顺序错了 / 句柄用错
 *      了 / struct_size 填错了"从像素问题里摘出来；
 *   ② 无 GPU、无 framebuffer 的 CI 里它永远可用（AUTO 的最后兜底）；
 *   ③ 它是"后端契约"的可执行版本：同样的误用在这里得到的结果码，真实后端也必须给。
 *
 * C++ 版的身材：状态机在 BackendBase（和 CPU 共用一份），这里只剩"创建对象"与
 * "如实上报"两件事；所有像素钩子用基类的空实现 —— 于是它天然就是"只校验 + 计数"。
 */
#include "bingpu_backend.h"

namespace bgpu {
namespace {

/* NULL 后端的纹理/缓冲只需要一个"跟着对象生死走"的记账钩子（stats.bytes_live）。
 * 0.1.0 的 C 版在 create 时加、析构时忘了减 —— 是个记账漏洞（测试没覆盖到），
 * C++ 版用析构补上：bytes_live 真正反映"当前活着的字节数"。 */
struct NullTexture final : Texture {
    Device* dev = nullptr;
    NullTexture(Backend& b, BgpuDevice owner, Device& d) : Texture(b, owner), dev(&d) {}
    ~NullTexture() override {
        if (dev) dev->bytes_live -= bytes;
    }
};

struct NullBuffer final : Buffer {
    Device* dev = nullptr;
    NullBuffer(Backend& b, BgpuDevice owner, Device& d) : Buffer(b, owner), dev(&d) {}
    ~NullBuffer() override {
        if (dev) dev->bytes_live -= bytes;
    }
};

class NullBackend final : public BackendBase {
public:
    BgpuBackendKind kind() const noexcept override { return BGPU_BACKEND_NULL; }
    const char* name() const noexcept override { return "null"; }
    bool available() const noexcept override { return true; }

    BgpuResult createDevice(const BgpuDeviceDesc& desc, BgpuDevice* out) override {
        const uint64_t handle =
            makeObject<Device>(*this, BGPU_INVALID, desc.flags, desc.width, desc.height);
        if (handle == BGPU_INVALID) {
            setReason("对象表分配失败（内存不足）");
            return BGPU_ERR_OUT_OF_MEMORY;
        }
        Device* dev = static_cast<Device*>(lookupObject(handle, kObjDevice));
        dev->self = handle;
        dev->surface_format = BGPU_FORMAT_RGBA8;
        *out = handle;
        return BGPU_OK;
    }

    void deviceInfo(Device& dev, BgpuDeviceInfo& out) override {
        out.backend = BGPU_BACKEND_NULL;
        out.surface_format = dev.surface_format;
        out.api_version = 1;   /* "NULL 后端第 1 版"，仅上报 */
        copyStr(out.adapter_name, sizeof(out.adapter_name), "binGPU NULL (validation only)");
        copyStr(out.driver_name, sizeof(out.driver_name), "none");
    }

    void deviceCaps(Device&, BgpuCaps& out) override {
        /* 如实上报：NULL 画不出任何东西，所以能力全 0。 */
        out.flags = 0;
        out.max_texture_size = BGPU_MAX_TEXTURE_SIZE;
        out.max_frames_in_flight = 1;
        out.msaa_samples = 1;
        out.max_quads_per_draw = 65536u;
    }

    void deviceStats(Device& dev, BgpuStats& out) override {
        out.frame_index = dev.frame_index;
        out.frames_presented = dev.frames_presented;
        out.draws = dev.draws;
        out.quads = dev.quads;
        out.triangles = dev.tris;
        out.bytes_uploaded = dev.bytes_uploaded;
        out.bytes_live = dev.bytes_live;
    }

    BgpuResult createSwapchain(Device& dev, const BgpuSwapchainDesc& desc,
                               BgpuSwapchain* out) override {
        const uint64_t handle = makeObject<Swapchain>(*this, dev.self);
        if (handle == BGPU_INVALID) {
            setReason("对象表分配失败（内存不足）");
            return BGPU_ERR_OUT_OF_MEMORY;
        }
        Swapchain* sc = static_cast<Swapchain*>(lookupObject(handle, kObjSwapchain));
        sc->dev = &dev;   /* 帧统计与"呈现"要用 */
        sc->w = desc.width;
        sc->h = desc.height;
        sc->fmt = desc.format;
        sc->sc_flags = desc.flags;
        /* 借出用的目标纹理：句柄必须是真的（调用方会拿它去 begin_render），
         * 但没有像素，所以只是登记一个带 BORROWED 标志的纹理对象。 */
        const uint64_t t = makeObject<NullTexture>(*this, dev.self, dev);
        if (t == BGPU_INVALID) {
            destroyObject(handle);
            setReason("对象表分配失败（内存不足）");
            return BGPU_ERR_OUT_OF_MEMORY;
        }
        Texture* tex = static_cast<Texture*>(lookupObject(t, kObjTexture));
        tex->w = desc.width;
        tex->h = desc.height;
        tex->fmt = desc.format;
        tex->tex_flags = BGPU_TEXTURE_RENDER_TARGET;
        tex->mip_levels = 1;
        tex->flags |= kObjFlagBorrowed;
        sc->target = t;
        *out = handle;
        return BGPU_OK;
    }

    BgpuResult resizeSwapchain(Swapchain& sc, uint32_t w, uint32_t h) override {
        sc.w = w;
        sc.h = h;
        Texture* tex = static_cast<Texture*>(lookupObject(sc.target, kObjTexture));
        if (tex) {
            tex->w = w;
            tex->h = h;
        }
        return BGPU_OK;
    }

    void swapchainInfo(Swapchain& sc, BgpuSwapchainInfo& out) override {
        out.width = sc.w;
        out.height = sc.h;
        out.format = sc.fmt;
        out.image_count = 1;
        out.msaa_samples = 1;
        out.present_paces_frames = 0;
    }

    BgpuResult frameRead(Swapchain&, void*, size_t, uint32_t*) override {
        setReason("NULL 后端不保存像素，frame_read 不可用（换 cpu 后端出图）");
        return BGPU_ERR_UNSUPPORTED;
    }

    BgpuResult createTexture(Device& dev, const BgpuTextureDesc& desc,
                             BgpuTexture* out) override {
        const uint64_t handle = makeObject<NullTexture>(*this, dev.self, dev);
        if (handle == BGPU_INVALID) {
            setReason("对象表分配失败（内存不足）");
            return BGPU_ERR_OUT_OF_MEMORY;
        }
        Texture* t = static_cast<Texture*>(lookupObject(handle, kObjTexture));
        t->w = desc.width;
        t->h = desc.height;
        t->fmt = desc.format;
        t->tex_flags = desc.flags;
        t->mip_levels = desc.mip_levels ? desc.mip_levels : 1;
        t->bytes = static_cast<uint64_t>(t->w) * t->h * bingpu_format_bytes(t->fmt);
        dev.bytes_live += t->bytes;
        *out = handle;
        return BGPU_OK;
    }

    void textureInfo(Texture& tex, BgpuTextureInfo& out) override {
        out.width = tex.w;
        out.height = tex.h;
        out.format = tex.fmt;
        out.flags = tex.tex_flags;
        out.mip_levels = tex.mip_levels;
    }

    BgpuResult textureUpload(Texture& tex, const BgpuBox* box, const void*, uint32_t stride) override {
        const uint32_t x = box ? box->x : 0;
        const uint32_t y = box ? box->y : 0;
        const uint32_t w = box ? box->w : tex.w;
        const uint32_t h = box ? box->h : tex.h;
        if (w == 0 || h == 0 || x + w > tex.w || y + h > tex.h) {
            setReason(cat("上传区域 ", x, ",", y, " ", w, "x", h, " 超出纹理 ", tex.w, "x",
                          tex.h));
            return BGPU_ERR_INVALID_ARGUMENT;
        }
        const uint32_t bpp = bingpu_format_bytes(tex.fmt);
        const uint32_t row = stride ? stride : w * bpp;
        if (row < w * bpp) {
            setReason(cat("stride=", row, " 小于一行 ", w * bpp, " 字节"));
            return BGPU_ERR_INVALID_ARGUMENT;
        }
        /* 没有像素要写，但"上传了多少字节"是有意义的信息（stats 用）。 */
        static_cast<Device*>(lookupObject(tex.device, kObjDevice))->bytes_uploaded +=
            static_cast<uint64_t>(row) * h;
        return BGPU_OK;
    }

    BgpuResult textureRead(Texture&, void*, size_t, uint32_t*) override {
        setReason("NULL 后端不保存像素，texture_read 不可用（换 cpu 后端）");
        return BGPU_ERR_UNSUPPORTED;
    }

    BgpuResult createBuffer(Device& dev, const BgpuBufferDesc& desc, BgpuBuffer* out) override {
        const uint64_t handle = makeObject<NullBuffer>(*this, dev.self, dev);
        if (handle == BGPU_INVALID) {
            setReason("对象表分配失败（内存不足）");
            return BGPU_ERR_OUT_OF_MEMORY;
        }
        Buffer* b = static_cast<Buffer*>(lookupObject(handle, kObjBuffer));
        b->size = desc.size;
        b->usage = desc.usage;
        b->bytes = desc.size;
        dev.bytes_live += b->bytes;
        *out = handle;
        return BGPU_OK;
    }

    BgpuResult bufferUpdate(Buffer& buf, uint32_t offset, const void*, uint32_t size) override {
        if (static_cast<uint64_t>(offset) + size > buf.size) {
            setReason(cat("写入区间 ", offset, "+", size, " 超出缓冲 ", buf.size, " 字节"));
            return BGPU_ERR_INVALID_ARGUMENT;
        }
        static_cast<Device*>(lookupObject(buf.device, kObjDevice))->bytes_uploaded += size;
        return BGPU_OK;
    }

    BgpuResult createSampler(Device& dev, const BgpuSamplerDesc& desc,
                             BgpuSampler* out) override {
        const uint64_t handle = makeObject<Sampler>(*this, dev.self);
        if (handle == BGPU_INVALID) {
            setReason("对象表分配失败（内存不足）");
            return BGPU_ERR_OUT_OF_MEMORY;
        }
        static_cast<Sampler*>(lookupObject(handle, kObjSampler))->desc = desc;
        *out = handle;
        return BGPU_OK;
    }

    BgpuResult createPipeline(Device& dev, const BgpuPipelineDesc& desc,
                              BgpuPipeline* out) override {
        const uint64_t handle = makeObject<Pipeline>(*this, dev.self);
        if (handle == BGPU_INVALID) {
            setReason("对象表分配失败（内存不足）");
            return BGPU_ERR_OUT_OF_MEMORY;
        }
        Pipeline* p = static_cast<Pipeline*>(lookupObject(handle, kObjPipeline));
        p->layout = desc.vertex_layout;
        p->blend = desc.blend;
        p->pipe_flags = desc.flags;
        *out = handle;
        return BGPU_OK;
    }

private:
    static void copyStr(char* dst, size_t cap, const char* src) {
        if (!dst || cap == 0) return;
        size_t i = 0;
        for (; src[i] && i + 1 < cap; i++) dst[i] = src[i];
        dst[i] = '\0';
    }
};

}  // namespace

Backend& nullBackend() {
    static NullBackend backend;
    return backend;
}

}  // namespace bgpu
