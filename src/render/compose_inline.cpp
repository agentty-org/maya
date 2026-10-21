// src/render/compose_inline.cpp — the inline frame: caret placement and the
// row-diff byte producer behind compose_inline_frame.
#include "serialize_internal.hpp"

namespace maya {

using namespace serialize_detail;

// Bounded multiplication for content_rows × width.  Returns SIZE_MAX on
// overflow — the caller treats that as "drop the prev cache, do a full
// repaint" rather than silently corrupting prev_cells with a wrapped
// size that re-uses uninitialized capacity.
[[nodiscard]] inline constexpr std::size_t safe_cells(int rows, int W) noexcept {
    if (rows <= 0 || W <= 0) return 0;
    const auto r = static_cast<std::size_t>(rows);
    const auto w = static_cast<std::size_t>(W);
    if (r > (std::size_t)(-1) / w) return (std::size_t)(-1);
    return r * w;
}

// Bandwidth coalesce (the `min_changed_rows` parameter that held a
// small diff across frames) was removed when the runtime concluded the
// hold was actively harmful on the typing path — a 1-row composer
// change would meet the hold criterion and swallow keystrokes until
// the third one forced a flush. See render_inline.cpp's compose_inline_frame
// call site for the full diagnosis. The parameter, the per-state
// `held_count` field, and the early-exit suppression in the per-row
// diff scan all went with it. If a future renderer needs a similar
// optimisation, the design should live ON TOP of the simple emit (a
// caller-side decision to skip render entirely for a frame), not
// inside compose where it has visibility into row-level state but no
// way to tell whether the diff is user-input or animation.

// ============================================================================
// compose_inline_frame
// ============================================================================
//
// Inline-mode incremental update. The output is a sequence of cursor
// moves and per-row sub-span rewrites: only the cells that actually
// changed between the previous and current canvas are emitted.
//
// Compared to the older "find first changed row, redraw the contiguous
// tail" approach, this matters in two stacked cases that are the norm
// during streaming:
//
//   1. The bottom of the frame animates every tick (spinner / status
//      bar / sparkline) even when the rest is idle.
//   2. New tokens land in mid-frame, growing a message bubble by one
//      wrapped row, while the spinner above-keeps-ticking.
//
// In both cases the old algorithm rewrote every row from the topmost
// change down to the bottom — including the unchanged middle. On a
// terminal that doesn't honor DEC mode 2026 (synchronized output), that
// large, contiguous repaint is rendered byte-by-byte and shows as
// flicker. The per-row + per-cell-span path emits only the rows that
// actually differ, and within each row only the changed columns —
// typically an order-of-magnitude reduction on streaming workloads.
//
// `synchronized_output` toggles the DEC 2026 wrapper: pass true when
// the host terminal is known to honor it (modern Kitty, WezTerm,
// Ghostty, Windows Terminal, Alacritty, iTerm 3.5+, vte ≥ 6200) so
// the frame swaps atomically. Pass false to skip the harmless-but-
// pointless escape on terminals that ignore it (Apple Terminal, plain
// xterm). The bytes save nothing visible either way, but knowing the
// answer upstream lets the application coalesce paints more
// aggressively when sync isn't available.
// ── Frame epilogue: hardware caret or parked hide ────────────────────
// (Evolution of the ghost-caret park; diagnosis credit: davidwed.)
//
// Every compose exit ends the wire on the frame's RESTING ROW (last
// wire row) — the per-row loop walks to content_rows-1, the shrink
// paths restore it, the no-op path never moved. From there this
// epilogue ends the frame in exactly one of two terminal cursor
// states, both reached by row-RELATIVE moves only (no absolute CUP —
// the frame's absolute anchor is unknowable in inline mode, and a
// row-targeting escape outside emit_move is what wire_row.hpp's T1
// forbids):
//
//   CARET  the canvas carries a cursor hint whose row is on-screen:
//          \r, CUU (resting_row - caret_row), CUF caret_col, ?25h.
//          The REAL terminal cursor becomes the composer's caret —
//          native blink at the terminal's own cadence (zero animation
//          wake-ups), IME candidate windows anchored at the true cell,
//          screen readers tracking the real position. The show sits
//          OUTSIDE the sync wrapper; the failure mode of a rolled-back
//          show is an invisible cursor for one frame (benign,
//          self-healing) — the exact dual of the rollback-proof hide.
//
//   PARK   no hint (no focused caret this frame): \r + ?25l — the
//          davidwed ghost-caret hardening, unchanged. A terminal-side
//          DECTCEM loss surfaces the cursor on border chrome at col 1,
//          never mid-content.
//
// The cursor's end-of-frame position is recorded in
// state.cursor_row_offset_ (rows above the resting row); the next
// frame's prologue normalizes it back with cursor_down BEFORE any
// delta math, so the serializer body never sees the difference.
// Off-screen hint rows (caret scrolled past the viewport top on an
// oversized frame) fall back to PARK — a CUU that would cross the
// viewport top is exactly the T1 no-ghost violation, so the clamp is
// load-bearing, not defensive.
namespace {

// Resolve the frame's caret cell, if any. Two sources, in priority:
//   1. an explicit Canvas::set_cursor_hint (direct-canvas users who
//      know coordinates — no cosmetics, terminal-default shape);
//   2. the caret_anchor style meta-bit — the widget route. A widget
//      cannot know its absolute canvas position at build() time (it
//      emits an Element tree; layout+paint resolve coordinates later,
//      and the component cache can blit cached CELLS without running
//      the widget at all), so the anchor rides in the interned style
//      and is recovered here from the painted cells themselves —
//      along with its DECSCUSR shape and caret color (the style's fg;
//      the glyph is concealed, so fg is free to carry it).
// The scan is gated on pool.has_caret_anchor() — an app with no
// hardware-caret widget pays one branch — and walks BOTTOM-UP because
// the composer overwhelmingly sits at the frame's bottom edge; "bottom-
// most anchor wins" is also the right semantic when a stale cached
// component above still carries an anchor cell.
[[nodiscard]] std::optional<ResolvedCaret>
resolve_caret_hint(const Canvas& canvas,
                   const StylePool& pool,
                   int content_rows,
                   int term_h) noexcept {
    if (canvas.cursor_hint()) {
        return ResolvedCaret{canvas.cursor_hint()->x,
                             canvas.cursor_hint()->y, 0, 0};
    }
    if (!pool.has_caret_anchor()) return std::nullopt;
    const int W = canvas.width();
    const uint64_t* cells = canvas.cells();
    // Only the visible window can host the hardware caret: rows above
    // the viewport top are unreachable (T1), rows past content are
    // blank. Bottom-up within that window.
    const int lo = std::max(0, content_rows - term_h);
    for (int y = std::min(content_rows, canvas.height()) - 1; y >= lo; --y) {
        const uint64_t* row = cells + static_cast<std::size_t>(y) * W;
        for (int x = 0; x < W; ++x) {
            const auto sid = static_cast<uint16_t>((row[x] >> 32) & 0xFFFF);
            if (sid != 0 && pool.is_caret_anchor(sid)) {
                const Style& st = pool.get(sid);
                uint32_t color = 0;
                if (st.fg.has_value()) {
                    // Resolve first: a caret styled with a theme slot used to
                    // fail the Rgb test and report "no colour", so a themed
                    // caret silently lost its tint.
                    const LitColor c = theme::live().resolve(*st.fg);
                    if (c.kind() == ColorKind::Rgb) {
                        color = 0x01000000u
                              | (static_cast<uint32_t>(c.r()) << 16)
                              | (static_cast<uint32_t>(c.g()) << 8)
                              |  static_cast<uint32_t>(c.b());
                    }
                }
                return ResolvedCaret{x, y, st.caret_shape, color};
            }
        }
    }
    return std::nullopt;
}

// Append `CSI <shape> SP q` / `OSC 12 ; #rrggbb ST` iff they differ
// from what the wire already carries (wire state passed by ref from
// the friend epilogue). shape/color 0 mean "terminal default": when
// the wire carries a non-default value and the target is 0, the RESET
// is emitted (`0 q` / OSC 112) — so a widget that stops overriding
// (e.g. composer returning to idle, which respects the user's own
// terminal cursor) actually gets the user's shape back mid-session,
// not only at finalize. Cosmetics ride OUTSIDE the sync wrapper with
// the show — they only matter when the cursor is visible.
void emit_caret_cosmetics(std::string& out,
                          uint8_t& shape_state,
                          uint32_t& color_state,
                          uint8_t shape,
                          uint32_t color) noexcept {
    if (shape != shape_state) {
        out += "\x1b[";
        out += static_cast<char>('0' + (shape % 10));
        out += " q";
        shape_state = shape;
    }
    if (color != color_state) {
        if (color == 0) {
            out += "\x1b]112\x1b\\";
        } else {
            char buf[24];
            std::snprintf(buf, sizeof buf, "\x1b]12;#%02x%02x%02x\x1b\\",
                          (color >> 16) & 0xFF, (color >> 8) & 0xFF,
                          color & 0xFF);
            out += buf;
        }
        color_state = color;
    }
}

} // namespace

// True when emit_caret_epilogue will SHOW the cursor at the hint rather
// than park it.
[[nodiscard]] bool caret_on_screen(const std::optional<ResolvedCaret>& hint,
                                   int content_rows, int term_h) noexcept {
    const int top_row = std::max(0, content_rows - term_h);
    return hint && hint->y >= top_row && hint->y < content_rows;
}

// External linkage (friend of InlineFrameState); TU-local by
// convention — no header declaration, same policy as
// compose_inline_frame_impl above.
void emit_caret_epilogue(std::string& out,
                         const std::optional<ResolvedCaret>& hint,
                         InlineFrameState& state,
                         int content_rows,
                         int term_h,
                         int width) noexcept {
    const int resting = std::min(content_rows, term_h) - 1;  // frame-local
    // For an oversized frame the viewport shows the LAST term_h canvas
    // rows; a hint above that window cannot be reached without crossing
    // the viewport top.
    const int top_row = std::max(0, content_rows - term_h);
    if (caret_on_screen(hint, content_rows, term_h)) {
        const int caret_screen_row = hint->y - top_row;      // frame-local
        const int up = resting - caret_screen_row;
        out += '\r';
        if (up > 0) ansi::write_cursor_up(out, up);
        if (hint->x > 0) {
            // CUF: column-only move — never crosses a row boundary
            // (DECAWM is off frame-wide; CUF clamps at the right edge
            // by spec).
            out += "\x1b[";
            out += std::to_string(hint->x);
            out += 'C';
        }
        // Cosmetics (DECSCUSR shape / OSC 12 color) — emit-on-change,
        // before the show so the first visible frame already has them.
        emit_caret_cosmetics(out, state.cursor_shape_, state.cursor_color_,
                             hint->shape, hint->color);
        out += ansi::show_cursor;
        state.cursor_hidden_     = false;
        state.cursor_shown_      = true;
        state.cursor_row_offset_ = up > 0 ? up : 0;
        state.cursor_col_        = hint->x;
    } else {
        out += '\r';
        // Park at the RIGHT edge of the resting row, not col 0. The
        // parked position matters twice:
        //   • terminal-side DECTCEM loss surfaces the cursor there —
        //     both frame corners are border chrome, equally benign;
        //   • tmux copy-mode draws ITS OWN cursor at the pane-cursor
        //     screen position, independent of DECTCEM (verified by
        //     probing: a hidden pane cursor still yields a copy-mode
        //     cursor, and a pane-side ?25l mid-copy-mode never reaches
        //     the outer terminal). As the user scrolls, that screen
        //     position acquires THREAD content — at col 0 it lands on
        //     the rail (the reported ghost); at the right margin it
        //     overwhelmingly lands on blank padding.
        if (width > 1) {
            out += "\x1b[";
            out += std::to_string(width - 1);
            out += 'C';   // CUF clamps at the right margin by spec
        }
        // Cosmetics reset — hand the USER's cursor back. Without this a
        // streaming override (bar + phase color) outlives the caret: the
        // wire keeps DECSCUSR 6 + OSC-12 after parking, and tmux's
        // copy-mode cursor — which re-emits the pane's latched style —
        // renders a phase-colored bar instead of the user's own block
        // (the reported "doesn't start out as a block").
        emit_caret_cosmetics(out, state.cursor_shape_, state.cursor_color_,
                             0, 0);
        out += ansi::hide_cursor;
        state.cursor_hidden_     = true;
        state.cursor_shown_      = false;
        state.cursor_row_offset_ = 0;
        state.cursor_col_        = width > 1 ? width - 1 : 0;
    }
}

// Ends a frame that opened a sync block. A shown caret goes INSIDE the block:
// Konsole repaints from a snapshot at ?2026l and nothing repaints after, so a
// ?25h that trails the block never shows (#82). A park stays outside, so a
// dropped block can't lose the hide.
void close_frame(std::string& out, bool synchronized_output,
                 const std::optional<ResolvedCaret>& hint,
                 InlineFrameState& state, int content_rows, int term_h,
                 int width) noexcept {
    if (!synchronized_output) {
        emit_caret_epilogue(out, hint, state, content_rows, term_h, width);
        return;
    }
    if (caret_on_screen(hint, content_rows, term_h)) {
        emit_caret_epilogue(out, hint, state, content_rows, term_h, width);
        out += tmux::sync_end();
    } else {
        out += tmux::sync_end();
        emit_caret_epilogue(out, hint, state, content_rows, term_h, width);
    }
}

// Internal byte-emitter for the inline frame path.
//
// This is the actual byte-producer that walks the canvas, diffs
// against `state.prev_cells`, and emits VT sequences. It is NOT a
// public symbol — the only legitimate caller is the witness-chain
// entrypoint `compose_inline_frame` in frame_bytes.cpp, which fuses
// this in-place mutation into the linear FrameBytes capsule so the
// state advance and byte delivery are inseparable.
//
// Direct callers cannot exist outside frame_bytes.cpp because the
// public surface for inline composition is the Witness Chain
// (`InlineFrame<Tag>::render`, which calls `compose_inline_frame`
// which calls THIS function). Bypassing the chain would mean
// composing without proving the shadow matches the wire — exactly
// the corruption surface the chain was built to close. The function
// is declared as a non-static symbol so it links across the TU
// boundary, but it has no public header declaration.
std::pair<std::string, InlineFrameState>
compose_inline_frame_impl(const Canvas& canvas,
                          int content_rows,
                          int term_h,
                          const StylePool& pool,
                          InlineFrameState&& state_in,
                          bool synchronized_output,
                          std::string_view reset_prefix)
{
    // Take ownership of the moved-in state. From here on `state` is
    // a function-local value; mutating it is unobservable to any
    // outside code (the caller gave up the value by &&-move). The
    // result is returned by value at the end — the operation is
    // mathematically a pure `(canvas, state_in) → (bytes, state_out)`
    // even though we write into the local for implementation
    // simplicity.
    InlineFrameState state{std::move(state_in)};
    std::string out;

    const int W = canvas.width();
    if (W <= 0 || content_rows <= 0 || term_h <= 0)
        return {std::move(out), std::move(state)};

#ifdef MAYA_DEBUG_SHADOW_VERIFY
    // Compose precondition: `content_rows` must equal
    // canvas.max_content_row() + 1 — i.e. the caller derived it from
    // content_height(canvas). If they don't match, somebody computed
    // content_rows from a different source (layout's computed height,
    // a hard-coded value, etc.) and the row-emit loop will walk past
    // the actual painted rows, serialising stale cells from previous
    // frames into the live frame. Catch the violation at the source
    // instead of letting it become a ghost-row report from a user.
    {
        const int expected = canvas.max_content_row() + 1;
        if (content_rows != expected) {
            std::fprintf(stderr,
                "\n[maya] COMPOSE PRECONDITION VIOLATED: content_rows=%d "
                "but canvas.max_content_row()+1=%d. The caller must derive "
                "content_rows from content_height(canvas).\n",
                content_rows, expected);
            std::abort();
        }
    }
#endif

    // Width change invalidates the cached cell buffer — row layouts shift.
    // Reset clears prev_width / prev_rows but doesn't free prev_cells; the
    // next resize will reuse the allocation if it's already big enough.
    if (state.prev_width_ != W) {
        state = std::move(state).reset_state();
        // Width-flap memory recovery: if the buffer is dramatically
        // larger than what this frame needs, release the excess so
        // a transient wide-window state can't pin memory forever.
        // shrink_to_fit_if_oversized's 2x threshold prevents
        // allocator thrashing on small steady-state variations.
        state.prev_cells_.shrink_to_fit_if_oversized(
            static_cast<std::size_t>(content_rows)
            * static_cast<std::size_t>(W));
    }

    // Pre-reserve `out` so the per-row ANSI emission doesn't trip a
    // reallocation cascade.  Rough heuristic: 24 bytes per row of pure
    // movement + EL + style switches, plus ~200 bytes for the frame
    // wrapper.
    //
    // Bound the reserve to the ACTUAL emit span, not content_rows. The
    // per-row loop below only visits rows [first_changed, content_rows),
    // and on a steady streaming frame first_changed sits near the bottom
    // (only the tail grows), so the emitted byte count is a handful of
    // rows. Reserving content_rows*24 unconditionally allocated O(content
    // _rows) bytes EVERY frame — ~336 KB per frame on a 14k-row transcript
    // — purely as headroom the frame never fills. That per-frame
    // allocation + zero-touch was a residual linear `cf` cost on a tall
    // turn. first_changed is computed just below, so hoist a cheap
    // upper-bound scan is unnecessary: cap the reserve at the viewport
    // span (term_h) plus a grow margin, which bounds the emit for every
    // streaming frame and is still generous for the rare full-viewport
    // repaint (which never emits more than term_h rows anyway). Fresh /
    // shrink paths reserve their own exact sizes further down.
    {
        const int grow = std::max(0, content_rows - state.prev_rows_);
        const int emit_span_ub = std::min(content_rows, term_h + grow);
        const std::size_t reserve_rows =
            static_cast<std::size_t>(std::max(1, emit_span_ub));
        out.reserve(out.size() + 256 + reserve_rows * 24);
    }

    const uint64_t* cells = canvas.cells();
    const int prev_rows       = state.prev_rows_;
    const int prev_on_screen  = std::min(prev_rows, term_h);
    const int updatable_start = prev_rows - prev_on_screen;
    const int common          = std::min(content_rows, prev_rows);

    const std::size_t need_prev = safe_cells(prev_rows, W);
    const bool have_prev =
        prev_rows > 0 && need_prev != (std::size_t)(-1) &&
        need_prev <= state.prev_cells_.size();
    const uint64_t* prev = have_prev ? state.prev_cells_.data() : nullptr;

    // Locate the first row that actually differs from the cached copy.
    // Rows in scrollback (y < updatable_start) are immutable — skip
    // them. Early-exit on the first hit: the per-row emit below walks
    // from first_changed to content_rows-1 anyway, so the changed-rows
    // count beyond the first hit isn't useful here.
    int first_changed = common;
    if (have_prev) {
        for (int y = updatable_start; y < common; ++y) {
            if (!simd::bulk_eq(cells + static_cast<std::size_t>(y) * W,
                               prev + static_cast<std::size_t>(y) * W,
                               static_cast<std::size_t>(W)))
            {
                first_changed = y;
                break;
            }
        }
    }

    // hide_cursor (DECTCEM ?25l) is RE-ASSERTED on every render call,
    // BEFORE the no-op early-return below, not latched. Inline mode
    // never positions the hardware cursor — the composer's caret is a
    // painted cell — so the real cursor must stay invisible for the
    // whole session, at ALL times including when the frame is idle.
    //
    // Anything OUTSIDE maya can re-show it between our frames: a
    // sandboxed subprocess that thinks it owns a tty and writes ?25h
    // to fd 1, an SSH reconnect, a terminal emulator that resets
    // DECTCEM on its own scroll/SU, or — the case that motivated
    // moving this above the early-return — a mobile terminal (Blink /
    // iSH / Termius on iOS) that shows its own cursor when the soft
    // keyboard or speech-to-text input field opens. If we only emit
    // the hide on content-changing frames, an idle composer (the
    // common state) hits the "nothing to do" return and never re-
    // hides, so the externally-reshown cursor sits there until the
    // next keystroke. Emitting the hide before the early-return
    // guarantees every render tick re-asserts invisibility regardless
    // of whether the canvas changed.
    //
    // ?25l is idempotent at the terminal (hiding an already-hidden
    // cursor is a no-op, paints nothing, moves nothing) so re-emitting
    // it on an otherwise-silent frame is safe: 6 bytes, no flicker. It
    // sits inside the sync wrapper on content frames; on the no-op
    // path it ships alone (no sync markers needed — there is no frame
    // body to make atomic).
    if (first_changed == common && content_rows == prev_rows) {
        // A reset_prefix (HardReset wipe) must never be silently
        // dropped: if the caller asked for a wipe, the wire is in an
        // unknown state and we owe it the wipe even on a no-op diff.
        // Wrap prefix + hide in the sync block so the terminal applies
        // them atomically. (In practice HardReset drops its state, so
        // prev_rows == 0 and this branch is unreachable for resets;
        // the wrapping is defensive against a future caller that
        // passes a prefix with carried state.)
        if (!reset_prefix.empty()) {
            if (synchronized_output) out += tmux::sync_begin();
            out += reset_prefix;
            out += ansi::hide_cursor;
            if (synchronized_output) out += tmux::sync_end();
            state.cursor_hidden_ = true;
            state.cursor_shown_ = false;
            state.cursor_row_offset_ = 0;
            state.cursor_col_ = 0;
        }
        // ── No-op frame cursor discipline ──────────────────────────
        // (Ghost-caret hardening — diagnosis: davidwed — generalized to
        // the hardware caret.)
        //
        // KEEP-STILL fast path: if the last frame SHOWED the cursor at
        // the caret and this frame's hint resolves to the very same
        // cell, emit NOTHING. Any cursor motion (even a same-cell CUP)
        // resets the blink phase on most terminals — an idle loop that
        // re-positioned every tick would pin the native blink solid,
        // defeating the point of the hardware caret. The terminal is
        // left alone to blink at its own cadence; and unlike the
        // painted caret, an idle agentty now schedules ZERO frames for
        // blinking.
        //
        // Note the deliberate trade: the parked path re-asserts ?25l
        // every tick (external ?25h recovery), but the shown path
        // cannot re-assert ?25h without touching the cursor — and a
        // shown cursor is the state external agents converge on anyway
        // (the failure mode "cursor got shown" IS our state). An
        // externally-HIDDEN cursor self-heals on the next content
        // frame's epilogue.
        {
            const int resting = std::min(content_rows, term_h) - 1;
            const int top_row = std::max(0, content_rows - term_h);
            const auto hint =
                resolve_caret_hint(canvas, pool, content_rows, term_h);
            const bool hint_visible =
                hint && hint->y >= top_row && hint->y < content_rows;
            const int want_up  = hint_visible ? resting - (hint->y - top_row) : 0;
            const int want_col = hint_visible ? hint->x : 0;
            if (hint_visible && state.cursor_shown_
                && state.cursor_row_offset_ == want_up
                && state.cursor_col_ == want_col) {
                // Same cell — no movement (motion resets the terminal's
                // blink phase). Cosmetics may still change on a phase
                // flip with identical cells (e.g. idle→awaiting flips
                // the DECSCUSR shape): shape/color escapes move no
                // cursor, so emitting just them preserves keep-still.
                emit_caret_cosmetics(out, state.cursor_shape_,
                                     state.cursor_color_,
                                     hint->shape, hint->color);
                return {std::move(out), std::move(state)};   // keep still
            }
            // Cursor state must change (hint appeared/moved/vanished, or
            // we were parked-hidden). Normalize to the resting row first
            // — the epilogue emits relative to it — then run the shared
            // epilogue. For the parked→parked case this degenerates to
            // exactly the davidwed park: \r + ?25l, re-asserted outside
            // any sync block, every idle tick.
            if (state.cursor_row_offset_ > 0) {
                ansi::write_cursor_down(out, state.cursor_row_offset_);
                state.cursor_row_offset_ = 0;
            }
            emit_caret_epilogue(out, hint, state, content_rows, term_h, W);
        }
        return {std::move(out), std::move(state)};
    }

    // ── Frame open ─────────────────────────────────────────────────────
    // DECAWM-off is emitted once and persisted across frames
    // (state.decawm_off_); state.finalize() restores it on shutdown.
    // hide_cursor is re-asserted here too (see the rationale at the
    // early-return above), inside the sync wrapper so it applies
    // atomically with the rest of the frame body.
    if (synchronized_output) out += tmux::sync_begin();
    // reset_prefix (e.g. HardReset's \x1b[2J\x1b[3J\x1b[H wipe) goes
    // FIRST, inside the sync wrapper, so the destructive clear and the
    // full repaint that follows are presented as a single atomic frame.
    // Without this the wipe shipped as its own write and a slow mobile
    // SSH terminal drew a blank screen, then visibly painted the
    // repaint top-to-bottom as the bytes trickled in.
    out += reset_prefix;
    out += ansi::hide_cursor;
    state.cursor_hidden_ = true;
    // ── Cursor normalization (hardware-caret prologue) ───────────────
    // The previous frame's caret epilogue may have left the physical
    // cursor cursor_row_offset_ rows ABOVE the resting row (at the
    // composer's caret cell). Every path below — diff delta math,
    // case-(B) redraw, Fresh growth — assumes the classic invariant
    // "cursor rests on the previous frame's last wire row". Restore it
    // with one row-relative CUD (cannot clamp: caret row + offset =
    // resting row, which is on-screen by construction) so the whole
    // body operates under the unchanged invariant. A reset_prefix wipe
    // re-anchored the cursor absolutely (\x1b[H) — the offset is moot,
    // just drop it. offset 0 ≡ classic parked frames: zero bytes.
    if (!reset_prefix.empty()) {
        state.cursor_row_offset_ = 0;
    } else if (state.cursor_row_offset_ > 0) {
        ansi::write_cursor_down(out, state.cursor_row_offset_);
        state.cursor_row_offset_ = 0;
    }
    state.cursor_shown_ = false;   // hide just shipped
    state.cursor_col_   = 0;

    // First-ever render (prev_rows == 0). Two distinct sub-cases,
    // differentiated by `state.prev_width`:
    //
    //   (A) prev_width == 0: truly fresh state — startup, or after a
    //       Divergent → Synced transition where the host emitted
    //       \x1b[2J\x1b[3J\x1b[H to clear+home. Cursor is at host's
    //       current position (row 0 after a home, or wherever the
    //       shell left it at startup). Emit serialize() from that
    //       position, growing the frame downward via \r\n's that
    //       scroll the terminal at term_h - 1. Live frame appears
    //       AT the cursor, host content above stays visible (inline
    //       mode convention).
    //
    //   (B) prev_width > 0: a previous frame was emitted and is on
    //       the wire, but force_redraw zeroed prev_rows to signal
    //       "diff state is stale, redraw fresh". Cursor was left at
    //       the previous frame's last row by that last emit (could
    //       be mid-viewport if a shrink had moved it up). Do an
    //       in-place soft redraw: cursor_up(content_rows - 1) to
    //       move up to where the frame's top should land (clamping
    //       at row 0 if it would overshoot), \r, serialize, then
    //       \x1b[J to clear anything below. For the typical
    //       force_redraw case (cursor at viewport row K, content
    //       fits above), cursor lands at K - content_rows + 1,
    //       serialize emits without scrolling, cursor returns to K.
    //       Composer stays at its viewport row — no "rushes to
    //       bottom" jolt, scrollback preserved.
    if (prev_rows == 0) {
        // Audit finding #2 history: an earlier case-(B) implementation
        // emitted serialize() from canvas row 0 with a cursor_up capped
        // at term_h-1; when content_rows > term_h the bottom-edge
        // \r\n's scrolled native scrollback by (content_rows - term_h)
        // rows, leaving a ghost copy of the just-finished turn one
        // screen above the live one. That bug was originally patched
        // by routing oversized force_redraws through a hard
        // \x1b[2J\x1b[3J\x1b[H reset — which fixed the ghost but
        // wiped the host's pre-launch scrollback every time it fired.
        //
        // The current case-(B) emit shape (below) is already scrollback-
        // safe for oversized content: it caps the cursor_up at term_h-1
        // AND caps the serialize at start_row = max(0, content_rows -
        // term_h), so only the last term_h rows of canvas land in the
        // viewport via term_h-1 inter-row \r\n's — exactly filling the
        // existing viewport, zero bottom-edge scrolls, zero scrollback
        // mutation. Rows of the inline frame already above viewport top
        // stay in native scrollback as the stream originally committed
        // them (see the SCOPE CONTRACT comment in the path below).
        //
        // So: no oversized special-case here. The hard \x1b[2J\x1b[3J\x1b[H
        // reset is reserved for the Divergent path (resize / write-fail
        // recovery), where the layout assumptions actually changed and
        // a full repaint from home is the correct response — see
        // the Device's resize handler (src/device/input.cpp).
        if (state.prev_width_ > 0) {
            // Force_redraw case (B): in-place soft redraw.
            //
            // ── SCOPE CONTRACT ──────────────────────────────────────
            //
            // This path is a VIEWPORT-ONLY recovery. It repaints the
            // last `term_h` rows of the canvas in place; any rows of
            // the inline frame that already overflowed into the
            // terminal's native scrollback are NOT re-emitted, and
            // (in inline mode) CANNOT be re-emitted.
            //
            // Rationale: inline mode shares the terminal viewport
            // with the host's pre-existing scrollback (the shell
            // prompts and program output that lived above the
            // composer before agentty started). Any sequence capable
            // of overwriting committed scrollback rows would by
            // definition also overwrite the host's content — there
            // is no "my scrollback" vs "host's scrollback"
            // distinction at the VT level once a row scrolls off the
            // top edge. The cells are owned by the terminal
            // emulator and are immutable to the application.
            //
            // What this path DOES fix:
            //   • Ghost cells inside the live viewport — composer
            //     outline survivors of a stream-finish shrink, stale
            //     status / footer rows below the new content_rows,
            //     SGR residue from a half-written frame.
            //   • prev_cells / wire desync — caller (Runtime::force_redraw)
            //     zeroed prev_rows to mark "diff state is stale,
            //     redraw fresh", so the per-row diff skips its
            //     usual byte-identical fast paths and re-emits
            //     every visible row.
            //
            // What this path DOES NOT fix:
            //   • Scrollback corruption (a stray subprocess wrote
            //     directly to fd 1 mid-frame; a tmux pane swap
            //     mangled the rows above the viewport; a terminal
            //     emulator dropped bytes during a resize). Those
            //     rows are off-viewport, committed, and unreachable.
            //     The user-facing recovery for that case is the
            //     terminal emulator's own redraw (most emulators
            //     bind their own Ctrl-L to a full repaint of the
            //     emulator's local cell grid), or a resize event
            //     that drops agentty into the Divergent path (which
            //     DOES emit \x1b[2J\x1b[3J\x1b[H, wiping scrollback
            //     and starting fresh — deliberately reserved for
            //     resize / write-fail because the scrollback wipe
            //     is destructive to the host's prior output).
            //
            // Callers wiring a user-facing "redraw" hotkey to this
            // path (see agentty's RedrawScreen → the force_redraw effect
            // in update/meta.cpp) should mirror this scope in their
            // user-facing docs so users don't expect Ctrl-L to fix
            // every kind of terminal corruption.
            //
            // ── EMIT SHAPE ──────────────────────────────────────────
            //
            // Cap the emit at the visible viewport (last term_h rows
            // of the canvas) rather than the full content_rows. The
            // frame may extend past viewport top — the tail above the
            // viewport sits in native scrollback exactly as the
            // stream originally committed it, and emitting it again
            // would only (a) push the user's prior viewport content
            // into scrollback via the bottom-edge \r\n scrolls
            // serialize uses to walk rows, and (b) leave a duplicate
            // copy of the just-finished turn one screen above the
            // live one — the "everything gets re-printed from the
            // beginning of the turn" symptom.
            //
            // The viewport-cap is what makes this path scrollback-safe
            // for oversized content (content_rows > term_h) too:
            // start_row clamps to content_rows - term_h, the cursor_up
            // clamps to term_h - 1, serialize emits exactly term_h
            // rows with term_h - 1 inter-row \r\n's — fills the
            // viewport in place, no bottom-edge scroll.
            //
            // ── Shrink case: erase prior frame's top rows ──────────
            //
            // \x1b[J at the tail only erases from the cursor
            // downward. When the new frame is SHORTER than what
            // force_redraw zeroed (state.ghost_rows_above), the rows
            // above the new frame's top still hold the prior frame's
            // content and would stay visible. We pre-position the
            // cursor above the new frame's top, EL-erase one row,
            // and walk down — total erased rows =
            // min(ghost_rows_above, term_h) - emit_rows. Done BEFORE
            // the new-frame emit so the cursor ends in the same
            // place the existing math expects.
            // Cursor's actual physical viewport row at entry: wire_rows - 1.
            // Decisions below must derive from this, NOT from term_h - 1
            // (which assumes the cursor is at the viewport bottom and is
            // wrong whenever the live frame was shorter than the viewport).
            const int wire_rows  = std::min(std::max(1, state.ghost_rows_above_),
                                            term_h);
            const int cursor_row = wire_rows - 1;
            const int emit_rows  = std::min(content_rows, term_h);
            const int start_row  = std::max(0, content_rows - term_h);

            // Where the new frame's top must land for its bottom to coincide
            // with the cursor (cursor_row). If negative, the new frame is
            // taller than the old one — the overwrite below simply extends
            // past the old bottom, and the per-row \r\n advance scrolls the
            // viewport exactly when it actually runs out of room (the same
            // bottom-edge behaviour normal streaming growth produces).
            const int new_top   = cursor_row - (emit_rows - 1);
            const int extra_top = (new_top > 0) ? new_top : 0;

            // ONE relative shape for grow / shrink / exact-fit. The cursor's
            // ABSOLUTE viewport row is unknowable in inline mode (the frame
            // may be anchored mid-screen below host output); every movement
            // must be relative to the cursor. An earlier version of the grow
            // case bottom-anchored instead — cursor_down((term_h-1) -
            // cursor_row) then scroll_n newlines — which implicitly assumed
            // the frame top sat at physical row 0. For a SHORT frame anchored
            // mid-screen (early session, force_redraw before first overflow),
            // CUD clamps at the bottom edge, the newlines over-scrolled, and
            // the repaint landed below the old frame's top rows — leaving
            // stale duplicates above that the next growth committed to native
            // scrollback permanently (the "duplicated rows in scrollback"
            // symptom).
            //
            // Relative version: walk UP to the old frame's top (a distance we
            // know exactly: cursor_row = wire_rows - 1), EL-erase any surplus
            // old rows when the new frame is SHORTER, then serialize the new
            // frame downward from the top. Growth scrolls at the bottom edge
            // naturally; every old row is either overwritten or erased; host
            // content above the frame is never touched. Correct at any
            // anchor row, including 0.
            if (cursor_row > 0)
                ansi::write_cursor_up(out, cursor_row);
            out += '\r';
            for (int i = 0; i < extra_top; ++i) {
                out += "\x1b[K";
                out += '\n';
            }
            serialize(canvas, pool, out, content_rows, start_row);
            out += "\x1b[J";
            state.ghost_rows_above_ = 0;
        } else {
            // Fresh state (A): inline-mode growth from cursor.
            // Leading \r anchors the first row at col 0 — inherited shell
            // cursor may be mid-line (e.g. host left output without a
            // trailing newline, or a `read -p` prompt). serialize() only
            // emits \r\n BETWEEN rows; the first row would otherwise
            // start at whatever column the host left the cursor at.
            out += '\r';
            serialize(canvas, pool, out, content_rows);
            state.ghost_rows_above_ = 0;
        }
        // Frame epilogue (hardware caret or davidwed park) — see
        // emit_caret_epilogue. Emitted before the pathological-size
        // bailout below so every return from this path is disciplined.
        close_frame(out, synchronized_output,
                    resolve_caret_hint(canvas, pool, content_rows, term_h),
                    state, content_rows, term_h, W);
        // Cache the new cell buffer for next frame's comparison.  This is
        // the one path that legitimately needs a full memcpy because
        // prev_cells is empty.  Overflow check protects against pathological
        // inputs (e.g. content_rows = INT_MAX from a buggy auto_height).
        const std::size_t new_size = safe_cells(content_rows, W);
        if (new_size == (std::size_t)(-1)) {
            // Pathological size — drop the cache; caller falls back to
            // Divergent on the next render.
            state = std::move(state).reset_state();
            return {std::move(out), std::move(state)};
        }
        if (state.prev_cells_.size() < new_size) state.prev_cells_.resize(new_size);
        std::memcpy(state.prev_cells_.data(), cells, new_size * sizeof(uint64_t));
        state.prev_width_ = W;
        state.prev_rows_  = content_rows;
        state.wire_cursor_rows_ = std::min(content_rows, term_h);
        ++state.gen_;   // content-advancing: invalidates any outstanding marker
        // Seed per-row hashes for the whole fresh frame and the combine.
        {
            const uint64_t* cbase = state.prev_cells_.data();
            state.row_hashes_.assign(static_cast<std::size_t>(content_rows), 0);
            for (int y = 0; y < content_rows; ++y)
                state.row_hashes_[static_cast<std::size_t>(y)] =
                    hash_row(cbase + static_cast<std::size_t>(y) * W,
                             static_cast<std::size_t>(W), y);
            state.shadow_hash_ = combine_rows(state.row_hashes_);
        }
        return {std::move(out), std::move(state)};
    }

    // A1 shadow-of-wire: pre-resize prev_cells now (before the per-row
    // loop writes to it). Cells we don't touch are guaranteed to match
    // canvas by the inductive invariant (rows < first_changed haven't
    // changed; cells outside [x_first_diff, x_last_diff] in a changed
    // row haven't changed; new rows get a full-row copy in the loop).
    const std::size_t new_total_pre = safe_cells(content_rows, W);
    if (new_total_pre == (std::size_t)(-1)) {
        // Pathological size — drop the cache, fall back to Divergent
        // next frame.
        state = std::move(state).reset_state();
        return {std::move(out), std::move(state)};
    }
    if (state.prev_cells_.size() < new_total_pre)
        state.prev_cells_.resize(new_total_pre);
    // Resize may have moved storage; re-take the pointer. `prev` is
    // const because the diff scan reads from it; we use a separate
    // mutable handle for the shadow writes.
    prev          = state.prev_cells_.data();
    uint64_t* prev_w = state.prev_cells_.data();

    // ── Position cursor at row first_changed, col 0 ────────────────────
    // Cursor is currently at the last row of the previously-rendered
    // frame (prev_rows - 1) at some column inside the content. Move
    // relatively: cursor_up for backward, \r\n for forward (the latter
    // scrolls the terminal at the bottom edge, which is exactly what
    // inline mode wants when growing past the previous bottom).
    const int cursor_row_start = prev_rows - 1;
    {
        const int delta = first_changed - cursor_row_start;
        if (delta < 0) {
            int up = std::min(-delta, prev_on_screen - 1);
            if (up > 0) ansi::write_cursor_up(out, up);
            out += '\r';
        } else if (delta == 0) {
            out += '\r';
        } else {
            // Growing past previous bottom — first \r\n scrolls the
            // terminal, then cursor_down advances within the new region.
            out += "\r\n";
            if (delta > 1) ansi::write_cursor_down(out, delta - 1);
        }
    }

    // DECAWM off for the entire frame body — emitted once, not per row.
    // Persisted across frames via state.decawm_off; only the first frame
    // (or first after a state.reset()) pays the 5-byte escape.
    if (!state.decawm_off_) {
        out += "\x1b[?7l";
        state.decawm_off_ = true;
    }
    uint16_t current_style = UINT16_MAX;

    // ── Per-row, per-cell-span emission ────────────────────────────────
    // Iterate from first_changed down through max(content_rows-1,
    // last_changed_in_common). Unchanged rows in the middle just advance
    // the cursor (\r\n) without emitting any cell content.
    const int last_row_to_visit = content_rows - 1;
    for (int y = first_changed; y <= last_row_to_visit; ++y) {
        if (y > first_changed) out += "\r\n";  // advance to row y, col 0

        const uint64_t* cur_row  = cells + static_cast<std::size_t>(y) * W;
        const uint64_t* prev_row = (have_prev && y < prev_rows)
                                 ? prev + static_cast<std::size_t>(y) * W
                                 : nullptr;
        const bool is_new_row    = (y >= prev_rows);

        // Rows about to be committed to native scrollback by this
        // frame's bottom-edge \r\n's. Once any of these rows scrolls
        // off, the emulator captures the bytes currently on the wire
        // for that row — forever, into its own scrollback that we
        // cannot rewrite. If prev_cells says "matches canvas" but the
        // wire actually shows old content (stale shadow, wide-char
        // boundary mis-snap, layout shift between frames), that
        // stale content commits.
        //
        // Defence: force a full-row re-emit for every row that this
        // frame will scroll off, plus the topmost still-visible row
        // (next frame's growth will scroll it). Previously the
        // heuristic protected only the topmost about-to-commit row
        // (single y == content_rows - term_h); if the frame grew by
        // more than one row in a single frame, the lower
        // about-to-commit rows still leaked.
        //
        // Rows scrolled off THIS frame: [prev_visible_top, new_visible_top).
        // Topmost still-visible row (next frame's risk): y == new_visible_top.
        // Union: [prev_visible_top, new_visible_top].
        const int new_visible_top = std::max(0, content_rows - term_h);
        const int prev_visible_top = std::max(0, prev_rows - term_h);
        const bool will_scroll_off =
            (content_rows >= term_h)
            && (y >= prev_visible_top)
            && (y <= new_visible_top);

        // Compatibility repaint: redraw each changed row IN FULL from column
        // 0, rather than positioning the cursor to the changed sub-span. The
        // sub-span update needs mid-row cursor moves (cursor-forward / CHA)
        // that some terminals — notably Zed's — mis-track, landing the update
        // at the wrong column and tearing the frame. A full-row redraw uses
        // only \r + content, exactly like the (working) static print path.
        // Auto-enabled on Zed; force anywhere with MAYA_COMPAT_REPAINT=1.
        static const bool full_row = [] {
            if (std::getenv("MAYA_COMPAT_REPAINT")) return true;
            const char* tp = std::getenv("TERM_PROGRAM");
            return tp && std::string_view{tp} == "zed";
        }();

        // Find the changed sub-span. For new rows (no prev), this is
        // [first_non_blank, last_non_blank+1). For the will-scroll-off
        // row, force a full-row consideration so any wire-vs-canvas
        // skew is corrected before the row leaves our control.
        const int row_first_diff = first_diff_col(cur_row, prev_row, W);

        // Widen the emit to the whole row (col 0 .. W-1, written with only
        // \r + content — no mid-row cursor moves) when EITHER:
        //   • the row is about to scroll off — re-emit in full to correct
        //     any wire-vs-canvas skew before it commits to native
        //     scrollback (unconditional, even if the diff says unchanged); OR
        //   • compat-repaint is on AND the row actually changed this frame.
        //
        // It is the `&& row_first_diff < W` that matters: a full_row repaint
        // must NOT force-emit rows the diff calls identical. Skipping an
        // unchanged row is already Zed-safe — the per-row `\r\n` advance
        // above steps the cursor whether or not we emit content, so vertical
        // tracking is unaffected and only mid-row HORIZONTAL moves (the ones
        // Zed mis-tracks) are avoided. Without this guard, full_row repainted
        // every visited row every frame, turning a one-cell change into a
        // whole-frame re-emit (bench_live on Zed: 3137 B/frame vs 96 B/frame).
        const bool whole_row = will_scroll_off || (full_row && row_first_diff < W);
        const int x_first_diff = whole_row
                                 ? 0
                                 : snap_first_diff_left(row_first_diff,
                                                        cur_row, prev_row, W);

        if (x_first_diff < W) {
            const int x_last_diff_raw = whole_row
                                       ? W - 1
                                       : last_diff_col(cur_row, prev_row, W);
            const int x_last_diff    = whole_row
                                       ? W - 1
                                       : snap_last_diff_right(x_last_diff_raw,
                                                              cur_row, prev_row, W);
            const int x_last_visible = canvas.last_content_col(y);

            // Emit cells through the last *visible* differing column.
            // If the tail of the diff is "current row went blank where
            // prev had content", we don't print blanks — we let EL
            // clean it up below.
            const int x_end_emit = std::max(x_first_diff,
                                            std::min(x_last_diff + 1,
                                                     x_last_visible + 1));

            // Cursor is at col 0 (we just emitted \r or \r\n at row
            // start). We move it forward to x_first_diff exactly once
            // — for either the cell-emit path or the EL-only path —
            // so we never overwrite unchanged cells at the start of
            // the row.
            const bool need_el   = is_new_row || x_last_diff > x_last_visible;
            const bool need_emit = x_end_emit > x_first_diff;

            if (need_emit || need_el) {
                write_cursor_forward(out, x_first_diff);
            }
            if (need_emit) {
                emit_cell_run(canvas, pool, y, x_first_diff, x_end_emit,
                              current_style, out);
                // cursor now at col x_end_emit — BUT with DECAWM off,
                // an emit that reached col W-1 leaves the cursor AT W-1
                // rather than advancing to col W. The EL branch below
                // accounts for this so the rightmost cell isn't erased.
            }
            if (need_el) {
                // EL erases from cursor to end of line, preserving
                // cells before the cursor. Reset SGR before EL so the
                // erased region inherits no attributes from the last
                // emitted cell.
                if (current_style != 0) {
                    pool.close_link(current_style, out); out.append(pool.sgr(0));
                    current_style = 0;
                }
                // DECAWM-off precondition: if the emit filled through
                // col W-1, cursor sits AT W-1 — \x1b[K from there would
                // erase the cell we just painted. Skip EL in that case:
                // there's nothing to erase to the right of the cursor
                // (cursor IS at the right edge). If the row was empty
                // (need_emit=false), cursor is at x_first_diff which is
                // strictly < W by the entry guard, so EL is safe.
                const bool cursor_at_right_edge =
                    need_emit && (x_end_emit >= W);
                if (!cursor_at_right_edge) {
                    out += "\x1b[K";
                }
            }

            // A1 shadow-of-wire: update prev_cells for the cells the
            // wire now shows. For a common-range row, only
            // [x_first_diff, x_last_diff+1) can differ; cells outside
            // that range matched prev by the definition of
            // first_diff_col / last_diff_col. For a new row prev_cells
            // doesn't have valid content yet, so copy the whole row.
            if (is_new_row) {
                std::memcpy(prev_w + (std::size_t)y * W,
                            cur_row,
                            (std::size_t)W * sizeof(uint64_t));
            } else {
                const std::size_t lo = (std::size_t)x_first_diff;
                const std::size_t hi = (std::size_t)x_last_diff + 1;
                std::memcpy(prev_w + (std::size_t)y * W + lo,
                            cur_row + lo,
                            (hi - lo) * sizeof(uint64_t));
            }
        } else if (is_new_row) {
            // All-blank new row — prev_cells has no entry yet. Copy the
            // (blank-packed) row so next frame's bulk_eq sees
            // blank-vs-blank match instead of zero-vs-default_cell.
            std::memcpy(prev_w + (std::size_t)y * W,
                        cur_row,
                        (std::size_t)W * sizeof(uint64_t));
        }
        // else: common-range identical row — prev_cells already
        // matches by induction; nothing to update.
    }

    // DECAWM is NOT restored here — it persists off across frames via
    // state.decawm_off. The next frame's diff path skips re-emitting
    // \x1b[?7l because state.decawm_off is already true. On shutdown
    // (or width change → state.reset()) the owner calls
    // state.finalize(out) to restore.
    //
    // We DO NOT emit ansi::reset here — it has to happen AFTER the
    // shrink path below, otherwise the shrink's re-emit of the bottom
    // row would believe the wire is still in `current_style` while
    // ansi::reset has just moved the wire to id 0. The mismatch would
    // cause the re-emit to skip the SGR transition for the first
    // styled run, drawing those cells in default style.

    // ── Shrink: erase rows past the new content_rows ───────────────────
    // The per-row loop left the cursor at row `last_row_to_visit`
    // (= content_rows - 1) at SOME column — specifically the column
    // where the row's last emit landed it (x_end_emit if the loop
    // emitted, x_first_diff if it only did \x1b[K, col 0 if the row
    // was unchanged-in-common and only \r\n was emitted). \x1b[J
    // wipes from THAT column through end of screen, which is what we
    // want for the rows below, but on the bottom row itself it
    // preserves cells [0, cursor_col) and erases [cursor_col, W).
    // That's correct only if `cursor_col` ≥ last_content_col + 1.
    // The middle-slice-only-changed case (need_el false,
    // x_end_emit < x_last_visible + 1) violates that and erases real
    // content from the bottom row.
    //
    // Make the precondition local: re-anchor the cursor to col 0 with
    // \r, re-emit the bottom row's visible content (idempotent — same
    // cells already on the wire), then \x1b[J. The re-emit positions
    // the cursor exactly at last_content_col + 1; \x1b[J then erases
    // a guaranteed-empty tail on the bottom row plus every row below
    // it. Cost: O(W) bytes for the bottom row at most, paid only when
    // the frame actually shrinks (rare).
    //
    // We also need to keep prev_cells consistent with this re-emit.
    // The bottom row was already shadowed in the per-row loop above
    // (rows < prev_rows hit the `is_new_row=false` path; new rows hit
    // the full-row copy). Re-emitting writes the SAME bytes the diff
    // already wrote, so prev_cells is correct without further action.
    if (content_rows < prev_rows) {
        out += '\r';
        const int last_visible = canvas.last_content_col(last_row_to_visit);
        if (last_visible >= 0) {
            emit_cell_run(canvas, pool, last_row_to_visit,
                          0, last_visible + 1,
                          current_style, out);
        }
        // Reset SGR before erase so erased cells inherit no attrs
        // from the last emitted cell.
        if (current_style != 0) {
            pool.close_link(current_style, out); out.append(pool.sgr(0));
            current_style = 0;
        }
        // Now position cursor and erase the rows that the new (shorter)
        // frame no longer covers. There are two cases governed by where
        // the emit left the cursor under DECAWM-off semantics:
        //
        //   (a) last_visible < W-1 (or empty row):
        //       Cursor naturally landed at col last_visible+1 < W
        //       (or col 0 from the leading \r if the row was empty).
        //       \x1b[J from there erases the bottom row's tail plus
        //       every row below. Cursor stays at (last_row_to_visit,
        //       last_visible+1) — the next compose's diff math assumes
        //       cursor row == prev_rows - 1, which holds.
        //
        //   (b) last_visible == W-1 (row fully painted):
        //       Cursor is stuck AT col W-1 (DECAWM-off doesn't advance
        //       past the right edge, ECMA-48 §8.3.118). \x1b[J here
        //       would erase the rightmost cell — the last-column
        //       corruption symptom on full-width content (code-block
        //       right borders, full-width rules). Instead walk down
        //       one row with \r\n, \x1b[J from there (which doesn't
        //       touch last_row_to_visit at all), then \x1b[A back to
        //       restore the cursor-row invariant the next compose
        //       depends on. \n at row `last_row_to_visit + 1 = content_rows`
        //       won't scroll: prev_rows > content_rows means at least
        //       row content_rows existed in the prior frame, so it's
        //       inside the viewport (prev_rows ≤ term_h holds because
        //       wire_cursor_rows clamps prev_rows at term_h on every
        //       compose).
        if (last_visible < W - 1) {
            out += "\x1b[J";
        } else {
            out += "\r\n\x1b[J\x1b[A";
        }
    }

    // SGR reset at the tail: drop any residual style state so the
    // next compose starts from a known floor (current_style sentinel
    // becomes UINT16_MAX on entry; the wire is at id 0). Always safe
    // — the shrink path above already reset to 0 when needed.
    if (links_used()) out += "\x1b]8;;\x1b\\";   // and any OSC 8 link
    out += ansi::reset;   // drop residual SGR

    // ── Frame epilogue: hardware caret or parked hide ────────────────
    // (Ghost-caret hardening — diagnosis credit: davidwed, maya PR #7 —
    // generalized into emit_caret_epilogue; see its header comment for
    // the full rationale. Short form: the frame's byte stream leaves
    // the physical cursor at an arbitrary column of the resting row;
    // with a canvas cursor hint we move it to the caret cell and SHOW
    // it (the composer's caret becomes the terminal's own cursor);
    // without one we park at col 0 + re-hide. Both outside the sync
    // wrapper so a rollback can't eat them; both row-relative so no
    // anchor assumption and no T1 violation.)
    close_frame(out, synchronized_output,
                resolve_caret_hint(canvas, pool, content_rows, term_h),
                state, content_rows, term_h, W);

    // ── Commit: prev_cells is already up-to-date ───────────────────────
    //
    // A1 shadow-of-wire: the per-row loop wrote each emitted slice into
    // prev_cells as it ran, so the bulk end-of-function memcpy that
    // older versions did here is no longer needed. Rows in
    // [0, first_changed) were byte-identical to begin with and are
    // untouched. Rows in [first_changed, content_rows) had their
    // canvas-visible portion copied into prev_cells inside the loop.
    // The buffer was pre-resized before the loop started, so writes
    // always landed in valid storage.
    //
    // Correctness: next frame's bulk_eq walks prev_cells[y][0..W]
    // against canvas[y][0..W] for y < min(content_rows, prev_rows).
    // By induction every cell in that range either matched last frame
    // (untouched here) or was just written from canvas (this frame's
    // shadow update) — so prev_cells equals the wire content.
    //
    // Perf: instead of O((content_rows - first_changed) × W × 8) bytes
    // memcpy'd unconditionally, the actual cost is the sum of the
    // changed-slice widths per visited row plus a full-row copy for
    // new rows. Sparse streaming frames (one line in a long transcript
    // changing) drop from "copy every row below first_changed" to
    // "copy a few hundred bytes per changed row".
    state.prev_width_ = W;
    state.prev_rows_  = content_rows;
    state.wire_cursor_rows_ = std::min(content_rows, term_h);
    ++state.gen_;   // content-advancing: invalidates any outstanding marker

    // ── Production shadow-of-wire hash (incremental) ───────────────────
    //
    // Per-row hashes are maintained across frames in state.row_hashes_;
    // shadow_hash_ is their XOR combine. This frame only changed rows
    // [first_changed, content_rows) (the per-row loop wrote those into
    // prev_cells), so only those rows need re-hashing. Rows below
    // first_changed are byte-identical at the same y, so their cached
    // hashes are still valid. verify_shadow re-folds this array before
    // the next compose (O(rows) XORs) and demotes to Divergent on
    // mismatch — the same corruption guard, ~8x cheaper than re-reading
    // every cell, and the UPDATE here is O(changed rows x width) instead
    // of O(content_rows x width).
    //
    // If the row-hash array is out of lockstep with the prior frame
    // (first compose after a path that didn't maintain it), fall back to
    // a full rebuild so the invariant re-establishes cleanly.
    {
        const uint64_t* shadow_base = state.prev_cells_.data();
        const bool in_lockstep =
            state.row_hashes_.size() == static_cast<std::size_t>(prev_rows);
        if (in_lockstep) {
            // INCREMENTAL COMBINE. shadow_hash_ is the XOR-fold of every
            // row hash; XOR is its own inverse, so the fold is maintained
            // in O(changed rows) instead of the O(content_rows) full
            // re-fold combine_rows() would cost. On a long streaming turn
            // only the tail rows change each frame (first_changed sits
            // near content_rows-1), so a full re-fold XORed all
            // ~content_rows hashes every frame — the linear `cf` growth
            // that pinned a tall transcript at ~3ms/frame. Here we:
            //   1. XOR OUT every tail row that is being dropped by a
            //      shrink (rows [content_rows, prev_rows) leave the fold),
            //   2. XOR OUT the old hash of each row we are about to
            //      re-hash, re-hash it, then XOR IN the new hash.
            // The result is bit-identical to combine_rows() over the
            // final array (proven by the XOR associativity the design
            // note at combine_rows documents), at O(changed) cost.
            uint64_t h = state.shadow_hash_;
            // Shrink: fold out the tail rows that no longer exist. Do this
            // BEFORE the resize so the stale hashes are still readable.
            for (int y = content_rows; y < prev_rows; ++y)
                h ^= state.row_hashes_[static_cast<std::size_t>(y)];
            // Resize the array to the new row count (shrink drops tail
            // hashes; grow leaves new slots to be filled by the loop).
            state.row_hashes_.resize(static_cast<std::size_t>(content_rows));
            const int begin = std::min(first_changed, content_rows);
            for (int y = begin; y < content_rows; ++y) {
                // Grown rows (y >= prev_rows) had no prior hash in the
                // fold, so there is nothing to XOR out for them; the
                // resize left their slot default-constructed (0), and
                // 0 is the XOR identity, so the unconditional xor-out
                // below is a harmless no-op for new rows.
                h ^= state.row_hashes_[static_cast<std::size_t>(y)];
                const uint64_t nh =
                    hash_row(shadow_base + static_cast<std::size_t>(y) * W,
                             static_cast<std::size_t>(W), y);
                state.row_hashes_[static_cast<std::size_t>(y)] = nh;
                h ^= nh;
            }
            state.shadow_hash_ = h;
        } else {
            state.row_hashes_.assign(static_cast<std::size_t>(content_rows), 0);
            for (int y = 0; y < content_rows; ++y)
                state.row_hashes_[static_cast<std::size_t>(y)] =
                    hash_row(shadow_base + static_cast<std::size_t>(y) * W,
                             static_cast<std::size_t>(W), y);
            state.shadow_hash_ = combine_rows(state.row_hashes_);
        }
    }

    // ── Shadow-of-wire invariant check (debug builds only) ─────────────
    //
    // The single most load-bearing invariant in the inline renderer:
    // `prev_cells[y][x]` must, at the end of every compose, equal
    // `canvas.cells()[y*W + x]` for every (x, y) the wire is about
    // to show — i.e. rows in [updatable_start, content_rows).
    //
    // Every historical "corrupted rows" bug in this file's git log
    // was a version of THIS invariant being temporarily violated:
    // over-commit_scrollback shift, cells_max_y heuristic stale,
    // streaming-markdown cache aliasing, empty-CodeBlock ghost,
    // partial-write residue inflation — all of them showed up
    // downstream as either a `simd::bulk_eq` returning "unchanged"
    // for a row that did change (frame freezes) or returning
    // "changed" for a row that didn't (cursor walks redundantly,
    // emitting bytes that drift the wire away from the shadow).
    //
    // Compile-time gate: define MAYA_DEBUG_SHADOW_VERIFY to enable
    // (off by default; dev/CI builds only). When on, this loop walks
    // every viewport-visible cell and aborts on the first mismatch
    // with a precise (y, x) trace, turning a class of "intermittent
    // ghost rows in production" into immediate test failures.
    //
    // Performance: O(prev_on_screen × W) u64 compares per frame, fully
    // vectorisable. ~1μs on a 200×80 viewport. Never compiled into
    // release.
#ifdef MAYA_DEBUG_SHADOW_VERIFY
    {
        const uint64_t* shadow = state.prev_cells_.data();
        const int verify_start = std::max(0, state.prev_rows_ - prev_on_screen);
        for (int y = verify_start; y < content_rows; ++y) {
            for (int x = 0; x < W; ++x) {
                const uint64_t s = shadow[static_cast<std::size_t>(y) * W + x];
                const uint64_t c = cells [static_cast<std::size_t>(y) * W + x];
                if (s != c) {
                    std::fprintf(stderr,
                        "\n[maya] SHADOW-OF-WIRE INVARIANT VIOLATED "
                        "at (y=%d, x=%d): shadow=0x%016llx canvas=0x%016llx "
                        "first_changed=%d content_rows=%d prev_rows_in=%d W=%d\n",
                        y, x,
                        static_cast<unsigned long long>(s),
                        static_cast<unsigned long long>(c),
                        first_changed, content_rows, prev_rows, W);
                    std::abort();
                }
            }
        }
    }
#endif
    return {std::move(out), std::move(state)};
}

} // namespace maya
