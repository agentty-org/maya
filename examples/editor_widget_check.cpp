// examples/editor_widget_check.cpp — lifetime + render smoke test.
//
//   cmake --build build --target maya_editor_widget_check && ./build/maya_editor_widget_check
//
// Builds EVERY editor widget as a TEMPORARY (the exact pattern that dangles a
// `[=, this]` capture) and renders each at several widths via render_to_string.
// If a widget's deferred ComponentElement lambda captured `this`, the temporary
// is dead by paint time and this crashes (SIGSEGV / bad_alloc). Exit 0 == all
// widgets are lifetime-safe. Run under ASAN for use-after-scope detection.

#include <maya/maya.hpp>
#include <maya/print.hpp>
#include <maya/widget/search_result.hpp>

#include <maya/widget/code_view.hpp>
#include <maya/widget/editor_view.hpp>
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
#include <maya/widget/fuzzy_line.hpp>
#include <maya/widget/text_editor.hpp>
#include <maya/widget/key_caps.hpp>
#include <maya/widget/color_swatch.hpp>
#include <maya/widget/empty_state.hpp>
#include <maya/widget/segmented_control.hpp>
#include <maya/widget/status_progress.hpp>
#include <maya/widget/commit_graph.hpp>
#include <maya/widget/merge_conflict.hpp>
#include <maya/widget/staging_view.hpp>
#include <maya/widget/branch_picker.hpp>
#include <maya/widget/quick_pick.hpp>
#include <maya/widget/call_hierarchy.hpp>
#include <maya/widget/test_explorer.hpp>
#include <maya/widget/output_channel.hpp>
#include <maya/widget/variables_tree.hpp>
#include <maya/widget/call_stack.hpp>
#include <maya/widget/history_timeline.hpp>
#include <maya/widget/settings_editor.hpp>
#include <maya/widget/extensions_list.hpp>
#include <maya/widget/tooltip.hpp>
#include <maya/widget/multi_cursor_strip.hpp>
#include <maya/widget/zen_layout.hpp>
#include <maya/widget/grid_split.hpp>
#include <maya/widget/inlay_hint_line.hpp>
#include <maya/widget/watch_panel.hpp>
#include <maya/widget/code_lens.hpp>
#include <maya/widget/snippet_preview.hpp>
#include <maya/widget/progress_ring.hpp>
#include <maya/widget/notification_badge.hpp>
#include <maya/widget/status_items.hpp>
#include <maya/widget/word_count.hpp>
#include <maya/widget/comment_block.hpp>
#include <maya/widget/git_lens_inline.hpp>
#include <maya/widget/folded_region.hpp>
#include <maya/widget/change_bar.hpp>
#include <maya/widget/sash.hpp>
#include <maya/widget/drop_indicator.hpp>
#include <maya/widget/option_picker.hpp>
#include <maya/widget/selection_info.hpp>
#include <maya/widget/ruler_guide.hpp>
#include <maya/widget/checkbox_list.hpp>
#include <maya/widget/tag_input.hpp>
#include <maya/widget/color_picker_grid.hpp>
#include <maya/widget/hunk_controls.hpp>
#include <maya/widget/keybinding_capture.hpp>
#include <maya/widget/blame_heatmap.hpp>
#include <maya/widget/tree_filter_bar.hpp>
#include <maya/widget/indent_scope_guides.hpp>

#include <cstdio>
#include <functional>
#include <vector>

using namespace maya;
using namespace maya::dsl;

// Each factory returns a freshly-built widget Element from a TEMPORARY widget.
static std::vector<std::pair<const char*, std::function<Element()>>> factories() {
    return {
        {"EditorView", []{ static std::vector<std::string> L={"int x;","  y();"}; static int s=0; EditorView ev; ev.lines=L; ev.row=1; ev.col=2; ev.lang=syntax::Lang::Cpp; ev.scroll=&s; return ev | grow(1); }},
        {"CodeView", []{ return CodeView{"int x = 1;\n  return x;\n", {.lang=syntax::Lang::Cpp}}
                            .set_caret(1,4); }},
        {"Minimap", []{ Minimap m{{.width=16,.lang=syntax::Lang::Cpp}}; m.set_source("a\n  b\n c\n");
                        m.set_viewport(1,2).set_active_line(1); return m | width(16) | height(6); }},
        {"OverviewRuler", []{ OverviewRuler r; r.total(40).viewport(3,12).cursor(6)
                              .mark(4,RulerMark::Error); return r | width(2) | height(10); }},
        {"EditorTabBar", []{ EditorTabBar t; t.tab({.name="a.cpp",.modified=true})
                             .tab({.name="b.rs",.diag=TabDiag::Error}).active(0); return Element{t}; }},
        {"Breadcrumb", []{ Breadcrumb b; b.crumb(SymKind::File,"a.cpp").crumb(SymKind::Class,"X"); return Element{b}; }},
        {"SymbolOutline", []{ SymbolOutline o; o.node(SymKind::Class,"X",0)
                              .node(SymKind::Method,"m",1,"()").active(1); return Element{o}; }},
        {"CompletionMenu", []{ CompletionMenu c; c.item({SymKind::Method,"at","()","doc",{0}})
                               .item({SymKind::Field,"w",":int"}).select(0); return c | width(40); }},
        {"SignatureHelp", []{ SignatureHelp s{"f","int"}; s.param("int a").param("int b").active(1)
                              .overload(1,2).doc("d"); return s | width(40); }},
        {"CommandPalette", []{ CommandPalette p; p.query="fm"; p.command({SymKind::Function,"Format","Editor","F"})
                               .select(0); return p | width(48); }},
        {"SearchResult", []{ SearchResult r{SearchKind::Grep, "x"}; r.set_expanded(true);
                             r.add_group({"src/a.cpp", {{3, "x = 1;"}}}); return r.build() | width(48); }},
        {"SearchResults", []{ SearchResults s; s.file("a.cpp",1).match(4,"  ","x"," ").active(0); return s | width(48); }},
        {"ContextMenu", []{ ContextMenu m; m.item("Go","F12").separator().check("Wrap",true)
                            .submenu("More").active(0); return m | width(36); }},
        {"DebugToolbar", []{ return Element{DebugToolbar{DebugToolbar::Paused}}; }},
        {"PeekView", []{ PeekView p; p.title("refs").location("a:1")
                         .body(CodeView{"x\n",{.lang=syntax::Lang::Cpp}}); return p | width(50); }},
        {"Toolbar", []{ Toolbar t; t.button("s","Save").toggle("w","Wrap",true).sep().disabled("d"); return Element{t}; }},
        {"PanelTabs", []{ PanelTabs p; p.tab("PROBLEMS",3).tab("OUTPUT").active(0); return Element{p}; }},
        {"MarkerGutter", []{ MarkerGutter g; g.line(Marker::Breakpoint).line(Marker::Execution); return g | width(3); }},
        {"DiffStat", []{ DiffStat d; d.file("very/long/path/name.cpp",42,8).file("b.hpp",1,0); return d | width(40); }},
        {"QuickInput", []{ QuickInput q; q.label("Rename").value("w").error("nope"); return q | width(40); }},
        {"WhichKeyMenu", []{ WhichKeyMenu w{"SPC"}; w.key("f","file").key("g","git",true); return w | width(40); }},
        {"NotificationStack", []{ NotificationStack n; n.toast(NotificationStack::Progress,"Idx","x",0.4f)
                                  .toast(NotificationStack::Error,"Fail","boom"); return n | width(40); }},
        {"TerminalPane", []{ TerminalPane t; t.prompt("~","ls").out("a").err("e").input("~","cd"); return Element{t}; }},
        {"DiffHunkView", []{ DiffHunkView d; d.hunk("@@ -1 +1 @@").del("a",1).add("b",1); return Element{d}; }},
        {"ProblemsPanel", []{ ProblemsPanel p; p.file("a.cpp",true).problem(Severity::Error,"m",1,1,"E").active(1); return p | width(50); }},
        {"HoverCard", []{ HoverCard h; h.signature("sig").doc("doc").note("n"); return h | width(40); }},
        {"FileTree", []{ FileTree t; t.root("r"); t.folder("src",0); t.file("x.cpp",1).git(GitState::Modified)
                         .diag(2,1); t.file("aVeryLongFileName.controller.ts",1).symlink("../t.ts"); t.filter("x")
                         .active(1); return t | width(24); }},
        {"GitBlameGutter", []{ GitBlameGutter g{{.width=20}}; g.line("Ada","1d ago","abc").uncommitted().active(0); return g | width(20); }},
        {"EditorStatusLine", []{ EditorStatusLine s; s.left(EditorStatusLine::mode("N")).right(EditorStatusLine::pos(1,1,9)); return s | width(60); }},
        {"DiagnosticLens", []{ return Element{DiagnosticLens{Severity::Error,"m"}.at(4,3)}; }},
        {"FuzzyLine", []{ auto r=FuzzyLine::match("src/a.cpp","a"); return FuzzyLine{"src/a.cpp",r.positions}.hint("f"); }},
        {"TextEditor", []{ TextEditor e{{.lang=syntax::Lang::Cpp}}; e.set_text("int x;\n  y();\n"); return e | grow(1); }},
        {"KeyCaps", []{ return Element{KeyCaps{"Ctrl","Shift","P"}}; }},
        {"ColorSwatch", []{ ColorSwatch c; c.color("m",0xCBA6F7).color("g",0xA6E3A1); return Element{c}; }},
        {"EmptyState", []{ EmptyState e; e.glyph("x").title("none").hint("h").action("C","a"); return e | grow(1); }},
        {"SegmentedControl", []{ SegmentedControl s; s.seg("A").seg("B").active(0); return Element{s}; }},
        {"StatusProgress", []{ return Element{StatusProgress{}.label("x").value(0.4f)}; }},
        {"CommitGraph", []{ CommitGraph g; g.commit(0,"a1c","m",{"HEAD"}).merge(0,1,"b2d","mg"); return Element{g}; }},
        {"MergeConflict", []{ MergeConflict c; c.ours("H",{"a"}).theirs("T",{"b"}); return Element{c}; }},
        {"StagingView", []{ StagingView s; s.staged("M","a.cpp").change("U","b.txt").active(0); return s | width(40); }},
        {"BranchPicker", []{ BranchPicker b; b.branch("main",true,0,0,"h").remote("origin/main","x").active(0); return b | width(48); }},
        {"QuickPick", []{ QuickPick q; q.query="a"; q.item("a.cpp","src").select(0).preview({"x"}); return q | width(70); }},
        {"CallHierarchy", []{ CallHierarchy h; h.root(SymKind::Method,"f").call(SymKind::Function,"g",1,"a:1"); return h | width(50); }},
        {"TestExplorer", []{ TestExplorer t; t.suite("S",1,2).test(TestStatus::Passed,"a",1,"1ms").test(TestStatus::Failed,"b",1); return t | width(48); }},
        {"OutputChannel", []{ OutputChannel o{"c"}; o.info("a").warn("b").error("c","12:00"); return o | grow(1); }},
        {"VariablesTree", []{ VariablesTree v; v.scope("L").var("i","3",VarKind::Number,1,true,true).var("s","\"x\"",VarKind::String,2,false,false,true); return v | width(44); }},
        {"CallStack", []{ CallStack c; c.frame("f","a:1",true).frame("g","libc",false,true); return c | width(48); }},
        {"HistoryTimeline", []{ HistoryTimeline t; t.event(TimelineKind::Commit,"m","2h","a1c").event(TimelineKind::Save,"s","3h"); return t | width(46); }},
        {"SettingsEditor", []{ SettingsEditor s; s.query="e"; s.toggle("a","A","d",true).number("b","B","d","1").choice("c","C","","x").active(0); return s | width(64); }},
        {"ExtensionsList", []{ ExtensionsList e; e.ext("a","1","x","desc",4.5f,true,true).ext("b","2","y","d",3.0f,false,false).active(0); return e | width(50); }},
        {"Tooltip", []{ return Element{Tooltip{"Go to Definition"}.shortcut("F12")}; }},
        {"MultiCursorStrip", []{ return Element{MultiCursorStrip{}.cursors(3).selections(2).selected_chars(48)}; }},
        {"ZenLayout", []{ return ZenLayout{}.max_width(40).content(text("hi")) | grow(1); }},
        {"GridSplit", []{ GridSplit g; g.pane(0,text("a"),"a").pane(1,text("b"),"b").pane(2,text("c"),"c").pane(3,text("d"),"d").focus(0); return g | grow(1); }},
        {"InlayHintLine", []{ return Element{InlayHintLine{}.code("auto x").hint(": int").code(" = 1;")}; }},
        {"WatchPanel", []{ WatchPanel w; w.watch("e","8",VarKind::Number).error("f","err").active(0); return w | width(46); }},
        {"CodeLens", []{ return Element{CodeLens{}.info("3 refs").action("Run")}; }},
        {"SnippetPreview", []{ return Element{SnippetPreview{}.lit("a").stop(1,"i").lit("b").active(1)}; }},
        {"ProgressRing", []{ return Element{ProgressRing{}.value(0.5f).label("x")}; }},
        {"NotificationBadge", []{ return Element{NotificationBadge{}.icon("b").label("P").count(3)}; }},
        {"StatusItems", []{ return Element{StatusItems{}.item("e","0").item("","UTF-8")}; }},
        {"WordCount", []{ return Element{WordCount::of("a b c\nd e\n")}; }},
        {"CommentBlock", []{ CommentBlock c; c.line("/// @param i the `index`").line("/// @return x"); return Element{c}; }},
        {"GitLensInline", []{ return Element{GitLensInline{"return x;"}.blame("A","2d","fix")}; }},
        {"FoldedRegion", []{ return Element{FoldedRegion{"struct X {"}.hidden(14)}; }},
        {"ChangeBar", []{ ChangeBar b; b.line(Change::Added).line(Change::Deleted).line(Change::None); return b | width(1); }},
        {"Sash", []{ return Sash{}.vertical().active(true) | height(6); }},
        {"DropIndicator", []{ return Element{DropIndicator{}.label("drop")}; }},
        {"OptionPicker", []{ OptionPicker p; p.option("LF","d").option("CRLF").select(0).focus(0); return p | width(48); }},
        {"SelectionInfo", []{ return Element{SelectionInfo{}.at(42,7).selection(3,128)}; }},
        {"RulerGuide", []{ RulerGuide r{20}; r.line("int x=1;").line("auto really_long_thing = compute();"); return Element{r}; }},
        {"CheckboxList", []{ CheckboxList c; c.item("a",true,"h").item("b",false).focus(0); return c | width(48); }},
        {"TagInput", []{ return Element{TagInput{}.tag("bug").tag("ui").input("perf")}; }},
        {"ColorPickerGrid", []{ ColorPickerGrid g; for(uint32_t h:{0xF38BA8u,0xA6E3A1u,0x89B4FAu}) g.color(h); g.columns(4).select(1); return Element{g}; }},
        {"HunkControls", []{ return Element{HunkControls{"@@ -1 +1 @@"}.staged(true)}; }},
        {"KeybindingCapture", []{ return KeybindingCapture{}.recording(true).keys({"Ctrl","K"}) | width(40); }},
        {"BlameHeatmap", []{ BlameHeatmap b; b.line(0).line(30).line(900); return b | width(1); }},
        {"TreeFilterBar", []{ TreeFilterBar f; f.query="x"; f.matches=3; f.total=40; f.fuzzy=true; return Element{f}; }},
        {"IndentScopeGuides", []{ IndentScopeGuides g; g.line("f(){",0).line("  a();",1).line("}",0).active(1,1,1); return Element{g}; }},
    };
}

int main() {
    const int widths[] = {1, 2, 8, 24, 60, 200};
    int fail = 0;
    for (auto& [name, make] : factories()) {
        for (int w : widths) {
            Element e = make();              // temporary widget → Element
            volatile auto s = maya::render_to_string(e, w).size();
            (void)s;
        }
        std::printf("  ok  %s\n", name);
    }
    std::printf(fail ? "FAIL\n" : "all widgets lifetime-safe\n");
    return fail;
}
