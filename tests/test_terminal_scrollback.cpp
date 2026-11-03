// test_terminal_scrollback — detect_scrollback against fake environments.
//
// One case per terminal: identify it from the env it really sets, read its
// setting from a fake config file, fall back to its documented default when
// the file says nothing, and let a multiplexer's own scrollback win.

#include <maya/terminal/scrollback.hpp>

#include <chrono>
#include <cstdio>
#include <map>
#include <string>

using maya::terminal::Scrollback;
using maya::terminal::ScrollbackInputs;

namespace {

int failures = 0;
void expect(bool ok, const std::string& what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++failures;
}

struct Fake {
    std::map<std::string, std::string> env, files, answers;
    int tmux = 0;
    bool read_config = true;

    Scrollback run() const {
        ScrollbackInputs in;
        in.env = [this](const char* k) { auto it = env.find(k); return it == env.end() ? std::string{} : it->second; };
        in.read_file = [this](const std::string& p) { auto it = files.find(p); return it == files.end() ? std::string{} : it->second; };
        in.ask = [this](const char* q) { auto it = answers.find(q); return it == answers.end() ? std::string{} : it->second; };
        in.tmux_history = tmux;
        in.read_config = read_config;
        return maya::terminal::detect_scrollback(in);
    }
};

Fake base() { Fake f; f.env["HOME"] = "/h"; return f; }

void check(const char* name, const Fake& f, int rows, bool exact, bool unlimited = false) {
    const auto s = f.run();
    const bool ok = s.rows == rows && s.exact == exact && s.unlimited == unlimited;
    expect(ok, std::string{name} + " -> rows=" + std::to_string(s.rows) + (s.unlimited ? " unlimited" : "")
                   + (s.exact ? " exact" : " default") + " (" + s.source + ")");
}

}  // namespace

int main() {
    std::printf("--- multiplexers win ---\n");
    { auto f = base(); f.env["TMUX"] = "x"; f.env["KITTY_WINDOW_ID"] = "1"; f.tmux = 100000;
      check("tmux history-limit", f, 100000, true); }
    { auto f = base(); f.env["TMUX"] = "x"; check("tmux, probe failed", f, 2000, false); }
    { auto f = base(); f.env["ZELLIJ"] = "0"; f.files["/h/.config/zellij/config.kdl"] = "// x\nscroll_buffer_size 50000\n";
      check("zellij config", f, 50000, true); }
    { auto f = base(); f.env["ZELLIJ"] = "0"; check("zellij default", f, 10000, false); }
    { auto f = base(); f.env["STY"] = "1.pts"; f.files["/h/.screenrc"] = "defscrollback 5000\n";
      check("screen .screenrc", f, 5000, true); }
    { auto f = base(); f.env["STY"] = "1.pts"; check("screen default", f, 100, false); }

    std::printf("--- terminals ---\n");
    { auto f = base(); f.env["KITTY_WINDOW_ID"] = "1";
      f.files["/h/.config/kitty/kitty.conf"] = "# scrollback_lines 99\nscrollback_lines 2000\nscrollback_lines  8000 # last wins\n";
      check("kitty.conf", f, 8000, true); }
    { auto f = base(); f.env["KITTY_WINDOW_ID"] = "1";
      f.files["/h/.config/kitty/kitty.conf"] = "include extra.conf\n";
      f.files["/h/.config/kitty/extra.conf"] = "scrollback_lines 12345\n";
      check("kitty include", f, 12345, true); }
    { auto f = base(); f.env["KITTY_WINDOW_ID"] = "1";
      f.files["/h/.config/kitty/kitty.conf"] = "scrollback_lines -1\n";
      check("kitty unlimited", f, 0, true, true); }
    { auto f = base(); f.env["TERM"] = "xterm-kitty"; check("kitty default", f, 2000, false); }
    { auto f = base(); f.env["TERM_PROGRAM"] = "WezTerm"; f.env["WEZTERM_CONFIG_FILE"] = "/h/.wezterm.lua";
      f.files["/h/.wezterm.lua"] = "local c = {}\n-- c.scrollback_lines = 1\nc.scrollback_lines = 20000\nreturn c\n";
      check("wezterm.lua", f, 20000, true); }
    { auto f = base(); f.env["TERM_PROGRAM"] = "WezTerm"; check("wezterm default", f, 3500, false); }
    { auto f = base(); f.env["ALACRITTY_WINDOW_ID"] = "9";
      f.files["/h/.config/alacritty/alacritty.toml"] = "[window]\nhistory = 1\n[scrolling]\nhistory = 50000 # x\n";
      check("alacritty.toml", f, 50000, true); }
    { auto f = base(); f.env["TERM"] = "alacritty"; check("alacritty default", f, 10000, false); }
    { auto f = base(); f.env["TERM"] = "foot";
      f.files["/h/.config/foot/foot.ini"] = "lines=1\n[scrollback]\nlines=3000\n";
      check("foot.ini", f, 3000, true); }
    { auto f = base(); f.env["TERM"] = "foot-extra"; check("foot default", f, 1000, false); }
    { auto f = base(); f.env["TERM_PROGRAM"] = "ghostty";
      f.files["/h/.config/ghostty/config"] = "scrollback-limit-lines = 7000\n";
      check("ghostty lines", f, 7000, true); }
    { auto f = base(); f.env["TERM_PROGRAM"] = "ghostty"; check("ghostty default", f, 10000, false); }
    { auto f = base(); f.env["KONSOLE_VERSION"] = "260802";
      f.files["/h/.config/konsolerc"] = "[Desktop Entry]\nDefaultProfile=Mine.profile\n";
      f.files["/h/.local/share/konsole/Mine.profile"] = "[Scrolling]\nHistoryMode=1\nHistorySize=4242\n";
      check("konsole profile", f, 4242, true); }
    { auto f = base(); f.env["KONSOLE_VERSION"] = "260802";
      f.files["/h/.config/konsolerc"] = "[Desktop Entry]\nDefaultProfile=Mine.profile\n";
      f.files["/h/.local/share/konsole/Mine.profile"] = "[Scrolling]\nHistoryMode=2\n";
      check("konsole unlimited", f, 0, true, true); }
    { auto f = base(); f.env["KONSOLE_VERSION"] = "260802"; check("konsole default", f, 10000, false); }
    { auto f = base(); f.env["VTE_VERSION"] = "8401"; f.env["XDG_CURRENT_DESKTOP"] = "XFCE";
      f.files["/h/.config/xfce4/terminal/terminalrc"] = "[Configuration]\nScrollingLines=25000\n";
      check("xfce4 terminalrc", f, 25000, true); }
    { auto f = base(); f.env["VTE_VERSION"] = "8401"; f.env["XDG_CURRENT_DESKTOP"] = "XFCE";
      check("xfce4 default", f, 1000, false); }
    { auto f = base(); f.env["VTE_VERSION"] = "8401";
      f.answers["gsettings-gnome-default-profile"] = "'b1dcc9dd-5262-4d8d-a863-c897e6d979b9'\n";
      f.answers["gsettings-gnome-unlimited:b1dcc9dd-5262-4d8d-a863-c897e6d979b9"] = "false\n";
      f.answers["gsettings-gnome-lines:b1dcc9dd-5262-4d8d-a863-c897e6d979b9"] = "30000\n";
      check("gnome-terminal gsettings", f, 30000, true); }
    { auto f = base(); f.env["VTE_VERSION"] = "8401"; check("VTE default", f, 10000, false); }
    { auto f = base(); f.env["WT_SESSION"] = "x"; f.env["LOCALAPPDATA"] = "C:/L";
      f.files["C:/L/Packages/Microsoft.WindowsTerminal_8wekyb3d8bbwe/LocalState/settings.json"] =
          R"({"profiles": {"defaults": {"historySize": 20000}, "list": [{"historySize": 100}]}})";
      check("Windows Terminal defaults", f, 20000, true); }
    { auto f = base(); f.env["WT_SESSION"] = "x"; check("Windows Terminal default", f, 9001, false); }
    { auto f = base(); f.env["TERM_PROGRAM"] = "iTerm.app";
      f.answers["iterm-plist-json"] =
          R"([{"Guid":"AAA","Scrollback Lines":1000,"Unlimited Scrollback":false},)"
          R"({"Guid":"BBB","Name":"Work","Scrollback Lines":10000,"Unlimited Scrollback":false}])";
      f.answers["iterm-default-guid"] = "BBB\n";
      check("iTerm2 default profile by Guid", f, 10000, true); }
    { auto f = base(); f.env["TERM_PROGRAM"] = "iTerm.app";
      f.answers["iterm-plist-json"] = R"([{"Guid":"AAA","Scrollback Lines":2500}])";
      check("iTerm2 first profile", f, 2500, true); }
    { auto f = base(); f.env["TERM_PROGRAM"] = "iTerm.app";
      f.answers["iterm-plist-json"] = R"([{"Guid":"AAA","Unlimited Scrollback":true,"Scrollback Lines":1000}])";
      check("iTerm2 unlimited", f, 0, true, true); }
    { auto f = base(); f.env["TERM_PROGRAM"] = "iTerm.app"; check("iTerm2 default", f, 1000, false); }
    { auto f = base(); f.env["TERM_PROGRAM"] = "Apple_Terminal"; check("Terminal.app", f, 0, false, true); }
    { auto f = base(); f.env["TERM_PROGRAM"] = "vscode"; check("VS Code default", f, 1000, false); }

    std::printf("--- fast path reads no config ---\n");
    { auto f = base(); f.env["KITTY_WINDOW_ID"] = "1"; f.read_config = false;
      f.files["/h/.config/kitty/kitty.conf"] = "scrollback_lines 8000\n";
      check("kitty, fast", f, 2000, false); }
    { auto f = base(); f.read_config = false; check("unknown terminal", f, 0, false); }

    std::printf("--- the fast path is cheap ---\n");
    {
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 1000; ++i) (void)maya::terminal::scrollback_fast();
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / 1000;
        expect(us < 50, "scrollback_fast() " + std::to_string(us) + " us per call");
    }

    if (failures) { std::printf("%d FAILED\n", failures); return 1; }
    std::printf("ALL TERMINAL SCROLLBACK TESTS PASSED\n");
    return 0;
}
