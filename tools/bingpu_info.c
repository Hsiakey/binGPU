/* tools/bingpu_info.c —— 自查工具：版本 / 后端可用性 / caps / stats
 *
 * 为什么需要它：
 *   绑定的作者（Python/Go/Rust）以及 CI 第一件想知道的事是"这台机器上
 *   binGPU 到底能用哪个后端、它自报什么能力"。把这件事做成一个 200 行的
 *   命令行工具，比写一堆文档靠谱 —— 它跑出来的就是真相。
 *
 * 用法：bingpu_info            列全部，并对**可用**后端各建一个 320x200 的
 *                               HEADLESS 设备，打印 info/caps/stats
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bingpu.h"

static void print_caps(const BgpuCaps* c) {
    printf("  caps：flags=0x%08X", c->flags);
    if (c->flags & BGPU_CAP_DEPTH) printf(" DEPTH");
    if (c->flags & BGPU_CAP_OFFSCREEN) printf(" OFFSCREEN");
    if (c->flags & BGPU_CAP_READBACK) printf(" READBACK");
    if (c->flags & BGPU_CAP_PRESENT_PACES) printf(" PRESENT_PACES");
    if (c->flags & BGPU_CAP_TRANSPARENT) printf(" TRANSPARENT");
    if (c->flags & BGPU_CAP_VSYNC) printf(" VSYNC");
    if (c->flags & BGPU_CAP_MSAA) printf(" MSAA");
    if (c->flags & BGPU_CAP_MESH) printf(" MESH");
    if (c->flags & BGPU_CAP_SAMPLERS) printf(" SAMPLERS");
    if (c->flags & BGPU_CAP_AFFINE) printf(" AFFINE");
    printf("\n");
    printf("  max_texture_size=%u  max_frames_in_flight=%u  msaa=%u  max_quads_per_draw=%u\n",
           c->max_texture_size, c->max_frames_in_flight, c->msaa_samples,
           c->max_quads_per_draw);
}

static void print_stats(const BgpuStats* s) {
    printf("  stats：frame_index=%u  frames_presented=%llu  draws=%llu  quads=%llu"
           "  triangles=%llu\n",
           s->frame_index, (unsigned long long)s->frames_presented,
           (unsigned long long)s->draws, (unsigned long long)s->quads,
           (unsigned long long)s->triangles);
    printf("         bytes_uploaded=%llu  bytes_live=%llu\n",
           (unsigned long long)s->bytes_uploaded, (unsigned long long)s->bytes_live);
}

/* 跑一遍最小调用序列：证明这个后端至少能走完 frame 的三个阶段。
 * 顺便让 stats 里有点数字可看（不是全 0 才叫"有意义"）。 */
static void smoke(BgpuDevice dev) {
    BgpuSwapchainDesc sd;
    memset(&sd, 0, sizeof(sd));
    sd.struct_size = (uint32_t)sizeof(sd);
    sd.width = 320;
    sd.height = 200;
    sd.format = BGPU_FORMAT_RGBA8;
    BgpuSwapchain sc = BGPU_INVALID;
    if (bingpu_swapchain_create(dev, &sd, &sc) != BGPU_OK) {
        printf("  swapchain_create 失败：%s\n", bingpu_last_error());
        return;
    }
    BgpuCmd cmd = BGPU_INVALID;
    if (bingpu_cmd_create(dev, &cmd) != BGPU_OK) {
        printf("  cmd_create 失败：%s\n", bingpu_last_error());
        bingpu_swapchain_destroy(dev, sc);
        return;
    }
    BgpuPipelineDesc pd;
    memset(&pd, 0, sizeof(pd));
    pd.struct_size = (uint32_t)sizeof(pd);
    pd.vertex_layout = BGPU_VERTEX_2D;
    pd.blend = BGPU_BLEND_PREMULTIPLIED;
    BgpuPipeline pipe = BGPU_INVALID;
    if (bingpu_pipeline_create(dev, &pd, &pipe) != BGPU_OK) {
        printf("  pipeline_create 失败：%s\n", bingpu_last_error());
        bingpu_cmd_destroy(dev, cmd);
        bingpu_swapchain_destroy(dev, sc);
        return;
    }
    BgpuTexture target = BGPU_INVALID;
    if (bingpu_frame_begin(sc, cmd, &target) != BGPU_OK) {
        printf("  frame_begin 失败：%s\n", bingpu_last_error());
        goto done;
    }
    BgpuRenderBeginDesc rd;
    memset(&rd, 0, sizeof(rd));
    rd.struct_size = (uint32_t)sizeof(rd);
    rd.target = target;
    rd.flags = BGPU_RENDER_CLEAR_COLOR;
    rd.clear_color = 0xFF1E3A8Au;
    if (bingpu_cmd_begin_render(cmd, &rd) != BGPU_OK) {
        printf("  begin_render 失败：%s\n", bingpu_last_error());
        goto done;
    }
    bingpu_cmd_set_pipeline(cmd, pipe);
    BgpuVertex quad[4];
    memset(quad, 0, sizeof(quad));
    const uint32_t c = 0xFFFFFFFFu;
    const BgpuVertex tl = {40, 40, 0, 0, 0, c};
    const BgpuVertex tr = {280, 40, 0, 1, 0, c};
    const BgpuVertex bl = {40, 160, 0, 0, 1, c};
    const BgpuVertex br = {280, 160, 0, 1, 1, c};
    quad[0] = tl;
    quad[1] = tr;
    quad[2] = bl;
    quad[3] = br;
    bingpu_cmd_draw_quads(cmd, quad, 1);
    bingpu_cmd_end_render(cmd);
    if (bingpu_frame_end(sc, cmd) != BGPU_OK) {
        printf("  frame_end 失败：%s\n", bingpu_last_error());
        goto done;
    }
    {
        BgpuStats st;
        memset(&st, 0, sizeof(st));
        st.struct_size = (uint32_t)sizeof(st);
        if (bingpu_device_stats(dev, &st) == BGPU_OK) print_stats(&st);
    }
    /* HEADLESS 的 CPU 后端还能把这一帧读回来 —— 工具顺手验证一遍。
     * frame_read 要的是"整帧"缓冲（它一次拷 w*h），不是一行。 */
    {
        const size_t bytes = (size_t)320 * 200 * 4;
        uint8_t* frame = (uint8_t*)malloc(bytes);
        uint32_t stride = 0;
        if (frame && bingpu_frame_read(sc, frame, bytes, &stride) == BGPU_OK) {
            printf("  frame_read: stride=%u first_px=%02X%02X%02X%02X\n", stride, frame[0],
                   frame[1], frame[2], frame[3]);
        } else {
            printf("  frame_read unavailable: %s\n", bingpu_last_error());
        }
        free(frame);
    }
done:
    bingpu_pipeline_destroy(dev, pipe);
    bingpu_cmd_destroy(dev, cmd);
    bingpu_swapchain_destroy(dev, sc);
}

int main(void) {
    printf("binGPU %s（ABI %u）\n", bingpu_version_string(), bingpu_abi_version());
    printf("后端 %u 个：\n", bingpu_backend_count());
    for (uint32_t i = 0; i < bingpu_backend_count(); i++) {
        const BgpuBackendKind k = bingpu_backend_at(i);
        printf("  [%u] %-7s %s\n", i, bingpu_backend_name(k),
               bingpu_backend_available(k) ? "可用" : "不可用");
    }
    for (uint32_t i = 0; i < bingpu_backend_count(); i++) {
        const BgpuBackendKind k = bingpu_backend_at(i);
        if (!bingpu_backend_available(k)) continue;
        printf("\n--- %s ---\n", bingpu_backend_name(k));
        BgpuDeviceDesc dd;
        memset(&dd, 0, sizeof(dd));
        dd.struct_size = (uint32_t)sizeof(dd);
        dd.backend = k;
        dd.width = 320;
        dd.height = 200;
        dd.flags = BGPU_DEVICE_HEADLESS;
        BgpuDevice dev = BGPU_INVALID;
        if (bingpu_device_create(&dd, &dev) != BGPU_OK) {
            printf("  device_create 失败：%s\n", bingpu_last_error());
            continue;
        }
        BgpuDeviceInfo info;
        memset(&info, 0, sizeof(info));
        info.struct_size = (uint32_t)sizeof(info);
        if (bingpu_device_info(dev, &info) == BGPU_OK) {
            printf("  后端=%s surface_format=%d adapter=\"%s\" driver=\"%s\" api=%u\n",
                   bingpu_backend_name(info.backend), (int)info.surface_format,
                   info.adapter_name, info.driver_name, info.api_version);
        }
        BgpuCaps caps;
        memset(&caps, 0, sizeof(caps));
        caps.struct_size = (uint32_t)sizeof(caps);
        if (bingpu_device_caps(dev, &caps) == BGPU_OK) print_caps(&caps);
        smoke(dev);
        bingpu_device_destroy(dev);
    }
    bingpu_shutdown();
    return 0;
}
