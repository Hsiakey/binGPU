/* bingpu_internal.h —— 内部基础设施（句柄表 / 对象基类 / 错误槽 / 后端注册表）
 *
 * 为什么它不在 include/ 里：
 *   公共头 bingpu.h 是**冻结的 C ABI**（只有定长 POD + uint64_t 句柄 + 扁平函数，
 *   必须能被纯 C 编译器单独编过）。本文件是 C++ 实现面的东西 —— 容器、unique_ptr、
 *   std::string —— 一旦漏进 include/，`make abi-check` 那一步就会把它当 C++ 设施拦下来。
 *
 * 语言/ABI 的分工（2026-11-15 定）：
 *   ABI 看的是"谁 include 它"（Python/Go/Rust 绑的是 C 面）；实现看的是表达力
 *   （句柄表、容器、生命周期、错误串拼接，在 C 里全是手写样板）。两者互不牵制：
 *   C++ 写、导出 C 符号是 Vulkan loader / HarfBuzz / bgfx / Skia 的常规做法。
 *
 * 句柄布局（64 位，全 0 = 无效）—— 与 0.1.0 的 C 实现逐位相同，行为不变：
 *   [63:60] 种类（1..7）  [59:32] 代数（28 位）  [31:0] 槽位 + 1
 *   销毁时代数 +1：旧句柄自然失效，复用的槽也不会让旧句柄"复活"。
 */
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "bingpu.h"

namespace bgpu {

class Backend;

/* ---------------------------------------------------------------
 * 对象基类
 *
 * 每个对象都记住自己的后端（`be`）与所属设备（`device`）：
 *   · 上层入口按句柄拿到对象之后，直接用 `obj->be->xxx(...)` 转发；
 *   · 设备销毁时按 `device` 字段扫表，连带清理（最后一道保险）。
 * 析构函数是虚的：所有资源释放（像素/缓冲/位图）都写在子类析构里（RAII），
 * 所以"显式 destroy"和"设备连带清理"走的是同一份代码 —— 这是 C++ 化最直接的收益：
 * 0.1.0 的 C 版两条销毁路线必须手工保持一致，现在不可能不一致。
 * --------------------------------------------------------------- */
enum ObjKind : uint32_t {
    kObjDevice = 1,
    kObjSwapchain = 2,
    kObjTexture = 3,
    kObjBuffer = 4,
    kObjSampler = 5,
    kObjPipeline = 6,
    kObjCmd = 7
};

/* 借用对象（交换链借出的帧目标）不许销毁 */
inline constexpr uint32_t kObjFlagBorrowed = 1u;

struct ObjectBase {
    uint32_t kind;
    uint32_t backend;
    uint32_t flags = 0;
    BgpuDevice device = BGPU_INVALID;
    Backend* be = nullptr;

    /* 定义内联在 bingpu_backend.h（那里 Backend 是完整类型，才能取 b.kind()） */
    ObjectBase(uint32_t k, Backend& b, BgpuDevice owner);
    virtual ~ObjectBase() = default;

    /* 某个对象即将被销毁：别的对象里若存着指向它的裸指针，在这里清掉。
     * 为什么需要：cmd 的状态里存着 target/depth/pipeline/texture/sampler 的指针
     * （避免每次 draw 都查表）。如果调用方在帧中途 destroy 了管线，下一笔 draw 就会
     * 撞上悬空指针 —— 0.1.0 的 C 版有这个洞（UB），C++ 版顺手补上：
     * 销毁前通知所有活对象把引用清空，之后的使用变成"没有管线"的 STATE 错误。 */
    virtual void onPeerDestroyed(ObjectBase&) {}
};

/* ---------------------------------------------------------------
 * 对象表
 * --------------------------------------------------------------- */
uint64_t registerObject(std::unique_ptr<ObjectBase> obj);
ObjectBase* lookupObject(uint64_t handle, uint32_t kind) noexcept;
void destroyObject(uint64_t handle);
uint32_t destroyOwnedObjects(BgpuDevice device);
uint32_t liveObjectCount() noexcept;
void resetObjects();

template <class T, class... A>
uint64_t makeObject(A&&... args) {
    return registerObject(std::make_unique<T>(std::forward<A>(args)...));
}

/* ---------------------------------------------------------------
 * 错误槽
 *
 * 语义与 0.1.0 完全一致（`docs/abi.md` 是对外承诺）：
 *   · 每次失败都写；成功不清空（GetLastError 语义）；
 *   · 格式 `<函数>: <原因> (file:line)`；后端原话拼进 <原因>；
 *   · _Thread_local —— 别的线程的失败不会串台。
 * 实现从"定长 char 缓冲 + snprintf"换成 thread_local std::string：
 * 不再有 512 字节上限，也不用担心截断把函数名吃掉。
 *
 * 错误串里那个 `<函数>` 是**公共函数名**：每个 extern "C" 入口用 ApiScope 设置，
 * 后端内部失败时用 fail(...) 写，于是 file:line 指向真正的失败位置（后端 .cpp），
 * 而函数名仍然是调用方看见的那个 —— 绑定作者两边都能对上。
 * --------------------------------------------------------------- */
class ApiScope {
public:
    explicit ApiScope(const char* fn) noexcept;
    ~ApiScope() noexcept;
    ApiScope(const ApiScope&) = delete;
    ApiScope& operator=(const ApiScope&) = delete;

private:
    const char* prev_;
};

unsigned errorSeq() noexcept;
const char* currentFn() noexcept;
void setReason(std::string text);
std::string takeReason();

BgpuResult fail(BgpuResult rc, const char* file, int line, std::string msg);
BgpuResult failFrom(BgpuResult rc, const char* file, int line);
BgpuResult failOom(const char* file, int line);
BgpuResult failExc(const char* file, int line, const char* what);
BgpuResult failUnknown(const char* file, int line);

/* 后端调用收口：后端返回非 OK 但没写错误串时，用结果名兜底 —— 保证 last_error 非空 */
BgpuResult settle(BgpuResult rc, const char* file, int line, unsigned seqBefore);

/* 小工具：拼接任意可打印片段（错误串用；性能不是这一层的关注点） */
inline void appendTo(std::string& s, const char* v) { s += v ? v : "(null)"; }
inline void appendTo(std::string& s, const std::string& v) { s += v; }
inline void appendTo(std::string& s, std::string_view v) { s += v; }
inline void appendTo(std::string& s, char v) { s += v; }
template <class T, std::enable_if_t<std::is_arithmetic_v<T> && !std::is_same_v<T, char>, int> = 0>
inline void appendTo(std::string& s, T v) {
    s += std::to_string(v);
}

template <class... A>
std::string cat(const A&... args) {
    std::string s;
    (appendTo(s, args), ...);
    return s;
}

/* 两个后端共用的能力上限（避免"这里能跑那里不能"的糊味） */
inline constexpr uint32_t BGPU_MAX_TEXTURE_SIZE = 8192u;
inline constexpr uint32_t BGPU_MAX_QUADS_PER_DRAW = 1u << 20;

/* ---------------------------------------------------------------
 * 后端注册表（实现在 bingpu_error.cpp）
 * --------------------------------------------------------------- */
Backend* backendOf(BgpuBackendKind kind) noexcept;
Backend* backendAuto() noexcept;
const char* backendUnavailableReason(BgpuBackendKind kind) noexcept;
uint32_t backendCount() noexcept;
BgpuBackendKind backendAt(uint32_t index) noexcept;
const char* backendName(BgpuBackendKind kind) noexcept;
int backendAvailable(BgpuBackendKind kind) noexcept;

}  // namespace bgpu
