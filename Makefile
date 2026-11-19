# Makefile - binGPU (MinGW / GCC / Clang)
#
# 语言（2026-11-15 定）：**实现是 C++17**（src/*.cpp），**对外 ABI 是 C 兼容的**
# （include/bingpu.h 必须能被纯 C 编译器单独编过 —— 见 abi-check）。两件事分开看：
#   ABI 看"谁要 include 它"（Python/Go/Rust 绑的是扁平 C 面），
#   实现看表达力（容器 / RAII / 异常在 C 里全是手写样板，见 docs/abi.md）。
#
# 测试与工具是 **C 源码**（tests/*.c、tools/*.c），故意如此：它们同时是
# "C 能消费这个 ABI"的证明。所以构建是两段式：
#   ① 用 gcc（纯 C）把 .c 编成目标文件；
#   ② 用 g++ 链接（因为静态库 libbingpu.a 里是 C++ 符号，需要 libstdc++）。
# 链接行的 RTLINK 静态链上 libgcc/libstdc++：产物自包含，换环境不会因为
# Git for Windows 的 /mingw64/bin 抢先命中不匹配的 libstdc++-6.dll 而炸（退出码 127）。
#
# 目标：
#   all        静态库 + 测试 + 工具
#   test       编好后逐个跑，任何失败非 0 退出（CI 就认这个）
#   bench      固定场景的 CPU 后端吞吐基准（同时打印像素校验和，见 docs/perf.md）
#   smoke      窗口路径烟测（VK / D3D11 / GL 各一遍；要开真窗口，不进 CI）
#   abi-check  ①纯 C 编译器单独编 include/bingpu.h（混进 C++ 语法当场炸）
#              ②grep 守规矩：头里不许 C++ 类型/命名空间/容器头
#              ③冒烟：C 目标文件 + C++ 静态库能被 g++ 链接成可执行并跑起来
#   bench      固定场景的 CPU 后端吞吐基准（同时打印像素校验和，见 docs/perf.md）
#   clean
#
# 第三方：Vulkan 头在 third_party/vulkan/include（不入库，scripts/fetch-deps.sh 拉；
# 没有头时 bingpu_vk.cpp 自动退化成"不可用"的桩，其余目标不受影响）。

# make 内建的 CC 默认是 `cc`；MinGW/GCC 发行版都带 cc，但为了不依赖那个软链，
# 当 CC/CXX 还是内建默认值时改用 gcc/g++（用户在命令行或环境里显式指定就尊重它）。
ifeq ($(origin CC),default)
CC := gcc
endif
ifeq ($(origin CXX),default)
CXX := g++
endif

CFLAGS   ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -MMD -MP
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -O2 -MMD -MP
LDFLAGS  ?=
RTLINK   ?= -static-libgcc -static-libstdc++

SRC_DIR  := src
INC_DIR  := include
TEST_SRC := tests
BUILD_DIR := build-mingw
BIN_DIR  := $(BUILD_DIR)/bin
TEST_DIR := $(BIN_DIR)/tests
OBJ_DIR  := $(BUILD_DIR)/obj

ifdef ComSpec
    EXT := .exe
else
    EXT :=
endif

# =============================================
# 静态库（C++17 实现）
# =============================================
LIB_SRCS := $(wildcard $(SRC_DIR)/*.cpp)
LIB_OBJS := $(patsubst $(SRC_DIR)/%.cpp,$(OBJ_DIR)/%.o,$(LIB_SRCS))
LIB_A    := $(BUILD_DIR)/libbingpu.a

# 只给 CPU 后端的热循环开 -O3：其余文件都是冷路径（句柄表 / 校验 / 注册表），
# -O3 在那里的收益是 0，只换来更长的编译时间。0.2.1 量过：-O3 让纹理路径再快 ~20%。
$(OBJ_DIR)/bingpu_cpu.o: CXXFLAGS += -O3
# Vulkan 后端：头文件在 third_party/vulkan/include（不入库，scripts/fetch-deps.sh 拉）。
# 只给这一个文件加 -I —— 其余源文件不该被第三方头污染。头不在时后端自动退化成桩。
$(OBJ_DIR)/bingpu_vk.o: CXXFLAGS += -Ithird_party/vulkan/include
# D3D11 后端：d3d11.h / dxgi.h 由 MinGW-w64 自带（在默认搜索路径里），不需要额外 -I；
# 运行期只 LoadLibrary(d3d11.dll)，不链接 d3d11.lib/dxgi.lib —— 所以链接行也不用加库。
# GL 后端：**不 include 任何 GL 头**（MinGW 的 GL/gl.h 只有 1.1 的枚举），类型与常量在
# bingpu_gl.cpp 里自带；运行期只 LoadLibrary(opengl32.dll/gdi32.dll)，不链接 opengl32.lib。

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -I$(INC_DIR) -I$(SRC_DIR) -c $< -o $@

$(LIB_A): $(LIB_OBJS)
	@mkdir -p $(dir $@)
	ar rcs $@ $^

# =============================================
# 测试（C 源码 → gcc 编 → g++ 链接；见文件头）
# =============================================
TEST_NAMES := test_abi test_cpu_render test_pace test_prims test_shape test_vk_render test_d3d11_render test_gl_render
TEST_BINS  := $(addprefix $(TEST_DIR)/,$(addsuffix $(EXT),$(TEST_NAMES)))

$(OBJ_DIR)/tests/%.o: $(TEST_SRC)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(INC_DIR) -c $< -o $@

$(TEST_DIR)/%$(EXT): $(OBJ_DIR)/tests/%.o $(LIB_A)
	@mkdir -p $(dir $@)
	$(CXX) $(RTLINK) -o $@ $< $(LIB_A) $(LDFLAGS)

# =============================================
# 工具（同样是 C 源码）
# =============================================
TOOL_BINS := $(BIN_DIR)/bingpu_info$(EXT) $(BIN_DIR)/bingpu_bench$(EXT) \
             $(BIN_DIR)/gpu_window_smoke$(EXT)

$(OBJ_DIR)/tools/%.o: tools/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(INC_DIR) -c $< -o $@

$(BIN_DIR)/%$(EXT): $(OBJ_DIR)/tools/%.o $(LIB_A)
	@mkdir -p $(dir $@)
	$(CXX) $(RTLINK) -o $@ $< $(LIB_A) $(LDFLAGS)

# =============================================
# 目标
# =============================================
.PHONY: all test bench abi-check smoke clean

all: $(LIB_A) $(TEST_BINS) $(TOOL_BINS)

# 基准：固定场景 + 校验和（见 tools/bingpu_bench.c 的文件头）
bench: $(BIN_DIR)/bingpu_bench$(EXT)
	@./$(BIN_DIR)/bingpu_bench$(EXT)

# 窗口路径烟测：三个真后端各跑一遍（交换链 / present / frame_read / resize）。
# 为什么它不在 `test` 里：这一项要开真窗口，CI 机上没有桌面会话时跑不了；
# 但每次"真后端落地"都该手动跑一次，所以给它固定的入口（见 tools/gpu_window_smoke.c）。
smoke: $(BIN_DIR)/gpu_window_smoke$(EXT)
	@fail=0; \
	for b in vk d3d11 gl; do \
	    echo "=== $$b ==="; \
	    ./$(BIN_DIR)/gpu_window_smoke$(EXT) $$b || fail=1; \
	done; \
	if [ $$fail -ne 0 ]; then echo "SMOKE FAILED"; exit 1; fi; \
	echo "window smoke passed"

test: $(TEST_BINS)
	@fail=0; \
	for t in $(TEST_BINS); do \
	    echo "=== $$t ==="; \
	    ./$$t || fail=1; \
	done; \
	if [ $$fail -ne 0 ]; then echo "TESTS FAILED"; exit 1; fi; \
	echo "all tests passed"

# 为什么留 -Wno-pragma-once-outside-header：
#   ABI 头第一行是 #pragma once（正常头文件写法）。GCC 13+ 在"头文件被当作主文件
#   编译"时会警告这一点，而这里的检查方式恰恰就是"把头文件当主文件单独编一遍"。
#   我们只关心语法（C++ 设施混入），所以精确关掉这条排版类警告而不是去掉 -Werror。
abi-check: $(LIB_A)
	@echo "--- 1) compile include/bingpu.h as pure C11 ---"
	$(CC) -std=c11 -Wall -Wextra -Wpedantic -Werror -Wno-pragma-once-outside-header \
	    -fsyntax-only -x c $(INC_DIR)/bingpu.h
	@echo "--- 2) no C++ facilities in the public header ---"
	@# 先把注释剥掉再 grep：头文件的说明文字里会正常出现 std::vector 这类词（讲"实现
	@# 用 C++、ABI 是 C"），要拦的是**声明/代码**里出现 C++ 设施。用编译器自己剥注释
	@# （-fpreprocessed -dD -E -P）比自己写正则可靠。
	@$(CC) -fpreprocessed -dD -E -P $(INC_DIR)/bingpu.h 2>/dev/null \
	    | grep -nE 'namespace|class |template|std::|<string>|<vector>' \
	    && { echo "abi-check FAILED: C++ facility found in include/bingpu.h"; exit 1; } \
	    || echo "ok: no C++ facility"
	@echo "--- 3) C object + C++ static library link with g++ ---"
	@printf '%s\n' '#include <stdio.h>' '#include "bingpu.h"' \
	    'int main(void) { printf("%s/ABI %u\\n", bingpu_version_string(), bingpu_abi_version()); return 0; }' \
	    > $(BUILD_DIR)/abi_smoke.c
	$(CC) -std=c11 -Wall -Wextra -Wpedantic -Werror -I$(INC_DIR) -c $(BUILD_DIR)/abi_smoke.c \
	    -o $(BUILD_DIR)/abi_smoke.o
	$(CXX) $(RTLINK) -o $(BUILD_DIR)/abi_smoke$(EXT) $(BUILD_DIR)/abi_smoke.o $(LIB_A)
	@$(BUILD_DIR)/abi_smoke$(EXT)
	@echo "abi-check passed"

clean:
	rm -rf $(BUILD_DIR)

# =============================================
# 头文件依赖（-MMD -MP 生成的 .d）
# =============================================
# 0.2.2 补：这两个开关一直在用，但生成出来的 .d **从来没被 include 过** —— 于是改
# include/bingpu.h（结构体、版本宏、枚举值）之后，目标文件不会重编，`make` 说
# "Nothing to be done"，链出来的还是旧库。0.2.2 改版本宏时就当场踩到：测试打印
# 0.2.1 而后端是新的。用 wildcard 一次收齐（库 / 测试 / 工具三种对象都在 obj 下）。
-include $(wildcard $(OBJ_DIR)/*.d $(OBJ_DIR)/*/*.d)
