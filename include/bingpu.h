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
#define BINGPU_VERSION_MINOR 13
#define BINGPU_VERSION_PATCH 0
#define BINGPU_VERSION_STRING "0.13.0"

/* 二进制 ABI 号：结构体布局 / 函数签名 / 枚举值一变就 +1
 *   ABI 1：0.1.0 - 0.3.0（初始 ABI；0.2.0 只换了实现语言，ABI 未动）
 *   ABI 2：0.4.0（新增 BGPU_BACKEND_D3D11 枚举值；既有值、结构体、签名全部不变）
 *          0.5.0 落 GL 后端时**一个字节都没动** —— GL 的枚举值 0.1.0 就在。
 *   ABI 3：0.6.0（**BgpuMeshVertex 加 float w** —— 透视校正的源头；
 *          新增 BgpuLight + bingpu_cmd_set_light；新增两个 cap 位。
 *          2D 路径（BgpuVertex / draw_quads）一个字节没动。）
 *   ABI 4：0.12.0（新增 BGPU_VERTEX_SHAPE 枚举值 + BgpuShapeVertex +
 *          BGPU_PIPE_SHAPE + BGPU_CAP_SHAPE + bingpu_cmd_draw_shapes；
 *          既有结构体、枚举值、签名全部不变）
 *   0.13.0 是**纯 VK 后端实现**的修复（布局/依赖/顶点流），ABI 仍是 4 —— 一个字节没动。 */
#define BINGPU_ABI_VERSION 4u

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
 * 渲染计划（0.8.0）—— **"怎么画、用什么画"由 binGPU 决定**
 *
 * 分层分工：binUI 定功能（节点/CSS/布局/语义原语），bin2d/bin3d 定绘制语义，
 * 而"用哪个 API、在哪执行、什么质量档"是**这一层**的事。上层不该再按
 * `vk/gl/d3d` 自己挑后端（那是把决策权抢回来），只需要问一句 plan。
 *
 *   API 轴  ：Vulkan / GL / D3D11 / CPU
 *  执行轴  ：同一套 2D 图元语义既能在 GPU 上求值，也能在 CPU 上求值 ——
 *            两条路用**同一份解析式（SDF）**，所以"换执行体不换像素"。
 *            选哪边由 `exec` 决定（设备能力 + 这笔活在哪更划算）。
 *  质量轴  ：msaa_samples / aa 方式，由这里给，不由调用方猜。
 * --------------------------------------------------------------- */
typedef enum BgpuExecMode {
    BGPU_EXEC_GPU = 0,   /* 设备上执行（各 API 后端） */
    BGPU_EXEC_CPU = 1    /* CPU 上执行（同一套图元语义，不依赖显卡/驱动） */
} BgpuExecMode;

typedef struct BgpuRenderPlan {
    uint32_t        struct_size;
    BgpuBackendKind backend;      /* 选中的后端（CPU 模式时为 BGPU_BACKEND_CPU） */
    BgpuExecMode    exec;         /* 在哪执行 */
    uint32_t        msaa_samples; /* 质量档：1 / 2 / 4 / 8（CPU 模式恒为 1） */
    uint32_t        flags;        /* 保留（BGPU_PLAN_*），当前恒 0 */
} BgpuRenderPlan;

/* 让 binGPU 出计划。
 *   want = BGPU_BACKEND_AUTO 时按 **VULKAN → GL → D3D11** 取第一个可用的 GPU 后端；
 *          一个都没有则返回 exec=CPU、backend=CPU（也就是"没显卡，用 CPU 算"）。
 *   want 指定某个后端时，只在那一个上判断（不可用则同样退 CPU 并在 last_error 里说明）。
 * 这是**唯一**该被上层用来决定"用 GPU 还是 CPU"的入口。 */
BgpuResult bingpu_render_plan(BgpuBackendKind want, BgpuRenderPlan* out_plan);
/* "gpu" / "cpu"，只用于日志 */
const char* bingpu_exec_mode_name(BgpuExecMode mode);

/* ---------------------------------------------------------------
 * 帧节拍器（0.9.0）：**"这一拍该不该画"由设备层回答**
 *
 * 为什么要它，以及为什么在这一层：
 *   1. GPU 画 1020×684 一帧只需要 0.54 ms（D3D11 实测 1848 fps，Vulkan 977 fps）。
 *      也就是说**帧率从来不是画出来的上限，而是被节拍器掐出来的上限**。
 *   2. 掐拍的输入全是设备层的东西：显示器刷新率、交换链 present mode、
 *      present 到底有没有阻塞到垂直同步、上一拍 present 花了多久。
 *      上层猜这些只能瞎猜（曾经在外壳里用 `present 耗时 >= 半个刷新周期` 猜，
 *      结果在 Vulkan 上每次多睡一格 → present 等一格 + 外壳再等一格 = 双重等待，
 *      实测 120 Hz 屏上掉到 55 fps，而同一份代码裸跑 present 是 119.9 fps）。
 *   3. 所以：**策略由上层给（bin2D/bin3D 的绘制语义层），闸门在这里算。**
 *      上层只有两个问题要问：`wait_us()` 还要等多久、`wants_vsync()` 该不该开垂直同步。
 *
 * 节拍规则（一句话）：**下一拍 = max(本拍开始 + 目标周期, 本拍结束)**。
 *   · `max(..., 本拍结束)` 就是防双重等待的那一刀：present 已经等掉了时间，
 *     就不要在它后面再补一整格 —— 这是 55 fps 那个坑的唯一根因。
 *   · 目标周期 = 刷新网格（或比刷新更细的网格，见 PACE_240）× 整数 k，
 *     k 由"本拍真实耗时"带滞回地往上调（装不下就升档，稳稳装得下才降档）。
 * --------------------------------------------------------------- */
typedef enum BgpuPacePolicy {
    /* 跟显示器刷新率：**上限**是刷新率，**不设下限**。
     *   · 画得完（一帧 ≤ 一个刷新周期）→ 钳在刷新率上，多的帧没意义还烧电；
     *   · 画不完（一帧 > 一个刷新周期）→ **尽力多画**，不往"整数个刷新周期"上凑。
     *     9.5 ms 的帧就是 ~105 fps，不会被硬凑成 2 个周期（60 fps）——
     *     凑整只对"明确要求 60/30"有意义，对"标准"就是白白扔掉 40% 的帧。
     * 这条是 2026-11-20 master 定的口径。 */
    BGPU_PACE_STANDARD = 0,
    BGPU_PACE_UNLIMITED = 1,  /* 完全不掐拍：能画多快画多快（需要关 vsync，会撕裂） */
    BGPU_PACE_240 = 2,
    BGPU_PACE_120 = 3,
    BGPU_PACE_90 = 4,
    BGPU_PACE_60 = 5,
    BGPU_PACE_30 = 6
} BgpuPacePolicy;

typedef struct BgpuPacer {
    uint32_t struct_size;
    int32_t  refresh_hz;      /* 显示器刷新率；<=0 时按 60 算 */
    int32_t  policy;          /* BgpuPacePolicy */
    int32_t  steps;           /* 上一拍占几个网格（滞回状态；0 = 还没学过） */
    int32_t  started;         /* 是否已经有基准 */
    int32_t  frame_open;      /* frame_begin 已经调过且还没 frame_end（0 是合法时间戳，不能拿它当哨兵）*/
    int64_t  next_us;         /* 下一拍最早开始时刻（调用方自己的时钟，微秒） */
    int64_t  frame_start_us;  /* 本拍开始时刻 */
    double   last_present_ms; /* 上一拍 present 耗时（用来估计"present 自己等了多少"） */
    double   last_frame_ms;   /* 上一拍 end - start */
    uint32_t flags;           /* 保留 */
} BgpuPacer;

/* "standard" / "unlimited" / "240" / "120" / "90" / "60" / "30"（大小写不敏感，
 * "0" 也接受为 unlimited）。成功返回 1。 */
int bingpu_pace_policy_parse(const char* text, BgpuPacePolicy* out_policy);
const char* bingpu_pace_policy_name(BgpuPacePolicy policy);
/* 策略的目标帧率；0 = 不掐拍（BGPU_PACE_UNLIMITED） */
int bingpu_pace_policy_fps(BgpuPacePolicy policy);

/* 显示器刷新率（现在挂在哪个窗口上就按哪台显示器算）。
 * 拿不到 / 远程会话 / 驱动乱报时返回 0（调用方按 60 处理）。 */
int bingpu_display_refresh_hz(void* native_window);

/* 初始化：refresh_hz 探测一次，policy = STANDARD，节拍状态清空。
 * native_window 可以为空（离屏），此时 refresh_hz 取 60。 */
void bingpu_pace_init(BgpuPacer* pacer, void* native_window);
/* 改策略。策略一变就丢掉相位重新学（避免拿 30 fps 的档位去套 120 fps）。 */
BgpuResult bingpu_pace_set_policy(BgpuPacer* pacer, BgpuPacePolicy policy);
BgpuPacePolicy bingpu_pace_policy(const BgpuPacer* pacer);
/* 目标帧间隔（ms）；0 = 不掐拍 */
double bingpu_pace_frame_ms(const BgpuPacer* pacer);
/* 这个策略下 vsync 该不该开：目标帧率 > 刷新率（含 unlimited）时必须关，
 * 否则 present 会把帧率钉在刷新率上，"240 fps" 就是一句空话。 */
int bingpu_pace_wants_vsync(const BgpuPacer* pacer);
/* 现在(now_us)还要等多少微秒才能开始下一拍；0 = 立刻可以画。 */
int64_t bingpu_pace_wait_us(const BgpuPacer* pacer, int64_t now_us);
/* 一帧开始画（beginFrame 之前调） */
void bingpu_pace_frame_begin(BgpuPacer* pacer, int64_t now_us);
/* 一帧画完且 present 已返回：end_us = present 返回的时刻，
 * present_ms = present() 自身的耗时（含它自己等垂直同步的那一段）。 */
void bingpu_pace_frame_end(BgpuPacer* pacer, int64_t end_us, double present_ms);

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
    BGPU_VERTEX_MESH = 1,
    /* 0.12.0：**解析圆角**（有符号距离场）四边形。片元着色器自己算覆盖率，
     * 不再吃"CPU 生成好的覆盖率模板"—— AA 数学真的跑在 GPU 上。 */
    BGPU_VERTEX_SHAPE = 2
} BgpuVertexLayout;

typedef struct BgpuVertex {
    float x, y, z;      /* 屏幕/裁剪空间由 viewport + affine 决定；z 用于深度 */
    float u, v;         /* 归一化纹理坐标 */
    uint32_t color;     /* 0xAARRGGBB 直通 */
} BgpuVertex;

typedef struct BgpuMeshVertex {
    float x, y, z;
    /* 世界空间法线（上层自己变换好；bingpu 只管插值与 Lambert）。
     * 为什么是世界空间而不是模型空间：固定管线没有"模型矩阵"这个状态 ——
     * 顶点位置送进来时已经是目标像素坐标，法线如果还是模型空间就没法参与光照。 */
    float nx, ny, nz;
    float u, v;
    uint32_t color;
    /* 裁剪空间的**原始 w**（透视除法之前）；0 视为 1（正交投影）。
     *
     * 为什么必须要它（ABI 3 新增）：顶点位置送进来时已经除过 w，属性如果按屏幕空间
     * 线性插值，"近处三角形被拉长、UV 却按屏幕比例走"—— 纹理在斜面上会明显滑动。
     * 上层把原始 w 带下来，后端用 `Σ(attr/w·λ) / Σ(1/w·λ)` 插值就是透视正确的。
     * 2D 四边形（BGPU_VERTEX_2D）没有这个字段：它按定义就在屏幕空间。 */
    float w;
} BgpuMeshVertex;

BGPU_STATIC_ASSERT(sizeof(BgpuVertex) == 24, "BgpuVertex 必须 24 字节");
BGPU_STATIC_ASSERT(sizeof(BgpuMeshVertex) == 40, "BgpuMeshVertex 必须 40 字节（ABI 3）");

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
    BGPU_CAP_AFFINE = 1u << 9,          /* 支持 set_affine（任意四边形） */
    /* ABI 3：追加在末尾，旧枚举值一个都没动 */
    BGPU_CAP_LIGHTING = 1u << 10,       /* 固定功能 Lambert（BGPU_PIPE_LIGHTING 可用） */
    BGPU_CAP_PERSPECTIVE = 1u << 11,    /* 网格顶点带 w、属性按 1/w 透视校正插值 */
    BGPU_CAP_SHAPE = 1u << 12           /* 解析圆角（BGPU_PIPE_SHAPE + draw_shapes） */
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
    BGPU_TEXTURE_MIPMAPS = 1u << 3,       /* 生成 mip 链（缩小时不闪） */
    /* 上传的数据**已经是预乘**的：后端跳过 straight→premul 这一步，原样存进去。
     * 给"上游本来就是预乘"的场景用（bin2D 的 Surface 就是预乘，每帧整张直传），
     * 省掉一次反预乘 + 再预乘的往返 —— 那趟往返每帧要多分配一块同尺寸的直通缓冲、
     * 再做 3×像素数 次整数除法，而且不是无损的。
     * 只影响 upload 的**解释方式**：纹理在采样/混合里始终按预乘参与（与其他纹理一致）。 */
    BGPU_TEXTURE_PREMULTIPLIED = 1u << 4
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
    BGPU_PIPE_TEXTURED = 1u << 3,     /* 采样纹理；关掉时顶点色直接出（纯色） */
    /* ABI 3：固定功能 Lambert。只对 BGPU_VERTEX_MESH 有意义（需要法线），
     * 光照参数由 bingpu_cmd_set_light 给。2D 管线设这一位是 BGPU_ERR_INVALID_ARGUMENT。 */
    BGPU_PIPE_LIGHTING = 1u << 4,
    /* ABI 4：解析圆角（SDF）管线。必须配 BGPU_VERTEX_SHAPE；
     * 覆盖率在片元着色器里算，**不采样纹理**。 */
    BGPU_PIPE_SHAPE = 1u << 5
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

/* ---------------------------------------------------------------
 * 光照（ABI 3）：固定功能 Lambert，与 bin3d 软光栅的公式逐字一致
 *
 *   片元颜色 = base × vcol × tex × (ambient + lightColor · max(dot(n, dir), 0))
 *
 * 其中 n 是**世界空间**法线的透视校正插值再归一化。base 是上层的材质色
 * （折进顶点色送下来）。整套只有三个参数，没有衰减、没有多光源、没有 specular ——
 * 因为这些是"上层 3D 语义"，不是设备层该管的。真要更复杂的光照，那是自定义
 * 着色器的事（见 docs/backends.md 的"用户着色器"一节）。
 *
 * 默认值必须与 bin3d 的 ShadeCtx 一致（dir=(0,1,0), color=(1,1,1), ambient=(0.1,0.1,0.1)），
 * 否则"没设光照就跑"的那一帧两边对不上。
 * 传 NULL = 复位成默认。requires BGPU_PIPE_LIGHTING 的管线。
 * --------------------------------------------------------------- */
typedef struct BgpuLight {
    float dir[3];       /* 归一化方向（指向光源） */
    float color[3];     /* 光源颜色（线性，可 > 1 用来过曝） */
    float ambient[3];   /* 环境光 */
} BgpuLight;

BgpuResult bingpu_cmd_set_light(BgpuCmd cmd, const BgpuLight* light);

/* ---------------------------------------------------------------
 * 解析圆角四边形（0.12.0）：BGPU_VERTEX_SHAPE 的顶点
 *
 * 为什么要有它：0.10.0 的圆角是"CPU 算一张覆盖率位图 → GPU 九宫格拉伸"。那条路
 * 的 AA 数学**在 CPU 上**，而且位图只能在整像素相位上生成。这里改成片元着色器
 * 一句有符号距离场（SDF）：
 *
 *     q = abs(p) - b + r;  d = length(max(q,0)) + min(max(q.x,q.y),0) - r;
 *     cov = clamp(0.5 - d, 0, 1);        // 轴对齐 1:1 时 |grad d| 恒为 1 ⇒ 1px 解析过渡
 *
 * 于是：AA 在 GPU 上逐像素算、不要纹理、不要模板缓存、半径不再是纹理键
 * （所有圆角矩形可以一次 draw 画完），而且相位精确到浮点。
 *
 * 顶点四个角按 TL,TR,BL,BR（与 BGPU_VERTEX_2D 一致）。字段口径：
 *   x, y       目标像素位置（走与 2D 相同的 viewport/affine）
 *   lx, ly     **相对盒子中心**的局部坐标（顶点处 = ±hx, ±hy；片元里线性插值，
 *              因为它是屏幕坐标的仿射函数，插值是精确的）
 *   hx, hy     盒子半尺寸（四个顶点相同）—— SDF 需要它，不能从 lx/ly 推回来
 *   radius     圆角半径，内部夹到 min(hx,hy)
 *   thick      0 = 实心；> 0 = **向内** thickness 的环带（与 bin2d 的 strokeRounded 同义）
 *   tint       0xAARRGGBB **直通**（与 BGPU_VERTEX_2D 的顶点色同口径）
 *
 * 已知边界：`scale` 由 transform 决定，而这条管线只在**轴对齐 1:1**（含整数平移）时
 * 精确 —— 旋转/缩放的圆角仍走模板那条路（bin2d 侧按 transform 选路）。
 * --------------------------------------------------------------- */
typedef struct BgpuShapeVertex {
    float x, y;
    float lx, ly;
    float hx, hy;
    float radius;
    float thick;
    uint32_t tint;
} BgpuShapeVertex;

BGPU_STATIC_ASSERT(sizeof(BgpuShapeVertex) == 36, "BgpuShapeVertex 必须 36 字节");

/* 一次下发 quad_count 个四边形，每 4 个顶点按 TL,TR,BL,BR 排。
 * 顶点是 CPU 数组：后端自己攒批后一次性提交（binUI 的三端都是这个形状，
 * 也是"每帧只传脏节点"的将来入口）。 */
BgpuResult bingpu_cmd_draw_quads(BgpuCmd cmd, const BgpuVertex* vertices,
                                 uint32_t quad_count);

/* 解析圆角四边形（0.12.0）：quad_count 个 BGPU_VERTEX_SHAPE 四边形一次提交，
 * 每次 4 个顶点按 TL,TR,BL,BR。要求管线是 BGPU_PIPE_SHAPE —— 覆盖率由片元着色器
 * 用有符号距离场算，不绑纹理。后端不支持时返回 BGPU_ERR_UNSUPPORTED
 * （先查 BGPU_CAP_SHAPE）。 */
BgpuResult bingpu_cmd_draw_shapes(BgpuCmd cmd, const BgpuShapeVertex* vertices,
                                  uint32_t quad_count);



/* ---------------------------------------------------------------
 * 2D 图元（0.10.0）：把"圆角矩形怎么拆成块"收进设备层
 *
 * 分工（与帧节拍器同一个道理）：
 *   · **AA 数学在调用方** —— "圆角看起来是什么样"是绘制语义，所以覆盖率模板由
 *     bin2d 的 `RasterizeRoundedTileCoverage` 出（它是唯一一份解析式，仓里三个 GPU 后端
 *     的圆角能逐字节一致就是靠共用它）；
 *   · **拆几块、怎么合批、走哪个后端** 是"怎么画" —— 在这一层。
 *
 * 实现上就是"把模板铺成 9 块，转成一次 draw_quads"，所以五个后端（CPU / VK / GL /
 * D3D11 / NULL）**零改动**就能画，像素也天然一致 —— 这正是这一层存在的意义：
 * 上层只描述形状，几何展开与合批在设备层。
 *
 * 调用前提：已经有 begin_render、已 `bingpu_cmd_set_pipeline`（一般 TEXTURED +
 * 目标混合模式）。这两个函数只负责绑模板纹理 + 发顶点。
 * --------------------------------------------------------------- */

/* 圆角实心：coverage 是 t×t 的覆盖率模板（t = tile_size，四通道都等于覆盖率），
 * radius 夹到 min(w,h)/2（与 bin2d 的 FillRounded 同一口径）。radius <= 0 或
 * tile_size <= 0 返回 BGPU_ERR_INVALID_ARGUMENT（调用方应改走 draw_quads 的直角路径）。
 * tint 是 0xAARRGGBB 直通（与顶点色同一口径）。
 *
 * **粗描边不在这一层循环**：粗描边 = 同心 1px 环叠层，每一层的半径不同、要一张**不同**的
 * 覆盖率模板，而模板缓存归调用方（只有它知道半径取整与缓存键）。所以这里只画"一层"，
 * 多层由调用方连着调 —— 这一层负责的是"怎么把一层变成四边形"。 */
BgpuResult bingpu_cmd_draw_rounded_rect(BgpuCmd cmd, BgpuTexture coverage,
                                        float x, float y, float w, float h,
                                        float radius, int32_t tile_size, uint32_t tint);

/* 同一张模板、一次绑定、**一次 draw_quads** 画 N 个圆角矩形。
 *
 * 为什么要有数组版（合批是这个层的职责，不是上层的）：
 *   234 个卡片如果各调一次 `draw_rounded_rect`，就是 234 次绑纹理 + 234 次 draw_quads
 *   + 234 次顶点提交。实测（1020×684、234 个 100×32 的圆角矩形）：
 *   逐个调用 CPU 后端 20.4 ms，而一次批量只有几毫秒 —— 差的全是调用开销，
 *   跟"画多少像素"无关。半径相同的形状在 UI 里是**绝大多数**（10 / 12 / 14 这几档），
 *   所以调用方按模板分组之后就能一次发完。
 */
typedef struct BgpuRoundedRect {
    float x, y, w, h;
    float radius;      /* 每项自己夹到 min(w,h)/2 */
    uint32_t tint;     /* 0xAARRGGBB 直通 */
} BgpuRoundedRect;

BgpuResult bingpu_cmd_draw_rounded_rects(BgpuCmd cmd, BgpuTexture coverage,
                                         int32_t tile_size, const BgpuRoundedRect* rects,
                                         uint32_t count);

/* 四角带色的圆角矩形（0.11.0）：同一次九宫格展开，颜色由**四角**给。
 *
 * 为什么要这个变体：圆角底的渐变不能拆成"先画方渐变、再拿模板抠"（没有 stencil，
 * 也没有第二次 pass）。而线性渐变在屏幕空间就是一个仿射函数，仿射函数被四边形上的
 * 双线性插值精确复现 —— 于是"一个九宫格 + 四角带色"就是**精确解**，不是近似。
 *
 * 口径：corner_tints 按 TL, TR, BL, BR 顺序（与 draw_quads 的顶点顺序一致），
 * 颜色是**预乘**的 0xAARRGGBB（与顶点色同一口径）。每个顶点的颜色 = 这四角在
 * "该顶点于盒子里的归一化位置"上的双线性插值；盒子四角上正好取回传入的值。
 *
 * 为什么插值放在这一层而不是让调用方自己算九块：九宫格的块边界（角片边长 = r、
 * 中段 UV 取模板正中 1 个像素）是这一层的知识，调用方只描述"四角什么颜色"。
 * 这一层不认识"渐变"—— 它只做"四色在矩形上插值"，那是纯几何。 */
BgpuResult bingpu_cmd_draw_rounded_rect_grad(BgpuCmd cmd, BgpuTexture coverage,
                                            float x, float y, float w, float h,
                                            float radius, int32_t tile_size,
                                            const uint32_t corner_tints[4]);

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
