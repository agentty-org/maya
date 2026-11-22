#pragma once
// maya/host/run.hpp — write a terminal app. This is the include a program
// uses, and the whole of maya comes with it.
//
// maya is to jaal what Ink is to React. jaal runs the program — model,
// update, Cmd, Sub, threads, timers, shutdown. maya is the view layer and
// the terminal it draws on. maya/host/ is where maya meets a runtime:
//
//   interop.hpp    maya's value types, as jaal sees them (Sendable/Frozen)
//   effects.hpp    what only the TERMINAL can do — set_title, commit_scrollback,
//                  write_clipboard, suspend, ... A program lists the ones it
//                  uses in its Cmd, so a host that can't do them won't compile.
//   sources.hpp    what the terminal REPORTS — terminal_events (KeyEvent,
//                  MouseEvent, PasteEvent, FocusEvent, ResizeEvent); the
//                  Program concept; keymap<Msg>.
//   terminal.hpp   terminal_host: maya as a jaal host.
//   run.hpp        this file: run<P>().
//
// Everything below maya/host/ is runtime-agnostic — the elements, the
// layout, the widgets, the renderer, the device. That is the whole point of
// the split, and tests/seam.sh keeps it true.
//
// A program is an ordinary jaal program with a view():
//
//   #include <maya/host/run.hpp>
//
//   struct Counter {
//       struct Model { int n = 0; };
//       struct Inc {}; struct Quit {};
//       using Msg = std::variant<Inc, Quit>;
//       using Cmd = jaal::Cmd<Msg>;
//       static Cmd update(Model& m, Inc)  { ++m.n; return {}; }
//       static Cmd update(Model&, Quit)   { return Cmd::quit(0); }
//       static maya::Element view(const Model& m) { return text(...); }
//       static std::optional<Msg> on(const Model&, const maya::KeyEvent& k) {
//           static const maya::keymap<Msg> keys{{'+', Inc{}}, {'q', Quit{}}};
//           return keys(k);
//       }
//   };
//   int main() { return maya::run<Counter>({.title = "counter"}); }
//
// The entry point is four lines, and that is the design working: the loop is
// jaal's, the terminal is Screen's, and run() only introduces them. If it
// ever grows a branch on program state, something that belongs in a
// Program's update() has leaked into the host.
//
// Link maya::app. docs/internals/design.md has the rules.

#include <cstdio>
#include <utility>

#include <jaal/jaal.hpp>

#include "executor.hpp"
#include "../device/options.hpp"
#include "../screen.hpp"
#include "interop.hpp"
#include "effects.hpp"
#include "sources.hpp"
#include "terminal.hpp"

namespace maya {

/// Run a Program on jaal, in this terminal. The one entry point.
///
/// `flags` is the data a run starts with, for a program that declares
/// `using Flags = ...; static Cmd init(Model&, const Flags&)`: the caller
/// reads it (argv, the environment, a config) and passes it in, so init
/// never reads a global.
///
/// Exit codes: the program's own via Cmd::quit(n), or 70 (EX_SOFTWARE) if
/// the terminal can't be opened — there is no screen to report on, so it is
/// the one failure run() answers for itself.
///
/// `Host` is the host template: maya's terminal_host by default. An app with
/// effects of its own derives from terminal_host and passes its host here,
/// so it never calls jaal directly.
///
/// `class...`, not `class`: terminal_host is <Program P, Device Dev = Screen>,
/// and binding a two-parameter template (defaulted or not) to a
/// `template <class> class` relies on P0522, which MSVC doesn't implement --
/// every maya::run<App>() call failed there with C2672.
template <Program P, template <class...> class Host = terminal_host>
int run(Options cfg = {}, jaal::run_options opt = {}, jaal::prog::flags_t<P> flags = {}) {
    // Report a missing handle()/start_source() in jaal's words — it names
    // the effect and the program. Without this the error still happens, as
    // an unsatisfied constraint on jaal::run several levels down.
    jaal::require_host_for<Host<P>, P>();
    check_program<P>();

    auto term = Screen::open(cfg);
    if (!term) {
        // Exit 70 used to be silent, which made "agentty just exits" a
        // guessing game. The device error already names the failing call
        // and errno (tcgetattr / tcsetattr / write); say it, plus where.
        std::fprintf(stderr, "maya: can't take the terminal: %s (%s:%u)\n",
                     term.error().message.c_str(),
                     term.error().location.file_name(),
                     static_cast<unsigned>(term.error().location.line()));
        return 70;
    }
    // maya's own parallel work (pixel rows, big markdown parses) runs on
    // jaal for the program's lifetime, then is joined before we return.
    host_detail::executor_scope exec;
    Host<P> host{*term, cfg.fps};
    host.set_interrupt_exit(opt.default_signal_exit);   // ^C typed in raw mode
    // Flags: the data the run starts with (a program's `using Flags`), handed
    // to its init(Model&, const Flags&). A program without Flags passes none.
    return jaal::run<P>(host, std::move(opt), jaal::durable<P>{}, std::move(flags));
}

}  // namespace maya
