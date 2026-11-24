#pragma once
// maya::simd::Ops<Sse2> - x86-64 baseline SIMD (128-bit, 2 cells/op)
//
// SSE2 is guaranteed on ALL x86-64 CPUs. Processes 2 packed cells
// (128 bits) per comparison. Falls through to scalar for tails.

#include "../../core/target.hpp"
#if !MAYA_CPU_X86_64
#error "SSE2 header included on non-x86-64 platform"
#endif

#include <bit>
#include <immintrin.h>
#include <cstddef>
#include <cstdint>

#include "../detect.hpp"
#include "scalar.hpp"

namespace maya::simd {

struct Sse2 {};

template <>
struct Ops<Sse2> {

    [[nodiscard]] static std::size_t find_first_diff(
        const uint64_t* MAYA_RESTRICT a,
        const uint64_t* MAYA_RESTRICT b,
        std::size_t count) noexcept
    {
        std::size_t i = 0;

        for (; i + 2 <= count; i += 2) {
            __m128i va  = _mm_loadu_si128(reinterpret_cast<const __m128i*>(a + i));
            __m128i vb  = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + i));
            __m128i cmp = _mm_cmpeq_epi8(va, vb);
            unsigned mask = static_cast<unsigned>(_mm_movemask_epi8(cmp));
            if (mask != 0xFFFFu) {
                int diff_byte = std::countr_zero(static_cast<unsigned>(~mask & 0xFFFFu));
                return i + static_cast<std::size_t>(diff_byte / 8);
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

        for (; i + 2 <= end; i += 2) {
            __m128i va  = _mm_loadu_si128(reinterpret_cast<const __m128i*>(a + i));
            __m128i vb  = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + i));
            __m128i cmp = _mm_cmpeq_epi8(va, vb);
            unsigned mask = static_cast<unsigned>(_mm_movemask_epi8(cmp));
            if (mask != 0xFFFFu) {
                int diff_byte = std::countr_zero(static_cast<unsigned>(~mask & 0xFFFFu));
                return i + static_cast<std::size_t>(diff_byte / 8);
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
        // Small buffers: regular fill (cache-warm for subsequent reads)
        static constexpr std::size_t kNtThreshold = 16384;
        if (count < kNtThreshold) {
            for (std::size_t i = 0; i < count; ++i) dst[i] = value;
            return;
        }

        // _mm_stream_si128 requires 16-byte alignment of dst. The cell
        // buffer is 64-byte aligned, but per-region fills (base + y0 * width_)
        // can land on any 8-byte boundary when width_ is odd. Handle the
        // misaligned head with scalar stores so the NT loop sees an aligned
        // pointer; otherwise we'd take an alignment fault (#GP -> SIGSEGV).
        std::size_t i = 0;
        __m128i val = _mm_set1_epi64x(static_cast<long long>(value));
        std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(dst);
        std::size_t head = (16 - (addr & 15)) & 15;          // bytes to align
        std::size_t head_cells = head / sizeof(uint64_t);    // 8B per cell
        if (head_cells > count) head_cells = count;
        for (; i < head_cells; ++i) dst[i] = value;
        for (; i + 2 <= count; i += 2)
            _mm_stream_si128(reinterpret_cast<__m128i*>(dst + i), val);

        for (; i < count; ++i) dst[i] = value;
        _mm_sfence();
    }

    [[nodiscard]] static uint64_t hash_row(
        const uint64_t* row, int width) noexcept
    {
#if MAYA_ISA_SSE42 || MAYA_ISA_AVX2
        uint64_t crc = 0xFFFFFFFFFFFFFFFFULL;
        int i = 0;
        for (; i + 4 <= width; i += 4) {
            crc = _mm_crc32_u64(crc, row[i]);
            crc = _mm_crc32_u64(crc, row[i + 1]);
            crc = _mm_crc32_u64(crc, row[i + 2]);
            crc = _mm_crc32_u64(crc, row[i + 3]);
        }
        for (; i < width; ++i)
            crc = _mm_crc32_u64(crc, row[i]);
        return crc;
#else
        return Ops<Scalar>::hash_row(row, width);
#endif
    }
};

} // namespace maya::simd
