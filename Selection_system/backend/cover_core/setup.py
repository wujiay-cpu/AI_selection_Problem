from setuptools import setup, Extension
from setuptools.command.build_ext import build_ext
import sys
import pybind11

import os

if os.name == "nt":
    extra_args = ["/O2", "/std:c++17", "/utf-8"]
else:
    extra_args = ["-O3", "-Wall", "-shared", "-std=c++17", "-fPIC"]

ext_modules = [
    Extension(
        "cover_core_ext",
        ["src/core.cpp", "src/bindings.cpp"],
        include_dirs=[pybind11.get_include()],
        language="c++",
        extra_compile_args=extra_args,
    ),
]

setup(
    name="cover_core",
    version="0.0.1",
    author="Author",
    description="A test project using pybind11",
    ext_modules=ext_modules,
    zip_safe=False,
)