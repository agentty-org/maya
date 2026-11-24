// maya::CanvasWitness — Layer 0 implementation. See canvas_witness.hpp.

#include <maya/core/target.hpp>
#include "maya/render/canvas_witness.hpp"

#include "maya/core/simd.hpp"   // maya::simd::hash_row — cross-platform word hash

#include <cstddef>
#include <cstdint>

// ── Cross-platform hardware-CRC32 dispatch for hash_canvas_cells ──────────
//
// hash_canvas_cells() runs once per frame over the whole W·H cell buffer; on
// a full-screen terminal that is the single largest fixed per-frame cost.
// The byte-wise FNV it used before is memory-bound *and* serial (one
// multiply per byte). The cell buffer is an array of packed uint64_t, so a
// word-wise hardware CRC32 (one instruction per cell) is ~10x faster and
// exists on every arch maya targets:
//
//   - x86-64  : SSE4.2 `crc32q`   (Nehalem 2008 and every chip since)
//   - AArch64 : NEON  `crc32d`    (ARMv8.1-A CRC; Apple Silicon, Graviton, Pi)
//   - other   : portable scalar FNV fallback (simd::hash_row / Ops<Scalar>)
//
// simd::hash_row() already resolves the COMPILE-TIME best of those. But
// portable release builds turn MAYA_NATIVE_TUNING off, so x86-64 compiles at
// the -march=x86-64 baseline where __SSE4_2__ is undefined and simd::hash_row
// would fall to scalar even on a CRC-capable CPU. To keep the hot path native
// on EVERY build config (per the cross-platform-at-native-speed requirement),
// x86-64 gets a RUNTIME dispatch: a target("sse4.2") helper selected once via
// __builtin_cpu_supports, with the scalar/compile-time path as the fallback.
//
// The value only ever needs to be self-consistent (verify_canvas mints it and
// the in-diff re-check recomputes it with the SAME function), so the specific
// hash algorithm is free to differ per arch — CRC32 on one machine, FNV on
// another. It is never compared across processes or persisted.

#if MAYA_CPU_X86_64
#  define MAYA_WITNESS_X86 1
#  include <nmmintrin.h>   // _mm_crc32_u64 (SSE4.2)
#else
#  define MAYA_WITNESS_X86 0
#endif

namespace maya {
namespace {

#if MAYA_WITNESS_X86

// SSE4.2 word-wise CRC32 over the cell buffer. Isolated in its own function
// so the target attribute enables `crc32q` codegen even when the translation
// unit is compiled at the baseline -march (GCC/Clang). MSVC always has the
// intrinsic available under /arch:AVX2 (or SSE4.2 baseline on x64), so no
// attribute is needed there.
#if MAYA_CC_GNU_EXT
__attribute__((target("sse4.2")))
#endif
std::uint64_t crc32_cells_sse42(const std::uint64_t* cells, std::size_t n) noexcept {
    std::uint64_t crc = 0xFFFFFFFFFFFFFFFFULL;
    std::size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        crc = _mm_crc32_u64(crc, cells[i]);
        crc = _mm_crc32_u64(crc, cells[i + 1]);
        crc = _mm_crc32_u64(crc, cells[i + 2]);
        crc = _mm_crc32_u64(crc, cells[i + 3]);
    }
    for (; i < n; ++i) crc = _mm_crc32_u64(crc, cells[i]);
    return crc;
}

// Resolve the CPU's CRC32 capability exactly once. __builtin_cpu_supports is
// GCC/Clang; MSVC on x64 always has SSE4.2 in its baseline so we take the
// hardware path unconditionally there.
bool x86_has_crc32() noexcept {
#if MAYA_CC_GNU_EXT
    static const bool ok = __builtin_cpu_supports("sse4.2");
    return ok;
#else
    return true;  // MSVC x64 baseline includes SSE4.2
#endif
}

#endif  // MAYA_WITNESS_X86

// Cross-platform word-wise hash of the packed cell buffer.
std::uint64_t hash_cells_words(const std::uint64_t* cells, std::size_t n) noexcept {
#if MAYA_WITNESS_X86
    if (x86_has_crc32()) return crc32_cells_sse42(cells, n);
#endif
    // Non-x86, or an ancient x86 without SSE4.2: use the SIMD layer's
    // hash_row, which is hardware CRC32 on AArch64/NEON and a compile-time
    // best-effort scalar FNV everywhere else. Width is an int in that API;
    // clamp defensively (a terminal never has >INT_MAX cells).
    return maya::simd::hash_row(cells, static_cast<int>(n));
}

}  // namespace

std::uint64_t hash_canvas_cells(const Canvas& c) noexcept {
    const auto* cells = c.cells();
    const std::size_t n = c.cell_count();
    return hash_cells_words(cells, n);
}

std::uint64_t hash_canvas_caches(const Canvas& c) noexcept {
    // Hash (last_content_col(y) for every y) + max_content_row().
    // Cheap: O(H), and folds the two derived caches into one value the
    // in-diff re-check can compare against the witness-issue value. Left as
    // byte-wise FNV: it hashes H+1 ints (not the cell buffer), so it is
    // already negligible next to the cell hash above.
    const int h = c.height();
    const int max_y = c.max_content_row();
    std::uint64_t seed = detail::fnv1a64(&max_y, sizeof(max_y));
    for (int y = 0; y < h; ++y) {
        const int v = c.last_content_col(y);
        seed = detail::fnv1a64(&v, sizeof(v), seed);
    }
    return seed;
}

// Slow-truth derivation. last_col_truth[y] = max x s.t. packed_cell(x,y)
// != Cell{}.pack(); max_y_truth = max y with any such cell.
//
// Disagreement with the cached values means a writer to cells_ failed to
// keep last_col_ / max_y_ in sync — either an overdraw path that didn't
// route through set()/fill() (raw pointer write), or a paint sequence
// that shrank the live extent without a clear_row(). Either way, the
// downstream diff would have read stale-high and produced ghost cells.
std::optional<CanvasWitness> verify_canvas(const Canvas& c) noexcept {
    const int w = c.width();
    const int h = c.height();
    if (w <= 0 || h <= 0) {
        // Degenerate canvas: caches are vacuously true. Issue a witness
        // with the trivial hashes so the diff fast-paths still typecheck.
        const std::uint64_t ch = hash_canvas_cells(c);
        const std::uint64_t kh = hash_canvas_caches(c);
        return CanvasWitness{&c, ch, kh};
    }

    const std::uint64_t blank = 0ULL;  // Cell{}.pack() == 0; all default fields.
    const std::uint64_t* cells = c.cells();

    int truth_max_y = -1;
    for (int y = 0; y < h; ++y) {
        int truth_last = -1;
        const std::uint64_t* row = cells + static_cast<std::ptrdiff_t>(y) * w;
        for (int x = w - 1; x >= 0; --x) {
            if (row[x] != blank) { truth_last = x; break; }
        }
        if (c.last_content_col(y) != truth_last) return std::nullopt;
        if (truth_last >= 0) truth_max_y = y;
    }
    if (c.max_content_row() != truth_max_y) return std::nullopt;

    return CanvasWitness{&c, hash_canvas_cells(c), hash_canvas_caches(c)};
}

} // namespace maya
