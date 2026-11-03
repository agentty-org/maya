// src/terminal/scrollback.cpp — see include/maya/terminal/scrollback.hpp.
//
// Defaults and config keys are taken from each project's source:
//   tmux history-limit (live) · zellij scroll_buffer_size 10000 ·
//   screen defscrollback 100 · kitty scrollback_lines 2000 (negative =
//   unlimited) · WezTerm scrollback_lines 3500 · Alacritty
//   [scrolling] history 10000 · foot [scrollback] lines 1000 · Ghostty
//   scrollback-limit-lines (else a 50 MB byte cap) · Konsole HistoryMode /
//   HistorySize 10000 · GNOME Terminal (VTE) scrollback-lines 10000 ·
//   xfce4-terminal ScrollingLines 1000 · Windows Terminal historySize 9001 ·
//   VS Code terminal.integrated.scrollback 1000 · iTerm2 "Scrollback Lines"
//   1000 · Terminal.app unlimited.

#include "maya/terminal/scrollback.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <string_view>

#include "maya/terminal/tmux.hpp"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <unistd.h>
#endif

namespace maya::terminal {

namespace {

using sv = std::string_view;

std::string trim(sv s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))  s.remove_suffix(1);
    return std::string{s};
}

// Leading integer of `s` (after optional spaces / sign); nullopt if none.
std::optional<long long> leading_int(sv s) {
    std::size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    bool neg = false;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) neg = s[i++] == '-';
    std::size_t start = i;
    long long v = 0;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])) && i - start < 18)
        v = v * 10 + (s[i++] - '0');
    if (i == start) return std::nullopt;
    return neg ? -v : v;
}

// Each line of `text`, trimmed, with `comment` (and everything after it on
// that line) removed. Keeps going over the whole file, so the LAST setting
// wins, as it does in every one of these formats.
template <class F>
void each_line(const std::string& text, char comment, F&& f) {
    std::size_t pos = 0;
    while (pos <= text.size()) {
        std::size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        sv line{text.data() + pos, nl - pos};
        if (comment)
            if (auto c = line.find(comment); c != sv::npos) line = line.substr(0, c);
        const std::string t = trim(line);
        if (!t.empty()) f(t);
        pos = nl + 1;
    }
}

struct Ctx {
    const ScrollbackInputs& in;
    std::string env(const char* k) const { return in.env ? in.env(k) : std::string{}; }
    std::string file(const std::string& p) const { return in.read_file ? in.read_file(p) : std::string{}; }
    std::string ask(const char* q) const { return in.ask ? in.ask(q) : std::string{}; }
    std::string home() const {
        auto h = env("HOME");
        if (h.empty()) h = env("USERPROFILE");
        return h;
    }
    std::string xdg_config() const {
        auto x = env("XDG_CONFIG_HOME");
        return x.empty() ? home() + "/.config" : x;
    }
};

Scrollback known(int rows, std::string source) {
    Scrollback s; s.rows = rows; s.exact = true; s.source = std::move(source); return s;
}
Scrollback fallback(int rows, std::string source) {
    Scrollback s; s.rows = rows; s.exact = false; s.source = std::move(source); return s;
}
Scrollback unlimited(std::string source, bool exact) {
    Scrollback s; s.unlimited = true; s.exact = exact; s.source = std::move(source); return s;
}

// ── per-terminal config readers ─────────────────────────────────────────
// Each returns nullopt when the file says nothing, so the caller falls
// back to the documented default.

// kitty.conf: `scrollback_lines N`; negative = unlimited. Follows `include`
// one level (kitty allows nesting; one level covers the usual split).
std::optional<Scrollback> kitty(const Ctx& c) {
    std::string dir = c.env("KITTY_CONFIG_DIRECTORY");
    if (dir.empty()) dir = c.xdg_config() + "/kitty";
    std::optional<Scrollback> got;
    auto scan = [&](const std::string& text, auto& self, int depth) -> void {
        each_line(text, '#', [&](const std::string& l) {
            if (l.rfind("scrollback_lines", 0) == 0 && l.size() > 16
                && std::isspace(static_cast<unsigned char>(l[16]))) {
                if (auto v = leading_int(sv{l}.substr(16))) {
                    got = *v < 0 ? unlimited("kitty.conf", true)
                                 : known(static_cast<int>(std::min<long long>(*v, 1 << 30)), "kitty.conf");
                }
            } else if (depth == 0 && l.rfind("include", 0) == 0 && l.size() > 8) {
                std::string p = trim(sv{l}.substr(7));
                if (!p.empty() && p[0] != '/') p = dir + "/" + p;
                self(c.file(p), self, depth + 1);
            }
        });
    };
    scan(c.file(dir + "/kitty.conf"), scan, 0);
    return got;
}

// wezterm.lua: `scrollback_lines = N` anywhere (the usual `config.x = N`
// or a table field). Lua can compute it; a literal is the common case.
std::optional<Scrollback> wezterm(const Ctx& c) {
    std::string path = c.env("WEZTERM_CONFIG_FILE");
    std::string text = path.empty() ? std::string{} : c.file(path);
    if (text.empty()) text = c.file(c.xdg_config() + "/wezterm/wezterm.lua");
    if (text.empty()) text = c.file(c.home() + "/.wezterm.lua");
    std::optional<Scrollback> got;
    each_line(text, '\0', [&](const std::string& l) {
        if (l.rfind("--", 0) == 0) return;
        auto k = l.find("scrollback_lines");
        if (k == std::string::npos) return;
        auto eq = l.find('=', k);
        if (eq == std::string::npos) return;
        if (auto v = leading_int(sv{l}.substr(eq + 1)); v && *v > 0)
            got = known(static_cast<int>(std::min<long long>(*v, 1 << 30)), "wezterm.lua");
    });
    return got;
}

// alacritty.toml: `history = N` inside [scrolling], or `scrolling.history = N`.
std::optional<Scrollback> alacritty(const Ctx& c) {
    std::string text = c.file(c.xdg_config() + "/alacritty/alacritty.toml");
    if (text.empty()) text = c.file(c.xdg_config() + "/alacritty.toml");
    if (text.empty()) text = c.file(c.home() + "/.alacritty.toml");
    if (text.empty()) text = c.file(c.env("APPDATA") + "/alacritty/alacritty.toml");
    std::optional<Scrollback> got;
    std::string section;
    each_line(text, '#', [&](const std::string& l) {
        if (l.front() == '[') { section = trim(sv{l}.substr(1, l.find(']') - 1)); return; }
        auto eq = l.find('=');
        if (eq == std::string::npos) return;
        const std::string key = trim(sv{l}.substr(0, eq));
        const bool hit = (section == "scrolling" && key == "history") || key == "scrolling.history";
        if (hit)
            if (auto v = leading_int(sv{l}.substr(eq + 1)); v && *v >= 0)
                got = known(static_cast<int>(*v), "alacritty.toml");
    });
    return got;
}

// foot.ini: `lines=N` under [scrollback].
std::optional<Scrollback> foot(const Ctx& c) {
    std::string text = c.file(c.xdg_config() + "/foot/foot.ini");
    std::optional<Scrollback> got;
    std::string section = "main";
    each_line(text, '#', [&](const std::string& l) {
        if (l.front() == '[') { section = trim(sv{l}.substr(1, l.find(']') - 1)); return; }
        auto eq = l.find('=');
        if (eq == std::string::npos || section != "scrollback") return;
        if (trim(sv{l}.substr(0, eq)) == "lines")
            if (auto v = leading_int(sv{l}.substr(eq + 1)); v && *v >= 0)
                got = known(static_cast<int>(*v), "foot.ini");
    });
    return got;
}

// Ghostty: `scrollback-limit-lines = N` (1.4+). Older configs only cap
// bytes, which can't be turned into rows, so those read as a default.
std::optional<Scrollback> ghostty(const Ctx& c) {
    std::string text = c.file(c.xdg_config() + "/ghostty/config");
    if (text.empty())
        text = c.file(c.home() + "/Library/Application Support/com.mitchellh.ghostty/config");
    std::optional<Scrollback> got;
    each_line(text, '#', [&](const std::string& l) {
        auto eq = l.find('=');
        if (eq == std::string::npos) return;
        if (trim(sv{l}.substr(0, eq)) != "scrollback-limit-lines") return;
        const std::string v = trim(sv{l}.substr(eq + 1));
        if (v == "unlimited") got = unlimited("ghostty config", true);
        else if (auto n = leading_int(v); n && *n >= 0)
            got = known(static_cast<int>(std::min<long long>(*n, 1 << 30)), "ghostty config");
    });
    return got;
}

// Konsole: konsolerc names the default profile; the profile's [Scrolling]
// section has HistoryMode (0 none, 1 fixed, 2 unlimited) and HistorySize.
std::optional<Scrollback> konsole(const Ctx& c) {
    std::string profile;
    std::string section;
    each_line(c.file(c.xdg_config() + "/konsolerc"), '#', [&](const std::string& l) {
        if (l.front() == '[') { section = l; return; }
        if (section == "[Desktop Entry]" && l.rfind("DefaultProfile=", 0) == 0)
            profile = l.substr(15);
    });
    if (profile.empty()) return std::nullopt;
    std::string data = c.env("XDG_DATA_HOME");
    if (data.empty()) data = c.home() + "/.local/share";
    const std::string text = c.file(data + "/konsole/" + profile);
    int mode = 1;
    std::optional<long long> size;
    section.clear();
    each_line(text, '#', [&](const std::string& l) {
        if (l.front() == '[') { section = l; return; }
        if (section != "[Scrolling]") return;
        if (l.rfind("HistoryMode=", 0) == 0) mode = static_cast<int>(leading_int(sv{l}.substr(12)).value_or(1));
        if (l.rfind("HistorySize=", 0) == 0) size = leading_int(sv{l}.substr(12));
    });
    if (mode == 2) return unlimited("konsole profile", true);
    if (mode == 0) return known(0, "konsole profile");
    if (size && *size >= 0) return known(static_cast<int>(*size), "konsole profile");
    return std::nullopt;
}

// xfce4-terminal: ~/.config/xfce4/terminal/terminalrc ScrollingLines /
// ScrollingUnlimited. It's a VTE terminal, so check it before gsettings.
std::optional<Scrollback> xfce(const Ctx& c) {
    const std::string text = c.file(c.xdg_config() + "/xfce4/terminal/terminalrc");
    if (text.empty()) return std::nullopt;
    std::optional<Scrollback> got;
    bool unl = false;
    each_line(text, '#', [&](const std::string& l) {
        if (l.rfind("ScrollingUnlimited=", 0) == 0) unl = l.substr(19) == "TRUE";
        if (l.rfind("ScrollingLines=", 0) == 0)
            if (auto v = leading_int(sv{l}.substr(15)); v && *v >= 0)
                got = known(static_cast<int>(*v), "xfce4 terminalrc");
    });
    if (unl) return unlimited("xfce4 terminalrc", true);
    // The file exists but doesn't override it: xfce4-terminal's own default.
    return got ? got : std::optional<Scrollback>{fallback(1000, "xfce4-terminal default")};
}

// GNOME Terminal (and anything else on its gsettings schema): ask gsettings
// for the default profile. Ptyxis keeps its own schema; it falls through to
// the VTE default.
std::optional<Scrollback> gnome(const Ctx& c) {
    std::string id = trim(c.ask("gsettings-gnome-default-profile"));
    if (id.size() >= 2 && id.front() == '\'') id = id.substr(1, id.size() - 2);
    if (id.empty()) return std::nullopt;
    const std::string unl = trim(c.ask(("gsettings-gnome-unlimited:" + id).c_str()));
    if (unl == "true") return unlimited("gnome-terminal profile", true);
    if (auto v = leading_int(trim(c.ask(("gsettings-gnome-lines:" + id).c_str()))); v && *v >= 0)
        return known(static_cast<int>(*v), "gnome-terminal profile");
    return std::nullopt;
}

// Windows Terminal: settings.json, `"historySize": N` in profiles.defaults
// (or a profile). Without parsing which profile we're in, take the
// defaults value, else the first one set.
std::optional<Scrollback> windows_terminal(const Ctx& c) {
    const std::string local = c.env("LOCALAPPDATA");
    if (local.empty()) return std::nullopt;
    std::string text = c.file(local + "/Packages/Microsoft.WindowsTerminal_8wekyb3d8bbwe/LocalState/settings.json");
    if (text.empty()) text = c.file(local + "/Packages/Microsoft.WindowsTerminalPreview_8wekyb3d8bbwe/LocalState/settings.json");
    if (text.empty()) text = c.file(local + "/Microsoft/Windows Terminal/settings.json");
    if (text.empty()) return std::nullopt;
    std::optional<Scrollback> first, in_defaults;
    const auto defaults_at = text.find("\"defaults\"");
    std::size_t pos = 0;
    while ((pos = text.find("\"historySize\"", pos)) != std::string::npos) {
        const auto colon = text.find(':', pos);
        if (colon == std::string::npos) break;
        if (auto v = leading_int(sv{text}.substr(colon + 1)); v && *v >= 0) {
            auto s = known(static_cast<int>(std::min<long long>(*v, 32767)), "Windows Terminal settings");
            if (!first) first = s;
            if (defaults_at != std::string::npos && pos > defaults_at && !in_defaults) in_defaults = s;
        }
        pos = colon;
    }
    return in_defaults ? in_defaults : first;
}

// VS Code: "terminal.integrated.scrollback" in the user settings.json.
std::optional<Scrollback> vscode(const Ctx& c) {
    std::string base;
#if defined(_WIN32)
    base = c.env("APPDATA") + "/Code/User/settings.json";
#elif defined(__APPLE__)
    base = c.home() + "/Library/Application Support/Code/User/settings.json";
#else
    base = c.xdg_config() + "/Code/User/settings.json";
#endif
    const std::string text = c.file(base);
    const auto k = text.find("\"terminal.integrated.scrollback\"");
    if (k == std::string::npos) return std::nullopt;
    const auto colon = text.find(':', k);
    if (colon == std::string::npos) return std::nullopt;
    if (auto v = leading_int(sv{text}.substr(colon + 1)); v && *v >= 0)
        return known(static_cast<int>(*v), "VS Code settings");
    return std::nullopt;
}

// iTerm2: the default profile's "Scrollback Lines" / "Unlimited Scrollback".
// `ask("iterm-plist-json")` is the profiles array as JSON (plutil), and
// `ask("iterm-default-guid")` the default profile's Guid. Without a Guid,
// the first profile is the default.
std::optional<Scrollback> iterm(const Ctx& c) {
    const std::string json = c.ask("iterm-plist-json");
    if (json.empty()) return std::nullopt;
    const std::string guid = trim(c.ask("iterm-default-guid"));
    // Find the profile object: the one containing the Guid, else the first.
    std::size_t at = 0;
    if (!guid.empty()) {
        const auto g = json.find("\"" + guid + "\"");
        if (g != std::string::npos) {
            const auto open = json.rfind("{", g);
            if (open != std::string::npos) at = open;
        }
    }
    // The object ends at the next top-level `},{` or `}]`; keys are looked
    // up only inside it.
    auto end = json.find("},{", at);
    if (end == std::string::npos) end = json.size();
    const sv obj{json.data() + at, end - at};
    auto value_of = [&](sv key) -> std::string {
        const auto k = obj.find(key);
        if (k == sv::npos) return {};
        const auto colon = obj.find(':', k);
        if (colon == sv::npos) return {};
        auto v = obj.substr(colon + 1);
        const auto stop = v.find_first_of(",}");
        return trim(v.substr(0, stop));
    };
    const std::string unl = value_of("\"Unlimited Scrollback\"");
    if (unl == "true" || unl == "1") return unlimited("iTerm2 profile", true);
    if (auto v = leading_int(value_of("\"Scrollback Lines\"")); v && *v >= 0)
        return known(static_cast<int>(*v), "iTerm2 profile");
    return std::nullopt;
}

// zellij: `scroll_buffer_size N` in config.kdl.
std::optional<Scrollback> zellij(const Ctx& c) {
    std::string dir = c.env("ZELLIJ_CONFIG_DIR");
    if (dir.empty()) dir = c.xdg_config() + "/zellij";
    std::string path = c.env("ZELLIJ_CONFIG_FILE");
    const std::string text = c.file(path.empty() ? dir + "/config.kdl" : path);
    std::optional<Scrollback> got;
    each_line(text, '\0', [&](const std::string& l) {
        if (l.rfind("//", 0) == 0) return;
        if (l.rfind("scroll_buffer_size", 0) == 0)
            if (auto v = leading_int(sv{l}.substr(18)); v && *v >= 0)
                got = known(static_cast<int>(*v), "zellij config");
    });
    return got;
}

// GNU screen: `defscrollback N` in ~/.screenrc (or $SCREENRC).
std::optional<Scrollback> screen(const Ctx& c) {
    std::string path = c.env("SCREENRC");
    const std::string text = c.file(path.empty() ? c.home() + "/.screenrc" : path);
    std::optional<Scrollback> got;
    each_line(text, '#', [&](const std::string& l) {
        if (l.rfind("defscrollback", 0) == 0)
            if (auto v = leading_int(sv{l}.substr(13)); v && *v >= 0)
                got = known(static_cast<int>(*v), ".screenrc");
    });
    return got;
}

enum class Term {
    Unknown, Kitty, WezTerm, Alacritty, Foot, Ghostty, Konsole, Xfce, Vte,
    WindowsTerminal, VSCode, ITerm, AppleTerminal, WinConsole,
};

Term identify(const Ctx& c) {
    const std::string prog = c.env("TERM_PROGRAM");
    const std::string term = c.env("TERM");
    if (!c.env("KITTY_WINDOW_ID").empty() || term == "xterm-kitty") return Term::Kitty;
    if (prog == "WezTerm" || !c.env("WEZTERM_PANE").empty()) return Term::WezTerm;
    if (!c.env("ALACRITTY_WINDOW_ID").empty() || !c.env("ALACRITTY_SOCKET").empty()
        || term == "alacritty") return Term::Alacritty;
    if (term.rfind("foot", 0) == 0) return Term::Foot;
    if (prog == "ghostty" || !c.env("GHOSTTY_RESOURCES_DIR").empty() || term == "xterm-ghostty")
        return Term::Ghostty;
    if (!c.env("KONSOLE_VERSION").empty()) return Term::Konsole;
    if (!c.env("WT_SESSION").empty()) return Term::WindowsTerminal;
    if (prog == "vscode") return Term::VSCode;
    if (prog == "iTerm.app" || !c.env("ITERM_SESSION_ID").empty()) return Term::ITerm;
    if (prog == "Apple_Terminal") return Term::AppleTerminal;
    if (!c.env("VTE_VERSION").empty()) {
        // xfce4-terminal, GNOME Terminal and Ptyxis are all VTE and set the
        // same variables. The terminal is our parent (or grandparent, via a
        // shell); its name tells them apart. Desktop is the fallback hint.
        const std::string parent = c.ask("terminal-process");
        if (parent.find("xfce4-terminal") != std::string::npos) return Term::Xfce;
        if (parent.empty() && c.env("XDG_CURRENT_DESKTOP").find("XFCE") != std::string::npos)
            return Term::Xfce;
        return Term::Vte;
    }
#if defined(_WIN32)
    return Term::WinConsole;
#else
    return Term::Unknown;
#endif
}

}  // namespace

Scrollback detect_scrollback(const ScrollbackInputs& in) {
    const Ctx c{in};

    // 1. Multiplexers keep their own scrollback; that is what's scrolled.
    if (in.tmux_history > 0) return known(in.tmux_history, "tmux");
    if (!c.env("TMUX").empty()) return fallback(2000, "tmux default");
    if (!c.env("ZELLIJ").empty()) {
        if (in.read_config) if (auto z = zellij(c)) return *z;
        return fallback(10000, "zellij default");
    }
    if (!c.env("STY").empty()) {
        if (in.read_config) if (auto s = screen(c)) return *s;
        return fallback(100, "screen default");
    }

    // 2. The terminal: its config, else its documented default.
    auto pick = [&](std::optional<Scrollback> (*read)(const Ctx&), Scrollback def) {
        if (in.read_config) if (auto s = read(c)) return *s;
        return def;
    };
    switch (identify(c)) {
        case Term::Kitty:     return pick(kitty,     fallback(2000,  "kitty default"));
        case Term::WezTerm:   return pick(wezterm,   fallback(3500,  "WezTerm default"));
        case Term::Alacritty: return pick(alacritty, fallback(10000, "Alacritty default"));
        case Term::Foot:      return pick(foot,      fallback(1000,  "foot default"));
        case Term::Ghostty:   return pick(ghostty,   fallback(10000, "Ghostty (byte limit)"));
        case Term::Konsole:   return pick(konsole,   fallback(10000, "Konsole default"));
        case Term::Xfce:      return pick(xfce,      fallback(1000,  "xfce4-terminal default"));
        case Term::Vte:       return pick(gnome,     fallback(10000, "VTE default"));
        case Term::WindowsTerminal:
            return pick(windows_terminal, fallback(9001, "Windows Terminal default"));
        case Term::VSCode:    return pick(vscode,    fallback(1000,  "VS Code default"));
        case Term::ITerm:     return pick(iterm,     fallback(1000,  "iTerm2 default"));
        case Term::AppleTerminal: return unlimited("Terminal.app default", false);
        case Term::WinConsole: {
            // Classic conhost: the screen buffer height IS the scrollback.
            if (auto v = leading_int(c.ask("win-console-buffer-rows")); v && *v > 0)
                return known(static_cast<int>(*v), "console buffer");
            return fallback(9001, "Windows console");
        }
        case Term::Unknown: break;
    }
    return Scrollback{};   // unknown
}

namespace {

std::string real_env(const char* k) {
    const char* v = std::getenv(k);
    return v ? std::string{v} : std::string{};
}

std::string real_file(const std::string& p) {
    if (p.empty()) return {};
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    // Read in chunks: /proc files report size 0, and a config file past
    // 4 MB isn't one we want anyway.
    std::string s;
    char buf[8192];
    while (f.read(buf, sizeof buf) || f.gcount() > 0) {
        s.append(buf, static_cast<std::size_t>(f.gcount()));
        if (s.size() > (4u << 20)) return {};
    }
    return s;
}

#if !defined(_WIN32)
std::string run(const std::string& cmd) {
    std::FILE* p = ::popen((cmd + " 2>/dev/null").c_str(), "r");
    if (!p) return {};
    std::string out;
    std::array<char, 256> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), p)) out += buf.data();
    ::pclose(p);
    return out;
}
#endif

bool safe_id(sv s) {
    for (char ch : s)
        if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_')) return false;
    return !s.empty();
}

std::string real_ask(const char* what) {
    const sv w{what};
#if defined(__linux__)
    if (w == "terminal-process") {
        // Walk up from our parent; the first ancestor that isn't a shell or
        // launcher is the terminal. A few /proc reads, no spawn.
        int pid = static_cast<int>(::getppid());
        for (int hop = 0; hop < 6 && pid > 1; ++hop) {
            std::string comm = trim(real_file("/proc/" + std::to_string(pid) + "/comm"));
            if (comm.find("terminal") != std::string::npos || comm == "ptyxis-agent"
                || comm == "ptyxis" || comm == "gnome-terminal-")
                return comm;
            const std::string stat = real_file("/proc/" + std::to_string(pid) + "/stat");
            const auto rp = stat.rfind(')');
            if (rp == std::string::npos) break;
            std::istringstream rest(stat.substr(rp + 2));
            std::string state; int ppid = 0;
            rest >> state >> ppid;
            pid = ppid;
        }
        return {};
    }
#endif
#if defined(_WIN32)
    if (w == "win-console-buffer-rows") {
        CONSOLE_SCREEN_BUFFER_INFO info{};
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        if (h != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(h, &info))
            return std::to_string(info.dwSize.Y);
    }
    return {};
#else
    if (w == "gsettings-gnome-default-profile")
        return run("gsettings get org.gnome.Terminal.ProfilesList default");
    if (w.rfind("gsettings-gnome-", 0) == 0) {
        const auto colon = w.find(':');
        if (colon == sv::npos) return {};
        const sv key = w.substr(16, colon - 16);
        const sv id  = w.substr(colon + 1);
        if (!safe_id(id)) return {};
        const char* gkey = key == "lines" ? "scrollback-lines" : "scrollback-unlimited";
        return run("gsettings get org.gnome.Terminal.Legacy.Profile:/org/gnome/terminal/legacy/profiles:/:"
                   + std::string{id} + "/ " + gkey);
    }
#  if defined(__APPLE__)
    // iTerm2 keeps profiles in "New Bookmarks"; the default one is the entry
    // whose Guid matches "Default Bookmark Guid". One `plutil` call converts
    // the plist to JSON, parsed in iterm() below.
    if (w == "iterm-plist-json")
        return run("/usr/bin/plutil -extract 'New Bookmarks' json -o - "
                   "\"$HOME/Library/Preferences/com.googlecode.iterm2.plist\"");
    if (w == "iterm-default-guid")
        return run("/usr/bin/defaults read com.googlecode.iterm2 'Default Bookmark Guid'");
#  endif
    return {};
#endif
}

}  // namespace

Scrollback scrollback_fast() noexcept {
    try {
        ScrollbackInputs in;
        in.env = real_env;
        in.read_config = false;
        in.tmux_history = tmux::history_limit();   // already probed, cached
        return detect_scrollback(in);
    } catch (...) { return {}; }
}

Scrollback scrollback_full() noexcept {
    // A function-local static is initialised once, thread-safely, by the
    // language; later calls just read it.
    static const Scrollback cached = [] {
        try {
            ScrollbackInputs in;
            in.env = real_env;
            in.read_file = real_file;
            in.ask = real_ask;
            in.tmux_history = tmux::history_limit();
            return detect_scrollback(in);
        } catch (...) { return Scrollback{}; }
    }();
    return cached;
}

}  // namespace maya::terminal
