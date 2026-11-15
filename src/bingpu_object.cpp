/* bingpu_object.cpp —— 句柄 → 对象 的那张表
 *
 * 与 0.1.0 的 C 版**行为逐位相同**（句柄编码、代数失效、按设备连带销毁），
 * 但所有权换成 `std::unique_ptr<ObjectBase>`、容器换成 `std::vector`：
 *   · 资源释放写在对象析构里（RAII），"显式 destroy"和"设备连带清理"复用同一份代码 ——
 *     C 版两条路线必须手工保持一致，这里不可能不一致；
 *   · dtor 不再需要"函数指针 + void*"的注册（C 版的表里存了一份 dtor），
 *     直接 delete 就是正确释放；
 *   · 悬空引用多了一层保护：销毁前通知所有活对象把指向它的裸指针清空
 *     （见 ObjectBase::onPeerDestroyed）。C 版里"帧中途 destroy 管线再 draw"
 *     是 use-after-free（UB），现在退化成 STATE 错误。
 *
 * 为什么不做锁：
 *   GPU 层的调用约定是"渲染线程单线程"（docs/contracts.md §6）。多线程**读错误**那条路
 *   已经用 thread_local 隔离；在这里加锁等于每次 draw 都付一次原子操作，而对象表本身
 *   只在帧外建/删。
 */
#include "bingpu_backend.h"

#include <vector>

namespace bgpu {
namespace {

struct Slot {
    std::unique_ptr<ObjectBase> obj;
    uint32_t gen = 0;   /* 0 = 从未用过；每次销毁 +1 */
    uint32_t kind = 0;
};

std::vector<Slot> g_slots;
uint32_t g_live = 0;

/* 进程退出时 g_slots 自己的析构会把还活着的对象统统 delete 掉（unique_ptr），
 * 而某些对象的析构要回手掏表（CpuSwapchain 会把借出的帧目标一起摘掉）——
 * 此时表正在被销毁，再进 destroyObject 就是 use-after-free。
 * 这个哨兵在 g_slots **之后**构造（同 TU 声明顺序），因此**先**销毁：
 * 它把 g_tearingDown 置位，destroyObject 直接返回，剩下的交给 unique_ptr。
 * 为什么要管这条路径：工具/测试在报错后 exit() 时，用户往往没有逐个 destroy。 */
bool g_tearingDown = false;

struct TableGuard {
    ~TableGuard() { g_tearingDown = true; }
};
TableGuard g_tableGuard;

constexpr uint64_t kKindShift = 60;
constexpr uint64_t kGenShift = 32;
constexpr uint64_t kIndexMask = 0xFFFFFFFFull;

uint64_t makeHandle(uint32_t index, uint32_t gen, uint32_t kind) noexcept {
    const uint64_t gen28 = static_cast<uint64_t>(gen & 0x0FFFFFFFu);
    return ((static_cast<uint64_t>(kind) & 0xFu) << kKindShift) | (gen28 << kGenShift) |
           (static_cast<uint64_t>(index) + 1u);
}

/* 句柄 → 槽；种类不符 / 已销毁 / 越界返回 nullptr */
Slot* slotOf(uint64_t handle, uint32_t kind) noexcept {
    if (handle == BGPU_INVALID) return nullptr;
    const uint32_t hkind = static_cast<uint32_t>((handle >> kKindShift) & 0xFu);
    if (hkind == 0) return nullptr;
    if (kind != 0 && hkind != kind) return nullptr;
    const uint64_t index1 = handle & kIndexMask;
    if (index1 == 0) return nullptr;
    const uint32_t idx = static_cast<uint32_t>(index1 - 1u);
    if (idx >= g_slots.size()) return nullptr;
    Slot& s = g_slots[idx];
    if (!s.obj || s.kind != hkind) return nullptr;
    const uint32_t gen = static_cast<uint32_t>((handle >> kGenShift) & 0x0FFFFFFFu);
    if (gen != s.gen) return nullptr;
    return &s;
}

void bumpGen(uint32_t& gen) noexcept {
    gen++;
    if (gen == 0 || (gen & 0xF0000000u) != 0) gen = 1;   /* 28 位回绕 */
}

}  // namespace

uint64_t registerObject(std::unique_ptr<ObjectBase> obj) {
    if (!obj) return BGPU_INVALID;
    uint32_t idx = static_cast<uint32_t>(g_slots.size());
    for (uint32_t i = 0; i < g_slots.size(); i++) {
        if (!g_slots[i].obj) {
            idx = i;
            break;
        }
    }
    if (idx == g_slots.size()) g_slots.emplace_back();   /* 可能抛 bad_alloc → 入口兜 */
    Slot& s = g_slots[idx];
    s.obj = std::move(obj);
    s.kind = s.obj->kind;
    bumpGen(s.gen);
    g_live++;
    return makeHandle(idx, s.gen, s.kind);
}

ObjectBase* lookupObject(uint64_t handle, uint32_t kind) noexcept {
    Slot* s = slotOf(handle, kind);
    return s ? s->obj.get() : nullptr;
}

void destroyObject(uint64_t handle) {
    if (g_tearingDown) return;   /* 见 g_tearingDown：退出期由 unique_ptr 接管 */
    Slot* s = slotOf(handle, 0);
    if (!s) return;   /* 幂等：无效 / 重复销毁都是 no-op */
    ObjectBase* dying = s->obj.get();
    /* 先通知还活着的对象把指向它的裸指针清掉（cmd 的状态里就有），再真正析构。 */
    for (Slot& other : g_slots) {
        if (other.obj && other.obj.get() != dying) other.obj->onPeerDestroyed(*dying);
    }
    std::unique_ptr<ObjectBase> keep = std::move(s->obj);
    s->kind = 0;
    bumpGen(s->gen);
    g_live--;
    /* keep 在函数末尾析构：资源在这里真正释放（RAII），设备此时还活着。 */
}

uint32_t destroyOwnedObjects(BgpuDevice device) {
    if (device == BGPU_INVALID) return 0;
    std::vector<uint64_t> victims;
    for (uint32_t i = 0; i < g_slots.size(); i++) {
        const Slot& s = g_slots[i];
        if (!s.obj || s.obj->device != device) continue;
        victims.push_back(makeHandle(i, s.gen, s.kind));
    }
    for (uint64_t h : victims) destroyObject(h);
    return static_cast<uint32_t>(victims.size());
}

uint32_t liveObjectCount() noexcept { return g_live; }

void resetObjects() {
    /* 必须分两遍：先资源、后设备。资源析构会回写所属设备的统计（bytes_live），
     * 设备结构体必须还活着 —— 单遍遍历时设备通常在第 0 槽，先被释放掉，
     * 后面的资源析构就是 use-after-free。这条路径只在"调用方忘了销毁设备"时走到，
     * 正是最不该崩的那条（0.1.0 的 C 版在这里踩过顺序坑）。 */
    std::vector<std::unique_ptr<ObjectBase>> doomed;
    doomed.reserve(g_slots.size());
    for (Slot& s : g_slots) {
        if (s.obj && s.obj->kind != kObjDevice) doomed.push_back(std::move(s.obj));
    }
    for (Slot& s : g_slots) {
        if (s.obj) doomed.push_back(std::move(s.obj));
    }
    g_slots.clear();
    g_live = 0;
    for (auto& p : doomed) p.reset();   /* 顺序 reset：资源先死、设备后死 */
}

}  // namespace bgpu
