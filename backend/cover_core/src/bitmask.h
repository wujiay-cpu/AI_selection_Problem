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
#pragma intrinsic(_BitScanForward64, __popcnt64)
static inline int __builtin_ctzll(unsigned long long x) {
    unsigned long idx = 0;
    _BitScanForward64(&idx, x);
    return (int)idx;
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
    int num_chunks = 0;

    ChunkedMask() = default;
    ChunkedMask(int n_chunks) : chunks(n_chunks, 0ULL), num_chunks(n_chunks) {}

    // 设置第 bit_idx 位
    inline void set_bit(int bit_idx) {
        chunks[bit_idx >> 6] |= (1ULL << (bit_idx & 63));
    }

    // 读取第 bit_idx 位
    inline bool get_bit(int bit_idx) const {
        return (chunks[bit_idx >> 6] >> (bit_idx & 63)) & 1ULL;
    }

    // popcount（所有块之和）
    inline int popcount() const {
        int cnt = 0;
        for (int i = 0; i < num_chunks; ++i)
            cnt += __builtin_popcountll(chunks[i]);
        return cnt;
    }

    // 是否全零
    inline bool is_zero() const {
        for (int i = 0; i < num_chunks; ++i)
            if (chunks[i]) return false;
        return true;
    }

    // 是否等于另一个掩码
    inline bool equals(const ChunkedMask& o) const {
        for (int i = 0; i < num_chunks; ++i)
            if (chunks[i] != o.chunks[i]) return false;
        return true;
    }

    // this |= other
    inline void or_inplace(const ChunkedMask& o) {
        for (int i = 0; i < num_chunks; ++i)
            chunks[i] |= o.chunks[i];
    }

    // this &= ~other  （清除 other 中有的位）
    inline void andnot_inplace(const ChunkedMask& o) {
        for (int i = 0; i < num_chunks; ++i)
            chunks[i] &= ~o.chunks[i];
    }

    // result = this & ~covered  并统计 popcount
    inline int gain_over(const ChunkedMask& covered) const {
        int cnt = 0;
        for (int i = 0; i < num_chunks; ++i)
            cnt += __builtin_popcountll(chunks[i] & ~covered.chunks[i]);
        return cnt;
    }

    // result = this & ~covered 的加权和（weights 按 bit_idx 索引）
    inline double weighted_gain_over(const ChunkedMask& covered,
                                     const std::vector<double>& weights) const {
        double score = 0.0;
        for (int ci = 0; ci < num_chunks; ++ci) {
            uint64_t bits = chunks[ci] & ~covered.chunks[ci];
            while (bits) {
                int bit_pos = __builtin_ctzll(bits);   // 最低位索引（硬件指令）
                score += weights[ci * 64 + bit_pos];
                bits &= bits - 1;                       // 清除最低位
            }
        }
        return score;
    }

    // 检查 this 是否包含 other 的所有位（即 (other & ~this) == 0）
    inline bool covers(const ChunkedMask& other) const {
        for (int i = 0; i < num_chunks; ++i)
            if (other.chunks[i] & ~chunks[i]) return false;
        return true;
    }

    // OR of two masks → new mask
    ChunkedMask or_with(const ChunkedMask& o) const {
        ChunkedMask res(num_chunks);
        for (int i = 0; i < num_chunks; ++i)
            res.chunks[i] = chunks[i] | o.chunks[i];
        return res;
    }

    // AND-NOT: result = this & ~o
    ChunkedMask andnot(const ChunkedMask& o) const {
        ChunkedMask res(num_chunks);
        for (int i = 0; i < num_chunks; ++i)
            res.chunks[i] = chunks[i] & ~o.chunks[i];
        return res;
    }
};

// ============================================================
//  Candidate：一个候选组合的所有信息
// ============================================================
struct Candidate {
    std::vector<int> members;   // 组合中的元素（原始池中的值）
    ChunkedMask mask;           // 覆盖哪些 target
    int bc = 0;                 // mask 的 popcount
    double weighted_score = 0.0; // 全量加权得分（用于 tie-break）
};
