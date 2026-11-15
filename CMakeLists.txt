# CMakeLists.txt - binGPU（C++17 实现 + C 兼容 ABI；静态库 + 两个 ctest 用例）
#
# 与 Makefile 是**等价的两条构建路**：Makefile 是主用（CI 与开发都用它），CMake
# 只是给"把 binGPU 作为子目录拉进别人的工程"准备的。两边编译选项保持一致。
#
# 语言说明：库是 C++17（src/*.cpp）；测试/工具是 C 源码（tests/*.c、tools/*.c），
# 它们同时是"C 能消费这个 ABI"的证明。C 目标链接 C++ 静态库时，链接器必须是 C++
# （要带 libstdc++），所以对它们显式设置 LINKER_LANGUAGE CXX。
cmake_minimum_required(VERSION 3.16)
project(binGPU C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# 与 Makefile 的 -static-libgcc -static-libstdc++ 对齐（MinGW 下产物自包含）
if(MINGW)
    add_link_options(-static-libgcc -static-libstdc++)
endif()

add_library(bingpu STATIC
    src/bingpu_api.cpp
    src/bingpu_backend.cpp
    src/bingpu_cpu.cpp
    src/bingpu_error.cpp
    src/bingpu_null.cpp
    src/bingpu_object.cpp
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

# ---- 测试（C 源码 → 用 C 编译器编 → 用 C++ 链接）----
enable_testing()

add_executable(test_abi tests/test_abi.c)
set_target_properties(test_abi PROPERTIES LINKER_LANGUAGE CXX C_STANDARD 11)
target_link_libraries(test_abi PRIVATE bingpu)
add_test(NAME abi COMMAND test_abi)

add_executable(test_cpu_render tests/test_cpu_render.c)
set_target_properties(test_cpu_render PROPERTIES LINKER_LANGUAGE CXX C_STANDARD 11)
target_link_libraries(test_cpu_render PRIVATE bingpu)
add_test(NAME cpu_render COMMAND test_cpu_render)

# ---- 工具（同样是 C 源码）----
add_executable(bingpu_info tools/bingpu_info.c)
set_target_properties(bingpu_info PROPERTIES LINKER_LANGUAGE CXX C_STANDARD 11)
target_link_libraries(bingpu_info PRIVATE bingpu)
