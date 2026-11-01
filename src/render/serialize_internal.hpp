#pragma once
// src/render/serialize_internal.hpp — shared by the serializer's .cpp files
// only: row hashing and the cell-run emitter.

#include "maya/render/serialize.hpp"

#include "maya/render/diff.hpp"  // for detail::encode_utf8
// simd::bulk_eq / find_first_diff come from canvas.hpp → core/simd.hpp
#include "maya/terminal/ansi.hpp"
#include "maya/terminal/tmux.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>    // gate/generation diagnostics (getenv-gated fprintf)
#include <cstdlib>   // std::getenv / std::abort for the invariant tripwires
#include <cstring>
#include "maya/style/theme.hpp"

namespace maya {
namespace serialize_detail {

// FNV-1a fold of one row's cells, mixed with the row index so two
// identical rows at different positions hash differently (the combine
// below is a position-independent XOR, so per-row position must be
// baked in here to keep row-reordering detectable).
inline uint64_t hash_row(const uint64_t* row, std::size_t W, int y) noexcept {
    uint64_t h = 14695981039346656037ULL;
    h ^= static_cast<uint64_t>(y) + 0x9E3779B97F4A7C15ULL;
    h *= 1099511628211ULL;
    for (std::size_t x = 0; x < W; ++x) {
        h ^= row[x];
        h *= 1099511628211ULL;
    }
    return h;
}

// Combine per-row hashes into the single shadow hash. XOR is
// associative/commutative so a single changed row can be updated in
// O(1) by XORing out the old row hash and XORing in the new one.
inline uint64_t combine_rows(const std::vector<uint64_t>& rows) noexcept {
    uint64_t h = 0;
    for (uint64_t r : rows) h ^= r;
    return h;
}

// ──────────────────────────────────────────────────────────────────────
// emit_cell_run — write cells [x_begin, x_end) of row y into `out`.
//
// Threads SGR state through `current_style` so consecutive runs in the
// same frame don't re-emit identical CSI sequences. Caller owns:
//   - cursor positioning (assumes cursor already at (y, x_begin))
//   - row separator (\r\n) before/after if applicable
//   - DECAWM bracket
//   - EL after this run if needed to clear stale tail
//
// This is the smallest reusable unit: full-row serialize() loops over
// it once per row; the inline diff calls it per changed sub-span.
// ──────────────────────────────────────────────────────────────────────
inline void emit_cell_run(const Canvas& canvas, const StylePool& pool,
                   int y, int x_begin, int x_end,
                   uint16_t& current_style, std::string& out)
{
    if (x_begin >= x_end) return;
    const uint64_t* cells = canvas.cells();
    const std::size_t row_base = static_cast<std::size_t>(y) * static_cast<std::size_t>(canvas.width());

    char ascii_buf[256];
    int ascii_len = 0;
    auto flush_ascii = [&] {
        if (ascii_len > 0) {
            out.append(ascii_buf, static_cast<size_t>(ascii_len));
            ascii_len = 0;
        }
    };

    // Whether the CURRENT style conceals (SGR 8). Resolved once per style
    // transition, not per cell. A concealed cell keeps its column(s) but
    // paints a SPACE instead of its glyph — background-independent and
    // universal, unlike delegating SGR 8 to the terminal (widely unsupported
    // / inconsistently rendered). This is what lets the streaming reveal's
    // ghost band hold the real text (stable wrap geometry) yet show nothing.
    bool current_conceal = false;

    for (int x = x_begin; x < x_end; ++x) {
        const uint64_t packed = cells[row_base + x];
        const auto ch  = static_cast<char32_t>(packed & 0xFFFFFFFF);
        const auto sid = static_cast<uint16_t>((packed >> 32) & 0xFFFF);
        const auto w   = static_cast<uint8_t>(packed >> 56);

        if (w == 2) [[unlikely]] continue; // wide-char second half placeholder

        if (sid != current_style) [[unlikely]] {
            // Differential SGR — see StylePool::write_transition_sgr.
            // Saves bytes per transition by skipping the redundant
            // `0;` reset when previous state is known.
            flush_ascii();
            pool.write_transition_sgr(current_style, sid, out);
            current_style = sid;
            current_conceal = pool.get(sid).conceal;
        }

        // Concealed: emit a space to occupy the column, never the glyph.
        const char32_t out_ch = current_conceal ? U' ' : ch;

        if (out_ch < 0x80) [[likely]] {
            ascii_buf[ascii_len++] = static_cast<char>(out_ch);
            if (ascii_len == 256) [[unlikely]] flush_ascii();
            // Run fast path: the cells that follow are almost always more
            // single-width ASCII in this same style (a word, a sentence).
            // Compare the upper 32 bits (style + width + flags) against this
            // cell's in one test and copy the low byte; anything else drops
            // back to the general loop above. Output is byte-identical.
            if (!current_conceal) {
                const uint64_t hi = packed & 0xFFFFFFFF00000000ull;
                int nx = x + 1;
                while (nx < x_end) {
                    const uint64_t p2 = cells[row_base + nx];
                    if ((p2 & 0xFFFFFFFF00000000ull) != hi) break;
                    const auto c2 = static_cast<uint32_t>(p2);
                    if (c2 >= 0x80) break;
                    ascii_buf[ascii_len++] = static_cast<char>(c2);
                    if (ascii_len == 256) [[unlikely]] flush_ascii();
                    ++nx;
                }
                x = nx - 1;
            }
        } else {
            flush_ascii();
            detail::encode_utf8(out_ch, out);
        }
    }
    flush_ascii();
}

// Find the last column of row y whose cell carries visible content
// (non-blank glyph or any styling). Returns -1 if the row is entirely
// blank+unstyled. Mirrors the trim logic used by serialize().
//
// Callers that have access to the Canvas should use canvas.last_content_col(y)
// instead — that's O(1) (incrementally maintained by set()/fill()/write_text)
// vs this function's O(W) backward scan. The function is retained for the
// inline diff path where the comparison buffer is a raw uint64_t* with no
// owning Canvas (state.prev_cells from a previous frame).
[[nodiscard]] inline int last_visible_col(const uint64_t* row, int W) noexcept {
    for (int x = W - 1; x >= 0; --x) {
        const uint64_t p = row[x];
        const auto c = static_cast<char32_t>(p & 0xFFFFFFFF);
        const auto s = static_cast<uint16_t>((p >> 32) & 0xFFFF);
        if ((c != U' ' && c != 0) || s != 0) return x;
    }
    return -1;
}

// Backward scan for the last column where two row buffers differ.
// `cur` is always valid; `prev` may be null, in which case the prev
// row is treated as logically blank (Cell{}.pack()). Returns -1 if no
// difference exists in [0, W).
[[nodiscard]] inline int last_diff_col(const uint64_t* cur, const uint64_t* prev,
                                int W) noexcept {
    static const uint64_t blank = Cell{}.pack();
    for (int x = W - 1; x >= 0; --x) {
        const uint64_t p = prev ? prev[x] : blank;
        if (cur[x] != p) return x;
    }
    return -1;
}

// Equivalent of simd::find_first_diff but tolerant of a null `prev`
// (treats it as a buffer of blank cells without materialising one).
[[nodiscard]] inline int first_diff_col(const uint64_t* cur, const uint64_t* prev,
                                 int W) noexcept {
    if (prev) {
        return static_cast<int>(simd::find_first_diff(
            cur, prev, static_cast<std::size_t>(W)));
    }
    static const uint64_t blank = Cell{}.pack();
    for (int x = 0; x < W; ++x) if (cur[x] != blank) return x;
    return W;
}

// Wide-glyph diff-edge snapping. The diff helpers above operate on raw
// packed u64 cells and have no notion of wide-char pairing. If the
// reported first/last diff column lands ON the trail half (width==2)
// of a wide glyph in EITHER cur or prev, emit_cell_run will silently
// `continue` over that trail and never repaint the lead — leaving the
// wire's old lead glyph in place while the shadow records the new
// canvas bytes as flushed. Snap the edge outward by one cell so the
// emit covers the entire wide pair.
//
// snap_first_diff_left:  if cur[x] OR prev[x] has width==2, return x-1
//                        (clamped to 0).
// snap_last_diff_right:  if cur[x] OR prev[x] has width==1 (lead),
//                        return x+1 (clamped to W-1).
//
// Both inputs (cur, prev) need to be checked: a layout shift can leave
// a stale wide pair in prev whose lead aligns with new content in cur,
// and vice versa.
[[nodiscard]] static inline uint8_t cell_width(uint64_t packed) noexcept {
    return static_cast<uint8_t>(packed >> 56);
}

[[nodiscard]] inline int snap_first_diff_left(int x, const uint64_t* cur,
                                       const uint64_t* prev, int W) noexcept {
    if (x <= 0 || x >= W) return x;
    const uint8_t cw = cell_width(cur[x]);
    const uint8_t pw = prev ? cell_width(prev[x]) : 0;
    if (cw == 2 || pw == 2) return x - 1;
    return x;
}

[[nodiscard]] inline int snap_last_diff_right(int x, const uint64_t* cur,
                                       const uint64_t* prev, int W) noexcept {
    if (x < 0 || x >= W - 1) return x;
    const uint8_t cw = cell_width(cur[x]);
    const uint8_t pw = prev ? cell_width(prev[x]) : 0;
    if (cw == 1 || pw == 1) return x + 1;
    return x;
}

// Write CSI <n> C (cursor forward) for n > 0. n == 0 is a no-op.
inline void write_cursor_forward(std::string& out, int n) {
    if (n <= 0) return;
    out += "\x1b[";
    ansi::detail::append_int(out, n);
    out += 'C';
}

} // namespace serialize_detail
} // namespace maya
