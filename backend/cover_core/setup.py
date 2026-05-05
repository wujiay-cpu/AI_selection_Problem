from setuptools import setup, Extension
from setuptools.command.build_ext import build_ext
import sys
import pybind11

import os

use_asan = os.environ.get("COVER_ASAN", "0") == "1"

if os.name == "nt":
    if use_asan:
        # ASan debug-oriented build on MSVC.
        extra_args = ["/Od", "/Zi", "/std:c++17", "/utf-8", "/fsanitize=address", "/GL-"]
        extra_link_args = ["/fsanitize=address"]
    else:
        extra_args = ["/O2", "/std:c++17", "/utf-8"]
        extra_link_args = []
else:
    if use_asan:
        extra_args = ["-O1", "-g", "-Wall", "-shared", "-std=c++17", "-fPIC", "-fsanitize=address"]
        extra_link_args = ["-fsanitize=address"]
    else:
        extra_args = ["-O3", "-Wall", "-shared", "-std=c++17", "-fPIC"]
        extra_link_args = []

ext_modules = [
    Extension(
        "cover_core_ext",
        ["src/bindings.cpp"],
        include_dirs=[pybind11.get_include()],
        language="c++",
        extra_compile_args=extra_args,
        extra_link_args=extra_link_args,
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
