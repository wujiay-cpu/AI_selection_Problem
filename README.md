# Selection System (集合覆盖算法展示系统)

本项目是一个全栈应用程序，专为解决并可视化复杂的**集合覆盖问题 (Set Cover Problem, NP-Hard)** 而设计。

前端采用 **React** 和 **Vite** 构建，提供交互式的参数输入与实时的计算结果展示。后端采用 **Python (FastAPI)** 实现，并集成了一个高度优化的 **C++ 扩展模块 (基于 pybind11)**，以应对组合爆炸带来的巨大计算压力，实现极速的算法求解。

## 目录

- [核心特性](#核心特性)
- [环境要求](#环境要求)
- [项目结构](#项目结构)
- [安装与配置](#安装与配置)
  - [1. 前端配置](#1-前端配置)
  - [2. 后端与 C++ 模块配置](#2-后端与-c-模块配置)
- [运行项目](#运行项目)
- [算法架构解析](#算法架构解析)
- [常见问题排查 (Troubleshooting)](#常见问题排查-troubleshooting)

## 核心特性

- **交互式 UI**：支持实时输入核心参数 ($m, n, k, j, s$) 并选择算法模式。
- **极致性能 (C++ 加速)**：核心的贪心集合覆盖算法由定制的 C++ 模块驱动。通过采用分块位掩码 (Chunked Bitmasks) 和动态目标权重技术，计算速度比纯 Python 实现**快百倍以上**。
- **智能降级 (Smart Fallback)**：系统会自动检测 C++ 加速模块。如果模块未编译，或者用户请求了不支持的参数（例如 `min_cover > 1`），系统会无缝平滑地降级回纯 Python 实现，保证程序不崩溃。
- **结果管理**：支持保存、查看和删除历史计算结果。

## 环境要求

在开始之前，请确保您的计算机已安装以下环境：

- **Node.js** (建议 v16 及以上版本)
- **npm** 或 **Yarn** / **pnpm**
- **Python 3.9+**
- **pip**
- **C++ 编译器**：
  - Windows：Visual Studio 2022 (MSVC)，需勾选“使用 C++ 的桌面开发”工作负载。
  - Linux/Mac：支持 C++17 的 GCC 或 Clang。

## 项目结构

- `UI/`：React 前端源代码（包含 `App.tsx` 等）。
- `backend/`：Python 后端服务。
  - `api.py`：FastAPI 服务器路由与接口定义。
  - `algorithm.py`：Python 算法实现及 C++ 模块的桥接层。
  - `cover_core/`：**C++ 源代码目录**，包含高性能贪心算法的核心实现。
- `results/`：以 `.json` 格式存储的历史计算结果。
- `package.json` / `vite.config.ts`：前端构建配置。

## 安装与配置

### 1. 前端配置

在项目根目录下打开终端，安装 Node.js 依赖：

```bash
npm install
```

### 2. 后端与 C++ 模块配置

在项目根目录下打开终端，安装所需的 Python 依赖包（包括 FastAPI, Uvicorn 和 pybind11）：

```bash
pip install fastapi uvicorn pybind11
```

**编译 C++ 加速模块：**

为了解锁贪心算法的极致性能，您必须手动编译 C++ 扩展模块（生成 `cover_core_ext.pyd` 或 `.so` 文件）。

```bash
cd backend/cover_core
python setup.py build_ext --inplace
```
*注意：编译成功后，当前目录下会生成一个名为 `cover_core_ext...pyd` (Windows) 或 `.so` (Linux/Mac) 的文件。系统在运行时会自动检测并加载它。*

## 运行项目

要完整运行该应用程序，您需要同时启动前端和后端服务器。请在**项目根目录**下打开两个独立的终端窗口。

### 终端 1：后端服务器

启动 FastAPI 后端服务：

```bash
uvicorn backend.api:app --host 0.0.0.0 --port 8000
```
*观察终端输出：如果您看到了 `[cover_core] C++ 加速模块加载成功`，说明 C++ 加速引擎已成功激活！*

### 终端 2：前端开发服务器

启动 Vite 前端开发服务：

```bash
npm run dev
```
前端应用启动后，可在浏览器中访问：[http://localhost:3000](http://localhost:3000)。

## 算法架构解析

本系统的核心挑战在于：从 $n$ 个数字的池子中，找出最少数量的大小为 $k$ 的组合，使得这些组合能够覆盖所有可能的大小为 $j$ 的目标子集。

1. **位掩码优化 (Bitmask Optimization)**：在 C++ 模块中，我们将每一个目标组合映射为 `ChunkedMask`（由多个 `uint64_t` 组成的数组）中的一个二进制位 (Bit)。传统的集合交集操作被替换为极其快速的按位运算 (`AND/OR`) 和底层硬件指令 (`popcount`)。
2. **动态权重分配 (Dynamic Weighting)**：稀缺目标（即很少有候选组合能覆盖到的目标）会被赋予更高的权重分数。贪心选择器会优先挑选能覆盖这些稀缺目标的组合，从而得到总数更少（更优）的最终结果。
3. **冗余剪枝 (Pruning)**：在选择阶段结束后，算法会进行激进的回溯检查，剔除掉那些对整体覆盖率没有提供任何独有贡献的冗余组合。

## 常见问题排查 (Troubleshooting)

- **`[Errno 10048] error while attempting to bind on address` 端口被占用**：
  后端 8000 端口正在被另一个进程使用。请找出并杀掉该进程，命令如下：
  - Windows：`netstat -ano | findstr :8000`（找到 PID 后用 `taskkill /F /PID <PID>` 结束）
  - Linux/Mac：`lsof -i :8000`
- **浏览器控制台报错 `net::ERR_CONNECTION_REFUSED`**：
  请确保后端运行在 8000 端口，而不是旧版的 8001。如果前端依然向错误的端口发请求，请在浏览器中按 `Ctrl + F5` 强制清空缓存并刷新页面。
- **输入超大参数后，前端提示 `Failed to fetch` 或 `ERR_ABORTED`**：
  这是由于组合爆炸（例如输入 $n=30, k=6, j=6$）导致后端计算需要消耗极其庞大的内存（15GB以上）和极长的运算时间。而前端默认设有 90 秒的硬性超时限制。建议您先使用较小的参数（例如 $n=20, k=5, j=4$）进行测试。
