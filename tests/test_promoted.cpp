// test_promoted.cpp — Tests for inline mode resize robustness
//
// Exercises every widget type through width-change cycles to verify
// no content garbling, overflow, or crashes after resize.

#include <maya/maya.hpp>
#include <maya/widget/badge.hpp>
#include <maya/widget/breadcrumb.hpp>
#include <maya/widget/agentty/divider.hpp>
#include <maya/widget/input.hpp>
#include <maya/widget/agentty/markdown.hpp>
#include <maya/widget/modal.hpp>
#include <maya/widget/progress.hpp>
#include <maya/widget/select.hpp>
#include <maya/widget/agentty/spinner.hpp>
#include <maya/widget/table.hpp>
#include <maya/widget/toast.hpp>
// NDEBUG guard: CMake builds tests in Release (-O3 -DNDEBUG), which strips
// assert(). Undefine it here so this file's runtime asserts actually fire.
#undef NDEBUG
#include "agtest.hpp"
#include <print>
#include <string>
#include <vector>

using namespace maya;

// ============================================================================
// Helpers
// ============================================================================

static std::string get_row(const Canvas& canvas, int y) {
    std::string s;
    for (int x = 0; x < canvas.width(); ++x) {
        Cell c = canvas.get(x, y);
        if (c.character >= 0x20 && c.character < 0x7F)
            s += static_cast<char>(c.character);
        else if (c.character != U' ' && c.character != 0)
            s += '?';
        else
            s += ' ';
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

struct RR {
    int content_h;
    std::vector<std::string> rows;
};

RR render_elem(const Element& elem, int w, int h = 500,
               bool auto_h = true) {
    StylePool pool;
    Canvas canvas(std::max(1, w - 1), h, &pool);
    canvas.clear();
    render_tree(elem, canvas, pool, theme::native, auto_h);
    int ch = content_height(canvas);
    std::vector<std::string> rows;
    for (int y = 0; y < ch; ++y)
        rows.push_back(get_row(canvas, y));
    return {ch, std::move(rows)};
}

void assert_fits(const RR& r, int w, const char* ctx) {
    for (int y = 0; y < r.content_h; ++y) {
        if (static_cast<int>(r.rows[y].size()) >= w) {
            std::println("  FAIL [{}]: row {} overflows w={} (len={}): {}",
                         ctx, y, w, r.rows[y].size(), r.rows[y]);
            assert(false);
        }
    }
}

void assert_stable(const RR& a, const RR& b, const char* ctx) {
    if (a.content_h != b.content_h) {
        std::println("  FAIL [{}]: height changed {} → {}", ctx, a.content_h, b.content_h);
        assert(false);
    }
    for (int y = 0; y < a.content_h; ++y) {
        if (a.rows[y] != b.rows[y]) {
            std::println("  FAIL [{}]: row {} differs", ctx, y);
            std::println("    before: |{}|", a.rows[y]);
            std::println("    after:  |{}|", b.rows[y]);
            assert(false);
        }
    }
}

// Render at w1 → w2 → w1 and assert first == last
void cycle(const Element& elem, int w1, int w2, const char* ctx) {
    auto r1 = render_elem(elem, w1);
    auto r2 = render_elem(elem, w2);
    auto r3 = render_elem(elem, w1);
    assert_fits(r1, w1, ctx);
    assert_fits(r2, w2, ctx);
    assert_stable(r1, r3, ctx);
}

// ============================================================================
// Test: Every standalone widget survives resize cycle
// ============================================================================
TEST_CASE("all widgets resize") {
    std::println("=== test_all_widgets_resize ===");

    // Table
    {
        Table tbl({{"Key", 0}, {"Value", 0}});
        tbl.add_row({"Language", "C++26"});
        tbl.add_row({"Compiler", "g++-15"});
        tbl.add_row({"Framework", "Maya TUI"});
        cycle(tbl, 80, 40, "table-80-40");
        cycle(tbl, 120, 30, "table-120-30");
        std::println("  table: ok");
    }

    // ProgressBar
    {
        ProgressBar bar;
        bar.set(0.75f);
        cycle(bar, 80, 40, "progress-80-40");
        cycle(bar, 120, 20, "progress-120-20");
        std::println("  progress: ok");
    }

    // Divider
    {
        Divider plain;
        Divider labeled("Section Title");
        cycle(plain, 80, 40, "divider-plain");
        cycle(labeled, 80, 40, "divider-labeled");
        cycle(labeled, 120, 25, "divider-labeled-narrow");
        std::println("  divider: ok");
    }

    // Badge
    {
        auto b = Badge::tool("read_file");
        cycle(b, 80, 20, "badge");
        std::println("  badge: ok");
    }

    // Breadcrumb
    {
        Breadcrumb bc({"project", "src", "widget", "table.hpp"});
        cycle(bc, 80, 30, "breadcrumb");
        std::println("  breadcrumb: ok");
    }

    // Select
    {
        Select menu({"Option A", "Option B", "Long option with lots of text"});
        cycle(menu, 80, 25, "select");
        std::println("  select: ok");
    }

    // Toast
    {
        ToastManager toasts;
        toasts.push("File saved", ToastLevel::Success);
        toasts.push("Deprecated API", ToastLevel::Warning);
        cycle(toasts, 80, 30, "toast");
        std::println("  toast: ok");
    }

    // Spinner
    {
        Spinner spin;
        cycle(spin, 80, 20, "spinner");
        std::println("  spinner: ok");
    }

    // Input
    {
        Input inp;
        inp.set_value("hello world this is a long input");
        cycle(inp, 80, 20, "input-80-20");
        cycle(inp, 120, 40, "input-120-40");
        std::println("  input: ok");
    }

    // Markdown (various patterns)
    {
        auto md_cycle = [](const char* label, const char* src) {
            auto elem = markdown(src);
            cycle(elem, 80, 40, label);
            cycle(elem, 120, 30, label);
        };
        md_cycle("md-bold", "Hello **world** and *italic*");
        md_cycle("md-list", "- **Maya** framework\n- SIMD diffing\n- Flexbox");
        md_cycle("md-code", "```cpp\nint x = 42;\n```");
        md_cycle("md-quote", "> Important note\n> continued here");
        md_cycle("md-table", "| A | B |\n|---|---|\n| 1 | 2 |\n| 3 | 4 |");
        md_cycle("md-heading", "## Title\n\nBody text here.");
        md_cycle("md-full",
            "I'll help you with that!\n\n"
            "Here's a quick overview:\n\n"
            "- **Maya** is a C++26 TUI framework\n"
            "- It uses **compile-time** DSL for type-safe UI\n"
            "- SIMD-accelerated terminal diffing\n\n"
            "The framework is designed for **high-performance** apps.");
        std::println("  markdown: ok");
    }

    std::println("  PASS\n");
}

