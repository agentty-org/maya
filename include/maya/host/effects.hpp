#pragma once
// maya/host/effects.hpp — things only the TERMINAL can do.
//
// These are this host's effects rather than jaal core ones, so a program
// lists the ones it uses in its Cmd:
//
//     using Cmd = jaal::Cmd<Msg, commit_scrollback>;
//     return commit_from(m.frozen.harvest());
//
// and a host that can't do them (a test host, a GUI) won't compile against
// it. terminal_host (host.hpp) is what carries them out; each one is a
// single Screen call.

#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <jaal/jaal.hpp>

#include "../render/scrollback_ledger.hpp"   // ScrollbackDebt
#include "../terminal/input.hpp"             // ClipboardDialect

namespace maya {

/// Commit rows of the last inline frame to the terminal's scrollback. Use it when view() is about to return
/// a shorter tree — a chat that virtualises old messages — so the row-diff
/// renderer doesn't read the shrink as rows removed from the bottom and
/// erase them. No effect in fullscreen.
///
/// It carries maya's TYPED ScrollbackDebt, not an int, on purpose. A debt
/// can only be minted by ScrollbackLedger::harvest(), whose rows were
/// recorded by maya's own paint pass, so a program structurally can't
/// commit a row count that drifts from what's on the wire — maya deprecated
/// its raw-int commit for exactly that reason. An `int rows` payload here
/// would have quietly thrown that guarantee away.
struct CommitScrollback { ScrollbackDebt debt; };
using commit_scrollback = jaal::pure_fx<CommitScrollback, "commit_scrollback">;

/// The Cmd for a harvested debt: nothing to do when it's empty.
template <class C>
[[nodiscard]] C commit_from(ScrollbackDebt debt) {
    if (debt.empty()) return C{};
    return C(CommitScrollback{debt});
}

/// Set the terminal window title.
struct SetTitle { std::string title; };
using set_title = jaal::pure_fx<SetTitle, "set_title">;

/// Put text on the system clipboard (OSC 52).
struct WriteClipboard { std::string text; };
using write_clipboard = jaal::pure_fx<WriteClipboard, "write_clipboard">;

/// Ask the terminal for its clipboard (OSC 52 read). The reply arrives as a
/// PasteEvent, so subscribe with on_paste.
/// `dialect` picks OSC 5522 / OSC 52 (see ClipboardDialect).
struct QueryClipboard {
    ClipboardDialect dialect = ClipboardDialect::automatic;
};
using query_clipboard = jaal::pure_fx<QueryClipboard, "query_clipboard">;

/// An already-formed control sequence for a cooperating HOST terminal
/// (editor OSC hooks, notifications), out of band with the frame renderer.
struct EmitHostSequence { std::string sequence; };
using emit_host_sequence = jaal::pure_fx<EmitHostSequence, "emit_host_sequence">;

/// `ESC ] code ; payload ST`.
[[nodiscard]] inline EmitHostSequence osc(int code, std::string_view payload) {
    std::string seq = "\x1b]" + std::to_string(code) + ';';
    seq.append(payload);
    seq += "\x1b\\";
    return {std::move(seq)};
}

/// Commit whatever of the last inline frame overflowed the viewport.
struct CommitOverflow {};
using commit_overflow = jaal::pure_fx<CommitOverflow, "commit_overflow">;

/// Repaint everything from scratch.
struct ForceRedraw {};
using force_redraw = jaal::pure_fx<ForceRedraw, "force_redraw">;

/// Drop the inline frame's history and start a fresh one below it.
struct ResetInline {};
using reset_inline = jaal::pure_fx<ResetInline, "reset_inline">;

/// Turn mouse capture on or off (off lets the terminal's own text
/// selection work). Starts as Options::mouse says.
struct SetMouse { bool on; };
using set_mouse = jaal::pure_fx<SetMouse, "set_mouse">;

/// Hand the real terminal to an interactive child (sudo, $EDITOR, a pager)
/// and fold the result back in. `run` executes on the
/// loop thread with the TUI torn down to a cooked tty; its return value is
/// the host's ANSWER (jaal D39), folded in the same step. It's a callback
/// because the child IS the effect, but it never leaves the loop thread,
/// and the answer is an ordinary message, so replay folds the result and
/// never re-runs the child.
template <class Msg> struct Suspend { std::function<Msg()> run; };
struct suspend {
    static constexpr std::string_view name = "suspend";
    template <class Msg> using type = Suspend<Msg>;
    template <class F, class M>
    static auto fmap(F&& f, Suspend<M> e) -> Suspend<std::invoke_result_t<F, M>> {
        return {[g = std::move(e.run), f = std::forward<F>(f)] { return f(g()); }};
    }
    template <class Id, class F, class M>
    static auto fmap_with(const Id& id, F&& f, Suspend<M> e)
        -> Suspend<std::invoke_result_t<F, const Id&, M>> {
        return {[g = std::move(e.run), f = std::forward<F>(f), id] { return f(id, g()); }};
    }
    template <class Self, class Msg> struct ctors {
        template <class F> requires std::is_invocable_r_v<Msg, F&>
        [[nodiscard]] static Self suspend(F f) { return Self(Suspend<Msg>{std::move(f)}); }
    };
};

/// Every terminal effect this host can carry out, for a program that wants
/// the whole set: `jaal::Cmd<Msg, maya::terminal_fx>` is not valid (a row
/// is a list), so spell it as `terminal_cmd<Msg>`.
template <class Msg>
using terminal_cmd = jaal::Cmd<Msg, commit_scrollback, set_title, write_clipboard,
                               query_clipboard, emit_host_sequence, commit_overflow,
                               force_redraw, reset_inline, set_mouse, suspend>;

}  // namespace maya
