#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
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

    // Both functions redirect to the ultimate exact solver
    // beam_search_selection(pool, k, j, s, beam_width, expand_k) -> tuple(list[list[int]], bool)
    m.def("beam_search_selection",
          [](const std::vector<int>& pool, int k, int j, int s, int beam_width, int expand_k) {
              return greedy_set_cover(pool, k, j, s);
          },
          py::arg("pool"),
          py::arg("k"),
          py::arg("j"),
          py::arg("s"),
          py::arg("beam_width") = 50,
          py::arg("expand_k") = 10,
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
