#pragma once
// maya::simd::Ops<Avx2> - AVX2 SIMD (256-bit, 4 cells/op)
//
// Processes 4 packed cells (256 bits) per comparison.
// Falls through to scalar for remaining elements.

#include "../../core/target.hpp"
#if !MAYA_ISA_AVX2
#error "AVX2 header included without __AVX2__ defined"
#endif

#include <bit>
#include <immintrin.h>
#include <cstddef>
#include <cstdint>

#include "../detect.hpp"
#include "sse2.hpp"

namespace maya::simd {

struct Avx2 {};

template <>
struct Ops<Avx2> {

    [[nodiscard]] static std::size_t find_first_diff(
        const uint64_t* MAYA_RESTRICT a,
        const uint64_t* MAYA_RESTRICT b,
        std::size_t count) noexcept
    {
        std::size_t i = 0;

        // AVX2 with 64-bit lane compare: 4 cells per iteration, producing
        // a 4-bit mask directly via movemask_pd — avoids the /8 divide of
        // the byte-level approach.
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
        // Below 32 cells: scalar fill keeps data in L1 (hot for subsequent diff).
        if (count < 32) {
            for (std::size_t i = 0; i < count; ++i) dst[i] = value;
            return;
        }

        __m256i val = _mm256_set1_epi64x(static_cast<long long>(value));

        // 32..2047 cells: AVX2 temporal stores — data stays in cache for
        // the upcoming diff pass. Unroll 4x (16 cells/iteration) to saturate
        // the store buffer on Alder Lake.
        static constexpr std::size_t kNtThreshold = 2048;
        if (count < kNtThreshold) {
            std::size_t i = 0;
            for (; i + 16 <= count; i += 16) {
                _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i),      val);
                _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i + 4),  val);
                _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i + 8),  val);
                _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i + 12), val);
            }
            for (; i + 4 <= count; i += 4)
                _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), val);
            for (; i < count; ++i) dst[i] = value;
            return;
        }

        // 2048+ cells: non-temporal stores — bypass cache entirely (full
        // screen clears, large background fills). The diff pass will pull
        // this data from DRAM on demand.
        //
        // _mm256_stream_si256 requires 32-byte alignment of dst. cells_ is
        // std::vector<uint64_t> whose buffer is only 16-byte aligned on
        // glibc, and per-region fills (base + y0 * width_) can land on any
        // 8-byte boundary. Handle the misaligned head with scalar stores so
        // the NT loop sees an aligned pointer; otherwise we'd take an
        // alignment fault (#GP → SIGSEGV).
        std::size_t i = 0;
        std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(dst);
        std::size_t head = (32 - (addr & 31)) & 31;          // bytes to align
        std::size_t head_cells = head / sizeof(uint64_t);    // 8B per cell
        if (head_cells > count) head_cells = count;
        for (; i < head_cells; ++i) dst[i] = value;
        for (; i + 16 <= count; i += 16) {
            _mm256_stream_si256(reinterpret_cast<__m256i*>(dst + i),      val);
            _mm256_stream_si256(reinterpret_cast<__m256i*>(dst + i + 4),  val);
            _mm256_stream_si256(reinterpret_cast<__m256i*>(dst + i + 8),  val);
            _mm256_stream_si256(reinterpret_cast<__m256i*>(dst + i + 12), val);
        }
        for (; i + 4 <= count; i += 4)
            _mm256_stream_si256(reinterpret_cast<__m256i*>(dst + i), val);
        for (; i < count; ++i) dst[i] = value;
        _mm_sfence();
    }

    [[nodiscard]] static uint64_t hash_row(
        const uint64_t* row, int width) noexcept
    {
        // AVX2 implies SSE4.2 — use hardware CRC32
        return Ops<Sse2>::hash_row(row, width);
    }
};

} // namespace maya::simd
