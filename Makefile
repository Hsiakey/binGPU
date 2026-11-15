# Makefile - binGPU (MinGW / GCC / Clang)
#
# 风格照 binUI：变量名、构建目录、产物三分（bin/ 工具、bin/tests/ 测试）。
# 只用 C 编译器 —— 这个项目**没有** C++ 源文件，故意如此（见 docs/abi.md）。
#
# 目标：
#   all        静态库 + 测试 + 工具
#   test       编好后逐个跑，任何失败非 0 退出（CI 就认这个）
#   abi-check  ①纯 C 编译器单独编 include/bingpu.h（混进 C++ 语法当场炸）
#              ②grep 守规矩：头里不许 C++ 类型/命名空间/容器头
#   clean

# make 内建的 CC 默认是 `cc`。MinGW/GCC 的发行版都带 cc，但为了不依赖那个软链，
# 当 CC 还是内建默认值时改用 gcc（用户在命令行或环境里显式指定就尊重它）。
ifeq ($(origin CC),default)
CC := gcc
endif
CFLAGS   ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -MMD -MP
LDFLAGS  ?=
# 静态链接 C 运行时：产物常在 sh 下被执行（make 的 SHELL=sh），
# 而 Git for Windows 的 /mingw64/bin 排在本机 MinGW 前面，
# 会先命中版本不匹配的 libgcc DLL，表现为莫名其妙的退出码 127。
RTLINK   ?= -static-libgcc

# Git for Windows 的 sh.exe 路径带空格：mingw32-make 在 -j N 并行下会把
# `C:/Program Files/...` 原样拼进命令行，CreateProcess 直接失败（process_begin /
# make (e=2)，看着像"系统找不到指定的文件"）。这里固定成 8.3 短路径绕开空格；
# 用 override 是为了调用方传 SHELL='C:/Program Files/Git/bin/sh.exe' 时也以本文件为准，
# 否则同一份 Makefile 串行/并行行为不一致。
ifeq ($(OS),Windows_NT)
    ifneq ($(wildcard C:/PROGRA~1/Git/bin/sh.exe),)
        override SHELL := C:/PROGRA~1/Git/bin/sh.exe
    endif
endif

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
# 静态库
# =============================================
LIB_SRCS := $(wildcard $(SRC_DIR)/*.c)
LIB_OBJS := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(LIB_SRCS))
LIB_A    := $(BUILD_DIR)/libbingpu.a

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(INC_DIR) -I$(SRC_DIR) -c $< -o $@

$(LIB_A): $(LIB_OBJS)
	@mkdir -p $(dir $@)
	ar rcs $@ $^

# =============================================
# 测试（纯 C11，不引第三方）
# =============================================
TEST_BINS := \
    $(TEST_DIR)/test_abi$(EXT) \
    $(TEST_DIR)/test_cpu_render$(EXT)

$(TEST_DIR)/%$(EXT): $(TEST_SRC)/%.c $(LIB_A)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(INC_DIR) -o $@ $< $(LIB_A) $(RTLINK) $(LDFLAGS)

# =============================================
# 工具
# =============================================
TOOL_BINS := $(BIN_DIR)/bingpu_info$(EXT)

$(BIN_DIR)/%$(EXT): tools/%.c $(LIB_A)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(INC_DIR) -o $@ $< $(LIB_A) $(RTLINK) $(LDFLAGS)

# =============================================
# 目标
# =============================================
.PHONY: all test abi-check clean

all: $(LIB_A) $(TEST_BINS) $(TOOL_BINS)

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
#   我们只关心语法（C++ 设施混入），不关心这条排版类警告，所以精确关掉它而不是
#   把 -Werror 去掉 —— 其余警告仍然是错误。
abi-check:
	@echo "--- 1) compile include/bingpu.h as pure C11 ---"
	$(CC) -std=c11 -Wall -Wextra -Wpedantic -Werror -Wno-pragma-once-outside-header \
	    -fsyntax-only -x c $(INC_DIR)/bingpu.h
	@echo "--- 2) no C++ facilities in the public header ---"
	@if grep -nE 'namespace|class |template|std::|<string>|<vector>' $(INC_DIR)/bingpu.h; then \
	    echo "abi-check FAILED: C++ facility found in include/bingpu.h"; exit 1; \
	else \
	    echo "ok: no C++ facility"; \
	fi
	@echo "abi-check passed"

clean:
	rm -rf $(BUILD_DIR)
