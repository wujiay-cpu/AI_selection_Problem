# cover_core — C++ 加速模块构建说明（Windows）

## 目录结构

```
你的项目/
├── algorithm.py          ← 替换为 algorithm_new.py 的内容
├── setup.py              ← 备用构建方式（pip）
└── cover_core/
    ├── CMakeLists.txt    ← 主构建文件
    └── src/
        ├── bitmask.h     ← 分块掩码数据结构
        ├── greedy.h      ← 贪心核心逻辑
        └── bindings.cpp  ← pybind11 绑定
```

构建成功后，`cover_core.pyd`（Windows 的 .so 等价物）会出现在项目根目录。

---

## 前提条件

1. **Python 3.8+**（建议 3.10/3.11）
2. **Visual Studio 2019/2022**，需安装"使用 C++ 的桌面开发"工作负载
   - 或者 **MinGW-w64**（g++ 11+）
3. **CMake 3.15+**：https://cmake.org/download/
4. **pybind11**：
   ```cmd
   pip install pybind11
   ```

---

## 方式一：CMake 构建（推荐）

```cmd
cd cover_core
mkdir build
cd build

:: Visual Studio（x64）
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release

:: 或者 MinGW
cmake .. -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build .
```

构建完成后 `cover_core.pyd` 会出现在项目根目录（`algorithm.py` 的同级目录）。

---

## 方式二：setup.py 构建（备用）

如果你更熟悉 pip 方式：

```cmd
:: 在项目根目录（setup.py 所在位置）运行
pip install pybind11
python setup.py build_ext --inplace
```

`cover_core.pyd` 会出现在当前目录。

---

## 验证安装

```python
import cover_core
result = cover_core.greedy_selection([1,2,3,4,5,6,7,8,9,10], k=5, j=4, s=3)
print(f"结果组数: {len(result)}")
print(f"第一组: {result[0]}")
```

---

## 常见问题

**Q: cmake 报错 "找不到 pybind11"**
```cmd
pip install pybind11
:: 然后重新运行 cmake
```

**Q: MSVC 报错 `__builtin_ctzll` 未定义**

在 `bitmask.h` 顶部加入以下兼容代码（文件里已有注释提示位置）：
```cpp
#ifdef _MSC_VER
#include <intrin.h>
static inline int __builtin_ctzll(unsigned long long x) {
    unsigned long idx;
    _BitScanForward64(&idx, x);
    return (int)idx;
}
static inline int __builtin_popcountll(unsigned long long x) {
    return (int)__popcnt64(x);
}
#endif
```

**Q: MinGW 链接报错 `undefined reference to __popcnt64`**

在 `CMakeLists.txt` 的编译选项里确认有 `-march=native` 或至少 `-mpopcnt`。

**Q: 构建成功但 `import cover_core` 仍然失败**

检查 `cover_core.pyd` 是否在 `algorithm.py` 的同级目录。
也可以把 `.pyd` 路径加入 `sys.path`：
```python
import sys
sys.path.insert(0, r"C:\你的路径\cover_core\build\Release")
import cover_core
```

---

## 替换 algorithm.py

将 `algorithm_new.py` 的内容替换掉原来的 `algorithm.py`。
新版本会在启动时自动检测 `cover_core` 是否可用：

- **可用**：`required_cover==1` 的贪心走 C++，打印 `[cover_core] C++ 加速模块加载成功`
- **不可用**：自动回退纯 Python，打印 `[cover_core] 未找到 C++ 模块，使用纯 Python 实现`

两种情况下接口完全一致，上层代码无需任何改动。
