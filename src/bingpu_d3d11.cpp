/* bingpu_d3d11.cpp —— D3D11 后端（第二个真实后端，异族模型的代表）
 *
 * 定位（见 docs/layering.md §4/§5）：D3D11 是"即时上下文"模型 —— 没有命令缓冲、
 * 没有 PSO、没有显式同步。binGPU 的 `BgpuCmd` 在 VK 里是命令缓冲，在这里退化成
 * **一个状态结构体 + 立即提交**（`contracts.md` §6 本来就禁止跨线程记录，所以不需要
 * Deferred Context）。这条推论成立与否，正是 D3D11 要验证的东西：如果这套 ABI 在
 * 上一代 API 上不用加任何原语就能逐像素对上 CPU，那 §3 的边界判据就站得住。
 *
 * 与 CPU 参考后端的关系：CPU 后端是**像素权威**，本后端必须和它逐像素一致。
 * 规格是 docs/raster_rules.md，验收是 tests/test_d3d11_render.c（与 VK 共用一份
 * 用例表 tests/gpu_parity.h）。所以：
 *   · 顶点变换、top-left 填充规则、预乘混合因子、UV 采样约定全部照抄规格；
 *   · MSAA 这一版**关掉**（与 CPU 一致，caps.msaa_samples == 1）；
 *   · 纹理/目标内部存**预乘** RGBA8：上传时预乘、回读时反预乘（raster_rules §9）。
 *
 * 构建不依赖 Windows SDK 的 fxc：
 *   · 着色器是 src/d3d11_shaders/quad.hlsl，fxc 编出的 DXBC 以数组入库
 *     （src/bingpu_d3d11_shaders.h，tools/gen_dxbc_header.py 生成）；
 *   · 运行期只 LoadLibrary("d3d11.dll") / ("dxgi.dll")，**不链接** d3d11.lib/dxgi.lib；
 *   · 头文件缺失时本文件退化成"登记但不可用"的桩（available() == false），
 *     其余后端与测试完全不受影响。
 *
 * 与 VK 后端的两处约定差异（都在 HLSL 里，见 src/d3d11_shaders/quad.hlsl）：
 *   ① D3D 的 NDC y 向上，Vulkan 向下 → 顶点着色器里 y 翻一次；
 *   ② 正面判定跟着翻（FrontCounterClockwise = FALSE），语义仍是"CCW = 正面"。
 *
 * 这一版明确没做（写进 docs/backends.md 的欠账，不是漏了）：
 *   · 多帧飞行（Present 之后不等 GPU；max_frames_in_flight 报 1）；
 *   · MSAA / mip 链 / 透明窗口（分层窗口 + premultiplied composite）；
 *   · Deferred Context（单线程记录，用不上）；
 *   · 自定义着色器（那要 ABI 3，见 docs/layering.md §6）。
 */
#include "bingpu_backend.h"

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32) && defined(__has_include)
#if __has_include("d3d11.h") && __has_include("dxgi.h")
#define BINGPU_HAS_D3D11 1
#endif
#endif

#ifndef BINGPU_HAS_D3D11

/* ---------------------------------------------------------------
 * 桩：编译时没有 d3d11.h / dxgi.h（MinGW-w64 自带这两个头；极简工具链可能没有）
 * --------------------------------------------------------------- */
namespace bgpu {
namespace {
class StubD3d11Backend final : public Backend {
public:
    BgpuBackendKind kind() const noexcept override { return BGPU_BACKEND_D3D11; }
    const char* name() const noexcept override { return "d3d11"; }
    bool available() const noexcept override { return false; }
    const char* unavailableReason() const noexcept override {
        return "编译时没有 d3d11.h / dxgi.h（换一个带 Direct3D 头的 MinGW 工具链再重编）";
    }
    BgpuResult createDevice(const BgpuDeviceDesc&, BgpuDevice*) override {
        return fail(BGPU_ERR_UNSUPPORTED, __FILE__, __LINE__, unavailableReason());
    }
    void deviceInfo(Device&, BgpuDeviceInfo&) override {}
    void deviceCaps(Device&, BgpuCaps&) override {}
    void deviceStats(Device&, BgpuStats&) override {}
    BgpuResult createSwapchain(Device&, const BgpuSwapchainDesc&, BgpuSwapchain*) override {
        return BGPU_ERR_UNSUPPORTED;
    }
    BgpuResult resizeSwapchain(Swapchain&, uint32_t, uint32_t) override {
        return BGPU_ERR_UNSUPPORTED;
    }
    void swapchainInfo(Swapchain&, BgpuSwapchainInfo&) override {}
    BgpuResult frameRead(Swapchain&, void*, size_t, uint32_t*) override {
        return BGPU_ERR_UNSUPPORTED;
    }
    BgpuResult createTexture(Device&, const BgpuTextureDesc&, BgpuTexture*) override {
        return BGPU_ERR_UNSUPPORTED;
    }
    void textureInfo(Texture&, BgpuTextureInfo&) override {}
    BgpuResult textureUpload(Texture&, const BgpuBox*, const void*, uint32_t) override {
        return BGPU_ERR_UNSUPPORTED;
    }
    BgpuResult textureRead(Texture&, void*, size_t, uint32_t*) override {
        return BGPU_ERR_UNSUPPORTED;
    }
    BgpuResult createBuffer(Device&, const BgpuBufferDesc&, BgpuBuffer*) override {
        return BGPU_ERR_UNSUPPORTED;
    }
    BgpuResult bufferUpdate(Buffer&, uint32_t, const void*, uint32_t) override {
        return BGPU_ERR_UNSUPPORTED;
    }
    BgpuResult createSampler(Device&, const BgpuSamplerDesc&, BgpuSampler*) override {
        return BGPU_ERR_UNSUPPORTED;
    }
    BgpuResult createPipeline(Device&, const BgpuPipelineDesc&, BgpuPipeline*) override {
        return BGPU_ERR_UNSUPPORTED;
    }
    BgpuResult createCmd(Device&, BgpuCmd*) override { return BGPU_ERR_UNSUPPORTED; }
    BgpuResult frameBegin(Swapchain&, Cmd&, BgpuTexture*) override { return BGPU_ERR_UNSUPPORTED; }
    BgpuResult frameEnd(Swapchain&, Cmd&) override { return BGPU_ERR_UNSUPPORTED; }
    BgpuResult beginRender(Cmd&, Texture&, Texture*, const BgpuRenderBeginDesc&) override {
        return BGPU_ERR_UNSUPPORTED;
    }
    BgpuResult endRender(Cmd&) override { return BGPU_ERR_UNSUPPORTED; }
    BgpuResult setViewport(Cmd&, float, float, float, float) override { return BGPU_OK; }
    BgpuResult setScissor(Cmd&, int32_t, int32_t, int32_t, int32_t) override { return BGPU_OK; }
    BgpuResult setPipeline(Cmd&, Pipeline&) override { return BGPU_OK; }
    BgpuResult setTexture(Cmd&, Texture*, Sampler*) override { return BGPU_OK; }
    BgpuResult setAffine(Cmd&, const float*) override { return BGPU_OK; }
    BgpuResult drawQuads(Cmd&, const BgpuVertex*, uint32_t) override {
        return BGPU_ERR_UNSUPPORTED;
    }
    BgpuResult drawIndexed(Cmd&, Buffer&, uint32_t, Buffer&, uint32_t, uint32_t) override {
        return BGPU_ERR_UNSUPPORTED;
    }
};
}  // namespace

Backend& d3d11Backend() {
    static StubD3d11Backend be;
    return be;
}

}  // namespace bgpu

#else /* ============================= 真实现 ============================= */

#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>

#include "bingpu_d3d11_shaders.h"

namespace bgpu {
namespace {

/* ---------------------------------------------------------------
 * 动态加载：只认 d3d11.dll / dxgi.dll（运行期），构建期零链接
 *
 * 为什么不用 d3d11.lib/dxgi.lib：与 Vulkan 后端同一条理由 —— 静态库一旦链上
 * DXGI，下游（binUI/bin2d）就得在链接行里补库，还得处理 lib 的搜索路径。
 * 函数指针只有两个，代价可以忽略。
 *
 * IID 自己写一份（而不是用 dxgi.h 里的 extern const GUID）：那些符号在
 * dxguid/libdxgi.a 里，不链接就取不到。GUID 是操作系统的稳定约定（DXGI 1.1
 * 起没变过），本地定义一份比多一个链接依赖便宜。
 * --------------------------------------------------------------- */
const GUID kIID_IDXGIFactory1 = {0x770aae78,
                                 0xf26f,
                                 0x4dba,
                                 {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};
const GUID kIID_ID3D11Texture2D = {0x6f15aaf2,
                                   0xd208,
                                   0x4e89,
                                   {0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c}};

struct D3dFns {
    HMODULE d3d11 = nullptr;
    HMODULE dxgi = nullptr;
    decltype(&::D3D11CreateDevice) CreateDevice = nullptr;
    decltype(&::CreateDXGIFactory1) CreateFactory1 = nullptr;
};

D3dFns g_d3d;
bool g_loadTried = false;
bool g_loadOk = false;

inline bool trace3() {
    static const bool on = std::getenv("BINGPU_D3D11_TRACE") != nullptr;
    return on;
}

template <class Fn>
bool bindProc(HMODULE lib, const char* name, Fn* out) {
    FARPROC raw = ::GetProcAddress(lib, name);
    if (!raw) return false;
    /* FARPROC → 函数指针：用 memcpy 而不是 reinterpret_cast，
     * 否则 -Wextra/-Wpedantic 会报 -Wcast-function-type（合法但噪音大）。 */
    std::memcpy(out, &raw, sizeof(raw));
    return true;
}

bool loadD3D11() {
    if (g_loadTried) return g_loadOk;
    g_loadTried = true;
    g_d3d.d3d11 = ::LoadLibraryA("d3d11.dll");
    if (!g_d3d.d3d11) {
        if (trace3()) std::fprintf(stderr, "[d3d11] LoadLibraryA(d3d11.dll) 失败 err=%lu\n",
                                   (unsigned long)::GetLastError());
        return false;
    }
    g_d3d.dxgi = ::LoadLibraryA("dxgi.dll");
    bool ok = bindProc(g_d3d.d3d11, "D3D11CreateDevice", &g_d3d.CreateDevice);
    if (g_d3d.dxgi) {
        /* DXGI 只在"要开窗口 / 要枚举适配器"时必需；没有它也能跑离屏（HARDWARE 驱动类型） */
        bindProc(g_d3d.dxgi, "CreateDXGIFactory1", &g_d3d.CreateFactory1);
    }
    if (!ok && trace3()) std::fprintf(stderr, "[d3d11] 缺 D3D11CreateDevice\n");
    g_loadOk = ok;
    if (trace3()) std::fprintf(stderr, "[d3d11] loadD3D11 ok=%d dxgi=%p\n", (int)ok,
                               (void*)g_d3d.dxgi);
    return ok;
}

/* ---------------------------------------------------------------
 * 小工具
 * --------------------------------------------------------------- */
std::string hrText(HRESULT hr) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned>(hr));
    return buf;
}

BgpuResult d3dFail(HRESULT hr, const char* what) {
    return fail(BGPU_ERR_BACKEND_FAILED, __FILE__, __LINE__,
                cat("D3D11 ", what, " 失败：hr=", hrText(hr)));
}

/* COM 的 RAII（只用 Release，不要 AddRef/QueryInterface 的花活） */
template <class T>
struct Com {
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() { reset(); }
    void reset() {
        if (p) {
            p->Release();
            p = nullptr;
        }
    }
    T** put() {
        reset();
        return &p;
    }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

/* 让 dst 与 src 共享同一个 COM 对象（各自持有一份引用）—— 交换链借出的帧目标
 * 与"离屏纹理"就是这种关系。先 AddRef 再 reset，避免 src==dst 时把自己放掉。 */
template <class T>
void share(Com<T>& dst, const Com<T>& src) {
    T* n = src.p;
    if (n) n->AddRef();
    dst.reset();
    dst.p = n;
}

inline DXGI_FORMAT toDxgiFormat(BgpuFormat f) {
    switch (f) {
        case BGPU_FORMAT_BGRA8: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case BGPU_FORMAT_R8: return DXGI_FORMAT_R8_UNORM;
        case BGPU_FORMAT_RGBA8:
        default: return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

inline BgpuFormat fromDxgiFormat(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_B8G8R8A8_UNORM ? BGPU_FORMAT_BGRA8 : BGPU_FORMAT_RGBA8;
}

/* HLSL 的 cbuffer Push 逐字段对应（64 字节，16 字节对齐打包） */
struct PushConstants {
    float vp[4];       /* offset  0 */
    float target[2];   /* offset 16 */
    float pad[2];      /* offset 24（cbuffer 对齐用，恒 0） */
    float xf0[4];      /* offset 32 */
    float xf1[4];      /* offset 48 */
};
static_assert(sizeof(PushConstants) % 16 == 0, "cbuffer 必须是 16 字节的倍数");

/* 直通 → 预乘（上传时做，raster_rules §9）。与 CPU/VK 同一套整数公式，
 * 逐字节相同：round(r*a/255) 用 (r*a+127)/255。 */
void premultiply(uint8_t* px, size_t count) {
    for (size_t i = 0; i < count; i++) {
        uint8_t* p = px + i * 4;
        const uint32_t a = p[3];
        p[0] = static_cast<uint8_t>((p[0] * a + 127) / 255);
        p[1] = static_cast<uint8_t>((p[1] * a + 127) / 255);
        p[2] = static_cast<uint8_t>((p[2] * a + 127) / 255);
    }
}

/* 预乘 → 直通（回读时做，§9；alpha==0 输出 0） */
void unpremultiply(uint8_t* px, size_t count) {
    for (size_t i = 0; i < count; i++) {
        uint8_t* p = px + i * 4;
        const uint32_t a = p[3];
        if (a == 0) {
            p[0] = p[1] = p[2] = 0;
            continue;
        }
        p[0] = static_cast<uint8_t>(std::min<uint32_t>(255, (p[0] * 255u + a / 2) / a));
        p[1] = static_cast<uint8_t>(std::min<uint32_t>(255, (p[1] * 255u + a / 2) / a));
        p[2] = static_cast<uint8_t>(std::min<uint32_t>(255, (p[2] * 255u + a / 2) / a));
    }
}

void bgraToRgbaInPlace(uint8_t* px, size_t count) {
    for (size_t i = 0; i < count; i++) std::swap(px[i * 4 + 0], px[i * 4 + 2]);
}

/* ---------------------------------------------------------------
 * 对象
 * --------------------------------------------------------------- */
struct D3dDeviceObj;
struct D3dTextureObj;

struct D3dDeviceObj final : Device {
    Com<ID3D11Device> dev;
    Com<ID3D11DeviceContext> ctx;
    Com<IDXGIFactory1> factory;
    Com<IDXGIAdapter> adapter;
    std::string adapter_name;
    uint32_t feature_level = 0;
    DXGI_FORMAT swap_format = DXGI_FORMAT_B8G8R8A8_UNORM;   /* 最近一次建出来的交换链格式 */
    bool windowed = false;
    bool lost = false;

    /* 设备级共享对象：固定功能的全部"管线状态"都在这里，按需绑定 */
    Com<ID3D11VertexShader> vs;
    Com<ID3D11PixelShader> ps;
    Com<ID3D11InputLayout> il_2d;
    Com<ID3D11InputLayout> il_mesh;
    Com<ID3D11Buffer> cb;
    Com<ID3D11BlendState> blend[5];
    Com<ID3D11DepthStencilState> ds[4];   /* 下标 = (测试?1:0) | (写入?2:0) */
    Com<ID3D11RasterizerState> rs[2];     /* 0 = 不剔除，1 = 剔除背面 */
    Com<ID3D11SamplerState> samp_default; /* 未绑采样器时：线性 + 钳制（与 VK/CPU 一致） */
    BgpuTexture white_handle = BGPU_INVALID;
    D3dTextureObj* white = nullptr;       /* 1×1 预乘白 */

    D3dDeviceObj(Backend& b, uint32_t flags, uint32_t w, uint32_t h)
        : Device(b, BGPU_INVALID, flags, w, h) {}
    ~D3dDeviceObj() override;
};

struct D3dTextureObj final : Texture {
    Com<ID3D11Texture2D> tex;
    Com<ID3D11ShaderResourceView> srv;
    Com<ID3D11RenderTargetView> rtv;
    Com<ID3D11DepthStencilView> dsv;
    DXGI_FORMAT dxgi = DXGI_FORMAT_R8G8B8A8_UNORM;
    /* 底层资源的真实尺寸。深度附件的资源按**目标**尺寸建（CPU 的深度数组也是这么
     * 分配的），而 `w/h` 仍上报调用方声明的那张纹理的尺寸 —— 两者别混。 */
    uint32_t res_w = 0, res_h = 0;
    bool is_depth = false;
    D3dDeviceObj* dev = nullptr;

    D3dTextureObj(Backend& b, BgpuDevice owner) : Texture(b, owner) {}
    ~D3dTextureObj() override = default;   /* COM 引用计数自己回收 */

    /* 与另一个纹理共享底层资源（交换链借出的帧目标用这个） */
    void shareFrom(const D3dTextureObj& o) {
        share(tex, o.tex);
        share(srv, o.srv);
        share(rtv, o.rtv);
        dxgi = o.dxgi;
    }
};

struct D3dBufferObj final : Buffer {
    Com<ID3D11Buffer> buf;
    D3dDeviceObj* dev = nullptr;

    D3dBufferObj(Backend& b, BgpuDevice owner) : Buffer(b, owner) {}
    ~D3dBufferObj() override = default;
};

struct D3dSamplerObj final : Sampler {
    Com<ID3D11SamplerState> samp;
    D3dDeviceObj* dev = nullptr;

    D3dSamplerObj(Backend& b, BgpuDevice owner) : Sampler(b, owner) {}
    ~D3dSamplerObj() override = default;
};

/* D3D11 没有 PSO：管线对象只是"布局 + 混合 + 标志"的记录，真正的状态对象
 * 在设备上（blend[5] / ds[4] / rs[2]）。所以这里没有懒建缓存。 */
struct D3dPipelineObj final : Pipeline {
    D3dDeviceObj* dev = nullptr;

    D3dPipelineObj(Backend& b, BgpuDevice owner) : Pipeline(b, owner) {}
    ~D3dPipelineObj() override = default;
};

struct D3dSwapchainObj final : Swapchain {
    Com<IDXGISwapChain> sc;
    Com<ID3D11Texture2D> staging;   /* frame_read 用（present 时拷一份） */
    DXGI_FORMAT dxgi = DXGI_FORMAT_B8G8R8A8_UNORM;
    HWND hwnd = nullptr;
    /* 借给 frame_begin 的对象（基类的 `target` 存的是它的**句柄**；别同名遮蔽） */
    D3dTextureObj* borrow = nullptr;
    /* HEADLESS 交换链的"帧图像"：一张自有离屏纹理（BGPU_INVALID = 窗口模式）。
     * 借用对象的 tex/rtv 直接共享它的（COM 引用计数，不会二次释放 —— VK 那边
     * 踩过"把交换链图像当自有资源销毁"，refcount 从结构上消掉了这个坑）。 */
    BgpuTexture offscreen = BGPU_INVALID;
    D3dDeviceObj* d3ddev = nullptr;

    D3dSwapchainObj(Backend& b, BgpuDevice owner) : Swapchain(b, owner) {}
    ~D3dSwapchainObj() override = default;
};

struct D3dCmdObj final : Cmd {
    D3dDeviceObj* d3ddev = nullptr;

    /* draw_quads 的顶点流与索引（每批重传；按需增长） */
    Com<ID3D11Buffer> vbuf;
    uint32_t vcap = 0;          /* 顶点缓冲容量（字节） */
    Com<ID3D11Buffer> ibuf;
    uint32_t icap_quads = 0;    /* 索引缓冲容量（四边形数） */

    /* 状态剔除（docs/layering.md §6.2 的契约）：只有真的变了才记 Set* 调用 */
    ID3D11BlendState* last_blend = nullptr;
    ID3D11DepthStencilState* last_ds = nullptr;
    ID3D11RasterizerState* last_rs = nullptr;
    ID3D11InputLayout* last_il = nullptr;
    ID3D11ShaderResourceView* last_srv = nullptr;
    ID3D11SamplerState* last_samp = nullptr;
    bool have_shaders = false;   /* 着色器 / 拓扑每个 pass 绑一次 */
    bool have_cb = false;        /* 常量缓冲同理（内容每笔 draw 都会更新） */
    bool have_scissor = false;
    int32_t last_sc[4] = {0, 0, 0, 0};

    D3dCmdObj(Backend& b, BgpuDevice owner, Device& d) : Cmd(b, owner, d) {}
    ~D3dCmdObj() override = default;
};

D3dDeviceObj::~D3dDeviceObj() {
    if (trace3()) std::fprintf(stderr, "[d3d11] ~D3dDeviceObj dev=%p\n", (void*)dev.p);
    /* 先等 GPU 拉平再放：immediate context 是"立即执行"，但驱动内部仍有在飞的命令 */
    if (ctx) ctx->ClearState();
    if (ctx) ctx->Flush();
}

/* ---------------------------------------------------------------
 * 后端
 * --------------------------------------------------------------- */
class D3d11Backend final : public BackendBase {
public:
    BgpuBackendKind kind() const noexcept override { return BGPU_BACKEND_D3D11; }
    const char* name() const noexcept override { return "d3d11"; }
    bool available() const noexcept override;
    const char* unavailableReason() const noexcept override;

    BgpuResult createDevice(const BgpuDeviceDesc& desc, BgpuDevice* out) override;
    void deviceInfo(Device& dev, BgpuDeviceInfo& out) override;
    void deviceCaps(Device& dev, BgpuCaps& out) override;
    void deviceStats(Device& dev, BgpuStats& out) override;
    bool deviceLost(Device& dev) override { return static_cast<D3dDeviceObj&>(dev).lost; }

    BgpuResult createSwapchain(Device& dev, const BgpuSwapchainDesc& desc,
                               BgpuSwapchain* out) override;
    BgpuResult resizeSwapchain(Swapchain& sc, uint32_t w, uint32_t h) override;
    void swapchainInfo(Swapchain& sc, BgpuSwapchainInfo& out) override;
    BgpuResult frameRead(Swapchain& sc, void* dst, size_t dst_size,
                         uint32_t* out_stride) override;

    BgpuResult createTexture(Device& dev, const BgpuTextureDesc& desc,
                             BgpuTexture* out) override;
    void textureInfo(Texture& tex, BgpuTextureInfo& out) override;
    BgpuResult textureUpload(Texture& tex, const BgpuBox* box, const void* pixels,
                             uint32_t stride) override;
    BgpuResult textureRead(Texture& tex, void* dst, size_t dst_size,
                           uint32_t* out_stride) override;
    BgpuResult createBuffer(Device& dev, const BgpuBufferDesc& desc, BgpuBuffer* out) override;
    BgpuResult bufferUpdate(Buffer& buf, uint32_t offset, const void* data,
                            uint32_t size) override;
    BgpuResult createSampler(Device& dev, const BgpuSamplerDesc& desc, BgpuSampler* out) override;
    BgpuResult createPipeline(Device& dev, const BgpuPipelineDesc& desc,
                              BgpuPipeline* out) override;

    BgpuResult createCmd(Device& dev, BgpuCmd* out) override;
    BgpuResult frameBegin(Swapchain& sc, Cmd& cmd, BgpuTexture* out) override;
    BgpuResult beginRender(Cmd& cmd, Texture& target, Texture* depth,
                           const BgpuRenderBeginDesc& desc) override;
    BgpuResult endRender(Cmd& cmd) override;

protected:
    BgpuResult rasterQuads(Cmd& cmd, const BgpuVertex* verts, uint32_t quad_count) override;
    BgpuResult rasterIndexed(Cmd& cmd, Buffer& vb, uint32_t vertex_offset, Buffer& ib,
                             uint32_t index_count, uint32_t index_offset) override;
    BgpuResult prepareDepth(Texture& depth, Texture& target,
                            const BgpuRenderBeginDesc& desc) override;
    /* clear 由 beginRender 里直接 ClearRenderTargetView 做（要先绑上 RTV），
     * 所以这个钩子只是占位 —— 与 VK 用 loadOp CLEAR 等价。 */
    BgpuResult clearColor(Texture&, uint32_t) override { return BGPU_OK; }
    /* 呈现：windowed 是 Present，HEADLESS 是"拷到回读用的 staging" */
    void presentFrame(Swapchain& sc) override;

private:
    bool ensureSwapchain(Swapchain& sc, uint32_t w, uint32_t h);
    bool makeBorrow(D3dSwapchainObj& sc, D3dDeviceObj& dev);
    bool refreshStaging(D3dSwapchainObj& sc, D3dDeviceObj& dev, uint32_t w, uint32_t h,
                        DXGI_FORMAT fmt);
    BgpuResult bindState(D3dCmdObj& c, D3dDeviceObj& d, Pipeline& pipe);
    BgpuResult bindTexture(D3dCmdObj& c, D3dDeviceObj& d, Pipeline& pipe);
    /* 返回 false = 裁剪矩形是空的（这一笔什么也不画） */
    bool applyScissor(D3dCmdObj& c, D3dTextureObj& target);
    bool ensureQuadBuffers(D3dCmdObj& c, D3dDeviceObj& d, uint32_t quads);
    void updateConstants(D3dCmdObj& c, D3dDeviceObj& d, D3dTextureObj& target);
    bool initDeviceObjects(D3dDeviceObj& d);
};

/* 从纹理/命令拿所属设备（句柄表查一次，集中在这里，别到处写两行） */
D3dDeviceObj& devOf(BgpuDevice handle) {
    return static_cast<D3dDeviceObj&>(*lookupObject(handle, kObjDevice));
}

bool D3d11Backend::available() const noexcept {
    static const bool ok = [] {
        if (!loadD3D11()) return false;
        ID3D11Device* dev = nullptr;
        ID3D11DeviceContext* ctx = nullptr;
        D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_10_0;
        const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                          D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
        const HRESULT hr = g_d3d.CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, want,
                                              4, D3D11_SDK_VERSION, &dev, &got, &ctx);
        if (FAILED(hr) || !dev) {
            if (trace3())
                std::fprintf(stderr, "[d3d11] D3D11CreateDevice(HARDWARE) hr=%s\n",
                             hrText(hr).c_str());
            return false;
        }
        dev->Release();
        if (ctx) ctx->Release();
        return true;
    }();
    return ok;
}

const char* D3d11Backend::unavailableReason() const noexcept {
    if (!loadD3D11()) return "d3d11.dll 加载失败（系统太老 / 没有 Direct3D 11）";
    return "这台机器建不出 D3D11 设备（没有可用适配器或 feature level < 10.0）";
}

/* ---------------------------------------------------------------
 * 设备
 * --------------------------------------------------------------- */
bool D3d11Backend::initDeviceObjects(D3dDeviceObj& d) {
    ID3D11Device* dev = d.dev.p;
    if (FAILED(dev->CreateVertexShader(d3d11shader::kQuadVsDxbc, d3d11shader::kQuadVsSize, nullptr,
                                       d.vs.put()))) {
        return false;
    }
    if (FAILED(dev->CreatePixelShader(d3d11shader::kQuadPsDxbc, d3d11shader::kQuadPsSize, nullptr,
                                      d.ps.put()))) {
        return false;
    }

    /* 两种顶点布局，语义名与 quad.hlsl 的 VSIn 对上；stride/offset 是
     * BgpuVertex(24) / BgpuMeshVertex(36) 的**逐字节**镜像 */
    const D3D11_INPUT_ELEMENT_DESC e2d[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    const D3D11_INPUT_ELEMENT_DESC emesh[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    if (FAILED(dev->CreateInputLayout(e2d, 3, d3d11shader::kQuadVsDxbc, d3d11shader::kQuadVsSize,
                                      d.il_2d.put()))) {
        return false;
    }
    if (FAILED(dev->CreateInputLayout(emesh, 3, d3d11shader::kQuadVsDxbc,
                                      d3d11shader::kQuadVsSize, d.il_mesh.put()))) {
        return false;
    }

    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = sizeof(PushConstants);
    cbd.Usage = D3D11_USAGE_DEFAULT;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(dev->CreateBuffer(&cbd, nullptr, d.cb.put()))) return false;

    /* 五种混合：片元输出已经是**预乘**的，所以因子组合与 raster_rules §8 一一对应 */
    for (int i = 0; i <= 4; i++) {
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        switch (static_cast<BgpuBlend>(i)) {
            case BGPU_BLEND_REPLACE:
                bd.RenderTarget[0].BlendEnable = FALSE;
                break;
            case BGPU_BLEND_KEEP:
                bd.RenderTarget[0].BlendEnable = TRUE;
                bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ZERO;
                bd.RenderTarget[0].DestBlend = D3D11_BLEND_SRC_ALPHA;
                bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
                bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_SRC_ALPHA;
                break;
            case BGPU_BLEND_ERASE:
                bd.RenderTarget[0].BlendEnable = TRUE;
                bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ZERO;
                bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
                bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
                bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
                break;
            case BGPU_BLEND_ADDITIVE:
                bd.RenderTarget[0].BlendEnable = TRUE;
                bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
                bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
                bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
                bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
                break;
            case BGPU_BLEND_PREMULTIPLIED:
            default:
                bd.RenderTarget[0].BlendEnable = TRUE;
                bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
                bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
                bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
                bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
                break;
        }
        if (FAILED(dev->CreateBlendState(&bd, d.blend[i].put()))) return false;
    }

    /* 深度：LESS（相等不通过，raster_rules §7），4 种 测试/写入 组合 */
    for (int i = 0; i < 4; i++) {
        D3D11_DEPTH_STENCIL_DESC dd{};
        dd.DepthEnable = (i & 1) ? TRUE : FALSE;
        dd.DepthWriteMask = (i & 2) ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        dd.DepthFunc = D3D11_COMPARISON_LESS;
        dd.StencilEnable = FALSE;
        if (FAILED(dev->CreateDepthStencilState(&dd, d.ds[i].put()))) return false;
    }

    /* 光栅化：ScissorEnable 必须为 TRUE，否则 RSSetScissorRects 不生效。
     * FrontCounterClockwise = FALSE 是因为 quad.hlsl 把 y 翻了一次（见文件头）；
     * 语义仍然是"TL,TR,BL 这个顺序算正面"，与 VK 的 CCW 一致。 */
    for (int i = 0; i < 2; i++) {
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = i ? D3D11_CULL_BACK : D3D11_CULL_NONE;
        rd.FrontCounterClockwise = FALSE;
        rd.DepthClipEnable = TRUE;
        rd.ScissorEnable = TRUE;
        rd.MultisampleEnable = FALSE;
        rd.AntialiasedLineEnable = FALSE;
        if (FAILED(dev->CreateRasterizerState(&rd, d.rs[i].put()))) return false;
    }

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxAnisotropy = 1;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MinLOD = 0.0f;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(dev->CreateSamplerState(&sd, d.samp_default.put()))) return false;
    return true;
}

BgpuResult D3d11Backend::createDevice(const BgpuDeviceDesc& desc, BgpuDevice* out) {
    if (!loadD3D11()) return fail(BGPU_ERR_UNSUPPORTED, __FILE__, __LINE__, unavailableReason());

    const uint64_t handle =
        makeObject<D3dDeviceObj>(*this, desc.flags, desc.width, desc.height);
    if (handle == BGPU_INVALID) return failOom(__FILE__, __LINE__);
    D3dDeviceObj* d =
        static_cast<D3dDeviceObj*>(lookupObject(handle, kObjDevice));
    d->self = handle;
    d->windowed = desc.native_window != nullptr;

    /* DXGI 工厂 + 适配器（没有 DXGI 也能跑：退化成 HARDWARE 驱动类型） */
    if (g_d3d.CreateFactory1) {
        if (SUCCEEDED(g_d3d.CreateFactory1(kIID_IDXGIFactory1,
                                          reinterpret_cast<void**>(d->factory.put())))) {
            d->factory->EnumAdapters(0, d->adapter.put());
        }
    }

    UINT flags = 0;
    if (desc.flags & BGPU_DEVICE_DEBUG) flags |= D3D11_CREATE_DEVICE_DEBUG;
    const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                      D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    const D3D_DRIVER_TYPE driver =
        d->adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;
    D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_10_0;
    HRESULT hr = g_d3d.CreateDevice(d->adapter.p, driver, nullptr, flags, want, 4,
                                    D3D11_SDK_VERSION, d->dev.put(), &got, d->ctx.put());
    if (FAILED(hr) && flags) {
        /* 调试层没装（SDK 才有 d3d11SDKLayers.dll）时退回不带 DEBUG 再试一次 */
        if (trace3())
            std::fprintf(stderr, "[d3d11] DEBUG 设备失败 %s，退回普通设备\n",
                         hrText(hr).c_str());
        hr = g_d3d.CreateDevice(d->adapter.p, driver, nullptr, 0, want, 4, D3D11_SDK_VERSION,
                                d->dev.put(), &got, d->ctx.put());
    }
    if (FAILED(hr) || !d->dev || !d->ctx) {
        destroyObject(handle);
        return d3dFail(hr, "D3D11CreateDevice");
    }
    d->feature_level = static_cast<uint32_t>(got);

    if (!initDeviceObjects(*d)) {
        destroyObject(handle);
        return fail(BGPU_ERR_BACKEND_FAILED, __FILE__, __LINE__,
                    "D3D11 固定管线对象创建失败（着色器/输入布局/状态）");
    }

    if (d->adapter) {
        DXGI_ADAPTER_DESC ad{};
        if (SUCCEEDED(d->adapter->GetDesc(&ad))) {
            /* WCHAR → UTF-8（DeviceInfo.adapter_name 约定是 UTF-8） */
            char buf[128 * 4] = {0};
            const int n = ::WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, buf,
                                                static_cast<int>(sizeof(buf)) - 1, nullptr,
                                                nullptr);
            if (n > 0) d->adapter_name.assign(buf, static_cast<size_t>(n - 1));
        }
    }
    if (d->adapter_name.empty()) d->adapter_name = "D3D11 默认适配器";

    /* 1×1 预乘白：未绑纹理时采样它（等价于"白色纹理"，raster_rules §6） */
    const uint8_t white_px[4] = {255, 255, 255, 255};
    BgpuTextureDesc td{};
    td.struct_size = sizeof(td);
    td.width = 1;
    td.height = 1;
    td.format = BGPU_FORMAT_RGBA8;
    td.flags = BGPU_TEXTURE_CPU_READ;
    BgpuResult r = createTexture(*d, td, &d->white_handle);
    if (r != BGPU_OK) {
        destroyObject(handle);
        return r;
    }
    d->white = static_cast<D3dTextureObj*>(lookupObject(d->white_handle, kObjTexture));
    r = textureUpload(*d->white, nullptr, white_px, 4);
    if (r != BGPU_OK) {
        destroyObject(handle);
        return r;
    }

    *out = handle;
    if (trace3()) {
        std::fprintf(stderr, "[d3d11] 设备就绪：%s（FL %u.%u，窗口=%d）\n", d->adapter_name.c_str(),
                     d->feature_level >> 12, (d->feature_level >> 8) & 0xF,
                     (int)d->windowed);
    }
    return BGPU_OK;
}

void D3d11Backend::deviceInfo(Device& dev, BgpuDeviceInfo& out) {
    D3dDeviceObj& d = static_cast<D3dDeviceObj&>(dev);
    out.backend = BGPU_BACKEND_D3D11;
    out.surface_format = fromDxgiFormat(d.swap_format);
    out.api_version = d.feature_level;   /* feature level，例如 0xB000 = FL 11.0 */
    std::snprintf(out.adapter_name, sizeof(out.adapter_name), "%s", d.adapter_name.c_str());
    /* DXGI 的 IDXGIAdapter 不暴露驱动版本号（那要走驱动私有的 D3DKMT 查询），
     * 这里如实上报 feature level —— 比编一个假版本号强。 */
    std::snprintf(out.driver_name, sizeof(out.driver_name), "FL %u.%u", d.feature_level >> 12,
                  (d.feature_level >> 8) & 0xF);
}

void D3d11Backend::deviceCaps(Device& dev, BgpuCaps& out) {
    (void)dev;
    out.flags = BGPU_CAP_DEPTH | BGPU_CAP_OFFSCREEN | BGPU_CAP_READBACK | BGPU_CAP_PRESENT_PACES |
                BGPU_CAP_VSYNC | BGPU_CAP_SAMPLERS | BGPU_CAP_AFFINE | BGPU_CAP_MESH;
    out.max_texture_size = BGPU_MAX_TEXTURE_SIZE;
    out.max_frames_in_flight = 1;   /* 立即上下文：Present 之后不等 GPU */
    out.msaa_samples = 1;           /* 与 CPU 参考后端一致（0 差验收的前提） */
    out.max_quads_per_draw = BGPU_MAX_QUADS_PER_DRAW;
}

void D3d11Backend::deviceStats(Device& dev, BgpuStats& out) {
    out.frame_index = dev.frame_index;
    out.frames_presented = dev.frames_presented;
    out.draws = dev.draws;
    out.quads = dev.quads;
    out.triangles = dev.tris;
    out.bytes_uploaded = dev.bytes_uploaded;
    out.bytes_live = dev.bytes_live;
}

/* ---------------------------------------------------------------
 * 纹理
 * --------------------------------------------------------------- */
BgpuResult D3d11Backend::createTexture(Device& dev, const BgpuTextureDesc& desc,
                                       BgpuTexture* out) {
    D3dDeviceObj& d = static_cast<D3dDeviceObj&>(dev);
    if (desc.width == 0 || desc.height == 0) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__, "纹理尺寸不能为 0");
    }
    if (desc.width > BGPU_MAX_TEXTURE_SIZE || desc.height > BGPU_MAX_TEXTURE_SIZE) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__,
                    cat("纹理超过上限 ", BGPU_MAX_TEXTURE_SIZE));
    }
    /* 与 CPU 同一条规矩：R8 是采样遮罩，不能当渲染目标 */
    if ((desc.flags & BGPU_TEXTURE_RENDER_TARGET) && desc.format == BGPU_FORMAT_R8) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__,
                    "R8 不能当渲染目标（只有 RGBA8/BGRA8 可以；R8 用作采样遮罩）");
    }
    const uint64_t handle = makeObject<D3dTextureObj>(*this, d.self);
    if (handle == BGPU_INVALID) return failOom(__FILE__, __LINE__);
    D3dTextureObj* t = static_cast<D3dTextureObj*>(lookupObject(handle, kObjTexture));
    t->w = desc.width;
    t->h = desc.height;
    t->fmt = desc.format;
    t->tex_flags = desc.flags;
    /* mip：固定管线没有 LOD 输入，采样永远取第 0 级 → 接受 MIPMAPS 但降级为 1 级
     * （与 CPU 一致；info 上报真实值）。 */
    t->mip_levels = 1;
    t->dev = &d;
    t->dxgi = toDxgiFormat(desc.format);

    D3D11_TEXTURE2D_DESC td{};
    td.Width = desc.width;
    td.Height = desc.height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = t->dxgi;
    td.SampleDesc.Count = 1;
    td.SampleDesc.Quality = 0;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (desc.flags & BGPU_TEXTURE_RENDER_TARGET) td.BindFlags |= D3D11_BIND_RENDER_TARGET;
    HRESULT hr = d.dev->CreateTexture2D(&td, nullptr, t->tex.put());
    if (FAILED(hr)) {
        destroyObject(handle);
        return d3dFail(hr, "CreateTexture2D");
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = t->dxgi;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MostDetailedMip = 0;
    sd.Texture2D.MipLevels = 1;
    /* 注意：MinGW 的 D3D11_SHADER_RESOURCE_VIEW_DESC 没有 Shader4ComponentMapping
     * 字段（SDK 头里有），所以 R8 遮罩采样拿到的仍是 (r,0,0,1)，而 CPU 把它当作
     * (v,v,v,v) 广播 —— 这条差异与 VK 后端完全一致（两个 GPU 后端之间是齐的），
     * 记在 docs/backends.md 的欠账里，等 ABI 3 的纹理视图映射一起处理。 */
    hr = d.dev->CreateShaderResourceView(t->tex.p, &sd, t->srv.put());
    if (FAILED(hr)) {
        destroyObject(handle);
        return d3dFail(hr, "CreateShaderResourceView");
    }
    if (desc.flags & BGPU_TEXTURE_RENDER_TARGET) {
        hr = d.dev->CreateRenderTargetView(t->tex.p, nullptr, t->rtv.put());
        if (FAILED(hr)) {
            destroyObject(handle);
            return d3dFail(hr, "CreateRenderTargetView");
        }
    }
    t->bytes = static_cast<uint64_t>(desc.width) * desc.height * bingpu_format_bytes(desc.format);
    t->res_w = desc.width;
    t->res_h = desc.height;
    dev.bytes_live += t->bytes;
    *out = handle;
    return BGPU_OK;
}

void D3d11Backend::textureInfo(Texture& tex, BgpuTextureInfo& out) {
    out.width = tex.w;
    out.height = tex.h;
    out.format = tex.fmt;
    out.flags = tex.tex_flags;
    out.mip_levels = tex.mip_levels;
}

BgpuResult D3d11Backend::textureUpload(Texture& tex, const BgpuBox* box, const void* pixels,
                                       uint32_t stride) {
    D3dTextureObj& t = static_cast<D3dTextureObj&>(tex);
    D3dDeviceObj& d = devOf(tex.device);
    if (!pixels) return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__, "pixels 为空");
    const BgpuBox b = box ? *box : BgpuBox{0, 0, t.w, t.h};
    if (b.w == 0 || b.h == 0 || b.x + b.w > t.w || b.y + b.h > t.h) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__, "上传区域越界");
    }
    const uint32_t bpp = bingpu_format_bytes(t.fmt);
    const uint32_t src_stride = stride ? stride : b.w * bpp;
    if (src_stride < b.w * bpp) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__,
                    cat("stride=", src_stride, " 小于一行 ", b.w * bpp, " 字节"));
    }

    /* 暂存纹理只做"这一段"，不缓存（与 VK 每次上传建一个 host buffer 等价，
     * 但省掉了全尺寸 staging 的显存）。 */
    D3D11_TEXTURE2D_DESC td{};
    td.Width = b.w;
    td.Height = b.h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = t.dxgi;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    Com<ID3D11Texture2D> staging;
    HRESULT hr = d.dev->CreateTexture2D(&td, nullptr, staging.put());
    if (FAILED(hr)) return d3dFail(hr, "CreateTexture2D(staging)");

    D3D11_MAPPED_SUBRESOURCE ms{};
    hr = d.ctx->Map(staging.p, 0, D3D11_MAP_WRITE, 0, &ms);
    if (FAILED(hr)) return d3dFail(hr, "Map(staging)");
    const uint64_t area = static_cast<uint64_t>(b.w) * b.h;
    if (bpp == 4) {
        /* 行内先拷再预乘（raster_rules §9）。局部上传同样要预乘 —— CPU 后端就是这么
         * 做的，不预乘会让"局部更新的半透明贴图"混出不一样的颜色（VK 那边第一版
         * 只在整张上传时预乘，0.4.0 一并修正）。 */
        for (uint32_t y = 0; y < b.h; y++) {
            uint8_t* drow = static_cast<uint8_t*>(ms.pData) + static_cast<size_t>(y) * ms.RowPitch;
            std::memcpy(drow, static_cast<const uint8_t*>(pixels) +
                                 static_cast<size_t>(y) * src_stride,
                        static_cast<size_t>(b.w) * 4);
            premultiply(drow, b.w);
        }
    } else {
        for (uint32_t y = 0; y < b.h; y++) {
            std::memcpy(static_cast<uint8_t*>(ms.pData) + static_cast<size_t>(y) * ms.RowPitch,
                        static_cast<const uint8_t*>(pixels) + static_cast<size_t>(y) * src_stride,
                        static_cast<size_t>(b.w) * bpp);
        }
    }
    d.ctx->Unmap(staging.p, 0);

    /* 注意参数顺序：**目的**区域由 DstX/DstY 给，最后一个参数是**源** box。
     * 把 box 当源传就等于"把 (x,y) 处的 w×h 搬到 (0,0)"（第一版就是这么错的，
     * box 用例的局部上传整体偏移了一格）。 */
    d.ctx->CopySubresourceRegion(t.tex.p, 0, b.x, b.y, 0, staging.p, 0, nullptr);
    d.bytes_uploaded += area * bpp;
    return BGPU_OK;
}

BgpuResult D3d11Backend::textureRead(Texture& tex, void* dst, size_t dst_size,
                                     uint32_t* out_stride) {
    D3dTextureObj& t = static_cast<D3dTextureObj&>(tex);
    if (!(t.tex_flags & (BGPU_TEXTURE_CPU_READ | BGPU_TEXTURE_RENDER_TARGET))) {
        return fail(BGPU_ERR_UNSUPPORTED, __FILE__, __LINE__,
                    "纹理没有 CPU_READ / RENDER_TARGET，不能回读");
    }
    D3dDeviceObj& d = devOf(tex.device);
    const uint32_t bpp = bingpu_format_bytes(t.fmt);
    const uint32_t row = t.w * bpp;
    if (dst_size < static_cast<size_t>(row) * t.h) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__, "dst 太小");
    }

    D3D11_TEXTURE2D_DESC td{};
    t.tex->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    Com<ID3D11Texture2D> staging;
    HRESULT hr = d.dev->CreateTexture2D(&td, nullptr, staging.put());
    if (FAILED(hr)) return d3dFail(hr, "CreateTexture2D(readback)");
    d.ctx->CopyResource(staging.p, t.tex.p);

    D3D11_MAPPED_SUBRESOURCE ms{};
    hr = d.ctx->Map(staging.p, 0, D3D11_MAP_READ, 0, &ms);
    if (FAILED(hr)) return d3dFail(hr, "Map(readback)");
    uint8_t* out = static_cast<uint8_t*>(dst);
    for (uint32_t y = 0; y < t.h; y++) {
        std::memcpy(out + static_cast<size_t>(y) * row,
                    static_cast<const uint8_t*>(ms.pData) + static_cast<size_t>(y) * ms.RowPitch,
                    row);
    }
    d.ctx->Unmap(staging.p, 0);
    /* 与 CPU/VK 一致：回读给的是**直通**颜色，字节序按纹理格式（RGBA8 / BGRA8）。
     * 反预乘对 0..2 字节做，与通道顺序无关。 */
    if (bpp == 4) unpremultiply(out, static_cast<size_t>(t.w) * t.h);
    if (out_stride) *out_stride = row;
    return BGPU_OK;
}

/* ---------------------------------------------------------------
 * 缓冲 / 采样器 / 管线
 * --------------------------------------------------------------- */
BgpuResult D3d11Backend::createBuffer(Device& dev, const BgpuBufferDesc& desc, BgpuBuffer* out) {
    D3dDeviceObj& d = static_cast<D3dDeviceObj&>(dev);
    if (desc.size == 0) return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__, "size 为 0");
    const uint64_t handle = makeObject<D3dBufferObj>(*this, d.self);
    if (handle == BGPU_INVALID) return failOom(__FILE__, __LINE__);
    D3dBufferObj* b = static_cast<D3dBufferObj*>(lookupObject(handle, kObjBuffer));
    b->size = desc.size;
    b->usage = desc.usage;
    b->dev = &d;

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = desc.size;
    /* DEFAULT 而不是 DYNAMIC：`buffer_update` 有 offset（局部更新），
     * DEFAULT + UpdateSubresource 能带 box 部分更新，DYNAMIC 的 WRITE_DISCARD
     * 只能整块重写。网格数据在本项目里是"建一次、偶尔改"，这一侧更省。 */
    bd.Usage = D3D11_USAGE_DEFAULT;
    if (desc.usage & BGPU_BUFFER_VERTEX) bd.BindFlags |= D3D11_BIND_VERTEX_BUFFER;
    if (desc.usage & BGPU_BUFFER_INDEX) bd.BindFlags |= D3D11_BIND_INDEX_BUFFER;
    if (bd.BindFlags == 0) bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;   /* 兜底 */
    HRESULT hr = d.dev->CreateBuffer(&bd, nullptr, b->buf.put());
    if (FAILED(hr)) {
        destroyObject(handle);
        return d3dFail(hr, "CreateBuffer");
    }
    b->bytes = desc.size;
    dev.bytes_live += b->bytes;
    *out = handle;
    return BGPU_OK;
}

BgpuResult D3d11Backend::bufferUpdate(Buffer& buf, uint32_t offset, const void* data,
                                      uint32_t size) {
    D3dBufferObj& b = static_cast<D3dBufferObj&>(buf);
    if (!data || size == 0) return BGPU_OK;
    if (static_cast<uint64_t>(offset) + size > b.size) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__,
                    cat("buffer_update 越界：offset=", offset, " size=", size, " cap=", b.size));
    }
    /* 缓冲区是 DEFAULT：用 box 做部分更新（left/top/front .. right/bottom/back） */
    const D3D11_BOX box{offset, 0, 0, offset + size, 1, 1};
    b.dev->ctx->UpdateSubresource(b.buf.p, 0, &box, data, 0, 0);
    return BGPU_OK;
}

BgpuResult D3d11Backend::createSampler(Device& dev, const BgpuSamplerDesc& desc,
                                       BgpuSampler* out) {
    D3dDeviceObj& d = static_cast<D3dDeviceObj&>(dev);
    const uint64_t handle = makeObject<D3dSamplerObj>(*this, d.self);
    if (handle == BGPU_INVALID) return failOom(__FILE__, __LINE__);
    D3dSamplerObj* s = static_cast<D3dSamplerObj*>(lookupObject(handle, kObjSampler));
    s->desc = desc;
    s->dev = &d;

    const bool nearest = desc.mag_filter == BGPU_FILTER_NEAREST ||
                         desc.min_filter == BGPU_FILTER_NEAREST;
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = nearest ? D3D11_FILTER_MIN_MAG_MIP_POINT : D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU =
        desc.wrap_u == BGPU_WRAP_REPEAT ? D3D11_TEXTURE_ADDRESS_WRAP : D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV =
        desc.wrap_v == BGPU_WRAP_REPEAT ? D3D11_TEXTURE_ADDRESS_WRAP : D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = sd.AddressV;
    sd.MaxAnisotropy = 1;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MinLOD = 0.0f;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    HRESULT hr = d.dev->CreateSamplerState(&sd, s->samp.put());
    if (FAILED(hr)) {
        destroyObject(handle);
        return d3dFail(hr, "CreateSamplerState");
    }
    *out = handle;
    return BGPU_OK;
}

BgpuResult D3d11Backend::createPipeline(Device& dev, const BgpuPipelineDesc& desc,
                                        BgpuPipeline* out) {
    D3dDeviceObj& d = static_cast<D3dDeviceObj&>(dev);
    const uint64_t handle = makeObject<D3dPipelineObj>(*this, d.self);
    if (handle == BGPU_INVALID) return failOom(__FILE__, __LINE__);
    D3dPipelineObj* p = static_cast<D3dPipelineObj*>(lookupObject(handle, kObjPipeline));
    p->layout = desc.vertex_layout;
    p->blend = desc.blend;
    p->pipe_flags = desc.flags;
    p->dev = &d;
    *out = handle;
    return BGPU_OK;
}

/* ---------------------------------------------------------------
 * 命令 / 状态绑定
 * --------------------------------------------------------------- */
BgpuResult D3d11Backend::createCmd(Device& dev, BgpuCmd* out) {
    D3dDeviceObj& d = static_cast<D3dDeviceObj&>(dev);
    /* 同 VK：**不能**复用 BackendBase::createCmd（那造的是基类 Cmd，
     * static_cast 到 D3dCmdObj 就是 UB） */
    const uint64_t handle = makeObject<D3dCmdObj>(*this, d.self, dev);
    if (handle == BGPU_INVALID) {
        setReason("对象表分配失败（内存不足）");
        return BGPU_ERR_OUT_OF_MEMORY;
    }
    static_cast<D3dCmdObj*>(lookupObject(handle, kObjCmd))->d3ddev = &d;
    *out = handle;
    return BGPU_OK;
}

bool D3d11Backend::ensureQuadBuffers(D3dCmdObj& c, D3dDeviceObj& d, uint32_t quads) {
    const uint64_t vbytes = static_cast<uint64_t>(quads) * 4 * sizeof(BgpuVertex);
    if (!c.vbuf || c.vcap < vbytes) {
        uint64_t cap = c.vcap ? c.vcap : 64 * 1024;
        while (cap < vbytes) cap *= 2;
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = static_cast<UINT>(cap);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        if (FAILED(d.dev->CreateBuffer(&bd, nullptr, c.vbuf.put()))) {
            c.vcap = 0;
            return false;
        }
        c.vcap = static_cast<uint32_t>(cap);
    }
    if (!c.ibuf || c.icap_quads < quads) {
        uint64_t cap = c.icap_quads ? c.icap_quads : 4096;
        while (cap < quads) cap *= 2;
        std::vector<uint32_t> idx(static_cast<size_t>(cap) * 6);
        for (uint64_t i = 0; i < cap; i++) {
            const uint32_t base = static_cast<uint32_t>(i * 4);
            idx[i * 6 + 0] = base + 0;
            idx[i * 6 + 1] = base + 1;
            idx[i * 6 + 2] = base + 2;
            idx[i * 6 + 3] = base + 2;
            idx[i * 6 + 4] = base + 1;
            idx[i * 6 + 5] = base + 3;
        }
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = static_cast<UINT>(cap * 6 * sizeof(uint32_t));
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        D3D11_SUBRESOURCE_DATA init{};
        init.pSysMem = idx.data();
        if (FAILED(d.dev->CreateBuffer(&bd, &init, c.ibuf.put()))) {
            c.icap_quads = 0;
            return false;
        }
        c.icap_quads = static_cast<uint32_t>(cap);
    }
    return true;
}

BgpuResult D3d11Backend::bindState(D3dCmdObj& c, D3dDeviceObj& d, Pipeline& pipe) {
    const uint32_t blend = static_cast<uint32_t>(pipe.blend);
    if (blend > 4) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__, "未知的混合模式");
    }
    if (d.blend[blend].p != c.last_blend) {
        const float f[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        d.ctx->OMSetBlendState(d.blend[blend].p, f, 0xFFFFFFFFu);
        c.last_blend = d.blend[blend].p;
    }

    /* 深度状态：没有深度附件时强制关掉测试（否则 D3D 会按"没有 DSV"报错） */
    const bool want_test = (pipe.pipe_flags & BGPU_PIPE_DEPTH_TEST) && c.depth;
    const bool want_write = (pipe.pipe_flags & BGPU_PIPE_DEPTH_WRITE) && c.depth;
    const int dsi = (want_test ? 1 : 0) | (want_write ? 2 : 0);
    if (d.ds[dsi].p != c.last_ds) {
        d.ctx->OMSetDepthStencilState(d.ds[dsi].p, 0);
        c.last_ds = d.ds[dsi].p;
    }

    const int rsi = (pipe.pipe_flags & BGPU_PIPE_CULL_BACK) ? 1 : 0;
    if (d.rs[rsi].p != c.last_rs) {
        d.ctx->RSSetState(d.rs[rsi].p);
        c.last_rs = d.rs[rsi].p;
    }

    ID3D11InputLayout* il = pipe.layout == BGPU_VERTEX_MESH ? d.il_mesh.p : d.il_2d.p;
    if (il != c.last_il) {
        d.ctx->IASetInputLayout(il);
        c.last_il = il;
    }

    /* 顶点/像素着色器与视图大小无关，一个 pass 只绑一次 */
    if (!c.have_shaders) {
        d.ctx->VSSetShader(d.vs.p, nullptr, 0);
        d.ctx->PSSetShader(d.ps.p, nullptr, 0);
        d.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        c.have_shaders = true;
    }
    return BGPU_OK;
}

BgpuResult D3d11Backend::bindTexture(D3dCmdObj& c, D3dDeviceObj& d, Pipeline& pipe) {
    D3dTextureObj* tex = d.white;
    if ((pipe.pipe_flags & BGPU_PIPE_TEXTURED) && c.tex) {
        tex = static_cast<D3dTextureObj*>(c.tex);
    }
    ID3D11SamplerState* samp = d.samp_default.p;
    if (tex != d.white && c.samp) samp = static_cast<D3dSamplerObj*>(c.samp)->samp.p;
    if (!tex || !tex->srv) {
        return fail(BGPU_ERR_STATE, __FILE__, __LINE__, "绑定的纹理没有视图");
    }
    if (tex->srv.p != c.last_srv) {
        d.ctx->PSSetShaderResources(0, 1, &tex->srv.p);
        c.last_srv = tex->srv.p;
    }
    if (samp != c.last_samp) {
        d.ctx->PSSetSamplers(0, 1, &samp);
        c.last_samp = samp;
    }
    return BGPU_OK;
}

bool D3d11Backend::applyScissor(D3dCmdObj& c, D3dTextureObj& target) {
    D3dDeviceObj& d = *c.d3ddev;
    int32_t x = 0, y = 0;
    int32_t r = static_cast<int32_t>(target.res_w);
    int32_t b = static_cast<int32_t>(target.res_h);
    if (c.scissorOn) {
        x = std::max<int32_t>(0, c.sc[0]);
        y = std::max<int32_t>(0, c.sc[1]);
        r = std::min<int32_t>(static_cast<int32_t>(target.res_w), c.sc[0] + c.sc[2]);
        b = std::min<int32_t>(static_cast<int32_t>(target.res_h), c.sc[1] + c.sc[3]);
        if (r <= x || b <= y) return false;   /* 空矩形：这一笔什么也不画 */
    }
    if (c.have_scissor && c.last_sc[0] == x && c.last_sc[1] == y && c.last_sc[2] == r &&
        c.last_sc[3] == b) {
        return true;
    }
    const D3D11_RECT rect{x, y, r, b};
    d.ctx->RSSetScissorRects(1, &rect);
    c.have_scissor = true;
    c.last_sc[0] = x;
    c.last_sc[1] = y;
    c.last_sc[2] = r;
    c.last_sc[3] = b;
    return true;
}

void D3d11Backend::updateConstants(D3dCmdObj& c, D3dDeviceObj& d, D3dTextureObj& target) {
    PushConstants pc{};
    pc.vp[0] = c.vp[0];
    pc.vp[1] = c.vp[1];
    pc.vp[2] = c.vp[2];
    pc.vp[3] = c.vp[3];
    pc.target[0] = static_cast<float>(target.res_w);
    pc.target[1] = static_cast<float>(target.res_h);
    if (c.affineOn) {
        /* c.xf 就是 [a c e | b d f]，与 HLSL 里两行 xf0/xf1 的顺序一致，直接搬 */
        pc.xf0[0] = c.xf[0];
        pc.xf0[1] = c.xf[1];
        pc.xf0[2] = c.xf[2];
        pc.xf0[3] = 1.0f;
        pc.xf1[0] = c.xf[3];
        pc.xf1[1] = c.xf[4];
        pc.xf1[2] = c.xf[5];
    } else {
        pc.xf0[0] = 1.0f;
        pc.xf1[1] = 1.0f;
    }
    d.ctx->UpdateSubresource(d.cb.p, 0, nullptr, &pc, 0, 0);
    if (!c.have_cb) {
        d.ctx->VSSetConstantBuffers(0, 1, &d.cb.p);
        c.have_cb = true;
    }
}

/* ---------------------------------------------------------------
 * 渲染开始 / 结束
 * --------------------------------------------------------------- */
BgpuResult D3d11Backend::prepareDepth(Texture& depth, Texture& target,
                                      const BgpuRenderBeginDesc& desc) {
    (void)desc;
    D3dTextureObj& dt = static_cast<D3dTextureObj&>(depth);
    D3dTextureObj& tgt = static_cast<D3dTextureObj&>(target);
    D3dDeviceObj& d = devOf(depth.device);
    /* ABI 里没有专门的深度格式（backends.md 欠账 2）：把一个 RENDER_TARGET 纹理
     * 当作 D32_FLOAT 深度附件用，资源尺寸跟**目标**一致（CPU 的深度数组也是这么
     * 分配的，所以这里不按 depth 纹理自己声明的尺寸来）。 */
    if (dt.is_depth && dt.res_w == tgt.res_w && dt.res_h == tgt.res_h && dt.dsv) {
        return BGPU_OK;
    }
    dt.tex.reset();
    dt.srv.reset();
    dt.rtv.reset();
    dt.dsv.reset();
    dt.is_depth = true;
    dt.res_w = tgt.res_w;
    dt.res_h = tgt.res_h;
    dt.dxgi = DXGI_FORMAT_D32_FLOAT;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = dt.res_w;
    td.Height = dt.res_h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_D32_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    HRESULT hr = d.dev->CreateTexture2D(&td, nullptr, dt.tex.put());
    if (FAILED(hr)) return d3dFail(hr, "CreateTexture2D(depth)");
    hr = d.dev->CreateDepthStencilView(dt.tex.p, nullptr, dt.dsv.put());
    if (FAILED(hr)) return d3dFail(hr, "CreateDepthStencilView");
    return BGPU_OK;
}

BgpuResult D3d11Backend::beginRender(Cmd& cmd, Texture& target, Texture* depth,
                                     const BgpuRenderBeginDesc& desc) {
    BgpuResult r = BackendBase::beginRender(cmd, target, depth, desc);
    if (r != BGPU_OK) return r;
    D3dCmdObj& c = static_cast<D3dCmdObj&>(cmd);
    D3dTextureObj& t = static_cast<D3dTextureObj&>(target);
    D3dDeviceObj& d = devOf(target.device);
    if (!t.rtv) {
        cmd.inRender = false;
        c.target = nullptr;
        return fail(BGPU_ERR_STATE, __FILE__, __LINE__,
                    "target 没有 RTV（不是 RENDER_TARGET 纹理，或是深度纹理）");
    }

    /* 同一个资源不能同时当 SRV 和 RTV：先把像素着色器的 SRV 清掉 */
    ID3D11ShaderResourceView* null_srv[1] = {nullptr};
    d.ctx->PSSetShaderResources(0, 1, null_srv);
    c.last_srv = nullptr;

    ID3D11DepthStencilView* dsv = depth ? static_cast<D3dTextureObj&>(*depth).dsv.p : nullptr;
    d.ctx->OMSetRenderTargets(1, &t.rtv.p, dsv);

    /* viewport 固定整目标（用户在 set_viewport 里给的是"目标像素"坐标，
     * 映射在顶点着色器里做，与 VK 同一套） */
    const D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(t.res_w),
                            static_cast<float>(t.res_h), 0.0f, 1.0f};
    d.ctx->RSSetViewports(1, &vp);
    const D3D11_RECT rect{0, 0, static_cast<LONG>(t.res_w), static_cast<LONG>(t.res_h)};
    d.ctx->RSSetScissorRects(1, &rect);

    if (desc.flags & BGPU_RENDER_CLEAR_COLOR) {
        const uint32_t cc = desc.clear_color;
        const float a = static_cast<float>((cc >> 24) & 0xFF) / 255.0f;
        /* clear color 是直通 0xAARRGGBB，目标存预乘 → rgb 先乘 alpha（§9）。
         * ClearRenderTargetView 的 float[4] 是 (R,G,B,A) **分量序**，与 Vk 一样：
         * 字节序由目标格式决定，这里不按 BGRA 交换。 */
        const float rgba[4] = {static_cast<float>((cc >> 16) & 0xFF) / 255.0f * a,
                               static_cast<float>((cc >> 8) & 0xFF) / 255.0f * a,
                               static_cast<float>(cc & 0xFF) / 255.0f * a, a};
        d.ctx->ClearRenderTargetView(t.rtv.p, rgba);
    }
    if (dsv && (desc.flags & BGPU_RENDER_CLEAR_DEPTH)) {
        d.ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, desc.clear_depth, 0);
    }

    /* 状态剔除的"上一次"全部作废：新 pass 从零开始 */
    c.last_blend = nullptr;
    c.last_ds = nullptr;
    c.last_rs = nullptr;
    c.last_il = nullptr;
    c.last_srv = nullptr;
    c.last_samp = nullptr;
    c.have_shaders = false;
    c.have_cb = false;
    c.have_scissor = true;
    c.last_sc[0] = 0;
    c.last_sc[1] = 0;
    c.last_sc[2] = static_cast<int32_t>(t.res_w);
    c.last_sc[3] = static_cast<int32_t>(t.res_h);
    return BGPU_OK;
}

BgpuResult D3d11Backend::endRender(Cmd& cmd) {
    D3dCmdObj& c = static_cast<D3dCmdObj&>(cmd);
    D3dDeviceObj& d = *c.d3ddev;
    BgpuResult r = BackendBase::endRender(cmd);
    if (r != BGPU_OK) return r;
    /* 解绑：之后这张纹理可能要当 SRV 采样 / 回读 / 被下一次 present 使用。
     * 不解绑的话下一笔 OMSetRenderTargets 之前的 SRV 绑定就是非法组合。 */
    d.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    c.last_srv = nullptr;
    c.last_samp = nullptr;
    c.have_shaders = false;
    c.have_cb = false;
    return BGPU_OK;
}

/* ---------------------------------------------------------------
 * 交换链 / 帧
 * --------------------------------------------------------------- */
bool D3d11Backend::makeBorrow(D3dSwapchainObj& sc, D3dDeviceObj& dev) {
    if (sc.borrow) return true;
    const uint64_t th = makeObject<D3dTextureObj>(*this, dev.self);
    if (th == BGPU_INVALID) return false;
    sc.borrow = static_cast<D3dTextureObj*>(lookupObject(th, kObjTexture));
    sc.borrow->borrowed = true;
    sc.borrow->flags |= kObjFlagBorrowed;
    sc.borrow->dev = &dev;
    sc.borrow->tex_flags = BGPU_TEXTURE_RENDER_TARGET;
    sc.target = th;
    return true;
}

bool D3d11Backend::refreshStaging(D3dSwapchainObj& sc, D3dDeviceObj& dev, uint32_t w, uint32_t h,
                                  DXGI_FORMAT fmt) {
    if (sc.staging) {
        D3D11_TEXTURE2D_DESC cur{};
        sc.staging->GetDesc(&cur);
        if (cur.Width == w && cur.Height == h && cur.Format == fmt) return true;
    }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    return SUCCEEDED(dev.dev->CreateTexture2D(&td, nullptr, sc.staging.put()));
}

BgpuResult D3d11Backend::createSwapchain(Device& dev, const BgpuSwapchainDesc& desc,
                                         BgpuSwapchain* out) {
    D3dDeviceObj& d = static_cast<D3dDeviceObj&>(dev);
    if (desc.flags & BGPU_DEVICE_TRANSPARENT) {
        return fail(BGPU_ERR_UNSUPPORTED, __FILE__, __LINE__,
                    "D3D11 后端这一版不支持透明窗口（分层窗口 + premultiplied composite 未做）");
    }
    if (desc.width == 0 || desc.height == 0) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__, "尺寸为 0");
    }
    const uint64_t handle = makeObject<D3dSwapchainObj>(*this, d.self);
    if (handle == BGPU_INVALID) return failOom(__FILE__, __LINE__);
    D3dSwapchainObj* sc = static_cast<D3dSwapchainObj*>(lookupObject(handle, kObjSwapchain));
    sc->d3ddev = &d;
    sc->dev = &d;
    sc->hwnd = static_cast<HWND>(desc.native_window);
    sc->w = desc.width;
    sc->h = desc.height;
    sc->sc_flags = desc.flags;
    if (!ensureSwapchain(static_cast<Swapchain&>(*sc), desc.width, desc.height)) {
        destroyObject(handle);
        return fail(BGPU_ERR_BACKEND_FAILED, __FILE__, __LINE__, "交换链创建失败");
    }
    *out = handle;
    return BGPU_OK;
}

/* 窗口客户区尺寸。窗口模式下交换链的尺寸由**窗口**决定（与 VK 后端一致：
 * surface 自己拥有 extent，`currentExtent` 非 0xFFFFFFFF 时请求值被忽略）。
 * 想"低分辨率渲染再放大上屏"就把离屏目标当画布，别指望交换链尺寸 —— 那条路
 * 两个真后端都走不通，也不该走通（`contracts.md` §2：不局部重绘）。 */
bool clientSize(HWND hwnd, uint32_t* w, uint32_t* h) {
    if (!hwnd) return false;
    RECT rc{};
    if (!::GetClientRect(hwnd, &rc)) return false;
    const long cw = rc.right - rc.left;
    const long ch = rc.bottom - rc.top;
    if (cw <= 0 || ch <= 0) return false;
    *w = static_cast<uint32_t>(cw);
    *h = static_cast<uint32_t>(ch);
    return true;
}

bool D3d11Backend::ensureSwapchain(Swapchain& sc_, uint32_t w, uint32_t h) {
    D3dSwapchainObj& sc = static_cast<D3dSwapchainObj&>(sc_);
    D3dDeviceObj& d = *sc.d3ddev;
    if (!makeBorrow(sc, d)) return false;

    /* 窗口模式：尺寸听窗口的（见 clientSize 的注释）。请求值只在客户区取不到时兜底。 */
    if (sc.hwnd) clientSize(sc.hwnd, &w, &h);

    /* 无窗口（HEADLESS）也要有交换链：语义是"帧的载体 + frame_read 出图"
     * （bin2d 的 GPU 桥就是这么用的）。这里用一张自有离屏纹理当"交换链图像"，
     * present 退化成"拷到回读 staging"。 */
    if (!sc.hwnd) {
        if (sc.offscreen == BGPU_INVALID || sc.w != w || sc.h != h) {
            if (sc.offscreen != BGPU_INVALID) destroyObject(sc.offscreen);
            BgpuTextureDesc td{};
            td.struct_size = sizeof(td);
            td.width = w;
            td.height = h;
            td.format = sc_.fmt;
            td.flags = BGPU_TEXTURE_RENDER_TARGET | BGPU_TEXTURE_CPU_READ;
            BgpuTexture th = BGPU_INVALID;
            if (createTexture(d, td, &th) != BGPU_OK) return false;
            sc.offscreen = th;
        }
        D3dTextureObj* ot = static_cast<D3dTextureObj*>(lookupObject(sc.offscreen, kObjTexture));
        if (!ot) return false;
        sc.borrow->shareFrom(*ot);
        sc.borrow->w = w;
        sc.borrow->h = h;
        /* res_w/res_h 也要给：beginRender 的 viewport、updateConstants 的 target、
         * 以及 applyScissor 的裁剪矩形都按它算。漏了它就是"设备 96x64、viewport 0x0"
         * —— 一笔都画不出来（bin2d 的 GPU 桥当场抓到，逐像素差 255）。 */
        sc.borrow->res_w = w;
        sc.borrow->res_h = h;
        sc.borrow->fmt = ot->fmt;
        sc.borrow->dxgi = ot->dxgi;
        sc.dxgi = ot->dxgi;
        sc.w = w;
        sc.h = h;
        sc.fmt = ot->fmt;
        return refreshStaging(sc, d, w, h, sc.dxgi);
    }

    /* 窗口模式：没有交换链（首次 / 上次失败）就建一条 */
    if (!sc.sc) {
        if (!d.factory) return false;
        DXGI_SWAP_CHAIN_DESC sd{};
        sd.BufferCount = 2;
        sd.BufferDesc.Width = w;
        sd.BufferDesc.Height = h;
        /* Windows 原生序（GDI/D3D 交换链都是 BGRA）；frame_read 会换回 RGBA */
        sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.BufferDesc.RefreshRate.Numerator = 0;
        sd.BufferDesc.RefreshRate.Denominator = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = sc.hwnd;
        sd.SampleDesc.Count = 1;
        /* blt 模型（不依赖 flip 的 buffer 轮转），与 binUI 的 D3D11 后端同一套选择 */
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        sd.Windowed = TRUE;
        sd.Flags = 0;
        HRESULT hr = d.factory->CreateSwapChain(d.dev.p, &sd, sc.sc.put());
        if (FAILED(hr)) {
            /* 有些老驱动对 BufferCount=2 的 blt 交换链不买账，退回 1 再试一次 */
            sd.BufferCount = 1;
            hr = d.factory->CreateSwapChain(d.dev.p, &sd, sc.sc.put());
        }
        if (FAILED(hr)) {
            if (trace3()) std::fprintf(stderr, "[d3d11] CreateSwapChain hr=%s\n", hrText(hr).c_str());
            return false;
        }
        /* Alt-Enter 全屏切换交给外层（binUI 的外壳），别让 DXGI 自己动窗口 */
        d.factory->MakeWindowAssociation(sc.hwnd, DXGI_MWA_NO_ALT_ENTER);
    }

    /* 每帧都重新取后台缓冲：ResizeBuffers 之后它是新对象，缓存旧指针就是 UB */
    Com<ID3D11Texture2D> back;
    if (FAILED(sc.sc->GetBuffer(0, kIID_ID3D11Texture2D,
                                reinterpret_cast<void**>(back.put())))) {
        return false;
    }
    /* 借用对象直接共享后台缓冲（COM 引用计数，交换链重建时自然换掉） */
    share(sc.borrow->tex, back);
    sc.borrow->rtv.reset();
    if (FAILED(d.dev->CreateRenderTargetView(back.p, nullptr, sc.borrow->rtv.put()))) {
        return false;
    }
    sc.borrow->srv.reset();
    D3D11_TEXTURE2D_DESC bd{};
    back->GetDesc(&bd);
    sc.dxgi = bd.Format;
    sc.fmt = fromDxgiFormat(bd.Format);
    sc.w = bd.Width;
    sc.h = bd.Height;
    sc.borrow->w = bd.Width;
    sc.borrow->h = bd.Height;
    sc.borrow->fmt = sc.fmt;
    sc.borrow->dxgi = sc.dxgi;
    sc.borrow->res_w = bd.Width;
    sc.borrow->res_h = bd.Height;
    d.swap_format = sc.dxgi;   /* device_info 的 surface_format 跟着后端实际给的东西走 */
    return refreshStaging(sc, d, sc.w, sc.h, sc.dxgi);
}

BgpuResult D3d11Backend::resizeSwapchain(Swapchain& sc_, uint32_t w, uint32_t h) {
    if (w == 0 || h == 0) return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__, "尺寸为 0");
    D3dSwapchainObj& sc = static_cast<D3dSwapchainObj&>(sc_);
    D3dDeviceObj& d = *sc.d3ddev;
    /* 窗口模式：以后台缓冲跟客户区对齐为准（与 VK 的 surface-owned extent 同一语义） */
    if (sc.hwnd) clientSize(sc.hwnd, &w, &h);
    if (sc.hwnd && sc.sc) {
        /* ResizeBuffers 之前必须放掉所有后台缓冲/RTV 的引用，否则
         * DXGI 返回 0x887A0001（INVALID_CALL）—— binUI 在 D3D11 上踩过这个。 */
        d.ctx->OMSetRenderTargets(0, nullptr, nullptr);
        d.ctx->ClearState();
        sc.staging.reset();
        if (sc.borrow) {
            sc.borrow->tex.reset();
            sc.borrow->rtv.reset();
            sc.borrow->srv.reset();
        }
        const UINT count = 0;   /* 0 = 沿用现有缓冲数 */
        HRESULT hr = sc.sc->ResizeBuffers(count, w, h, DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr)) {
            if (trace3()) std::fprintf(stderr, "[d3d11] ResizeBuffers hr=%s\n", hrText(hr).c_str());
            return d3dFail(hr, "ResizeBuffers");
        }
        sc.w = w;
        sc.h = h;
    } else {
        sc.w = w;
        sc.h = h;
    }
    if (!ensureSwapchain(sc_, w, h)) {
        return fail(BGPU_ERR_BACKEND_FAILED, __FILE__, __LINE__, "交换链重建失败");
    }
    return BGPU_OK;
}

void D3d11Backend::swapchainInfo(Swapchain& sc, BgpuSwapchainInfo& out) {
    D3dSwapchainObj& s = static_cast<D3dSwapchainObj&>(sc);
    out.width = sc.w;
    out.height = sc.h;
    out.format = sc.fmt;
    out.image_count = s.hwnd ? 2u : 1u;
    out.msaa_samples = 1;
    out.present_paces_frames = (sc.sc_flags & BGPU_DEVICE_VSYNC) ? 1u : 0u;
}

BgpuResult D3d11Backend::frameBegin(Swapchain& sc_, Cmd& cmd, BgpuTexture* out) {
    D3dSwapchainObj& sc = static_cast<D3dSwapchainObj&>(sc_);
    if (cmd.inFrame) {
        return fail(BGPU_ERR_STATE, __FILE__, __LINE__, "cmd 已经在一个帧里（先 frame_end）");
    }
    if (sc.hwnd && !sc.sc) {
        return fail(BGPU_ERR_STATE, __FILE__, __LINE__, "交换链无效（先 resize）");
    }
    /* 窗口模式的"帧目标"每次都要重新取：ResizeBuffers 之后后台缓冲是新对象 */
    if (sc.hwnd) {
        if (!ensureSwapchain(sc_, sc.w, sc.h)) {
            return fail(BGPU_ERR_BACKEND_FAILED, __FILE__, __LINE__, "取后台缓冲失败");
        }
    }
    return BackendBase::frameBegin(sc_, cmd, out);
}

void D3d11Backend::presentFrame(Swapchain& sc_) {
    D3dSwapchainObj& sc = static_cast<D3dSwapchainObj&>(sc_);
    D3dDeviceObj& d = *sc.d3ddev;
    if (!sc.borrow || !sc.borrow->tex) return;

    /* 先把这一帧拷进 staging（frame_read 读"上一帧刚呈现的图像"，与 VK 后端
     * 每帧拷一次回读缓冲的选择一致：多一次 GPU 拷贝，换来 frame_read 的语义
     * 在两个真实后端上完全相同）。 */
    if (sc.staging) d.ctx->CopyResource(sc.staging.p, sc.borrow->tex.p);

    if (sc.sc) {
        const UINT sync = (sc.sc_flags & BGPU_DEVICE_VSYNC) ? 1u : 0u;
        const HRESULT hr = sc.sc->Present(sync, 0);
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            d.lost = true;
        } else if (FAILED(hr) && trace3()) {
            std::fprintf(stderr, "[d3d11] Present hr=%s\n", hrText(hr).c_str());
        }
    }
}

BgpuResult D3d11Backend::frameRead(Swapchain& sc_, void* dst, size_t dst_size,
                                   uint32_t* out_stride) {
    D3dSwapchainObj& sc = static_cast<D3dSwapchainObj&>(sc_);
    if (!sc.staging) return fail(BGPU_ERR_STATE, __FILE__, __LINE__, "交换链没有可回读的帧");
    D3dDeviceObj& d = *sc.d3ddev;
    const uint32_t row = sc.w * 4;
    if (dst_size < static_cast<size_t>(row) * sc.h) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__, "dst 太小");
    }
    d.ctx->Flush();
    D3D11_MAPPED_SUBRESOURCE ms{};
    HRESULT hr = d.ctx->Map(sc.staging.p, 0, D3D11_MAP_READ, 0, &ms);
    if (FAILED(hr)) return d3dFail(hr, "Map(swapchain staging)");
    uint8_t* out = static_cast<uint8_t*>(dst);
    for (uint32_t y = 0; y < sc.h; y++) {
        std::memcpy(out + static_cast<size_t>(y) * row,
                    static_cast<const uint8_t*>(ms.pData) + static_cast<size_t>(y) * ms.RowPitch,
                    row);
    }
    d.ctx->Unmap(sc.staging.p, 0);
    unpremultiply(out, static_cast<size_t>(sc.w) * sc.h);
    /* frame_read 的输出**永远**是 RGBA 序（raster_rules §9）；交换链是 BGRA 时换回来 */
    if (sc.dxgi == DXGI_FORMAT_B8G8R8A8_UNORM) bgraToRgbaInPlace(out, static_cast<size_t>(sc.w) * sc.h);
    if (trace3())
        std::fprintf(stderr, "[d3d11] frame_read %ux%u dxgi=%d 前 4 字节=%02X%02X%02X%02X\n", sc.w,
                     sc.h, (int)sc.dxgi, out[0], out[1], out[2], out[3]);
    if (out_stride) *out_stride = row;
    return BGPU_OK;
}

/* ---------------------------------------------------------------
 * 绘制
 * --------------------------------------------------------------- */
BgpuResult D3d11Backend::rasterQuads(Cmd& cmd, const BgpuVertex* verts, uint32_t quad_count) {
    D3dCmdObj& c = static_cast<D3dCmdObj&>(cmd);
    D3dDeviceObj& d = *c.d3ddev;
    if (!c.target) return fail(BGPU_ERR_STATE, __FILE__, __LINE__, "draw 时不在渲染里");
    D3dTextureObj& target = static_cast<D3dTextureObj&>(*c.target);

    BgpuResult r = bindState(c, d, *c.pipe);
    if (r != BGPU_OK) return r;
    r = bindTexture(c, d, *c.pipe);
    if (r != BGPU_OK) return r;
    if (!applyScissor(c, target)) return BGPU_OK;   /* 空裁剪：什么也不画 */

    if (!ensureQuadBuffers(c, d, quad_count)) {
        return fail(BGPU_ERR_OUT_OF_MEMORY, __FILE__, __LINE__, "顶点/索引缓冲扩容失败");
    }
    const UINT vbytes = quad_count * 4u * static_cast<UINT>(sizeof(BgpuVertex));
    const D3D11_BOX box{0, 0, 0, vbytes, 1, 1};
    d.ctx->UpdateSubresource(c.vbuf.p, 0, &box, verts, 0, 0);
    updateConstants(c, d, target);

    UINT stride = sizeof(BgpuVertex);
    UINT offset = 0;
    ID3D11Buffer* vb = c.vbuf.p;
    ID3D11Buffer* ib = c.ibuf.p;
    d.ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    d.ctx->IASetIndexBuffer(ib, DXGI_FORMAT_R32_UINT, 0);
    d.ctx->DrawIndexed(quad_count * 6u, 0, 0);
    return BGPU_OK;
}

BgpuResult D3d11Backend::rasterIndexed(Cmd& cmd, Buffer& vb, uint32_t vertex_offset, Buffer& ib,
                                       uint32_t index_count, uint32_t index_offset) {
    D3dCmdObj& c = static_cast<D3dCmdObj&>(cmd);
    D3dDeviceObj& d = *c.d3ddev;
    if (!c.target) return fail(BGPU_ERR_STATE, __FILE__, __LINE__, "draw 时不在渲染里");
    D3dTextureObj& target = static_cast<D3dTextureObj&>(*c.target);

    BgpuResult r = bindState(c, d, *c.pipe);
    if (r != BGPU_OK) return r;
    r = bindTexture(c, d, *c.pipe);
    if (r != BGPU_OK) return r;
    if (!applyScissor(c, target)) return BGPU_OK;

    D3dBufferObj& vbo = static_cast<D3dBufferObj&>(vb);
    D3dBufferObj& ibo = static_cast<D3dBufferObj&>(ib);
    if (!vbo.buf || !ibo.buf) {
        return fail(BGPU_ERR_STATE, __FILE__, __LINE__, "顶点/索引缓冲无效");
    }
    updateConstants(c, d, target);
    /* 偏移是**字节**（raster_rules §11），D3D11 的两个偏移参数正好也是字节 */
    UINT stride = sizeof(BgpuMeshVertex);
    d.ctx->IASetVertexBuffers(0, 1, &vbo.buf.p, &stride, &vertex_offset);
    d.ctx->IASetIndexBuffer(ibo.buf.p, DXGI_FORMAT_R32_UINT, index_offset);
    d.ctx->DrawIndexed(index_count, 0, 0);
    return BGPU_OK;
}

}  // namespace

Backend& d3d11Backend() {
    static D3d11Backend be;
    return be;
}

}  // namespace bgpu

#endif  // BINGPU_HAS_D3D11
