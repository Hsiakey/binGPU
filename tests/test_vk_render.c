/* tests/test_vk_render.c —— Vulkan 后端 vs CPU 参考后端的**逐像素**对照
 *
 * 用例、判据、容差全在 tests/gpu_parity.h 里（与 D3D11 共用同一份规格）：
 * 这个文件只负责"被测后端是哪一个"。新增一个 GPU 后端 = 新加一个 20 行的
 * 同构文件，用例表一个字都不用动 —— 那才是"一个定义 + 四份实现 + 一套门禁"。
 */
#include "gpu_parity.h"

int main(void) { return parity_run("Vulkan", BGPU_BACKEND_VULKAN); }
