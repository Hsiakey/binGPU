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
 *   · MSAA 这一版**关掉**（与 CPU 一致，caps.msaa_samples == 1）；
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
 *   · MSAA / mip 链 / 透明窗口（composite alpha）；
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

/* 与 quad.vert 的 push_constant 逐字段对应（48 字节） */
/* std430 布局（GLSL push_constant 块）：vec4 在 0，vec2 在 16，然后**补到 32**
 * 才是下一个 vec4 —— C++ 结构体必须逐字节对上，否则仿射矩阵读到的是错位数据。
 * （第一版就是这里错的：xf0 落在 24，GLSL 读 32。） */
struct PushConstants {
    float vp[4];       /* offset  0 */
    float target[2];   /* offset 16 */
    float pad[2];      /* offset 24（std430 对齐用，恒 0） */
    float xf0[4];      /* offset 32 */
    float xf1[4];      /* offset 48 */
};

inline bool trace2() {
    static const bool on = std::getenv("BINGPU_VK_TRACE") != nullptr;
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
    VkDescriptorPool desc_pool = VK_NULL_HANDLE;
    VkPipelineLayout pipe_layout = VK_NULL_HANDLE;
    VkShaderModule vert = VK_NULL_HANDLE;
    VkShaderModule frag = VK_NULL_HANDLE;
    VkSampler default_sampler = VK_NULL_HANDLE;
    VkFormat swap_format = VK_FORMAT_B8G8R8A8_UNORM;
    VkFence fence = VK_NULL_HANDLE;
    VkTextureObj* white = nullptr;   /* 1×1 预乘白：未绑纹理时的采样源 */
    BgpuTexture white_handle = BGPU_INVALID;
    bool windowed = false;
    bool lost = false;

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
    /* 交换链借出的目标是**别人的** image/view：析构时绝不能销毁（第一版就是
     * 把交换链的 image 当自己的销毁，窗口烟测直接崩）。 */
    bool own_image = true;
    VkDevice dev = VK_NULL_HANDLE;

    VkTextureObj(Backend& b, BgpuDevice owner) : Texture(b, owner) {}
    ~VkTextureObj() override {
        if (!dev) return;
        if (own_image) {
            if (view) g_vk.vkDestroyImageView(dev, view, nullptr);
            if (image) g_vk.vkDestroyImage(dev, image, nullptr);
            if (mem) g_vk.vkFreeMemory(dev, mem, nullptr);
        }
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
    VkDescriptorSet desc = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkFormat color_fmt = VK_FORMAT_UNDEFINED;
    VkFormat depth_fmt = VK_FORMAT_UNDEFINED;

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

    /* 状态剔除（docs/layering.md §6.2 写的契约）：只有真的变了才记 vkCmd* */
    VkPipeline last_pipe = VK_NULL_HANDLE;
    VkDescriptorSet last_desc = VK_NULL_HANDLE;
    VkTextureObj* last_tex = nullptr;
    VkSamplerObj* last_samp = nullptr;
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
        /* cb 由设备销毁时随 command pool 一起回收（pool 非持久），这里不单 free */
    }
};

VkDeviceObj::~VkDeviceObj() {
    if (trace2()) std::fprintf(stderr, "[vk] ~VkDeviceObj dev=%p\n", (void*)dev);
    if (dev) {
        g_vk.vkDeviceWaitIdle(dev);
        if (fence) g_vk.vkDestroyFence(dev, fence, nullptr);
        if (default_sampler) g_vk.vkDestroySampler(dev, default_sampler, nullptr);
        if (vert) g_vk.vkDestroyShaderModule(dev, vert, nullptr);
        if (frag) g_vk.vkDestroyShaderModule(dev, frag, nullptr);
        if (pipe_layout) g_vk.vkDestroyPipelineLayout(dev, pipe_layout, nullptr);
        if (desc_pool) g_vk.vkDestroyDescriptorPool(dev, desc_pool, nullptr);
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

protected:
    BgpuResult rasterQuads(Cmd& cmd, const BgpuVertex* verts, uint32_t quad_count) override;
    BgpuResult rasterIndexed(Cmd& cmd, Buffer& vb, uint32_t vertex_offset, Buffer& ib,
                             uint32_t index_count, uint32_t index_offset) override;
    BgpuResult prepareDepth(Texture& depth, Texture& target,
                            const BgpuRenderBeginDesc& desc) override;
    BgpuResult clearColor(Texture&, uint32_t) override { return BGPU_OK; }  /* clear 用 loadOp */
    void presentFrame(Swapchain&) override {}                              /* frameEnd 里做 */

private:
    BgpuResult transition(VkCmdObj& c, VkTextureObj& t, VkImageLayout want);
    bool beginRecording(VkCmdObj& c, VkDeviceObj& dev);
    VkPipeline pipelineFor(VkPipelineObj& p, VkDeviceObj& dev, VkFormat color_fmt,
                           VkFormat depth_fmt);
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

    VkDescriptorPoolSize psz{};
    psz.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    psz.descriptorCount = 128;
    VkDescriptorPoolCreateInfo dpc{};
    dpc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpc.maxSets = 64;
    dpc.poolSizeCount = 1;
    dpc.pPoolSizes = &psz;
    vr = g_vk.vkCreateDescriptorPool(vkdev, &dpc, nullptr, &d->desc_pool);
    if (vr != VK_SUCCESS) {
        destroyObject(handle);
        return vkFail(vr, "vkCreateDescriptorPool");
    }

    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
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
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
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
    (void)dev;
    out.flags = BGPU_CAP_DEPTH | BGPU_CAP_OFFSCREEN | BGPU_CAP_READBACK | BGPU_CAP_PRESENT_PACES |
                BGPU_CAP_VSYNC | BGPU_CAP_SAMPLERS | BGPU_CAP_AFFINE | BGPU_CAP_MESH;
    out.max_texture_size = BGPU_MAX_TEXTURE_SIZE;
    out.max_frames_in_flight = 1;   /* 每帧 submit + 等 fence */
    out.msaa_samples = 1;           /* 与 CPU 参考后端一致（0 差验收的前提） */
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
    if (desc.flags & BGPU_TEXTURE_MIPMAPS) {
        return fail(BGPU_ERR_UNSUPPORTED, __FILE__, __LINE__,
                    "Vulkan 后端这一版不支持 mip 链（见 docs/backends.md 欠账）");
    }
    const uint64_t handle = makeObject<VkTextureObj>(*this, d.self);
    if (handle == BGPU_INVALID) return failOom(__FILE__, __LINE__);
    VkTextureObj* t = static_cast<VkTextureObj*>(lookupObject(handle, kObjTexture));
    t->w = desc.width;
    t->h = desc.height;
    t->fmt = desc.format;
    t->tex_flags = desc.flags;
    t->mip_levels = 1;
    t->dev = d.dev;
    t->vfmt = toVkFormat(desc.format);

    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = t->vfmt;
    ici.extent = {desc.width, desc.height, 1};
    ici.mipLevels = 1;
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
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
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
    if (t.layout == want) return BGPU_OK;
    const VkImageAspectFlags aspect =
        t.is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = t.layout;
    b.newLayout = want;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = t.image;
    b.subresourceRange = {aspect, 0, 1, 0, 1};

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
            b.srcAccessMask = 0;
            b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
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

    std::vector<uint8_t> tmp(bytes);
    for (uint32_t y = 0; y < b.h; y++) {
        std::memcpy(tmp.data() + static_cast<size_t>(y) * b.w * bpp,
                    static_cast<const uint8_t*>(pixels) + static_cast<size_t>(y) * src_stride,
                    static_cast<size_t>(b.w) * bpp);
    }
    /* 直通 → 预乘：**局部上传也要预乘**（CPU 是按区域预乘的；只在整张上传时预乘
     * 会让"局部更新的半透明贴图"混出偏亮的结果 —— 0.3.0 就是这么写的，
     * tests/gpu_parity.h 的 texture(box) 用例把它钉住了）。 */
    if (bpp == 4) premultiply(tmp.data(), static_cast<size_t>(b.w) * b.h);

    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    void* staging_map = nullptr;

    if (!allocHostBuffer(d, bytes, &staging, &staging_mem, &staging_map)) {
        return fail(BGPU_ERR_OUT_OF_MEMORY, __FILE__, __LINE__, "上传暂存分配失败");
    }
    std::memcpy(staging_map, tmp.data(), bytes);

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
        transition(tmp_cmd, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageOffset = {static_cast<int32_t>(b.x), static_cast<int32_t>(b.y), 0};
        region.imageExtent = {b.w, b.h, 1};
        g_vk.vkCmdCopyBufferToImage(cb, staging, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    1, &region);
        transition(tmp_cmd, t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
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
        transition(tmp_cmd, t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {t.w, t.h, 1};
        g_vk.vkCmdCopyImageToBuffer(cb, t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1,
                                    &region);
        transition(tmp_cmd, t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        g_vk.vkEndCommandBuffer(cb);
        g_vk.vkResetFences(dev.dev, 1, &dev.fence);
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        ok = g_vk.vkQueueSubmit(dev.queue, 1, &si, dev.fence) == VK_SUCCESS &&
             g_vk.vkWaitForFences(dev.dev, 1, &dev.fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
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
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
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
                                  VkFormat depth_fmt) {
    const uint64_t key = (static_cast<uint64_t>(color_fmt) << 40) |
                         (static_cast<uint64_t>(depth_fmt) << 24) |
                         (static_cast<uint64_t>(p.layout) << 16) |
                         (static_cast<uint64_t>(p.blend) << 8) | p.pipe_flags;
    auto it = p.cache.find(key);
    if (it != p.cache.end()) return it->second;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = dev.vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = dev.frag;
    stages[1].pName = "main";

    VkVertexInputBindingDescription bind{};
    VkVertexInputAttributeDescription attrs[3]{};
    VkPipelineVertexInputStateCreateInfo vin{};
    vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    if (p.layout == BGPU_VERTEX_2D) {
        bind.binding = 0;
        bind.stride = sizeof(BgpuVertex);   /* 24 */
        bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
        attrs[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, 12};
        attrs[2] = {2, 0, VK_FORMAT_R8G8B8A8_UNORM, 20};
    } else {
        bind.binding = 0;
        bind.stride = sizeof(BgpuMeshVertex);   /* 36 */
        bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
        attrs[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, 24};
        attrs[2] = {2, 0, VK_FORMAT_R8G8B8A8_UNORM, 32};
    }
    vin.vertexBindingDescriptionCount = 1;
    vin.pVertexBindingDescriptions = &bind;
    vin.vertexAttributeDescriptionCount = 3;
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
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

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
    c.vused = 0;
    c.have_state = false;
    c.last_pipe = VK_NULL_HANDLE;
    c.last_desc = VK_NULL_HANDLE;
    c.last_tex = nullptr;
    c.last_samp = nullptr;
    c.color_fmt = VK_FORMAT_UNDEFINED;
    c.depth_fmt = VK_FORMAT_UNDEFINED;
    return true;
}

bool VkBackend::ensureVertexSpace(VkCmdObj& c, VkDeviceObj& dev, uint64_t need) {
    if (c.vcap >= c.vused + need) return true;
    uint64_t cap = c.vcap ? c.vcap : 64 * 1024;
    while (cap < c.vused + need) cap *= 2;
    if (c.vmap) {
        g_vk.vkUnmapMemory(dev.dev, c.vmem);
        c.vmap = nullptr;
    }
    if (c.vbuf) g_vk.vkDestroyBuffer(dev.dev, c.vbuf, nullptr);
    if (c.vmem) g_vk.vkFreeMemory(dev.dev, c.vmem, nullptr);
    c.vbuf = VK_NULL_HANDLE;
    c.vmem = VK_NULL_HANDLE;
    if (!allocHostBuffer(dev, cap, &c.vbuf, &c.vmem, &c.vmap)) {
        c.vcap = 0;
        return false;
    }
    c.vcap = cap;
    return true;
}

bool VkBackend::ensureIndexSpace(VkCmdObj& c, VkDeviceObj& dev, uint64_t quads) {
    if (c.icap_quads >= quads) return true;
    uint64_t cap = c.icap_quads ? c.icap_quads : 4096;
    while (cap < quads) cap *= 2;
    if (c.ibuf) g_vk.vkDestroyBuffer(dev.dev, c.ibuf, nullptr);
    if (c.imem) g_vk.vkFreeMemory(dev.dev, c.imem, nullptr);
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
    c.ibuf = buf;
    c.imem = mem;
    c.icap_quads = cap;
    return true;
}

bool VkBackend::submitNow(VkDeviceObj& dev, VkCmdObj& c) {
    if (!c.recording) return false;
    g_vk.vkEndCommandBuffer(c.cb);
    c.recording = false;
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &c.cb;
    g_vk.vkResetFences(dev.dev, 1, &dev.fence);
    if (g_vk.vkQueueSubmit(dev.queue, 1, &si, dev.fence) != VK_SUCCESS) return false;
    return g_vk.vkWaitForFences(dev.dev, 1, &dev.fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
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
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = d.desc_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &d.set_layout;
    if (g_vk.vkAllocateDescriptorSets(d.dev, &ai, &c->desc) != VK_SUCCESS) {
        destroyObject(*out);
        return fail(BGPU_ERR_OUT_OF_MEMORY, __FILE__, __LINE__, "descriptor set 分配失败");
    }
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
    out.msaa_samples = 1;
    out.present_paces_frames = (sc.sc_flags & BGPU_DEVICE_VSYNC) ? 1 : 0;
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
    sc.borrow->w = sc.extent.width;
    sc.borrow->h = sc.extent.height;

    c.inFrame = true;
    c.frame = &sc_;
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
    VkImageView depth_view = VK_NULL_HANDLE;
    c.depth_fmt = VK_FORMAT_UNDEFINED;
    if (depth) {
        VkTextureObj& dt = static_cast<VkTextureObj&>(*depth);
        c.depth_fmt = dt.vfmt;
        depth_view = dt.view;
    }
    if (transition(c, t, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) != BGPU_OK) {
        cmd.inRender = false;
        return BGPU_ERR_BACKEND_FAILED;
    }
    if (depth) {
        transition(c, static_cast<VkTextureObj&>(*depth), VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    }

    VkRenderingAttachmentInfo color_att{};
    color_att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color_att.imageView = t.view;
    color_att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
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
    BgpuResult r = BackendBase::endRender(cmd);
    if (r != BGPU_OK) return r;

    if (!c.inFrame) {
        /* 离屏：这一段自己提交并等完成，之后纹理可采样/可回读 */
        VkDeviceObj& d = static_cast<VkDeviceObj&>(*lookupObject(cmd.device, kObjDevice));
        if (t) transition(c, *t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (!submitNow(d, c)) {
            return fail(BGPU_ERR_BACKEND_FAILED, __FILE__, __LINE__, "离屏提交失败");
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
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = dt.vfmt;
    ici.extent = {dt.w, dt.h, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
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

    VkPipeline pipe = pipelineFor(p, d, target.vfmt, c.depth_fmt);
    if (pipe == VK_NULL_HANDLE) {
        return fail(BGPU_ERR_BACKEND_FAILED, __FILE__, __LINE__, "管线创建失败");
    }
    if (pipe != c.last_pipe) {
        g_vk.vkCmdBindPipeline(c.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        c.last_pipe = pipe;
    }

    /* 纹理：没开 TEXTURED 或没绑纹理 → 1×1 预乘白（raster_rules §6） */
    VkTextureObj* tex = d.white;
    VkSamplerObj* samp = nullptr;
    if ((p.pipe_flags & BGPU_PIPE_TEXTURED) && c.tex) {
        tex = static_cast<VkTextureObj*>(c.tex);
        samp = c.samp ? static_cast<VkSamplerObj*>(c.samp) : nullptr;
    }
    if (tex != c.last_tex || samp != c.last_samp) {
        transition(c, *tex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        VkDescriptorImageInfo ii{};
        ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        ii.imageView = tex->view;
        ii.sampler = samp ? samp->sampler : d.default_sampler;
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = c.desc;
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &ii;
        g_vk.vkUpdateDescriptorSets(d.dev, 1, &w, 0, nullptr);
        c.last_tex = tex;
        c.last_samp = samp;
        c.last_desc = VK_NULL_HANDLE;   /* 强制重绑 */
    }
    if (c.last_desc != c.desc) {
        g_vk.vkCmdBindDescriptorSets(c.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d.pipe_layout, 0, 1,
                                     &c.desc, 0, nullptr);
        c.last_desc = c.desc;
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
    pc.vp[0] = cmd.vp[0];
    pc.vp[1] = cmd.vp[1];
    pc.vp[2] = cmd.vp[2];
    pc.vp[3] = cmd.vp[3];
    pc.target[0] = static_cast<float>(target.w);
    pc.target[1] = static_cast<float>(target.h);
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
    g_vk.vkCmdPushConstants(c.cb, d.pipe_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
    if (trace2())
        std::fprintf(stderr,
                     "[vk] push: target=%.0fx%.0f vp=(%.0f,%.0f,%.0f,%.0f) affine=%.1f "
                     "xf0=(%.2f,%.2f,%.2f) xf1=(%.2f,%.2f,%.2f)\n",
                     pc.target[0], pc.target[1], pc.vp[0], pc.vp[1], pc.vp[2], pc.vp[3],
                     pc.xf0[3], pc.xf0[0], pc.xf0[1], pc.xf0[2], pc.xf1[0], pc.xf1[1],
                     pc.xf1[2]);
    /* 顶点：追加到本帧的流缓冲，索引固定 TL,TR,BL / BL,TR,BR（raster_rules §2） */
    const uint64_t bytes = static_cast<uint64_t>(quad_count) * 4 * sizeof(BgpuVertex);
    if (!ensureVertexSpace(c, d, bytes)) {
        return fail(BGPU_ERR_OUT_OF_MEMORY, __FILE__, __LINE__, "顶点流扩容失败");
    }
    if (!ensureIndexSpace(c, d, quad_count)) {
        return fail(BGPU_ERR_OUT_OF_MEMORY, __FILE__, __LINE__, "索引缓冲扩容失败");
    }
    std::memcpy(static_cast<uint8_t*>(c.vmap) + c.vused, verts, bytes);
    const uint32_t first_vertex = static_cast<uint32_t>(c.vused / sizeof(BgpuVertex));
    c.vused += bytes;
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
    VkPipeline pipe = pipelineFor(p, d, target.vfmt, c.depth_fmt);
    if (pipe == VK_NULL_HANDLE) {
        return fail(BGPU_ERR_BACKEND_FAILED, __FILE__, __LINE__, "管线创建失败");
    }
    if (pipe != c.last_pipe) {
        g_vk.vkCmdBindPipeline(c.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        c.last_pipe = pipe;
    }
    VkTextureObj* tex = d.white;
    VkSamplerObj* samp = nullptr;
    if ((p.pipe_flags & BGPU_PIPE_TEXTURED) && c.tex) {
        tex = static_cast<VkTextureObj*>(c.tex);
        samp = c.samp ? static_cast<VkSamplerObj*>(c.samp) : nullptr;
    }
    if (tex != c.last_tex || samp != c.last_samp) {
        transition(c, *tex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        VkDescriptorImageInfo ii{};
        ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        ii.imageView = tex->view;
        ii.sampler = samp ? samp->sampler : d.default_sampler;
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = c.desc;
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &ii;
        g_vk.vkUpdateDescriptorSets(d.dev, 1, &w, 0, nullptr);
        c.last_tex = tex;
        c.last_samp = samp;
        c.last_desc = VK_NULL_HANDLE;
    }
    if (c.last_desc != c.desc) {
        g_vk.vkCmdBindDescriptorSets(c.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d.pipe_layout, 0, 1,
                                     &c.desc, 0, nullptr);
        c.last_desc = c.desc;
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
    pc.vp[0] = cmd.vp[0];
    pc.vp[1] = cmd.vp[1];
    pc.vp[2] = cmd.vp[2];
    pc.vp[3] = cmd.vp[3];
    pc.target[0] = static_cast<float>(target.w);
    pc.target[1] = static_cast<float>(target.h);
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
    g_vk.vkCmdPushConstants(c.cb, d.pipe_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
    if (trace2())
        std::fprintf(stderr,
                     "[vk] push: target=%.0fx%.0f vp=(%.0f,%.0f,%.0f,%.0f) affine=%.1f "
                     "xf0=(%.2f,%.2f,%.2f) xf1=(%.2f,%.2f,%.2f)\n",
                     pc.target[0], pc.target[1], pc.vp[0], pc.vp[1], pc.vp[2], pc.vp[3],
                     pc.xf0[3], pc.xf0[0], pc.xf0[1], pc.xf0[2], pc.xf1[0], pc.xf1[1],
                     pc.xf1[2]);
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
