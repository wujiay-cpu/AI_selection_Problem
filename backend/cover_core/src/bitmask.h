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

// ============================================================
//  ChunkedMask
//  用 vector<uint64_t> 表示任意宽度的位掩码。
//  target_count 最大约 3 万，需要最多 ~470 个 uint64。
// ============================================================
struct ChunkedMask {
    std::vector<uint64_t> chunks;   // chunks[i] 存第 i*64 .. i*64+63 位

    ChunkedMask() = default;
    ChunkedMask(int n_chunks) : chunks(n_chunks, 0ULL) {}

    inline void set_bit(int bit_idx) {
        chunks[bit_idx >> 6] |= (1ULL << (bit_idx & 63));
    }

    inline bool get_bit(int bit_idx) const {
        return (chunks[bit_idx >> 6] >> (bit_idx & 63)) & 1ULL;
    }
};

// ============================================================
//  Candidate：一个候选组合的所有信息
// ============================================================
struct Candidate {
    std::vector<int> members;   // 组合中的元素（原始池中的值）
    ChunkedMask mask;           // 覆盖哪些 target
    int bc = 0;                 // mask 的 popcount
};
