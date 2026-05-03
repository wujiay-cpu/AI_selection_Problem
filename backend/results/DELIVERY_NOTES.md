# 交付说明（Final Baseline v2）

## 结果概览

- 测试脚本：`backend/tests/final_baseline_runner_14.py`
- 结果文件：`backend/results/final_baseline_v2.json`
- 覆盖验证：`14/14` 全部通过（`verified=True`）
- 质量指标：`diff<=10` 为 `10/14`
- 总耗时：`914.1s`

## 已知限制

- 大规模 case 存在明显超时：case 11、13、14。
- 运行时间存在随机性波动，尤其是 case 11。
- 30 秒预算下，case 02、05、13 的误差相对较大。

## 说明

- 本次已清理中间日志与冗余脚本，保留最终可复现实验脚本和结果。
- `backend/cover_core/src/dfs.h` 仍被 `bindings.cpp` 引用，属于核心代码，已保留。
