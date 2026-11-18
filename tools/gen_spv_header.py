#!/usr/bin/env python3
"""tools/gen_spv_header.py —— 把 src/vk_shaders/*.spv 转成 C++ 头（SPIR-V 以数组入库）

为什么要这一步：构建**不应该**依赖着色器工具链（本机没有 Vulkan SDK，只有 Android NDK
自带的 glslc）。所以 .spv 与生成后的头文件一起入库，`make` 只编 C++。

重新生成（改了 .vert/.frag 之后）：
    GLSLC=<NDK>/shader-tools/windows-x86_64/glslc.exe
    $GLSLC --target-env=vulkan1.0 -O src/vk_shaders/quad.vert -o src/vk_shaders/quad.vert.spv
    $GLSLC --target-env=vulkan1.0 -O src/vk_shaders/quad.frag -o src/vk_shaders/quad.frag.spv
    python3 tools/gen_spv_header.py

与 binUI 的 tools/gen_vk_shaders.py 同源；区别只在输出名字与命名空间。
"""
import pathlib
import struct

ROOT = pathlib.Path(__file__).resolve().parent.parent


def words_of(path):
    data = pathlib.Path(path).read_bytes()
    if len(data) % 4 != 0:
        raise SystemExit("SPIR-V 长度不是 4 的倍数：" + str(path))
    return struct.unpack("<%dI" % (len(data) // 4), data)


def emit(name, words):
    out = [f"static const uint32_t {name}[] = {{"]
    line = "    "
    for w in words:
        tok = f"0x{w:08x}u, "
        if len(line) + len(tok) > 96:
            out.append(line.rstrip())
            line = "    "
        line += tok
    if line.strip():
        out.append(line.rstrip())
    out.append("};")
    return "\n".join(out)


def main():
    vs = words_of(ROOT / "src/vk_shaders/quad.vert.spv")
    fs = words_of(ROOT / "src/vk_shaders/quad.frag.spv")
    hdr = f'''// bingpu_vk_shaders.h - Vulkan 后端的 SPIR-V（由 src/vk_shaders/*.vert/.frag 生成，勿手改）
//
// 生成方式（构建**不需要**这一步，产物已入库，与 binUI 同一套规矩）：
//   glslc --target-env=vulkan1.0 -O src/vk_shaders/quad.vert -o src/vk_shaders/quad.vert.spv
//   glslc --target-env=vulkan1.0 -O src/vk_shaders/quad.frag -o src/vk_shaders/quad.frag.spv
//   python3 tools/gen_spv_header.py
//
// 本机没有 Vulkan SDK，但 Android NDK 自带 glslc；SPIR-V 以 C 数组入库，
// 于是 `make` 只依赖 C++ 编译器。
#ifndef BINGPU_VK_SHADERS_H
#define BINGPU_VK_SHADERS_H

#include <cstdint>

namespace bgpu {{
namespace vkshader {{

{emit("kQuadVertSpv", vs)}

{emit("kQuadFragSpv", fs)}

}}  // namespace vkshader
}}  // namespace bgpu

#endif  // BINGPU_VK_SHADERS_H
'''
    out = ROOT / "src/bingpu_vk_shaders.h"
    out.write_text(hdr, encoding="utf-8", newline="\n")   # 仓库一律 LF
    print(f"wrote {out} (vert {len(vs)} words, frag {len(fs)} words)")


if __name__ == "__main__":
    main()
