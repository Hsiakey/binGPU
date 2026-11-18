/* bingpu_gl.cpp —— OpenGL 后端（第三个真实后端，异族模型的第二个代表）
 *
 * 为什么它是"异族"里更极端的那一个（见 docs/layering.md §4）：
 *   GL 没有命令缓冲、没有 PSO、没有资源对象的所有权概念（纹理/缓冲都是**绑定点上的
 *   全局状态**），连"深度附件"都不是独立对象而是 framebuffer 的一个挂点。binGPU 的
 *   `BgpuCmd` 在这里退化成"当前状态 + 立即提交"，和 D3D11 那条路同构，但状态的存放
 *   位置完全不同 —— 于是它是"这套 ABI 到底是不是 API 世代无关"的最后一块拼图。
 *
 * 与 CPU 参考后端的关系：CPU 后端是**像素权威**（docs/backends.md），本后端必须和它
 * 逐像素一致。规格是 docs/raster_rules.md，验收是 tests/test_gl_render.c（与 VK/D3D11
 * 共用 tests/gpu_parity.h 的同一份 12 个用例）。
 *
 * 构建不依赖任何 GL 开发包：
 *   · 不 include <GL/gl.h>（它只有 1.1 的枚举），本文件自带所需的类型与常量；
 *   · 运行期只 LoadLibrary("opengl32.dll") / ("gdi32.dll")，**不链接** OpenGL32.lib；
 *   · 函数一律 wglGetProcAddress 优先、opengl32.dll 兜底（1.1 的老函数走后者）。
 *
 * 两个必须写清楚的坐标约定（GL 的窗口原点在**左下**，与 VK/D3D 都不同）：
 *   ① 顶点着色器：`ndc.y = 1 - 2*s.y/h`（和 D3D 一样，因为 GL 的 NDC y 也向上）；
 *   ② 渲染到**默认帧缓冲**（窗口）时图像要"翻一次"才正，而渲染到 **FBO**（离屏纹理）
 *      时不翻 —— 于是 `uFlip` 是 0/1 的 uniform，读回与采样两边就都自洽：
 *      FBO 的存储行 0 = s.y=0（图像顶行），glReadPixels 也从下往上吐，正好对上；
 *      窗口那条路在读回时把行序翻回来。
 *   ③ 裁剪矩形：FBO 上窗口 y == s.y；窗口目标要换成 `h - y - hh`。
 * 这三条是 GL 后端唯一"和另外两个后端写法不同"的地方，其余逐行同构。
 *
 * 这一版明确没做（写进 docs/backends.md 的欠账）：
 *   · MSAA / mip 链 / 透明窗口 / 多帧飞行（与 VK/D3D11 一致）；
 *   · 一个 HWND 只能绑一个 GL 设备（WGL 的上下文绑在 DC 上，`contracts.md` §4 也这么规定）。
 */
#include "bingpu_backend.h"

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#define BINGPU_HAS_GL 1
#endif

#ifndef BINGPU_HAS_GL

namespace bgpu {
namespace {
class StubGlBackend final : public Backend {
public:
    BgpuBackendKind kind() const noexcept override { return BGPU_BACKEND_GL; }
    const char* name() const noexcept override { return "gl"; }
    bool available() const noexcept override { return false; }
    const char* unavailableReason() const noexcept override {
        return "这个平台没有 WGL（binGPU 的 GL 后端只做了 Windows/wgl 的路径）";
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

Backend& glBackend() {
    static StubGlBackend be;
    return be;
}

}  // namespace bgpu

#else /* ============================= 真实现 ============================= */

#include <windows.h>

namespace bgpu {
namespace {

/* ---------------------------------------------------------------
 * GL 的类型与常量：只抄本文件用到的那些
 *
 * 为什么不 include <GL/gl.h>：MinGW 那份头只有 1.1 的枚举（GL_ARRAY_BUFFER、
 * GL_FRAMEBUFFER 之类全没有），而系统头里还会声明一批同名函数原型 —— 那些原型会
 * 和我们自己的函数指针表打架。既然这一层只用到固定的一小组入口，自带定义最干净
 * （数字全部来自 GL 规范，与平台/驱动无关）。
 * --------------------------------------------------------------- */
typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef float GLfloat;
typedef double GLdouble;
typedef unsigned int GLbitfield;
typedef unsigned char GLboolean;
typedef unsigned char GLubyte;
typedef char GLchar;
typedef signed long long GLint64;
typedef signed long long GLsizeiptr;
typedef signed long long GLintptr;
typedef void GLvoid;

enum : GLenum {
    GL_FALSE = 0,
    GL_TRUE = 1,
    GL_ZERO = 0,
    GL_ONE = 1,
    GL_TRIANGLES = 0x0004,
    GL_UNSIGNED_BYTE = 0x1401,
    GL_UNSIGNED_INT = 0x1405,
    GL_FLOAT = 0x1406,
    GL_RED = 0x1903,
    GL_RGBA = 0x1908,
    GL_DEPTH_COMPONENT = 0x1902,
    GL_VERSION = 0x1F02,
    GL_RENDERER = 0x1F01,
    GL_MAJOR_VERSION = 0x821B,
    GL_MINOR_VERSION = 0x821C,
    GL_NEAREST = 0x2600,
    GL_LINEAR = 0x2601,
    GL_TEXTURE_MAG_FILTER = 0x2800,
    GL_TEXTURE_MIN_FILTER = 0x2801,
    GL_TEXTURE_WRAP_S = 0x2802,
    GL_TEXTURE_WRAP_T = 0x2803,
    GL_REPEAT = 0x2901,
    GL_CLAMP_TO_EDGE = 0x812F,
    GL_TEXTURE_2D = 0x0DE1,
    GL_TEXTURE0 = 0x84C0,
    GL_R8 = 0x8229,
    GL_RGBA8 = 0x8058,
    GL_DEPTH_COMPONENT24 = 0x81A6,
    GL_BGRA = 0x80E1,
    GL_BLEND = 0x0BE2,
    GL_SRC_ALPHA = 0x0302,
    GL_ONE_MINUS_SRC_ALPHA = 0x0303,
    GL_DEPTH_TEST = 0x0B71,
    GL_LESS = 0x0201,
    GL_CULL_FACE = 0x0B44,
    GL_BACK = 0x0405,
    GL_CCW = 0x0901,
    GL_CW = 0x0900,
    GL_SCISSOR_TEST = 0x0C11,
    GL_COLOR_BUFFER_BIT = 0x00004000,
    GL_DEPTH_BUFFER_BIT = 0x00000100,
    GL_PACK_ALIGNMENT = 0x0D05,
    GL_UNPACK_ALIGNMENT = 0x0CF5,
    GL_MAX_TEXTURE_SIZE = 0x0D33,
    GL_ARRAY_BUFFER = 0x8892,
    GL_ELEMENT_ARRAY_BUFFER = 0x8893,
    GL_STREAM_DRAW = 0x88E0,
    GL_STATIC_DRAW = 0x88E4,
    GL_DYNAMIC_DRAW = 0x88E8,
    GL_VERTEX_SHADER = 0x8B31,
    GL_FRAGMENT_SHADER = 0x8B30,
    GL_COMPILE_STATUS = 0x8B81,
    GL_LINK_STATUS = 0x8B82,
    GL_INFO_LOG_LENGTH = 0x8B84,
    GL_FRAMEBUFFER = 0x8D40,
    GL_RENDERBUFFER = 0x8D41,
    GL_COLOR_ATTACHMENT0 = 0x8CE0,
    GL_DEPTH_ATTACHMENT = 0x8D00,
    GL_FRAMEBUFFER_COMPLETE = 0x8CD5,
    GL_READ_FRAMEBUFFER = 0x8CA8,
    GL_DRAW_FRAMEBUFFER = 0x8CA9,
    GL_TEXTURE_BINDING_2D = 0x8069
};

/* WGL 的两个扩展常量（wglext.h 里的值） */
enum : int {
    WGL_CONTEXT_MAJOR_VERSION_ARB = 0x2091,
    WGL_CONTEXT_MINOR_VERSION_ARB = 0x2092,
    WGL_CONTEXT_PROFILE_MASK_ARB = 0x9126,
    WGL_CONTEXT_CORE_PROFILE_BIT_ARB = 0x0001
};

/* ---------------------------------------------------------------
 * 动态加载：opengl32.dll / gdi32.dll（运行期），构建期零链接
 * --------------------------------------------------------------- */
struct GlFns {
    HMODULE gl = nullptr;
    HMODULE gdi = nullptr;

    /* WGL */
    HGLRC(APIENTRY* wglCreateContext)(HDC) = nullptr;
    BOOL(APIENTRY* wglDeleteContext)(HGLRC) = nullptr;
    BOOL(APIENTRY* wglMakeCurrent)(HDC, HGLRC) = nullptr;
    HGLRC(APIENTRY* wglGetCurrentContext)() = nullptr;
    PROC(APIENTRY* wglGetProcAddress)(LPCSTR) = nullptr;
    HGLRC(APIENTRY* wglCreateContextAttribsARB)(HDC, HGLRC, const int*) = nullptr;
    BOOL(APIENTRY* wglSwapIntervalEXT)(int) = nullptr;

    /* GDI（像素格式 / 呈现） */
    int(APIENTRY* ChoosePixelFormat)(HDC, const PIXELFORMATDESCRIPTOR*) = nullptr;
    BOOL(APIENTRY* SetPixelFormat)(HDC, int, const PIXELFORMATDESCRIPTOR*) = nullptr;
    int(APIENTRY* DescribePixelFormat)(HDC, int, UINT, PIXELFORMATDESCRIPTOR*) = nullptr;
    BOOL(APIENTRY* SwapBuffers)(HDC) = nullptr;

    /* GL 1.1（opengl32.dll 直接导出） */
    GLenum(APIENTRY* glGetError)() = nullptr;
    const GLubyte*(APIENTRY* glGetString)(GLenum) = nullptr;
    void(APIENTRY* glGetIntegerv)(GLenum, GLint*) = nullptr;
    void(APIENTRY* glEnable)(GLenum) = nullptr;
    void(APIENTRY* glDisable)(GLenum) = nullptr;
    void(APIENTRY* glViewport)(GLint, GLint, GLsizei, GLsizei) = nullptr;
    void(APIENTRY* glScissor)(GLint, GLint, GLsizei, GLsizei) = nullptr;
    void(APIENTRY* glClearColor)(GLfloat, GLfloat, GLfloat, GLfloat) = nullptr;
    void(APIENTRY* glClear)(GLbitfield) = nullptr;
    void(APIENTRY* glClearDepth)(GLdouble) = nullptr;
    void(APIENTRY* glDepthFunc)(GLenum) = nullptr;
    void(APIENTRY* glDepthMask)(GLboolean) = nullptr;
    void(APIENTRY* glCullFace)(GLenum) = nullptr;
    void(APIENTRY* glFrontFace)(GLenum) = nullptr;
    void(APIENTRY* glPixelStorei)(GLenum, GLint) = nullptr;
    void(APIENTRY* glGenTextures)(GLsizei, GLuint*) = nullptr;
    void(APIENTRY* glBindTexture)(GLenum, GLuint) = nullptr;
    void(APIENTRY* glDeleteTextures)(GLsizei, const GLuint*) = nullptr;
    void(APIENTRY* glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum,
                                 const void*) = nullptr;
    void(APIENTRY* glTexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum,
                                    const void*) = nullptr;
    void(APIENTRY* glTexParameteri)(GLenum, GLenum, GLint) = nullptr;
    void(APIENTRY* glDrawElements)(GLenum, GLsizei, GLenum, const void*) = nullptr;
    void(APIENTRY* glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*) = nullptr;
    void(APIENTRY* glFinish)() = nullptr;
    void(APIENTRY* glFlush)() = nullptr;
    void(APIENTRY* glGenFramebuffers)(GLsizei, GLuint*) = nullptr;
    void(APIENTRY* glBindFramebuffer)(GLenum, GLuint) = nullptr;
    void(APIENTRY* glFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint) = nullptr;
    void(APIENTRY* glFramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint) = nullptr;
    GLenum(APIENTRY* glCheckFramebufferStatus)(GLenum) = nullptr;
    void(APIENTRY* glDeleteFramebuffers)(GLsizei, const GLuint*) = nullptr;
    void(APIENTRY* glGenRenderbuffers)(GLsizei, GLuint*) = nullptr;
    void(APIENTRY* glBindRenderbuffer)(GLenum, GLuint) = nullptr;
    void(APIENTRY* glRenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei) = nullptr;
    void(APIENTRY* glDeleteRenderbuffers)(GLsizei, const GLuint*) = nullptr;

    /* GL 1.2+ / 2.0 / 3.0 / 3.3（wglGetProcAddress） */
    void(APIENTRY* glBlendFuncSeparate)(GLenum, GLenum, GLenum, GLenum) = nullptr;
    void(APIENTRY* glActiveTexture)(GLenum) = nullptr;
    void(APIENTRY* glGenSamplers)(GLsizei, GLuint*) = nullptr;
    void(APIENTRY* glBindSampler)(GLuint, GLuint) = nullptr;
    void(APIENTRY* glSamplerParameteri)(GLuint, GLenum, GLint) = nullptr;
    void(APIENTRY* glDeleteSamplers)(GLsizei, const GLuint*) = nullptr;
    void(APIENTRY* glGenBuffers)(GLsizei, GLuint*) = nullptr;
    void(APIENTRY* glBindBuffer)(GLenum, GLuint) = nullptr;
    void(APIENTRY* glBufferData)(GLenum, GLsizeiptr, const void*, GLenum) = nullptr;
    void(APIENTRY* glBufferSubData)(GLenum, GLintptr, GLsizeiptr, const void*) = nullptr;
    void(APIENTRY* glDeleteBuffers)(GLsizei, const GLuint*) = nullptr;
    void(APIENTRY* glGenVertexArrays)(GLsizei, GLuint*) = nullptr;
    void(APIENTRY* glBindVertexArray)(GLuint) = nullptr;
    void(APIENTRY* glDeleteVertexArrays)(GLsizei, const GLuint*) = nullptr;
    void(APIENTRY* glEnableVertexAttribArray)(GLuint) = nullptr;
    void(APIENTRY* glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei,
                                          const void*) = nullptr;
    GLuint(APIENTRY* glCreateShader)(GLenum) = nullptr;
    void(APIENTRY* glShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*) = nullptr;
    void(APIENTRY* glCompileShader)(GLuint) = nullptr;
    void(APIENTRY* glGetShaderiv)(GLuint, GLenum, GLint*) = nullptr;
    void(APIENTRY* glGetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*) = nullptr;
    void(APIENTRY* glDeleteShader)(GLuint) = nullptr;
    GLuint(APIENTRY* glCreateProgram)() = nullptr;
    void(APIENTRY* glAttachShader)(GLuint, GLuint) = nullptr;
    void(APIENTRY* glLinkProgram)(GLuint) = nullptr;
    void(APIENTRY* glGetProgramiv)(GLuint, GLenum, GLint*) = nullptr;
    void(APIENTRY* glGetProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*) = nullptr;
    void(APIENTRY* glUseProgram)(GLuint) = nullptr;
    void(APIENTRY* glDeleteProgram)(GLuint) = nullptr;
    GLint(APIENTRY* glGetUniformLocation)(GLuint, const GLchar*) = nullptr;
    void(APIENTRY* glUniform1i)(GLint, GLint) = nullptr;
    void(APIENTRY* glUniform1f)(GLint, GLfloat) = nullptr;
    void(APIENTRY* glUniform2fv)(GLint, GLsizei, const GLfloat*) = nullptr;
    void(APIENTRY* glUniform4fv)(GLint, GLsizei, const GLfloat*) = nullptr;
};

GlFns g_gl;
bool g_loadTried = false;
bool g_loadOk = false;

inline bool trace4() {
    static const bool on = std::getenv("BINGPU_GL_TRACE") != nullptr;
    return on;
}

template <class Fn>
bool bindP(void* (*resolve)(const char*), const char* name, Fn* out) {
    void* raw = resolve(name);
    if (!raw) return false;
    std::memcpy(out, &raw, sizeof(raw));
    return true;
}

/* 按名字在指定 DLL 里找导出（wgl/gdi 的入口在 opengl32.dll / gdi32.dll 里） */
HMODULE g_resolve_lib = nullptr;
void* libProc(const char* name) {
    if (!g_resolve_lib) return nullptr;
    FARPROC r = ::GetProcAddress(g_resolve_lib, name);
    void* o = nullptr;
    std::memcpy(&o, &r, sizeof(o));
    return o;
}

/* wglGetProcAddress 优先，opengl32.dll 兜底（1.1 的函数只在前者里拿不到） */
void* glProc(const char* name) {
    if (g_gl.wglGetProcAddress) {
        void* p = reinterpret_cast<void*>(g_gl.wglGetProcAddress(name));
        if (p) return p;
    }
    FARPROC raw = ::GetProcAddress(g_gl.gl, name);
    void* out = nullptr;
    std::memcpy(&out, &raw, sizeof(out));
    return out;
}

bool loadGlLibs() {
    if (g_loadTried) return g_loadOk;
    g_loadTried = true;
    g_gl.gl = ::LoadLibraryA("opengl32.dll");
    g_gl.gdi = ::LoadLibraryA("gdi32.dll");
    if (!g_gl.gl || !g_gl.gdi) {
        if (trace4()) std::fprintf(stderr, "[gl] 载入 opengl32/gdi32 失败\n");
        return false;
    }
    bool ok = true;
#define BGPU_GL_WGL(name)                                                     \
    g_resolve_lib = g_gl.gl;                                                  \
    ok = bindP(&libProc, #name, &g_gl.name) && ok
#define BGPU_GL_GDI(name)                                                     \
    g_resolve_lib = g_gl.gdi;                                                 \
    ok = bindP(&libProc, #name, &g_gl.name) && ok
    BGPU_GL_WGL(wglCreateContext);
    BGPU_GL_WGL(wglDeleteContext);
    BGPU_GL_WGL(wglMakeCurrent);
    BGPU_GL_WGL(wglGetCurrentContext);
    BGPU_GL_WGL(wglGetProcAddress);
    BGPU_GL_GDI(ChoosePixelFormat);
    BGPU_GL_GDI(SetPixelFormat);
    BGPU_GL_GDI(DescribePixelFormat);
    BGPU_GL_GDI(SwapBuffers);
#undef BGPU_GL_WGL
#undef BGPU_GL_GDI
    g_loadOk = ok;
    if (trace4()) std::fprintf(stderr, "[gl] loadGlLibs ok=%d\n", (int)ok);
    return ok;
}

/* 必须在**上下文 current** 之后调用：wglGetProcAddress 返回的指针是随上下文/像素格式
 * 变化的（Windows 的老规矩），所以这一步放在设备建好之后。 */
bool loadGlFunctions() {
    bool ok = true;
#define BGPU_GL_FN(name) ok = bindP(glProc, #name, &g_gl.name) && ok
    BGPU_GL_FN(glGetError);
    BGPU_GL_FN(glGetString);
    BGPU_GL_FN(glGetIntegerv);
    BGPU_GL_FN(glEnable);
    BGPU_GL_FN(glDisable);
    BGPU_GL_FN(glViewport);
    BGPU_GL_FN(glScissor);
    BGPU_GL_FN(glClearColor);
    BGPU_GL_FN(glClear);
    BGPU_GL_FN(glClearDepth);
    BGPU_GL_FN(glDepthFunc);
    BGPU_GL_FN(glDepthMask);
    BGPU_GL_FN(glCullFace);
    BGPU_GL_FN(glFrontFace);
    BGPU_GL_FN(glPixelStorei);
    BGPU_GL_FN(glGenTextures);
    BGPU_GL_FN(glBindTexture);
    BGPU_GL_FN(glDeleteTextures);
    BGPU_GL_FN(glTexImage2D);
    BGPU_GL_FN(glTexSubImage2D);
    BGPU_GL_FN(glTexParameteri);
    BGPU_GL_FN(glDrawElements);
    BGPU_GL_FN(glReadPixels);
    BGPU_GL_FN(glFinish);
    BGPU_GL_FN(glFlush);
    BGPU_GL_FN(glGenFramebuffers);
    BGPU_GL_FN(glBindFramebuffer);
    BGPU_GL_FN(glFramebufferTexture2D);
    BGPU_GL_FN(glFramebufferRenderbuffer);
    BGPU_GL_FN(glCheckFramebufferStatus);
    BGPU_GL_FN(glDeleteFramebuffers);
    BGPU_GL_FN(glGenRenderbuffers);
    BGPU_GL_FN(glBindRenderbuffer);
    BGPU_GL_FN(glRenderbufferStorage);
    BGPU_GL_FN(glDeleteRenderbuffers);
    BGPU_GL_FN(glBlendFuncSeparate);
    BGPU_GL_FN(glActiveTexture);
    BGPU_GL_FN(glGenSamplers);
    BGPU_GL_FN(glBindSampler);
    BGPU_GL_FN(glSamplerParameteri);
    BGPU_GL_FN(glDeleteSamplers);
    BGPU_GL_FN(glGenBuffers);
    BGPU_GL_FN(glBindBuffer);
    BGPU_GL_FN(glBufferData);
    BGPU_GL_FN(glBufferSubData);
    BGPU_GL_FN(glDeleteBuffers);
    BGPU_GL_FN(glGenVertexArrays);
    BGPU_GL_FN(glBindVertexArray);
    BGPU_GL_FN(glDeleteVertexArrays);
    BGPU_GL_FN(glEnableVertexAttribArray);
    BGPU_GL_FN(glVertexAttribPointer);
    BGPU_GL_FN(glCreateShader);
    BGPU_GL_FN(glShaderSource);
    BGPU_GL_FN(glCompileShader);
    BGPU_GL_FN(glGetShaderiv);
    BGPU_GL_FN(glGetShaderInfoLog);
    BGPU_GL_FN(glDeleteShader);
    BGPU_GL_FN(glCreateProgram);
    BGPU_GL_FN(glAttachShader);
    BGPU_GL_FN(glLinkProgram);
    BGPU_GL_FN(glGetProgramiv);
    BGPU_GL_FN(glGetProgramInfoLog);
    BGPU_GL_FN(glUseProgram);
    BGPU_GL_FN(glDeleteProgram);
    BGPU_GL_FN(glGetUniformLocation);
    BGPU_GL_FN(glUniform1i);
    BGPU_GL_FN(glUniform1f);
    BGPU_GL_FN(glUniform2fv);
    BGPU_GL_FN(glUniform4fv);
#undef BGPU_GL_FN
    if (trace4()) std::fprintf(stderr, "[gl] loadGlFunctions ok=%d\n", (int)ok);
    return ok;
}

/* ---------------------------------------------------------------
 * 着色器（GLSL 330 core，与 quad.vert/quad.frag、quad.hlsl 逐行同构）
 * --------------------------------------------------------------- */
const char* kVertSrc =
    "#version 330 core\n"
    "layout(location = 0) in vec3 inPos;\n"
    "layout(location = 1) in vec2 inUv;\n"
    "layout(location = 2) in vec4 inColor;\n"
    "uniform vec4 uVp;     // x, y, w, h（目标像素）\n"
    "uniform vec2 uTarget; // 目标尺寸\n"
    "uniform vec4 uXf0;    // 仿射第 0 行 a, c, e；w = 是否打开\n"
    "uniform vec4 uXf1;    // 仿射第 1 行 b, d, f\n"
    "uniform float uFlip;  // 1 = 默认帧缓冲（图像原来的 y 朝上）\n"
    "out vec2 vUv;\n"
    "out vec4 vColor;\n"
    "void main() {\n"
    "  vec2 s = vec2(uVp.x + (inPos.x / uTarget.x) * uVp.z,\n"
    "                uVp.y + (inPos.y / uTarget.y) * uVp.w);\n"
    "  if (uXf0.w > 0.5) {\n"
    "    s = vec2(uXf0.x * s.x + uXf0.y * s.y + uXf0.z,\n"
    "             uXf1.x * s.x + uXf1.y * s.y + uXf1.z);\n"
    "  }\n"
    "  float yDown = s.y / uTarget.y * 2.0 - 1.0;\n"
    "  float yUp = 1.0 - s.y / uTarget.y * 2.0;\n"
    "  gl_Position = vec4(s.x / uTarget.x * 2.0 - 1.0, mix(yDown, yUp, uFlip), inPos.z, 1.0);\n"
    "  vUv = inUv;\n"
    "  vColor = inColor.zyxw;   // 内存 (B,G,R,A) -> (R,G,B,A)\n"
    "}\n";

const char* kFragSrc =
    "#version 330 core\n"
    "in vec2 vUv;\n"
    "in vec4 vColor;\n"
    "uniform sampler2D uTex;\n"
    "out vec4 outColor;\n"
    "void main() {\n"
    "  vec4 t = texture(uTex, vUv);\n"   // 纹理是预乘的
    "  float a = vColor.a;\n"
    "  outColor = vec4(t.rgb * vColor.rgb * a, t.a * a);\n"
    "}\n";

/* ---------------------------------------------------------------
 * 小工具
 * --------------------------------------------------------------- */
BgpuResult glFail(const char* what, bool check = true) {
    if (check) {
        const GLenum e = g_gl.glGetError();
        if (e != 0) {
            return fail(BGPU_ERR_BACKEND_FAILED, __FILE__, __LINE__,
                        cat("GL ", what, " 失败：glGetError=0x", static_cast<int>(e)));
        }
    }
    return BGPU_OK;
}

inline GLenum toGlInternal(BgpuFormat f) {
    return f == BGPU_FORMAT_R8 ? GL_R8 : GL_RGBA8;
}

inline GLenum toGlExternal(BgpuFormat f) {
    if (f == BGPU_FORMAT_R8) return GL_RED;
    return f == BGPU_FORMAT_BGRA8 ? GL_BGRA : GL_RGBA;
}

/* 直通 → 预乘（上传时做，raster_rules §9）：与 CPU/VK/D3D11 逐字节相同的整数公式 */
void premultiply(uint8_t* px, size_t count) {
    for (size_t i = 0; i < count; i++) {
        uint8_t* p = px + i * 4;
        const uint32_t a = p[3];
        p[0] = static_cast<uint8_t>((p[0] * a + 127) / 255);
        p[1] = static_cast<uint8_t>((p[1] * a + 127) / 255);
        p[2] = static_cast<uint8_t>((p[2] * a + 127) / 255);
    }
}

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

/* GL 的 glReadPixels 自下而上吐像素；窗口目标要翻回来，FBO 目标不用（见文件头 ②） */
void flipRows(uint8_t* px, uint32_t w, uint32_t h) {
    const size_t row = static_cast<size_t>(w) * 4u;
    std::vector<uint8_t> tmp(row);
    for (uint32_t y = 0; y < h / 2; y++) {
        uint8_t* a = px + static_cast<size_t>(y) * row;
        uint8_t* b = px + static_cast<size_t>(h - 1 - y) * row;
        std::memcpy(tmp.data(), a, row);
        std::memcpy(a, b, row);
        std::memcpy(b, tmp.data(), row);
    }
}

/* ---------------------------------------------------------------
 * 对象
 * --------------------------------------------------------------- */
struct GlDeviceObj;
struct GlTextureObj;

struct GlDeviceObj final : Device {
    HDC dc = nullptr;
    HGLRC ctx = nullptr;
    HWND hwnd = nullptr;
    bool owns_window = false;
    bool windowed = false;
    bool lost = false;
    std::string renderer;
    std::string version;
    uint32_t api_version = 0;

    GLuint vao = 0;
    GLuint prog = 0;
    GLint u_vp = -1, u_target = -1, u_xf0 = -1, u_xf1 = -1, u_flip = -1, u_tex = -1;
    GLuint def_samp = 0;
    BgpuTexture white_handle = BGPU_INVALID;
    GlTextureObj* white = nullptr;

    GlDeviceObj(Backend& b, uint32_t flags, uint32_t w, uint32_t h)
        : Device(b, BGPU_INVALID, flags, w, h) {}
    ~GlDeviceObj() override;
};

struct GlTextureObj final : Texture {
    GLuint tex = 0;
    GLuint fbo = 0;
    GLuint depth_rb = 0;
    /* 底层资源的真实尺寸：深度附件挂在**目标**上，所以这里记录的是目标尺寸 */
    uint32_t res_w = 0, res_h = 0;
    /* 交换链借出的**窗口后台缓冲**没有 GL 纹理对象（fbo 恒 0 = 默认帧缓冲） */
    bool window_target = false;
    bool owns_tex = true;
    GlDeviceObj* dev = nullptr;

    GlTextureObj(Backend& b, BgpuDevice owner) : Texture(b, owner) {}
    ~GlTextureObj() override;
};

struct GlBufferObj final : Buffer {
    GLuint buf = 0;
    bool index = false;
    GlDeviceObj* dev = nullptr;

    GlBufferObj(Backend& b, BgpuDevice owner) : Buffer(b, owner) {}
    ~GlBufferObj() override;
};

struct GlSamplerObj final : Sampler {
    GLuint samp = 0;
    GlDeviceObj* dev = nullptr;

    GlSamplerObj(Backend& b, BgpuDevice owner) : Sampler(b, owner) {}
    ~GlSamplerObj() override;
};

struct GlPipelineObj final : Pipeline {
    GlDeviceObj* dev = nullptr;
    GlPipelineObj(Backend& b, BgpuDevice owner) : Pipeline(b, owner) {}
    ~GlPipelineObj() override = default;
};

struct GlSwapchainObj final : Swapchain {
    HWND hwnd = nullptr;
    GlDeviceObj* gldev = nullptr;
    /* 借给 frame_begin 的对象（基类的 `target` 存的是它的**句柄**） */
    GlTextureObj* borrow = nullptr;
    BgpuTexture offscreen = BGPU_INVALID;
    std::vector<uint8_t> readback;   /* 窗口目标：present 时先读一份（换缓冲后内容不保证） */
    bool is_window = false;

    GlSwapchainObj(Backend& b, BgpuDevice owner) : Swapchain(b, owner) {}
    ~GlSwapchainObj() override = default;
};

struct GlCmdObj final : Cmd {
    GlDeviceObj* gldev = nullptr;
    /* draw_quads 的顶点流与索引（每批重传；按需增长） */
    GLuint vbuf = 0;
    uint32_t vcap = 0;
    GLuint ibuf = 0;
    uint32_t icap_quads = 0;

    /* 状态剔除（docs/layering.md §6.2）：只有真的变了才发 GL 调用 */
    int32_t last_blend = -1;
    int32_t last_depth = -1;      /* -1 未知；0 关；1 测试；3 测试+写 */
    int32_t last_cull = -1;
    GLuint last_samp = 0;
    GLuint last_tex = 0;
    bool have_scissor = false;
    int32_t last_sc[4] = {0, 0, 0, 0};
    bool uniforms_dirty = true;

    GlCmdObj(Backend& b, BgpuDevice owner, Device& d) : Cmd(b, owner, d) {}
    ~GlCmdObj() override;
};

GlTextureObj::~GlTextureObj() {
    if (!dev) return;
    /* 上下文必须 current 才能删 GL 对象；设备析构时自己的上下文一定 current */
    if (depth_rb) g_gl.glDeleteRenderbuffers(1, &depth_rb);
    if (fbo) g_gl.glDeleteFramebuffers(1, &fbo);
    if (owns_tex && tex) g_gl.glDeleteTextures(1, &tex);
}

GlBufferObj::~GlBufferObj() {
    if (dev && buf) g_gl.glDeleteBuffers(1, &buf);
}

GlSamplerObj::~GlSamplerObj() {
    if (dev && samp) g_gl.glDeleteSamplers(1, &samp);
}

GlCmdObj::~GlCmdObj() {
    if (!gldev) return;
    if (vbuf) g_gl.glDeleteBuffers(1, &vbuf);
    if (ibuf) g_gl.glDeleteBuffers(1, &ibuf);
}

GlDeviceObj::~GlDeviceObj() {
    if (trace4()) std::fprintf(stderr, "[gl] ~GlDeviceObj ctx=%p\n", (void*)ctx);
    if (!ctx) return;
    if (g_gl.wglGetCurrentContext() != ctx) g_gl.wglMakeCurrent(dc, ctx);
    if (prog) g_gl.glDeleteProgram(prog);
    if (vao) g_gl.glDeleteVertexArrays(1, &vao);
    if (def_samp) g_gl.glDeleteSamplers(1, &def_samp);
    g_gl.glFinish();
    g_gl.wglMakeCurrent(nullptr, nullptr);
    g_gl.wglDeleteContext(ctx);
    if (dc && hwnd) ::ReleaseDC(hwnd, dc);
    if (owns_window && hwnd) ::DestroyWindow(hwnd);
}

/* ---------------------------------------------------------------
 * 后端
 * --------------------------------------------------------------- */
class GlBackend final : public BackendBase {
public:
    BgpuBackendKind kind() const noexcept override { return BGPU_BACKEND_GL; }
    const char* name() const noexcept override { return "gl"; }
    bool available() const noexcept override;
    const char* unavailableReason() const noexcept override;

    BgpuResult createDevice(const BgpuDeviceDesc& desc, BgpuDevice* out) override;
    void deviceInfo(Device& dev, BgpuDeviceInfo& out) override;
    void deviceCaps(Device& dev, BgpuCaps& out) override;
    void deviceStats(Device& dev, BgpuStats& out) override;
    bool deviceLost(Device& dev) override { return static_cast<GlDeviceObj&>(dev).lost; }
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
    BgpuResult beginRender(Cmd& cmd, Texture& target, Texture* depth,
                           const BgpuRenderBeginDesc& desc) override;
    BgpuResult endRender(Cmd& cmd) override;

protected:
    BgpuResult rasterQuads(Cmd& cmd, const BgpuVertex* verts, uint32_t quad_count) override;
    BgpuResult rasterIndexed(Cmd& cmd, Buffer& vb, uint32_t vertex_offset, Buffer& ib,
                             uint32_t index_count, uint32_t index_offset) override;
    BgpuResult prepareDepth(Texture& depth, Texture& target,
                            const BgpuRenderBeginDesc& desc) override;
    BgpuResult clearColor(Texture&, uint32_t) override { return BGPU_OK; }  /* beginRender 里清 */
    void presentFrame(Swapchain& sc) override;

private:
    bool activate(GlDeviceObj& d);
    bool initDeviceObjects(GlDeviceObj& d);
    bool compileProgram(GlDeviceObj& d);
    bool ensureFbo(GlTextureObj& t, GlDeviceObj& d);
    bool ensureSwapchain(Swapchain& sc, uint32_t w, uint32_t h);
    BgpuResult bindState(GlCmdObj& c, GlDeviceObj& d, GlTextureObj& target, Pipeline& pipe,
                         bool has_depth);
    BgpuResult bindTexture(GlCmdObj& c, GlDeviceObj& d, Pipeline& pipe);
    bool applyScissor(GlCmdObj& c, GlTextureObj& target);
    void updateUniforms(GlCmdObj& c, GlDeviceObj& d, GlTextureObj& target);
    bool ensureQuadBuffers(GlCmdObj& c, GlDeviceObj& d, uint32_t quads);
};

GlDeviceObj& gdevOf(BgpuDevice handle) {
    return static_cast<GlDeviceObj&>(*lookupObject(handle, kObjDevice));
}

/* 创建一个不显示的窗口（只为了拿到 DC/上下文）。返回 nullptr = 失败。 */
HWND makeHiddenWindow() {
    static bool registered = false;
    static const char* kClass = "BinGlHiddenWindow";
    if (!registered) {
        WNDCLASSA wc;
        std::memset(&wc, 0, sizeof(wc));
        wc.style = CS_OWNDC;
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = ::GetModuleHandleA(nullptr);
        wc.lpszClassName = kClass;
        if (!::RegisterClassA(&wc) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return nullptr;
        registered = true;
    }
    return ::CreateWindowExA(0, kClass, "binGPU gl", WS_POPUP, 0, 0, 8, 8, nullptr, nullptr,
                             ::GetModuleHandleA(nullptr), nullptr);
}

bool GlBackend::activate(GlDeviceObj& d) {
    if (!d.ctx || !d.dc) return false;
    /* WGL 的 current context 是**线程级**的；同一线程上别的实例可能抢走它 */
    if (g_gl.wglGetCurrentContext() == d.ctx) return true;
    return g_gl.wglMakeCurrent(d.dc, d.ctx) != FALSE;
}

/* 建上下文：先用 1.1 的 legacy 上下文把 wglCreateContextAttribsARB 拿到，再切 3.3 core */
static bool createGlContext(HDC dc, HGLRC* out_ctx) {
    PIXELFORMATDESCRIPTOR pfd;
    std::memset(&pfd, 0, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cAlphaBits = 8;
    pfd.cDepthBits = 24;
    pfd.iLayerType = PFD_MAIN_PLANE;
    int pf = g_gl.ChoosePixelFormat(dc, &pfd);
    if (pf <= 0) {
        /* 回退：遍历格式表，挑一个 32 位双缓冲 RGBA + 深度的 */
        for (int i = 1; i <= 256; i++) {
            PIXELFORMATDESCRIPTOR d2;
            if (!g_gl.DescribePixelFormat(dc, i, sizeof(d2), &d2)) continue;
            if (!(d2.dwFlags & PFD_DRAW_TO_WINDOW) || !(d2.dwFlags & PFD_SUPPORT_OPENGL)) continue;
            if (!(d2.dwFlags & PFD_DOUBLEBUFFER) || d2.iPixelType != PFD_TYPE_RGBA) continue;
            pf = i;
            if (d2.cColorBits >= 32 && d2.cDepthBits >= 24) break;
        }
    }
    if (pf <= 0 || !g_gl.SetPixelFormat(dc, pf, &pfd)) {
        if (trace4()) std::fprintf(stderr, "[gl] 选/设像素格式失败 pf=%d err=%lu\n", pf,
                                   (unsigned long)::GetLastError());
        return false;
    }
    HGLRC legacy = g_gl.wglCreateContext(dc);
    if (!legacy || !g_gl.wglMakeCurrent(dc, legacy)) {
        if (legacy) g_gl.wglDeleteContext(legacy);
        return false;
    }
    void* raw = nullptr;
    {
        FARPROC p = g_gl.wglGetProcAddress ? g_gl.wglGetProcAddress("wglCreateContextAttribsARB")
                                           : nullptr;
        std::memcpy(&raw, &p, sizeof(raw));
    }
    std::memcpy(&g_gl.wglCreateContextAttribsARB, &raw, sizeof(raw));
    if (!g_gl.wglCreateContextAttribsARB) {
        if (trace4()) std::fprintf(stderr, "[gl] 没有 wglCreateContextAttribsARB（太老的驱动）\n");
        g_gl.wglMakeCurrent(nullptr, nullptr);
        g_gl.wglDeleteContext(legacy);
        return false;
    }
    const int attribs[] = {WGL_CONTEXT_MAJOR_VERSION_ARB, 3,   WGL_CONTEXT_MINOR_VERSION_ARB, 3,
                           WGL_CONTEXT_PROFILE_MASK_ARB,  WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0};
    HGLRC core = g_gl.wglCreateContextAttribsARB(dc, nullptr, attribs);
    g_gl.wglMakeCurrent(nullptr, nullptr);
    g_gl.wglDeleteContext(legacy);
    if (!core) {
        if (trace4()) std::fprintf(stderr, "[gl] 3.3 core 上下文创建失败\n");
        return false;
    }
    *out_ctx = core;
    return true;
}

bool GlBackend::available() const noexcept {
    static const bool ok = [] {
        if (!loadGlLibs()) return false;
        HWND hwnd = makeHiddenWindow();
        if (!hwnd) return false;
        HDC dc = ::GetDC(hwnd);
        HGLRC ctx = nullptr;
        const bool made = dc && createGlContext(dc, &ctx);
        if (ctx) {
            g_gl.wglMakeCurrent(dc, ctx);
            loadGlFunctions();
            g_gl.wglMakeCurrent(nullptr, nullptr);
            g_gl.wglDeleteContext(ctx);
        }
        if (dc) ::ReleaseDC(hwnd, dc);
        ::DestroyWindow(hwnd);
        if (trace4()) std::fprintf(stderr, "[gl] available=%d\n", (int)made);
        return made;
    }();
    return ok;
}

const char* GlBackend::unavailableReason() const noexcept {
    if (!loadGlLibs()) return "opengl32.dll / gdi32.dll 载入失败";
    return "这台机器建不出 OpenGL 3.3 core 上下文（老显卡驱动 / 远程会话没有硬件 GL）";
}

/* ---------------------------------------------------------------
 * 设备
 * --------------------------------------------------------------- */
bool GlBackend::compileProgram(GlDeviceObj& d) {
    GLuint vs = g_gl.glCreateShader(GL_VERTEX_SHADER);
    const GLchar* vp = kVertSrc;
    g_gl.glShaderSource(vs, 1, &vp, nullptr);
    g_gl.glCompileShader(vs);
    GLint ok = GL_FALSE;
    g_gl.glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        g_gl.glGetShaderInfoLog(vs, sizeof(log) - 1, nullptr, log);
        g_gl.glDeleteShader(vs);
        if (trace4()) std::fprintf(stderr, "[gl] vs 编译失败：%s\n", log);
        return false;
    }
    GLuint fs = g_gl.glCreateShader(GL_FRAGMENT_SHADER);
    const GLchar* fp = kFragSrc;
    g_gl.glShaderSource(fs, 1, &fp, nullptr);
    g_gl.glCompileShader(fs);
    g_gl.glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        g_gl.glGetShaderInfoLog(fs, sizeof(log) - 1, nullptr, log);
        g_gl.glDeleteShader(fs);
        g_gl.glDeleteShader(vs);
        if (trace4()) std::fprintf(stderr, "[gl] fs 编译失败：%s\n", log);
        return false;
    }
    d.prog = g_gl.glCreateProgram();
    g_gl.glAttachShader(d.prog, vs);
    g_gl.glAttachShader(d.prog, fs);
    g_gl.glLinkProgram(d.prog);
    g_gl.glGetProgramiv(d.prog, GL_LINK_STATUS, &ok);
    g_gl.glDeleteShader(vs);
    g_gl.glDeleteShader(fs);
    if (!ok) {
        char log[1024] = {0};
        g_gl.glGetProgramInfoLog(d.prog, sizeof(log) - 1, nullptr, log);
        if (trace4()) std::fprintf(stderr, "[gl] program 链接失败：%s\n", log);
        return false;
    }
    d.u_vp = g_gl.glGetUniformLocation(d.prog, "uVp");
    d.u_target = g_gl.glGetUniformLocation(d.prog, "uTarget");
    d.u_xf0 = g_gl.glGetUniformLocation(d.prog, "uXf0");
    d.u_xf1 = g_gl.glGetUniformLocation(d.prog, "uXf1");
    d.u_flip = g_gl.glGetUniformLocation(d.prog, "uFlip");
    d.u_tex = g_gl.glGetUniformLocation(d.prog, "uTex");
    g_gl.glUseProgram(d.prog);
    if (d.u_tex >= 0) g_gl.glUniform1i(d.u_tex, 0);
    return true;
}

bool GlBackend::initDeviceObjects(GlDeviceObj& d) {
    if (!compileProgram(d)) return false;
    g_gl.glGenVertexArrays(1, &d.vao);
    g_gl.glBindVertexArray(d.vao);
    g_gl.glGenSamplers(1, &d.def_samp);
    g_gl.glSamplerParameteri(d.def_samp, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    g_gl.glSamplerParameteri(d.def_samp, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    g_gl.glSamplerParameteri(d.def_samp, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    g_gl.glSamplerParameteri(d.def_samp, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    g_gl.glActiveTexture(GL_TEXTURE0);
    g_gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    g_gl.glPixelStorei(GL_PACK_ALIGNMENT, 1);
    g_gl.glEnable(GL_SCISSOR_TEST);
    return true;
}

BgpuResult GlBackend::createDevice(const BgpuDeviceDesc& desc, BgpuDevice* out) {
    if (!loadGlLibs()) return fail(BGPU_ERR_UNSUPPORTED, __FILE__, __LINE__, unavailableReason());
    const uint64_t handle = makeObject<GlDeviceObj>(*this, desc.flags, desc.width, desc.height);
    if (handle == BGPU_INVALID) return failOom(__FILE__, __LINE__);
    GlDeviceObj* d = static_cast<GlDeviceObj*>(lookupObject(handle, kObjDevice));
    d->self = handle;

    d->hwnd = desc.native_window ? static_cast<HWND>(desc.native_window) : makeHiddenWindow();
    if (!d->hwnd) {
        destroyObject(handle);
        return fail(BGPU_ERR_BACKEND_FAILED, __FILE__, __LINE__, "建 GL 用的窗口失败");
    }
    d->owns_window = desc.native_window == nullptr;
    d->windowed = desc.native_window != nullptr;   /* 无窗口设备只跑离屏（FBO） */
    d->dc = ::GetDC(d->hwnd);
    if (!d->dc || !createGlContext(d->dc, &d->ctx)) {
        destroyObject(handle);
        return fail(BGPU_ERR_UNSUPPORTED, __FILE__, __LINE__, unavailableReason());
    }
    if (!g_gl.wglMakeCurrent(d->dc, d->ctx)) {
        destroyObject(handle);
        return fail(BGPU_ERR_BACKEND_FAILED, __FILE__, __LINE__, "wglMakeCurrent 失败");
    }
    if (!loadGlFunctions()) {
        destroyObject(handle);
        return fail(BGPU_ERR_BACKEND_FAILED, __FILE__, __LINE__, "GL 函数指针装载失败（3.3 core）");
    }
    /* vsync：默认关（由交换链的 BGPU_DEVICE_VSYNC 决定，与另两个后端一致） */
    if (g_gl.wglSwapIntervalEXT) g_gl.wglSwapIntervalEXT(0);

    const GLubyte* rend = g_gl.glGetString(GL_RENDERER);
    const GLubyte* ver = g_gl.glGetString(GL_VERSION);
    d->renderer.assign(reinterpret_cast<const char*>(rend ? rend : (const GLubyte*)"?"));
    d->version.assign(reinterpret_cast<const char*>(ver ? ver : (const GLubyte*)"?"));
    GLint major = 0, minor = 0;
    g_gl.glGetIntegerv(GL_MAJOR_VERSION, &major);
    g_gl.glGetIntegerv(GL_MINOR_VERSION, &minor);
    d->api_version = static_cast<uint32_t>(major * 10000 + minor * 100);

    if (!initDeviceObjects(*d)) {
        destroyObject(handle);
        return fail(BGPU_ERR_BACKEND_FAILED, __FILE__, __LINE__,
                    "GL 固定管线对象创建失败（着色器 / VAO / 采样器）");
    }
    if (glFail("init") != BGPU_OK) {
        destroyObject(handle);
        return fail(BGPU_ERR_BACKEND_FAILED, __FILE__, __LINE__, "GL 初始化阶段有 GL 错误");
    }

    /* 1×1 预乘白：未绑纹理时采样它（raster_rules §6） */
    const uint8_t white_px[4] = {255, 255, 255, 255};
    BgpuTextureDesc td{};
    td.struct_size = sizeof(td);
    td.width = 1;
    td.height = 1;
    td.format = BGPU_FORMAT_RGBA8;
    td.flags = BGPU_TEXTURE_CPU_READ;
    BgpuResult r = createTexture(*d, td, &d->white_handle);
    if (r != BGPU_OK) {
        destroyObject(handle);
        return r;
    }
    d->white = static_cast<GlTextureObj*>(lookupObject(d->white_handle, kObjTexture));
    r = textureUpload(*d->white, nullptr, white_px, 4);
    if (r != BGPU_OK) {
        destroyObject(handle);
        return r;
    }

    *out = handle;
    if (trace4()) {
        std::fprintf(stderr, "[gl] 设备就绪：%s / %s（窗口=%d）\n", d->renderer.c_str(),
                     d->version.c_str(), (int)d->windowed);
    }
    return BGPU_OK;
}

void GlBackend::deviceInfo(Device& dev, BgpuDeviceInfo& out) {
    GlDeviceObj& d = static_cast<GlDeviceObj&>(dev);
    out.backend = BGPU_BACKEND_GL;
    out.surface_format = BGPU_FORMAT_RGBA8;
    out.api_version = d.api_version;
    std::snprintf(out.adapter_name, sizeof(out.adapter_name), "%s", d.renderer.c_str());
    std::snprintf(out.driver_name, sizeof(out.driver_name), "%s", d.version.c_str());
}

void GlBackend::deviceCaps(Device& dev, BgpuCaps& out) {
    GlDeviceObj& d = static_cast<GlDeviceObj&>(dev);
    GLint max_tex = 0;
    if (activate(d)) g_gl.glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_tex);
    out.flags = BGPU_CAP_DEPTH | BGPU_CAP_OFFSCREEN | BGPU_CAP_READBACK | BGPU_CAP_PRESENT_PACES |
                BGPU_CAP_VSYNC | BGPU_CAP_SAMPLERS | BGPU_CAP_AFFINE | BGPU_CAP_MESH;
    out.max_texture_size = std::min<uint32_t>(BGPU_MAX_TEXTURE_SIZE,
                                              max_tex > 0 ? static_cast<uint32_t>(max_tex)
                                                          : BGPU_MAX_TEXTURE_SIZE);
    out.max_frames_in_flight = 1;
    out.msaa_samples = 1;   /* 与 CPU 参考后端一致（0 差验收的前提） */
    out.max_quads_per_draw = BGPU_MAX_QUADS_PER_DRAW;
}

void GlBackend::deviceStats(Device& dev, BgpuStats& out) {
    out.frame_index = dev.frame_index;
    out.frames_presented = dev.frames_presented;
    out.draws = dev.draws;
    out.quads = dev.quads;
    out.triangles = dev.tris;
    out.bytes_uploaded = dev.bytes_uploaded;
    out.bytes_live = dev.bytes_live;
}

/* 等 GPU 画完：glFinish（GL 没有"设备等待"，这是唯一能落地的语义）。
 * 要先把设备自己的 context 切上来 —— 线程当前上下文可能是别的设备的。 */
BgpuResult GlBackend::deviceWaitIdle(Device& dev) {
    GlDeviceObj& d = static_cast<GlDeviceObj&>(dev);
    if (!d.dc || !d.ctx) return BGPU_OK;
    const bool wasCurrent = g_gl.wglGetCurrentContext() == d.ctx;
    if (!wasCurrent && !g_gl.wglMakeCurrent(d.dc, d.ctx)) {
        return fail(BGPU_ERR_BACKEND_FAILED, __FILE__, __LINE__, "wglMakeCurrent 失败（wait_idle）");
    }
    if (g_gl.glFinish) g_gl.glFinish();
    if (!wasCurrent) g_gl.wglMakeCurrent(nullptr, nullptr);
    return BGPU_OK;
}

/* ---------------------------------------------------------------
 * 纹理
 * --------------------------------------------------------------- */
BgpuResult GlBackend::createTexture(Device& dev, const BgpuTextureDesc& desc,
                                    BgpuTexture* out) {
    GlDeviceObj& d = static_cast<GlDeviceObj&>(dev);
    if (desc.width == 0 || desc.height == 0) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__, "纹理尺寸不能为 0");
    }
    if (desc.width > BGPU_MAX_TEXTURE_SIZE || desc.height > BGPU_MAX_TEXTURE_SIZE) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__,
                    cat("纹理超过上限 ", BGPU_MAX_TEXTURE_SIZE));
    }
    if ((desc.flags & BGPU_TEXTURE_RENDER_TARGET) && desc.format == BGPU_FORMAT_R8) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__,
                    "R8 不能当渲染目标（只有 RGBA8/BGRA8 可以；R8 用作采样遮罩）");
    }
    if (!activate(d)) {
        return fail(BGPU_ERR_BACKEND_FAILED, __FILE__, __LINE__, "GL 上下文不可用");
    }
    const uint64_t handle = makeObject<GlTextureObj>(*this, d.self);
    if (handle == BGPU_INVALID) return failOom(__FILE__, __LINE__);
    GlTextureObj* t = static_cast<GlTextureObj*>(lookupObject(handle, kObjTexture));
    t->w = desc.width;
    t->h = desc.height;
    t->fmt = desc.format;
    t->tex_flags = desc.flags;
    t->mip_levels = 1;   /* 与 CPU/D3D11 一致：接受 MIPMAPS 但降级为 1 级 */
    t->dev = &d;
    t->res_w = desc.width;
    t->res_h = desc.height;

    g_gl.glGenTextures(1, &t->tex);
    if (!t->tex) {
        destroyObject(handle);
        return fail(BGPU_ERR_OUT_OF_MEMORY, __FILE__, __LINE__, "glGenTextures 失败");
    }
    g_gl.glBindTexture(GL_TEXTURE_2D, t->tex);
    g_gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    g_gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    /* 内部**一律 RGBA8**：BGRA8 只是"调用方看到的字节序"，上传时由 GL 换一次、
     * 回读时我们换回来。这样采样/混合只需要一种内部格式，也少一类格式分支。 */
    g_gl.glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(toGlInternal(desc.format)),
                      static_cast<GLsizei>(desc.width), static_cast<GLsizei>(desc.height), 0,
                      toGlExternal(desc.format), GL_UNSIGNED_BYTE, nullptr);
    if (glFail("glTexImage2D") != BGPU_OK) {
        destroyObject(handle);
        return fail(BGPU_ERR_BACKEND_FAILED, __FILE__, __LINE__, "glTexImage2D 失败");
    }
    t->bytes = static_cast<uint64_t>(desc.width) * desc.height * bingpu_format_bytes(desc.format);
    dev.bytes_live += t->bytes;
    *out = handle;
    return BGPU_OK;
}

void GlBackend::textureInfo(Texture& tex, BgpuTextureInfo& out) {
    out.width = tex.w;
    out.height = tex.h;
    out.format = tex.fmt;
    out.flags = tex.tex_flags;
    out.mip_levels = tex.mip_levels;
}

BgpuResult GlBackend::textureUpload(Texture& tex, const BgpuBox* box, const void* pixels,
                                    uint32_t stride) {
    GlTextureObj& t = static_cast<GlTextureObj&>(tex);
    GlDeviceObj& d = gdevOf(tex.device);
    if (!pixels) return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__, "pixels 为空");
    if (!t.tex) return fail(BGPU_ERR_STATE, __FILE__, __LINE__, "纹理没有 GL 名字");
    if (!activate(d)) return fail(BGPU_ERR_BACKEND_FAILED, __FILE__, __LINE__, "上下文不可用");
    const BgpuBox b = box ? *box : BgpuBox{0, 0, t.w, t.h};
    if (b.w == 0 || b.h == 0 || b.x + b.w > t.w || b.y + b.h > t.h) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__, "上传区域越界");
    }
    const uint32_t bpp = bingpu_format_bytes(t.fmt);
    const uint32_t src_stride = stride ? stride : b.w * bpp;
    if (src_stride < b.w * bpp) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__,
                    cat("stride=", src_stride, " 小于一行 ", b.w * bpp, " 字节"));
    }
    /* 先压成紧凑行（GL 的 UNPACK_ROW_LENGTH 要 3.0+，但紧凑行更省事也更可控），
     * 再预乘 —— 局部上传也要预乘，与 CPU/VK/D3D11 完全一致。 */
    std::vector<uint8_t> tmp(static_cast<size_t>(b.w) * b.h * bpp);
    for (uint32_t y = 0; y < b.h; y++) {
        std::memcpy(tmp.data() + static_cast<size_t>(y) * b.w * bpp,
                    static_cast<const uint8_t*>(pixels) + static_cast<size_t>(y) * src_stride,
                    static_cast<size_t>(b.w) * bpp);
    }
    if (bpp == 4) premultiply(tmp.data(), static_cast<size_t>(b.w) * b.h);

    g_gl.glBindTexture(GL_TEXTURE_2D, t.tex);
    g_gl.glTexSubImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(b.x), static_cast<GLint>(b.y),
                         static_cast<GLsizei>(b.w), static_cast<GLsizei>(b.h),
                         toGlExternal(t.fmt), GL_UNSIGNED_BYTE, tmp.data());
    if (glFail("glTexSubImage2D") != BGPU_OK) {
        return fail(BGPU_ERR_BACKEND_FAILED, __FILE__, __LINE__, "glTexSubImage2D 失败");
    }
    d.bytes_uploaded += static_cast<uint64_t>(b.w) * b.h * bpp;
    return BGPU_OK;
}

BgpuResult GlBackend::textureRead(Texture& tex, void* dst, size_t dst_size,
                                  uint32_t* out_stride) {
    GlTextureObj& t = static_cast<GlTextureObj&>(tex);
    if (!(t.tex_flags & (BGPU_TEXTURE_CPU_READ | BGPU_TEXTURE_RENDER_TARGET))) {
        return fail(BGPU_ERR_UNSUPPORTED, __FILE__, __LINE__,
                    "纹理没有 CPU_READ / RENDER_TARGET，不能回读");
    }
    if (!t.tex) return fail(BGPU_ERR_STATE, __FILE__, __LINE__, "纹理没有 GL 名字");
    GlDeviceObj& d = gdevOf(tex.device);
    if (!activate(d)) return fail(BGPU_ERR_BACKEND_FAILED, __FILE__, __LINE__, "上下文不可用");
    const uint32_t bpp = bingpu_format_bytes(t.fmt);
    const uint32_t row = t.w * bpp;
    if (dst_size < static_cast<size_t>(row) * t.h) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__, "dst 太小");
    }
    /* 挂到 FBO 上再用 glReadPixels：core profile 里这条路一定可用，也能让 GL 做
     * 一次"内部 RGBA8 → 你要的字节序"的转换（BGRA8 在这里换回来）。 */
    if (!ensureFbo(t, d)) {
        return fail(BGPU_ERR_BACKEND_FAILED, __FILE__, __LINE__, "临时 FBO 创建失败");
    }
    g_gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, t.fbo);
    g_gl.glPixelStorei(GL_PACK_ALIGNMENT, 1);
    g_gl.glReadPixels(0, 0, static_cast<GLsizei>(t.w), static_cast<GLsizei>(t.h),
                      toGlExternal(t.fmt), GL_UNSIGNED_BYTE, dst);
    g_gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    if (glFail("glReadPixels") != BGPU_OK) {
        return fail(BGPU_ERR_BACKEND_FAILED, __FILE__, __LINE__, "回读失败");
    }
    uint8_t* p = static_cast<uint8_t*>(dst);
    if (bpp == 4) unpremultiply(p, static_cast<size_t>(t.w) * t.h);
    /* FBO 的行程 & 行序：glReadPixels 自下而上，而 FBO 的存储行 0 = 图像顶行
     * （因为渲染到 FBO 时 uFlip=0），所以这里**不需要**翻行。 */
    if (out_stride) *out_stride = row;
    return BGPU_OK;
}

bool GlBackend::ensureFbo(GlTextureObj& t, GlDeviceObj& d) {
    if (t.fbo) return true;
    if (!t.tex) return false;
    g_gl.glGenFramebuffers(1, &t.fbo);
    if (!t.fbo) return false;
    g_gl.glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
    g_gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.tex, 0);
    const GLenum st = g_gl.glCheckFramebufferStatus(GL_FRAMEBUFFER);
    g_gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
    (void)d;
    return st == GL_FRAMEBUFFER_COMPLETE;
}

/* ---------------------------------------------------------------
 * 缓冲 / 采样器 / 管线
 * --------------------------------------------------------------- */
BgpuResult GlBackend::createBuffer(Device& dev, const BgpuBufferDesc& desc, BgpuBuffer* out) {
    GlDeviceObj& d = static_cast<GlDeviceObj&>(dev);
    if (desc.size == 0) return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__, "size 为 0");
    if (!activate(d)) return fail(BGPU_ERR_BACKEND_FAILED, __FILE__, __LINE__, "上下文不可用");
    const uint64_t handle = makeObject<GlBufferObj>(*this, d.self);
    if (handle == BGPU_INVALID) return failOom(__FILE__, __LINE__);
    GlBufferObj* b = static_cast<GlBufferObj*>(lookupObject(handle, kObjBuffer));
    b->size = desc.size;
    b->usage = desc.usage;
    b->index = (desc.usage & BGPU_BUFFER_INDEX) != 0 && (desc.usage & BGPU_BUFFER_VERTEX) == 0;
    b->dev = &d;
    g_gl.glGenBuffers(1, &b->buf);
    if (!b->buf) {
        destroyObject(handle);
        return fail(BGPU_ERR_OUT_OF_MEMORY, __FILE__, __LINE__, "glGenBuffers 失败");
    }
    const GLenum target = b->index ? GL_ELEMENT_ARRAY_BUFFER : GL_ARRAY_BUFFER;
    g_gl.glBindBuffer(target, b->buf);
    g_gl.glBufferData(target, static_cast<GLsizeiptr>(desc.size), nullptr,
                      (desc.flags & BGPU_TEXTURE_DYNAMIC) ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
    b->bytes = desc.size;
    dev.bytes_live += b->bytes;
    *out = handle;
    return BGPU_OK;
}

BgpuResult GlBackend::bufferUpdate(Buffer& buf, uint32_t offset, const void* data,
                                   uint32_t size) {
    GlBufferObj& b = static_cast<GlBufferObj&>(buf);
    if (!data || size == 0) return BGPU_OK;
    if (static_cast<uint64_t>(offset) + size > b.size) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__,
                    cat("buffer_update 越界：offset=", offset, " size=", size, " cap=", b.size));
    }
    GlDeviceObj& d = *b.dev;
    if (!activate(d)) return fail(BGPU_ERR_BACKEND_FAILED, __FILE__, __LINE__, "上下文不可用");
    const GLenum target = b.index ? GL_ELEMENT_ARRAY_BUFFER : GL_ARRAY_BUFFER;
    g_gl.glBindBuffer(target, b.buf);
    g_gl.glBufferSubData(target, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(size),
                         data);
    return BGPU_OK;
}

BgpuResult GlBackend::createSampler(Device& dev, const BgpuSamplerDesc& desc,
                                    BgpuSampler* out) {
    GlDeviceObj& d = static_cast<GlDeviceObj&>(dev);
    if (!activate(d)) return fail(BGPU_ERR_BACKEND_FAILED, __FILE__, __LINE__, "上下文不可用");
    const uint64_t handle = makeObject<GlSamplerObj>(*this, d.self);
    if (handle == BGPU_INVALID) return failOom(__FILE__, __LINE__);
    GlSamplerObj* s = static_cast<GlSamplerObj*>(lookupObject(handle, kObjSampler));
    s->desc = desc;
    s->dev = &d;
    g_gl.glGenSamplers(1, &s->samp);
    if (!s->samp) {
        destroyObject(handle);
        return fail(BGPU_ERR_OUT_OF_MEMORY, __FILE__, __LINE__, "glGenSamplers 失败");
    }
    g_gl.glSamplerParameteri(s->samp, GL_TEXTURE_MAG_FILTER,
                             desc.mag_filter == BGPU_FILTER_NEAREST ? GL_NEAREST : GL_LINEAR);
    /* MIN 不用 mipmap 版本：我们只有 1 级 mip（固定管线没有 LOD 输入） */
    g_gl.glSamplerParameteri(s->samp, GL_TEXTURE_MIN_FILTER,
                             desc.min_filter == BGPU_FILTER_NEAREST ? GL_NEAREST : GL_LINEAR);
    g_gl.glSamplerParameteri(s->samp, GL_TEXTURE_WRAP_S,
                             desc.wrap_u == BGPU_WRAP_REPEAT ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    g_gl.glSamplerParameteri(s->samp, GL_TEXTURE_WRAP_T,
                             desc.wrap_v == BGPU_WRAP_REPEAT ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    *out = handle;
    return BGPU_OK;
}

BgpuResult GlBackend::createPipeline(Device& dev, const BgpuPipelineDesc& desc,
                                     BgpuPipeline* out) {
    GlDeviceObj& d = static_cast<GlDeviceObj&>(dev);
    const uint64_t handle = makeObject<GlPipelineObj>(*this, d.self);
    if (handle == BGPU_INVALID) return failOom(__FILE__, __LINE__);
    GlPipelineObj* p = static_cast<GlPipelineObj*>(lookupObject(handle, kObjPipeline));
    p->layout = desc.vertex_layout;
    p->blend = desc.blend;
    p->pipe_flags = desc.flags;
    p->dev = &d;
    *out = handle;
    return BGPU_OK;
}

/* ---------------------------------------------------------------
 * 命令
 * --------------------------------------------------------------- */
BgpuResult GlBackend::createCmd(Device& dev, BgpuCmd* out) {
    GlDeviceObj& d = static_cast<GlDeviceObj&>(dev);
    const uint64_t handle = makeObject<GlCmdObj>(*this, d.self, dev);
    if (handle == BGPU_INVALID) {
        setReason("对象表分配失败（内存不足）");
        return BGPU_ERR_OUT_OF_MEMORY;
    }
    static_cast<GlCmdObj*>(lookupObject(handle, kObjCmd))->gldev = &d;
    *out = handle;
    return BGPU_OK;
}

bool GlBackend::ensureQuadBuffers(GlCmdObj& c, GlDeviceObj& d, uint32_t quads) {
    if (!activate(d)) return false;
    const uint64_t vbytes = static_cast<uint64_t>(quads) * 4 * sizeof(BgpuVertex);
    if (!c.vbuf) {
        g_gl.glGenBuffers(1, &c.vbuf);
        c.vcap = 0;
    }
    if (c.vcap < vbytes) {
        uint64_t cap = c.vcap ? c.vcap : 64 * 1024;
        while (cap < vbytes) cap *= 2;
        g_gl.glBindBuffer(GL_ARRAY_BUFFER, c.vbuf);
        g_gl.glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(cap), nullptr, GL_STREAM_DRAW);
        c.vcap = static_cast<uint32_t>(cap);
    }
    if (!c.ibuf || c.icap_quads < quads) {
        uint64_t cap = c.icap_quads ? c.icap_quads : 4096;
        while (cap < quads) cap *= 2;
        std::vector<uint32_t> idx(static_cast<size_t>(cap) * 6);
        for (uint64_t i = 0; i < cap; i++) {
            const uint32_t base = static_cast<uint32_t>(i * 4);
            idx[i * 6 + 0] = base + 0;
            idx[i * 6 + 1] = base + 1;
            idx[i * 6 + 2] = base + 2;
            idx[i * 6 + 3] = base + 2;
            idx[i * 6 + 4] = base + 1;
            idx[i * 6 + 5] = base + 3;
        }
        if (!c.ibuf) g_gl.glGenBuffers(1, &c.ibuf);
        g_gl.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, c.ibuf);
        g_gl.glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                          static_cast<GLsizeiptr>(cap * 6 * sizeof(uint32_t)), idx.data(),
                          GL_STATIC_DRAW);
        c.icap_quads = static_cast<uint32_t>(cap);
    }
    return true;
}

BgpuResult GlBackend::bindState(GlCmdObj& c, GlDeviceObj& d, GlTextureObj& target, Pipeline& pipe,
                                bool has_depth) {
    (void)d;
    (void)target;
    const int32_t blend = static_cast<int32_t>(pipe.blend);
    if (blend > 4) return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__, "未知的混合模式");
    if (blend != c.last_blend) {
        switch (static_cast<BgpuBlend>(blend)) {
            case BGPU_BLEND_REPLACE:
                g_gl.glDisable(GL_BLEND);
                break;
            case BGPU_BLEND_KEEP:
                g_gl.glEnable(GL_BLEND);
                g_gl.glBlendFuncSeparate(GL_ZERO, GL_SRC_ALPHA, GL_ZERO, GL_SRC_ALPHA);
                break;
            case BGPU_BLEND_ERASE:
                g_gl.glEnable(GL_BLEND);
                g_gl.glBlendFuncSeparate(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO,
                                         GL_ONE_MINUS_SRC_ALPHA);
                break;
            case BGPU_BLEND_ADDITIVE:
                g_gl.glEnable(GL_BLEND);
                g_gl.glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ONE);
                break;
            case BGPU_BLEND_PREMULTIPLIED:
            default:
                g_gl.glEnable(GL_BLEND);
                g_gl.glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE,
                                         GL_ONE_MINUS_SRC_ALPHA);
                break;
        }
        c.last_blend = blend;
    }

    /* 深度：没有深度附件时必须关掉测试，否则 GL 会按"没有深度位"报 INVALID_OPERATION */
    const int32_t depth_state =
        has_depth ? (((pipe.pipe_flags & BGPU_PIPE_DEPTH_TEST) ? 1 : 0) |
                     ((pipe.pipe_flags & BGPU_PIPE_DEPTH_WRITE) ? 2 : 0))
                  : 0;
    if (depth_state != c.last_depth) {
        if (depth_state & 1) {
            g_gl.glEnable(GL_DEPTH_TEST);
            g_gl.glDepthFunc(GL_LESS);   /* raster_rules §7：相等不通过 */
        } else {
            g_gl.glDisable(GL_DEPTH_TEST);
        }
        g_gl.glDepthMask((depth_state & 2) ? GL_TRUE : GL_FALSE);
        c.last_depth = depth_state;
    }

    const int32_t cull = (pipe.pipe_flags & BGPU_PIPE_CULL_BACK) ? 1 : 0;
    if (cull != c.last_cull) {
        if (cull) {
            g_gl.glEnable(GL_CULL_FACE);
            g_gl.glCullFace(GL_BACK);
            g_gl.glFrontFace(GL_CCW);   /* GL 的 NDC y 向上，与 D3D 同侧：TL,TR,BL 是 CCW */
        } else {
            g_gl.glDisable(GL_CULL_FACE);
        }
        c.last_cull = cull;
    }
    return BGPU_OK;
}

BgpuResult GlBackend::bindTexture(GlCmdObj& c, GlDeviceObj& d, Pipeline& pipe) {
    GlTextureObj* tex = d.white;
    GLuint samp = d.def_samp;
    if ((pipe.pipe_flags & BGPU_PIPE_TEXTURED) && c.tex) {
        tex = static_cast<GlTextureObj*>(c.tex);
        if (c.samp) samp = static_cast<GlSamplerObj*>(c.samp)->samp;
    }
    if (!tex || !tex->tex) {
        return fail(BGPU_ERR_STATE, __FILE__, __LINE__, "绑定的纹理没有 GL 名字");
    }
    if (tex->tex != c.last_tex) {
        g_gl.glBindTexture(GL_TEXTURE_2D, tex->tex);
        c.last_tex = tex->tex;
    }
    if (samp != c.last_samp) {
        g_gl.glBindSampler(0, samp);
        c.last_samp = samp;
    }
    return BGPU_OK;
}

bool GlBackend::applyScissor(GlCmdObj& c, GlTextureObj& target) {
    int32_t sx = 0, sy = 0;
    int32_t sr = static_cast<int32_t>(target.res_w);
    int32_t sb = static_cast<int32_t>(target.res_h);
    if (c.scissorOn) {
        sx = std::max<int32_t>(0, c.sc[0]);
        sy = std::max<int32_t>(0, c.sc[1]);
        sr = std::min<int32_t>(static_cast<int32_t>(target.res_w), c.sc[0] + c.sc[2]);
        sb = std::min<int32_t>(static_cast<int32_t>(target.res_h), c.sc[1] + c.sc[3]);
        if (sr <= sx || sb <= sy) return false;   /* 空矩形：这一笔什么也不画 */
    }
    /* GL 的窗口原点在**左下**：渲染到 FBO 时（uFlip=0）窗口 y 就等于 s.y；
     * 渲染到默认帧缓冲时（uFlip=1）要镜像成 h - y - hh（见文件头 ③）。 */
    const int32_t gy =
        target.window_target ? (static_cast<int32_t>(target.res_h) - sy - (sb - sy)) : sy;
    const int32_t gw = sr - sx, gh = sb - sy;
    if (!c.have_scissor || c.last_sc[0] != sx || c.last_sc[1] != gy || c.last_sc[2] != gw ||
        c.last_sc[3] != gh) {
        g_gl.glScissor(sx, gy, gw, gh);
        c.last_sc[0] = sx;
        c.last_sc[1] = gy;
        c.last_sc[2] = gw;
        c.last_sc[3] = gh;
        c.have_scissor = true;
    }
    return true;
}

void GlBackend::updateUniforms(GlCmdObj& c, GlDeviceObj& d, GlTextureObj& target) {
    const GLfloat vp[4] = {c.vp[0], c.vp[1], c.vp[2], c.vp[3]};
    const GLfloat tg[2] = {static_cast<GLfloat>(target.res_w),
                           static_cast<GLfloat>(target.res_h)};
    GLfloat xf0[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    GLfloat xf1[4] = {0.0f, 1.0f, 0.0f, 0.0f};
    if (c.affineOn) {
        /* c.xf 就是 [a c e | b d f]，与着色器里两行一致，直接搬 */
        xf0[0] = c.xf[0];
        xf0[1] = c.xf[1];
        xf0[2] = c.xf[2];
        xf0[3] = 1.0f;
        xf1[0] = c.xf[3];
        xf1[1] = c.xf[4];
        xf1[2] = c.xf[5];
    }
    if (d.u_vp >= 0) g_gl.glUniform4fv(d.u_vp, 1, vp);
    if (d.u_target >= 0) g_gl.glUniform2fv(d.u_target, 1, tg);
    if (d.u_xf0 >= 0) g_gl.glUniform4fv(d.u_xf0, 1, xf0);
    if (d.u_xf1 >= 0) g_gl.glUniform4fv(d.u_xf1, 1, xf1);
    if (d.u_flip >= 0) g_gl.glUniform1f(d.u_flip, target.window_target ? 1.0f : 0.0f);
}

/* ---------------------------------------------------------------
 * 交换链 / 帧
 * --------------------------------------------------------------- */
BgpuResult GlBackend::createSwapchain(Device& dev, const BgpuSwapchainDesc& desc,
                                      BgpuSwapchain* out) {
    GlDeviceObj& d = static_cast<GlDeviceObj&>(dev);
    if (desc.flags & BGPU_DEVICE_TRANSPARENT) {
        return fail(BGPU_ERR_UNSUPPORTED, __FILE__, __LINE__,
                    "GL 后端这一版不支持透明窗口（分层窗口 + premultiplied composite 未做）");
    }
    if (desc.width == 0 || desc.height == 0) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__, "尺寸为 0");
    }
    /* GL 的呈现绑在**上下文所在的那个 DC** 上（WGL 的规矩），所以交换链的窗口
     * 要么是设备自己那个窗口，要么为空（只出离屏）。 */
    if (desc.native_window && static_cast<HWND>(desc.native_window) != d.hwnd) {
        return fail(BGPU_ERR_UNSUPPORTED, __FILE__, __LINE__,
                    "GL 的交换链必须绑在设备创建时用的那个窗口上（WGL 的上下文绑在 DC 上）");
    }
    const uint64_t handle = makeObject<GlSwapchainObj>(*this, d.self);
    if (handle == BGPU_INVALID) return failOom(__FILE__, __LINE__);
    GlSwapchainObj* sc = static_cast<GlSwapchainObj*>(lookupObject(handle, kObjSwapchain));
    sc->gldev = &d;
    sc->dev = &d;
    sc->hwnd = desc.native_window ? d.hwnd : nullptr;
    sc->is_window = sc->hwnd != nullptr;
    sc->w = desc.width;
    sc->h = desc.height;
    sc->sc_flags = desc.flags;
    if (sc->is_window && g_gl.wglSwapIntervalEXT) {
        g_gl.wglSwapIntervalEXT((desc.flags & BGPU_DEVICE_VSYNC) ? 1 : 0);
    }
    if (!ensureSwapchain(static_cast<Swapchain&>(*sc), desc.width, desc.height)) {
        destroyObject(handle);
        return fail(BGPU_ERR_BACKEND_FAILED, __FILE__, __LINE__, "交换链创建失败");
    }
    *out = handle;
    return BGPU_OK;
}

bool GlBackend::ensureSwapchain(Swapchain& sc_, uint32_t w, uint32_t h) {
    GlSwapchainObj& sc = static_cast<GlSwapchainObj&>(sc_);
    GlDeviceObj& d = *sc.gldev;
    if (!activate(d)) return false;

    /* 借给 frame_begin 的对象：只有一份，window 模式是个"空壳"（fbo 0 = 默认帧缓冲） */
    if (!sc.borrow) {
        const uint64_t th = makeObject<GlTextureObj>(*this, d.self);
        if (th == BGPU_INVALID) return false;
        sc.borrow = static_cast<GlTextureObj*>(lookupObject(th, kObjTexture));
        sc.borrow->borrowed = true;
        sc.borrow->flags |= kObjFlagBorrowed;
        sc.borrow->dev = &d;
        sc.borrow->tex_flags = BGPU_TEXTURE_RENDER_TARGET;
        sc.target = th;
    }

    if (sc.is_window) {
        /* 窗口模式下尺寸听窗口的（与 VK/D3D11 同一语义） */
        RECT rc;
        if (::GetClientRect(sc.hwnd, &rc) && rc.right > rc.left && rc.bottom > rc.top) {
            w = static_cast<uint32_t>(rc.right - rc.left);
            h = static_cast<uint32_t>(rc.bottom - rc.top);
        }
        sc.w = w;
        sc.h = h;
        sc.fmt = BGPU_FORMAT_RGBA8;   /* 默认帧缓冲读回永远按 RGBA 解释 */
        sc.borrow->w = w;
        sc.borrow->h = h;
        sc.borrow->res_w = w;
        sc.borrow->res_h = h;
        sc.borrow->fmt = sc.fmt;
        sc.borrow->window_target = true;
        sc.borrow->owns_tex = false;
        sc.readback.assign(static_cast<size_t>(w) * h * 4, 0);
        return true;
    }

    /* HEADLESS：一张自有离屏纹理当"交换链图像"，present 退化成 glFinish */
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
    }
    GlTextureObj* ot = static_cast<GlTextureObj*>(lookupObject(sc.offscreen, kObjTexture));
    if (!ot || !ensureFbo(*ot, d)) return false;
    /* 借用对象直接共享离屏纹理（引用计数由 owns_tex 控制，不会二次删除） */
    sc.borrow->tex = ot->tex;
    sc.borrow->fbo = ot->fbo;
    sc.borrow->owns_tex = false;
    sc.borrow->window_target = false;
    sc.borrow->w = w;
    sc.borrow->h = h;
    sc.borrow->res_w = w;
    sc.borrow->res_h = h;
    sc.borrow->fmt = ot->fmt;
    sc.w = w;
    sc.h = h;
    sc.fmt = ot->fmt;
    return true;
}

BgpuResult GlBackend::resizeSwapchain(Swapchain& sc_, uint32_t w, uint32_t h) {
    if (w == 0 || h == 0) return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__, "尺寸为 0");
    GlSwapchainObj& sc = static_cast<GlSwapchainObj&>(sc_);
    sc.w = w;
    sc.h = h;
    if (!ensureSwapchain(sc_, w, h)) {
        return fail(BGPU_ERR_BACKEND_FAILED, __FILE__, __LINE__, "交换链重建失败");
    }
    return BGPU_OK;
}

void GlBackend::swapchainInfo(Swapchain& sc, BgpuSwapchainInfo& out) {
    GlSwapchainObj& s = static_cast<GlSwapchainObj&>(sc);
    out.width = sc.w;
    out.height = sc.h;
    out.format = sc.fmt;
    out.image_count = s.is_window ? 2u : 1u;   /* 双缓冲 / 单离屏，与另两个后端一致 */
    out.msaa_samples = 1;
    out.present_paces_frames = (sc.sc_flags & BGPU_DEVICE_VSYNC) ? 1u : 0u;
}

BgpuResult GlBackend::frameBegin(Swapchain& sc_, Cmd& cmd, BgpuTexture* out) {
    GlSwapchainObj& sc = static_cast<GlSwapchainObj&>(sc_);
    GlDeviceObj& d = *sc.gldev;
    if (cmd.inFrame) {
        return fail(BGPU_ERR_STATE, __FILE__, __LINE__, "cmd 已经在一个帧里（先 frame_end）");
    }
    if (!activate(d)) return fail(BGPU_ERR_BACKEND_FAILED, __FILE__, __LINE__, "上下文不可用");
    if (sc.is_window) {
        /* 窗口尺寸可能变了：每帧按客户区对齐（与 VK/D3D11 同一语义） */
        if (!ensureSwapchain(sc_, sc.w, sc.h)) {
            return fail(BGPU_ERR_BACKEND_FAILED, __FILE__, __LINE__, "交换链刷新失败");
        }
        g_gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
    } else if (sc.borrow && sc.borrow->fbo) {
        g_gl.glBindFramebuffer(GL_FRAMEBUFFER, sc.borrow->fbo);
    }
    RECT rc;
    if (sc.is_window && ::GetClientRect(sc.hwnd, &rc)) {
        g_gl.glViewport(0, 0, rc.right - rc.left, rc.bottom - rc.top);
    } else {
        g_gl.glViewport(0, 0, static_cast<GLsizei>(sc.w), static_cast<GLsizei>(sc.h));
    }
    return BackendBase::frameBegin(sc_, cmd, out);
}

void GlBackend::presentFrame(Swapchain& sc_) {
    GlSwapchainObj& sc = static_cast<GlSwapchainObj&>(sc_);
    GlDeviceObj& d = *sc.gldev;
    if (!activate(d)) return;
    if (!sc.is_window) {
        g_gl.glFinish();   /* 离屏：没有"呈现"，但要让 GL 把活干完再返回 */
        return;
    }
    /* 先把这一帧读一份（SwapBuffers 之后后台缓冲内容不保证），行序翻回来 */
    if (!sc.readback.empty()) {
        g_gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        g_gl.glPixelStorei(GL_PACK_ALIGNMENT, 1);
        g_gl.glReadPixels(0, 0, static_cast<GLsizei>(sc.w), static_cast<GLsizei>(sc.h), GL_RGBA,
                          GL_UNSIGNED_BYTE, sc.readback.data());
        flipRows(sc.readback.data(), sc.w, sc.h);
    }
    g_gl.SwapBuffers(d.dc);
}

BgpuResult GlBackend::frameRead(Swapchain& sc_, void* dst, size_t dst_size,
                                uint32_t* out_stride) {
    GlSwapchainObj& sc = static_cast<GlSwapchainObj&>(sc_);
    GlDeviceObj& d = *sc.gldev;
    const uint32_t row = sc.w * 4;
    if (dst_size < static_cast<size_t>(row) * sc.h) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__, "dst 太小");
    }
    if (!activate(d)) return fail(BGPU_ERR_BACKEND_FAILED, __FILE__, __LINE__, "上下文不可用");
    uint8_t* out = static_cast<uint8_t*>(dst);
    if (sc.is_window) {
        if (sc.readback.empty()) {
            return fail(BGPU_ERR_STATE, __FILE__, __LINE__, "还没有可回读的帧");
        }
        std::memcpy(out, sc.readback.data(), static_cast<size_t>(row) * sc.h);
    } else {
        if (!sc.borrow || !sc.borrow->fbo) {
            return fail(BGPU_ERR_STATE, __FILE__, __LINE__, "交换链没有可回读的帧");
        }
        /* FBO 这条路**不翻行**：渲染到 FBO 时 uFlip=0（s.y=0 → NDC -1 → 窗口 y=0），
         * 而 glReadPixels 正是从窗口 y=0 开始往上吐 —— 第一条就是图像顶行。
         * （第一版在这里也翻了，结果整幅上下镜像；bin2d 的 GPU 桥用一张 45° 渐变
         * 框把这条抓出来，通道差 38。） */
        g_gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, sc.borrow->fbo);
        g_gl.glPixelStorei(GL_PACK_ALIGNMENT, 1);
        g_gl.glReadPixels(0, 0, static_cast<GLsizei>(sc.w), static_cast<GLsizei>(sc.h), GL_RGBA,
                          GL_UNSIGNED_BYTE, out);
        g_gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    }
    /* 两个真后端/CPU 的约定：frame_read 的输出是**直通 RGBA** */
    unpremultiply(out, static_cast<size_t>(sc.w) * sc.h);
    if (trace4())
        std::fprintf(stderr, "[gl] frame_read %ux%u 前 4 字节=%02X%02X%02X%02X\n", sc.w, sc.h,
                     out[0], out[1], out[2], out[3]);
    if (out_stride) *out_stride = row;
    return BGPU_OK;
}

/* ---------------------------------------------------------------
 * 渲染开始 / 结束
 * --------------------------------------------------------------- */
BgpuResult GlBackend::prepareDepth(Texture& depth, Texture& target,
                                   const BgpuRenderBeginDesc& desc) {
    (void)desc;
    GlTextureObj& dt = static_cast<GlTextureObj&>(depth);
    GlTextureObj& tgt = static_cast<GlTextureObj&>(target);
    GlDeviceObj& d = gdevOf(depth.device);
    (void)dt;   /* 深度附件挂在**目标**的 FBO 上（CPU 也是按目标尺寸开数组） */
    /* 窗口目标用像素格式自带的 24 位深度缓冲，不用 FBO */
    if (tgt.window_target) return BGPU_OK;
    if (!ensureFbo(tgt, d)) {
        return fail(BGPU_ERR_BACKEND_FAILED, __FILE__, __LINE__, "目标的 FBO 创建失败");
    }
    if (tgt.depth_rb && tgt.res_w == tgt.w && tgt.res_h == tgt.h) return BGPU_OK;
    if (tgt.depth_rb) {
        g_gl.glDeleteRenderbuffers(1, &tgt.depth_rb);
        tgt.depth_rb = 0;
    }
    g_gl.glGenRenderbuffers(1, &tgt.depth_rb);
    g_gl.glBindRenderbuffer(GL_RENDERBUFFER, tgt.depth_rb);
    g_gl.glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, static_cast<GLsizei>(tgt.w),
                               static_cast<GLsizei>(tgt.h));
    g_gl.glBindFramebuffer(GL_FRAMEBUFFER, tgt.fbo);
    g_gl.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER,
                                   tgt.depth_rb);
    const GLenum st = g_gl.glCheckFramebufferStatus(GL_FRAMEBUFFER);
    g_gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        return fail(BGPU_ERR_BACKEND_FAILED, __FILE__, __LINE__,
                    cat("深度附件的 FBO 不完整（0x", static_cast<int>(st), "）"));
    }
    return BGPU_OK;
}

BgpuResult GlBackend::beginRender(Cmd& cmd, Texture& target, Texture* depth,
                                  const BgpuRenderBeginDesc& desc) {
    BgpuResult r = BackendBase::beginRender(cmd, target, depth, desc);
    if (r != BGPU_OK) return r;
    GlCmdObj& c = static_cast<GlCmdObj&>(cmd);
    GlTextureObj& t = static_cast<GlTextureObj&>(target);
    GlDeviceObj& d = *c.gldev;
    if (!activate(d)) {
        cmd.inRender = false;
        return fail(BGPU_ERR_BACKEND_FAILED, __FILE__, __LINE__, "上下文不可用");
    }
    if (!t.window_target && !ensureFbo(t, d)) {
        cmd.inRender = false;
        return fail(BGPU_ERR_BACKEND_FAILED, __FILE__, __LINE__, "目标的 FBO 创建失败");
    }
    g_gl.glBindFramebuffer(GL_FRAMEBUFFER, t.window_target ? 0 : t.fbo);
    g_gl.glViewport(0, 0, static_cast<GLsizei>(t.res_w), static_cast<GLsizei>(t.res_h));
    /* GL_SCISSOR_TEST 是全局开关，裁剪框的默认值是**建上下文时那个窗口**的大小
     * （离屏设备上是隐藏窗口的 8×8）。不清成整目标的话，clear 只清左上角一小块，
     * 其余像素保持未初始化 —— 第一版就是这个症状（clear 用例 98% 差异）。 */
    g_gl.glScissor(0, 0, static_cast<GLsizei>(t.res_w), static_cast<GLsizei>(t.res_h));
    c.have_scissor = true;
    c.last_sc[0] = 0;
    c.last_sc[1] = 0;
    c.last_sc[2] = static_cast<int32_t>(t.res_w);
    c.last_sc[3] = static_cast<int32_t>(t.res_h);
    g_gl.glUseProgram(d.prog);
    g_gl.glBindVertexArray(d.vao);

    if (desc.flags & BGPU_RENDER_CLEAR_COLOR) {
        const uint32_t cc = desc.clear_color;
        const float a = static_cast<float>((cc >> 24) & 0xFF) / 255.0f;
        /* clear 是直通 0xAARRGGBB，目标存预乘 → rgb 先乘 alpha（raster_rules §9） */
        g_gl.glClearColor(static_cast<float>((cc >> 16) & 0xFF) / 255.0f * a,
                          static_cast<float>((cc >> 8) & 0xFF) / 255.0f * a,
                          static_cast<float>(cc & 0xFF) / 255.0f * a, a);
    }
    GLbitfield mask = 0;
    if (desc.flags & BGPU_RENDER_CLEAR_COLOR) mask |= GL_COLOR_BUFFER_BIT;
    if (depth && (desc.flags & BGPU_RENDER_CLEAR_DEPTH)) {
        g_gl.glDepthMask(GL_TRUE);   /* 清深度要求深度写打开 */
        g_gl.glClearDepth(static_cast<GLdouble>(desc.clear_depth));
        mask |= GL_DEPTH_BUFFER_BIT;
    }
    if (mask) g_gl.glClear(mask);

    /* 状态剔除的"上一次"全部作废：新 pass 从零开始（正在用的也作废，宁可多设一次） */
    c.last_blend = -1;
    c.last_depth = -1;
    c.last_cull = -1;
    c.last_tex = 0;
    c.last_samp = 0;
    c.have_scissor = false;
    return BGPU_OK;
}

BgpuResult GlBackend::endRender(Cmd& cmd) {
    GlCmdObj& c = static_cast<GlCmdObj&>(cmd);
    BgpuResult r = BackendBase::endRender(cmd);
    if (r != BGPU_OK) return r;
    (void)c;
    return BGPU_OK;
}

/* ---------------------------------------------------------------
 * 绘制
 * --------------------------------------------------------------- */
BgpuResult GlBackend::rasterQuads(Cmd& cmd, const BgpuVertex* verts, uint32_t quad_count) {
    GlCmdObj& c = static_cast<GlCmdObj&>(cmd);
    GlDeviceObj& d = *c.gldev;
    if (!c.target) return fail(BGPU_ERR_STATE, __FILE__, __LINE__, "draw 时不在渲染里");
    GlTextureObj& target = static_cast<GlTextureObj&>(*c.target);
    if (!activate(d)) return fail(BGPU_ERR_BACKEND_FAILED, __FILE__, __LINE__, "上下文不可用");

    BgpuResult r = bindState(c, d, target, *c.pipe, c.depth != nullptr);
    if (r != BGPU_OK) return r;
    r = bindTexture(c, d, *c.pipe);
    if (r != BGPU_OK) return r;
    updateUniforms(c, d, target);
    if (!applyScissor(c, target)) return BGPU_OK;   /* 空裁剪：什么也不画 */

    if (!ensureQuadBuffers(c, d, quad_count)) {
        return fail(BGPU_ERR_OUT_OF_MEMORY, __FILE__, __LINE__, "顶点/索引缓冲扩容失败");
    }
    const GLsizeiptr vbytes =
        static_cast<GLsizeiptr>(quad_count) * 4 * static_cast<GLsizeiptr>(sizeof(BgpuVertex));
    g_gl.glBindBuffer(GL_ARRAY_BUFFER, c.vbuf);
    g_gl.glBufferSubData(GL_ARRAY_BUFFER, 0, vbytes, verts);
    g_gl.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, c.ibuf);

    /* 属性指针是 VAO 状态，每笔都重指（3 次调用，换来"缓冲随时换"的自由） */
    g_gl.glEnableVertexAttribArray(0);
    g_gl.glEnableVertexAttribArray(1);
    g_gl.glEnableVertexAttribArray(2);
    g_gl.glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(BgpuVertex),
                               reinterpret_cast<const void*>(0));
    g_gl.glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(BgpuVertex),
                               reinterpret_cast<const void*>(12));
    g_gl.glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(BgpuVertex),
                               reinterpret_cast<const void*>(20));
    g_gl.glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(quad_count * 6), GL_UNSIGNED_INT,
                        reinterpret_cast<const void*>(0));
    if (glFail("draw_quads") != BGPU_OK) {
        return fail(BGPU_ERR_BACKEND_FAILED, __FILE__, __LINE__, "GL 绘制失败");
    }
    return BGPU_OK;
}

BgpuResult GlBackend::rasterIndexed(Cmd& cmd, Buffer& vb, uint32_t vertex_offset, Buffer& ib,
                                    uint32_t index_count, uint32_t index_offset) {
    GlCmdObj& c = static_cast<GlCmdObj&>(cmd);
    GlDeviceObj& d = *c.gldev;
    if (!c.target) return fail(BGPU_ERR_STATE, __FILE__, __LINE__, "draw 时不在渲染里");
    GlTextureObj& target = static_cast<GlTextureObj&>(*c.target);
    if (!activate(d)) return fail(BGPU_ERR_BACKEND_FAILED, __FILE__, __LINE__, "上下文不可用");

    BgpuResult r = bindState(c, d, target, *c.pipe, c.depth != nullptr);
    if (r != BGPU_OK) return r;
    r = bindTexture(c, d, *c.pipe);
    if (r != BGPU_OK) return r;
    updateUniforms(c, d, target);
    if (!applyScissor(c, target)) return BGPU_OK;

    GlBufferObj& vbo = static_cast<GlBufferObj&>(vb);
    GlBufferObj& ibo = static_cast<GlBufferObj&>(ib);
    if (!vbo.buf || !ibo.buf) {
        return fail(BGPU_ERR_STATE, __FILE__, __LINE__, "顶点/索引缓冲无效");
    }
    g_gl.glBindBuffer(GL_ARRAY_BUFFER, vbo.buf);
    g_gl.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo.buf);
    g_gl.glEnableVertexAttribArray(0);
    g_gl.glEnableVertexAttribArray(1);
    g_gl.glEnableVertexAttribArray(2);
    const GLsizei stride = sizeof(BgpuMeshVertex);
    g_gl.glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride,
                               reinterpret_cast<const void*>(static_cast<size_t>(vertex_offset)));
    g_gl.glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride,
                               reinterpret_cast<const void*>(static_cast<size_t>(vertex_offset) + 24));
    g_gl.glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride,
                               reinterpret_cast<const void*>(static_cast<size_t>(vertex_offset) + 32));
    g_gl.glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(index_count), GL_UNSIGNED_INT,
                        reinterpret_cast<const void*>(static_cast<size_t>(index_offset)));
    if (glFail("draw_indexed") != BGPU_OK) {
        return fail(BGPU_ERR_BACKEND_FAILED, __FILE__, __LINE__, "GL 绘制失败");
    }
    return BGPU_OK;
}

}  // namespace

Backend& glBackend() {
    static GlBackend be;
    return be;
}

}  // namespace bgpu

#endif  // BINGPU_HAS_GL
