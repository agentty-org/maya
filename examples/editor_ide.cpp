// examples/editor_ide.cpp — the editor widgets composed as a real workspace.
//
//   cmake --build build --target maya_editor_ide && ./build/maya_editor_ide
//
// A symbol-outline sidebar, a code view with an inline git-blame column and an
// overview ruler, a floating completion popup, and a status line — the newer
// widgets shown in a believable layout rather than a stacked gallery.
//
// Keys: ↑/↓ or j/k move the completion selection, q quits.

#include <maya/host/run.hpp>
#include <maya/maya.hpp>
#include <maya/widget/code_view.hpp>
#include <maya/widget/git_blame_gutter.hpp>
#include <maya/widget/overview_ruler.hpp>
#include <maya/widget/symbol_outline.hpp>
#include <maya/widget/agentty/completion_menu.hpp>
#include <maya/widget/editor_tab_bar.hpp>
#include <maya/widget/breadcrumb_bar.hpp>
#include <maya/widget/editor_status_line.hpp>

#include <string>

using namespace maya;
using namespace maya::dsl;

static const std::string kSrc =
    "#pragma once\n"
    "#include <string>\n"
    "\n"
    "// A persistent rope buffer.\n"
    "struct Rope {\n"
    "    std::string text;\n"
    "    Rope*       left  = nullptr;\n"
    "    Rope*       right = nullptr;\n"
    "    std::size_t weight = 0;\n"
    "\n"
    "    char at(std::size_t i) const {\n"
    "        if (i < weight && left) return left->at(i);\n"
    "        return text[i - weight];\n"
    "    }\n"
    "};\n";

struct IDE {
    struct Model { int sel = 2; };
    struct Up{}; struct Down{}; struct Quit{};
    using Msg = std::variant<Up, Down, Quit>;

    using Cmd = jaal::Cmd<Msg>;

    static Cmd update(Model&, Quit)   { return Cmd::quit(0); }
    static Cmd update(Model& m, Up)   { m.sel = std::max(0, m.sel - 1); return {}; }
    static Cmd update(Model& m, Down) { m.sel = std::min(5, m.sel + 1); return {}; }

    static Element view(const Model& m) {
        auto label = [](std::string_view s) {
            return text(std::string(s)) | fgc(Color::hex(0x585B70)) | Bold;
        };

        // ── tab bar ─────────────────────────────────────────────────────────
        EditorTabBar tb;
        tb.tab({.name = "rope.hpp", .modified = true})
          .tab({.name = "rope.cpp"})
          .tab({.name = "editor.rs", .diag = TabDiag::Warning})
          .active(0);

        // ── left sidebar: symbol outline ────────────────────────────────────
        SymbolOutline outline;
        outline.node(SymKind::File,   "rope.hpp", 0)
               .node(SymKind::Struct, "Rope", 0)
               .node(SymKind::Field,  "text",   1, ": string")
               .node(SymKind::Field,  "weight", 1, ": size_t")
               .node(SymKind::Method, "at",     1, "(size_t)")
               .active(4);
        auto sidebar = v(label(" OUTLINE"), outline) | width(26);

        // ── center: breadcrumb + (blame | code | ruler) ─────────────────────
        Breadcrumb bc;
        bc.crumb(SymKind::File, "rope.hpp")
          .crumb(SymKind::Struct, "Rope")
          .crumb(SymKind::Method, "at");

        CodeView cv{kSrc, {.lang = syntax::Lang::Cpp}};
        cv.set_caret(11, 8).mark(6, LineMark::Added).mark(7, LineMark::Added);

        GitBlameGutter blame{{.width = 18}};
        blame.line("Ayush Bhat", "1 hour ago", "a1c2f3e")
             .line("Ayush Bhat", "1 hour ago", "a1c2f3e")
             .uncommitted()
             .line("Ada Lovelace", "3 mo ago", "9f0b12d")
             .line("Ada Lovelace", "3 mo ago", "9f0b12d")
             .line("Grace Hopper", "1 yr ago", "77aa10c")
             .line("Grace Hopper", "1 yr ago", "77aa10c")
             .line("Grace Hopper", "1 yr ago", "77aa10c")
             .line("Grace Hopper", "1 yr ago", "77aa10c")
             .line("Ken Thompson", "2 yr ago", "0b1d2e3")
             .line("Ken Thompson", "2 yr ago", "0b1d2e3")
             .line("Ken Thompson", "2 yr ago", "0b1d2e3")
             .line("Ken Thompson", "2 yr ago", "0b1d2e3")
             .line("Ken Thompson", "2 yr ago", "0b1d2e3")
             .line("Ken Thompson", "2 yr ago", "0b1d2e3")
             .active(10);

        OverviewRuler ruler;
        ruler.total(cv.line_count()).viewport(1, cv.line_count()).cursor(11)
             .mark(6, RulerMark::Change).mark(7, RulerMark::Change)
             .mark(11, RulerMark::Search);

        auto editor = v(
            label(" rope.hpp"),
            bc,
            h(blame | width(18), cv | grow(1), ruler | width(2)) | grow(1)
        ) | grow(1);

        // ── floating completion popup, anchored under the caret ─────────────
        CompletionMenu cm;
        cm.item({SymKind::Method, "at",     "(size_t) -> char", "Random access by index."})
          .item({SymKind::Field,  "weight", ": std::size_t",    "Length of the left subtree."})
          .item({SymKind::Field,  "text",   ": std::string",    "Leaf payload."})
          .item({SymKind::Method, "concat", "(Rope*, Rope*)",   "Join two ropes."})
          .item({SymKind::Field,  "left",   ": Rope*"})
          .item({SymKind::Field,  "right",  ": Rope*"})
          .select(m.sel);
        auto popup = (cm | width(44)) | padding(0, 0, 0, 30);

        // ── status line ─────────────────────────────────────────────────────
        EditorStatusLine sl;
        sl.left(EditorStatusLine::mode("INSERT"))
          .left(EditorStatusLine::branch("main"))
          .left(EditorStatusLine::file("rope.hpp", true))
          .right(EditorStatusLine::info("spaces: 4"))
          .right(EditorStatusLine::lang("C++"))
          .right(EditorStatusLine::pos(11, 8, cv.line_count()));

        return v(
            tb,
            h(sidebar, editor) | grow(1),
            popup,
            sl
        ) | grow(1);
    }

    static std::optional<Msg> on(const Model&, const KeyEvent& k) {
        static const keymap<Msg> keys{
            {SpecialKey::Up, Up{}},   {'k', Up{}},
            {SpecialKey::Down, Down{}}, {'j', Down{}},
            {'q', Quit{}}, {SpecialKey::Escape, Quit{}},
        };
        return keys(k);
    }
};

static_assert(Program<IDE>);

int main() { return run<IDE>({.title = "maya editor — IDE layout"}); }
