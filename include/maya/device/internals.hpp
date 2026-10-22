#pragma once
// maya/device/internals.hpp — detail::Device, the terminal device's internals
// (terminal, input parser, writer, canvases, render state). Not a loop:
// maya::Screen (<maya/screen.hpp>) is its public face; the host in
// <maya/host/run.hpp> drives it one step at a time. Implemented in src/device/,
// one .cpp per concern.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "../core/concepts.hpp"
#include "wire_coalesce.hpp"
#include "../core/expected.hpp"
#include "../core/function.hpp"
#include "../core/motion.hpp"
#include "../core/overload.hpp"
#include "../core/render_context.hpp"
#include "../core/scroll_state.hpp"
#include "../core/types.hpp"
#include "../element/builder.hpp"
#include "../element/element.hpp"
#include "../platform/select.hpp"
#include "../render/canvas.hpp"
#include "../render/diff.hpp"
#include "../render/inline_frame.hpp"
#include "../render/pipeline.hpp"
#include "../render/renderer.hpp"
#include "../render/serialize.hpp"
#include "../style/theme.hpp"
#include "../terminal/input.hpp"
#include "../terminal/terminal.hpp"
#include "../terminal/writer.hpp"
#include "options.hpp"
#include "frame_request.hpp"
#include "keys.hpp"

namespace maya {

inline void app_set_theme(const Theme& t);   // defined below Device

// ============================================================================
// detail::Device — terminal resource owner (not public API)
// ============================================================================
// Owns the terminal, event source, writer, canvases, and render state.
// Exposes granular methods; maya::Screen is its public face and the jaal
// host (<maya/host/run.hpp>) drives it one non-blocking step at a time.

namespace detail {

// ============================================================================
// Render-coherence type-state (parameterized double-buffer)
// ============================================================================
// The terminal/canvas relationship has two genuine states, encoded as the
// alternatives of a std::variant.  This is the parameterized-canvas form:
// the *only* path that carries the canonical front buffer is the Synced
// alternative — Divergent literally has no Canvas member.  An incremental
// diff therefore cannot be issued from a Divergent state because the front
// buffer required by the diff routine is physically inaccessible.
//
//   FullscreenSynced { Canvas front; }
//       Terminal pixels are known to match `front`.  The next render can
//       diff (canvas_ vs front) and emit only the changed cells.  Producer:
//       a successful end-to-end frame write.
//
//   InlineSynced { InlineFrameState state; }
//       Inline analogue: `state.prev_cells/prev_rows` accurately model
//       the current scrollback tail.  compose_inline_frame() can do its
//       row-diff against this state.
//
//   Divergent {}
//       Terminal pixels are unknown — write failed mid-frame, resize
//       happened, or this is the first render.  The only legal next move
//       is a full serialize / clear-and-paint, which (on success) returns
//       to Synced.
//
// std::visit on either variant gives compile-time exhaustiveness: adding
// a new state forces every dispatcher to handle it or fail to build.
//
// INLINE COHERENCE NOTE
// ————————————————————————————————————————————
// The inline path no longer uses `coherent::InlineState`. It now uses
// `maya::inline_frame::InlineCoherence` (the Witness Chain), which has
// six type-tagged states (Empty / Fresh / Synced / Stale / HardReset /
// Sealed) instead of the previous two (InlineSynced / Divergent). The
// chain enforces every render precondition at compile time — see
// docs/internals/witness-chain.md. The legacy InlineSynced/Divergent
// types remain for FULLSCREEN coherence; inline mode uses InlineCoherence.
namespace coherent {

struct FullscreenSynced {
    Canvas front;   // canonical "what the terminal currently displays"
};

struct Divergent {};

using FullscreenState = std::variant<FullscreenSynced, Divergent>;

} // namespace coherent

class Device {
public:
    static auto create(Options cfg) -> Result<Device>;

    [[nodiscard]] Size size() const noexcept { return size_; }
    [[nodiscard]] const Theme& theme() const noexcept { return theme_; }

    /// Swap the palette mid-run.
    ///
    /// A theme picked from a settings screen has to take effect on the NEXT
    /// frame, not the next launch: a user judges a theme by looking at it,
    /// and a restart between choosing and seeing makes that impossible.
    /// The theme is only read during render, so replacing it between frames
    /// is safe — and every frame is a pure function of model + theme, so
    /// nothing caches a colour across the swap.
    void set_theme(const Theme& t) noexcept { theme_ = t; on_theme_changed(theme_); }

    /// Publish this runtime's theme slot to app_set_theme().
    ///
    /// Two sources can name the starting theme, and exactly one wins:
    ///
    ///   * app_set_theme() called before any Device exists (it parks the
    ///     theme in its own storage). That is the host saying "use THIS",
    ///     after reading its own config, so it must not be reverted.
    ///   * Options::theme, stored in theme_ by create().
    ///
    /// Adoption used to be `theme_ = theme::live()` unconditionally, which
    /// honoured the first and silently threw the second away: live() is
    /// native when nobody set anything, so run<App>({.theme = dracula})
    /// started in native.
    /// Whether anyone set a theme pre-runtime is exactly what live_epoch()
    /// counts, so that decides it, rather than comparing against native (a
    /// host may deliberately set native over a Options default).
    void publish_theme_slot() noexcept {
        if (theme::live_epoch() != 0) theme_ = theme::live();
        live_theme() = &theme_;
        on_theme_changed(theme_);
    }

    /// The theme the CURRENT run paints with, for hosts whose update path is
    /// pure and so cannot hold a reference to the runtime.
    ///
    /// An Elm-shaped host keeps update() free of the runtime on purpose, but
    /// a theme swap still has to reach the renderer. Rather than thread a
    /// mutable runtime through every reducer — which would hand every one of
    /// them the ability to repaint — the swap goes through this one named
    /// seam, so the places that can change the palette stay greppable.
    static Theme*& live_theme() noexcept {
        static Theme* t = nullptr;
        return t;
    }

    /// Subscribers notified whenever the live theme is replaced.
    ///
    /// Most of maya's colour is resolved per-frame straight from the Theme,
    /// so it needs no notification. A few subsystems instead keep a
    /// PROJECTED palette — markdown's ~35 `colors::` globals are the main
    /// one — because their render path is hot and reads a flat struct
    /// rather than walking a Theme. Those have to be re-derived when the
    /// theme changes, or a scheme repaints the chrome while the prose,
    /// code spans and tables stay on the old palette. That half-themed
    /// result is exactly what makes a theme picker feel broken.
    ///
    /// A hook rather than a direct call because those subsystems live
    /// ABOVE this header (markdown.hpp is opt-in; app.hpp must not drag it
    /// in). Each registers itself once; the runtime does not know who they
    /// are, only that they must be told.
    static std::vector<void (*)(const Theme&)>& theme_subscribers() {
        static std::vector<void (*)(const Theme&)> subs;
        return subs;
    }

    static void on_theme_changed(const Theme& t) {
        // Re-seat the style layer's view of the theme. Slot-kind colours
        // (every themed widget Config default) resolve against this at
        // SGR-emit time, which is below the app layer and cannot reach a
        // Device — so it gets its own pointer to the same object.
        //
        // This also bumps the theme EPOCH, which is what refreshes every
        // theme::projected<P> palette. Those used to need a subscriber each
        // (see below); now deriving is their read path, so a projection that
        // "forgets to register" is not expressible.
        theme::set_live(t);
        // Drop every cross-frame cached component.
        //
        // The renderer blits a cached subtree's PIXELS — colours already
        // resolved — keyed on a content hash. A theme swap changes none of
        // that content, so without this every cached turn, panel and row
        // keeps painting the old palette until its content happens to
        // change. That is the difference between "the new frame is themed"
        // and "the screen is themed", and it is why a theme change looked
        // like it only touched whatever was being redrawn anyway.
        //
        // Cost is one repaint of visible content on a swap, which is
        // exactly what the user asked for by picking a theme.
        render_detail::clear_component_cache();
        for (auto* fn : theme_subscribers()) fn(t);
    }
    [[nodiscard]] bool is_inline() const noexcept { return inline_terminal_.has_value(); }

    // Row offset for translating absolute SGR mouse coordinates into
    // frame-relative coordinates in inline mode. In inline mode the frame is
    // drawn partway down the terminal (at inline_top_row_, learned via a
    // cursor-position query at create()), but the terminal reports mouse
    // events in absolute rows. Callers subtract this from a mouse event's row
    // so hit-testing (scroll states, on_click, mouse_pos) lines up with the
    // rendered UI, exactly as it already does in fullscreen mode. Returns 0
    // when fullscreen, or when the anchor is unknown (query unanswered), in
    // which case behavior is unchanged.
    [[nodiscard]] int inline_mouse_dy() const noexcept {
        return (is_inline() && inline_top_row_ > 0) ? inline_top_row_ - 1 : 0;
    }
    // Content height (rows) of the last inline frame; used to drop mouse
    // events that fall outside the frame so the app doesn't react to clicks
    // in the surrounding scrollback. 0 = unknown (don't suppress).
    [[nodiscard]] int inline_frame_rows() const noexcept { return inline_frame_rows_; }

    // Enable/disable terminal mouse reporting at runtime. Drives the public
    // maya::set_mouse(bool); keeps mouse_enabled_ in sync so cleanup() emits
    // the matching disable on exit.
    void apply_mouse(bool on) noexcept {
        if (output_handle_ == platform::invalid_handle) return;
        static constexpr std::string_view kOn  =
            "\x1b[?1000h\x1b[?1002h\x1b[?1006h\x1b[?1007h";
        static constexpr std::string_view kOnHover =
            "\x1b[?1000h\x1b[?1003h\x1b[?1006h\x1b[?1007h";
        static constexpr std::string_view kOff =
            "\x1b[?1007l\x1b[?1006l\x1b[?1003l\x1b[?1002l\x1b[?1000l";
        (void)platform::io_write_all(output_handle_,
            on ? (hover_motion_ ? kOnHover : kOn) : kOff);
        mouse_enabled_ = on;
    }

    // Does the host terminal honor DEC mode 2026 (synchronized update)?
    // Detected once at Device::create() via env-var heuristic; immutable
    // afterwards.
    //
    // This is the HONEST answer (env_supports_synchronized_output()), NOT
    // "did we emit the wrapper". We emit the CSI ?2026h … l wrapper on
    // every terminal by default because unknown private modes are no-ops
    // where unsupported (see emit_sync_wrapper_) — but on Apple Terminal,
    // ish, and other emulators that genuinely lack mode 2026, the wrapper
    // does nothing and multi-row repaints still tear. Applications gate
    // their ANIMATION TICK RATE on this signal: when it's false, drop the
    // caret/spinner/gradient cadence (e.g. 60 Hz → ~10 Hz) so the
    // bottom-of-frame redraw that can't be made atomic happens far less
    // often, which is the only effective flicker mitigation left on those
    // terminals. When true, the wrapper makes frames atomic and apps can
    // tick freely.
    [[nodiscard]] bool supports_synchronized_output() const noexcept {
        return sync_supported_;
    }

    // Handle a resize signal: drain, update size, mark needs_clear.
    void handle_resize();

    // Read terminal input, parse into events.
    auto read_events() -> Result<std::vector<Event>>;

    // Return unconsumed events to the front of the input stream, to be
    // delivered before any fresh bytes on the next read_events().
    //
    // The run loop uses this to end an input batch early after a navigation
    // key, so the frame that follows shows the row the user actually steered
    // to instead of only the last one in the read. Ordering is FIFO and
    // ahead of new input, which is what makes the deferral invisible: the
    // events run in the order they were typed, just one frame later.
    void push_back_events(std::vector<Event> evs) {
        if (evs.empty()) return;
        if (startup_events_.empty()) {
            startup_events_ = std::move(evs);
            return;
        }
        // Anything already queued was typed EARLIER, so it stays in front.
        startup_events_.insert(startup_events_.end(),
                               std::make_move_iterator(evs.begin()),
                               std::make_move_iterator(evs.end()));
    }

    // Drop a duplicate clipboard-read PasteEvent (the tmux OSC 5522 + OSC 52
    // double-reply case). Mutates `events` in place.
    void dedup_clipboard_pastes(std::vector<Event>& events);

    // Flush parser timeout events (e.g., bare Escape after delay).
    auto flush_timeouts() -> std::vector<Event>;

    // Render an element tree to the terminal (src/device/render.cpp), then one
    // of the two paths:
    //   render_inline      compose_inline_frame: row-diff, scrollback-preserving
    //   render_fullscreen  RenderPipeline: clear → paint → diff/serialize
    auto render(const Element& root) -> Status;
private:
    auto render_inline(const Element& root, int w) -> Status;
    // Drain refused bytes / decide to coalesce: a value = no frame this tick.
    std::optional<Status> inline_wire_gate();
    // Size + bounded-clear the canvas, keeping the committed prefix.
    void inline_prepare_canvas(int w);
    auto render_fullscreen(const Element& root, int w) -> Status;
public:

    // True when this Device emits binary grid frames (RenderBackend::Grid)
    // instead of ANSI — a cooperating host is painting cells for us.
    [[nodiscard]] bool grid_mode() const noexcept { return grid_mode_; }

    // Pre-warm the cross-frame component cache by laying out + painting
    // `root` into a scratch canvas, WITHOUT touching the wire.
    //
    // Use case: after a heavy model swap (e.g. agentty resuming a tool-
    // heavy thread with hundreds of frozen rows), the very first
    // render() call pays the full layout + paint cost — typically tens
    // to hundreds of milliseconds for content the user has yet to see.
    // Calling warmup_render() with the same Element tree the next
    // render() will receive populates every ComponentElement with a
    // hash_id (CacheId) in the renderer's content cache; the subsequent
    // render() then takes the cell-blit fast path, dropping the visible
    // first frame to its steady-state cost.
    //
    // Constraints:
    //   — Same thread as render() (cache is thread_local).
    //   — Same StylePool (we use pool_ internally so the captured
    //     cells' style ids are valid when blit'd in render()).
    //   — Only meaningful for hash_id-keyed entries; pointer-keyed
    //     ComponentElements have ephemeral identity that won't
    //     survive the scratch → real render handoff.
    //   — Wire is NOT touched; the scratch canvas is discarded.
    //
    // Cost: roughly equal to render_tree() of `root` (layout + paint +
    // cache capture), but pays its own canvas allocation each call so
    // burn it sparingly — on the resume edge, not per-frame.
    void warmup_render(const Element& root);

    // Set terminal title via OSC 0.
    void set_title(std::string_view title);

    // Copy `text` to the system clipboard via OSC 52. Cross-platform
    // — the transport (writer_ → platform::io_write) abstracts
    // POSIX::write / Win32::WriteFile, and the escape sequence
    // itself is interpreted by the terminal emulator, not the OS.
    // Empty `text` clears the clipboard.
    //
    // Compatibility: kitty, alacritty, wezterm, foot, ghostty, rio,
    // xterm, iTerm2 (opt-in via Prefs), Windows Terminal 1.7+, tmux
    // with `set -g set-clipboard on`. Apple Terminal.app and
    // conhost.exe ignore the sequence silently — no fallback path
    // here, hosts that need guaranteed clipboard access can layer
    // xclip / wl-copy / pbcopy / clip.exe at a higher level.
    void write_clipboard(std::string_view text);

    // Emit an OSC 52 clipboard READ query. The terminal replies inline
    // (OSC 52 ; c ; <base64> ST), which InputParser decodes into a
    // PasteEvent. The portable, remote-tool-free clipboard read used by
    // the SSH image-paste path. No-op effect on terminals that don't
    // honour OSC 52 reads (no reply ever arrives — host falls back).
    void query_clipboard(ClipboardDialect dialect = ClipboardDialect::automatic);

    // Emit an arbitrary, already-formed control sequence to the host
    // terminal, out-of-band with the frame renderer (see the emit_host_sequence effect
    // for the cursor-neutrality contract). Rides the SAME writer path as
    // set_title / write_clipboard — write_or_buffer, so a congested tty won't
    // drop it and the frame diff never re-emits it. The caller owns
    // well-formedness; maya writes it verbatim between frames. Empty sequence
    // is a no-op.
    void emit_host_sequence(std::string_view sequence);

    // Suspend the TUI, hand the real terminal to `fn` (an interactive
    // child process that inherits stdin/stdout/stderr), then restore the
    // TUI and force a full repaint. Blocks for the child's whole run —
    // that's intentional: the user IS interacting with the child (typing
    // a sudo password, watching output). While suspended the terminal is
    // in cooked mode with the TUI escapes torn down, so the child sees a
    // clean, line-disciplined tty. On return the next render repaints
    // from scratch (force_redraw / reset_inline as appropriate) so the
    // frame the child scrolled over is cleanly redrawn.
    //
    // No-op (fn still runs) if neither terminal is engaged — fn just runs
    // against whatever the fds currently are.
    void suspend(const std::function<void()>& fn);

    // Row count of the last composed inline frame (0 in fullscreen mode
    // or before the first render). Callers can use this as a cheap proxy
    // for tree height when deciding to virtualize. Returns 0 in any
    // inline state that doesn't carry a row count (Empty, Fresh,
    // HardReset, Sealed) — by construction these states have no
    // committed-frame row count to report.
    // Monotonic count of scrollback-invariant recoveries (committed
    // off-viewport rows + soft-repaint) since program start. Zero on a
    // healthy session; a rising value in the field is the release-build
    // signal that the single-source-of-truth invariant (maya's prev_rows
    // shadow) was violated by an upstream frame and recovered. Exposed so
    // a host / test harness can assert it stayed 0. See the member decl.
    [[nodiscard]] unsigned long scrollback_recovery_count() const noexcept {
        return scrollback_recovery_count_;
    }

    [[nodiscard]] int inline_content_rows() const noexcept {
        if (auto* s = std::get_if<inline_frame::InlineFrame<inline_frame::Synced>>(
                &in_coherence_))
            return s->rows();
        if (auto* s = std::get_if<inline_frame::InlineFrame<inline_frame::Stale>>(
                &in_coherence_))
            return s->rows();
        return 0;
    }

    // Legacy no-op. The inline composer anti-bounce is now FULLY
    // AUTONOMOUS inside Device::render (see the transient-hold block):
    // maya detects a 1-frame content dip and bridges it itself, with no
    // host policy bit. An earlier design drove the hold from this setter
    // via a Cmd, but the Cmd reliably arrived AFTER the dip window had
    // passed (and after content overflowed the viewport), so it never
    // engaged. Kept as a no-op so the old SetHeightHold plumbing and any
    // host call site remain valid. No effect.
    void set_height_hold(bool /*on*/) noexcept {}
    [[nodiscard]] int inline_min_content() const noexcept {
        return render_ctx_.inline_min_content;
    }

    // Mark prev-frame rows as committed to scrollback.
    //
    // SAFETY: the `rows` argument is a MEASURED debt from the host's
    // ScrollbackLedger::harvest() (paint-recorded rows) OR a legacy
    // host guess. The actual rows committed are
    // `min(rows, max(0, prev_rows - term_h))` — only rows that have
    // PROVABLY overflowed the current shadow's viewport are ever removed
    // from prev_cells. A caller that over-claims is clamped to the safe
    // value rather than corrupting prev_cells alignment.
    //
    // OBSERVABILITY (the type-hardening seam): the clamp is the exact
    // second-accountant signature — a row count measured against one
    // frame's geometry applied to a shadow that has since advanced
    // (harvest at frame N, this commit at frame N+k after another
    // render). It was historically SILENT, masking host↔shadow drift as
    // a benign no-op. We now COUNT every biting clamp into the release-
    // safe scrollback_recovery_count_ (so a field regression surfaces as
    // a rising metric / fails the PTY oracle's `== 0` assertion) and, in
    // debug builds, abort on it as an invariant tripwire — the host must
    // not hand maya a stale debt. A well-behaved host (ledger harvest in
    // the same update cycle that renders) never trips this: its debt is
    // exactly the rows the current shadow overflowed.
    //
    // Only meaningful when inline coherence is Synced; any other state
    // (Empty, Fresh, Stale, HardReset, Sealed) has no committed-frame
    // row count to commit against and the call is a no-op.
    void commit_inline_prefix(int rows) noexcept {
        if (rows <= 0) return;
        // Grid backend: the ANSI witness machine is inert.  Record the commit
        // so the next render_grid_frame emits a Commit frame — the host moves
        // its top `rows` grid rows into scrollback.  Return before touching
        // the inline coherence variant (which is Empty in grid mode).
        if (grid_mode_) { grid_pending_commit_ += rows; return; }
        if (!is_inline()) return;
        auto* s = std::get_if<inline_frame::InlineFrame<inline_frame::Synced>>(
            &in_coherence_);
        if (!s) return;
        const int term_h = std::max(1, size_.height.raw());
        const int safe_max = std::max(0, s->rows() - term_h);
        const int safe_rows = std::min(rows, safe_max);
        if (rows > safe_max) {
            // Host over-claimed against the current shadow: a stale debt.
            // Count it (release-safe, monotonic) so the drift is never
            // masked; tripwire it in debug so a mis-plumbed host is caught
            // at development time rather than in the field.
            ++scrollback_recovery_count_;
#ifndef NDEBUG
            if (std::getenv("MAYA_GATE_ABORT")) {
                std::fprintf(stderr,
                    "[maya] commit_inline_prefix: host claimed %d rows but only "
                    "%d have provably overflowed the current shadow (prev_rows=%d, "
                    "term_h=%d). This is a STALE DEBT applied to a superseded "
                    "frame -- the host harvested against one frame and committed "
                    "against another. Unset MAYA_GATE_ABORT to observe-and-clamp "
                    "instead of aborting.\n",
                    rows, safe_max, s->rows(), term_h);
                std::abort();
            }
#endif
        }
        if (safe_rows <= 0) return;
        // Move the Synced out, commit, store the new Synced back.
        // ScrollbackMarker is consumed by `commit()`; the typed token
        // guarantees `safe_rows <= prev_rows` at issue time.
        in_coherence_ = std::move(*s).commit(s->scrollback_marker(safe_rows));
        // A commit is a STRUCTURAL event that stays Synced: the next
        // frame's tree is `safe_rows` shorter (the host dropped that
        // prefix), but the render loop's bounded-clear canvas
        // preservation gates only on "prior coherence == Synced" and
        // would preserve the PRE-commit canvas prefix. When the drop
        // exceeds term_h + margin, stale rows survive BELOW the new
        // (shorter) tree's bottom, inflate content_height(), and get
        // serialized as live content — stranding the composer/status
        // chrome in native scrollback with duplicated transcript rows
        // below it. Force the next render to full-clear the canvas.
        canvas_preserve_inhibit_ = true;
    }

    // Commit every prev-frame row that has provably already overflowed
    // the viewport — i.e. `max(0, prev_rows - term_h)` rows.
    void commit_inline_overflow() noexcept {
        if (!is_inline()) return;
        auto* s = std::get_if<inline_frame::InlineFrame<inline_frame::Synced>>(
            &in_coherence_);
        if (!s) return;
        const int term_h = std::max(1, size_.height.raw());
        const int overflow_rows = s->rows() - term_h;
        if (overflow_rows <= 0) return;
        in_coherence_ = std::move(*s).commit(s->scrollback_marker(overflow_rows));
        // Same structural-event rule as commit_inline_prefix above:
        // the shadow shifted while coherence stays Synced, so the next
        // render must not preserve the pre-commit canvas prefix.
        canvas_preserve_inhibit_ = true;
    }

    // Force the next render to be a full repaint.
    //
    // For inline mode: demote the current Synced/Fresh state to Stale,
    // which routes the next render through compose's case (B) soft
    // redraw — cursor walks up, paints in place, erases below, no
    // scrollback wipe. The composer stays at its current viewport
    // row; host content above is preserved.
    //
    // If the inline state is already Stale or HardReset, leave it.
    //
    // Fullscreen always goes Divergent (no soft-redraw equivalent for
    // the alternate-screen-buffer model).
    void force_redraw() noexcept {
        fs_coherence_ = coherent::Divergent{};
        if (auto* s = std::get_if<inline_frame::InlineFrame<inline_frame::Synced>>(
                &in_coherence_)) {
            in_coherence_ = std::move(*s).demote_to_stale();
        }
        // This demote is CONTENT-PRESERVING: force_redraw drops wire
        // trust (ghost glyphs, foreign writes) but the canvas — the
        // app's own truth — is untouched, and the recovery repaint
        // re-serializes only the viewport window. Mark the next render
        // eligible for the frozen-prefix canvas preserve so a tall
        // transcript doesn't pay a full O(content_rows) clear+repaint
        // for a viewport-scoped fix. Recovery demotes (verify poison /
        // gate recovery) do NOT set this — their content may have
        // shifted, which is exactly why they demoted.
        canvas_preserve_stale_ok_ = true;
        // Fresh/Stale/HardReset already produce the right behavior on
        // the next render. Empty/Sealed are non-render states; ignore.
    }

    // Force the next inline render to be a HARD reset:
    // `\x1b[2J\x1b[3J\x1b[H` (clear viewport + clear saved-lines +
    // home) followed by a clean repaint from canvas row 0.
    //
    // This is the ONLY host-callable path that can reach rows the
    // application already scrolled into native scrollback. It is
    // therefore the correct recovery for a WHOLESALE CONTENT SWAP
    // into shorter content (thread switch / new thread): the old
    // thread may have overflowed the viewport, committing dozens of
    // its rows to native scrollback. Neither force_redraw (viewport-
    // only, case-B) nor commit_scrollback_overflow (advances
    // prev_rows down to term_h but leaves the physical off-viewport
    // rows on the wire) can erase those rows — they strand a copy of
    // the old transcript above the new one. demote_to_hard_reset is
    // the only coherent fix.
    //
    // Cost / caveat: \x1b[3J wipes the terminal's saved-lines
    // (native scrollback), so any pre-agentty shell history above
    // the frame is lost. That is acceptable ONLY for an explicit,
    // destructive, user-initiated content swap — never for a routine
    // frame transition (that's what force_redraw / commit-overflow
    // are for). Do NOT wire this to a per-frame or per-turn path.
    //
    // Inline only. Fullscreen already owns the whole screen via the
    // alt buffer; route it through Divergent for parity (full
    // serialize from home next frame).
    void reset_inline() noexcept {
        fs_coherence_ = coherent::Divergent{};
        if (auto* s = std::get_if<inline_frame::InlineFrame<inline_frame::Synced>>(
                &in_coherence_)) {
            in_coherence_ = std::move(*s).demote_to_hard_reset();
        } else if (auto* st = std::get_if<inline_frame::InlineFrame<inline_frame::Stale>>(
                &in_coherence_)) {
            in_coherence_ = std::move(*st).escalate_to_hard_reset();
        }
        // Empty/Fresh/HardReset/Sealed: nothing to demote — Empty/Fresh
        // already repaint cleanly, HardReset is already armed.
    }

    // True iff the underlying writer is holding undelivered bytes from a
    // prior non-blocking write — i.e. the last render() either fully or
    // partially deferred its output because the tty pipe couldn't accept
    // it. The runtime loop must re-fire a render on the next iteration
    // (with a short poll timeout) so the deferred bytes drain rather than
    // sitting in residue indefinitely waiting for an unrelated event.
    //
    // Without this signal, render() returning ok() after a WouldBlock
    // looks identical to a fully-successful frame; the loop clears
    // needs_render and the deferred bytes never get flushed until some
    // foreground event (keystroke, stream delta, timer) happens to fire.
    // On a resize-triggered full repaint (Divergent → Synced) where the
    // emitted byte stream is large enough to saturate the writer, this
    // surfaces as the viewport scrolling partway through the new frame
    // and then sitting blank for several seconds until the next
    // unrelated event triggers a retry.
    [[nodiscard]] bool has_pending_writes() const noexcept {
        return writer_ != nullptr && writer_->has_residue();
    }

    // True iff the last render() COALESCED instead of composing — i.e. the
    // frame the caller asked for has not been painted and no bytes are
    // queued that would paint it.
    //
    // The caller must keep asking while this is true. `has_pending_writes()`
    // answers "bytes are queued"; this answers "a frame was never composed",
    // and only the pair covers every way a paint can still be owed. Without
    // it the coalesce gate silently drops one-shot visual changes (the theme
    // preview that rendered 28 KB and never reached the wire), because the
    // gate's own "the caller re-fires" assumption only holds for streams.
    [[nodiscard]] bool has_deferred_frame() const noexcept {
        // Two terms, and they answer different questions.
        //
        // coalesced_last_render_ is about the WIRE: a compose was skipped to
        // let a congested terminal catch up, so the model's current frame was
        // never built. Only the compose path knows that happened.
        //
        // owes_paint(in_coherence_) is about the SCREEN, and it is DERIVED
        // rather than enumerated. Anything other than Synced/Sealed means the
        // wire does not yet match the canvas — which is exactly what those
        // states already mean. This replaced a hand-maintained OR of
        // per-reason booleans; the reason it had to is that the list drifted
        // from the truth. A theme swap demotes Synced→Stale WITHOUT emitting
        // (demote_to_stale is a state change; the Stale arm paints on a later
        // frame), no flag said so, and a key repeat could defer that paint
        // indefinitely — the theme browser stayed one entry behind.
        //
        // Deriving means a future state cannot forget to join the list,
        // because there is no list. See owes_paint() in inline_frame.hpp.
        //
        // Gated on is_inline() because in_coherence_ is the INLINE witness
        // chain: fullscreen and grid never advance it, so it sits at its
        // default Empty for the life of the process and would report an
        // eternal debt — a busy loop rather than a missed frame. Those
        // backends carry their own coherence (fs_coherence_, grid_need_full_)
        // and re-state unconditionally when they need to.
        return coalesced_last_render_
            || (is_inline() && inline_frame::owes_paint(in_coherence_));
    }

    // True iff the input parser is holding a partial escape sequence —
    // e.g. a lone ESC byte that is either a bare Escape keypress or the
    // head of an arrow / Home / End / function-key CSI whose tail hasn't
    // arrived yet. The parser can only RESOLVE the ambiguity via
    // flush_timeout() once escape_timeout_ (50ms) elapses, and the main
    // host calls flush_timeouts() after each wait for input returns.
    //
    // Without surfacing this, an idle (fps=0) loop with no timers/spinner
    // sleeps the full 100ms idle poll while the parser sits on the ESC,
    // so a bare Escape (close picker) or a split arrow sequence (slow
    // pty / SSH / tmux delivering ESC and `[A` in separate reads) appears
    // to hang for up to 100ms before resolving. The loop consults this to
    // clamp its poll timeout to the escape deadline, mirroring the
    // has_pending_writes() → short-retry pattern above.
    [[nodiscard]] bool has_pending_input() const noexcept {
        return parser_.has_pending();
    }

    // Final cleanup (show cursor, reset, newline).
    auto cleanup() -> Status;

    /// Swallow terminal replies still in flight, so they don't land on the
    /// shell's prompt after we exit.
    ///
    /// Every inline frame ends with a DSR (`CSI 5 n`); the terminal answers
    /// `CSI 0 n`. If we stop reading before the answer arrives, the next
    /// reader of the tty gets it — and the user sees a literal `^[[0n` in
    /// their prompt. We asked, so we read.
    ///
    /// Bounded and best-effort: a few milliseconds, everything discarded,
    /// never blocking. A terminal with nothing to say costs one poll().
    void drain_pending_replies() noexcept;

    // Emit the inline frame's owed restore bytes (cursor back to the
    // resting row, ?25h / ?7h / DECSCUSR / OSC-112 as claimed) and seal
    // the coherence chain. Runs in cleanup() BEFORE the Terminal dtor's
    // teardown \r\n — the hardware-caret epilogue parks the physical
    // cursor at the caret cell (above the frame bottom), and a \r\n
    // issued from there would drop the shell prompt mid-frame,
    // clobbering the remaining rows in scrollback. Idempotent.
    void finalize_inline_frame() noexcept;

    // Move-only
    Device(Device&&) noexcept;
    Device& operator=(Device&&) noexcept;
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    ~Device();

private:
    Device() = default;

    // -- Terminal ownership ---------------------------------------------------
    // Exactly one of these is engaged for the lifetime of the runtime.
    // Both states own their cleanup in their destructor: ~Terminal<AltScreen>
    // leaves the alt screen + restores keyboard/mouse state, and
    // ~Terminal<Inline> reverses the per-feature opt-ins (KKP,
    // modifyOtherKeys, bracketed paste, cursor) before disabling raw mode.
    // An exception escaping maya::run() therefore restores the terminal as
    // cleanly as a graceful exit — the type system enforces it.
    std::optional<Terminal<AltScreen>>  alt_terminal_;
    std::optional<Terminal<InlineMode>> inline_terminal_;
    platform::NativeHandle output_handle_ = platform::invalid_handle;
    platform::NativeHandle input_handle_  = platform::invalid_handle;

    // -- Platform signal handling ---------------------------------------------

    // -- Rendering pipeline ---------------------------------------------------
    // canvas_ is the back buffer that every render paints into.  The
    // *front* canvas (fullscreen) and prev-frame state (inline) live
    // exclusively inside the corresponding `*Synced` alternative of the
    // coherence variants — they don't exist as bare members because
    // they're meaningless in the Divergent state.  This is the encoding
    // that makes "diff after a failed write" structurally inexpressible.
    std::unique_ptr<Writer>         writer_;
    StylePool                       pool_;
    Canvas                          canvas_;
    std::string                     out_;
    std::vector<layout::LayoutNode> layout_nodes_;

    // ── Grid render backend (RenderBackend::Grid) ─────────────────────────
    // A parallel output path for a cooperating host: emit the cell diff as a
    // binary grid frame instead of ANSI, WITHOUT involving the ANSI inline
    // witness machine.  We keep our own previous-frame cell snapshot and row-
    // diff canvas_ against it (the same 64-bit-packed compare the ANSI path
    // uses), so grid and ANSI never share mutable diff state.
    bool                            grid_mode_        = false;
    bool                            grid_need_full_   = true;   // full frame next
    // Set when StylePool::retheme() reports an actual theme swap; consumed by
    // the inline Synced arm to force a repaint. A theme change is invisible
    // to both wire diffs (they compare style IDS, which do not change) so it
    // has to be carried as an explicit signal rather than detected.
    bool                            retheme_repaint_  = false;
    // Sticky "a theme swap has been observed but not yet painted".
    //
    // Separate from retheme_repaint_ because the two have different
    // lifetimes: retheme_repaint_ is consumed by the inline frame arm within
    // one compose, while this survives across any number of frames that
    // return early (coalesce, backed-up wire) until one actually composes.
    // StylePool::retheme() cannot serve that role itself — it is an edge
    // detector that stores the new theme as it reports it, so the edge is
    // gone after the first call whether or not anything was painted.
    bool                            pending_retheme_  = false;
    int                             grid_prev_w_      = 0;
    int                             grid_prev_rows_   = 0;
    // Scrollback: rows the app has committed to history since the last grid
    // frame.  commit_inline_prefix() accumulates here in grid mode (the ANSI
    // witness machine is inert); render_grid_frame() emits a Commit frame for
    // them (host appends its top N rows to scrollback) and shifts its own
    // prev-cell snapshot up by N so the next diff lines up.
    int                             grid_pending_commit_ = 0;
    // True scrollback: total canvas rows PERMANENTLY committed to the host's
    // history.  The live viewport starts at this row and never re-emits below
    // it, so committed content can't duplicate.  Advances only when content
    // overflows term_h (render_grid_frame emits a Commit for the overflow).
    int                             grid_committed_rows_ = 0;
    int                             grid_prev_content_h_ = -1;   // last content_h
    std::vector<std::uint64_t>      grid_prev_cells_; // packed cells, row-major
    // Paint `root`, diff against grid_prev_cells_, emit a grid frame. Called
    // from render() when grid_mode_. Returns the same Status contract.
    auto render_grid_frame(const Element& root) -> Status;

    // Observe a theme swap and latch it until a frame actually composes.
    //
    // Must be called on EVERY render() entry, before any path that can
    // return early. StylePool::retheme() both reports the swap and consumes
    // it, so calling it late (past the coalesce gate) loses swaps whenever
    // two keypresses straddle one deferred frame.
    void note_theme_swap() {
        if (pool_.retheme()) pending_retheme_ = true;
    }
    // Initial state matters: in inline mode, defaulting to anything
    // that emits a hard-reset (\x1b[2J\x1b[3J\x1b[H) would wipe the
    // user's shell scrollback on startup. The Witness Chain's
    // `InlineFrame<Empty>` is the safe initial state — the runtime's
    // first render path seeds it to `Fresh`, which routes compose
    // through case (A): emit from the cursor's current position via
    // serialize(), growing downward without disturbing host content
    // above. Fullscreen has no equivalent concern because alt-screen
    // entry already cleared the buffer — Divergent's "home + serialize"
    // path is benign there.
    coherent::FullscreenState       fs_coherence_ = coherent::Divergent{};
    inline_frame::InlineCoherence   in_coherence_ = inline_frame::InlineFrame<inline_frame::Empty>{};

    // -- Configuration --------------------------------------------------------
    Theme         theme_              = theme::native;
    Size          size_{};
    RenderContext render_ctx_;
    uint32_t      resize_generation_  = 0;
    // Per-frame width-backstop debounce. The backstop (Device::render)
    // re-queries TIOCGWINSZ every frame to catch a missed SIGWINCH; but
    // some terminals (observed: kitty under certain DPI / decoration
    // states) momentarily report a width 1-2 cols off on alternating
    // queries, which without hysteresis triggers a resize storm — every
    // frame flips width, invalidates caches, and repaints (visible chrome
    // bounce). A genuine resize PERSISTS across frames, a query glitch does
    // not: only act on a width that's been observed on two consecutive
    // frames. `width_candidate_` is the last differing reading; it must
    // repeat to be accepted.
    int           width_candidate_    = 0;
    // Transient monotonic-height hold (composer anti-bounce). Maya
    // absorbs a 1-frame downward step in inline content height so the
    // composer doesn't bounce. `hold_peak_` is the running-max unpadded
    // content height while it fits the viewport; `hold_decay_` counts
    // consecutive NON-RISING frames the content has stayed below the
    // peak. After kHoldDecayFrames the shrink is treated as real and the
    // peak falls to it (pad → 0) so idle/post-settle never carries dead
    // space. Fully autonomous — no host policy bit, no Cmd race. See the
    // hold block in Device::render.
    //
    // kHoldDecayFrames must exceed the indicator→content handoff window:
    // the 1-row activity indicator hands off to a transient intermediate
    // height (first markdown slice not yet fully measured) that can sit
    // flat for a few frames BEFORE the real content lands and overflows.
    // Measured that gap at ~5 frames; 6 bridges it with margin. The only
    // cost of a larger value is a genuine idle shrink holding its blank
    // pad rows a few extra frames before collapsing — imperceptible.
    int           hold_peak_          = 0;
    int           hold_decay_         = 0;
    int           hold_last_unpadded_ = 0;

    // ── Adaptive wire coalescing (backpressure-driven frame batching) ──────
    //
    // On a REMOTE link (mosh / SSH / Tailscale) the wire, not the CPU, is
    // the bottleneck: a streaming turn firing ~60 RAF/sec pushes ~60 tiny
    // diff frames/sec, and each pays a fixed CUP+SGR navigation tax. The
    // wire_bytes_bench measured this as a 12.8x amplification (70 KB on the
    // wire for a 5.5 KB doc). Coalescing N appends into ONE cumulative diff
    // frame removes that per-frame overhead (identical content reaches the
    // wire) — measured 3-5x less wire.
    //
    // Device::render() enforces a MINIMUM interval between composes that is
    // 0 on a fast wire (zero behavior change) and grows only when the wire
    // shows backpressure. `coalesce_.congestion` is an EWMA in [0,1] of "did
    // last frame leave residue / defer": 0 = wire keeps up, 1 = saturated.
    // `coalesce_.last_compose_ms` timestamps the last frame actually
    // composed. A frame that arrives inside the congestion-scaled interval
    // is SKIPPED (coalesced): the model keeps advancing, so the next compose
    // is cumulative and strictly cheaper than the frames it replaced. A
    // deferred frame is never lost — needs_render stays set by the caller
    // and the residue-retry poll clamp re-fires it. MAYA_NO_COALESCE=1
    // disables entirely.
    // ── Adaptive wire coalescing state (see wire_coalesce.hpp) ───────────
    // The EWMA congestion estimate + last-compose timestamp that decide
    // whether to batch a streaming frame. Inert on a fast wire.
    detail::CoalesceState coalesce_{};
    std::chrono::steady_clock::time_point coalesce_epoch_{
        std::chrono::steady_clock::now()};
    // Set by render() when the coalesce gate SKIPPED a compose, cleared the
    // moment one actually happens.
    //
    // Why this is not an internal detail: a coalesced frame returns ok() and
    // emits nothing, which is byte-identical, from the caller's side, to a
    // frame that painted. The loop then clears needs_render and the frame is
    // GONE. That is fine for a streaming append (the next delta re-fires
    // within milliseconds and the diff is cumulative) and wrong for a
    // one-shot model change like a keystroke: nothing re-fires, so the paint
    // is not deferred, it is dropped.
    //
    // The residue-retry path cannot cover this case either, because the gate
    // returns BEFORE composing — there are no pending bytes for
    // has_pending_writes() to notice. So the runtime has to say so itself.
    bool coalesced_last_render_ = false;
    static constexpr int kHoldDecayFrames = 6;
    // Release-safe scrollback-invariant recovery counter. Bumped every
    // time the overflowed-frame gate (or the verify-poison arm) has to
    // COMMIT off-viewport rows + soft-repaint because prev_cells no
    // longer matches the wire — i.e. a frame that would have corrupted
    // native scrollback was caught and recovered. Zero on a healthy
    // session. Unlike the #ifndef NDEBUG abort tripwire in
    // Device::render (which fires on the specific committed-prefix
    // MISMATCH), this counts EVERY non-Synced recovery and survives into
    // release builds, so a field regression surfaces as a rising metric
    // in the profiler line / via scrollback_recovery_count() instead of
    // only under a debug build. Monotonic; never reset.
    unsigned long scrollback_recovery_count_ = 0;
    // One-shot inhibitor for the bounded-clear canvas preservation in
    // Device::render. Set by commit_inline_prefix / commit_inline_overflow
    // — the two structural events that shift the shadow while REMAINING
    // Synced, which the gate's coherence-index check cannot see. Consumed
    // (cleared) by the next render, which full-clears the canvas instead
    // of preserving a prefix that no longer matches the shortened tree.
    bool          canvas_preserve_inhibit_ = false;
    // One-shot: the pending Stale frame came from force_redraw (content
    // unchanged), so the frozen-prefix canvas preserve may fire despite
    // coherence not being Synced. Consumed by the next render pass.
    bool          canvas_preserve_stale_ok_ = false;
    // Whether to EMIT the DEC ?2026 wrapper around every frame. On by
    // default (only MAYA_NO_SYNC disables it) because unknown DEC private
    // modes are no-ops where unsupported — emitting costs ~12 bytes/frame
    // and never corrupts. The render paths read this to decide whether to
    // append sync_start/sync_end.
    //
    // The exception is TERM=dumb, and it is the DEFAULT that has to know:
    // Device::create() refines this flag, but the inline path a host like
    // agentty uses never runs create(), so a default of plain `true` meant
    // the wrapper was emitted no matter what TERM said. "Unknown modes are
    // no-ops" assumes a DEC private-mode parser; a dumb terminal has none,
    // so the bytes print as garbage. That is issue #37's "doesn't respect
    // TERM", and initialising from the shared predicate fixes every entry
    // point at once rather than the one that happened to be audited.
    bool          emit_sync_wrapper_  = !theme::terminal_is_dumb();

    // The HONEST env-heuristic answer to "does this terminal support mode
    // 2026". Cached at create() so we don't re-query env every frame; this
    // is what supports_synchronized_output() returns and what callers gate
    // their animation tick rate on. False on Apple Terminal / ish / plain
    // xterm / unconfigured tmux. See ansi::env_supports_synchronized_output().
    bool          sync_supported_     = false;

    // Mouse tracking was requested via Options::mouse. Emit the SGR
    // mouse-reporting enable sequence in create() and the matching disable
    // in cleanup(); cached here so cleanup() doesn't need the Options.
    bool          mouse_enabled_      = false;
    // Options::hover_motion — when set, the enable sequence also turns on
    // ANY-motion reporting (mode 1003) so bare hover (no button) produces
    // move events for hover highlights. Cached so re-enable paths match.
    bool          hover_motion_       = false;
    // Options::enhanced_keyboard — true once the kitty keyboard protocol
    // push was emitted, so cleanup()/dtor emit the matching pop on every exit
    // path (leaving it enabled would corrupt key input for the next program
    // sharing the terminal).
    bool          kitty_kbd_enabled_  = false;
    // True once the window title was saved with the xterm title stack
    // (CSI 22;0t) before we set ours, so exit pops it back (CSI 23;0t).
    bool          title_pushed_       = false;

public:
    /// The terminal's input handle. The host (<maya/host/run.hpp>) watches it with
    /// jaal's reactor and calls read_events() when it's readable. Borrowed:
    /// the Device keeps ownership; the handle is valid while it lives.
    [[nodiscard]] platform::NativeHandle input_handle() const noexcept { return input_handle_; }

    /// The tty's output handle: a runtime watches it for WRITABILITY
    /// while a frame is backed up, instead of polling on a timer.
    [[nodiscard]] platform::NativeHandle output_handle() const noexcept { return output_handle_; }

    /// Frame acknowledgements the parser saw since the last call (see
    /// InputParser::take_acks and maya::Screen's flow control).
    [[nodiscard]] int take_acks() noexcept { return parser_.take_acks(); }

    /// Push bytes the tty refused earlier (the tail of a frame too big for
    /// the pty buffer). True when nothing is left.
    bool drain_residue() {
        if (!writer_ || !writer_->has_residue()) return true;
        if (auto d = writer_->try_drain_residue(); !d) {
            if (d.error().kind != ErrorKind::WouldBlock) {
                writer_->discard_residue();          // hard error: repaint from scratch
                fs_coherence_ = coherent::Divergent{};
                return true;
            }
        }
        return !writer_->has_residue();
    }

private:
    // -- State ----------------------------------------------------------------
    InputParser parser_;

    // Dedup guard for clipboard-read paste replies. Inside tmux we send BOTH
    // OSC 5522 (kitty image) and OSC 52 (text) because kitty is undetectable
    // there; a kitty outer terminal answers both, which would otherwise
    // surface as two paste events. Drop a PasteEvent that repeats the
    // PREVIOUS one's bytes within a short window. Content identity is what
    // separates the duplicate answer from a continuation chunk of one large
    // paste — a bare time window drops the latter too, truncating the paste
    // to its first chunk. Zero timestamp = no paste seen yet.
    std::chrono::steady_clock::time_point last_paste_at_{};
    std::string                           last_paste_content_;

    // Inline-mode mouse anchor: 1-based terminal row of the frame's top,
    // learned via a cursor-position query (DSR) at create() when mouse is
    // enabled. 0 = fullscreen or unknown (query unanswered) => no offset.
    int inline_top_row_ = 0;
    // OSC 5522 (kitty multi-format clipboard) capability, probed ONCE at
    // create() via DECRQM `CSI ? 5522 $ p` alongside the DSR query.
    //
    // Three genuinely different answers, so it is an enum. It was an int
    // whose comment had to enumerate the legal values (+1/0/-1) — which is
    // the tell that the type was never written down: nothing stopped a
    // fourth value, and `== 1` at the use site silently meant "supported"
    // while everything else collapsed into the fallback. Feature DETECTION
    // beats enumeration: a future WezTerm/Ghostty that adopts the spec is
    // discovered by asking, not by a release adding its name to a list.
    enum class Osc5522 : std::uint8_t {
        Unknown,      // no answer (query swallowed — old terminal, or a
                      // multiplexer that doesn't forward DECRQM). Fall back
                      // to env sniffing.
        Unsupported,  // terminal answered "not recognized" — definitive.
        Supported,    // answered DECRPM with set/reset: the terminal
                      // implements the 5522 family, so its clipboard READ
                      // escape is available even when the MODE is off.
    };
    Osc5522 osc5522_support_ = Osc5522::Unknown;
    int inline_frame_rows_ = 0;
    // Events that arrived interleaved with the DSR reply during create()'s
    // cursor-position query; delivered ahead of fresh input on the next
    // read_events() so a keypress in that window isn't dropped.
    std::vector<Event> startup_events_;
};

} // namespace detail
} // namespace maya
