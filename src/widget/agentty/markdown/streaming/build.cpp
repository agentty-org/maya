// build.cpp — StreamingMarkdown::build(), the per-frame element assembler.
//
// Four tiers, fastest-first: untouched → cached; tail-only fast path
// (mutate children.back() in place); commit fast path (swap the prefix
// ComponentElement for one with a fresh generation hash_id); full rebuild.
// A monotonic source_version_ lets each tier short-circuit with O(1)
// compares. The committed prefix is wrapped in one generation-keyed
// ComponentElement so the renderer's component cache amortises its paint.
// The live-mode overlay is delegated to render_live_overlay_ (reveal_fx.cpp).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "maya/core/render_context.hpp"   // available_width — eager pad guard
#include "maya/element/builder.hpp"
#include "maya/render/cache_id.hpp"
#include "maya/style/style.hpp"
#include "maya/widget/agentty/markdown.hpp"
#include "maya/widget/agentty/markdown/internal.hpp"

#include "maya/widget/agentty/markdown/streaming_internal.hpp"

namespace maya {

using ::maya::md_detail::streaming::fnv1a64;

const Element& StreamingMarkdown::build() const {

    // ── Inert fast path ────────────────────────────────────
    // Once a message has SETTLED (finish() flipped live_ off) and the
    // settle-frame rebuild has run (build_dirty_ cleared), the widget is
    // frozen: no reveal cursor to advance, no tail to re-clip, no async
    // parse can still land (streaming is over). cached_build_ is the final
    // transcript Element and can never change until clear()/set_content
    // mutates the widget (both of which re-set build_dirty_). Return it
    // directly — an O(1) branch that skips the async-mutex acquire in
    // maybe_apply_async_(), the UTF-8 scans in advance_reveal_cursor_(),
    // and the two-hop indirection through render_live_overlay_().
    //
    // The host loop already stops requesting frames for a settled widget
    // (is_live()/is_finalizing()/is_parsing() all false), so this path is
    // rarely hit — but it makes a settled widget cheap even when something
    // DOES call build() (a resize relayout, a stray repaint). Guarded on
    // !live_ so the still-animating case is completely untouched, and on
    // !build_dirty_ so the one settle-frame rebuild still happens.
    if (!live_ && !build_dirty_ && !is_parsing()) {
        return cached_build_;
    }

    // Poll any landed async parse — may mutate prefix_/source_ and
    // flip build_dirty_, so it must run BEFORE the early-out.
    maybe_apply_async_();

    // Advance the reveal cursor and republish the tail clip BEFORE
    // the cache short-circuit. Three things this fixes:
    //   1. The clip is current for THIS frame, not the next — no
    //      one-frame flash where a newly-arrived eager-table burst
    //      lands unrevealed then collapses.
    //   2. If reveal_cp_ advances without source changing, we bump
    //      build_dirty_ here so the cached path falls through to
    //      rebuild with the new clip — keeps the typewriter running
    //      through wire stalls.
    //   3. Finalize-ramp completion (which flips live_ off) is
    //      observed before the cache check, so the next frame doesn't
    //      serve a stale live-mode build.
    if (advance_reveal_cursor_()) {
        build_dirty_ = true;
    }

    // Reveal-paced progressive commit: the cursor advances on its own
    // wall clock even when no bytes arrive (draining a backlog, the
    // finalize ramp), and append_safe — the only other commit site —
    // doesn't run on those frames. Poll here so each pending boundary
    // commits the moment the cursor sweeps past it, keeping the live
    // tail bounded to ~one block + the cursor's lag window. No-op when
    // not (reveal_fx_ && live_) or nothing is pending. Committing bumps
    // build_dirty_ + source_version_ internally, so the cache tiers
    // below see a coherent state.
    if (reveal_fx_ && live_)
        const_cast<StreamingMarkdown*>(this)->commit_revealed_();

    // Untouched-since-last-build: return cached. Dominant case when the
    // widget is idle (no streaming).
    if (!build_dirty_) return render_live_overlay_();

    // Visible end-of-source for THIS frame. When reveal_fx_ && live_
    // and advance_reveal_cursor_ has run at least once, clamp to the
    // cursor's byte clip so render_tail can't eager-style bytes the
    // typewriter hasn't reached yet (the heading underline / code-fence
    // chrome / table rules burst). Otherwise it's source_.size().
    //
    // LINE-GRANULAR CLIP (scrollback-safety invariant). The raw cursor
    // clip can fall in the MIDDLE of a physical source line. If that
    // half-line then overflows the viewport and scrolls off into the
    // terminal's native (immutable) scrollback, the committed row is
    // the truncated text — and the reveal cursor catching up later
    // cannot rewrite scrollback. Result: a permanently half-revealed
    // code line frozen in history (the reveal_scrollback_test failure).
    //
    // Fix: round the clip UP to the end of the line it lands in (the
    // next '\n', or source end). Every COMPLETED line is therefore
    // always rendered in full — its wrapped height equals the settled
    // height, so whatever scrolls off matches the final transcript
    // byte-for-byte. Only the single actively-typed last line reveals
    // char-by-char, and that line is the bottom-most live edge which
    // never scrolls off while it's still being typed. The overlay's
    // ghost band (invisible cells for unrevealed cp) animates the
    // within-line typewriter; the line-granular clip just guarantees
    // the bytes are PRESENT so height stays stable. Eager block chrome
    // (heading underline / fence border) is still gated per-line by
    // commit deferral, so revealing a completed line in full doesn't
    // dump ahead-of-cursor structural decoration.
    std::size_t visible_end = source_.size();
    if (reveal_fx_ && live_
        && reveal_byte_clip_ != static_cast<std::size_t>(-1)) {
        std::size_t clip = std::min(source_.size(), reveal_byte_clip_);
        std::size_t bcap = source_.size();
        if (clip < source_.size()) {
            // Gate the tail exactly at the reveal cursor. An earlier design
            // rounded a COMPLETED line up to its '\n' so it rendered whole —
            // but a completed *source* line is a whole soft-wrapped
            // *paragraph* (prose is one source line per paragraph; build has
            // no width and cannot know the wrap points). Rounding dumped the
            // entire remaining paragraph the instant its '\n' arrived (a
            // measured +172-cell one-frame pop) AND pushed the settled block
            // through render_tail's component path, defeating the prose ghost
            // band. Gating at the cursor keeps the in-progress block a flat
            // TextElement the overlay glides through per glyph.
            //
            // The tail slice [committed_, clip) is entirely UNCOMMITTED and
            // redrawn in place each frame; only commit_range (reveal-paced,
            // block-aligned) freezes bytes into scrollback, and by then the
            // cursor is past the block — so gating here is scrollback-safe.
            //
            // bcap: never let the clip (or the monotonic clamp below) cross a
            // block boundary the cursor hasn't reached. A blank line ('\n\n')
            // ends a block; without this the clamp would walk the clip into
            // the NEXT block, so a completed block stops being the tail's last
            // leaf and the overlay's last-leaf conceal misses it — the
            // paragraph→blockquote pop. (A blank line is width-independent, so
            // build can find it safely.)
            bcap = clip;
            while (true) {
                std::size_t b = source_.find('\n', bcap);
                if (b == std::string::npos) { bcap = source_.size(); break; }
                if (b + 1 < source_.size() && source_[b + 1] == '\n') {
                    bcap = b + 1;   // stop at the blank line's first '\n'
                    break;
                }
                bcap = b + 1;       // soft line break inside the block — keep going
                if (bcap >= source_.size()) break;
            }
            if (clip > bcap) clip = bcap;
        }
        visible_end = clip;
        // Monotonic clip: never reveal LESS than a previous frame did for
        // the same live stream. The reveal cursor advances monotonically, so
        // visible_end should only ever grow — but keep this clamp as a safety
        // belt: a drop would un-render an already-visible row, a height shrink
        // that violates the scrollback-monotonicity invariant. Clamp up to
        // the last frame's clip.
        if (cached_tail_clip_ != static_cast<std::size_t>(-1)
            && cached_tail_clip_ <= source_.size()
            && visible_end < cached_tail_clip_)
            visible_end = cached_tail_clip_;
        // The block-boundary cap wins over the monotonic clamp too: a stale
        // higher cached_tail_clip_ from a previous frame must not re-introduce
        // the next block into the tail. (bcap only ever grows as the cursor
        // advances into later blocks, so this never un-reveals committed work.)
        if (visible_end > bcap) visible_end = bcap;
    }
    std::string_view tail = (committed_ < visible_end)
        ? std::string_view{source_}.substr(committed_, visible_end - committed_)
        : std::string_view{};
    // Keep the tail slot PRESENT while streaming with a committed
    // prefix, even when the tail is momentarily empty. Between two
    // appends that both end on `\n\n`, commit_range may consume all of
    // source_ — for that one frame `tail` is empty. If has_tail
    // tracked emptiness directly, the outer vstack would collapse from
    // [prefix, gap, tail] to [prefix] and DROP the trailing .gap(1) row,
    // then re-add it the instant the next byte arrives: a 1-row down/up
    // bounce at EVERY block boundary that ripples the composer/status
    // bar below (the "chrome flickers while md renders" symptom). While
    // live, reserve the tail slot so the inter-block gap stays put; the
    // empty tail renders as a 0-row element, and the gap matches the
    // steady state the next arriving byte lands in. finish()/settle
    // clears live_, so a genuinely-finished message with no tail does
    // NOT carry a dangling gap.
    const bool has_prefix = !prefix_->blocks.empty();
    const bool has_tail   = !tail.empty() || (live_ && has_prefix);

    // ── Empty special case ──
    if (!has_prefix && !has_tail) {
        cached_build_      = Element{TextElement{""}};
        cached_tail_size_  = 0;
        cached_prefix_gen_ = prefix_->generation;
        cached_has_tail_   = false;
        cached_has_prefix_ = false;
        cached_block_count_ = 0;
        cached_window_lo_   = 0;
        cached_tail_version_     = source_version_;
        cached_tail_clip_        = visible_end;
        build_dirty_       = false;
        return render_live_overlay_();
    }

    // ── Tail-only fast path ──
    //
    // The hot streaming case: prefix unchanged, has_tail/has_prefix
    // shape unchanged, only the tail's content moved. We can mutate
    // cached_build_'s tail child (the LAST child of the outer vstack)
    // in place — every prefix child keeps its address and per-component
    // caches stay warm, while only render_tail's lightweight inline-
    // cache path runs.
    //
    // Sub-fast-path: if the tail body bytes AND the committed-side
    // fence parity are byte-identical to the cache, render_tail's
    // output is provably identical to what's already in
    // children.back() — skip the call and the assignment entirely,
    // leaving cached_build_ wholly untouched. The bytes-equality check
    // matters for the case where build_dirty_ flipped (e.g., a feed
    // arrived) but the resulting tail body didn't actually change
    // (commit_range absorbed the new bytes; the body shape happened
    // to round-trip; any future code path that bumps build_dirty_
    // without changing the tail).

    // Helper: build ONE committed block's hash-keyed ComponentElement.
    // Each committed block is a DIRECT child of the outer vstack (not
    // wrapped in a single prefix ComponentElement). The key is stable
    // CONTENT identity (instance + source byte range + fold state),
    // never a pointer — committed blocks are immutable, so a block's key
    // is fixed the moment it commits. The renderer's hash-keyed cell
    // cache paints each block once and blits it (memcpy/row) every later
    // frame. When a commit appends block N, blocks 0..N-1 keep their keys
    // and blit; ONLY the new block renders AND captures cells. Per-commit
    // cost is therefore O(new block), not O(transcript) — the previous
    // single-wrapper design re-captured the WHOLE prefix's cells on every
    // generation bump (an O(total rows) per-row memcpy at every paragraph
    // boundary), which is the streaming lag on a tall live body.
    //
    // Safe where the EARLIER per-block attempt wasn't: that one used the
    // Element{shared_ptr} ctor keyed on POINTER identity (id_for_shared),
    // hitting the pointer-keyed cache's stale-height blit path that left
    // blank rows on scrollback overflow. Hash-keyed entries go through
    // the content-stable cell cache whose capture path guards OOB reads
    // and width changes.
    auto make_block_component = [&](std::size_t i) -> Element {
        return render_committed_block_(i);
    };

    // ── Committed-prefix windowing (outer-tree O(1)/frame) ──────────
    //
    // Even though make_block_component keeps the WIDGET's per-commit cost
    // O(new block) (blocks 0..N-1 carry stable hash_ids and blit from the
    // renderer's cell cache), the OUTER renderer still walks all N block
    // children EVERY frame: build_layout_tree emplaces N leaf layout
    // nodes, layout::compute runs a flex pass over N items, and
    // paint_element iterates N children. That is O(committed blocks)/frame
    // — the per-frame render() growth on a long streaming turn (the ~30%
    // CPU report).
    //
    // Fix: collapse the OLD committed blocks (which are immutable and sit
    // far above the viewport) into a bounded number of hash-keyed SEGMENT
    // wrapper ComponentElements, each covering a FIXED block range
    // [k*CHUNK, (k+1)*CHUNK). The outer vstack then sees
    //   [seg_0, seg_1, ..., seg_{m-1}, block_lo .. block_{N-1}, tail]
    // = O(N/CHUNK + window) children. Off-screen segments measure O(1)
    // (cached height) and are skipped in paint (off-screen bail), so their
    // per-frame cost is a single hash-map touch.
    //
    // Why FIXED-range segments, not one growing head wrapper: a single
    // wrapper covering [0, lo) has a hash keyed on lo, which advances as
    // the stream grows — so every window slide re-renders the ENTIRE
    // collapsed prefix (O(lo) blocks), giving O(N²) total render work over
    // a long turn (measured: per-commit render calls climbing 8→58, worst
    // 299). A fixed-range segment's hash is keyed on its immutable block
    // range: once the segment is COMPLETE (the window base has advanced
    // past its end) it is frozen forever and renders EXACTLY ONCE. Only
    // the single newest, still-filling segment re-renders as blocks land
    // in it — bounded to O(CHUNK). Total collapsed-prefix render work is
    // therefore O(N/CHUNK) segment renders × O(CHUNK) each = O(N), not
    // O(N²).
    //
    // Row-layout identity (scrollback-safety): the outer container is
    // vstack().gap(1).padding(0,0,0,2). Each segment wrapper is one outer
    // child, so the outer gap(1) sits between segments exactly where the
    // gap between block_{k*CHUNK-1} and block_{k*CHUNK} was; inside a
    // segment, an inner vstack().gap(1) reproduces the gaps between its
    // blocks; and the wrappers carry NO padding (the outer padding already
    // indents the column). So every committed row lands on exactly the row
    // it did before and native scrollback stays byte-identical.
    constexpr std::size_t kWindowBlocks = 32;  // individual-block window
    constexpr std::size_t kSegBlocks    = 32;  // blocks per collapsed segment

    // First block index kept as an individual child. Blocks [0, lo) are
    // collapsed into fixed-size segments. lo is quantised DOWN to a
    // segment boundary so a segment is only ever collapsed as a WHOLE —
    // the newest individual window never straddles a partially-collapsed
    // segment, and a completed segment's block range is fixed forever.
    auto window_lo = [&](std::size_t n) -> std::size_t {
        if (n <= kWindowBlocks) return 0;
        // Largest segment-boundary ≤ (n - kWindowBlocks): everything below
        // it is collapsed into complete segments; the remainder stays
        // individual (≥ kWindowBlocks, < kWindowBlocks + kSegBlocks).
        return ((n - kWindowBlocks) / kSegBlocks) * kSegBlocks;
    };

    // Helper: build ONE segment wrapper collapsing blocks [seg_lo, seg_hi)
    // into a single hash-keyed ComponentElement. Hash-keyed on
    // (instance, seg_lo, seg_hi, fold_generation) — the range is FIXED
    // for a completed segment, so its hash (and cached cells) are stable
    // forever; a fold toggle bumps fold_generation and re-keys it.
    auto make_segment = [&](std::size_t seg_lo, std::size_t seg_hi) -> Element {
        ComponentElement seg;
        seg.hash_id = CacheIdBuilder{}
            .add(std::string_view{"strmd-seg"})
            .add(static_cast<std::uint64_t>(instance_id_))
            .add(static_cast<std::uint64_t>(seg_lo))
            .add(static_cast<std::uint64_t>(seg_hi))
            .add(static_cast<std::uint64_t>(fold_generation_))
            .build();
        auto p = prefix_;
        auto folds = folds_;
        seg.render = [p, folds, seg_lo, seg_hi](int, int) -> Element {
            std::vector<Element> kids;
            kids.reserve(seg_hi - seg_lo);
            for (std::size_t i = seg_lo; i < seg_hi && i < p->blocks.size(); ++i) {
                const auto& meta =
                    (i < p->metas.size()) ? p->metas[i] : BlockMeta{};
                auto fit = folds.find(meta.source_offset);
                const bool folded = fit != folds.end() && fit->second;
                if (folded) {
                    const char* kind_label = "block";
                    switch (meta.kind) {
                        case BlockKind::CodeBlock:  kind_label = "code";       break;
                        case BlockKind::Table:      kind_label = "table";      break;
                        case BlockKind::List:       kind_label = "list";       break;
                        case BlockKind::Blockquote: kind_label = "blockquote"; break;
                        case BlockKind::Paragraph:  kind_label = "paragraph";  break;
                        case BlockKind::Heading:    kind_label = "heading";    break;
                        case BlockKind::HtmlBlock:  kind_label = "html";       break;
                        default: break;
                    }
                    char buf[160];
                    if (!meta.lang.empty()) {
                        std::snprintf(buf, sizeof(buf),
                            "\xe2\x96\xb8 %u line%s of %s (%s) hidden \xe2\x80\x94 unfold",
                            static_cast<unsigned>(meta.line_count),
                            meta.line_count == 1 ? "" : "s",
                            kind_label, meta.lang.c_str());
                    } else {
                        std::snprintf(buf, sizeof(buf),
                            "\xe2\x96\xb8 %u line%s of %s hidden \xe2\x80\x94 unfold",
                            static_cast<unsigned>(meta.line_count),
                            meta.line_count == 1 ? "" : "s",
                            kind_label);
                    }
                    kids.push_back(Element{TextElement{
                        .content = std::string{buf},
                        .style   = Style{}.with_fg(colors::strike_fg()).with_dim(),
                    }});
                } else {
                    kids.push_back(*p->blocks[i]);
                }
            }
            return (detail::vstack().gap(1)(std::move(kids))).build();
        };
        return Element{std::move(seg)};
    };

    // Helper: append the collapsed segments covering [0, lo) into `dst`.
    // lo is always a multiple of kSegBlocks (window_lo quantises it), so
    // this emits floor(lo / kSegBlocks) full segments, each with a fixed
    // block range.
    auto append_segments = [&](std::vector<Element>& dst, std::size_t lo) {
        for (std::size_t s = 0; s < lo; s += kSegBlocks) {
            const std::size_t hi = std::min(lo, s + kSegBlocks);
            dst.push_back(make_segment(s, hi));
        }
    };

    // Helper: append the WINDOWED committed-block children into `dst`:
    //   [seg_0 .. seg_{m-1}, block_lo .. block_{N-1}]
    auto append_block_components = [&](std::vector<Element>& dst) {
        const std::size_t n  = prefix_->blocks.size();
        const std::size_t lo = window_lo(n);
        dst.reserve(dst.size() + (lo / kSegBlocks) + (n - lo));
        append_segments(dst, lo);
        for (std::size_t i = lo; i < n; ++i)
            dst.push_back(make_block_component(i));
    };

    if (cached_prefix_gen_ == prefix_->generation
        && cached_fold_gen_  == fold_generation_
        && cached_has_prefix_ == has_prefix
        && cached_has_tail_   == has_tail
        && std::holds_alternative<BoxElement>(cached_build_.inner)) {
        auto& box = std::get<BoxElement>(cached_build_.inner);
        // Windowed prefix child count: [head_wrapper?, block_lo..N-1].
        // Generation is unchanged here, so N (and therefore lo, the
        // window base) is unchanged too — the prefix child layout is
        // byte-identical and only the tail child can have moved.
        std::size_t prefix_n = 0;
        if (has_prefix) {
            const std::size_t n  = prefix_->blocks.size();
            const std::size_t lo = window_lo(n);
            prefix_n = (lo / kSegBlocks) + (n - lo);
        }
        const std::size_t expected = prefix_n + (has_tail ? 1u : 0u);
        if (box.children.size() == expected) {
            if (has_tail) {
                // Fast path: source_version_ matches what produced the
                // current cached tail AND the reveal clip is unchanged —
                // tail bytes provably byte-identical to last frame. The
                // clip endpoint is part of the tail's identity because
                // build() truncates `tail` by it before render_tail sees
                // it; if the clip moved with version unchanged (typewriter
                // cursor advanced without new bytes), the tail bytes
                // DIFFER from what's in cached_build_, and skipping the
                // re-render would leave the stale longer tail visible —
                // the reveal effectively wouldn't run.
                //
                // Slow path: version moved OR clip moved, fall back to
                // the (fence, length, FNV-1a) digest. This catches the
                // rare case where commit_range fired but the tail
                // happened to land at the same bytes — lets us still
                // skip render_tail when the body round-tripped.
                bool tail_unchanged;
                std::uint64_t tail_hash = 0;
                if (cached_tail_version_ == source_version_
                    && cached_tail_clip_ == visible_end)
                {
                    tail_unchanged = true;
                } else if (cached_tail_version_ == source_version_) {
                    // Source bytes are unchanged (version matches) but the
                    // reveal clip moved — the typewriter cursor advanced
                    // without new bytes. The tail SLICE therefore changed
                    // length, so it IS different and must re-render. Skip
                    // the O(tail) fnv1a64 digest: it can only confirm
                    // "different" (we already know that from the clip move),
                    // and computing it every animation frame over the whole
                    // uncommitted tail is the long-turn streaming lag. Leave
                    // tail_hash = 0; render_tail's own (version,len)-gated
                    // memo keeps the re-render cheap, and the stale
                    // cached_tail_hash_ is harmless (the next real byte
                    // delta bumps source_version_ and recomputes it).
                    tail_unchanged = false;
                } else {
                    tail_hash = fnv1a64(tail);
                    tail_unchanged =
                        cached_tail_in_fence_ == in_code_fence_
                        && cached_tail_len_   == tail.size()
                        && cached_tail_hash_  == tail_hash;
                }
                if (!tail_unchanged) {
                    // Swap in the new tail. children.back() is the
                    // tail slot regardless of whether prefix is
                    // present:
                    //   [prefix, tail] → children[1]
                    //   [tail]         → children[0]
                    box.children.back() = render_tail(tail);
                    cached_tail_hash_     = tail_hash;
                    cached_tail_len_      = tail.size();
                    cached_tail_in_fence_ = in_code_fence_;
                }
                cached_tail_version_     = source_version_;
                cached_tail_clip_        = visible_end;
            }
            cached_tail_size_ = tail.size();
            build_dirty_      = false;
            return render_live_overlay_();
        }
    }

    // ── Commit fast path ──
    //
    // Generation moved (commit_range fired / a fold toggled), but the
    // has_prefix / has_tail shape is unchanged. Rebuild the outer
    // vstack's children in place: re-emit the per-block components
    // (blocks 0..N-1 carry UNCHANGED hash_ids — the renderer blits them
    // from cache without re-rendering or re-capturing; only the newly
    // committed block N misses and renders) plus refresh the tail. No
    // builder pipeline, no .build() call.
    //
    // This is the hot path during a long streaming reply: a commit fires
    // at every paragraph / fenced-block boundary (~10-100x per turn). The
    // per-block direct-child layout means each commit costs O(new block),
    // not O(transcript) — the fix for the tall-live-body streaming lag.
    if ((cached_prefix_gen_ != prefix_->generation
         || cached_fold_gen_  != fold_generation_)
        && cached_has_prefix_ == has_prefix && has_prefix
        && cached_has_tail_   == has_tail
        && std::holds_alternative<BoxElement>(cached_build_.inner)) {
        auto& box = std::get<BoxElement>(cached_build_.inner);

        // Incremental-append sub-path: the common commit shape is a PURE
        // GROWTH of the prefix — commit_range appended one or more new
        // blocks while every already-committed block kept its identity
        // (same source range, same fold state). When folds are unchanged
        // (cached_fold_gen_ matches) and the cached children vector still
        // holds exactly [block0..block_{M-1}, tail] for the OLD block count
        // M, we can keep blocks 0..M-1 in place (their ComponentElements
        // are immutable and value-equal to what append_block_components
        // would re-emit — same hash_id, so the renderer blits them either
        // way) and only build the NEW blocks M..N-1. That turns the
        // per-commit cost from O(total blocks) back to O(new blocks),
        // which is the whole point of the per-block direct-child layout —
        // without it, a 250-block turn re-wraps all 250 block components at
        // every paragraph boundary (the residual streaming-window cost).
        const std::size_t new_n = prefix_->blocks.size();
        const std::size_t new_lo = window_lo(new_n);
        const std::size_t old_total = box.children.size();

        // Reconstruct the OLD prefix child model from the tracked window
        // state: [seg_0..seg_{old_lo/kSeg-1}, block_old_lo..block_{old_n-1},
        // tail?].
        const std::size_t old_n  = cached_block_count_;
        const std::size_t old_lo = cached_window_lo_;
        const std::size_t old_segs    = old_lo / kSegBlocks;
        const std::size_t old_window_n = (old_n >= old_lo) ? (old_n - old_lo) : 0u;
        const std::size_t old_tail    = cached_has_tail_ ? 1u : 0u;
        const std::size_t expected_old_total = old_segs + old_window_n + old_tail;

        // Pure same-window growth: the window base lo did NOT advance (no
        // new segment collapsed), folds are unchanged, blocks only grew,
        // and the cached child vector matches the reconstructed old model.
        // Then every collapsed segment is byte-identical (same fixed range,
        // same fold_gen → same hash), the individual blocks [old_lo, old_n)
        // stay put, and we append the NEW individual blocks [old_n, new_n).
        // Per-commit cost stays O(new blocks). When lo advanced, a new
        // segment must be collapsed and the individual window re-based —
        // that reshape falls through to the full re-emit below (which is
        // still bounded: it re-wraps only O(N/kSeg) segment handles +
        // O(window) blocks, and the segments blit from cache).
        const bool pure_growth =
            cached_fold_gen_ == fold_generation_
            && new_n >= old_n
            && new_lo == old_lo
            && old_total == expected_old_total;

        if (pure_growth) {
            // Drop the old tail slot (if any); append the new block
            // components old_n..new_n-1; then push the fresh tail.
            //
            // NO reserve() here — and that absence is load-bearing. This arm
            // runs on EVERY commit (once per block boundary on a long turn),
            // and it fills the vector exactly, so a reserve leaves
            // capacity == size behind; the NEXT commit's exact reserve then
            // always reallocates, moving all committed children — O(N) per
            // commit, O(N²) per turn. push_back's amortised geometric
            // doubling makes the same appends O(1) amortised.
            if (cached_has_tail_ && !box.children.empty())
                box.children.pop_back();
            for (std::size_t i = old_n; i < new_n; ++i)
                box.children.push_back(make_block_component(i));
            if (has_tail) {
                std::uint64_t tail_hash = fnv1a64(tail);
                box.children.push_back(render_tail(tail));
                cached_tail_hash_     = tail_hash;
                cached_tail_len_      = tail.size();
                cached_tail_in_fence_ = in_code_fence_;
                cached_tail_version_  = source_version_;
                cached_tail_clip_     = visible_end;
            }
            cached_prefix_gen_  = prefix_->generation;
            cached_fold_gen_    = fold_generation_;
            cached_block_count_ = new_n;
            cached_window_lo_   = new_lo;
            cached_tail_size_   = tail.size();
            build_dirty_        = false;
            return render_live_overlay_();
        }

        // Window slide / fold toggle / non-growth reshape: fall back to a
        // full children re-emit (still O(window), but rare — a window
        // slide happens once per kWindowChunk commits, folds change
        // rarely).
        std::vector<Element> kids;
        append_block_components(kids);
        if (has_tail) {
            std::uint64_t tail_hash = fnv1a64(tail);
            kids.push_back(render_tail(tail));
            cached_tail_hash_     = tail_hash;
            cached_tail_len_      = tail.size();
            cached_tail_in_fence_ = in_code_fence_;
            cached_tail_version_  = source_version_;
            cached_tail_clip_     = visible_end;
        }
        box.children = std::move(kids);
        cached_prefix_gen_  = prefix_->generation;
        cached_fold_gen_    = fold_generation_;
        cached_block_count_ = new_n;
        cached_window_lo_   = new_lo;
        cached_tail_size_   = tail.size();
        build_dirty_        = false;
        return render_live_overlay_();
    }

    // ── Full rebuild ──
    //
    // Either the prefix generation moved (commit_range fired), the
    // shape (has_prefix / has_tail) changed, or the cache was reset.
    // Build the outer vstack with N + (has_tail ? 1 : 0) children:
    //   children[0 .. N-1] = committed prefix blocks (deep-copied)
    //   children[last]     = render_tail() output (when has_tail)
    //
    // Why direct children instead of wrapping in a ComponentElement.
    // The earlier approach wrapped each prefix shared_ptr<const Element>
    // via the Element{sp} converting constructor, producing one
    // ComponentElement per block whose hash_id was derived from sp
    // identity. The renderer then cached per-block layout+paint cells
    // by (sp, width), making per-commit cost O(new_block_size) instead
    // of O(transcript_size). The trade was correctness: the
    // ComponentElement cache hit a cells_rows < content_h blit path
    // where the cached cell snapshot was shorter than the layout-
    // allocated content height. When the inline frame later overflowed
    // term_h and pushed rows into the terminal's native (immutable)
    // scrollback, those short cell ranges left blank rows above and
    // below the block's actual content — the "ghost rectangles" that
    // accumulate after 2-3 turns of long streaming. Direct children
    // bypass the cells cache entirely: every frame's layout walks the
    // prefix's pre-built Element values directly, producing the same
    // cells the live frame would have rendered, so the cells captured
    // into native scrollback during overflow are correct.
    //
    // Cost: O(transcript_size) deep-copies of Element variant bodies
    // per generation bump. Pricey on long sessions, but maybe_virtualize
    // bounds the relevant tail of the transcript — older turns get
    // committed to scrollback and their blocks drop out of the active
    // prefix entirely.
    std::vector<Element> outer_children;
    outer_children.reserve(
        (has_prefix ? prefix_->blocks.size() : 0u) + (has_tail ? 1u : 0u));

    if (has_prefix) {
        // Each committed block is its OWN hash-keyed ComponentElement,
        // pushed as a DIRECT child of the outer vstack. The renderer's
        // component_cache stores rendered cells per block hash_id; a new
        // commit changes ONLY the appended block's presence, so blocks
        // 0..N-1 keep their keys and blit (memcpy/row) while only block N
        // renders + captures. Per-commit cost is O(new block), not
        // O(transcript) — the whole-prefix re-capture that made a tall
        // live body lag at every paragraph boundary is gone. Hash-keyed
        // (not pointer-keyed) entries avoid the stale-height blit that
        // caused the earlier per-block attempt's scrollback ghost rows.
        append_block_components(outer_children);
    }
    if (has_tail) {
        outer_children.push_back(render_tail(tail));
    }

    // align_self(Stretch) forces this vstack to claim the parent's full
    // cross-axis size, regardless of how much its OWN children would
    // naturally span. Without this, early streaming frames where the
    // tail is a short inline-only paragraph (one wrapped row or less)
    // produce a vstack whose natural width = tail length = short, and
    // any parent header doing justify-content: space-between against
    // that width packs its children to the left. Setting Stretch makes
    // the streaming widget's width invariant across the
    // short-tail → multi-row-tail → committed-blocks transitions —
    // the parent's flex math sees the same number every frame, so the
    // header layout doesn't flicker between content-sized and
    // terminal-sized.
    // The 2-column left pad is decoration; the TEXT is the point. On an
    // ultra-narrow surface (a 1-2 column canvas: a terminal mid-resize
    // reporting a degenerate width, a deeply nested container whose
    // ancestors' chrome has eaten everything) that pad would consume the
    // entire content area and the document would render as NOTHING —
    // silent data loss, which is the worst failure a renderer has. So the
    // pad must yield on a degenerate width.
    //
    // ── DO NOT wrap this in a component() ────────────────────────────
    //
    // It was, and that single wrapper is the worst streaming regression
    // this widget has had. A ComponentElement is a DEFERRED subtree: with
    // no measure callback the engine auto-measures by INVOKING render()
    // during layout, and the cross-frame cache that would absorb it is
    // keyed on hash_id — an empty hash_id gets pointer keying, whose
    // cross-frame hits are deliberately rejected. So an un-keyed component
    // re-renders its whole subtree every frame, forever. Wrapping the
    // document to decide ONE pad dragged every committed block plus the
    // live tail into a per-frame rebuild, and the reveal cursor — a
    // deadline system sharing this thread — started missing frames.
    //
    // Measured (reveal_cost_probe / reveal_smoothness_probe):
    //   component renders/frame 3.54 -> 4.54   (+1.00: one un-cached comp)
    //   layout 93us -> 144us, paint 60us -> 129us
    //   streaming stall frames  18 -> 112      (6.2x)
    //
    // Hoisting the built tree OUT of the lambda so the builder body is
    // O(1) does NOT fix it — measured, still 112. The deferral itself is
    // the cost. See the COST CONTRACT block in element/builder.hpp.
    //
    // Resolved EAGERLY whenever the width is knowable, and deferred only
    // when it genuinely is not.
    //
    // During normal streaming there is an ambient RenderContext (the app
    // paints inside one), so available_width() is the real terminal width
    // and the pad is decided here — no component, no per-frame rebuild.
    // That is the hot path and it must stay free.
    //
    // Outside a render pass (render_to_string(el, w), a host that builds
    // before it paints) there is no context to ask; available_width()
    // would answer with its 80-column default and a genuinely 1-2 column
    // surface would keep the pad and render nothing. Only THAT case takes
    // a component — and it is not the streaming path, so the per-frame
    // cost does not apply.
    const bool width_known = have_render_context();
    const int  ctx_w       = available_width();
    const int  known_w     = width_known ? ctx_w : *last_paint_width_cell_;

    if (width_known || known_w > 0) {
        const int pad = (known_w > 0 && known_w < 4) ? 0 : 2;
        // Feed the reveal's wrap-aware line_bounded clamp the CONTENT width
        // (what the tail actually wraps inside), not the outer width — the
        // outer value would place the last-visual-row boundary two columns
        // late.
        if (known_w > pad) *last_paint_width_cell_ = known_w - pad;
        cached_build_ = (
            detail::vstack().gap(1).padding(0, 0, 0, pad)
                .align_self(Align::Stretch)(std::move(outer_children))
        ).build();
    } else {
        auto padded_children = std::move(outer_children);
        cached_build_ = detail::component(
            [w_cell = last_paint_width_cell_,
             kids = std::move(padded_children)](int avail_w, int) -> Element {
                const int pad = avail_w >= 4 ? 2 : 0;
                // Capture the shared CELL, never `this`: this lambda runs at
                // LAYOUT time and the Element holding it can outlive the
                // widget (settled_element() hands the settled tree to the
                // host as a shared_ptr, and the renderer caches it across
                // frames). Writing `this->last_paint_width_` here was a
                // 4-byte write into freed memory on every layout of a
                // settled tree.
                *w_cell = avail_w - pad;
                return (detail::vstack().gap(1).padding(0, 0, 0, pad)
                            .align_self(Align::Stretch)(kids)).build();
            }).build();
    }
    cached_tail_size_     = tail.size();
    cached_prefix_gen_    = prefix_->generation;
    cached_fold_gen_      = fold_generation_;
    cached_has_tail_      = has_tail;
    cached_has_prefix_    = has_prefix;
    cached_block_count_   = has_prefix ? prefix_->blocks.size() : 0u;
    cached_window_lo_     = has_prefix ? window_lo(prefix_->blocks.size()) : 0u;
    cached_tail_in_fence_ = in_code_fence_;
    if (has_tail) {
        cached_tail_hash_ = fnv1a64(tail);
        cached_tail_len_  = tail.size();
    } else {
        cached_tail_hash_ = 0;
        cached_tail_len_  = 0;
    }
    cached_tail_version_ = source_version_;
    cached_tail_clip_    = visible_end;
    build_dirty_          = false;
    return render_live_overlay_();
}

} // namespace maya
