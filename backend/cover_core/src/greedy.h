#pragma once
#include "bitmask.h"
#include <vector>
#include <algorithm>
#include <numeric>
#include <unordered_map>
#include <iostream>
#include <cstring>
#include <chrono>
#include <limits>

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

static inline size_t comb_estimate_clamped(int n, int k, size_t cap = 2000000) {
    if (k < 0 || k > n) return 0;
    k = std::min(k, n - k);
    long double v = 1.0L;
    for (int i = 1; i <= k; ++i) {
        v = v * (long double)(n - k + i) / (long double)i;
        if (v >= (long double)cap) return cap;
    }
    if (v < 1.0L) return 1;
    return (size_t)v;
}

static inline bool mask_is_subset_of(const ChunkedMask& a, const ChunkedMask& b) {
    int n = (int)a.chunks.size();
    for (int i = 0; i < n; ++i) {
        if (a.chunks[i] & ~b.chunks[i]) return false;
    }
    return true;
}

static inline std::vector<std::vector<size_t>> build_nck_table(int n, int r, size_t cap = (size_t)4e9) {
    std::vector<std::vector<size_t>> c(n + 1, std::vector<size_t>(r + 1, 0));
    for (int i = 0; i <= n; ++i) {
        c[i][0] = 1;
        for (int j = 1; j <= std::min(i, r); ++j) {
            size_t v = c[i - 1][j - 1] + c[i - 1][j];
            c[i][j] = (v > cap) ? cap : v;
        }
    }
    return c;
}

// Colex rank: rank = C(c0,1) + C(c1,2) + ... + C(c_{r-1}, r)
static inline size_t rank_combination_colex(const std::vector<int>& comb, const std::vector<std::vector<size_t>>& nck) {
    size_t rank = 0;
    for (int i = 0; i < (int)comb.size(); ++i) {
        int ci = comb[i];
        int pick = i + 1;
        if (ci >= 0 && ci < (int)nck.size() && pick < (int)nck[ci].size()) {
            rank += nck[ci][pick];
        }
    }
    return rank;
}

struct DFSCandEntry {
    int ci;
    int gain;
};

// ============================================================
//  核心构造器：动态 Target 映射 + CSR 构建
// ============================================================
static int build_optimized_problem(
    const std::vector<int>& pool, int k, int j, int s,
    std::vector<Candidate>& out_candidates,
    std::vector<int>& out_flat_cands,
    std::vector<int>& out_offsets,
    std::vector<int>* out_cand_flat_targets = nullptr,
    std::vector<int>* out_cand_offsets = nullptr,
    std::vector<uint64_t>* out_all_masks = nullptr)
{
    int n = (int)pool.size();
    out_candidates.clear();
    out_candidates.reserve(comb_estimate_clamped(n, k));
    std::unordered_map<uint64_t, int> cand_mask_to_id; // fallback path
    auto nck_for_k = build_nck_table(n, k, (size_t)2e9);
    size_t total_cands = (k >= 0 && k < (int)nck_for_k[n].size()) ? nck_for_k[n][k] : 0;
    bool use_rank_map = (total_cands > 0 && total_cands <= (size_t)20000000);
    if (!use_rank_map && n > 64) {
        std::cerr << "[Error] n > 64 requires rank-map path to avoid uint64 hash collisions." << std::endl;
        return 0;
    }
    std::vector<int> rank_to_cand_id;
    if (use_rank_map) {
        rank_to_cand_id.assign(total_cands, -1);
    } else {
        cand_mask_to_id.reserve(out_candidates.capacity() * 2 + 1);
    }

    // 1) 先完整生成所有 k-candidate，建立 mask->id 映射（避免后续重复构造 candidate）
    gen_combinations_fast(n, k, [&](const std::vector<int>& cand_idx) {
        Candidate c;
        c.bc = 0;
        c.members.reserve(k);
        for (int x : cand_idx) c.members.push_back(pool[x]);
        int cand_id = (int)out_candidates.size();
        out_candidates.push_back(std::move(c));
        if (use_rank_map) {
            size_t rid = rank_combination_colex(cand_idx, nck_for_k);
            if (rid < rank_to_cand_id.size()) rank_to_cand_id[rid] = cand_id;
        } else {
            cand_mask_to_id.emplace(encode_to_mask(cand_idx), cand_id);
        }
    });

    // 2) 外层按 j-target 枚举；内层按交集规模 t 生成能够覆盖该 target 的 candidate
    std::vector<std::vector<int>> bit_to_cands_map;
    bit_to_cands_map.reserve(comb_estimate_clamped(n, j));
    gen_combinations_fast(n, j, [&](const std::vector<int>& target_idx) {
        bit_to_cands_map.emplace_back();
        auto& linked_cands = bit_to_cands_map.back();

        std::vector<int> outside_idx;
        outside_idx.reserve(n - j);
        int tp = 0;
        for (int i = 0; i < n; ++i) {
            if (tp < j && i == target_idx[tp]) {
                tp++;
            } else {
                outside_idx.push_back(i);
            }
        }

        int max_t = std::min(k, j);
        for (int t = s; t <= max_t; ++t) {
            if (k - t > n - j) continue;
            gen_combinations_fast(j, t, [&](const std::vector<int>& target_sub_idx) {
                gen_combinations_fast(n - j, k - t, [&](const std::vector<int>& out_sub_idx) {
                    std::vector<int> cand_idx(k);
                    int p1 = 0, p2 = 0, pc = 0;
                    while (p1 < t || p2 < k - t) {
                        if (p2 == k - t || (p1 < t && target_idx[target_sub_idx[p1]] < outside_idx[out_sub_idx[p2]])) {
                            cand_idx[pc++] = target_idx[target_sub_idx[p1++]];
                        } else {
                            cand_idx[pc++] = outside_idx[out_sub_idx[p2++]];
                        }
                    }
                    if (use_rank_map) {
                        size_t rid = rank_combination_colex(cand_idx, nck_for_k);
                        if (rid < rank_to_cand_id.size()) {
                            int cid = rank_to_cand_id[rid];
                            if (cid >= 0) linked_cands.push_back(cid);
                        }
                    } else {
                        auto it = cand_mask_to_id.find(encode_to_mask(cand_idx));
                        if (it != cand_mask_to_id.end()) {
                            linked_cands.push_back(it->second);
                        }
                    }
                });
            });
        }
    });

    int target_count = (int)bit_to_cands_map.size();
    int num_chunks = (target_count + 63) / 64;

    // 3) 统计每个候选的覆盖数并填充 mask
    // 反向枚举下 (target, candidate) 理论上唯一，不需要每个 target 做 sort+unique。
    for (int tid = 0; tid < target_count; ++tid) {
        auto& lst = bit_to_cands_map[tid];
        for (int ci : lst) {
            out_candidates[ci].bc++;
        }
    }

    for (int i = 0; i < (int)out_candidates.size(); ++i) {
        out_candidates[i].mask = ChunkedMask(num_chunks);
    }
    for (int tid = 0; tid < target_count; ++tid) {
        for (int ci : bit_to_cands_map[tid]) {
            out_candidates[ci].mask.set_bit(tid);
        }
    }

    // 3.5) 候选支配关系预处理：若 mask[A] ⊆ mask[B]，则 A 可删除（等成本）
    // 为避免 O(N^2) 在超大候选集上爆炸，超过阈值时跳过该预处理。
    if (out_candidates.size() <= 30000) {
        std::vector<int> order(out_candidates.size());
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [&](int x, int y) {
            if (out_candidates[x].bc != out_candidates[y].bc) return out_candidates[x].bc > out_candidates[y].bc;
            return x < y;
        });

        std::vector<int> kept;
        kept.reserve(order.size());
        std::vector<char> keep_flag(out_candidates.size(), 0);

        for (int ci : order) {
            if (out_candidates[ci].bc <= 0) continue;
            bool dominated = false;
            for (int kj : kept) {
                if (mask_is_subset_of(out_candidates[ci].mask, out_candidates[kj].mask)) {
                    dominated = true;
                    break;
                }
            }
            if (!dominated) {
                keep_flag[ci] = 1;
                kept.push_back(ci);
            }
        }

        if (kept.size() < out_candidates.size()) {
            std::vector<int> remap(out_candidates.size(), -1);
            std::vector<Candidate> new_candidates;
            new_candidates.reserve(kept.size());
            for (int old_id : kept) {
                remap[old_id] = (int)new_candidates.size();
                new_candidates.push_back(std::move(out_candidates[old_id]));
            }
            out_candidates.swap(new_candidates);

            for (int tid = 0; tid < target_count; ++tid) {
                auto& lst = bit_to_cands_map[tid];
                int w = 0;
                for (int old_id : lst) {
                    int nid = remap[old_id];
                    if (nid >= 0) lst[w++] = nid;
                }
                lst.resize(w);
            }
        }
    }

    // 4) 可选：构建扁平候选 mask（提高 DFS 热路径局部性）
    if (out_all_masks) {
        out_all_masks->assign((size_t)out_candidates.size() * (size_t)num_chunks, 0ULL);
        for (size_t ci = 0; ci < out_candidates.size(); ++ci) {
            std::memcpy(out_all_masks->data() + ci * (size_t)num_chunks,
                        out_candidates[ci].mask.chunks.data(),
                        sizeof(uint64_t) * (size_t)num_chunks);
        }
    }

    // 5) 可选：构建 candidate->targets 邻接（用于更强下界）
    if (out_cand_flat_targets && out_cand_offsets) {
        out_cand_offsets->assign(out_candidates.size() + 1, 0);
        for (int tid = 0; tid < target_count; ++tid) {
            for (int ci : bit_to_cands_map[tid]) {
                (*out_cand_offsets)[ci + 1]++;
            }
        }
        for (size_t i = 0; i + 1 < out_cand_offsets->size(); ++i) {
            (*out_cand_offsets)[i + 1] += (*out_cand_offsets)[i];
        }
        out_cand_flat_targets->assign((*out_cand_offsets).back(), 0);
        std::vector<int> cursor = *out_cand_offsets;
        for (int tid = 0; tid < target_count; ++tid) {
            for (int ci : bit_to_cands_map[tid]) {
                int pos = cursor[ci]++;
                (*out_cand_flat_targets)[pos] = tid;
            }
        }
    }

    // 6) 构建 CSR (压缩邻接表)，并动态释放 bit_to_cands_map 内存
    out_offsets.assign(target_count + 1, 0);
    out_flat_cands.clear();
    size_t total_edges = 0;
    for (const auto& list : bit_to_cands_map) total_edges += list.size();
    out_flat_cands.reserve(total_edges);

    for (int i = 0; i < target_count; ++i) {
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
    int target_count,
    int global_max_gain_upper,
    int branch_cap_per_level,
    const uint64_t* cand_masks_flat,
    const int* cand_flat_targets,
    const int* cand_offsets,
    std::vector<int>& gains,
    std::vector<int>& live_deg,
    std::vector<int>& gain_rollback,
    std::vector<DFSCandEntry>& local_pool,
    int local_pool_stride,
    std::vector<int>& blocked_ver,
    int& blocked_cur_ver,
    uint64_t timeout_check_every_nodes,
    uint64_t& node_count,
    const std::chrono::time_point<std::chrono::steady_clock>& deadline, bool& aborted)
{
    if (aborted) return;
    if ((int)path.size() >= best_ub) return;

    // Hard depth guard to avoid Windows stack overflow in deep DFS recursion.
    if (depth >= max_depth_limit) {
        return;
    }

    // Check timeout every N nodes
    if ((++node_count % timeout_check_every_nodes) == 0ULL) {
        if (std::chrono::steady_clock::now() > deadline) {
            aborted = true;
            return;
        }
    }
    
    // 1. 寻找最难覆盖的 Target (DLX 启发式)
    int best_bit = -1;
    int min_deg = 999999;
    int remaining_targets = 0;

    for (int c = 0; c < num_chunks; ++c) {
        uint64_t miss = all_mask[c] & ~covered[c];
        remaining_targets += __builtin_popcountll(miss);
        while (miss) {
            int bit = c * 64 + __builtin_ctzll(miss);
            int deg = live_deg[bit];
            if (deg == 0) {
                return; // 存在未覆盖 target 且无可用候选，当前分支不可行
            }
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

    // 安全下界剪枝
    // 1) LB1 = ceil(remaining_targets / global_max_gain_upper)
    // 2) LB2 = 不相交打包下界：选若干 pairwise-incompatible 的未覆盖 target
    {
        int max_possible_gain = std::max(1, std::min(global_max_gain_upper, remaining_targets));
        int lb1 = (remaining_targets + max_possible_gain - 1) / max_possible_gain;
        int lb2 = 0;
        if (cand_flat_targets && cand_offsets && remaining_targets >= 64) {
            blocked_cur_ver++;
            if (blocked_cur_ver == std::numeric_limits<int>::max()) {
                std::fill(blocked_ver.begin(), blocked_ver.end(), 0);
                blocked_cur_ver = 1;
            }
            for (int tid = 0; tid < target_count; ++tid) {
                int c = tid >> 6;
                uint64_t b = 1ULL << (tid & 63);
                if ((all_mask[c] & b) == 0 || (covered[c] & b) != 0 || blocked_ver[tid] == blocked_cur_ver) continue;
                lb2++;
                if ((int)path.size() + std::max(lb1, lb2) >= best_ub) {
                    return;
                }
                blocked_ver[tid] = blocked_cur_ver;
                for (int p = offsets[tid]; p < offsets[tid + 1]; ++p) {
                    int ci = flat_cands[p];
                    for (int q = cand_offsets[ci]; q < cand_offsets[ci + 1]; ++q) {
                        blocked_ver[cand_flat_targets[q]] = blocked_cur_ver;
                    }
                }
            }
        }
        int lb = std::max(lb1, lb2);
        if ((int)path.size() + lb >= best_ub) return;
    }

found_bit:
    // 收集候选者并按当前实际增益排序（使用按深度复用池，减少频繁分配）
    size_t need_local = (size_t)(depth + 1) * (size_t)local_pool_stride;
    if (local_pool.size() < need_local) {
        local_pool.resize(need_local);
    }
    int base = depth * local_pool_stride;
    int local_count = 0;
    for (int i = offsets[best_bit]; i < offsets[best_bit + 1]; ++i) {
        int ci = flat_cands[i];
        int gain = gains[ci];
        if (gain > 0) {
            local_pool[base + local_count++] = {ci, gain};
        }
    }

    // 按实际增益降序排序
    std::sort(local_pool.begin() + base, local_pool.begin() + base + local_count, [](const DFSCandEntry& a, const DFSCandEntry& b) {
        return a.gain > b.gain;
    });

    // 获取当前层的堆内存指针 (按需扩容，避免硬编码深度导致丢解)
    size_t need = (size_t)(depth + 1) * (size_t)num_chunks;
    if (backup_memory.size() < need) {
        backup_memory.resize(need);
    }
    uint64_t* backup = &backup_memory[depth * num_chunks];

    // 2. 遍历 CSR 中的候选者
    int expanded = 0;
    for (int li = 0; li < local_count; ++li) {
        const auto& lc = local_pool[base + li];
        if (branch_cap_per_level > 0 && expanded >= branch_cap_per_level) {
            break;
        }
        int ci = lc.ci;
        size_t gain_mark = gain_rollback.size();
        bool any_new = false;
        const uint64_t* c_mask = cand_masks_flat ? (cand_masks_flat + (size_t)ci * (size_t)num_chunks) : cand[ci].mask.chunks.data();
        
        // 保存现场
        for (int k = 0; k < num_chunks; ++k) {
            backup[k] = covered[k];
            uint64_t add_bits = c_mask[k] & ~covered[k];
            if (add_bits) any_new = true;
            covered[k] |= c_mask[k];
        }
        if (!any_new) {
            for (int k = 0; k < num_chunks; ++k) covered[k] = backup[k];
            continue;
        }

        // 增量更新 gain：对新覆盖 target 的所有关联候选减 1
        for (int c = 0; c < num_chunks; ++c) {
            uint64_t newly = c_mask[c] & ~backup[c];
            while (newly) {
                int local = __builtin_ctzll(newly);
                int tid = c * 64 + local;
                for (int p = offsets[tid]; p < offsets[tid + 1]; ++p) {
                    int cj = flat_cands[p];
                    if (gains[cj] > 0) {
                        int old_gain = gains[cj];
                        gains[cj]--;
                        gain_rollback.push_back(cj);
                        if (old_gain == 1 && cand_flat_targets && cand_offsets) {
                            for (int q = cand_offsets[cj]; q < cand_offsets[cj + 1]; ++q) {
                                int t2 = cand_flat_targets[q];
                                live_deg[t2]--;
                            }
                        }
                    }
                }
                newly &= newly - 1;
            }
        }

        path.push_back(ci);
        dfs_ultra(cand, flat_cands, offsets, covered, all_mask, num_chunks, best_ub, path, best_path,
                  backup_memory, depth + 1, max_depth_limit, target_count, global_max_gain_upper, branch_cap_per_level,
                  cand_masks_flat,
                  cand_flat_targets, cand_offsets,
                  gains, live_deg, gain_rollback, local_pool, local_pool_stride,
                  blocked_ver, blocked_cur_ver,
                  timeout_check_every_nodes, node_count, deadline, aborted);
        path.pop_back();
        expanded++;
        
        while (gain_rollback.size() > gain_mark) {
            int cj = gain_rollback.back();
            gain_rollback.pop_back();
            int new_gain = ++gains[cj];
            if (new_gain == 1 && cand_flat_targets && cand_offsets) {
                for (int q = cand_offsets[cj]; q < cand_offsets[cj + 1]; ++q) {
                    int t2 = cand_flat_targets[q];
                    live_deg[t2]++;
                }
            }
        }

        // 恢复现场
        for (int k = 0; k < num_chunks; ++k) covered[k] = backup[k];
    }
}

// ============================================================
//  主入口
// ============================================================
inline std::tuple<std::vector<std::vector<int>>, bool> greedy_fast_set_cover(
    const std::vector<int>& pool, int k, int j, int s)
{
    std::vector<Candidate> candidates;
    std::vector<int> flat_cands, offsets;
    std::vector<uint64_t> all_cand_masks;

    int target_count = build_optimized_problem(pool, k, j, s, candidates, flat_cands, offsets, nullptr, nullptr, &all_cand_masks);
    if (target_count <= 0 || candidates.empty()) {
        return {{}, false};
    }

    int num_chunks = (target_count + 63) / 64;
    std::vector<uint64_t> all_mask(num_chunks, 0);
    for (int i = 0; i < target_count; ++i) all_mask[i / 64] |= (1ULL << (i % 64));

    std::vector<uint64_t> covered(num_chunks, 0);
    std::vector<int> selected_ids;
    std::vector<char> used(candidates.size(), 0);

    auto is_fully_covered = [&]() -> bool {
        for (int i = 0; i < num_chunks; ++i) {
            if ((covered[i] & all_mask[i]) != all_mask[i]) return false;
        }
        return true;
    };

    while (!is_fully_covered()) {
        int best_ci = -1;
        int best_gain = 0;
        int best_bc = -1;

        for (int ci = 0; ci < (int)candidates.size(); ++ci) {
            if (used[ci]) continue;
            const uint64_t* c_mask = all_cand_masks.data() + (size_t)ci * (size_t)num_chunks;
            int gain = 0;
            for (int c = 0; c < num_chunks; ++c) {
                gain += __builtin_popcountll(c_mask[c] & ~covered[c]);
            }
            if (gain > best_gain || (gain == best_gain && gain > 0 && candidates[ci].bc > best_bc)) {
                best_gain = gain;
                best_bc = candidates[ci].bc;
                best_ci = ci;
            }
        }

        if (best_ci < 0 || best_gain <= 0) {
            // 无法继续覆盖，返回当前最优近似
            break;
        }

        used[best_ci] = 1;
        selected_ids.push_back(best_ci);
        const uint64_t* b_mask = all_cand_masks.data() + (size_t)best_ci * (size_t)num_chunks;
        for (int c = 0; c < num_chunks; ++c) {
            covered[c] |= b_mask[c];
        }
    }

    // 轻量冗余剔除
    bool changed = true;
    while (changed && selected_ids.size() > 1) {
        changed = false;
        for (int i = 0; i < (int)selected_ids.size(); ++i) {
            std::vector<uint64_t> mask_without(num_chunks, 0);
            for (int j2 = 0; j2 < (int)selected_ids.size(); ++j2) {
                if (j2 == i) continue;
                int ci = selected_ids[j2];
                const uint64_t* c_mask = all_cand_masks.data() + (size_t)ci * (size_t)num_chunks;
                for (int c = 0; c < num_chunks; ++c) {
                    mask_without[c] |= c_mask[c];
                }
            }
            bool still_cover = true;
            for (int c = 0; c < num_chunks; ++c) {
                if ((mask_without[c] & all_mask[c]) != all_mask[c]) {
                    still_cover = false;
                    break;
                }
            }
            if (still_cover) {
                selected_ids.erase(selected_ids.begin() + i);
                changed = true;
                break;
            }
        }
    }

    std::vector<std::vector<int>> result;
    result.reserve(selected_ids.size());
    for (int id : selected_ids) {
        auto members = candidates[id].members;
        std::sort(members.begin(), members.end());
        result.push_back(std::move(members));
    }
    return {result, false};
}

inline std::tuple<std::vector<std::vector<int>>, bool> greedy_set_cover(
    const std::vector<int>& pool, int k, int j, int s, double time_limit_sec = 90.0,
    int branch_cap_per_level = 0, int timeout_check_every_nodes = 10000, int max_depth_cap = 0)
{
    std::vector<Candidate> candidates;
    std::vector<int> flat_cands, offsets;
    std::vector<int> cand_flat_targets, cand_offsets;
    std::vector<uint64_t> all_cand_masks;
    std::vector<std::vector<int>> fallback_result;
    
    std::cout << "[Step 1] Building Optimized Model..." << std::endl;
    int target_count = build_optimized_problem(pool, k, j, s, candidates, flat_cands, offsets, &cand_flat_targets, &cand_offsets, &all_cand_masks);
    if (target_count <= 0 || candidates.empty()) {
        return {{}, false};
    }

    // 先跑快速贪心拿到一个可行解，作为 DFS 初始上界和超时兜底答案
    {
        auto seed = greedy_fast_set_cover(pool, k, j, s);
        fallback_result = std::get<0>(seed);
    }
    
    int num_chunks = (target_count + 63) / 64;
    std::vector<uint64_t> all_mask(num_chunks, 0);
    for (int i = 0; i < target_count; ++i) all_mask[i / 64] |= (1ULL << (i % 64));

    std::cout << "[Step 2] Running Ultra DFS on " << target_count << " targets..." << std::endl;
    
    int best_ub = fallback_result.empty() ? std::numeric_limits<int>::max() : (int)fallback_result.size();
    int global_max_gain_upper = 1;
    std::vector<int> gains(candidates.size(), 0);
    std::vector<int> live_deg(target_count, 0);
    for (size_t i = 0; i < candidates.size(); ++i) {
        gains[i] = candidates[i].bc;
        if (gains[i] > global_max_gain_upper) global_max_gain_upper = gains[i];
    }
    for (int tid = 0; tid < target_count; ++tid) {
        int deg = 0;
        for (int p = offsets[tid]; p < offsets[tid + 1]; ++p) {
            if (gains[flat_cands[p]] > 0) deg++;
        }
        live_deg[tid] = deg;
    }
    std::vector<int> gain_rollback;
    gain_rollback.reserve((size_t)target_count * 8);
    int max_degree = 1;
    for (int tid = 0; tid < target_count; ++tid) {
        int deg = offsets[tid + 1] - offsets[tid];
        if (deg > max_degree) max_degree = deg;
    }
    std::vector<DFSCandEntry> local_pool;
    std::vector<int> blocked_ver(target_count, 0);
    int blocked_cur_ver = 0;
    std::vector<int> path, best_path;
    
    // 动态分配而不是固定 MAX_CHUNKS，节省内存
    std::vector<uint64_t> covered(num_chunks, 0);
    
    // 预分配回溯用的栈内存：假设最大深度为 pool 选出来的 k 元素的数量，保守估计 500 层，不够会自动处理或崩溃。
    // 但是，最坏深度其实也就是最优解的层数，通常远小于 target_count。
    // 我们先给一个合理的深度，如果实际 best_ub 比这个还大，那就截断
    // Keep recursion depth conservative to prevent process crash on Windows default stack.
    int max_depth = (best_ub == std::numeric_limits<int>::max()) ? target_count : best_ub;
    if (max_depth_cap > 0) {
        max_depth = std::min(max_depth, max_depth_cap);
    }
    if (max_depth <= 0) max_depth = 1;
    size_t initial_depth = (size_t)std::min(max_depth, 64);
    local_pool.reserve(std::max<size_t>(1, initial_depth * (size_t)max_degree));
    std::vector<uint64_t> backup_memory(std::max<size_t>(1, initial_depth * (size_t)num_chunks), 0);

    auto start_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(time_limit_sec));
    auto deadline = start_time + duration;
    bool aborted = false;
    uint64_t node_count = 0;
    uint64_t timeout_check_every = (uint64_t)std::max(1, timeout_check_every_nodes);
    
    // 若限制深度小于当前上界，收紧上界避免无效探索
    if (best_ub > max_depth) {
        best_ub = max_depth;
    }

    dfs_ultra(candidates, flat_cands.data(), offsets.data(),
              covered.data(), all_mask.data(), num_chunks,
              best_ub, path, best_path, backup_memory, 0, max_depth, target_count, global_max_gain_upper, branch_cap_per_level,
              all_cand_masks.data(),
              cand_flat_targets.data(), cand_offsets.data(),
              gains, live_deg, gain_rollback, local_pool, max_degree,
              blocked_ver, blocked_cur_ver,
              timeout_check_every, node_count, deadline, aborted);

    if (aborted) {
        std::cout << "DFS Ultra aborted due to time limit (" << time_limit_sec << "s)." << std::endl;
    }

    std::vector<std::vector<int>> result;
    if (!best_path.empty()) {
        std::cout << "DFS Ultra best size: " << best_path.size() << std::endl;
        result.reserve(best_path.size());
        for (int id : best_path) {
            auto members = candidates[id].members;
            std::sort(members.begin(), members.end());
            result.push_back(members);
        }
    } else if (!fallback_result.empty()) {
        std::cout << "DFS Ultra fallback to greedy seed size: " << fallback_result.size() << std::endl;
        result = std::move(fallback_result);
    } else {
        std::cerr << "Warning: DFS Ultra failed to cover all targets." << std::endl;
    }
    return {result, aborted};
}
