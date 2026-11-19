#!/usr/bin/env python3
"""tools/gen_dxbc_header.py —— 把 fxc 编出来的 .cso 转成 C++ 头（DXBC 以数组入库）

为什么要这一步：构建**不应该**依赖 DirectX 着色器编译器（Windows SDK 的 fxc.exe）。
所以 .cso 与生成后的头文件一起入库，`make` 只编 C++；没装 SDK 的机器也能构建。

重新生成（改了 .hlsl 之后）：
    FXC="C:/Program Files (x86)/Windows Kits/10/bin/<ver>/x64/fxc.exe"
    "$FXC" /nologo /T vs_4_0 /E vs      /O3 /Fo src/d3d11_shaders/quad.vs.cso      src/d3d11_shaders/quad.hlsl
    "$FXC" /nologo /T vs_4_0 /E vs_mesh /O3 /Fo src/d3d11_shaders/quad.vs_mesh.cso src/d3d11_shaders/quad.hlsl
    "$FXC" /nologo /T ps_4_0 /E ps      /O3 /Fo src/d3d11_shaders/quad.ps.cso      src/d3d11_shaders/quad.hlsl
    python3 tools/gen_dxbc_header.py

（ABI 3 起有两个 VS 入口：2D 顶点没有法线/w，不能与网格共用同一份输入签名。）

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
    vs_mesh = bytes_of(ROOT / "src/d3d11_shaders/quad.vs_mesh.cso")
    fs = bytes_of(ROOT / "src/d3d11_shaders/quad.ps.cso")
    # ABI 4：解析圆角（vs_shape / ps_shape）
    vs_shape = bytes_of(ROOT / "src/d3d11_shaders/quad.vs_shape.cso")
    ps_shape = bytes_of(ROOT / "src/d3d11_shaders/quad.ps_shape.cso")
    hdr = f'''// bingpu_d3d11_shaders.h - D3D11 后端的 DXBC（由 src/d3d11_shaders/quad.hlsl 生成，勿手改）
//
// 生成方式（构建**不需要**这一步，产物已入库，与 Vulkan 那份 SPIR-V 同一套规矩）：
//   fxc /nologo /T vs_4_0 /E vs       /O3 /Fo src/d3d11_shaders/quad.vs.cso       src/d3d11_shaders/quad.hlsl
//   fxc /nologo /T vs_4_0 /E vs_mesh  /O3 /Fo src/d3d11_shaders/quad.vs_mesh.cso  src/d3d11_shaders/quad.hlsl
//   fxc /nologo /T vs_4_0 /E vs_shape /O3 /Fo src/d3d11_shaders/quad.vs_shape.cso src/d3d11_shaders/quad.hlsl
//   fxc /nologo /T ps_4_0 /E ps       /O3 /Fo src/d3d11_shaders/quad.ps.cso       src/d3d11_shaders/quad.hlsl
//   fxc /nologo /T ps_4_0 /E ps_shape /O3 /Fo src/d3d11_shaders/quad.ps_shape.cso src/d3d11_shaders/quad.hlsl
//   python3 tools/gen_dxbc_header.py
//
// 为什么有两个 VS：D3D11 的输入布局必须覆盖 VS 声明的全部输入语义。2D 顶点没有
// 法线与 w，与网格共用一份签名就只能把它们指向别的字段（读垃圾值）；拆开之后
// 2D 的顶点流与旧版本逐字节相同。ABI 4 的解析圆角是第三种顶点（36 字节），
// 于是有第三对入口。
//
// DXBC 以 C 数组入库，于是 `make` 只依赖 C++ 编译器（不需要 Windows SDK / fxc）。
// 每个入口一份字节码：ID3D11Device::CreateVertexShader / CreatePixelShader 只吃
// (ptr, size)，不需要对齐，所以这里用 uint8_t[] 而不是 uint32_t[]。
#ifndef BINGPU_D3D11_SHADERS_H
#define BINGPU_D3D11_SHADERS_H

#include <cstdint>

namespace bgpu {{
namespace d3d11shader {{

{emit("kQuadVsDxbc", vs)}
static const uint32_t kQuadVsSize = {len(vs)}u;

{emit("kQuadVsMeshDxbc", vs_mesh)}
static const uint32_t kQuadVsMeshSize = {len(vs_mesh)}u;

{emit("kQuadPsDxbc", fs)}
static const uint32_t kQuadPsSize = {len(fs)}u;

{emit("kShapeVsDxbc", vs_shape)}
static const uint32_t kShapeVsSize = {len(vs_shape)}u;

{emit("kShapePsDxbc", ps_shape)}
static const uint32_t kShapePsSize = {len(ps_shape)}u;

}}  // namespace d3d11shader
}}  // namespace bgpu

#endif  // BINGPU_D3D11_SHADERS_H
'''
    out = ROOT / "src/bingpu_d3d11_shaders.h"
    out.write_text(hdr, encoding="utf-8", newline="\n")   # 仓库一律 LF
    print(f"wrote {out} (vs {len(vs)}, vs_mesh {len(vs_mesh)}, ps {len(fs)}, "
          f"vs_shape {len(vs_shape)}, ps_shape {len(ps_shape)} bytes)")


if __name__ == "__main__":
    main()
