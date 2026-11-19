/* bingpu_pace.cpp —— 帧节拍器（0.9.0）
 *
 * 这一层为什么该有这个文件，见 include/bingpu.h 里 "帧节拍器" 那段注释。这里只记
 * **实现上的三个决定**，以及踩过的坑：
 *
 * ① 为什么 `pacer` 是调用方持有的一块 POD，而不是一个不透明句柄：
 *    它没有资源、没有生命周期问题，只有几个整数状态；做成句柄就要多一层句柄表、
 *    多一种"忘了 destroy"的失败模式。而且它是**每窗口一个**的东西，调用方（bin2d
 *    的合成器 / 外壳）天生就有一个合适的位置放它。
 *
 * ② 为什么时间用 `int64_t 微秒` 由调用方传进来，而不是这里读 steady_clock：
 *    · 调用方本来就得有一份自己的时钟（它要睡、要算 dt），两份时钟必然有一处忘了同步；
 *    · 传进来的话这个文件是**纯函数式的状态机**，单元测试可以喂任意时间序列，
 *      不用真睡 —— 节拍逻辑是那种"错一格就肉眼可见"的东西，必须能离线测。
 *
 * ③ 为什么"下一拍 = max(本拍开始 + 目标周期, 本拍结束)"这一条就够了：
 *    这是 0.8.0 踩了三天坑换来的。旧写法（外壳里）是"下一拍 = 本拍开始 + 整数个
 *    刷新周期"，然后靠一个 `present 耗时 >= 半个刷新周期` 的启发式决定要不要少睡一格。
 *    启发式漏掉的情形就是**双重等待**：present 自己已经等了一格（垂直同步），
 *    外壳再补一格 → 120 Hz 屏上实测掉到 55 fps（Vulkan），而完全相同的代码
 *    裸跑 present 是 119.9 fps。`max(..., 本拍结束)` 不需要任何启发式：
 *    present 等掉的时间已经体现在"本拍结束"里了，就不会在后面重复补。
 *
 * 还有一处必须记下来的（不然改的时候一定会再踩）：**估计 k 用的耗时要去掉 present 的等待**。
 *   present 阻塞到垂直同步时，"本拍耗时"里含着一整格等待 —— 直接拿它去算
 *   `k = ceil(耗时/网格)` 会算出 2（120 Hz 屏上）→ 目标周期翻倍 → 60 fps。
 *   所以 present 耗时 ≥ 半个网格时视为"它在等刷新"，估计耗时里把它减掉；
 *   否则（不等刷新的 present）它就是真实成本，要算进去。
 */
#include <cmath>
#include <cstdio>
#include <cstring>

#include "bingpu_internal.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

/* 刷新率上限：>360 视为驱动乱报（远距离/远程会话常见），按 60 处理 */
constexpr int kMaxRefreshHz = 360;
constexpr int kFallbackRefreshHz = 60;
/* 滞回幅度：占网格的比例。降档要求"本拍真的装得下更小的档"再留这么多余量，
 * 否则帧耗时压在档位边界上会 2/3 交替，那本身就是一顿一顿。 */
constexpr double kHysteresis = 0.15;
/* 一个节拍最多吸收多少个网格（防止数据异常时排出几百拍的空转） */
constexpr int kMaxSteps = 512;

int ClampRefresh(int hz) {
    if (hz <= 1) return kFallbackRefreshHz;
    if (hz > kMaxRefreshHz) return kFallbackRefreshHz;
    return hz;
}

int EffectiveRefresh(const BgpuPacer* p) {
    return p && p->refresh_hz > 1 ? ClampRefresh(p->refresh_hz) : kFallbackRefreshHz;
}

/* 网格（微秒）。**比刷新更细的策略**（PACE_240 落在 120 Hz 屏上）时网格取目标周期，
 * 否则"240 fps"会被 120 Hz 的网格直接钉成 120 —— 那还不如不许选。 */
int64_t GridUs(const BgpuPacer* p) {
    const int target = bingpu_pace_policy_fps(static_cast<BgpuPacePolicy>(p->policy));
    int hz = EffectiveRefresh(p);
    if (target > hz) hz = target;
    return 1000000 / hz;
}

/* 目标周期占几个网格（最少 1）。
 * 只能取**整数个网格**：帧落点不在刷新边界上就抖（这正是网格存在的理由）。
 * 所以"90 fps 落在 120 Hz 屏上"实际做不到 —— 会四舍五入到 120 fps。
 * 这不是妥协，是物理：106 个 8.33 ms 里塞不下 90 个 11.11 ms 的整齐落点。
 * 喊出来的数字由 bingpu_pace_frame_ms 按生效周期给，不按名义策略给。 */
int MinSteps(const BgpuPacer* p) {
    const int target = bingpu_pace_policy_fps(static_cast<BgpuPacePolicy>(p->policy));
    if (target <= 0) return 1;                       /* unlimited / standard */
    int base = EffectiveRefresh(p);
    if (target > base) base = target;                /* 与 GridUs 同一口径 */
    long long k = (static_cast<long long>(base) + target / 2) / target;
    if (k < 1) k = 1;
    if (k > kMaxSteps) k = kMaxSteps;
    return static_cast<int>(k);
}

}  // namespace

extern "C" {

const char* bingpu_pace_policy_name(BgpuPacePolicy policy) {
    switch (policy) {
        case BGPU_PACE_STANDARD:  return "standard";
        case BGPU_PACE_UNLIMITED: return "unlimited";
        case BGPU_PACE_240:       return "240";
        case BGPU_PACE_120:       return "120";
        case BGPU_PACE_90:        return "90";
        case BGPU_PACE_60:        return "60";
        case BGPU_PACE_30:        return "30";
    }
    return "unknown";
}

int bingpu_pace_policy_parse(const char* text, BgpuPacePolicy* out_policy) {
    if (!out_policy) return 0;
    if (!text || !*text) return 0;
    /* 大小写不敏感的比较（不引入 <cctype> 的 locale 麻烦，手写一遍） */
    auto eq = [](const char* a, const char* b) {
        while (*a && *b) {
            char ca = *a++, cb = *b++;
            if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
            if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
            if (ca != cb) return false;
        }
        return *a == '\0' && *b == '\0';
    };
    struct Entry { const char* name; BgpuPacePolicy policy; };
    static const Entry kTable[] = {
        {"standard", BGPU_PACE_STANDARD}, {"auto", BGPU_PACE_STANDARD},
        {"refresh", BGPU_PACE_STANDARD},  {"unlimited", BGPU_PACE_UNLIMITED},
        {"none", BGPU_PACE_UNLIMITED},    {"off", BGPU_PACE_UNLIMITED},
        {"0", BGPU_PACE_UNLIMITED},       {"240", BGPU_PACE_240},
        {"120", BGPU_PACE_120},           {"90", BGPU_PACE_90},
        {"60", BGPU_PACE_60},             {"30", BGPU_PACE_30},
    };
    for (const Entry& e : kTable) {
        if (eq(text, e.name)) { *out_policy = e.policy; return 1; }
    }
    return 0;
}

int bingpu_pace_policy_fps(BgpuPacePolicy policy) {
    switch (policy) {
        case BGPU_PACE_UNLIMITED: return 0;
        case BGPU_PACE_240:       return 240;
        case BGPU_PACE_120:       return 120;
        case BGPU_PACE_90:        return 90;
        case BGPU_PACE_60:        return 60;
        case BGPU_PACE_30:        return 30;
        case BGPU_PACE_STANDARD:  return -1;   /* -1 = 跟刷新率，具体值要问 pacer */
    }
    return -1;
}

int bingpu_display_refresh_hz(void* native_window) {
#if defined(_WIN32)
    HWND hwnd = static_cast<HWND>(native_window);
    if (!hwnd) return 0;
    /* 为什么不直接调 GetDC / GetDeviceCaps：那要给 libbingpu 加 -lgdi32 -luser32 的
     * **链接期**依赖，而 binGPU 在无窗口的服务器/CI 上也要能被纯 CPU 用户链上
     * （其余 Win32 调用点也都是 LoadLibraryA + GetProcAddress，见 bingpu_gl.cpp）。
     * 这两个函数只在这里用一次，动态取一下最省事。 */
    using GetDCFn = HDC(WINAPI*)(HWND);
    using ReleaseDCFn = int(WINAPI*)(HWND, HDC);
    using GetDeviceCapsFn = int(WINAPI*)(HDC, int);
    static GetDCFn pGetDC = [] {
        HMODULE m = ::LoadLibraryA("user32.dll");
        return m ? reinterpret_cast<GetDCFn>(reinterpret_cast<void*>(
                       ::GetProcAddress(m, "GetDC")))
                 : nullptr;
    }();
    static ReleaseDCFn pReleaseDC = [] {
        HMODULE m = ::LoadLibraryA("user32.dll");
        return m ? reinterpret_cast<ReleaseDCFn>(reinterpret_cast<void*>(
                       ::GetProcAddress(m, "ReleaseDC")))
                 : nullptr;
    }();
    static GetDeviceCapsFn pGetDeviceCaps = [] {
        HMODULE m = ::LoadLibraryA("gdi32.dll");
        return m ? reinterpret_cast<GetDeviceCapsFn>(reinterpret_cast<void*>(
                       ::GetProcAddress(m, "GetDeviceCaps")))
                 : nullptr;
    }();
    if (!pGetDC || !pReleaseDC || !pGetDeviceCaps) return 0;
    HDC dc = pGetDC(hwnd);
    if (!dc) return 0;
    const int hz = pGetDeviceCaps(dc, VREFRESH);
    pReleaseDC(hwnd, dc);
    if (hz <= 1 || hz > kMaxRefreshHz) return 0;
    return hz;
#else
    (void)native_window;
    return 0;
#endif
}

void bingpu_pace_init(BgpuPacer* pacer, void* native_window) {
    if (!pacer) return;
    std::memset(pacer, 0, sizeof(*pacer));
    pacer->struct_size = static_cast<uint32_t>(sizeof(BgpuPacer));
    const int hz = bingpu_display_refresh_hz(native_window);
    pacer->refresh_hz = hz > 1 ? hz : kFallbackRefreshHz;
    pacer->policy = BGPU_PACE_STANDARD;
    pacer->steps = 0;
    pacer->started = 0;
    pacer->next_us = 0;
}

BgpuResult bingpu_pace_set_policy(BgpuPacer* pacer, BgpuPacePolicy policy) {
    bgpu::ApiScope api("bingpu_pace_set_policy");
    if (!pacer || pacer->struct_size != sizeof(BgpuPacer)) {
        return bgpu::fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__,
                          "pacer 为空或 struct_size 不对（先 bingpu_pace_init）");
    }
    if (static_cast<int>(policy) < 0 || policy > BGPU_PACE_30) {
        return bgpu::fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__,
                          bgpu::cat("未知的节拍策略 ", static_cast<int>(policy)));
    }
    const bool changed = pacer->policy != static_cast<int32_t>(policy);
    pacer->policy = static_cast<int32_t>(policy);
    if (changed) {
        /* 策略换了就把相位丢掉重新学：拿 30 fps 学出来的档位去套 120 fps，
         * 头几拍会连续迟到（每拍都要升档一次）。 */
        pacer->steps = 0;
        pacer->started = 0;
        pacer->next_us = 0;
    }
    return BGPU_OK;
}

BgpuPacePolicy bingpu_pace_policy(const BgpuPacer* pacer) {
    if (!pacer) return BGPU_PACE_STANDARD;
    return static_cast<BgpuPacePolicy>(pacer->policy);
}

double bingpu_pace_frame_ms(const BgpuPacer* pacer) {
    if (!pacer) return 0.0;
    const int target = bingpu_pace_policy_fps(static_cast<BgpuPacePolicy>(pacer->policy));
    if (target == 0) return 0.0;                 /* unlimited */
    /* **生效**周期，不是名义周期：120 Hz 屏上选 90 fps 实际跑在 120（见 MinSteps）。 */
    const double gridMs = static_cast<double>(GridUs(pacer)) / 1000.0;
    return gridMs * static_cast<double>(MinSteps(pacer));
}

int bingpu_pace_wants_vsync(const BgpuPacer* pacer) {
    if (!pacer) return 1;
    const int target = bingpu_pace_policy_fps(static_cast<BgpuPacePolicy>(pacer->policy));
    if (target == 0) return 0;                   /* unlimited：不关 vsync 就永远超不过刷新率 */
    if (target < 0) return 1;                    /* standard：垂直同步就是节拍器本身 */
    return target <= EffectiveRefresh(pacer) ? 1 : 0;
}

int64_t bingpu_pace_wait_us(const BgpuPacer* pacer, int64_t now_us) {
    if (!pacer) return 0;
    if (bingpu_pace_policy_fps(static_cast<BgpuPacePolicy>(pacer->policy)) == 0) return 0;
    if (!pacer->started || pacer->next_us <= 0) return 0;
    const int64_t wait = pacer->next_us - now_us;
    return wait > 0 ? wait : 0;
}

void bingpu_pace_frame_begin(BgpuPacer* pacer, int64_t now_us) {
    if (!pacer) return;
    pacer->frame_start_us = now_us;
    pacer->frame_open = 1;
    if (!pacer->started) {
        pacer->started = 1;
        pacer->next_us = now_us;
    }
}

void bingpu_pace_frame_end(BgpuPacer* pacer, int64_t end_us, double present_ms) {
    if (!pacer) return;
    pacer->last_present_ms = present_ms;

    const int target = bingpu_pace_policy_fps(static_cast<BgpuPacePolicy>(pacer->policy));
    if (target == 0) {                 /* unlimited：不留基准，下一次 wait 恒为 0 */
        pacer->next_us = 0;
        pacer->steps = 0;
        pacer->last_frame_ms = 0.0;
        return;
    }

    const int64_t gridUs = GridUs(pacer);
    const double gridMs = static_cast<double>(gridUs) / 1000.0;
    const int minSteps = MinSteps(pacer);

    const int64_t startUs = pacer->frame_open ? pacer->frame_start_us : end_us;
    pacer->frame_open = 0;
    const double totalMs = static_cast<double>(end_us - startUs) / 1000.0;
    pacer->last_frame_ms = totalMs;

    /* present 到底有没有自己等垂直同步。这个判据同时决定**相位锚在哪**：
     *   · 等了（vsync 开着）→ present 返回的时刻本身就是刷新边界附近的点，
     *     下一拍从**本拍结束**起算：next = end + (k-1) 格。k=1 就是"立刻再来"，
     *     k=2 就是"跳一个刷新"= 60 fps @120Hz —— 相位天然锁在刷新上。
     *   · 没等 → 只能从**本拍开始**起算：next = start + k 格（至少不早于本拍结束）。
     * 不这么分的后果实测过：从 start 起算 + present 等 vblank 时，两者相位互相漂，
     * 120 fps 档的帧间隔呈双峰（0.78 格和 1.17 格各一半，实测 114 fps 而不是 120）。 */
    const bool presentPaced = gridMs > 0.0 && present_ms >= gridMs * 0.5;

    /* "标准"：**上限是刷新率，下限不设**（2026-11-20 master 的口径）。
     * 画得完就钳在刷新周期上；画不完就直接往下走 —— 一刻都不多等。
     * 所以 9.5 ms 的帧就是 ~105 fps，不会被凑成 2 格（60 fps）：
     * 凑整只对"明确要 60/30"有意义，对"标准"是白扔帧。 */
    if (target < 0) {
        pacer->steps = 1;
        int64_t next = presentPaced ? end_us : startUs + gridUs;
        if (next < end_us) next = end_us;
        pacer->next_us = next;
        return;
    }

    /* 这一拍真正"花掉"了多少（去掉 present 自己等垂直同步的那一段）——
     * 见文件头 ③ 的最后一段。 */
    double costMs = totalMs - (presentPaced ? present_ms : 0.0);
    if (costMs < 0.0) costMs = 0.0;

    /* 本拍至少要占几个网格才装得下 */
    long long need = gridMs > 0.0 ? static_cast<long long>(std::ceil(costMs / gridMs)) : 1;
    if (need < 1) need = 1;

    long long k = pacer->steps > 0 ? pacer->steps : need;
    if (need > k) {
        k = need;                                        /* 装不下立刻升档 */
    } else if (k > minSteps &&
               costMs <= gridMs * static_cast<double>(k - 1) - gridMs * kHysteresis) {
        --k;                                             /* 稳稳装得下才降档 */
    }
    if (k < minSteps) k = minSteps;
    if (k < 1) k = 1;
    if (k > kMaxSteps) k = kMaxSteps;
    pacer->steps = static_cast<int32_t>(k);

    /* 下一拍的两种锚法（见上面 presentPaced 的说明）：
     *   present 自己在等刷新 → 从本拍结束起算，跳 (k-1) 个刷新（k=1 就是立刻）
     *   否则                 → 从本拍开始起算，占满 k 个网格（至少不早于本拍结束）
     * 两者都保证了同一件事：present 已经等掉的时间不会被重复补一遍。 */
    int64_t next;
    if (presentPaced) {
        next = end_us + gridUs * (k - 1);
    } else {
        next = startUs + gridUs * k;
    }
    if (next < end_us) next = end_us;
    pacer->next_us = next;
}

}  // extern "C"
