// ide.cpp — VS Code / Zed-inspired terminal IDE layout
//
// Model holds panel visibility, the active tab and the simulated build state;
// update() handles one message per key plus Tick, which advances the build
// (subscribed only while building); view() is a pure function of Model.
//
// A stunning mini IDE showcasing the full maya widget toolkit:
// file tree, tabbed editor with syntax highlighting, outline,
// diagnostics, git status, terminal output, and status bar.
//
// Controls:
//   Tab       cycle open file tabs
//   1         toggle left sidebar
//   2         toggle right sidebar
//   3         toggle bottom panel
//   b         simulate build (progress bar + diagnostics)
//   q/Esc     quit
//
// Usage:  ./maya_ide

#include <maya/host/run.hpp>
#include <maya/maya.hpp>
#include <maya/widget/badge.hpp>
#include <maya/widget/breadcrumb.hpp>
#include <maya/widget/agentty/sparkline.hpp>
#include <maya/widget/progress.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <variant>
#include <vector>

using namespace maya::dsl;

// ── Helpers ─────────────────────────────────────────────────────────────────

static maya::Style fgc(uint8_t r, uint8_t g, uint8_t b) {
    return maya::Style{}.with_fg(maya::Color::rgb(r, g, b));
}

static maya::Color rgb(uint8_t r, uint8_t g, uint8_t b) {
    return maya::Color::rgb(r, g, b);
}

// ── Syntax theme ────────────────────────────────────────────────────────────

static maya::Style kw_style()      { return fgc(198, 120, 221); }   // keywords — purple
static maya::Style str_style()     { return fgc(152, 195, 121); }   // strings — green
static maya::Style cmt_style()     { return fgc(92, 99, 112); }     // comments — gray
static maya::Style type_style()    { return fgc(86, 182, 194); }    // types — cyan
static maya::Style num_style()     { return fgc(209, 154, 102); }   // numbers — orange
static maya::Style fn_style()      { return fgc(229, 192, 123); }   // functions — yellow
static maya::Style plain_style()   { return fgc(171, 178, 191); }   // plain text
static maya::Style punct_style()   { return fgc(150, 156, 170); }   // punctuation

// ── State ───────────────────────────────────────────────────────────────────

struct Model {
    int   active_tab     = 0;
    bool  show_left      = true;
    bool  show_right     = true;
    bool  show_bottom    = true;
    bool  building       = false;
    float build_progress = 0.0f;
    bool  build_done     = false;
    int   frame          = 0;
    int   selected_file  = 3;   // index into file tree
    std::vector<std::string> build_log = {
        "$ cmake --build build --target app",
        "[1/12] Compiling src/main.cpp",
        "[2/12] Compiling src/app.cpp",
        "[3/12] Compiling src/renderer.cpp",
    };
};

// ── File tree data ──────────────────────────────────────────────────────────

struct FileNode {
    std::string name;
    int depth;
    bool is_dir;
    bool expanded;
    const char* ext; // for coloring
};

static const std::vector<FileNode> file_tree = {
    {"src",           0, true,  true,  ""},
    {"main.cpp",      1, false, false, ".cpp"},
    {"app.cpp",       1, false, false, ".cpp"},
    {"app.hpp",       1, false, false, ".hpp"},
    {"renderer.cpp",  1, false, false, ".cpp"},
    {"renderer.hpp",  1, false, false, ".hpp"},
    {"widget",        1, true,  true,  ""},
    {"button.cpp",    2, false, false, ".cpp"},
    {"button.hpp",    2, false, false, ".hpp"},
    {"input.cpp",     2, false, false, ".cpp"},
    {"utils",         0, true,  false, ""},
    {"tests",         0, true,  true,  ""},
    {"test_app.cpp",  1, false, false, ".cpp"},
    {"test_widget.cpp", 1, false, false, ".cpp"},
    {"CMakeLists.txt",  0, false, false, ".txt"},
    {"README.md",       0, false, false, ".md"},
    {"config.py",       0, false, false, ".py"},
};

// ── Tab data ────────────────────────────────────────────────────────────────

struct TabInfo {
    std::string name;
    std::string path;
    std::string language;
    std::vector<std::string> breadcrumb;
};

static const std::array<TabInfo, 4> tabs = {{
    {"main.cpp",     "src/main.cpp",      "C++", {"src", "main.cpp"}},
    {"app.hpp",      "src/app.hpp",       "C++", {"src", "app.hpp"}},
    {"button.cpp",   "src/widget/button.cpp", "C++", {"src", "widget", "button.cpp"}},
    {"config.py",    "config.py",         "Python", {"config.py"}},
}};

// ── Fake code content ───────────────────────────────────────────────────────

struct CodeLine {
    std::string text;
    std::vector<maya::StyledRun> runs;
};

// Helper: create a styled run relative to line start
static maya::StyledRun run(size_t start, size_t len, maya::Style s) {
    return maya::StyledRun{start, len, s};
}

static std::vector<CodeLine> make_main_cpp() {
    std::vector<CodeLine> lines;

    // Line: #include <iostream>
    {
        std::string t = "#include <iostream>";
        std::vector<maya::StyledRun> r;
        r.push_back(run(0, 8, kw_style()));  // #include
        r.push_back(run(9, 10, str_style())); // <iostream>
        lines.push_back({t, r});
    }
    {
        std::string t = "#include \"app.hpp\"";
        std::vector<maya::StyledRun> r;
        r.push_back(run(0, 8, kw_style()));
        r.push_back(run(9, 9, str_style()));
        lines.push_back({t, r});
    }
    lines.push_back({"", {}});
    {
        std::string t = "// Entry point for the application";
        std::vector<maya::StyledRun> r;
        r.push_back(run(0, t.size(), cmt_style()));
        lines.push_back({t, r});
    }
    {
        std::string t = "namespace app {";
        std::vector<maya::StyledRun> r;
        r.push_back(run(0, 9, kw_style()));     // namespace
        r.push_back(run(10, 3, type_style()));   // app
        r.push_back(run(14, 1, punct_style()));  // {
        lines.push_back({t, r});
    }
    lines.push_back({"", {}});
    {
        std::string t = "int main(int argc, char** argv) {";
        std::vector<maya::StyledRun> r;
        r.push_back(run(0, 3, type_style()));    // int
        r.push_back(run(4, 4, fn_style()));      // main
        r.push_back(run(9, 3, type_style()));    // int
        r.push_back(run(20, 4, type_style()));   // char
        lines.push_back({t, r});
    }
    {
        std::string t = "    auto config = Config::load(\"app.toml\");";
        std::vector<maya::StyledRun> r;
        r.push_back(run(4, 4, kw_style()));      // auto
        r.push_back(run(17, 6, type_style()));   // Config
        r.push_back(run(25, 4, fn_style()));     // load
        r.push_back(run(30, 10, str_style()));   // "app.toml"
        lines.push_back({t, r});
    }
    {
        std::string t = "    auto app = Application(config);";
        std::vector<maya::StyledRun> r;
        r.push_back(run(4, 4, kw_style()));
        r.push_back(run(15, 11, type_style()));
        lines.push_back({t, r});
    }
    lines.push_back({"", {}});
    {
        std::string t = "    // Initialize the rendering pipeline";
        std::vector<maya::StyledRun> r;
        r.push_back(run(0, t.size(), cmt_style()));
        lines.push_back({t, r});
    }
    {
        std::string t = "    if (!app.init()) {";
        std::vector<maya::StyledRun> r;
        r.push_back(run(4, 2, kw_style()));    // if
        r.push_back(run(12, 4, fn_style()));   // init
        lines.push_back({t, r});
    }
    {
        std::string t = "        std::cerr << \"Failed to initialize\\n\";";
        std::vector<maya::StyledRun> r;
        r.push_back(run(8, 3, type_style()));  // std
        r.push_back(run(22, 24, str_style())); // "Failed to initialize\n"
        lines.push_back({t, r});
    }
    {
        std::string t = "        return 1;";
        std::vector<maya::StyledRun> r;
        r.push_back(run(8, 6, kw_style()));    // return
        r.push_back(run(15, 1, num_style()));  // 1
        lines.push_back({t, r});
    }
    {
        std::string t = "    }";
        lines.push_back({t, {}});
    }
    lines.push_back({"", {}});
    {
        std::string t = "    app.run();  // blocks until quit";
        std::vector<maya::StyledRun> r;
        r.push_back(run(8, 3, fn_style()));
        r.push_back(run(16, 20, cmt_style()));
        lines.push_back({t, r});
    }
    {
        std::string t = "    return 0;";
        std::vector<maya::StyledRun> r;
        r.push_back(run(4, 6, kw_style()));
        r.push_back(run(11, 1, num_style()));
        lines.push_back({t, r});
    }
    {
        std::string t = "}";
        lines.push_back({t, {}});
    }
    lines.push_back({"", {}});
    {
        std::string t = "} // namespace app";
        std::vector<maya::StyledRun> r;
        r.push_back(run(2, 16, cmt_style()));
        lines.push_back({t, r});
    }

    return lines;
}

static std::vector<CodeLine> make_app_hpp() {
    std::vector<CodeLine> lines;

    lines.push_back({"#pragma once", {{run(0, 12, kw_style())}}});
    lines.push_back({"", {}});
    lines.push_back({"#include <string>", {{run(0, 8, kw_style()), run(9, 8, str_style())}}});
    lines.push_back({"#include <vector>", {{run(0, 8, kw_style()), run(9, 8, str_style())}}});
    lines.push_back({"#include <memory>", {{run(0, 8, kw_style()), run(9, 8, str_style())}}});
    lines.push_back({"", {}});
    lines.push_back({"namespace app {", {{run(0, 9, kw_style()), run(10, 3, type_style())}}});
    lines.push_back({"", {}});
    {
        std::string t = "struct Config {";
        lines.push_back({t, {{run(0, 6, kw_style()), run(7, 6, type_style())}}});
    }
    {
        std::string t = "    std::string title;";
        lines.push_back({t, {{run(4, 3, type_style()), run(9, 6, type_style())}}});
    }
    {
        std::string t = "    int width  = 1280;";
        lines.push_back({t, {{run(4, 3, type_style()), run(17, 4, num_style())}}});
    }
    {
        std::string t = "    int height = 720;";
        lines.push_back({t, {{run(4, 3, type_style()), run(17, 3, num_style())}}});
    }
    {
        std::string t = "    bool vsync = true;";
        lines.push_back({t, {{run(4, 4, type_style()), run(17, 4, kw_style())}}});
    }
    lines.push_back({"", {}});
    {
        std::string t = "    static Config load(std::string_view path);";
        lines.push_back({t, {{run(4, 6, kw_style()), run(11, 6, type_style()),
                              run(18, 4, fn_style()), run(23, 3, type_style()),
                              run(28, 11, type_style())}}});
    }
    lines.push_back({"};", {}});
    lines.push_back({"", {}});
    {
        std::string t = "class Application {";
        lines.push_back({t, {{run(0, 5, kw_style()), run(6, 11, type_style())}}});
    }
    {
        std::string t = "public:";
        lines.push_back({t, {{run(0, 6, kw_style())}}});
    }
    {
        std::string t = "    explicit Application(const Config& cfg);";
        lines.push_back({t, {{run(4, 8, kw_style()), run(13, 11, type_style()),
                              run(25, 5, kw_style()), run(31, 6, type_style())}}});
    }
    lines.push_back({"", {}});
    {
        std::string t = "    bool init();";
        lines.push_back({t, {{run(4, 4, type_style()), run(9, 4, fn_style())}}});
    }
    {
        std::string t = "    void run();";
        lines.push_back({t, {{run(4, 4, type_style()), run(9, 3, fn_style())}}});
    }
    {
        std::string t = "    void quit();";
        lines.push_back({t, {{run(4, 4, type_style()), run(9, 4, fn_style())}}});
    }
    lines.push_back({"};", {}});
    lines.push_back({"", {}});
    lines.push_back({"} // namespace app", {{run(2, 16, cmt_style())}}});

    return lines;
}

static std::vector<CodeLine> make_button_cpp() {
    std::vector<CodeLine> lines;

    lines.push_back({"#include \"button.hpp\"", {{run(0, 8, kw_style()), run(9, 12, str_style())}}});
    lines.push_back({"", {}});
    lines.push_back({"namespace app::widget {", {{run(0, 9, kw_style()), run(10, 3, type_style()),
                                                   run(15, 6, type_style())}}});
    lines.push_back({"", {}});
    {
        std::string t = "Button::Button(std::string label, Callback on_click)";
        lines.push_back({t, {{run(0, 6, type_style()), run(8, 6, type_style()),
                              run(15, 3, type_style()), run(20, 6, type_style()),
                              run(35, 8, type_style())}}});
    }
    {
        std::string t = "    : label_(std::move(label))";
        lines.push_back({t, {{run(6, 6, plain_style()), run(13, 3, type_style()),
                              run(18, 4, fn_style())}}});
    }
    {
        std::string t = "    , on_click_(std::move(on_click))";
        lines.push_back({t, {{run(16, 3, type_style()), run(21, 4, fn_style())}}});
    }
    lines.push_back({"{}", {}});
    lines.push_back({"", {}});
    {
        std::string t = "void Button::render(Canvas& canvas) {";
        lines.push_back({t, {{run(0, 4, type_style()), run(5, 6, type_style()),
                              run(13, 6, fn_style()), run(20, 6, type_style())}}});
    }
    {
        std::string t = "    auto [x, y, w, h] = bounds();";
        lines.push_back({t, {{run(4, 4, kw_style()), run(27, 6, fn_style())}}});
    }
    lines.push_back({"", {}});
    {
        std::string t = "    // Draw button background";
        lines.push_back({t, {{run(0, t.size(), cmt_style())}}});
    }
    {
        std::string t = "    for (int i = 0; i < w; ++i) {";
        lines.push_back({t, {{run(4, 3, kw_style()), run(9, 3, type_style()),
                              run(17, 1, num_style())}}});
    }
    {
        std::string t = "        canvas.set(x + i, y, ' ', style_);";
        lines.push_back({t, {{run(15, 3, fn_style()), run(35, 3, str_style())}}});
    }
    lines.push_back({"    }", {}});
    lines.push_back({"", {}});
    {
        std::string t = "    // Center the label text";
        lines.push_back({t, {{run(0, t.size(), cmt_style())}}});
    }
    {
        std::string t = "    int offset = (w - static_cast<int>(label_.size())) / 2;";
        lines.push_back({t, {{run(4, 3, type_style()), run(18, 11, kw_style()),
                              run(30, 3, type_style())}}});
    }
    {
        std::string t = "    canvas.write(x + offset, y, label_, style_);";
        lines.push_back({t, {{run(11, 5, fn_style())}}});
    }
    lines.push_back({"}", {}});
    lines.push_back({"", {}});
    lines.push_back({"} // namespace app::widget", {{run(2, 24, cmt_style())}}});

    return lines;
}

static std::vector<CodeLine> make_config_py() {
    std::vector<CodeLine> lines;

    lines.push_back({"#!/usr/bin/env python3", {{run(0, 22, cmt_style())}}});
    {
        std::string t = "\"\"\"Application configuration module.\"\"\"";
        lines.push_back({t, {{run(0, t.size(), str_style())}}});
    }
    lines.push_back({"", {}});
    {
        std::string t = "import toml";
        lines.push_back({t, {{run(0, 6, kw_style())}}});
    }
    {
        std::string t = "from pathlib import Path";
        lines.push_back({t, {{run(0, 4, kw_style()), run(14, 6, kw_style()),
                              run(21, 4, type_style())}}});
    }
    lines.push_back({"", {}});
    {
        std::string t = "DEFAULT_WIDTH  = 1280";
        lines.push_back({t, {{run(17, 4, num_style())}}});
    }
    {
        std::string t = "DEFAULT_HEIGHT = 720";
        lines.push_back({t, {{run(17, 3, num_style())}}});
    }
    lines.push_back({"", {}});
    {
        std::string t = "class AppConfig:";
        lines.push_back({t, {{run(0, 5, kw_style()), run(6, 9, type_style())}}});
    }
    {
        std::string t = "    \"\"\"Holds parsed app configuration.\"\"\"";
        lines.push_back({t, {{run(4, t.size() - 4, str_style())}}});
    }
    lines.push_back({"", {}});
    {
        std::string t = "    def __init__(self, path: str = \"app.toml\"):";
        lines.push_back({t, {{run(4, 3, kw_style()), run(8, 8, fn_style()),
                              run(22, 3, type_style()), run(33, 10, str_style())}}});
    }
    {
        std::string t = "        self.data = toml.load(path)";
        lines.push_back({t, {{run(8, 4, kw_style()), run(25, 4, fn_style())}}});
    }
    lines.push_back({"", {}});
    {
        std::string t = "    @property";
        lines.push_back({t, {{run(4, 9, kw_style())}}});
    }
    {
        std::string t = "    def title(self) -> str:";
        lines.push_back({t, {{run(4, 3, kw_style()), run(8, 5, fn_style()),
                              run(23, 3, type_style())}}});
    }
    {
        std::string t = "        return self.data.get(\"title\", \"Untitled\")";
        lines.push_back({t, {{run(8, 6, kw_style()), run(33, 7, str_style()),
                              run(42, 10, str_style())}}});
    }

    return lines;
}

static const std::array<std::vector<CodeLine>, 4> code_buffers = {
    make_main_cpp(), make_app_hpp(), make_button_cpp(), make_config_py(),
};

// ── Outline data ────────────────────────────────────────────────────────────

struct Symbol {
    std::string name;
    std::string kind; // fn, class, struct, var
    int line;
};

static const std::array<std::vector<Symbol>, 4> outlines = {{
    {{"main", "fn", 7}, {"config", "var", 8}, {"app", "var", 9}, {"init", "fn", 12}, {"run", "fn", 17}},
    {{"Config", "struct", 9}, {"load", "fn", 16}, {"Application", "class", 19}, {"init", "fn", 24}, {"run", "fn", 25}, {"quit", "fn", 26}},
    {{"Button", "class", 5}, {"render", "fn", 10}, {"offset", "var", 20}, {"write", "fn", 21}},
    {{"DEFAULT_WIDTH", "var", 7}, {"DEFAULT_HEIGHT", "var", 8}, {"AppConfig", "class", 10}, {"__init__", "fn", 13}, {"title", "fn", 17}},
}};

// ── Diagnostics ─────────────────────────────────────────────────────────────

struct Diagnostic {
    std::string file;
    int line;
    std::string message;
    int severity; // 0=error, 1=warn, 2=info
};

static const std::vector<Diagnostic> diagnostics = {
    {"src/renderer.cpp", 42, "unused variable 'tmp'",              1},
    {"src/app.cpp",      87, "implicit conversion loses precision", 1},
    {"src/widget/input.cpp", 15, "uninitialized member 'buf_'",    0},
    {"src/main.cpp",     23, "consider using std::string_view",     2},
    {"tests/test_app.cpp", 9, "deprecated function 'setUp'",       1},
};

// ── Git changes ─────────────────────────────────────────────────────────────

struct GitChange {
    std::string file;
    int added;
    int removed;
    char status; // M, A, D
};

static const std::vector<GitChange> git_changes = {
    {"src/main.cpp",        12,  3, 'M'},
    {"src/app.cpp",         45, 18, 'M'},
    {"src/widget/button.cpp", 8,  0, 'A'},
    {"src/renderer.hpp",     3,  7, 'M'},
    {"config.py",            5,  0, 'A'},
};

// ── Build output ────────────────────────────────────────────────────────────


static const std::vector<std::string> build_complete_log = {
    "$ cmake --build build --target app",
    "[1/12] Compiling src/main.cpp",
    "[2/12] Compiling src/app.cpp",
    "[3/12] Compiling src/renderer.cpp",
    "[4/12] Compiling src/widget/button.cpp",
    "[5/12] Compiling src/widget/input.cpp",
    "[6/12] Linking libwidget.a",
    "[7/12] Linking app",
    "",
    "src/renderer.cpp:42:9: warning: unused variable 'tmp'",
    "src/app.cpp:87:15: warning: implicit conversion",
    "src/widget/input.cpp:15:5: error: uninitialized member",
    "",
    "Build finished with 1 error, 2 warnings.",
};

// ── UI Builders ─────────────────────────────────────────────────────────────

static maya::Element build_file_tree(const Model& m) {
    std::vector<maya::Element> rows;

    for (int i = 0; i < static_cast<int>(file_tree.size()); ++i) {
        auto& f = file_tree[static_cast<size_t>(i)];

        std::string indent;
        for (int d = 0; d < f.depth; ++d) indent += "  ";

        std::string icon;
        maya::Style name_style;

        if (f.is_dir) {
            icon = f.expanded ? "▾ " : "▸ ";
            name_style = fgc(200, 204, 212).with_bold();
        } else {
            icon = "  ";
            // Color by extension
            std::string ext(f.ext);
            if (ext == ".cpp") name_style = fgc(97, 175, 239);
            else if (ext == ".hpp") name_style = fgc(152, 195, 121);
            else if (ext == ".py") name_style = fgc(229, 192, 123);
            else if (ext == ".md") name_style = fgc(92, 99, 112);
            else name_style = fgc(171, 178, 191);
        }

        std::string content = indent + icon + f.name;
        std::vector<maya::StyledRun> runs;

        // Dim indent
        if (!indent.empty()) {
            runs.push_back(run(0, indent.size(), fgc(50, 55, 70)));
        }
        // Icon
        runs.push_back(run(indent.size(), icon.size(), fgc(150, 156, 170)));
        // Name
        if (i == m.selected_file) {
            runs.push_back(run(indent.size() + icon.size(), f.name.size(),
                              name_style.with_bold().with_underline()));
        } else {
            runs.push_back(run(indent.size() + icon.size(), f.name.size(), name_style));
        }

        rows.push_back(maya::Element{maya::TextElement{
            .content = std::move(content),
            .style = {},
            .runs = std::move(runs),
        }});
    }

    return vstack().border(maya::BorderStyle::Round)
        .border_color(rgb(50, 55, 70))
        .border_text(" EXPLORER ", maya::BorderTextPos::Top)
        .padding(0, 1, 0, 1)
        .width(22)(std::move(rows));
}

static maya::Element build_tab_bar(const Model& m) {
    std::string content;
    std::vector<maya::StyledRun> runs;

    auto active_style = maya::Style{}.with_bold().with_underline()
                            .with_fg(rgb(97, 175, 239));
    auto inactive_style = maya::Style{}.with_fg(rgb(150, 156, 170));
    auto sep_style = maya::Style{}.with_fg(rgb(50, 55, 70));

    for (int i = 0; i < 4; ++i) {
        if (i > 0) {
            std::string sep = " | ";
            runs.push_back(maya::StyledRun{content.size(), sep.size(), sep_style});
            content += sep;
        }
        auto& tab = tabs[static_cast<size_t>(i)];
        runs.push_back(maya::StyledRun{
            content.size(), tab.name.size(),
            (i == m.active_tab) ? active_style : inactive_style,
        });
        content += tab.name;
    }

    return maya::Element{maya::TextElement{
        .content = std::move(content),
        .style = inactive_style,
        .runs = std::move(runs),
    }};
}

static maya::Element build_breadcrumb(const Model& m) {
    auto& tab = tabs[static_cast<size_t>(m.active_tab)];
    maya::Breadcrumb bc(tab.breadcrumb);
    return bc.build();
}

static maya::Element build_code_editor(const Model& m) {
    auto& lines = code_buffers[static_cast<size_t>(m.active_tab)];

    std::vector<maya::Element> rows;

    auto line_num_style = fgc(92, 99, 112);

    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        auto& line = lines[static_cast<size_t>(i)];

        // Build line number + code as single text element with runs
        char num_buf[8];
        std::snprintf(num_buf, sizeof(num_buf), "%3d ", i + 1);
        std::string num_str(num_buf);

        std::string content = num_str + line.text;
        std::vector<maya::StyledRun> runs;

        // Line number run
        runs.push_back(maya::StyledRun{0, num_str.size(), line_num_style});

        // Code runs (offset by line number width)
        for (auto& r : line.runs) {
            runs.push_back(maya::StyledRun{
                r.byte_offset + num_str.size(), r.byte_length,
                r.style,
            });
        }

        // If no runs for code, add plain style
        if (line.runs.empty() && !line.text.empty()) {
            runs.push_back(maya::StyledRun{num_str.size(), line.text.size(), plain_style()});
        }

        rows.push_back(maya::Element{maya::TextElement{
            .content = std::move(content),
            .style = plain_style(),
            .runs = std::move(runs),
        }});
    }

    return vstack()(std::move(rows));
}

static maya::Element build_minimap(const Model& m) {
    auto& lines = code_buffers[static_cast<size_t>(m.active_tab)];

    // Build sparkline data: "code density" per line
    std::vector<float> density;
    density.reserve(lines.size());
    for (auto& line : lines) {
        float d = static_cast<float>(line.text.size()) / 60.0f;
        density.push_back(std::clamp(d, 0.0f, 1.0f));
    }

    maya::Sparkline spark(density);
    spark.set_color(rgb(60, 80, 120));

    return spark.build();
}

static maya::Element build_editor_panel(const Model& m) {
    std::vector<maya::Element> rows;

    rows.push_back(build_tab_bar(m));
    rows.push_back(build_breadcrumb(m));
    rows.push_back((h(
        build_code_editor(m),
        space,
        build_minimap(m)
    )).build());

    return vstack().border(maya::BorderStyle::Round)
        .border_color(rgb(50, 55, 70))
        .border_text(std::string(" ") + tabs[static_cast<size_t>(m.active_tab)].name + " ",
                     maya::BorderTextPos::Top)
        .padding(0, 1, 0, 1)
        .grow(1)(std::move(rows));
}

static maya::Element build_outline_panel(const Model& m) {
    auto& syms = outlines[static_cast<size_t>(m.active_tab)];
    std::vector<maya::Element> rows;

    for (auto& sym : syms) {
        std::string icon;
        maya::Style icon_style;
        if (sym.kind == "fn") {
            icon = "f ";
            icon_style = fgc(198, 120, 221);
        } else if (sym.kind == "class") {
            icon = "C ";
            icon_style = fgc(229, 192, 123);
        } else if (sym.kind == "struct") {
            icon = "S ";
            icon_style = fgc(86, 182, 194);
        } else {
            icon = "v ";
            icon_style = fgc(152, 195, 121);
        }

        std::string line_str = ":" + std::to_string(sym.line);

        std::string content = icon + sym.name + line_str;
        std::vector<maya::StyledRun> runs;
        runs.push_back(maya::StyledRun{0, icon.size(), icon_style});
        runs.push_back(maya::StyledRun{icon.size(), sym.name.size(), fgc(200, 204, 212)});
        runs.push_back(maya::StyledRun{icon.size() + sym.name.size(), line_str.size(), fgc(92, 99, 112)});

        rows.push_back(maya::Element{maya::TextElement{
            .content = std::move(content),
            .style = {},
            .runs = std::move(runs),
        }});
    }

    return vstack().border(maya::BorderStyle::Round)
        .border_color(rgb(50, 55, 70))
        .border_text(" OUTLINE ", maya::BorderTextPos::Top)
        .padding(0, 1, 0, 1)(std::move(rows));
}

static maya::Element build_diagnostics_panel(const Model& m) {
    std::vector<maya::Element> rows;

    for (auto& d : diagnostics) {
        maya::Element badge_elem = (d.severity == 0)
            ? maya::Badge::error("ERR").build()
            : (d.severity == 1 ? maya::Badge::warning("WRN").build()
                               : maya::Badge::info("INF").build());

        std::string loc = d.file + ":" + std::to_string(d.line);

        rows.push_back((h(
            std::move(badge_elem),
            text(loc, fgc(100, 180, 255)) | clip,
            text(" " + d.message) | Dim | clip
        ) | gap_<1>).build());
    }

    return vstack().border(maya::BorderStyle::Round)
        .border_color(rgb(50, 55, 70))
        .border_text(" DIAGNOSTICS ", maya::BorderTextPos::Top)
        .padding(0, 1, 0, 1)(std::move(rows));
}

static maya::Element build_git_panel(const Model& m) {
    std::vector<maya::Element> rows;

    for (auto& g : git_changes) {
        char status_ch = g.status;
        maya::Style status_style;
        if (status_ch == 'M') status_style = fgc(229, 192, 123);
        else if (status_ch == 'A') status_style = fgc(152, 195, 121);
        else status_style = fgc(224, 108, 117);

        std::string adds = "+" + std::to_string(g.added);
        std::string dels = "-" + std::to_string(g.removed);

        std::string content;
        std::vector<maya::StyledRun> runs;

        std::string s(1, status_ch);
        content += s + " ";
        runs.push_back(maya::StyledRun{0, 1, status_style.with_bold()});

        size_t fname_start = content.size();
        content += g.file;
        runs.push_back(maya::StyledRun{fname_start, g.file.size(), fgc(171, 178, 191)});

        content += " ";
        size_t add_start = content.size();
        content += adds;
        runs.push_back(maya::StyledRun{add_start, adds.size(), fgc(152, 195, 121)});

        content += " ";
        size_t del_start = content.size();
        content += dels;
        if (g.removed > 0)
            runs.push_back(maya::StyledRun{del_start, dels.size(), fgc(224, 108, 117)});
        else
            runs.push_back(maya::StyledRun{del_start, dels.size(), fgc(92, 99, 112)});

        rows.push_back(maya::Element{maya::TextElement{
            .content = std::move(content),
            .style = {},
            .runs = std::move(runs),
        }});
    }

    return vstack().border(maya::BorderStyle::Round)
        .border_color(rgb(50, 55, 70))
        .border_text(" GIT CHANGES ", maya::BorderTextPos::Top)
        .padding(0, 1, 0, 1)(std::move(rows));
}

static maya::Element build_right_sidebar(const Model& m) {
    return vstack().width(28)(
        build_outline_panel(m),
        build_diagnostics_panel(m),
        build_git_panel(m)
    );
}

static maya::Element build_terminal_panel(const Model& m) {
    std::vector<maya::Element> rows;

    auto& log = m.building || m.build_done ? build_complete_log : m.build_log;
    int show_lines = m.building
        ? std::min(static_cast<int>(log.size()),
                   static_cast<int>(m.build_progress * static_cast<float>(log.size())))
        : static_cast<int>(log.size());

    for (int i = 0; i < show_lines && i < static_cast<int>(log.size()); ++i) {
        auto& line = log[static_cast<size_t>(i)];
        maya::Style line_style;
        if (line.starts_with("$")) {
            line_style = fgc(152, 195, 121);
        } else if (line.find("error") != std::string::npos) {
            line_style = fgc(224, 108, 117);
        } else if (line.find("warning") != std::string::npos) {
            line_style = fgc(229, 192, 123);
        } else if (line.find("Build finished") != std::string::npos) {
            line_style = fgc(200, 204, 212).with_bold();
        } else {
            line_style = fgc(120, 126, 140);
        }
        rows.push_back(text(line, line_style).build());
    }

    // Progress bar if building
    if (m.building) {
        maya::ProgressBar bar;
        bar.set(m.build_progress);
        bar.set_label("Building...");
        rows.push_back(bar.build());
    }

    // Fill to minimum height
    while (static_cast<int>(rows.size()) < 4) {
        rows.push_back(text("").build());
    }

    return vstack().border(maya::BorderStyle::Round)
        .border_color(rgb(50, 55, 70))
        .border_text(" TERMINAL ", maya::BorderTextPos::Top)
        .padding(0, 1, 0, 1)(std::move(rows));
}

static maya::Element build_status_bar(const Model& m) {
    auto& tab = tabs[static_cast<size_t>(m.active_tab)];

    // Error/warning counts
    int errors = 0, warnings = 0;
    for (auto& d : diagnostics) {
        if (d.severity == 0) errors++;
        else if (d.severity == 1) warnings++;
    }

    std::string err_str = std::to_string(errors) + " errors";
    std::string warn_str = std::to_string(warnings) + " warnings";

    return (h(
        text(" " + tab.name) | Bold | Fg<97, 175, 239>,
        text("  Ln 1, Col 1") | Fg<140, 140, 160>,
        text("  " + tab.language) | Fg<171, 178, 191>,
        text("  UTF-8") | Fg<140, 140, 160>,
        space,
        maya::Badge::error(err_str).build(),
        text(" "),
        maya::Badge::warning(warn_str).build(),
        text("  "),
        maya::Badge::info("main").build(),
        text(" ")
    ) | pad<0, 1, 0, 1> | Bg<30, 30, 42>).build();
}

// ── Render ──────────────────────────────────────────────────────────────────

static maya::Element render(const Model& m) {
    // Main layout: 3 columns with optional sidebars
    std::vector<maya::Element> columns;

    if (m.show_left) {
        columns.push_back(build_file_tree(m));
    }

    columns.push_back(build_editor_panel(m));

    if (m.show_right) {
        columns.push_back(build_right_sidebar(m));
    }

    auto main_row = hstack().grow(1)(std::move(columns));

    // Vertical stack: main row + optional bottom + status bar
    std::vector<maya::Element> main_stack;
    main_stack.push_back(std::move(main_row));

    if (m.show_bottom) {
        main_stack.push_back(build_terminal_panel(m));
    }

    main_stack.push_back(build_status_bar(m));

    return vstack()(std::move(main_stack));
}

// ── Program ─────────────────────────────────────────────────────────────────

struct Tick {};
struct NextTab {};
struct ToggleLeft {};
struct ToggleRight {};
struct ToggleBottom {};
struct StartBuild {};
struct Quit {};
using Msg = std::variant<Tick, NextTab, ToggleLeft, ToggleRight, ToggleBottom, StartBuild, Quit>;

struct Ide {
    using Model = ::Model;
    using Msg   = ::Msg;
    using Cmd   = jaal::Cmd<Msg>;
    using Sub   = jaal::Sub<Msg>;

    static Cmd update(Model& m, Tick) {
        m.frame++;
        // Advance build simulation
        if (m.building) {
            m.build_progress += 0.02f;
            if (m.build_progress >= 1.0f) {
                m.build_progress = 1.0f;
                m.building = false;
                m.build_done = true;
            }
        }
        return {};
    }
    static Cmd update(Model& m, NextTab)      { m.active_tab = (m.active_tab + 1) % 4; return {}; }
    static Cmd update(Model& m, ToggleLeft)   { m.show_left = !m.show_left; return {}; }
    static Cmd update(Model& m, ToggleRight)  { m.show_right = !m.show_right; return {}; }
    static Cmd update(Model& m, ToggleBottom) { m.show_bottom = !m.show_bottom; return {}; }
    static Cmd update(Model& m, StartBuild) {
        if (!m.building) {
            m.building = true;
            m.build_progress = 0.0f;
            m.build_done = false;
        }
        return {};
    }
    static Cmd update(Model&, Quit) { return Cmd::quit(0); }

    static maya::Element view(const Model& m) { return render(m); }

    static Sub subscribe(const Model& m) {
        if (m.building) return Sub::every(std::chrono::milliseconds(100), Tick{});
        return Sub::none();
    }
    static std::optional<Msg> on(const Model&, const maya::KeyEvent& k) {
        static const maya::keymap<Msg> keys{
            {'q', Quit{}},
            {maya::SpecialKey::Escape, Quit{}},
            {maya::SpecialKey::Tab, NextTab{}},
            {'1', ToggleLeft{}},
            {'2', ToggleRight{}},
            {'3', ToggleBottom{}},
            {'b', StartBuild{}},
        };
        return keys(k);
    }
    static bool subs_key(const Model& m) { return m.building; }
};

static_assert(maya::Program<Ide>);

int main() {
    return maya::run<Ide>({.title = "ide"});
}
