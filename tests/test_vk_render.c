/* tests/test_vk_render.c —— Vulkan 后端 vs CPU 参考后端的**逐像素**对照
 *
 * 用例、判据、容差全在 tests/gpu_parity.h 里（与 D3D11 / GL 共用同一份规格）：
 * 这个文件只负责"被测后端是哪一个"。新增一个 GPU 后端 = 新加一个 20 行的
 * 同构文件，用例表一个字都不用动 —— 那才是"一个定义 + 四份实现 + 一套门禁"。
 *
 * ABI 3 的光照与透视校正用例也在 gpu_parity.h 里（0.6.1 起）：三个后端的这两项
 * 一起落地之后就没有"谁先做完谁的用例先绿"的问题了，一份场景 + 0 容差更硬。
 * 判据与 tests/test_cpu_render.c 的 [lighting]（255 / 26 / 206）和
 * [perspective]（u=0.5625 vs 0.2432）同源，金标就是 CPU 后端自己。
 */
#include "gpu_parity.h"

int main(void) {
    return parity_run("Vulkan", BGPU_BACKEND_VULKAN);
}
