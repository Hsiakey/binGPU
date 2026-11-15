/* bingpu_error.c —— 错误槽 / 名字表 / 版本 / 后端注册表 / 进程退出清理
 *
 * 为什么错误槽是 _Thread_local：
 *   ABI 里没有"错误对象"可以传，只能靠进程/线程级的最后错误。进程级的话，
 *   一个线程的失败会盖掉另一个线程正在看的原因（渲染线程 + 资源加载线程是
 *   完全现实的组合）。_Thread_local 是 C11 标准设施，代价是一次 TLS 寻址，
 *   换来"谁失败谁读得到"。字符串**不跨调用**持有：头文件写明"有效期到本线程
 *   下一次 bingpu_* 调用为止"，所以实现就是每线程一块静态缓冲，不分配内存。
 *
 * 为什么后端注册表放在这个文件：
 *   注册表要回答的三个问题（有哪些后端、能不能用、叫什么）和错误串是同一类
 *   "元信息"；而且 AUTO 的挑选结果要能写进错误串（"AUTO 挑了 cpu"）。放在一起
 *   少一个 .c，也少一处"两边各有一份顺序表"的隐患。
 *
 * 为什么 VK/D3D12/GL 明明没实现也要登记：
 *   bingpu_backend_count/at 是"这台机器上 binGPU 认识哪些后端"的稳定枚举。
 *   上层按索引探测能力时，看到 vulkan 存在但 available()==0 + 明确的 reason，
 *   比"枚举里根本没这一项"更好写（也避免以后加了 VK 之后枚举顺序变化）。
 */
#include "bingpu_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ============================================================
 * 错误槽
 * ============================================================ */
static _Thread_local char t_err[512];
static _Thread_local char t_reason[512];
static _Thread_local unsigned t_seq;

unsigned bgpu_error_seq(void) { return t_seq; }

static const char* base_name(const char* path) {
    const char* base = path;
    for (const char* p = path; p && *p; p++) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    return base;
}

const char* bingpu_result_name(BgpuResult result) {
    switch (result) {
        case BGPU_OK: return "BGPU_OK";
        case BGPU_ERR_INVALID_ARGUMENT: return "BGPU_ERR_INVALID_ARGUMENT";
        case BGPU_ERR_INVALID_HANDLE: return "BGPU_ERR_INVALID_HANDLE";
        case BGPU_ERR_UNSUPPORTED: return "BGPU_ERR_UNSUPPORTED";
        case BGPU_ERR_OUT_OF_MEMORY: return "BGPU_ERR_OUT_OF_MEMORY";
        case BGPU_ERR_DEVICE_LOST: return "BGPU_ERR_DEVICE_LOST";
        case BGPU_ERR_STATE: return "BGPU_ERR_STATE";
        case BGPU_ERR_BACKEND_FAILED: return "BGPU_ERR_BACKEND_FAILED";
        case BGPU_ERR_NOT_READY: return "BGPU_ERR_NOT_READY";
        case BGPU_ERR_LIMIT: return "BGPU_ERR_LIMIT";
        default: return "BGPU_ERR_UNKNOWN";
    }
}

void bgpu_set_reason(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t_reason, sizeof(t_reason), fmt, ap);
    va_end(ap);
}

const char* bgpu_take_reason(void) {
    const char* r = t_reason;
    t_reason[0] = '\0';
    return r;
}

/* 写一条完整错误：<函数>: <原因> (file:line)。
 * reason 为空时退回结果名 —— 保证 last_error 永远非空。 */
static void put_error(BgpuResult result, const char* fn, const char* file, int line,
                      const char* reason) {
    t_seq++;
    if (!reason || !reason[0]) reason = bingpu_result_name(result);
    snprintf(t_err, sizeof(t_err), "%s: %s (%s:%d)", fn ? fn : "bingpu", reason,
             base_name(file), line);
}

BgpuResult bgpu_fail(BgpuResult result, const char* fn, const char* file, int line,
                     const char* fmt, ...) {
    char buf[384];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    put_error(result, fn, file, line, buf);
    return result;
}

/* 后端返回非 OK、但没留原因时的收口（也负责把后端原话拼进来） */
BgpuResult bgpu_fail_from(BgpuResult result, const char* fn, const char* file, int line) {
    const char* reason = bgpu_take_reason();
    put_error(result, fn, file, line, reason);
    return result;
}

const char* bingpu_last_error(void) { return t_err; }

void bingpu_clear_error(void) {
    t_err[0] = '\0';
    t_reason[0] = '\0';
}

/* ============================================================
 * 版本 / 格式
 * ============================================================ */
uint32_t bingpu_abi_version(void) { return BINGPU_ABI_VERSION; }

const char* bingpu_version_string(void) { return BINGPU_VERSION_STRING; }

uint32_t bingpu_format_bytes(BgpuFormat format) {
    switch (format) {
        case BGPU_FORMAT_RGBA8: return 4;
        case BGPU_FORMAT_BGRA8: return 4;
        case BGPU_FORMAT_R8: return 1;
        default: return 0;
    }
}

/* ============================================================
 * 后端注册表
 * ============================================================ */
typedef struct BgpuBackendEntry {
    BgpuBackendKind kind;
    const char* name;
    const BgpuVt* vt;        /* NULL = 登记了但这一版没实现 */
    const char* reason;      /* 不可用时给调用方的解释 */
} BgpuBackendEntry;

/* AUTO 的挑选顺序也由这张表隐含表达：真实后端 → CPU → NULL。
 * 顺序即优先级，AUTO 取第一个 "vt != NULL && available()" 的项。 */
static const BgpuBackendEntry g_backends[] = {
    {BGPU_BACKEND_VULKAN, "vulkan", NULL,
     "未实现：Vulkan 后端是 0.2.0 的第一实现，路线见 docs/backends.md"},
    {BGPU_BACKEND_D3D12, "d3d12", NULL,
     "未实现：D3D12 是第二个真实后端，依赖 Vulkan 版把状态机钉死之后开工"},
    {BGPU_BACKEND_GL, "gl", NULL,
     "不迁：GL 走 binUI 现有的后端（docs/backends.md 写了不迁的理由）"},
    {BGPU_BACKEND_CPU, "cpu", &bgpu_cpu_vt, NULL},
    {BGPU_BACKEND_NULL, "null", &bgpu_null_vt, NULL},
};

#define BGPU_BACKEND_COUNT ((uint32_t)(sizeof(g_backends) / sizeof(g_backends[0])))

uint32_t bingpu_backend_count(void) { return BGPU_BACKEND_COUNT; }

BgpuBackendKind bingpu_backend_at(uint32_t index) {
    if (index >= BGPU_BACKEND_COUNT) {
        bgpu_fail(BGPU_ERR_INVALID_ARGUMENT, __func__, __FILE__, __LINE__,
                  "index=%u 越界（共 %u 个后端）", index, BGPU_BACKEND_COUNT);
        return BGPU_BACKEND_AUTO;
    }
    return g_backends[index].kind;
}

static const BgpuBackendEntry* find_entry(BgpuBackendKind kind) {
    for (uint32_t i = 0; i < BGPU_BACKEND_COUNT; i++)
        if (g_backends[i].kind == kind) return &g_backends[i];
    return NULL;
}

const char* bingpu_backend_name(BgpuBackendKind kind) {
    const BgpuBackendEntry* e = find_entry(kind);
    if (e) return e->name;
    switch (kind) {
        case BGPU_BACKEND_AUTO: return "auto";
        default: return "unknown";
    }
}

int bingpu_backend_available(BgpuBackendKind kind) {
    const BgpuBackendEntry* e = find_entry(kind);
    if (!e || !e->vt || !e->vt->available) return 0;
    return e->vt->available() ? 1 : 0;
}

const BgpuVt* bgpu_vt(BgpuBackendKind kind) {
    const BgpuBackendEntry* e = find_entry(kind);
    return e ? e->vt : NULL;
}

const BgpuVt* bgpu_vt_auto(void) {
    for (uint32_t i = 0; i < BGPU_BACKEND_COUNT; i++) {
        const BgpuBackendEntry* e = &g_backends[i];
        if (e->vt && e->vt->available && e->vt->available()) return e->vt;
    }
    return NULL;
}

/* api.c 用：把"这个后端为什么没用"变成一条可读原因 */
const char* bgpu_backend_unavailable_reason(BgpuBackendKind kind) {
    const BgpuBackendEntry* e = find_entry(kind);
    if (!e) return "未知后端";
    return e->reason ? e->reason : "后端不可用";
}

/* ============================================================
 * 进程退出清理
 * ============================================================ */
void bingpu_shutdown(void) {
    /* 语义：把**所有**还活着的对象连像素一起放掉，然后清空表。
     * 这样"设备忘了销毁"不会在进程退出时留下泄漏（valgrind/ASan 干净）。
     * 可重复调用：第二次开始就是空表，什么也不做。 */
    bgpu_obj_reset();
    bingpu_clear_error();
}
