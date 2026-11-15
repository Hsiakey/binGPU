/* bingpu_cpu.cpp —— CPU 软件参考实现（整个项目的像素裁判）
 *
 * 定位没变（0.1.0 定下的）：它是"同一份绘制指令应该得到什么像素"的**唯一权威**。
 * C++ 化之后代码更短，但像素规则一条没动 —— tests/test_cpu_render.c 的期望值全部
 * 仍然成立（334 条）。三条目标按优先级：
 *   1. 确定性：整数（预乘/反预乘）或 IEEE 单精度（插值/混合），加法顺序固定；
 *   2. 规则明确：每条规则在 docs/raster_rules.md，GPU 后端逐条对齐；
 *   3. 简单：一个 for 扫三角形，没有 tile/SIMD/线程 —— 它是出图和 CI 用的。
 *
 * 存储模型（最容易错的地方，先钉死）：
 *   · 纹理与渲染目标**内部都存预乘 alpha**的 RGBA8（与 GPU 的 framebuffer 一致）；
 *   · upload 进来的是**直通**像素 → 立刻预乘；readback 出去一律反预乘（直通）；
 *   · 因此非 255 alpha 的"上传→读回"往返会有 ±1 的量化误差（8 位存不下两次除法），
 *     这是规格的一部分（docs/abi.md），测试挑的是能整除的值。
 *
 * C++ 化带来的变化只有"怎么管内存"：
 *   · 像素/缓冲是 std::vector<uint8_t>（不用手写 malloc/free）；
 *   · 交换链借出的目标纹理用**别名**指向 swapchain 的 back 缓冲（std::vector 重新
 *     分配时地址会变，所以存指针的是 vector*，不是裸 data 指针）；
 *   · 深度缓冲 std::vector<float>，懒分配；
 *   · 以上资源的释放全在析构里（RAII），"显式 destroy"与"设备连带清理"同一份代码。
 */
#include "bingpu_backend.h"

#include <cmath>
#include <cstring>
#include <vector>

namespace bgpu {
namespace {

/* ---------------------------------------------------------------
 * 8 位像素小工具：预乘/反预乘用整数（可复现），插值/混合用 float
 * --------------------------------------------------------------- */
inline uint8_t clamp_u8(int v) { return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v)); }

/* 乘 alpha：round(c*a/255) —— +127 的整数四舍五入，跨平台一致 */
inline uint8_t premul8(uint32_t c, uint32_t a) {
    return static_cast<uint8_t>((c * a + 127u) / 255u);
}

/* 反预乘：round(p*255/a)；a==0 时按 0 处理（透明像素没有颜色信息）。
 * 0.2.1 起用 (预乘值, alpha) → 结果 的 64 KiB 查表：逐项结果与现算**完全一样**
 * （表就是拿同一个整数公式预先算好的），但 1024² 回读省掉 4M 次整数除法。 */
struct UnpremulTable {
    uint8_t v[65536];
    UnpremulTable() {
        for (uint32_t a = 0; a < 256; a++) {
            for (uint32_t p = 0; p < 256; p++) {
                uint8_t out = 0;
                if (a != 0) {
                    const uint32_t x = (p * 255u + a / 2u) / a;
                    out = static_cast<uint8_t>(x > 255u ? 255u : x);
                }
                v[p * 256u + a] = out;
            }
        }
    }
};

inline const UnpremulTable& unpremulTable() {
    static const UnpremulTable table;
    return table;
}

inline uint8_t unpremul8(uint32_t p, uint32_t a) {
    return unpremulTable().v[(p & 0xFFu) * 256u + (a & 0xFFu)];
}

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

inline uint8_t q8(float v) {
    const int i = static_cast<int>(v * 255.0f + 0.5f);
    return static_cast<uint8_t>(i < 0 ? 0 : (i > 255 ? 255 : i));
}

inline uint32_t bytesPerPx(BgpuFormat fmt) { return bingpu_format_bytes(fmt); }

/* 字节 → float 查表：位模式与 `p / 255.0f` 现算完全一致（同一串除法预先算好）。
 * 0.2.0 每个像素要除 4 次，这是最贵的一处；混合循环现在只查表。 */
struct ByteToFloat {
    float v[256];
    ByteToFloat() {
        for (int i = 0; i < 256; i++) v[i] = static_cast<float>(i) / 255.0f;
    }
};

inline const ByteToFloat& byteToFloat() {
    static const ByteToFloat table;
    return table;
}

/* 通道序：RGBA8 内存里是 R,G,B,A；BGRA8 是 B,G,R,A。内部一律按"逻辑 r,g,b,a"读写。 */
inline void px_store(uint8_t* p, BgpuFormat fmt, const float c[4]) {
    const uint8_t r = q8(c[0]), g = q8(c[1]), b = q8(c[2]), a = q8(c[3]);
    if (fmt == BGPU_FORMAT_BGRA8) {
        p[0] = b;
        p[1] = g;
        p[2] = r;
        p[3] = a;
    } else {
        p[0] = r;
        p[1] = g;
        p[2] = b;
        p[3] = a;
    }
}

inline void px_load(const uint8_t* p, BgpuFormat fmt, float c[4]) {
    const ByteToFloat& lut = byteToFloat();
    if (fmt == BGPU_FORMAT_BGRA8) {
        c[0] = lut.v[p[2]];
        c[1] = lut.v[p[1]];
        c[2] = lut.v[p[0]];
        c[3] = lut.v[p[3]];
    } else {
        c[0] = lut.v[p[0]];
        c[1] = lut.v[p[1]];
        c[2] = lut.v[p[2]];
        c[3] = lut.v[p[3]];
    }
}

/* 把直通颜色写进预乘存储（清除路径用） */
inline void store_straight(uint8_t* p, BgpuFormat fmt, uint32_t argb) {
    const uint32_t a = (argb >> 24) & 0xFFu;
    float c[4];
    c[0] = premul8((argb >> 16) & 0xFFu, a) / 255.0f;
    c[1] = premul8((argb >> 8) & 0xFFu, a) / 255.0f;
    c[2] = premul8(argb & 0xFFu, a) / 255.0f;
    c[3] = a / 255.0f;
    px_store(p, fmt, c);
}

/* ---------------------------------------------------------------
 * CPU 特有的对象：像素/深度/位图 + RAII 记账
 *
 * bytes_live 的加减必须跟着对象生死走：0.1.0 的 C 版在 NULL 后端的析构里漏了减法
 * （记账漏洞），C++ 版把两边的减法都写进析构 —— 显式 destroy 与设备连带清理共用。
 * --------------------------------------------------------------- */
struct CpuTexture final : Texture {
    Device* dev = nullptr;
    std::vector<uint8_t> own;        /* 自有像素（borrowed == false 时有效） */
    std::vector<uint8_t>* alias = nullptr;   /* 借来的帧目标：指向 swapchain 的 back */
    std::vector<float> depth;        /* 当深度目标时懒分配（float32） */
    uint32_t depth_w = 0, depth_h = 0;

    CpuTexture(Backend& b, BgpuDevice owner, Device& d) : Texture(b, owner), dev(&d) {}
    ~CpuTexture() override {
        if (dev) dev->bytes_live -= bytes;
    }

    std::vector<uint8_t>& store() { return borrowed ? *alias : own; }
    const std::vector<uint8_t>& store() const { return borrowed ? *alias : own; }
    uint8_t* data() { return store().data(); }
    const uint8_t* data() const { return store().data(); }
    size_t dataBytes() const { return store().size(); }
};

struct CpuBuffer final : Buffer {
    Device* dev = nullptr;
    std::vector<uint8_t> data;
    CpuBuffer(Backend& b, BgpuDevice owner, Device& d) : Buffer(b, owner), dev(&d) {}
    ~CpuBuffer() override {
        if (dev) dev->bytes_live -= bytes;
    }
};

struct CpuSwapchain final : Swapchain {
    std::vector<uint8_t> back;    /* 当前帧的目标 */
    std::vector<uint8_t> front;   /* 上一帧呈现结果（frame_read 读它） */

    CpuSwapchain(Backend& b, BgpuDevice owner, Device& d) : Swapchain(b, owner) { dev = &d; }
    ~CpuSwapchain() override {
        /* 借出的目标纹理由交换链拥有：先把它从对象表里摘掉（句柄随即失效），
         * 免得它留着一个指向已释放 back 的别名。 */
        if (target != BGPU_INVALID) destroyObject(target);
        if (dev) dev->bytes_live -= static_cast<uint64_t>(w) * h * bytesPerPx(fmt) * 2u;
    }
};

/* ---------------------------------------------------------------
 * 采样
 * --------------------------------------------------------------- */
/* 内联 floor：|x| < 2^23 时与 std::floor 逐位相同。
 * 为什么不直接调 std::floor：不带 -fno-math-errno 时 GCC 会生成一次 libm 调用，
 * 而采样器每个像素要 floor 两到四次 —— 1024² 就是四百万次调用。 */
inline float fast_floor(float x) {
    const float t = static_cast<float>(static_cast<int>(x));
    return (t > x) ? t - 1.0f : t;
}

inline uint32_t wrap_index(int64_t i, uint32_t n, bool repeat) {
    if (!repeat) {
        if (i < 0) return 0;
        return static_cast<uint32_t>(i) >= n ? n - 1u : static_cast<uint32_t>(i);
    }
    /* 常见情形（i 在 [0,n) 内）先走比较，别为一次取模付整数除法 */
    if (i >= 0 && static_cast<uint64_t>(i) < n) return static_cast<uint32_t>(i);
    int64_t m = i % static_cast<int64_t>(n);
    if (m < 0) m += static_cast<int64_t>(n);
    return static_cast<uint32_t>(m);
}

/* 采样状态：每像素都要读的东西提到循环外（0.2.0 每像素都重新解引用 sampler/纹理，
 * 还要重新判断 filter/wrap —— 1024² 就是一百万次无用的分支）。 */
struct TexState {
    const uint8_t* base = nullptr;
    uint32_t w = 0, h = 0;
    uint32_t bpp = 4;
    size_t stride = 0;     /* w * bpp：逐行寻址少一次乘法 */
    BgpuFormat fmt = BGPU_FORMAT_RGBA8;
    bool linear = true;
    bool rep_u = false, rep_v = false;
    bool fast4 = false;    /* 4 字节格式：可以按 uint32 取纹素 */
    bool bgra = false;
};

inline TexState make_tex_state(const CpuTexture* t, const Sampler* s) {
    TexState ts;
    ts.base = t->data();
    ts.w = t->w;
    ts.h = t->h;
    ts.bpp = bytesPerPx(t->fmt);
    ts.stride = static_cast<size_t>(t->w) * ts.bpp;
    ts.fmt = t->fmt;
    ts.fast4 = ts.bpp == 4;
    ts.bgra = t->fmt == BGPU_FORMAT_BGRA8;
    ts.linear = s ? (s->desc.mag_filter == BGPU_FILTER_LINEAR) : true;
    ts.rep_u = (s != nullptr) && s->desc.wrap_u == BGPU_WRAP_REPEAT;
    ts.rep_v = (s != nullptr) && s->desc.wrap_v == BGPU_WRAP_REPEAT;
    return ts;
}

inline void texel_at_fast(const TexState& ts, uint32_t x, uint32_t y, float out[4]) {
    const uint8_t* base = ts.base + static_cast<size_t>(y) * ts.stride;
    if (ts.fast4) {
        const uint32_t w = *reinterpret_cast<const uint32_t*>(base + static_cast<size_t>(x) * 4u);
        const ByteToFloat& lut = byteToFloat();
        if (ts.bgra) {
            out[0] = lut.v[(w >> 16) & 0xFFu];
            out[1] = lut.v[(w >> 8) & 0xFFu];
            out[2] = lut.v[w & 0xFFu];
        } else {
            out[0] = lut.v[w & 0xFFu];
            out[1] = lut.v[(w >> 8) & 0xFFu];
            out[2] = lut.v[(w >> 16) & 0xFFu];
        }
        out[3] = lut.v[(w >> 24) & 0xFFu];
        return;
    }
    const uint8_t* p = base + static_cast<size_t>(x) * ts.bpp;
    const float f = byteToFloat().v[p[0]];
    out[0] = out[1] = out[2] = out[3] = f;   /* R8 遮罩：采样成 (v,v,v,v) */
}

/* 采样规则见 docs/raster_rules.md §6：clamp 先夹到 [0,1]；repeat 绕回 [0,1)；
 * nearest 取 floor(u*w)；linear 用 u*w-0.5 的纹素中心约定做双线性。
 * 与 0.2.0 的 cpu_sample 是**同一串浮点运算**，只是状态已经预先放进 TexState。 */
inline void sample_fast(const TexState& ts, float u, float v, float out[4]) {
    if (ts.rep_u) {
        u = u - fast_floor(u);
    } else {
        u = clampf(u, 0.0f, 1.0f);
    }
    if (ts.rep_v) {
        v = v - fast_floor(v);
    } else {
        v = clampf(v, 0.0f, 1.0f);
    }

    const float fw = static_cast<float>(ts.w);
    const float fh = static_cast<float>(ts.h);
    if (!ts.linear) {
        /* clamp/repeat 之后 u,v ∈ [0,1]，u*w ≥ 0，截断就是 floor（少一次 floor 调用） */
        const uint32_t ix = wrap_index(static_cast<int64_t>(u * fw), ts.w, ts.rep_u);
        const uint32_t iy = wrap_index(static_cast<int64_t>(v * fh), ts.h, ts.rep_v);
        texel_at_fast(ts, ix, iy, out);
        return;
    }
    const float fx = u * fw - 0.5f;
    const float fy = v * fh - 0.5f;
    const float x0f = fast_floor(fx);
    const float y0f = fast_floor(fy);
    const float tx = fx - x0f;
    const float ty = fy - y0f;
    const int64_t x0 = static_cast<int64_t>(x0f);
    const int64_t y0 = static_cast<int64_t>(y0f);
    const uint32_t ix0 = wrap_index(x0, ts.w, ts.rep_u);
    const uint32_t ix1 = wrap_index(x0 + 1, ts.w, ts.rep_u);
    const uint32_t iy0 = wrap_index(y0, ts.h, ts.rep_v);
    const uint32_t iy1 = wrap_index(y0 + 1, ts.h, ts.rep_v);
    float c00[4], c10[4], c01[4], c11[4];
    texel_at_fast(ts, ix0, iy0, c00);
    texel_at_fast(ts, ix1, iy0, c10);
    texel_at_fast(ts, ix0, iy1, c01);
    texel_at_fast(ts, ix1, iy1, c11);
    for (int i = 0; i < 4; i++) {
        const float a = c00[i] + (c10[i] - c00[i]) * tx;
        const float b = c01[i] + (c11[i] - c01[i]) * tx;
        out[i] = a + (b - a) * ty;
    }
}

/* 老入口：一个三角形的均匀 UV 只需要采样一次，建状态的开销可以忽略 */
inline void cpu_sample(const CpuTexture* t, float u, float v, const Sampler* s, float out[4]) {
    sample_fast(make_tex_state(t, s), u, v, out);
}

/* ---------------------------------------------------------------
 * 混合（预乘域，逐通道；公式见 docs/raster_rules.md §8）
 * --------------------------------------------------------------- */
void blend_premul(float dst[4], const float src[4], BgpuBlend mode) {
    const float sa = src[3];
    switch (mode) {
        case BGPU_BLEND_REPLACE:
            for (int i = 0; i < 4; i++) dst[i] = src[i];
            return;
        case BGPU_BLEND_KEEP:      /* dst *= srcA */
            for (int i = 0; i < 4; i++) dst[i] *= sa;
            return;
        case BGPU_BLEND_ERASE:     /* dst *= (1-srcA) */
            for (int i = 0; i < 4; i++) dst[i] *= (1.0f - sa);
            return;
        case BGPU_BLEND_ADDITIVE:  /* src + dst */
            for (int i = 0; i < 4; i++) dst[i] = clampf(dst[i] + src[i], 0.0f, 1.0f);
            return;
        case BGPU_BLEND_PREMULTIPLIED:
        default:
            for (int i = 0; i < 4; i++) dst[i] = clampf(src[i] + dst[i] * (1.0f - sa), 0.0f, 1.0f);
            return;
    }
}

/* ---------------------------------------------------------------
 * 光栅（0.2.1 重写了内层循环；**语义与 0.2.0 逐位相同**）
 *
 * 为什么这样改（先量后改，基线见 docs/perf.md）：
 *   0.2.0 的内层循环是"一条路径打天下"：不管画纯色还是纹理，每个像素都要插值
 *   u/v/z、过一遍管线的 blend switch、把目标像素读成 float（每通道一次除 255）、
 *   混合、再乘回 255 取整。1024² 纯色填充只有 ~57 MPix/s。
 *
 *   改法（每条都不改变输出字节）：
 *   ① 按"是否纹理 / 是否有深度"分循环：不纹理不算 u/v，不测深度不插 z。
 *   ② 混合模式变模板参数（switch 提到三角形外）；其中 REPLACE、以及"预乘 + 源
 *      alpha 正好 255"不需要读目标（dst*0 = 0，src+0 = src），直接写 4 字节。
 *   ③ **混合查表**：均匀色三角形的混合结果是"目标字节 → 结果字节"的纯函数，
 *      于是每个三角形（实际是每种 src 颜色）预先算一张 4×256 的置换表，像素里
 *      只剩查表 + 写。表的每一项就是用同一条混合公式现算出来的，所以逐位一致。
 *      KEEP/ERASE/ADDITIVE 这类"目标相关"的混合由此从 ~78 MPix/s 提到数百。
 *   ④ **均匀色三角形**把整条片元计算提到循环外（纹理时还要求同 UV 才直接写常量）。
 *      为什么仍然逐位一致：插值结果 = c·(w0+w1+w2)，权重和与 1 的偏差在 1e-7 量级，
 *      而 8 位量化的余量是 0.5/255 ≈ 2e-3，量化后必然是同一个字节。
 *   ⑤ 纹理采样状态（尺寸/格式/filter/wrap）提到循环外；字节↔float 用 256 项查表，
 *      反预乘用 64 KiB 的 (p,a) 查表 —— 都是把原来的运算**预先算一遍**，位模式相同。
 *
 *   没有做（会改像素或收益不划算，见 docs/perf.md 的"未做"）：边缘函数增量递推、
 *   行内 memset 快路径、多线程。
 * --------------------------------------------------------------- */
struct RVert {
    float x, y, z;
    float u, v;
    float r, g, b, a;   /* 直通色（插值在直通域做，与 GPU 的顶点色一致） */
};

/* 一次 draw 的常量状态 */
struct Paint {
    CpuTexture* tgt = nullptr;
    CpuTexture* depth = nullptr;
    int l = 0, t = 0, r = 0, b = 0;   /* scissor ∩ 目标（右/下开区间） */
    BgpuBlend blend = BGPU_BLEND_PREMULTIPLIED;
    bool textured = false;
    bool depthOn = false;
    bool depthTest = false;
    bool depthWrite = false;
    TexState texState;

    /* 混合查表的缓存：同一批小四边形往往同色同混合，只算一次 */
    bool lutValid = false;
    BgpuBlend lutBlend = BGPU_BLEND_PREMULTIPLIED;
    float lutSrc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    uint8_t lut[4][256] = {};
};

inline float edgef(const RVert* a, const RVert* b, float px, float py) {
    return (b->x - a->x) * (py - a->y) - (b->y - a->y) * (px - a->x);
}

/* top-left 填充规则（y 向下、area>0 归一化之后）：
 *   水平且向右的边算 top；向下的边算 left。作用：相邻三角形共享边只被一个三角形覆盖，
 *   半透明拼接不会出现一条缝（tests/test_cpu_render.c 的 seam_top_left 钉这条）。 */
inline bool is_top_left(const RVert* a, const RVert* b) {
    const float dx = b->x - a->x;
    const float dy = b->y - a->y;
    return (dy == 0.0f && dx > 0.0f) || (dy < 0.0f);
}

/* 单通道混合：查表构建与逐像素两条路径共用这一条公式（所以两条路径不可能算不一样） */
template <BgpuBlend BLEND>
inline float blend_one(float d, float s, float sa) {
    if constexpr (BLEND == BGPU_BLEND_REPLACE) {
        return s;
    } else if constexpr (BLEND == BGPU_BLEND_KEEP) {
        return d * sa;
    } else if constexpr (BLEND == BGPU_BLEND_ERASE) {
        return d * (1.0f - sa);
    } else if constexpr (BLEND == BGPU_BLEND_ADDITIVE) {
        return clampf(d + s, 0.0f, 1.0f);
    } else {
        return clampf(s + d * (1.0f - sa), 0.0f, 1.0f);
    }
}

template <BgpuBlend BLEND>
inline void blend_dst(float dst[4], const float src[4]) {
    const float sa = src[3];
    for (int i = 0; i < 4; i++) dst[i] = blend_one<BLEND>(dst[i], src[i], sa);
}

/* 4 字节打包（内存里就是这个顺序：写一次 uint32 顶 4 次字节写） */
inline uint32_t pack_px(BgpuFormat fmt, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    if (fmt == BGPU_FORMAT_BGRA8) return b | (g << 8) | (r << 16) | (a << 24);
    return r | (g << 8) | (b << 16) | (a << 24);
}

/* 混合查表：tbl[c][i] = q8(blend_one(src[c], 目标字节 i))。逐项与现算同一条公式。 */
template <BgpuBlend BLEND>
void prepare_blend_lut(Paint& p, const float src[4]) {
    if (p.lutValid && p.lutBlend == BLEND && p.lutSrc[0] == src[0] && p.lutSrc[1] == src[1] &&
        p.lutSrc[2] == src[2] && p.lutSrc[3] == src[3]) {
        return;   /* 同一颜色同一混合：复用上一张表 */
    }
    const ByteToFloat& f = byteToFloat();
    for (int c = 0; c < 4; c++) {
        for (int i = 0; i < 256; i++) {
            p.lut[c][i] = q8(blend_one<BLEND>(f.v[i], src[c], src[3]));
        }
        p.lutSrc[c] = src[c];
    }
    p.lutValid = true;
    p.lutBlend = BLEND;
}

/* 均匀色特化循环：边函数 + 覆盖判断 + 一次查表/写。
 * OPAQUE 为真时不读目标（REPLACE / 预乘且 srcA==1），直接写打包值。 */
template <BgpuBlend BLEND, bool OPAQUE>
void loop_uniform(const Paint& p, const RVert* va, const RVert* vb, const RVert* vd,
                  uint32_t packed, int x0, int x1, int y0, int y1) {
    const bool tl0 = is_top_left(vb, vd);
    const bool tl1 = is_top_left(vd, va);
    const bool tl2 = is_top_left(va, vb);
    const bool rgba = (p.tgt->fmt == BGPU_FORMAT_RGBA8);
    const uint8_t* lut0 = p.lut[0];
    const uint8_t* lut1 = p.lut[1];
    const uint8_t* lut2 = p.lut[2];
    const uint8_t* lut3 = p.lut[3];
    uint8_t* base = p.tgt->data() + static_cast<size_t>(y0) * p.tgt->w * 4u;

    for (int y = y0; y < y1; y++) {
        const float py = static_cast<float>(y) + 0.5f;
        uint8_t* row = base + static_cast<size_t>(y - y0) * p.tgt->w * 4u;
        uint32_t* row32 = reinterpret_cast<uint32_t*>(row);
        for (int x = x0; x < x1; x++) {
            const float px = static_cast<float>(x) + 0.5f;
            const float e0 = edgef(vb, vd, px, py);
            const float e1 = edgef(vd, va, px, py);
            const float e2 = edgef(va, vb, px, py);
            if (!((e0 > 0.0f) || (e0 == 0.0f && tl0))) continue;
            if (!((e1 > 0.0f) || (e1 == 0.0f && tl1))) continue;
            if (!((e2 > 0.0f) || (e2 == 0.0f && tl2))) continue;
            if constexpr (OPAQUE) {
                row32[x] = packed;
            } else {
                uint8_t* q = row + static_cast<size_t>(x) * 4u;
                if (rgba) {
                    q[0] = lut0[q[0]];
                    q[1] = lut1[q[1]];
                    q[2] = lut2[q[2]];
                    q[3] = lut3[q[3]];
                } else {
                    q[0] = lut2[q[0]];
                    q[1] = lut1[q[1]];
                    q[2] = lut0[q[2]];
                    q[3] = lut3[q[3]];
                }
            }
        }
    }
}

/* 纹理 + 顶点色恒定（很常见：白顶点色 × 一张图）：采样逐像素，颜色只乘一次 */
template <BgpuBlend BLEND>
void loop_textured_uniform(const Paint& p, const RVert* va, const RVert* vb, const RVert* vd,
                           const float mult[4], float inv, int x0, int x1, int y0, int y1) {
    const bool tl0 = is_top_left(vb, vd);
    const bool tl1 = is_top_left(vd, va);
    const bool tl2 = is_top_left(va, vb);
    const bool rgba = (p.tgt->fmt == BGPU_FORMAT_RGBA8);
    const TexState& ts = p.texState;
    uint8_t* base = p.tgt->data() + static_cast<size_t>(y0) * p.tgt->w * 4u;

    for (int y = y0; y < y1; y++) {
        const float py = static_cast<float>(y) + 0.5f;
        uint8_t* row = base + static_cast<size_t>(y - y0) * p.tgt->w * 4u;
        for (int x = x0; x < x1; x++) {
            const float px = static_cast<float>(x) + 0.5f;
            const float e0 = edgef(vb, vd, px, py);
            const float e1 = edgef(vd, va, px, py);
            const float e2 = edgef(va, vb, px, py);
            if (!((e0 > 0.0f) || (e0 == 0.0f && tl0))) continue;
            if (!((e1 > 0.0f) || (e1 == 0.0f && tl1))) continue;
            if (!((e2 > 0.0f) || (e2 == 0.0f && tl2))) continue;
            const float u = (e0 * va->u + e1 * vb->u + e2 * vd->u) * inv;
            const float v = (e0 * va->v + e1 * vb->v + e2 * vd->v) * inv;
            float tex[4];
            sample_fast(ts, u, v, tex);
            float frag[4];
            frag[0] = tex[0] * mult[0];
            frag[1] = tex[1] * mult[1];
            frag[2] = tex[2] * mult[2];
            frag[3] = tex[3] * mult[3];
            uint8_t* q = row + static_cast<size_t>(x) * 4u;
            float dst[4];
            px_load(q, rgba ? BGPU_FORMAT_RGBA8 : BGPU_FORMAT_BGRA8, dst);
            blend_dst<BLEND>(dst, frag);
            px_store(q, rgba ? BGPU_FORMAT_RGBA8 : BGPU_FORMAT_BGRA8, dst);
        }
    }
}

/* 通用路径：逐像素插值 + 采样 + 深度 + 混合（数学与 0.2.0 相同，只是状态提到 Paint 里） */
void shade_general(const Paint& p, float px, float py, const RVert* v0, const RVert* v1,
                   const RVert* v2, float w0, float w1, float w2) {
    CpuTexture& tgt = *p.tgt;
    const uint32_t xi = static_cast<uint32_t>(px);
    const uint32_t yi = static_cast<uint32_t>(py);

    if (p.depthOn && (p.depthTest || p.depthWrite)) {
        const float z = w0 * v0->z + w1 * v1->z + w2 * v2->z;
        float* d = &p.depth->depth[static_cast<size_t>(yi) * p.depth->depth_w + xi];
        if (p.depthTest && !(z < *d)) return;   /* LESS：相等不通过 */
        if (p.depthWrite) *d = z;
    }

    float frag[4];
    const float vr = w0 * v0->r + w1 * v1->r + w2 * v2->r;
    const float vg = w0 * v0->g + w1 * v1->g + w2 * v2->g;
    const float vb = w0 * v0->b + w1 * v1->b + w2 * v2->b;
    const float va = w0 * v0->a + w1 * v1->a + w2 * v2->a;
    if (p.textured) {
        const float u = w0 * v0->u + w1 * v1->u + w2 * v2->u;
        const float v = w0 * v0->v + w1 * v1->v + w2 * v2->v;
        float tex[4];
        sample_fast(p.texState, u, v, tex);
        /* 纹理存预乘色、顶点色直通：straight = texStraight * vcol，再预乘
         * ⇒ rgb = tex.rgb * vcol.rgb * vcol.a, a = tex.a * vcol.a */
        frag[0] = tex[0] * vr * va;
        frag[1] = tex[1] * vg * va;
        frag[2] = tex[2] * vb * va;
        frag[3] = tex[3] * va;
    } else {
        frag[0] = vr * va;
        frag[1] = vg * va;
        frag[2] = vb * va;
        frag[3] = va;
    }

    uint8_t* q = tgt.data() + (static_cast<size_t>(yi) * tgt.w + xi) * 4u;
    float dst[4];
    px_load(q, tgt.fmt, dst);
    blend_premul(dst, frag, p.blend);
    px_store(q, tgt.fmt, dst);
}

void loop_general(const Paint& p, const RVert* va, const RVert* vb, const RVert* vd, int x0,
                  int x1, int y0, int y1, float area) {
    const bool tl0 = is_top_left(vb, vd);
    const bool tl1 = is_top_left(vd, va);
    const bool tl2 = is_top_left(va, vb);
    const float inv = 1.0f / area;
    for (int y = y0; y < y1; y++) {
        const float py = static_cast<float>(y) + 0.5f;
        for (int x = x0; x < x1; x++) {
            const float px = static_cast<float>(x) + 0.5f;
            const float e0 = edgef(vb, vd, px, py);
            const float e1 = edgef(vd, va, px, py);
            const float e2 = edgef(va, vb, px, py);
            if (!((e0 > 0.0f) || (e0 == 0.0f && tl0))) continue;
            if (!((e1 > 0.0f) || (e1 == 0.0f && tl1))) continue;
            if (!((e2 > 0.0f) || (e2 == 0.0f && tl2))) continue;
            shade_general(p, px, py, va, vb, vd, e0 * inv, e1 * inv, e2 * inv);
        }
    }
}

/* 均匀色 + 无深度：挑一条查表混合 */
void dispatch_uniform(Paint& p, const RVert* a, const RVert* b, const RVert* d, float src[4],
                      int x0, int x1, int y0, int y1) {
    const uint32_t packed = pack_px(p.tgt->fmt, q8(src[0]), q8(src[1]), q8(src[2]), q8(src[3]));
    switch (p.blend) {
        case BGPU_BLEND_REPLACE:
            loop_uniform<BGPU_BLEND_REPLACE, true>(p, a, b, d, packed, x0, x1, y0, y1);
            return;
        case BGPU_BLEND_KEEP:
            prepare_blend_lut<BGPU_BLEND_KEEP>(p, src);
            loop_uniform<BGPU_BLEND_KEEP, false>(p, a, b, d, packed, x0, x1, y0, y1);
            return;
        case BGPU_BLEND_ERASE:
            prepare_blend_lut<BGPU_BLEND_ERASE>(p, src);
            loop_uniform<BGPU_BLEND_ERASE, false>(p, a, b, d, packed, x0, x1, y0, y1);
            return;
        case BGPU_BLEND_ADDITIVE:
            prepare_blend_lut<BGPU_BLEND_ADDITIVE>(p, src);
            loop_uniform<BGPU_BLEND_ADDITIVE, false>(p, a, b, d, packed, x0, x1, y0, y1);
            return;
        default:
            /* 预乘：srcA 正好 1 时 dst*0 == 0，结果就是 src —— 等价于直接写 */
            if (src[3] >= 1.0f) {
                loop_uniform<BGPU_BLEND_PREMULTIPLIED, true>(p, a, b, d, packed, x0, x1, y0, y1);
            } else {
                prepare_blend_lut<BGPU_BLEND_PREMULTIPLIED>(p, src);
                loop_uniform<BGPU_BLEND_PREMULTIPLIED, false>(p, a, b, d, packed, x0, x1, y0, y1);
            }
            return;
    }
}

/* 一个三角形：算包围盒 / 裁剪，然后挑一条循环 */
void raster_tri(Paint& p, RVert* a, RVert* b, RVert* d) {
    float area = (b->x - a->x) * (d->y - a->y) - (b->y - a->y) * (d->x - a->x);
    if (area == 0.0f) return;
    if (area < 0.0f) {
        RVert tmp = *b;
        *b = *d;
        *d = tmp;
        area = -area;
    }
    /* 像素包围盒（右/下开区间） */
    const float fx0 = a->x < b->x ? (a->x < d->x ? a->x : d->x) : (b->x < d->x ? b->x : d->x);
    const float fx1 = a->x > b->x ? (a->x > d->x ? a->x : d->x) : (b->x > d->x ? b->x : d->x);
    const float fy0 = a->y < b->y ? (a->y < d->y ? a->y : d->y) : (b->y < d->y ? b->y : d->y);
    const float fy1 = a->y > b->y ? (a->y > d->y ? a->y : d->y) : (b->y > d->y ? b->y : d->y);
    int x0 = static_cast<int>(std::floor(fx0));
    int x1 = static_cast<int>(std::ceil(fx1));
    int y0 = static_cast<int>(std::floor(fy0));
    int y1 = static_cast<int>(std::ceil(fy1));
    if (x1 <= x0 || y1 <= y0) return;
    if (x0 < p.l) x0 = p.l;
    if (y0 < p.t) y0 = p.t;
    if (x1 > p.r) x1 = p.r;
    if (y1 > p.b) y1 = p.b;
    if (x1 <= x0 || y1 <= y0) return;

    const bool uniformColor = (a->r == b->r && b->r == d->r && a->g == b->g && b->g == d->g &&
                               a->b == b->b && b->b == d->b && a->a == b->a && b->a == d->a);
    /* 只在不挂深度时走快路径：深度比较用**插值后的 z**，换成常数值虽然也只差 1 ulp，
     * 但那是"相等不通过"的比较，不值得为它冒风险。 */
    const bool wideTarget = (p.tgt->fmt == BGPU_FORMAT_RGBA8 || p.tgt->fmt == BGPU_FORMAT_BGRA8);
    if (!p.depthOn && uniformColor && wideTarget) {
        if (!p.textured) {
            float src[4];
            src[0] = a->r * a->a;
            src[1] = a->g * a->a;
            src[2] = a->b * a->a;
            src[3] = a->a;
            dispatch_uniform(p, a, b, d, src, x0, x1, y0, y1);
            return;
        }
        /* 纹理：顶点色恒定 → 采样逐像素、颜色只乘一次 */
        const float mult[4] = {a->r * a->a, a->g * a->a, a->b * a->a, a->a};
        const bool uniformUV = (a->u == b->u && b->u == d->u && a->v == b->v && b->v == d->v);
        if (uniformUV) {
            float tex[4];
            sample_fast(p.texState, a->u, a->v, tex);
            float src[4];
            src[0] = tex[0] * a->r * a->a;
            src[1] = tex[1] * a->g * a->a;
            src[2] = tex[2] * a->b * a->a;
            src[3] = tex[3] * a->a;
            dispatch_uniform(p, a, b, d, src, x0, x1, y0, y1);
            return;
        }
        if (p.blend == BGPU_BLEND_PREMULTIPLIED) {
            loop_textured_uniform<BGPU_BLEND_PREMULTIPLIED>(p, a, b, d, mult, 1.0f / area, x0,
                                                            x1, y0, y1);
            return;
        }
    }

    loop_general(p, a, b, d, x0, x1, y0, y1, area);
}

void transform_vert(const Cmd& c, const Texture& tgt, bool affine, const BgpuVertex& in,
                    RVert& out) {
    float sx = c.vp[0] + (in.x / static_cast<float>(tgt.w)) * c.vp[2];
    float sy = c.vp[1] + (in.y / static_cast<float>(tgt.h)) * c.vp[3];
    if (affine) {
        const float* m = c.xf;
        const float nx = m[0] * sx + m[1] * sy + m[2];
        const float ny = m[3] * sx + m[4] * sy + m[5];
        sx = nx;
        sy = ny;
    }
    out.x = sx;
    out.y = sy;
    out.z = in.z;
    out.u = in.u;
    out.v = in.v;
    out.r = ((in.color >> 16) & 0xFFu) / 255.0f;
    out.g = ((in.color >> 8) & 0xFFu) / 255.0f;
    out.b = (in.color & 0xFFu) / 255.0f;
    out.a = ((in.color >> 24) & 0xFFu) / 255.0f;
}

void build_paint(Cmd& c, Paint& p) {
    CpuTexture* tgt = static_cast<CpuTexture*>(c.target);
    p.tgt = tgt;
    p.depth = c.depth ? static_cast<CpuTexture*>(c.depth) : nullptr;
    p.blend = c.pipe->blend;
    p.textured = (c.pipe->pipe_flags & BGPU_PIPE_TEXTURED) != 0 && c.tex != nullptr;
    if (p.textured) p.texState = make_tex_state(static_cast<CpuTexture*>(c.tex), c.samp);
    p.depthOn = p.depth != nullptr && p.depth->depth_w == tgt->w && p.depth->depth_h == tgt->h;
    p.depthTest = p.depthOn && (c.pipe->pipe_flags & BGPU_PIPE_DEPTH_TEST) != 0;
    p.depthWrite = p.depthOn && (c.pipe->pipe_flags & BGPU_PIPE_DEPTH_WRITE) != 0;
    int l = 0, t = 0, r = static_cast<int>(tgt->w), b = static_cast<int>(tgt->h);
    if (c.scissorOn) {
        const int sl = c.sc[0];
        const int st = c.sc[1];
        const int sr = c.sc[0] + c.sc[2];
        const int sb = c.sc[1] + c.sc[3];
        if (sl > l) l = sl;
        if (st > t) t = st;
        if (sr < r) r = sr;
        if (sb < b) b = sb;
    }
    p.l = l;
    p.t = t;
    p.r = r;
    p.b = b;
}



/* 交换链位图 → 直通 RGBA8（frame_read 的输出永远是 RGBA8，与内部格式无关） */
void copy_out_straight(const std::vector<uint8_t>& src, BgpuFormat fmt, uint32_t w, uint32_t h,
                       uint8_t* dst) {
    const uint32_t bpp = bytesPerPx(fmt);
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t* p = src.data() + (static_cast<size_t>(y) * w + x) * bpp;
            uint8_t r, g, b, a;
            if (fmt == BGPU_FORMAT_BGRA8) {
                b = p[0];
                g = p[1];
                r = p[2];
                a = p[3];
            } else {
                r = p[0];
                g = p[1];
                b = p[2];
                a = p[3];
            }
            uint8_t* o = dst + (static_cast<size_t>(y) * w + x) * 4u;
            o[0] = unpremul8(r, a);
            o[1] = unpremul8(g, a);
            o[2] = unpremul8(b, a);
            o[3] = a;
        }
    }
}

/* ============================================================
 * CpuBackend
 * ============================================================ */
class CpuBackend final : public BackendBase {
public:
    BgpuBackendKind kind() const noexcept override { return BGPU_BACKEND_CPU; }
    const char* name() const noexcept override { return "cpu"; }
    bool available() const noexcept override { return true; }

    BgpuResult createDevice(const BgpuDeviceDesc& desc, BgpuDevice* out) override {
        const uint64_t handle =
            makeObject<Device>(*this, BGPU_INVALID, desc.flags, desc.width, desc.height);
        if (handle == BGPU_INVALID) {
            setReason("对象表分配失败（内存不足）");
            return BGPU_ERR_OUT_OF_MEMORY;
        }
        Device* dev = static_cast<Device*>(lookupObject(handle, kObjDevice));
        dev->self = handle;
        dev->surface_format = BGPU_FORMAT_RGBA8;
        /* 窗口由 VK/D3D12 负责：CPU 只在 HEADLESS 下工作（见 createSwapchain）。 */
        (void)desc.native_window;
        *out = handle;
        return BGPU_OK;
    }

    void deviceInfo(Device& dev, BgpuDeviceInfo& out) override {
        out.backend = BGPU_BACKEND_CPU;
        out.surface_format = dev.surface_format;
        out.api_version = 1;   /* CPU 参考实现第 1 版，仅上报 */
        copyStr(out.adapter_name, sizeof(out.adapter_name), "binGPU CPU reference");
        copyStr(out.driver_name, sizeof(out.driver_name), "scalar C++17");
    }

    void deviceCaps(Device&, BgpuCaps& out) override {
        /* 如实上报：能深度/离屏/回读/采样器/仿射/网格；没有 MSAA、没有垂直同步、
         * 不呈现到窗口 —— 那三个 cap 位故意不置。 */
        out.flags = BGPU_CAP_DEPTH | BGPU_CAP_OFFSCREEN | BGPU_CAP_READBACK |
                    BGPU_CAP_SAMPLERS | BGPU_CAP_AFFINE | BGPU_CAP_MESH;
        out.max_texture_size = BGPU_MAX_TEXTURE_SIZE;
        out.max_frames_in_flight = 1;
        out.msaa_samples = 1;
        out.max_quads_per_draw = BGPU_MAX_QUADS_PER_DRAW;
    }

    void deviceStats(Device& dev, BgpuStats& out) override {
        out.frame_index = dev.frame_index;
        out.frames_presented = dev.frames_presented;
        out.draws = dev.draws;
        out.quads = dev.quads;
        out.triangles = dev.tris;
        out.bytes_uploaded = dev.bytes_uploaded;
        out.bytes_live = dev.bytes_live;
    }

    /* ---------------- 交换链 / 帧 ---------------- */
    BgpuResult createSwapchain(Device& dev, const BgpuSwapchainDesc& desc,
                               BgpuSwapchain* out) override {
        if (desc.native_window != nullptr) {
            /* 真窗口留给 VK/D3D12：CPU 后端要往 HWND 上贴一帧就得走 GDI，
             * 那会把"参考实现"和窗口/DPI/合成器这些不可复现的东西绑在一起。 */
            setReason("CPU 后端只做 HEADLESS（native_window 必须为 NULL）；"
                      "窗口呈现请用 Vulkan/D3D12 后端");
            return BGPU_ERR_UNSUPPORTED;
        }
        const uint64_t handle = makeObject<CpuSwapchain>(*this, dev.self, dev);
        if (handle == BGPU_INVALID) {
            setReason("对象表分配失败（内存不足）");
            return BGPU_ERR_OUT_OF_MEMORY;
        }
        CpuSwapchain* sc = static_cast<CpuSwapchain*>(lookupObject(handle, kObjSwapchain));
        sc->dev = &dev;
        sc->w = desc.width;
        sc->h = desc.height;
        sc->fmt = pickSurfaceFormat(desc.format);
        sc->sc_flags = desc.flags;
        const size_t bitmaps = static_cast<size_t>(sc->w) * sc->h * bytesPerPx(sc->fmt) * 2u;
        sc->back.assign(bitmaps / 2u, 0);
        sc->front.assign(bitmaps / 2u, 0);
        dev.bytes_live += static_cast<uint64_t>(bitmaps);

        /* 借出的帧目标：别名到 back（只存 vector*，resize 重新分配后自动跟上）。 */
        const uint64_t t = makeObject<CpuTexture>(*this, dev.self, dev);
        if (t == BGPU_INVALID) {
            destroyObject(handle);
            setReason("对象表分配失败（内存不足）");
            return BGPU_ERR_OUT_OF_MEMORY;
        }
        CpuTexture* tex = static_cast<CpuTexture*>(lookupObject(t, kObjTexture));
        tex->dev = &dev;
        tex->w = sc->w;
        tex->h = sc->h;
        tex->fmt = sc->fmt;
        tex->tex_flags = BGPU_TEXTURE_RENDER_TARGET;
        tex->mip_levels = 1;
        tex->borrowed = true;
        tex->alias = &sc->back;
        tex->flags |= kObjFlagBorrowed;
        sc->target = t;
        *out = handle;
        return BGPU_OK;
    }

    BgpuResult resizeSwapchain(Swapchain& sc_, uint32_t w, uint32_t h) override {
        CpuSwapchain& sc = static_cast<CpuSwapchain&>(sc_);
        Device& dev = *sc.dev;
        const size_t bytes = static_cast<size_t>(w) * h * bytesPerPx(sc.fmt);
        std::vector<uint8_t> nb(bytes, 0), nf(bytes, 0);   /* 先分配成功再替换 */
        dev.bytes_live -= static_cast<uint64_t>(sc.w) * sc.h * bytesPerPx(sc.fmt) * 2u;
        sc.back.swap(nb);
        sc.front.swap(nf);
        sc.w = w;
        sc.h = h;
        dev.bytes_live += static_cast<uint64_t>(bytes) * 2u;
        CpuTexture* tex = static_cast<CpuTexture*>(lookupObject(sc.target, kObjTexture));
        if (tex) {
            tex->w = w;
            tex->h = h;
            tex->alias = &sc.back;   /* 别名重新指向（vector 内容已换） */
        }
        return BGPU_OK;
    }

    void swapchainInfo(Swapchain& sc, BgpuSwapchainInfo& out) override {
        out.width = sc.w;
        out.height = sc.h;
        out.format = sc.fmt;
        out.image_count = 1;           /* 单缓冲：frame_end 就是 memcpy，没有队列 */
        out.msaa_samples = 1;
        out.present_paces_frames = 0;  /* 不阻塞；外壳自己按刷新率节流 */
    }

    BgpuResult frameRead(Swapchain& sc_, void* dst, size_t dst_size,
                         uint32_t* out_stride) override {
        CpuSwapchain& sc = static_cast<CpuSwapchain&>(sc_);
        const uint64_t need = static_cast<uint64_t>(sc.w) * sc.h * 4u;
        if (dst_size < need) {
            setReason(cat("dst_size=", dst_size, " 不够（", sc.w, "x", sc.h,
                          " RGBA8 需要 ", need, " 字节）"));
            return BGPU_ERR_INVALID_ARGUMENT;
        }
        copy_out_straight(sc.front, sc.fmt, sc.w, sc.h, static_cast<uint8_t*>(dst));
        if (out_stride) *out_stride = sc.w * 4u;
        return BGPU_OK;
    }

    /* ---------------- 纹理 ---------------- */
    BgpuResult createTexture(Device& dev, const BgpuTextureDesc& desc,
                             BgpuTexture* out) override {
        /* 渲染目标必须是 4 字节格式：光栅与 clear 都按一次写 4 字节实现（0.2.0 的
         * px_store 也是），R8 目标会写越界。0.2.1 起明确拒绝，而不是悄悄踩内存。 */
        if ((desc.flags & BGPU_TEXTURE_RENDER_TARGET) && desc.format == BGPU_FORMAT_R8) {
            setReason("R8 不能当渲染目标（只有 RGBA8/BGRA8 可以；R8 用作采样遮罩）");
            return BGPU_ERR_INVALID_ARGUMENT;
        }
        const uint64_t handle = makeObject<CpuTexture>(*this, dev.self, dev);
        if (handle == BGPU_INVALID) {
            setReason("对象表分配失败（内存不足）");
            return BGPU_ERR_OUT_OF_MEMORY;
        }
        CpuTexture* t = static_cast<CpuTexture*>(lookupObject(handle, kObjTexture));
        t->dev = &dev;
        t->w = desc.width;
        t->h = desc.height;
        t->fmt = desc.format;
        t->tex_flags = desc.flags;
        /* mip：固定管线没有 LOD 输入，采样永远取第 0 级 → CPU 只存一张图。
         * 调用方要 MIPMAPS 时**接受但降级**（info 上报真实值），见 docs/backends.md。 */
        t->mip_levels = 1;
        t->bytes = static_cast<uint64_t>(t->w) * t->h * bytesPerPx(t->fmt);
        t->own.assign(static_cast<size_t>(t->bytes), 0);
        dev.bytes_live += t->bytes;
        *out = handle;
        return BGPU_OK;
    }

    void textureInfo(Texture& tex, BgpuTextureInfo& out) override {
        out.width = tex.w;
        out.height = tex.h;
        out.format = tex.fmt;
        out.flags = tex.tex_flags;
        out.mip_levels = tex.mip_levels;
    }

    BgpuResult textureUpload(Texture& tex_, const BgpuBox* box, const void* pixels,
                             uint32_t stride) override {
        CpuTexture& tex = static_cast<CpuTexture&>(tex_);
        const uint32_t bpp = bytesPerPx(tex.fmt);
        const uint32_t x0 = box ? box->x : 0u;
        const uint32_t y0 = box ? box->y : 0u;
        const uint32_t w = box ? box->w : tex.w;
        const uint32_t h = box ? box->h : tex.h;
        if (w == 0 || h == 0 || x0 + w > tex.w || y0 + h > tex.h) {
            setReason(cat("上传区域 ", x0, ",", y0, " ", w, "x", h, " 超出纹理 ", tex.w, "x",
                          tex.h));
            return BGPU_ERR_INVALID_ARGUMENT;
        }
        const uint32_t row = stride ? stride : w * bpp;
        if (row < w * bpp) {
            setReason(cat("stride=", row, " 小于一行 ", w * bpp, " 字节"));
            return BGPU_ERR_INVALID_ARGUMENT;
        }
        const uint8_t* src = static_cast<const uint8_t*>(pixels);
        uint8_t* base = tex.data();
        for (uint32_t y = 0; y < h; y++) {
            const uint8_t* srow = src + static_cast<size_t>(y) * row;
            uint8_t* drow = base + (static_cast<size_t>(y0 + y) * tex.w + x0) * bpp;
            for (uint32_t x = 0; x < w; x++) {
                const uint8_t* s = srow + static_cast<size_t>(x) * bpp;
                uint8_t* d = drow + static_cast<size_t>(x) * bpp;
                if (tex.fmt == BGPU_FORMAT_R8) {
                    d[0] = s[0];   /* 遮罩：原样存，采样时当作 (v,v,v,v) */
                } else if (tex.fmt == BGPU_FORMAT_BGRA8) {
                    const uint32_t a = s[3];
                    d[0] = premul8(s[0], a);
                    d[1] = premul8(s[1], a);
                    d[2] = premul8(s[2], a);
                    d[3] = static_cast<uint8_t>(a);
                } else {
                    const uint32_t a = s[3];
                    d[0] = premul8(s[0], a);
                    d[1] = premul8(s[1], a);
                    d[2] = premul8(s[2], a);
                    d[3] = static_cast<uint8_t>(a);
                }
            }
        }
        if (tex.dev) tex.dev->bytes_uploaded += static_cast<uint64_t>(row) * h;
        return BGPU_OK;
    }

    BgpuResult textureRead(Texture& tex_, void* dst, size_t dst_size,
                           uint32_t* out_stride) override {
        CpuTexture& tex = static_cast<CpuTexture&>(tex_);
        if (!(tex.tex_flags & (BGPU_TEXTURE_CPU_READ | BGPU_TEXTURE_RENDER_TARGET))) {
            setReason("纹理没有 BGPU_TEXTURE_CPU_READ / RENDER_TARGET 标志，不能回读");
            return BGPU_ERR_UNSUPPORTED;
        }
        const uint32_t bpp = bytesPerPx(tex.fmt);
        const uint64_t need = static_cast<uint64_t>(tex.w) * tex.h * bpp;
        if (dst_size < need) {
            setReason(cat("dst_size=", dst_size, " 不够（", tex.w, "x", tex.h, " 需要 ", need,
                          " 字节）"));
            return BGPU_ERR_INVALID_ARGUMENT;
        }
        uint8_t* out = static_cast<uint8_t*>(dst);
        const uint8_t* base = tex.data();
        if (tex.fmt == BGPU_FORMAT_R8) {
            if (need) std::memcpy(out, base, static_cast<size_t>(need));
            if (out_stride) *out_stride = tex.w;
            return BGPU_OK;
        }
        for (uint32_t y = 0; y < tex.h; y++) {
            for (uint32_t x = 0; x < tex.w; x++) {
                const uint8_t* p = base + (static_cast<size_t>(y) * tex.w + x) * bpp;
                uint8_t* o = out + (static_cast<size_t>(y) * tex.w + x) * bpp;
                uint8_t r, g, b, a;
                if (tex.fmt == BGPU_FORMAT_BGRA8) {
                    b = p[0];
                    g = p[1];
                    r = p[2];
                    a = p[3];
                } else {
                    r = p[0];
                    g = p[1];
                    b = p[2];
                    a = p[3];
                }
                const uint8_t sr = unpremul8(r, a), sg = unpremul8(g, a), sb = unpremul8(b, a);
                if (tex.fmt == BGPU_FORMAT_BGRA8) {
                    o[0] = sb;
                    o[1] = sg;
                    o[2] = sr;
                } else {
                    o[0] = sr;
                    o[1] = sg;
                    o[2] = sb;
                }
                o[3] = a;
            }
        }
        if (out_stride) *out_stride = tex.w * bpp;
        return BGPU_OK;
    }

    /* ---------------- 缓冲 / 采样器 / 管线 ---------------- */
    BgpuResult createBuffer(Device& dev, const BgpuBufferDesc& desc, BgpuBuffer* out) override {
        const uint64_t handle = makeObject<CpuBuffer>(*this, dev.self, dev);
        if (handle == BGPU_INVALID) {
            setReason("对象表分配失败（内存不足）");
            return BGPU_ERR_OUT_OF_MEMORY;
        }
        CpuBuffer* b = static_cast<CpuBuffer*>(lookupObject(handle, kObjBuffer));
        b->dev = &dev;
        b->size = desc.size;
        b->usage = desc.usage;
        b->bytes = desc.size;
        b->data.assign(desc.size, 0);
        dev.bytes_live += b->bytes;
        *out = handle;
        return BGPU_OK;
    }

    BgpuResult bufferUpdate(Buffer& buf_, uint32_t offset, const void* data,
                            uint32_t size) override {
        CpuBuffer& buf = static_cast<CpuBuffer&>(buf_);
        if (static_cast<uint64_t>(offset) + size > buf.size) {
            setReason(cat("写入区间 ", offset, "+", size, " 超出缓冲 ", buf.size, " 字节"));
            return BGPU_ERR_INVALID_ARGUMENT;
        }
        std::memcpy(buf.data.data() + offset, data, size);
        if (buf.dev) buf.dev->bytes_uploaded += size;
        return BGPU_OK;
    }

    BgpuResult createSampler(Device& dev, const BgpuSamplerDesc& desc,
                             BgpuSampler* out) override {
        const uint64_t handle = makeObject<Sampler>(*this, dev.self);
        if (handle == BGPU_INVALID) {
            setReason("对象表分配失败（内存不足）");
            return BGPU_ERR_OUT_OF_MEMORY;
        }
        static_cast<Sampler*>(lookupObject(handle, kObjSampler))->desc = desc;
        *out = handle;
        return BGPU_OK;
    }

    BgpuResult createPipeline(Device& dev, const BgpuPipelineDesc& desc,
                              BgpuPipeline* out) override {
        const uint64_t handle = makeObject<Pipeline>(*this, dev.self);
        if (handle == BGPU_INVALID) {
            setReason("对象表分配失败（内存不足）");
            return BGPU_ERR_OUT_OF_MEMORY;
        }
        Pipeline* p = static_cast<Pipeline*>(lookupObject(handle, kObjPipeline));
        p->layout = desc.vertex_layout;
        p->blend = desc.blend;
        p->pipe_flags = desc.flags;
        *out = handle;
        return BGPU_OK;
    }

protected:
    /* ---------------- 帧呈现 / 深度 / 清除 ---------------- */
    void presentFrame(Swapchain& sc_) override {
        CpuSwapchain& sc = static_cast<CpuSwapchain&>(sc_);
        /* "呈现"= back 拷进 front。单缓冲 CPU 后端没有队列，这一步是同步的；
         * frame_read 读 front，所以"上一帧"的语义与 GPU 后端一致。 */
        sc.front = sc.back;
    }

    BgpuResult prepareDepth(Texture& depth_, Texture& target, const BgpuRenderBeginDesc& desc) override {
        CpuTexture& depth = static_cast<CpuTexture&>(depth_);
        const uint32_t w = target.w, h = target.h;
        if (depth.depth.empty() || depth.depth_w != w || depth.depth_h != h) {
            depth.depth.assign(static_cast<size_t>(w) * h, 1.0f);
            depth.depth_w = w;
            depth.depth_h = h;
        }
        if (desc.flags & BGPU_RENDER_CLEAR_DEPTH) {
            for (float& d : depth.depth) d = desc.clear_depth;
        }
        return BGPU_OK;
    }

    BgpuResult clearColor(Texture& target_, uint32_t argb) override {
        CpuTexture& target = static_cast<CpuTexture&>(target_);
        /* 填充色对每个像素都是同一个值：预乘 + 量化一次，然后按 uint32 铺。
         * 与 store_straight 的逐像素浮点写法逐位相同（q8(k/255*255) == k，
         * 0..255 全部成立 —— tests/test_cpu_render.c 的 clear 用例守着这条）。 */
        const uint32_t a = (argb >> 24) & 0xFFu;
        const uint32_t r = premul8((argb >> 16) & 0xFFu, a);
        const uint32_t g = premul8((argb >> 8) & 0xFFu, a);
        const uint32_t b = premul8(argb & 0xFFu, a);
        const uint32_t packed = pack_px(target.fmt, r, g, b, a);
        uint32_t* dst = reinterpret_cast<uint32_t*>(target.data());
        const size_t n = static_cast<size_t>(target.w) * target.h;
        for (size_t i = 0; i < n; i++) dst[i] = packed;
        return BGPU_OK;
    }

    /* ---------------- 光栅化 ---------------- */
    BgpuResult rasterQuads(Cmd& c, const BgpuVertex* verts, uint32_t quad_count) override {
        Paint p;
        build_paint(c, p);
        const bool affine = c.affineOn;
        for (uint32_t q = 0; q < quad_count; q++) {
            const BgpuVertex* v = verts + static_cast<size_t>(q) * 4u;
            RVert t[4];
            for (int i = 0; i < 4; i++) transform_vert(c, *p.tgt, affine, v[i], t[i]);
            /* TL,TR,BL,BR → 三角形 (0,1,2) 与 (2,1,3)：与 binUI 的 emitQuad 同一套索引，
             * 保证共享的对角线在 top-left 规则下只被一个三角形覆盖。 */
            raster_tri(p, &t[0], &t[1], &t[2]);
            raster_tri(p, &t[2], &t[1], &t[3]);
        }
        return BGPU_OK;
    }

    BgpuResult rasterIndexed(Cmd& c, Buffer& vb_, uint32_t vertex_offset, Buffer& ib_,
                             uint32_t index_count, uint32_t index_offset) override {
        CpuBuffer& vb = static_cast<CpuBuffer&>(vb_);
        CpuBuffer& ib = static_cast<CpuBuffer&>(ib_);
        Paint p;
        build_paint(c, p);

        /* 先整段校验索引（越界是调用方 bug：宁可一笔都不画，也不要"画一半再报错"）。 */
        const uint32_t* idx = reinterpret_cast<const uint32_t*>(ib.data.data() + index_offset);
        const uint32_t vsize = static_cast<uint32_t>(sizeof(BgpuMeshVertex));
        for (uint32_t i = 0; i < index_count; i++) {
            const uint64_t off =
                static_cast<uint64_t>(vertex_offset) + static_cast<uint64_t>(idx[i]) * vsize;
            if (off + vsize > vb.size) {
                return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__,
                            cat("索引 ", i, " 指向顶点 ", idx[i], "，超出顶点缓冲（offset=",
                                vertex_offset, " size=", vb.size, "）"));
            }
        }

        for (uint32_t i = 0; i + 2 < index_count; i += 3) {
            RVert tri[3];
            for (int k = 0; k < 3; k++) {
                const uint32_t vi = idx[i + static_cast<uint32_t>(k)];
                BgpuMeshVertex mv;
                std::memcpy(&mv,
                            vb.data.data() + vertex_offset + static_cast<size_t>(vi) * vsize,
                            sizeof(mv));
                const BgpuVertex v2 = {mv.x, mv.y, mv.z, mv.u, mv.v, mv.color};
                transform_vert(c, *p.tgt, c.affineOn, v2, tri[k]);
            }
            raster_tri(p, &tri[0], &tri[1], &tri[2]);
        }
        return BGPU_OK;
    }

private:
    static BgpuFormat pickSurfaceFormat(BgpuFormat want) {
        /* 只支持两种 8 位格式；R8 之类退回 RGBA8 —— 交换链是画布，不是遮罩。 */
        return (want == BGPU_FORMAT_BGRA8) ? BGPU_FORMAT_BGRA8 : BGPU_FORMAT_RGBA8;
    }

    static void copyStr(char* dst, size_t cap, const char* src) {
        if (!dst || cap == 0) return;
        size_t i = 0;
        for (; src[i] && i + 1 < cap; i++) dst[i] = src[i];
        dst[i] = '\0';
    }
};

}  // namespace

Backend& cpuBackend() {
    static CpuBackend backend;
    return backend;
}

}  // namespace bgpu
