// reveal_fx.cpp — StreamingMarkdown live-mode animated reveal overlay.
//
// render_live_overlay_() is build()'s finalize step. When the widget isn't
// live (or reveal_fx is off, the default) it returns the settled
// cached_build_ untouched. When reveal_fx is on it layers three effects on
// the trailing edge of the live tail — scramble→resolve, a hot→cool age
// gradient over the last ~24 codepoints, and a pulsing inline block caret —
// driven by the animation framework's frame requests, mutating only a
// shallow copy.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "maya/core/anim_clock.hpp"

#include "maya/element/builder.hpp"
#include "maya/style/style.hpp"
#include "maya/terminal/ansi.hpp"   // env_supports_synchronized_output()
#include "maya/widget/agentty/markdown.hpp"
#include "maya/widget/agentty/markdown/internal.hpp"
#include "maya/widget/agentty/markdown/streaming_internal.hpp"
#include "maya/core/motion.hpp"     // anim::keep_animating — frame requests
#include "maya/core/animation.hpp"   // anim::ease::*, anim::lerp(Color)
#include "maya/anim/text_reveal.hpp"  // anim::decorate_text_reveal / end_caret

namespace maya {

// The trailing-edge reveal decoration (scramble / hot→cool gradient / ghost
// materialise / sweep cursor / pulsing end-caret) is now ENTIRELY owned by the
// framework primitive in maya/anim/text_reveal.hpp. This translation unit keeps
// only the StreamingMarkdown-specific glue: advancing the reveal cursor off the
// wire bytes and walking the live tree to the trailing text leaf. All easing /
// pulse / colour math lives in maya::anim so it stays in ONE place and any
// other widget can reuse it. (The former local reveal_detail::pulse01 was dead
// after the Phase-3 lift and has been removed.)

namespace {

// ── Eager-block (table) reveal helpers ──────────────────────────────────────
//
// Lists / blockquotes / code blocks render their LIVE tail as a plain
// BoxElement, so the overlay's find_last_text already walks to their last
// content line and decorates it. A TABLE is the exception: render_eager_slice
// wraps it in a hash-keyed ComponentElement whose render() returns ANOTHER
// (width-aware) ComponentElement, so the materialised cell rows only exist at
// paint time. find_last_text bails at the outer component and the table never
// animates ("the animation doesn't happen when its rendering things like
// table"). The helpers below let the overlay reach the materialised rows from
// inside a render-closure wrapper (see the eager branch in
// render_live_overlay_).

// Box-drawing glyphs the table / border builders emit. A text row whose every
// non-space codepoint is one of these is a STRUCTURAL border / separator row
// (┌─┬─┐ / ├┈┼┈┤ / └─┴─┘), not a data row — skip it when choosing which row to
// animate so the shimmer lands on the newest CONTENT row, not the frame.
[[nodiscard]] inline bool is_table_border_glyph(char32_t c) noexcept {
    switch (c) {
        case U'\u2500': case U'\u2502': case U'\u250C': case U'\u2510':
        case U'\u2514': case U'\u2518': case U'\u251C': case U'\u2524':
        case U'\u252C': case U'\u2534': case U'\u253C': case U'\u2508':
            return true;
        default:
            return false;
    }
}

// True when `s` (a row's rendered content) carries any non-space, non-border
// codepoint — i.e. it is a data/header row, not a pure border/separator line.
[[nodiscard]] inline bool row_has_content(std::string_view s) noexcept {
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char b = static_cast<unsigned char>(s[i]);
        char32_t cp; std::size_t len;
        if (b < 0x80)            { cp = b;         len = 1; }
        else if ((b >> 5) == 0x6){ cp = b & 0x1F; len = 2; }
        else if ((b >> 4) == 0xE){ cp = b & 0x0F; len = 3; }
        else if ((b >> 3) == 0x1E){ cp = b & 0x07; len = 4; }
        else { ++i; continue; }   // stray continuation byte — skip
        for (std::size_t k = 1; k < len && i + k < s.size(); ++k)
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        i += len;
        if (cp == U' ' || cp == U'\t' || cp == U'\r') continue;
        if (!is_table_border_glyph(cp)) return true;
    }
    return false;
}

// Walk a MATERIALISED element tree (BoxElement / ElementList of TextElements —
// e.g. a rendered table) and return the rightmost-in-render-order TextElement
// that is a content row. Falls back to the last non-empty text leaf when every
// row is structural (degenerate all-empty-cell table). Returns nullptr if no
// text leaf exists. ComponentElement children are NOT descended (they need a
// width to materialise — the caller wraps those separately).
inline void collect_last_content_leaf(Element& e, TextElement*& last_any,
                                      TextElement*& last_content) noexcept {
    if (auto* t = std::get_if<TextElement>(&e.inner)) {
        if (!t->content.empty()) {
            last_any = t;
            if (row_has_content(t->content)) last_content = t;
        }
        return;
    }
    if (auto* b = std::get_if<BoxElement>(&e.inner)) {
        for (auto& c : b->children)
            collect_last_content_leaf(c, last_any, last_content);
        return;
    }
    if (auto* l = std::get_if<ElementList>(&e.inner)) {
        for (auto& it : l->items)
            collect_last_content_leaf(it, last_any, last_content);
        return;
    }
    // ComponentElement / ElementListRef: not descended here.
}

[[nodiscard]] inline TextElement* rightmost_content_text_leaf(Element& root) noexcept {
    TextElement* last_any = nullptr;
    TextElement* last_content = nullptr;
    collect_last_content_leaf(root, last_any, last_content);
    return last_content ? last_content : last_any;
}

// Count UTF-8 codepoints in [begin, end) of `s` — a lead byte is any byte
// that is NOT a 0b10xxxxxx continuation. Used by request_finalize() to
// derive an ACCURATE backlog: the integrator keeps an incremental cp count
// (cached_total_cp_) but that cache can be stale at finalize time, and
// falling back to source_.size() (raw BYTES) over-counts multibyte text —
// inflating the backlog and stretching the finalize ramp past its target.
[[nodiscard]] inline std::size_t utf8_cp_count(std::string_view s) noexcept {
    std::size_t n = 0;
    for (unsigned char b : s)
        if ((b & 0xC0) != 0x80) ++n;
    return n;
}

} // namespace

void StreamingMarkdown::request_finalize(int ramp_ms) noexcept {
    // Nothing to ramp if we're not live: the next build() would short-
    // circuit at the !live_ guard and the ramp would just sit unrunnable,
    // wedging finish() behind is_finalizing() forever. The host calls
    // this every frame while settled, so a no-op here on the trailing
    // frames (post-completion) is the natural idempotency.
    if (!live_) return;
    // Idempotent: a ramp already in flight keeps its deadline. Picking
    // the EARLIER deadline if the host calls us again with a shorter
    // window would be defensible, but in practice the host calls us once
    // per frame at settle — the idempotency that matters is "called
    // every frame while settled" (turn.cpp passes through here every
    // render) not recomputing the deadline forward.
    if (finalize_armed_) return;
    if (ramp_ms < 0) ramp_ms = 0;

    // Adaptive ramp floor: the host passes a SHORT fixed ramp (e.g. 200 ms)
    // — fine when the reveal cursor is already near the live edge. But on a
    // long, fast reply the cursor cruises at a bounded readable speed
    // (RateCursor's clamped cruise) and can be a large backlog behind the
    // edge when the wire closes. Snapping that whole backlog out in 200 ms
    // is itself a BURST — the exact "it dumps the rest at once" symptom at
    // settle. So stretch the ramp to whatever it takes for the remaining
    // backlog to GLIDE out at no more than kFinishSpeedup× the cruise speed.
    // The reveal then accelerates only modestly at the end and the tail
    // types out smoothly instead of pasting. (The ramp still bypasses the
    // per-frame cruise cap in RateCursor::tick so completion is guaranteed;
    // this just sets a deadline far enough out that the guaranteed rate
    // stays readable.)
    {
        const std::size_t cur_cp =
            static_cast<std::size_t>(reveal_cp_ < 0.0 ? 0.0 : reveal_cp_);
        // Accurate codepoint total. The integrator's incremental cache
        // (cached_total_cp_) is only valid when it covers the whole
        // source; if it is stale, recount codepoints rather than falling
        // back to source_.size() (raw BYTES), which would over-count
        // multibyte text and inflate the backlog → an over-long ramp.
        const std::size_t total_cp =
            (cached_total_cp_at_ == source_.size())
                ? cached_total_cp_ : utf8_cp_count(source_);
        const double backlog =
            (total_cp > cur_cp) ? static_cast<double>(total_cp - cur_cp) : 0.0;
        if (backlog > 0.0 && reveal_floor_cps_ > 0.0) {
            // Let the tail finish at up to ~2× cruise so it feels like the
            // typewriter speeding up to land, not an instant paste.
            constexpr double kFinishSpeedup = 2.0;
            // ...but never make the user wait more than kMaxFinishMs for the
            // reveal to drain after the wire closed. A pathological backlog
            // (model dumped tens of KB the cursor never caught up to)
            // finishes faster than 2× cruise rather than stalling the UI for
            // tens of seconds — a brief end burst is the lesser evil vs. a
            // multi-second "why is it still typing" wait. ~2.5 s is long
            // enough to glide a few hundred cp out smoothly.
            constexpr double kMaxFinishMs = 2500.0;
            double glide_ms =
                (backlog / (reveal_floor_cps_ * kFinishSpeedup)) * 1000.0;
            if (glide_ms > kMaxFinishMs) glide_ms = kMaxFinishMs;
            if (glide_ms > static_cast<double>(ramp_ms))
                ramp_ms = static_cast<int>(glide_ms);
        }
    }

    const auto now_ms = anim_now_ms();
    finalize_armed_ = true;
    finalize_deadline_ms_ = now_ms + ramp_ms;
    finalize_hard_ = false;               // adaptive ramp: #4 re-eval may extend
    finalize_armed_size_ = source_.size();
}

void StreamingMarkdown::snap_reveal_to_edge(int glide_ms) noexcept {
    // Only meaningful while live_ with reveal_fx_: a settled build has no
    // ghost/scramble to snap, and with reveal_fx_ off the tail already
    // renders fully. Guard so a stray host call outside a live reveal is a
    // pure no-op (no dirty, no rebuild).
    if (!live_ || !reveal_fx_) return;

    // Total codepoints in source_. Reuse the incremental cache when it
    // still covers the whole buffer; otherwise fall back to a byte-size
    // upper bound (reveal_cp_ is compared against source_.size() elsewhere,
    // so overshooting to bytes is a safe "fully revealed" signal — the
    // next advance_reveal_cursor_() reconciles the exact cp count).
    const double total_cp =
        (cached_total_cp_at_ == source_.size())
            ? static_cast<double>(cached_total_cp_)
            : static_cast<double>(source_.size());

    // Already at (or past) the edge — nothing ghosted, nothing to do.
    if (reveal_cp_ >= total_cp) return;

    // Bounded glide: instead of pasting the whole backlog in one frame, arm
    // a HARD-deadline finalize ramp so the cursor sprints to the edge over
    // `glide_ms` (a fast but visible catch-up). Hard deadline, NOT the
    // adaptive stretch in request_finalize — the caller guarantees this
    // window is scrollback-safe (short enough that a growing card can't
    // strand a lagged row before the ramp lands), so we honour it exactly
    // rather than lengthening it for a big backlog. build_dirty_ is bumped
    // by advance_reveal_cursor_ as the ramp moves the cursor each frame.
    if (glide_ms > 0) {
        finalize_armed_ = true;
        finalize_deadline_ms_ = anim_now_ms() + glide_ms;
        // HARD deadline: the caller's scrollback-safety analysis assumed
        // exactly glide_ms. Suppress the adaptive #4 stretch in
        // advance_reveal_cursor_ (a 300-cp backlog at floor 45 cps would
        // otherwise stretch a 150 ms tool-seam snap to 2.5 s, holding
        // deferred tool cards hidden the whole time).
        finalize_hard_ = true;
        finalize_armed_size_ = source_.size();
        build_dirty_ = true;
        return;
    }

    // Instant snap (glide_ms == 0). Jump the public cursor and the central
    // integrator to the edge so the next build() renders the tail fully-
    // revealed (no ghost cells, no scramble). Keep live_ intact — the widget
    // resumes normal typewriter pacing on the next grow. Stamp BOTH clocks
    // so the following frame's elapsed-time delta starts from now (no
    // spurious catch-up burst): reveal_ms_ feeds the scramble/gradient age
    // math, but the per-frame dt is integrated off reveal_us_ (the µs
    // clock) — stamping only the ms clock (#6, the old bug) left up to
    // kMaxCatchupS of stale µs-elapsed to burst-integrate if bytes arrived
    // right after a mid-stream resize snap.
    reveal_cp_ = total_cp;
    reveal_rate_cursor_.set_pos(total_cp);
    const auto now_ms2 = anim_now_ms();
    reveal_ms_ = now_ms2;
    reveal_us_ = anim_now_us();
    reveal_edge_reached_ms_ = now_ms2;
    // The tail shape depends on reveal_byte_clip_ (derived from reveal_cp_):
    // force a rebuild so build() re-extracts the now-unclipped tail.
    build_dirty_ = true;
}

void StreamingMarkdown::advance_reveal_floor(std::size_t cp) noexcept {
    // Only meaningful while a live reveal is actually clipping the tail.
    // With reveal_fx_ off (or settled) the tail already renders in full, so
    // there is nothing ghosted to resolve and a stray host call is a pure
    // no-op — no dirty, no rebuild.
    if (!live_ || !reveal_fx_) return;

    // Clamp to the codepoint total so a caller's row→cp estimate that
    // overshoots (it is deliberately rounded UP for safety) degrades to
    // "resolve everything" instead of parking the cursor past the edge.
    // Reuse the incremental cache when it still covers the whole buffer;
    // otherwise recount. Do NOT fall back to source_.size() (raw BYTES)
    // like snap_reveal_to_edge's at-edge probe does: here the value becomes
    // the cursor POSITION, and a byte count over-counts multibyte text,
    // which would resolve more of the tail than the caller asked for.
    const std::size_t total_cp =
        (cached_total_cp_at_ == source_.size())
            ? cached_total_cp_ : utf8_cp_count(source_);
    if (cp > total_cp) cp = total_cp;

    // MONOTONE: never rewind. A floor at/below the cursor means the reveal
    // has already passed this point, so the rows in question are resolved
    // and the host's per-frame call costs nothing. Rewinding would re-ghost
    // a row that may already have scrolled into scrollback — precisely the
    // corruption this API exists to prevent.
    const double floor_cp = static_cast<double>(cp);
    if (!(floor_cp > reveal_cp_)) return;

    reveal_cp_ = floor_cp;
    // Keep the integrator in sync, or the next tick() would integrate from
    // its own stale (lower) position and the cursor would visibly snap BACK.
    // advance_floor is the matching monotone primitive.
    reveal_rate_cursor_.advance_floor(floor_cp);

    // Deliberately NOT stamped here (unlike snap_reveal_to_edge):
    //   • reveal_ms_ / reveal_us_ — the tail past the floor is still
    //     animating, and this is called every frame while a card is
    //     visible. Re-stamping the µs clock each frame would zero the
    //     per-frame dt and FREEZE the remaining reveal (the exact stall
    //     this whole path exists to remove).
    //   • reveal_edge_reached_ms_ — the cursor has not reached the live
    //     edge; only advance_reveal_cursor_ may declare that, and a false
    //     stamp would start the settle/scramble age counter early.
    //
    // The tail shape depends on reveal_byte_clip_ (derived from reveal_cp_),
    // so force a rebuild to re-extract the now-shorter clipped tail. Only
    // reached when the cursor actually moved.
    build_dirty_ = true;
}

// Advance reveal_cp_ for this frame. Called from build() top so the
// cursor's effect on the live overlay (scramble/gradient/caret aging) is
// current BEFORE the cache short-circuit. Three things this fixes:
//   1. Finalize-ramp completion (which flips live_ off) is observed
//      before the cache check, so the next frame doesn't serve a stale
//      live-mode build.
//   2. The committed-snap below: if commit_range fired during this
//      frame, snap reveal_cp_ forward to committed_cp so the cursor
//      never lags behind visibly-rendered committed content. Without
//      this, on a burst delta that fires commit_range, age_at_tail_ms
//      would stay 0 for hundreds of ms while the cursor crawls past
//      committed bytes the user already sees — the scramble window
//      would never start aging, so the trailing edge stayed in noise
//      mode long after the bytes settled.
//   3. Scratch values (ms_total, total_cp, age_at_tail_ms) are stamped
//      for render_live_overlay_() to consume without re-walking source_
//      or re-reading the clock.
//
// Returns true when the frame's cached_build_ is stale and a rebuild is
// needed (finalize-ramp completed); caller uses this to bump
// build_dirty_ before the cache check.
bool StreamingMarkdown::advance_reveal_cursor_() const {
    if (!live_ || !reveal_fx_) return false;

    const std::int64_t ms_total = anim_now_ms();
    cursor_advance_ms_total_ = ms_total;

    // Grow / shrink detection off source_ size. last_seen_size_ is the
    // edge-change detector; the trailing-edge AGE is derived from the
    // cursor-edge clock (reveal_edge_reached_ms_), NOT from a byte-arrival
    // timestamp — so no last_grow_ms_ is kept. (An earlier age model
    // stamped a byte-arrival time and assumed a fixed ~220 cps reveal
    // rate; it desynced from the real cursor whenever reveal_floor_cps_
    // was retuned. The cursor-edge clock has no such assumption.)
    if (source_.size() > last_seen_size_) {
        // Stream RESUMED after the ramp did its job: if a finalize ramp is
        // armed and the cursor had already reached the edge of the content
        // it was armed against (reveal_edge_reached_ms_ != 0 → previous
        // frame ended at-edge), the "stream is ending" premise is dead —
        // this is a mid-message pause (or an interleaved text block), not
        // the end. Disarm, or the widget stays in ramp mode for the rest
        // of the turn: every frame the #4 re-eval rolls the deadline to
        // backlog/(2·floor) and the cursor cruises at ≥2× floor, collapsing
        // the drain_secs jitter buffer into exactly the stop-and-go the
        // pacing retune removed. The genuine #4 case (finalize armed while
        // the last delta is still draining) has the cursor BEHIND the edge
        // (reveal_edge_reached_ms_ == 0) and is untouched.
        if (finalize_armed_ && reveal_edge_reached_ms_ != 0
            && source_.size() > finalize_armed_size_) {
            finalize_armed_       = false;
            finalize_deadline_ms_ = 0;
            finalize_hard_        = false;
            finalize_armed_size_  = 0;
        }
        // #3: if the cursor was AT THE EDGE when this growth arrived, the
        // wall-clock gap since the last build is idle-waiting, not owed
        // typewriter time. The at-edge snap in the dt block below
        // (reveal_us_ = us_total) only runs while builds keep coming — but
        // frames STOP once the caret-pulse window (4 s) expires, so after
        // a long model silence reveal_us_ is seconds stale. The next build
        // (this one, triggered by the new bytes) would see a huge dt with
        // a backlog and drain the gap at kMaxCatchupS per frame ≈ 15× real
        // time — the incoming chunk PASTES, exactly the artifact the carry
        // comment promises to prevent. Re-stamp on next advance instead:
        // the burst frame integrates from now, dt ≈ 0, normal glide.
        if (reveal_edge_reached_ms_ != 0) reveal_us_ = 0;
        last_seen_size_ = source_.size();
        // New bytes arrived — the cursor will fall behind the (now
        // larger) edge, so the "reached edge" clock is invalid. Restart
        // it when the cursor next catches up to the new edge.
        reveal_edge_reached_ms_ = 0;
    } else if (source_.size() < last_seen_size_) {
        last_seen_size_ = source_.size();
        reveal_cp_      = 0.0;
        reveal_ms_      = ms_total;
        reveal_us_      = 0;   // re-stamp on next advance (µs clock)
        reveal_edge_reached_ms_ = 0;
    }

    // Total codepoints in source_. Incremental update: cached value
    // covers source_[0 .. cached_total_cp_at_); count just the new bytes.
    // On a size shrink (clear()/set_content rollback) the cache is
    // invalidated and recounted from 0.
    if (cached_total_cp_at_ > source_.size()) {
        cached_total_cp_ = 0;
        cached_total_cp_at_ = 0;
    }
    for (std::size_t i = cached_total_cp_at_; i < source_.size(); ++i)
        if ((static_cast<unsigned char>(source_[i]) & 0xC0) != 0x80)
            ++cached_total_cp_;
    cached_total_cp_at_ = source_.size();
    const std::size_t total_cp = cached_total_cp_;
    cursor_advance_total_cp_ = total_cp;

    // First-call init: stamp the wall-clock baseline and start the
    // cursor at 0. Must run BEFORE the committed-snap below so the
    // snap survives — otherwise the init clobbers the snapped value
    // back to 0 on the first build() after construction.
    if (reveal_ms_ == 0) { reveal_ms_ = ms_total; reveal_cp_ = 0.0; }

    // Codepoint count of the COMMITTED prefix. The cursor must never
    // lag behind committed_ because committed bytes are already rendered
    // as full styled blocks in the prefix — they're visible no matter
    // what the reveal cursor says. Letting reveal_cp_ stay below
    // committed_cp wastes the typewriter budget catching up to bytes
    // the user already sees, and starves the scramble/gradient age
    // counter on the tail (age_at_tail_ms only starts ticking once the
    // cursor reaches the live edge). On a burst delta that fires
    // commit_range, the scramble window would never start aging — the
    // trailing edge stayed in noise mode long after the bytes settled.
    //
    // Snap forward to committed_cp here. Reveals nothing the user
    // hasn't already seen, frees the typewriter to animate ONLY the
    // live tail (the part where reveal makes visual sense).
    if (cached_committed_cp_at_ > committed_) {
        cached_committed_cp_ = 0;
        cached_committed_cp_at_ = 0;
    }
    for (std::size_t i = cached_committed_cp_at_;
         i < committed_ && i < source_.size(); ++i)
        if ((static_cast<unsigned char>(source_[i]) & 0xC0) != 0x80)
            ++cached_committed_cp_;
    cached_committed_cp_at_ = committed_;
    const std::size_t committed_cp = cached_committed_cp_;
    if (reveal_cp_ < static_cast<double>(committed_cp))
        reveal_cp_ = static_cast<double>(committed_cp);

    {
        // Per-frame dt in SECONDS, from the MICROSECOND clock. anim_now_ms()
        // truncates to whole ms, so two build()s in the same ms give dt==0
        // and the cursor stalls that frame (RateCursor early-outs on
        // dt<=0), bunching motion into the next ms tick — a micro-stutter
        // that is worst on fast/local terminals. reveal_us_ tracks the same
        // instants at µs resolution so the dt is exact.
        const std::int64_t us_total = anim_now_us();
        if (reveal_us_ == 0) reveal_us_ = us_total;
        double elapsed_s = (us_total - reveal_us_) / 1'000'000.0;
        if (elapsed_s < 0.0) elapsed_s = 0.0;
        // Cap a catch-up frame so a long stall (GC pause, blocked terminal
        // write, SSH round-trip) doesn't teleport the cursor to the edge in
        // one frame. 250 ms (was 120 ms): the old cap discarded real elapsed
        // time on any frame gap > 120 ms, so the backlog then drained over
        // FOLLOWING frames via the low-pass instead of being caught up —
        // turning a delivery hitch into visible slow-motion lag. 250 ms
        // still bounds a pathological pop while letting an ordinary dropped
        // frame (one skipped 60 fps wake ≈ 33 ms, or a 100 ms non-sync tick)
        // integrate its true duration so motion stays continuous. The
        // finalize ramp handles genuine end-of-stream catch-up separately.
        //
        // Crucially the clamp does NOT throw the excess elapsed time away:
        // it advances reveal_us_ by only the consumed slice, so the residual
        // is integrated on subsequent frames. Discarding it (the old
        // behaviour) permanently shortened the reveal budget by the stall
        // duration — a 2 s pause made ~1.75 s of typewriter time vanish and
        // the tail could paste, the exact artifact this animation prevents.
        constexpr double kMaxCatchupS = 0.250;
        std::int64_t consumed_us = us_total - reveal_us_;
        if (elapsed_s > kMaxCatchupS) {
            elapsed_s = kMaxCatchupS;
            consumed_us = static_cast<std::int64_t>(kMaxCatchupS * 1'000'000.0);
        }
        // Carry the un-consumed residual forward ONLY while there is a
        // backlog to reveal. If the cursor is already at the edge (idle at
        // settle, or between bursts) there is nothing owed — snap the clock
        // to now so a long idle gap can't accumulate "owed" time that then
        // bursts the next chunk out in one frame. reveal_cp_ still holds the
        // pre-tick value here; total_cp is this frame's true target.
        const bool has_backlog = reveal_cp_ < static_cast<double>(total_cp);
        reveal_us_ = has_backlog ? (reveal_us_ + consumed_us) : us_total;
        reveal_ms_ = ms_total;
        // Central integrator: the RateCursor owns ALL pacing arithmetic —
        // floor-cps ceiling, burst drain, adaptive rate smoothing and the
        // finalize-deadline ramp (see anim::RateCursor in core/animation.hpp,
        // covered by reveal_pacing_test). Keep it in lock-step with
        // reveal_cp_ (the public cursor) every frame: push the
        // committed-snap + pacing in, integrate, read back.
        reveal_rate_cursor_.set_pacing(reveal_floor_cps_, reveal_drain_secs_);
        reveal_rate_cursor_.set_adaptive(reveal_adaptive_,
                                         reveal_adapt_floor_min_,
                                         reveal_adapt_floor_max_);
        reveal_rate_cursor_.set_pos(reveal_cp_);          // honour the snap above
        if (finalize_armed_) {
            // #4: re-evaluate the ramp target every frame. If source_ grew
            // AFTER the ramp was armed (finalize is often requested before
            // the last wire delta drains), the original deadline was set
            // against a smaller total and would land early — revealing the
            // late-arriving tail via the fallback path faster than the
            // ramp, i.e. a paste. Push the deadline forward so the current
            // backlog still glides out at ≤ kFinishSpeedup× cruise.
            //
            // HARD deadlines (snap_reveal_to_edge glide) are exempt: the
            // caller guaranteed that exact window is scrollback-safe and
            // the stretch would silently 16× it on a big backlog. A hard
            // ramp still tracks late GROWTH via the RateCursor deadline
            // rate below (backlog/remaining bypasses the cruise cap), so
            // completion by the promised wall-clock stays guaranteed.
            constexpr double kFinishSpeedup = 2.0;
            constexpr double kMaxFinishMs   = 2500.0;
            const double backlog =
                static_cast<double>(total_cp) - reveal_cp_;
            if (!finalize_hard_ && backlog > 0.0 && reveal_floor_cps_ > 0.0) {
                double need_ms =
                    (backlog / (reveal_floor_cps_ * kFinishSpeedup)) * 1000.0;
                if (need_ms > kMaxFinishMs) need_ms = kMaxFinishMs;
                const std::int64_t want_deadline =
                    ms_total + static_cast<std::int64_t>(need_ms);
                if (want_deadline > finalize_deadline_ms_)
                    finalize_deadline_ms_ = want_deadline;
            }
            const double remaining_s =
                (finalize_deadline_ms_ - ms_total) / 1000.0;
            reveal_rate_cursor_.set_deadline(remaining_s);
        } else {
            reveal_rate_cursor_.clear_deadline();
        }
        reveal_cp_ = reveal_rate_cursor_.tick(
            static_cast<double>(total_cp), elapsed_s);
        // #6: NaN/Inf guard. Every downstream consumer casts reveal_cp_ to
        // std::size_t (byte clip, overlay revealed_cp, age math); a non-
        // finite value (e.g. a 0/0 from a degenerate rate·dt) casts to an
        // implementation-defined size_t — 0 on some targets, a colossal
        // out-of-range index on others (→ tail over-clip / OOB). Clamp back
        // to a sane bound before anyone reads it.
        if (!std::isfinite(reveal_cp_)) {
            reveal_cp_ = static_cast<double>(committed_cp);
            reveal_rate_cursor_.set_pos(reveal_cp_);
        }
    }

    // ── Cursor-edge clock ──
    // Maintain reveal_edge_reached_ms_: the wall-clock of the FIRST frame
    // the cursor reached the edge in the current at-edge run. While the
    // cursor is behind the edge, hold it at 0; stamp it the frame it
    // lands. (Source-grow already reset it to 0 above, so a fresh burst
    // restarts the clock the next time the cursor catches up.)
    const bool cursor_at_edge = reveal_cp_ >= static_cast<double>(total_cp);
    if (cursor_at_edge) {
        if (reveal_edge_reached_ms_ == 0) reveal_edge_reached_ms_ = ms_total;
    } else {
        reveal_edge_reached_ms_ = 0;
    }

    // Ramp completion — flip live_ off. live_'s Tracked<> bumps
    // build_dirty_ for us, but return dirty=true so the caller doesn't
    // double-check.
    //
    // GATE on TWO conditions, not just the cursor position:
    //   (1) the reveal cursor reached the live edge (reveal_cp_ >= total_cp),
    //       AND
    //   (2) the scramble→resolve animation at the trailing edge has FULLY
    //       settled.
    // Why (2): the scramble tip shows random glyphs for ~scramble_ms after a
    // codepoint is REVEALED (the scramble is anchored to the cursor, not to
    // byte arrival), and the gradient cools over a bit longer. The finalize
    // ramp can drive the CURSOR to the edge FAST (200 ms, or instantly on a
    // small backlog) while the freshest codepoints the sweep just exposed
    // are still INSIDE that scramble window. If we flip live_ off the
    // instant the cursor lands, the LAST live frame painted (and thus what
    // maya's prev_cells / the host's freeze snapshot captured) still has
    // scramble garbage on the final glyphs — and because the widget is no
    // longer live there's no further animation frame to repaint them clean.
    // The result is permanent scrambled junk frozen on the tail of a
    // settled message (e.g. "…/agentty.\u03b2\u03b1\u2588=*\u25a1e"). Holding live_
    // until the tail has visually resolved guarantees the final live frame
    // IS clean text, so the freeze handoff captures clean cells. The settle
    // threshold matches decorate_text_reveal's scramble_active window:
    //   scramble_ms + scramble_len * char_step_ms
    // = 220 + 6*26 = 376 ms (derived below from anim::TextRevealParams).
    //
    // CRITICAL: the window is measured from reveal_edge_reached_ms_ (when
    // the CURSOR reached the edge), NOT from last_grow_ms_ (when the last
    // byte arrived). On a long message whose bytes all landed seconds ago,
    // the ramp sweeps the cursor across a big backlog quickly; last_grow_ms_
    // would already be > 376 ms in the past and the gate would pass
    // immediately, freezing the still-scrambling cursor-swept tail. Keying
    // off the cursor-edge clock gives the swept tail its full window.
    // The settle window MUST equal decorate_text_reveal's worst-case
    // scramble window, or a tunable change there silently freezes scramble
    // glyphs onto the settled tail (#5). Derive it from the ONE source of
    // truth (anim::TextRevealParams) instead of hardcoding 220 + 6*26 here:
    // a default-constructed params carries the same tunables the decorator
    // uses, and settle_window_ms() is the exact expression the decorator's
    // internal "caught-up" check computes. static_assert pins the legacy
    // value so an accidental default change is caught at compile time.
    // The gate must cover the WORST-CASE scramble window. The prose overlay
    // now speed-scales scramble_ms/char_step_ms up to 2.5× the defaults on a
    // slow wire (see the decoration above), so a fixed 376 ms gate could flip
    // live_ off while the tail is still churning — freezing scramble glyphs
    // onto settled text. Multiply the single-source-of-truth window by the
    // same max scale.
    constexpr double kMaxScrambleScale = 2.5;
    constexpr std::int64_t kScrambleSettleMs = static_cast<std::int64_t>(
        anim::TextRevealParams{}.settle_window_ms() * kMaxScrambleScale);
    static_assert(anim::TextRevealParams{}.settle_window_ms() == 220 + 6 * 26,
                  "reveal settle window drifted from decorate_text_reveal "
                  "defaults; the finalize gate would freeze scramble glyphs "
                  "onto settled text");
    const std::int64_t edge_age_ms =
        cursor_at_edge ? (ms_total - reveal_edge_reached_ms_) : 0;
    const bool tail_visually_settled = edge_age_ms >= kScrambleSettleMs;
    if (finalize_armed_ && cursor_at_edge) {
        if (tail_visually_settled) {
            finalize_armed_ = false;
            finalize_deadline_ms_ = 0;
            finalize_hard_        = false;
            finalize_armed_size_  = 0;
            live_ = false;
            anim::keep_animating();
            return true;
        }
        // Cursor is at the edge but the scramble is still resolving. Keep
        // the ramp armed and request another frame so the tail animates to
        // its clean settled glyphs BEFORE we drop live_. is_finalizing()
        // stays true, so the host keeps the 16 ms frame armed and re-checks
        // this gate every frame until the tail cools.
        anim::keep_animating();
    }

    // Age of the trailing edge that the overlay's scramble/gradient
    // decoration reads (p.edge_age_ms). This MUST agree with the
    // ramp-completion gate above: both measure "how long since the cursor
    // reached the edge", NOT "how long since the last byte arrived".
    //
    // The scramble is anchored to the CURSOR (decorate_text_reveal
    // scrambles [unrevealed_cp, unrevealed_cp + scramble_n) with age =
    // edge_age + dist*char_step), so a codepoint is "freshly typed" the
    // moment the cursor sweeps it. If we fed last_grow_ms_ here, a long
    // message whose bytes arrived long ago would render its cursor-swept
    // tail as already-cool (no scramble) even though the cursor only just
    // exposed it — desyncing the visible animation from the gate. Feeding
    // the edge-reached clock makes the overlay scramble the tail for
    // exactly the window the gate waits on, so the final live frame is
    // provably clean text before live_ drops.
    cursor_advance_age_tail_ms_ =
        cursor_at_edge ? (ms_total - reveal_edge_reached_ms_) : 0;

    // Refresh the visible-byte clip for this frame. With reveal_fx_ on
    // and live_, build()'s tail extraction clamps to this value so
    // render_tail's eager-styled blocks (heading bold, code-fence
    // border, table rules, bullet markers) don't paint ahead of the
    // typewriter cursor. With reveal_fx_ off OR not live, the clip is
    // source_.size() (no-op). Commit gating in append_safe commits
    // completed blocks BEHIND the reveal cursor (find_block_boundary),
    // keeping the live tail bounded to ~one block no matter how long the
    // turn is; finish() flushes any remainder in one snap at end-of-stream.
    if (reveal_fx_ && live_) {
        const std::size_t cursor_cp =
            static_cast<std::size_t>(reveal_cp_);
        const std::size_t new_clip = byte_offset_for_cp(cursor_cp);
        if (new_clip != reveal_byte_clip_) {
            reveal_byte_clip_ = new_clip;
            build_dirty_ = true;  // tail visible window moved — re-render
        }
    } else {
        reveal_byte_clip_ = source_.size();
    }

    return false;
}

const Element& StreamingMarkdown::render_live_overlay_() const {
        if (!live_) return cached_build_;
        if (!reveal_fx_) return cached_build_;

        // ── Geeky-as-fuck live animation ──
        //
        // Three layered effects on the trailing edge of the live tail:
        //
        //   (1) SCRAMBLE → RESOLVE
        //       Newly-arrived chars start as random ASCII/box glyphs
        //       and "decrypt" into the real char over ~180ms. Drives
        //       a strong "text materialising in real time" feel.
        //
        //   (2) HOT → COOL GRADIENT TRAIL
        //       The last ~24 codepoints follow a per-char color
        //       gradient indexed by age:
        //           freshest  →  hot magenta + bold
        //           young     →  bright cyan
        //           settling  →  ice blue, dim
        //           locked    →  base fg
        //       Each char migrates leftward through the gradient as
        //       new ones arrive on the right.
        //
        //   (3) PULSING BLOCK CARET with TRAILING TRACER
        //       Wide block (▊) cycling magenta→cyan, plus a thin
        //       trailing dim half-block to the right that fades over
        //       300ms — makes the caret feel like it's leaving a
        //       trail as it moves forward.
        //
        // All three driven by the framework's frame requests so they
        // continue ticking at ~60 fps. Pure visual layer: cached_build_
        // is not touched; every frame builds cached_live_ fresh from
        // a shallow copy + mutated tail-leaf TextElement.
        //
        // The reveal cursor (reveal_cp_) was already advanced this
        // frame by advance_reveal_cursor_(), called from build() top
        // BEFORE its cache short-circuit — so the cursor and the
        // visual decoration are in sync. We just read the scratch.
        const std::int64_t ms_total    = cursor_advance_ms_total_;
        const std::size_t  total_cp    = cursor_advance_total_cp_;
        const std::size_t  revealed_cp = static_cast<std::size_t>(reveal_cp_);
        const std::int64_t age_at_tail_ms = cursor_advance_age_tail_ms_;

        // ── Idle-overlay memo (steady-state fast path) ──────────────────
        //
        // build() reaches here every animation frame while the widget is
        // live_ — including the long stretch AFTER the cursor has caught up
        // to the live edge and the trail has cooled, when the ONLY moving
        // part is the end-caret pulse. In that steady state the overlay's
        // output is a pure function of a handful of quantities:
        //   • the committed prefix (prefix_->generation),
        //   • the tail bytes (source_version_),
        //   • the reveal cursor position (revealed_cp),
        //   • the trailing-edge age (age_at_tail_ms), which drives the
        //     scramble/gradient — but only while < the cool threshold,
        //   • the caret pulse phase (ms_total bucketed to the 650 ms
        //     pulse's visible-step granularity).
        //
        // Everything else in this function (the child-vector reshape, the
        // find_last_text spine walk, decorate_text_reveal's re-slice +
        // run rebuild, the end-caret recolour) is deterministic work that
        // reproduces the SAME cached_live_ when those inputs are unchanged.
        // Recomputing it burns a few µs per idle frame that scales with the
        // committed block count (the spine walk + the child refresh) and the
        // tail length (decorate's re-slice) — the per-frame floor a long
        // live turn pays between deltas, i.e. the stream_md_lag_test
        // steady-frame regression. Memoise: when the signature below is
        // unchanged from the last overlay build, return cached_live_ as-is.
        //
        // The caret pulse bucket uses a coarse step (the pulse only visibly
        // changes a few times a second) so a genuinely-idle frame with the
        // same bucket is a no-op — matching the frame-request cadence at the
        // bottom, which already declines to re-arm 60 fps once the reveal is
        // out of motion. While the tail is still hot (age below the cool
        // threshold) the age term forces the signature to move every frame
        // so the scramble/gradient still animate normally.
        {
            constexpr std::int64_t kTrailCoolMs   = 700;
            constexpr std::int64_t kCaretBucketMs  = 60;   // pulse visible step
            // Quantum for the scramble/gradient age term. While the tail is
            // hot the age drives the FX, but feeding it EXACT (ms-precise)
            // made the signature move every single frame, so the memo below
            // never hit and every glide frame paid the full spine walk +
            // decorate re-slice. The scramble/gradient change imperceptibly
            // between ~33 ms steps, so bucket the age to ~30 fps: consecutive
            // frames inside one bucket reuse the cached overlay (memo hit)
            // AND — because the memo'd frame re-arms at the SAME cadence
            // below — emit ZERO bytes to the terminal. This halves the reveal
            // repaint rate the host terminal must parse / shape / GPU-upload
            // during a live stream (kitty-class GPU emulators spend real CPU
            // per repainted row), for no perceptible loss: the gradient cools
            // over 700 ms, so ~21 steps still read smooth.
            constexpr std::int64_t kAgeBucketMs = 33;
            const bool tail_hot = age_at_tail_ms < kTrailCoolMs;
            // Age term: bucketed while hot (drives scramble/gradient but lets
            // same-bucket frames hit the memo), pinned to the cool sentinel
            // once cooled so it stops perturbing the sig entirely.
            const std::int64_t age_term =
                tail_hot ? (age_at_tail_ms / kAgeBucketMs) : (kTrailCoolMs / kAgeBucketMs);
            OverlaySig sig{
                prefix_->generation,
                source_version_,
                revealed_cp,
                total_cp,
                reveal_byte_clip_,
                age_term,
                ms_total / kCaretBucketMs,
            };
            if (overlay_sig_valid_ && overlay_sig_ == sig
                && std::holds_alternative<BoxElement>(cached_live_.inner)) {
                // Nothing visible changed since the last overlay frame; the
                // previously-decorated cached_live_ is still exact. Still
                // honour the frame-request cadence so the caret keeps
                // pulsing when the bucket eventually ticks.
                //
                // Re-arm 60 fps for the WHOLE hot-tail window (age <=
                // kTrailCoolMs), not a shorter 360 ms slice: the gradient
                // trail visibly cools until kTrailCoolMs, so cutting the 60
                // fps re-arm off at 360 ms left the 360..700 ms fade-out
                // stepping at the coarse 100 ms non-sync bucket — a visible
                // stutter in the cool-down. Keying both on the same
                // kTrailCoolMs constant keeps the position glide and the FX
                // fade on ONE cadence.
                if (age_at_tail_ms <= kTrailCoolMs || revealed_cp < total_cp) {
                    anim::keep_animating();
                } else {
                    // Fully cooled, cursor at edge: only the caret pulses.
                    // Wake exactly at the next phase-bucket boundary so a
                    // non-sync terminal doesn't tear the chrome at 60 fps
                    // for a pulse that only changes a few times a second.
                    static const std::int64_t kAnimPhaseMs =
                        ansi::env_supports_synchronized_output() ? 33 : 100;
                    anim::keep_animating_after(
                        kAnimPhaseMs - (ms_total % kAnimPhaseMs));
                }
                return cached_live_;
            }
            overlay_sig_       = sig;
            overlay_sig_valid_ = true;
        }

        // Walk a copy of cached_build_ down its rightmost spine to
        // the last leaf TextElement. Descends through ComponentElement
        // by materializing it (calling its render()) and REPLACING it in
        // the parent with the materialized Element — so subsequent
        // mutations land in the copy, not in the cached lambda's capture.
        //
        // `eager_render` is set true when the descent reveals that the
        // tail is a structured block (table / list / blockquote / code).
        // Heuristic: the inline-fallback path produces a flat shape
        // (outer vstack → prefix? → tail TextElement) where the tail-slot
        // child is DIRECTLY a TextElement. Any descent that goes deeper
        // than that on the tail side — through a ComponentElement OR
        // through a nested BoxElement — means the tail is an eager
        // render whose rightmost leaf is just one cell / item / token,
        // NOT the full live byte slice. Source-level cursor-cut on that
        // leaf would erase it while the rest of the structure stays
        // visible (garbled rows / empty last cell), so the caller skips
        // the cut for eager renders and only applies the visual edge
        // effects (scramble, gradient, caret).
        bool eager_render = false;
        auto find_last_text = [&eager_render](Element& root) -> TextElement* {
            // Reset descent state on every call — the lambda is invoked
            // twice per frame (trail + caret) and we want the eager_render
            // signal computed fresh each time, not accumulated.
            eager_render = false;
            int  tail_depth        = 0;
            bool entered_tail_slot = false;
            Element* cur = &root;
            for (int step = 0; step < 64; ++step) {
                if (auto* t = std::get_if<TextElement>(&cur->inner)) {
                    return t->content.empty() ? nullptr : t;
                }
                if (auto* b = std::get_if<BoxElement>(&cur->inner)) {
                    if (b->children.empty()) return nullptr;
                    cur = &b->children.back();
                    if (entered_tail_slot) {
                        // We're already past the tail slot — any deeper
                        // Box is a structural shape (list-items Box,
                        // table-rows Box, blockquote Box).
                        if (++tail_depth > 0) eager_render = true;
                    } else {
                        // Stepping out of the outer vstack INTO its
                        // tail slot. From the next step onward, depth
                        // counts.
                        entered_tail_slot = true;
                    }
                    continue;
                }
                if (auto* c = std::get_if<ComponentElement>(&cur->inner)) {
                    // Descent hit a ComponentElement. We cannot
                    // materialize it here — the render lambda needs the
                    // actual avail_w handed down by the layout engine,
                    // and calling c->render(0, 0) at descent time
                    // produces a width-0 layout: tables collapse to
                    // one-cell columns, code blocks rewrap to one
                    // glyph per line, and that corrupted Element
                    // would REPLACE the live ComponentElement in the
                    // animated tree, becoming what the paint pass
                    // renders. (Bug seen as char-chopped tables
                    // mid-stream.) Eager-rendered tails (table /
                    // list / blockquote / codeblock) don't need a
                    // TextElement leaf to splice the cursor into
                    // anyway — the caller skips source-cut and the
                    // visual edge effects do not apply when the
                    // rightmost leaf is structural rather than a
                    // flat text run. Mark eager_render and bail.
                    (void)c;
                    eager_render = true;
                    return nullptr;
                }
                return nullptr;
            }
            return nullptr;
        };

        // Companion to find_last_text for the EAGER (component-wrapped) tail.
        // When find_last_text bails (returns nullptr with eager_render = true)
        // the live block is a TABLE or fenced CODE block whose rows live
        // behind a ComponentElement; this walks the same rightmost spine and
        // hands back that component so the caller can wrap its render() and
        // glide the newest materialised row.
        auto find_last_eager_component =
            [](Element& root) -> ComponentElement* {
            Element* cur = &root;
            for (int step = 0; step < 64; ++step) {
                if (auto* c = std::get_if<ComponentElement>(&cur->inner))
                    return c;
                if (auto* b = std::get_if<BoxElement>(&cur->inner)) {
                    if (b->children.empty()) return nullptr;
                    cur = &b->children.back();
                    continue;
                }
                return nullptr;   // TextElement / list — not the component case
            }
            return nullptr;
        };

        // Step-back / age / colour / scramble helpers now live in the
        // framework primitive (maya::anim, anim/text_reveal.hpp). The overlay
        // here only walks to the tail leaf (find_last_text above) and hands
        // the tunables below to anim::decorate_text_reveal.

        // Tunables.
        //
        // kTrailLen covers the gradient band. kScrambleLen is the
        // churn window at the very tip. kGhostExtra is how far PAST
        // the gradient the ghost (faded-fg) overlay extends when the
        // reveal cursor still has bytes to walk — gives the typewriter
        // its visible "materialising" body without changing height.
        constexpr std::size_t  kTrailLen    = 36;
        constexpr std::size_t  kScrambleLen = 6;
        constexpr std::int64_t kScrambleMs  = 220;
        constexpr std::int64_t kCharStepMs  = 26;
        constexpr std::size_t  kGhostExtra  = 96;
        // (Eager glide reuses the prose tunables above — no separate table
        //  constants; reveal_frac + line_bounded confine it to the live row.)

        // Shallow copy of cached_build_; we'll splice a freshly-built
        // tail TextElement onto its rightmost leaf.
        //
        // Note: this copies the outer vstack BoxElement and its
        // children vector. Children are:
        //   [prefix_component, tail_text]  (or one of those alone)
        // The prefix is a ComponentElement — its copy is O(1) (one
        // std::function copy + shared_ptr refcount bump). The actual
        // committed-prefix content lives inside the lambda's captured
        // shared_ptr<CommittedPrefix>; copying the ComponentElement
        // does NOT touch the prefix block trees, and the renderer's
        // component_cache (keyed on hash_id) keeps the painted cells
        // warm across frames. So this copy is O(1) regardless of
        // transcript length — long sessions stay cheap.
        //
        // Build the animated body WITHOUT re-copying the committed prefix
        // every frame. cached_build_ is vstack[block0..blockN, tail]: each
        // committed block is its OWN immutable hash-keyed ComponentElement
        // (the renderer blits its cells from the component cache), and the
        // overlay only ever decorates the LAST child (the live tail leaf).
        // Copying the whole children vector each frame is O(committed block
        // count) — the residual long-turn streaming cost after the commit
        // fix. Instead:
        //   • when the committed prefix is unchanged (prefix generation
        //     stable) AND cached_live_ already mirrors cached_build_'s shape
        //     (same outer BoxElement, same child count, non-empty), reuse
        //     cached_live_'s committed children in place and refresh ONLY
        //     its last child by value-copying cached_build_'s last child;
        //   • otherwise (prefix grew / shape changed / first overlay frame)
        //     rebuild cached_live_ from cached_build_ once.
        // Either way the per-frame work is O(tail), not O(turn).
        //
        // Aliasing safety: cached_live_'s children are independent value
        // copies of cached_build_'s immutable ComponentElements (made on the
        // rebuild frame); they hold no reference into cached_build_. The
        // per-frame refresh value-copies one child (children.back()), so
        // there is no shared mutable state and no dangling reference between
        // the two caches. We then decorate cached_live_ in place via
        // find_last_text(cached_live_) and RETURN cached_live_ directly.
        const bool reuse_prefix =
            live_overlay_prefix_gen_ == prefix_->generation
            && std::holds_alternative<BoxElement>(cached_live_.inner)
            && std::holds_alternative<BoxElement>(cached_build_.inner)
            && std::get<BoxElement>(cached_live_.inner).children.size()
                   == std::get<BoxElement>(cached_build_.inner).children.size()
            && !std::get<BoxElement>(cached_build_.inner).children.empty();

        if (reuse_prefix) {
            // Refresh only the tail child (last); committed children stay put.
            auto& live_box  = std::get<BoxElement>(cached_live_.inner);
            auto& build_box = std::get<BoxElement>(cached_build_.inner);
            live_box.children.back() = build_box.children.back();  // fresh tail
        } else if (
            // ── Incremental prefix-growth arm ──
            // The generation moved (a commit landed) but the shape is pure
            // growth: cached_build_ appended block children while every
            // child cached_live_ already mirrors is an immutable
            // ComponentElement value-equal to its cached_build_ twin (same
            // hash_id — committed blocks never mutate). With the reveal-
            // paced commit gate, commits fire steadily THROUGHOUT a long
            // turn (one per block as the cursor sweeps it), so the full
            // cached_live_ = cached_build_ copy here was O(committed
            // blocks) per commit ≈ O(turn) per block boundary — the
            // residual long-turn overlay cost. Copy only the NEW children
            // (plus refresh the old tail slot, which the commit replaced
            // with a block) — O(new blocks) per commit.
            std::holds_alternative<BoxElement>(cached_live_.inner)
            && std::holds_alternative<BoxElement>(cached_build_.inner)
            && !std::get<BoxElement>(cached_live_.inner).children.empty()
            && std::get<BoxElement>(cached_build_.inner).children.size()
                   >= std::get<BoxElement>(cached_live_.inner).children.size())
        {
            auto& live_box  = std::get<BoxElement>(cached_live_.inner);
            auto& build_box = std::get<BoxElement>(cached_build_.inner);
            const std::size_t keep = live_box.children.size() - 1; // drop old tail
            live_box.children.resize(keep);
            // NO exact-size reserve here (mirrors build()'s growth arm): this
            // arm runs at every commit and fills the vector exactly, so
            // reserve(build_n) leaves capacity == size and the NEXT commit
            // reallocates + moves all N committed children — O(N) per commit,
            // O(N²) over a long turn. push_back's geometric doubling keeps the
            // append amortised O(1).
            for (std::size_t i = keep; i < build_box.children.size(); ++i)
                live_box.children.push_back(build_box.children[i]);
            live_overlay_prefix_gen_ = prefix_->generation;
        } else {
            cached_live_ = cached_build_;   // O(N) — first frame / reshape only
            live_overlay_prefix_gen_ = prefix_->generation;
        }
        // We decorate cached_live_ in place from here on.
        Element& animated_body = cached_live_;
        if (TextElement* tail = find_last_text(animated_body)) {
            // No content-cut here. build() renders the full tail; the
            // reveal effect is purely visual (scramble + gradient on the
            // trailing edge, pulsing caret on the last glyph). Cutting
            // bytes would shrink the tail TextElement's rendered height
            // — the host's chrome (composer / status bar) sees that as
            // a height delta and reflows. The trailing-edge animation
            // delivers the "text materialising" feel without changing
            // layout.
            (void)revealed_cp;
            (void)total_cp;

            const bool clip_active =
                reveal_fx_
                && reveal_byte_clip_ != static_cast<std::size_t>(-1)
                && reveal_byte_clip_ < source_.size();
            // The tail leaf ends AT the reveal cursor (build.cpp gates the
            // tail at the cursor, never rounding a settled-but-uncommitted
            // line whole), so there is NO not-yet-typed content in the leaf
            // to ghost — unrevealed_cp is 0. The scramble + gradient + caret
            // ride the trailing edge of the REVEALED bytes.
            const std::size_t unrevealed_cp = 0;
            (void)clip_active;

            // ── Inert-decoration skip (idle-but-live steady state) ──
            // Once the cursor is AT the live edge (nothing ghosted) and the
            // trailing edge has fully cooled (edge age past the gradient's
            // 700 ms band — see reveal_detail::trail_style — which also
            // covers the 376 ms scramble window), decorate_text_reveal is a
            // provable visual NO-OP: every trail cp takes the base-style
            // branch, no ghost cells, no sweep. But it still re-slices the
            // content, rebuilds the runs vector and re-appends the trail
            // bytes — a few µs of pure waste on EVERY idle animation frame
            // for as long as the turn stays live (the caret pulse keeps
            // frames coming). The eager arm below has always had exactly
            // this gate (age_at_tail_ms <= 720); the prose arm lacking it
            // made an idle prose tail ~5× the cost of an idle eager tail —
            // the per-frame floor a long live turn pays between deltas
            // (stream_md_lag_test's steady-frame ratio). Skipping also
            // PRESERVES the leaf's real styled runs (bold / inline code) in
            // the trail window instead of flattening them to base style, so
            // the settled-cool live frame matches the committed render
            // byte-for-byte AND style-for-style. The end-caret below still
            // runs — it is the only live decoration at this point.
            constexpr std::int64_t kTrailCoolMs = 700;
            const bool decoration_inert =
                unrevealed_cp == 0 && age_at_tail_ms >= kTrailCoolMs;
            if (!decoration_inert) {
            // ── Delegate the trailing-edge decoration to the framework ──
            //
            // The scramble / hot→cool gradient / ghost-materialise / sweep
            // cursor algorithm now lives in maya::anim::decorate_text_reveal
            // (anim/text_reveal.hpp) — a reusable primitive any widget can
            // call on any text leaf. The markdown widget keeps only the
            // tree-specific glue: walking to the live tail leaf (above) and
            // computing the clip-aware unrevealed-cp span (above). The
            // visual algorithm itself is shared. Tunables below mirror the
            // historical constants (kTrailLen / kScrambleLen / kScrambleMs /
            // kCharStepMs / kGhostExtra) so behaviour is unchanged.
            //
            // SCROLLBACK SAFETY (line_bounded). The ghost band is a CONCEAL
            // STYLE (SGR-8), not a content edit, since 468a48d fixed the
            // wrap-boundary word-pop. Concealing is width-stable but it
            // MUTATES the style of every cell in the unrevealed tail — and
            // that tail spans multiple WRAPPED visual lines on a long
            // paragraph. When an upper wrapped line scrolls into native
            // immutable scrollback while still concealed, the later un-
            // conceal rewrites a committed row → the duplicated line
            // reveal_scrollback_test R5 catches. Confine every effect to the
            // leaf's LAST visual line — the only row provably NOT in
            // scrollback — exactly like the eager/table arm below. (Before
            // 468a48d the ghost was width-matched SPACES, a content edit that
            // left committed rows byte-identical, so the unbounded band was
            // safe; the conceal rewrite made line_bounded load-bearing.)
            anim::TextRevealParams rp;
            rp.ms_total              = ms_total;
            rp.edge_age_ms           = age_at_tail_ms;
            rp.revealed_cp           = revealed_cp;
            rp.total_cp              = total_cp;
            rp.clip_active           = clip_active;
            rp.clipped_unrevealed_cp = unrevealed_cp;
            rp.line_bounded          = true;   // bottom visual line only — scrollback-safe
            rp.trail_len             = kTrailLen;
            rp.scramble_len          = kScrambleLen;
            // SCROLLBACK SAFETY: keep the churn's visual signature but emit the
            // REAL glyph, never a random one. The live bottom row can scroll
            // into immutable scrollback at any frame (the overlay is
            // viewport-blind), and a substituted glyph frozen there is
            // permanent corruption — scrollback_oracle_test's SCRAMBLE-GARBAGE
            // check reproduces it ("…like real assistan8??/%8").
            rp.scramble_glyph_safe   = true;
            // Speed-scaled shimmer: the scramble/gradient windows are TIME
            // constants, so at a fast reveal they smear over far more cells
            // than at a slow one — the trail looks inconsistent as the
            // adaptive rate varies. Scale the per-char step (and the scramble
            // duration with it) inversely with the glide rate so the shimmer
            // always covers ~a constant SPATIAL window (~kScrambleLen cells).
            // At the reference 45 cps the values equal the historical
            // constants; clamped so it never gets jittery-fast or sluggish.
            {
                const double rate = reveal_rate_cursor_.effective_rate();
                constexpr double kRefCps = 45.0;
                double scale = (rate > 1.0) ? (kRefCps / rate) : 1.0;
                if (scale < 0.35) scale = 0.35;   // fast wire: keep some churn
                if (scale > 2.5)  scale = 2.5;    // slow wire: don't drag
                rp.scramble_ms  = static_cast<std::int64_t>(kScrambleMs * scale);
                rp.char_step_ms = static_cast<std::int64_t>(kCharStepMs * scale);
            }
            rp.ghost_extra           = kGhostExtra;
            // wrap_width lets line_bounded find the last VISUAL row. Prose is
            // one source line with no '\n', so without it the "last line"
            // bound degenerates to the whole paragraph and every effect
            // re-sweeps the already-settled wrapped rows each frame (the
            // visible "glides, goes back, glides again"), including rows that
            // may already sit in immutable scrollback. last_paint_width_ is
            // the width the layout engine handed this widget on the previous
            // paint; the tail leaf is wrapped at that same width, so it is the
            // correct basis. A stale value after a resize costs at most one
            // frame of the old bound, self-correcting on the next paint.
            rp.wrap_width = last_paint_width() > 0
                                ? static_cast<std::size_t>(last_paint_width())
                                : 0u;
            if (reveal_decorate_) (void)anim::decorate_text_reveal(*tail, rp);
            }
        } else if (eager_render
                   && (age_at_tail_ms <= 720
                       || cursor_advance_total_cp_ > revealed_cp)) {
            // ── Eager (table / code-block) tail: clean left-to-right glide ──
            //
            // find_last_text bailed: the live block is a TABLE or a fenced
            // CODE block whose rows live behind a ComponentElement. Give its
            // NEWEST row the SAME ghost reveal prose uses — cell content
            // materialises out of width-matched blank space behind a bright
            // sweep front with a hot→cool trail — so a streaming table reads
            // exactly like the prose around it.
            //
            // Commit-safe by construction (the property the old whole-row
            // scramble lacked):
            //   • line_bounded confines every effect to the leaf's LAST visual
            //     line, so a multi-line code leaf only ever animates its bottom
            //     row — a line already scrolled into native scrollback is never
            //     rewritten (reveal_scrollback_test R1).
            //   • protect_structure keeps the │/─ borders + column padding
            //     byte-identical, so the frame is height- AND width-stable.
            //   • reveal_frac drives the front off the global reveal cursor's
            //     0..1 progress through the row's SOURCE span — the rendered
            //     row is wider than its source (padding/borders), so a source-
            //     cp cursor wouldn't map onto it; the fraction does.
            //   • no scramble (clean), bounded-rate hash re-key so committed
            //     rows blit unchanged while only the bottom row re-renders.
            if (ComponentElement* comp =
                    find_last_eager_component(animated_body)) {
                // Cursor's horizontal progress (0..1) through the SOURCE row
                // the typewriter is currently on — the newest rendered row's
                // span. Mirrors the prose clip math, expressed as a fraction.
                // Per-LINE (not per-block) because line_bounded confines the
                // reveal to the leaf's bottom visual row — the only row we can
                // prove hasn't scrolled into immutable scrollback.
                double reveal_frac = 1.0;
                const bool clip_active =
                    reveal_fx_
                    && reveal_byte_clip_ != static_cast<std::size_t>(-1)
                    && reveal_byte_clip_ < source_.size();
                if (clip_active) {
                    const std::size_t clip_b =
                        std::min(reveal_byte_clip_, source_.size());
                    std::size_t ls = clip_b;
                    while (ls > 0 && source_[ls - 1] != '\n') --ls;
                    std::size_t le = clip_b;
                    while (le < source_.size() && source_[le] != '\n') ++le;
                    reveal_frac = (le > ls)
                        ? static_cast<double>(clip_b - ls)
                              / static_cast<double>(le - ls)
                        : 1.0;
                    if (reveal_frac < 0.0) reveal_frac = 0.0;
                    if (reveal_frac > 1.0) reveal_frac = 1.0;
                }

                anim::TextRevealParams rp;
                rp.ms_total          = ms_total;
                rp.edge_age_ms       = age_at_tail_ms;
                rp.revealed_cp       = 0;      // front from reveal_frac below;
                rp.total_cp          = 0;      //  cp derived from the row leaf
                rp.reveal_frac       = reveal_frac;
                rp.trail_len         = kTrailLen;
                rp.scramble_len      = 0;
                rp.scramble_ms       = kScrambleMs;
                rp.char_step_ms      = kCharStepMs;
                rp.ghost_extra       = kGhostExtra;
                rp.enable_scramble   = false;  // clean glide, no churn
                rp.enable_gradient   = true;   // hot→cool trail behind the front
                rp.enable_ghost      = true;   // materialise out of blank
                rp.enable_sweep      = true;   // bright cursor rides the front
                rp.enable_caret      = false;  // multi-row block — no end-caret
                rp.protect_structure = true;   // │/─ borders + padding stay put
                rp.line_bounded      = true;   // only the bottom row animates
                auto orig = comp->render;
                const bool decorate = reveal_decorate_;
                comp->render = [orig, rp, decorate](int w, int h) -> Element {
                    // The layout engine hands the REAL width here, and only
                    // here. line_bounded needs it: a live tail row is a single
                    // source line with no '\n', so bounding by newline alone
                    // leaves the window covering every WRAPPED row of the
                    // block — each frame then re-styles rows that already
                    // settled (visible as the tail "going back" mid-glide) and
                    // can rewrite a row that has scrolled into immutable
                    // scrollback, which is the corruption line_bounded exists
                    // to prevent. Copy per call: `rp` is shared by every paint.
                    anim::TextRevealParams rpw = rp;
                    rpw.wrap_width = w > 0 ? static_cast<std::size_t>(w) : 0u;
                    Element out = orig ? orig(w, h) : Element{};
                    // The eager wrapper's render hands back the block's OWN
                    // lazy renderer (another ComponentElement — the table's
                    // width-aware builder). Wrap THAT so we reach the
                    // materialised rows; if it already produced a Box/Text
                    // tree, decorate it directly. Either way decorate only the
                    // newest CONTENT row — exactly like the prose tail.
                    //
                    // NOTE: line_bounded stays TRUE (bottom row only). An
                    // earlier attempt to conceal across ALL rows of a multi-
                    // row eager block (to per-glyph the completed rows above
                    // the cursor under a burst) re-rendered a row that had
                    // already scrolled into native immutable scrollback —
                    // reveal_scrollback_test R5 caught the duplicated line.
                    // The overlay can't see the viewport/scroll position, so
                    // the only row provably NOT in scrollback is the live
                    // bottom one. The residual burst row-pop is bounded, under
                    // the probe's report-only cap, and cosmetic; a scrollback
                    // rewrite is a hard corruption. Bottom-row-only it is.
                    if (auto* inner =
                            std::get_if<ComponentElement>(&out.inner)) {
                        auto inner_render = inner->render;
                        inner->render =
                            [inner_render, rpw, decorate](int w2, int h2) -> Element {
                            Element mat = inner_render ? inner_render(w2, h2)
                                                       : Element{};
                            if (TextElement* leaf =
                                    rightmost_content_text_leaf(mat))
                                if (decorate)
                                    (void)anim::decorate_text_reveal(*leaf, rpw);
                            return mat;
                        };
                        inner->hash_id = {};
                    } else if (TextElement* leaf =
                                   rightmost_content_text_leaf(out)) {
                        if (decorate)
                            (void)anim::decorate_text_reveal(*leaf, rpw);
                    }
                    return out;
                };
                // Re-key (don't clear) the component hash at a bounded rate so
                // the decorated bottom row re-renders (committed rows blit
                // unchanged from the cell cache) without a full layout::compute
                // every frame. Mixing the component's ORIGINAL content hash
                // (which already moves whenever a new row is revealed) with a
                // coarse time bucket re-lays-out on a new row OR ~30 fps; once
                // the gate goes false the stable hash is left intact and the
                // table re-settles cached.
                constexpr std::int64_t kEagerAnimBucketMs = 33;
                const std::uint64_t content_key = comp->hash_id.hash();
                comp->hash_id = CacheIdBuilder{}
                    .add(std::string_view{"eager-reveal-glide"})
                    .add(content_key)
                    .add(static_cast<std::uint64_t>(
                             ms_total / kEagerAnimBucketMs))
                    .build();
            }
        }

        // ── Inline end-caret ──
        //
        // When the typewriter cursor is still walking (unrevealed_cp > 0)
        // we already painted a bright sweep-cursor at the reveal-front
        // above — a second pulse at the tail end would compete with it
        // and confuse the eye. Skip the end-caret in that case. When the
        // cursor HAS caught up, paint the end-caret as the "awaiting next
        // byte" cue.
        const bool cursor_walking =
            cursor_advance_total_cp_ > static_cast<std::size_t>(reveal_cp_);
        if (!cursor_walking) {
            // Pulsing "awaiting next byte" caret on the last glyph —
            // delegated to the framework primitive (anim::decorate_end_caret
            // in anim/text_reveal.hpp). magenta↔cyan over a 650 ms period,
            // recoloured in place (height/width-stable) so no extra row can
            // appear at the live↔settled seam.
            if (TextElement* tail = find_last_text(animated_body)) {
                if (reveal_decorate_)
                    anim::decorate_end_caret(*tail, ms_total, 650);
            }
        } // !cursor_walking

        // Mirror cached_build_'s cross-axis sizing. Stretch is
        // load-bearing here: parent flex layouts use this widget's
        // reported width for space-between math; the BoxElement
        // default of Align::Start collapses the live wrapper to its
        // children's natural width, which then shrinks the message's
        // allotted columns and explodes per-row wrap.
        //
        // animated_body IS cached_live_ (we decorated it in place above),
        // so there is nothing to move — the Stretch sizing came across with
        // the children. Just fall through to the cadence + return.
        (void)animated_body;

        // ── Frame-request cadence ──
        //
        // Two distinct clocks meet here, and conflating them is what made
        // the reveal stutter:
        //
        //   • The COLOR/FX phase (scramble glyphs, gradient hue, caret
        //     pulse) only needs to step a few times a second. Bucketing it
        //     avoids tearing the chrome BELOW the live tail on terminals
        //     without DEC 2026 (Apple Terminal, plain xterm, bare tmux),
        //     where each multi-row repaint paints progressively.
        //
        //   • The REVEAL CURSOR (this widget's own typewriter, advanced
        //     above from wall-clock) needs ~60 fps wakes to unfold text
        //     smoothly. The loop only wakes for animation when something
        //     re-armed a frame request; RAF is that something.
        //
        // Fix: while the reveal cursor is still catching up to the live
        // edge OR the edge moved within the last kRevealActiveMs, re-arm at
        // the animation-frame interval (≈16 ms) so the reveal plays out
        // smoothly. Once the cursor reaches the edge and the wire goes
        // quiet, fall back to the color-phase bucket so the residual caret
        // pulse doesn't keep a non-sync terminal tearing at 60 Hz.
        static const std::int64_t kAnimPhaseMs =
            ansi::env_supports_synchronized_output() ? 33 : 100;
        // The reveal needs ~60fps wakes ONLY while it actually has something
        // new to show each frame: either the cursor is still walking toward
        // the live edge (cursor_catching_up), or the freshest cp are still
        // inside the scramble/gradient "hot" window and visibly changing
        // colour. Once the cursor reaches the edge AND the tip has cooled, the
        // overlay is BYTE- and STYLE-identical frame to frame — re-arming
        // 60fps then is pure wasted CPU (a full UI repaint + terminal write
        // that paints the same pixels). The old code held 60fps for a flat
        // 250 ms after every byte; during steady streaming that is ALWAYS
        // true, pinning the whole turn at 60fps. Gate on real visible change
        // instead: the calm color-phase bucket (33/100 ms) covers the
        // residual caret pulse once the reveal settles.
        const bool cursor_catching_up =
            reveal_cp_ < static_cast<double>(total_cp);
        // "Hot" = the trailing edge is young enough that scramble or the
        // hot end of the gradient is still animating it. Past this the trail
        // is settled and only the (slow) caret pulse remains.
        constexpr std::int64_t kHotTailMs = 360;
        const bool tail_still_hot = age_at_tail_ms <= kHotTailMs;
        const bool reveal_in_motion = cursor_catching_up || tail_still_hot;

        if (reveal_in_motion) {
            // Smooth reveal: one wake per animation frame.
            anim::keep_animating();
        } else {
            // Reveal is settled: cursor at the edge, tail cooled. The only
            // remaining animation is the "awaiting next byte" end-caret
            // pulse. That cue is meaningful for a few seconds after the last
            // byte — but a widget can sit live_ INDEFINITELY (a settled turn
            // that never had finish() called: e.g. a no-tool reply whose
            // size was unchanged on its settle frame, or a thread rehydrated
            // from disk). Re-arming the caret bucket forever then drives a
            // permanent ~10-20 Hz wake loop over the WHOLE idle session:
            // every wake re-runs the host view + this overlay for zero
            // visible change beyond a caret nobody is waiting on. Bound it.
            //
            // Past kCaretPulseWindowMs of edge-idle, stop re-arming — the
            // caret freezes on its current phase and the loop returns to a
            // true idle poll (zero CPU). A real resumed stream re-grows
            // source_, which resets reveal_edge_reached_ms_ to 0 upstream,
            // flips reveal_in_motion true again, and the caret revives on the
            // very next frame. So the cue is preserved exactly where it
            // matters (right after a byte) and dropped only once it's inert.
            constexpr std::int64_t kCaretPulseWindowMs = 4000;
            const bool caret_still_relevant =
                cursor_advance_age_tail_ms_ <= kCaretPulseWindowMs;
            if (caret_still_relevant) {
                // One wake at the next phase-bucket boundary — the caret
                // pulse cadence — instead of a fast re-arm gated on a
                // remembered bucket (which woke a full frame early and
                // needed mutable state to dedup).
                anim::keep_animating_after(
                    kAnimPhaseMs - (ms_total % kAnimPhaseMs));
            }
            // else: inert settled caret — do NOT re-arm. Idle drops to zero.
        }
        return cached_live_;
}

} // namespace maya
