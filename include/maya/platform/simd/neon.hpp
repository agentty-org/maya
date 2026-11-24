#pragma once
// maya::simd::Ops<Neon> - ARM NEON SIMD (128-bit, 2-4 cells/op)
//
// Processes 4 cells per iteration (2x 128-bit unrolled). On AArch64 the
// all-equal check is a single `vminvq_u32` (Apple Silicon, Pi 3/4/5).
// On ARMv7+NEON (Pi 2 v1.1, Pi Zero W, older 32-bit boards) `vceqq_u64`
// and `vminvq_*` are A64-only, so we synthesise both from the A32 subset
// that's available — see detail_neon::{vceqq_u64_compat, all_lanes_set_u32x4}.
// Same vector width, same load/store/AND ops; just an extra reduce step
// on the all-equal check.

#include "../../core/target.hpp"
#if !(MAYA_CPU_ARM64 || MAYA_ISA_NEON)
#error "NEON header included on platform without NEON support"
#endif

#include <arm_neon.h>
#if defined(__ARM_FEATURE_CRC32)
    #include <arm_acle.h>
#endif

#include <cstddef>
#include <cstdint>

#include "scalar.hpp"

namespace maya::simd {

namespace detail_neon {

// 64-bit-element vector equality. `vceqq_u64` is AArch64-only; on ARMv7
// NEON we emulate via a 32-bit-element compare and AND the two halves
// of each 64-bit lane via a pair-swap. Result domain matches `vceqq_u64`:
// each 64-bit lane is 0xFF…FF if equal, 0 otherwise.
[[nodiscard]] MAYA_FORCEINLINE uint64x2_t vceqq_u64_compat(
    uint64x2_t a, uint64x2_t b) noexcept
{
#if MAYA_CPU_ARM64
    return vceqq_u64(a, b);
#else
    uint32x4_t c   = vceqq_u32(vreinterpretq_u32_u64(a),
                               vreinterpretq_u32_u64(b));
    uint32x4_t rev = vrev64q_u32(c);
    uint32x4_t both = vandq_u32(c, rev);
    return vreinterpretq_u64_u32(both);
#endif
}

// "Every 32-bit lane is set" — equivalent to `vminvq_u32(v) != 0` when
// every lane is in {0, 0xFF…FF}. Used only on the result of vceq* chains
// where that domain holds by construction.
[[nodiscard]] MAYA_FORCEINLINE bool all_lanes_set_u32x4(
    uint32x4_t v) noexcept
{
#if MAYA_CPU_ARM64
    return vminvq_u32(v) != 0;
#else
    uint32x2_t lo = vget_low_u32(v);
    uint32x2_t hi = vget_high_u32(v);
    uint32x2_t a  = vand_u32(lo, hi);
    return (vget_lane_u32(a, 0) & vget_lane_u32(a, 1)) != 0;
#endif
}

} // namespace detail_neon

struct Neon {};

template <>
struct Ops<Neon> {

    [[nodiscard]] static std::size_t find_first_diff(
        const uint64_t* MAYA_RESTRICT a,
        const uint64_t* MAYA_RESTRICT b,
        std::size_t count) noexcept
    {
        std::size_t i = 0;

        // Unrolled: 4 cells per iteration (2x 128-bit loads)
        for (; i + 4 <= count; i += 4) {
            uint64x2_t va0 = vld1q_u64(a + i);
            uint64x2_t vb0 = vld1q_u64(b + i);
            uint64x2_t va1 = vld1q_u64(a + i + 2);
            uint64x2_t vb1 = vld1q_u64(b + i + 2);
            uint64x2_t cmp0 = detail_neon::vceqq_u64_compat(va0, vb0);
            uint64x2_t cmp1 = detail_neon::vceqq_u64_compat(va1, vb1);
            uint64x2_t all = vandq_u64(cmp0, cmp1);
            uint32x4_t all32 = vreinterpretq_u32_u64(all);
            if (detail_neon::all_lanes_set_u32x4(all32)) continue;
            // Slow path: find which cell differs
            if (vgetq_lane_u64(cmp0, 0) == 0) return i;
            if (vgetq_lane_u64(cmp0, 1) == 0) return i + 1;
            if (vgetq_lane_u64(cmp1, 0) == 0) return i + 2;
            return i + 3;
        }

        // Handle remaining 2-cell block
        for (; i + 2 <= count; i += 2) {
            uint64x2_t va  = vld1q_u64(a + i);
            uint64x2_t vb  = vld1q_u64(b + i);
            uint64x2_t cmp = detail_neon::vceqq_u64_compat(va, vb);
            uint32x4_t cmp32 = vreinterpretq_u32_u64(cmp);
            if (detail_neon::all_lanes_set_u32x4(cmp32)) continue;
            if (vgetq_lane_u64(cmp, 0) == 0) return i;
            return i + 1;
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
            uint64x2_t va0 = vld1q_u64(a + i);
            uint64x2_t vb0 = vld1q_u64(b + i);
            uint64x2_t va1 = vld1q_u64(a + i + 2);
            uint64x2_t vb1 = vld1q_u64(b + i + 2);
            uint64x2_t cmp0 = detail_neon::vceqq_u64_compat(va0, vb0);
            uint64x2_t cmp1 = detail_neon::vceqq_u64_compat(va1, vb1);
            uint64x2_t all = vandq_u64(cmp0, cmp1);
            if (vgetq_lane_u64(all, 0) != 0 && vgetq_lane_u64(all, 1) != 0)
                continue;
            if (vgetq_lane_u64(cmp0, 0) == 0) return i;
            if (vgetq_lane_u64(cmp0, 1) == 0) return i + 1;
            if (vgetq_lane_u64(cmp1, 0) == 0) return i + 2;
            return i + 3;
        }

        for (; i + 2 <= end; i += 2) {
            uint64x2_t va  = vld1q_u64(a + i);
            uint64x2_t vb  = vld1q_u64(b + i);
            uint64x2_t cmp = detail_neon::vceqq_u64_compat(va, vb);
            if (vgetq_lane_u64(cmp, 0) == 0) return i;
            if (vgetq_lane_u64(cmp, 1) == 0) return i + 1;
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
        // Always use NEON: 128-bit stores are ~2x scalar throughput on
        // Apple Silicon even for small buffers (no NT-store concern on ARM).
        std::size_t i = 0;
        uint64x2_t val = vdupq_n_u64(value);

        // 8 cells/iteration (4x 128-bit stores) — saturates M1/M2 store units
        for (; i + 8 <= count; i += 8) {
            vst1q_u64(dst + i,     val);
            vst1q_u64(dst + i + 2, val);
            vst1q_u64(dst + i + 4, val);
            vst1q_u64(dst + i + 6, val);
        }
        for (; i + 2 <= count; i += 2)
            vst1q_u64(dst + i, val);

        if (i < count) dst[i] = value;
    }

    [[nodiscard]] static uint64_t hash_row(
        const uint64_t* row, int width) noexcept
    {
#if defined(__ARM_FEATURE_CRC32)
        // Hardware CRC32: ~1 cycle/cell on Apple Silicon
        uint32_t crc = 0xFFFFFFFFu;
        int i = 0;
        for (; i + 4 <= width; i += 4) {
            crc = __crc32d(crc, row[i]);
            crc = __crc32d(crc, row[i + 1]);
            crc = __crc32d(crc, row[i + 2]);
            crc = __crc32d(crc, row[i + 3]);
        }
        for (; i < width; ++i)
            crc = __crc32d(crc, row[i]);
        return static_cast<uint64_t>(crc);
#else
        return Ops<Scalar>::hash_row(row, width);
#endif
    }
};

} // namespace maya::simd
