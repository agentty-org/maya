#pragma once
// maya::simd - Tag-dispatched SIMD operations
//
// Each ISA is a type (Avx512, Avx2, Sse2, Neon, Scalar).
// Each has an Ops<Isa> specialization in its own header.
// The Best alias selects the highest available ISA at compile time.
// Public API functions dispatch to Ops<Best> — zero-cost, fully inlined.
//
// Adding a new ISA (SVE, RISC-V V) means:
//   1. Create a new header with Ops<NewIsa> specialization
//   2. Add the ISA to the Best selection chain below

#include "../../core/target.hpp"
#include "../detect.hpp"

// Include the best available ISA (and its dependencies)
#if MAYA_ISA_AVX512F
    #include "avx512.hpp"   // includes avx2.hpp -> scalar.hpp
#elif MAYA_ISA_AVX2
    #include "avx2.hpp"     // includes sse2.hpp -> scalar.hpp
#elif MAYA_CPU_X86_64
    #include "sse2.hpp"     // includes scalar.hpp
#elif MAYA_CPU_ARM64 || MAYA_ISA_NEON
    #include "neon.hpp"     // includes scalar.hpp
#else
    #include "scalar.hpp"
#endif

#include <cstddef>
#include <cstdint>

namespace maya::simd {

// ============================================================================
// Best — the highest available ISA at compile time
// ============================================================================

using Best =
#if MAYA_ISA_AVX512F
    Avx512;
#elif MAYA_ISA_AVX2
    Avx2;
#elif MAYA_CPU_X86_64
    Sse2;
#elif MAYA_CPU_ARM64 || MAYA_ISA_NEON
    Neon;
#else
    Scalar;
#endif

// ============================================================================
// Public API — zero-cost dispatch to Ops<Best>
// ============================================================================
// These are the functions the rest of maya calls. They resolve to
// Ops<Best>::method at compile time — no vtable, no function pointer.

[[nodiscard]] inline std::size_t find_first_diff(
    const uint64_t* MAYA_RESTRICT a,
    const uint64_t* MAYA_RESTRICT b,
    std::size_t count) noexcept
{
    return Ops<Best>::find_first_diff(a, b, count);
}

[[nodiscard]] inline std::size_t skip_equal(
    const uint64_t* MAYA_RESTRICT a,
    const uint64_t* MAYA_RESTRICT b,
    std::size_t start,
    std::size_t end) noexcept
{
    return Ops<Best>::skip_equal(a, b, start, end);
}

[[nodiscard]] inline bool bulk_eq(
    const uint64_t* MAYA_RESTRICT a,
    const uint64_t* MAYA_RESTRICT b,
    std::size_t count) noexcept
{
    return find_first_diff(a, b, count) == count;
}

inline void streaming_fill(
    uint64_t* MAYA_RESTRICT dst,
    std::size_t count,
    uint64_t value) noexcept
{
    Ops<Best>::streaming_fill(dst, count, value);
}

[[nodiscard]] inline uint64_t hash_row(
    const uint64_t* row, int width) noexcept
{
    return Ops<Best>::hash_row(row, width);
}

} // namespace maya::simd
