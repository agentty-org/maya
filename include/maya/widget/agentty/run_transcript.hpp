#pragma once
// maya::RunTranscript — what the terminal shows while a host runs a command
// on the real tty with the TUI suspended (agentty's Ctrl+G code blocks).
//
// There is no canvas here: maya is suspended, the tty is cooked, and the
// child writes straight to the terminal. So this widget does not build an
// Element; it hands back the exact BYTES for each moment of the run, and the
// host writes them. Every escape (colour, scroll margins, cursor, title) is
// decided here, so the host never spells one:
//
//   ╭─ running ─ Ctrl-C to stop ──────────────
//   $ npm test
//   … the child's own output, scrolling …
//   ⣷ running… 12s · npm · Ctrl-C to stop      ← pinned bottom row
//
//   ╰─ ✓ done  exit 0  ·  12s
//      press any key to return to agentty…
//
// `styled` is false for a pipe, NO_COLOR or TERM=dumb: then only the plain
// words are emitted (no colour, no title, no pinned row, no clears), so
// `agentty … | tee` stays clean and the captured output never sees a byte of
// chrome. `rows` is the terminal height; under 3 there is no pinned row.

#include <string>
#include <string_view>

namespace maya {

class RunTranscript {
public:
    enum class Outcome { Done, Failed, Stopped, TimedOut };

    struct Config {
        bool        styled = false;   // a colour tty; false = plain words only
        int         rows   = 0;       // terminal height; < 3 = no pinned row
        std::string app    = "agentty";
    };

    explicit RunTranscript(Config c) : cfg_(std::move(c)) {}

    [[nodiscard]] bool pinned() const noexcept { return cfg_.styled && cfg_.rows >= 3; }

    // A clean viewport, cursor home. Scrollback is left alone (ED2, not ED3):
    // the conversation the user scrolled through stays in native history.
    [[nodiscard]] std::string clear() const {
        return cfg_.styled ? std::string{"\x1b[2J\x1b[H"} : std::string{};
    }

    // The framed header: where the run starts and that Ctrl-C stops it, then
    // the command as a shell prompt so the transcript stays copy-faithful.
    [[nodiscard]] std::string header(std::string_view command) const {
        std::string s;
        s += sgr("2");
        s += "\xe2\x95\xad\xe2\x94\x80 running \xe2\x94\x80 Ctrl-C to stop ";   // ╭─ running ─ … 
        for (int i = 0; i < 50; ++i) s += "\xe2\x94\x80";                         // ─
        s += sgr("0");
        s += "\n";
        s += sgr("36") + "$ " + sgr("0");
        s += sgr("1");
        s += command;
        s += sgr("0");
        s += "\n";
        return s;
    }

    // The captured runner's header (no live tee: the output arrives in the
    // result card when the command finishes). Starts on a fresh line.
    [[nodiscard]] std::string captured_header(std::string_view command) const {
        std::string s = sgr("2");
        s += "\n\xe2\x95\xad\xe2\x94\x80 running \xe2\x94\x80 (output shown when it finishes) "
             "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80";
        s += sgr("0") + "\n" + sgr("36") + "$ " + sgr("0") + sgr("1");
        s += command;
        s += sgr("0") + "\n";
        return s;
    }

    // The captured runner's heartbeat: title plus one line redrawn in place
    // (no scroll margins; nothing else writes to the console meanwhile).
    [[nodiscard]] std::string inline_tick(long secs, std::string_view label, int frame) const {
        if (!cfg_.styled) return {};
        std::string s = title(secs, label);
        s += "\r\x1b[2K" + sgr("2");
        s += spin(frame);
        s += " running\xe2\x80\xa6 " + std::to_string(secs) + "s" + sgr("0");
        return s;
    }

    // The captured runner's teardown before its footer: wipe the heartbeat
    // line, restore the title.
    [[nodiscard]] std::string end_inline() const {
        return cfg_.styled ? std::string{"\r\x1b[2K\x1b]2;\x07"} : std::string{};
    }

    // Reserve the bottom row (DECSTBM margin 1..rows-1) and park the cursor
    // inside the region, so output scrolls above the status row.
    [[nodiscard]] std::string begin_status() const {
        if (!pinned()) return {};
        return "\x1b[1;" + std::to_string(cfg_.rows - 1) + "r"
             + "\x1b[" + std::to_string(cfg_.rows - 1) + ";1H";
    }

    // One heartbeat: the window title (OSC 2; some multiplexers drop it) and
    // the pinned row (universal), both showing the elapsed seconds.
    [[nodiscard]] std::string tick(long secs, std::string_view label, int frame) const {
        if (!cfg_.styled) return {};
        std::string s = title(secs, label);
        if (pinned()) {
            s += "\x1b[s";                                           // save cursor
            s += "\x1b[" + std::to_string(cfg_.rows) + ";1H\x1b[2K";  // status row, cleared
            s += sgr("2");
            s += spin(frame);
            s += " running\xe2\x80\xa6 " + std::to_string(secs) + "s \xc2\xb7 ";
            s += label;
            s += " \xc2\xb7 Ctrl-C to stop";
            s += sgr("0");
            s += "\x1b[u";                                           // back into the region
        }
        return s;
    }

    // Undo begin_status and every tick: margins reset, status row cleared,
    // title restored, cursor parked at the bottom of the old region.
    [[nodiscard]] std::string end_status() const {
        if (!cfg_.styled) return {};
        std::string s;
        if (pinned()) {
            s += "\x1b[r";
            s += "\x1b[" + std::to_string(cfg_.rows) + ";1H\x1b[2K";
            s += "\x1b[" + std::to_string(cfg_.rows - 1) + ";1H";
        }
        s += "\x1b]2;\x07";
        return s;
    }

    // The completion footer: outcome in its status colour, exit and time dim.
    // `fresh_line` = start with a newline (the live tee may end mid-line).
    [[nodiscard]] std::string footer(Outcome o, int exit_code, long secs, bool fresh_line = true) const {
        std::string s = fresh_line ? "\n" : "";
        switch (o) {
            case Outcome::Stopped: s += sgr("33") + "\xe2\x95\xb0\xe2\x94\x80 \xe2\x96\xa0 stopped"; break;  // ╰─ ■
            case Outcome::Done:    s += sgr("32") + "\xe2\x95\xb0\xe2\x94\x80 \xe2\x9c\x93 done";    break;  // ╰─ ✓
            case Outcome::Failed:  s += sgr("31") + "\xe2\x95\xb0\xe2\x94\x80 \xe2\x9c\x97 failed";  break;  // ╰─ ✗
            case Outcome::TimedOut: s += sgr("33") + "\xe2\x95\xb0\xe2\x94\x80 \xe2\x96\xa0 timed out"; break;
        }
        s += sgr("0");
        s += sgr("2");
        s += "  exit " + std::to_string(exit_code) + "  \xc2\xb7  " + std::to_string(secs) + "s";
        s += sgr("0");
        s += "\n";
        return s;
    }

    // The hold-the-screen prompt, and the line wipe once a key arrives.
    [[nodiscard]] std::string continue_prompt() const {
        return sgr("2") + "   press any key to return to " + cfg_.app + "\xe2\x80\xa6" + sgr("0");
    }
    [[nodiscard]] std::string clear_line() const {
        return cfg_.styled ? std::string{"\r\x1b[2K"} : std::string{};
    }

private:
    [[nodiscard]] static const char* spin(int frame) {
        static constexpr const char* kSpin[] = {"⣷","⣯","⣟","⡿","⣾","⣽","⣻","⣷"};
        return kSpin[static_cast<unsigned>(frame) % 8];
    }
    // OSC 2: "● 12s — npm — agentty".
    [[nodiscard]] std::string title(long secs, std::string_view label) const {
        std::string s = "\x1b]2;\xe2\x97\x8f " + std::to_string(secs) + "s \xe2\x80\x94 ";
        s += label;
        s += " \xe2\x80\x94 " + cfg_.app + "\x07";
        return s;
    }
    [[nodiscard]] std::string sgr(std::string_view code) const {
        if (!cfg_.styled) return {};
        return "\x1b[" + std::string{code} + "m";
    }

    Config cfg_;
};

} // namespace maya
