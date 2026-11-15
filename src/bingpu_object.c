/* bingpu_object.c —— 句柄 → 对象 的那张表
 *
 * 为什么是"表"而不是指针：
 *   ABI 里句柄是 uint64_t（跨语言安全）。表把句柄翻译成进程内对象，同时天然获得
 *   两个性质：① destroy 之后旧句柄立刻无效（代数 +1，槽即便复用也不会让旧句柄
 *   "复活"）；② 设备销毁时可以按 owner 扫一遍，把忘了销毁的子资源收掉（C 没有
 *   析构，这是最后一道保险 —— 但正确用法仍然是先子后父，见 docs/abi.md）。
 *
 * 为什么不做锁：
 *   GPU 层的调用约定是"渲染线程单线程"（见 docs/contracts.md 第 6 条）。绑定的
 *   Python/Go/Rust 侧如果要跨线程，应该在那一层串行化；在这里加锁等于每次 draw
 *   都付一次原子操作。多线程**读错误**那条路已经用 _Thread_local 隔离掉了
 *   （见 bingpu_error.c），所以这里不需要锁来保证正确性。
 *
 * 为什么槽位用线性扫描而不是空闲链表：
 *   同时活着的对象是"设备 + 几个交换链 + 一批纹理/管线"的量级（几百个），
 *   建/销毁都发生在帧外。空闲链表省下的那点 CPU 不值得多一份要测的状态。
 */
#include "bingpu_internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct BgpuSlot {
    void* obj;
    void (*dtor)(void*);
    uint32_t gen;      /* 0 = 从未用过；每次销毁 +1 */
    uint32_t kind;
} BgpuSlot;

static BgpuSlot* g_slots = NULL;
static uint32_t g_cap = 0;
static uint32_t g_live = 0;

static uint64_t make_handle(uint32_t index, uint32_t gen, uint32_t kind) {
    const uint64_t gen28 = (uint64_t)(gen & 0x0FFFFFFFu);
    return ((uint64_t)(kind & 0xFu) << BGPU_HANDLE_KIND_SHIFT) |
           (gen28 << BGPU_HANDLE_GEN_SHIFT) | (uint64_t)(index + 1u);
}

/* 找一个能用的槽：优先复用空槽，没有就扩容（容量翻倍）。
 * 返回槽下标；扩容失败返回 UINT32_MAX。 */
static uint32_t slot_alloc(void) {
    for (uint32_t i = 0; i < g_cap; i++)
        if (g_slots[i].obj == NULL) return i;
    const uint32_t ncap = g_cap ? g_cap * 2u : 16u;
    BgpuSlot* n = (BgpuSlot*)realloc(g_slots, (size_t)ncap * sizeof(BgpuSlot));
    if (!n) return UINT32_MAX;
    memset(n + g_cap, 0, (size_t)(ncap - g_cap) * sizeof(BgpuSlot));
    g_slots = n;
    g_cap = ncap;
    return g_cap / 2u;
}

uint64_t bgpu_obj_new(uint32_t kind, BgpuBackendKind backend, BgpuDevice owner,
                      size_t bytes, void (*dtor)(void*)) {
    if (bytes < sizeof(BgpuObjectBase)) bytes = sizeof(BgpuObjectBase);
    void* obj = calloc(1, bytes);
    if (!obj) return BGPU_INVALID;
    const uint32_t idx = slot_alloc();
    if (idx == UINT32_MAX) {
        free(obj);
        return BGPU_INVALID;
    }
    BgpuObjectBase* base = (BgpuObjectBase*)obj;
    base->kind = kind;
    base->backend = (uint32_t)backend;
    base->device = owner;

    BgpuSlot* s = &g_slots[idx];
    s->obj = obj;
    s->dtor = dtor;
    s->kind = kind;
    s->gen++;
    if (s->gen == 0 || (s->gen & 0xF0000000u) != 0) s->gen = 1;   /* 28 位回绕 */
    g_live++;
    return make_handle(idx, s->gen, kind);
}

void* bgpu_obj_get(uint64_t handle, uint32_t kind) {
    if (handle == BGPU_INVALID) return NULL;
    const uint32_t hkind = (uint32_t)((handle >> BGPU_HANDLE_KIND_SHIFT) & 0xFu);
    if (hkind == 0) return NULL;
    if (kind != 0 && hkind != kind) return NULL;
    const uint64_t index1 = handle & BGPU_HANDLE_INDEX_MASK;
    if (index1 == 0) return NULL;
    const uint32_t idx = (uint32_t)(index1 - 1u);
    if (idx >= g_cap) return NULL;
    const BgpuSlot* s = &g_slots[idx];
    if (!s->obj || s->kind != hkind) return NULL;
    const uint32_t gen = (uint32_t)((handle >> BGPU_HANDLE_GEN_SHIFT) & 0x0FFFFFFFu);
    if (gen != s->gen) return NULL;
    return s->obj;
}

void bgpu_obj_destroy(uint64_t handle) {
    if (handle == BGPU_INVALID) return;
    const uint32_t hkind = (uint32_t)((handle >> BGPU_HANDLE_KIND_SHIFT) & 0xFu);
    const uint64_t index1 = handle & BGPU_HANDLE_INDEX_MASK;
    if (hkind == 0 || index1 == 0) return;
    const uint32_t idx = (uint32_t)(index1 - 1u);
    if (idx >= g_cap) return;
    BgpuSlot* s = &g_slots[idx];
    if (!s->obj || s->kind != hkind) return;
    const uint32_t gen = (uint32_t)((handle >> BGPU_HANDLE_GEN_SHIFT) & 0x0FFFFFFFu);
    if (gen != s->gen) return;
    void (*dtor)(void*) = s->dtor;
    void* obj = s->obj;
    s->obj = NULL;
    s->dtor = NULL;
    s->kind = 0;
    s->gen++;
    if (s->gen == 0 || (s->gen & 0xF0000000u) != 0) s->gen = 1;
    g_live--;
    if (dtor) dtor(obj);   /* dtor 负责 free(obj)，见各后端的 xxx_dtor */
}

uint32_t bgpu_obj_destroy_owned(BgpuDevice device) {
    if (device == BGPU_INVALID) return 0;
    uint32_t n = 0;
    /* 反复从头扫：dtor 理论上不会再建/删对象，但即使删了也不会漏（下一轮会看到）。 */
    for (;;) {
        uint64_t found = BGPU_INVALID;
        for (uint32_t i = 0; i < g_cap; i++) {
            const BgpuSlot* s = &g_slots[i];
            if (!s->obj) continue;
            const BgpuObjectBase* base = (const BgpuObjectBase*)s->obj;
            if (base->device != device) continue;
            found = make_handle(i, s->gen, s->kind);
            break;
        }
        if (found == BGPU_INVALID) break;
        bgpu_obj_destroy(found);
        n++;
    }
    return n;
}

uint32_t bgpu_obj_count(void) { return g_live; }

void bgpu_obj_reset(void) {
    /* 语义：把所有还活着的对象连内存一起放掉（bingpu_shutdown 用）。
     *
     * **必须分两遍**：先资源、后设备。资源析构会回写所属设备的记账
     * （`bytes_live -= bytes`），设备结构体必须还活着 —— 单遍遍历时设备
     * 通常在第 0 槽，先被 free 掉，后面的资源析构就是 use-after-free。
     * 这条路径在"调用方忘了销毁设备"时才会走到，正是最不该崩的那条。
     */
    const uint32_t cap = g_cap;
    BgpuSlot* slots = g_slots;
    g_slots = NULL;
    g_cap = 0;
    g_live = 0;
    if (!slots) return;
    for (int dev_pass = 0; dev_pass < 2; dev_pass++) {
        for (uint32_t i = 0; i < cap; i++) {
            if (!slots[i].obj) continue;
            const int is_dev = slots[i].kind == BGPU_OBJ_DEVICE;
            if ((dev_pass == 0) == is_dev) continue;   /* 第一遍只放非设备 */
            if (slots[i].dtor) slots[i].dtor(slots[i].obj);
            slots[i].obj = NULL;
            slots[i].dtor = NULL;
        }
    }
    free(slots);
}
