/* tests/test_abi.c —— 公共 ABI 的契约测试（纯 C11，不碰后端像素）
 *
 * 这里钉的是"绑定作者会踩的坑"：
 *   · struct_size 协议（填小了必须被拒绝，不是越界写）；
 *   · 0 / 已销毁 / 别的设备的句柄都必须是 BGPU_ERR_INVALID_HANDLE；
 *   · 状态机误用的结果码（未 begin_render 就 draw 之类）；
 *   · last_error 一定非空、一定带函数名和 file:line；
 *   · 错误槽是线程本地的（两个线程不串台）；
 *   · destroy / shutdown 幂等（清理路径永远不许把程序打断）。
 * 这些结论一旦成立，上层（bin2d/bin3d/binUI）就可以放心地在错误处理里相信它们。
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bingpu.h"

#ifdef _WIN32
#include <windows.h>
#endif

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

#define CHECK_EQ(a, b)                                                       \
    do {                                                                     \
        const long long va = (long long)(a), vb = (long long)(b);            \
        if (va == vb) {                                                      \
            g_pass++;                                                        \
        } else {                                                             \
            g_fail++;                                                        \
            printf("  FAIL %s:%d: %s(%lld) != %s(%lld)\n", __FILE__, __LINE__, \
                   #a, va, #b, vb);                                          \
        }                                                                    \
    } while (0)

static int err_has(const char* needle) {
    const char* e = bingpu_last_error();
    return e && needle && strstr(e, needle) != NULL;
}

/* 一条"失败后必须留下可用错误串"的通用断言：非空、带函数名、带 file:line */
static void check_error_shape(const char* fn_name) {
    const char* e = bingpu_last_error();
    CHECK(e != NULL);
    if (!e) return;
    CHECK(e[0] != '\0');
    CHECK(strstr(e, fn_name) != NULL);
    CHECK(strstr(e, "(") != NULL);
    CHECK(strstr(e, ")") != NULL);
}

/* ============================================================
 * 1) 版本 / 尺寸 / 枚举
 * ============================================================ */
static void test_version(void) {
    printf("[version]\n");
    CHECK_EQ(BINGPU_VERSION_MAJOR, 0);
    CHECK_EQ(BINGPU_VERSION_MINOR, 6);   /* 0.6.0：光照 + 透视校正（ABI 3） */
    CHECK_EQ(BINGPU_VERSION_PATCH, 0);
    CHECK(strcmp(bingpu_version_string(), BINGPU_VERSION_STRING) == 0);
    CHECK(strcmp(bingpu_version_string(), "0.6.0") == 0);
    CHECK_EQ(bingpu_abi_version(), BINGPU_ABI_VERSION);
    /* ABI 3 = BgpuMeshVertex 加 w（透视校正）+ BgpuLight + 两个 cap 位。
     * 2D 路径（BgpuVertex / draw_quads）一个字节没动 —— 那一行的断言仍然钉 24。
     * 枚举值全部是追加，旧二进制调用方不受影响。 */
    CHECK_EQ(BINGPU_ABI_VERSION, 3u);
    /* 结构体布局是 ABI 的一部分（头里也 static_assert 了，这里再钉一次） */
    CHECK_EQ(sizeof(BgpuVertex), 24u);
    CHECK_EQ(sizeof(BgpuMeshVertex), 40u);
    CHECK_EQ(sizeof(BgpuLight), 36u);   /* 3 个 float[3] */
    CHECK_EQ(sizeof(BgpuDevice), 8u);
    CHECK(sizeof(BgpuDeviceDesc) >= sizeof(uint32_t) * 3);
}

static void test_result_names(void) {
    printf("[result_name]\n");
    const BgpuResult all[] = {
        BGPU_OK,          BGPU_ERR_INVALID_ARGUMENT, BGPU_ERR_INVALID_HANDLE,
        BGPU_ERR_UNSUPPORTED, BGPU_ERR_OUT_OF_MEMORY, BGPU_ERR_DEVICE_LOST,
        BGPU_ERR_STATE,   BGPU_ERR_BACKEND_FAILED,   BGPU_ERR_NOT_READY,
        BGPU_ERR_LIMIT,
    };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        const char* n = bingpu_result_name(all[i]);
        CHECK(n != NULL);
        CHECK(strncmp(n, "BGPU_", 5) == 0);
    }
    CHECK(strcmp(bingpu_result_name(BGPU_OK), "BGPU_OK") == 0);
    CHECK(strcmp(bingpu_result_name(BGPU_ERR_STATE), "BGPU_ERR_STATE") == 0);
    /* 越界枚举必须给一个名字，不能返回 NULL（绑定会把它当 C 字符串用） */
    CHECK(bingpu_result_name((BgpuResult)999) != NULL);
}

static void test_format_bytes(void) {
    printf("[format_bytes]\n");
    CHECK_EQ(bingpu_format_bytes(BGPU_FORMAT_RGBA8), 4);
    CHECK_EQ(bingpu_format_bytes(BGPU_FORMAT_BGRA8), 4);
    CHECK_EQ(bingpu_format_bytes(BGPU_FORMAT_R8), 1);
    CHECK_EQ(bingpu_format_bytes((BgpuFormat)42), 0);
}

/* ============================================================
 * 2) 后端注册表
 * ============================================================ */
static void test_backends(void) {
    printf("[backends]\n");
    CHECK_EQ(bingpu_backend_count(), 6u);
    const BgpuBackendKind expect[6] = {BGPU_BACKEND_VULKAN, BGPU_BACKEND_D3D11, BGPU_BACKEND_D3D12,
                                       BGPU_BACKEND_GL, BGPU_BACKEND_CPU, BGPU_BACKEND_NULL};
    for (uint32_t i = 0; i < 6; i++) {
        CHECK(bingpu_backend_at(i) == expect[i]);
        CHECK(bingpu_backend_name(bingpu_backend_at(i)) != NULL);
        CHECK(bingpu_backend_name(bingpu_backend_at(i))[0] != '\0');
    }
    /* CPU/NULL 永远可用；Vulkan（0.3.0）/ D3D11（0.4.0）/ GL（0.5.0）起是真后端 ——
     * 能不能用**取决于机器**（驱动/物理设备），所以这里只断言"是合法的 0/1"，具体能力由
     * tests/test_{vk,d3d11,gl}_render.c 在有卡时验证。D3D12 仍登记但未实现。 */
    CHECK_EQ(bingpu_backend_available(BGPU_BACKEND_CPU), 1);
    CHECK_EQ(bingpu_backend_available(BGPU_BACKEND_NULL), 1);
    {
        const int vk = bingpu_backend_available(BGPU_BACKEND_VULKAN);
        CHECK(vk == 0 || vk == 1);
    }
    {
        const int d3d = bingpu_backend_available(BGPU_BACKEND_D3D11);
        CHECK(d3d == 0 || d3d == 1);
    }
    {
        const int gl = bingpu_backend_available(BGPU_BACKEND_GL);
        CHECK(gl == 0 || gl == 1);
    }
    CHECK_EQ(bingpu_backend_available(BGPU_BACKEND_D3D12), 0);
    CHECK_EQ(bingpu_backend_available(BGPU_BACKEND_AUTO), 0);
    CHECK(strcmp(bingpu_backend_name(BGPU_BACKEND_AUTO), "auto") == 0);
    CHECK(strcmp(bingpu_backend_name((BgpuBackendKind)77), "unknown") == 0);
    /* 越界索引：写错误槽，返回 AUTO 作为哨兵 */
    bingpu_clear_error();
    CHECK(bingpu_backend_at(99) == BGPU_BACKEND_AUTO);
    check_error_shape("bingpu_backend_at");
}

/* ============================================================
 * 3) struct_size / 参数校验
 * ============================================================ */
static void test_struct_size(void) {
    printf("[struct_size]\n");
    BgpuDevice device = BGPU_INVALID;
    BgpuDeviceDesc bad;
    memset(&bad, 0, sizeof(bad));
    bad.struct_size = 0;
    bad.backend = BGPU_BACKEND_NULL;
    bad.width = 16;
    bad.height = 16;
    CHECK(bingpu_device_create(&bad, &device) == BGPU_ERR_INVALID_ARGUMENT);
    CHECK(device == BGPU_INVALID);
    check_error_shape("bingpu_device_create");

    CHECK(bingpu_device_create(NULL, &device) == BGPU_ERR_INVALID_ARGUMENT);

    /* 正常建一个 NULL 设备，然后拿各种"太小的输出结构体"去问 */
    BgpuDeviceDesc d;
    memset(&d, 0, sizeof(d));
    d.struct_size = (uint32_t)sizeof(d);
    d.backend = BGPU_BACKEND_NULL;
    d.width = 16;
    d.height = 16;
    d.flags = BGPU_DEVICE_HEADLESS;
    CHECK(bingpu_device_create(&d, &device) == BGPU_OK);
    CHECK(device != BGPU_INVALID);

    BgpuDeviceInfo info;
    memset(&info, 0, sizeof(info));
    info.struct_size = 4;
    CHECK(bingpu_device_info(device, &info) == BGPU_ERR_INVALID_ARGUMENT);
    check_error_shape("bingpu_device_info");

    BgpuCaps caps;
    memset(&caps, 0, sizeof(caps));
    caps.struct_size = 0;
    CHECK(bingpu_device_caps(device, &caps) == BGPU_ERR_INVALID_ARGUMENT);

    BgpuStats stats;
    memset(&stats, 0, sizeof(stats));
    stats.struct_size = (uint32_t)sizeof(stats);
    CHECK(bingpu_device_stats(device, &stats) == BGPU_OK);

    BgpuEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.struct_size = (uint32_t)sizeof(ev);
    CHECK(bingpu_poll(device, &ev) == BGPU_OK);
    CHECK_EQ(ev.kind, BGPU_EVENT_NONE);

    CHECK_EQ(bingpu_device_lost(device), 0);
    CHECK(bingpu_device_wait_idle(device) == BGPU_OK);

    /* AUTO 的语义：按注册表顺序选第一个可用后端（现在 Vulkan 优先，没有就 CPU）。
     * 这里显式用 CPU 做后续的"CPU 不接受窗口"断言，AUTO 的结果只断言"落在可用后端里"。 */
    {
        BgpuDevice auto_dev = BGPU_INVALID;
        BgpuDeviceDesc ad2;
        memset(&ad2, 0, sizeof(ad2));
        ad2.struct_size = (uint32_t)sizeof(ad2);
        ad2.backend = BGPU_BACKEND_AUTO;
        ad2.width = 8;
        ad2.height = 8;
        ad2.flags = BGPU_DEVICE_HEADLESS;
        CHECK(bingpu_device_create(&ad2, &auto_dev) == BGPU_OK);
        BgpuDeviceInfo ai2;
        memset(&ai2, 0, sizeof(ai2));
        ai2.struct_size = (uint32_t)sizeof(ai2);
        CHECK(bingpu_device_info(auto_dev, &ai2) == BGPU_OK);
        CHECK(ai2.backend == BGPU_BACKEND_CPU || ai2.backend == BGPU_BACKEND_VULKAN ||
              ai2.backend == BGPU_BACKEND_D3D11 || ai2.backend == BGPU_BACKEND_GL);
        bingpu_device_destroy(auto_dev);
    }
    BgpuDevice auto_dev = BGPU_INVALID;
    BgpuDeviceDesc ad;
    memset(&ad, 0, sizeof(ad));
    ad.struct_size = (uint32_t)sizeof(ad);
    ad.backend = BGPU_BACKEND_CPU;
    ad.width = 8;
    ad.height = 8;
    CHECK(bingpu_device_create(&ad, &auto_dev) == BGPU_OK);
    BgpuDeviceInfo ai;
    memset(&ai, 0, sizeof(ai));
    ai.struct_size = (uint32_t)sizeof(ai);
    CHECK(bingpu_device_info(auto_dev, &ai) == BGPU_OK);
    CHECK_EQ(ai.backend, BGPU_BACKEND_CPU);
    /* CPU 后端不接受窗口：窗口呈现是 VK/D3D12 的事 */
    BgpuSwapchain window_sc = BGPU_INVALID;
    BgpuSwapchainDesc wd;
    memset(&wd, 0, sizeof(wd));
    wd.struct_size = (uint32_t)sizeof(wd);
    wd.native_window = (void*)(uintptr_t)1;
    wd.width = 8;
    wd.height = 8;
    wd.format = BGPU_FORMAT_RGBA8;
    CHECK(bingpu_swapchain_create(auto_dev, &wd, &window_sc) == BGPU_ERR_UNSUPPORTED);
    check_error_shape("bingpu_swapchain_create");
    CHECK(window_sc == BGPU_INVALID);

    /* 未实现的后端（D3D12）：UNSUPPORTED + 明确原因 */
    bingpu_clear_error();
    BgpuDevice d3d_dev = BGPU_INVALID;
    BgpuDeviceDesc vd;
    memset(&vd, 0, sizeof(vd));
    vd.struct_size = (uint32_t)sizeof(vd);
    vd.backend = BGPU_BACKEND_D3D12;
    vd.width = 8;
    vd.height = 8;
    CHECK(bingpu_device_create(&vd, &d3d_dev) == BGPU_ERR_UNSUPPORTED);
    check_error_shape("bingpu_device_create");
    CHECK(err_has("未实现") || err_has("d3d12") || err_has("D3D12"));

    bingpu_device_destroy(auto_dev);
    bingpu_device_destroy(device);
}

/* ============================================================
 * 4) 无效句柄 / 已销毁句柄
 * ============================================================ */
static void test_handles(void) {
    printf("[handles]\n");
    BgpuDevice device = BGPU_INVALID;
    BgpuDeviceDesc d;
    memset(&d, 0, sizeof(d));
    d.struct_size = (uint32_t)sizeof(d);
    d.backend = BGPU_BACKEND_NULL;
    d.width = 32;
    d.height = 32;
    CHECK(bingpu_device_create(&d, &device) == BGPU_OK);

    /* 0 句柄 */
    BgpuDeviceInfo info;
    memset(&info, 0, sizeof(info));
    info.struct_size = (uint32_t)sizeof(info);
    CHECK(bingpu_device_info(BGPU_INVALID, &info) == BGPU_ERR_INVALID_HANDLE);
    check_error_shape("bingpu_device_info");

    BgpuTexture tex = BGPU_INVALID;
    BgpuTextureDesc td;
    memset(&td, 0, sizeof(td));
    td.struct_size = (uint32_t)sizeof(td);
    td.width = 4;
    td.height = 4;
    td.format = BGPU_FORMAT_RGBA8;
    CHECK(bingpu_texture_create(BGPU_INVALID, &td, &tex) == BGPU_ERR_INVALID_HANDLE);
    CHECK(tex == BGPU_INVALID);
    CHECK(bingpu_texture_create(device, &td, &tex) == BGPU_OK);

    /* 尺寸 0 / 超上限 */
    BgpuTextureDesc bad = td;
    bad.width = 0;
    BgpuTexture t2 = BGPU_INVALID;
    CHECK(bingpu_texture_create(device, &bad, &t2) == BGPU_ERR_INVALID_ARGUMENT);
    bad = td;
    bad.width = 100000;
    CHECK(bingpu_texture_create(device, &bad, &t2) == BGPU_ERR_LIMIT);
    /* 非法格式 */
    bad = td;
    bad.format = (BgpuFormat)42;
    CHECK(bingpu_texture_create(device, &bad, &t2) == BGPU_ERR_INVALID_ARGUMENT);

    /* 种类串用：拿纹理句柄当设备用 */
    CHECK(bingpu_device_info(tex, &info) == BGPU_ERR_INVALID_HANDLE);

    /* 销毁后立刻失效 */
    bingpu_texture_destroy(device, tex);
    BgpuTextureInfo ti;
    memset(&ti, 0, sizeof(ti));
    ti.struct_size = (uint32_t)sizeof(ti);
    CHECK(bingpu_texture_info(tex, &ti) == BGPU_ERR_INVALID_HANDLE);
    /* 重复销毁是 no-op */
    bingpu_texture_destroy(device, tex);
    CHECK_EQ(1, 1);

    /* 两个设备之间的资源不能混用 */
    BgpuDevice dev2 = BGPU_INVALID;
    CHECK(bingpu_device_create(&d, &dev2) == BGPU_OK);
    BgpuTexture tex_a = BGPU_INVALID;
    CHECK(bingpu_texture_create(device, &td, &tex_a) == BGPU_OK);
    BgpuCmd cmd_b = BGPU_INVALID;
    CHECK(bingpu_cmd_create(dev2, &cmd_b) == BGPU_OK);
    BgpuRenderBeginDesc rb;
    memset(&rb, 0, sizeof(rb));
    rb.struct_size = (uint32_t)sizeof(rb);
    rb.target = tex_a;
    CHECK(bingpu_cmd_begin_render(cmd_b, &rb) == BGPU_ERR_INVALID_HANDLE);
    check_error_shape("bingpu_cmd_begin_render");

    /* 设备销毁连带清理：之后的子句柄全部失效 */
    bingpu_device_destroy(device);
    CHECK(bingpu_texture_info(tex_a, &ti) == BGPU_ERR_INVALID_HANDLE);
    bingpu_device_destroy(device);   /* 幂等 */
    bingpu_device_destroy(dev2);
    CHECK_EQ(1, 1);
}

/* ============================================================
 * 5) 状态机（NULL 后端：只校验 + 计数）
 * ============================================================ */
static void test_state_machine(void) {
    printf("[state_machine]\n");
    BgpuDevice device = BGPU_INVALID;
    BgpuDeviceDesc d;
    memset(&d, 0, sizeof(d));
    d.struct_size = (uint32_t)sizeof(d);
    d.backend = BGPU_BACKEND_NULL;
    d.width = 64;
    d.height = 64;
    d.flags = BGPU_DEVICE_HEADLESS;
    CHECK(bingpu_device_create(&d, &device) == BGPU_OK);

    BgpuSwapchain sc = BGPU_INVALID;
    BgpuSwapchainDesc sd;
    memset(&sd, 0, sizeof(sd));
    sd.struct_size = (uint32_t)sizeof(sd);
    sd.width = 64;
    sd.height = 64;
    sd.format = BGPU_FORMAT_RGBA8;
    CHECK(bingpu_swapchain_create(device, &sd, &sc) == BGPU_OK);

    BgpuCmd cmd = BGPU_INVALID;
    CHECK(bingpu_cmd_create(device, &cmd) == BGPU_OK);

    BgpuTexture target = BGPU_INVALID;
    BgpuTexture rt = BGPU_INVALID;
    BgpuTextureDesc td;
    memset(&td, 0, sizeof(td));
    td.struct_size = (uint32_t)sizeof(td);
    td.width = 64;
    td.height = 64;
    td.format = BGPU_FORMAT_RGBA8;
    td.flags = BGPU_TEXTURE_RENDER_TARGET;
    CHECK(bingpu_texture_create(device, &td, &rt) == BGPU_OK);

    BgpuPipeline pipe = BGPU_INVALID;
    BgpuPipelineDesc pd;
    memset(&pd, 0, sizeof(pd));
    pd.struct_size = (uint32_t)sizeof(pd);
    pd.vertex_layout = BGPU_VERTEX_2D;
    pd.blend = BGPU_BLEND_PREMULTIPLIED;
    pd.flags = BGPU_PIPE_TEXTURED;
    CHECK(bingpu_pipeline_create(device, &pd, &pipe) == BGPU_OK);
    pd.blend = (BgpuBlend)9;
    BgpuPipeline badpipe = BGPU_INVALID;
    CHECK(bingpu_pipeline_create(device, &pd, &badpipe) == BGPU_ERR_INVALID_ARGUMENT);

    /* frame_end 之前必须先 frame_begin */
    CHECK(bingpu_frame_end(sc, cmd) == BGPU_ERR_STATE);
    check_error_shape("bingpu_frame_end");

    CHECK(bingpu_frame_begin(sc, cmd, &target) == BGPU_OK);
    CHECK(target != BGPU_INVALID);
    /* 重复 frame_begin */
    BgpuTexture t2 = BGPU_INVALID;
    CHECK(bingpu_frame_begin(sc, cmd, &t2) == BGPU_ERR_STATE);

    /* 未 begin_render 就 draw / end_render */
    BgpuVertex quad[4];
    memset(quad, 0, sizeof(quad));
    CHECK(bingpu_cmd_draw_quads(cmd, quad, 1) == BGPU_ERR_STATE);
    check_error_shape("bingpu_cmd_draw_quads");
    CHECK(bingpu_cmd_end_render(cmd) == BGPU_ERR_STATE);
    check_error_shape("bingpu_cmd_end_render");

    BgpuRenderBeginDesc rb;
    memset(&rb, 0, sizeof(rb));
    rb.struct_size = (uint32_t)sizeof(rb);
    rb.target = rt;
    rb.flags = BGPU_RENDER_CLEAR_COLOR;
    rb.clear_color = 0xFF102030u;
    CHECK(bingpu_cmd_begin_render(cmd, &rb) == BGPU_OK);
    /* 重复 begin_render */
    CHECK(bingpu_cmd_begin_render(cmd, &rb) == BGPU_ERR_STATE);
    /* 没有 set_pipeline 就 draw */
    CHECK(bingpu_cmd_draw_quads(cmd, quad, 1) == BGPU_ERR_STATE);
    CHECK(bingpu_cmd_set_pipeline(cmd, pipe) == BGPU_OK);
    CHECK(bingpu_cmd_draw_quads(cmd, quad, 2) == BGPU_OK);
    /* 空绘制合法 */
    CHECK(bingpu_cmd_draw_quads(cmd, NULL, 0) == BGPU_OK);
    CHECK(bingpu_cmd_end_render(cmd) == BGPU_OK);
    /* frame 里还在 render 就 frame_end */
    CHECK(bingpu_cmd_begin_render(cmd, &rb) == BGPU_OK);
    CHECK(bingpu_frame_end(sc, cmd) == BGPU_ERR_STATE);
    CHECK(bingpu_cmd_end_render(cmd) == BGPU_OK);
    CHECK(bingpu_frame_end(sc, cmd) == BGPU_OK);

    /* draw_indexed 需要 MESH 管线，偏移必须是字节且对齐 */
    BgpuPipeline mesh_pipe = BGPU_INVALID;
    pd.blend = BGPU_BLEND_PREMULTIPLIED;
    pd.vertex_layout = BGPU_VERTEX_MESH;
    pd.flags = 0;
    CHECK(bingpu_pipeline_create(device, &pd, &mesh_pipe) == BGPU_OK);
    BgpuBuffer vb = BGPU_INVALID;
    BgpuBufferDesc bd;
    memset(&bd, 0, sizeof(bd));
    bd.struct_size = (uint32_t)sizeof(bd);
    bd.size = (uint32_t)sizeof(BgpuMeshVertex) * 4u;
    bd.usage = BGPU_BUFFER_VERTEX;
    CHECK(bingpu_buffer_create(device, &bd, &vb) == BGPU_OK);
    BgpuBuffer ib = BGPU_INVALID;
    bd.size = 6u * 4u;
    bd.usage = BGPU_BUFFER_INDEX;
    CHECK(bingpu_buffer_create(device, &bd, &ib) == BGPU_OK);
    uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
    CHECK(bingpu_buffer_update(ib, 0, idx, sizeof(idx)) == BGPU_OK);
    CHECK(bingpu_buffer_update(ib, 4, idx, sizeof(idx)) == BGPU_ERR_INVALID_ARGUMENT);
    BgpuMeshVertex verts[4];
    memset(verts, 0, sizeof(verts));
    CHECK(bingpu_buffer_update(vb, 0, verts, sizeof(verts)) == BGPU_OK);

    CHECK(bingpu_cmd_begin_render(cmd, &rb) == BGPU_OK);
    CHECK(bingpu_cmd_set_pipeline(cmd, pipe) == BGPU_OK);
    CHECK(bingpu_cmd_draw_indexed(cmd, vb, 0, ib, 6, 0) == BGPU_ERR_STATE);
    CHECK(bingpu_cmd_set_pipeline(cmd, mesh_pipe) == BGPU_OK);
    CHECK(bingpu_cmd_draw_indexed(cmd, vb, 1, ib, 6, 0) == BGPU_ERR_INVALID_ARGUMENT);
    CHECK(bingpu_cmd_draw_indexed(cmd, vb, 0, ib, 6, 2) == BGPU_ERR_INVALID_ARGUMENT);
    CHECK(bingpu_cmd_draw_indexed(cmd, vb, 0, ib, 6, 0) == BGPU_OK);
    CHECK(bingpu_cmd_end_render(cmd) == BGPU_OK);

    CHECK(bingpu_frame_begin(sc, cmd, &target) == BGPU_OK);
    CHECK(bingpu_cmd_begin_render(cmd, &rb) == BGPU_OK);
    CHECK(bingpu_cmd_end_render(cmd) == BGPU_OK);
    CHECK(bingpu_frame_end(sc, cmd) == BGPU_OK);

    /* 计数：成功的绘制只有两次 —— 一次 draw_quads(2 quad ⇒ 4 三角)、
     * 一次 draw_indexed(6 索引 ⇒ 2 三角)；中途那些 STATE/INVALID 的调用不计。 */
    BgpuStats st;
    memset(&st, 0, sizeof(st));
    st.struct_size = (uint32_t)sizeof(st);
    CHECK(bingpu_device_stats(device, &st) == BGPU_OK);
    CHECK_EQ(st.frames_presented, 2u);
    CHECK_EQ(st.frame_index, 2u);
    CHECK_EQ(st.draws, 2u);
    CHECK_EQ(st.quads, 2u);
    CHECK_EQ(st.triangles, 6u);
    CHECK(st.bytes_live > 0);

    /* NULL 后端不存像素：回读必须明确拒绝 */
    uint8_t buf[64 * 64 * 4];
    uint32_t stride = 0;
    CHECK(bingpu_frame_read(sc, buf, sizeof(buf), &stride) == BGPU_ERR_UNSUPPORTED);
    check_error_shape("bingpu_frame_read");
    CHECK(bingpu_texture_read(rt, buf, sizeof(buf), &stride) == BGPU_ERR_UNSUPPORTED);

    /* 纹理上传的区域校验 */
    uint8_t px[4 * 4 * 4];
    memset(px, 0xFF, sizeof(px));
    BgpuBox box = {62, 62, 4, 4};   /* 62+4 > 64：越界 */
    CHECK(bingpu_texture_upload(rt, &box, px, 16) == BGPU_ERR_INVALID_ARGUMENT);
    box.x = 0;
    box.y = 0;
    CHECK(bingpu_texture_upload(rt, &box, px, 16) == BGPU_OK);
    CHECK(bingpu_texture_upload(rt, NULL, px, 64 * 4) == BGPU_OK);

    /* swapchain 信息 + resize */
    BgpuSwapchainInfo si;
    memset(&si, 0, sizeof(si));
    si.struct_size = (uint32_t)sizeof(si);
    CHECK(bingpu_swapchain_info(sc, &si) == BGPU_OK);
    CHECK_EQ(si.width, 64u);
    CHECK_EQ(si.image_count, 1u);
    CHECK_EQ(si.present_paces_frames, 0u);
    CHECK_EQ(bingpu_present_paces_frames(sc), 0);
    CHECK(bingpu_swapchain_resize(sc, 128, 96) == BGPU_OK);
    CHECK(bingpu_swapchain_info(sc, &si) == BGPU_OK);
    CHECK_EQ(si.width, 128u);
    CHECK_EQ(si.height, 96u);

    /* 清理：先子后父 */
    bingpu_buffer_destroy(device, vb);
    bingpu_buffer_destroy(device, ib);
    bingpu_pipeline_destroy(device, pipe);
    bingpu_pipeline_destroy(device, mesh_pipe);
    bingpu_texture_destroy(device, rt);
    bingpu_cmd_destroy(device, cmd);
    bingpu_swapchain_destroy(device, sc);
    bingpu_device_destroy(device);
}

/* ============================================================
 * 6) 线程本地错误槽
 * ============================================================ */
#ifdef _WIN32
static HANDLE g_worker_go = NULL;
static DWORD g_worker_ok = 0;

static DWORD WINAPI worker_main(LPVOID param) {
    (void)param;
    WaitForSingleObject(g_worker_go, INFINITE);
    /* 这个线程故意失败一个**和主线程不同**的函数 */
    BgpuDeviceInfo info;
    memset(&info, 0, sizeof(info));
    info.struct_size = (uint32_t)sizeof(info);
    (void)bingpu_device_info(BGPU_INVALID, &info);
    const char* e = bingpu_last_error();
    if (e && strstr(e, "bingpu_device_info") && !strstr(e, "bingpu_cmd_set_viewport")) {
        g_worker_ok = 1;
    }
    return 0;
}
#endif

static void test_thread_local_error(void) {
    printf("[thread_local_error]\n");
#ifdef _WIN32
    g_worker_go = CreateEventA(NULL, FALSE, FALSE, NULL);
    CHECK(g_worker_go != NULL);
    HANDLE th = CreateThread(NULL, 0, worker_main, NULL, 0, NULL);
    CHECK(th != NULL);
    /* 主线程先失败一个函数，再放行工作线程 */
    (void)bingpu_cmd_set_viewport(BGPU_INVALID, 1.0f, 2.0f, 3.0f, 4.0f);
    check_error_shape("bingpu_cmd_set_viewport");
    SetEvent(g_worker_go);
    WaitForSingleObject(th, INFINITE);
    /* 工作线程写过错误之后，主线程看到的仍然是自己那条 */
    const char* e = bingpu_last_error();
    CHECK(e && strstr(e, "bingpu_cmd_set_viewport") != NULL);
    CHECK(e && strstr(e, "bingpu_device_info") == NULL);
    CHECK_EQ(g_worker_ok, 1u);
    CloseHandle(th);
    CloseHandle(g_worker_go);
    g_worker_go = NULL;
#else
    /* 非 Windows 平台没有跨线程测试（本项目主战场是 Windows/MinGW） */
    printf("  (skip: 非 Windows 平台)\n");
#endif
}

/* ============================================================
 * 7) 成功不清空错误槽（GetLastError 语义）+ clear_error
 * ============================================================ */
static void test_error_slot(void) {
    printf("[error_slot]\n");
    bingpu_clear_error();
    CHECK(strcmp(bingpu_last_error(), "") == 0);
    BgpuDeviceInfo info;
    memset(&info, 0, sizeof(info));
    info.struct_size = (uint32_t)sizeof(info);
    (void)bingpu_device_info(BGPU_INVALID, &info);
    CHECK(bingpu_last_error()[0] != '\0');
    /* 一次成功调用不该把上一条错误清掉（否则"失败后紧接一个查询"就丢信息了） */
    (void)bingpu_backend_count();
    CHECK(bingpu_last_error()[0] != '\0');
    bingpu_clear_error();
    CHECK(bingpu_last_error()[0] == '\0');
    /* shutdown 可重复调用 */
    bingpu_shutdown();
    bingpu_shutdown();
    CHECK_EQ(1, 1);
}

/* ============================================================
 * 8) shutdown 的兜底清理（调用方忘了销毁设备/资源）
 *
 * 这条路径最容易崩：资源析构要回写所属设备的记账，如果 shutdown 先放设备
 * 再放资源，就是 use-after-free。所以断言两件事：不崩 + 句柄全部失效。
 * ============================================================ */
static void test_shutdown_cleanup(void) {
    printf("[shutdown_cleanup]\n");
    BgpuDevice device = BGPU_INVALID;
    BgpuDeviceDesc d;
    memset(&d, 0, sizeof(d));
    d.struct_size = (uint32_t)sizeof(d);
    d.backend = BGPU_BACKEND_CPU;
    d.width = 32;
    d.height = 32;
    d.flags = BGPU_DEVICE_HEADLESS;
    CHECK(bingpu_device_create(&d, &device) == BGPU_OK);
    BgpuTextureDesc td;
    memset(&td, 0, sizeof(td));
    td.struct_size = (uint32_t)sizeof(td);
    td.width = 8;
    td.height = 8;
    td.format = BGPU_FORMAT_RGBA8;
    td.flags = BGPU_TEXTURE_RENDER_TARGET | BGPU_TEXTURE_CPU_READ;
    BgpuTexture tex = BGPU_INVALID;
    CHECK(bingpu_texture_create(device, &td, &tex) == BGPU_OK);
    BgpuSwapchainDesc sd;
    memset(&sd, 0, sizeof(sd));
    sd.struct_size = (uint32_t)sizeof(sd);
    sd.width = 32;
    sd.height = 32;
    sd.format = BGPU_FORMAT_RGBA8;
    BgpuSwapchain sc = BGPU_INVALID;
    CHECK(bingpu_swapchain_create(device, &sd, &sc) == BGPU_OK);
    BgpuCmd cmd = BGPU_INVALID;
    CHECK(bingpu_cmd_create(device, &cmd) == BGPU_OK);
    /* 故意什么都不销毁，直接 shutdown（"忘了清理"的兜底路径） */
    bingpu_shutdown();
    CHECK(bingpu_last_error()[0] == '\0');   /* shutdown 顺手清了错误槽 */
    BgpuTextureInfo ti;
    memset(&ti, 0, sizeof(ti));
    ti.struct_size = (uint32_t)sizeof(ti);
    CHECK(bingpu_texture_info(tex, &ti) == BGPU_ERR_INVALID_HANDLE);
    BgpuDeviceInfo di;
    memset(&di, 0, sizeof(di));
    di.struct_size = (uint32_t)sizeof(di);
    CHECK(bingpu_device_info(device, &di) == BGPU_ERR_INVALID_HANDLE);
    bingpu_shutdown();   /* 再调一次仍然是 no-op */
    CHECK_EQ(1, 1);
}

int main(void) {
    printf("binGPU ABI 契约测试（C11，ABI %u，版本 %s）\n", bingpu_abi_version(),
           bingpu_version_string());
    test_version();
    test_result_names();
    test_format_bytes();
    test_backends();
    test_struct_size();
    test_handles();
    test_state_machine();
    test_thread_local_error();
    test_error_slot();
    test_shutdown_cleanup();
    printf("通过 %d 项，失败 %d 项\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
