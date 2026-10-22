#pragma once
// maya/screen.hpp — the terminal as a device: maya without a runtime.
//
// A Screen owns the tty (through the type-state maya::Terminal<State>):
// raw mode, alt screen or inline, the input parser,
// the frame encoder and what it knows is on screen and in scrollback) and
// NOTHING that waits. Every method is one non-blocking step; there is no
// loop, no timer, no thread, no quit flag. A runtime (jaal, through
// <maya/host/run.hpp>, or a hand-written loop in a test) drives it:
//
//     auto term = maya::Screen::open({.mode = Mode::Inline});
//     watch(term->input_handle());                 // the runtime's reactor
//     ...on readable:   for (auto& ev : *term->read()) route(ev);
//     ...on a change:   Presented f = term->present(view(model));
//                       schedule(f.redraw_at);     // an animation wants more
//                       if (f.backpressured) watch_writable();
//     ...on writable:   term->flush();
//
// Why `Presented` is a return value: drawing has outputs a scheduler must act
// on (a widget asked to be drawn again; a scroll view learned its size and
// the frame used stale zeros; the tty refused bytes). They used to be
// thread-local globals every loop had to remember to read, and one of
// maya's three loops forgot two of them. A struct returned by the call that
// produces it can't be forgotten: using the frame means holding it.
//
// See docs/internals/design.md for the design.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "device.hpp"
#include "render/scrollback_ledger.hpp"

namespace maya {

/// What one draw needs from whoever schedules draws.
struct Presented {
    using clock = std::chrono::steady_clock;

    /// A widget asked to be drawn again (a spinner, a caret, a tween): call
    /// present() again at this time, even if nothing else changes.
    std::optional<clock::time_point> redraw_at;

    /// This frame measured a scroll view for the first time (or its extent
    /// moved), so it was drawn from stale sizes. Draw once more, now.
    bool redraw_now = false;

    /// The tty took only part of the frame. The rest is buffered: call
    /// flush() when the output is writable (or present() again).
    bool backpressured = false;

    /// Nothing visible changed, so nothing was drawn (the program's
    /// visual_hash matched and no widget asked for a frame).
    bool skipped = false;

    [[nodiscard]] bool wants_redraw() const noexcept { return redraw_now || redraw_at.has_value(); }
};

/// Device configuration: only what the TERMINAL needs. (Frame rate, key
/// maps and quitting are the runtime's business.)
class Screen {
public:
    using clock = Presented::clock;

    /// Take the terminal: raw mode, alt screen or inline region, capability
    /// probes. The destructor gives it back, on every path.
    [[nodiscard]] static Result<Screen> open(const Options& cfg) {
        auto rt = detail::Device::create(cfg);
        if (!rt) return std::unexpected(rt.error());
        Screen t{std::move(*rt)};
        // Published only now: this is the address that lives (see
        // Runtime::publish_theme_slot on why create() can't).
        return t;
    }

    Screen(Screen&&) noexcept            = default;
    Screen& operator=(Screen&&) noexcept = default;
    Screen(const Screen&)                = delete;
    Screen& operator=(const Screen&)     = delete;
    ~Screen() { if (rt_) (void)rt_->cleanup(); }

    // ── input ────────────────────────────────────────────────────────────

    /// The handle a runtime watches for readability.
    [[nodiscard]] platform::NativeHandle input_handle() const noexcept { return rt_->input_handle(); }

    /// Everything the terminal has sent, parsed. Never blocks. Frame
    /// acknowledgements are consumed here (see ready()), never returned.
    [[nodiscard]] Result<std::vector<Event>> read() {
        auto evs = rt_->read_events();
        if (const int acks = rt_->take_acks(); acks > 0) on_acks(acks);
        if (!evs) return evs;
        // Input the DEVICE owns, done here so every runtime gets it:
        //
        //  * inline mouse rows: the terminal reports screen rows, a program
        //    draws in frame rows. Translate, and drop clicks outside the
        //    frame (they're on the user's scrollback, not on us).
        //  * scroll views painted last frame take the wheel and scrollbar
        //    drags (ScrollState::auto_dispatch): only the device knows where
        //    each bar was painted. KEYS are not forwarded: the program owns
        //    them and routes a scroll key to its ScrollState in update(),
        //    or an arrow key would move a view the program didn't ask to.
        std::vector<Event> out;
        out.reserve(evs->size());
        for (auto& ev : *evs) {
            if (const int dy = rt_->inline_mouse_dy(); dy > 0) {
                if (auto* me = std::get_if<MouseEvent>(&ev)) {
                    const int fr = me->y.value - dy;
                    const int fh = rt_->inline_frame_rows();
                    if (fh > 0 && (fr < 1 || fr > fh)) continue;
                    me->y = Rows{fr};
                }
            }
            if (std::holds_alternative<MouseEvent>(ev))
                for (auto* s : detail::live_scroll_states())
                    if (s && s->auto_dispatch) (void)s->handle_event(ev);
            out.push_back(std::move(ev));
        }
        return out;
    }

    /// Call after SIGWINCH: re-reads the size and invalidates the frame.
    void on_resize() { rt_->handle_resize(); }

    /// A partial sequence is waiting for its next byte (a lone ESC could be
    /// the Escape key or the start of an arrow key). Ask again after
    /// kEscapeTimeout with resolve_pending_input(): if nothing came, it resolves.
    [[nodiscard]] bool has_pending_input() const noexcept { return rt_->has_pending_input(); }
    static constexpr std::chrono::milliseconds kEscapeTimeout{50};

    /// What a stalled partial sequence resolves to (a bare Escape key, or
    /// nothing for an abandoned CSI). Never blocks.
    [[nodiscard]] std::vector<Event> resolve_pending_input() { return rt_->flush_timeouts(); }
    [[nodiscard]] Size size() const noexcept { return rt_->size(); }

    // ── output ───────────────────────────────────────────────────────────

    /// Draw one frame: theme canvas, layout, paint, diff against what the
    /// terminal shows, write what the tty takes. Returns what the caller
    /// has to schedule.
    ///
    /// Takes the tree already built. Widgets ask for animation frames while
    /// they are BUILT, so a tree built before this call has already made
    /// its requests; to keep them, build through present(build) instead.
    Presented present(const Element& root) {
        Element framed = detail::apply_theme_canvas(root, rt_->theme(), rt_->size().width.value);
        (void)rt_->render(framed);
        send_probe();
        return collect();
    }

    /// Build the tree and draw it, in the one order that works: clear the
    /// animation requests, THEN build (widgets request as they're built),
    /// THEN draw and collect them. A caller that builds first and clears
    /// after throws away every request, and animations freeze on their
    /// first frame (the jaal smoke run's `--animates` check catches that).
    template <std::invocable Build>
    Presented present(Build&& build) {
        detail::animation_requested_ = false;
        detail::next_frame_delay_ms_ = -1;
        return present(Element{std::forward<Build>(build)()});
    }

    /// Like present(), but builds the tree only if the caller's
    /// `visual_hash` moved, a widget asked to be redrawn, or `force` (the
    /// caller knows something the hash can't: a resize, a suspended child
    /// that scribbled on the screen). `build` is called at most once, AFTER
    /// the animation request is cleared, so the widgets it constructs can
    /// re-request.
    template <std::invocable Build>
    Presented present_if(std::uint64_t visual_hash, Build&& build, bool force = false) {
        if (!force && last_hash_ && *last_hash_ == visual_hash && !pending_redraw_
            && !rt_->has_deferred_frame() && !rt_->has_pending_writes()) {
            Presented f; f.skipped = true; return f;
        }
        last_hash_ = visual_hash;
        detail::animation_requested_ = false;
        detail::next_frame_delay_ms_ = -1;
        Element built  = std::forward<Build>(build)();
        Element framed = detail::apply_theme_canvas(std::move(built), rt_->theme(),
                                                    rt_->size().width.value);
        (void)rt_->render(framed);
        send_probe();
        return collect();
    }

    /// Paint off-wire into the render cache (the one-shot warmup after a
    /// heavy model loads, so the first visible frame takes the blit path).
    void warm(const Element& root) { rt_->warmup_render(root); }

    /// Push bytes the tty refused earlier. True when nothing is left; while
    /// false, watch output_handle() for writability and call again.
    bool flush() { return rt_->drain_residue(); }

    /// The handle to watch for writability while backpressured().
    [[nodiscard]] platform::NativeHandle output_handle() const noexcept { return rt_->output_handle(); }

    /// Bytes are waiting for the tty (flush() when it's writable).
    [[nodiscard]] bool pending_output() const noexcept { return rt_->has_pending_writes(); }

    /// A frame is owed: bytes are waiting, or a frame was deferred.
    [[nodiscard]] bool backpressured() const noexcept {
        return rt_->has_pending_writes() || rt_->has_deferred_frame();
    }

    // ── device effects: one call each ────────────────────────────────────

    void set_title(std::string_view t)             { rt_->set_title(t); }
    void write_clipboard(std::string_view s)       { rt_->write_clipboard(s); }
    void query_clipboard(ClipboardDialect d = ClipboardDialect::automatic) { rt_->query_clipboard(d); }
    void emit_host_sequence(std::string_view seq)  { rt_->emit_host_sequence(seq); }
    void commit_scrollback(ScrollbackDebt debt)    { rt_->commit_inline_prefix(debt.rows()); }
    void commit_overflow()                         { rt_->commit_inline_overflow(); invalidate(); }
    void reset_inline()                            { rt_->reset_inline(); invalidate(); }
    void force_redraw()                            { rt_->force_redraw(); invalidate(); }
    void set_mouse(bool on)                        { rt_->apply_mouse(on); }

    /// Hand the real tty to an interactive child (sudo, $EDITOR): the TUI
    /// is torn down to a cooked terminal, `run` executes, the TUI comes
    /// back and the next present() repaints from scratch. Returns what
    /// `run` returned.
    template <std::invocable F>
    auto suspend(F&& run) -> std::invoke_result_t<F&> {
        using R = std::invoke_result_t<F&>;
        invalidate();
        if constexpr (std::is_void_v<R>) {
            rt_->suspend([&] { run(); });
        } else {
            std::optional<R> out;
            rt_->suspend([&] { out.emplace(run()); });
            return std::move(*out);
        }
    }

    [[nodiscard]] bool is_inline() const noexcept { return rt_->is_inline(); }
    [[nodiscard]] const Theme& theme() const noexcept { return rt_->theme(); }

    // ── flow control: never more than a frame ahead of the glass ─────────
    //
    // A terminal is not the other end of the pty. Over ssh the pty drains
    // into sshd instantly and the real bottleneck (the network, the remote
    // terminal's parser) is far downstream, behind buffers that can hold
    // megabytes. A program that draws as fast as the pty accepts fills them,
    // and every keypress (including `q`) then waits behind seconds of frames
    // the user will never see: measured, doom_fire exits 1 ms after `q` but
    // the screen keeps playing old fire for as long as the backlog lasts.
    //
    // So each frame ends with a Device Status Report query (`CSI 5 n`). The
    // terminal answers `CSI 0 n` once it has parsed everything before it,
    // which makes the answer an acknowledgement that the frame reached the
    // glass. At most `kWindow` frames are in flight; while the window is
    // full the caller keeps updating its model and simply doesn't draw. The
    // next frame drawn is the LATEST state, so a burst of input costs one
    // frame, not a queue of them, and the frame rate settles at exactly
    // what the link sustains. Locally the ack returns in well under a
    // millisecond, so nothing is throttled.
    //
    // A terminal that never answers (a dumb pipe, a very old emulator) must
    // not freeze the program: if no ack ever arrives the first frames are
    // released by a timeout and, after kProbeStrikes misses, flow control
    // switches off for the session.

    /// May a frame be drawn now? False while the window is full.
    [[nodiscard]] bool ready() noexcept {
        if (!flow_on_ || in_flight_ < kWindow) return true;
        if (clock::now() - oldest_sent_ > ack_timeout()) {    // an ack went missing
            if (++strikes_ >= kProbeStrikes && acks_seen_ == 0) flow_on_ = false;
            in_flight_ = 0;
            return true;
        }
        return false;
    }

    /// When a blocked frame could next be allowed without an ack arriving
    /// (the timeout), so a scheduler can bound its sleep.
    [[nodiscard]] std::optional<clock::time_point> ready_deadline() const noexcept {
        if (!flow_on_ || in_flight_ < kWindow) return std::nullopt;
        return oldest_sent_ + ack_timeout();
    }

    /// Smoothed time from sending a frame to its ack: how far away the
    /// glass is. Zero until the first ack.
    [[nodiscard]] clock::duration round_trip() const noexcept { return srtt_; }
    [[nodiscard]] int frames_in_flight() const noexcept { return in_flight_; }
    [[nodiscard]] bool flow_control() const noexcept { return flow_on_; }

    /// The underlying implementation, for the runtimes being migrated off
    /// it. Not part of the device API.
    [[nodiscard]] detail::Device& impl() noexcept { return *rt_; }

private:
    explicit Screen(detail::Device rt) : rt_(std::make_unique<detail::Device>(std::move(rt))) {
        rt_->publish_theme_slot();
    }

    // Anything that repaints from scratch must defeat the hash gate.
    void invalidate() noexcept { last_hash_.reset(); }

    // One frame in flight is the design: the terminal parses frame N while
    // frame N+1 is being computed, and nothing queues behind it. (A window
    // of 2 would let one stale frame sit in the buffers; the latency a user
    // feels after a keypress is the depth of that queue.)
    static constexpr int kWindow       = 1;
    static constexpr int kProbeStrikes = 3;

    // Generous: an ack is late only when it is far later than the link has
    // ever been. Before the first ack there is no estimate, so 250 ms.
    [[nodiscard]] clock::duration ack_timeout() const noexcept {
        using namespace std::chrono_literals;
        if (acks_seen_ == 0) return 250ms;
        return std::clamp<clock::duration>(srtt_ * 8, 100ms, 2s);
    }

    void send_probe() {
        if (!flow_on_) return;
        // Only when this render actually put bytes on the wire (or queued
        // them). A frame the hash gate or the coalescer skipped has nothing
        // to acknowledge.
        if (rt_->has_deferred_frame()) return;
        rt_->emit_host_sequence("\x1b[5n");
        if (in_flight_ == 0) oldest_sent_ = clock::now();
        ++in_flight_;
    }

    void on_acks(int n) {
        const auto now = clock::now();
        for (int i = 0; i < n && in_flight_ > 0; ++i) {
            // Acks arrive in order: this one is for the oldest frame out.
            // Sample its round trip into a smoothed estimate (RFC 6298).
            const auto rtt = now - oldest_sent_;
            srtt_ = acks_seen_ == 0 ? rtt : (srtt_ * 7 + rtt) / 8;
            ++acks_seen_;
            --in_flight_;
            oldest_sent_ = now;   // window of 1: nothing older is left
        }
        strikes_ = 0;
    }

    clock::time_point oldest_sent_{};
    clock::duration   srtt_{};
    int               in_flight_ = 0;
    int               acks_seen_ = 0;
    int               strikes_   = 0;
    bool              flow_on_   = true;

    // Gather the draw's outputs from where the view layer leaves them. This
    // is the ONE place those globals are read, so no runtime has to.
    Presented collect() {
        Presented f;
        if (detail::animation_requested_) {
            const auto delay = detail::next_frame_delay_ms_ > 0
                ? std::chrono::milliseconds(detail::next_frame_delay_ms_)
                : detail::kAnimationFrameInterval;
            f.redraw_at = clock::now() + delay;
        }
        if (detail::scroll_writeback_dirty) {
            detail::scroll_writeback_dirty = false;
            f.redraw_now = true;
            last_hash_.reset();
        }
        f.backpressured  = rt_->has_pending_writes() || rt_->has_deferred_frame();
        pending_redraw_  = f.wants_redraw();
        return f;
    }

    // Heap-held: the Runtime publishes its own address (the theme slot), so
    // it must not move once published. The Screen handle can.
    std::unique_ptr<detail::Device> rt_;
    std::optional<std::uint64_t>     last_hash_;
    bool                             pending_redraw_ = false;
};

}  // namespace maya
