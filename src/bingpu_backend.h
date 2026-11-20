/* bingpu_backend.h —— 后端抽象（C++ 类）与对象类型
 *
 * 形状照 binUI 的 `IBinUIGfx`：一个纯接口 `Backend`，`NullBackend` / `CpuBackend`
 * 继承它。0.1.0 那版是"手写虚表 + 函数指针 + void*"，C++ 版直接换成虚函数：
 *   · 签名即文档（参数类型一眼看清，不用去查 vtable 的成员顺序）；
 *   · 加一个后端只要实现新类，不用改注册表里的函数指针表；
 *   · 共享逻辑放在 `BackendBase`（两个后端一字不差的部分：命令状态机、参数校验、
 *     统计计数），后端只实现"真正不一样的那几段"。
 *
 * 为什么状态机放在基类而不是各写一遍：
 *   0.1.0 里 Null 与 CPU 各写了一套 —— 那是**故意**的对照（互相验校验逻辑）。
 *   但对照的价值已经被 `tests/test_abi.c` 固定住了；C++ 版把它们收成一份，
 *   换来的收益是"两个后端不可能给出不同的结果码"（文档承诺过后端无关的行为）。
 *
 * 对象模型：
 *   ObjectBase（种类/后端/所属设备/借用标志 + 虚析构）
 *     ├── Device / Swapchain / Texture / Buffer / Sampler / Pipeline / Cmd
 *     └── CPU 特有的子类在 bingpu_cpu.cpp 内部（CpuTexture / CpuBuffer / ...）
 *   资源释放写在析构里（RAII）—— "显式 destroy" 与 "设备连带清理" 复用同一份代码。
 */
#pragma once

#include "bingpu_internal.h"

namespace bgpu {

struct Device;
struct Texture;
struct Buffer;
struct Sampler;
struct Pipeline;
struct Swapchain;
struct Cmd;

/* ---------------------------------------------------------------
 * 通用对象：两个后端共用的字段（后端特有的状态在子类里）
 * --------------------------------------------------------------- */
struct Device : ObjectBase {
    BgpuDevice self = BGPU_INVALID;   /* 自己的句柄：资源 owner 用它（设备自己 device==0） */
    uint32_t dev_flags = 0;
    uint32_t w = 0, h = 0;
    BgpuFormat surface_format = BGPU_FORMAT_RGBA8;
    /* 累计统计（语义与 0.1.0 一致：设备生命周期内的累计值，docs/abi.md §7） */
    uint64_t frames_presented = 0, draws = 0, quads = 0, tris = 0;
    uint64_t bytes_uploaded = 0, bytes_live = 0;
    uint32_t frame_index = 0;

    Device(Backend& b, BgpuDevice owner, uint32_t flags, uint32_t width, uint32_t height);
};

struct Texture : ObjectBase {
    uint32_t w = 0, h = 0;
    BgpuFormat fmt = BGPU_FORMAT_RGBA8;
    uint32_t tex_flags = 0;
    uint32_t mip_levels = 1;   /* CPU 后端固定 1（无 LOD 输入），见 docs/backends.md */
    uint64_t bytes = 0;
    bool borrowed = false;     /* 交换链借出的帧目标：不许销毁 */

    Texture(Backend& b, BgpuDevice owner);
};

struct Buffer : ObjectBase {
    uint32_t size = 0;
    uint32_t usage = 0;
    uint64_t bytes = 0;

    Buffer(Backend& b, BgpuDevice owner);
};

struct Sampler : ObjectBase {
    BgpuSamplerDesc desc{};
    Sampler(Backend& b, BgpuDevice owner);
};

struct Pipeline : ObjectBase {
    BgpuVertexLayout layout = BGPU_VERTEX_2D;
    BgpuBlend blend = BGPU_BLEND_PREMULTIPLIED;
    uint32_t pipe_flags = 0;

    Pipeline(Backend& b, BgpuDevice owner);
};

struct Swapchain : ObjectBase {
    Device* dev = nullptr;   /* 帧统计与"呈现"要用（具体后端在 create 时填） */
    uint32_t w = 0, h = 0;
    BgpuFormat fmt = BGPU_FORMAT_RGBA8;
    uint32_t sc_flags = 0;
    BgpuTexture target = BGPU_INVALID;   /* 借给 frame_begin 的纹理句柄 */
    uint64_t frames = 0;

    Swapchain(Backend& b, BgpuDevice owner);
};

struct Cmd : ObjectBase {
    Device* dev = nullptr;
    /* 帧 / 渲染状态（状态机在 BackendBase） */
    bool inFrame = false;
    Swapchain* frame = nullptr;
    bool inRender = false;
    Texture* target = nullptr;
    Texture* depth = nullptr;
    Pipeline* pipe = nullptr;
    Texture* tex = nullptr;
    Sampler* samp = nullptr;
    float vp[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    bool scissorOn = false;
    int32_t sc[4] = {0, 0, 0, 0};
    bool affineOn = false;
    float xf[6] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    /* ABI 3：光照状态。默认值与 bin3d 的 ShadeCtx 一致（见 bingpu.h） */
    BgpuLight light{{0.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, {0.1f, 0.1f, 0.1f}};

    Cmd(Backend& b, BgpuDevice owner, Device& d);

    /* 被引用的对象要销毁了：清掉裸指针，避免帧中途 destroy 造成的悬空引用。
     * 之后的使用会退化成"没有管线/没有纹理"的 STATE 错误，而不是 UB。 */
    void onPeerDestroyed(ObjectBase& dying) override;
};

/* ---------------------------------------------------------------
 * 后端接口
 *
 * 所有入参都是**已经解析好的对象引用**：句柄查找、种类校验、设备归属、struct_size
 * 这些"ABI 层"的检查在 bingpu_api.cpp 里统一做；后端只管"能不能画、画成什么样"。
 * 失败时后端用 `failHere(...)` 写错误（当前公共函数名由入口设置，见 internal.h）。
 * --------------------------------------------------------------- */
class Backend {
public:
    virtual ~Backend() = default;

    virtual BgpuBackendKind kind() const noexcept = 0;
    virtual const char* name() const noexcept = 0;
    virtual bool available() const noexcept { return true; }
    virtual const char* unavailableReason() const noexcept { return ""; }

    /* 设备 */
    virtual BgpuResult createDevice(const BgpuDeviceDesc& desc, BgpuDevice* out) = 0;
    virtual void deviceInfo(Device& dev, BgpuDeviceInfo& out) = 0;
    virtual void deviceCaps(Device& dev, BgpuCaps& out) = 0;
    virtual void deviceStats(Device& dev, BgpuStats& out) = 0;
    virtual bool deviceLost(Device& dev) { (void)dev; return false; }
    /* 等 GPU 把已提交的工作做完。**默认什么都不等**是不行的：CPU/NULL 后端天然同步，
     * 但真实后端必须真的等队列 —— 否则"等空闲"是个谎，基准测试量到的是命令录制时间，
     * 截图/回读也会读到半张图（0.5.2 才补上：VK=设备等待、D3D11=EVENT 查询、
     * GL=glFinish）。 */
    virtual BgpuResult deviceWaitIdle(Device& dev) { (void)dev; return BGPU_OK; }

    /* 交换链 / 帧 */
    virtual BgpuResult createSwapchain(Device& dev, const BgpuSwapchainDesc& desc,
                                       BgpuSwapchain* out) = 0;
    virtual BgpuResult resizeSwapchain(Swapchain& sc, uint32_t w, uint32_t h) = 0;
    virtual void swapchainInfo(Swapchain& sc, BgpuSwapchainInfo& out) = 0;
    virtual BgpuResult frameRead(Swapchain& sc, void* dst, size_t dst_size,
                                 uint32_t* out_stride) = 0;

    /* 资源 */
    virtual BgpuResult createTexture(Device& dev, const BgpuTextureDesc& desc,
                                     BgpuTexture* out) = 0;
    virtual void textureInfo(Texture& tex, BgpuTextureInfo& out) = 0;
    virtual BgpuResult textureUpload(Texture& tex, const BgpuBox* box, const void* pixels,
                                     uint32_t stride) = 0;
    virtual BgpuResult textureRead(Texture& tex, void* dst, size_t dst_size,
                                   uint32_t* out_stride) = 0;
    virtual BgpuResult createBuffer(Device& dev, const BgpuBufferDesc& desc,
                                    BgpuBuffer* out) = 0;
    virtual BgpuResult bufferUpdate(Buffer& buf, uint32_t offset, const void* data,
                                    uint32_t size) = 0;
    virtual BgpuResult createSampler(Device& dev, const BgpuSamplerDesc& desc,
                                     BgpuSampler* out) = 0;
    virtual BgpuResult createPipeline(Device& dev, const BgpuPipelineDesc& desc,
                                      BgpuPipeline* out) = 0;

    /* 命令 */
    virtual BgpuResult createCmd(Device& dev, BgpuCmd* out) = 0;
    virtual BgpuResult frameBegin(Swapchain& sc, Cmd& cmd, BgpuTexture* out) = 0;
    virtual BgpuResult frameEnd(Swapchain& sc, Cmd& cmd) = 0;
    virtual BgpuResult beginRender(Cmd& cmd, Texture& target, Texture* depth,
                                   const BgpuRenderBeginDesc& desc) = 0;
    virtual BgpuResult endRender(Cmd& cmd) = 0;
    /* ABI 6：把一张"延迟 resolve"的渲染目标立刻落成 1×（不采样也能主动收尾）。
     * 默认是 **no-op**：没有多采样 companion 的后端（CPU/NULL/D3D11、以及 msaa=1 的
     * VK/GL）根本不存在"挂起的 resolve"，这个调用天然无事可做。 */
    virtual BgpuResult resolveTarget(Cmd& cmd, Texture& target) {
        (void)cmd;
        (void)target;
        return BGPU_OK;
    }
    virtual BgpuResult setViewport(Cmd& cmd, float x, float y, float w, float h) = 0;
    virtual BgpuResult setScissor(Cmd& cmd, int32_t x, int32_t y, int32_t w, int32_t h) = 0;
    virtual BgpuResult setPipeline(Cmd& cmd, Pipeline& pipe) = 0;
    virtual BgpuResult setTexture(Cmd& cmd, Texture* tex, Sampler* samp) = 0;
    virtual BgpuResult setAffine(Cmd& cmd, const float* xf) = 0;
    /* 光照状态（ABI 3）。默认实现只把参数记进 cmd —— 真正的后端要覆写它，
     * 把三个向量搬进 shader 的 uniform/常量缓冲。默认实现是**完整正确**的
     * 语义（NULL 后端与 CPU 后端都靠它），覆写只加"怎么让 GPU 看见"。 */
    virtual BgpuResult setLight(Cmd& cmd, const BgpuLight& light) {
        cmd.light = light;
        return BGPU_OK;
    }
    virtual BgpuResult drawQuads(Cmd& cmd, const BgpuVertex* verts, uint32_t quad_count) = 0;
    /* ABI 4：解析圆角（SDF）四边形。BackendBase 做状态校验后派发到 rasterShapes 钩子；
     * 钩子默认 UNSUPPORTED —— 后端实现了才覆盖它，并把 BGPU_CAP_SHAPE 打开。 */
    virtual BgpuResult drawShapes(Cmd& cmd, const BgpuShapeVertex* verts,
                                  uint32_t quad_count) = 0;
    virtual BgpuResult drawIndexed(Cmd& cmd, Buffer& vb, uint32_t vertex_offset, Buffer& ib,
                                   uint32_t index_count, uint32_t index_offset) = 0;
};

/* ---------------------------------------------------------------
 * 共享实现：命令状态机 + 参数校验 + 统计
 *
 * 只有"真正要画/要存/要读"的几段是纯虚或钩子：
 *   presentFrame / prepareDepth / clearColor / rasterQuads / rasterIndexed。
 * Null 后端的钩子全是空实现（于是它自然就是"只校验 + 计数"）。
 * --------------------------------------------------------------- */
class BackendBase : public Backend {
public:
    BgpuResult createCmd(Device& dev, BgpuCmd* out) override;
    BgpuResult frameBegin(Swapchain& sc, Cmd& cmd, BgpuTexture* out) override;
    BgpuResult frameEnd(Swapchain& sc, Cmd& cmd) override;
    BgpuResult beginRender(Cmd& cmd, Texture& target, Texture* depth,
                           const BgpuRenderBeginDesc& desc) override;
    BgpuResult endRender(Cmd& cmd) override;
    BgpuResult setViewport(Cmd& cmd, float x, float y, float w, float h) override;
    BgpuResult setScissor(Cmd& cmd, int32_t x, int32_t y, int32_t w, int32_t h) override;
    BgpuResult setPipeline(Cmd& cmd, Pipeline& pipe) override;
    BgpuResult setTexture(Cmd& cmd, Texture* tex, Sampler* samp) override;
    BgpuResult setAffine(Cmd& cmd, const float* xf) override;
    BgpuResult drawQuads(Cmd& cmd, const BgpuVertex* verts, uint32_t quad_count) override;
    BgpuResult drawShapes(Cmd& cmd, const BgpuShapeVertex* verts, uint32_t quad_count) override;
    BgpuResult drawIndexed(Cmd& cmd, Buffer& vb, uint32_t vertex_offset, Buffer& ib,
                           uint32_t index_count, uint32_t index_offset) override;

protected:
    /* 帧呈现（CPU：back → front；NULL：什么都不做） */
    virtual void presentFrame(Swapchain& sc) { (void)sc; }
    /* 深度附件准备（CPU：懒分配 + 按需清除；NULL：只记录） */
    virtual BgpuResult prepareDepth(Texture& depth, Texture& target,
                                    const BgpuRenderBeginDesc& desc) {
        (void)depth;
        (void)target;
        (void)desc;
        return BGPU_OK;
    }
    /* 颜色清除（CPU：写预乘像素；NULL：无像素） */
    virtual BgpuResult clearColor(Texture& target, uint32_t argb) {
        (void)target;
        (void)argb;
        return BGPU_OK;
    }
    /* 光栅化（NULL：空实现 = 只计数） */
    virtual BgpuResult rasterQuads(Cmd& cmd, const BgpuVertex* verts, uint32_t quad_count) {
        (void)cmd;
        (void)verts;
        (void)quad_count;
        return BGPU_OK;
    }
    /* 解析圆角的钩子：默认**不支持**（NULL 后端也走这里 —— 它只计数、不画）。
     * 实现了的后端覆盖它，并在 caps 里给 BGPU_CAP_SHAPE。 */
    virtual BgpuResult rasterShapes(Cmd& cmd, const BgpuShapeVertex* verts,
                                    uint32_t quad_count) {
        (void)cmd;
        (void)verts;
        (void)quad_count;
        return fail(BGPU_ERR_UNSUPPORTED, __FILE__, __LINE__,
                    "这个后端没有实现解析圆角（BGPU_CAP_SHAPE 为 0）");
    }
    virtual BgpuResult rasterIndexed(Cmd& cmd, Buffer& vb, uint32_t vertex_offset, Buffer& ib,
                                     uint32_t index_count, uint32_t index_offset) {
        (void)cmd;
        (void)vb;
        (void)vertex_offset;
        (void)ib;
        (void)index_count;
        (void)index_offset;
        return BGPU_OK;
    }
};

/* ObjectBase 的构造:定义在这里而不是 bingpu_internal.h —— 那里 Backend 只有前向声明，
 * 取 b.kind() 需要完整类型。inline 保证不会出现"两份符号"。 */
inline ObjectBase::ObjectBase(uint32_t k, Backend& b, BgpuDevice owner)
    : kind(k), backend(static_cast<uint32_t>(b.kind())), device(owner), be(&b) {}

/* 后端单例（各自的 .cpp 里定义；注册表在 bingpu_error.cpp 里引用） */
Backend& nullBackend();
Backend& cpuBackend();
Backend& vkBackend();
Backend& d3d11Backend();
Backend& glBackend();

}  // namespace bgpu
