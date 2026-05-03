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
#include <thread>
#include <mutex>
#include <atomic>
#include <cstdint>
#include <random>
#include <cstdlib>
#include <sstream>
#include <cassert>
#if defined(__AVX512VPOPCNTDQ__) && defined(__AVX512BW__)
#include <immintrin.h>
#endif

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

static inline int popcount_andnot_gain(const uint64_t* cand_mask, const uint64_t* covered_mask, int num_chunks) {
#if defined(__AVX512VPOPCNTDQ__) && defined(__AVX512BW__)
    __m512i acc = _mm512_setzero_si512();
    int c = 0;
    for (; c + 8 <= num_chunks; c += 8) {
        __m512i a = _mm512_loadu_si512((const void*)(cand_mask + c));
        __m512i b = _mm512_loadu_si512((const void*)(covered_mask + c));
        __m512i d = _mm512_andnot_si512(b, a);
        acc = _mm512_add_epi64(acc, _mm512_popcnt_epi64(d));
    }
    int gain = (int)_mm512_reduce_add_epi64(acc);
    for (; c < num_chunks; ++c) {
        gain += __builtin_popcountll(cand_mask[c] & ~covered_mask[c]);
    }
    return gain;
#else
    int gain = 0;
    for (int c = 0; c < num_chunks; ++c) {
        gain += __builtin_popcountll(cand_mask[c] & ~covered_mask[c]);
    }
    return gain;
#endif
}

struct DFSCandEntry {
    int ci;
    int gain;
};

static inline uint64_t ceil_mul_div_u64(uint64_t a, uint64_t b, uint64_t den) {
    if (den == 0) return std::numeric_limits<uint64_t>::max();
    long double v = ((long double)a * (long double)b) / (long double)den;
    if (v > (long double)std::numeric_limits<uint64_t>::max()) {
        return std::numeric_limits<uint64_t>::max();
    }
    uint64_t flo = (uint64_t)v;
    return (v > (long double)flo) ? (flo + 1ULL) : flo;
}

// Schonheim lower bound for covering design C(v, k, t), primarily used when s == j.
static inline int schonheim_lower_bound(int v, int k, int t) {
    if (t < 0 || k <= 0 || v < 0 || t > k || k > v) return 0;
    if (t == 0) return 1;
    uint64_t lb = 1;
    // Nested ceil: ceil(v/k * ceil((v-1)/(k-1) * ...))
    for (int i = t - 1; i >= 0; --i) {
        int num = v - i;
        int den = k - i;
        if (num <= 0 || den <= 0) return 0;
        lb = ceil_mul_div_u64(lb, (uint64_t)num, (uint64_t)den);
        if (lb > (uint64_t)std::numeric_limits<int>::max()) {
            return std::numeric_limits<int>::max();
        }
    }
    return (int)lb;
}

static inline void update_coverage_count_by_candidate(
    int cid,
    int delta,
    const std::vector<uint64_t>& all_cand_masks,
    int num_chunks,
    int target_count,
    std::vector<int>& coverage_count)
{
    const uint64_t* m = all_cand_masks.data() + (size_t)cid * (size_t)num_chunks;
    for (int c = 0; c < num_chunks; ++c) {
        uint64_t bits = m[c];
        while (bits) {
            int local = __builtin_ctzll(bits);
            int tid = c * 64 + local;
            if (tid < target_count) coverage_count[tid] += delta;
            bits &= bits - 1;
        }
    }
}

static inline bool can_remove_candidate_fast(
    int cid,
    const std::vector<uint64_t>& all_cand_masks,
    int num_chunks,
    int target_count,
    const std::vector<int>& coverage_count)
{
    const uint64_t* m = all_cand_masks.data() + (size_t)cid * (size_t)num_chunks;
    for (int c = 0; c < num_chunks; ++c) {
        uint64_t bits = m[c];
        while (bits) {
            int local = __builtin_ctzll(bits);
            int tid = c * 64 + local;
            if (tid < target_count && coverage_count[tid] <= 1) return false;
            bits &= bits - 1;
        }
    }
    return true;
}

static inline bool full_covered_by_count(const std::vector<int>& coverage_count) {
    for (int v : coverage_count) {
        if (v <= 0) return false;
    }
    return true;
}

static inline void local_search_swap(
    std::vector<int>& selected_ids,
    const std::vector<Candidate>& candidates,
    const std::vector<uint64_t>& all_cand_masks,
    int num_chunks,
    int target_count,
    int max_iters);

static inline bool is_fully_covered_mask(const std::vector<uint64_t>& covered, const std::vector<uint64_t>& all_mask, int num_chunks) {
    for (int i = 0; i < num_chunks; ++i) {
        if ((covered[i] & all_mask[i]) != all_mask[i]) return false;
    }
    return true;
}

static inline std::string members_key(const std::vector<int>& members) {
    std::string key;
    key.reserve(members.size() * 4);
    for (size_t i = 0; i < members.size(); ++i) {
        if (i) key.push_back(',');
        key += std::to_string(members[i]);
    }
    return key;
}

static inline bool cover_debug_trace_enabled() {
    static int cached = -1;
    if (cached == -1) {
        const char* env_v = std::getenv("COVER_DEBUG_TRACE");
        cached = (env_v && std::atoi(env_v) != 0) ? 1 : 0;
    }
    return cached == 1;
}

static inline void greedy_complete_ids(
    std::vector<int>& selected_ids,
    std::vector<uint64_t>& covered,
    std::vector<char>& used,
    const std::vector<Candidate>& candidates,
    const std::vector<uint64_t>& all_cand_masks,
    const std::vector<uint64_t>& all_mask,
    int num_chunks,
    std::mt19937_64& rng,
    int top_k_random = 2)
{
    int randomized_topk = std::max(1, top_k_random);
    while (!is_fully_covered_mask(covered, all_mask, num_chunks)) {
        int best_gain = 0;
        std::vector<int> top_ids;
        std::vector<int> top_bc;
        top_ids.reserve((size_t)randomized_topk);
        top_bc.reserve((size_t)randomized_topk);
        for (int ci = 0; ci < (int)candidates.size(); ++ci) {
            if (used[ci]) continue;
            const uint64_t* c_mask = all_cand_masks.data() + (size_t)ci * (size_t)num_chunks;
            int gain = popcount_andnot_gain(c_mask, covered.data(), num_chunks);
            if (gain <= 0) continue;
            if (gain > best_gain) {
                best_gain = gain;
                top_ids.clear();
                top_bc.clear();
            }
            if (gain == best_gain) {
                int bc = candidates[ci].bc;
                int pos = (int)top_ids.size();
                while (pos > 0 && top_bc[pos - 1] < bc) pos--;
                if (pos < randomized_topk) {
                    top_ids.insert(top_ids.begin() + pos, ci);
                    top_bc.insert(top_bc.begin() + pos, bc);
                    if ((int)top_ids.size() > randomized_topk) {
                        top_ids.pop_back();
                        top_bc.pop_back();
                    }
                }
            }
        }
        if (top_ids.empty() || best_gain <= 0) break;
        int pick_idx = 0;
        if ((int)top_ids.size() > 1) {
            std::uniform_int_distribution<int> dist(0, (int)top_ids.size() - 1);
            pick_idx = dist(rng);
        }
        int picked = top_ids[pick_idx];
        used[picked] = 1;
        selected_ids.push_back(picked);
        const uint64_t* m = all_cand_masks.data() + (size_t)picked * (size_t)num_chunks;
        for (int c = 0; c < num_chunks; ++c) covered[c] |= m[c];
    }
}

static inline std::vector<int> ils_improve_ids(
    const std::vector<int>& init_ids,
    const std::vector<Candidate>& candidates,
    const std::vector<uint64_t>& all_cand_masks,
    const std::vector<uint64_t>& all_mask,
    int num_chunks,
    int target_count,
    const std::chrono::time_point<std::chrono::steady_clock>& deadline,
    std::mt19937_64& rng,
    double destroy_ratio_hint = -1.0)
{
    if (init_ids.empty()) return init_ids;
    std::vector<int> current = init_ids;
    std::vector<int> best = init_ids;
    int no_improve = 0;
    int no_improve_cap = 200;
    std::uniform_real_distribution<double> u01(0.0, 1.0);
    double base_ratio = (destroy_ratio_hint > 0.0) ? destroy_ratio_hint : 0.20;
    double adaptive_ratio = base_ratio;
    double temperature = 0.8;
    while (std::chrono::steady_clock::now() < deadline && no_improve < no_improve_cap) {
        std::vector<int> trial = current;
        if ((int)trial.size() <= 1) break;
        int destroy_size = std::max(1, (int)std::round((double)trial.size() * adaptive_ratio));
        if ((int)trial.size() <= 5) {
            destroy_size = std::max(destroy_size, std::max(1, (int)trial.size() / 2));
        } else if ((int)trial.size() <= 15) {
            destroy_size = std::max(destroy_size, std::max(2, (int)trial.size() / 4));
        } else {
            destroy_size = std::max(destroy_size, std::max(3, (int)trial.size() / 5));
        }
        destroy_size = std::min(destroy_size, (int)trial.size() - 1);
        std::shuffle(trial.begin(), trial.end(), rng);
        trial.resize(trial.size() - destroy_size);

        std::vector<uint64_t> covered(num_chunks, 0);
        std::vector<char> used(candidates.size(), 0);
        for (int ci : trial) {
            if (ci < 0 || ci >= (int)candidates.size() || used[ci]) continue;
            used[ci] = 1;
            const uint64_t* m = all_cand_masks.data() + (size_t)ci * (size_t)num_chunks;
            for (int c = 0; c < num_chunks; ++c) covered[c] |= m[c];
        }

        greedy_complete_ids(trial, covered, used, candidates, all_cand_masks, all_mask, num_chunks, rng, 2);
        if (!is_fully_covered_mask(covered, all_mask, num_chunks)) {
            no_improve++;
            continue;
        }

        local_search_swap(trial, candidates, all_cand_masks, num_chunks, target_count, 30);
        if (trial.size() < current.size()) {
            current = std::move(trial);
            no_improve = 0;
            adaptive_ratio = std::max(base_ratio, adaptive_ratio * 0.90);
            if (current.size() < best.size()) {
                best = current;
            }
        } else if (trial.size() == current.size()) {
            current = std::move(trial);
            no_improve++;
        } else {
            int delta = (int)trial.size() - (int)current.size();
            double prob = std::exp(-(double)delta / std::max(0.02, temperature));
            if (u01(rng) < prob) {
                current = std::move(trial);
            }
            no_improve++;
        }
        if (no_improve > 0 && (no_improve % 40) == 0) {
            adaptive_ratio = std::min(0.60, adaptive_ratio + 0.05);
            no_improve_cap = std::min(400, no_improve_cap + 20);
        }
        temperature = std::max(0.02, temperature * 0.995);
    }
    return best;
}

// s == 1 fast path:
// Cover all j-subsets by ensuring the union of chosen k-sets has size >= n-j+1.
// Optimal count is ceil((n-j+1)/k), and this constructor reaches that bound.
static inline std::vector<std::vector<int>> solve_s1_fast(const std::vector<int>& pool, int k, int j) {
    std::vector<std::vector<int>> out;
    int n = (int)pool.size();
    if (n <= 0 || k <= 0 || j <= 0 || j > n || k > n) return out;

    int need_union = n - j + 1;
    if (need_union <= 0) return out;
    int groups = (need_union + k - 1) / k;
    if (groups <= 0) return out;

    int cursor = 0;
    out.reserve(groups);
    for (int g = 0; g < groups; ++g) {
        std::vector<int> block;
        block.reserve(k);
        int can_take_new = std::min(k, std::max(0, need_union - cursor));
        for (int t = 0; t < can_take_new; ++t) {
            block.push_back(pool[cursor++]);
        }
        int fill_idx = 0;
        while ((int)block.size() < k) {
            block.push_back(pool[fill_idx++ % n]);
        }
        std::sort(block.begin(), block.end());
        out.push_back(std::move(block));
    }
    return out;
}

// Local search:
// - 1-for-0: remove redundant selected candidate
// - 2-for-1: replace two selected candidates with one unselected candidate
static inline void local_search_swap(
    std::vector<int>& selected_ids,
    const std::vector<Candidate>& candidates,
    const std::vector<uint64_t>& all_cand_masks,
    int num_chunks,
    int target_count,
    int max_iters = 100)
{
    if (selected_ids.empty() || candidates.empty() || num_chunks <= 0) return;
    if (target_count <= 0) return;

    std::vector<int> coverage_count(target_count, 0);
    std::vector<char> selected_flag(candidates.size(), 0);
    for (int cid : selected_ids) {
        if (cid < 0 || cid >= (int)candidates.size()) continue;
        selected_flag[cid] = 1;
        update_coverage_count_by_candidate(cid, +1, all_cand_masks, num_chunks, target_count, coverage_count);
    }
    if (!full_covered_by_count(coverage_count)) return;

    for (int iter = 0; iter < max_iters; ++iter) {
        bool changed = false;

        // 1-for-0
        for (int i = 0; i < (int)selected_ids.size(); ++i) {
            int cid = selected_ids[i];
            if (can_remove_candidate_fast(cid, all_cand_masks, num_chunks, target_count, coverage_count)) {
                update_coverage_count_by_candidate(cid, -1, all_cand_masks, num_chunks, target_count, coverage_count);
                selected_flag[cid] = 0;
                selected_ids.erase(selected_ids.begin() + i);
                changed = true;
                break;
            }
        }
        if (changed) continue;

        // 2-for-1
        int m = (int)selected_ids.size();
        for (int i = 0; i < m && !changed; ++i) {
            for (int j = i + 1; j < m && !changed; ++j) {
                int a = selected_ids[i];
                int b = selected_ids[j];
                const uint64_t* ma = all_cand_masks.data() + (size_t)a * (size_t)num_chunks;
                const uint64_t* mb = all_cand_masks.data() + (size_t)b * (size_t)num_chunks;

                std::vector<uint64_t> deficit_mask(num_chunks, 0ULL);
                bool has_deficit = false;

                for (int c = 0; c < num_chunks; ++c) {
                    uint64_t uni = ma[c] | mb[c];
                    while (uni) {
                        int local = __builtin_ctzll(uni);
                        int tid = c * 64 + local;
                        if (tid < target_count) {
                            int in_pair = ((ma[c] >> local) & 1ULL) + ((mb[c] >> local) & 1ULL);
                            if (coverage_count[tid] - in_pair <= 0) {
                                deficit_mask[c] |= (1ULL << local);
                                has_deficit = true;
                            }
                        }
                        uni &= uni - 1;
                    }
                }

                if (!has_deficit) continue; // 2-for-0 is intentionally skipped; 1-for-0 handles redundancy safely.
                int replacement = -1;
                int best_bc = -1;
                for (int ci = 0; ci < (int)candidates.size(); ++ci) {
                    if (selected_flag[ci]) continue;
                    const uint64_t* mc = all_cand_masks.data() + (size_t)ci * (size_t)num_chunks;
                    bool cover_all_gap = true;
                    for (int c = 0; c < num_chunks; ++c) {
                        if ((mc[c] & deficit_mask[c]) != deficit_mask[c]) {
                            cover_all_gap = false;
                            break;
                        }
                    }
                    if (cover_all_gap && candidates[ci].bc > best_bc) {
                        best_bc = candidates[ci].bc;
                        replacement = ci;
                    }
                }
                if (replacement < 0) continue;

                int hi = std::max(i, j), lo = std::min(i, j);
                update_coverage_count_by_candidate(selected_ids[hi], -1, all_cand_masks, num_chunks, target_count, coverage_count);
                update_coverage_count_by_candidate(selected_ids[lo], -1, all_cand_masks, num_chunks, target_count, coverage_count);
                selected_flag[selected_ids[hi]] = 0;
                selected_flag[selected_ids[lo]] = 0;
                selected_ids.erase(selected_ids.begin() + hi);
                selected_ids.erase(selected_ids.begin() + lo);

                if (replacement >= 0) {
                    selected_ids.push_back(replacement);
                    selected_flag[replacement] = 1;
                    update_coverage_count_by_candidate(replacement, +1, all_cand_masks, num_chunks, target_count, coverage_count);
                }

                if (!full_covered_by_count(coverage_count)) {
                    // Safety rollback: if bug slips in, keep previous valid solution by aborting local search.
                    return;
                }
                changed = true;
            }
        }

        if (!changed) break;
    }
}

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
    std::vector<uint64_t>* out_all_masks = nullptr,
    bool enable_dominance_pruning = true)
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
    // 为避免 O(N^2 * num_chunks) 爆炸，按乘积阈值控制。
    if (enable_dominance_pruning && (size_t)out_candidates.size() * (size_t)num_chunks <= 100000000ULL) {
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
                // kept 按 bc 非增顺序加入，出现更小 bc 后可提前停止。
                if (out_candidates[kj].bc < out_candidates[ci].bc) break;
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
    std::vector<int>& gain_cnt,
    int& cur_max_gain,
    std::vector<int>& live_deg,
    std::vector<int>& gain_rollback,
    std::vector<int>& uncovered_list,
    std::vector<int>& uncovered_pos,
    std::vector<int>& uncovered_rollback,
    std::vector<DFSCandEntry>& local_pool,
    int local_pool_stride,
    std::vector<int>& blocked_ver,
    int& blocked_cur_ver,
    bool enable_lb2,
    bool enable_lb3,
    uint64_t& last_improvement_node,
    uint64_t no_improve_node_cap,
    uint64_t timeout_check_every_nodes,
    uint64_t& node_count,
    const std::chrono::time_point<std::chrono::steady_clock>& deadline, bool& aborted)
{
    if (cover_debug_trace_enabled() && depth <= 1) {
        std::cerr << "[trace][dfs] enter depth=" << depth
                  << " path=" << path.size()
                  << " best_ub=" << best_ub
                  << " nodes=" << node_count << "\n";
        std::cerr.flush();
    }
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
    if (no_improve_node_cap > 0 && node_count > last_improvement_node &&
        (node_count - last_improvement_node) > no_improve_node_cap) {
        aborted = true;
        return;
    }
    
    // 1. 在未覆盖 target 列表中寻找最难覆盖目标 (DLX 启发式)
    int best_bit = -1;
    int min_deg = 999999;
    int remaining_targets = (int)uncovered_list.size();
    if (remaining_targets == 0) { // 全部覆盖完成
        best_ub = path.size();
        best_path = path;
        last_improvement_node = node_count;
        std::cout << "Optimal = " << best_ub << std::endl;
        return;
    }

    for (int tid : uncovered_list) {
        int deg = live_deg[tid];
        if (deg == 0) {
            return; // 存在未覆盖 target 且无可用候选，当前分支不可行
        }
        if (deg < min_deg) {
            min_deg = deg;
            best_bit = tid;
            if (min_deg == 1) break;
        }
    }
    if (best_bit < 0) return;

    // 安全下界剪枝
    // 1) LB1 = ceil(remaining_targets / cur_max_gain)
    // 2) LB3 = LP/Lovasz 风格下界：sum_t 1 / max_{c covers t}(gain[c])
    // 3) LB2 = 不相交打包下界（更贵，放最后）
    {
        int max_possible_gain = std::max(1, std::min(cur_max_gain, remaining_targets));
        int lb1 = (remaining_targets + max_possible_gain - 1) / max_possible_gain;
        if ((int)path.size() + lb1 >= best_ub) return;

        int lb3 = 0;
        bool should_eval_lb3 = enable_lb3 && ((int)path.size() + lb1 + 3 >= best_ub);
        if (should_eval_lb3) {
            double sum_lp = 0.0;
            for (int tid : uncovered_list) {
                int max_cov = 1;
                for (int p = offsets[tid]; p < offsets[tid + 1]; ++p) {
                    int ci = flat_cands[p];
                    int g = gains[ci];
                    if (g > max_cov) max_cov = g;
                }
                sum_lp += 1.0 / (double)max_cov;
            }
            lb3 = (int)std::ceil(sum_lp - 1e-9);
        }
        int lb13 = std::max(lb1, lb3);
        if ((int)path.size() + lb13 >= best_ub) return;

        int lb2 = 0;
        if (enable_lb2 && cand_flat_targets && cand_offsets && remaining_targets <= 5000) {
            blocked_cur_ver++;
            if (blocked_cur_ver == std::numeric_limits<int>::max()) {
                std::fill(blocked_ver.begin(), blocked_ver.end(), 0);
                blocked_cur_ver = 1;
            }
            for (int tid : uncovered_list) {
                if (blocked_ver[tid] == blocked_cur_ver) continue;
                lb2++;
                if ((int)path.size() + std::max(lb13, lb2) >= best_ub) {
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
        int lb = std::max(lb13, lb2);
        if ((int)path.size() + lb >= best_ub) return;
    }
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

    // backup_memory 由调用方按 max_depth 预分配，DFS 内只做契约校验。
    size_t need = (size_t)(depth + 1) * (size_t)num_chunks;
    assert(backup_memory.size() >= need && "backup_memory undersized; UAF risk");
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
        size_t uncovered_mark = uncovered_rollback.size();
        int cur_max_mark = cur_max_gain;
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
                if (tid < target_count && uncovered_pos[tid] != -1) {
                    int pos = uncovered_pos[tid];
                    int last_tid = uncovered_list.back();
                    std::swap(uncovered_list[pos], uncovered_list.back());
                    uncovered_list.pop_back();
                    uncovered_pos[last_tid] = pos;
                    uncovered_pos[tid] = -1;
                    uncovered_rollback.push_back(tid);
                }
                for (int p = offsets[tid]; p < offsets[tid + 1]; ++p) {
                    int cj = flat_cands[p];
                    if (gains[cj] > 0) {
                        int old_gain = gains[cj];
                        gain_cnt[old_gain]--;
                        gains[cj]--;
                        gain_cnt[old_gain - 1]++;
                        gain_rollback.push_back(cj);
                        while (cur_max_gain > 0 && gain_cnt[cur_max_gain] == 0) cur_max_gain--;
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
                  gains, gain_cnt, cur_max_gain, live_deg, gain_rollback,
                  uncovered_list, uncovered_pos, uncovered_rollback, local_pool, local_pool_stride,
                  blocked_ver, blocked_cur_ver,
                  enable_lb2, enable_lb3, last_improvement_node, no_improve_node_cap,
                  timeout_check_every_nodes, node_count, deadline, aborted);
        path.pop_back();
        expanded++;
        
        while (gain_rollback.size() > gain_mark) {
            int cj = gain_rollback.back();
            gain_rollback.pop_back();
            int old_gain = gains[cj];
            gain_cnt[old_gain]--;
            int new_gain = ++gains[cj];
            gain_cnt[new_gain]++;
            if (new_gain == 1 && cand_flat_targets && cand_offsets) {
                for (int q = cand_offsets[cj]; q < cand_offsets[cj + 1]; ++q) {
                    int t2 = cand_flat_targets[q];
                    live_deg[t2]++;
                }
            }
        }
        while (uncovered_rollback.size() > uncovered_mark) {
            int tid = uncovered_rollback.back();
            uncovered_rollback.pop_back();
            uncovered_pos[tid] = (int)uncovered_list.size();
            uncovered_list.push_back(tid);
        }
        cur_max_gain = cur_max_mark;

        // 恢复现场
        for (int k = 0; k < num_chunks; ++k) covered[k] = backup[k];
    }
}

// ============================================================
//  主入口
// ============================================================
inline std::tuple<std::vector<std::vector<int>>, bool> greedy_fast_set_cover(
    const std::vector<int>& pool, int k, int j, int s,
    int top_k_random = 1, int restarts = 1, uint64_t base_seed = 123456789ULL,
    const std::chrono::steady_clock::time_point* hard_deadline = nullptr)
{
    if (s == 1) {
        return {solve_s1_fast(pool, k, j), false};
    }
    std::vector<Candidate> candidates;
    std::vector<int> flat_cands, offsets;
    std::vector<uint64_t> all_cand_masks;

    bool enable_dominance_pruning = (s != j);
    int target_count = build_optimized_problem(pool, k, j, s, candidates, flat_cands, offsets, nullptr, nullptr, &all_cand_masks, enable_dominance_pruning);
    if (target_count <= 0 || candidates.empty()) {
        return {{}, false};
    }

    int num_chunks = (target_count + 63) / 64;
    std::vector<uint64_t> all_mask(num_chunks, 0);
    for (int i = 0; i < target_count; ++i) all_mask[i / 64] |= (1ULL << (i % 64));

    int randomized_topk = std::max(1, top_k_random);
    int total_restarts = std::max(1, restarts);
    std::vector<int> best_selected_ids;

    auto run_one = [&](std::mt19937_64& rng) -> std::vector<int> {
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
            if (hard_deadline && std::chrono::steady_clock::now() >= *hard_deadline) break;
            int best_gain = 0;
            std::vector<int> top_ids;
            top_ids.reserve((size_t)randomized_topk);
            std::vector<int> top_bc;
            top_bc.reserve((size_t)randomized_topk);

            for (int ci = 0; ci < (int)candidates.size(); ++ci) {
                if (used[ci]) continue;
                const uint64_t* c_mask = all_cand_masks.data() + (size_t)ci * (size_t)num_chunks;
                int gain = popcount_andnot_gain(c_mask, covered.data(), num_chunks);
                if (gain <= 0) continue;
                if (gain > best_gain) {
                    best_gain = gain;
                    top_ids.clear();
                    top_bc.clear();
                }
                if (gain == best_gain) {
                    int bc = candidates[ci].bc;
                    int pos = (int)top_ids.size();
                    while (pos > 0 && top_bc[pos - 1] < bc) pos--;
                    if (pos < randomized_topk) {
                        top_ids.insert(top_ids.begin() + pos, ci);
                        top_bc.insert(top_bc.begin() + pos, bc);
                        if ((int)top_ids.size() > randomized_topk) {
                            top_ids.pop_back();
                            top_bc.pop_back();
                        }
                    }
                }
            }

            if (top_ids.empty() || best_gain <= 0) break;
            int pick_idx = 0;
            if ((int)top_ids.size() > 1) {
                std::uniform_int_distribution<int> dist(0, (int)top_ids.size() - 1);
                pick_idx = dist(rng);
            }
            int best_ci = top_ids[pick_idx];
            used[best_ci] = 1;
            selected_ids.push_back(best_ci);
            const uint64_t* b_mask = all_cand_masks.data() + (size_t)best_ci * (size_t)num_chunks;
            for (int c = 0; c < num_chunks; ++c) covered[c] |= b_mask[c];
        }

        if (!(hard_deadline && std::chrono::steady_clock::now() >= *hard_deadline)) {
            local_search_swap(selected_ids, candidates, all_cand_masks, num_chunks, target_count, 100);
        }

        return selected_ids;
    };

    std::mt19937_64 rng(base_seed);
    for (int r = 0; r < total_restarts; ++r) {
        auto cur = run_one(rng);
        if (best_selected_ids.empty() || (!cur.empty() && cur.size() < best_selected_ids.size())) {
            best_selected_ids = std::move(cur);
        }
    }

    std::vector<std::vector<int>> result;
    result.reserve(best_selected_ids.size());
    for (int id : best_selected_ids) {
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
    if (s == 1) {
        return {solve_s1_fast(pool, k, j), false};
    }
    auto t0 = std::chrono::steady_clock::now();
    // 自动放宽：大规模 case 在严格 30s 内 build 都跑不完，自动延长 time_limit
    int n_pool = (int)pool.size();
    size_t est_targets = comb_estimate_clamped(n_pool, j);
    size_t est_cands = comb_estimate_clamped(n_pool, k);
    // 经验阈值：targets * cands > 5e8 时 build 需要 30+ 秒
    double min_needed_sec = ((double)est_targets * (double)est_cands) / 5e6;  // 经验:每秒 5e6 element-pair
    if (min_needed_sec > time_limit_sec * 0.5 && time_limit_sec < 120.0) {
        double new_limit = std::min(120.0, std::max(time_limit_sec, min_needed_sec * 1.3));
        std::cout << "[AutoExtend] estimated build needs " << min_needed_sec
                  << "s; extending time_limit from " << time_limit_sec
                  << "s to " << new_limit << "s" << std::endl;
        time_limit_sec = new_limit;
    }
    auto hard_deadline = t0 + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(time_limit_sec));
    std::vector<Candidate> candidates;
    std::vector<int> flat_cands, offsets;
    std::vector<int> cand_flat_targets, cand_offsets;
    std::vector<uint64_t> all_cand_masks;
    std::vector<std::vector<int>> fallback_result;
    uint64_t node_count = 0;
    
    std::cout << "[Step 1] Building Optimized Model..." << std::endl;
    bool enable_dominance_pruning = (s != j);
    int target_count = build_optimized_problem(pool, k, j, s, candidates, flat_cands, offsets, &cand_flat_targets, &cand_offsets, &all_cand_masks, enable_dominance_pruning);
    auto t1 = std::chrono::steady_clock::now();
    auto ms = [](const std::chrono::steady_clock::time_point& a, const std::chrono::steady_clock::time_point& b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    if (std::chrono::steady_clock::now() >= hard_deadline) {
        std::cout << "[HardTimeLimit] build exceeded budget; returning empty." << std::endl;
        std::cout << "[Timing] build=" << ms(t0, t1) << "ms (over budget)" << std::endl;
        return {{}, true};
    }
    if (target_count <= 0 || candidates.empty()) {
        std::cout << "[Timing] build=" << ms(t0, t1) << "ms  greedy=0ms  dfs=0ms" << std::endl;
        std::cout << "[Stats]  targets=" << target_count << "  cands=" << candidates.size() << "  dfs_nodes=0" << std::endl;
        return {{}, false};
    }

    int static_lb = 0;
    if (s == j) {
        static_lb = schonheim_lower_bound((int)pool.size(), k, j);
        if (static_lb > 0) {
            std::cout << "[LB] Schonheim lower bound = " << static_lb << std::endl;
        }
    }

    // 先跑快速贪心拿到一个可行解，作为 DFS 初始上界和超时兜底答案
    int greedy_restarts = (s == j) ? 4 : 1;
    int greedy_topk = (s == j) ? 3 : 1;
    {
        auto seed = greedy_fast_set_cover(pool, k, j, s, greedy_topk, greedy_restarts, 123456789ULL, &hard_deadline);
        fallback_result = std::get<0>(seed);
        auto t2 = std::chrono::steady_clock::now();
        if (std::chrono::steady_clock::now() >= hard_deadline) {
            std::cout << "[HardTimeLimit] greedy seed exhausted budget; returning seed size=" << fallback_result.size() << std::endl;
            std::cout << "[Timing] build=" << ms(t0, t1) << "ms  greedy=" << ms(t1, t2) << "ms  dfs=0ms  ils=0ms" << std::endl;
            return {fallback_result, true};
        }
        if (static_lb > 0 && !fallback_result.empty()) {
            double ratio = (double)fallback_result.size() / (double)static_lb;
            std::cout << "[LB] seed/LB ratio = " << ratio << " (" << fallback_result.size() << "/" << static_lb << ")" << std::endl;
            if ((int)fallback_result.size() <= static_lb) {
                std::cout << "[LB] Seed hits lower bound; skip DFS." << std::endl;
                std::cout << "[Timing] build=" << ms(t0, t1) << "ms  greedy=" << ms(t1, t2) << "ms  dfs=0ms" << std::endl;
                std::cout << "[Stats]  targets=" << target_count << "  cands=" << candidates.size() << "  dfs_nodes=0" << std::endl;
                return {fallback_result, false};
            }
        }
    }
    auto t2 = std::chrono::steady_clock::now();
    
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
    std::vector<int> gain_cnt(global_max_gain_upper + 2, 0);
    for (int g : gains) gain_cnt[g]++;
    int cur_max_gain = global_max_gain_upper;
    for (int tid = 0; tid < target_count; ++tid) {
        int deg = 0;
        for (int p = offsets[tid]; p < offsets[tid + 1]; ++p) {
            if (gains[flat_cands[p]] > 0) deg++;
        }
        live_deg[tid] = deg;
    }
    std::vector<int> gain_rollback;
    gain_rollback.reserve((size_t)target_count * 8);
    std::vector<int> uncovered_list(target_count, 0);
    std::iota(uncovered_list.begin(), uncovered_list.end(), 0);
    std::vector<int> uncovered_pos(target_count, -1);
    for (int tid = 0; tid < target_count; ++tid) uncovered_pos[tid] = tid;
    std::vector<int> uncovered_rollback;
    uncovered_rollback.reserve((size_t)target_count * 2);
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
    std::vector<uint64_t> backup_memory(std::max<size_t>(1, (size_t)max_depth * (size_t)num_chunks), 0);

    auto start_time = std::chrono::steady_clock::now();
    double remaining_sec = std::chrono::duration<double>(hard_deadline - start_time).count();
    if (remaining_sec <= 0.5) {
        auto t3 = std::chrono::steady_clock::now();
        std::cout << "[HardTimeLimit] no time for DFS; returning seed size=" << fallback_result.size() << std::endl;
        std::cout << "[Timing] build=" << ms(t0, t1) << "ms  greedy=" << ms(t1, t2) << "ms  dfs=0ms  ils=0ms  total=" << ms(t0, t3) << "ms" << std::endl;
        return {fallback_result, true};
    }
    double effective_time_limit_sec = remaining_sec;
    double dfs_time_budget_sec = std::max(0.2, effective_time_limit_sec * 0.40);
    auto dfs_duration = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(dfs_time_budget_sec));
    auto deadline = start_time + dfs_duration;
    bool aborted = false;
    uint64_t timeout_check_every = (uint64_t)std::max(1, timeout_check_every_nodes);
    
    bool enable_lb2 = (s != j);
    bool enable_lb3 = true;
    uint64_t last_improvement_node = 0;
    uint64_t no_improve_node_cap = 5000000ULL;

    // 若限制深度小于当前上界，收紧上界避免无效探索
    if (best_ub > max_depth) {
        best_ub = max_depth;
    }

    // 预选根节点候选，支持根层并行
    int root_best_bit = -1;
    int root_min_deg = std::numeric_limits<int>::max();
    for (int tid : uncovered_list) {
        int deg = live_deg[tid];
        if (deg <= 0) {
            root_best_bit = -1;
            break;
        }
        if (deg < root_min_deg) {
            root_min_deg = deg;
            root_best_bit = tid;
            if (deg == 1) break;
        }
    }

    std::vector<DFSCandEntry> root_cands;
    if (root_best_bit >= 0) {
        root_cands.reserve(offsets[root_best_bit + 1] - offsets[root_best_bit]);
        for (int i = offsets[root_best_bit]; i < offsets[root_best_bit + 1]; ++i) {
            int ci = flat_cands[i];
            int g = gains[ci];
            if (g > 0) root_cands.push_back({ci, g});
        }
        std::sort(root_cands.begin(), root_cands.end(), [](const DFSCandEntry& a, const DFSCandEntry& b) {
            return a.gain > b.gain;
        });
        if (branch_cap_per_level > 0 && (int)root_cands.size() > branch_cap_per_level) {
            root_cands.resize(branch_cap_per_level);
        }
    }

    unsigned int hw_threads = std::thread::hardware_concurrency();
    bool enable_root_parallel = (hw_threads > 1 && root_cands.size() >= 2);
    if (!enable_root_parallel) {
        dfs_ultra(candidates, flat_cands.data(), offsets.data(),
                  covered.data(), all_mask.data(), num_chunks,
                  best_ub, path, best_path, backup_memory, 0, max_depth, target_count, global_max_gain_upper, branch_cap_per_level,
                  all_cand_masks.data(),
                  cand_flat_targets.data(), cand_offsets.data(),
                  gains, gain_cnt, cur_max_gain, live_deg, gain_rollback,
                  uncovered_list, uncovered_pos, uncovered_rollback, local_pool, max_degree,
                  blocked_ver, blocked_cur_ver,
                  enable_lb2, enable_lb3, last_improvement_node, no_improve_node_cap,
                  timeout_check_every, node_count, deadline, aborted);
    } else {
        int worker_count = std::min<int>((int)root_cands.size(), (int)hw_threads);
        // debug-only: 强制 worker 数，未来排查并发问题用。
        if (const char* env_w = std::getenv("COVER_DEBUG_WORKERS")) {
            int forced = std::atoi(env_w);
            if (forced > 0) worker_count = std::max(1, std::min(worker_count, forced));
        }
        std::atomic<int> shared_best_ub(best_ub);
        std::atomic<bool> any_aborted(false);
        std::atomic<uint64_t> shared_node_count(0);
        std::mutex best_path_mtx;
        std::vector<int> shared_best_path = best_path;
        std::vector<std::thread> workers;
        workers.reserve(worker_count);

        auto run_branch = [&](int widx) {
            try {
                if (cover_debug_trace_enabled()) {
                    std::cerr << "[trace][w" << widx << "] start worker_count=" << worker_count
                              << " root_cands=" << root_cands.size() << "\n";
                    std::cerr.flush();
                }
                bool local_thread_aborted = false;
                for (int bi = widx; bi < (int)root_cands.size(); bi += worker_count) {
                    if (std::chrono::steady_clock::now() > deadline) {
                        local_thread_aborted = true;
                        break;
                    }
                    int ci = root_cands[bi].ci;
                    int local_best_ub = std::min(shared_best_ub.load(std::memory_order_relaxed), max_depth);
                    if (local_best_ub <= 1) break;
                    if (cover_debug_trace_enabled()) {
                        std::cerr << "[trace][w" << widx << "] bi=" << bi
                                  << " ci=" << ci
                                  << " local_best_ub=" << local_best_ub << "\n";
                        std::cerr.flush();
                    }

                    std::vector<uint64_t> covered_l(num_chunks, 0);
                    std::vector<int> gains_l = gains;
                    std::vector<int> gain_cnt_l = gain_cnt;
                    int cur_max_gain_l = cur_max_gain;
                    std::vector<int> live_deg_l = live_deg;
                    std::vector<int> uncovered_list_l = uncovered_list;
                    std::vector<int> uncovered_pos_l = uncovered_pos;
                    std::vector<int> gain_rollback_l;
                    gain_rollback_l.reserve((size_t)target_count * 8);
                    std::vector<int> uncovered_rollback_l;
                    uncovered_rollback_l.reserve((size_t)target_count * 2);
                    std::vector<DFSCandEntry> local_pool_l;
                    local_pool_l.reserve(std::max<size_t>(1, initial_depth * (size_t)max_degree));
                    std::vector<int> blocked_ver_l(target_count, 0);
                    int blocked_cur_ver_l = 0;
                    std::vector<uint64_t> backup_memory_l(std::max<size_t>(1, (size_t)max_depth * (size_t)num_chunks), 0);
                    std::vector<int> path_l;
                    std::vector<int> best_path_l;
                    uint64_t node_count_l = 0;
                    bool aborted_l = false;
                    uint64_t last_improvement_node_l = 0;

                    const uint64_t* c_mask = all_cand_masks.data() + (size_t)ci * (size_t)num_chunks;
                    bool any_new = false;
                    for (int c = 0; c < num_chunks; ++c) {
                        uint64_t add_bits = c_mask[c] & ~covered_l[c];
                        if (add_bits) any_new = true;
                        covered_l[c] |= c_mask[c];
                    }
                    if (!any_new) continue;

                    for (int c = 0; c < num_chunks; ++c) {
                        uint64_t newly = c_mask[c];
                        while (newly) {
                            int local = __builtin_ctzll(newly);
                            int tid = c * 64 + local;
                            if (tid < target_count && uncovered_pos_l[tid] != -1) {
                                int pos = uncovered_pos_l[tid];
                                int last_tid = uncovered_list_l.back();
                                std::swap(uncovered_list_l[pos], uncovered_list_l.back());
                                uncovered_list_l.pop_back();
                                uncovered_pos_l[last_tid] = pos;
                                uncovered_pos_l[tid] = -1;
                                uncovered_rollback_l.push_back(tid);
                            }
                            for (int p = offsets[tid]; p < offsets[tid + 1]; ++p) {
                                int cj = flat_cands[p];
                                if (gains_l[cj] > 0) {
                                    int old_gain = gains_l[cj];
                                    gain_cnt_l[old_gain]--;
                                    gains_l[cj]--;
                                    gain_cnt_l[old_gain - 1]++;
                                    if (old_gain == 1) {
                                        for (int q = cand_offsets[cj]; q < cand_offsets[cj + 1]; ++q) {
                                            int t2 = cand_flat_targets[q];
                                            live_deg_l[t2]--;
                                        }
                                    }
                                }
                            }
                            newly &= newly - 1;
                        }
                    }
                    while (cur_max_gain_l > 0 && gain_cnt_l[cur_max_gain_l] == 0) cur_max_gain_l--;

                    path_l.push_back(ci);
                    if (cover_debug_trace_enabled()) {
                        std::cerr << "[trace][w" << widx << "] before_dfs ci=" << ci
                                  << " path_l=" << path_l.size() << "\n";
                        std::cerr.flush();
                    }
                    dfs_ultra(candidates, flat_cands.data(), offsets.data(),
                              covered_l.data(), all_mask.data(), num_chunks,
                              local_best_ub, path_l, best_path_l, backup_memory_l, 1, max_depth, target_count, global_max_gain_upper, branch_cap_per_level,
                              all_cand_masks.data(),
                              cand_flat_targets.data(), cand_offsets.data(),
                              gains_l, gain_cnt_l, cur_max_gain_l, live_deg_l, gain_rollback_l,
                              uncovered_list_l, uncovered_pos_l, uncovered_rollback_l, local_pool_l, max_degree,
                              blocked_ver_l, blocked_cur_ver_l,
                              enable_lb2, enable_lb3, last_improvement_node_l, no_improve_node_cap,
                              timeout_check_every, node_count_l, deadline, aborted_l);
                    if (cover_debug_trace_enabled()) {
                        std::cerr << "[trace][w" << widx << "] after_dfs ci=" << ci
                                  << " node_count_l=" << node_count_l
                                  << " aborted_l=" << aborted_l << "\n";
                        std::cerr.flush();
                    }
                    shared_node_count.fetch_add(node_count_l, std::memory_order_relaxed);

                    if (aborted_l) local_thread_aborted = true;
                    if (!best_path_l.empty()) {
                        int now_best = shared_best_ub.load(std::memory_order_relaxed);
                        if (local_best_ub < now_best) {
                            std::lock_guard<std::mutex> lk(best_path_mtx);
                            now_best = shared_best_ub.load(std::memory_order_relaxed);
                            if (local_best_ub < now_best) {
                                shared_best_ub.store(local_best_ub, std::memory_order_relaxed);
                                shared_best_path = best_path_l;
                            }
                        }
                    }
                }
                if (local_thread_aborted) any_aborted.store(true, std::memory_order_relaxed);
            } catch (const std::exception& e) {
                std::cerr << "[Worker " << widx << "] exception: " << e.what() << std::endl;
                any_aborted.store(true, std::memory_order_relaxed);
            } catch (...) {
                std::cerr << "[Worker " << widx << "] unknown exception" << std::endl;
                any_aborted.store(true, std::memory_order_relaxed);
            }
        };

        for (int t = 0; t < worker_count; ++t) {
            workers.emplace_back(run_branch, t);
        }
        for (auto& th : workers) th.join();

        best_ub = shared_best_ub.load(std::memory_order_relaxed);
        best_path = std::move(shared_best_path);
        aborted = any_aborted.load(std::memory_order_relaxed) || (std::chrono::steady_clock::now() > deadline);
        node_count += shared_node_count.load(std::memory_order_relaxed);
    }

    if (aborted) {
        std::cout << "DFS Ultra aborted due to time limit (" << dfs_time_budget_sec << "s)." << std::endl;
    }
    auto t3 = std::chrono::steady_clock::now();

    // ILS 接管剩余预算：当 DFS 未给出可证最优且仍有时间时，继续做迭代扰动改进。
    std::vector<int> current_ids = !best_path.empty() ? best_path : std::vector<int>{};
    if (current_ids.empty() && !fallback_result.empty()) {
        std::unordered_map<std::string, int> key_to_id;
        key_to_id.reserve(candidates.size() * 2 + 1);
        for (int ci = 0; ci < (int)candidates.size(); ++ci) {
            auto m = candidates[ci].members;
            std::sort(m.begin(), m.end());
            key_to_id.emplace(members_key(m), ci);
        }
        for (const auto& sset : fallback_result) {
            auto it = key_to_id.find(members_key(sset));
            if (it != key_to_id.end()) current_ids.push_back(it->second);
        }
    }

    auto global_deadline = hard_deadline;
    auto now_after_dfs = std::chrono::steady_clock::now();
    double ils_ms = 0.0;
    if (!current_ids.empty() && now_after_dfs + std::chrono::milliseconds(400) < global_deadline) {
        auto ils_start = std::chrono::steady_clock::now();
        unsigned int hw = std::thread::hardware_concurrency();
        int ils_threads = std::max(1, std::min((int)hw, 4));
        std::vector<double> destroy_hints = {0.10, 0.25, 0.40, 0.55};
        std::vector<std::thread> ils_workers;
        ils_workers.reserve(ils_threads);
        std::mutex ils_best_mtx;
        std::vector<int> ils_best = current_ids;
        int ils_best_sz = (int)current_ids.size();

        auto worker_fn = [&](int tid) {
            std::mt19937_64 ils_rng(987654321ULL + (uint64_t)tid * 7919ULL);
            std::vector<int> my_start;
            {
                std::vector<uint64_t> covered0(num_chunks, 0);
                std::vector<char> used0(candidates.size(), 0);
                greedy_complete_ids(my_start, covered0, used0, candidates, all_cand_masks, all_mask, num_chunks, ils_rng, 5);
                if (!my_start.empty()) {
                    local_search_swap(my_start, candidates, all_cand_masks, num_chunks, target_count, 30);
                }
            }
            int fb_threshold = std::max(8, (int)current_ids.size() / 5);
            if (my_start.empty() || (int)my_start.size() > (int)current_ids.size() + fb_threshold) {
                my_start = current_ids;
            }
            double hint = destroy_hints[tid % (int)destroy_hints.size()];
            auto improved = ils_improve_ids(my_start, candidates, all_cand_masks, all_mask, num_chunks, target_count, global_deadline, ils_rng, hint);
            if (!improved.empty()) {
                int sz = (int)improved.size();
                if (sz < ils_best_sz) {
                    std::lock_guard<std::mutex> lk(ils_best_mtx);
                    if (sz < ils_best_sz) {
                        ils_best = std::move(improved);
                        ils_best_sz = sz;
                    }
                }
            }
        };

        for (int t = 0; t < ils_threads; ++t) ils_workers.emplace_back(worker_fn, t);
        for (auto& th : ils_workers) th.join();

        auto ils_end = std::chrono::steady_clock::now();
        ils_ms = ms(ils_start, ils_end);
        if (!ils_best.empty() && (best_path.empty() || ils_best.size() < best_path.size())) {
            best_path = std::move(ils_best);
            best_ub = (int)best_path.size();
        }
    }

    auto t4 = std::chrono::steady_clock::now();
    std::cout << "[Timing] build=" << ms(t0, t1) << "ms  greedy=" << ms(t1, t2) << "ms  dfs=" << ms(t2, t3) << "ms  ils=" << ils_ms << "ms  total=" << ms(t0, t4) << "ms" << std::endl;
    std::cout << "[Stats]  targets=" << target_count << "  cands=" << candidates.size() << "  dfs_nodes=" << node_count << std::endl;

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
