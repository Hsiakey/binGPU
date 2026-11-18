/* bingpu_error.cpp —— 错误槽 / 名字表 / 版本 / 格式 / 后端注册表 / 进程退出清理
 *
 * 为什么错误槽是 thread_local std::string：
 *   ABI 里没有"错误对象"可以传，只能靠线程级的最后错误；进程级会被别的线程覆盖
 *   （渲染线程 + 资源加载线程是现实组合）。0.1.0 用 _Thread_local char[512] +
 *   snprintf —— 512 字节够用但没道理，而且截断可能把函数名吃掉。C++ 版直接
 *   thread_local std::string：不截断、拼接自然，代价是一次 TLS 寻址 + 失败时的分配
 *   （失败路径本来就不该是热路径）。指针有效期仍是"本线程下一次 bingpu_* 调用为止"。
 *
 * 为什么后端注册表也在这里：
 *   注册表要回答的三件事（有哪些后端、能不能用、叫什么）和错误串同属"元信息"；
 *   AUTO 的挑选结果要能写进错误串。放一起少一个编译单元，也少一处"两边各有一份
 *   顺序表"的隐患。
 *
 * 为什么 VK/D3D12/GL 登记了却不实现：
 *   bingpu_backend_count/at 是"这台机器上 binGPU 认识哪些后端"的稳定枚举。上层按
 *   索引探测能力时，看到 vulkan 存在但 available()==0 + 明确原因，比"枚举里根本没
 *   这一项"更好写，也避免以后加了 VK 之后枚举顺序变化。
 */
#include "bingpu_backend.h"

#include <cstdio>

namespace bgpu {
namespace {

/* 每个线程一份：最后错误、后端原因、写入序号、当前公共函数名 */
thread_local std::string t_error;
thread_local std::string t_reason;
thread_local unsigned t_seq = 0;
thread_local const char* t_currentFn = "bingpu";

const char* baseName(const char* path) noexcept {
    const char* base = path;
    for (const char* p = path; p && *p; ++p) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    return base;
}

/* 写一条完整错误：<函数>: <原因> (file:line)。原因空时退回结果名 —— 保证非空。 */
void putError(BgpuResult rc, const char* file, int line, std::string reason) {
    t_seq++;
    if (reason.empty()) reason = bingpu_result_name(rc);
    t_error = cat(currentFn(), ": ", reason, " (", baseName(file), ":", line, ")");
}

}  // namespace

ApiScope::ApiScope(const char* fn) noexcept : prev_(t_currentFn) {
    t_currentFn = fn ? fn : "bingpu";
}

ApiScope::~ApiScope() noexcept { t_currentFn = prev_; }

unsigned errorSeq() noexcept { return t_seq; }

const char* currentFn() noexcept { return t_currentFn; }

void setReason(std::string text) { t_reason = std::move(text); }

std::string takeReason() {
    std::string r = std::move(t_reason);
    t_reason.clear();
    return r;
}

BgpuResult fail(BgpuResult rc, const char* file, int line, std::string msg) {
    putError(rc, file, line, std::move(msg));
    return rc;
}

BgpuResult failFrom(BgpuResult rc, const char* file, int line) {
    putError(rc, file, line, takeReason());
    return rc;
}

BgpuResult failOom(const char* file, int line) {
    return fail(BGPU_ERR_OUT_OF_MEMORY, file, line,
                "内存不足（std::bad_alloc：容器/对象分配失败）");
}

BgpuResult failExc(const char* file, int line, const char* what) {
    return fail(BGPU_ERR_BACKEND_FAILED, file, line,
                cat("实现内部抛出异常：", what ? what : "(unknown)"));
}

BgpuResult failUnknown(const char* file, int line) {
    return fail(BGPU_ERR_BACKEND_FAILED, file, line, "实现内部抛出未知异常（非 std::exception）");
}

BgpuResult settle(BgpuResult rc, const char* file, int line, unsigned seqBefore) {
    if (rc == BGPU_OK) return rc;
    if (t_seq == seqBefore) putError(rc, file, line, takeReason());
    return rc;
}

}  // namespace bgpu

/* ============================================================
 * 对外：错误 / 版本 / 格式
 * ============================================================ */
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

const char* bingpu_last_error(void) {
    /* 与 0.1.0 一致：不抛、不过期（直到本线程下一次写错误），指针别存。 */
    return bgpu::t_error.c_str();
}

void bingpu_clear_error(void) {
    bgpu::t_error.clear();
    bgpu::t_reason.clear();
}

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
namespace bgpu {
namespace {

struct Entry {
    BgpuBackendKind kind;
    const char* name;
    Backend* be;        /* nullptr = 登记了但这一版没实现 */
    const char* reason; /* 不可用时给调用方的解释 */
};

/* 顺序即 AUTO 的优先级：真实后端 → CPU → NULL（见 docs/backends.md）
 * 同族里 VK 先于 D3D12，异族 D3D11 紧随 VK 之后（`layering.md` §5 的落地顺序）。 */
const Entry kBackends[] = {
    {BGPU_BACKEND_VULKAN, "vulkan", &vkBackend(), nullptr},
    {BGPU_BACKEND_D3D11, "d3d11", &d3d11Backend(), nullptr},
    {BGPU_BACKEND_D3D12, "d3d12", nullptr,
     "未实现：D3D12 与 VK 同族、验证价值最低，排期见 docs/layering.md §5"},
    {BGPU_BACKEND_GL, "gl", nullptr,
     "未实现：GL 是异族（即时状态）的兜底后端，排在 D3D11 之后，见 docs/layering.md §4/§5"},
    {BGPU_BACKEND_CPU, "cpu", &cpuBackend(), nullptr},
    {BGPU_BACKEND_NULL, "null", &nullBackend(), nullptr},
};

constexpr uint32_t kBackendCount = static_cast<uint32_t>(sizeof(kBackends) / sizeof(kBackends[0]));

const Entry* findEntry(BgpuBackendKind kind) noexcept {
    for (uint32_t i = 0; i < kBackendCount; i++) {
        if (kBackends[i].kind == kind) return &kBackends[i];
    }
    return nullptr;
}

}  // namespace

Backend* backendOf(BgpuBackendKind kind) noexcept {
    const Entry* e = findEntry(kind);
    return e ? e->be : nullptr;
}

Backend* backendAuto() noexcept {
    for (uint32_t i = 0; i < kBackendCount; i++) {
        if (kBackends[i].be && kBackends[i].be->available()) return kBackends[i].be;
    }
    return nullptr;
}

const char* backendUnavailableReason(BgpuBackendKind kind) noexcept {
    const Entry* e = findEntry(kind);
    if (!e) return "未知后端";
    return e->reason ? e->reason : "后端不可用";
}

uint32_t backendCount() noexcept { return kBackendCount; }

BgpuBackendKind backendAt(uint32_t index) noexcept {
    if (index >= kBackendCount) return BGPU_BACKEND_AUTO;
    return kBackends[index].kind;
}

const char* backendName(BgpuBackendKind kind) noexcept {
    const Entry* e = findEntry(kind);
    if (e) return e->name;
    if (kind == BGPU_BACKEND_AUTO) return "auto";
    return "unknown";
}

int backendAvailable(BgpuBackendKind kind) noexcept {
    const Entry* e = findEntry(kind);
    if (!e || !e->be || !e->be->available()) return 0;
    return 1;
}

}  // namespace bgpu

uint32_t bingpu_backend_count(void) {
    return bgpu::backendCount();
}

BgpuBackendKind bingpu_backend_at(uint32_t index) {
    bgpu::ApiScope scope("bingpu_backend_at");
    if (index >= bgpu::backendCount()) {
        bgpu::fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__,
                   bgpu::cat("index=", index, " 越界（共 ", bgpu::backendCount(), " 个后端）"));
        return BGPU_BACKEND_AUTO;
    }
    return bgpu::backendAt(index);
}

const char* bingpu_backend_name(BgpuBackendKind kind) {
    return bgpu::backendName(kind);
}

int bingpu_backend_available(BgpuBackendKind kind) {
    return bgpu::backendAvailable(kind);
}

/* ============================================================
 * 进程退出清理
 * ============================================================ */
void bingpu_shutdown(void) {
    /* 语义：把**所有**还活着的对象连像素一起放掉，然后清空表。
     * 可重复调用：第二次开始就是空表，什么也不做。
     * resetObjects 内部按"先资源后设备"两遍走 —— 资源析构要回写设备的统计，
     * 设备结构体必须还活着（0.1.0 的 C 版在这里踩过顺序坑，见 object.cpp）。 */
    bgpu::resetObjects();
    bingpu_clear_error();
}
