// src/device/host_effects.cpp — title, clipboard, raw sequences, suspend.
#include "internal.hpp"

namespace maya::detail {

// ============================================================================
// Device::set_title — set terminal title via OSC 0
// ============================================================================
//
// Routed through write_or_buffer so the OSC queues behind any pending
// render residue. write_raw bypassed the residue queue and called
// write_all directly, which on a partial-write WouldBlock emits the
// CAN/SUB/ST recovery sequence — landing mid-frame on the wire while
// a streaming compose's bytes are still draining. The interleave
// could split a CSI mid-sequence, corrupting the wire's contents
// against our prev_cells shadow and freezing the visible frame until
// a hard redraw (resize) rebuilt shadow from scratch. Same fix shape
// as write_clipboard's earlier migration; the two OSC paths now have
// identical residue semantics.
void Device::set_title(std::string_view title) {
    auto seq = std::format("\x1b]0;{}\x07", title);
    (void)writer_->write_or_buffer(seq);
}

// ============================================================================
// Device::write_clipboard — system clipboard via OSC 52
// ============================================================================
//
// OSC 52 protocol:
//
//   ESC ] 52 ; c ; <base64-encoded-utf8> ST
//
// The `c` selector targets the regular clipboard (vs `p` for primary on
// X11). Bytes are base64-encoded so binary / multi-line payloads survive
// the terminal's escape-sequence parser unaltered. Empty payload clears
// the clipboard — routed through the encoder uniformly, no special-case.
//
// Cross-platform story:
//
//   Transport: writer_ → platform::io_write, which dispatches to
//   POSIX `::write()` on Linux/macOS and Win32 `::WriteFile()` on
//   Windows. The platform abstraction is the same one set_title()
//   uses and matches every other byte that leaves the runtime.
//
//   Protocol: OSC 52 is interpreted by the terminal emulator, not
//   the OS. Native support: xterm, kitty, alacritty, wezterm, foot,
//   ghostty, rio, iTerm2 (opt-in: Prefs → General → Selection →
//   "Applications in terminal may access clipboard"), Windows
//   Terminal 1.7+, tmux/screen with set-clipboard enabled.
//   Terminals without OSC 52 support discard the OSC string
//   silently per ECMA-48 §8.3.89.
//
// Bytes go through write_or_buffer (not write_raw) so the sequence
// queues behind any pending render residue instead of landing
// mid-frame on the wire — interleaving OSC 52 bytes into an
// unfinished CSI / OSC from the previous compose corrupts the wire's
// shadow against prev_cells and causes a visible repaint glitch.
//
// Terminator: ST (ESC \) rather than BEL. ST is the spec terminator
// per ECMA-48 §8.3.143; BEL is the historic xterm shorthand. ST
// survives tmux's set-clipboard passthrough cleanly on every tmux
// version we've tested; BEL is mangled by some older builds.
void Device::write_clipboard(std::string_view text) {
    // RFC 4648 standard alphabet — OSC 52 requires standard (not URL-safe)
    // base64. Padding is required per the OSC 52 spec; terminals reject
    // non-padded payloads inconsistently.
    static constexpr char kB64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string encoded;
    encoded.reserve((text.size() + 2) / 3 * 4);
    std::size_t i = 0;
    const auto* src = reinterpret_cast<const unsigned char*>(text.data());
    while (i + 3 <= text.size()) {
        std::uint32_t v = (std::uint32_t(src[i])     << 16)
                        | (std::uint32_t(src[i + 1]) <<  8)
                        |  std::uint32_t(src[i + 2]);
        encoded.push_back(kB64[(v >> 18) & 0x3F]);
        encoded.push_back(kB64[(v >> 12) & 0x3F]);
        encoded.push_back(kB64[(v >>  6) & 0x3F]);
        encoded.push_back(kB64[ v        & 0x3F]);
        i += 3;
    }
    const std::size_t rem = text.size() - i;
    if (rem == 1) {
        std::uint32_t v = std::uint32_t(src[i]) << 16;
        encoded.push_back(kB64[(v >> 18) & 0x3F]);
        encoded.push_back(kB64[(v >> 12) & 0x3F]);
        encoded.push_back('=');
        encoded.push_back('=');
    } else if (rem == 2) {
        std::uint32_t v = (std::uint32_t(src[i])     << 16)
                        | (std::uint32_t(src[i + 1]) <<  8);
        encoded.push_back(kB64[(v >> 18) & 0x3F]);
        encoded.push_back(kB64[(v >> 12) & 0x3F]);
        encoded.push_back(kB64[(v >>  6) & 0x3F]);
        encoded.push_back('=');
    }

    auto seq = std::format("\x1b]52;c;{}\x1b\\", encoded);
    (void)writer_->write_or_buffer(seq);
}

void Device::query_clipboard(ClipboardDialect dialect) {
    // Clipboard read query. Two dialects:
    //   • OSC 5522 (kitty) — multi-format: the reply can carry IMAGE
    //     bytes, which OSC 52 read replies never do. kitty is the only
    //     implementation, so gate on its env fingerprint.
    //   • OSC 52 read (everything else) — text-only, but widely
    //     honoured (iTerm2, WezTerm, foot, Ghostty, xterm w/ opts).
    // Either reply arrives on the input stream and is decoded by
    // InputParser into a PasteEvent. write_or_buffer so a congested tty
    // doesn't drop it; it's a control sequence the diff path never
    // re-emits.
    //
    // tmux twist: inside tmux kitty is undetectable (its env fingerprints
    // are stripped and TERM is rewritten), AND tmux swallows any OSC it
    // doesn't recognise unless it's wrapped in tmux passthrough. So when
    // TMUX is set we (1) send the OSC 5522 image request WRAPPED for tmux
    // — a kitty outer terminal answers with the image, a non-kitty one
    // ignores the unknown OSC — and (2) ALSO send OSC 52 (likewise wrapped)
    // as the text fallback for the non-kitty case. On kitty both may reply;
    // a short dedup window (see on paste handling) drops the second.
    const bool in_tmux = ansi::tmux_in_path();
    // The host knows which one the terminal speaks: send only that, so a
    // text reply can't race the image. Wrapped for tmux when it's in the path.
    if (dialect != ClipboardDialect::automatic) {
        const std::string q = dialect == ClipboardDialect::images ? ansi::request_clipboard_image()
                                                                  : ansi::request_clipboard();
        (void)writer_->write_or_buffer(in_tmux ? ansi::wrap_for_tmux(q) : q);
        return;
    }
    if (in_tmux) {
        (void)writer_->write_or_buffer(
            ansi::wrap_for_tmux(ansi::request_clipboard_image()));
        (void)writer_->write_or_buffer(
            ansi::wrap_for_tmux(ansi::request_clipboard()));
        return;
    }
    // Outside tmux: the DECRQM probe from create() is authoritative in the
    // POSITIVE direction only. +1 = the terminal implements the mode-5522
    // family, which mandates the OSC 5522 read escape → use it, regardless
    // of env. But 0 ("mode not recognized") does NOT imply the read escape
    // is absent: kitty itself implemented OSC 5522 reads YEARS before the
    // mode spec existed and reports unknown modes as 0 — disabling on 0
    // would break image paste on the reference implementation. So on 0/-1
    // fall back to env sniffing exactly as before the probe existed.
    const bool use_5522 = osc5522_support_ == Osc5522::Supported
                       || ansi::env_supports_osc5522();
    (void)writer_->write_or_buffer(use_5522
                                       ? ansi::request_clipboard_image()
                                       : ansi::request_clipboard());
}

// ============================================================================
// Device::emit_host_sequence — raw host escape, out-of-band with the frame
// ============================================================================
void Device::emit_host_sequence(std::string_view sequence) {
    if (sequence.empty()) return;
    // Same transport + buffering discipline as set_title/write_clipboard: the
    // bytes ride write_or_buffer so a congested tty stashes rather than drops
    // them, and the frame diff never accounts for or re-emits them. The
    // caller guarantees cursor-neutrality (the emit_host_sequence effect's contract), so
    // no coherence-state change is needed here.
    (void)writer_->write_or_buffer(sequence);
}

// ============================================================================
// Device::suspend — hand the real terminal to an interactive child
// ============================================================================
//
// Blocks on the UI thread for the child's whole run — deliberate: the
// user is interacting WITH the child (sudo password, live output), so
// there is nothing for the UI thread to do meanwhile. The heavy lifting
// (mode teardown/restore escapes, cooked↔raw toggling on the same fd)
// lives in Terminal<State>::suspend so the escape sets stay adjacent to
// the destructor sequences they mirror.
//
// Post-suspend repaint policy:
//   • Fullscreen → Divergent: alt-screen re-entry cleared the buffer,
//     the next render must full-serialize from home. force_redraw()
//     already encodes exactly that.
//   • Inline → the child scrolled arbitrary content under our frame;
//     the physical viewport no longer matches prev_cells anywhere. The
//     inline coherence must drop to a state that repaints from the
//     cursor's CURRENT position without trusting any prior row
//     accounting. reset_to_fresh_after_suspend(): re-anchor like a
//     first render (case A — serialize at cursor, grow downward,
//     never touch the child's output above). The child's output thus
//     stays in native scrollback ABOVE the re-rendered frame, exactly
//     like shell history above a freshly-launched TUI.
//
// Mouse reporting is torn down/restored by the Terminal suspend only in
// alt mode; inline mode enables mouse at the Device layer (create), so
// mirror that here.
void Device::suspend(const std::function<void()>& fn) {
    if (!fn) return;
    static constexpr std::string_view kMouseOn      = "\x1b[?1000h\x1b[?1002h\x1b[?1006h\x1b[?1007h";
    static constexpr std::string_view kMouseOnHover = "\x1b[?1000h\x1b[?1003h\x1b[?1006h\x1b[?1007h";
    static constexpr std::string_view kMouseOff = "\x1b[?1007l\x1b[?1006l\x1b[?1003l\x1b[?1002l\x1b[?1000l";

    // Flush any residue the writer is still holding — those bytes belong
    // to the pre-suspend frame and must not interleave with the child's
    // output after the mode switch. Bounded best-effort drain (~200 ms):
    // the fd is non-blocking, so spin try_drain_residue with tiny sleeps
    // rather than blocking forever on a wedged tty.
    if (writer_ && writer_->has_residue()) {
        for (int i = 0; i < 100 && writer_->has_residue(); ++i) {
            if (auto st = writer_->try_drain_residue(); !st) break;
            if (writer_->has_residue())
                (void)platform::io_poll_writable(output_handle_, 2);
        }
    }

    // The inline frame owes cursor-restore bytes: the hardware-caret
    // epilogue leaves the physical cursor at the caret cell, rows ABOVE
    // the frame bottom. The child (editor / pager) starts writing at
    // the cursor — from mid-frame it would clobber the composer rows
    // (same failure as the exit path). finalize seals the chain and
    // returns the cursor to the resting row; the post-child re-anchor
    // below already re-seeds Empty, so losing frame state here is free.
    finalize_inline_frame();

    if (mouse_enabled_ && output_handle_ != platform::invalid_handle)
        (void)platform::io_write_all(output_handle_, kMouseOff);
    if (output_handle_ != platform::invalid_handle)
        (void)platform::io_write_all(output_handle_, ansi::disable_focus);
    // The child (pager / editor) must see the terminal's native key encoding.
    if (kitty_kbd_enabled_ && output_handle_ != platform::invalid_handle)
        (void)platform::io_write_all(output_handle_, ansi::kitty_keyboard_pop);

    // Drop O_NONBLOCK for the child — it shares the open file
    // description and pagers / bulk writers don't expect EAGAIN.
    if (writer_) writer_->suspend_nonblocking();

    if (alt_terminal_) {
        (void)alt_terminal_->suspend(fn);
    } else if (inline_terminal_) {
        (void)inline_terminal_->suspend(fn);
    } else {
        fn();
    }

    if (writer_) writer_->resume_nonblocking();

    if (mouse_enabled_ && output_handle_ != platform::invalid_handle)
        (void)platform::io_write_all(output_handle_,
            hover_motion_ ? kMouseOnHover : kMouseOn);
    if (output_handle_ != platform::invalid_handle)
        (void)platform::io_write_all(output_handle_, ansi::enable_focus);
    // Re-negotiate the kitty keyboard protocol for our own input.
    if (kitty_kbd_enabled_ && output_handle_ != platform::invalid_handle)
        (void)platform::io_write_all(output_handle_, ansi::kitty_keyboard_push);

    // ── Re-anchor rendering ──
    fs_coherence_ = coherent::Divergent{};
    if (inline_terminal_) {
        // The child may have scrolled/printed anything; no prior inline
        // state is trustworthy. Re-seed to Empty — the next render takes
        // the first-frame path (case A): serialize at the cursor's
        // current row, growing downward, leaving the child's output
        // intact above. This is the same safe initial state create()
        // uses, for the same reason.
        in_coherence_ = inline_frame::InlineFrame<inline_frame::Empty>{};
        // The frame anchor learned at create() is stale — the child
        // moved the cursor arbitrarily. Zero it so mouse translation
        // doesn't mis-map rows until the next DSR/anchor pass.
        inline_top_row_    = 0;
        inline_frame_rows_ = 0;
    }
}

} // namespace maya::detail
