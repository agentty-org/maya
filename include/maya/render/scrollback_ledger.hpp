#pragma once
// maya::ScrollbackLedger — paint-measured accounting for an append-only
// sealed prefix, and the ONLY mint for scrollback commit counts.
//
// ─────────────────────────────────────────────────────────────────────────
// Part of the Witness Chain. This module discharges the Trim Accounting
// theorem:
//
//   For any front-trim of the sealed prefix, the row count committed to
//   maya's shadow (commit_inline_prefix) equals the number of physical
//   rows the dropped blocks occupied on the wire in the previous frame.
//
// The hazard this closes: the host used to compute that count ITSELF —
// a parallel measurement pipeline (an element re-measure through the
// layout engine at a host-reconstructed width, plus width-drift healing,
// plus N parallel bookkeeping vectors kept in lockstep by discipline).
// Every historical trim-corruption bug was drift between that pipeline
// and what maya actually painted:
//
//   - a 2-column wrap-width mis-reconstruction under-counted every
//     wrapped entry on narrow terminals (the phone-over-SSH duplication
//     ghost);
//   - a stale post-resize height stamp over-counted and over-committed,
//     re-scrolling the visible tail;
//   - an estimate that disagreed with the render by ONE row dropped an
//     on-screen entry (duplicate) or committed a still-visible row.
//
// The ledger closes the whole class BY CONSTRUCTION, with two moves:
//
//   1. maya measures, not the host. The renderer's paint pass records
//      each sealed block's real laid-out height into the ledger every
//      frame (see the ElementListRef arm in renderer.cpp). The recorded
//      height IS the height the compose serialized — same layout pass,
//      same width, same frame. There is no second measurement pipeline
//      to drift.
//
//   2. the commit count is a typed token, not an int. drop_front()
//      accrues the recorded heights of the dropped blocks as internal
//      debt; harvest() mints a ScrollbackDebt whose constructor is
//      private to the ledger. the commit_scrollback effect (CommitScrollback{ScrollbackDebt}) is
//      the only consumer. The host cannot fabricate, scale, or "adjust"
//      a count — it can only transport the token. (The legacy int
//      overload remains for non-ledger hosts; ledger hosts must use the
//      token path.)
//
// Provability gate
// ────────────────
// drop_front() drops ONLY blocks whose height has been recorded by a
// real ledger-tagged paint (rows >= 0), stopping the walk at the first
// unrecorded block. A block sealed this update cycle has not yet been
// painted THROUGH THE LEDGER — but its rows may already be physical on
// the wire (the live tail painted the identical content before the
// seal re-labelled it). Dropping it would shed real wire rows while
// accruing zero debt — an under-commit of exactly its height, the
// stranded-duplicate ghost. The gate makes that unrepresentable: a
// block becomes droppable only after one ledger paint has recorded the
// height the commit will use. The trim policy is merely LAZY for one
// frame (the next trim catches the now-recorded block); accounting is
// exact always.
//
// Estimates are policy, never accounting
// ──────────────────────────────────────
// seal() takes an estimated row count. It is used ONLY by row_total() /
// block_rows() — the host's trim POLICY (when to trim, how much recent
// context to keep) reads those and may be sloppy without consequence.
// The moment a block has been painted once, the recording replaces the
// estimate. Debt is minted exclusively from recorded heights.
//
// Width changes
// ─────────────
// Recorded heights are re-stamped on EVERY frame's paint at the live
// width, so a resize self-heals within one frame — there is no
// "re-measure the prefix to the new width" step for the host to forget.
// If a trim lands in the update cycle BETWEEN a width change and the
// next paint, the minted debt reflects the old width; that is safe:
// a width change demotes inline coherence to HardReset, and
// commit_inline_prefix is a no-op in any non-Synced state — the debt
// evaporates against a frame that is about to repaint from scratch
// anyway (there is no committed shadow left to mis-align).
//
// Cost
// ────
// record_paint is one int store per sealed block per frame (the blocks
// are all in the layout anyway). Everything else is O(dropped) at trim
// time. No allocation on the paint path.

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "../element/element.hpp"

namespace maya {

class ScrollbackLedger;

// ─────────────────────────────────────────────────────────────────────────
// ScrollbackDebt — the typed commit count
// ─────────────────────────────────────────────────────────────────────────
//
// Constructible only by ScrollbackLedger::harvest(). Carries the number
// of paint-recorded rows the host's tree shed from the front of the
// sealed prefix since the last harvest. Consumed by
// the commit_scrollback effect (CommitScrollback{ScrollbackDebt}).

class [[nodiscard(
    "ScrollbackDebt carries rows the shadow must be advanced by; "
    "dropping it desyncs prev_cells against the shortened tree")]]
ScrollbackDebt {
public:
    [[nodiscard]] int rows() const noexcept { return rows_; }
    [[nodiscard]] bool empty() const noexcept { return rows_ <= 0; }

private:
    constexpr explicit ScrollbackDebt(int rows) noexcept : rows_(rows) {}
    friend class ScrollbackLedger;

    int rows_ = 0;
};

// ─────────────────────────────────────────────────────────────────────────
// ScrollbackLedger
// ─────────────────────────────────────────────────────────────────────────

class ScrollbackLedger {
public:
    ScrollbackLedger() = default;

    // Non-copyable: the ledger is the single accounting authority for
    // one inline surface; a copied ledger would double-mint debt.
    ScrollbackLedger(const ScrollbackLedger&) = delete;
    ScrollbackLedger& operator=(const ScrollbackLedger&) = delete;
    ScrollbackLedger(ScrollbackLedger&&) noexcept = default;
    ScrollbackLedger& operator=(ScrollbackLedger&&) noexcept = default;

    // ── Host write API ───────────────────────────────────────────────

    /// Append a sealed block. `est_rows` seeds the POLICY row count
    /// until the first paint records the real height; it never feeds
    /// debt. `separator` marks inter-block chrome (gap rows, dividers)
    /// that must never lead the prefix.
    void seal(Element e, std::size_t est_rows, bool separator = false,
              std::uint64_t source = 0) {
        if (est_rows < 1) est_rows = 1;
        elements_.push_back(std::move(e));
        meta_.push_back(Meta{-1, static_cast<std::int64_t>(est_rows),
                             separator, source});
        est_total_ += est_rows;
    }

    /// Seal with a MAYA-MEASURED estimate: lays the block out through
    /// the real layout engine at the width the paint pass recorded for
    /// this ledger's fragment (record_paint_width), and uses the
    /// resulting height as the policy estimate. Falls back to
    /// `fallback_est_rows` when no paint has recorded a width yet
    /// (fresh surface before the first frame).
    ///
    /// TWO deliberate effects, NEITHER of them accounting:
    ///
    ///  1. CACHE WARMING (load-bearing for the freeze seam). A block
    ///     sealed at the freeze instant carries the SAME hash_id the
    ///     live tail stamped, and the renderer's hash-keyed measure
    ///     trusts a cached entry's height blindly. The live-phase
    ///     entry can hold a stale height; if the freeze frame lays the
    ///     sealed block out at that stale height, the tree transiently
    ///     shrinks vs prev_cells and the render gate fires an
    ///     avoidable (non-destructive) recovery. Running the block
    ///     through the real layout engine here refreshes the entry to
    ///     its natural height at the recorded width — so the freeze
    ///     frame is byte-stable. This subsumes the last measurement
    ///     hosts used to do themselves: the width is maya's own
    ///     paint-recorded constraint, not a host reconstruction of
    ///     "terminal columns minus the chrome paddings" (the -2/-4
    ///     drift class that caused the narrow-terminal duplication
    ///     ghost).
    ///
    ///  2. POLICY estimate: the measured height seeds block_rows()
    ///     until the first ledger paint records the real value.
    ///
    /// Accounting never touches this number — commits are minted
    /// exclusively from paint-recorded heights (harvest()), so a
    /// measurement drift here costs at most one avoidable soft
    /// repaint, never scrollback corruption.
    ///
    /// Returns the rows sealed under (measured, or the fallback).
    /// Defined in src/render/scrollback_ledger.cpp (needs the layout
    /// engine; keeping the header dependency-light).
    std::size_t seal_measured(Element e, std::size_t fallback_est_rows,
                              bool separator = false,
                              std::uint64_t source = 0);

    /// Replace block k in place (rehydrate-time collapse of an
    /// off-screen giant body). Resets the recorded height — the next
    /// paint re-records the replacement's real height.
    void replace(std::size_t k, Element e, std::size_t est_rows) {
        if (k >= elements_.size()) return;
        if (est_rows < 1) est_rows = 1;
        elements_[k] = std::move(e);
        auto& mt = meta_[k];
        if (mt.recorded >= 0) recorded_total_ -= mt.recorded;
        est_total_ -= mt.estimate;
        mt.recorded = -1;
        mt.estimate = static_cast<std::int64_t>(est_rows);
        est_total_ += mt.estimate;
    }

    /// Drop the first `n` blocks, quantized to a SAFE boundary:
    ///
    ///   1. EXTEND over exposed separators — the drop absorbs any
    ///      separator that would otherwise become the new front (a
    ///      leading gap renders as a hole at the canvas top, and —
    ///      load-bearing — a separator-terminated drop makes the last
    ///      dropped row BLANK, which is what keeps the host's top
    ///      padding row byte-aligned across the commit; a drop ending
    ///      on a content row strands that row against the padding and
    ///      trips the render gate's soft recovery).
    ///   2. CLAMP to the paint-recorded prefix (Provability gate
    ///      above): only blocks whose wire height maya has recorded
    ///      may shed rows.
    ///   3. BACK OFF if the clamp left a separator at the front —
    ///      un-drop back to the previous non-separator boundary rather
    ///      than expose it.
    ///
    /// Rows of the dropped blocks accrue as debt; harvest() mints the
    /// token. Returns the number of blocks actually dropped (may be
    /// less — or slightly more, via the separator extension — than n).
    std::size_t drop_front(std::size_t n) {
        if (n > elements_.size()) n = elements_.size();
        // 1. Extend across exposed separators.
        while (n < elements_.size() && meta_[n].separator) ++n;
        // 2. Provability clamp.
        std::size_t rec = 0;
        while (rec < elements_.size() && meta_[rec].recorded >= 0) ++rec;
        if (n > rec) n = rec;
        // 3. Boundary back-off: never leave a separator at the front.
        while (n > 0 && n < elements_.size() && meta_[n].separator) --n;
        if (n == 0) return 0;
        for (std::size_t k = 0; k < n; ++k) {
            debt_rows_      += meta_[k].recorded;
            recorded_total_ -= meta_[k].recorded;
            est_total_      -= meta_[k].estimate;
        }
        const auto d = static_cast<std::ptrdiff_t>(n);
        elements_.erase(elements_.begin(), elements_.begin() + d);
        meta_.erase(meta_.begin(), meta_.begin() + d);
        return n;
    }

    /// Strip leading separator blocks unconditionally. For FRESH
    /// (never-painted) prefixes only — rehydrate after clear(), where
    /// no committed wire rows exist and the swap goes through a hard
    /// reset anyway. Debt still accrues only from recorded heights
    /// (zero on a fresh prefix). Mid-session trims must NOT call this;
    /// drop_front's separator extension already owns that job under
    /// the provability gate.
    std::size_t drop_leading_separators() {
        std::size_t n = 0;
        while (n < meta_.size() && meta_[n].separator) ++n;
        if (n == 0) return 0;
        for (std::size_t k = 0; k < n; ++k) {
            if (meta_[k].recorded >= 0) {
                debt_rows_      += meta_[k].recorded;
                recorded_total_ -= meta_[k].recorded;
            }
            est_total_ -= meta_[k].estimate;
        }
        const auto d = static_cast<std::ptrdiff_t>(n);
        elements_.erase(elements_.begin(), elements_.begin() + d);
        meta_.erase(meta_.begin(), meta_.begin() + d);
        return n;
    }

    /// Mint the accumulated debt as a typed token and reset it. The
    /// caller forwards the token verbatim into
    /// the commit_scrollback effect (CommitScrollback{ScrollbackDebt}). Debt is monotonic and
    /// never lost: multiple drops in one update cycle accumulate into
    /// one harvest; an unharvested cycle's debt rides into the next.
    [[nodiscard]] ScrollbackDebt harvest() noexcept {
        const std::int64_t r = debt_rows_;
        debt_rows_ = 0;
        constexpr std::int64_t kIntMax = 2147483647;
        return ScrollbackDebt{static_cast<int>(r > kIntMax ? kIntMax : r)};
    }

    [[nodiscard]] bool has_debt() const noexcept { return debt_rows_ > 0; }

    /// Wholesale reset (thread swap, rebuild). Debt is discarded too:
    /// a wholesale content swap goes through reset_inline (HardReset),
    /// which rebuilds the shadow from scratch — stale debt against a
    /// wiped frame would be meaningless. paint_width_ deliberately
    /// SURVIVES: the surface's width didn't change with the content,
    /// and the very next seal_measured() calls (rehydrate) want to
    /// warm the measure cache at the real width, not fall back to
    /// unmeasured estimates for the whole rebuilt prefix.
    void clear() noexcept {
        elements_.clear();
        meta_.clear();
        recorded_total_ = 0;
        est_total_      = 0;
        debt_rows_      = 0;
    }

    // ── Host read API (policy) ───────────────────────────────────────

    [[nodiscard]] std::size_t size() const noexcept { return elements_.size(); }
    [[nodiscard]] bool empty() const noexcept { return elements_.empty(); }

    /// Recorded-or-estimate rows for block k. Policy input only.
    [[nodiscard]] std::size_t block_rows(std::size_t k) const noexcept {
        if (k >= meta_.size()) return 0;
        const auto& mt = meta_[k];
        return static_cast<std::size_t>(
            mt.recorded >= 0 ? mt.recorded : mt.estimate);
    }

    /// Sum of block_rows over the whole prefix. Policy input only.
    [[nodiscard]] std::size_t row_total() const noexcept {
        std::int64_t t = recorded_total_;
        for (std::size_t k = 0; k < meta_.size(); ++k)
            if (meta_[k].recorded < 0) t += meta_[k].estimate;
        return static_cast<std::size_t>(t < 0 ? 0 : t);
    }

    [[nodiscard]] bool separator_at(std::size_t k) const noexcept {
        return k < meta_.size() && meta_[k].separator;
    }

    /// True iff block k has a paint-recorded height (droppable).
    [[nodiscard]] bool recorded_at(std::size_t k) const noexcept {
        return k < meta_.size() && meta_[k].recorded >= 0;
    }

    /// The width (in columns) the paint pass laid this ledger's
    /// fragment out at on the most recent ledger-tagged frame; 0 until
    /// the first paint. seal_measured() lays new blocks out at this
    /// width so the seal-time warm matches the next frame's layout.
    [[nodiscard]] int paint_width() const noexcept { return paint_width_; }

    /// The host's tag for block k (seal's `source`), e.g. the index of the
    /// message it was built from. Drops keep it in step with the blocks,
    /// so a host can ask "where does my live prefix start?" without
    /// tracking every trim itself. 0 for an out-of-range k.
    [[nodiscard]] std::uint64_t source(std::size_t k) const noexcept {
        return k < meta_.size() ? meta_[k].source : 0;
    }

    /// The sealed elements, for rendering (Conversation::Config::ledger
    /// hands maya an ElementListRef over this vector, tagged with `this`
    /// so the paint pass can record heights back).
    [[nodiscard]] const std::vector<Element>& elements() const noexcept {
        return elements_;
    }

    // ── Renderer write-back (maya-internal) ──────────────────────────

    /// Called by the paint pass (renderer.cpp, ElementListRef arm) with
    /// block k's laid-out height this frame. THE ground truth: same
    /// layout pass, same width, same frame as the bytes on the wire.
    /// Re-stamped every frame, so width changes self-heal in one paint.
    ///
    /// const + mutable: recording is an OBSERVATION of the paint, not a
    /// host mutation — the render pipeline is const over the model
    /// (view(const Model&)), and the recorded heights are logically a
    /// cache of what maya itself just did. Debt/drop/seal (the actual
    /// state transitions) remain non-const.
    void record_paint(std::size_t k, int rows) const noexcept {
        if (k >= meta_.size() || rows < 0) return;
        auto& mt = meta_[k];
        if (mt.recorded >= 0) recorded_total_ -= mt.recorded;
        mt.recorded = rows;
        recorded_total_ += rows;
    }

    /// Called by the paint pass with the WIDTH constraint the ledger's
    /// fragment was laid out at this frame. seal_measured() lays new
    /// blocks out at this width — maya's own constraint, not a host
    /// reconstruction. Same const+mutable rationale as record_paint.
    void record_paint_width(int cols) const noexcept {
        if (cols > 0) paint_width_ = cols;
    }

private:
    struct Meta {
        std::int64_t recorded = -1;   // paint-recorded rows; -1 = never painted
        std::int64_t estimate = 1;    // host policy estimate (seal-time)
        bool         separator = false;
        std::uint64_t source  = 0;    // host tag (see source())
    };

    std::vector<Element>      elements_;
    mutable std::vector<Meta> meta_;
    mutable std::int64_t      recorded_total_ = 0;   // sum of recorded (>=0) rows
    std::int64_t              est_total_      = 0;   // sum of estimates (kept for cheap audits)
    std::int64_t              debt_rows_      = 0;   // accrued, un-harvested commit rows
    mutable int               paint_width_    = 0;   // fragment layout width, last ledger paint
};

} // namespace maya

// ─────────────────────────────────────────────────────────────────────────
// detail::ledger_ref — the render entry point for a sealed prefix
// ─────────────────────────────────────────────────────────────────────────
// Declared in element/builder.hpp (alongside list_ref); defined here
// where ScrollbackLedger is complete. Renders the ledger's sealed
// blocks as a zero-copy fragment AND tags the fragment so the paint
// pass records each block's laid-out height back into the ledger.

namespace maya::detail {

[[nodiscard]] inline Element ledger_ref(const ScrollbackLedger& ledger) {
    ElementListRef r;
    r.items_ref = &ledger.elements();
    r.ledger    = &ledger;
    return Element{r};
}

} // namespace maya::detail
