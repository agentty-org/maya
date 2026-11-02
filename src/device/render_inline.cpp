// src/device/render_inline.cpp — the inline path: row-diff under the scrollback.
#include "internal.hpp"

namespace maya::detail {

// The wire gate: drain what the tty refused last frame and decide whether
// to coalesce this one. A value means "no frame this tick" (the status to
// return); nullopt means go on and compose.
std::optional<Status> Device::inline_wire_gate() {
    // ── Adaptive wire coalescing: congestion sample ───────────────
    // Residue on entry = the wire couldn't take last frame in full →
    // a congestion sample of 1.0; a clean entry = 0.0. The EWMA
    // (α=0.25) smooths transient hiccups into a stable [0,1] estimate
    // that drives the coalesce interval below. Cheap, allocation-free,
    // computed once per render. See the field doc in app.hpp.
    static const bool coalesce_enabled =
        (std::getenv("MAYA_NO_COALESCE") == nullptr);
    const bool congested_now = writer_->has_residue();

    // ── Backpressure via non-blocking writer ───────────────────────
    // Output fd is O_NONBLOCK (set by Writer ctor). On a congested
    // tty the previous frame may have left bytes in the writer's
    // residue buffer. Drain those first; if the wire still won't
    // accept them, defer the new frame entirely — DO NOT compose,
    // because compose_inline_frame would update prev_cells to
    // reflect a frame the wire hasn't received, breaking the diff
    // invariant on the next paint.
    //
    // Unlike the older poll(POLLOUT) skip, this can't run away into
    // a feedback loop: prev_cells never lies, so when residue
    // finally drains the next compose produces the same bounded
    // diff (canvas vs wire) it would have on a fast tty. The cost
    // of a deferred render is one event-loop iteration of delay,
    // not an inflated next-frame cost.
    if (writer_->has_residue()) {
        auto d = writer_->try_drain_residue();
        if (!d) {
            if (d.error().kind == ErrorKind::WouldBlock) {
                // Same contract as the coalesce gate below: this call
                // did not compose, so the caller still owes a paint.
                // has_pending_writes() also reports true here (there IS
                // residue), but say it explicitly so the caller needs
                // only one rule: keep rendering while a frame is owed.
                coalesced_last_render_ = true;
                return ok();   // wire still backed up; retry next tick
            }
            // Hard I/O error — toss the residue and demote inline
            // coherence to HardReset so the next render does a
            // full reset.
            writer_->discard_residue();
            in_coherence_ = std::visit(
                [](auto&& arm) -> inline_frame::InlineCoherence {
                    using T = std::decay_t<decltype(arm)>;
                    if constexpr (std::is_same_v<T,
                            inline_frame::InlineFrame<inline_frame::Synced>>)
                        return std::move(arm).demote_to_hard_reset();
                    else if constexpr (std::is_same_v<T,
                            inline_frame::InlineFrame<inline_frame::Stale>>)
                        return std::move(arm).escalate_to_hard_reset();
                    else
                        return std::move(arm);
                }, std::move(in_coherence_));
            return d;
        }
    }

    // ── Adaptive wire coalescing: the compose gate ─────────────────
    // With residue now drained, decide whether to compose THIS frame or
    // coalesce it into the next. The interval scales with measured
    // congestion: 0 ms on a fast wire (coalesce_.congestion ≈ 0 → gate is
    // a no-op, zero behavior change), rising to ~33 ms (≈30 fps) as the
    // wire saturates. A frame arriving inside the interval is skipped;
    // the model keeps advancing between the caller's re-fires, so the
    // next compose is a single CUMULATIVE diff covering every append
    // that landed meanwhile — removing the per-frame CUP+SGR tax that
    // the wire_bytes_bench isolated. The skipped frame is never lost:
    // the caller's needs_render stays set and the residue-retry poll
    // clamp (2-8 ms) re-fires promptly.
    //
    // Safety: this only ever DELAYS a compose, and only while the wire
    // is already too slow to have shown the intermediate frames anyway.
    // It never touches the Witness Chain state, never composes a frame
    // the wire won't receive, and cannot strand the stream (bounded
    // interval + guaranteed re-fire). On a fast/local wire it is inert.
    if (coalesce_enabled) {
        using namespace std::chrono;
        const double now_ms = duration<double, std::milli>(
            steady_clock::now() - coalesce_epoch_).count();
        if (coalesce_.should_coalesce(now_ms, congested_now)) {
            // Tell the caller the frame was NOT painted. Returning ok()
            // alone is indistinguishable from a successful compose, and
            // the loop clears needs_render on ok() — which is exactly how
            // a one-shot change (a theme preview keystroke) got dropped
            // instead of deferred. The run loop re-fires while this is
            // set, so "never lost" is now true by construction rather
            // than by assuming a stream will come along and re-ask.
            coalesced_last_render_ = true;
            return ok();   // coalesce: skip this compose, batch into next
        }
    }
    // Past the gate: this call composes, so nothing is owed.
    coalesced_last_render_ = false;
    return std::nullopt;
}

// Size the canvas for this frame and clear it, keeping the frozen prefix
// (the rows already committed to scrollback) intact: a bounded clear is
// what keeps a long transcript's per-frame cost flat.
void Device::inline_prepare_canvas(int w) {
    constexpr int kMinCanvasHeight = 500;

    // Reasons to (re)allocate the canvas:
    //   — width changed (terminal resize),
    //   — height below the minimum floor,
    //   — height is now ridiculously oversized vs current content.
    //
    // The last condition is load-bearing for long-session perf.
    // canvas_.clear() does streaming_fill over width * height
    // cells every frame; once a tall transcript bumped the
    // canvas to e.g. 6000 rows, clearing 6000 * 100 cells per
    // frame stays expensive forever — even after trim shrunk
    // the actual content back to a few hundred rows.
    //
    // Shrink target: content + 64 (small headroom so a one-turn
    // append doesn't immediately re-grow). Trigger at 1.5x the
    // target instead of 2x so we reclaim memory more eagerly on
    // tool-heavy sessions where the all-time-peak content (a
    // 500-line write that has since trimmed) leaves the canvas
    // sitting at ~1000 rows even though steady-state needs ~200.
    // 1.5x still avoids resize thrash on normal grow/shrink
    // cycles: a turn that adds ~30 rows on top of a steady 400
    // rows lands at 430, well below the 600 trigger.
    const int prev_content_rows = content_height(canvas_);
    const int shrink_target = std::max(kMinCanvasHeight,
                                       prev_content_rows + 64);
    const bool oversized = canvas_.height() * 2 > shrink_target * 3;

    bool canvas_reallocated = false;
    if (canvas_.width() != w
        || canvas_.height() < kMinCanvasHeight
        || oversized) {
        canvas_.set_style_pool(&pool_);
        const int target_h = oversized
            ? shrink_target
            : std::max(kMinCanvasHeight, canvas_.height());
        canvas_.resize(w, target_h);
        canvas_reallocated = true;
    }
    // Recover from any unmatched push_clip in the previous frame's
    // paint pass (e.g. a paint callback that threw past pop_clip).
    canvas_.reset_clips();

    // ── Bounded clear: preserve the immutable frozen prefix ─────────
    // The compose diff only ever reads/rewrites the last `term_h`
    // rows of the canvas; rows above that overflowed into native
    // scrollback and are immutable on the wire. During steady
    // streaming the FROZEN prefix (settled tool cards, prior turns)
    // is byte-stable frame-to-frame: its hash-keyed entries re-blit
    // identical cells to identical rows. Re-clearing + re-blitting
    // those hundreds/thousands of rows every frame is the dominant
    // cost on a tall transcript (a 3000-row `write` result pins the
    // per-frame render at ~11 ms). We therefore clear only the rows
    // at/below a conservative floor and PRESERVE the prefix, letting
    // render_tree's blit_packed_row_cached skip the memcpy for any
    // frozen row whose canvas cells already match (bulk_eq probe;
    // full blit on any mismatch, so correctness never depends on the
    // preservation being valid).
    //
    // SAFETY GATE. The preservation is only sound when the canvas
    // allocation is UNCHANGED since last frame — a resize
    // (reallocation) fills fresh memory, so there is nothing valid
    // to preserve and we must full-clear. Beyond that, the per-row
    // `bulk_eq` guard inside blit_packed_row_cached is the actual
    // correctness backstop: any frozen row whose canvas cells do
    // NOT already match its cached cells (a layout shift moved the
    // prefix, an inter-card gap changed) falls through to a full
    // blit that overwrites the stale bytes. Rows that no entry
    // repaints at all are only ever the all-blank gaps ABOVE the
    // top frozen card, which never carry content the diff emits.
    // The floor is `prev_content_rows - term_h - margin`: strictly
    // above the viewport, so nothing the diff or the
    // scrollback-prefix compare reads is ever preserved (those
    // readers touch [content_rows - term_h, content_rows) and
    // [0, overflow); the margin keeps the preserved region clear of
    // both by construction).
    const int term_h_now = query_term_rows(output_handle_).value();
    int keep_top = 0;
    constexpr int kPreserveMargin = 8;
    // Only preserve when the prior frame ended in the steady Synced
    // state (coherence index 2). Any structural event — a trim /
    // freeze that dropped or reshuffled frozen entries, a
    // scrollback commit, a verify-poison recovery — leaves
    // coherence in a non-Synced arm; in those frames the frozen
    // prefix may have shifted or an entry may have vanished
    // (leaving stale preserved rows that would poison max_y_). A
    // full clear on those frames re-establishes a clean canvas, and
    // the preservation resumes on the next steady frame.
    //
    // EXCEPTION the index check can't see: commit_inline_prefix /
    // commit_inline_overflow shift the shadow while REMAINING
    // Synced. After a large front-trim commit the new tree is N
    // rows shorter; preserving the pre-commit canvas would leave
    // stale rows (including the old composer/status chrome)
    // BELOW the new tree's bottom, inflating content_height() and
    // serializing that chrome into native scrollback — the
    // "whole chrome in the scrollback / rows cut off" corruption.
    // Those paths set canvas_preserve_inhibit_; consume it here
    // (one-shot) and full-clear.
    const bool prev_synced = (in_coherence_.index() == 2
                              || (in_coherence_.index() == 3
                                  && canvas_preserve_stale_ok_))
                          && !canvas_preserve_inhibit_;
    canvas_preserve_inhibit_  = false;
    canvas_preserve_stale_ok_ = false;
    if (!canvas_reallocated
        && prev_synced
        && prev_content_rows > 0
        && term_h_now > 0
        && prev_content_rows > term_h_now) {
        keep_top = prev_content_rows - term_h_now - kPreserveMargin;
        if (keep_top < 0) keep_top = 0;
    }
    if (keep_top > 0) {
        // Cap the cleared tail at the previous frame's painted extent
        // (plus a small margin), NOT the full canvas height. The
        // canvas is grown with ~25% headroom slack above the content
        // so a resize fires rarely; that slack is never painted, yet
        // clearing [keep_top, height_) re-blanked it EVERY frame — an
        // O(rows)/frame fill that scaled with the turn length (the
        // residual `rt` creep the frame profiler pinned on a tall
        // streaming turn: rt/krow held constant at ~0.18ms while cf
        // stayed flat). prev_content_rows bounds the region that could
        // hold content this frame: a GROW extends into the
        // freshly-blank slack (assign() left it blank at the last
        // resize) so uncleared slack there is already correct; a
        // SHRINK abandons rows in [new_content, prev_content) which
        // this cap still covers. The margin absorbs a one-frame grow
        // that outruns prev_content_rows before the next clear.
        const int clear_bottom = prev_content_rows + kPreserveMargin;
        canvas_.clear_below(keep_top, clear_bottom);
    } else {
        canvas_.clear();
    }

}

auto Device::render_inline(const Element& root, int w) -> Status {
    FILE* const prof_out = frame_prof_out();
    const bool prof = prof_out != nullptr;
    const auto t_frame_start = std::chrono::steady_clock::now();
    auto since = [](auto t0) { return ms_since(t0); };
    if (auto skip = inline_wire_gate()) return *skip;


    // ── Inline path: Witness Chain dispatch ─────────────────────────
    // std::visit selects the InlineFrame<Tag>::render whose
    // precondition matches the current coherence state. Each arm
    // returns a new InlineCoherence directly — the type system
    // guarantees every legal transition is encoded by the return
    // type, and that the only path into Synced is through a
    // successful commit_to of a witness-verified compose.
    inline_prepare_canvas(w);

    auto t_rt0 = std::chrono::steady_clock::now();
    // Refresh theme-derived SGR before anything is interned this frame.
    //
    // A bg-less style renders the CANVAS background (build_sgr), so its
    // cached bytes belong to a specific theme. The fullscreen path gets
    // this from RenderPipeline's clear() stage; inline composes its own
    // frame and never touches that pipeline, so without this call the
    // pool keeps whatever it interned before the host published a theme
    // — and inline is the mode agentty actually runs in.
    //
    // A value compare on every frame but the one after a swap.
    //
    // When it DOES fire, both inline wire shadows are silently stale:
    // they diff packed (glyph, style_id) cells, and a retheme changes
    // neither while changing what those ids render as. Every row would
    // compare equal and nothing would be emitted — the terminal keeps
    // the old background. So a swap invalidates both: the grid re-states
    // every row, and the ANSI frame raises retheme_repaint_, which the
    // Synced arm below consumes by committing whatever already scrolled
    // off and demoting to Stale (a repaint of the live viewport, not a
    // re-anchor — re-seeding to Empty would print the transcript twice).
    if (pending_retheme_) {
        pending_retheme_ = false;
        grid_need_full_ = true;
        retheme_repaint_ = true;
        render_detail::clear_component_cache();
        canvas_.clear();
    }
    render_tree(root, canvas_, pool_, theme_, layout_nodes_,
                /*auto_height=*/true);
    double rt_ms = since(t_rt0);

    int ch = content_height(canvas_);
    // Regrow gate keys on the LAYOUT's computed height, NOT on
    // content_height (max painted row). The two diverge exactly when
    // the rows at the canvas boundary are blank (markdown paragraph
    // separators, turn gaps, card padding): the painted-row proxy
    // stays below canvas.height() while the layout needs more rows,
    // so a `ch >= height` precondition never trips and everything
    // past the canvas bottom — composer + status bar, the LAST
    // children of the frame — is silently clipped until a painted
    // row happens to land on the boundary (the "hidden chrome" bug).
    if (!layout_nodes_.empty()) {
        int needed = layout_nodes_[0].computed.size.height.raw();
        if (needed > canvas_.height()) {
            // Grow with HEADROOM (~25%, min 64 rows) rather than a
            // bare +8. A streaming turn's live tail gains rows every
            // frame; with +8 the very next frame's content again
            // exceeded canvas height, so this branch fired a SECOND
            // full render_tree pass on EVERY growing frame (double the
            // layout+paint cost for the whole stream once a thread has
            // passed the kMinCanvasHeight floor). A generous slack lets
            // many frames of growth land in one allocation, so the
            // re-render fires once every ~N frames instead of always.
            // Bounded under the oversized-shrink trigger (1.5x) above
            // so it can never thrash against the shrink path.
            const int headroom = std::max(64, needed / 4);
            canvas_.resize(w, needed + headroom);
            canvas_.clear();
            render_tree(root, canvas_, pool_, theme_, layout_nodes_,
                        /*auto_height=*/true);
            ch = content_height(canvas_);
        }
    }

    if (ch <= 0) {
        // Empty frame — leave coherence as-is; the wire is unchanged.
        return ok();
    }

    // ── Transient monotonic-height hold (composer anti-bounce) ──
    // The inline composer rides content_height, so a 1-row dip in the
    // live transcript bounces it up then back down. The dips are
    // artefacts of the live tree mutating (activity indicator handing
    // off to the first revealed char — a different subtree; the
    // typewriter crossing a block boundary; a tool card collapsing).
    // None are a height change the user should perceive.
    //
    // maya absorbs them AUTONOMOUSLY — no host policy bit, so there is
    // no Cmd-delivery race against the render that shows the dip
    // (an earlier host-driven design lost that race: the bit arrived
    // long after the dip window had passed). The rule is purely local
    // and conservative:
    //   • Only while content FITS the viewport (unpadded <= term_h):
    //     once it overflows, the composer is pinned at the viewport
    //     bottom and a dip can't move it, so the hold disengages.
    //   • Track a running-max `hold_peak_`. When unpadded content dips
    //     below the peak, pad up to the peak so the rendered height
    //     stays put. The peak rises instantly with content.
    //   • DECAY: a dip is only ever transient IF content is on its
    //     way back up. A streaming markdown reveal sits BELOW peak for
    //     many frames while it slowly re-grows (the typewriter reveals
    //     row by row, the StreamingMarkdown widget's height is
    //     non-monotone across block boundaries) — that must stay
    //     bridged the whole time or the composer bounces mid-reveal.
    //     So decay only advances on frames where content is NOT
    //     climbing (this frame <= last frame): a genuine settle/fold
    //     STAYS flat or shrinks, whereas a reveal keeps rising and
    //     resets the counter. Once the lower height has been stable
    //     (non-rising) for kHoldDecayFrames, the shrink is real and we
    //     let the peak fall to it (pad → 0) so idle carries no dead
    //     space. This is a brief bridge across a transient dip, never
    //     a permanent floor.
    {
        // The hold bridges a SMALL transient seam dip only (the
        // indicator→first-text handoff is 1-2 rows). A larger dip is
        // never a seam artefact — it is real content movement (a
        // markdown block committing/expanding, a fold toggle, the
        // reveal clip crossing a block) and MUST show immediately,
        // not be padded then snapped. So the pad is capped: any dip
        // beyond kMaxHoldPad is shown as-is (peak follows content
        // down at once). Without this cap the hold built up 30-40
        // blank rows on a long reveal, then released them in one
        // frame — the "goes up and redraws everything suddenly" jump.
        constexpr int kMaxHoldPad = 2;
        const int prev_pad = render_ctx_.inline_min_content;
        const int unpadded = ch - prev_pad;          // real content this frame
        // Fool-proof scrollback invariant: the pad must be ZERO at
        // the moment content crosses the viewport boundary into
        // native scrollback, so a height change from the pad can
        // never perturb the overflow→scrollback seam. We therefore
        // only ever engage the hold while content sits with at least
        // kMaxHoldPad rows of headroom BELOW the viewport bottom
        // (the `band` ceiling). Within that last band — content
        // about to overflow — the pad is forced to zero, so the
        // crossing always happens at pad=0. Combined with the
        // per-dip cap (pad <= kMaxHoldPad), the pad is structurally
        // incapable of being present when rows cross term_h.
        const int band = size_.height.raw() - kMaxHoldPad;
        int new_pad = 0;
        if (unpadded <= band) {
            if (unpadded >= hold_peak_) {
                // Content caught up to / passed the peak: track it,
                // reset the decay counter, no pad needed.
                hold_peak_       = unpadded;
                hold_decay_      = 0;
            } else if (hold_peak_ - unpadded > kMaxHoldPad) {
                // Dip too large to be a seam transient — it's real
                // content movement. Drop the peak to it instantly so
                // the height shows the true content; no pad, no snap.
                hold_peak_  = unpadded;
                hold_decay_ = 0;
            } else {
                // Small dip (<= kMaxHoldPad) — the seam transient the
                // hold exists for. Bridge it. Only count down toward
                // releasing when content has STOPPED climbing back.
                if (unpadded <= hold_last_unpadded_) {
                    if (++hold_decay_ >= kHoldDecayFrames) {
                        hold_peak_  = unpadded;
                        hold_decay_ = 0;
                    } else {
                        new_pad = hold_peak_ - unpadded;
                    }
                } else {
                    // Still climbing back toward the peak — keep the
                    // bridge and reset decay.
                    hold_decay_ = 0;
                    new_pad     = hold_peak_ - unpadded;
                }
            }
        } else {
            // At/above the headroom band (content near or past the
            // viewport bottom) — disengage and reset so the pad is
            // zero across the overflow seam and a fresh peak starts
            // the next time content settles back into the band.
            hold_peak_  = 0;
            hold_decay_ = 0;
        }
        hold_last_unpadded_ = unpadded;
        // If the pad changed, the tree we just laid out is stale by
        // `new_pad - prev_pad` rows. Re-run layout+paint ONCE so the
        // committed frame already carries the corrected pad — no
        // one-frame flash of the unpadded height ever reaches the
        // wire. The pad rows are emitted by a LAZY component in
        // AppLayout::build (reads inline_min_content at paint time, as
        // the vstack's last child), so this re-render picks up the new
        // value. Cheap: live tail ~1 viewport, frozen prefix
        // cache-blitted; fires only on a dip/decay frame, never in
        // steady streaming.
        if (new_pad != prev_pad) {
            render_ctx_.inline_min_content = new_pad;
            canvas_.reset_clips();
            canvas_.clear();
            render_tree(root, canvas_, pool_, theme_, layout_nodes_,
                        /*auto_height=*/true);
            ch = content_height(canvas_);
        }
    }

    // Typed terminal-rows witness. Re-queried via
    // `query_term_rows(output_handle_)` per render so a resize
    // between the layout pass and the compose can't desync the
    // viewport bounds compose uses to decide what scrolls off
    // (the case-(B) erase distance + the will_scroll_off
    // heuristic both consume this value). Cheap — a single
    // TIOCGWINSZ ioctl on POSIX.
    const TermRows term_h = query_term_rows(output_handle_);
    const auto rows   = content_rows(canvas_);   // typed witness
    auto t_cf0 = std::chrono::steady_clock::now();

    // Maintain the inline mouse anchor: if the frame no longer fits below
    // its recorded top row, the terminal scrolled it up to make room, so
    // move the anchor up by the overflow. Keeps mouse-row translation
    // correct as content grows. No-op unless we have an anchor (inline +
    // mouse, DSR answered).
    if (inline_top_row_ > 0) {
        const int h = rows.value();
        inline_frame_rows_ = h;   // for out-of-frame mouse suppression
        if (inline_top_row_ + h - 1 > term_h.value())
            inline_top_row_ = std::max(1, term_h.value() - h + 1);
    }

    // Coherence state index before the visit, so the prof log can
    // flag any frame that DIDN'T stay Synced→Synced — those are the
    // full-viewport repaints (Stale soft-redraw, HardReset wipe,
    // verify-poison demote) that show as intermittent flicker.
    const std::size_t coh_before = in_coherence_.index();
    bool verify_demoted = false;

    in_coherence_ = std::visit(
        [&](auto&& arm) -> inline_frame::InlineCoherence {
            using T = std::decay_t<decltype(arm)>;
            using namespace inline_frame;

            auto lift = [](auto&& outcome) -> InlineCoherence {
                return std::visit(
                    [](auto&& a) -> InlineCoherence { return std::move(a); },
                    std::move(outcome));
            };

            if constexpr (std::is_same_v<T, InlineFrame<Empty>>) {
                auto fresh = std::move(arm).seed();
                return lift(std::move(fresh).render(
                    canvas_, rows, term_h, pool_, *writer_,
                    emit_sync_wrapper_));
            }
            else if constexpr (std::is_same_v<T, InlineFrame<Fresh>>) {
                return lift(std::move(arm).render(
                    canvas_, rows, term_h, pool_, *writer_,
                    emit_sync_wrapper_));
            }
            else if constexpr (std::is_same_v<T, InlineFrame<Synced>>) {
                // ── THE SCROLLBACK INVARIANT (one gate) ──────────
                //
                // A row that has scrolled into native terminal
                // scrollback is IMMUTABLE — we can never rewrite it.
                // The per-row diff assumes the committed overflow
                // prefix (rows [0, prev_rows-term_h) of the previous
                // frame) is still byte-identical in the new canvas.
                // agentty keeps a TALL live tail during streaming, so
                // that assumption breaks whenever the content above
                // the viewport top SHIFTS between frames:
                //
                //   • a card SHRINKS at/above the viewport top (turn
                //     settle, code-block fold) → rows below shift UP;
                //   • a card GROWS mid-turn (a bash tool going
                //     Running→Failed swaps a 1-row spinner for a
                //     multi-line error body) → rows above shift UP and
                //     a content-changed row (e.g. an ACTIONS header
                //     "0/1 … Bash" → "1/1 … 114ms") crosses the top;
                //   • the shadow gets poisoned mid-scroll.
                //
                // verify() CANNOT catch any of these — the shadow is
                // internally consistent; it is the WIRE that now holds
                // stale committed rows. A naive diff re-emits the
                // shifted prefix over immutable scrollback, stranding
                // a duplicate one screen up (the reported "card cut
                // off / turn duplicated in scrollback" corruption).
                //
                // agent_session never trips this: its live tail
                // collapses to empty at MessageStop (frozen and live
                // are mutually exclusive) so its frames stay
                // viewport-sized and nothing above the top ever
                // shifts. agentty's two-tier render can't guarantee
                // that structurally, so we enforce the invariant HERE,
                // uniformly, with ONE gate that every overflowed frame
                // passes through BEFORE the diff is trusted:
                //
                //   overflowed AND committed-prefix MISMATCH ?
                //     → the diff is unsafe. Recover — GROW and SHRINK
                //       alike — by committing the off-viewport rows and
                //       soft-repainting the viewport via case (B).
                //       Non-destructive, host scrollback preserved (no
                //       \x1b[3J).
                //
                //       HISTORY: the growing arm used to demote to
                //       HardReset (\x1b[2J\x1b[3J\x1b[H wipe) because
                //       the case-(B) of that era serialized from canvas
                //       row 0 with bottom-edge scrolls — a grown frame
                //       re-overflowed on repaint and scrolled a SECOND
                //       copy into scrollback (maya f010530). Today's
                //       case-(B) is VIEWPORT-CAPPED (start_row =
                //       content_rows - term_h, cursor_up ≤ term_h - 1,
                //       exactly term_h rows with term_h - 1 inter-row
                //       \r\n — see serialize.cpp's EMIT SHAPE
                //       contract): zero bottom-edge scrolls at ANY
                //       content height, so the second-copy failure mode
                //       is structurally impossible and the wipe is pure
                //       loss. The wipe erased the user's ENTIRE
                //       transcript + pre-launch shell history whenever
                //       a rare untested-regime shift landed on a grow
                //       frame ("only the last few turns are visible").
                //
                //       Residual cost of the soft recovery: the rows
                //       already in native scrollback keep their
                //       PRE-shift bytes (immutable at the VT level —
                //       nothing could have fixed them short of the
                //       wipe), and the grow delta's rows never reach
                //       scrollback (a gap of new_rows - prev_rows rows
                //       at the seam). Bounded, cosmetic, and strictly
                //       better than losing the whole history.
                //   overflowed AND prefix MATCHES ? → the diff is safe
                //     (append-only for grow-below-the-top, shrink at
                //     the bottom). Fall through to verify()+render.
                //   not overflowed ? → nothing is committed; the diff
                //     can rewrite every on-screen row. Fall through.
                //
                // This single check SUBSUMES the former three separate
                // shrink / grow / verify-poison-overflow branches: any
                // overflowed-prefix shift, from ANY cause (present or
                // future), is caught by the one memcmp before the diff
                // ever runs. Corruption is prevented by construction,
                // ── Scrollback gate, now type-enforced ──────────────
                // check_scrollback IS the gate: it returns nullopt when
                // the committed prefix shifted (recover), a vacuous
                // proof when the frame fits, a witnessed proof when the
                // prefix matched. The proof is then REQUIRED by render,
                // so there is no code path that diffs an overflowed
                // frame without having run this check — the type system
                // enforces what used to be a disciplined bool call.
                // ── Retheme FIRST, before the scrollback gate ────────
                //
                // A palette swap changes what every cell PAINTS while
                // moving no glyph, and that has TWO consequences. The
                // order of these checks is what makes both come out
                // right, so do not reorder them:
                //
                //  1. The gate must not read a restyle as corruption.
                //     check_scrollback handles that itself now: it
                //     re-compares ignoring style_id and lets a
                //     style-only difference through.
                //
                //  2. But passing the gate is NOT enough. The diff that
                //     follows also compares packed cells, and style_id
                //     is a StylePool-LOCAL id that the retheme just
                //     re-interned — the same id can now mean a different
                //     colour. A cell whose glyph and id both happen to
                //     match is skipped, so the frame ships a cursor move
                //     and nothing else. Measured while browsing themes on
                //     a long thread: 88 of 151 frames emitted exactly 13
                //     bytes, which is the "whole screen lags one
                //     keypress" report.
                //
                // So a retheme is not diffable at all: it has to re-state
                // the viewport, which is what demote-to-Stale does (case
                // B, an in-place soft repaint with no scrollback wipe).
                // This check used to sit BELOW the gate, where it was
                // only reachable once the gate had already failed — so
                // fixing the gate to pass style-only diffs would have
                // silently stopped the repaint from happening at all.
                if (retheme_repaint_) {
                    // demote_to_stale() only CHANGES STATE; it emits
                    // nothing, and the Stale arm paints on the NEXT
                    // frame. That frame is guaranteed by owes_paint():
                    // any state other than Synced/Sealed keeps
                    // has_deferred_frame() true, so the loop must come
                    // back and paint. No latch to set here, and none to
                    // forget.
                    const int prev_rows = arm.rows();
                    if (prev_rows > term_h.value()) {
                        const int overflow = prev_rows - term_h.value();
                        auto marker = arm.scrollback_marker(overflow);
                        auto committed = std::move(arm).commit(marker);
                        return std::move(committed).demote_to_stale();
                    }
                    return std::move(arm).demote_to_stale();
                }

                auto proof = arm.check_scrollback(canvas_, term_h.value());
                if (!proof) {
                    // Committed prefix SHIFTED. Same recovery as before:
                    // commit off-viewport + soft-repaint (case B), no
                    // scrollback wipe.
                    const int prev_rows = arm.rows();
                    const int overflow  = prev_rows - term_h.value();
#ifndef NDEBUG
                    // Debug-only invariant tripwire, now OPT-IN via
                    // MAYA_GATE_ABORT=1. A shifted committed prefix is
                    // the bug class every scrollback fix chased — but a
                    // Debug build is also what people daily-drive, and
                    // this abort killed real user sessions (two field
                    // SIGABRTs, 2026-08-15) when the perfectly good
                    // soft-recovery below would have self-healed with no
                    // visible artifact. The invariant is now guarded by
                    // scrollback_oracle_test's detectors in CI, so the
                    // LOUD-crash default has done its job: keep the dump
                    // + abort available for bug hunts, default to
                    // recovering the user's session.
                    if (std::getenv("MAYA_GATE_ABORT")) {
                        arm.dump_prefix_mismatch(canvas_, overflow);
                        std::fprintf(stderr,
                            "[maya] FATAL: scrollback-invariant "
                            "gate fired (committed prefix shifted: "
                            "prev_rows=%d term_h=%d overflow=%d). "
                            "A frame rewrote a committed row. See "
                            "the [gate] dump above. Unset "
                            "MAYA_GATE_ABORT to soft-recover "
                            "instead.\n",
                            prev_rows, term_h.value(), overflow);
                        std::fflush(stderr);
                        std::abort();
                    }
#endif
                    verify_demoted = true;
                    ++scrollback_recovery_count_;
                    auto marker = arm.scrollback_marker(overflow);
                    auto committed = std::move(arm).commit(marker);
                    return std::move(committed).demote_to_stale();
                }

                // (The retheme repaint that used to live here now runs
                // BEFORE check_scrollback — see the comment there for the
                // full story and for why the order is load-bearing.)
                auto wit = arm.verify();
                if (!wit) {
                    verify_demoted = true;
                    ++scrollback_recovery_count_;
                    // Shadow poisoned: prev_cells no longer matches the
                    // wire. If the frame is OVERFLOWED, the committed
                    // prefix was already validated by check_scrollback
                    // above (it matched, else we'd have recovered), so
                    // a plain commit-off-viewport + soft-repaint is
                    // guaranteed safe (the rows we commit equal what
                    // physically overflowed). If it FITS the viewport,
                    // no row scrolled off — every diverged row is still
                    // on screen and rewritable, so a plain demote-to-
                    // Stale (case B) repaints in place. Either way,
                    // NON-destructive: no \x1b[3J wipe.
                    const int prev_rows = arm.rows();
                    if (prev_rows > term_h.value()) {
                        const int overflow = prev_rows - term_h.value();
                        auto marker = arm.scrollback_marker(overflow);
                        auto committed = std::move(arm).commit(marker);
                        return std::move(committed).demote_to_stale();
                    }
                    return std::move(arm).demote_to_stale();
                }
                return lift(std::move(arm).render(
                    canvas_, rows, term_h, pool_, *writer_,
                    *std::move(wit), *std::move(proof),
                    emit_sync_wrapper_));
            }
            else if constexpr (std::is_same_v<T, InlineFrame<Stale>>) {
                return lift(std::move(arm).render(
                    canvas_, rows, term_h, pool_, *writer_,
                    emit_sync_wrapper_));
            }
            else if constexpr (std::is_same_v<T, InlineFrame<HardReset>>) {
                // A width change wiped the terminal's scrollback. If the
                // host gave us its history, bring it back at the new width
                // in this same frame (see scrollback_replay.hpp).
                std::string history;
                if (replay_owed_ && (replay_.blocks || replay_.chunk)) {
                    replay_owed_ = false;
                    const auto t0 = std::chrono::steady_clock::now();
                    auto got = replay::render_history(replay_, canvas_.width(), theme_);
                    history = std::move(got.bytes);
                    if (replay_.after) replay_.after();
                    last_replay_ = ReplayStats{
                        got.rows, got.blocks, got.cached, got.capped,
                        history.size(),
                        std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - t0).count()};
                    io_log("scrollback replay: rows=%d blocks=%d cached=%d capped=%d bytes=%zu ms=%.1f build_ms=%.1f render_ms=%.1f",
                           got.rows, got.blocks, got.cached, got.capped ? 1 : 0,
                           history.size(), last_replay_.ms, got.build_ms, got.render_ms);
                }
                replay_owed_ = false;
                return lift(std::move(arm).render(
                    canvas_, rows, term_h, pool_, *writer_,
                    emit_sync_wrapper_, history));
            }
            else {
                static_assert(std::is_same_v<T, InlineFrame<Sealed>>);
                return std::move(arm);   // sealed: no-op
            }
        }, std::move(in_coherence_));

    // The repaint request is consumed by THIS frame, whichever arm ran.
    //
    // Only the Synced arm acts on it — every other arm is already doing a
    // full-viewport repaint, so a swap needs nothing extra from them. But
    // clearing it only inside that arm leaks: a theme change landing on a
    // Fresh/Stale/HardReset frame would leave the flag raised, and the
    // next Synced frame (possibly many frames later, mid-stream) would
    // demote for a swap that had already been painted — a gratuitous
    // repaint attributed to the wrong keystroke. One unconditional clear
    // at the end of the frame is the whole invariant: the flag means
    // "a swap happened during THIS frame", never "...at some point".
    retheme_repaint_ = false;

    double cf_ms = since(t_cf0);
    if (prof) {
        // coh: which inline coherence arm RAN this frame (the
        // before-index). 2 == Synced (the steady, no-flicker path);
        // anything else is a full-viewport repaint. `demote` flags
        // the verify-poison Synced→Stale transition specifically.
        static std::uint64_t prev_skip = 0, prev_cmp = 0, prev_ent = 0;
        static std::uint64_t prev_stamp = 0;
        static std::uint64_t prev_b = 0, prev_l = 0, prev_p = 0;
        const std::uint64_t now_skip = render_detail::blit_rows_epoch_skip();
        const std::uint64_t now_cmp  = render_detail::blit_rows_compared();
        const std::uint64_t now_ent  = render_detail::blit_entries_walked();
        const std::uint64_t now_stamp = canvas_.stamp_row_count();
        const std::uint64_t now_b = render_detail::rt_build_ns();
        const std::uint64_t now_l = render_detail::rt_layout_ns();
        const std::uint64_t now_p = render_detail::rt_paint_ns();
        std::fprintf(prof_out,
            "maya-frame: rt=%.2f cf=%.2f total=%.2f nodes=%zu rows=%d w=%d "
            "term_h=%d coh=%zu->%zu peak=%d pad=%d decay=%d recov=%lu "
            "blit[ent=%llu skip=%llu cmp=%llu] stamps=%llu "
            "ph[b=%.2f l=%.2f p=%.2f]%s%s\n",
            rt_ms, cf_ms, since(t_frame_start),
            layout_nodes_.size(), ch, w, term_h.value(),
            coh_before, in_coherence_.index(),
            hold_peak_, render_ctx_.inline_min_content, hold_decay_,
            scrollback_recovery_count_,
            (unsigned long long)(now_ent - prev_ent),
            (unsigned long long)(now_skip - prev_skip),
            (unsigned long long)(now_cmp - prev_cmp),
            (unsigned long long)(now_stamp - prev_stamp),
            (now_b - prev_b) / 1e6, (now_l - prev_l) / 1e6,
            (now_p - prev_p) / 1e6,
            coh_before != 2 ? " FLICKER" : "",
            verify_demoted ? " VERIFY-DEMOTE" : "");
        prev_skip = now_skip; prev_cmp = now_cmp; prev_ent = now_ent;
        prev_stamp = now_stamp;
        prev_b = now_b; prev_l = now_l; prev_p = now_p;
        std::fflush(prof_out);
    }

    // Live-app surfacing of the SoT invariant, independent of the
    // profiler (which needs MAYA_IO_LOG + a log reader). Gated on
    // MAYA_WARN_RECOVERY so it never disturbs a normal session: the
    // FIRST time the scrollback gate recovers (shadow disagreed with
    // the wire OR the host committed a stale debt), emit a single
    // stderr line pointing the operator at the metric. One-shot via a
    // static latch — a rising counter is the field signature of a
    // scrollback regression the type guards + PTY oracle are meant to
    // keep at zero. Off by default; costs one getenv the first frame.
    if (scrollback_recovery_count_ != 0) {
        static bool warned = false;
        static const bool want_warn =
            std::getenv("MAYA_WARN_RECOVERY") != nullptr;
        if (want_warn && !warned) {
            warned = true;
            std::fprintf(stderr,
                "[maya] NOTICE: scrollback gate recovered (recov=%lu). The "
                "single-source-of-truth invariant held \u2014 no corruption "
                "reached the screen \u2014 but a shadow/wire (or host-debt) "
                "disagreement was caught + repaired. On a healthy session this "
                "stays 0; a rising count is a regression. Run under "
                "MAYA_IO_LOG=<path> to see the recov= metric per frame.\n",
                scrollback_recovery_count_);
            std::fflush(stderr);
        }
    }
    return ok();
}

} // namespace maya::detail
