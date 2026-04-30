#pragma once
#include "bitmask.h"
#include <vector>
#include <queue>
#include <tuple>
#include <algorithm>
#include <numeric>
#include <unordered_map>
#include <unordered_set>
#include <cmath>
#include <functional>
#include <iostream>

// ============================================================
//  组合生成器
// ============================================================
static void gen_combinations(const std::vector<int>& pool, int r,
                              std::function<void(const std::vector<int>&)> callback) {
    int n = (int)pool.size();
    if (r > n || r <= 0) return;
    std::vector<int> idx(r);
    std::iota(idx.begin(), idx.end(), 0);
    while (true) {
        std::vector<int> combo(r);
        for (int i = 0; i < r; ++i) combo[i] = pool[idx[i]];
        callback(combo);
        int i = r - 1;
        while (i >= 0 && idx[i] == n - r + i) --i;
        if (i < 0) break;
        ++idx[i];
        for (int j = i + 1; j < r; ++j) idx[j] = idx[j-1] + 1;
    }
}

// 整数打包 key：子集元素值 ≤ 999，最多 6 个元素，用 *1000 进位
struct U64Hash {
    size_t operator()(uint64_t x) const noexcept {
        x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
        x ^= x >> 33; return (size_t)x;
    }
};
static inline uint64_t make_int_key(const std::vector<int>& v) {
    uint64_t key = 0;
    for (int x : v) key = key * 1000 + (uint64_t)x;
    return key;
}

// ============================================================
//  build_targets_and_candidates
// ============================================================
static int build_targets_and_candidates(
    const std::vector<int>& pool,
    int k, int j, int s,
    std::vector<Candidate>& out_candidates)
{
    // Step1: 枚举所有 j-target
    std::vector<std::vector<int>> all_targets;
    gen_combinations(pool, j, [&](const std::vector<int>& t) {
        all_targets.push_back(t);
    });
    int target_count = (int)all_targets.size();
    int num_chunks   = (target_count + 63) / 64;

    // Step2: 建 sub → vector<target_idx> 倒排（只存索引，不存 ChunkedMask）
    // 内存: C(n,j)*C(j,s) 个 int，n=21,j=4,s=3: 5985*4=~24K 条目，极小
    std::unordered_map<uint64_t, std::vector<int>, U64Hash> sub_to_tidxs;
    sub_to_tidxs.reserve(target_count * j * 2);

    for (int ti = 0; ti < target_count; ++ti) {
        gen_combinations(all_targets[ti], s, [&](const std::vector<int>& sub) {
            sub_to_tidxs[make_int_key(sub)].push_back(ti);
        });
    }

    // Step3: 枚举所有 k-候选，构建 cover mask
    // 去重：mask 用 string(chunk bytes) 作为 key
    // Step3: 枚举候选，直接用 uint64 数组填充 cover mask
    // 用 cover_buf + fill(0) 比 tidxs+sort+unique 快（避免 54264次 O(360log360) sort）
    struct RawCand {
        std::vector<uint64_t> mask_chunks; // 已填好的 cover mask
        std::vector<int>      members;
        int bc = 0;
    };
    std::vector<RawCand> raw;
    raw.reserve(65536);

    // cover_buf 复用，每次候选用 memset 清零（752字节 << sort(360元素)）
    std::vector<uint64_t> cover_buf(num_chunks, 0ULL);

    gen_combinations(pool, k, [&](const std::vector<int>& cand) {
        std::fill(cover_buf.begin(), cover_buf.end(), 0ULL);
        int bc = 0;
        gen_combinations(cand, s, [&](const std::vector<int>& sub) {
            auto it = sub_to_tidxs.find(make_int_key(sub));
            if (it == sub_to_tidxs.end()) return;
            for (int ti : it->second) {
                cover_buf[ti >> 6] |= (1ULL << (ti & 63));
            }
        });
        for (int ci = 0; ci < num_chunks; ++ci) bc += __builtin_popcountll(cover_buf[ci]);
        if (bc == 0) return;
        raw.push_back({cover_buf, cand, bc});
    });

    // Step4: 计算 target_freq（bit 枚举）和加权分，构建 Candidate 列表
    int C = (int)raw.size();
    std::vector<int> target_freq(target_count, 0);

    for (auto& rc : raw) {
        for (int ci = 0; ci < num_chunks; ++ci) {
            uint64_t bits = rc.mask_chunks[ci];
            while (bits) {
                target_freq[ci * 64 + __builtin_ctzll(bits)]++;
                bits &= bits - 1;
            }
        }
    }

    std::vector<double> target_weight(target_count);
    for (int i = 0; i < target_count; ++i)
        target_weight[i] = 1.0 / std::max(target_freq[i], 1);

    out_candidates.resize(C);
    for (int i = 0; i < C; ++i) {
        out_candidates[i].members        = std::move(raw[i].members);
        out_candidates[i].mask           = ChunkedMask(num_chunks);
        out_candidates[i].mask.chunks    = std::move(raw[i].mask_chunks);
        out_candidates[i].bc             = raw[i].bc;
        double score = 0.0;
        for (int ci = 0; ci < num_chunks; ++ci) {
            uint64_t bits = out_candidates[i].mask.chunks[ci];
            while (bits) {
                score += target_weight[ci * 64 + __builtin_ctzll(bits)];
                bits &= bits - 1;
            }
        }
        out_candidates[i].weighted_score = score;
    }

    // 按 bc 降序
    std::sort(out_candidates.begin(), out_candidates.end(),
              [](const Candidate& a, const Candidate& b){ return a.bc > b.bc; });

    return target_count;
}

// ============================================================
//  run_greedy_phase（加权贪心，静态 tie-break）
// ============================================================
static void run_greedy_phase(
    const std::vector<int>& indexes,
    const std::vector<Candidate>& candidates,
    ChunkedMask& covered,
    int& remaining,
    std::vector<int>& selected_ids)
{
    if (remaining <= 0 || indexes.empty()) return;

    using HeapElem = std::tuple<int, double, int>;
    auto cmp = [](const HeapElem& a, const HeapElem& b) {
        if (std::get<0>(a) != std::get<0>(b)) return std::get<0>(a) > std::get<0>(b);
        return std::get<1>(a) > std::get<1>(b);
    };
    std::priority_queue<HeapElem, std::vector<HeapElem>, decltype(cmp)> heap(cmp);

    std::vector<bool> active(candidates.size(), false);
    for (int i : indexes) {
        active[i] = true;
        heap.push({-candidates[i].bc, -candidates[i].weighted_score, i});
    }

    while (remaining > 0 && !heap.empty()) {
        auto [neg_gain, neg_ws, i] = heap.top(); heap.pop();
        if (!active[i]) continue;

        int real_gain = candidates[i].mask.gain_over(covered);
        if (real_gain == 0) { active[i] = false; continue; }

        if (real_gain != -neg_gain) {
            heap.push({-real_gain, -candidates[i].weighted_score, i});
            continue;
        }

        active[i] = false;
        selected_ids.push_back(i);
        covered.or_inplace(candidates[i].mask);
        remaining -= real_gain;
    }
}

// ============================================================
//  prune_redundant（前缀/后缀掩码加速）
// ============================================================
static void prune_redundant(
    std::vector<int>& selected_ids,
    const std::vector<Candidate>& candidates,
    int num_chunks)
{
    bool changed = true;
    while (changed) {
        changed = false;
        int m = (int)selected_ids.size();
        if (m <= 1) break;

        std::vector<ChunkedMask> prefix(m + 1, ChunkedMask(num_chunks));
        std::vector<ChunkedMask> suffix(m + 1, ChunkedMask(num_chunks));
        for (int i = 0; i < m; ++i)
            prefix[i+1] = prefix[i].or_with(candidates[selected_ids[i]].mask);
        for (int i = m - 1; i >= 0; --i)
            suffix[i] = suffix[i+1].or_with(candidates[selected_ids[i]].mask);

        ChunkedMask& full = prefix[m];
        for (int i = 0; i < m; ++i) {
            ChunkedMask rest = prefix[i].or_with(suffix[i+1]);
            if (rest.equals(full)) {
                selected_ids.erase(selected_ids.begin() + i);
                changed = true;
                break;
            }
        }
    }
}

// ============================================================
//  try_replace_2for1（2-opt 替换，倒排索引加速）
// ============================================================
static void try_replace_2for1(
    std::vector<int>& selected_ids,
    const std::vector<Candidate>& candidates,
    const ChunkedMask& all_mask,
    int num_chunks)
{
    int total_bits = num_chunks * 64;
    // 预建倒排：bit_idx → 覆盖该 target 的候选列表（按 bc 降序已保证）
    std::vector<std::vector<int>> bit_to_cands(total_bits);
    for (int ci = 0; ci < (int)candidates.size(); ++ci) {
        for (int chunk = 0; chunk < num_chunks; ++chunk) {
            uint64_t bits = candidates[ci].mask.chunks[chunk];
            while (bits) {
                bit_to_cands[chunk * 64 + __builtin_ctzll(bits)].push_back(ci);
                bits &= bits - 1;
            }
        }
    }

    bool changed = true;
    while (changed) {
        changed = false;
        int m = (int)selected_ids.size();
        if (m < 2) break;

        std::unordered_set<int> selected_set(selected_ids.begin(), selected_ids.end());
        bool found = false;

        for (int ii = 0; ii < m && !found; ++ii) {
            for (int jj = ii + 1; jj < m && !found; ++jj) {
                ChunkedMask rest(num_chunks);
                for (int kk = 0; kk < m; ++kk) {
                    if (kk == ii || kk == jj) continue;
                    rest.or_inplace(candidates[selected_ids[kk]].mask);
                }
                ChunkedMask gap(num_chunks);
                bool gap_empty = true;
                for (int ci = 0; ci < num_chunks; ++ci) {
                    gap.chunks[ci] = all_mask.chunks[ci] & ~rest.chunks[ci];
                    if (gap.chunks[ci]) gap_empty = false;
                }

                if (gap_empty) {
                    std::vector<int> new_ids;
                    for (int kk = 0; kk < m; ++kk)
                        if (kk != ii && kk != jj) new_ids.push_back(selected_ids[kk]);
                    selected_ids = std::move(new_ids);
                    changed = true; found = true; break;
                }

                // 用倒排：找 gap 中频率最低的 bit，用其候选列表做过滤
                // 选 bit_to_cands 最小的那个 bit（最稀缺），大幅减少候选数
                int rarest_bit = -1;
                size_t min_count = SIZE_MAX;
                for (int ci = 0; ci < num_chunks; ++ci) {
                    uint64_t bits = gap.chunks[ci];
                    while (bits) {
                        int bp = ci * 64 + __builtin_ctzll(bits);
                        if (bit_to_cands[bp].size() < min_count) {
                            min_count = bit_to_cands[bp].size();
                            rarest_bit = bp;
                        }
                        bits &= bits - 1;
                    }
                }

                if (rarest_bit < 0) continue;

                // 只遍历覆盖了最稀缺bit的候选（远少于全部 C 个候选）
                for (int ci : bit_to_cands[rarest_bit]) {
                    if (selected_set.count(ci)) continue;
                    if (candidates[ci].mask.covers(gap)) {
                        std::vector<int> new_ids;
                        for (int kk = 0; kk < m; ++kk)
                            if (kk != ii && kk != jj) new_ids.push_back(selected_ids[kk]);
                        new_ids.push_back(ci);
                        selected_ids = std::move(new_ids);
                        changed = true; found = true; break;
                    }
                }
            }
        }
    }
}

// ============================================================
//  greedy_set_cover（主入口）
// ============================================================
inline std::vector<std::vector<int>> greedy_set_cover(
    const std::vector<int>& pool, int k, int j, int s)
{
    std::vector<Candidate> candidates;
    int target_count = build_targets_and_candidates(pool, k, j, s, candidates);
    int num_chunks   = (target_count + 63) / 64;
    int C            = (int)candidates.size();

    std::cout << "开始计算(C++)... 共有 " << target_count
              << " 个目标，" << C << " 个候选组" << std::endl;

    ChunkedMask all_mask(num_chunks);
    for (int i = 0; i < target_count; ++i) all_mask.set_bit(i);

    std::vector<int> phase1, phase2;
    if ((int)pool.size() >= 21 && C > 20000) {
        std::vector<std::pair<int,int>> bc_idx(C);
        for (int i = 0; i < C; ++i) bc_idx[i] = {candidates[i].bc, i};
        std::sort(bc_idx.begin(), bc_idx.end(), std::greater<>());
        std::unordered_set<int> top_set;
        for (int i = 0; i < 20000 && i < C; ++i) top_set.insert(bc_idx[i].second);
        for (int i = 0; i < C; ++i) {
            if (top_set.count(i)) phase1.push_back(i); else phase2.push_back(i);
        }
    } else {
        phase1.resize(C); std::iota(phase1.begin(), phase1.end(), 0);
    }

    ChunkedMask covered(num_chunks);
    int remaining = target_count;
    std::vector<int> selected_ids;

    run_greedy_phase(phase1, candidates, covered, remaining, selected_ids);
    if (remaining > 0)
        run_greedy_phase(phase2, candidates, covered, remaining, selected_ids);

    if (remaining != 0) std::cerr << "警告：无法完全覆盖所有目标" << std::endl;

    prune_redundant(selected_ids, candidates, num_chunks);

    int before_2opt = (int)selected_ids.size();
    try_replace_2for1(selected_ids, candidates, all_mask, num_chunks);
    prune_redundant(selected_ids, candidates, num_chunks);
    int after_2opt = (int)selected_ids.size();
    if (after_2opt < before_2opt)
        std::cout << "2-opt 替换优化：" << before_2opt << " -> " << after_2opt << " 组" << std::endl;

    std::vector<std::vector<int>> result;
    result.reserve(selected_ids.size());
    for (int id : selected_ids) {
        auto members = candidates[id].members;
        std::sort(members.begin(), members.end());
        result.push_back(members);
    }
    return result;
}
