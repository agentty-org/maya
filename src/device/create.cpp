// src/device/create.cpp — take the terminal: raw mode, screen, probes.
#include "internal.hpp"

#include <cctype>
#include <optional>

#include "maya/render/canvas.hpp"   // terminal_color_level
#include "maya/style/theme.hpp"     // set_terminal_colors

namespace maya::detail {

auto Device::create(Options cfg) -> Result<Device> {
    // Move the terminal through the type-state chain: Cooked → Raw → AltScreen
    MAYA_TRY_DECL(auto cooked, Terminal<Cooked>::create());
    MAYA_TRY_DECL(auto raw, std::move(cooked).enable_raw_mode());

    auto input_h  = raw.input_handle();
    auto output_h = raw.output_handle();

    // No resize handler here. SIGWINCH belongs to the runtime: jaal watches
    // it and delivers `jaal::sig::resize`, which terminal_host::on_signal
    // turns into on_resize() + a ResizeEvent. maya used to install a second
    // handler with a self-pipe, and nothing ever read that pipe — only
    // drain()ed it — so the device's copy was pure overhead AND a hazard:
    // whoever installs last wins, and jaal had to add a special case for
    // maya's handler to stop resizes being dropped (see the comment on
    // honour_inherited_ignore in jaal's src/platform/posix/signals.cpp).
    // One owner, and it is jaal's.

    // Both Fullscreen and Inline transitions consume the Raw terminal and
    // return a new type-state whose destructor reverses the opt-ins.  This
    // is the entire fault-tolerance story for terminal cleanup — no manual
    // cleanup() function can be forgotten because the type's destructor
    // runs on every exit path, including stack unwinding from exceptions.
    std::optional<Terminal<AltScreen>>  alt_term;
    std::optional<Terminal<InlineMode>> inline_term;

    if (cfg.mode == Mode::Fullscreen) {
        MAYA_TRY_DECL(auto alt, std::move(raw).enter_alt_screen());
        alt_term = std::move(alt);
    } else {
        MAYA_TRY_DECL(auto inl, std::move(raw).enable_inline_mode());
        inline_term = std::move(inl);
    }


    Device rt;

    rt.alt_terminal_    = std::move(alt_term);
    rt.inline_terminal_ = std::move(inline_term);
    rt.output_handle_   = output_h;
    rt.input_handle_    = input_h;
    rt.writer_          = std::make_unique<Writer>(output_h);
    rt.theme_           = cfg.theme;
    // NOTE: the theme slot is NOT published here. This `rt` is a local that
    // gets moved into Result<Device>, then moved AGAIN into the caller's
    // variable, so a pointer taken now names storage that is dead before
    // the first frame. Publishing happens in run<>(), against the object
    // that actually lives for the session. (This was the bug behind "the
    // theme changes the text but never the background": app_set_theme()
    // wrote through the stale pointer, so rt.theme() — which is what the
    // canvas fill is keyed on — never left its startup value.)
    // Grid backend: emit binary cell frames for a cooperating host instead of
    // ANSI.  The host paints cells directly — so we also suppress the ANSI-
    // only chrome (the DEC-2026 sync wrapper below) that would otherwise
    // interleave escape bytes into the grid stream.
    rt.grid_mode_       = (cfg.backend == RenderBackend::Grid);
    // Emit DEC mode 2026 (synchronized output) brackets by DEFAULT, not
    // only when the env-heuristic can fingerprint the terminal. On every
    // terminal that supports the mode they make each frame swap atomically
    // — the single most effective flicker cure, and it works even on
    // terminals the heuristic misses (ssh/tmux passthrough, niche
    // emulators, anything that supports 2026 without leaving an env
    // marker). On terminals that DON'T support it the private-mode
    // set/reset is silently ignored (ECMA-48: unknown DEC private modes
    // are no-ops) — harmless-but-pointless, never corrupting. The only
    // opt-out is an explicit MAYA_NO_SYNC, for the rare emulator that
    // echoes unknown private modes as literal text.
    {
        const char* no_sync = std::getenv("MAYA_NO_SYNC");
        const std::string_view ns = no_sync ? no_sync : "";
        const bool disabled = !ns.empty()
            && ns != "0" && ns != "false" && ns != "no";
        // TERM=dumb is the one case the "harmless no-op" argument above does
        // not cover. It does not mean "a terminal that might not know this
        // mode" — it means NO escape sequences, which is a promise a user
        // makes deliberately (and issue #37 is someone noticing we broke it).
        // A dumb terminal has no DEC private-mode parser to ignore the
        // bytes with, so "silently ignored" becomes "printed as garbage".
        const bool dumb = theme::terminal_is_dumb();
        // Emit the wrapper unless explicitly disabled — harmless no-op
        // where unsupported, atomic frames where supported.
        rt.emit_sync_wrapper_ = !disabled && !dumb;
        // The HONEST support answer, for tick-rate gating. MAYA_NO_SYNC
        // forces it false; otherwise consult the env heuristic so apps
        // on Apple Terminal / ish / unconfigured tmux slow their
        // animation cadence instead of tearing every frame.
        rt.sync_supported_ =
            !disabled && ansi::env_supports_synchronized_output();
    }

    // Set terminal title if provided. Save the current one first on the
    // xterm title stack, so exit puts it back instead of leaving our title
    // on the tab. Terminals without the stack ignore it.
    if (!cfg.title.empty()) {
        const bool stack = !theme::terminal_is_dumb();
        auto seq = std::format("{}\x1b]0;{}\x07", stack ? "\x1b[22;0t" : "", cfg.title);
        (void)platform::io_write_all(output_h, seq);
        rt.title_pushed_ = stack;
    }

    // Enable mouse reporting if requested. Use maya's canonical sequence:
    // 1000 (button press/release) + 1002 (button-drag motion) + 1006 (SGR
    // extended coordinates) + 1007 (alt-scroll). This matches what
    // enter_alt_screen() emits and what kitty / xterm / wezterm expect —
    // 1003 (ANY-motion) floods move events and some terminals (kitty)
    // handle it inconsistently. Cached on the Device so cleanup()/dtor
    // emit the matching disable on every exit path (the InlineMode /
    // AltScreen destructors restore raw mode + screen but the simple-run
    // inline path does NOT enable mouse on its own).
    rt.mouse_enabled_ = cfg.mouse;
    rt.hover_motion_  = cfg.hover_motion;
    if (cfg.mouse) {
        // Base set: 1000 (press/release) + 1006 (SGR coords) + 1007 (alt-scroll).
        // Motion: 1002 (button-drag only) by default; 1003 (ANY-motion) when
        // the app opts into hover highlights via Options::hover_motion.
        static constexpr std::string_view kMouseOn =
            "\x1b[?1000h\x1b[?1002h\x1b[?1006h\x1b[?1007h";
        static constexpr std::string_view kMouseOnHover =
            "\x1b[?1000h\x1b[?1003h\x1b[?1006h\x1b[?1007h";
        (void)platform::io_write_all(output_h,
            cfg.hover_motion ? kMouseOnHover : kMouseOn);
    }

    // Focus reporting (?1004): CSI I / CSI O arrive as FocusEvent and
    // reach apps via on(const Model&, const FocusEvent&). Cheap, universally ignored where
    // unsupported, and the hardware-caret path wants it (hide the caret
    // while the terminal window is unfocused). Same enable/disable
    // discipline as mouse: on at create, off at suspend + cleanup.
    (void)platform::io_write_all(output_h, ansi::enable_focus);

    // Negotiate the kitty keyboard protocol (progressive enhancement). The
    // push is a private CSI that unsupported terminals silently ignore; on
    // terminals that DO support it (kitty, ghostty, foot, WezTerm, iTerm2
    // 3.5+, Konsole, recent xterm/Alacritty/Rio, Blink Shell on iOS, tmux
    // 3.3+), modifier chords legacy encoding can't express — Ctrl+/, Ctrl+Tab,
    // Shift+Enter — now arrive as unambiguous CSI-u events, and Esc stops
    // needing the disambiguation timeout. Popped on every exit path so the
    // next program on this terminal sees the mode it expects.
    {
        const char* off = std::getenv("MAYA_NO_KITTY_KEYBOARD");
        const bool env_off = off && *off && !(off[0] == '0' && off[1] == '\0');
        if (cfg.enhanced_keyboard && !env_off) {
            (void)platform::io_write_all(output_h, ansi::kitty_keyboard_push);
            rt.kitty_kbd_enabled_ = true;
        }
    }

    // Query initial terminal size.
    if (rt.alt_terminal_) {
        rt.size_ = rt.alt_terminal_->size();
    } else if (rt.inline_terminal_) {
        rt.size_ = rt.inline_terminal_->size();
    }

    rt.render_ctx_.width      = rt.size_.width.raw();
    rt.render_ctx_.height     = rt.size_.height.raw();
    rt.render_ctx_.generation = 0;

    // Pre-reserve layout_nodes_ so the first big-frame render doesn't
    // pay a chain of std::vector reallocs (each doubling means the
    // last realloc copies every node built so far). 1024 nodes covers
    // typical agentty trees (composer + scrollback + a few turns);
    // deeper trees still grow on demand via vector's amortised
    // doubling, but the steady-state working set lives entirely
    // inside this initial capacity. Cost: 1024 * sizeof(LayoutNode)
    // ≈ 184 KB — paid once per Device lifetime.
    rt.layout_nodes_.reserve(1024);

    // Schedule hint — see platform/thread.hpp. macOS: QoS user-interactive.
    // Linux/Win32: no-op.
    platform::set_ui_thread_priority();

    // Inline mode: start in the Witness Chain's `Empty` state. The first
    // render's path seeds it to `Fresh` (via `seed()`) and runs compose's
    // case (A) — emit from the cursor's current position via serialize(),
    // growing downward without disturbing host content above.
    //
    // We can't pre-seed `cursor_hidden = true` the way the legacy state
    // could because InlineFrame<Empty> has no state to carry. The cost
    // is that compose emits the hide_cursor escape on its first frame
    // when it would have been a no-op under the legacy seed — a 6-byte
    // wire cost paid exactly once per session. Worth it for the type-
    // state guarantee that the runtime starts from a state the chain
    // can construct, not one synthesized by side-effect.
    (void)rt;   // is_inline()-conditional init no longer needed; the default
                // `InlineFrame<Empty>{}` field initializer covers it.

    // ── Inline mouse anchor (cursor-position query / DSR) ────────────────
    // In inline mode the frame is drawn partway down the terminal, but SGR
    // mouse reports are ABSOLUTE. Ask the terminal where the cursor is right
    // now (= where the first frame's top row will land) so the run loop can
    // translate mouse coordinates into frame-relative space. Done here, once,
    // synchronously: during this exchange we KNOW a Cursor-Position Report is
    // coming, so parsing `CSI <row>;<col> R` is unambiguous (in the normal
    // input stream that form collides with modified-F3, which is why we do
    // NOT route it through the parser). Bounded + best-effort: if the
    // terminal never answers, inline_top_row_ stays 0 → no offset → behavior
    // is identical to before. Only runs for inline + mouse.
    // Both startup queries go out in ONE write and are answered in one
    // round-trip: the cursor position (inline + mouse) and, outside tmux,
    // the OSC 5522 probe with its DA1 fence. Replies come back in order, so
    // when the DA1 lands everything asked has arrived. Over ssh each round
    // trip is a network RTT, so two in a row doubled the wait for the first
    // frame.
    const bool want_cpr  = cfg.mouse && rt.inline_terminal_;
    const bool want_5522 = !ansi::tmux_in_path();
    // OSC 10/11/4: the terminal's own foreground, background and 16-colour
    // palette. Asked in the same write, so it costs no extra round-trip.
    // Native colours can then be measured, which is what tinted bands and
    // panels need. Truecolor only: on a 256/16-colour terminal a mixed tint
    // would be crushed to the nearest palette entry and look worse.
    const bool want_colors = terminal_color_level() >= 3;
    if (want_cpr || want_5522 || want_colors) {
        std::string ask;
        if (want_cpr)  ask += "\x1b[6n";
        if (want_5522) ask += "\x1b[?5522$p";
        if (want_colors) {
            ask += "\x1b]10;?\x1b\\\x1b]11;?\x1b\\";
            for (int k = 0; k < 16; ++k)
                ask += "\x1b]4;" + std::to_string(k) + ";?\x1b\\";
        }
        ask += "\x1b[c";   // DA1 fence: every terminal answers, last
        (void)platform::io_write_all(output_h, ask);

        // `CSI rows ; cols R` → rows (erased), 0 if none complete yet.
        auto take_cpr = [](std::string& buf) -> int {
            for (std::size_t i = 0; i + 1 < buf.size(); ++i) {
                if (buf[i] != '\x1b' || buf[i + 1] != '[') continue;
                std::size_t j = i + 2; long row = 0; bool digits = false;
                for (; j < buf.size() && buf[j] >= '0' && buf[j] <= '9'; ++j) {
                    row = row * 10 + (buf[j] - '0'); digits = true;
                }
                if (!digits) continue;                 // not a numeric CSI
                if (j >= buf.size()) return 0;          // incomplete → wait
                if (buf[j] != ';') continue;            // some other CSI
                for (++j; j < buf.size() && buf[j] >= '0' && buf[j] <= '9'; ++j) {}
                if (j >= buf.size()) return 0;          // incomplete → wait
                if (buf[j] != 'R') continue;            // not a CPR
                buf.erase(i, j - i + 1);                // splice the CPR out
                return static_cast<int>(row);
            }
            return 0;
        };
        // `CSI ? 5522 ; Ps $ y` → Ps (erased), -1 if absent.
        auto take_rpm = [](std::string& buf) -> int {
            const std::string_view pre = "\x1b[?5522;";
            auto i = buf.find(pre);
            if (i == std::string::npos) return -1;
            std::size_t j = i + pre.size();
            long ps = 0; bool digits = false;
            for (; j < buf.size() && buf[j] >= '0' && buf[j] <= '9'; ++j) {
                ps = ps * 10 + (buf[j] - '0'); digits = true;
            }
            if (!digits || j + 1 >= buf.size()) return -1;   // incomplete
            if (buf[j] != '$' || buf[j + 1] != 'y') return -1;
            buf.erase(i, j - i + 2);
            return static_cast<int>(ps);
        };
        // Strip a complete DA1 reply (CSI ? … c); true when it has landed.
        auto take_da1 = [](std::string& buf) -> bool {
            auto i = buf.find("\x1b[?");
            while (i != std::string::npos) {
                std::size_t j = i + 3;
                while (j < buf.size()
                       && (buf[j] == ';' || (buf[j] >= '0' && buf[j] <= '9')))
                    ++j;
                if (j >= buf.size()) return false;   // incomplete → wait
                if (buf[j] == 'c') { buf.erase(i, j - i + 1); return true; }
                i = buf.find("\x1b[?", i + 1);
            }
            return false;
        };
        auto read_raw = [&]() -> std::string {
            if (rt.inline_terminal_) {
                if (auto r = rt.inline_terminal_->read_raw()) return *r;
            } else if (rt.alt_terminal_) {
                if (auto r = rt.alt_terminal_->read_raw()) return *r;
            }
            return {};
        };
        // `OSC 1x ; rgb:RRRR/GGGG/BBBB ST` → colour (erased). Accepts 1-4 hex
        // digits per channel and either ST or BEL as the terminator.
        auto take_osc_color = [](std::string& buf, int which,
                                 int sub = -1) -> std::optional<LitColor> {
            const std::string pre = "\x1b]" + std::to_string(which)
                + (sub >= 0 ? ";" + std::to_string(sub) : std::string{}) + ";rgb:";
            auto i = buf.find(pre);
            if (i == std::string::npos) return std::nullopt;
            std::size_t j = i + pre.size();
            int ch[3] = {0, 0, 0};
            for (int c = 0; c < 3; ++c) {
                int v = 0, n = 0;
                while (j < buf.size() && std::isxdigit(static_cast<unsigned char>(buf[j]))) {
                    const char x = buf[j++];
                    v = v * 16 + (std::isdigit(static_cast<unsigned char>(x))
                                      ? x - '0' : (std::tolower(x) - 'a' + 10));
                    ++n;
                }
                if (n == 0) return std::nullopt;
                // Scale an n-digit hex value to 8 bits.
                const int max = (1 << (4 * n)) - 1;
                ch[c] = (v * 255 + max / 2) / max;
                if (c < 2) {
                    if (j >= buf.size() || buf[j] != '/') return std::nullopt;
                    ++j;
                }
            }
            std::size_t end = j;
            if (end < buf.size() && buf[end] == '\x07') ++end;
            else if (end + 1 < buf.size() && buf[end] == '\x1b' && buf[end + 1] == '\\') end += 2;
            else return std::nullopt;   // incomplete
            buf.erase(i, end - i);
            return LitColor::rgb(static_cast<uint8_t>(ch[0]), static_cast<uint8_t>(ch[1]),
                                 static_cast<uint8_t>(ch[2]));
        };
        // The deadline is only a safety net for a pty that answers nothing.
        const auto deadline = std::chrono::steady_clock::now()
                            + std::chrono::milliseconds(150);
        std::string resp;
        int row = 0, ps = -1;
        std::optional<LitColor> term_fg, term_bg;
        std::optional<LitColor> term_pal[16];
        auto take_colors = [&] {
            if (!want_colors) return;
            if (!term_fg) term_fg = take_osc_color(resp, 10);
            if (!term_bg) term_bg = take_osc_color(resp, 11);
            for (int k = 0; k < 16; ++k)
                if (!term_pal[k]) term_pal[k] = take_osc_color(resp, 4, k);
        };
        bool fenced = false;
        while (std::chrono::steady_clock::now() < deadline) {
            auto data = read_raw();
            if (!data.empty()) {
                resp += data;
                if (want_cpr && row == 0) row = take_cpr(resp);
                if (want_5522 && ps < 0)  ps = take_rpm(resp);
                take_colors();
                // DA1 is answered last: once it's here, so is the rest.
                if (take_da1(resp)) { fenced = true; break; }
            } else {
                // Wake when the reply arrives, not on a timer.
                (void)platform::io_poll_readable(rt.input_handle_, 2);
            }
        }
        if (fenced) {   // a reply split across reads before the fence
            if (want_cpr && row == 0) row = take_cpr(resp);
            if (want_5522 && ps < 0)  ps = take_rpm(resp);
            take_colors();
        }
        if (term_fg && term_bg) theme::set_terminal_colors(*term_fg, *term_bg);
        for (int k = 0; k < 16; ++k)
            if (term_pal[k]) theme::set_terminal_palette(k, *term_pal[k]);
        if (row > 0) rt.inline_top_row_ = row;
        if (ps >= 0)
            rt.osc5522_support_ = (ps >= 1 && ps <= 4)
                ? detail::Device::Osc5522::Supported
                : detail::Device::Osc5522::Unsupported;
        // Anything else that arrived (a keypress during the query) goes
        // through the parser and is replayed on the first read_events().
        if (!resp.empty())
            for (auto& e : rt.parser_.feed(resp))
                rt.startup_events_.push_back(std::move(e));
    }

    return ok(std::move(rt));
}

// ============================================================================
// Device::handle_resize — update internal state on terminal resize
// ============================================================================

} // namespace maya::detail
