/* tests/test_pace.c —— 帧节拍器的规格书（纯 C11）
 *
 * 为什么这个测试值得单独存在：
 *   节拍逻辑是那种"错一格就肉眼可见"的东西，但它错起来**不报错、不掉帧、不算回归**
 *   —— 只会稳稳地跑在错误的一半帧率上。0.8.0 就是这么翻车的：外壳靠"present 耗时
 *   ≥ 半个刷新周期"猜"它自己等过没有"，Vulkan 上每拍多睡一格，120 Hz 屏实测 55 fps，
 *   而同一份代码裸跑 present 是 119.9 fps。
 *
 * 所以这里的断言全部是**手算的期望周期**，配一个时间可注入的模拟循环：
 *   模拟里没有真睡、没有 GPU、没有窗口 —— 只有"画多久 / present 多久 / 是否等刷新"，
 *   于是节拍器的行为可以被钉死在数字上。
 *
 * 两个 present 模型：
 *   free  ：present 立刻返回（vsync 关 / 驱动不阻塞），耗时 = 给定值
 *   paced ：present 阻塞到**下一个刷新边界**（vsync 开，DWM 合成下的真实行为）
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bingpu.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (cond) {                                                          \
            g_pass++;                                                        \
        } else {                                                             \
            g_fail++;                                                        \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                    \
    } while (0)

static int near(double got, double want, double tolFrac) {
    const double tol = want * tolFrac;
    return got >= want - tol && got <= want + tol;
}

/* ---------------------------------------------------------------
 * 模拟循环：返回实测 fps（用帧数-1 个间隔，避开首拍锚定）
 * --------------------------------------------------------------- */
static double sim_fps(int refresh_hz, BgpuPacePolicy policy, double draw_ms,
                      double present_ms, int frames, int paced_model) {
    BgpuPacer p;
    bingpu_pace_init(&p, 0);
    p.refresh_hz = refresh_hz;
    bingpu_pace_set_policy(&p, policy);

    const double grid_ms = 1000.0 / (double)refresh_hz;
    int64_t now = 0;          /* 微秒；调用方的时钟 */
    int64_t first_start = 0;
    double sum_interval_ms = 0.0;
    int64_t prev_start = 0;

    for (int i = 0; i < frames; ++i) {
        const int64_t wait = bingpu_pace_wait_us(&p, now);
        if (wait > 0) now += wait;
        const int64_t start = now;
        bingpu_pace_frame_begin(&p, start);

        /* 画 */
        now += (int64_t)(draw_ms * 1000.0 + 0.5);

        /* 呈现 */
        double pm = present_ms;
        if (paced_model) {
            /* 阻塞到下一个刷新边界 */
            const double now_ms = (double)now / 1000.0;
            const double next_boundary = ((double)(int64_t)(now_ms / grid_ms) + 1.0) * grid_ms;
            pm = next_boundary - now_ms;
            if (pm < 0.0) pm = 0.0;
        }
        now += (int64_t)(pm * 1000.0 + 0.5);
        bingpu_pace_frame_end(&p, now, pm);

        if (i == 0) first_start = start;
        else sum_interval_ms += (double)(start - prev_start) / 1000.0;
        prev_start = start;
    }
    (void)first_start;
    if (frames <= 1) return 0.0;
    return (double)(frames - 1) / (sum_interval_ms / 1000.0);
}

/* 模拟里最大的帧间隔（看有没有 2/3 档交替的尖刺） */
static double sim_max_interval_ms(int refresh_hz, BgpuPacePolicy policy, double draw_ms,
                                  double present_ms, int frames, int paced_model) {
    BgpuPacer p;
    bingpu_pace_init(&p, 0);
    p.refresh_hz = refresh_hz;
    bingpu_pace_set_policy(&p, policy);
    const double grid_ms = 1000.0 / (double)refresh_hz;
    int64_t now = 0, prev_start = 0;
    double worst = 0.0;
    for (int i = 0; i < frames; ++i) {
        const int64_t wait = bingpu_pace_wait_us(&p, now);
        if (wait > 0) now += wait;
        const int64_t start = now;
        bingpu_pace_frame_begin(&p, start);
        now += (int64_t)(draw_ms * 1000.0 + 0.5);
        double pm = present_ms;
        if (paced_model) {
            const double now_ms = (double)now / 1000.0;
            const double next_boundary = ((double)(int64_t)(now_ms / grid_ms) + 1.0) * grid_ms;
            pm = next_boundary - now_ms;
            if (pm < 0.0) pm = 0.0;
        }
        now += (int64_t)(pm * 1000.0 + 0.5);
        bingpu_pace_frame_end(&p, now, pm);
        if (i > 2) {
            const double iv = (double)(start - prev_start) / 1000.0;
            if (iv > worst) worst = iv;
        }
        prev_start = start;
    }
    return worst;
}

static void test_policy_table(void) {
    BgpuPacePolicy pol = BGPU_PACE_STANDARD;
    CHECK(bingpu_pace_policy_parse("standard", &pol) == 1 && pol == BGPU_PACE_STANDARD);
    CHECK(bingpu_pace_policy_parse("Standard", &pol) == 1 && pol == BGPU_PACE_STANDARD);
    CHECK(bingpu_pace_policy_parse("unlimited", &pol) == 1 && pol == BGPU_PACE_UNLIMITED);
    CHECK(bingpu_pace_policy_parse("0", &pol) == 1 && pol == BGPU_PACE_UNLIMITED);
    CHECK(bingpu_pace_policy_parse("240", &pol) == 1 && pol == BGPU_PACE_240);
    CHECK(bingpu_pace_policy_parse("120", &pol) == 1 && pol == BGPU_PACE_120);
    CHECK(bingpu_pace_policy_parse("90", &pol) == 1 && pol == BGPU_PACE_90);
    CHECK(bingpu_pace_policy_parse("60", &pol) == 1 && pol == BGPU_PACE_60);
    CHECK(bingpu_pace_policy_parse("30", &pol) == 1 && pol == BGPU_PACE_30);
    CHECK(bingpu_pace_policy_parse("", &pol) == 0);
    CHECK(bingpu_pace_policy_parse("abc", &pol) == 0);
    CHECK(bingpu_pace_policy_parse(0, &pol) == 0);
    CHECK(bingpu_pace_policy_parse("60", 0) == 0);

    CHECK(bingpu_pace_policy_fps(BGPU_PACE_UNLIMITED) == 0);
    CHECK(bingpu_pace_policy_fps(BGPU_PACE_60) == 60);
    CHECK(bingpu_pace_policy_fps(BGPU_PACE_STANDARD) < 0);   /* 跟刷新率 */
    for (int i = 0; i <= BGPU_PACE_30; ++i) {
        const char* n = bingpu_pace_policy_name((BgpuPacePolicy)i);
        CHECK(n && n[0] && strcmp(n, "unknown") != 0);
    }
}

static void test_effective_period(void) {
    BgpuPacer p;
    bingpu_pace_init(&p, 0);
    p.refresh_hz = 120;

    CHECK(near(bingpu_pace_frame_ms(&p), 1000.0 / 120.0, 0.01));   /* standard */

    bingpu_pace_set_policy(&p, BGPU_PACE_60);
    CHECK(near(bingpu_pace_frame_ms(&p), 1000.0 / 60.0, 0.01));

    bingpu_pace_set_policy(&p, BGPU_PACE_30);
    CHECK(near(bingpu_pace_frame_ms(&p), 1000.0 / 30.0, 0.01));

    /* 240 > 120：网格跟着目标走，否则"240"会被刷新网格钉成 120 */
    bingpu_pace_set_policy(&p, BGPU_PACE_240);
    CHECK(near(bingpu_pace_frame_ms(&p), 1000.0 / 240.0, 0.02));

    /* 90 落在 120 Hz 屏上：只能取整数个网格 → 实际是 120 fps。
     * 喊出来的数字必须是**生效**的那个（不许声称 90 却跑 120，也不许跑 60）。 */
    bingpu_pace_set_policy(&p, BGPU_PACE_90);
    CHECK(near(bingpu_pace_frame_ms(&p), 1000.0 / 120.0, 0.01));

    bingpu_pace_set_policy(&p, BGPU_PACE_UNLIMITED);
    CHECK(bingpu_pace_frame_ms(&p) == 0.0);

    /* 60 Hz 屏 */
    p.refresh_hz = 60;
    bingpu_pace_set_policy(&p, BGPU_PACE_STANDARD);
    CHECK(near(bingpu_pace_frame_ms(&p), 1000.0 / 60.0, 0.01));
    bingpu_pace_set_policy(&p, BGPU_PACE_120);
    CHECK(near(bingpu_pace_frame_ms(&p), 1000.0 / 120.0, 0.02));   /* 要 120 就得比刷新细 */
}

static void test_wants_vsync(void) {
    BgpuPacer p;
    bingpu_pace_init(&p, 0);
    p.refresh_hz = 120;
    CHECK(bingpu_pace_wants_vsync(&p) == 1);                       /* standard */
    bingpu_pace_set_policy(&p, BGPU_PACE_60);
    CHECK(bingpu_pace_wants_vsync(&p) == 1);
    bingpu_pace_set_policy(&p, BGPU_PACE_120);
    CHECK(bingpu_pace_wants_vsync(&p) == 1);
    bingpu_pace_set_policy(&p, BGPU_PACE_240);
    CHECK(bingpu_pace_wants_vsync(&p) == 0);                       /* 不关就是空话 */
    bingpu_pace_set_policy(&p, BGPU_PACE_UNLIMITED);
    CHECK(bingpu_pace_wants_vsync(&p) == 0);
}

static void test_invalid_policy(void) {
    BgpuPacer p;
    bingpu_pace_init(&p, 0);
    CHECK(bingpu_pace_set_policy(&p, (BgpuPacePolicy)99) != BGPU_OK);
    CHECK(bingpu_pace_set_policy(0, BGPU_PACE_60) != BGPU_OK);
    BgpuPacer bad;
    memset(&bad, 0, sizeof(bad));          /* struct_size 没填 */
    CHECK(bingpu_pace_set_policy(&bad, BGPU_PACE_60) != BGPU_OK);
    /* 失败不该改掉原策略 */
    CHECK(bingpu_pace_policy(&p) == BGPU_PACE_STANDARD);
}

/* ---- 实测帧率（模拟）：每条断言都是手算的期望值 ---- */
static void test_achieved_fps(void) {
    /* 1) standard @120，present 不等刷新（vsync 关）：应该是 120 fps */
    CHECK(near(sim_fps(120, BGPU_PACE_STANDARD, 1.0, 0.5, 60, 0), 120.0, 0.06));

    /* 2) **回归**：standard @120，present 阻塞到刷新边界。
     *    旧实现这里会 60 fps（present 等一格 + 外壳再睡一格 = 双重等待）。 */
    {
        const double fps = sim_fps(120, BGPU_PACE_STANDARD, 1.0, 0.0, 60, 1);
        CHECK(near(fps, 120.0, 0.08));
        CHECK(fps > 100.0);            /* 明确钉死"不许掉到 60" */
    }

    /* 3) **"标准"的下限不设**（2026-11-20 master 口径）：本拍装不下一格
     *    （9 ms 画 + 0.5 ms present > 8.33 ms）时**尽力多画**，不许凑成 2 格（60 fps）。
     *    期望 = 1000/9.5 ≈ 105 fps。 */
    {
        const double fps = sim_fps(120, BGPU_PACE_STANDARD, 9.0, 0.5, 60, 0);
        CHECK(near(fps, 1000.0 / 9.5, 0.10));
        CHECK(fps > 95.0);       /* 明确钉死"不许退成 60" */
        CHECK(fps < 120.0);      /* 上限仍然是刷新率 */
        /* 也不许出现"2 格"那种 16.7 ms 的长间隔 */
        const double worst = sim_max_interval_ms(120, BGPU_PACE_STANDARD, 9.0, 0.5, 60, 0);
        CHECK(worst <= 9.5 * 1.35);
    }

    /* 3b) 明确要 60/30 时才凑整：60 fps 目标下 20 ms 的帧 → 3 格（25 ms，40 fps），
     *     因为"整数个刷新周期"是这条策略自己给的约束。 */
    CHECK(near(sim_fps(120, BGPU_PACE_60, 20.0, 0.5, 60, 0), 40.0, 0.10));

    /* 4) 60 fps @120：paced / free 两条路都必须落在 60 */
    CHECK(near(sim_fps(120, BGPU_PACE_60, 1.0, 0.0, 60, 1), 60.0, 0.08));
    CHECK(near(sim_fps(120, BGPU_PACE_60, 1.0, 0.5, 60, 0), 60.0, 0.08));

    /* 5) 30 fps @120 */
    CHECK(near(sim_fps(120, BGPU_PACE_30, 1.0, 0.0, 60, 1), 30.0, 0.08));
    CHECK(near(sim_fps(120, BGPU_PACE_30, 1.0, 0.5, 60, 0), 30.0, 0.08));

    /* 6) 240 fps @120（比刷新快 —— 网格跟着目标，vsync 必须关） */
    CHECK(near(sim_fps(120, BGPU_PACE_240, 1.0, 0.5, 120, 0), 240.0, 0.10));

    /* 7) unlimited：能画多快画多快。1.0 + 0.5 = 1.5 ms → 666 fps */
    {
        const double fps = sim_fps(120, BGPU_PACE_UNLIMITED, 1.0, 0.5, 200, 0);
        CHECK(near(fps, 1000.0 / 1.5, 0.06));
        CHECK(fps > 600.0);
    }

    /* 8) 60 Hz 屏上的 standard */
    CHECK(near(sim_fps(60, BGPU_PACE_STANDARD, 1.0, 0.0, 60, 1), 60.0, 0.08));
}

static void test_unlimited_never_waits(void) {
    BgpuPacer p;
    bingpu_pace_init(&p, 0);
    p.refresh_hz = 120;
    CHECK(bingpu_pace_set_policy(&p, BGPU_PACE_UNLIMITED) == BGPU_OK);
    bingpu_pace_frame_begin(&p, 0);
    bingpu_pace_frame_end(&p, 1000, 300.0);      /* 假装 present 花了 300 ms */
    CHECK(bingpu_pace_wait_us(&p, 1000) == 0);
    CHECK(bingpu_pace_wait_us(&p, 5000) == 0);
}

static void test_policy_change_relearns(void) {
    BgpuPacer p;
    bingpu_pace_init(&p, 0);
    p.refresh_hz = 120;
    /* 先在 30 fps 下学出 k=4 */
    CHECK(bingpu_pace_set_policy(&p, BGPU_PACE_30) == BGPU_OK);
    bingpu_pace_frame_begin(&p, 0);
    bingpu_pace_frame_end(&p, 33000, 0.5);
    CHECK(p.steps >= 4);
    /* 换成 standard 必须丢相位，否则头几拍会拿 k=4 去套 */
    CHECK(bingpu_pace_set_policy(&p, BGPU_PACE_STANDARD) == BGPU_OK);
    CHECK(p.steps == 0);
    CHECK(p.started == 0);
    /* 同一个策略重复设置不该清状态（否则每帧设一次就等于每帧重学） */
    bingpu_pace_frame_begin(&p, 100000);
    bingpu_pace_frame_end(&p, 101000, 0.5);
    CHECK(p.started == 1);
    CHECK(bingpu_pace_set_policy(&p, BGPU_PACE_STANDARD) == BGPU_OK);
    CHECK(p.started == 1);
}

static void test_frame_end_clamps_to_now(void) {
    /* present 自己等了一整格：下一拍就是"现在"，不许再补一格 */
    BgpuPacer p;
    bingpu_pace_init(&p, 0);
    p.refresh_hz = 120;
    bingpu_pace_set_policy(&p, BGPU_PACE_STANDARD);
    bingpu_pace_frame_begin(&p, 0);
    const int64_t end_us = 9400;             /* 画 1.1 ms + present 等 8.3 ms */
    bingpu_pace_frame_end(&p, end_us, 8.3);
    CHECK(bingpu_pace_wait_us(&p, end_us) == 0);
    CHECK(p.next_us == end_us);              /* max(start+grid, end) = end */
    CHECK(p.steps == 1);                     /* 标准永远是 1 格，不学档位 */
}

/* "标准"画得完时仍然钳在刷新周期上（不许跑到 240 fps 去烧电） */
static void test_standard_still_caps(void) {
    BgpuPacer p;
    bingpu_pace_init(&p, 0);
    p.refresh_hz = 120;
    bingpu_pace_set_policy(&p, BGPU_PACE_STANDARD);
    bingpu_pace_frame_begin(&p, 1000);
    bingpu_pace_frame_end(&p, 2000, 0.2);    /* 一帧只要 1 ms */
    CHECK(p.next_us == 1000 + 1000000 / 120);   /* 下一拍 = 开始 + 一格，不是"立刻" */
    CHECK(bingpu_pace_wait_us(&p, 2000) > 6000);
}

int main(void) {
    printf("[test_pace] 帧节拍器规格\n");
    test_policy_table();
    test_effective_period();
    test_wants_vsync();
    test_invalid_policy();
    test_achieved_fps();
    test_unlimited_never_waits();
    test_policy_change_relearns();
    test_frame_end_clamps_to_now();
    test_standard_still_caps();
    printf("[test_pace] pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
