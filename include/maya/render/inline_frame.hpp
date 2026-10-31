#pragma once
// maya::render::inline_frame — Tagged type-state for the inline renderer
//
// ─────────────────────────────────────────────────────────────────────────
// Part of the Witness Chain (see docs/internals/witness-chain.md). This
// module is the apex of the chain: every legal state of the inline
// renderer is a distinct type; every legal transition is a function
// whose signature only accepts the predecessor tag; every illegal
// transition (compose-after-finalize, render-without-verify, etc.) is
// a compile error.
// ─────────────────────────────────────────────────────────────────────────
//
// The six tags
// ────────────
//   InlineFrame<Empty>     — freshly constructed, no frame ever rendered.
//                            Only legal next move: into Fresh via `seed()`.
//   InlineFrame<Fresh>     — about to render the very first frame.
//                            Compose's case (A) path fires (emit from
//                            cursor downward; host content preserved
//                            above).
//   InlineFrame<Synced>    — last frame committed; shadow matches wire.
//                            Render requires a ShadowWitness obtained
//                            from `verify()`.
//   InlineFrame<Stale>     — SOFT recovery: shadow may be wrong but
//                            the wire's content is probably OK
//                            (host called `force_redraw`, shadow hash
//                            mismatch detected). Render goes through
//                            compose's case (B): soft redraw in
//                            place, NO scrollback wipe.
//   InlineFrame<HardReset> — HARD recovery: wire content is unknown
//                            (write failed mid-frame, terminal
//                            resize, host called `request_hard_reset`).
//                            Render emits `\x1b[2J\x1b[3J\x1b[H` to
//                            wipe the viewport AND the saved-lines
//                            buffer, then runs a fresh first-frame
//                            paint via case (A). The scrollback wipe
//                            is the critical difference from Stale.
//   InlineFrame<Sealed>    — `finalize()` consumed the state.
//                            Compose is no longer possible;
//                            render-after-finalize is a compile error.
//
// The legal transitions
// ─────────────────────
//                            seed()
//   InlineFrame<Empty>     ────────────▶  InlineFrame<Fresh>
//   InlineFrame<Fresh>     ──render────▶  Synced | Stale | HardReset    (case A)
//   InlineFrame<Synced>    ──verify────▶  optional<ShadowWitness>
//   InlineFrame<Synced>    ──render────▶  Synced | Stale | HardReset    (with witness)
//   InlineFrame<Synced>    ──demote_to_stale()────▶ InlineFrame<Stale>
//   InlineFrame<Synced>    ──demote_to_hard_reset()─▶ InlineFrame<HardReset>
//   InlineFrame<Synced>    ──commit(marker)──▶ InlineFrame<Synced>      (scrollback advance)
//   InlineFrame<Stale>     ──render────▶  Synced | Stale | HardReset    (case B)
//   InlineFrame<HardReset> ──render────▶  Synced | Stale | HardReset    (wipe + case A)
//   InlineFrame<*>         ──finalize(out)──▶ InlineFrame<Sealed>
//
// Each transition is a member function that consumes the predecessor
// by value-move and returns the successor. The underlying
// InlineFrameState (carrying prev_cells, prev_rows, etc.) is moved
// through every transition; AlignedBuffer's pointer-swap move makes
// this cheap.
//
// Render entry shape
// ──────────────────
//
//   InlineCoherence c = ...;                     // variant over the 6 tags
//   c = std::visit(overload{
//       [&](InlineFrame<Synced>& s) -> InlineCoherence {
//           auto wit = s.verify();
//           if (!wit) return std::move(s).demote_to_stale();
//           auto outcome = std::move(s).render(canvas, rows, term_h, pool,
//                                               writer, *std::move(wit), sync);
//           return std::visit([](auto&& arm) -> InlineCoherence {
//               return std::move(arm);
//           }, std::move(outcome));
//       },
//       [&](InlineFrame<Fresh>& f) -> InlineCoherence { ... },
//       [&](InlineFrame<Stale>& st) -> InlineCoherence { ... },
//       [&](InlineFrame<HardReset>& hr) -> InlineCoherence { ... },
//       [&](InlineFrame<Empty>& e) -> InlineCoherence { return std::move(e).seed(); },
//       [&](InlineFrame<Sealed>&) -> InlineCoherence { /* unreachable */ ... },
//   }, std::move(c));
//
// Skipping verify is uncompilable: `Synced::render` takes
// `ShadowWitness&&` by value. Constructing one without going through
// verify() is impossible because ShadowWitness's constructor is
// friended only to `verify_shadow`.
//
// Holding two views of the state simultaneously is impossible:
// transitions consume by move; the source is moved-from and using it
// is detected by compilers under -Wmoved-from.
//
// Cost
// ────
// Zero. The tags are empty types; the wrappers carry the same
// InlineFrameState by value as today. Move semantics ensure no
// copying. The variant return type compiles to a tagged union with
// no allocations.

#include <cassert>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "frame_bytes.hpp"
#include "serialize.hpp"

namespace maya { class Writer; }

namespace maya::inline_frame {

// ─────────────────────────────────────────────────────────────────────────
// Forward declarations
// ─────────────────────────────────────────────────────────────────────────

template <class Tag> class InlineFrame;

struct Empty     {};
struct Fresh     {};
struct Synced    {};
struct Stale     {};
struct HardReset {};
struct Sealed    {};

// ─────────────────────────────────────────────────────────────────────────
// RenderOutcome — variant of post-render states
// ─────────────────────────────────────────────────────────────────────────
//
// Three possible arms:
//   - Synced: write succeeded; shadow matches the wire.
//   - Stale:  shadow drifted (verification failure detected mid-render,
//             or write_or_buffer returned a soft retryable error in a
//             corner case).
//   - HardReset: write failed hard (kernel I/O error after partial
//             delivery); the wire is in an unknown state.
//
// std::variant forces the caller to handle all three via std::visit.
using RenderOutcome = std::variant<InlineFrame<Synced>,
                                   InlineFrame<Stale>,
                                   InlineFrame<HardReset>>;

// ─────────────────────────────────────────────────────────────────────────
// detail::lift_commit_outcome — dispatch CommitOutcome → RenderOutcome
// ─────────────────────────────────────────────────────────────────────────
//
// Internal helper used by every render() implementation to turn the
// commit::* variant returned by FrameBytes::commit_to into the
// corresponding InlineFrame<Tag> arm of RenderOutcome. Lives at
// namespace scope (not in an anonymous namespace) so it can be
// friended by the three InlineFrame specializations whose private
// constructors it needs.
namespace detail {
    RenderOutcome lift_commit_outcome(CommitOutcome outcome) noexcept;
}

// ─────────────────────────────────────────────────────────────────────────
// InlineFrame<Empty>
// ─────────────────────────────────────────────────────────────────────────

template <>
class [[nodiscard("InlineFrame<Empty> carries the renderer's only handle to its state; dropping the value forfeits every subsequent render")]] InlineFrame<Empty> {
public:
    InlineFrame() noexcept = default;
    InlineFrame(const InlineFrame&)            = delete;
    InlineFrame& operator=(const InlineFrame&) = delete;
    InlineFrame(InlineFrame&&) noexcept            = default;
    InlineFrame& operator=(InlineFrame&&) noexcept = default;

    /// Transition into Fresh, ready for the first render.
    [[nodiscard]] InlineFrame<Fresh> seed() && noexcept;

    /// Transition directly into Sealed without ever rendering.
    [[nodiscard]] InlineFrame<Sealed> finalize(std::string& /*out*/) && noexcept;
};

// ─────────────────────────────────────────────────────────────────────────
// InlineFrame<Fresh>
// ─────────────────────────────────────────────────────────────────────────
//
// First-ever render. Case (A) path: emit from cursor's current
// position via serialize(). No diff because there is no prev_cells.

template <>
class [[nodiscard("InlineFrame<Fresh> carries the renderer's only handle to its state; dropping the value forfeits every subsequent render")]] InlineFrame<Fresh> {
public:
    InlineFrame(const InlineFrame&)            = delete;
    InlineFrame& operator=(const InlineFrame&) = delete;
    InlineFrame(InlineFrame&&) noexcept            = default;
    InlineFrame& operator=(InlineFrame&&) noexcept = default;

    /// Run the first-ever render.
    ///
    /// `reset_prefix` is normally empty. The HardReset path passes its
    /// destructive wipe (\x1b[2J\x1b[3J\x1b[H) here so the wipe is
    /// folded INSIDE the same synchronized-output frame as the repaint
    /// — the terminal swaps wipe+repaint atomically instead of showing
    /// a blank screen then a visible top-to-bottom paint on a slow link.
    [[nodiscard]] RenderOutcome render(
        const Canvas& canvas,
        ContentRows rows,
        TermRows term_h,
        const StylePool& pool,
        Writer& writer,
        bool synchronized_output = true,
        std::string_view reset_prefix = {}) &&;

    /// Finalize without rendering.
    [[nodiscard]] InlineFrame<Sealed> finalize(std::string& out) && noexcept;

private:
    InlineFrame() noexcept = default;
    explicit InlineFrame(InlineFrameState s) noexcept : state_(std::move(s)) {}
    friend class InlineFrame<Empty>;
    friend class InlineFrame<HardReset>;
    friend RenderOutcome detail::lift_commit_outcome(CommitOutcome) noexcept;

    InlineFrameState state_;
};

// ─────────────────────────────────────────────────────────────────────────
// InlineFrame<Synced>
// ─────────────────────────────────────────────────────────────────────────

template <>
class [[nodiscard("InlineFrame<Synced> carries the renderer's only handle to its state; dropping the value forfeits every subsequent render")]] InlineFrame<Synced> {
public:
    InlineFrame(const InlineFrame&)            = delete;
    InlineFrame& operator=(const InlineFrame&) = delete;
    InlineFrame(InlineFrame&&) noexcept            = default;
    InlineFrame& operator=(InlineFrame&&) noexcept = default;

    /// Recompute the shadow hash and check it against the recorded
    /// value. Returns std::nullopt iff the shadow has been mutated
    /// outside the renderer.
    [[nodiscard]] std::optional<ShadowWitness> verify() const noexcept {
        return verify_shadow(state_);
    }

    /// Render a new frame against the verified shadow. Consumes the
    /// witness AND a ScrollbackProof (proof the committed off-viewport
    /// prefix is byte-stable — obtained from check_scrollback, vacuous
    /// when the frame fits the viewport). Requiring the proof makes
    /// "render an overflowed frame without the scrollback gate" a
    /// COMPILE error. Returns Synced, Stale, or HardReset.
    [[nodiscard]] RenderOutcome render(
        const Canvas& canvas,
        ContentRows rows,
        TermRows term_h,
        const StylePool& pool,
        Writer& writer,
        ShadowWitness&& witness,
        ScrollbackProof&& proof,
        bool synchronized_output = true) &&;

    /// Explicit soft demotion. Used by force_redraw and by verify()
    /// failure paths. The next render goes through Stale (case B).
    [[nodiscard]] InlineFrame<Stale> demote_to_stale() && noexcept;

    /// Explicit hard demotion. Used by resize handlers. The next
    /// render emits `\x1b[2J\x1b[3J\x1b[H` and starts fresh.
    [[nodiscard]] InlineFrame<HardReset> demote_to_hard_reset() && noexcept;

    /// Commit `marker.rows()` rows of scrollback. Returns a new
    /// Synced whose underlying state has `prev_cells` shifted up
    /// and `prev_rows` reduced. The old state is consumed.
    [[nodiscard]] InlineFrame<Synced> commit(ScrollbackMarker marker) && noexcept {
        InlineFrameState s = std::move(state_).committed(marker);
        return InlineFrame<Synced>{std::move(s)};
    }

    [[nodiscard]] ScrollbackMarker scrollback_marker(int rows) const noexcept {
        return state_.scrollback_marker(rows);
    }

    /// Whether the first `rows` rows of `canvas` are byte-identical to
    /// this frame's prev_cells prefix. Lets the runtime distinguish a
    /// turn-finish freeze (prefix unchanged) from a scrollback-content
    /// shift (prefix differs) when deciding the overflow-shrink recovery.
    [[nodiscard]] bool scrollback_prefix_matches(
        const Canvas& canvas, int rows) const noexcept {
        return state_.scrollback_prefix_matches(canvas, rows);
    }
    void dump_prefix_mismatch(const Canvas& canvas, int rows) const noexcept {
        state_.dump_prefix_mismatch(canvas, rows);
    }

    /// Run the scrollback gate and, on success, mint the ScrollbackProof
    /// that render() requires. Returns nullopt iff the committed prefix
    /// shifted (caller must recover). This is the ONLY way to obtain the
    /// proof — forwarding to the free `check_scrollback` with the private
    /// state so the obligation binds to THIS frame's shadow.
    [[nodiscard]] std::optional<ScrollbackProof> check_scrollback(
        const Canvas& canvas, int term_h) const noexcept {
        return maya::check_scrollback(state_, canvas, term_h);
    }

    [[nodiscard]] int rows()  const noexcept { return state_.prev_rows(); }
    [[nodiscard]] int width() const noexcept { return state_.prev_width(); }
    [[nodiscard]] int wire_cursor_rows() const noexcept {
        return state_.wire_cursor_rows();
    }
    /// Identity stamp of the underlying state (see InlineFrameState::
    /// generation / ScrollbackMarker). A marker minted from this frame
    /// carries this value; committing it against a frame whose stamp has
    /// since advanced is rejected. Exposed for the provenance oracle.
    [[nodiscard]] std::uint64_t state_generation() const noexcept {
        return state_.generation();
    }

    [[nodiscard]] InlineFrame<Sealed> finalize(std::string& out) && noexcept;

private:
    InlineFrame() noexcept = default;
    explicit InlineFrame(InlineFrameState s) noexcept : state_(std::move(s)) {}
    friend class InlineFrame<Empty>;
    friend class InlineFrame<Fresh>;
    friend class InlineFrame<Stale>;
    friend class InlineFrame<HardReset>;
    friend RenderOutcome detail::lift_commit_outcome(CommitOutcome) noexcept;

    InlineFrameState state_;
};

// ─────────────────────────────────────────────────────────────────────────
// InlineFrame<Stale>
// ─────────────────────────────────────────────────────────────────────────
//
// Soft recovery. Render goes through compose's case (B): cursor walks
// to the new frame's top, paint in place, erase below. No scrollback
// wipe — host's pre-existing content above the viewport is preserved.

template <>
class [[nodiscard("InlineFrame<Stale> carries the renderer's only handle to its state; dropping the value forfeits every subsequent render")]] InlineFrame<Stale> {
public:
    InlineFrame(const InlineFrame&)            = delete;
    InlineFrame& operator=(const InlineFrame&) = delete;
    InlineFrame(InlineFrame&&) noexcept            = default;
    InlineFrame& operator=(InlineFrame&&) noexcept = default;

    [[nodiscard]] RenderOutcome render(
        const Canvas& canvas,
        ContentRows rows,
        TermRows term_h,
        const StylePool& pool,
        Writer& writer,
        bool synchronized_output = true) &&;

    [[nodiscard]] int rows()  const noexcept { return state_.prev_rows(); }
    [[nodiscard]] int width() const noexcept { return state_.prev_width(); }

    /// Escalate to HardReset (e.g. when a Stale render's write also
    /// fails — the soft path is no longer trustworthy).
    [[nodiscard]] InlineFrame<HardReset> escalate_to_hard_reset() && noexcept;

    [[nodiscard]] InlineFrame<Sealed> finalize(std::string& out) && noexcept;

private:
    InlineFrame() noexcept = default;
    explicit InlineFrame(InlineFrameState s) noexcept : state_(std::move(s)) {}
    friend class InlineFrame<Empty>;
    friend class InlineFrame<Fresh>;
    friend class InlineFrame<Synced>;
    friend class InlineFrame<HardReset>;
    friend FrameBytes;
    friend RenderOutcome detail::lift_commit_outcome(CommitOutcome) noexcept;

    InlineFrameState state_;
};

// ─────────────────────────────────────────────────────────────────────────
// InlineFrame<HardReset>
// ─────────────────────────────────────────────────────────────────────────
//
// Hard recovery. Render emits `\x1b[2J\x1b[3J\x1b[H` (wipe viewport,
// wipe saved-lines, home cursor), then runs a fresh case-(A) paint.
// The scrollback wipe is destructive to host content above the
// viewport — appropriate only when the wire's state is genuinely
// unknown (post-resize, post-write-failure).

template <>
class [[nodiscard("InlineFrame<HardReset> must be rendered to wipe-and-recover; dropping it leaves the wire in an unknown state")]] InlineFrame<HardReset> {
public:
    // No carried state — HardReset is a tag the runtime can synthesize
    // freely to express "the wire is in an unknown state and the next
    // render should wipe and start fresh." The default constructor is
    // therefore public; the transitions into HardReset from other
    // tags (Synced::demote_to_hard_reset, Stale::escalate_to_hard_reset,
    // commit_to's I/O-error branch) all produce a HardReset by value.
    InlineFrame() noexcept = default;

    InlineFrame(const InlineFrame&)            = delete;
    InlineFrame& operator=(const InlineFrame&) = delete;
    InlineFrame(InlineFrame&&) noexcept            = default;
    InlineFrame& operator=(InlineFrame&&) noexcept = default;

    [[nodiscard]] RenderOutcome render(
        const Canvas& canvas,
        ContentRows rows,
        TermRows term_h,
        const StylePool& pool,
        Writer& writer,
        bool synchronized_output = true,
        std::string_view history = {}) &&;

    [[nodiscard]] InlineFrame<Sealed> finalize(std::string& out) && noexcept;
};

// ─────────────────────────────────────────────────────────────────────────
// InlineFrame<Sealed>
// ─────────────────────────────────────────────────────────────────────────

template <>
class InlineFrame<Sealed> {
public:
    InlineFrame(InlineFrame&&) noexcept            = default;
    InlineFrame& operator=(InlineFrame&&) noexcept = default;
    InlineFrame(const InlineFrame&)            = delete;
    InlineFrame& operator=(const InlineFrame&) = delete;

    /// No-op: a Sealed frame has already had its terminal-restore
    /// bytes emitted. The method exists so `finalize_coherence`'s
    /// visit lambda compiles uniformly across all six tags.
    [[nodiscard]] InlineFrame<Sealed> finalize(std::string& /*out*/) && noexcept {
        return std::move(*this);
    }

private:
    InlineFrame() noexcept = default;
    friend class InlineFrame<Empty>;
    friend class InlineFrame<Fresh>;
    friend class InlineFrame<Synced>;
    friend class InlineFrame<Stale>;
    friend class InlineFrame<HardReset>;
};

// ─────────────────────────────────────────────────────────────────────────
// InlineCoherence — the variant the Runtime stores
// ─────────────────────────────────────────────────────────────────────────
//
// Every legal state the renderer can be in is one of these. Sealed is
// included for completeness but the Runtime should never see it in
// steady state — Sealed is the terminal state produced by `finalize()`
// at shutdown.

using InlineCoherence = std::variant<InlineFrame<Empty>,
                                     InlineFrame<Fresh>,
                                     InlineFrame<Synced>,
                                     InlineFrame<Stale>,
                                     InlineFrame<HardReset>,
                                     InlineFrame<Sealed>>;

// ─────────────────────────────────────────────────────────────────────────
// owes_paint — does this state still owe the wire a frame?
// ─────────────────────────────────────────────────────────────────────────
//
// DERIVED, never enumerated. The host used to answer this by OR-ing a
// hand-maintained list of booleans ("did we coalesce", "is a theme swap
// pending", "did a demote skip its paint"), which is a PARALLEL DESCRIPTION
// of a fact this variant already states — and the two drifted. A theme swap
// demotes Synced→Stale without emitting, the repaint lands on the NEXT
// frame, and nothing in that list said so, so a key repeat could defer the
// paint indefinitely and the screen stayed one keystroke behind.
//
// The states already mean this:
//
//   Synced     the wire matches the canvas — nothing owed
//   Empty      nothing painted yet
//   Fresh      seeded, not yet painted
//   Stale      marked for repaint; the Stale arm paints on a LATER frame
//   HardReset  wipe + repaint pending
//   Sealed     shut down; the loop is gone, so nothing to schedule
//
// Only Synced and Sealed rest. Asking the variant means a new state cannot
// forget to register itself, because there is no registry — and the visit
// below names every alternative EXPLICITLY, so adding one to
// InlineCoherence without deciding whether it owes a paint is a compile
// error rather than a silently-dropped frame. That is the whole point: the
// question is answered by the type, not by a list someone has to update.
namespace detail {
template <class> inline constexpr bool owes_paint_unhandled = false;
}

[[nodiscard]] inline bool owes_paint(const InlineCoherence& c) noexcept {
    return std::visit([](const auto& f) -> bool {
        using T = std::decay_t<decltype(f)>;
        if      constexpr (std::is_same_v<T, InlineFrame<Synced>>)    return false;
        else if constexpr (std::is_same_v<T, InlineFrame<Sealed>>)    return false;
        else if constexpr (std::is_same_v<T, InlineFrame<Empty>>)     return true;
        else if constexpr (std::is_same_v<T, InlineFrame<Fresh>>)     return true;
        else if constexpr (std::is_same_v<T, InlineFrame<Stale>>)     return true;
        else if constexpr (std::is_same_v<T, InlineFrame<HardReset>>) return true;
        else {
            static_assert(detail::owes_paint_unhandled<T>,
                "a new InlineCoherence state must say whether it owes the "
                "wire a paint: a state that is not Synced/Sealed and does "
                "not emit will strand its own repaint");
            return true;
        }
    }, c);
}

// ─────────────────────────────────────────────────────────────────────────
// finalize_coherence — emit terminal-restore bytes from any tag
// ─────────────────────────────────────────────────────────────────────────
//
// Convenience for callers that hold an InlineCoherence variant
// and want to consume it into Sealed at shutdown. Visits the
// current state, calls its `.finalize(out)` (which emits the
// DECAWM and cursor-visibility restore escapes the state knows
// it owes), and returns the resulting Sealed.
inline InlineFrame<Sealed> finalize_coherence(InlineCoherence&& c,
                                              std::string& out) noexcept {
    return std::visit(
        [&](auto&& arm) -> InlineFrame<Sealed> {
            return std::move(arm).finalize(out);
        }, std::move(c));
}

} // namespace maya::inline_frame
