#!/usr/bin/env python3
"""tools/gen_dxbc_header.py —— 把 fxc 编出来的 .cso 转成 C++ 头（DXBC 以数组入库）

为什么要这一步：构建**不应该**依赖 DirectX 着色器编译器（Windows SDK 的 fxc.exe）。
所以 .cso 与生成后的头文件一起入库，`make` 只编 C++；没装 SDK 的机器也能构建。

重新生成（改了 .hlsl 之后）：
    FXC="C:/Program Files (x86)/Windows Kits/10/bin/<ver>/x64/fxc.exe"
    "$FXC" /nologo /T vs_4_0 /E vs /O3 /Fo src/d3d11_shaders/quad.vs.cso src/d3d11_shaders/quad.hlsl
    "$FXC" /nologo /T ps_4_0 /E ps /O3 /Fo src/d3d11_shaders/quad.ps.cso src/d3d11_shaders/quad.hlsl
    python3 tools/gen_dxbc_header.py

与 tools/gen_spv_header.py 同一套规矩（那边是 SPIR-V + glslc，这边是 DXBC + fxc）：
工具只在"改着色器"时需要，构建期只要编译器。
"""
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent


def bytes_of(path):
    data = pathlib.Path(path).read_bytes()
    if len(data) < 4 or data[:4] != b"DXBC":
        raise SystemExit("不是 DXBC 容器（fxc 的输出应以 DXBC 开头）：" + str(path))
    return data


def emit(name, data):
    out = [f"static const uint8_t {name}[] = {{"]
    line = "    "
    for i, b in enumerate(data):
        tok = f"0x{b:02x}, "
        if len(line) + len(tok) > 96:
            out.append(line.rstrip())
            line = "    "
        line += tok
    if line.strip():
        out.append(line.rstrip())
    out.append("};")
    return "\n".join(out)


def main():
    vs = bytes_of(ROOT / "src/d3d11_shaders/quad.vs.cso")
    fs = bytes_of(ROOT / "src/d3d11_shaders/quad.ps.cso")
    hdr = f'''// bingpu_d3d11_shaders.h - D3D11 后端的 DXBC（由 src/d3d11_shaders/quad.hlsl 生成，勿手改）
//
// 生成方式（构建**不需要**这一步，产物已入库，与 Vulkan 那份 SPIR-V 同一套规矩）：
//   fxc /nologo /T vs_4_0 /E vs /O3 /Fo src/d3d11_shaders/quad.vs.cso src/d3d11_shaders/quad.hlsl
//   fxc /nologo /T ps_4_0 /E ps /O3 /Fo src/d3d11_shaders/quad.ps.cso src/d3d11_shaders/quad.hlsl
//   python3 tools/gen_dxbc_header.py
//
// DXBC 以 C 数组入库，于是 `make` 只依赖 C++ 编译器（不需要 Windows SDK / fxc）。
// 两个入口各一份字节码：ID3D11Device::CreateVertexShader / CreatePixelShader 只吃
// (ptr, size)，不需要对齐，所以这里用 uint8_t[] 而不是 uint32_t[]。
#ifndef BINGPU_D3D11_SHADERS_H
#define BINGPU_D3D11_SHADERS_H

#include <cstdint>

namespace bgpu {{
namespace d3d11shader {{

{emit("kQuadVsDxbc", vs)}
static const uint32_t kQuadVsSize = {len(vs)}u;

{emit("kQuadPsDxbc", fs)}
static const uint32_t kQuadPsSize = {len(fs)}u;

}}  // namespace d3d11shader
}}  // namespace bgpu

#endif  // BINGPU_D3D11_SHADERS_H
'''
    out = ROOT / "src/bingpu_d3d11_shaders.h"
    out.write_text(hdr, encoding="utf-8", newline="\n")   # 仓库一律 LF
    print(f"wrote {out} (vs {len(vs)} bytes, ps {len(fs)} bytes)")


if __name__ == "__main__":
    main()
