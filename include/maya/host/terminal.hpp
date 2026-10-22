#pragma once
// maya/host/terminal.hpp — terminal_host: the one class that makes maya a jaal
// host. maya is to jaal what Ink is to React: jaal is the runtime (model,
// update, Cmd, Sub, threads, timers, shutdown), maya draws it (elements,
// layout, widgets, the terminal device). This is the class where they meet.
// docs/internals/design.md has the rules; a program includes <maya/host/run.hpp>.
//
// The host protocol (jaal/kernel/run.hpp), each step one Screen call:
//
//   attach(cx)       watch screen.input_handle() with jaal's reactor
//   on_ready(cx, r)  screen.read(); emit each event; jaal hands it to the
//                    program's on(model, event) and folds it on the spot
//   on_signal(resize) screen.on_resize(); emit a ResizeEvent
//   present(k)       screen.present(build view(model)) -> Presented; schedule
//                    Presented::redraw_at, honour redraw_now / backpressured
//   owes_frame()     an animation deadline arrived, a redraw is owed, or
//   wait_hint()      the tty is backed up: jaal wakes for those, and sleeps
//                    otherwise (an idle app costs nothing)
//   handle(effect)   terminal effects: title, clipboard, scrollback, suspend
//
// The host holds NO policy of its own: every decision it reports is either a
// Screen answer (Presented) or a program answer (Msg). Its whole state is a
// Screen&, a frame deadline, and the input it is holding back.

#include <algorithm>
#include <string>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <variant>

#include <jaal/jaal.hpp>

#include "../maya.hpp"            // an app needs the whole view layer: one include
#include "../element/element.hpp"
#include "../terminal/input.hpp"
#include "../device.hpp"
#include "../screen.hpp"

#include "interop.hpp"            // maya's value types, as jaal sees them
#include "effects.hpp"            // what only the terminal can do
#include "sources.hpp"            // what the terminal reports
#include "device.hpp"             // the device surface this host drives

namespace maya {

// The terminal's events (terminal_events), its effects (set_title,
// commit_scrollback, suspend, ...), the Program concept and keymap all come
// from the siblings included above. This file is the host and nothing else.

// ── the host ───────────────────────────────────────────────────────────────
//
// jaal's side of maya::Screen. The host owns no terminal logic: it watches
// the Screen's input handle, routes what read() parses, and turns jaal's
// "the model changed" into Screen::present(). Everything a draw needs from
// the scheduler comes back in Presented (an animation deadline, a redraw
// the frame asked for, a backed-up tty): the host reads no globals.
//
// It is templated on the DEVICE, not bound to Screen, because the host is
// where maya's only untestable logic lived: frame debt, the animation
// deadline, the ack window, the input held back across a navigation key.
// Against a concrete Screen those could only be exercised through a real
// pty. `Dev` is any type modelling Device (host/device.hpp), so a test can
// hand it a scripted fake and assert on the calls it makes — see
// tests/test_host.cpp. Production still instantiates it with Screen, so
// there is no indirection and no vtable: the default argument keeps
// `terminal_host<P>` spelled exactly as before.
template <Program P, Device Dev = Screen>
class terminal_host {
public:
    // Everything the terminal produces. jaal's run() adds jaal::sig to it
    // (kernel_event_t), so a program can also write on(const Model&, sig).
    using event_type = terminal_events;
    using clock      = Presented::clock;
    using device_type = Dev;

    /// `fps` > 0 redraws continuously at that rate (Options::fps), for a
    /// program whose view() reads the wall clock itself: a clock, a
    /// throughput graph, an FPS counter. 0 (the default) is event-driven:
    /// draw only when the model changes or a widget asks.
    explicit terminal_host(Dev& term, int fps = 0) noexcept
        : term_(term),
          frame_period_(fps > 0 ? std::chrono::nanoseconds(1'000'000'000LL / fps)
                                : std::chrono::nanoseconds::zero()) {}

    terminal_host(const terminal_host&)            = delete;
    terminal_host& operator=(const terminal_host&) = delete;

    // Register the terminal's input with jaal's reactor, and tell the
    // program its size before it ever draws.
    //
    // Templated on the CONTEXT, not fixed to host_context<terminal_host>.
    // jaal detects these hooks with `requires { host.attach(cx) }` where cx
    // is host_context<TheActualHost> — so a host that DERIVES from this one
    // (to add its own effects) gets a different context type, the
    // requires-test fails, and the kernel silently skips attach(). The
    // symptom is an app that opens and then accepts no input at all, with
    // nothing logged. Accepting any context keeps derived hosts working.
    template <class Cx>
    void attach(Cx& cx) {
        auto reg = cx.watch(term_.input_handle(), jaal::interest::read, kInput);
        if (!reg) { cx.stop(70); return; }        // no input: nothing to drive us
        input_reg_.emplace(std::move(*reg));
        cx_ = ctx_ref{&cx};
        const auto sz = term_.size();
        (void)cx.emit(ResizeEvent{sz.width, sz.height});
    }

    template <class Cx>
    void on_ready(Cx& cx, const jaal::readiness& r) {
        if (r.token == kOutput) {
            // The tty took bytes again: push the rest of the backed-up frame.
            // Only when ALL of it has gone is the next frame composed (a
            // frame diffed against a front that isn't on screen yet would be
            // wrong); then stop watching for writability.
            if (term_.flush()) watch_output(false);
            return;
        }
        if (r.token != kInput) return;
        if (r.hangup) { cx.stop(0); return; }     // the terminal went away
        // Leftovers from the last read first: a navigation key ended that
        // batch early so its frame could be drawn (see below).
        const bool resuming = !held_.empty();
        std::vector<Event> events = std::exchange(held_, {});
        if (auto fresh = term_.read(); !fresh) { cx.stop(70); return; }
        else events.insert(events.end(), std::make_move_iterator(fresh->begin()),
                           std::make_move_iterator(fresh->end()));
        // The read ended inside a sequence (a lone ESC so far): note when, so
        // wait_hint() can wake us to resolve it if no more bytes come.
        // Stamp only the START of a pending run: a later read (the frame ack,
        // which also arrives as input) must not push the deadline back.
        if (!term_.has_pending_input()) escape_since_.reset();
        else if (!escape_since_) escape_since_ = clock::now();
        // A navigation key's frame is still owed (input is held): this read
        // may be just the terminal's frame ack arriving. Queue what came in
        // behind the held keys, and let present() resume them after the
        // frame. Emitting here would fold the next key before its frame and
        // skip a row (measured: rows 1, 3, 5, ... of a burst).
        if (!events.empty() && resuming) { held_ = std::move(events); return; }
        emit_until_navigation(cx, events);
    }

    // Emit events one at a time; stop after a NAVIGATION key (arrows,
    // Home/End, PgUp/PgDn, Tab) and hold the rest for the next wakeup.
    //
    // A fast terminal delivers a whole key-repeat run in one read. Folding
    // it all before drawing is right for typing (the end state is all anyone
    // wants) and wrong for navigation: holding Down through a list, the rows
    // in between were computed and never shown, so the cursor visibly
    // jumped 2 -> 4 -> 6. The cursor IS the feedback for an arrow key, so
    // each one is drawn. (maya's old loop did this too; it's the same rule.)
    // A partial sequence has waited out the escape timeout.
    [[nodiscard]] bool escape_due() const noexcept {
        return escape_since_ && clock::now() - *escape_since_ >= Dev::kEscapeTimeout;
    }
    // How long until it will have.
    [[nodiscard]] std::optional<std::chrono::milliseconds> escape_wait() const noexcept {
        if (!escape_since_) return std::nullopt;
        const auto left = *escape_since_ + Dev::kEscapeTimeout - clock::now();
        return std::max(std::chrono::milliseconds(0), std::chrono::ceil<std::chrono::milliseconds>(left));
    }
    static std::optional<std::chrono::milliseconds> earliest(std::optional<std::chrono::milliseconds> a,
                                                              std::chrono::milliseconds b) noexcept {
        return a && *a < b ? *a : b;
    }

    template <class Cx>
    void emit_until_navigation(Cx& cx, std::vector<Event>& events) {
        // jaal folds an emitted event on the spot (route() runs update), so
        // "emit, then draw" needs the loop to turn between two navigation
        // keys: stop right after one and hold the rest.
        for (std::size_t i = 0; i < events.size(); ++i) {
            const bool nav = detail::is_navigation_key(events[i]);
            const std::size_t produced = cx.emit(events[i]);
            // Raw mode turns ^C into a key, so jaal never sees a SIGINT and
            // its "unhandled interrupt exits 130" rule can't fire. Apply it
            // here: a ^C the program didn't take ends it like a real SIGINT.
            if (produced == 0 && interrupt_exit_ && is_ctrl_c(events[i])) {
                cx.stop(kInterruptExit);
                return;
            }
            if (nav && i + 1 < events.size()) {
                held_.assign(std::make_move_iterator(events.begin() + static_cast<std::ptrdiff_t>(i) + 1),
                             std::make_move_iterator(events.end()));
                return;
            }
        }
    }

    // SIGWINCH is the one signal the host owns.
    template <class Cx>
    void on_signal(Cx& cx, jaal::sig s) {
        if (s != jaal::sig::resize) return;
        term_.on_resize();
        const auto sz = term_.size();
        (void)cx.emit(ResizeEvent{sz.width, sz.height});
        dirty_ = true;                            // re-lay-out even if the program has no on(ResizeEvent)
    }

    /// jaal::run_options::default_signal_exit, applied to ^C typed in raw
    /// mode. maya::run copies it from the run options; on by default.
    void set_interrupt_exit(bool on) noexcept { interrupt_exit_ = on; }

    static constexpr int kInterruptExit = 128 + 2;   // what SIGINT exits with

    [[nodiscard]] static bool is_ctrl_c(const Event& ev) noexcept {
        const auto* k = std::get_if<KeyEvent>(&ev);
        return k && ctrl_is(*k, 'c');
    }

    // The model changed, or a frame is owed: draw it.
    //
    // A program that declares visual_hash lets the Screen skip view()
    // when its pixels can't have changed; the Screen overrides that when
    // a widget asked for an animation frame or a redraw (a skipped build
    // can't re-request, so honouring the hash there would freeze an
    // animation until a keypress).
    template <class K>
    void present(K& k) {
        resolved_ = false;
        // A lone ESC (or a truncated sequence) that nothing followed: the
        // parser can only call it the Escape key once 50 ms have passed
        // with no next byte, and no byte means no input wakeup. wait_hint()
        // woke us for exactly this; resolve it and hand the key over.
        if (escape_due() && cx_) {
            escape_since_.reset();
            auto flushed = term_.resolve_pending_input();
            // Emitting folds the key now; returning with owes_frame() true
            // turns the loop once more, so its effect (a quit, a redraw)
            // is acted on in the next step rather than after the next input.
            if (!flushed.empty()) { emit_until_navigation(cx_, flushed); resolved_ = true; return; }
        }
        const bool drew = present_frame(k);
        // A navigation key ended the last input batch so ITS frame could be
        // drawn. Only once that frame has actually gone out (not held back
        // by the ack window or a backed-up tty) does the batch resume, and
        // then only up to the NEXT navigation key: emitting folds it at
        // once, so the loop has to turn (and draw) before the one after.
        // Resuming the whole rest here drew every other row of a burst.
        if (drew && !held_.empty() && cx_) {
            auto rest = std::exchange(held_, {});
            emit_until_navigation(cx_, rest);
        }
    }

    // Draw a frame if the Screen can take one now. True if one went out.
    template <class K>
    bool present_frame(K& k) {
        // A frame is still going out: don't compose another on top of it.
        // (The Screen diffs against what it believes is on the terminal;
        // composing now would diff against bytes the tty hasn't taken yet.)
        // The output watch drains it, and the frame is owed afterwards.
        if (term_.pending_output()) {
            if (term_.flush()) watch_output(false);
            else { watch_output(true); owed_ = true; return false; }
        }
        // The last frame hasn't reached the glass yet (no ack): hold this one.
        // The model keeps changing meanwhile; when the ack arrives (it comes
        // in as input, which wakes us) the frame drawn is the latest state,
        // so input never queues behind stale frames. On a local terminal the
        // ack is back before the next frame is due, and this never blocks.
        if (!term_.ready()) { owed_ = true; return false; }
        owed_ = false;

        const bool force = std::exchange(dirty_, false) || frame_due();
        auto build = [&] {
            // view() may call app_set_theme(); the Screen reads the theme
            // after build() returns, so a theme set here paints this frame.
            return P::view(k.model());
        };
        Presented f;
        // A view() that throws is a FAULT, not an abort: the model is
        // untouched by a failed draw, so jaal's fault policy decides (stop
        // by default, skip keeps the last good frame on screen), the fault
        // is reported with its site, and the teardown still gives the
        // terminal back. Escaping here instead would unwind through the
        // reactor — the terminal was restored by the RAII device, but the
        // program died with SIGABRT and no report.
        try {
            if constexpr (jaal::HasVisualHash<P>) {
                f = term_.present_if(P::visual_hash(k.model()), build, force);
            } else {
                f = term_.present([&] {
                    Element root = build();
                    if constexpr (jaal::HasNeedsWarmup<P>) warm(k, root);
                    return root;
                });
            }
        } catch (...) {
            k.view_faulted(std::current_exception());
            return false;                         // no frame this step
        }
        if (f.redraw_now) dirty_ = true;          // drawn from stale scroll sizes
        // The tty took only part of it: watch for writability and push the
        // rest from on_ready, instead of re-rendering on a timer (which is
        // what throttled a 2.5 MB/s animation to 7 fps).
        if (term_.pending_output()) watch_output(true);
        schedule(f);
        return !f.skipped;
    }

    // One deadline for the next frame: the EARLIER of a widget's animation
    // request and the fixed-rate tick. owes_frame() and wait_hint() read it.
    void schedule(const Presented& f) {
        const auto now = clock::now();
        std::optional<clock::time_point> next = f.redraw_at;
        if (frame_period_ > std::chrono::nanoseconds::zero()) {
            // Keep PHASE: advance by whole periods, so a slow frame doesn't
            // push every later one back; resync after a stall instead of
            // firing a burst of catch-up frames (the rule jaal's `every`
            // follows too).
            if (next_tick_ == clock::time_point{})
                next_tick_ = now + frame_period_;
            else if (next_tick_ <= now) {
                next_tick_ += frame_period_;
                if (next_tick_ <= now) next_tick_ = now + frame_period_;
            }
            if (!next || next_tick_ < *next) next = next_tick_;
        }
        next_frame_at_ = next;                    // nullopt: settled, back to idle
    }

    // jaal asks after every step whether we owe a frame even though the
    // model didn't change: a redraw the last frame asked for, an animation
    // deadline that has arrived, a frame held back by pending output that
    // has now drained, or a coalesced frame the Screen still owes.
    [[nodiscard]] bool owes_frame() const noexcept {
        if (escape_due()) return true;              // a lone ESC is ready to resolve
        if (resolved_) return true;                 // ...and was: turn once more to act on it
        if (!held_.empty()) return true;            // held input resumes after a frame
        if (term_.pending_output()) return false;   // the output watch will wake us
        if (awaiting_ack()) return false;           // the ack (input) will wake us
        return dirty_ || owed_ || term_.backpressured() || frame_due();
    }

    // ...and how long it may sleep before asking again. While output is
    // pending, as long as it likes: writability wakes it, not a timer. While
    // a frame is unacknowledged, until the ack (it arrives as input) or the
    // ack timeout, whichever is first.
    [[nodiscard]] std::optional<std::chrono::milliseconds> wait_hint() const noexcept {
        if (!held_.empty() || resolved_) return std::chrono::milliseconds(0);
        std::optional<std::chrono::milliseconds> esc = escape_wait();
        if (term_.pending_output()) return std::nullopt;
        if (awaiting_ack()) {
            const auto left = *term_.ready_deadline() - clock::now();
            return earliest(esc, std::max(std::chrono::milliseconds(0),
                                          std::chrono::ceil<std::chrono::milliseconds>(left)));
        }
        if (dirty_ || owed_) return std::chrono::milliseconds(0);
        if (term_.backpressured()) return earliest(esc, std::chrono::milliseconds(4));   // a coalesced frame
        if (next_frame_at_) {
            const auto left = *next_frame_at_ - clock::now();
            if (left <= clock::duration::zero()) return std::chrono::milliseconds(0);
            return earliest(esc, std::chrono::ceil<std::chrono::milliseconds>(left));
        }
        return esc;
    }

    // Screen effects: each one is a single Screen call.
    void handle(CommitScrollback c) { term_.commit_scrollback(c.debt); }
    void handle(SetTitle t)         { term_.set_title(t.title); }
    void handle(WriteClipboard w)   { term_.write_clipboard(w.text); }
    void handle(QueryClipboard q)   { term_.query_clipboard(q.dialect); }
    void handle(EmitHostSequence e) { term_.emit_host_sequence(e.sequence); }
    void handle(CommitOverflow)     { term_.commit_overflow(); dirty_ = true; }
    void handle(ForceRedraw)        { term_.force_redraw();    dirty_ = true; }
    void handle(ResetInline)        { term_.reset_inline();    dirty_ = true; }
    void handle(SetMouse s)         { term_.set_mouse(s.on); }

    // Hand the real tty to an interactive child and answer with how it went
    // (jaal D39: the answer is folded in this step).
    template <class M>
    std::optional<M> handle(Suspend<M> s) {
        if (!s.run) return std::nullopt;
        M out = term_.suspend([&] { return s.run(); });
        dirty_ = true;                            // repaint over what the child left
        return out;
    }

    void release() { output_reg_.reset(); input_reg_.reset(); cx_ = ctx_ref{}; }

private:
    static constexpr std::uint64_t kInput = 1;

    [[nodiscard]] bool frame_due() const noexcept {
        return next_frame_at_ && clock::now() >= *next_frame_at_;
    }

    // A frame is owed but the last one hasn't been acknowledged (and its ack
    // timeout hasn't passed): the host waits for the terminal.
    [[nodiscard]] bool awaiting_ack() const noexcept {
        const auto d = term_.ready_deadline();
        return (owed_ || dirty_ || frame_due()) && d && clock::now() < *d;
    }

    // Watch the output for writability only while bytes are waiting: an
    // idle tty is ALWAYS writable, so a permanent watch would wake the loop
    // continuously (the rule jaal's hosts guide spells out for sockets).
    // The output is its own fd (stdout; the input is stdin), so it gets its
    // own registration and token, and the registration is dropped the
    // moment the frame is out.
    void watch_output(bool on) {
        if (on == output_reg_.has_value() || !cx_) return;
        if (!on) { output_reg_.reset(); return; }
        if (auto reg = cx_.watch(term_.output_handle(), jaal::interest::write, kOutput))
            output_reg_.emplace(std::move(*reg));
    }

    // One-shot cache warmup on the RISING edge of needs_warmup (a heavy
    // thread just rehydrated), so the visible frame takes the blit path.
    template <class K>
    void warm(K& k, const Element& root) {
        const bool want = P::needs_warmup(k.model());
        if (want && !last_warmup_) term_.warm(root);
        last_warmup_ = want;
    }

    // The attached context, type-erased.
    //
    // attach() is templated (see there), so the context type varies with the
    // host that ends up running, but this member has to name ONE type. The
    // host only asks it to watch a handle, emit an event or stop.
    struct ctx_ref {
        void* p = nullptr;
        jaal::result<jaal::platform::native_reactor::registration>
            (*watch_fn)(void*, jaal::platform::native_reactor::handle,
                        jaal::interest, std::uint64_t) = nullptr;
        std::size_t (*emit_fn)(void*, const Event&) = nullptr;
        void (*stop_fn)(void*, int) = nullptr;

        ctx_ref() = default;
        template <class Cx>
        explicit ctx_ref(Cx* cx)
            : p(cx),
              watch_fn([](void* self,
                          jaal::platform::native_reactor::handle h,
                          jaal::interest what, std::uint64_t tok) {
                  return static_cast<Cx*>(self)->watch(h, what, tok);
              }),
              emit_fn([](void* self, const Event& ev) -> std::size_t {
                  return static_cast<Cx*>(self)->emit(ev);
              }),
              stop_fn([](void* self, int code) { static_cast<Cx*>(self)->stop(code); }) {}

        explicit operator bool() const noexcept { return p != nullptr; }
        [[nodiscard]] auto watch(jaal::platform::native_reactor::handle h,
                                 jaal::interest what,
                                 std::uint64_t tok) const {
            return watch_fn(p, h, what, tok);
        }
        std::size_t emit(const Event& ev) const { return emit_fn(p, ev); }
        void stop(int code) const { stop_fn(p, code); }
    };

    Dev&                                                    term_;
    ctx_ref                                                     cx_;
    std::optional<jaal::platform::native_reactor::registration> input_reg_;
    std::optional<jaal::platform::native_reactor::registration> output_reg_;   // only while output is pending
    static constexpr std::uint64_t                            kOutput = 2;
    bool                                                      dirty_ = true;
    std::vector<Event>                                        held_;   // input after a navigation key, awaiting its frame
    std::optional<clock::time_point>                          escape_since_;   // input ended mid-sequence at
    bool                                                      resolved_ = false;   // a timed-out sequence was just emitted
    bool                                                      owed_  = false;   // a frame held back by pending output
    bool                                                      last_warmup_ = false;
    std::optional<clock::time_point>                          next_frame_at_;
    std::chrono::nanoseconds                                  frame_period_;   // 0 = event-driven
    bool                                                      interrupt_exit_ = true;   // unhandled ^C exits 130
    clock::time_point                                         next_tick_{};    // next fps frame
};

}  // namespace maya
