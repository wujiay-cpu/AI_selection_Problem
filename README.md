# Selection System：覆盖问题求解器

小组作业项目：求解 covering design 问题。给定 `n` 个元素，从中选出若干个 `k`-子集，使得每个 `j`-子集都至少与某个选中 `k`-子集有 `>= s` 个公共元素。

## 项目结构

```text
Selection_system/
├─ UI/                              # 前端（React + TypeScript + Vite）
├─ backend/
│  ├─ cover_core/                   # C++ 算法核心（pybind11 扩展）
│  │  ├─ src/
│  │  │  ├─ bindings.cpp
│  │  │  ├─ bitmask.h
│  │  │  ├─ core.cpp
│  │  │  ├─ core.h
│  │  │  ├─ dfs.h
│  │  │  └─ greedy.h
│  │  ├─ CMakeLists.txt
│  │  ├─ __init__.py
│  │  └─ setup.py
│  ├─ tests/
│  │  ├─ benchmark_cases.py
│  │  └─ final_baseline_runner_14.py
│  ├─ results/
│  │  ├─ final_baseline_v2.json
│  │  └─ DELIVERY_NOTES.md
│  ├─ algorithm.py
│  ├─ api.py
│  ├─ storage.py
│  ├─ setup.py
│  └─ BUILD.md
├─ TEST_REPORT.md
└─ README.md
```

## 核心算法

求解流程：

1. 建模：枚举 `candidates`（`k`-子集）与 `targets`（`j`-子集），构建覆盖关系。
2. 贪心 seed：快速构造可行解作为初始上界。
3. DFS：分支定界搜索，根并行加速。
4. ILS：在预算内做扰动与局部改进。

关键技术：

- bitmask 与分块掩码加速集合运算
- 多重下界剪枝
- 多线程根并行 DFS
- 大规模 case 的时间预算自适应

## 编译与运行

### 环境要求

- Python 3.13
- Node.js 18+
- Windows: Visual Studio 2022 (MSVC)

### 安装依赖

```bash
npm install
pip install -r requirements.txt
```

### 编译 C++ 扩展

```bash
cd backend/cover_core
python setup.py build_ext --inplace --force
```

### 启动后端 API

```bash
cd backend
python api.py
```

### 启动前端

```bash
npm run dev
```

## 测试

```bash
python backend/tests/final_baseline_runner_14.py
```

测试结果输出到 `backend/results/final_baseline_v2.json`，详细分析见 `TEST_REPORT.md`。

## 测试结果摘要

- `verified=True`: `14/14`
- `diff<=10`: `10/14`
- 总耗时：`914.1s`

## 算法选择

后端固定使用 `run_backtracking_pruning` 算法（先贪心后回溯精确搜索）。前端不暴露算法选择，所有规模统一走该算法路径，与脚本基线测试一致。

## 前端使用说明

### 复现脚本基线

1. 选择手动模式。
2. `selected_numbers` 输入 `1, 2, ..., n`（与 case 的 `n` 对应）。
3. `k`、`m(=j)`、`t(=s)` 与测试表一致。
4. 提交后等待返回。

### 关于随机模式

随机模式从 `1..m` 中抽取 `n` 个元素作为 pool。由于算法只关心集合关系，不依赖元素数值本身，结果应与基线一致；当 pool 大小或参数不同，结果不同属于正常现象。

### 运行时间预期

- case 01-10、12：约 30s 内完成
- case 11/13/14：约 100-400s（已知限制，build 阶段不可中断）
- case 15（`n=28`）：不支持

前端客户端超时已设为 400 秒，可容纳大 case。

## 已知限制

### 时间

默认 `time_limit=30s`，但大规模实例存在超时现象：

- case 11：约 `100-400s`（存在随机波动）
- case 13：约 `100s`
- case 14：约 `150-340s`
- case 15（`n=28`）：未支持

### 解质量

在 `30s` 时限下，以下 case 误差较大：

- case 02：`+41`
- case 05：`+53`
- case 13：`+13`
