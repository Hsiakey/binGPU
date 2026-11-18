#!/usr/bin/env bash
# scripts/fetch-deps.sh —— 拉第三方源（目前只有 Vulkan 头）
#
# 为什么要脚本而不是入库：Vulkan 头一个 `vulkan_core.h` 就 ~1 MB，且是上游版本化产物；
# 入库会随上游漂移、还得在每次升级时 diff 一遍。这里和 binUI 用的是同一套规矩：
# **third_party/ 不入库，构建前拉一次。**
#
# 用法：
#   bash scripts/fetch-deps.sh                # 优先从同级 binUI 复制（离线可用）
#   bash scripts/fetch-deps.sh --download     # 强制从 Khronos 官方仓库下载
#
# 拉完之后 `make` 会把 `src/bingpu_vk.cpp` 编成真后端；**没有头也能编** ——
# 它会退化成"登记但不可用"的桩（`bingpu_backend_available(VULKAN) == 0`），
# 其余后端与测试完全不受影响。
set -euo pipefail

cd "$(dirname "$0")/.."
DEST="third_party/vulkan/include"
SRC_LOCAL="../binUI/third_party/vulkan/include"

if [ "${1:-}" != "--download" ] && [ -d "$SRC_LOCAL/vulkan" ]; then
    echo "从同级 binUI 复制 Vulkan 头：$SRC_LOCAL -> $DEST"
    mkdir -p "$DEST"
    cp -r "$SRC_LOCAL/vulkan" "$DEST/"
    echo "完成。"
    exit 0
fi

echo "从 Khronos 官方仓库下载 Vulkan 头（需要网络）……"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
curl -fsSL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/heads/main.tar.gz \
    | tar -xz -C "$TMP"
mkdir -p "$DEST"
cp -r "$TMP"/Vulkan-Headers-main/include/vulkan "$DEST/"
echo "完成。"
