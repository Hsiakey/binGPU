#pragma once
/* ============================================================
 * bingpu.h —— binGPU 公共 C ABI（v0.2.1，ABI 1）
 *
 * binGPU 是"设备 / 交换链 / 资源 / 管线 / 命令 / 呈现"这一层：把同一件事
 * （建窗口面、上传纹理、切混合状态、离屏、present）从每个渲染器里抽出来，
 * 只写一遍。它**不知道**节点、布局、文本、场景图 —— 那些是上层的事。
 *
 *     bin3d  (C++17) ──► bin2d (C++17)
 *              │                  │
 *          binUI (C++17) ─────────┘
 *                       │
 *                 binGPU (C11 / C ABI)   ← 本文件
 *                       │
 *            GL / Vulkan / D3D12 / CPU（参考实现）
 *
 * 语言（2026-11-15 master 定）：**实现用 C++17**，但**对外这一圈仍是 C 兼容的
 * 扁平 ABI**。两件事要分开看：
 *   ① ABI 看的是"谁要 include 它"：bindings/ 里 Python / Go / Rust 只吃 C 接口，
 *      将来别的语言要直接绑这一层，也只需要一个扁平 C 面；
 *   ② "C 更快"不成立（binUI 的 docs/future.md 有实测：同一份 alpha 混合内核交
 *      给 gcc -O2 和 g++ -O2，生成的汇编逐行相同，差别只在前者没法内联取样器）。
 *      所以实现语言按**表达力**选 —— 手写容器、句柄表、生命周期在 C 里全是负债；
 *   ③ 头文件保持 C 兼容还有个顺带的好处：上面三层都是 C++，直接 include 即可，
 *      不需要 `extern "C"` 包装层。
 * 于是：实现是 `.cpp`（std::vector / std::string / RAII / 内部用异常），函数边界
 * 一律 `extern "C"` + `noexcept` + try/catch —— **异常绝不越过 ABI**：内存不足
 * 转 BGPU_ERR_OUT_OF_MEMORY，其余转 BGPU_ERR_BACKEND_FAILED 并把原话写进错误串。
 *
 * 硬约束（写在这里，实现时别吵）：
 *   · 头文件里只有定长 POD + uint64_t 句柄 + 扁平函数。它必须能被**纯 C 编译器**
 *     单独编过（`make abi-check` 会拿 gcc -x c 编一遍）：实现是 C++ 不等于这个头
 *     可以出现 C++ 类型/模板/命名空间 —— 那会把 C 绑定这条路直接堵死。
 *   · 句柄是 uint64_t **不是指针**（跨语言传指针是被绑语言的噩梦）。
 *     0 = 无效句柄，任何拿 0 调用的函数返回 BGPU_ERR_INVALID_HANDLE。
 *   · 没有回调：device lost / resize 这类事件用显式轮询
 *     bingpu_poll() 在渲染线程上取。一旦有回调，就要处理"回调里再进
 *     GPU 层"的可重入问题 —— 那是最贵的一类 bug。
 *   · 销毁顺序仍由调用方保证（C ABI 上没有析构）。上层的 C++ 包装把这条收在一处；
 *     设备销毁时会连带清理名下残留资源，作为最后一道保险而不是常规路径。
 *   · 静态库会带 C++ 运行时（libstdc++）。用 C ABI 绑它的语言在链接时要带上
 *     这份运行时（或让本库静态链接进去）—— 这是"实现用 C++"换来的唯一外部可见代价，
 *     已写进 docs/abi.md。
 *
 * 颜色约定（和 binUI 一致，三端逐像素一致的前提）：
 *   一律 0xAARRGGBB **直通（非预乘）**。乘 alpha 发生在写入阶段 ——
 *   GPU 后端在着色器里做（输出预乘），CPU 后端在核心循环里做。
 *   纹理上传的像素同样是直通 RGBA8；采样后先乘顶点色、再预乘。
 *   帧缓冲不用 sRGB 视图。
 *
 * 帧模型（三条，违反其中一条三端必然不一致）：
 *   ① 一帧 = frame_begin → cmd_begin_render(target) → 记录 → cmd_end_render
 *      → frame_end。frame_begin 拿到的目标纹理是**借用**的（不许销毁）。
 *   ② 绝不在交换链图像上做"局部重绘"：交换链内容不保证保留，每帧都是
 *      "清 + 整画 + present"。
 *   ③ present 是否阻塞到垂直同步由后端决定，并且**自报**：
 *      bingpu_present_paces_frames() 返回 1 时外壳不再自己掐帧（让
 *      acquire/present 当节拍器）；返回 0 时外壳按刷新率节流。
 *
 * 版本策略：BINGPU_ABI_VERSION 只在二进制 ABI（结构体布局 / 函数签名 /
 * 枚举值）变化时 +1；语义化版本 bingpu_version_string() 随功能走。
 * 所有描述结构体以 uint32_t struct_size 开头（= sizeof），后端按这个字段
 * 判断调用方认识哪些尾巴，新加字段是 ABI 兼容的。
 * ============================================================ */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BINGPU_VERSION_MAJOR 0
#define BINGPU_VERSION_MINOR 5
#define BINGPU_VERSION_PATCH 2
#define BINGPU_VERSION_STRING "0.5.2"

/* 二进制 ABI 号：结构体布局 / 函数签名 / 枚举值一变就 +1
 *   ABI 1：0.1.0 - 0.3.0（初始 ABI；0.2.0 只换了实现语言，ABI 未动）
 *   ABI 2：0.4.0（新增 BGPU_BACKEND_D3D11 枚举值；既有值、结构体、签名全部不变）
 *          0.5.0 落 GL 后端时**一个字节都没动** —— GL 的枚举值 0.1.0 就在。 */
#define BINGPU_ABI_VERSION 2u

/* 句柄都是 uint64_t；0 = 无效 */
#define BGPU_INVALID 0ull
typedef uint64_t BgpuDevice;
typedef uint64_t BgpuSwapchain;
typedef uint64_t BgpuTexture;
typedef uint64_t BgpuBuffer;
typedef uint64_t BgpuSampler;
typedef uint64_t BgpuPipeline;
typedef uint64_t BgpuCmd;

#if defined(__cplusplus)
#define BGPU_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define BGPU_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

/* ---------------------------------------------------------------
 * 结果码
 * --------------------------------------------------------------- */
typedef enum BgpuResult {
    BGPU_OK = 0,
    BGPU_ERR_INVALID_ARGUMENT = 1,  /* 参数不合法（空指针 / 0 尺寸 / 超出上限） */
    BGPU_ERR_INVALID_HANDLE = 2,    /* 句柄是 0、已销毁、或属于别的设备 */
    BGPU_ERR_UNSUPPORTED = 3,       /* 这个后端/这台机器不支持该能力 */
    BGPU_ERR_OUT_OF_MEMORY = 4,
    BGPU_ERR_DEVICE_LOST = 5,       /* 设备丢了（拔卡/驱动重启/TDR）；上层重建 */
    BGPU_ERR_STATE = 6,             /* 调用顺序不对（未 begin_render 就 draw、重复 begin…） */
    BGPU_ERR_BACKEND_FAILED = 7,    /* 底层 API 直接报错，细节在 last_error */
    BGPU_ERR_NOT_READY = 8,         /* 设备还在异步初始化 / 已经关闭 */
    BGPU_ERR_LIMIT = 9              /* 超出该后端的资源上限（纹理尺寸、批次数…） */
} BgpuResult;

/* ---------------------------------------------------------------
 * 后端
 *   NULL  : 只做参数校验，不产生像素。用来跑通调用序列 / 回归校验逻辑。
 *   CPU   : 软件参考实现。确定性像素，能 readback —— 无 GPU 的 CI、
 *           离屏出图、像素基准都靠它。三端"逐像素一致"的裁判也是它。
 *   VULKAN: 真实后端（0.3.0）。动态渲染 + 固定管线，要求 Vulkan 1.3。
 *   D3D11 : 真实后端（0.4.0）。即时上下文模型，feature level 10.0 起。
 *   GL    : 真实后端（0.5.0）。WGL + OpenGL 3.3 core，全局状态映射。
 *   D3D12 : 声明了但还没实现（返回 BGPU_ERR_UNSUPPORTED，见 docs/backends.md）。
 * --------------------------------------------------------------- */
typedef enum BgpuBackendKind {
    BGPU_BACKEND_AUTO = 0,   /* 让 bingpu 自己挑：CPU 之外能跑的第一个 */
    BGPU_BACKEND_NULL = 1,
    BGPU_BACKEND_CPU = 2,
    BGPU_BACKEND_VULKAN = 3,
    BGPU_BACKEND_D3D12 = 4,
    BGPU_BACKEND_GL = 5,
    /* ABI 2：追加在末尾，既有枚举值一个都没动（所以旧的二进制调用方仍然正确） */
    BGPU_BACKEND_D3D11 = 6
} BgpuBackendKind;

uint32_t      bingpu_backend_count(void);
BgpuBackendKind bingpu_backend_at(uint32_t index);
const char*   bingpu_backend_name(BgpuBackendKind kind);
/* 编译进来了并且在当前会话里可用（例如 VK 运行期 LoadLibrary 成功才算可用） */
int           bingpu_backend_available(BgpuBackendKind kind);

/* ---------------------------------------------------------------
 * 像素格式（全部 8 位/通道；49 是保留给将来的 HDR）
 * --------------------------------------------------------------- */
typedef enum BgpuFormat {
    BGPU_FORMAT_RGBA8 = 0,  /* 4B：R,G,B,A 直通，bin2d / binUI 的内部格式 */
    BGPU_FORMAT_BGRA8 = 1,  /* 4B：Windows 原生序（GDI / D3D 交换链） */
    BGPU_FORMAT_R8 = 2      /* 1B：遮罩 / 覆盖率 */
} BgpuFormat;

uint32_t bingpu_format_bytes(BgpuFormat format);

/* ---------------------------------------------------------------
 * 顶点（固定管线只有一种"纹理×顶点色"，顶点就是位置 + UV + 颜色）
 *
 *   2D 四边形：BGPU_VERTEX_2D，每 quad 4 个顶点，顺序
 *   TL, TR, BL, BR（与 binUI 的 emitQuad 一致，双线性 UV 因此不会
 *   在半浮点边界上翻面）。draw_quads 的 quad_count 个 4 顶点块
 *   由后端自行展开成 2 个三角形。
 *   3D 网格：BGPU_VERTEX_MESH 带法线（Lambert 光照用）；网格走
 *   draw_indexed，顶点/索引放在 BgpuBuffer 里。
 * --------------------------------------------------------------- */
typedef enum BgpuVertexLayout {
    BGPU_VERTEX_2D = 0,
    BGPU_VERTEX_MESH = 1
} BgpuVertexLayout;

typedef struct BgpuVertex {
    float x, y, z;      /* 屏幕/裁剪空间由 viewport + affine 决定；z 用于深度 */
    float u, v;         /* 归一化纹理坐标 */
    uint32_t color;     /* 0xAARRGGBB 直通 */
} BgpuVertex;

typedef struct BgpuMeshVertex {
    float x, y, z;
    float nx, ny, nz;   /* 世界空间法线 */
    float u, v;
    uint32_t color;
} BgpuMeshVertex;

BGPU_STATIC_ASSERT(sizeof(BgpuVertex) == 24, "BgpuVertex 必须 24 字节");
BGPU_STATIC_ASSERT(sizeof(BgpuMeshVertex) == 36, "BgpuMeshVertex 必须 36 字节");

/* ---------------------------------------------------------------
 * 设备
 * --------------------------------------------------------------- */
typedef enum BgpuDeviceFlag {
    BGPU_DEVICE_VSYNC = 1u << 0,     /* 换帧等垂直同步（present 自己掐拍） */
    BGPU_DEVICE_TRANSPARENT = 1u << 1, /* 分层窗口：画布 alpha 透出桌面 */
    BGPU_DEVICE_VALIDATE = 1u << 2,  /* 打开参数/顺序校验（发布版也保留，开销小） */
    BGPU_DEVICE_HEADLESS = 1u << 3,  /* 无窗口：只做离屏 */
    BGPU_DEVICE_DEBUG = 1u << 4      /* 后端尽量开调试层，细节打 last_error */
} BgpuDeviceFlag;

typedef struct BgpuDeviceDesc {
    uint32_t struct_size;
    BgpuBackendKind backend;
    void* native_window;    /* Win32 是 HWND；NULL（HEADLESS）时只做离屏 */
    uint32_t width;
    uint32_t height;
    uint32_t flags;         /* BgpuDeviceFlag 位或 */
    uint32_t msaa_samples;  /* 1 = 不开 MSAA；4 = 4x（后端不支时自动降到 1） */
} BgpuDeviceDesc;

typedef struct BgpuDeviceInfo {
    uint32_t struct_size;
    BgpuBackendKind backend;
    BgpuFormat surface_format;     /* 交换链实际格式 */
    uint32_t api_version;          /* 后端自己的版本号，仅用于上报 */
    char adapter_name[64];         /* UTF-8，例如 "NVIDIA GeForce RTX 4060" */
    char driver_name[32];          /* UTF-8 */
} BgpuDeviceInfo;

typedef enum BgpuCapFlag {
    BGPU_CAP_DEPTH = 1u << 0,           /* 支持深度附件（bin3d GPU 阶段的前提） */
    BGPU_CAP_OFFSCREEN = 1u << 1,       /* 支持离屏目标（图层 / 视口 / 出图） */
    BGPU_CAP_READBACK = 1u << 2,        /* 支持纹理/帧回读 */
    BGPU_CAP_PRESENT_PACES = 1u << 3,   /* present 会阻塞到垂直同步 */
    BGPU_CAP_TRANSPARENT = 1u << 4,     /* 分层窗口呈现 */
    BGPU_CAP_VSYNC = 1u << 5,           /* 支持垂直同步开关 */
    BGPU_CAP_MSAA = 1u << 6,
    BGPU_CAP_MESH = 1u << 7,            /* draw_indexed（网格） */
    BGPU_CAP_SAMPLERS = 1u << 8,        /* 支持自定义采样器（线性/最近、重复/钳制） */
    BGPU_CAP_AFFINE = 1u << 9           /* 支持 set_affine（任意四边形） */
} BgpuCapFlag;

typedef struct BgpuCaps {
    uint32_t struct_size;
    uint32_t flags;                 /* BgpuCapFlag 位或 */
    uint32_t max_texture_size;
    uint32_t max_frames_in_flight;
    uint32_t msaa_samples;          /* 实际生效值（设备层面） */
    uint32_t max_quads_per_draw;
} BgpuCaps;

typedef struct BgpuStats {
    uint32_t struct_size;
    uint32_t frame_index;
    uint64_t frames_presented;
    uint64_t draws;
    uint64_t quads;
    uint64_t triangles;
    uint64_t bytes_uploaded;
    uint64_t bytes_live;        /* 当前活着的显存/内存资源字节数 */
} BgpuStats;

typedef enum BgpuEventKind {
    BGPU_EVENT_NONE = 0,
    BGPU_EVENT_DEVICE_LOST = 1,
    BGPU_EVENT_RESIZE_REQUIRED = 2,   /* 交换链失效（窗口被拖到别的显示器/尺寸变） */
    BGPU_EVENT_PRESENT_FAILED = 3
} BgpuEventKind;

typedef struct BgpuEvent {
    uint32_t struct_size;
    uint32_t kind;      /* BgpuEventKind */
    uint32_t a, b;      /* 事件附带参数；RESIZE_REQUIRED 时是建议的宽高 */
} BgpuEvent;

BgpuResult bingpu_device_create(const BgpuDeviceDesc* desc, BgpuDevice* out_device);
/* 销毁设备会连带销毁它名下所有还活着的资源（C 没有析构，这里是最后一道保险）；
 * 但正确做法仍然是调用方按"先子后父"的顺序销毁。 */
void       bingpu_device_destroy(BgpuDevice device);
BgpuResult bingpu_device_info(BgpuDevice device, BgpuDeviceInfo* out_info);
BgpuResult bingpu_device_caps(BgpuDevice device, BgpuCaps* out_caps);
BgpuResult bingpu_device_stats(BgpuDevice device, BgpuStats* out_stats);
int        bingpu_device_lost(BgpuDevice device);
BgpuResult bingpu_device_wait_idle(BgpuDevice device);
/* 取一个待处理事件；队列空时返回 BGPU_OK 且 kind == BGPU_EVENT_NONE。
 * 这是**唯一的**事件通道（没有回调），外壳每帧调一次即可。 */
BgpuResult bingpu_poll(BgpuDevice device, BgpuEvent* out_event);

/* ---------------------------------------------------------------
 * 交换链 / 帧
 * --------------------------------------------------------------- */
typedef struct BgpuSwapchainDesc {
    uint32_t struct_size;
    void* native_window;    /* NULL = 无窗口（HEADLESS）：frame_read 出图 */
    uint32_t width;
    uint32_t height;
    uint32_t flags;         /* BGPU_DEVICE_VSYNC / _TRANSPARENT 的子集 */
    BgpuFormat format;      /* BGPU_FORMAT_RGBA8 或 BGRA8；后端可退回支持的格式 */
} BgpuSwapchainDesc;

typedef struct BgpuSwapchainInfo {
    uint32_t struct_size;
    uint32_t width;
    uint32_t height;
    BgpuFormat format;
    uint32_t image_count;
    uint32_t msaa_samples;
    uint32_t present_paces_frames;  /* 1 = present 自己掐拍，外壳别再节流 */
} BgpuSwapchainInfo;

BgpuResult bingpu_swapchain_create(BgpuDevice device, const BgpuSwapchainDesc* desc,
                                   BgpuSwapchain* out_swapchain);
void       bingpu_swapchain_destroy(BgpuDevice device, BgpuSwapchain swapchain);
BgpuResult bingpu_swapchain_resize(BgpuSwapchain swapchain, uint32_t width, uint32_t height);
BgpuResult bingpu_swapchain_info(BgpuSwapchain swapchain, BgpuSwapchainInfo* out_info);
int        bingpu_present_paces_frames(BgpuSwapchain swapchain);

/* 取当前帧的目标纹理：**借用**句柄，不许 destroy，有效期到 frame_end 为止。 */
BgpuResult bingpu_frame_begin(BgpuSwapchain swapchain, BgpuCmd cmd, BgpuTexture* out_target);
/* 提交 cmd 的内容并呈现；帧结束后 cmd 回到可记录状态。 */
BgpuResult bingpu_frame_end(BgpuSwapchain swapchain, BgpuCmd cmd);
/* 回读上一帧刚呈现的图像（HEADLESS 或调试用）；dst 必须是 RGBA8 直通。 */
BgpuResult bingpu_frame_read(BgpuSwapchain swapchain, void* dst, size_t dst_size,
                             uint32_t* out_stride);

/* ---------------------------------------------------------------
 * 纹理
 * --------------------------------------------------------------- */
typedef enum BgpuTextureFlag {
    BGPU_TEXTURE_RENDER_TARGET = 1u << 0, /* 可当渲染目标（离屏图层 / 视口 / 出图） */
    BGPU_TEXTURE_CPU_READ = 1u << 1,      /* 允许 texture_read 回读 */
    BGPU_TEXTURE_DYNAMIC = 1u << 2,       /* 每帧 upload 覆盖（视频帧 / 动态位图） */
    BGPU_TEXTURE_MIPMAPS = 1u << 3        /* 生成 mip 链（缩小时不闪） */
} BgpuTextureFlag;

typedef struct BgpuTextureDesc {
    uint32_t struct_size;
    uint32_t width;
    uint32_t height;
    BgpuFormat format;
    uint32_t flags;         /* BgpuTextureFlag 位或 */
    uint32_t mip_levels;    /* 0 = 1；MIPMAPS 时后端自动补全 */
} BgpuTextureDesc;

typedef struct BgpuTextureInfo {
    uint32_t struct_size;
    uint32_t width;
    uint32_t height;
    BgpuFormat format;
    uint32_t flags;
    uint32_t mip_levels;
} BgpuTextureInfo;

/* 上传/回读的区域；传 NULL 表示整张 */
typedef struct BgpuBox {
    uint32_t x, y, w, h;
} BgpuBox;

BgpuResult bingpu_texture_create(BgpuDevice device, const BgpuTextureDesc* desc,
                                 BgpuTexture* out_texture);
void       bingpu_texture_destroy(BgpuDevice device, BgpuTexture texture);
BgpuResult bingpu_texture_info(BgpuTexture texture, BgpuTextureInfo* out_info);
/* pixels 是直通 RGBA8（BGRA8 顺序随 format），行距 stride 字节；stride = 0 视为 w*4。
 * 允许在渲染线程外准备数据，但**调用**本身要在渲染线程上（和 binUI 一致）。 */
BgpuResult bingpu_texture_upload(BgpuTexture texture, const BgpuBox* box,
                                 const void* pixels, uint32_t stride);
/* 把纹理内容读回 CPU（布局同 upload；返回实际行距）。要求 CPU_READ 或 RENDER_TARGET。 */
BgpuResult bingpu_texture_read(BgpuTexture texture, void* dst, size_t dst_size,
                               uint32_t* out_stride);

/* ---------------------------------------------------------------
 * 缓冲（网格 / 顶点流；2D 四边形走 draw_quads 的 CPU 数组，不必建缓冲）
 * --------------------------------------------------------------- */
typedef enum BgpuBufferUsage {
    BGPU_BUFFER_VERTEX = 1u << 0,
    BGPU_BUFFER_INDEX = 1u << 1,   /* 32 位索引 */
    BGPU_BUFFER_UNIFORM = 1u << 2
} BgpuBufferUsage;

typedef struct BgpuBufferDesc {
    uint32_t struct_size;
    uint32_t size;      /* 字节 */
    uint32_t usage;     /* BgpuBufferUsage 位或 */
    uint32_t flags;     /* BGPU_TEXTURE_DYNAMIC 同义：每帧重写 */
} BgpuBufferDesc;

BgpuResult bingpu_buffer_create(BgpuDevice device, const BgpuBufferDesc* desc,
                                BgpuBuffer* out_buffer);
void       bingpu_buffer_destroy(BgpuDevice device, BgpuBuffer buffer);
BgpuResult bingpu_buffer_update(BgpuBuffer buffer, uint32_t offset, const void* data,
                                uint32_t size);

/* ---------------------------------------------------------------
 * 采样器（binUI 的颗粒噪声就是 NEEDEST + REPEAT 的那一组）
 * --------------------------------------------------------------- */
typedef enum BgpuFilter {
    BGPU_FILTER_LINEAR = 0,
    BGPU_FILTER_NEAREST = 1
} BgpuFilter;

typedef enum BgpuWrap {
    BGPU_WRAP_CLAMP = 0,
    BGPU_WRAP_REPEAT = 1
} BgpuWrap;

typedef struct BgpuSamplerDesc {
    uint32_t struct_size;
    BgpuFilter min_filter;
    BgpuFilter mag_filter;
    BgpuWrap wrap_u;
    BgpuWrap wrap_v;
} BgpuSamplerDesc;

BgpuResult bingpu_sampler_create(BgpuDevice device, const BgpuSamplerDesc* desc,
                                 BgpuSampler* out_sampler);
void       bingpu_sampler_destroy(BgpuDevice device, BgpuSampler sampler);

/* ---------------------------------------------------------------
 * 管线（固定功能；0.1.0 没有用户着色器 —— 那要等 binGPU 有 SPIR-V/HLSL
 * 编译通路，是 bin3d 进 GPU 阶段的前提，见 docs/backends.md）
 *
 * 混合模式是三端一致的关键，和 binUI 的 5 种状态一一对应：
 *   PREMULTIPLIED : src + dst*(1-srcA)   普通半透明绘制（预乘语义）
 *   REPLACE       : src                  覆盖（图层/毛玻璃合成、底色清空）
 *   KEEP          : dst * srcA           SVG <mask> 的保留趟
 *   ERASE         : dst * (1-srcA)       裁剪补集擦除（clip-path）
 *   ADDITIVE      : src + dst            高光 / 粒子
 * --------------------------------------------------------------- */
typedef enum BgpuBlend {
    BGPU_BLEND_PREMULTIPLIED = 0,
    BGPU_BLEND_REPLACE = 1,
    BGPU_BLEND_KEEP = 2,
    BGPU_BLEND_ERASE = 3,
    BGPU_BLEND_ADDITIVE = 4
} BgpuBlend;

typedef enum BgpuPipelineFlag {
    BGPU_PIPE_DEPTH_TEST = 1u << 0,   /* 打开深度测试（默认深度函数 LESS） */
    BGPU_PIPE_DEPTH_WRITE = 1u << 1,
    BGPU_PIPE_CULL_BACK = 1u << 2,    /* 背面剔除（3D） */
    BGPU_PIPE_TEXTURED = 1u << 3      /* 采样纹理；关掉时顶点色直接出（纯色） */
} BgpuPipelineFlag;

typedef struct BgpuPipelineDesc {
    uint32_t struct_size;
    BgpuVertexLayout vertex_layout;
    BgpuBlend blend;
    uint32_t flags;     /* BgpuPipelineFlag 位或 */
} BgpuPipelineDesc;

BgpuResult bingpu_pipeline_create(BgpuDevice device, const BgpuPipelineDesc* desc,
                                  BgpuPipeline* out_pipeline);
void       bingpu_pipeline_destroy(BgpuDevice device, BgpuPipeline pipeline);

/* ---------------------------------------------------------------
 * 命令缓冲
 *
 * 一个 cmd 同一时刻只能被一帧抓在手里（没有多线程记录 —— 那要线程池和
 * 记录期资源生命周期，和"上层每帧单线程记录"的现状不匹配）。
 * --------------------------------------------------------------- */
BgpuResult bingpu_cmd_create(BgpuDevice device, BgpuCmd* out_cmd);
void       bingpu_cmd_destroy(BgpuDevice device, BgpuCmd cmd);

/* 渲染目标可以是布局渲染目标纹理，也可以是 frame_begin 借来的交换链目标。
 * 深度：目标的 BgpuRenderBeginDesc 里 depth_texture 非 0 时才有深度附件。 */
typedef enum BgpuRenderFlag {
    BGPU_RENDER_CLEAR_COLOR = 1u << 0,
    BGPU_RENDER_CLEAR_DEPTH = 1u << 1
} BgpuRenderFlag;

typedef struct BgpuRenderBeginDesc {
    uint32_t struct_size;
    BgpuTexture target;
    BgpuTexture depth_target;   /* 可选；0 = 没有深度 */
    uint32_t flags;             /* BgpuRenderFlag */
    uint32_t clear_color;       /* 0xAARRGGBB 直通 */
    float clear_depth;          /* 默认 1.0f */
} BgpuRenderBeginDesc;

BgpuResult bingpu_cmd_begin_render(BgpuCmd cmd, const BgpuRenderBeginDesc* desc);
BgpuResult bingpu_cmd_end_render(BgpuCmd cmd);

/* viewport 用**目标像素**坐标：(x, y, w, h)，左上原点，y 向下。
 * 打开 set_affine 之后，顶点位置在这个 viewport 映射**之后**再被仿射矩阵变换
 * （即 xf 作用在屏幕空间），这样 binUI 那边"节点局部坐标 + 一个矩阵"的用法
 * 原样搬得过来。 */
BgpuResult bingpu_cmd_set_viewport(BgpuCmd cmd, float x, float y, float w, float h);
/* scissor 是**求交**语义：传矩形即裁剪，w/h 为负 = 关掉裁剪（全目标）。
 * 像素对齐取整，绝不留半像素缝（三端一致的老坑）。 */
BgpuResult bingpu_cmd_set_scissor(BgpuCmd cmd, int32_t x, int32_t y, int32_t w, int32_t h);

BgpuResult bingpu_cmd_set_pipeline(BgpuCmd cmd, BgpuPipeline pipeline);
/* texture 传 0 = 解绑（TEXTURED 管线下按白纹素采样）；sampler 传 0 = 默认线性/钳制 */
BgpuResult bingpu_cmd_set_texture(BgpuCmd cmd, BgpuTexture texture, BgpuSampler sampler);

/* 2×3 行主序仿射 [a c e; b d f]：屏幕 = (a*x + c*y + e, b*x + d*y + f)。
 * 传 NULL 复位成单位阵。这是**状态**而不是逐顶点参数 —— 文字/图标是几百个
 * 小四边形，逐四边形传矩阵不现实。 */
BgpuResult bingpu_cmd_set_affine(BgpuCmd cmd, const float xf[6]);

/* 一次下发 quad_count 个四边形，每 4 个顶点按 TL,TR,BL,BR 排。
 * 顶点是 CPU 数组：后端自己攒批后一次性提交（binUI 的三端都是这个形状，
 * 也是"每帧只传脏节点"的将来入口）。 */
BgpuResult bingpu_cmd_draw_quads(BgpuCmd cmd, const BgpuVertex* vertices,
                                 uint32_t quad_count);

/* 索引绘制（3D 网格）：顶点格式由管线的 vertex_layout 决定。 */
BgpuResult bingpu_cmd_draw_indexed(BgpuCmd cmd, BgpuBuffer vertex_buffer,
                                   uint32_t vertex_offset, BgpuBuffer index_buffer,
                                   uint32_t index_count, uint32_t index_offset);

/* ---------------------------------------------------------------
 * 错误
 *
 * 每个函数都可能失败；失败细节（文件名:行号 + 后端原话）走最后错误槽。
 * 槽是**线程本地**的：渲染线程读自己的，别的线程的失败不会串台。
 * 返回的指针有效期到本线程下一次 bingpu_* 调用为止（别存）。
 * --------------------------------------------------------------- */
const char* bingpu_result_name(BgpuResult result);
const char* bingpu_last_error(void);
void        bingpu_clear_error(void);

/* 版本 / 自检 */
uint32_t    bingpu_abi_version(void);
const char* bingpu_version_string(void);
/* 进程级退出清理（后端全局状态、驱动 DLL 卸载）。可重复调用。 */
void        bingpu_shutdown(void);

#ifdef __cplusplus
} /* extern "C" */
#endif
