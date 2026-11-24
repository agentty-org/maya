#pragma once
// maya::simd::Ops<Avx512> - AVX-512F SIMD (512-bit, 8 cells/op)
//
// Processes 8 packed cells (512 bits) per comparison using native
// 64-bit lane comparison (_mm512_cmpneq_epi64_mask). Falls through
// to AVX2 for 4-7 remaining cells, then scalar for < 4.

#include "../../core/target.hpp"
#if !MAYA_ISA_AVX512F
#error "AVX512 header included without __AVX512F__ defined"
#endif

#include <bit>
#include <immintrin.h>
#include <cstddef>
#include <cstdint>

#include "avx2.hpp"

namespace maya::simd {

struct Avx512 {};

template <>
struct Ops<Avx512> {

    [[nodiscard]] static std::size_t find_first_diff(
        const uint64_t* MAYA_RESTRICT a,
        const uint64_t* MAYA_RESTRICT b,
        std::size_t count) noexcept
    {
        std::size_t i = 0;

        // AVX-512: 8 cells per iteration
        for (; i + 8 <= count; i += 8) {
            __m512i va   = _mm512_loadu_si512(reinterpret_cast<const __m512i*>(a + i));
            __m512i vb   = _mm512_loadu_si512(reinterpret_cast<const __m512i*>(b + i));
            __mmask8 neq = _mm512_cmpneq_epi64_mask(va, vb);
            if (neq != 0)
                return i + static_cast<std::size_t>(std::countr_zero(static_cast<unsigned>(neq)));
        }

        // AVX2 cleanup: 4 cells (always available when AVX-512 is).
        // 64-bit lane compare — same pattern as Ops<Avx2>.
        for (; i + 4 <= count; i += 4) {
            __m256i va  = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + i));
            __m256i vb  = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + i));
            __m256i cmp = _mm256_cmpeq_epi64(va, vb);
            unsigned mask = static_cast<unsigned>(_mm256_movemask_pd(_mm256_castsi256_pd(cmp)));
            if (mask != 0xFu) {
                return i + static_cast<std::size_t>(
                    std::countr_zero(static_cast<unsigned>(~mask & 0xFu)));
            }
        }

        // Scalar tail
        for (; i < count; ++i) {
            if (a[i] != b[i]) return i;
        }
        return count;
    }

    [[nodiscard]] static std::size_t skip_equal(
        const uint64_t* MAYA_RESTRICT a,
        const uint64_t* MAYA_RESTRICT b,
        std::size_t start,
        std::size_t end) noexcept
    {
        std::size_t i = start;

        for (; i + 8 <= end; i += 8) {
            __m512i va   = _mm512_loadu_si512(reinterpret_cast<const __m512i*>(a + i));
            __m512i vb   = _mm512_loadu_si512(reinterpret_cast<const __m512i*>(b + i));
            __mmask8 neq = _mm512_cmpneq_epi64_mask(va, vb);
            if (neq != 0)
                return i + static_cast<std::size_t>(std::countr_zero(static_cast<unsigned>(neq)));
        }

        for (; i + 4 <= end; i += 4) {
            __m256i va  = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + i));
            __m256i vb  = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + i));
            __m256i cmp = _mm256_cmpeq_epi64(va, vb);
            unsigned mask = static_cast<unsigned>(_mm256_movemask_pd(_mm256_castsi256_pd(cmp)));
            if (mask != 0xFu) {
                return i + static_cast<std::size_t>(
                    std::countr_zero(static_cast<unsigned>(~mask & 0xFu)));
            }
        }

        for (; i < end; ++i) {
            if (a[i] != b[i]) return i;
        }
        return end;
    }

    static void streaming_fill(
        uint64_t* MAYA_RESTRICT dst,
        std::size_t count,
        uint64_t value) noexcept
    {
        static constexpr std::size_t kNtThreshold = 16384;
        if (count < kNtThreshold) {
            for (std::size_t i = 0; i < count; ++i) dst[i] = value;
            return;
        }

        // _mm512_stream_si512 requires 64-byte alignment of dst. The cell
        // buffer is 64-byte aligned, but per-region fills (base + y0 * width_)
        // can land on any 8-byte boundary when width_ is not a multiple of 8.
        // Handle the misaligned head with scalar stores so the NT loop sees an
        // aligned pointer; otherwise we'd take an alignment fault (#GP -> SIGSEGV).
        std::size_t i = 0;
        __m512i val = _mm512_set1_epi64(static_cast<long long>(value));
        std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(dst);
        std::size_t head = (64 - (addr & 63)) & 63;          // bytes to align
        std::size_t head_cells = head / sizeof(uint64_t);    // 8B per cell
        if (head_cells > count) head_cells = count;
        for (; i < head_cells; ++i) dst[i] = value;
        for (; i + 8 <= count; i += 8)
            _mm512_stream_si512(reinterpret_cast<__m512i*>(dst + i), val);

        for (; i < count; ++i) dst[i] = value;
        _mm_sfence();
    }

    [[nodiscard]] static uint64_t hash_row(
        const uint64_t* row, int width) noexcept
    {
        return Ops<Avx2>::hash_row(row, width);
    }
};

} // namespace maya::simd
