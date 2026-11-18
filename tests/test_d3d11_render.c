/* tests/test_d3d11_render.c —— D3D11 后端 vs CPU 参考后端的**逐像素**对照
 *
 * 用例、判据、容差全在 tests/gpu_parity.h 里（与 Vulkan 共用同一份规格）——
 * D3D11 是"异族模型"（即时上下文、没有命令缓冲），这条用例表一个字都没改就
 * 跑在它上面，正好说明 `docs/layering.md` §3 的边界判据不依赖图形 API 的世代。
 */
#include "gpu_parity.h"

int main(void) { return parity_run("D3D11", BGPU_BACKEND_D3D11); }
