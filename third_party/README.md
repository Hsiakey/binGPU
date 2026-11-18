# third_party/

第三方源码，**不入库**（`.gitignore` 里整目录忽略，只留本说明）。

## 现在只有一份东西：Vulkan 头

| 路径 | 内容 | 谁在要 |
|---|---|---|
| `third_party/vulkan/include/vulkan/*.h` | Khronos 官方 Vulkan 头（`vulkan_core.h` 约 1 MB） | `src/bingpu_vk.cpp` |

拉取：

```sh
bash scripts/fetch-deps.sh              # 优先从同级 ../binUI/third_party 复制（离线）
bash scripts/fetch-deps.sh --download   # 或从 Khronos 官方仓库下载
```

## 没有它也能构建

`src/bingpu_vk.cpp` 用 `__has_include` 判断头在不在：

- **在** → 编成真后端（运行期 `LoadLibrary("vulkan-1.dll")`，**不链接 vulkan-1.lib**，
  不需要 Vulkan SDK）；
- **不在** → 编成"登记但不可用"的桩（`bingpu_backend_available(BGPU_BACKEND_VULKAN) == 0`），
  其余后端、测试、工具全部照常。

所以 CI 可以在没有第三方源码的机器上跑：只会少一个后端，不会少一条断言
（`tests/test_vk_render.c` 在无 Vulkan 时整份跳过）。
