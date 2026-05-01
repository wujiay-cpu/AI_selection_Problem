#pragma once
#include "bitmask.h"
#include "greedy.h"
#include <vector>
#include <tuple>
#include <iostream>

// ============================================================
//  backtracking_set_cover（主入口）
// ============================================================
inline std::tuple<std::vector<std::vector<int>>, bool> backtracking_set_cover(
    const std::vector<int>& pool, int k, int j, int s, double time_limit_sec)
{
    std::cout << "[DFS Module] Starting... k=" << k << ", j=" << j << ", s=" << s << std::endl;

    // Use greedy_set_cover as it delegates to the new Ultra DFS
    return greedy_set_cover(pool, k, j, s, time_limit_sec);
}
