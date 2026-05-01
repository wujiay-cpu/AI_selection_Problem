#pragma once
#include "bitmask.h"
#include <vector>
#include <algorithm>
#include <numeric>
#include <unordered_map>
#include <iostream>
#include <cstring>
#include <chrono>

// ============================================================
//  极速建模层：Bitmask 替代 Rank
// ============================================================

// 使用位掩码表示组合，支持最多 64 个元素的 pool
static inline uint64_t encode_to_mask(const std::vector<int>& indices) {
    uint64_t m = 0;
    for (int x : indices) m |= (1ULL << x);
    return m;
}

// 组合生成辅助函数
template<typename F>
static void gen_combinations_fast(int n, int r, F callback) {
    if (r > n || r < 0) return;
    if (r == 0) {
        std::vector<int> empty;
        callback(empty);
        return;
    }
    std::vector<int> idx(r);
    std::iota(idx.begin(), idx.end(), 0);
    while (true) {
        callback(idx);
        int i = r - 1;
        while (i >= 0 && idx[i] == n - r + i) --i;
        if (i < 0) break;
        ++idx[i];
        for (int j = i + 1; j < r; ++j) idx[j] = idx[j - 1] + 1;
    }
}

// ============================================================
//  核心构造器：动态 Target 映射 + CSR 构建
// ============================================================
static int build_optimized_problem(
    const std::vector<int>& pool, int k, int j, int s,
    std::vector<Candidate>& out_candidates,
    std::vector<int>& out_flat_cands,
    std::vector<int>& out_offsets)
{
    int n = (int)pool.size();
    std::unordered_map<uint64_t, int> target_to_id;
    std::vector<std::vector<int>> bit_to_cands_map;

    out_candidates.clear();

    // 1. 遍历所有候选者，提取活跃 Target 并构建映射
    gen_combinations_fast(n, k, [&](const std::vector<int>& cand_idx) {
        int cand_id = out_candidates.size();
        int bc = 0;
        
        // 生成外部元素索引
        std::vector<int> outside_idx;
        outside_idx.reserve(n - k);
        int cand_ptr = 0;
        for (int i = 0; i < n; ++i) {
            if (cand_ptr < k && i == cand_idx[cand_ptr]) {
                cand_ptr++;
            } else {
                outside_idx.push_back(i);
            }
        }

        int max_t = std::min(k, j);
        for (int t = s; t <= max_t; ++t) {
            if (j - t > n - k) continue; // outside 不够选

            gen_combinations_fast(k, t, [&](const std::vector<int>& cand_sub_idx) {
                gen_combinations_fast(n - k, j - t, [&](const std::vector<int>& out_sub_idx) {
                    // 合并组成一个 j-target
                    std::vector<int> target_idx(j);
                    int p1 = 0, p2 = 0, pt = 0;
                    while (p1 < t || p2 < j - t) {
                        if (p2 == j - t || (p1 < t && cand_idx[cand_sub_idx[p1]] < outside_idx[out_sub_idx[p2]])) {
                            target_idx[pt++] = cand_idx[cand_sub_idx[p1++]];
                        } else {
                            target_idx[pt++] = outside_idx[out_sub_idx[p2++]];
                        }
                    }
                    
                    uint64_t t_mask = encode_to_mask(target_idx);
                    auto it = target_to_id.find(t_mask);
                    int tid;
                    if (it == target_to_id.end()) {
                        tid = target_to_id.size();
                        target_to_id[t_mask] = tid;
                        bit_to_cands_map.emplace_back(); // dynamic resize
                    } else {
                        tid = it->second;
                    }
                    bit_to_cands_map[tid].push_back(cand_id);
                    bc++;
                });
            });
        }
        if (bc > 0) {
            Candidate c;
            c.bc = bc;
            for (int x : cand_idx) c.members.push_back(pool[x]);
            out_candidates.push_back(std::move(c));
        }
    });

    int target_count = target_to_id.size();
    int num_chunks = (target_count + 63) / 64;
    
    // 提前释放 target_to_id 节约内存
    std::unordered_map<uint64_t, int>().swap(target_to_id);

    // 2. 填充 Candidate 对象中的 mask
    for (int i = 0; i < (int)out_candidates.size(); ++i) {
        out_candidates[i].mask = ChunkedMask(num_chunks);
    }
    for (int tid = 0; tid < target_count; ++tid) {
        for (int ci : bit_to_cands_map[tid]) {
            out_candidates[ci].mask.set_bit(tid);
        }
    }

    // 3. 构建 CSR (压缩邻接表)，并动态释放 bit_to_cands_map 内存
    out_offsets.assign(target_count + 1, 0);
    out_flat_cands.clear();
    // 预分配大约需要的空间
    size_t total_edges = 0;
    for (const auto& list : bit_to_cands_map) total_edges += list.size();
    out_flat_cands.reserve(total_edges);

    for (int i = 0; i < target_count; ++i) {
        // 先给 candidate 按 bc 降序排个序，有利于 DFS 第一步走好
        std::sort(bit_to_cands_map[i].begin(), bit_to_cands_map[i].end(), [&](int a, int b) {
            return out_candidates[a].bc > out_candidates[b].bc;
        });
        out_offsets[i + 1] = out_offsets[i] + bit_to_cands_map[i].size();
        for (int ci : bit_to_cands_map[i]) out_flat_cands.push_back(ci);
        
        // Free memory early!
        std::vector<int>().swap(bit_to_cands_map[i]);
    }
    std::vector<std::vector<int>>().swap(bit_to_cands_map);

    return target_count;
}

// ============================================================
//  极限 DFS：CSR + 增量回溯 (修复栈溢出)
// ============================================================
static void dfs_ultra(
    const std::vector<Candidate>& cand,
    const int* flat_cands, const int* offsets,
    uint64_t* covered, const uint64_t* all_mask,
    int num_chunks, int& best_ub,
    std::vector<int>& path, std::vector<int>& best_path,
    std::vector<uint64_t>& backup_memory, int depth,
    int max_depth_limit,
    uint64_t& node_count,
    const std::chrono::time_point<std::chrono::steady_clock>& deadline, bool& aborted)
{
    if (aborted) return;
    if ((int)path.size() >= best_ub) return;

    // Hard depth guard to avoid Windows stack overflow in deep DFS recursion.
    if (depth >= max_depth_limit) {
        aborted = true;
        return;
    }

    // Check timeout every N nodes
    if ((++node_count % 10000ULL) == 0ULL) {
        if (std::chrono::steady_clock::now() > deadline) {
            aborted = true;
            return;
        }
    }
    
    // 如果深度即将超过我们预分配的内存，中止该分支
    if (depth >= best_ub - 1) return;

    // 1. 寻找最难覆盖的 Target (DLX 启发式)
    int best_bit = -1;
    int min_deg = 999999;
    int remaining_targets = 0;

    for (int c = 0; c < num_chunks; ++c) {
        uint64_t miss = all_mask[c] & ~covered[c];
        remaining_targets += __builtin_popcountll(miss);
        while (miss) {
            int bit = c * 64 + __builtin_ctzll(miss);
            int deg = offsets[bit + 1] - offsets[bit];
            if (deg < min_deg) {
                min_deg = deg;
                best_bit = bit;
                if (min_deg == 1) goto found_bit; // 优化：度为1直接选
            }
            miss &= miss - 1;
        }
    }
    if (best_bit == -1) { // 全部覆盖完成
        best_ub = path.size();
        best_path = path;
        std::cout << "Optimal = " << best_ub << std::endl;
        return;
    }

    // 简单下界剪枝 (LB = 剩余目标数 / 最大单次覆盖数)
    // 假设最大的单次覆盖数为 cand[0].bc (因为按 bc 排序过了，但严谨起见，可以动态算或者给个保守值，这里假设 max_bc 为 cand 里的最大值，为了简单这里只计算池中可选候选者的最大可能增益)
    // 这里做个保守的剪枝，如果剩余的目标就算每次能覆盖很多也无法在达到 best_ub 前完成
    // 假设最大的单次增益不超过初始的最大 bc
    {
        int max_possible_gain = 1;
        for (int i = offsets[best_bit]; i < offsets[best_bit + 1]; ++i) {
            int ci = flat_cands[i];
            int gain = 0;
            for (int k = 0; k < num_chunks; ++k) {
                gain += __builtin_popcountll(cand[ci].mask.chunks[k] & ~covered[k]);
            }
            if (gain > max_possible_gain) max_possible_gain = gain;
        }
        int lb = (remaining_targets + max_possible_gain - 1) / max_possible_gain;
        if ((int)path.size() + lb >= best_ub) return;
    }

found_bit:
    // 收集候选者并按当前实际增益排序
    struct LocalCand {
        int ci;
        int gain;
    };
    std::vector<LocalCand> local_cands;
    local_cands.reserve(offsets[best_bit + 1] - offsets[best_bit]);
    
    for (int i = offsets[best_bit]; i < offsets[best_bit + 1]; ++i) {
        int ci = flat_cands[i];
        int gain = 0;
        for (int k = 0; k < num_chunks; ++k) {
            gain += __builtin_popcountll(cand[ci].mask.chunks[k] & ~covered[k]);
        }
        if (gain > 0) {
            local_cands.push_back({ci, gain});
        }
    }
    
    // 按实际增益降序排序
    std::sort(local_cands.begin(), local_cands.end(), [](const LocalCand& a, const LocalCand& b) {
        return a.gain > b.gain;
    });

    // 获取当前层的堆内存指针 (避免 Stack Overflow)
    uint64_t* backup = &backup_memory[depth * num_chunks];

    // 2. 遍历 CSR 中的候选者
    for (const auto& lc : local_cands) {
        int ci = lc.ci;
        
        // 保存现场
        for (int k = 0; k < num_chunks; ++k) {
            backup[k] = covered[k];
            covered[k] |= cand[ci].mask.chunks[k];
        }

        path.push_back(ci);
        dfs_ultra(cand, flat_cands, offsets, covered, all_mask, num_chunks, best_ub, path, best_path,
                  backup_memory, depth + 1, max_depth_limit, node_count, deadline, aborted);
        path.pop_back();
        
        // 恢复现场
        for (int k = 0; k < num_chunks; ++k) covered[k] = backup[k];
    }
}

// ============================================================
//  主入口
// ============================================================
inline std::tuple<std::vector<std::vector<int>>, bool> greedy_set_cover(
    const std::vector<int>& pool, int k, int j, int s, double time_limit_sec = 90.0)
{
    std::vector<Candidate> candidates;
    std::vector<int> flat_cands, offsets;
    
    std::cout << "[Step 1] Building Optimized Model..." << std::endl;
    int target_count = build_optimized_problem(pool, k, j, s, candidates, flat_cands, offsets);
    
    int num_chunks = (target_count + 63) / 64;
    std::vector<uint64_t> all_mask(num_chunks, 0);
    for (int i = 0; i < target_count; ++i) all_mask[i / 64] |= (1ULL << (i % 64));

    std::cout << "[Step 2] Running Ultra DFS on " << target_count << " targets..." << std::endl;
    
    int best_ub = 999999;
    std::vector<int> path, best_path;
    
    // 动态分配而不是固定 MAX_CHUNKS，节省内存
    std::vector<uint64_t> covered(num_chunks, 0);
    
    // 预分配回溯用的栈内存：假设最大深度为 pool 选出来的 k 元素的数量，保守估计 500 层，不够会自动处理或崩溃。
    // 但是，最坏深度其实也就是最优解的层数，通常远小于 target_count。
    // 我们先给一个合理的深度，如果实际 best_ub 比这个还大，那就截断
    // Keep recursion depth conservative to prevent process crash on Windows default stack.
    int max_depth = std::min(target_count, 128);
    if (max_depth <= 0) max_depth = 1;
    std::vector<uint64_t> backup_memory(max_depth * num_chunks, 0);

    auto start_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(time_limit_sec));
    auto deadline = start_time + duration;
    bool aborted = false;
    uint64_t node_count = 0;
    
    // 如果最优解的上界大于我们分配的内存深度，把它限制住
    best_ub = max_depth;

    dfs_ultra(candidates, flat_cands.data(), offsets.data(),
              covered.data(), all_mask.data(), num_chunks,
              best_ub, path, best_path, backup_memory, 0, max_depth, node_count, deadline, aborted);

    if (aborted) {
        std::cout << "DFS Ultra aborted due to time limit (" << time_limit_sec << "s)." << std::endl;
    }

    if (best_path.empty()) {
        std::cerr << "Warning: DFS Ultra failed to cover all targets." << std::endl;
        return {{}, aborted};
    }

    std::cout << "DFS Ultra optimal size: " << best_path.size() << std::endl;

    // 格式化输出
    std::vector<std::vector<int>> result;
    result.reserve(best_path.size());
    for (int id : best_path) {
        auto members = candidates[id].members;
        std::sort(members.begin(), members.end());
        result.push_back(members);
    }
    return {result, aborted};
}
