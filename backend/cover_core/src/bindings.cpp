#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <algorithm>
#include "greedy.h"
#include "dfs.h"

namespace py = pybind11;

PYBIND11_MODULE(cover_core_ext, m) {
    m.doc() = "C++ accelerated set cover greedy & backtracking";

    // greedy_selection(pool, k, j, s) -> tuple(list[list[int]], bool)
    m.def("greedy_selection",
          [](const std::vector<int>& pool, int k, int j, int s) {
              return greedy_set_cover(pool, k, j, s);
          },
          py::arg("pool"),
          py::arg("k"),
          py::arg("j"),
          py::arg("s"),
          R"doc(
C++ 加速版加权贪心集合覆盖（仅 required_cover==1 路径）。
)doc");

    // greedy_fast_selection(pool, k, j, s) -> tuple(list[list[int]], bool)
    m.def("greedy_fast_selection",
          [](const std::vector<int>& pool, int k, int j, int s) {
              return greedy_fast_set_cover(pool, k, j, s);
          },
          py::arg("pool"),
          py::arg("k"),
          py::arg("j"),
          py::arg("s"),
          R"doc(
C++ 快速贪心预览路径（速度优先），用于前置展示与 warm-up。
)doc");

    // Both functions redirect to the ultimate exact solver
    // beam_search_selection(pool, k, j, s, beam_width, expand_k, time_limit_sec) -> tuple(list[list[int]], bool)
    m.def("beam_search_selection",
          [](const std::vector<int>& pool, int k, int j, int s, int beam_width, int expand_k, double time_limit_sec) {
              // 将 beam 参数映射为 DFS 预算：
              // - branch_cap：每层最多展开的候选分支数
              // - timeout_check_every：更密集地检查超时，提升可中断性
              // - max_depth_cap：快速模式限制递归深度，换速度
              int branch_cap = std::max(4, beam_width / 2 + expand_k);
              int timeout_check_every = (beam_width <= 30) ? 2000 : 5000;
              int max_depth_cap = (beam_width <= 30) ? 64 : ((beam_width <= 80) ? 96 : 128);
              return greedy_set_cover(pool, k, j, s, time_limit_sec, branch_cap, timeout_check_every, max_depth_cap);
          },
          py::arg("pool"),
          py::arg("k"),
          py::arg("j"),
          py::arg("s"),
          py::arg("beam_width") = 50,
          py::arg("expand_k") = 10,
          py::arg("time_limit_sec") = 90.0,
          R"doc(
C++ 加速版集束搜索 (Beam Search) 混合算法，适用于 n <= 20 寻求极高质量解。
)doc");

    // backtracking_selection(pool, k, j, s, time_limit_sec) -> tuple(list[list[int]], bool)
    m.def("backtracking_selection",
          [](const std::vector<int>& pool, int k, int j, int s, double time_limit_sec) {
              return backtracking_set_cover(pool, k, j, s, time_limit_sec);
          },
          py::arg("pool"),
          py::arg("k"),
          py::arg("j"),
          py::arg("s"),
          py::arg("time_limit_sec") = 90.0,
          R"doc(
C++ 加速版带剪枝的 DFS 回溯搜索（带 3-opt 兜底）。
返回 (best_combinations, aborted)
)doc");
}
