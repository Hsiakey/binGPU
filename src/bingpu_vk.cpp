/* bingpu_vk.cpp —— Vulkan 后端（第一个真实后端）
 *
 * 定位（见 docs/layering.md）：
 *   · binGPU 只暴露**固定功能批处理**：两种顶点布局 × 五种混合 × 位置/颜色/UV + 仿射；
 *   · 没有用户着色器、没有 render pass 模板、没有 descriptor 反射；
 *   · 所以这里是一个**状态映射**实现，不是"半个 Vulkan 封装"：上层每笔 draw 只决定
 *     (管线, 纹理, scissor, 仿射)，其余都不出这一层。
 *
 * 与 CPU 参考后端的关系：CPU 后端是**像素权威**（docs/backends.md），本后端必须和它
 * 逐像素一致 —— 规格是 docs/raster_rules.md，验收是 tests/test_vk_render.c。所以：
 *   · 顶点变换、top-left 填充规则、预乘混合因子、UV 采样约定全部照抄规格；
 *   · 默认 MSAA 关掉（与 CPU 一致，caps.msaa_samples 报设备上限、实际档由 desc 决定）；
 *   · 纹理/目标内部存**预乘** RGBA8：上传时预乘、回读时反预乘（raster_rules §9）。
 *
 * 构建不依赖 Vulkan SDK：
 *   · 头文件在 third_party/vulkan/include（不入库，scripts/fetch-deps.sh 拉）；
 *   · 运行期只 LoadLibrary("vulkan-1.dll")，不链接 vulkan-1.lib；
 *   · 头文件缺失时本文件退化成"登记但不可用"的桩（available() == false），
 *     其余后端与测试完全不受影响。
 *
 * 这一版明确没做（写进 docs/backends.md 的欠账，不是漏了）：
 *   · 多帧飞行（现在每帧 submit + 等 fence，先要正确、再要重叠）；
 *   · 透明窗口（composite alpha）；MSAA 与 mip 已实现（见 docs/backends.md
 *     的质量档一节：多采样 companion + resolve、逐级 blit 生成 mip）；
 *   · 自定义着色器（那要 ABI 2，见 docs/layering.md §4）。
 */
#include "bingpu_backend.h"

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#if defined(_WIN32) && defined(__has_include)
#if __has_include("vulkan/vulkan.h")
#define BINGPU_HAS_VK 1
#endif
#endif

#ifndef BINGPU_HAS_VK

/* ---------------------------------------------------------------
 * 桩：编译时没有 Vulkan 头（third_party/vulkan/include 没拉下来）
 * --------------------------------------------------------------- */
namespace bgpu {
namespace {
class StubVkBackend final : public Backend {
public:
    BgpuBackendKind kind() const noexcept override { return BGPU_BACKEND_VULKAN; }
    const char* name() const noexcept override { return "vulkan"; }
    bool available() const noexcept override { return false; }
    const char* unavailableReason() const noexcept override {
        return "编译时没有 third_party/vulkan/include：先跑 scripts/fetch-deps.sh 再重编";
    }
    BgpuResult createDevice(const BgpuDeviceDesc&, BgpuDevice*) override {
        return fail(BGPU_ERR_UNSUPPORTED, __FILE__, __LINE__, unavailableReason());
    }
    void deviceInfo(Device&, BgpuDeviceInfo&) override {}
    void deviceCaps(Device&, BgpuCaps&) override {}
    void deviceStats(Device&, BgpuStats&) override {}
    BgpuResult createSwapchain(Device&, const BgpuSwapchainDesc&, BgpuSwapchain*) override {
        return BGPU_ERR_UNSUPPORTED;
    }
    BgpuResult resizeSwapchain(Swapchain&, uint32_t, uint32_t) override {
        return BGPU_ERR_UNSUPPORTED;
    }
    void swapchainInfo(Swapchain&, BgpuSwapchainInfo&) override {}
    BgpuResult frameRead(Swapchain&, void*, size_t, uint32_t*) override {
        return BGPU_ERR_UNSUPPORTED;
    }
    BgpuResult createTexture(Device&, const BgpuTextureDesc&, BgpuTexture*) override {
        return BGPU_ERR_UNSUPPORTED;
    }
    void textureInfo(Texture&, BgpuTextureInfo&) override {}
    BgpuResult textureUpload(Texture&, const BgpuBox*, const void*, uint32_t) override {
        return BGPU_ERR_UNSUPPORTED;
    }
    BgpuResult textureRead(Texture&, void*, size_t, uint32_t*) override {
        return BGPU_ERR_UNSUPPORTED;
    }
    BgpuResult createBuffer(Device&, const BgpuBufferDesc&, BgpuBuffer*) override {
        return BGPU_ERR_UNSUPPORTED;
    }
    BgpuResult bufferUpdate(Buffer&, uint32_t, const void*, uint32_t) override {
        return BGPU_ERR_UNSUPPORTED;
    }
    BgpuResult createSampler(Device&, const BgpuSamplerDesc&, BgpuSampler*) override {
        return BGPU_ERR_UNSUPPORTED;
    }
    BgpuResult createPipeline(Device&, const BgpuPipelineDesc&, BgpuPipeline*) override {
        return BGPU_ERR_UNSUPPORTED;
    }
    BgpuResult createCmd(Device&, BgpuCmd*) override { return BGPU_ERR_UNSUPPORTED; }
    BgpuResult frameBegin(Swapchain&, Cmd&, BgpuTexture*) override { return BGPU_ERR_UNSUPPORTED; }
    BgpuResult frameEnd(Swapchain&, Cmd&) override { return BGPU_ERR_UNSUPPORTED; }
    BgpuResult beginRender(Cmd&, Texture&, Texture*, const BgpuRenderBeginDesc&) override {
        return BGPU_ERR_UNSUPPORTED;
    }
    BgpuResult endRender(Cmd&) override { return BGPU_ERR_UNSUPPORTED; }
    BgpuResult setViewport(Cmd&, float, float, float, float) override { return BGPU_OK; }
    BgpuResult setScissor(Cmd&, int32_t, int32_t, int32_t, int32_t) override { return BGPU_OK; }
    BgpuResult setPipeline(Cmd&, Pipeline&) override { return BGPU_OK; }
    BgpuResult setTexture(Cmd&, Texture*, Sampler*) override { return BGPU_OK; }
    BgpuResult setAffine(Cmd&, const float*) override { return BGPU_OK; }
    BgpuResult drawQuads(Cmd&, const BgpuVertex*, uint32_t) override {
        return BGPU_ERR_UNSUPPORTED;
    }
    BgpuResult drawIndexed(Cmd&, Buffer&, uint32_t, Buffer&, uint32_t, uint32_t) override {
        return BGPU_ERR_UNSUPPORTED;
    }
};
}  // namespace

Backend& vkBackend() {
    static StubVkBackend be;
    return be;
}

}  // namespace bgpu

#else /* ============================= 真实现 ============================= */

#include <windows.h>

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>

#include "bingpu_vk_shaders.h"

namespace bgpu {
namespace {

/* ---------------------------------------------------------------
 * 动态加载：只认 vulkan-1.dll（运行期），构建期零链接
 * --------------------------------------------------------------- */
struct VkFns {
    HMODULE lib = nullptr;
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr = nullptr;

#define BGPU_VK_FN(name) PFN_##name name = nullptr
    BGPU_VK_FN(vkCreateInstance);
    BGPU_VK_FN(vkDestroyInstance);
    BGPU_VK_FN(vkEnumeratePhysicalDevices);
    BGPU_VK_FN(vkGetPhysicalDeviceProperties);
    BGPU_VK_FN(vkGetPhysicalDeviceFeatures);
    BGPU_VK_FN(vkGetPhysicalDeviceQueueFamilyProperties);
    BGPU_VK_FN(vkGetPhysicalDeviceMemoryProperties);
    BGPU_VK_FN(vkCreateDevice);
    BGPU_VK_FN(vkDestroyDevice);
    BGPU_VK_FN(vkGetDeviceQueue);
    BGPU_VK_FN(vkDeviceWaitIdle);
    BGPU_VK_FN(vkDestroySurfaceKHR);
    BGPU_VK_FN(vkCreateWin32SurfaceKHR);
    BGPU_VK_FN(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    BGPU_VK_FN(vkGetPhysicalDeviceSurfaceFormatsKHR);
    BGPU_VK_FN(vkGetPhysicalDeviceSurfaceSupportKHR);
    BGPU_VK_FN(vkCreateSwapchainKHR);
    BGPU_VK_FN(vkDestroySwapchainKHR);
    BGPU_VK_FN(vkGetSwapchainImagesKHR);
    BGPU_VK_FN(vkAcquireNextImageKHR);
    BGPU_VK_FN(vkQueuePresentKHR);
    BGPU_VK_FN(vkAllocateMemory);
    BGPU_VK_FN(vkFreeMemory);
    BGPU_VK_FN(vkCreateImage);
    BGPU_VK_FN(vkDestroyImage);
    BGPU_VK_FN(vkGetImageMemoryRequirements);
    BGPU_VK_FN(vkBindImageMemory);
    BGPU_VK_FN(vkCreateImageView);
    BGPU_VK_FN(vkDestroyImageView);
    BGPU_VK_FN(vkCreateBuffer);
    BGPU_VK_FN(vkDestroyBuffer);
    BGPU_VK_FN(vkGetBufferMemoryRequirements);
    BGPU_VK_FN(vkBindBufferMemory);
    BGPU_VK_FN(vkMapMemory);
    BGPU_VK_FN(vkUnmapMemory);
    BGPU_VK_FN(vkCreateSampler);
    BGPU_VK_FN(vkDestroySampler);
    BGPU_VK_FN(vkCreateShaderModule);
    BGPU_VK_FN(vkDestroyShaderModule);
    BGPU_VK_FN(vkCreateDescriptorSetLayout);
    BGPU_VK_FN(vkDestroyDescriptorSetLayout);
    BGPU_VK_FN(vkCreatePipelineLayout);
    BGPU_VK_FN(vkDestroyPipelineLayout);
    BGPU_VK_FN(vkCreateGraphicsPipelines);
    BGPU_VK_FN(vkDestroyPipeline);
    BGPU_VK_FN(vkCreateDescriptorPool);
    BGPU_VK_FN(vkDestroyDescriptorPool);
    BGPU_VK_FN(vkAllocateDescriptorSets);
    BGPU_VK_FN(vkUpdateDescriptorSets);
    BGPU_VK_FN(vkResetDescriptorPool);
    BGPU_VK_FN(vkCreateCommandPool);
    BGPU_VK_FN(vkDestroyCommandPool);
    BGPU_VK_FN(vkAllocateCommandBuffers);
    BGPU_VK_FN(vkFreeCommandBuffers);
    BGPU_VK_FN(vkBeginCommandBuffer);
    BGPU_VK_FN(vkEndCommandBuffer);
    BGPU_VK_FN(vkCmdBeginRendering);
    BGPU_VK_FN(vkCmdEndRendering);
    BGPU_VK_FN(vkCmdSetViewport);
    BGPU_VK_FN(vkCmdSetScissor);
    BGPU_VK_FN(vkCmdBindPipeline);
    BGPU_VK_FN(vkCmdBindDescriptorSets);
    BGPU_VK_FN(vkCmdPushConstants);
    BGPU_VK_FN(vkCmdBindVertexBuffers);
    BGPU_VK_FN(vkCmdBindIndexBuffer);
    BGPU_VK_FN(vkCmdDrawIndexed);
    BGPU_VK_FN(vkCmdCopyBufferToImage);
    BGPU_VK_FN(vkCmdCopyImageToBuffer);
    BGPU_VK_FN(vkCmdBlitImage);
    BGPU_VK_FN(vkCmdResolveImage);
    BGPU_VK_FN(vkCmdPipelineBarrier);
    BGPU_VK_FN(vkQueueSubmit);
    BGPU_VK_FN(vkCreateFence);
    BGPU_VK_FN(vkDestroyFence);
    BGPU_VK_FN(vkWaitForFences);
    BGPU_VK_FN(vkResetFences);
#undef BGPU_VK_FN
};

VkFns g_vk;
bool g_loadTried = false;
bool g_loadOk = false;

/* 用临时实例把所有需要的函数指针取齐。失败就说明驱动太旧/根本装不了 Vulkan。 */
bool loadVulkan() {
    if (g_loadTried) return g_loadOk;
    g_loadTried = true;

    const bool trace = std::getenv("BINGPU_VK_TRACE") != nullptr;
    g_vk.lib = ::LoadLibraryA("vulkan-1.dll");
    if (!g_vk.lib) {
        if (trace) std::fprintf(stderr, "[vk] LoadLibraryA(vulkan-1.dll) 失败 err=%lu\n",
                                (unsigned long)::GetLastError());
        return false;
    }
    /* FARPROC → 函数指针：用 memcpy 而不是 reinterpret_cast，
     * 否则 -Wextra/-Wpedantic 会报 -Wcast-function-type（合法但噪音大）。 */
    {
        FARPROC raw = ::GetProcAddress(g_vk.lib, "vkGetInstanceProcAddr");
        if (!raw) return false;
        std::memcpy(&g_vk.GetInstanceProcAddr, &raw, sizeof(raw));
    }
    if (!g_vk.GetInstanceProcAddr) return false;

    VkApplicationInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.apiVersion = VK_API_VERSION_1_3;
    /* 取函数指针的临时实例**必须**开着 surface 扩展：扩展函数的指针只有在
     * 该实例上启用之后才非空（vkGetInstanceProcAddr 的规矩）。 */
    const char* tmp_exts[] = {VK_KHR_SURFACE_EXTENSION_NAME,
                              VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &ai;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = tmp_exts;

    auto gipa = g_vk.GetInstanceProcAddr;
    g_vk.vkCreateInstance =
        reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
    if (!g_vk.vkCreateInstance) return false;
    VkInstance tmp = VK_NULL_HANDLE;
    {
        const VkResult ir = g_vk.vkCreateInstance(&ici, nullptr, &tmp);
        if (ir != VK_SUCCESS) {
            if (trace) std::fprintf(stderr, "[vk] 临时实例失败 r=%d\n", (int)ir);
            return false;
        }
    }

    /* gipa(实例, 名字) 能取到实例级与设备级函数（loader 保证；比 vkGetDeviceProcAddr 更省事） */
    bool ok = true;
    auto load = [&](const char* name) -> PFN_vkVoidFunction { return gipa(tmp, name); };
#define BGPU_LOAD(name)                                                       \
    g_vk.name = reinterpret_cast<PFN_##name>(load(#name));                    \
    if (!g_vk.name) { ok = false; if (trace) std::fprintf(stderr, "[vk] 缺函数 %s\n", #name); }
    BGPU_LOAD(vkDestroyInstance);
    BGPU_LOAD(vkEnumeratePhysicalDevices);
    BGPU_LOAD(vkGetPhysicalDeviceProperties);
    BGPU_LOAD(vkGetPhysicalDeviceFeatures);
    BGPU_LOAD(vkGetPhysicalDeviceQueueFamilyProperties);
    BGPU_LOAD(vkGetPhysicalDeviceMemoryProperties);
    BGPU_LOAD(vkCreateDevice);
    BGPU_LOAD(vkDestroyDevice);
    BGPU_LOAD(vkGetDeviceQueue);
    BGPU_LOAD(vkDeviceWaitIdle);
    BGPU_LOAD(vkDestroySurfaceKHR);
    BGPU_LOAD(vkCreateWin32SurfaceKHR);
    BGPU_LOAD(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    BGPU_LOAD(vkGetPhysicalDeviceSurfaceFormatsKHR);
    BGPU_LOAD(vkGetPhysicalDeviceSurfaceSupportKHR);
    BGPU_LOAD(vkCreateSwapchainKHR);
    BGPU_LOAD(vkDestroySwapchainKHR);
    BGPU_LOAD(vkGetSwapchainImagesKHR);
    BGPU_LOAD(vkAcquireNextImageKHR);
    BGPU_LOAD(vkQueuePresentKHR);
    BGPU_LOAD(vkAllocateMemory);
    BGPU_LOAD(vkFreeMemory);
    BGPU_LOAD(vkCreateImage);
    BGPU_LOAD(vkDestroyImage);
    BGPU_LOAD(vkGetImageMemoryRequirements);
    BGPU_LOAD(vkBindImageMemory);
    BGPU_LOAD(vkCreateImageView);
    BGPU_LOAD(vkDestroyImageView);
    BGPU_LOAD(vkCreateBuffer);
    BGPU_LOAD(vkDestroyBuffer);
    BGPU_LOAD(vkGetBufferMemoryRequirements);
    BGPU_LOAD(vkBindBufferMemory);
    BGPU_LOAD(vkMapMemory);
    BGPU_LOAD(vkUnmapMemory);
    BGPU_LOAD(vkCreateSampler);
    BGPU_LOAD(vkDestroySampler);
    BGPU_LOAD(vkCreateShaderModule);
    BGPU_LOAD(vkDestroyShaderModule);
    BGPU_LOAD(vkCreateDescriptorSetLayout);
    BGPU_LOAD(vkDestroyDescriptorSetLayout);
    BGPU_LOAD(vkCreatePipelineLayout);
    BGPU_LOAD(vkDestroyPipelineLayout);
    BGPU_LOAD(vkCreateGraphicsPipelines);
    BGPU_LOAD(vkDestroyPipeline);
    BGPU_LOAD(vkCreateDescriptorPool);
    BGPU_LOAD(vkDestroyDescriptorPool);
    BGPU_LOAD(vkAllocateDescriptorSets);
    BGPU_LOAD(vkUpdateDescriptorSets);
    BGPU_LOAD(vkResetDescriptorPool);
    BGPU_LOAD(vkCreateCommandPool);
    BGPU_LOAD(vkDestroyCommandPool);
    BGPU_LOAD(vkAllocateCommandBuffers);
    BGPU_LOAD(vkFreeCommandBuffers);
    BGPU_LOAD(vkBeginCommandBuffer);
    BGPU_LOAD(vkEndCommandBuffer);
    BGPU_LOAD(vkCmdBeginRendering);
    BGPU_LOAD(vkCmdEndRendering);
    BGPU_LOAD(vkCmdSetViewport);
    BGPU_LOAD(vkCmdSetScissor);
    BGPU_LOAD(vkCmdBindPipeline);
    BGPU_LOAD(vkCmdBindDescriptorSets);
    BGPU_LOAD(vkCmdPushConstants);
    BGPU_LOAD(vkCmdBindVertexBuffers);
    BGPU_LOAD(vkCmdBindIndexBuffer);
    BGPU_LOAD(vkCmdDrawIndexed);
    BGPU_LOAD(vkCmdCopyBufferToImage);
    BGPU_LOAD(vkCmdCopyImageToBuffer);
    BGPU_LOAD(vkCmdBlitImage);
    BGPU_LOAD(vkCmdResolveImage);
    BGPU_LOAD(vkCmdPipelineBarrier);
    BGPU_LOAD(vkQueueSubmit);
    BGPU_LOAD(vkCreateFence);
    BGPU_LOAD(vkDestroyFence);
    BGPU_LOAD(vkWaitForFences);
    BGPU_LOAD(vkResetFences);
#undef BGPU_LOAD
    g_vk.vkDestroyInstance(tmp, nullptr);
    g_loadOk = ok;
    if (trace) std::fprintf(stderr, "[vk] loadVulkan ok=%d\n", (int)ok);
    return ok;
}

/* ---------------------------------------------------------------
 * 小工具
 * --------------------------------------------------------------- */
inline VkFormat toVkFormat(BgpuFormat f) {
    return f == BGPU_FORMAT_BGRA8 ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
}

/* 0.14 的 MSAA 档：desc 里只承诺 1/4/8（0 视为 1），这里转成 Vulkan 枚举。 */
inline VkSampleCountFlagBits toVkSamples(uint32_t n) {
    if (n >= 8) return VK_SAMPLE_COUNT_8_BIT;
    if (n >= 4) return VK_SAMPLE_COUNT_4_BIT;
    return VK_SAMPLE_COUNT_1_BIT;
}

/* 描述符里的 mip 级数：MIPMAPS 时补全成整条链（上限 16）；否则显式值（0/1 = 1 级）。
 * **默认路径必须返回 1** —— 返回别的值就等于改了 0 差合同的默认像素。 */
uint32_t mipLevelsFor(const BgpuTextureDesc& desc) {
    if (desc.flags & BGPU_TEXTURE_MIPMAPS) {
        uint32_t full = 1;
        uint32_t dim = std::max(desc.width, desc.height);
        while (dim > 1 && full < 16) {
            dim >>= 1;
            full++;
        }
        return full;
    }
    if (desc.mip_levels > 1) return std::min<uint32_t>(desc.mip_levels, 16);
    return 1;
}

BgpuResult vkFail(VkResult r, const char* what) {
    return fail(BGPU_ERR_UNSUPPORTED, __FILE__, __LINE__,
                cat("Vulkan ", what, " 失败：VkResult=", static_cast<int>(r)));
}

uint32_t pickMemoryType(VkPhysicalDevice phys, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp{};
    g_vk.vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    }
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if (bits & (1u << i)) return i;
    }
    return 0;
}

/* 与 quad.vert / quad.frag 的 push_constant 块逐字段对应（112 字节）。
 * std430 布局（GLSL push_constant 块）：vec4 在 0，vec2 在 16，下一个 vec2 在 24，
 * 然后 vec4 从 32 开始 —— C++ 结构体必须逐字节对上，否则读到的是错位数据。
 * （ABI 2 时这里踩过：xf0 落在 24，GLSL 读 32。）
 *
 * ABI 3 把原来的 pad[2] 用成 flags（前排布不变，2D 用例逐字节不受影响），
 * 再接三组 vec4 放光照。总长 112 ≤ 规范保证的 maxPushConstantsSize 128。 */
struct PushConstants {
    float vp[4];          /* offset   0 */
    float target[2];      /* offset  16 */
    float flags[2];       /* offset  24：x = 透视校正（网格），y = 光照（ABI 3） */
    float xf0[4];         /* offset  32 */
    float xf1[4];         /* offset  48 */
    float light_dir[4];   /* offset  64：xyz = 指向光源的单位方向 */
    float light_color[4]; /* offset  80：rgb = 光色（可 > 1） */
    float light_amb[4];   /* offset  96：rgb = 环境光 */
};

/* 一个描述符池装多少个 set：一帧的 draw 笔数通常会超过它，超了就再开一个池。
 * 512 是"一次 vkAllocateDescriptorSets 批量拿"的粒度，不是上限。 */
constexpr uint32_t kDescSetsPerPool = 512;

/* 把一条命令的状态拍成 push constant。
 * 2D 与网格两条 draw 路径共用它 —— 光照/透视的字段只在这里算一次，
 * 免得两边各写一段、日后漂移（这正是 ABI 3 之前三端圆角漂移的老路）。 */
void fillPushConstants(const Cmd& cmd, bool perspective, uint32_t target_w, uint32_t target_h,
                       PushConstants& pc) {
    pc.vp[0] = cmd.vp[0];
    pc.vp[1] = cmd.vp[1];
    pc.vp[2] = cmd.vp[2];
    pc.vp[3] = cmd.vp[3];
    pc.target[0] = static_cast<float>(target_w);
    pc.target[1] = static_cast<float>(target_h);
    pc.flags[0] = perspective ? 1.0f : 0.0f;
    pc.flags[1] = (cmd.pipe->pipe_flags & BGPU_PIPE_LIGHTING) ? 1.0f : 0.0f;
    if (cmd.affineOn) {
        /* cmd.xf 的 6 个数就是 [a c e | b d f]，与 quad.vert 里两行的顺序一致，
         * 直接搬；别按列主序重排（第一版就这么错的，仿射用例整块画不出来）。 */
        pc.xf0[0] = cmd.xf[0];
        pc.xf0[1] = cmd.xf[1];
        pc.xf0[2] = cmd.xf[2];
        pc.xf0[3] = 1.0f;
        pc.xf1[0] = cmd.xf[3];
        pc.xf1[1] = cmd.xf[4];
        pc.xf1[2] = cmd.xf[5];
    } else {
        pc.xf0[0] = 1.0f;
        pc.xf1[1] = 1.0f;
    }
    /* 光照参数：即使管线没开 LIGHTING 也推进去（片元侧由 flags.y 决定用不用），
     * 于是"设了光再换管线"不需要额外的状态同步。 */
    for (int i = 0; i < 3; i++) {
        pc.light_dir[i] = cmd.light.dir[i];
        pc.light_color[i] = cmd.light.color[i];
        pc.light_amb[i] = cmd.light.ambient[i];
    }
}

inline bool trace2() {
    static const bool on = std::getenv("BINGPU_VK_TRACE") != nullptr;
    return on;
}

/* 每帧"1×→4× LOAD 预置 blit / pass 末 resolve"的次数与吞吐（0.16 的分账工具）。
 * 打开后只在 frame_end 打**一行**，不掺进逐笔日志。 */
inline bool traceResolve() {
    static const bool on = std::getenv("BINGPU_TRACE_RESOLVE") != nullptr;
    return on;
}

/* 0.16 的自动延迟策略（只对整窗尺寸的目标、一帧里第 2 趟起 defer）可以用环境变量
 * 关掉：量 A/B 时要一个"和 0.15.1 的 resolve 时机完全一样"的对照命令流。
 * 它不是质量开关 —— 只影响 resolve 的**时机**，两种设置的像素应当逐位相同。 */
inline bool noAutoDefer() {
    static const bool on = std::getenv("BINGPU_NO_AUTO_DEFER") != nullptr;
    return on;
}
/* 逐 pass 一行（尺寸 + CLEAR/LOAD + 采样数），用来核实"每帧几个整窗往返"。 */
inline bool tracePasses() {
    static const bool on = std::getenv("BINGPU_TRACE_PASSES") != nullptr;
    return on;
}

/* 直通 → 预乘（上传时做，raster_rules §9） */
void premultiply(uint8_t* px, size_t count) {
    for (size_t i = 0; i < count; i++) {
        uint8_t* p = px + i * 4;
        const uint32_t a = p[3];
        p[0] = static_cast<uint8_t>((p[0] * a + 127) / 255);
        p[1] = static_cast<uint8_t>((p[1] * a + 127) / 255);
        p[2] = static_cast<uint8_t>((p[2] * a + 127) / 255);
    }
}

/* 预乘 → 直通（回读时做，§9；alpha==0 输出 0） */
void unpremultiply(uint8_t* px, size_t count) {
    for (size_t i = 0; i < count; i++) {
        uint8_t* p = px + i * 4;
        const uint32_t a = p[3];
        if (a == 0) {
            p[0] = p[1] = p[2] = 0;
            continue;
        }
        p[0] = static_cast<uint8_t>(std::min<uint32_t>(255, (p[0] * 255u + a / 2) / a));
        p[1] = static_cast<uint8_t>(std::min<uint32_t>(255, (p[1] * 255u + a / 2) / a));
        p[2] = static_cast<uint8_t>(std::min<uint32_t>(255, (p[2] * 255u + a / 2) / a));
    }
}

/* ---------------------------------------------------------------
 * 对象
 * --------------------------------------------------------------- */
struct VkDeviceObj;
struct VkTextureObj;

struct VkDeviceObj final : Device {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
    VkCommandPool cmd_pool = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    /* 描述符池按帧复用：每帧开始时 vkResetDescriptorPool 全部回收。
     * 之所以要**多个池**：一笔 draw 一个 set（见 allocDescSet 的注释），一帧的笔数
     * 不设上限，池满了就再开一个。 */
    std::vector<VkDescriptorPool> desc_pools;
    VkPipelineLayout pipe_layout = VK_NULL_HANDLE;
    VkShaderModule vert = VK_NULL_HANDLE;
    VkShaderModule frag = VK_NULL_HANDLE;
    /* ABI 4：解析圆角那一对。模块建不出来就**不报错**，只是没有 BGPU_CAP_SHAPE，
     * 上层继续用覆盖率模板（与 GL 后端同一条退路）。 */
    VkShaderModule vert_shape = VK_NULL_HANDLE;
    VkShaderModule frag_shape = VK_NULL_HANDLE;
    bool caps_shape = false;
    VkSampler default_sampler = VK_NULL_HANDLE;
    VkFormat swap_format = VK_FORMAT_B8G8R8A8_UNORM;
    VkFence fence = VK_NULL_HANDLE;
    VkTextureObj* white = nullptr;   /* 1×1 预乘白：未绑纹理时的采样源 */
    BgpuTexture white_handle = BGPU_INVALID;
    bool windowed = false;
    bool lost = false;
    /* 0.14 质量档：msaa_max = 设备能给的上限（caps 上报），msaa = 本次实际选的档。
     * 默认 1/1 —— 不显式要时与 CPU 参考后端的像素合同一个字不变。 */
    uint32_t msaa_max = 1;
    uint32_t msaa = 1;

    /* 帧中销毁的纹理/缓冲：**不能立即释放**。
     *
     * 命令缓冲是"先录后执行"（docs/backends.md 的 0.13.0 那条：录的时候只是记命令，
     * 帧尾才真正提交）。录进去的 draw / barrier / 附件引用的是 VkImage / VkBuffer
     * 句柄，而调用方完全可以在帧中途 destroy 一张纹理（binUI 的模糊链换尺寸时就是
     * 这么干的）—— 那一刻立即 vkDestroyImage + vkFreeMemory，等于让还在命令缓冲里的
     * 命令去读一块已经还给驱动、并且很可能已经被下一次分配复用的显存。
     *
     * 实测（0.15.0 定位）：1× 时这块内存被同尺寸的新图复用，读出的是"错的但有效的"
     * 数据，画面看不太出来；4× 时被释放的还有 MSAA companion，GPU 当场页错误，
     * 驱动报 VK_ERROR_DEVICE_LOST，整帧命令全丢（binUI 的 svg-filter 页整页黑）。
     *
     * 所以录制期间只把句柄挂进这张"退役表"，等本帧提交完（submitNow 内部等 fence）
     * 再真正释放 —— 与 0.13.0 处理"顶点流扩容时旧 buffer 不能当场销毁"是同一条规矩。 */
    struct Retired {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
    };
    struct RetiredBuffer {
        VkBuffer buf = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        void* mapped = nullptr;
    };
    std::vector<Retired> retired;
    std::vector<RetiredBuffer> retired_buffers;
    uint32_t recording_cbs = 0;   /* 正在录制的命令缓冲数（>0 时销毁要推迟） */
    /* 0.16 分账：一帧里的 LOAD 预置 blit / resolve 次数与像素量（BINGPU_TRACE_RESOLVE） */
    uint32_t frame_blits = 0;
    uint32_t frame_resolves = 0;
    uint64_t frame_blit_px = 0;
    uint64_t frame_resolve_px = 0;
    uint32_t frame_blits_big = 0;     /* 其中"整窗尺寸"（画布 / 贴屏目标）的次数 */
    uint32_t frame_resolves_big = 0;
    void retire(VkImage image, VkImageView view, VkDeviceMemory mem) {
        retired.push_back(Retired{image, view, mem});
    }
    /* 真正释放退役表里的东西。只在"提交且等完"之后调用。 */
    void drainRetired() {
        for (const Retired& r : retired) {
            if (r.view) g_vk.vkDestroyImageView(dev, r.view, nullptr);
            if (r.image) g_vk.vkDestroyImage(dev, r.image, nullptr);
            if (r.mem) g_vk.vkFreeMemory(dev, r.mem, nullptr);
        }
        retired.clear();
        for (const RetiredBuffer& b : retired_buffers) {
            if (b.buf) g_vk.vkDestroyBuffer(dev, b.buf, nullptr);
            if (b.mem) g_vk.vkFreeMemory(dev, b.mem, nullptr);
        }
        retired_buffers.clear();
    }

    VkDeviceObj(Backend& b, uint32_t flags, uint32_t w, uint32_t h)
        : Device(b, BGPU_INVALID, flags, w, h) {}
    ~VkDeviceObj() override;
};

struct VkTextureObj final : Texture {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkFormat vfmt = VK_FORMAT_R8G8B8A8_UNORM;
    bool is_depth = false;
    /* 深度图 / 颜色目标的采样数（1 = 普通）。深度附件必须与颜色附件一致。 */
    uint32_t samples = 1;
    /* MSAA 颜色附件（companion）：只在设备 msaa > 1 且这张纹理当颜色目标时懒建。
     * 渲染写它，pass 结束时 resolve 回上面的单采样 image —— 于是采样/回读/uniform
     * 拿到的永远是单采样结果（docs/backends.md 的质量档一节）。 */
    VkImage msaa_image = VK_NULL_HANDLE;
    VkDeviceMemory msaa_mem = VK_NULL_HANDLE;
    VkImageView msaa_view = VK_NULL_HANDLE;
    VkImageLayout msaa_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t msaa_samples = 0;    /* companion 的采样数；0 = 还没建 */
    uint32_t msaa_w = 0, msaa_h = 0;
    VkFormat msaa_fmt = VK_FORMAT_UNDEFINED;
    /* 0.16 延迟 resolve 的两条状态（ABI 6）：
     *   msaa_valid      —— 4× companion 里的内容就是这张图的最新内容（LOAD 时不必再从
     *                      1× image 回灌）；只有 image 被外部写（上传 / 换交换链图）才失效。
     *   resolve_pending —— 1× image 落后于 companion（DEFER_RESOLVE 结束的 pass），
     *                      谁要采样它就得先 resolve。 */
    bool msaa_valid = false;
    bool resolve_pending = false;
    /* 一帧里这张目标被 begin_render 了几次（0.16 自动延迟策略用）：
     * 第 2 趟起才 defer —— 只进出一次的目标 defer 不省任何东西（结束时终究要 resolve），
     * 反而把 resolve 推迟到采样那一趟、多一次状态切换。 */
    uint32_t pass_frame_stamp = 0xFFFFFFFFu;
    uint32_t pass_count = 0;
    /* 交换链借出的目标是**别人的** image/view：析构时绝不能销毁（第一版就是
     * 把交换链的 image 当自己的销毁，窗口烟测直接崩）。 */
    bool own_image = true;
    VkDevice dev = VK_NULL_HANDLE;

    VkTextureObj(Backend& b, BgpuDevice owner) : Texture(b, owner) {}
    ~VkTextureObj() override {
        if (!dev) return;
        /* 帧中途销毁（见 VkDeviceObj::retired）：改成挂进退役表，等提交完再放。
         * 1× 下靠"同尺寸复用"侥幸不炸，4× 下 MSAA companion 被释放后 GPU 直接
         * 页错误（VK_ERROR_DEVICE_LOST），所以这一条对两个档都要成立。 */
        VkDeviceObj* owner = nullptr;
        if (device != BGPU_INVALID) {
            owner = static_cast<VkDeviceObj*>(lookupObject(device, kObjDevice));
        }
        const bool defer = owner && owner->recording_cbs > 0;
        auto release = [&](VkImage im, VkImageView vw, VkDeviceMemory mm) {
            if (!im && !vw && !mm) return;
            if (defer) {
                owner->retire(im, vw, mm);
            } else {
                if (vw) g_vk.vkDestroyImageView(dev, vw, nullptr);
                if (im) g_vk.vkDestroyImage(dev, im, nullptr);
                if (mm) g_vk.vkFreeMemory(dev, mm, nullptr);
            }
        };
        /* companion 是**我们自己**的，无论底层 image 是否借用都要销毁 */
        release(msaa_image, msaa_view, msaa_mem);
        if (own_image) release(image, view, mem);
    }
};

struct VkBufferObj final : Buffer {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDevice dev = VK_NULL_HANDLE;

    VkBufferObj(Backend& b, BgpuDevice owner) : Buffer(b, owner) {}
    ~VkBufferObj() override {
        if (!dev) return;
        /* 帧中途销毁：与纹理同一条规矩（见 VkDeviceObj::retired）。顶点流扩容
         * 那条路 0.13.0 是用"抄进新 buffer"解决的，这里兜住调用方自己 destroy。 */
        VkDeviceObj* owner = nullptr;
        if (device != BGPU_INVALID) {
            owner = static_cast<VkDeviceObj*>(lookupObject(device, kObjDevice));
        }
        if (owner && owner->recording_cbs > 0) {
            /* 缓冲不进图片退役表（那张表是 image/view/mem 三元组），单独排队 */
            owner->retired_buffers.push_back(VkDeviceObj::RetiredBuffer{buf, mem, mapped});
            if (mapped) g_vk.vkUnmapMemory(dev, mem);   /* 映射可以立刻撤 */
            return;
        }
        if (mapped) g_vk.vkUnmapMemory(dev, mem);
        if (buf) g_vk.vkDestroyBuffer(dev, buf, nullptr);
        if (mem) g_vk.vkFreeMemory(dev, mem, nullptr);
    }
};

struct VkSamplerObj final : Sampler {
    VkSampler sampler = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;

    VkSamplerObj(Backend& b, BgpuDevice owner) : Sampler(b, owner) {}
    ~VkSamplerObj() override {
        if (dev && sampler) g_vk.vkDestroySampler(dev, sampler, nullptr);
    }
};

/* 管线按 (布局, 混合, flags, 目标颜色格式, 深度格式) 懒建并缓存：
 * 动态渲染下附件格式是管线状态的一部分，所以格式进 key。 */
struct VkPipelineObj final : Pipeline {
    VkDevice dev = VK_NULL_HANDLE;
    std::map<uint64_t, VkPipeline> cache;

    VkPipelineObj(Backend& b, BgpuDevice owner) : Pipeline(b, owner) {}
    ~VkPipelineObj() override {
        if (!dev) return;
        for (auto& kv : cache) g_vk.vkDestroyPipeline(dev, kv.second, nullptr);
    }
};

struct VkSwapchainObj final : Swapchain {
    VkDevice vkdev = VK_NULL_HANDLE;
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;   /* 借设备的，不在这里销毁 */
    VkFormat vfmt = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D extent{0, 0};
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    uint32_t current = 0;
    bool has_current = false;
    /* 借给 frame_begin 的对象（基类的 `target` 存的是它的**句柄**；别同名遮蔽） */
    VkTextureObj* borrow = nullptr;
    VkBuffer readback = VK_NULL_HANDLE;
    VkDeviceMemory readback_mem = VK_NULL_HANDLE;
    void* readback_map = nullptr;
    uint64_t readback_bytes = 0;
    /* HEADLESS 交换链的"帧图像"：一张自有离屏纹理（0 = 没有 / 窗口模式）。
     * owns_images：views/sc 是不是**交换链的**。HEADLESS 时 views[0] 指向离屏纹理的
     * 视图，析构里再销毁一次就是 double free（踩过）。 */
    BgpuTexture offscreen = BGPU_INVALID;
    bool owns_images = false;

    VkSwapchainObj(Backend& b, BgpuDevice owner) : Swapchain(b, owner) {}
    ~VkSwapchainObj() override;
};

struct VkCmdObj final : Cmd {
    VkCommandBuffer cb = VK_NULL_HANDLE;
    bool recording = false;
    bool rendering = false;

    /* 本趟录制里**按笔分配**的 descriptor set（见 allocDescSet 的注释）。
     * desc_used 是游标；beginRecording 会整批回收，所以录制期间只增不减。 */
    std::vector<VkDescriptorSet> desc_sets;
    size_t desc_used = 0;
    VkDevice dev = VK_NULL_HANDLE;
    VkFormat color_fmt = VK_FORMAT_UNDEFINED;
    VkFormat depth_fmt = VK_FORMAT_UNDEFINED;
    /* 当前 pass 的采样数（1 或设备的 msaa 档）：管线缓存键必须带上它。 */
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    /* 当前 pass 结束时会不会 resolve（begin_render 定；DEFER_RESOLVE 时是 false）。 */
    bool pass_resolves = false;

    /* 顶点流：每帧重置、按需增长；draw_quads 追加后一次 draw_indexed 画出 */
    VkBuffer vbuf = VK_NULL_HANDLE;
    VkDeviceMemory vmem = VK_NULL_HANDLE;
    void* vmap = nullptr;
    uint64_t vcap = 0;
    uint64_t vused = 0;

    /* 六边形索引（TL,TR,BL / BL,TR,BR → 0,1,2 2,1,3），按需增长 */
    VkBuffer ibuf = VK_NULL_HANDLE;
    VkDeviceMemory imem = VK_NULL_HANDLE;
    uint64_t icap_quads = 0;

    /* 扩容时**退役**的顶点/索引缓冲。命令缓冲是"先录后执行"的：扩容那一刻，
     * 前面那些 draw 已经把 vkCmdBindVertexBuffers(旧 buf) 录进去了，旧 buf 必须在
     * 这趟提交真正跑完之前一直活着（销毁 = 已录命令引用野内存）。回收点在
     * beginRecording（此时上一趟已经 fence 等完）。 */
    struct RetiredBuffer {
        VkBuffer buf;
        VkDeviceMemory mem;
    };
    std::vector<RetiredBuffer> retired;
    void freeRetired() {
        for (const RetiredBuffer& r : retired) {
            if (r.buf) g_vk.vkDestroyBuffer(dev, r.buf, nullptr);
            if (r.mem) g_vk.vkFreeMemory(dev, r.mem, nullptr);
        }
        retired.clear();
    }

    /* 状态剔除（docs/layering.md §6.2 写的契约）：只有真的变了才记 vkCmd* */
    VkPipeline last_pipe = VK_NULL_HANDLE;
    bool last_depth_test = false, last_depth_write = false, last_cull = false;
    bool have_state = false;

    VkCmdObj(Backend& b, BgpuDevice owner, Device& d) : Cmd(b, owner, d) {}
    ~VkCmdObj() override {
        if (trace2()) std::fprintf(stderr, "[vk] ~VkCmdObj dev=%p cb=%p\n", (void*)dev, (void*)cb);
        if (!dev) return;
        if (vmap) g_vk.vkUnmapMemory(dev, vmem);
        if (vbuf) g_vk.vkDestroyBuffer(dev, vbuf, nullptr);
        if (vmem) g_vk.vkFreeMemory(dev, vmem, nullptr);
        if (ibuf) g_vk.vkDestroyBuffer(dev, ibuf, nullptr);
        if (imem) g_vk.vkFreeMemory(dev, imem, nullptr);
        freeRetired();
        /* cb 由设备销毁时随 command pool 一起回收（pool 非持久），这里不单 free */
    }
};

VkDeviceObj::~VkDeviceObj() {
    if (trace2()) std::fprintf(stderr, "[vk] ~VkDeviceObj dev=%p\n", (void*)dev);
    if (dev) {
        g_vk.vkDeviceWaitIdle(dev);
        drainRetired();   /* 退役表里剩下的（帧中途销毁、之后再没提交过）在这里放掉 */
        if (fence) g_vk.vkDestroyFence(dev, fence, nullptr);
        if (default_sampler) g_vk.vkDestroySampler(dev, default_sampler, nullptr);
        if (vert) g_vk.vkDestroyShaderModule(dev, vert, nullptr);
        if (frag) g_vk.vkDestroyShaderModule(dev, frag, nullptr);
        if (vert_shape) g_vk.vkDestroyShaderModule(dev, vert_shape, nullptr);
        if (frag_shape) g_vk.vkDestroyShaderModule(dev, frag_shape, nullptr);
        if (pipe_layout) g_vk.vkDestroyPipelineLayout(dev, pipe_layout, nullptr);
        for (VkDescriptorPool p : desc_pools) g_vk.vkDestroyDescriptorPool(dev, p, nullptr);
        if (set_layout) g_vk.vkDestroyDescriptorSetLayout(dev, set_layout, nullptr);
        if (cmd_pool) g_vk.vkDestroyCommandPool(dev, cmd_pool, nullptr);
        g_vk.vkDestroyDevice(dev, nullptr);
    }
    if (surface) g_vk.vkDestroySurfaceKHR(instance, surface, nullptr);
    if (instance) g_vk.vkDestroyInstance(instance, nullptr);
}

VkSwapchainObj::~VkSwapchainObj() {
    if (!vkdev) return;
    if (owns_images) {
        for (VkImageView v : views) g_vk.vkDestroyImageView(vkdev, v, nullptr);
        if (sc) g_vk.vkDestroySwapchainKHR(vkdev, sc, nullptr);
    }
    if (readback_map) g_vk.vkUnmapMemory(vkdev, readback_mem);
    if (readback) g_vk.vkDestroyBuffer(vkdev, readback, nullptr);
    if (readback_mem) g_vk.vkFreeMemory(vkdev, readback_mem, nullptr);
}

/* ---------------------------------------------------------------
 * 后端
 * --------------------------------------------------------------- */
class VkBackend final : public BackendBase {
public:
    BgpuBackendKind kind() const noexcept override { return BGPU_BACKEND_VULKAN; }
    const char* name() const noexcept override { return "vulkan"; }
    bool available() const noexcept override;
    const char* unavailableReason() const noexcept override;

    BgpuResult createDevice(const BgpuDeviceDesc& desc, BgpuDevice* out) override;
    void deviceInfo(Device& dev, BgpuDeviceInfo& out) override;
    void deviceCaps(Device& dev, BgpuCaps& out) override;
    void deviceStats(Device& dev, BgpuStats& out) override;
    bool deviceLost(Device& dev) override { return static_cast<VkDeviceObj&>(dev).lost; }
    BgpuResult deviceWaitIdle(Device& dev) override;

    BgpuResult createSwapchain(Device& dev, const BgpuSwapchainDesc& desc,
                               BgpuSwapchain* out) override;
    BgpuResult resizeSwapchain(Swapchain& sc, uint32_t w, uint32_t h) override;
    void swapchainInfo(Swapchain& sc, BgpuSwapchainInfo& out) override;
    BgpuResult frameRead(Swapchain& sc, void* dst, size_t dst_size,
                         uint32_t* out_stride) override;

    BgpuResult createTexture(Device& dev, const BgpuTextureDesc& desc,
                             BgpuTexture* out) override;
    void textureInfo(Texture& tex, BgpuTextureInfo& out) override;
    BgpuResult textureUpload(Texture& tex, const BgpuBox* box, const void* pixels,
                             uint32_t stride) override;
    BgpuResult textureRead(Texture& tex, void* dst, size_t dst_size,
                           uint32_t* out_stride) override;
    BgpuResult createBuffer(Device& dev, const BgpuBufferDesc& desc, BgpuBuffer* out) override;
    BgpuResult bufferUpdate(Buffer& buf, uint32_t offset, const void* data,
                            uint32_t size) override;
    BgpuResult createSampler(Device& dev, const BgpuSamplerDesc& desc, BgpuSampler* out) override;
    BgpuResult createPipeline(Device& dev, const BgpuPipelineDesc& desc,
                              BgpuPipeline* out) override;

    BgpuResult createCmd(Device& dev, BgpuCmd* out) override;
    BgpuResult frameBegin(Swapchain& sc, Cmd& cmd, BgpuTexture* out) override;
    BgpuResult frameEnd(Swapchain& sc, Cmd& cmd) override;
    BgpuResult beginRender(Cmd& cmd, Texture& target, Texture* depth,
                           const BgpuRenderBeginDesc& desc) override;
    BgpuResult endRender(Cmd& cmd) override;
    BgpuResult resolveTarget(Cmd& cmd, Texture& target) override;

protected:
    BgpuResult rasterQuads(Cmd& cmd, const BgpuVertex* verts, uint32_t quad_count) override;
    BgpuResult rasterShapes(Cmd& cmd, const BgpuShapeVertex* verts,
                            uint32_t quad_count) override;
    BgpuResult rasterIndexed(Cmd& cmd, Buffer& vb, uint32_t vertex_offset, Buffer& ib,
                             uint32_t index_count, uint32_t index_offset) override;
    BgpuResult prepareDepth(Texture& depth, Texture& target,
                            const BgpuRenderBeginDesc& desc) override;
    BgpuResult clearColor(Texture&, uint32_t) override { return BGPU_OK; }  /* clear 用 loadOp */
    void presentFrame(Swapchain&) override {}                              /* frameEnd 里做 */

private:
    BgpuResult transition(VkCmdObj& c, VkTextureObj& t, VkImageLayout want);
    BgpuResult transitionImpl(VkCmdObj& c, VkTextureObj& t, VkImageLayout want, bool force);
    /* 只对一段 mip 范围插 barrier（mip 生成时逐级换布局用）。 */
    void barrierRange(VkCmdObj& c, VkTextureObj& t, VkImageLayout from, VkImageLayout to,
                      uint32_t base_level, uint32_t level_count);
    /* 懒建 / 复用 MSAA 颜色 companion（失败返回 false）。 */
    bool ensureMsaa(VkTextureObj& t, VkDeviceObj& d);
    void transitionMsaa(VkCmdObj& c, VkTextureObj& t, VkImageLayout want);
    /* 0.16 延迟 resolve：把挂起的 companion→1× resolve 立刻补上（必要时暂停/恢复
     * 当前渲染实例）。返回 OK 时 t.resolve_pending 一定是 false。 */
    BgpuResult resolvePending(VkCmdObj& c, VkDeviceObj& d, VkTextureObj& t);
    /* 把当前渲染实例收起来 / 原样开回去（自愈 resolve 要插在两次 vkCmd*Rendering
     * 之间）。收起来的 pass 一律用 LOAD 恢复：clear 已经落进附件，内容不能丢。 */
    void suspendPass(VkCmdObj& c);
    BgpuResult resumePass(VkCmdObj& c);
    bool createDescPool(VkDeviceObj& d);
    VkDescriptorSet allocDescSet(VkCmdObj& c, VkDeviceObj& d);
    void resetDescSets(VkCmdObj& c, VkDeviceObj& d);
    bool beginRecording(VkCmdObj& c, VkDeviceObj& dev);
    VkPipeline pipelineFor(VkPipelineObj& p, VkDeviceObj& dev, VkFormat color_fmt,
                           VkFormat depth_fmt, VkSampleCountFlagBits samples);
    bool ensureVertexSpace(VkCmdObj& c, VkDeviceObj& dev, uint64_t need);
    bool ensureIndexSpace(VkCmdObj& c, VkDeviceObj& dev, uint64_t quads);
    bool allocHostBuffer(VkDeviceObj& dev, uint64_t bytes, VkBuffer* buf, VkDeviceMemory* mem,
                         void** map);
    bool submitNow(VkDeviceObj& dev, VkCmdObj& c);
    bool copyTextureToHost(VkDeviceObj& dev, VkTextureObj& t, void* dst, size_t dst_size,
                           uint32_t* out_stride, bool convert_channels);
    bool ensureSwapchain(Swapchain& sc, uint32_t w, uint32_t h);
};

bool VkBackend::available() const noexcept {
    if (!loadVulkan()) {
        if (std::getenv("BINGPU_VK_TRACE")) std::fprintf(stderr, "[vk] loadVulkan 失败\n");
        return false;
    }
    VkApplicationInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &ai;
    VkInstance inst = VK_NULL_HANDLE;
    VkResult r = g_vk.vkCreateInstance(&ici, nullptr, &inst);
    if (r != VK_SUCCESS) {
        if (std::getenv("BINGPU_VK_TRACE"))
            std::fprintf(stderr, "[vk] vkCreateInstance(VK_API_VERSION_1_3) = %d\n", (int)r);
        return false;
    }
    uint32_t n = 0;
    r = g_vk.vkEnumeratePhysicalDevices(inst, &n, nullptr);
    g_vk.vkDestroyInstance(inst, nullptr);
    if (std::getenv("BINGPU_VK_TRACE"))
        std::fprintf(stderr, "[vk] enumeratePhysicalDevices r=%d n=%u\n", (int)r, n);
    return r == VK_SUCCESS && n > 0;
}

const char* VkBackend::unavailableReason() const noexcept {
    if (!loadVulkan()) return "vulkan-1.dll 加载失败（没装显卡驱动？）";
    return "没有可用的 Vulkan 物理设备";
}

/* ---------------------------------------------------------------
 * 设备
 * --------------------------------------------------------------- */
BgpuResult VkBackend::createDevice(const BgpuDeviceDesc& desc, BgpuDevice* out) {
    if (!loadVulkan()) return fail(BGPU_ERR_UNSUPPORTED, __FILE__, __LINE__, unavailableReason());


    VkApplicationInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.pApplicationName = "binGPU";
    ai.apiVersion = VK_API_VERSION_1_3;
    const char* exts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &ai;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = exts;
    VkInstance instance = VK_NULL_HANDLE;
    VkResult vr = g_vk.vkCreateInstance(&ici, nullptr, &instance);
    if (vr != VK_SUCCESS) return vkFail(vr, "vkCreateInstance");

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (desc.native_window) {
        VkWin32SurfaceCreateInfoKHR sci{};
        sci.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        sci.hinstance = ::GetModuleHandleA(nullptr);
        sci.hwnd = static_cast<HWND>(desc.native_window);
        vr = g_vk.vkCreateWin32SurfaceKHR(instance, &sci, nullptr, &surface);
        if (vr != VK_SUCCESS) {
            g_vk.vkDestroyInstance(instance, nullptr);
            return vkFail(vr, "vkCreateWin32SurfaceKHR");
        }
    }

    uint32_t dev_count = 0;
    if (g_vk.vkEnumeratePhysicalDevices(instance, &dev_count, nullptr) != VK_SUCCESS ||
        dev_count == 0) {
        if (surface) g_vk.vkDestroySurfaceKHR(instance, surface, nullptr);
        g_vk.vkDestroyInstance(instance, nullptr);
        return fail(BGPU_ERR_UNSUPPORTED, __FILE__, __LINE__, "没有 Vulkan 物理设备");
    }
    std::vector<VkPhysicalDevice> phys(dev_count);
    g_vk.vkEnumeratePhysicalDevices(instance, &dev_count, phys.data());

    VkPhysicalDevice chosen = VK_NULL_HANDLE;
    uint32_t qfam = 0;
    for (VkPhysicalDevice p : phys) {
        VkPhysicalDeviceProperties props{};
        g_vk.vkGetPhysicalDeviceProperties(p, &props);
        if (props.apiVersion < VK_API_VERSION_1_3) continue;   /* 动态渲染走 1.3 core */
        uint32_t qn = 0;
        g_vk.vkGetPhysicalDeviceQueueFamilyProperties(p, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        g_vk.vkGetPhysicalDeviceQueueFamilyProperties(p, &qn, qs.data());
        for (uint32_t i = 0; i < qn; i++) {
            if (!(qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            if (surface) {
                VkBool32 support = VK_FALSE;
                g_vk.vkGetPhysicalDeviceSurfaceSupportKHR(p, i, surface, &support);
                if (!support) continue;
            }
            chosen = p;
            qfam = i;
            break;
        }
        if (chosen != VK_NULL_HANDLE) break;
    }
    if (chosen == VK_NULL_HANDLE) {
        if (surface) g_vk.vkDestroySurfaceKHR(instance, surface, nullptr);
        g_vk.vkDestroyInstance(instance, nullptr);
        return fail(BGPU_ERR_UNSUPPORTED, __FILE__, __LINE__,
                    "没有满足条件的物理设备（需要 Vulkan 1.3 + 图形队列 [+ present]）");
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = qfam;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkPhysicalDeviceVulkan13Features f13{};
    f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    f13.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2{};
    f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    f2.pNext = &f13;
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = &f2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    const char* dev_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dev_exts;
    VkDevice vkdev = VK_NULL_HANDLE;
    vr = g_vk.vkCreateDevice(chosen, &dci, nullptr, &vkdev);
    if (vr != VK_SUCCESS) {
        if (surface) g_vk.vkDestroySurfaceKHR(instance, surface, nullptr);
        g_vk.vkDestroyInstance(instance, nullptr);
        return vkFail(vr, "vkCreateDevice(dynamicRendering)");
    }


    const uint64_t handle = makeObject<VkDeviceObj>(*this, desc.flags, desc.width, desc.height);

    if (handle == BGPU_INVALID) {
        g_vk.vkDestroyDevice(vkdev, nullptr);
        if (surface) g_vk.vkDestroySurfaceKHR(instance, surface, nullptr);
        g_vk.vkDestroyInstance(instance, nullptr);
        return failOom(__FILE__, __LINE__);
    }
    VkDeviceObj* d = static_cast<VkDeviceObj*>(lookupObject(handle, kObjDevice));
    d->self = handle;
    d->instance = instance;
    d->phys = chosen;
    d->dev = vkdev;
    d->surface = surface;
    d->queue_family = qfam;
    d->windowed = desc.native_window != nullptr;
    g_vk.vkGetDeviceQueue(vkdev, qfam, 0, &d->queue);

    /* 0.14 质量档：设备能给到的 MSAA 上限。颜色与深度两种附件都要支持同一个采样数
     * （深度图在 prepareDepth 里按同一个档建），所以取交集。对外只承诺 4 / 8 ——
     * BgpuDeviceDesc 的校验只接受这两个值。默认（desc.msaa_samples 为 0/1）仍是 1。 */
    {
        VkPhysicalDeviceProperties props{};
        g_vk.vkGetPhysicalDeviceProperties(chosen, &props);
        const VkSampleCountFlags common = props.limits.framebufferColorSampleCounts &
                                          props.limits.framebufferDepthSampleCounts;
        if (common & VK_SAMPLE_COUNT_8_BIT) {
            d->msaa_max = 8;
        } else if (common & VK_SAMPLE_COUNT_4_BIT) {
            d->msaa_max = 4;
        } else {
            d->msaa_max = 1;
        }
        const uint32_t want = desc.msaa_samples ? desc.msaa_samples : 1;
        if (want <= 1) {
            d->msaa = 1;
        } else if (d->msaa_max >= want) {
            d->msaa = want;
        } else if (d->msaa_max >= 4) {
            d->msaa = 4;   /* 要 8 但设备只到 4：降级到能给的最高档 */
        } else {
            d->msaa = 1;
        }
        if (std::getenv("BINGPU_VK_TRACE")) {
            std::fprintf(stderr, "[vk] MSAA：要 %u，设备最高 %u，实际 %u\n", want, d->msaa_max,
                         d->msaa);
        }
    }

    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = qfam;
    vr = g_vk.vkCreateCommandPool(vkdev, &pci, nullptr, &d->cmd_pool);
    if (vr != VK_SUCCESS) {
        destroyObject(handle);
        return vkFail(vr, "vkCreateCommandPool");
    }

    VkDescriptorSetLayoutBinding b{};
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dsl{};
    dsl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsl.bindingCount = 1;
    dsl.pBindings = &b;
    vr = g_vk.vkCreateDescriptorSetLayout(vkdev, &dsl, nullptr, &d->set_layout);
    if (vr != VK_SUCCESS) {
        destroyObject(handle);
        return vkFail(vr, "vkCreateDescriptorSetLayout");
    }

    /* 描述符池：先开一个，之后按需增长（allocDescSet）。一笔 draw 一个 set ——
     * 见那里关于"set 内容在执行期读取"的注释。 */
    if (!createDescPool(*d)) {
        destroyObject(handle);
        return vkFail(VK_ERROR_OUT_OF_HOST_MEMORY, "vkCreateDescriptorPool");
    }

    VkPushConstantRange pcr{};
    /* ABI 3 起片元着色器也要读（flags / light*），所以范围覆盖两个阶段 */
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcr.size = sizeof(PushConstants);
    VkPipelineLayoutCreateInfo plc{};
    plc.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plc.setLayoutCount = 1;
    plc.pSetLayouts = &d->set_layout;
    plc.pushConstantRangeCount = 1;
    plc.pPushConstantRanges = &pcr;
    vr = g_vk.vkCreatePipelineLayout(vkdev, &plc, nullptr, &d->pipe_layout);
    if (vr != VK_SUCCESS) {
        destroyObject(handle);
        return vkFail(vr, "vkCreatePipelineLayout");
    }

    VkShaderModuleCreateInfo smi{};
    smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smi.codeSize = sizeof(vkshader::kQuadVertSpv);
    smi.pCode = vkshader::kQuadVertSpv;
    vr = g_vk.vkCreateShaderModule(vkdev, &smi, nullptr, &d->vert);
    if (vr != VK_SUCCESS) {
        destroyObject(handle);
        return vkFail(vr, "vkCreateShaderModule(vert)");
    }
    smi.codeSize = sizeof(vkshader::kQuadFragSpv);
    smi.pCode = vkshader::kQuadFragSpv;
    vr = g_vk.vkCreateShaderModule(vkdev, &smi, nullptr, &d->frag);
    if (vr != VK_SUCCESS) {
        destroyObject(handle);
        return vkFail(vr, "vkCreateShaderModule(frag)");
    }

    /* ABI 4 的解析圆角：**失败不致命**。驱动/工具链把 SPIR-V 拒了，就只是没有
     * BGPU_CAP_SHAPE —— bin2d 会照旧走覆盖率模板，像素不会坏。 */
    {
        VkResult sr = VK_SUCCESS;
        smi.codeSize = sizeof(vkshader::kShapeVertSpv);
        smi.pCode = vkshader::kShapeVertSpv;
        sr = g_vk.vkCreateShaderModule(vkdev, &smi, nullptr, &d->vert_shape);
        if (sr == VK_SUCCESS) {
            smi.codeSize = sizeof(vkshader::kShapeFragSpv);
            smi.pCode = vkshader::kShapeFragSpv;
            sr = g_vk.vkCreateShaderModule(vkdev, &smi, nullptr, &d->frag_shape);
        }
        d->caps_shape = (sr == VK_SUCCESS);
        if (!d->caps_shape) {
            if (d->vert_shape) {
                g_vk.vkDestroyShaderModule(vkdev, d->vert_shape, nullptr);
                d->vert_shape = VK_NULL_HANDLE;
            }
            if (d->frag_shape) {
                g_vk.vkDestroyShaderModule(vkdev, d->frag_shape, nullptr);
                d->frag_shape = VK_NULL_HANDLE;
            }
            if (trace2()) std::fprintf(stderr, "[vk] shape 着色器模块创建失败（%d），没有 CAP_SHAPE\n",
                                      static_cast<int>(sr));
        }
    }

    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vr = g_vk.vkCreateFence(vkdev, &fci, nullptr, &d->fence);
    if (vr != VK_SUCCESS) {
        destroyObject(handle);
        return vkFail(vr, "vkCreateFence");
    }


    /* 默认采样器由**设备自己**持有（不是 Sampler 对象）：对象会被 destroyOwnedObjects
     * 先销毁，设备析构再销毁一次就是 double free（第一版就这么崩的）。 */
    {
        VkSamplerCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter = VK_FILTER_LINEAR;
        sci.minFilter = VK_FILTER_LINEAR;
        /* 默认采样器也打开 mipmap LINEAR：没有 mip 链的纹理 LOD 只会取到 0 级，
         * 像素与以前完全一样；带链的纹理由这里按 LOD 取级（否则 maxLod=0 恒取 0 级）。 */
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.minLod = 0.0f;
        sci.maxLod = VK_LOD_CLAMP_NONE;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        const VkResult sr = g_vk.vkCreateSampler(vkdev, &sci, nullptr, &d->default_sampler);

        if (sr != VK_SUCCESS) {
            destroyObject(handle);
            return vkFail(sr, "vkCreateSampler(default)");
        }
    }
    BgpuResult r = BGPU_OK;

    /* 1×1 预乘白：未绑纹理时采样它（等价于"白色纹理"，raster_rules §6） */
    const uint8_t white_px[4] = {255, 255, 255, 255};
    BgpuTextureDesc td{};
    td.struct_size = sizeof(td);
    td.width = 1;
    td.height = 1;
    td.format = BGPU_FORMAT_RGBA8;
    td.flags = BGPU_TEXTURE_CPU_READ;
    r = createTexture(*d, td, &d->white_handle);
    if (r != BGPU_OK) {
        destroyObject(handle);
        return r;
    }
    d->white = static_cast<VkTextureObj*>(lookupObject(d->white_handle, kObjTexture));

    r = textureUpload(*d->white, nullptr, white_px, 4);

    if (r != BGPU_OK) {
        destroyObject(handle);
        return r;
    }

    *out = handle;
    if (std::getenv("BINGPU_VK_TRACE")) {
        VkPhysicalDeviceProperties props{};
        g_vk.vkGetPhysicalDeviceProperties(chosen, &props);
        std::fprintf(stderr, "[vk] 设备就绪：%s（api=%u.%u.%u，窗口=%d）\n", props.deviceName,
                     VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
                     VK_VERSION_PATCH(props.apiVersion), (int)d->windowed);
    }
    return BGPU_OK;
}

void VkBackend::deviceInfo(Device& dev, BgpuDeviceInfo& out) {
    VkDeviceObj& d = static_cast<VkDeviceObj&>(dev);
    VkPhysicalDeviceProperties props{};
    g_vk.vkGetPhysicalDeviceProperties(d.phys, &props);
    out.backend = BGPU_BACKEND_VULKAN;
    out.surface_format =
        d.swap_format == VK_FORMAT_B8G8R8A8_UNORM ? BGPU_FORMAT_BGRA8 : BGPU_FORMAT_RGBA8;
    out.api_version = props.apiVersion;
    std::snprintf(out.adapter_name, sizeof(out.adapter_name), "%s", props.deviceName);
    std::snprintf(out.driver_name, sizeof(out.driver_name), "%u.%u.%u", props.driverVersion >> 22,
                  (props.driverVersion >> 12) & 0x3FFu, props.driverVersion & 0xFFFu);
}

void VkBackend::deviceCaps(Device& dev, BgpuCaps& out) {
    VkDeviceObj& d = static_cast<VkDeviceObj&>(dev);
    out.flags = BGPU_CAP_DEPTH | BGPU_CAP_OFFSCREEN | BGPU_CAP_READBACK | BGPU_CAP_PRESENT_PACES |
                BGPU_CAP_VSYNC | BGPU_CAP_SAMPLERS | BGPU_CAP_AFFINE | BGPU_CAP_MESH |
                /* ABI 3：光照（片元里做 Lambert）与透视校正（顶点带 w）都已实现 */
                BGPU_CAP_LIGHTING | BGPU_CAP_PERSPECTIVE;
    /* ABI 4：解析圆角 —— 片元着色器里算 SDF 覆盖率。模块建不出来就没有这一位。 */
    if (d.caps_shape) out.flags |= BGPU_CAP_SHAPE;
    /* 下一版（ABI 仍 4，只加枚举值）：真 mip 链与 MSAA 质量档。
     * mip 链走 vkCmdBlitImage 逐级降采样，固定管线不需要 LOD 输入（隐式 LOD）。 */
    out.flags |= BGPU_CAP_MIPMAPS;
    if (d.msaa_max >= 4) out.flags |= BGPU_CAP_MSAA;
    out.max_texture_size = BGPU_MAX_TEXTURE_SIZE;
    out.max_frames_in_flight = 1;   /* 每帧 submit + 等 fence */
    /* 能力上限，不是"已开"：实际档位看交换链信息。默认设备仍报 1（没开）。 */
    out.msaa_samples = d.msaa_max;
    out.max_quads_per_draw = BGPU_MAX_QUADS_PER_DRAW;
}

void VkBackend::deviceStats(Device& dev, BgpuStats& out) {
    out.frame_index = dev.frame_index;
    out.frames_presented = dev.frames_presented;
    out.draws = dev.draws;
    out.quads = dev.quads;
    out.triangles = dev.tris;
    out.bytes_uploaded = dev.bytes_uploaded;
    out.bytes_live = dev.bytes_live;
}

/* 等 GPU 画完：VK 有原语，直接 vkDeviceWaitIdle（比插栅栏简单且语义足够）。 */
BgpuResult VkBackend::deviceWaitIdle(Device& dev) {
    VkDeviceObj& d = static_cast<VkDeviceObj&>(dev);
    if (!d.dev) return BGPU_OK;
    g_vk.vkDeviceWaitIdle(d.dev);
    return BGPU_OK;
}

/* ---------------------------------------------------------------
 * 纹理
 * --------------------------------------------------------------- */
BgpuResult VkBackend::createTexture(Device& dev, const BgpuTextureDesc& desc,
                                    BgpuTexture* out) {
    VkDeviceObj& d = static_cast<VkDeviceObj&>(dev);
    if (desc.width == 0 || desc.height == 0) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__, "纹理尺寸不能为 0");
    }
    if (desc.width > BGPU_MAX_TEXTURE_SIZE || desc.height > BGPU_MAX_TEXTURE_SIZE) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__,
                    cat("纹理超过上限 ", BGPU_MAX_TEXTURE_SIZE));
    }
    /* mip：默认 1 级（与 CPU/D3D11 的默认路径逐位一致）。带 BGPU_TEXTURE_MIPMAPS 时
     * 建**真链**（floor(log2(max(w,h)))+1，上限 16），上传后逐级 blit 生成；
     * 采样器的 mipmap mode 也在这条路上打开（见 createSampler）。 */
    const uint32_t levels = mipLevelsFor(desc);
    const uint64_t handle = makeObject<VkTextureObj>(*this, d.self);
    if (handle == BGPU_INVALID) return failOom(__FILE__, __LINE__);
    VkTextureObj* t = static_cast<VkTextureObj*>(lookupObject(handle, kObjTexture));
    t->w = desc.width;
    t->h = desc.height;
    t->fmt = desc.format;
    t->tex_flags = desc.flags;
    t->mip_levels = levels;
    t->dev = d.dev;
    t->vfmt = toVkFormat(desc.format);

    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = t->vfmt;
    ici.extent = {desc.width, desc.height, 1};
    ici.mipLevels = levels;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT;
    if ((desc.flags & BGPU_TEXTURE_RENDER_TARGET) && desc.format != BGPU_FORMAT_R8) {
        ici.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult vr = g_vk.vkCreateImage(d.dev, &ici, nullptr, &t->image);
    if (vr != VK_SUCCESS) {
        destroyObject(handle);
        return vkFail(vr, "vkCreateImage");
    }
    VkMemoryRequirements req{};
    g_vk.vkGetImageMemoryRequirements(d.dev, t->image, &req);
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = pickMemoryType(d.phys, req.memoryTypeBits, 0);
    vr = g_vk.vkAllocateMemory(d.dev, &mai, nullptr, &t->mem);
    if (vr != VK_SUCCESS) {
        destroyObject(handle);
        return vkFail(vr, "vkAllocateMemory(image)");
    }
    vr = g_vk.vkBindImageMemory(d.dev, t->image, t->mem, 0);
    if (vr != VK_SUCCESS) {
        destroyObject(handle);
        return vkFail(vr, "vkBindImageMemory");
    }
    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = t->image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = t->vfmt;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
    vr = g_vk.vkCreateImageView(d.dev, &vci, nullptr, &t->view);
    if (vr != VK_SUCCESS) {
        destroyObject(handle);
        return vkFail(vr, "vkCreateImageView");
    }
    t->bytes = static_cast<uint64_t>(desc.width) * desc.height * bingpu_format_bytes(desc.format);
    dev.bytes_live += t->bytes;
    *out = handle;
    return BGPU_OK;
}

void VkBackend::textureInfo(Texture& tex, BgpuTextureInfo& out) {
    out.width = tex.w;
    out.height = tex.h;
    out.format = tex.fmt;
    out.flags = tex.tex_flags;
    out.mip_levels = tex.mip_levels;
}

BgpuResult VkBackend::transition(VkCmdObj& c, VkTextureObj& t, VkImageLayout want) {
    return transitionImpl(c, t, want, /*force=*/false);
}

/* force=true：**布局不变也插 barrier**。用于"同一个目标连续开两个渲染实例"——
 * 第一个实例的写入必须在第二个实例的 load 之前落地（RAW）。动态渲染的
 * vkCmdBeginRendering/vkCmdEndRendering 自己**不插**任何依赖，靠布局变化带出来的
 * barrier 又会在布局相同的时候被跳过，于是第二趟的 load 可能读到旧内容。 */
BgpuResult VkBackend::transitionImpl(VkCmdObj& c, VkTextureObj& t, VkImageLayout want,
                                     bool force) {
    if (t.layout == want && !force) return BGPU_OK;
    const VkImageAspectFlags aspect =
        t.is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = t.layout;
    b.newLayout = want;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = t.image;
    /* 整张图一起转（mip 链的所有级；非 mip 纹理就是 1 级，与以前逐位相同） */
    b.subresourceRange = {aspect, 0, t.mip_levels, 0, 1};

    VkPipelineStageFlags src_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkPipelineStageFlags dst_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    switch (want) {
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT |
                              VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            b.dstAccessMask =
                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            dst_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            break;
        case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
            b.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                              VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            dst_stage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
            break;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            dst_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            break;
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            /* 0.16：resolve 也要走这条（写 1× 镜像）。以前 srcAccess=0 + TOP_OF_PIPE
             * 是给"新建图第一次上传"用的；对已渲染过的图太弱（WAR 不成立），
             * 这里收回成"等所有先前的写/读落地"，对上传那条路只是多一次等待。 */
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT |
                              VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
            b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            break;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
            b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            break;
        case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
            b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            b.dstAccessMask = 0;
            src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            dst_stage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
            break;
        default:
            return fail(BGPU_ERR_BACKEND_FAILED, __FILE__, __LINE__, "未知的目标布局");
    }
    if (t.layout == VK_IMAGE_LAYOUT_UNDEFINED) b.srcAccessMask = 0;
    g_vk.vkCmdPipelineBarrier(c.cb, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
    t.layout = want;
    return BGPU_OK;
}

/* 单段 barrier（mip 生成时每一级的布局是**分开**的，不能整张图一把转）。
 * 只覆盖 mip 生成用到的三种迁移；其它组合走 transitionImpl。 */
void VkBackend::barrierRange(VkCmdObj& c, VkTextureObj& t, VkImageLayout from,
                             VkImageLayout to, uint32_t base_level, uint32_t level_count) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = t.image;
    b.subresourceRange = {t.is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT,
                          base_level, level_count, 0, 1};
    VkPipelineStageFlags src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkPipelineStageFlags dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    if (from == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    } else if (from == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    } else {
        b.srcAccessMask = 0;
    }
    if (to == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    } else if (to == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    } else if (to == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        dst_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else {
        b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    }
    g_vk.vkCmdPipelineBarrier(c.cb, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

bool VkBackend::createDescPool(VkDeviceObj& d) {
    VkDescriptorPoolSize psz{};
    psz.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    psz.descriptorCount = kDescSetsPerPool;
    VkDescriptorPoolCreateInfo dpc{};
    dpc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpc.maxSets = kDescSetsPerPool;
    dpc.poolSizeCount = 1;
    dpc.pPoolSizes = &psz;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (g_vk.vkCreateDescriptorPool(d.dev, &dpc, nullptr, &pool) != VK_SUCCESS) return false;
    d.desc_pools.push_back(pool);
    return true;
}

/* 一笔 draw 一个 descriptor set。
 *
 * 为什么必须按笔分配：descriptor set 的内容是**执行期**读取的，不是录制期快照。
 * 整个 cmd 共用一个 set、在两条 draw 之间 vkUpdateDescriptorSets 改写它，等于两条
 * draw 都去采样**最后一次写入**的那张纹理 —— 0.12.x 就是这样：一帧里换着采样
 * 一堆纹理（模糊降采样链、从当前帧目标抓背景）时，所有 draw 都读到同一张图，
 * 画面大面积错乱。GL 后端是逐笔真实绑定，所以它一直是对的。
 *
 * 每帧开始整批回收（resetDescSets），所以 set 数量按"一帧的笔数"走，不按帧累加。 */
VkDescriptorSet VkBackend::allocDescSet(VkCmdObj& c, VkDeviceObj& d) {
    if (c.desc_used < c.desc_sets.size()) return c.desc_sets[c.desc_used++];
    if (d.desc_pools.empty() && !createDescPool(d)) return VK_NULL_HANDLE;
    for (int attempt = 0; attempt < 4; attempt++) {
        std::vector<VkDescriptorSet> batch(kDescSetsPerPool);
        /* pSetLayouts 是**数组**（descriptorSetCount 个），不是单个 layout —— 批量
         * 分配时传一个指针进来，驱动会顺着往后读越界的句柄（当场 SIGSEGV）。 */
        const std::vector<VkDescriptorSetLayout> layouts(kDescSetsPerPool, d.set_layout);
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = d.desc_pools.back();
        ai.descriptorSetCount = kDescSetsPerPool;
        ai.pSetLayouts = layouts.data();
        if (g_vk.vkAllocateDescriptorSets(d.dev, &ai, batch.data()) == VK_SUCCESS) {
            c.desc_sets.insert(c.desc_sets.end(), batch.begin(), batch.end());
            return c.desc_sets[c.desc_used++];
        }
        /* 当前池满了（每池固定 kDescSetsPerPool 个）：再开一个池继续 */
        if (!createDescPool(d)) return VK_NULL_HANDLE;
    }
    return VK_NULL_HANDLE;
}

/* 回收本趟录制用掉的所有 set。调用点保证"上一帧已经 fence 等完"（frame_end /
 * 离屏 end_render 都会等），所以这里重置池是安全的。 */
void VkBackend::resetDescSets(VkCmdObj& c, VkDeviceObj& d) {
    if (c.desc_sets.empty()) return;
    for (VkDescriptorPool p : d.desc_pools) g_vk.vkResetDescriptorPool(d.dev, p, 0);
    c.desc_sets.clear();
    c.desc_used = 0;
}

BgpuResult VkBackend::textureUpload(Texture& tex, const BgpuBox* box, const void* pixels,
                                    uint32_t stride) {
    VkTextureObj& t = static_cast<VkTextureObj&>(tex);
    VkDeviceObj& d = static_cast<VkDeviceObj&>(*lookupObject(tex.device, kObjDevice));
    if (!pixels) return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__, "pixels 为空");
    const BgpuBox b = box ? *box : BgpuBox{0, 0, t.w, t.h};
    if (b.w == 0 || b.h == 0 || b.x + b.w > t.w || b.y + b.h > t.h) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__, "上传区域越界");
    }
    const uint32_t bpp = bingpu_format_bytes(t.fmt);
    const uint32_t src_stride = stride ? stride : b.w * bpp;
    const uint32_t bytes = b.w * b.h * bpp;

    /* 直通 → 预乘：**局部上传也要预乘**（CPU 是按区域预乘的；只在整张上传时预乘
     * 会让"局部更新的半透明贴图"混出偏亮的结果 —— 0.3.0 就是这么写的，
     * tests/gpu_parity.h 的 texture(box) 用例把它钉住了）。
     *
     * 两个例外连这块临时缓冲都不必分配（bin2d 的上屏路径每帧都走这条）：
     *   · 纹理带 BGPU_TEXTURE_PREMULTIPLIED（数据本来就是预乘，不用预乘）；
     *   · 行本来就紧凑（src_stride == b.w*bpp，不用压行）。
     * 都满足时直接把调用方的指针 memcpy 进暂存缓冲，省掉一次整幅中转。 */
    const bool premulInput = (t.tex_flags & BGPU_TEXTURE_PREMULTIPLIED) != 0;
    const uint8_t* src = static_cast<const uint8_t*>(pixels);
    std::vector<uint8_t> tmp;
    if (!premulInput || src_stride != b.w * bpp) {
        tmp.resize(bytes);
        for (uint32_t y = 0; y < b.h; y++) {
            std::memcpy(tmp.data() + static_cast<size_t>(y) * b.w * bpp,
                        static_cast<const uint8_t*>(pixels) + static_cast<size_t>(y) * src_stride,
                        static_cast<size_t>(b.w) * bpp);
        }
        if (!premulInput && bpp == 4) premultiply(tmp.data(), static_cast<size_t>(b.w) * b.h);
        src = tmp.data();
    }

    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    void* staging_map = nullptr;

    if (!allocHostBuffer(d, bytes, &staging, &staging_mem, &staging_map)) {
        return fail(BGPU_ERR_OUT_OF_MEMORY, __FILE__, __LINE__, "上传暂存分配失败");
    }
    std::memcpy(staging_map, src, bytes);

    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool = d.cmd_pool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    bool ok = g_vk.vkAllocateCommandBuffers(d.dev, &cbi, &cb) == VK_SUCCESS;
    if (ok) {
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        g_vk.vkBeginCommandBuffer(cb, &bi);

        /* 借一个临时的 Cmd 对象壳，只为复 transition() 里录命令那段 */
        VkCmdObj tmp_cmd(*this, d.self, d);
        tmp_cmd.cb = cb;
        /* 0.16：上传要写 1× 镜像。若这张图还挂着"延迟 resolve"，先把 companion 里
         * 的最新内容落进镜像，再叠上传 —— 否则上传块会被后来的 resolve 覆盖掉。 */
        if (t.resolve_pending) {
            const BgpuResult rr = resolvePending(tmp_cmd, d, t);
            if (rr != BGPU_OK) ok = false;
        }
        if (ok) transition(tmp_cmd, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageOffset = {static_cast<int32_t>(b.x), static_cast<int32_t>(b.y), 0};
        region.imageExtent = {b.w, b.h, 1};
        g_vk.vkCmdCopyBufferToImage(cb, staging, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    1, &region);
        if (t.mip_levels > 1) {
            /* 逐级 blit 降采样（LINEAR）：level i-1 → level i，直到 1×1。
             * 每级的布局单独转 —— 整张图的 barrier 无法表达"0 级已 SRC、末级仍 DST"。
             * 这是 BGPU_TEXTURE_MIPMAPS 唯一改变像素的地方，默认 1 级路径不进来。 */
            const uint32_t levels = t.mip_levels;
            for (uint32_t i = 1; i < levels; i++) {
                barrierRange(tmp_cmd, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, i - 1, 1);
                const uint32_t sw = std::max(1u, t.w >> (i - 1));
                const uint32_t sh = std::max(1u, t.h >> (i - 1));
                const uint32_t dw = std::max(1u, t.w >> i);
                const uint32_t dh = std::max(1u, t.h >> i);
                VkImageBlit blit{};
                blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 1};
                blit.srcOffsets[0] = {0, 0, 0};
                blit.srcOffsets[1] = {static_cast<int32_t>(sw), static_cast<int32_t>(sh), 1};
                blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
                blit.dstOffsets[0] = {0, 0, 0};
                blit.dstOffsets[1] = {static_cast<int32_t>(dw), static_cast<int32_t>(dh), 1};
                g_vk.vkCmdBlitImage(cb, t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.image,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                                    VK_FILTER_LINEAR);
            }
            for (uint32_t i = 0; i < levels; i++) {
                const VkImageLayout from = (i + 1 == levels)
                                               ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                                               : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                barrierRange(tmp_cmd, t, from, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, i, 1);
            }
            t.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        } else {
            transition(tmp_cmd, t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        g_vk.vkEndCommandBuffer(cb);
        g_vk.vkResetFences(d.dev, 1, &d.fence);
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        ok = g_vk.vkQueueSubmit(d.queue, 1, &si, d.fence) == VK_SUCCESS &&
             g_vk.vkWaitForFences(d.dev, 1, &d.fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
        g_vk.vkFreeCommandBuffers(d.dev, d.cmd_pool, 1, &cb);
    }
    g_vk.vkUnmapMemory(d.dev, staging_mem);
    g_vk.vkDestroyBuffer(d.dev, staging, nullptr);
    g_vk.vkFreeMemory(d.dev, staging_mem, nullptr);
    if (!ok) return fail(BGPU_ERR_BACKEND_FAILED, __FILE__, __LINE__, "上传提交失败");
    /* 镜像被外部写过：4× companion 不再是"最新内容"，下一趟 LOAD 必须重新回灌。 */
    t.msaa_valid = false;
    t.resolve_pending = false;
    d.bytes_uploaded += bytes;
    return BGPU_OK;
}

bool VkBackend::allocHostBuffer(VkDeviceObj& dev, uint64_t bytes, VkBuffer* buf,
                                VkDeviceMemory* mem, void** map) {
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (g_vk.vkCreateBuffer(dev.dev, &bci, nullptr, buf) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    g_vk.vkGetBufferMemoryRequirements(dev.dev, *buf, &req);
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex =
        pickMemoryType(dev.phys, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (g_vk.vkAllocateMemory(dev.dev, &mai, nullptr, mem) != VK_SUCCESS) return false;
    if (g_vk.vkBindBufferMemory(dev.dev, *buf, *mem, 0) != VK_SUCCESS) return false;
    if (g_vk.vkMapMemory(dev.dev, *mem, 0, VK_WHOLE_SIZE, 0, map) != VK_SUCCESS) return false;
    std::memset(*map, 0, static_cast<size_t>(bytes));
    return true;
}

bool VkBackend::copyTextureToHost(VkDeviceObj& dev, VkTextureObj& t, void* dst,
                                  size_t dst_size, uint32_t* out_stride,
                                  bool convert_channels) {
    const uint32_t bpp = bingpu_format_bytes(t.fmt);
    const uint32_t row = t.w * bpp;
    if (dst_size < static_cast<size_t>(row) * t.h) return false;

    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    void* map = nullptr;
    if (!allocHostBuffer(dev, static_cast<uint64_t>(row) * t.h, &buf, &mem, &map)) return false;

    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool = dev.cmd_pool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    bool ok = g_vk.vkAllocateCommandBuffers(dev.dev, &cbi, &cb) == VK_SUCCESS;
    if (ok) {
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        g_vk.vkBeginCommandBuffer(cb, &bi);
        VkCmdObj tmp_cmd(*this, dev.self, dev);
        tmp_cmd.cb = cb;
        /* 0.16：回读要的是 1× 内容 —— 挂起的 resolve 先补上。 */
        if (t.resolve_pending && resolvePending(tmp_cmd, dev, t) != BGPU_OK) ok = false;
        if (ok) {
            transition(tmp_cmd, t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {t.w, t.h, 1};
            g_vk.vkCmdCopyImageToBuffer(cb, t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1,
                                        &region);
            transition(tmp_cmd, t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        g_vk.vkEndCommandBuffer(cb);
        if (ok) {
            g_vk.vkResetFences(dev.dev, 1, &dev.fence);
            VkSubmitInfo si{};
            si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            si.commandBufferCount = 1;
            si.pCommandBuffers = &cb;
            ok = g_vk.vkQueueSubmit(dev.queue, 1, &si, dev.fence) == VK_SUCCESS &&
                 g_vk.vkWaitForFences(dev.dev, 1, &dev.fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
        }
        g_vk.vkFreeCommandBuffers(dev.dev, dev.cmd_pool, 1, &cb);
    }
    if (ok) {
        std::memcpy(dst, map, static_cast<size_t>(row) * t.h);
        if (bpp == 4) {
            unpremultiply(static_cast<uint8_t*>(dst), static_cast<size_t>(t.w) * t.h);
            if (convert_channels && t.vfmt == VK_FORMAT_B8G8R8A8_UNORM) {
                /* 交换链是 BGRA 时，frame_read 的输出必须仍是 RGBA（raster_rules §9） */
                uint8_t* p = static_cast<uint8_t*>(dst);
                for (size_t i = 0; i < static_cast<size_t>(t.w) * t.h; i++) {
                    std::swap(p[i * 4 + 0], p[i * 4 + 2]);
                }
            }
        }
    }
    g_vk.vkUnmapMemory(dev.dev, mem);
    g_vk.vkDestroyBuffer(dev.dev, buf, nullptr);
    g_vk.vkFreeMemory(dev.dev, mem, nullptr);
    if (out_stride) *out_stride = row;
    return ok;
}

BgpuResult VkBackend::textureRead(Texture& tex, void* dst, size_t dst_size,
                                  uint32_t* out_stride) {
    VkTextureObj& t = static_cast<VkTextureObj&>(tex);
    if (!(t.tex_flags & (BGPU_TEXTURE_CPU_READ | BGPU_TEXTURE_RENDER_TARGET))) {
        return fail(BGPU_ERR_STATE, __FILE__, __LINE__,
                    "纹理没有 CPU_READ / RENDER_TARGET，不能回读");
    }
    VkDeviceObj& d = static_cast<VkDeviceObj&>(*lookupObject(tex.device, kObjDevice));
    g_vk.vkDeviceWaitIdle(d.dev);
    if (!copyTextureToHost(d, t, dst, dst_size, out_stride, false)) {
        return fail(BGPU_ERR_BACKEND_FAILED, __FILE__, __LINE__, "回读失败");
    }
    return BGPU_OK;
}

BgpuResult VkBackend::createBuffer(Device& dev, const BgpuBufferDesc& desc, BgpuBuffer* out) {
    VkDeviceObj& d = static_cast<VkDeviceObj&>(dev);
    if (desc.size == 0) return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__, "size 为 0");
    const uint64_t handle = makeObject<VkBufferObj>(*this, d.self);
    if (handle == BGPU_INVALID) return failOom(__FILE__, __LINE__);
    VkBufferObj* b = static_cast<VkBufferObj*>(lookupObject(handle, kObjBuffer));
    b->size = desc.size;
    b->usage = desc.usage;
    b->dev = d.dev;
    if (!allocHostBuffer(d, desc.size, &b->buf, &b->mem, &b->mapped)) {
        destroyObject(handle);
        return fail(BGPU_ERR_OUT_OF_MEMORY, __FILE__, __LINE__, "buffer 分配失败");
    }
    b->bytes = desc.size;
    dev.bytes_live += b->bytes;
    *out = handle;
    return BGPU_OK;
}

BgpuResult VkBackend::bufferUpdate(Buffer& buf, uint32_t offset, const void* data,
                                   uint32_t size) {
    VkBufferObj& b = static_cast<VkBufferObj&>(buf);
    if (!data || size == 0) return BGPU_OK;
    if (offset + size > b.size) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__,
                    cat("buffer_update 越界：offset=", offset, " size=", size, " cap=", b.size));
    }
    std::memcpy(static_cast<uint8_t*>(b.mapped) + offset, data, size);
    return BGPU_OK;
}

BgpuResult VkBackend::createSampler(Device& dev, const BgpuSamplerDesc& desc,
                                    BgpuSampler* out) {
    VkDeviceObj& d = static_cast<VkDeviceObj&>(dev);
    const uint64_t handle = makeObject<VkSamplerObj>(*this, d.self);
    if (handle == BGPU_INVALID) return failOom(__FILE__, __LINE__);
    VkSamplerObj* s = static_cast<VkSamplerObj*>(lookupObject(handle, kObjSampler));
    s->desc = desc;
    s->dev = d.dev;
    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = desc.mag_filter == BGPU_FILTER_NEAREST ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    sci.minFilter = desc.min_filter == BGPU_FILTER_NEAREST ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    /* mip 滤法跟着 min 走（LINEAR → 三级线性，NEAREST → 最近级）。maxLod 必须显式
     * 设成 CLAMP_NONE：VkSamplerCreateInfo 零初始化时 maxLod = 0，采样永远只取 0 级，
     * 那样"有 mip 链"也等于没有。 */
    sci.mipmapMode = desc.min_filter == BGPU_FILTER_NEAREST ? VK_SAMPLER_MIPMAP_MODE_NEAREST
                                                            : VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.minLod = 0.0f;
    sci.maxLod = VK_LOD_CLAMP_NONE;
    sci.addressModeU = desc.wrap_u == BGPU_WRAP_REPEAT ? VK_SAMPLER_ADDRESS_MODE_REPEAT
                                                       : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = desc.wrap_v == BGPU_WRAP_REPEAT ? VK_SAMPLER_ADDRESS_MODE_REPEAT
                                                       : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = sci.addressModeV;
    sci.compareOp = VK_COMPARE_OP_NEVER;
    VkResult vr = g_vk.vkCreateSampler(d.dev, &sci, nullptr, &s->sampler);
    if (vr != VK_SUCCESS) {
        destroyObject(handle);
        return vkFail(vr, "vkCreateSampler");
    }
    *out = handle;
    return BGPU_OK;
}

BgpuResult VkBackend::createPipeline(Device& dev, const BgpuPipelineDesc& desc,
                                     BgpuPipeline* out) {
    VkDeviceObj& d = static_cast<VkDeviceObj&>(dev);
    const uint64_t handle = makeObject<VkPipelineObj>(*this, d.self);
    if (handle == BGPU_INVALID) return failOom(__FILE__, __LINE__);
    VkPipelineObj* p = static_cast<VkPipelineObj*>(lookupObject(handle, kObjPipeline));
    p->layout = desc.vertex_layout;
    p->blend = desc.blend;
    p->pipe_flags = desc.flags;
    p->dev = d.dev;
    *out = handle;
    return BGPU_OK;
}

/* ---------------------------------------------------------------
 * 管线：按 (布局, 混合, flags, 颜色格式, 深度格式) 建一次并缓存
 * --------------------------------------------------------------- */
VkPipeline VkBackend::pipelineFor(VkPipelineObj& p, VkDeviceObj& dev, VkFormat color_fmt,
                                  VkFormat depth_fmt, VkSampleCountFlagBits samples) {
    const uint64_t key = (static_cast<uint64_t>(color_fmt) << 40) |
                         (static_cast<uint64_t>(depth_fmt) << 24) |
                         (static_cast<uint64_t>(samples) << 20) |
                         (static_cast<uint64_t>(p.layout) << 16) |
                         (static_cast<uint64_t>(p.blend) << 8) | p.pipe_flags;
    auto it = p.cache.find(key);
    if (it != p.cache.end()) return it->second;

    /* 解析圆角走另一对着色器 + 另一份顶点输入（36 字节）。
     * 其余状态（栅格化/混合/深度/动态 viewport+scissor/push constant）**完全共用**，
     * 因为两边用的是同一个管线布局（dev.pipe_layout）：shape 着色器声明了同一块
     * push_constant 但不用其中的光照字段，也**不**声明采样器 —— 未使用的绑定
     * 不影响布局兼容性。 */
    const bool shape = (p.layout == BGPU_VERTEX_SHAPE);
    if (shape && (!dev.vert_shape || !dev.frag_shape)) return VK_NULL_HANDLE;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = shape ? dev.vert_shape : dev.vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = shape ? dev.frag_shape : dev.frag;
    stages[1].pName = "main";

    VkVertexInputBindingDescription bind{};
    VkVertexInputAttributeDescription attrs[6]{};
    VkPipelineVertexInputStateCreateInfo vin{};
    vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    if (shape) {
        /* BgpuShapeVertex（36）：x,y(0) lx,ly(8) hx,hy(16) radius(24) thick(28) tint(32) */
        bind.binding = 0;
        bind.stride = sizeof(BgpuShapeVertex);   /* 36（ABI 4 的静态断言钉住的） */
        bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        attrs[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, 0};      /* x, y    */
        attrs[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, 8};      /* lx, ly  */
        attrs[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT, 16};     /* hx, hy  */
        attrs[3] = {3, 0, VK_FORMAT_R32_SFLOAT, 24};        /* radius  */
        attrs[4] = {4, 0, VK_FORMAT_R32_SFLOAT, 28};        /* thick   */
        attrs[5] = {5, 0, VK_FORMAT_R8G8B8A8_UNORM, 32};    /* tint    */
        vin.vertexAttributeDescriptionCount = 6;
    } else if (p.layout == BGPU_VERTEX_2D) {
        bind.binding = 0;
        bind.stride = sizeof(BgpuVertex);   /* 24（ABI 3 未变） */
        bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
        attrs[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, 12};
        attrs[2] = {2, 0, VK_FORMAT_R8G8B8A8_UNORM, 20};
        /* location 3/4 是 ABI 3 的网格字段。2D 顶点没有它们，但着色器里声明了
         * 就必须由管线提供（验证层 VUID-...-Input-07904），所以指向本顶点的
         * 位置字节：值无意义、但**落在缓冲区内**；flags.x = 0 时用不到它们。 */
        attrs[3] = {3, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
        attrs[4] = {4, 0, VK_FORMAT_R32_SFLOAT, 0};
    } else {
        bind.binding = 0;
        bind.stride = sizeof(BgpuMeshVertex);   /* 40（ABI 3：末尾加了 w） */
        bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};     /* x, y, z   */
        attrs[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, 24};       /* u, v      */
        attrs[2] = {2, 0, VK_FORMAT_R8G8B8A8_UNORM, 32};      /* color     */
        attrs[3] = {3, 0, VK_FORMAT_R32G32B32_SFLOAT, 12};    /* nx, ny, nz */
        attrs[4] = {4, 0, VK_FORMAT_R32_SFLOAT, 36};          /* w         */
    }
    vin.vertexBindingDescriptionCount = 1;
    vin.pVertexBindingDescriptions = &bind;
    if (!shape) vin.vertexAttributeDescriptionCount = 5;   /* shape 的分支在上面设成 6 */
    vin.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = (p.pipe_flags & BGPU_PIPE_CULL_BACK) ? VK_CULL_MODE_BACK_BIT
                                                       : VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = samples;   /* 必须与 pass 的附件采样数一致 */

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = (p.pipe_flags & BGPU_PIPE_DEPTH_TEST) ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = (p.pipe_flags & BGPU_PIPE_DEPTH_WRITE) ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;   /* raster_rules §7：LESS，相等不通过 */

    VkPipelineColorBlendAttachmentState cb{};
    cb.colorWriteMask = 0xF;
    switch (p.blend) {
        case BGPU_BLEND_REPLACE:
            cb.blendEnable = VK_FALSE;
            break;
        case BGPU_BLEND_KEEP:
            cb.blendEnable = VK_TRUE;
            cb.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
            cb.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            cb.colorBlendOp = VK_BLEND_OP_ADD;
            cb.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            cb.dstAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            cb.alphaBlendOp = VK_BLEND_OP_ADD;
            break;
        case BGPU_BLEND_ERASE:
            cb.blendEnable = VK_TRUE;
            cb.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
            cb.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            cb.colorBlendOp = VK_BLEND_OP_ADD;
            cb.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            cb.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            cb.alphaBlendOp = VK_BLEND_OP_ADD;
            break;
        case BGPU_BLEND_ADDITIVE:
            cb.blendEnable = VK_TRUE;
            cb.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            cb.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            cb.colorBlendOp = VK_BLEND_OP_ADD;
            cb.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            cb.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            cb.alphaBlendOp = VK_BLEND_OP_ADD;
            break;
        case BGPU_BLEND_PREMULTIPLIED:
        default:
            cb.blendEnable = VK_TRUE;
            cb.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            cb.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            cb.colorBlendOp = VK_BLEND_OP_ADD;
            cb.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            cb.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            cb.alphaBlendOp = VK_BLEND_OP_ADD;
            break;
    }
    VkPipelineColorBlendStateCreateInfo cbs{};
    cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cbs.attachmentCount = 1;
    cbs.pAttachments = &cb;

    const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynci{};
    dynci.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynci.dynamicStateCount = 2;
    dynci.pDynamicStates = dyn;

    VkPipelineRenderingCreateInfo prci{};
    prci.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    prci.colorAttachmentCount = 1;
    prci.pColorAttachmentFormats = &color_fmt;
    prci.depthAttachmentFormat = depth_fmt;

    VkGraphicsPipelineCreateInfo gp{};
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.pNext = &prci;
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vin;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cbs;
    gp.pDynamicState = &dynci;
    gp.layout = dev.pipe_layout;

    VkPipeline out_pipe = VK_NULL_HANDLE;
    if (g_vk.vkCreateGraphicsPipelines(dev.dev, VK_NULL_HANDLE, 1, &gp, nullptr, &out_pipe) !=
        VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    p.cache[key] = out_pipe;
    return out_pipe;
}

/* ---------------------------------------------------------------
 * 命令缓冲 / 顶点流
 * --------------------------------------------------------------- */
bool VkBackend::beginRecording(VkCmdObj& c, VkDeviceObj& dev) {
    if (c.recording) return true;
    /* 上一趟已经 fence 等完（frame_end / 离屏 end_render 都会等），退役表可以清了。
     * 只有"没有别的命令缓冲在录"时才动它 —— 见 VkDeviceObj::retired 的注释。 */
    if (dev.recording_cbs == 0) dev.drainRetired();
    if (!c.cb) {
        VkCommandBufferAllocateInfo cbi{};
        cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbi.commandPool = dev.cmd_pool;
        cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbi.commandBufferCount = 1;
        if (g_vk.vkAllocateCommandBuffers(dev.dev, &cbi, &c.cb) != VK_SUCCESS) return false;
    }
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (g_vk.vkBeginCommandBuffer(c.cb, &bi) != VK_SUCCESS) return false;
    c.recording = true;
    ++dev.recording_cbs;
    c.vused = 0;
    c.have_state = false;
    c.last_pipe = VK_NULL_HANDLE;
    c.color_fmt = VK_FORMAT_UNDEFINED;
    c.depth_fmt = VK_FORMAT_UNDEFINED;
    resetDescSets(c, dev);   /* 上一趟已经 fence 等完：整批回收 set */
    c.freeRetired();         /* 同理：上一趟的顶点/索引缓冲可以真正销毁了 */
    return true;
}

/* 顶点流扩容。
 *
 * 命令缓冲是**先录后执行**的：扩容那一刻，前面那些 draw 已经把
 * `vkCmdBindVertexBuffers(旧 buffer)` 录进去了。所以旧 buffer **不能当场销毁** ——
 * 驱动会把它的显存立刻放回池子，下一笔分配（就是这里的新 buffer）很可能拿到同一段
 * 显存，于是已经录好的 draw 读到的是"全 0 的新顶点流"：整块内容消失，
 * 既不出错也不崩溃，特别难查（bin2d/binUI 整页渲染一帧几千个四边形，必然扩容；
 * GL/D3D11 是即时执行 —— glBufferData / Map 立刻生效，没有这个坑）。
 *
 * 所以这里只把旧 buffer"退役"，真正的销毁放在 beginRecording（上一趟已经 fence
 * 等完）。顶点数据不用往新 buffer 抄：每笔 draw 的 vkCmdBindVertexBuffers 绑的是
 * 它录制时的那一个 buffer，各画各的。 */
bool VkBackend::ensureVertexSpace(VkCmdObj& c, VkDeviceObj& dev, uint64_t need) {
    if (c.vcap >= c.vused + need) return true;
    uint64_t cap = c.vcap ? c.vcap : 64 * 1024;
    while (cap < c.vused + need) cap *= 2;
    VkBuffer nbuf = VK_NULL_HANDLE;
    VkDeviceMemory nmem = VK_NULL_HANDLE;
    void* nmap = nullptr;
    if (!allocHostBuffer(dev, cap, &nbuf, &nmem, &nmap)) {
        c.vcap = 0;
        return false;
    }
    if (c.vbuf) {
        if (c.vmap) g_vk.vkUnmapMemory(dev.dev, c.vmem);
        c.retired.push_back({c.vbuf, c.vmem});
    }
    c.vbuf = nbuf;
    c.vmem = nmem;
    c.vmap = nmap;
    c.vcap = cap;
    return true;
}

bool VkBackend::ensureIndexSpace(VkCmdObj& c, VkDeviceObj& dev, uint64_t quads) {
    if (c.icap_quads >= quads) return true;
    uint64_t cap = c.icap_quads ? c.icap_quads : 4096;
    while (cap < quads) cap *= 2;
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    void* map = nullptr;
    if (!allocHostBuffer(dev, cap * 6 * sizeof(uint32_t), &buf, &mem, &map)) return false;
    uint32_t* idx = static_cast<uint32_t*>(map);
    for (uint64_t i = 0; i < cap; i++) {
        const uint32_t base = static_cast<uint32_t>(i * 4);
        idx[i * 6 + 0] = base + 0;
        idx[i * 6 + 1] = base + 1;
        idx[i * 6 + 2] = base + 2;
        idx[i * 6 + 3] = base + 2;
        idx[i * 6 + 4] = base + 1;
        idx[i * 6 + 5] = base + 3;
    }
    /* 旧索引缓冲同样只"退役"不销毁：前面的 draw 还指着它 */
    if (c.ibuf) c.retired.push_back({c.ibuf, c.imem});
    c.ibuf = buf;
    c.imem = mem;
    c.icap_quads = cap;
    return true;
}

bool VkBackend::submitNow(VkDeviceObj& dev, VkCmdObj& c) {
    if (!c.recording) return false;
    g_vk.vkEndCommandBuffer(c.cb);
    c.recording = false;
    if (dev.recording_cbs) --dev.recording_cbs;
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &c.cb;
    g_vk.vkResetFences(dev.dev, 1, &dev.fence);
    if (g_vk.vkQueueSubmit(dev.queue, 1, &si, dev.fence) != VK_SUCCESS) return false;
    const VkResult wr = g_vk.vkWaitForFences(dev.dev, 1, &dev.fence, VK_TRUE, UINT64_MAX);
    if (wr != VK_SUCCESS && std::getenv("BINGPU_VK_TRACE"))
        std::fprintf(stderr, "[vk] submitNow: wait=%d\n", (int)wr);
    /* 这一趟已经等完：退役表里的纹理/缓冲现在真的没人用了，可以释放 */
    if (dev.recording_cbs == 0) dev.drainRetired();
    return wr == VK_SUCCESS;
}

BgpuResult VkBackend::createCmd(Device& dev, BgpuCmd* out) {
    VkDeviceObj& d = static_cast<VkDeviceObj&>(dev);
    /* 注意：**不能**复用 BackendBase::createCmd —— 它造的是基类 Cmd，转成 VkCmdObj
     * 就是 UB（第一版就踩过：vkCmdPipelineBarrier 报 Invalid commandBuffer）。 */
    const uint64_t handle = makeObject<VkCmdObj>(*this, d.self, dev);
    if (handle == BGPU_INVALID) {
        setReason("对象表分配失败（内存不足）");
        return BGPU_ERR_OUT_OF_MEMORY;
    }
    *out = handle;
    VkCmdObj* c = static_cast<VkCmdObj*>(lookupObject(*out, kObjCmd));
    c->dev = d.dev;
    /* descriptor set 不在这里预分配：一笔 draw 一个，录制时按需拿（allocDescSet）。 */
    return BGPU_OK;
}

/* ---------------------------------------------------------------
 * 交换链 / 帧
 * --------------------------------------------------------------- */
bool VkBackend::ensureSwapchain(Swapchain& sc_, uint32_t w, uint32_t h) {
    VkSwapchainObj& sc = static_cast<VkSwapchainObj&>(sc_);
    VkDeviceObj& d = static_cast<VkDeviceObj&>(*lookupObject(sc_.device, kObjDevice));

    /* 无窗口（HEADLESS）也要有交换链：语义是"帧的载体 + frame_read 出图"
     * （bin2d 的 GPU 桥就是这么用的）。这里用一张自有离屏纹理当"交换链图像"，
     * present 退化成"拷到回读缓冲"（与 CPU 后端的 back→front 等价）。 */
    if (!d.surface) {
        if (sc.offscreen == BGPU_INVALID || sc.w != w || sc.h != h) {
            if (sc.offscreen != BGPU_INVALID) destroyObject(sc.offscreen);
            BgpuTextureDesc td{};
            td.struct_size = sizeof(td);
            td.width = w;
            td.height = h;
            td.format = sc_.fmt;
            td.flags = BGPU_TEXTURE_RENDER_TARGET | BGPU_TEXTURE_CPU_READ;
            BgpuTexture th = BGPU_INVALID;
            if (createTexture(d, td, &th) != BGPU_OK) return false;
            sc.offscreen = th;
            sc.borrow = nullptr;
        }
        VkTextureObj* ot = static_cast<VkTextureObj*>(lookupObject(sc.offscreen, kObjTexture));
        if (!ot) return false;
        sc.images.assign(1, ot->image);
        sc.views.assign(1, ot->view);
        sc.vfmt = ot->vfmt;
        sc.extent = {w, h};
        sc.w = w;
        sc.h = h;
        sc.fmt = ot->fmt;
        sc.readback_bytes = static_cast<uint64_t>(w) * h * 4;
        if (sc.readback_map) {
            g_vk.vkUnmapMemory(d.dev, sc.readback_mem);
            sc.readback_map = nullptr;
        }
        if (sc.readback) g_vk.vkDestroyBuffer(d.dev, sc.readback, nullptr);
        if (sc.readback_mem) g_vk.vkFreeMemory(d.dev, sc.readback_mem, nullptr);
        sc.readback = VK_NULL_HANDLE;
        sc.readback_mem = VK_NULL_HANDLE;
        if (!allocHostBuffer(d, sc.readback_bytes, &sc.readback, &sc.readback_mem,
                             &sc.readback_map)) {
            return false;
        }
        if (!sc.borrow) {
            const uint64_t th = makeObject<VkTextureObj>(*this, d.self);
            if (th == BGPU_INVALID) return false;
            sc.borrow = static_cast<VkTextureObj*>(lookupObject(th, kObjTexture));
            sc.borrow->own_image = false;
            sc.borrow->borrowed = true;
            sc.borrow->dev = d.dev;
            sc.target = th;
        }
        sc.borrow->image = ot->image;
        sc.borrow->view = ot->view;
        sc.borrow->layout = ot->layout;   /* 与离屏纹理保持同一份布局状态 */
        sc.borrow->w = w;
        sc.borrow->h = h;
        sc.borrow->fmt = ot->fmt;
        sc.borrow->vfmt = ot->vfmt;
        sc.borrow->tex_flags = BGPU_TEXTURE_RENDER_TARGET;
        sc.borrow->is_depth = false;
        sc.borrow->msaa_valid = false;
        sc.borrow->resolve_pending = false;
        return true;
    }

    VkSurfaceCapabilitiesKHR caps{};
    if (g_vk.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(d.phys, d.surface, &caps) != VK_SUCCESS) {
        return false;
    }
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) {
        extent.width = std::clamp(w, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(h, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) return false;

    uint32_t fmt_count = 0;
    g_vk.vkGetPhysicalDeviceSurfaceFormatsKHR(d.phys, d.surface, &fmt_count, nullptr);
    if (fmt_count == 0) return false;
    std::vector<VkSurfaceFormatKHR> fmts(fmt_count);
    g_vk.vkGetPhysicalDeviceSurfaceFormatsKHR(d.phys, d.surface, &fmt_count, fmts.data());
    VkSurfaceFormatKHR chosen = fmts[0];
    /* 优先 B8G8R8A8（Windows 原生序，binUI 也是这么选的） */
    for (const auto& f : fmts) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f;
            break;
        }
    }

    const bool vsync = (sc_.sc_flags & BGPU_DEVICE_VSYNC) != 0;
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;

    VkSwapchainCreateInfoKHR sci{};
    sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sci.surface = d.surface;
    sci.minImageCount = std::max(2u, caps.minImageCount);
    if (caps.maxImageCount && sci.minImageCount > caps.maxImageCount) {
        sci.minImageCount = caps.maxImageCount;
    }
    sci.imageFormat = chosen.format;
    sci.imageColorSpace = chosen.colorSpace;
    sci.imageExtent = extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = alpha;
    sci.presentMode = vsync ? VK_PRESENT_MODE_FIFO_KHR : VK_PRESENT_MODE_IMMEDIATE_KHR;
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = sc.sc;

    VkSwapchainKHR new_sc = VK_NULL_HANDLE;
    if (g_vk.vkCreateSwapchainKHR(d.dev, &sci, nullptr, &new_sc) != VK_SUCCESS) return false;

    uint32_t img_count = 0;
    g_vk.vkGetSwapchainImagesKHR(d.dev, new_sc, &img_count, nullptr);
    std::vector<VkImage> images(img_count);
    g_vk.vkGetSwapchainImagesKHR(d.dev, new_sc, &img_count, images.data());
    std::vector<VkImageView> views(img_count);
    for (uint32_t i = 0; i < img_count; i++) {
        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = images[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = chosen.format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (g_vk.vkCreateImageView(d.dev, &vci, nullptr, &views[i]) != VK_SUCCESS) {
            return false;
        }
    }

    /* 旧链回收：视图 + swapchain + 回读缓冲（target 对象保留，只换 image/view） */
    if (sc.sc) {
        for (VkImageView v : sc.views) g_vk.vkDestroyImageView(d.dev, v, nullptr);
        g_vk.vkDestroySwapchainKHR(d.dev, sc.sc, nullptr);
    }
    if (sc.readback_map) {
        g_vk.vkUnmapMemory(d.dev, sc.readback_mem);
        sc.readback_map = nullptr;
    }
    if (sc.readback) g_vk.vkDestroyBuffer(d.dev, sc.readback, nullptr);
    if (sc.readback_mem) g_vk.vkFreeMemory(d.dev, sc.readback_mem, nullptr);
    sc.readback = VK_NULL_HANDLE;
    sc.readback_mem = VK_NULL_HANDLE;

    sc.sc = new_sc;
    sc.owns_images = true;
    sc.images = std::move(images);
    sc.views = std::move(views);
    sc.vfmt = chosen.format;
    sc.extent = extent;
    sc.w = extent.width;
    sc.h = extent.height;
    sc.fmt = chosen.format == VK_FORMAT_B8G8R8A8_UNORM ? BGPU_FORMAT_BGRA8 : BGPU_FORMAT_RGBA8;
    sc.readback_bytes = static_cast<uint64_t>(extent.width) * extent.height * 4;
    if (!allocHostBuffer(d, sc.readback_bytes, &sc.readback, &sc.readback_mem,
                         &sc.readback_map)) {
        return false;
    }
    d.swap_format = chosen.format;
    sc.has_current = false;

    /* 借出的目标对象：**不分配 image/view**（交换链图像的所有权在 swapchain），
     * 每帧按 acquired 索引把 image/view 指过去。 */
    if (!sc.borrow) {
        const uint64_t th = makeObject<VkTextureObj>(*this, d.self);
        if (th == BGPU_INVALID) return false;
        sc.borrow = static_cast<VkTextureObj*>(lookupObject(th, kObjTexture));
        sc.borrow->own_image = false;
        sc.borrow->borrowed = true;
        sc.borrow->dev = d.dev;
        sc.target = th;
    }
    sc.borrow->w = extent.width;
    sc.borrow->h = extent.height;
    sc.borrow->fmt = sc.fmt;
    sc.borrow->vfmt = chosen.format;
    sc.borrow->tex_flags = BGPU_TEXTURE_RENDER_TARGET;
    sc.borrow->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    sc.borrow->is_depth = false;
    sc.borrow->msaa_valid = false;
    sc.borrow->resolve_pending = false;
    return true;
}

BgpuResult VkBackend::createSwapchain(Device& dev, const BgpuSwapchainDesc& desc,
                                      BgpuSwapchain* out) {
    VkDeviceObj& d = static_cast<VkDeviceObj&>(dev);
    /* 无窗口设备也允许建"交换链"：它的语义是"帧的载体 + frame_read 出图"
     * （contracts.md 的帧模型），实现上退化成一张离屏纹理。 */
    if (desc.flags & BGPU_DEVICE_TRANSPARENT) {
        return fail(BGPU_ERR_UNSUPPORTED, __FILE__, __LINE__,
                    "Vulkan 后端这一版不支持透明窗口（composite alpha premultiplied 未做）");
    }
    const uint64_t handle = makeObject<VkSwapchainObj>(*this, d.self);
    if (handle == BGPU_INVALID) return failOom(__FILE__, __LINE__);
    VkSwapchainObj* sc = static_cast<VkSwapchainObj*>(lookupObject(handle, kObjSwapchain));
    sc->vkdev = d.dev;
    sc->w = desc.width;
    sc->h = desc.height;
    sc->sc_flags = desc.flags;
    VkSwapchainObj& sc_ref = *sc;
    sc_ref.dev = &d;   /* 基类的 Device*：present 统计要用 */
    if (!ensureSwapchain(static_cast<Swapchain&>(sc_ref), desc.width, desc.height)) {
        destroyObject(handle);
        return fail(BGPU_ERR_UNSUPPORTED, __FILE__, __LINE__, "交换链创建失败");
    }
    *out = handle;
    return BGPU_OK;
}

BgpuResult VkBackend::resizeSwapchain(Swapchain& sc, uint32_t w, uint32_t h) {
    if (w == 0 || h == 0) return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__, "尺寸为 0");
    if (!ensureSwapchain(sc, w, h)) {
        return fail(BGPU_ERR_UNSUPPORTED, __FILE__, __LINE__, "交换链重建失败");
    }
    return BGPU_OK;
}

void VkBackend::swapchainInfo(Swapchain& sc, BgpuSwapchainInfo& out) {
    out.width = sc.w;
    out.height = sc.h;
    out.format = sc.fmt;
    out.image_count = static_cast<uint32_t>(static_cast<VkSwapchainObj&>(sc).views.size());
    /* 实际生效值：设备建的时候按 desc.msaa_samples 选过档，这里如实报。 */
    out.msaa_samples = static_cast<VkSwapchainObj&>(sc).dev
                           ? static_cast<VkDeviceObj*>(static_cast<VkSwapchainObj&>(sc).dev)->msaa
                           : 1;
    out.present_paces_frames = (sc.sc_flags & BGPU_DEVICE_VSYNC) ? 1 : 0;
}

/* MSAA 颜色 companion：与目标同尺寸、同格式、N 采样，只当颜色附件。
 * 尺寸/格式/采样数变了就重建（交换链 resize 会走到这里）。 */
bool VkBackend::ensureMsaa(VkTextureObj& t, VkDeviceObj& d) {
    if (d.msaa < 2) return false;
    if (t.msaa_image && t.msaa_samples == d.msaa && t.msaa_w == t.w && t.msaa_h == t.h &&
        t.msaa_fmt == t.vfmt) {
        return true;
    }
    if (t.msaa_view) {
        g_vk.vkDestroyImageView(d.dev, t.msaa_view, nullptr);
        t.msaa_view = VK_NULL_HANDLE;
    }
    if (t.msaa_image) {
        g_vk.vkDestroyImage(d.dev, t.msaa_image, nullptr);
        t.msaa_image = VK_NULL_HANDLE;
    }
    if (t.msaa_mem) {
        g_vk.vkFreeMemory(d.dev, t.msaa_mem, nullptr);
        t.msaa_mem = VK_NULL_HANDLE;
    }
    t.msaa_samples = 0;
    t.msaa_layout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = t.vfmt;
    ici.extent = {t.w, t.h, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = toVkSamples(d.msaa);
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (g_vk.vkCreateImage(d.dev, &ici, nullptr, &t.msaa_image) != VK_SUCCESS) {
        if (trace2()) std::fprintf(stderr, "[vk] MSAA companion 建图失败 %ux%u\n", t.w, t.h);
        return false;
    }
    VkMemoryRequirements req{};
    g_vk.vkGetImageMemoryRequirements(d.dev, t.msaa_image, &req);
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = pickMemoryType(d.phys, req.memoryTypeBits, 0);
    if (g_vk.vkAllocateMemory(d.dev, &mai, nullptr, &t.msaa_mem) != VK_SUCCESS) return false;
    if (g_vk.vkBindImageMemory(d.dev, t.msaa_image, t.msaa_mem, 0) != VK_SUCCESS) return false;
    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = t.msaa_image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = t.vfmt;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (g_vk.vkCreateImageView(d.dev, &vci, nullptr, &t.msaa_view) != VK_SUCCESS) return false;
    t.msaa_samples = d.msaa;
    t.msaa_w = t.w;
    t.msaa_h = t.h;
    t.msaa_fmt = t.vfmt;
    return true;
}

/* companion 的布局迁移（它只有颜色附件 / 传输两种用途，比 transitionImpl 简单）。 */
void VkBackend::transitionMsaa(VkCmdObj& c, VkTextureObj& t, VkImageLayout want) {
    if (t.msaa_layout == want || t.msaa_image == VK_NULL_HANDLE) return;
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = t.msaa_layout;
    b.newLayout = want;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = t.msaa_image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkPipelineStageFlags src_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkPipelineStageFlags dst_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    if (want == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        b.dstAccessMask =
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dst_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    } else if (want == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        b.srcAccessMask = 0;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else {
        b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    if (t.msaa_layout == VK_IMAGE_LAYOUT_UNDEFINED) b.srcAccessMask = 0;
    g_vk.vkCmdPipelineBarrier(c.cb, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
    t.msaa_layout = want;
}

BgpuResult VkBackend::frameBegin(Swapchain& sc_, Cmd& cmd, BgpuTexture* out) {
    VkSwapchainObj& sc = static_cast<VkSwapchainObj&>(sc_);
    VkCmdObj& c = static_cast<VkCmdObj&>(cmd);
    VkDeviceObj& d = static_cast<VkDeviceObj&>(*lookupObject(sc_.device, kObjDevice));
    if (c.inFrame) {
        return fail(BGPU_ERR_STATE, __FILE__, __LINE__, "cmd 已经在一个帧里（先 frame_end）");
    }
    if (d.surface && sc.sc == VK_NULL_HANDLE) {
        return fail(BGPU_ERR_STATE, __FILE__, __LINE__, "交换链无效（先 resize）");
    }

    uint32_t index = 0;
    if (d.surface) {   /* HEADLESS 没有可 acquire 的图像：固定第 0 张 */
        g_vk.vkResetFences(d.dev, 1, &d.fence);
        const VkResult vr = g_vk.vkAcquireNextImageKHR(d.dev, sc.sc, UINT64_MAX, VK_NULL_HANDLE,
                                                       d.fence, &index);
        if (vr != VK_SUCCESS) {
            d.lost = (vr == VK_ERROR_DEVICE_LOST);
            return vkFail(vr, "vkAcquireNextImageKHR");
        }
        g_vk.vkWaitForFences(d.dev, 1, &d.fence, VK_TRUE, UINT64_MAX);
    }

    sc.current = index;
    sc.has_current = true;
    sc.borrow->image = sc.images[index];
    sc.borrow->view = sc.views[index];
    sc.borrow->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    /* 每帧借到的图像可能换了一张（窗口模式）：companion 里是**上一张图**的内容，
     * 不能再当成"这张图的最新内容"；挂起的 resolve 也一并作废（镜像内容未定义）。 */
    sc.borrow->msaa_valid = false;
    sc.borrow->resolve_pending = false;
    sc.borrow->w = sc.extent.width;
    sc.borrow->h = sc.extent.height;

    c.inFrame = true;
    c.frame = &sc_;
    if (traceResolve()) {
        d.frame_blits = 0;
        d.frame_resolves = 0;
        d.frame_blit_px = 0;
        d.frame_resolve_px = 0;
        d.frame_blits_big = 0;
        d.frame_resolves_big = 0;
    }
    if (!beginRecording(c, d)) {
        c.inFrame = false;
        c.frame = nullptr;
        return fail(BGPU_ERR_BACKEND_FAILED, __FILE__, __LINE__, "命令缓冲开始记录失败");
    }
    *out = sc_.target;
    return BGPU_OK;
}

BgpuResult VkBackend::beginRender(Cmd& cmd, Texture& target, Texture* depth,
                                  const BgpuRenderBeginDesc& desc) {
    BgpuResult r = BackendBase::beginRender(cmd, target, depth, desc);
    if (r != BGPU_OK) return r;
    VkCmdObj& c = static_cast<VkCmdObj&>(cmd);
    VkTextureObj& t = static_cast<VkTextureObj&>(target);
    VkDeviceObj& d = static_cast<VkDeviceObj&>(*lookupObject(target.device, kObjDevice));
    if (!beginRecording(c, d)) {
        cmd.inRender = false;
        return fail(BGPU_ERR_BACKEND_FAILED, __FILE__, __LINE__, "命令缓冲开始记录失败");
    }
    c.color_fmt = t.vfmt;
    /* 0.14：设备开了 MSAA 时，所有颜色目标（画布 / 图层 / 离屏）都走"多采样附件 +
     * pass 末 resolve 回单采样"这一条路。默认 msaa == 1 时 use_msaa 恒 false，
     * 下面的命令与 0.13.0 逐字节相同。 */
    const bool use_msaa = d.msaa >= 2 && !t.is_depth;
    if (use_msaa && !ensureMsaa(t, d)) {
        cmd.inRender = false;
        return fail(BGPU_ERR_OUT_OF_MEMORY, __FILE__, __LINE__, "MSAA 颜色附件创建失败");
    }
    VkImageView depth_view = VK_NULL_HANDLE;
    c.depth_fmt = VK_FORMAT_UNDEFINED;
    if (depth) {
        VkTextureObj& dt = static_cast<VkTextureObj&>(*depth);
        c.depth_fmt = dt.vfmt;
        depth_view = dt.view;
    }
    /* 0.16：这一趟的 1× 结果要不要。
     *   · 显式 DEFER_RESOLVE：调用方说了不要（任何尺寸都听）；
     *   · 自动策略：只对**整窗尺寸**的目标、且是这一帧里第 2 趟起 defer。
     *     为什么是整窗尺寸：bin2D 的层/blur 目标按元素尺寸分配（0.22.8），它们刚画完
     *     就立刻被父级合成采样 —— defer 一定紧接着自愈，白拆一次 pass；真正被
     *     "离开再回来"反复进出的是画布/贴屏目标（整窗），那个 4×→1× 才是纯浪费。
     *     正确性由 resolvePending 的自愈兜住（被采样前一定已经 resolve 过）。 */
    const uint32_t pass_no = (t.pass_frame_stamp == d.frame_index) ? t.pass_count + 1u : 1u;
    t.pass_frame_stamp = d.frame_index;
    t.pass_count = pass_no;
    const bool full_surface = static_cast<uint64_t>(t.w) * t.h >=
                              static_cast<uint64_t>(d.w) * d.h;
    const bool defer_resolve =
        use_msaa && ((desc.flags & BGPU_RENDER_DEFER_RESOLVE) != 0 ||
                     (c.inFrame && pass_no >= 2u && full_surface && !noAutoDefer()));
    if (use_msaa) {
        /* LOAD 的 pass 必须先从**单采样镜像**把已有内容搬进多采样附件 —— 上一趟
         * 结束时内容已经 resolve 到单采样镜像里，多采样附件本身不保留它。
         * （没有 CLEAR 又没有历史内容的帧这里搬的是未定义数据，与不用 MSAA 时
         *  loadOp=LOAD 的语义一致。）
         *
         * 0.16：companion 里**本来就是这张图的最新内容**时（上一趟刚画完 / 刚 resolve），
         * 这次回灌是纯多余的全窗往返 —— 直接跳过，接着在 companion 上画。 */
        if (!(desc.flags & BGPU_RENDER_CLEAR_COLOR) && !t.msaa_valid) {
            if (transitionImpl(c, t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, /*force=*/false) !=
                BGPU_OK) {
                cmd.inRender = false;
                return BGPU_ERR_BACKEND_FAILED;
            }
            transitionMsaa(c, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.srcOffsets[1] = {static_cast<int32_t>(t.w), static_cast<int32_t>(t.h), 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.dstOffsets[1] = {static_cast<int32_t>(t.w), static_cast<int32_t>(t.h), 1};
            g_vk.vkCmdBlitImage(c.cb, t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.msaa_image,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
            d.frame_blits++;
            d.frame_blit_px += static_cast<uint64_t>(t.w) * t.h;
            if (static_cast<uint64_t>(t.w) * t.h >= static_cast<uint64_t>(d.w) * d.h)
                d.frame_blits_big++;
        }
        if (transitionImpl(c, t, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, /*force=*/true) !=
            BGPU_OK) {
            cmd.inRender = false;
            return BGPU_ERR_BACKEND_FAILED;
        }
        transitionMsaa(c, t, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        c.samples = toVkSamples(d.msaa);
        t.msaa_valid = true;        /* 从这一刻起 companion 是"最新内容"的持有者 */
        t.resolve_pending = false;  /* 这一趟开始前 1× 上没有挂着的 resolve */
    } else {
        if (transitionImpl(c, t, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, /*force=*/true) !=
            BGPU_OK) {
            cmd.inRender = false;
            return BGPU_ERR_BACKEND_FAILED;
        }
        c.samples = VK_SAMPLE_COUNT_1_BIT;
    }
    if (depth) {
        transition(c, static_cast<VkTextureObj&>(*depth), VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    }

    VkRenderingAttachmentInfo color_att{};
    color_att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color_att.imageView = use_msaa ? t.msaa_view : t.view;
    color_att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    /* 0.16：这一趟不需要 1× 结果时不落 1×（DEFER_RESOLVE / 自动策略）。 */
    if (use_msaa && !defer_resolve) {
        /* 动态渲染的 resolve：vkCmdEndRendering 时自动把 N 个样本平均进单采样镜像。
         * 之后 frame_read / 采样 / 下一次 LOAD 看到的都是单采样结果。 */
        color_att.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        color_att.resolveImageView = t.view;
        color_att.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    c.pass_resolves = use_msaa && !defer_resolve;
    color_att.loadOp = (desc.flags & BGPU_RENDER_CLEAR_COLOR) ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                                              : VK_ATTACHMENT_LOAD_OP_LOAD;
    color_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    /* clear color 是直通 0xAARRGGBB；目标存预乘 → rgb 先乘 alpha（raster_rules §9）。
     * 通道序按**目标格式**放：RGBA8 是 R,G,B；BGRA8 是 B,G,R。（第一版把顺序写死成
     * B,G,R，RGBA8 目标上红蓝就对调了 —— clear 用例当场 100% 差异。） */
    const uint32_t cc = desc.clear_color;
    const float cr = static_cast<float>((cc >> 16) & 0xFF) / 255.0f;
    const float cg = static_cast<float>((cc >> 8) & 0xFF) / 255.0f;
    const float cb = static_cast<float>(cc & 0xFF) / 255.0f;
    /* float32 数组永远是 (R,G,B,A) **分量序**，字节序由 VkFormat 决定 —— 所以这里
     * 不按 BGRA 交换（第一版换了，BGRA 交换链上红蓝对调，窗口烟测当场 100% 差异）。 */
    color_att.clearValue.color.float32[0] = cr;
    color_att.clearValue.color.float32[1] = cg;
    color_att.clearValue.color.float32[2] = cb;
    color_att.clearValue.color.float32[3] = static_cast<float>((cc >> 24) & 0xFF) / 255.0f;
    const float a = color_att.clearValue.color.float32[3];
    color_att.clearValue.color.float32[0] *= a;
    color_att.clearValue.color.float32[1] *= a;
    color_att.clearValue.color.float32[2] *= a;

    VkRenderingAttachmentInfo depth_att{};
    depth_att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth_att.imageView = depth_view;
    depth_att.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth_att.loadOp = (desc.flags & BGPU_RENDER_CLEAR_DEPTH) ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                                              : VK_ATTACHMENT_LOAD_OP_LOAD;
    depth_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth_att.clearValue.depthStencil.depth = desc.clear_depth;

    VkRenderingInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea = {{0, 0}, {t.w, t.h}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color_att;
    ri.pDepthAttachment = depth ? &depth_att : nullptr;
    g_vk.vkCmdBeginRendering(c.cb, &ri);
    c.rendering = true;

    /* viewport 固定整目标（映射在顶点着色器里按 push constant 做） */
    VkViewport vp{};
    vp.x = 0.0f;
    vp.y = 0.0f;
    vp.width = static_cast<float>(t.w);
    vp.height = static_cast<float>(t.h);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    g_vk.vkCmdSetViewport(c.cb, 0, 1, &vp);
    VkRect2D scissor{{0, 0}, {t.w, t.h}};
    g_vk.vkCmdSetScissor(c.cb, 0, 1, &scissor);
    c.have_state = false;
    if (tracePasses())
        std::fprintf(stderr, "[vk][pass] %ux%u %s msaa=%u\n", t.w, t.h,
                     (desc.flags & BGPU_RENDER_CLEAR_COLOR) ? "CLEAR" : "LOAD",
                     use_msaa ? d.msaa : 1u);
    return BGPU_OK;
}

BgpuResult VkBackend::endRender(Cmd& cmd) {
    VkCmdObj& c = static_cast<VkCmdObj&>(cmd);
    if (!c.inRender) {
        return fail(BGPU_ERR_STATE, __FILE__, __LINE__, "没有正在进行的渲染");
    }
    if (c.rendering) {
        g_vk.vkCmdEndRendering(c.cb);
        c.rendering = false;
    }
    VkTextureObj* t = c.target ? static_cast<VkTextureObj*>(c.target) : nullptr;
    /* 0.16：这一趟按 DEFER 结束 ⇒ 1× image 落后（挂起），companion 仍是权威内容。
     * 谁要采样 / 回读它，谁就得先把这次 resolve 补上（见 resolvePending）。 */
    if (t && c.samples != VK_SAMPLE_COUNT_1_BIT) {
        if (c.pass_resolves) {
            VkDeviceObj& dd = static_cast<VkDeviceObj&>(*lookupObject(cmd.device, kObjDevice));
            dd.frame_resolves++;
            dd.frame_resolve_px += static_cast<uint64_t>(t->w) * t->h;
            if (static_cast<uint64_t>(t->w) * t->h >= static_cast<uint64_t>(dd.w) * dd.h)
                dd.frame_resolves_big++;
            t->resolve_pending = false;
        } else {
            t->resolve_pending = true;
        }
    }
    BgpuResult r = BackendBase::endRender(cmd);
    if (r != BGPU_OK) return r;

    if (!c.inFrame) {
        /* 离屏：这一段自己提交并等完成，之后纹理可采样/可回读。
         * 0.16：挂了 resolve 的时候不转到 SHADER_READ_ONLY —— 1× 还是旧的，
         * 转布局没有意义；真正被采样/回读时由 resolvePending 补完再转。 */
        VkDeviceObj& d = static_cast<VkDeviceObj&>(*lookupObject(cmd.device, kObjDevice));
        if (t && !t->resolve_pending) transition(c, *t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (!submitNow(d, c)) {
            return fail(BGPU_ERR_BACKEND_FAILED, __FILE__, __LINE__, "离屏提交失败");
        }
    }
    return BGPU_OK;
}

/* ---------------------------------------------------------------
 * 0.16：延迟 resolve 的自愈
 *
 * 不变量：`resolve_pending == true` 的目标，1× 镜像落后于 4× companion。
 * 谁要读那张 1×（采样 / texture_read / frame_read / 显式 resolve），就先把这次
 * resolve 补上 —— 上层**不需要**记住哪一趟 defer 了哪一张。
 *
 * 难点只有一个：采样发生在渲染实例**内部**（rasterQuads 里绑 sampler），而
 * vkCmdResolveImage 不能在渲染实例里录。所以这里把当前实例收起来（endRendering
 * 会把 4× 附件存回去）、resolve 完再原样开回去（LOAD）。这条路径只在"被 defer 的
 * 目标真的要被采样"时走一次，正常帧里每个目标至多一次。
 * --------------------------------------------------------------- */
void VkBackend::suspendPass(VkCmdObj& c) {
    if (!c.rendering) return;
    g_vk.vkCmdEndRendering(c.cb);
    c.rendering = false;
}

BgpuResult VkBackend::resumePass(VkCmdObj& c) {
    /* 目标/深度从 cmd 的**当前状态**取（不是另存指针）：帧中途销毁时
     * Cmd::onPeerDestroyed 会把它们清成 nullptr，这里就不会去碰悬垂对象。 */
    VkTextureObj* t = c.target ? static_cast<VkTextureObj*>(c.target) : nullptr;
    if (!t) return fail(BGPU_ERR_STATE, __FILE__, __LINE__, "没有可恢复的渲染实例");
    VkTextureObj* dt = c.depth ? static_cast<VkTextureObj*>(c.depth) : nullptr;
    const bool msaa = c.samples != VK_SAMPLE_COUNT_1_BIT;
    VkRenderingAttachmentInfo color_att{};
    color_att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color_att.imageView = msaa ? t->msaa_view : t->view;
    color_att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    /* resolve 策略跟着原 pass：defer 的 pass 恢复后仍然 defer。 */
    if (msaa && c.pass_resolves) {
        color_att.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        color_att.resolveImageView = t->view;
        color_att.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    /* 原 pass 的 clear 已经落进附件了，恢复一律用 LOAD（不能重清）。 */
    color_att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingAttachmentInfo depth_att{};
    depth_att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth_att.imageView = dt ? dt->view : VK_NULL_HANDLE;
    depth_att.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth_att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea = {{0, 0}, {t->w, t->h}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color_att;
    ri.pDepthAttachment = dt ? &depth_att : nullptr;
    g_vk.vkCmdBeginRendering(c.cb, &ri);
    c.rendering = true;

    VkViewport vp{};
    vp.width = static_cast<float>(t->w);
    vp.height = static_cast<float>(t->h);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    g_vk.vkCmdSetViewport(c.cb, 0, 1, &vp);
    VkRect2D scissor{{0, 0}, {t->w, t->h}};
    g_vk.vkCmdSetScissor(c.cb, 0, 1, &scissor);
    /* 实例边界会打断"上一笔的状态"：管线/深度状态重新下发。 */
    c.have_state = false;
    c.last_pipe = VK_NULL_HANDLE;
    return BGPU_OK;
}

BgpuResult VkBackend::resolvePending(VkCmdObj& c, VkDeviceObj& d, VkTextureObj& t) {
    if (!t.resolve_pending) return BGPU_OK;
    if (t.msaa_image == VK_NULL_HANDLE || t.msaa_samples < 2) {
        t.resolve_pending = false;
        return BGPU_OK;
    }
    bool suspended = false;
    if (c.rendering) {
        if (c.target == &t) {
            /* 采样自己正在画的目标 = 反馈回路，本来就是非法用法；拆 pass 也救不了它。 */
            if (traceResolve())
                std::fprintf(stderr, "[vk][resolve] 跳过：%ux%u 是当前 pass 的目标（反馈回路）\n",
                             t.w, t.h);
            return BGPU_OK;
        }
        suspendPass(c);
        suspended = true;
    }
    BgpuResult r = transitionImpl(c, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, /*force=*/false);
    if (r == BGPU_OK) {
        transitionMsaa(c, t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkImageResolve rr{};
        rr.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        rr.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        rr.extent = {t.w, t.h, 1};
        g_vk.vkCmdResolveImage(c.cb, t.msaa_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rr);
        /* 回到附件布局：后续 transition（SHADER_READ_ONLY / TRANSFER_SRC）都按
         * "源是颜色附件"写访问掩码。 */
        r = transitionImpl(c, t, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, /*force=*/false);
    }
    if (r != BGPU_OK) {
        if (suspended) (void)resumePass(c);
        return fail(BGPU_ERR_BACKEND_FAILED, __FILE__, __LINE__, "延迟 resolve 失败");
    }
    t.resolve_pending = false;
    t.msaa_valid = true;
    d.frame_resolves++;
    d.frame_resolve_px += static_cast<uint64_t>(t.w) * t.h;
    if (static_cast<uint64_t>(t.w) * t.h >= static_cast<uint64_t>(d.w) * d.h)
        d.frame_resolves_big++;
    if (traceResolve())
        std::fprintf(stderr, "[vk][resolve] self-heal %ux%u（采样/回读前补一次）\n", t.w, t.h);
    if (suspended) return resumePass(c);
    return BGPU_OK;
}

BgpuResult VkBackend::resolveTarget(Cmd& cmd, Texture& target) {
    VkCmdObj& c = static_cast<VkCmdObj&>(cmd);
    VkTextureObj& t = static_cast<VkTextureObj&>(target);
    if (!t.resolve_pending) return BGPU_OK;   /* 没挂起的：no-op */
    VkDeviceObj& d = static_cast<VkDeviceObj&>(*lookupObject(target.device, kObjDevice));
    if (!beginRecording(c, d)) {
        return fail(BGPU_ERR_BACKEND_FAILED, __FILE__, __LINE__, "命令缓冲开始记录失败");
    }
    const BgpuResult r = resolvePending(c, d, t);
    if (r != BGPU_OK) return r;
    if (!c.inFrame) {
        transition(c, t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (!submitNow(d, c)) {
            return fail(BGPU_ERR_BACKEND_FAILED, __FILE__, __LINE__, "resolve_target 提交失败");
        }
    }
    return BGPU_OK;
}

BgpuResult VkBackend::frameEnd(Swapchain& sc_, Cmd& cmd) {
    VkSwapchainObj& sc = static_cast<VkSwapchainObj&>(sc_);
    VkCmdObj& c = static_cast<VkCmdObj&>(cmd);
    if (!c.inFrame || c.frame != &sc_) {
        return fail(BGPU_ERR_STATE, __FILE__, __LINE__,
                    "这个 cmd 没抓着这个交换链的帧（frame_begin 呢？）");
    }
    if (c.inRender) {
        return fail(BGPU_ERR_STATE, __FILE__, __LINE__, "还在 begin_render/end_render 之间");
    }
    VkDeviceObj& d = static_cast<VkDeviceObj&>(*lookupObject(sc_.device, kObjDevice));
    c.inFrame = false;
    c.frame = nullptr;

    /* 回读 + 呈现：先把图像拷到主机缓冲（frame_read 用），再转 PRESENT_SRC */
    if (sc.has_current) {
        VkTextureObj& t = *sc.borrow;
        VkCmdObj& cc = c;
        if (!cc.recording && !beginRecording(cc, d)) {
            return fail(BGPU_ERR_BACKEND_FAILED, __FILE__, __LINE__, "帧尾命令缓冲开始失败");
        }
        /* 0.16：贴屏目标被 DEFER 过的话，回读/呈现之前先落一次 1×。 */
        {
            const BgpuResult rr = resolvePending(cc, d, t);
            if (rr != BGPU_OK) return rr;
        }
        transition(cc, t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {sc.extent.width, sc.extent.height, 1};
        g_vk.vkCmdCopyImageToBuffer(cc.cb, t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                    sc.readback, 1, &region);
        if (d.surface) {
            transition(cc, t, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
            if (!submitNow(d, cc)) {
                return fail(BGPU_ERR_BACKEND_FAILED, __FILE__, __LINE__, "帧提交失败");
            }
            VkPresentInfoKHR pi{};
            pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            pi.swapchainCount = 1;
            pi.pSwapchains = &sc.sc;
            pi.pImageIndices = &sc.current;
            const VkResult pr = g_vk.vkQueuePresentKHR(d.queue, &pi);
            if (pr != VK_SUCCESS) d.lost = true;   /* 事件通过 poll() 给出去（只记标志） */
        } else {
            /* HEADLESS：拷完回读缓冲后把图像交还给渲染目标布局，下一帧继续画 */
            transition(cc, t, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            if (!submitNow(d, cc)) {
                return fail(BGPU_ERR_BACKEND_FAILED, __FILE__, __LINE__, "离屏帧提交失败");
            }
            VkTextureObj* ot = static_cast<VkTextureObj*>(lookupObject(sc.offscreen, kObjTexture));
            if (ot) ot->layout = t.layout;   /* 借用对象与离屏纹理共享同一份布局状态 */
        }
        sc.frames++;
        sc.dev->frames_presented++;
        sc.dev->frame_index++;
    }

    if (traceResolve()) {
        /* 每帧一行：LOAD 预置 blit 的次数/像素、pass 末 resolve 的次数/像素。
         * 4× 的采样数是 resolve 的写入量 = 1× 像素 × 4；读同为 4× —— 带宽估算用。 */
        const double mb = 1.0 / (1024.0 * 1024.0);
        std::fprintf(stderr,
                     "[vk][resolve] frame=%u blits=%u/%u_big (%.2f Mpx, 1x->4x %.2f MB+%.2f MB) "
                     "resolves=%u/%u_big (%.2f Mpx, 4x->1x %.2f MB+%.2f MB)\n",
                     sc.dev->frame_index, d.frame_blits, d.frame_blits_big,
                     d.frame_blit_px / 1e6, d.frame_blit_px * 4.0 * mb, d.frame_blit_px * 16.0 * mb,
                     d.frame_resolves, d.frame_resolves_big, d.frame_resolve_px / 1e6,
                     d.frame_resolve_px * 16.0 * mb, d.frame_resolve_px * 4.0 * mb);
    }

    return BGPU_OK;
}

BgpuResult VkBackend::frameRead(Swapchain& sc_, void* dst, size_t dst_size,
                                uint32_t* out_stride) {
    VkSwapchainObj& sc = static_cast<VkSwapchainObj&>(sc_);
    if (!sc.readback_map) {
        return fail(BGPU_ERR_STATE, __FILE__, __LINE__, "交换链没有可回读的帧");
    }
    const uint32_t row = sc.extent.width * 4;
    if (dst_size < static_cast<size_t>(row) * sc.extent.height) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__, "dst 太小");
    }
    VkDeviceObj& d = static_cast<VkDeviceObj&>(*lookupObject(sc_.device, kObjDevice));
    g_vk.vkDeviceWaitIdle(d.dev);
    std::memcpy(dst, sc.readback_map, static_cast<size_t>(row) * sc.extent.height);
    unpremultiply(static_cast<uint8_t*>(dst), static_cast<size_t>(sc.extent.width) *
                                                   sc.extent.height);
    if (trace2())
        std::fprintf(stderr, "[vk] frame_read vfmt=%d fmt=%d 前 4 字节=%02X%02X%02X%02X\n",
                     (int)sc.vfmt, (int)sc.fmt, static_cast<uint8_t*>(dst)[0],
                     static_cast<uint8_t*>(dst)[1], static_cast<uint8_t*>(dst)[2],
                     static_cast<uint8_t*>(dst)[3]);
    if (sc.vfmt == VK_FORMAT_B8G8R8A8_UNORM) {
        uint8_t* p = static_cast<uint8_t*>(dst);
        for (size_t i = 0; i < static_cast<size_t>(sc.extent.width) * sc.extent.height; i++) {
            std::swap(p[i * 4 + 0], p[i * 4 + 2]);
        }
    }
    if (out_stride) *out_stride = row;
    return BGPU_OK;
}

BgpuResult VkBackend::prepareDepth(Texture& depth, Texture& target,
                                   const BgpuRenderBeginDesc& desc) {
    (void)target;
    (void)desc;
    VkTextureObj& dt = static_cast<VkTextureObj&>(depth);
    if (dt.is_depth) return BGPU_OK;
    /* ABI 里没有专门的深度格式（backends.md 欠账 2）：把一个 RENDER_TARGET 纹理
     * 当成 D32 深度附件用，尺寸跟目标一致。 */
    VkDeviceObj& d = static_cast<VkDeviceObj&>(*lookupObject(depth.device, kObjDevice));
    dt.is_depth = true;
    dt.vfmt = VK_FORMAT_D32_SFLOAT;
    /* 深度附件必须与颜色附件同样的采样数：设备开了 MSAA 就按同一个档建。 */
    dt.samples = d.msaa >= 2 ? d.msaa : 1;
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = dt.vfmt;
    ici.extent = {dt.w, dt.h, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = toVkSamples(dt.samples);
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (g_vk.vkCreateImage(d.dev, &ici, nullptr, &dt.image) != VK_SUCCESS) {
        return fail(BGPU_ERR_OUT_OF_MEMORY, __FILE__, __LINE__, "深度图创建失败");
    }
    VkMemoryRequirements req{};
    g_vk.vkGetImageMemoryRequirements(d.dev, dt.image, &req);
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = pickMemoryType(d.phys, req.memoryTypeBits, 0);
    if (g_vk.vkAllocateMemory(d.dev, &mai, nullptr, &dt.mem) != VK_SUCCESS ||
        g_vk.vkBindImageMemory(d.dev, dt.image, dt.mem, 0) != VK_SUCCESS) {
        return fail(BGPU_ERR_OUT_OF_MEMORY, __FILE__, __LINE__, "深度图内存分配失败");
    }
    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = dt.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = dt.vfmt;
    vci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    if (g_vk.vkCreateImageView(d.dev, &vci, nullptr, &dt.view) != VK_SUCCESS) {
        return fail(BGPU_ERR_OUT_OF_MEMORY, __FILE__, __LINE__, "深度图视图创建失败");
    }
    dt.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return BGPU_OK;
}

/* ---------------------------------------------------------------
 * 绘制
 * --------------------------------------------------------------- */
BgpuResult VkBackend::rasterQuads(Cmd& cmd, const BgpuVertex* verts, uint32_t quad_count) {
    VkCmdObj& c = static_cast<VkCmdObj&>(cmd);
    VkDeviceObj& d = static_cast<VkDeviceObj&>(*lookupObject(cmd.device, kObjDevice));
    VkPipelineObj& p = static_cast<VkPipelineObj&>(*cmd.pipe);
    if (!c.rendering || !c.target) {
        return fail(BGPU_ERR_STATE, __FILE__, __LINE__, "draw 时不在渲染里");
    }
    VkTextureObj& target = static_cast<VkTextureObj&>(*c.target);

    VkPipeline pipe = pipelineFor(p, d, target.vfmt, c.depth_fmt, c.samples);
    if (pipe == VK_NULL_HANDLE) {
        return fail(BGPU_ERR_BACKEND_FAILED, __FILE__, __LINE__, "管线创建失败");
    }

    /* 纹理：没开 TEXTURED 或没绑纹理 → 1×1 预乘白（raster_rules §6） */
    VkTextureObj* tex = d.white;
    VkSamplerObj* samp = nullptr;
    if ((p.pipe_flags & BGPU_PIPE_TEXTURED) && c.tex) {
        tex = static_cast<VkTextureObj*>(c.tex);
        samp = c.samp ? static_cast<VkSamplerObj*>(c.samp) : nullptr;
    }
    /* 采样前必须把它转到 SHADER_READ_ONLY：目标刚渲染完（布局是 COLOR_ATTACHMENT）
     * 就要在这里转回来 —— 这个"来回横跳"是 end_render 之后**必须**补的那一步，
     * 因为离屏 pass 在帧内不会自动转（帧内的 end_render 只结束渲染实例）。
     * transition 自己会在布局已经对的时候跳过，不会白插 barrier。
     * 0.16：这张图如果挂着"延迟 resolve"，1× 还是旧的 —— 先补一次再采样。
     * 自愈会拆/合当前渲染实例，所以管线绑定放在它**后面**（resumePass 会清 last_pipe）。 */
    if (tex != &target && tex->resolve_pending) {
        const BgpuResult rr = resolvePending(c, d, *tex);
        if (rr != BGPU_OK) return rr;
    }
    if (pipe != c.last_pipe) {
        g_vk.vkCmdBindPipeline(c.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        c.last_pipe = pipe;
    }
    transition(c, *tex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    {
        const VkDescriptorSet set = allocDescSet(c, d);
        if (set == VK_NULL_HANDLE) {
            return fail(BGPU_ERR_OUT_OF_MEMORY, __FILE__, __LINE__, "descriptor set 分配失败");
        }
        VkDescriptorImageInfo ii{};
        ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        ii.imageView = tex->view;
        ii.sampler = samp ? samp->sampler : d.default_sampler;
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = set;
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &ii;
        g_vk.vkUpdateDescriptorSets(d.dev, 1, &w, 0, nullptr);
        g_vk.vkCmdBindDescriptorSets(c.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d.pipe_layout, 0, 1,
                                     &set, 0, nullptr);
    }

    /* scissor：与目标求交；负数宽度已在 setScissor 里变成"关闭裁剪" */
    VkRect2D sc{{0, 0}, {target.w, target.h}};
    if (cmd.scissorOn) {
        const int32_t x = std::max(0, cmd.sc[0]);
        const int32_t y = std::max(0, cmd.sc[1]);
        const int32_t r = std::min<int32_t>(target.w, cmd.sc[0] + cmd.sc[2]);
        const int32_t b = std::min<int32_t>(target.h, cmd.sc[1] + cmd.sc[3]);
        if (r <= x || b <= y) return BGPU_OK;   /* 空矩形：什么也不画 */
        sc = {{x, y}, {static_cast<uint32_t>(r - x), static_cast<uint32_t>(b - y)}};
    }
    g_vk.vkCmdSetScissor(c.cb, 0, 1, &sc);

    PushConstants pc{};
    fillPushConstants(cmd, /*perspective=*/false, target.w, target.h, pc);
    g_vk.vkCmdPushConstants(c.cb, d.pipe_layout,
                            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                            sizeof(pc), &pc);
    if (trace2())
        std::fprintf(stderr,
                     "[vk] push: target=%.0fx%.0f vp=(%.0f,%.0f,%.0f,%.0f) affine=%.1f "
                     "persp=%.0f lighting=%.0f lightDir=(%.2f,%.2f,%.2f)\n",
                     pc.target[0], pc.target[1], pc.vp[0], pc.vp[1], pc.vp[2], pc.vp[3],
                     pc.xf0[3], pc.flags[0], pc.flags[1], pc.light_dir[0], pc.light_dir[1],
                     pc.light_dir[2]);
    /* 顶点：追加到本帧的流缓冲，索引固定 TL,TR,BL / BL,TR,BR（raster_rules §2）。
     * 对齐理由同 rasterShapes：形状那一路是 36 字节的 stride，与 24 交错之后
     * `vused / sizeof(BgpuVertex)` 会截断。 */
    const uint64_t stride = sizeof(BgpuVertex);
    const uint64_t base = (c.vused + stride - 1) / stride * stride;
    const uint64_t bytes = static_cast<uint64_t>(quad_count) * 4 * stride;
    if (!ensureVertexSpace(c, d, (base - c.vused) + bytes)) {
        return fail(BGPU_ERR_OUT_OF_MEMORY, __FILE__, __LINE__, "顶点流扩容失败");
    }
    if (!ensureIndexSpace(c, d, quad_count)) {
        return fail(BGPU_ERR_OUT_OF_MEMORY, __FILE__, __LINE__, "索引缓冲扩容失败");
    }
    std::memcpy(static_cast<uint8_t*>(c.vmap) + base, verts, bytes);
    const uint32_t first_vertex = static_cast<uint32_t>(base / stride);
    c.vused = base + bytes;
    const VkDeviceSize off = 0;
    g_vk.vkCmdBindVertexBuffers(c.cb, 0, 1, &c.vbuf, &off);
    g_vk.vkCmdBindIndexBuffer(c.cb, c.ibuf, 0, VK_INDEX_TYPE_UINT32);
    g_vk.vkCmdDrawIndexed(c.cb, quad_count * 6, 1, 0, first_vertex, 0);
    return BGPU_OK;
}

/* ABI 4：解析圆角。流程与 rasterQuads 一致，少了纹理那一段（不采样），
 * 顶点流换成 36 字节的 BgpuShapeVertex —— 索引仍是同一张 "TL,TR,BL / BL,TR,BR"。 */
BgpuResult VkBackend::rasterShapes(Cmd& cmd, const BgpuShapeVertex* verts,
                                   uint32_t quad_count) {
    VkCmdObj& c = static_cast<VkCmdObj&>(cmd);
    VkDeviceObj& d = static_cast<VkDeviceObj&>(*lookupObject(cmd.device, kObjDevice));
    VkPipelineObj& p = static_cast<VkPipelineObj&>(*cmd.pipe);
    if (!c.rendering || !c.target) {
        return fail(BGPU_ERR_STATE, __FILE__, __LINE__, "draw 时不在渲染里");
    }
    if (!d.caps_shape) {
        return fail(BGPU_ERR_UNSUPPORTED, __FILE__, __LINE__,
                    "Vulkan 后端的解析圆角着色器不可用");
    }
    VkTextureObj& target = static_cast<VkTextureObj&>(*c.target);

    VkPipeline pipe = pipelineFor(p, d, target.vfmt, c.depth_fmt, c.samples);
    if (pipe == VK_NULL_HANDLE) {
        return fail(BGPU_ERR_BACKEND_FAILED, __FILE__, __LINE__, "管线创建失败");
    }
    if (pipe != c.last_pipe) {
        g_vk.vkCmdBindPipeline(c.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        c.last_pipe = pipe;
    }

    VkRect2D sc{{0, 0}, {target.w, target.h}};
    if (cmd.scissorOn) {
        const int32_t x = std::max(0, cmd.sc[0]);
        const int32_t y = std::max(0, cmd.sc[1]);
        const int32_t r = std::min<int32_t>(target.w, cmd.sc[0] + cmd.sc[2]);
        const int32_t b = std::min<int32_t>(target.h, cmd.sc[1] + cmd.sc[3]);
        if (r <= x || b <= y) return BGPU_OK;
        sc = {{x, y}, {static_cast<uint32_t>(r - x), static_cast<uint32_t>(b - y)}};
    }
    g_vk.vkCmdSetScissor(c.cb, 0, 1, &sc);

    PushConstants pc{};
    fillPushConstants(cmd, /*perspective=*/false, target.w, target.h, pc);
    g_vk.vkCmdPushConstants(c.cb, d.pipe_layout,
                            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                            sizeof(pc), &pc);

    /* 顶点流是 **2D（24 字节）与形状（36 字节）共用**的一段字节，而 first_vertex
     * 是按本笔的 stride 倒算的，所以追加前必须把游标对齐到本笔的 stride。
     * 不对齐就会截断：先画一个 2D 四边形（占 96 字节）再画形状时 96/36 = 2，
     * 顶点从第 72 字节开始读 —— 读到上一笔的尾巴，形状整块不见。
     * （bin2d 的 display_list 跨执行体对拍抓到的：VK 上"上半透明圆角渐变"整条消失。） */
    const uint64_t stride = sizeof(BgpuShapeVertex);
    const uint64_t base = (c.vused + stride - 1) / stride * stride;
    const uint64_t bytes = static_cast<uint64_t>(quad_count) * 4 * stride;
    if (!ensureVertexSpace(c, d, (base - c.vused) + bytes)) {
        return fail(BGPU_ERR_OUT_OF_MEMORY, __FILE__, __LINE__, "顶点流扩容失败");
    }
    if (!ensureIndexSpace(c, d, quad_count)) {
        return fail(BGPU_ERR_OUT_OF_MEMORY, __FILE__, __LINE__, "索引缓冲扩容失败");
    }
    std::memcpy(static_cast<uint8_t*>(c.vmap) + base, verts, bytes);
    const uint32_t first_vertex = static_cast<uint32_t>(base / stride);
    c.vused = base + bytes;
    const VkDeviceSize off = 0;
    g_vk.vkCmdBindVertexBuffers(c.cb, 0, 1, &c.vbuf, &off);
    g_vk.vkCmdBindIndexBuffer(c.cb, c.ibuf, 0, VK_INDEX_TYPE_UINT32);
    g_vk.vkCmdDrawIndexed(c.cb, quad_count * 6, 1, 0, first_vertex, 0);
    return BGPU_OK;
}

BgpuResult VkBackend::rasterIndexed(Cmd& cmd, Buffer& vb, uint32_t vertex_offset, Buffer& ib,
                                    uint32_t index_count, uint32_t index_offset) {
    VkCmdObj& c = static_cast<VkCmdObj&>(cmd);
    VkDeviceObj& d = static_cast<VkDeviceObj&>(*lookupObject(cmd.device, kObjDevice));
    VkPipelineObj& p = static_cast<VkPipelineObj&>(*cmd.pipe);
    if (!c.rendering || !c.target) {
        return fail(BGPU_ERR_STATE, __FILE__, __LINE__, "draw 时不在渲染里");
    }
    VkTextureObj& target = static_cast<VkTextureObj&>(*c.target);
    VkPipeline pipe = pipelineFor(p, d, target.vfmt, c.depth_fmt, c.samples);
    if (pipe == VK_NULL_HANDLE) {
        return fail(BGPU_ERR_BACKEND_FAILED, __FILE__, __LINE__, "管线创建失败");
    }
    VkTextureObj* tex = d.white;
    VkSamplerObj* samp = nullptr;
    if ((p.pipe_flags & BGPU_PIPE_TEXTURED) && c.tex) {
        tex = static_cast<VkTextureObj*>(c.tex);
        samp = c.samp ? static_cast<VkSamplerObj*>(c.samp) : nullptr;
    }
    /* 同 rasterQuads：采样前一定把布局转回 SHADER_READ_ONLY，并**逐笔**拿一个新的
     * descriptor set（set 内容是执行期读的，跨笔共用会串）。
     * 0.16：挂着延迟 resolve 的采样源先补一次（见 resolvePending）；管线绑定放在
     * 自愈之后（resumePass 会清 last_pipe）。 */
    if (tex != &target && tex->resolve_pending) {
        const BgpuResult rr = resolvePending(c, d, *tex);
        if (rr != BGPU_OK) return rr;
    }
    if (pipe != c.last_pipe) {
        g_vk.vkCmdBindPipeline(c.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        c.last_pipe = pipe;
    }
    transition(c, *tex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    {
        const VkDescriptorSet set = allocDescSet(c, d);
        if (set == VK_NULL_HANDLE) {
            return fail(BGPU_ERR_OUT_OF_MEMORY, __FILE__, __LINE__, "descriptor set 分配失败");
        }
        VkDescriptorImageInfo ii{};
        ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        ii.imageView = tex->view;
        ii.sampler = samp ? samp->sampler : d.default_sampler;
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = set;
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &ii;
        g_vk.vkUpdateDescriptorSets(d.dev, 1, &w, 0, nullptr);
        g_vk.vkCmdBindDescriptorSets(c.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d.pipe_layout, 0, 1,
                                     &set, 0, nullptr);
    }
    VkRect2D sc{{0, 0}, {target.w, target.h}};
    if (cmd.scissorOn) {
        const int32_t x = std::max(0, cmd.sc[0]);
        const int32_t y = std::max(0, cmd.sc[1]);
        const int32_t r = std::min<int32_t>(target.w, cmd.sc[0] + cmd.sc[2]);
        const int32_t b = std::min<int32_t>(target.h, cmd.sc[1] + cmd.sc[3]);
        if (r <= x || b <= y) return BGPU_OK;
        sc = {{x, y}, {static_cast<uint32_t>(r - x), static_cast<uint32_t>(b - y)}};
    }
    g_vk.vkCmdSetScissor(c.cb, 0, 1, &sc);

    PushConstants pc{};
    fillPushConstants(cmd, /*perspective=*/true, target.w, target.h, pc);
    g_vk.vkCmdPushConstants(c.cb, d.pipe_layout,
                            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                            sizeof(pc), &pc);
    if (trace2())
        std::fprintf(stderr,
                     "[vk] push: target=%.0fx%.0f vp=(%.0f,%.0f,%.0f,%.0f) affine=%.1f "
                     "persp=%.0f lighting=%.0f lightDir=(%.2f,%.2f,%.2f)\n",
                     pc.target[0], pc.target[1], pc.vp[0], pc.vp[1], pc.vp[2], pc.vp[3],
                     pc.xf0[3], pc.flags[0], pc.flags[1], pc.light_dir[0], pc.light_dir[1],
                     pc.light_dir[2]);
    VkBufferObj& vbo = static_cast<VkBufferObj&>(vb);
    VkBufferObj& ibo = static_cast<VkBufferObj&>(ib);
    const VkDeviceSize voff = vertex_offset;
    g_vk.vkCmdBindVertexBuffers(c.cb, 0, 1, &vbo.buf, &voff);
    g_vk.vkCmdBindIndexBuffer(c.cb, ibo.buf, index_offset, VK_INDEX_TYPE_UINT32);
    g_vk.vkCmdDrawIndexed(c.cb, index_count, 1, 0, 0, 0);
    return BGPU_OK;
}

}  // namespace

Backend& vkBackend() {
    static VkBackend be;
    return be;
}

}  // namespace bgpu

#endif  // BINGPU_HAS_VK
