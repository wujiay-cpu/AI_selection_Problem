import os
import sys

# 将当前目录加入 sys.path 以便导入 C++ 编译生成的 .pyd / .so 文件
current_dir = os.path.dirname(os.path.abspath(__file__))
if current_dir not in sys.path:
    sys.path.insert(0, current_dir)

try:
    from cover_core_ext import add_numbers
except ImportError as e:
    print(f"[Warning] Failed to import cover_core_ext. Error: {e}")
    # 提供一个纯 Python 的 fallback，以免编译失败时整个后端挂掉
    def add_numbers(a, b):
        print("[Fallback] Using pure python add_numbers")
        return a + b

__all__ = ["add_numbers"]
