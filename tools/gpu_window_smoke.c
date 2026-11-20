/* tools/gpu_window_smoke.c —— 窗口路径烟测：交换链 / present / frame_read / resize
 *
 * 为什么它是**仓库里的工具**而不是临时程序：HEADLESS 的逐像素用例（tests/gpu_parity.h）
 * 覆盖不到"交换链 + 呈现"这一段，而那正是 binUI 当年三个 bug 出没的地方（D3D11 的
 * ResizeBuffers、VK 的两份交换链、flipping 窗口下的 GDI 贴屏）。每个真后端落地时都要
 * 跑一遍，所以给它一个固定的位置。
 *
 * 用法（make smoke 会把三个后端各跑一遍）：
 *     build-mingw/bin/gpu_window_smoke vk | d3d11 | gl [msaa]
 *
 * 第三个参数是 MSAA 档（1/4/8，省略 = 设备默认 1×）。窗口路径的 MSAA 是
 * "多采样附件 → pass 末 resolve 到交换链图像"，与 HEADLESS 那条是两段实现，
 * 所以开档之后也要手动跑一次（D3D11 不支持，会自己降级为 1×）。
 *
 * 判据（三个后端必须给出同一组数字）：
 *   · 3 帧 clear 的颜色经 present → frame_read 往返**逐字节相等**；
 *   · 四边形覆盖处读回是白色（形状/坐标/呈现方向都对）；
 *   · 改窗口尺寸后 resize 交换链，报告的尺寸 = 新客户区尺寸。
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#define BGPU_SMOKE_WIN32 1
#endif

#include "bingpu.h"

static int g_fail = 0;
#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            g_fail++;                                                       \
            printf("  FAIL: %s（%s）\n", (msg), bingpu_last_error());       \
        }                                                                   \
    } while (0)

static BgpuBackendKind parse_kind(const char* s) {
    if (!s) return BGPU_BACKEND_D3D11;
    if (strcmp(s, "vk") == 0 || strcmp(s, "vulkan") == 0) return BGPU_BACKEND_VULKAN;
    if (strcmp(s, "gl") == 0 || strcmp(s, "opengl") == 0) return BGPU_BACKEND_GL;
    if (strcmp(s, "d3d11") == 0) return BGPU_BACKEND_D3D11;
    return BGPU_BACKEND_NULL;
}

int main(int argc, char** argv) {
    const BgpuBackendKind kind = parse_kind(argc > 1 ? argv[1] : NULL);
    const uint32_t want_msaa =
        (argc > 2) ? (uint32_t)strtoul(argv[2], NULL, 10) : 0;
    printf("窗口路径烟测：%s（binGPU %s / ABI %u，msaa 请求=%u）\n", bingpu_backend_name(kind),
           bingpu_version_string(), bingpu_abi_version(), want_msaa ? want_msaa : 1);
    if (!bingpu_backend_available(kind)) {
        printf("跳过：这台机器没有可用的 %s 后端\n", bingpu_backend_name(kind));
        return 0;
    }
#ifndef BGPU_SMOKE_WIN32
    printf("跳过：这不是 Windows，没有窗口路径\n");
    return 0;
#else
    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "BinGpuWindowSmoke";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "binGPU window smoke", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 480, 320, NULL, NULL, wc.hInstance,
                                NULL);
    if (!hwnd) {
        printf("建窗失败\n");
        return 1;
    }
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    BgpuDeviceDesc dd;
    memset(&dd, 0, sizeof(dd));
    dd.struct_size = (uint32_t)sizeof(dd);
    dd.backend = kind;
    dd.native_window = hwnd;
    dd.width = 480;
    dd.height = 320;
    dd.flags = BGPU_DEVICE_VSYNC;
    dd.msaa_samples = want_msaa;
    BgpuDevice dev = BGPU_INVALID;
    CHECK(bingpu_device_create(&dd, &dev) == BGPU_OK, "device_create(窗口)");
    if (g_fail) return 1;

    RECT rc;
    GetClientRect(hwnd, &rc);
    BgpuSwapchainDesc sd;
    memset(&sd, 0, sizeof(sd));
    sd.struct_size = (uint32_t)sizeof(sd);
    sd.native_window = hwnd;
    sd.width = (uint32_t)(rc.right - rc.left);
    sd.height = (uint32_t)(rc.bottom - rc.top);
    sd.flags = BGPU_DEVICE_VSYNC;
    sd.format = BGPU_FORMAT_RGBA8;
    BgpuSwapchain sc = BGPU_INVALID;
    CHECK(bingpu_swapchain_create(dev, &sd, &sc) == BGPU_OK, "swapchain_create");
    BgpuSwapchainInfo si;
    memset(&si, 0, sizeof(si));
    si.struct_size = (uint32_t)sizeof(si);
    CHECK(bingpu_swapchain_info(sc, &si) == BGPU_OK, "swapchain_info");
    printf("  交换链 %ux%u 图数=%u msaa=%u present_paces=%u\n", si.width, si.height,
           si.image_count, si.msaa_samples, si.present_paces_frames);

    BgpuPipelineDesc pd;
    memset(&pd, 0, sizeof(pd));
    pd.struct_size = (uint32_t)sizeof(pd);
    pd.vertex_layout = BGPU_VERTEX_2D;
    pd.blend = BGPU_BLEND_PREMULTIPLIED;
    BgpuPipeline pipe = BGPU_INVALID;
    CHECK(bingpu_pipeline_create(dev, &pd, &pipe) == BGPU_OK, "pipeline_create");
    BgpuCmd cmd = BGPU_INVALID;
    CHECK(bingpu_cmd_create(dev, &cmd) == BGPU_OK, "cmd_create");

    const uint32_t clears[3] = {0xFF102030u, 0xFF30A050u, 0xFF2030E0u};
    const size_t bytes = (size_t)si.width * si.height * 4;
    uint8_t* frame = (uint8_t*)malloc(bytes);
    for (int i = 0; i < 3; i++) {
        BgpuTexture target = BGPU_INVALID;
        CHECK(bingpu_frame_begin(sc, cmd, &target) == BGPU_OK, "frame_begin");
        BgpuRenderBeginDesc rd;
        memset(&rd, 0, sizeof(rd));
        rd.struct_size = (uint32_t)sizeof(rd);
        rd.target = target;
        rd.flags = BGPU_RENDER_CLEAR_COLOR;
        rd.clear_color = clears[i];
        CHECK(bingpu_cmd_begin_render(cmd, &rd) == BGPU_OK, "begin_render");
        bingpu_cmd_set_pipeline(cmd, pipe);
        BgpuVertex v[4];
        memset(v, 0, sizeof(v));
        v[0].x = 40;  v[0].y = 40;  v[0].color = 0xFFFFFFFFu;
        v[1].x = 240; v[1].y = 40;  v[1].u = 1; v[1].color = 0xFFFFFFFFu;
        v[2].x = 40;  v[2].y = 200; v[2].v = 1; v[2].color = 0xFFFFFFFFu;
        v[3].x = 240; v[3].y = 200; v[3].u = 1; v[3].v = 1; v[3].color = 0xFFFFFFFFu;
        CHECK(bingpu_cmd_draw_quads(cmd, v, 1) == BGPU_OK, "draw_quads");
        CHECK(bingpu_cmd_end_render(cmd) == BGPU_OK, "end_render");
        CHECK(bingpu_frame_end(sc, cmd) == BGPU_OK, "frame_end");
        uint32_t stride = 0;
        if (bingpu_frame_read(sc, frame, bytes, &stride) == BGPU_OK) {
            const uint32_t want = clears[i];
            const uint32_t got = ((uint32_t)frame[3] << 24) | ((uint32_t)frame[0] << 16) |
                                 ((uint32_t)frame[1] << 8) | (uint32_t)frame[2];
            const size_t mid = ((size_t)100 * si.width + 100) * 4;
            const uint32_t q = ((uint32_t)frame[mid + 3] << 24) |
                               ((uint32_t)frame[mid + 0] << 16) |
                               ((uint32_t)frame[mid + 1] << 8) | (uint32_t)frame[mid + 2];
            printf("  第 %d 帧 clear=%08X 读回 px(0,0)=%08X px(100,100)=%08X\n", i + 1, want, got,
                   q);
            CHECK(got == want, "clear 往返逐位相等");
            CHECK(q == 0xFFFFFFFFu, "四边形覆盖处是白色");
        } else {
            printf("  frame_read 失败：%s\n", bingpu_last_error());
            g_fail++;
        }
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }
    free(frame);

    printf("  改窗口尺寸后 resize 交换链\n");
    SetWindowPos(hwnd, NULL, 0, 0, 400, 260, SWP_NOMOVE | SWP_NOZORDER);
    GetClientRect(hwnd, &rc);
    CHECK(bingpu_swapchain_resize(sc, (uint32_t)(rc.right - rc.left),
                                 (uint32_t)(rc.bottom - rc.top)) == BGPU_OK,
          "swapchain_resize");
    memset(&si, 0, sizeof(si));
    si.struct_size = (uint32_t)sizeof(si);
    CHECK(bingpu_swapchain_info(sc, &si) == BGPU_OK, "swapchain_info(2)");
    printf("  重建后 %ux%u 图数=%u（客户区 %ldx%ld）\n", si.width, si.height, si.image_count,
           rc.right - rc.left, rc.bottom - rc.top);
    CHECK(si.width == (uint32_t)(rc.right - rc.left), "重建后宽度 = 客户区宽度");
    CHECK(si.height == (uint32_t)(rc.bottom - rc.top), "重建后高度 = 客户区高度");

    bingpu_cmd_destroy(dev, cmd);
    bingpu_swapchain_destroy(dev, sc);
    bingpu_device_destroy(dev);
    DestroyWindow(hwnd);
    printf(g_fail ? "窗口烟测：失败 %d 项\n" : "窗口烟测：全部通过\n", g_fail);
    return g_fail ? 1 : 0;
#endif
}
