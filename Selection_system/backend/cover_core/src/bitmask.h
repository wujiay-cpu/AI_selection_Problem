#pragma once
#include <vector>
#include <cstdint>
#include <cstring>
#include <string>

// ============================================================
//  MSVC 兼容层：用 Windows 内建函数模拟 GCC/Clang 内建
// ============================================================
#ifdef _MSC_VER
#include <intrin.h>
#pragma intrinsic(_BitScanForward64, _BitScanReverse64, __popcnt64)
static inline int __builtin_ctzll(unsigned long long x) {
    unsigned long idx = 0;
    _BitScanForward64(&idx, x);
    return (int)idx;
}
static inline int __builtin_clzll(unsigned long long x) {
    unsigned long idx = 0;
    _BitScanReverse64(&idx, x);
    return 63 - (int)idx;
}
static inline int __builtin_popcountll(unsigned long long x) {
    return (int)__popcnt64(x);
}
#endif
#include <sstream>

// ============================================================
//  ChunkedMask
//  用 vector<uint64_t> 表示任意宽度的位掩码。
//  target_count 最大约 3 万，需要最多 ~470 个 uint64。
// ============================================================
struct ChunkedMask {
    std::vector<uint64_t> chunks;   // chunks[i] 存第 i*64 .. i*64+63 位

    ChunkedMask() = default;
    ChunkedMask(int n_chunks) : chunks(n_chunks, 0ULL) {}

    // num_chunks 统一从 chunks.size() 取，消除两套计数不一致的风险
    inline int num_chunks() const { return (int)chunks.size(); }

    // 设置第 bit_idx 位
    inline void set_bit(int bit_idx) {
        chunks[bit_idx >> 6] |= (1ULL << (bit_idx & 63));
    }

    // 读取第 bit_idx 位
    inline bool get_bit(int bit_idx) const {
        return (chunks[bit_idx >> 6] >> (bit_idx & 63)) & 1ULL;
    }
    
    // 兼容别名
    inline bool test_bit(int bit_idx) const {
        return get_bit(bit_idx);
    }

    // popcount（所有块之和）
    inline int popcount() const {
        int cnt = 0;
        for (auto v : chunks) cnt += __builtin_popcountll(v);
        return cnt;
    }

    // 是否全零
    inline bool is_zero() const {
        for (auto v : chunks) if (v) return false;
        return true;
    }

    // 是否等于另一个掩码
    inline bool equals(const ChunkedMask& o) const {
        return chunks == o.chunks;
    }

    // this |= other
    inline void or_inplace(const ChunkedMask& o) {
        int n = (int)chunks.size();
        for (int i = 0; i < n; ++i) chunks[i] |= o.chunks[i];
    }

    // 记录变更的结构体，用于 rollback
    struct Change {
        int chunk_idx;
        uint64_t old_val;
    };

    // 带有 undo 记录的 or_inplace（消除 DFS 中的堆分配拷贝开销）
    inline void or_with_rollback(const ChunkedMask& o, std::vector<Change>& changes) {
        int n = (int)chunks.size();
        for (int i = 0; i < n; ++i) {
            if (o.chunks[i] & ~chunks[i]) {
                changes.push_back({i, chunks[i]});
                chunks[i] |= o.chunks[i];
            }
        }
    }

    // 恢复到指定的 changes size
    inline void rollback(std::vector<Change>& changes, size_t target_size) {
        while (changes.size() > target_size) {
            auto& ch = changes.back();
            chunks[ch.chunk_idx] = ch.old_val;
            changes.pop_back();
        }
    }

    // this &= ~other
    inline void andnot_inplace(const ChunkedMask& o) {
        int n = (int)chunks.size();
        for (int i = 0; i < n; ++i) chunks[i] &= ~o.chunks[i];
    }

    // result = this & ~covered 的 popcount
    inline int gain_over(const ChunkedMask& covered) const {
        int cnt = 0;
        int n = (int)chunks.size();
        for (int i = 0; i < n; ++i)
            cnt += __builtin_popcountll(chunks[i] & ~covered.chunks[i]);
        return cnt;
    }

    // result = this & ~covered 的加权和
    inline double weighted_gain_over(const ChunkedMask& covered,
                                     const std::vector<double>& weights) const {
        double score = 0.0;
        int n = (int)chunks.size();
        for (int chunk_idx = 0; chunk_idx < n; ++chunk_idx) {
            uint64_t bits = chunks[chunk_idx] & ~covered.chunks[chunk_idx];
            while (bits) {
                score += weights[chunk_idx * 64 + __builtin_ctzll(bits)];
                bits &= bits - 1;
            }
        }
        return score;
    }

    // 检查 this 是否包含 other 的所有位
    inline bool covers(const ChunkedMask& other) const {
        int n = (int)chunks.size();
        for (int i = 0; i < n; ++i)
            if (other.chunks[i] & ~chunks[i]) return false;
        return true;
    }
    
    // 检查 this 是否与 other 有交集
    inline bool intersects(const ChunkedMask& other) const {
        int n = (int)chunks.size();
        for (int i = 0; i < n; ++i)
            if (chunks[i] & other.chunks[i]) return true;
        return false;
    }

    // OR of two masks → new mask（尽量用 or_with_rollback 代替，避免堆分配）
    ChunkedMask or_with(const ChunkedMask& o) const {
        int n = (int)chunks.size();
        ChunkedMask res(n);
        for (int i = 0; i < n; ++i) res.chunks[i] = chunks[i] | o.chunks[i];
        return res;
    }

    // AND-NOT: result = this & ~o
    ChunkedMask andnot(const ChunkedMask& o) const {
        int n = (int)chunks.size();
        ChunkedMask res(n);
        for (int i = 0; i < n; ++i) res.chunks[i] = chunks[i] & ~o.chunks[i];
        return res;
    }
};

// ============================================================
//  Candidate：一个候选组合的所有信息
// ============================================================
struct Candidate {
    std::vector<int> members;        // 组合中的元素（原始池中的值）
    ChunkedMask mask;                // 覆盖哪些 target
    int bc = 0;                      // mask 的 popcount
    double weighted_score = 0.0;     // 全量加权得分（用于 tie-break）

    // 预存所有置位的 bit indices（在 build_targets_and_candidates 中填充）。
    // 用于 coverage_count 更新、synergy_penalty 等需要遍历 mask 所有置位的热路径，
    // 避免反复执行 chunk 循环 + ctzll 拆包，直接 for(auto b : bits) 即可。
    std::vector<uint16_t> bits;
};