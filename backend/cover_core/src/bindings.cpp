#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include "greedy.h"

namespace py = pybind11;

PYBIND11_MODULE(cover_core_ext, m) {
    m.doc() = "C++ accelerated set cover greedy (required_cover==1 path)";

    // greedy_selection(pool, k, j, s) -> list[list[int]]
    // 与 Python 版 get_heuristic_greedy_selection(pool, k, j, s, min_cover=1) 接口一致
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

参数:
    pool : list[int]   样本池
    k    : int         每个候选组合的大小
    j    : int         目标组合的大小
    s    : int         子集大小

返回:
    list[list[int]]    选中的候选组合列表，每个组合内部已排序
)doc");
}
