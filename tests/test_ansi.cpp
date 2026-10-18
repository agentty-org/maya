// Tests for ANSI escape sequence generation and StyleApplier
#include <maya/maya.hpp>
#include <maya/terminal/tmux.hpp>
// NDEBUG guard: CMake builds tests in Release (-O3 -DNDEBUG), which strips
// assert(). Undefine it here so this file's runtime asserts actually fire.
#undef NDEBUG
#include "agtest.hpp"
#include <cstdlib>   // setenv / unsetenv (tmux sync-detection test)
#include <print>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
namespace {
// The tmux sync-detection test fabricates an environment per case. POSIX
// setenv/unsetenv; the CRT's _putenv_s has the same contract (name, value,
// replace) and an empty value drops the variable for getenv.
void set_env(const char* k, const char* v, int) noexcept { (void)_putenv_s(k, v); }
void unset_env(const char* k) noexcept { (void)_putenv_s(k, ""); }
}  // namespace
#else
namespace {
void set_env(const char* k, const char* v, int replace) noexcept { ::setenv(k, v, replace); }
void unset_env(const char* k) noexcept { ::unsetenv(k); }
}  // namespace
#endif

using namespace maya;

// ============================================================================
// write_move_to (zero-alloc) vs move_to (allocating) consistency
// ============================================================================

TEST_CASE("move to format") {
    std::println("--- test_move_to_format ---");
    // move_to(col, row) → ESC[row;colH
    assert(ansi::move_to(1, 1) == "\x1b[1;1H");
    assert(ansi::move_to(10, 5) == "\x1b[5;10H");
    assert(ansi::move_to(80, 24) == "\x1b[24;80H");
    std::println("PASS\n");
}

TEST_CASE("write move to matches move to") {
    std::println("--- test_write_move_to_matches_move_to ---");
    // Zero-alloc write_move_to must produce identical output to move_to
    for (auto [col, row] : std::initializer_list<std::pair<int,int>>{{1,1},{10,5},{80,24},{1,100}}) {
        std::string expected = ansi::move_to(col, row);
        std::string got;
        ansi::write_move_to(got, col, row);
        assert(got == expected);
    }
    std::println("PASS\n");
}

TEST_CASE("write move to appends") {
    std::println("--- test_write_move_to_appends ---");
    std::string out = "PREFIX";
    ansi::write_move_to(out, 1, 1);
    assert(out.starts_with("PREFIX"));
    assert(out.size() > 6);
    std::println("PASS\n");
}

// ============================================================================
// Cursor movement helpers
// ============================================================================

TEST_CASE("move up down left right") {
    std::println("--- test_move_up_down_left_right ---");
    assert(ansi::move_up(1)    == "\x1b[1A");
    assert(ansi::move_down(1)  == "\x1b[1B");
    assert(ansi::move_right(1) == "\x1b[1C");
    assert(ansi::move_left(1)  == "\x1b[1D");
    assert(ansi::move_up(3)    == "\x1b[3A");
    assert(ansi::move_down(5)  == "\x1b[5B");
    std::println("PASS\n");
}

TEST_CASE("move zero returns empty") {
    std::println("--- test_move_zero_returns_empty ---");
    assert(ansi::move_up(0).empty());
    assert(ansi::move_down(0).empty());
    assert(ansi::move_right(0).empty());
    assert(ansi::move_left(0).empty());
    std::println("PASS\n");
}

TEST_CASE("home sequence") {
    std::println("--- test_home_sequence ---");
    assert(ansi::home() == "\x1b[H");
    std::println("PASS\n");
}

// ============================================================================
// Screen clearing
// ============================================================================

TEST_CASE("clear screen sequence") {
    std::println("--- test_clear_screen_sequence ---");
    assert(ansi::clear_screen() == "\x1b[2J");
    std::println("PASS\n");
}

TEST_CASE("clear line sequence") {
    std::println("--- test_clear_line_sequence ---");
    assert(ansi::clear_line() == "\x1b[2K");
    std::println("PASS\n");
}

// ============================================================================
// Known ANSI constants
// ============================================================================

TEST_CASE("cursor show hide constants") {
    std::println("--- test_cursor_show_hide_constants ---");
    // DEC private mode 25: show/hide cursor
    std::string show(ansi::show_cursor);
    std::string hide(ansi::hide_cursor);
    assert(show.find("?25h") != std::string::npos);
    assert(hide.find("?25l") != std::string::npos);
    std::println("PASS\n");
}

TEST_CASE("sync markers") {
    std::println("--- test_sync_markers ---");
    // DEC private mode 2026: synchronized output
    std::string start(ansi::sync_start);
    std::string end(ansi::sync_end);
    assert(start.find("?2026h") != std::string::npos);
    assert(end.find("?2026l")   != std::string::npos);
    std::println("PASS\n");
}

TEST_CASE("reset constant") {
    std::println("--- test_reset_constant ---");
    std::string r(ansi::reset);
    assert(r == "\x1b[0m" || r == "\x1b[m");
    std::println("PASS\n");
}

// ============================================================================
// Color SGR sequences
// ============================================================================

TEST_CASE("ansi fg sequence") {
    std::println("--- test_ansi_fg_sequence ---");
    std::string s = ansi::fg(Color::red());
    assert(s == "\x1b[31m");
    std::println("PASS\n");
}

TEST_CASE("ansi bg sequence") {
    std::println("--- test_ansi_bg_sequence ---");
    std::string s = ansi::bg(Color::blue());
    assert(s == "\x1b[44m");
    std::println("PASS\n");
}

// ============================================================================
// StyleApplier - allocating variants
// ============================================================================

TEST_CASE("style applier apply empty") {
    std::println("--- test_style_applier_apply_empty ---");
    std::string s = ansi::StyleApplier::apply(Style{});
    assert(s.empty()); // empty style produces no SGR
    std::println("PASS\n");
}

TEST_CASE("style applier apply bold") {
    std::println("--- test_style_applier_apply_bold ---");
    std::string s = ansi::StyleApplier::apply(Style{}.with_bold());
    assert(s.find("1") != std::string::npos);
    assert(s.starts_with("\x1b["));
    assert(s.ends_with("m"));
    std::println("PASS\n");
}

TEST_CASE("style applier apply multiple") {
    std::println("--- test_style_applier_apply_multiple ---");
    Style s = Style{}.with_bold().with_italic().with_fg(Color::red());
    std::string sgr = ansi::StyleApplier::apply(s);
    assert(sgr.find("1")  != std::string::npos); // bold
    assert(sgr.find("3")  != std::string::npos); // italic
    assert(sgr.find("31") != std::string::npos); // red fg
    std::println("PASS\n");
}

TEST_CASE("style applier transition same style") {
    std::println("--- test_style_applier_transition_same_style ---");
    Style s = Style{}.with_bold().with_fg(Color::green());
    assert(ansi::StyleApplier::transition(s, s).empty());
    std::println("PASS\n");
}

TEST_CASE("style applier transition add attribute") {
    std::println("--- test_style_applier_transition_add_attribute ---");
    Style a = Style{}.with_bold();
    Style b = Style{}.with_bold().with_fg(Color::cyan());
    std::string t = ansi::StyleApplier::transition(a, b);
    assert(!t.empty());
    assert(t.find("36") != std::string::npos); // cyan fg = 36
    std::println("PASS\n");
}

TEST_CASE("style applier transition remove attribute resets") {
    std::println("--- test_style_applier_transition_remove_attribute_resets ---");
    // Removing bold requires a reset because SGR has no individual "un-bold"
    Style a = Style{}.with_bold().with_fg(Color::red());
    Style b = Style{}.with_fg(Color::red()); // bold removed
    std::string t = ansi::StyleApplier::transition(a, b);
    assert(!t.empty());
    assert(t.find("\x1b[0m") != std::string::npos); // reset
    std::println("PASS\n");
}

// ============================================================================
// StyleApplier - zero-alloc variants
// ============================================================================

TEST_CASE("style applier apply to matches apply") {
    std::println("--- test_style_applier_apply_to_matches_apply ---");
    Style s = Style{}.with_bold().with_fg(Color::green());
    std::string expected = ansi::StyleApplier::apply(s);
    std::string got;
    ansi::StyleApplier::apply_to(s, got);
    assert(got == expected);
    std::println("PASS\n");
}

TEST_CASE("style applier transition to matches transition") {
    std::println("--- test_style_applier_transition_to_matches_transition ---");
    Style a = Style{}.with_bold();
    Style b = Style{}.with_bold().with_fg(Color::blue());
    std::string expected = ansi::StyleApplier::transition(a, b);
    std::string got;
    ansi::StyleApplier::transition_to(a, b, got);
    assert(got == expected);
    std::println("PASS\n");
}

TEST_CASE("style applier apply to appends") {
    std::println("--- test_style_applier_apply_to_appends ---");
    Style s = Style{}.with_bold();
    std::string out = "START";
    ansi::StyleApplier::apply_to(s, out);
    assert(out.starts_with("START"));
    assert(out.size() > 5);
    std::println("PASS\n");
}

TEST_CASE("style applier transition to empty on same") {
    std::println("--- test_style_applier_transition_to_empty_on_same ---");
    Style s = Style{}.with_italic();
    std::string out;
    ansi::StyleApplier::transition_to(s, s, out);
    assert(out.empty());
    std::println("PASS\n");
}

// ============================================================================
// env_supports_synchronized_output — multiplexer (tmux/screen) robustness
// ============================================================================
// Under tmux ALL of the outer terminal's identifying env vars are stripped;
// the detector must lean on what SURVIVES the pane: tmux's own version
// (TERM_PROGRAM=tmux + TERM_PROGRAM_VERSION) and COLORTERM. A modern tmux
// (≥ 3.4) forwards synchronized output, so it must NOT be blanket-disabled.
TEST_CASE("sync detect tmux") {
    std::println("--- test_sync_detect_tmux ---");

    // Wipe every var the detector consults so each case is hermetic.
    auto clear_env = [] {
        for (const char* v : {"MAYA_FORCE_SYNC", "MAYA_NO_SYNC", "TERM_PROGRAM",
                              "TERM_PROGRAM_VERSION", "KITTY_WINDOW_ID",
                              "ALACRITTY_LOG", "ALACRITTY_WINDOW_ID",
                              "GHOSTTY_RESOURCES_DIR", "WEZTERM_EXECUTABLE",
                              "WT_SESSION", "KONSOLE_VERSION", "VTE_VERSION",
                              "TERM", "COLORTERM", "TMUX",
                              "MAYA_TMUX_FEATURES", "MAYA_TMUX_PASSTHROUGH",
                              "MAYA_TMUX_GET_CLIPBOARD"})
            unset_env(v);
        // Inside tmux the sync answer now comes from maya::tmux, which
        // probes the LIVE server. Force the fabricated-env path so these
        // cases never depend on the developer's own tmux config, and drop
        // the memoised probe between cases.
        set_env("MAYA_TMUX_FAKE", "1", 1);
        maya::tmux::reset_cache_for_test();
    };

    // Modern tmux (3.4+) reporting its own version → sync supported.
    clear_env();
    set_env("TMUX", "/tmp/tmux-1000/default,1,0", 1);
    set_env("TERM", "tmux-256color", 1);
    set_env("TERM_PROGRAM", "tmux", 1);
    set_env("TERM_PROGRAM_VERSION", "3.7b", 1);
    set_env("MAYA_TMUX_PASSTHROUGH", "1", 1);
    maya::tmux::reset_cache_for_test();
    assert(ansi::env_supports_synchronized_output()
           && "tmux 3.7 with passthrough must report sync-capable");

    // tmux 3.4 exactly is the cutoff → supported.
    set_env("TERM_PROGRAM_VERSION", "3.4", 1);
    assert(ansi::env_supports_synchronized_output()
           && "tmux 3.4 with passthrough must report sync-capable");

    // Old tmux (3.2) with no truecolor signal → not supported.
    clear_env();
    set_env("TMUX", "/tmp/tmux-1000/default,1,0", 1);
    set_env("TERM", "tmux-256color", 1);
    set_env("TERM_PROGRAM", "tmux", 1);
    set_env("TERM_PROGRAM_VERSION", "3.2a", 1);
    // NEW CONTRACT: inside tmux the version is irrelevant — what matters
    // is whether tmux will FORWARD the markers (allow-passthrough). A raw
    // CSI ?2026 is swallowed by tmux regardless of its version, so
    // "passthrough off" is the only correct "no sync" answer.
    assert(!ansi::env_supports_synchronized_output()
           && "tmux without passthrough must NOT report sync-capable");
    set_env("MAYA_TMUX_PASSTHROUGH", "1", 1);
    maya::tmux::reset_cache_for_test();
    assert(ansi::env_supports_synchronized_output()
           && "tmux WITH passthrough must report sync-capable");
    unset_env("MAYA_TMUX_PASSTHROUGH");
    maya::tmux::reset_cache_for_test();

    // Old/unknown-version multiplexer but COLORTERM=truecolor survives
    // (modern outer terminal) → treat as supported.
    clear_env();
    set_env("TMUX", "/tmp/tmux-1000/default,1,0", 1);
    set_env("TERM", "screen-256color", 1);
    set_env("COLORTERM", "truecolor", 1);
    // Under the new contract COLORTERM can't rescue a multiplexer that
    // won't forward the markers — passthrough is the deciding fact.
    set_env("MAYA_TMUX_PASSTHROUGH", "1", 1);
    maya::tmux::reset_cache_for_test();
    assert(ansi::env_supports_synchronized_output()
           && "multiplexer WITH passthrough must report sync-capable");

    // GNU screen, no version, no truecolor → not supported (screen never
    // forwards 2026).
    clear_env();
    set_env("TERM", "screen", 1);
    assert(!ansi::env_supports_synchronized_output()
           && "bare GNU screen must NOT report sync-capable");

    // Explicit override still wins inside a multiplexer.
    clear_env();
    set_env("TMUX", "/tmp/tmux-1000/default,1,0", 1);
    set_env("TERM", "tmux-256color", 1);
    set_env("TERM_PROGRAM", "tmux", 1);
    set_env("TERM_PROGRAM_VERSION", "3.7b", 1);
    set_env("MAYA_NO_SYNC", "1", 1);
    assert(!ansi::env_supports_synchronized_output()
           && "MAYA_NO_SYNC must override even a sync-capable tmux");
    unset_env("MAYA_NO_SYNC");

    clear_env();
    std::println("PASS\n");
}

// ============================================================================
// tmux clipboard passthrough (image paste under tmux)
// ============================================================================

TEST_CASE("wrap_for_tmux wraps only inside tmux, doubling ESCs") {
    std::println("--- test_wrap_for_tmux ---");

    // Not in tmux: sequence returned unchanged.
    //
    // Clearing $TMUX alone is NOT enough to be "outside tmux", and this
    // assertion failed for anyone running the suite from inside tmux:
    // presence is also inferred from a tmux-*/screen-* $TERM (the
    // ssh-from-local-tmux topology), which survives unsetenv("TMUX") as
    // "tmux-256color". The probe also memoises its answer, so a stale
    // verdict outlives the env change. Clear BOTH signals and reset the
    // cache so the case tests the branch it names on every host.
    unset_env("TMUX");
    set_env("TERM", "xterm-256color", 1);
    unset_env("MAYA_TMUX_FAKE");
    tmux::reset_cache_for_test();
    const std::string seq = "\x1b]5522;type=read;Zm9v\x1b\\";
    assert(ansi::wrap_for_tmux(seq) == seq
           && "outside tmux wrap_for_tmux must be a no-op");

    // In tmux: wrapped in ESC P tmux ; … ESC \ with every inner ESC DOUBLED.
    set_env("TMUX", "/tmp/tmux-1000/default,1,0", 1);
    tmux::reset_cache_for_test();
    const std::string wrapped = ansi::wrap_for_tmux(seq);
    // Must start with the DCS passthrough intro and end with ST.
    assert(wrapped.rfind("\x1bPtmux;", 0) == 0
           && "tmux wrap must start with ESC P tmux ;");
    assert(wrapped.size() >= 2
           && wrapped[wrapped.size() - 2] == '\x1b'
           && wrapped[wrapped.size() - 1] == '\\'
           && "tmux wrap must end with ST (ESC \\)");
    // Every ESC from the ORIGINAL payload is doubled. The original seq has 2
    // ESCs (the OSC intro and its ST); the wrapper adds its own intro ESC and
    // a trailing ST ESC. Count ESCs: wrapper_intro(1) + doubled_payload(2*2=4)
    // + trailing_ST(1) = 6.
    std::size_t escs = 0;
    for (char c : wrapped) if (c == '\x1b') ++escs;
    assert(escs == 6 && "payload ESCs must be doubled inside the tmux wrapper");

    unset_env("TMUX");
    std::println("PASS\n");
}

TEST_CASE("env_supports_osc5522 is speculative inside tmux") {
    std::println("--- test_osc5522_tmux ---");
    auto clear = [] {
        for (const char* v : {"KITTY_WINDOW_ID", "TERM", "TMUX"})
            unset_env(v);
    };

    // kitty locally.
    clear();
    set_env("KITTY_WINDOW_ID", "1", 1);
    assert(ansi::env_supports_osc5522() && "KITTY_WINDOW_ID -> true");

    // kitty over ssh (only TERM survives).
    clear();
    set_env("TERM", "xterm-kitty", 1);
    assert(ansi::env_supports_osc5522() && "TERM=xterm-kitty -> true");

    // Plain non-kitty, no tmux: false.
    clear();
    set_env("TERM", "xterm-256color", 1);
    assert(!ansi::env_supports_osc5522() && "plain xterm -> false");

    // Inside tmux, kitty's fingerprints are erased (TERM rewritten, no
    // KITTY_WINDOW_ID) — we return true SPECULATIVELY so the tmux-wrapped
    // request gets sent and kitty (if outer) can answer with an image.
    clear();
    set_env("TERM", "tmux-256color", 1);
    set_env("TMUX", "/tmp/tmux-1000/default,1,0", 1);
    assert(ansi::env_supports_osc5522()
           && "inside tmux env_supports_osc5522 must be speculatively true");

    clear();
    std::println("PASS\n");
}

