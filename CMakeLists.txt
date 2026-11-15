# CMakeLists.txt - binGPU（C11 静态库 + 两个 ctest 用例）
#
# 与 Makefile 是**等价的两条构建路**：Makefile 是主用（CI 与开发都用它），CMake
# 只是给"把 binGPU 作为子目录拉进别人的工程"准备的。两边的编译选项保持一致，
# 任何一边出警告都当 bug 处理。
cmake_minimum_required(VERSION 3.16)
project(binGPU C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_library(bingpu STATIC
    src/bingpu_api.c
    src/bingpu_cpu.c
    src/bingpu_error.c
    src/bingpu_null.c
    src/bingpu_object.c
)
target_include_directories(bingpu
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(bingpu PRIVATE /W4)
else()
    target_compile_options(bingpu PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---- 测试 ----
enable_testing()

add_executable(test_abi tests/test_abi.c)
target_link_libraries(test_abi PRIVATE bingpu)
add_test(NAME abi COMMAND test_abi)

add_executable(test_cpu_render tests/test_cpu_render.c)
target_link_libraries(test_cpu_render PRIVATE bingpu)
add_test(NAME cpu_render COMMAND test_cpu_render)

# ---- 工具 ----
add_executable(bingpu_info tools/bingpu_info.c)
target_link_libraries(bingpu_info PRIVATE bingpu)
