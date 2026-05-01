"""
备用构建方式：pip install . 或 python setup.py build_ext --inplace
适用于不想用 CMake 的用户。
"""
from setuptools import setup, Extension
import pybind11
import sys
import os

# 编译器标志
if sys.platform == "win32":
    extra_compile_args = ["/O2", "/GL", "/arch:AVX2", "/std:c++17"]
    extra_link_args    = ["/LTCG"]
else:
    extra_compile_args = ["-O3", "-march=native", "-funroll-loops",
                          "-std=c++17", "-fvisibility=hidden"]
    extra_link_args    = []

ext = Extension(
    name="cover_core",
    sources=["cover_core/src/bindings.cpp"],
    include_dirs=[
        pybind11.get_include(),
        "cover_core/src",
    ],
    extra_compile_args=extra_compile_args,
    extra_link_args=extra_link_args,
    language="c++",
)

setup(
    name="cover_core",
    version="1.0.0",
    ext_modules=[ext],
    zip_safe=False,
)
