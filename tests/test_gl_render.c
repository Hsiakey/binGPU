/* tests/test_gl_render.c —— OpenGL 后端 vs CPU 参考后端的**逐像素**对照
 *
 * 用例、判据、容差全在 tests/gpu_parity.h 里（VK / D3D11 / GL 共用同一份规格）。
 * GL 是异族里最极端的那一个（全局绑定点状态、没有命令缓冲、窗口原点在左下），
 * 这份用例表同样一个字没改 —— uFlip/scissor 镜像这些 GL 特有的换算全在后端内部。
 */
#include "gpu_parity.h"

int main(void) { return parity_run("OpenGL", BGPU_BACKEND_GL); }
