#include "maya/render/diff.hpp"
#include "maya/render/canvas_witness.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include "maya/style/theme.hpp"

#if defined(__GNUC__) || defined(__clang__)
#  define MAYA_PREFETCH_R(addr) __builtin_prefetch((addr), 0, 1)
#  define MAYA_PREFETCH_W(addr) __builtin_prefetch((addr), 1, 1)
#  define MAYA_LIKELY(x)   __builtin_expect(!!(x), 1)
#  define MAYA_UNLIKELY(x) __builtin_expect(!!(x), 0)
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
// MSVC lacks __builtin_prefetch; emit a native SSE prefetch. The GCC hint
// level we want (1 = "some temporal locality") maps best to _MM_HINT_T2
// (L3-only), matching the one-touch-per-frame access pattern of Cell rows
// in the diff loop. _mm_prefetch is read-only — MSVC has no write-prefetch
// intrinsic; PREFETCHW is available on AMD but we stay portable here.
// __assume used for branch hints: lets the optimizer know the expected
// edge without relying on PGO feedback.
#  include <intrin.h>
#  define MAYA_PREFETCH_R(addr) _mm_prefetch(reinterpret_cast<const char*>(addr), _MM_HINT_T2)
#  define MAYA_PREFETCH_W(addr) _mm_prefetch(reinterpret_cast<const char*>(addr), _MM_HINT_T2)
#  define MAYA_LIKELY(x)   (x)
#  define MAYA_UNLIKELY(x) (x)
#else
#  define MAYA_PREFETCH_R(addr) ((void)0)
#  define MAYA_PREFETCH_W(addr) ((void)0)
#  define MAYA_LIKELY(x)   (x)
#  define MAYA_UNLIKELY(x) (x)
#endif

namespace maya {

// The style-id field of a packed Cell (bits 32-47; see Cell::pack).
static constexpr uint64_t kStyleMask = uint64_t{0xFFFF} << 32;

void diff(
    const Canvas& old_canvas,
    const Canvas& new_canvas,
    const StylePool& pool,
    std::string& out)
{
    const int width  = new_canvas.width();
    const int height = new_canvas.height();

    if (width == 0 || height == 0) [[unlikely]] return;

    const Rect damage = new_canvas.damage();
    if (damage.size.is_zero()) [[unlikely]] return;

    // Clamp damage to canvas bounds.
    const int x0 = std::max(0, damage.left().value);
    const int y0 = std::max(0, damage.top().value);
    const int x1 = std::min(width,  damage.right().value);
    const int y1 = std::min(height, damage.bottom().value);

    if (x0 >= x1 || y0 >= y1) [[unlikely]] return;

    // Pre-reserve output buffer: worst case ~20 bytes/cell for style+char+cursor.
    // In practice most cells are unchanged; reserve for ~5% damage.
    const std::size_t estimated = static_cast<std::size_t>((x1 - x0) * (y1 - y0)) / 20 + 256;
    if (out.capacity() < out.size() + estimated)
        out.reserve(out.size() + estimated);

    // Cursor and style state (unknown at start).
    int      cursor_x      = -1;
    int      cursor_y      = -1;
    uint16_t current_style = UINT16_MAX; // sentinel: no style written yet

    const bool size_changed = (old_canvas.width() != width || old_canvas.height() != height);
    const uint64_t blank    = Cell{}.pack();

    // Pre-fetch the raw cell buffers for direct pointer arithmetic in the loop.
    const uint64_t* new_cells = new_canvas.cells();
    const uint64_t* old_cells = size_changed ? nullptr : old_canvas.cells();
    const int       old_w     = size_changed ? 0 : old_canvas.width();
    const int       old_h     = size_changed ? 0 : old_canvas.height();

    // ASCII batch buffer — accumulates consecutive ASCII characters with the
    // same style to reduce per-character append() overhead in the inner loop.
    char ascii_buf[256];
    int  ascii_len = 0;

    // Whether the CURRENT style conceals (SGR 8). Resolved once per style
    // transition; a concealed cell paints a SPACE (occupy the column, show
    // no glyph) — background-independent, terminal-independent. Mirrors
    // emit_cell_run in serialize.cpp so the incremental diff path and the
    // full-repaint path agree.
    bool current_conceal = false;

    // Lambda: flush the ASCII batch buffer.
    auto flush_ascii = [&] {
        if (ascii_len > 0) {
            out.append(ascii_buf, static_cast<std::size_t>(ascii_len));
            ascii_len = 0;
        }
    };

    for (int y = y0; y < y1; ++y) {
        const std::size_t new_row_base = static_cast<std::size_t>(y) * static_cast<std::size_t>(width);
        const std::size_t old_row_base = static_cast<std::size_t>(y) * static_cast<std::size_t>(old_w);
        const bool old_row_valid = !size_changed && y < old_h;

        // Prefetch the next row's data into L2 while processing this row.
        if (y + 1 < y1) [[likely]] {
            MAYA_PREFETCH_R(new_cells + static_cast<std::size_t>(y + 1) * width);
            if (old_row_valid) MAYA_PREFETCH_R(old_cells + static_cast<std::size_t>(y + 1) * old_w);
        }

        // ── Level-1 SIMD skip: entire unchanged row ──────────────────────
        if (old_row_valid && old_w == width && x0 == 0 && x1 == width) [[likely]] {
            if (simd::bulk_eq(new_cells + new_row_base, old_cells + old_row_base,
                              static_cast<std::size_t>(width)))
                continue;
        }

        // ── Pre-scan: find last non-blank column in new canvas ───────────
        // This enables the EL optimisation below: instead of writing each
        // trailing blank cell individually, one \x1b[K clears them all.
        //
        // Canvas tracks per-row last-content column incrementally — read
        // it in O(1) and clamp into our [x0, x1) damage window instead of
        // scanning backward each call.
        const int cached_last = new_canvas.last_content_col(y);
        int last_content = (cached_last < x0) ? x0 - 1
                         : (cached_last >= x1) ? x1 - 1
                         : cached_last;
        // Inner loop only needs to process up to last_content (inclusive).
        const auto content_end = static_cast<std::size_t>(last_content + 1);

        // ── Level-2 SIMD skip: within-row unchanged runs ─────────────────
        auto x  = static_cast<std::size_t>(x0);

        const auto old_xe = old_row_valid
                          ? std::min(content_end, static_cast<std::size_t>(old_w))
                          : static_cast<std::size_t>(0);

        while (x < content_end) {
            // Skip unchanged prefix via SIMD (only when old data is available).
            if (old_row_valid && x < old_xe) {
                const std::size_t avail = old_xe - x;
                const std::size_t skip  = simd::find_first_diff(
                    new_cells + new_row_base + x,
                    old_cells + old_row_base + x,
                    avail);
                if (skip > 0) {
                    // Flush ASCII buffer before skipping — cursor position
                    // will be discontinuous after the unchanged run.
                    flush_ascii();
                }
                x += skip;
                if (x >= content_end) break;
            }

            const uint64_t new_packed = new_cells[new_row_base + x];
            const uint64_t old_packed = (old_row_valid && x < old_xe)
                                      ? old_cells[old_row_base + x]
                                      : blank;

            if (new_packed == old_packed) [[likely]] {
                // Flush before gap in changed cells.
                flush_ascii();
                ++x; continue;
            }

            // Same glyph, different style id: is it a DIFFERENT LOOK on
            // this terminal? Two ids whose colours degrade to the same
            // palette entries (a truecolor gradient on a 256-colour ssh
            // link) paint identically, and re-sending the cell is pure
            // wire. Measured on doom_fire at 214x60: 7-10% of the cells
            // it re-sent every frame looked exactly the same. The pool
            // answers from a per-id "look" key it computes once per style.
            if (old_row_valid && x < old_xe
                && (new_packed & ~kStyleMask) == (old_packed & ~kStyleMask)
                && pool.same_look(static_cast<uint16_t>((new_packed >> 32) & 0xFFFF),
                                  static_cast<uint16_t>((old_packed >> 32) & 0xFFFF))) {
                flush_ascii();
                ++x; continue;
            }

            // Wide-char second-half: check packed width byte directly
            // (bits 56-63) without full unpack. Value 2 = placeholder.
            if ((new_packed >> 56) == 2) [[unlikely]] { ++x; continue; }

            // Cursor positioning — only emit CUP if cursor isn't already here.
            // After writing a character, cursor_x advances naturally, so
            // consecutive changed cells on the same row need no CUP.
            const int cx = static_cast<int>(x);
            if (cursor_x != cx || cursor_y != y) {
                flush_ascii();
                // Same row, moving right, and the cursor is a real column
                // (not parked past the edge in a pending-wrap state, which
                // cursor_x < cx < width rules out): CUF is the shorter move.
                if (cursor_y == y && cursor_x >= 0 && cursor_x < cx)
                    detail::write_cuf(out, cx - cursor_x);
                else
                    detail::write_cup(out, cx + 1, y + 1);
                cursor_x = cx;
                cursor_y = y;
            }

            // Extract style_id and character from packed value directly.
            const auto style_id = static_cast<uint16_t>((new_packed >> 32) & 0xFFFF);
            const auto character = static_cast<char32_t>(new_packed & 0xFFFFFFFF);
            const auto cell_w = static_cast<uint8_t>(new_packed >> 56);

            // Style transition — differential SGR. Emits only the bits
            // that changed (attribute toggles, fg/bg setters), saving
            // the leading `0;` reset + redundant attribute re-emission
            // on every transition. Falls back to the pre-cached full
            // sequence when current_style == UINT16_MAX (terminal state
            // unknown at frame start).
            if (style_id != current_style) {
                flush_ascii();
                pool.write_transition_sgr(current_style, style_id, out);
                current_style = style_id;
                current_conceal = pool.get(style_id).conceal;
            }

            // Concealed cell: paint a space, keep the column.
            const char32_t out_ch = current_conceal ? U' ' : character;

            // Character — batch ASCII into buffer, encode non-ASCII directly.
            if (out_ch < 0x80) [[likely]] {
                ascii_buf[ascii_len++] = static_cast<char>(out_ch);
                if (ascii_len == 256) [[unlikely]] {
                    out.append(ascii_buf, 256);
                    ascii_len = 0;
                }
            } else {
                flush_ascii();
                detail::encode_utf8(out_ch, out);
            }
            // Wide-char first half (width==1) occupies 2 columns; normal chars 1.
            cursor_x += (cell_w == 1) ? 2 : 1;
            ++x;
        }

        // ── EL optimisation: clear trailing old content in one shot ──────
        // If old canvas had non-blank cells beyond last_content, a single
        // \x1b[K (erase to end of line) replaces what would otherwise be
        // dozens of individual space writes + cursor moves.
        //
        // Soundness guard: \x1b[K erases to the PHYSICAL end of the line,
        // not to x1. It is only equivalent to "blank the cells in
        // [last_content+1, x1)" when the damage window reaches the
        // canvas's right edge (x1 == width) — true for every current
        // caller (full-width damage rects). For a partial-width window
        // with live content right of x1, EL would wipe screen cells the
        // front canvas still records as present: silent front/screen
        // divergence no later diff would repair (the bytes match in
        // memory). Such a window instead takes the per-cell fallback
        // below — explicit blank writes bounded to [trail_start, x1) —
        // because the main loop never walks past content_end and would
        // otherwise leave the stale trail on screen entirely.
        if (old_row_valid && last_content < static_cast<int>(x1) - 1) {
            const auto trail_start = static_cast<std::size_t>(last_content + 1);
            const auto trail_end   = std::min(static_cast<std::size_t>(x1),
                                              static_cast<std::size_t>(old_w));
            // Use SIMD to check if old trailing region is all blank.
            // bulk_eq against a blank row would require a blank buffer;
            // instead use find_first_diff against the new (already blank)
            // region — if old differs from new anywhere, we need to clear.
            bool need_clear = false;
            std::size_t first_diff = 0;
            if (trail_start < trail_end) {
                const std::size_t trail_len = trail_end - trail_start;
                const std::size_t same = simd::find_first_diff(
                    new_cells + new_row_base + trail_start,
                    old_cells + old_row_base + trail_start,
                    trail_len);
                need_clear = (same < trail_len);
                first_diff = trail_start + same;
            }
            if (need_clear && x1 == width) {
                // Full-width window: one EL wipes the whole trail.
                flush_ascii();
                const int el_col = last_content + 1;
                if (cursor_x != el_col || cursor_y != y) {
                    detail::write_cup(out, el_col + 1, y + 1);
                }
                // EL erases with the CURRENT background, so clear the
                // attributes but re-assert the theme's bg — otherwise the
                // trail is wiped to the TERMINAL's colour and leaves a
                // ragged un-themed stripe to the right edge on every
                // repainted row. Under `native` there is no bg to assert
                // and this stays a plain reset, which is correct: the
                // terminal is meant to show through.
                if (current_style != 0) {
                    pool.close_link(current_style, out); out.append(pool.sgr(0));
                    current_style = 0;
                }
                if (const Theme& th = theme::live(); theme::owns_canvas(th)) {
                    out += "\x1b[";
                    th.background.degrade(terminal_color_level())
                                 .append_bg_sgr(out);
                    out += "m";
                    // Emitted outside the pool, so force the next run to
                    // re-state its style rather than assume style 0.
                    current_style = UINT16_MAX;
                }
                out += "\x1b[K";
                cursor_x = el_col;
                cursor_y = y;
            } else if (need_clear) {
                // Partial-width window: bounded per-cell blank writes.
                // Start at the first actual difference — cells before it
                // are already blank on screen.
                flush_ascii();
                if (current_style != 0) {
                    pool.close_link(current_style, out); out.append(pool.sgr(0));
                    current_style = 0;
                }
                for (std::size_t cx = first_diff; cx < trail_end; ++cx) {
                    if (new_cells[new_row_base + cx]
                        == old_cells[old_row_base + cx]) continue;
                    const int col = static_cast<int>(cx);
                    if (cursor_x != col || cursor_y != y) {
                        detail::write_cup(out, col + 1, y + 1);
                        cursor_x = col;
                        cursor_y = y;
                    }
                    out += ' ';
                    ++cursor_x;
                }
            }
        }
    }
    // Flush remaining ASCII.
    flush_ascii();
    // a link left open would take in the cursor moves of the next frame
    if (current_style != UINT16_MAX) pool.close_link(current_style, out);
}

// ============================================================================
// Witness-gated diff (Layer 0 of the chain).
// ============================================================================
//
// Preconditions enforced *by the type system*:
//   - Each CanvasWitness was minted by verify_canvas() — i.e. at issue
//     time, last_col_/max_y_ were coherent with cells_ and the cells/caches
//     hashes were recorded on the witness.
//   - Witnesses are move-only; each consumes exactly one verify_canvas()
//     call. Re-using a witness, or fabricating one, is a compile error.
//
// Postcondition (A4 hedge), gated by MAYA_WITNESS_AUDIT:
//   - The cells & caches hashes are recomputed on entry against the canvas
//     each witness binds. Mismatch ⇒ std::abort. Single-threaded renderer;
//     the only causes are use-after-free, buffer overrun in adjacent code,
//     or a host bug that mutated the canvas after verify. Each of those
//     would silently produce ghost rows on the legacy path — here, the
//     process dies loudly instead.
//
// Why it is a build-gated hedge, not an always-on check: the witness is
// consumed by the SAME thread that minted it (the renderer is single-
// threaded), with no canvas-mutating call in between — the type system
// (move-only, one-verify-per-witness) already proves coherence in correct
// code. The re-hash only catches memory CORRUPTION from unrelated code, so
// it belongs in the same tier as the sanitizers. Recomputing it in every
// release frame re-hashes the FULL cell buffer TWICE more per frame — on a
// full-screen terminal the dominant fixed frame cost. MAYA_WITNESS_AUDIT
// defaults ON for debug + sanitizer builds and OFF for release (-DNDEBUG),
// and can be forced either way from the build. Fully portable — pure
// preprocessor, no platform-specific code.
#ifndef MAYA_WITNESS_AUDIT
#  if !defined(NDEBUG) || defined(__SANITIZE_ADDRESS__) || \
      defined(__SANITIZE_THREAD__) || defined(MAYA_WITNESS_STRICT)
#    define MAYA_WITNESS_AUDIT 1
#  elif defined(__has_feature)
#    if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || \
        __has_feature(memory_sanitizer) || __has_feature(undefined_behavior_sanitizer)
#      define MAYA_WITNESS_AUDIT 1
#    else
#      define MAYA_WITNESS_AUDIT 0
#    endif
#  else
#    define MAYA_WITNESS_AUDIT 0
#  endif
#endif

void diff(
    CanvasWitness&& old_witness,
    CanvasWitness&& new_witness,
    const StylePool& pool,
    std::string& out)
{
    const Canvas& old_canvas = old_witness.canvas();
    const Canvas& new_canvas = new_witness.canvas();

#if MAYA_WITNESS_AUDIT
    if (hash_canvas_cells(old_canvas)  != old_witness.cells_hash_at_issue()  ||
        hash_canvas_caches(old_canvas) != old_witness.caches_hash_at_issue() ||
        hash_canvas_cells(new_canvas)  != new_witness.cells_hash_at_issue()  ||
        hash_canvas_caches(new_canvas) != new_witness.caches_hash_at_issue()) {
        // Drift between verify_canvas() and diff(). Unrecoverable: emitting
        // bytes computed against either side would land an inconsistent
        // frame on the wire and corrupt scrollback.
        std::abort();
    }
#else
    // Release: trust the type-system precondition (single-threaded, move-only
    // witness, no canvas mutation between verify and diff). The hashes stored
    // on the witness are still consumed — silence the unused-field warning by
    // touching them once with no side effect.
    (void)old_witness.cells_hash_at_issue();
    (void)new_witness.cells_hash_at_issue();
#endif

    diff(old_canvas, new_canvas, pool, out);
}

} // namespace maya
