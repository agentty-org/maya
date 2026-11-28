// examples/editor_widgets.cpp — independent widget showcase browser.
//
//   cmake --build build --target maya_editor_widgets && ./build/maya_editor_widgets
//
// One widget at a time, centered and titled. Nothing else on screen.
//   ← / →  (or h/l)   switch widget
//   ↑ / ↓  (or k/j)   interact with the current widget
//   q                 quit

#include <maya/host/run.hpp>
#include <maya/maya.hpp>
#include <maya/widget/code_view.hpp>
#include <maya/widget/minimap.hpp>
#include <maya/widget/overview_ruler.hpp>
#include <maya/widget/editor_tab_bar.hpp>
#include <maya/widget/breadcrumb_bar.hpp>
#include <maya/widget/symbol_outline.hpp>
#include <maya/widget/agentty/completion_menu.hpp>
#include <maya/widget/signature_help.hpp>
#include <maya/widget/command_palette_bar.hpp>
#include <maya/widget/search_results.hpp>
#include <maya/widget/context_menu.hpp>
#include <maya/widget/debug_toolbar.hpp>
#include <maya/widget/peek_view.hpp>
#include <maya/widget/toolbar.hpp>
#include <maya/widget/panel_tabs.hpp>
#include <maya/widget/marker_gutter.hpp>
#include <maya/widget/diff_stat.hpp>
#include <maya/widget/quick_input.hpp>
#include <maya/widget/which_key_menu.hpp>
#include <maya/widget/notification_stack.hpp>
#include <maya/widget/terminal_pane.hpp>
#include <maya/widget/diff_hunk_view.hpp>
#include <maya/widget/problems_panel.hpp>
#include <maya/widget/hover_card.hpp>
#include <maya/widget/file_tree.hpp>
#include <maya/widget/git_blame_gutter.hpp>
#include <maya/widget/editor_status_line.hpp>
#include <maya/widget/diagnostic_lens.hpp>
#include <maya/widget/find_replace_bar.hpp>
#include <maya/widget/fuzzy_line.hpp>

#include <array>
#include <string>

using namespace maya;
using namespace maya::dsl;

static const std::string kSrc =
    "#include <optional>\n"
    "\n"
    "// Fibonacci with memoisation.\n"
    "template <class T>\n"
    "T fib(int n, std::vector<std::optional<T>>& memo) {\n"
    "    if (n < 2) return n;\n"
    "    if (memo[n]) return *memo[n];\n"
    "    T v = fib<T>(n - 1, memo) + fib<T>(n - 2, memo);\n"
    "    memo[n] = v;\n"
    "    return v;   // cached\n"
    "}\n";

static constexpr std::array<const char*, 30> kNames = {
    "SearchResults", "ContextMenu", "DebugToolbar", "PeekView", "Toolbar",
    "WhichKeyMenu", "QuickInput", "DiffStat", "MarkerGutter", "PanelTabs",
    "NotificationStack", "TerminalPane", "DiffHunkView", "ProblemsPanel",
    "CommandPalette", "HoverCard", "FileTree", "SignatureHelp",
    "CompletionMenu", "GitBlameGutter", "SymbolOutline", "FindReplaceBar",
    "Breadcrumb", "OverviewRuler", "EditorTabBar", "DiagnosticLens",
    "FuzzyLine", "CodeView", "Minimap", "EditorStatusLine",
};

struct Show {
    struct Model { int page = 0; int k = 0; };
    struct Prev{}; struct Next{}; struct Up{}; struct Down{}; struct Quit{};
    using Msg = std::variant<Prev, Next, Up, Down, Quit>;

    static constexpr int N = static_cast<int>(kNames.size());

    using Cmd = jaal::Cmd<Msg>;

    static Cmd update(Model&, Quit)   { return Cmd::quit(0); }
    static Cmd update(Model& m, Prev) { m.page = (m.page + N - 1) % N; m.k = 0; return {}; }
    static Cmd update(Model& m, Next) { m.page = (m.page + 1) % N;     m.k = 0; return {}; }
    static Cmd update(Model& m, Up)   { m.k = std::max(0, m.k - 1); return {}; }
    static Cmd update(Model& m, Down) { m.k = m.k + 1; return {}; }

    // ── each widget, built in a representative state (newest first) ─────────
    static Element widget(int page, int k) {
        switch (page) {
        case 0: { // SearchResults
            SearchResults s;
            s.file("src/rope.cpp", 3)
               .match(41, "    return ", "weight", " + left->size();")
               .match(50, "  std::size_t ", "weight", " = 0;")
               .match(63, "    n->", "weight", " = a ? a->size() : 0;")
             .file("include/rope.hpp", 1)
               .match(8, "  std::size_t ", "weight", ";");
            s.active(k % 4);
            return s | width(64);
        }
        case 1: { // ContextMenu
            ContextMenu m;
            m.item("Go to Definition", "F12", "\xef\x87\x89")
             .item("Peek References", "\xe2\x87\xa7 F12")
             .separator()
             .item("Rename Symbol", "F2")
             .item("Change All Occurrences", "\xe2\x8c\x98 F2")
             .separator()
             .check("Word Wrap", true)
             .disabled("Format Selection")
             .submenu("Refactor")
             .active(k % 7);
            return m | width(42);
        }
        case 2: { // DebugToolbar
            DebugToolbar::State st[] = {DebugToolbar::Running, DebugToolbar::Paused,
                                        DebugToolbar::Stopped};
            return DebugToolbar{st[k % 3]};
        }
        case 3: { // PeekView
            CodeView snip{"char Rope::at(std::size_t i) const {\n"
                          "    if (i < weight && left)\n"
                          "        return left->at(i);\n"
                          "    return text[i - weight];\n}\n",
                          {.lang = syntax::Lang::Cpp, .first_line = 40}};
            snip.set_active_line(41 + (k % 4));
            PeekView p;
            p.title("3 references").location("rope.hpp:42").body(snip);
            return p | width(66);
        }
        case 4: { // Toolbar
            Toolbar t;
            t.button("\xef\x83\x87", "Save")
             .toggle("\xef\x81\xb0", "Wrap", (k % 2))
             .sep()
             .button("\xef\x80\xa1", "Refresh")
             .button("\xef\x84\xa2", "Collapse All")
             .sep()
             .disabled("\xef\x87\xb8", "Delete");
            return t;
        }
        case 5: { // WhichKeyMenu
            WhichKeyMenu wk{"SPACE"};
            wk.key("f", "find file").key("/", "search")
              .key("g", "git", true).key("b", "buffers", true)
              .key("w", "window", true).key("l", "lsp", true)
              .key("p", "project", true).key("q", "quit").key("t", "terminal");
            return wk | width(50);
        }
        case 6: { // QuickInput
            QuickInput q;
            q.label("Rename Symbol").value("weight").hint("Enter to confirm  Esc to cancel");
            if (k % 3 == 2) q.error("'weight' already exists in this scope");
            return q | width(48);
        }
        case 7: { // DiffStat
            DiffStat d;
            d.file("src/rope.cpp", 42, 8)
             .file("include/rope.hpp", 3, 0)
             .file("tests/rope_test.cpp", 0, 15)
             .file("CMakeLists.txt", 2, 1);
            return d | width(52);
        }
        case 8: { // MarkerGutter
            MarkerGutter g;
            g.line(Marker::None).line(Marker::Breakpoint)
             .line(Marker::ConditionalBreakpoint).line(Marker::Execution)
             .line(Marker::None).line(Marker::Bookmark)
             .line(Marker::BreakpointDisabled).line(Marker::None);
            return g | width(3);
        }
        case 9: { // PanelTabs
            PanelTabs p;
            p.tab("PROBLEMS", 4).tab("OUTPUT").tab("DEBUG CONSOLE").tab("TERMINAL")
             .active(k % 4);
            return p;
        }
        case 10: { // NotificationStack
            NotificationStack n;
            n.toast(NotificationStack::Success, "Saved", "rope.cpp written (2.1 KB)")
             .toast(NotificationStack::Progress, "Indexing workspace",
                    "1,204 / 3,900 files", 0.31f + 0.1f * (k % 5))
             .toast(NotificationStack::Warning, "2 deprecations", "std::aligned_storage")
             .toast(NotificationStack::Error, "Build failed", "3 errors in editor.cpp");
            return n | width(48);
        }
        case 11: { // TerminalPane
            TerminalPane t;
            t.prompt("~/rope", "cmake --build build -j")
             .out("[ 62%] Building CXX object rope.cpp.o")
             .out("[100%] Linking CXX executable maya")
             .ok("Build succeeded in 4.2s")
             .prompt("~/rope", "ctest --test-dir build")
             .info("Test project /home/ayush/rope/build")
             .err("1/64 Test #7: rope_test .......... Failed")
             .input("~/rope", "git commit -am \"fix rope\"");
            return t;
        }
        case 12: { // DiffHunkView
            DiffHunkView d;
            d.hunk("@@ -10,6 +10,7 @@ char Rope::at(std::size_t i) const")
             .ctx("    if (i < weight && left)", 10, 10)
             .ctx("        return left->at(i);", 11, 11)
             .del("    return text[i - weight];", 12)
             .add("    return text[i - weight];   // leaf fast-path", 13)
             .add("    // TODO: bounds check", 14)
             .ctx("}", 13, 15);
            return d;
        }
        case 13: { // ProblemsPanel
            ProblemsPanel p;
            p.file("rope.cpp", true)
               .problem(Severity::Error,   "expected ';' after expression", 41, 18, "E0001")
               .problem(Severity::Error,   "use of undeclared identifier 'wieght'", 41, 9)
               .problem(Severity::Warning, "unused variable 'weight'", 12, 9)
             .file("editor.rs", true)
               .problem(Severity::Warning, "value assigned is never read", 88, 13)
               .problem(Severity::Hint,    "consider using `if let`", 90, 5)
             .file("docs.md", false)
               .problem(Severity::Info, "dead link", 3, 1);
            p.active(k % 6);
            return p | width(62);
        }
        case 14: { // CommandPalette
            CommandPalette p; p.query = "fmt";
            p.command({SymKind::Function, "Format Document",  "Editor", "\xe2\x87\xa7\xe2\x8c\xa5 F"})
             .command({SymKind::Function, "Format Selection", "Editor", ""})
             .command({SymKind::Method,   "Fold All",         "View",   "\xe2\x8c\x98 K 0"})
             .command({SymKind::Variable, "Toggle Word Wrap", "View",   "\xe2\x8c\xa5 Z"})
             .command({SymKind::Field,    "Find in Files",    "Search", "\xe2\x87\xa7\xe2\x8c\x98 F"})
             .select(k % 5);
            return p | width(58);
        }
        case 15: { // HoverCard
            HoverCard h;
            h.signature("char Rope::at(std::size_t i) const")
             .doc("Random access into the rope by flat index. Descends the tree "
                  "following subtree weights, so lookup is O(log n).")
             .note("rope.hpp").note("since 0.3").note("noexcept");
            return h | width(54);
        }
        case 16: { // FileTree
            FileTree t;
            t.root("rope");
            t.folder("src", 0);
              t.file("rope.cpp", 1).git(GitState::Modified).diag(2, 1).marked();
              t.file("rope.hpp", 1);
              t.file("editor.rs", 1).git(GitState::Added);
              t.file("link.hpp", 1).symlink("../shared/link.hpp");
            t.folder("tests", 0, false).count(4);
            t.folder("docs", 0);
              t.file("ropes.md", 1).git(GitState::Untracked).marked();
            t.file("CMakeLists.txt", 0).diag(0, 1);
            t.file(".gitignore", 0).hidden();
            t.file("README.md", 0);
            t.filter("rope");        // highlight matches (↑/↓ moves selection)
            t.active(k % 10);
            return t | width(34);
        }
        case 17: { // SignatureHelp
            SignatureHelp sh{"concat", "Rope*"};
            sh.param("Rope* a").param("Rope* b").active(k % 2)
              .overload(1, 2).doc("Join two ropes into one balanced tree.");
            return sh | width(52);
        }
        case 18: { // CompletionMenu
            CompletionMenu cm;
            cm.item({SymKind::Method, "concat", "(Rope*, Rope*) -> Rope*", "Join two ropes.", {0,1,2}})
              .item({SymKind::Method, "collapse", "() -> std::string", "Flatten to a string."})
              .item({SymKind::Field,  "weight", ": std::size_t", "Left-subtree length."})
              .item({SymKind::Field,  "text",   ": std::string", "Leaf payload."})
              .item({SymKind::Method, "at",     "(size_t) -> char", "Random access."})
              .item({SymKind::Constant, "kInlineMax", ": std::size_t"})
              .select(k % 6);
            return cm | width(46);
        }
        case 19: { // GitBlameGutter
            GitBlameGutter g{{.width = 24}};
            g.line("Ayush Bhat", "1 hour ago", "a1c2f3e")
             .uncommitted()
             .line("Ada Lovelace", "3 months ago", "9f0b12d")
             .line("Grace Hopper", "1 year ago", "77aa10c")
             .line("Ken Thompson", "2 years ago", "0b1d2e3")
             .active(k % 5);
            return g | width(24);
        }
        case 20: { // SymbolOutline
            SymbolOutline o;
            o.node(SymKind::Namespace, "rope", 0)
             .node(SymKind::Struct, "Rope", 1)
             .node(SymKind::Field,  "text",   2, ": std::string")
             .node(SymKind::Field,  "weight", 2, ": std::size_t")
             .node(SymKind::Method, "at",     2, "(size_t) -> char")
             .node(SymKind::Method, "concat", 2, "(Rope*, Rope*) -> Rope*")
             .active(k % 6);
            return o;
        }
        case 21: { // FindReplaceBar
            FindReplaceBar f;
            f.query = "weight"; f.current = 2; f.total = 3;
            f.case_sensitive = (k % 2); f.regex = ((k / 2) % 2);
            f.replace_mode = true; f.replacement = "weight_";
            return f | width(56);
        }
        case 22: { // Breadcrumb
            Breadcrumb bc;
            bc.crumb(SymKind::Folder, "src").crumb(SymKind::File, "rope.cpp")
              .crumb(SymKind::Namespace, "rope").crumb(SymKind::Class, "Rope")
              .crumb(SymKind::Method, "concat");
            return bc;
        }
        case 23: { // OverviewRuler
            OverviewRuler r;
            int c = 1 + (k % 40);
            r.total(40).viewport(std::max(1, c - 6), std::min(40, c + 6)).cursor(c)
             .mark(4, RulerMark::Error).mark(12, RulerMark::Warning)
             .mark(22, RulerMark::Search).mark(31, RulerMark::Change);
            return r | width(2) | height(14);
        }
        case 24: { // EditorTabBar
            EditorTabBar tb;
            tb.tab({.name = "rope.cpp", .modified = true})
              .tab({.name = "rope.hpp"})
              .tab({.name = "editor.rs", .diag = TabDiag::Error})
              .tab({.name = "parser.go", .diag = TabDiag::Warning})
              .tab({.name = "config.json", .pinned = true})
              .tab({.name = "NOTES.md", .preview = true})
              .active(k % 6);
            return tb;
        }
        case 25: { // DiagnosticLens
            Severity sev = static_cast<Severity>(k % 4);
            const char* msgs[] = {
                "expected ';' after expression",
                "unused variable 'weight'",
                "inferred type: std::size_t",
                "prefer std::move here",
            };
            return DiagnosticLens{sev, msgs[k % 4]}.at(4, 6);
        }
        case 26: { // FuzzyLine
            const char* files[] = {"src/rope.cpp", "src/rope.hpp",
                                   "include/editor/rope.hpp", "tests/rope_test.cpp",
                                   "docs/ropes.md"};
            std::vector<Element> rows;
            for (auto* fpath : files) {
                auto r = FuzzyLine::match(fpath, "rope");
                if (r) rows.push_back(FuzzyLine{fpath, r.positions}
                                        .icon("\xef\x85\x9b ").hint("file"));
            }
            return v(std::move(rows));
        }
        case 27: { // CodeView
            CodeView cv{kSrc, {.lang = syntax::Lang::Cpp}};
            int line = 1 + (k % 11);
            cv.set_caret(line, 4)
              .mark(3, LineMark::Added).mark(9, LineMark::Modified)
              .mark(5, LineMark::Warning)
              .set_selection(6, 8, 6, 20);
            return cv;
        }
        case 28: { // Minimap
            Minimap mm{{.width = 18, .lang = syntax::Lang::Cpp}};
            mm.set_source(kSrc);
            int c = 1 + (k % 11);
            mm.set_viewport(std::max(1, c - 3), std::min(11, c + 3)).set_active_line(c);
            return mm | width(18) | height(11);
        }
        default: { // EditorStatusLine
            const char* modes[] = {"NORMAL", "INSERT", "VISUAL", "REPLACE"};
            EditorStatusLine sl;
            sl.left(EditorStatusLine::mode(modes[k % 4]))
              .left(EditorStatusLine::branch("main"))
              .left(EditorStatusLine::file("rope.cpp", true))
              .right(EditorStatusLine::info("utf-8"))
              .right(EditorStatusLine::info("LF"))
              .right(EditorStatusLine::lang("C++"))
              .right(EditorStatusLine::pos(8, 12, 40));
            return sl | width(70);
        }
        }
    }

    static Element view(const Model& m) {
        std::string title = "\xe2\x97\x86  " + std::string(kNames[m.page]);
        std::string counter = std::to_string(m.page + 1) + " / " + std::to_string(N);

        auto header = h(
            text(title) | fgc(Color::hex(0xCBA6F7)) | Bold,
            spacer(),
            text(counter) | fgc(Color::hex(0x585B70))
        );
        auto footer = text("\xe2\x86\x90 \xe2\x86\x92 switch widget    \xe2\x86\x91 \xe2\x86\x93 interact    q quit")
                    | fgc(Color::hex(0x585B70));

        return v(
            header,
            text(""),
            widget(m.page, m.k),
            spacer(),
            footer
        ) | padding(1, 3, 1, 3) | grow(1);
    }

    static std::optional<Msg> on(const Model&, const KeyEvent& k) {
        static const keymap<Msg> keys{
            {SpecialKey::Left, Prev{}},  {'h', Prev{}},
            {SpecialKey::Right, Next{}}, {'l', Next{}},
            {SpecialKey::Up, Up{}},      {'k', Up{}},
            {SpecialKey::Down, Down{}},  {'j', Down{}},
            {'q', Quit{}}, {SpecialKey::Escape, Quit{}},
        };
        return keys(k);
    }
};

static_assert(Program<Show>);

int main() { return run<Show>({.title = "maya editor widgets"}); }
