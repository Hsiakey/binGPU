/* bingpu_internal.h —— binGPU 内部契约（不对外，也不进 include/）
 *
 * 为什么要有这一层：
 *   公共头 include/bingpu.h 是"句柄 + 扁平函数"，但实现需要三样它不能有的东西：
 *   ① 一张 句柄 → 对象 的表（句柄是 uint64_t，不能是指针）；
 *   ② 一个后端虚表（NULL / CPU / 将来的 VK 各实现一份，上层入口按句柄找到后端转发）；
 *   ③ 线程本地的错误槽（公共头只暴露字符串）。
 *   这些都放在这里，并且**只有 .c 引用** —— 一旦 include/ 里出现这层的东西，
 *   abi-check 那关会把它当 C++ 一起拦下来（本文件用到 _Thread_local 等 C11 设施）。
 *
 * 句柄布局（64 位，全 0 = 无效）：
 *   [63:60] 对象种类（1..7）  [59:32] 代数（28 位）  [31:0] 槽位 + 1
 *   代数在每次 destroy 时 +1：旧句柄于是"自然失效"，不需要额外的黑名单。
 *   槽位 +1 是为了让"第 0 槽"的句柄不等于 0（0 必须永远是无效句柄）。
 */
#ifndef BINGPU_INTERNAL_H
#define BINGPU_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "bingpu.h"

/* ---------------------------------------------------------------
 * 对象种类（句柄高 4 位）
 * --------------------------------------------------------------- */
enum BgpuObjKind {
    BGPU_OBJ_DEVICE = 1,
    BGPU_OBJ_SWAPCHAIN = 2,
    BGPU_OBJ_TEXTURE = 3,
    BGPU_OBJ_BUFFER = 4,
    BGPU_OBJ_SAMPLER = 5,
    BGPU_OBJ_PIPELINE = 6,
    BGPU_OBJ_CMD = 7
};

#define BGPU_HANDLE_KIND_SHIFT 60
#define BGPU_HANDLE_GEN_SHIFT 32
#define BGPU_HANDLE_INDEX_MASK 0xFFFFFFFFull

/* 对象标志：借用对象不许被销毁（交换链借出的帧目标，见 docs/abi.md） */
#define BGPU_OBJ_F_BORROWED 1u

/* 每个资源对象的头部：定位后端、判断"属于哪个设备"全靠它 */
typedef struct BgpuObjectBase {
    uint32_t kind;       /* BgpuObjKind */
    uint32_t backend;    /* BgpuBackendKind */
    uint32_t obj_flags;  /* BGPU_OBJ_F_* */
    uint32_t reserved;   /* 对齐用，保持头部 24 字节 */
    BgpuDevice device;   /* 所属设备句柄；设备自己为 0 */
} BgpuObjectBase;

/* ---------------------------------------------------------------
 * 对象表
 *
 * 设计取舍：析构函数**存在表里**（和对象一起注册），而不是在后端虚表里再开一组
 * xxx_destroy。理由：销毁路径有两条 —— 显式 destroy 和"设备销毁时连带清理" ——
 * 两条都必须做同一件事（释放像素/缓冲 + 摘槽）；存一份就不可能两边走偏。
 * 于是后端的 destroy 义务只有"把资源指针交给 dtor"这一件。
 * --------------------------------------------------------------- */
/* 注册新对象；失败返回 BGPU_INVALID（内存不够）。dtor 可为 NULL。 */
uint64_t bgpu_obj_new(uint32_t kind, BgpuBackendKind backend, BgpuDevice owner,
                      size_t bytes, void (*dtor)(void*));
/* 按句柄取对象；种类不符 / 已销毁 / 越界一律返回 NULL。kind 传 0 表示不限种类。 */
void* bgpu_obj_get(uint64_t handle, uint32_t kind);
/* 销毁单个对象：调 dtor、清槽、代数 +1。无效句柄无副作用。 */
void bgpu_obj_destroy(uint64_t handle);
/* 销毁某设备名下所有还活着的对象（不含设备自己），返回销毁个数。 */
uint32_t bgpu_obj_destroy_owned(BgpuDevice device);
/* 还活着的对象数（shutdown 自检 / bingpu_info 用）。 */
uint32_t bgpu_obj_count(void);
/* 清空整张表（bingpu_shutdown 用；调用前必须保证对象已全部 destroy 过）。 */
void bgpu_obj_reset(void);

/* ---------------------------------------------------------------
 * 错误槽
 *
 * 一切失败都必须让 last_error 非空 —— 这是 docs/abi.md 里对绑定的承诺。
 * 分工：
 *   · 上层入口（api.c）发现参数/句柄/顺序问题 → bgpu_fail(...) 直接写完整串；
 *   · 后端只用自己的上下文细节写"原因"（bgpu_set_reason），由入口拼成
 *     "<函数>: <原因> (file:line)"。
 * 这样后端不需要知道公共函数叫什么，错误串又总是以调用方看见的函数名开头。
 * --------------------------------------------------------------- */
unsigned bgpu_error_seq(void);                 /* 单调递增；用来判断"这次调用有没有写错误" */
void     bgpu_set_reason(const char* fmt, ...);
const char* bgpu_take_reason(void);            /* 取走原因（取后清空），没写过返回 "" */

BgpuResult bgpu_fail(BgpuResult result, const char* fn, const char* file, int line,
                     const char* fmt, ...);
BgpuResult bgpu_fail_from(BgpuResult result, const char* fn, const char* file, int line);

/* 描述结构体的最低长度校验（ABI 尾字段兼容：只要求覆盖当前已知布局） */
static inline int bgpu_size_ok(uint32_t struct_size, size_t want) {
    return struct_size >= (uint32_t)want;
}

/* ---------------------------------------------------------------
 * 后端虚表
 *
 * 入参都是"对象指针"（后端自己的结构体），不是句柄 —— 句柄查找/校验/设备归属
 * 留在 api.c，后端只管语义。返回码必须是 BgpuResult；失败时后端应调
 * bgpu_set_reason() 留下原话（可以不留，入口会退化成结果名）。
 * --------------------------------------------------------------- */
typedef struct BgpuVt {
    BgpuBackendKind kind;
    const char* name;
    int (*available)(void);
    const char* unavailable_reason;   /* available()==0 时给调用方的解释 */

    /* 设备 */
    BgpuResult (*device_create)(const BgpuDeviceDesc* desc, BgpuDevice* out_device);
    void (*device_info)(void* dev, BgpuDeviceInfo* out);
    void (*device_caps)(void* dev, BgpuCaps* out);
    void (*device_stats)(void* dev, BgpuStats* out);
    int (*device_lost)(void* dev);

    /* 交换链 / 帧 */
    BgpuResult (*swapchain_create)(void* dev, const BgpuSwapchainDesc* desc,
                                   BgpuSwapchain* out);
    BgpuResult (*swapchain_resize)(void* sc, uint32_t w, uint32_t h);
    void (*swapchain_info)(void* sc, BgpuSwapchainInfo* out);
    BgpuResult (*frame_begin)(void* sc, void* cmd, BgpuTexture* out_target);
    BgpuResult (*frame_end)(void* sc, void* cmd);
    BgpuResult (*frame_read)(void* sc, void* dst, size_t dst_size, uint32_t* out_stride);

    /* 纹理 */
    BgpuResult (*texture_create)(void* dev, const BgpuTextureDesc* desc, BgpuTexture* out);
    void (*texture_info)(void* tex, BgpuTextureInfo* out);
    BgpuResult (*texture_upload)(void* tex, const BgpuBox* box, const void* pixels,
                                 uint32_t stride);
    BgpuResult (*texture_read)(void* tex, void* dst, size_t dst_size, uint32_t* out_stride);

    /* 缓冲 */
    BgpuResult (*buffer_create)(void* dev, const BgpuBufferDesc* desc, BgpuBuffer* out);
    BgpuResult (*buffer_update)(void* buf, uint32_t offset, const void* data, uint32_t size);

    /* 采样器 / 管线 */
    BgpuResult (*sampler_create)(void* dev, const BgpuSamplerDesc* desc, BgpuSampler* out);
    BgpuResult (*pipeline_create)(void* dev, const BgpuPipelineDesc* desc, BgpuPipeline* out);

    /* 命令缓冲 */
    BgpuResult (*cmd_create)(void* dev, BgpuCmd* out);
    BgpuResult (*cmd_begin_render)(void* cmd, const BgpuRenderBeginDesc* desc);
    BgpuResult (*cmd_end_render)(void* cmd);
    BgpuResult (*cmd_set_viewport)(void* cmd, float x, float y, float w, float h);
    BgpuResult (*cmd_set_scissor)(void* cmd, int32_t x, int32_t y, int32_t w, int32_t h);
    BgpuResult (*cmd_set_pipeline)(void* cmd, void* pipeline);
    BgpuResult (*cmd_set_texture)(void* cmd, void* tex, void* sampler);
    BgpuResult (*cmd_set_affine)(void* cmd, const float xf[6]);
    BgpuResult (*cmd_draw_quads)(void* cmd, const BgpuVertex* vertices, uint32_t quad_count);
    BgpuResult (*cmd_draw_indexed)(void* cmd, void* vb, uint32_t vertex_offset, void* ib,
                                   uint32_t index_count, uint32_t index_offset);
} BgpuVt;

const BgpuVt* bgpu_vt(BgpuBackendKind kind);   /* 未注册的种类返回 NULL */
const BgpuVt* bgpu_vt_auto(void);              /* AUTO 的挑选结果（见 bingpu_error.c） */
/* "这个后端为什么不可用"的可读原因（未登记种类返回"未知后端"） */
const char* bgpu_backend_unavailable_reason(BgpuBackendKind kind);

extern const BgpuVt bgpu_null_vt;
extern const BgpuVt bgpu_cpu_vt;

/* 两个后端共用的能力上限，避免"这里能跑那里不能"的糊味 */
#define BGPU_MAX_TEXTURE_SIZE 8192u
#define BGPU_MAX_QUADS_PER_DRAW (1u << 20)

#endif /* BINGPU_INTERNAL_H */
