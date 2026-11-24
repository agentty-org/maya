// test_color_tier_consistency — one terminal, one answer, everywhere.
//
// THE BUG THIS PINS. On Windows the diff's green `+` bands vanished while
// every other platform showed them. Three layers each answered a slightly
// different question and disagreed:
//
//   1. platform::is_tty() asked "is this a Win32 CONSOLE object?" via
//      GetConsoleMode. Under mintty / Git Bash / Cygwin / ConPTY, fds 0/1
//      are named PIPES carrying VT bytes, so it answered NO for a terminal
//      the user is plainly looking at. Win32Terminal::open() already knew
//      this (it falls into pipe_mode_ rather than failing) -- is_tty was
//      never told, so every other consumer inherited the wrong answer.
//
//   2. detect_tier() rejected on !tty BEFORE consulting the host markers,
//      so WT_SESSION ("I am Windows Terminal, I do 24-bit") could not save
//      it. Result: Mono. Mono then collapses every named scheme to native
//      (ui_prefs::resolve's can_paint gate) and drops diff row bands
//      (ToolBodyPreview::diff_bands_ok needs level >= 2).
//
//   3. detect_color_level() hardcoded `tty=true` when asking detect_tier,
//      so the EMIT path could reach a different conclusion than the
//      settings UI that reports "detected: ...".
//
// The invariant that was missing: a terminal's colour capability is one
// fact, and every layer must derive it from the same place with the same
// precedence. These tests state that precedence as executable rules.

#include <maya/core/target.hpp>
#include <doctest/doctest.h>

#include <maya/style/theme.hpp>

#include <cstdlib>
#include <string>

namespace {

// Scoped environment edit: set/unset a var and restore it afterwards, so
// cases cannot leak into each other (detection reads the real environment).
class EnvVar {
public:
    EnvVar(const char* key, const char* value) : key_(key) {
        if (const char* old = std::getenv(key)) { had_ = true; old_ = old; }
#if MAYA_OS_WINDOWS
        _putenv_s(key, value ? value : "");
#else
        if (value) ::setenv(key, value, 1); else ::unsetenv(key);
#endif
    }
    ~EnvVar() {
#if MAYA_OS_WINDOWS
        _putenv_s(key_.c_str(), had_ ? old_.c_str() : "");
#else
        if (had_) ::setenv(key_.c_str(), old_.c_str(), 1);
        else      ::unsetenv(key_.c_str());
#endif
    }
    EnvVar(const EnvVar&) = delete;
    EnvVar& operator=(const EnvVar&) = delete;
private:
    std::string key_, old_;
    bool        had_ = false;
};

// A clean slate: every signal detection looks at, cleared. Each test then
// sets only the ones it is actually about.
struct CleanEnv {
    EnvVar maya_color{"MAYA_COLOR", nullptr};
    EnvVar no_color{"NO_COLOR", nullptr};
    EnvVar clicolor{"CLICOLOR_FORCE", nullptr};
    EnvVar colorterm{"COLORTERM", nullptr};
    EnvVar term{"TERM", nullptr};
    EnvVar wt{"WT_SESSION", nullptr};
    EnvVar conemu{"ConEmuANSI", nullptr};
    EnvVar term_program{"TERM_PROGRAM", nullptr};
};

using maya::theme::ColorTier;
using maya::theme::detect_tier;

} // namespace

TEST_CASE("tier: a self-identifying host beats the handle probe") {
    CleanEnv env;

    // THE WINDOWS BUG, exactly. Windows Terminal sets WT_SESSION but not
    // COLORTERM, and under an MSYS2/ConPTY pipe the handle probe says
    // "not a console". Ordering the probe first returned Mono -- no theme,
    // no diff bands -- for a terminal that does 24-bit colour.
    EnvVar wt{"WT_SESSION", "1"};
    CHECK(detect_tier(/*tty=*/true)  == ColorTier::TrueColor);
    CHECK(detect_tier(/*tty=*/false) == ColorTier::TrueColor);
}

TEST_CASE("tier: ConEmu and TERM_PROGRAM hosts are treated the same way") {
    {
        CleanEnv env;
        EnvVar v{"ConEmuANSI", "ON"};
        CHECK(detect_tier(/*tty=*/false) == ColorTier::TrueColor);
    }
    {
        CleanEnv env;
        EnvVar v{"TERM_PROGRAM", "vscode"};
        CHECK(detect_tier(/*tty=*/false) == ColorTier::TrueColor);
    }
}

TEST_CASE("tier: an ordinary redirect is still Mono") {
    // The host-marker exemption must not become "always paint". A plain
    // pipe with no terminal behind it gets no colour, which is what keeps
    // `agentty > log.txt` free of escape soup.
    CleanEnv env;
    EnvVar term{"TERM", "xterm-256color"};
    CHECK(detect_tier(/*tty=*/false) == ColorTier::Mono);
    CHECK(detect_tier(/*tty=*/true)  == ColorTier::Ansi256);
}

TEST_CASE("tier: the refusals outrank every capability signal") {
    // Order matters more than any single answer: a user who says "no" must
    // win over a terminal that says "I can".
    SUBCASE("NO_COLOR beats a truecolor host") {
        CleanEnv env;
        EnvVar wt{"WT_SESSION", "1"};
        EnvVar ct{"COLORTERM", "truecolor"};
        EnvVar nc{"NO_COLOR", "1"};
        CHECK(detect_tier(/*tty=*/true) == ColorTier::Mono);
    }
    SUBCASE("TERM=dumb beats a truecolor host") {
        CleanEnv env;
        EnvVar wt{"WT_SESSION", "1"};
        EnvVar term{"TERM", "dumb"};
        CHECK(detect_tier(/*tty=*/true) == ColorTier::Mono);
        CHECK(maya::theme::terminal_is_dumb());
    }
    SUBCASE("an empty NO_COLOR is unset, per no-color.org") {
        CleanEnv env;
        EnvVar ct{"COLORTERM", "truecolor"};
        EnvVar nc{"NO_COLOR", ""};
        CHECK(detect_tier(/*tty=*/true) == ColorTier::TrueColor);
    }
    SUBCASE("MAYA_COLOR overrides everything, including NO_COLOR") {
        CleanEnv env;
        EnvVar nc{"NO_COLOR", "1"};
        EnvVar mc{"MAYA_COLOR", "truecolor"};
        CHECK(detect_tier(/*tty=*/true) == ColorTier::TrueColor);
    }
}

TEST_CASE("tier: every terminal family lands somewhere sane") {
    // The consistency claim, stated as a table. No terminal a user is
    // plausibly sitting in front of may answer Mono, because Mono silently
    // disables named schemes AND diff bands -- the two things that made
    // this look broken on one platform and fine on every other.
    struct Row { const char* term; const char* colorterm; ColorTier want; };
    const Row rows[] = {
        // Self-declaring truecolor.
        {"xterm-256color", "truecolor", ColorTier::TrueColor},
        {"xterm-256color", "24bit",     ColorTier::TrueColor},
        // Truecolor terminals that only say so via TERM (ssh drops
        // COLORTERM but forwards TERM).
        {"xterm-kitty",    nullptr,     ColorTier::TrueColor},
        {"xterm-ghostty",  nullptr,     ColorTier::TrueColor},
        {"wezterm",        nullptr,     ColorTier::TrueColor},
        {"alacritty",      nullptr,     ColorTier::TrueColor},
        {"foot",           nullptr,     ColorTier::TrueColor},
        // 256-colour families.
        {"tmux-256color",  nullptr,     ColorTier::Ansi256},
        {"screen-256color",nullptr,     ColorTier::Ansi256},
        {"xterm-256color", nullptr,     ColorTier::Ansi256},
        // Bare families: indexed colour is universally safe here.
        {"xterm",          nullptr,     ColorTier::Ansi256},
        {"screen",         nullptr,     ColorTier::Ansi256},
        {"tmux",           nullptr,     ColorTier::Ansi256},
        {"linux",          nullptr,     ColorTier::Ansi256},
    };

    for (const auto& r : rows) {
        CleanEnv env;
        EnvVar term{"TERM", r.term};
        EnvVar ct{"COLORTERM", r.colorterm};
        const ColorTier got = detect_tier(/*tty=*/true);
        INFO("TERM=" << r.term
             << " COLORTERM=" << (r.colorterm ? r.colorterm : "<unset>"));
        CHECK(got == r.want);
        // The load-bearing half: never Mono for a real terminal.
        CHECK(got != ColorTier::Mono);
    }
}

TEST_CASE("tier: diff bands are available on every real terminal") {
    // ToolBodyPreview::diff_bands_ok() is `level >= 2`, i.e. Ansi256 or
    // better. That gate is why the green `+` band disappeared on Windows
    // while showing everywhere else, so state it directly: any terminal a
    // human is using must clear it.
    const char* terms[] = {
        "xterm-256color", "tmux-256color", "screen-256color",
        "xterm-kitty", "alacritty", "wezterm", "foot", "xterm", "linux",
    };
    for (const char* t : terms) {
        CleanEnv env;
        EnvVar term{"TERM", t};
        const ColorTier got = detect_tier(/*tty=*/true);
        INFO("TERM=" << t);
        CHECK((got == ColorTier::TrueColor || got == ColorTier::Ansi256));
    }

    // And the Windows host case, which has no useful TERM at all.
    {
        CleanEnv env;
        EnvVar wt{"WT_SESSION", "1"};
        const ColorTier got = detect_tier(/*tty=*/false);
        CHECK((got == ColorTier::TrueColor || got == ColorTier::Ansi256));
    }
}

TEST_CASE("tier: detection is a pure function of its inputs") {
    // Same environment in, same tier out -- no hidden state, no first-call
    // caching inside detect_tier. (terminal_color_level memoises on top of
    // it deliberately; that is a different layer.)
    CleanEnv env;
    EnvVar term{"TERM", "xterm-256color"};
    EnvVar ct{"COLORTERM", "truecolor"};
    const ColorTier a = detect_tier(true);
    for (int i = 0; i < 8; ++i) CHECK(detect_tier(true) == a);
}
