cmake_minimum_required(VERSION 3.15)
project(cover_core LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- Release 模式下开启最大优化 ----
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# MSVC (Visual Studio)
if(MSVC)
  add_compile_options(
    /O2          # 最大速度优化
    /GL          # 全程序优化
    /arch:AVX2   # 启用 AVX2（含 BMI1，支持 _blsi_u64 等指令）
    /W3
    /utf-8       # 修复中文注释导致的 C2059 语法错误
  )
  add_link_options(/LTCG)  # 链接时代码生成
else()
  # GCC / Clang（MinGW 也走这里）
  add_compile_options(
    -O3
    -march=native   # 使用本机最优指令集（含 BMI1/popcnt）
    -funroll-loops
    -fvisibility=hidden
  )
endif()

# ---- 找 Python 和 pybind11 ----
find_package(Python3 COMPONENTS Interpreter Development REQUIRED)

# pybind11 的查找方式：优先用 pip 安装的版本
execute_process(
  COMMAND "${Python3_EXECUTABLE}" -c "import pybind11; print(pybind11.get_cmake_dir())"
  OUTPUT_VARIABLE pybind11_DIR
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE pybind11_result
)

if(NOT pybind11_result EQUAL 0)
  message(FATAL_ERROR
    "找不到 pybind11。请先运行：pip install pybind11\n"
    "然后重新执行 cmake。"
  )
endif()

find_package(pybind11 CONFIG REQUIRED PATHS "${pybind11_DIR}")

# ---- 构建扩展模块 ----
pybind11_add_module(cover_core
  src/bindings.cpp
)

target_include_directories(cover_core PRIVATE src)

# Windows 上输出到项目根目录，方便 Python 直接 import
set_target_properties(cover_core PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/.."
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/.."
)
