// src/device/input.cpp — resize and input: size, bytes -> events.
#include "internal.hpp"

namespace maya::detail {

void Device::handle_resize() {
    Size new_size;
    if (alt_terminal_) {
        new_size = alt_terminal_->size();
    } else if (inline_terminal_) {
        new_size = inline_terminal_->size();
    } else {
        return;
    }

    if (new_size != size_) {
        const int prev_w = size_.width.raw();
        size_ = new_size;
        ++resize_generation_;
        render_ctx_.width      = size_.width.raw();
        render_ctx_.height     = size_.height.raw();
        render_ctx_.generation = resize_generation_;

        // Width vs height-only resize have very different costs in
        // inline mode:
        //
        //   • Width change   — the prev-frame cell grid is invalid
        //     (every row's wrap points and column layout shift). The
        //     row-diff can't reuse any of it, so we HardReset: next
        //     render emits \x1b[2J\x1b[3J\x1b[H and repaints fresh.
        //     The wipe takes the terminal's scrollback; a host with a
        //     replay source gets its history back, rewrapped at the new
        //     width, in the same frame (render/scrollback_replay.hpp).
        //
        //   • Height-only change — same width means every cached row
        //     is still byte-valid; only the viewport's vertical extent
        //     moved. This is the common case on mobile: the soft
        //     keyboard / dictation field opening or closing changes
        //     the terminal HEIGHT, not its width. A HardReset here
        //     would needlessly wipe the screen and force a full
        //     top-to-bottom repaint (the symptom the user saw on
        //     iPhone). Instead demote to Stale: the next render runs
        //     compose's case-(B) soft redraw — walk the cursor up,
        //     repaint the visible viewport in place, erase below.
        //     No \x1b[2J\x1b[3J\x1b[H, no scrollback wipe, host
        //     content above the viewport is preserved, and the
        //     repaint is bounded to the rows that actually moved.
        //
        // Fullscreen always collapses to Divergent (a width-independent
        // full repaint is cheap there because the alt-screen owns the
        // whole grid and there's no host scrollback to preserve).
        const bool width_changed = (prev_w != size_.width.raw());
        if (width_changed) replay_owed_ = true;
        fs_coherence_ = coherent::Divergent{};
        in_coherence_ = std::visit(
            [width_changed](auto&& arm) -> inline_frame::InlineCoherence {
                using T = std::decay_t<decltype(arm)>;
                if constexpr (std::is_same_v<T,
                        inline_frame::InlineFrame<inline_frame::Synced>>) {
                    return width_changed
                        ? inline_frame::InlineCoherence{std::move(arm).demote_to_hard_reset()}
                        : inline_frame::InlineCoherence{std::move(arm).demote_to_stale()};
                } else if constexpr (std::is_same_v<T,
                        inline_frame::InlineFrame<inline_frame::Stale>>) {
                    // Already Stale: a width change can no longer reuse
                    // the (zeroed) prev grid, so escalate; a height-only
                    // change stays Stale and rides the same case-(B)
                    // soft redraw.
                    return width_changed
                        ? inline_frame::InlineCoherence{std::move(arm).escalate_to_hard_reset()}
                        : inline_frame::InlineCoherence{std::move(arm)};
                } else if constexpr (std::is_same_v<T,
                        inline_frame::InlineFrame<inline_frame::Fresh>>) {
                    // Resize before first render: the wire hasn't seen
                    // anything from us yet. Stay Fresh; the next render's
                    // case (A) paints at the new dimensions from the
                    // cursor's current position without disturbing the
                    // host content above.
                    return std::move(arm);
                } else if constexpr (std::is_same_v<T,
                        inline_frame::InlineFrame<inline_frame::HardReset>>) {
                    return std::move(arm);   // already in hard-reset
                } else if constexpr (std::is_same_v<T,
                        inline_frame::InlineFrame<inline_frame::Empty>>) {
                    return std::move(arm);   // nothing on screen yet
                } else {
                    // Sealed: finalize already ran; resize is a no-op.
                    return std::move(arm);
                }
            }, std::move(in_coherence_));
    }
}

// ============================================================================
// Device::read_events — read and parse terminal input
// ============================================================================

auto Device::read_events() -> Result<std::vector<Event>> {
    std::vector<Event> result;

    // Replay events that arrived during create()'s cursor-position query
    // (a keypress interleaved with the DSR reply) ahead of fresh input.
    if (!startup_events_.empty()) result.swap(startup_events_);

    if (alt_terminal_) {
        MAYA_TRY_DECL(auto data, alt_terminal_->read_raw());
        if (!data.empty()) {
            std::FILE* const lf = input_log();
            if (lf) log_input_bytes(lf, data);
            for (auto& event : parser_.feed(data)) {
                if (lf) log_input_event(lf, event);
                result.push_back(std::move(event));
            }
            if (lf) {
                if (const int acks = parser_.peek_acks(); acks) std::fprintf(lf, "            acks pending %d\n", acks);
                if (parser_.has_pending()) std::fputs("            (partial sequence held)\n", lf);
                std::fflush(lf);
            }
        }
    } else if (inline_terminal_) {
        io_log("read_raw begin");
        MAYA_TRY_DECL(auto data, inline_terminal_->read_raw());
        io_log("read_raw end bytes=%zu", data.size());
        if (!data.empty()) {
            std::FILE* const lf = input_log();
            if (lf) log_input_bytes(lf, data);
            for (auto& event : parser_.feed(data)) {
                if (lf) log_input_event(lf, event);
                result.push_back(std::move(event));
            }
            if (lf) {
                if (const int acks = parser_.peek_acks(); acks) std::fprintf(lf, "            acks pending %d\n", acks);
                if (parser_.has_pending()) std::fputs("            (partial sequence held)\n", lf);
                std::fflush(lf);
            }
        }
    }
    dedup_clipboard_pastes(result);
    io_log("read_events -> %zu events", result.size());
    return ok(std::move(result));
}

// Drop the DUPLICATE clipboard-read reply that a kitty terminal inside tmux
// produces, without ever swallowing part of a real paste.
//
// In tmux we must send OSC 5522 (kitty image) AND OSC 52 (text) because kitty
// can't be identified there; a kitty outer terminal answers BOTH, so the same
// clipboard arrives twice and would paste twice.
//
// This used to drop any PasteEvent landing within 250 ms of the previous one.
// That is not a duplicate test — it is a rate limit, and it silently ate real
// data: the parser ships a large paste as SEVERAL PasteEvent chunks (the
// kMaxOscLen streaming path, and one per OSC 5522 DATA burst), which arrive
// milliseconds apart by construction. A 100 KB image came through as its
// first 4 KB chunk and nothing else — the chip read "4 KB", the rest was
// dropped, and because the PNG header was in that first chunk everything
// downstream still looked like a valid (merely truncated) image.
//
// The two cases are distinguishable by CONTENT, not by timing: the duplicate
// reply carries the SAME bytes, while a continuation chunk carries different
// bytes. So dedup on content identity within the window. Two genuinely
// identical pastes 250 ms apart are indistinguishable from the tmux double-
// answer even in principle, and dropping one of those is the same behaviour
// as before; a human cannot paste the same buffer twice that fast anyway.
void Device::dedup_clipboard_pastes(std::vector<Event>& events) {
    constexpr auto kWindow = std::chrono::milliseconds(250);
    const auto now = std::chrono::steady_clock::now();
    std::vector<Event> kept;
    kept.reserve(events.size());
    for (auto& ev : events) {
        if (auto* pe = std::get_if<PasteEvent>(&ev)) {
            const bool in_window =
                last_paste_at_.time_since_epoch().count() != 0
                && now - last_paste_at_ < kWindow;
            if (in_window && pe->content == last_paste_content_) {
                last_paste_at_ = now;   // keep sliding so a 3rd copy also drops
                continue;               // swallow the duplicate answer
            }
            last_paste_at_      = now;
            last_paste_content_ = pe->content;
        }
        kept.push_back(std::move(ev));
    }
    events = std::move(kept);
}

// ============================================================================
// Device::flush_timeouts — flush parser timeout events
// ============================================================================

auto Device::flush_timeouts() -> std::vector<Event> {
    std::vector<Event> result;
    std::FILE* const lf = input_log();
    for (auto& ev : parser_.flush_timeout()) {
        if (lf) { std::fputs("            timeout resolved:\n", lf); log_input_event(lf, ev); std::fflush(lf); }
        result.push_back(std::move(ev));
    }
    return result;
}

// ============================================================================
// Device::render — render an element tree to the terminal
// ============================================================================
// Fullscreen: RenderPipeline type-state machine (Idle→Cleared→Painted→Opened→Closed)
// Inline: compose_inline_frame (row-diff, scrollback-preserving)

} // namespace maya::detail
