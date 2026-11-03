// maya/terminal/scrollback.hpp — how many rows of scrollback the terminal keeps.
//
// No escape sequence reports this, so it is pieced together:
//
//   1. a multiplexer between us and the terminal keeps its OWN scrollback,
//      and that is what the user scrolls: tmux (history-limit, from the
//      probe tmux.cpp already makes), zellij, GNU screen.
//   2. otherwise the terminal, identified from the environment it sets,
//      and its setting read from its config file, else its documented
//      default.
//
// Startup pays nothing. `fast()` uses only the environment and the tmux
// probe that is already cached. `full()` also reads config files (and
// gsettings / defaults on Linux / macOS), so call it off the UI thread;
// it caches, so later calls are free.
#pragma once

#include <functional>
#include <string>

namespace maya::terminal {

struct Scrollback {
    int         rows      = 0;      // 0 = unknown, or see `unlimited`
    bool        unlimited = false;  // the terminal keeps all of it
    bool        exact     = false;  // read from its config or live, not a default
    std::string source;             // "tmux", "kitty.conf", "kitty default", ...
};

// Env + cached tmux only. Cheap; safe on the UI thread.
[[nodiscard]] Scrollback scrollback_fast() noexcept;

// Also reads the terminal's config. May touch the disk or run one small
// helper (gsettings, defaults); call it from a worker. Cached after the
// first call.
[[nodiscard]] Scrollback scrollback_full() noexcept;

// The detection itself, so tests can feed a fake environment and files.
struct ScrollbackInputs {
    std::function<std::string(const char*)>        env;        // "" = unset
    std::function<std::string(const std::string&)> read_file;  // "" = missing
    std::function<std::string(const char*)>        ask;        // helper output, "" = none
    int  tmux_history = 0;     // tmux history-limit, 0 = not in tmux / unknown
    bool read_config  = true;  // false = the fast path
};
[[nodiscard]] Scrollback detect_scrollback(const ScrollbackInputs& in);

}  // namespace maya::terminal
