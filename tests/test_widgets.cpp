// test_widgets.cpp — Verify all widgets render correctly at various widths
//
// ASSERTS MUST BE LIVE HERE. The test suite builds Release (-DNDEBUG), which
// silently compiles out every assert() below — the file was green while
// checking NOTHING. Undef NDEBUG before <cassert> so the checks run in every
// build type. (New checks should still prefer MAYA_TEST_CHECK from check.hpp,
// which never depended on NDEBUG.)
#ifdef NDEBUG
#  undef NDEBUG
#endif
#include <cassert>

#include <maya/maya.hpp>
#include <maya/widget/badge.hpp>
#include <maya/widget/agent_timeline.hpp>
#include <maya/widget/breadcrumb.hpp>
#include <maya/widget/composer.hpp>
#include <maya/widget/context_gauge.hpp>
#include <maya/widget/divider.hpp>
#include <maya/widget/input.hpp>
#include <maya/widget/markdown.hpp>
#include <maya/widget/search_result.hpp>
#include <maya/widget/modal.hpp>
#include <maya/widget/model_badge.hpp>
#include <maya/widget/panel.hpp>
#include <maya/widget/peek_card.hpp>
#include <maya/widget/progress.hpp>
#include <maya/widget/reasoning.hpp>
#include <maya/widget/select.hpp>
#include <maya/widget/spinner.hpp>
#include <maya/widget/status_bar.hpp>
#include <maya/widget/table.hpp>
#include <maya/widget/toast.hpp>
#include <maya/element/text.hpp>
#include "check.hpp"
#include "agtest.hpp"
#include <print>
#include <string>
#include <vector>

using namespace maya;

static std::string get_row(const Canvas& canvas, int y) {
    std::string s;
    for (int x = 0; x < canvas.width(); ++x) {
        Cell c = canvas.get(x, y);
        if (c.character >= 0x20 && c.character < 0x7F)
            s += static_cast<char>(c.character);
        else if (c.character == 0x2500)
            s += '-';
        else if (c.character == 0x25C6)
            s += '*';
        else if (c.character == 0x276F)
            s += '>';
        else if (c.character == 0x25CF)
            s += '@';
        else if (c.character == 0x2588)
            s += '#';
        else if (c.character != U' ' && c.character != 0)
            s += '?';
        else
            s += ' ';
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

struct RenderResult {
    std::vector<std::string> rows;
    int content_h;
};

RenderResult render_at(const Element& elem, int width, int height = 500,
                       bool auto_h = true) {
    StylePool pool;
    Canvas canvas(width, height, &pool);
    render_tree(elem, canvas, pool, theme::native, auto_h);
    int ch = content_height(canvas);
    std::vector<std::string> rows;
    for (int y = 0; y < ch; ++y)
        rows.push_back(get_row(canvas, y));
    return {rows, ch};
}

void dump(const std::string& label, const RenderResult& r) {
    std::println("  {} ({} rows):", label, r.content_h);
    for (int y = 0; y < r.content_h; ++y)
        std::println("    {:2}|{}|", y, r.rows[y]);
}

// Assert no row exceeds the canvas width
void assert_fits(const RenderResult& r, int max_w, const char* ctx) {
    for (int y = 0; y < r.content_h; ++y) {
        if (static_cast<int>(r.rows[y].size()) > max_w) {
            std::println("  FAIL [{}]: row {} exceeds width {} (len={}): {}",
                         ctx, y, max_w, r.rows[y].size(), r.rows[y]);
            assert(false);
        }
    }
}

// Assert rendering at width w1, then w2, then w1 produces same result
void assert_resize_stable(const Element& elem, int w1, int w2, const char* ctx) {
    auto r1 = render_at(elem, w1);
    auto r2 = render_at(elem, w2);
    auto r3 = render_at(elem, w1);
    assert(r1.content_h == r3.content_h);
    for (int y = 0; y < r1.content_h; ++y) {
        if (r1.rows[y] != r3.rows[y]) {
            std::println("  FAIL [{}]: row {} differs after {}→{}→{}", ctx, y, w1, w2, w1);
            std::println("    before: |{}|", r1.rows[y]);
            std::println("    after:  |{}|", r3.rows[y]);
            assert(false);
        }
    }
    (void)r2;
}

// ============================================================================
// Table tests
// ============================================================================
TEST_CASE("table") {
    std::println("=== test_table ===");

    Table tbl({{"Property", 0}, {"Value", 0}});
    tbl.add_row({"Language", "C++"});
    tbl.add_row({"Standard", "C++26"});
    tbl.add_row({"Compiler", "g++-15"});

    for (int w : {20, 30, 40, 60, 80, 120}) {
        auto r = render_at(tbl, w);
        assert(r.content_h > 0);
        assert_fits(r, w, "table");
    }
    assert_resize_stable(tbl, 80, 40, "table");

    // Regression: a host-supplied `edge` gutter marker wider than one
    // display column (emoji, CJK, or multi-glyph) must NOT shear the row's
    // columns rightward. The gutter is a fixed 2-column budget; the edge is
    // clamped to exactly one column in build_row. Build a selectable table
    // where a NON-cursor row carries a 2-column emoji edge and assert every
    // data column starts at the same canvas x as on a plain-edge row.
    {
        TableConfig cfg;
        cfg.selectable   = true;
        cfg.show_header  = false;
        cfg.show_separator = false;
        cfg.stripe_rows  = false;
        Table t({{"A", 8}, {"B", 8}}, cfg);
        TableRow r0({"aaa", "bbb"});           // cursor row (row 0)
        TableRow r1({"ccc", "ddd"});
        r1.edge = "\xf0\x9f\x94\xa5";          // 🔥 U+1F525 (2 display cols)
        TableRow r2({"eee", "fff"});
        r2.edge = "x";                          // 1-column edge (reference)
        t.set_rows({r0, r1, r2});
        t.set_selected(0);

        StylePool pool;
        Canvas canvas(40, 8, &pool);
        render_tree(t.build(), canvas, pool, theme::native, true);

        // Find the x of the first non-space glyph on the emoji-edge row (1)
        // and the plain-edge row (2). After the fix they must match: the
        // gutter is one column + one space regardless of the edge glyph's
        // width, so the first data cell lands at the same column.
        auto first_cell_x = [&](int y) {
            for (int x = 2; x < canvas.width(); ++x) {  // skip 2-col gutter
                char32_t c = canvas.get(x, y).character;
                if (c >= 'a' && c <= 'z') return x;
            }
            return -1;
        };
        int x_emoji = first_cell_x(1);
        int x_plain = first_cell_x(2);
        if (x_emoji != x_plain || x_emoji < 0) {
            std::println("  FAIL [table-wide-edge]: data column sheared "
                         "(emoji row x={}, plain row x={})", x_emoji, x_plain);
            assert(false);
        }
    }

    std::println("  PASS\n");
}

// ============================================================================
// ProgressBar tests
// ============================================================================
TEST_CASE("progress") {
    std::println("=== test_progress ===");

    ProgressBar bar;
    bar.set(0.5f);

    for (int w : {20, 40, 60, 80, 120}) {
        auto r = render_at(bar, w);
        assert(r.content_h > 0);
        assert_fits(r, w, "progress");
    }
    assert_resize_stable(bar, 80, 40, "progress");

    std::println("  PASS\n");
}

// ============================================================================
// Divider tests
// ============================================================================
TEST_CASE("divider") {
    std::println("=== test_divider ===");

    Divider plain;
    Divider labeled("Section");

    for (int w : {20, 40, 80, 120}) {
        auto r1 = render_at(plain, w);
        auto r2 = render_at(labeled, w);
        assert(r1.content_h > 0);
        assert(r2.content_h > 0);
        assert_fits(r1, w, "divider-plain");
        assert_fits(r2, w, "divider-labeled");
    }
    assert_resize_stable(plain, 80, 40, "divider-plain");
    assert_resize_stable(labeled, 80, 40, "divider-labeled");

    std::println("  PASS\n");
}

// ============================================================================
// Badge tests
// ============================================================================
TEST_CASE("badge") {
    std::println("=== test_badge ===");

    auto b = Badge::tool("read_file");
    for (int w : {15, 20, 40, 80}) {
        auto r = render_at(b, w);
        assert(r.content_h > 0);
        assert_fits(r, w, "badge");
    }

    std::println("  PASS\n");
}

// ============================================================================
// Breadcrumb tests
// ============================================================================
TEST_CASE("breadcrumb") {
    std::println("=== test_breadcrumb ===");

    Breadcrumb bc({"project", "src", "widget", "table.hpp"});
    for (int w : {20, 40, 80, 120}) {
        auto r = render_at(bc, w);
        assert(r.content_h > 0);
        assert_fits(r, w, "breadcrumb");
    }
    assert_resize_stable(bc, 80, 40, "breadcrumb");

    std::println("  PASS\n");
}

// ============================================================================
// Select tests
// ============================================================================
TEST_CASE("select") {
    std::println("=== test_select ===");

    Select menu({"Option A", "Option B", "Long option with lots of text"});
    for (int w : {20, 40, 80}) {
        auto r = render_at(menu, w);
        assert(r.content_h > 0);
        assert_fits(r, w, "select");
    }
    assert_resize_stable(menu, 80, 30, "select");

    std::println("  PASS\n");
}

// ============================================================================
// Toast tests
// ============================================================================
TEST_CASE("toast") {
    std::println("=== test_toast ===");

    ToastManager toasts;
    toasts.push("File saved", ToastLevel::Success);
    toasts.push("Warning: deprecated API", ToastLevel::Warning);

    for (int w : {30, 60, 80}) {
        auto r = render_at(toasts, w);
        assert(r.content_h > 0);
        assert_fits(r, w, "toast");
    }

    std::println("  PASS\n");
}

// ============================================================================
// Spinner tests
// ============================================================================
TEST_CASE("spinner") {
    std::println("=== test_spinner ===");

    Spinner spin;
    for (int w : {10, 20, 40, 80}) {
        auto r = render_at(spin, w);
        assert(r.content_h > 0);
        assert_fits(r, w, "spinner");
    }

    std::println("  PASS\n");
}

// ============================================================================
// The animation purity contract
// ============================================================================
// maya's animation doctrine is LOGICAL purity, not mechanical constness:
// build()/get() may cache, prune, and request frames internally, but the
// rendered frame must be a pure function of (model state, shared animation
// clock). This test enforces the contract for the clock-driven widgets:
//
//   1. same state + same frozen clock  ⇒ byte-identical frames, repeatedly
//      (the internal mutation is invisible — idempotent housekeeping only);
//   2. advancing the clock a full step  ⇒ the frame CHANGES (the clock is
//      the only time input — nothing else is secretly driving phase).
//
// If a widget ever grows hidden per-instance animation state again (the
// dt-accumulator regression this system removed), case 1 or 2 breaks.
TEST_CASE("animation purity: frozen clock pins every widget frame") {
    std::println("=== test_animation_purity ===");
    maya::testing::freeze_anim_clock(5'000'000);

    {   // Spinner: pure function of the clock, stable across rebuilds.
        Spinner a, b;
        auto r1 = render_at(a, 20);
        auto r2 = render_at(a, 20);
        auto r3 = render_at(b, 20);   // ANY instance — lockstep by design
        assert(r1.rows == r2.rows && "same clock ⇒ same spinner frame");
        assert(r1.rows == r3.rows && "all spinners step in lockstep");
        assert(a.current_frame() == b.current_frame()
               && "lockstep down to the glyph");
        const auto f0 = a.frame_index();
        const auto g0 = std::string{a.current_frame()};
        maya::testing::advance_anim_clock_ms(1000);  // >> one 80ms step
        assert(a.frame_index() != f0 && "clock is the only phase input");
        assert(std::string{a.current_frame()} != g0
               && "advancing the clock moves the glyph");
        // (rendered rows are compared via current_frame(): the ASCII test
        //  canvas folds all braille frames to the same '?' placeholder.)
    }

    {   // Toasts: deadline expiry against the same clock, idempotent build.
        ToastManager toasts;
        toasts.push("hello", ToastLevel::Info);       // 3s duration
        auto r1 = render_at(toasts, 40);
        auto r2 = render_at(toasts, 40);
        assert(r1.rows == r2.rows && "frozen clock ⇒ stable toast frame");
        assert(!toasts.empty());
        maya::testing::advance_anim_clock_ms(10'000); // past expiry
        (void)render_at(toasts, 40);                  // build prunes
        assert(toasts.empty() && "toast expires on the shared clock");
    }

    maya::testing::unfreeze_anim_clock();
    std::println("  PASS\n");
}

// ============================================================================
// Input tests
// ============================================================================
TEST_CASE("input") {
    std::println("=== test_input ===");

    Input inp;
    inp.set_value("hello world this is a long input text");
    for (int w : {15, 30, 60, 80}) {
        auto r = render_at(inp, w);
        assert(r.content_h > 0);
        assert_fits(r, w, "input");
    }
    assert_resize_stable(inp, 80, 30, "input");

    std::println("  PASS\n");
}

// ============================================================================
// Markdown tests
// ============================================================================
TEST_CASE("markdown") {
    std::println("=== test_markdown ===");

    auto test_md = [](const char* label, const char* src) {
        auto elem = markdown(src);
        for (int w : {40, 80, 120}) {
            auto r = render_at(elem, w);
            assert(r.content_h > 0);
            assert_fits(r, w, label);
        }
        assert_resize_stable(elem, 80, 40, label);
    };

    test_md("simple bold", "Hello **world**");
    test_md("two paragraphs", "First para\n\nSecond **bold** para");
    test_md("list plain", "- Item one\n- Item two");
    test_md("list with bold", "- **Maya** is a framework\n- Item two");
    test_md("code block", "```cpp\nint x = 1;\n```");
    test_md("blockquote", "> This is a quote\n> with two lines");
    test_md("heading", "## Heading\n\nParagraph text");
    test_md("table", "| A | B |\n|---|---|\n| 1 | 2 |");
    test_md("exclamation", "Hello! World! Done!");
    test_md("full response",
        "I'll help you with that!\n\n"
        "Here's a quick overview:\n\n"
        "- **Maya** is a C++26 TUI framework\n"
        "- It uses **compile-time** DSL for type-safe UI\n"
        "- SIMD-accelerated terminal diffing\n"
        "- Flexbox layout engine\n\n"
        "The framework is designed for **high-performance** terminal applications.");

    std::println("  PASS\n");
}

// ============================================================================
// Streaming markdown tests
// ============================================================================
TEST_CASE("markdown streaming") {
    std::println("=== test_markdown_streaming ===");

    std::vector<std::string> tokens = {
        "I", "'ll", " help", " you", " with", " that", "!\n\n",
        "Here", "'s", " a", " quick", " overview", ":\n\n",
        "- ", "**Maya**", " is", " a", " C++26", " TUI", " framework", "\n",
        "- ", "It", " uses", " **compile-time**", " DSL", " for", " type-safe", " UI", "\n",
        "- ", "SIMD", "-accelerated", " terminal", " diffing", "\n",
        "- ", "Flexbox", " layout", " engine", "\n\n",
        "The", " framework", " is", " designed", " for", " **high-performance**",
        " terminal", " applications", ".",
    };

    StreamingMarkdown md;
    for (auto& tok : tokens) {
        md.append(tok);
        auto elem = md.build();
        auto r = render_at(elem, 80);
        assert(r.content_h > 0);
    }
    md.finish();
    auto elem = md.build();
    auto r = render_at(elem, 80);
    assert(r.content_h >= 8);

    std::println("  {} tokens streamed, final: {} rows", tokens.size(), r.content_h);
    std::println("  PASS\n");
}

// ============================================================================
// Status bar responsiveness tests
// ============================================================================
//
// The activity row MUST be exactly 3 rows tall (top accent + 1 content
// row + bottom accent) at EVERY terminal width. If any piece wraps onto
// a phantom second line the whole status bar grows to 4 rows, shoving
// the Thread above it and triggering a full-viewport repaint. We also
// assert the most meaningful piece (the phase verb) survives down to a
// usably-wide terminal, and that no glyphs ever bleed past the right
// edge (every rendered row must fit within `width` columns).
TEST_CASE("status bar responsive") {
    std::println("=== test_status_bar_responsive ===");

    auto make = []() {
        StatusBar::Config cfg;
        cfg.phase_color        = Color::cyan();
        cfg.breadcrumb.title   = "refactor the responsive status bar layout";
        cfg.phase.glyph        = "\xe2\xa0\x8b";   // spinner
        cfg.phase.verb         = "Streaming";
        cfg.phase.color        = Color::bright_cyan();
        cfg.phase.verb_width   = 10;
        cfg.phase.elapsed_secs = 12.3f;
        cfg.model_badge        = ModelBadge{{.label = "Sonnet 4.5"}}.build();
        cfg.context.used       = 84'000;
        cfg.context.max        = 200'000;
        cfg.context.cells      = 10;
        return cfg;
    };

    // Sweep from absurdly narrow to very wide. The row count must be a
    // constant 3 and nothing may overflow the width at ANY of them.
    for (int w = 12; w <= 220; ++w) {
        Element el = StatusBar{make()}.build();
        auto r = render_at(el, w);
        assert(r.content_h == 3 && "status bar must stay 3 rows at every width");
        for (const auto& row : r.rows)
            assert(static_cast<int>(row.size()) <= w
                   && "status bar row must not overflow terminal width");
    }

    // The phase verb ("what's happening now") is the highest-value
    // signal: it must still be present at a normal-narrow 60-col width.
    {
        auto r = render_at(StatusBar{make()}.build(), 60);
        bool has_verb = false;
        for (const auto& row : r.rows)
            if (row.find("Streaming") != std::string::npos) has_verb = true;
        assert(has_verb && "phase verb must survive at 60 cols");
    }

    // ── NO SILENT CLIPPING (the "overlapped status bar" field artifact).
    // overflow:Hidden keeps the row 1 line tall, but if the ladder accepts
    // a shape wider than the terminal, the RIGHT group is what gets cut —
    // the context gauge dies mid-glyph ("… · C") while a lower-value
    // fragment (thread title) still shows. Invariant: the context gauge is
    // the rightmost fragment and always ends with '%'; at EVERY width, if
    // any activity-row content rendered at all, the trimmed row must end
    // with the intact gauge — clipping is never the mechanism that makes
    // the row fit. And the thread title must be the FIRST thing shed:
    // whenever the title survives, the full gauge must too.
    auto activity_row = [](const std::vector<std::string>& rows) {
        // Row 0/2 are the accent rules; row 1 is the activity strip.
        return rows.size() >= 2 ? rows[1] : std::string{};
    };
    auto rtrim = [](std::string s) {
        while (!s.empty() && s.back() == ' ') s.pop_back();
        return s;
    };
    for (int w = 24; w <= 220; ++w) {
        auto cfg = make();
        // A long model label + a long title — maximum squeeze pressure. The
        // host-style " · Provider" suffix widens the badge like agentty's
        // real status bar does. The label is passed already-decoded (the
        // widget is presentation-only; agentty's model_name SSOT does the
        // id → "Fable 1" decode before it reaches here).
        cfg.model_badge = h(ModelBadge{{.label = "Fable 1"}}.build(),
                            dsl::text(" \xc2\xb7 Anthropic")).build();
        auto r = render_at(StatusBar{cfg}.build(), w);
        const std::string row = rtrim(activity_row(r.rows));
        if (row.empty()) continue;   // sub-minimal width: fine, nothing shows
        // SHED, NEVER CLIP: a fragment is either fully present or fully
        // absent. "Streamin" without the g, "Anthropi", "CTX … 4" without
        // the % — each means the ladder admitted a shape wider than the
        // terminal and overflow:Hidden ate the excess mid-word (the field
        // "overlapped status bar" artifact). The leanest rungs legitimately
        // shed the gauge / verb entirely — absence is fine, amputation isn't.
        if (row.find("CTX") != std::string::npos)
            MAYA_TEST_CHECK(row.find('%') != std::string::npos,
                            "gauge present implies intact percent readout");
        if (auto p = row.find("Streamin"); p != std::string::npos)
            MAYA_TEST_CHECK(row.compare(p, 9, "Streaming") == 0,
                            "phase verb must never be clipped mid-word");
        if (auto p = row.find("Anthropi"); p != std::string::npos)
            MAYA_TEST_CHECK(row.compare(p, 9, "Anthropic") == 0,
                            "provider suffix must never be clipped mid-word");
        if (auto p = row.find("Fabl"); p != std::string::npos)
            MAYA_TEST_CHECK(row.compare(p, 5, "Fable") == 0,
                            "model name must never be clipped mid-word");
        if (row.find("refactor") != std::string::npos) {
            // Title is the MOST expendable fragment: its presence implies
            // every higher-value fragment (verb, badge, gauge) also fits.
            MAYA_TEST_CHECK(row.find("Streaming") != std::string::npos,
                            "title present implies phase verb present");
            MAYA_TEST_CHECK(row.find('%') != std::string::npos,
                            "title present implies gauge present");
        }
    }
    std::println("  no clipped right edge across widths 24..220");

    std::println("  3 rows + no overflow across widths 12..220");
    std::println("  PASS\n");
}

// ============================================================================
// Widget-audit regression tests (agentty-facing widgets)
// ============================================================================

// Composer: a cursor byte-offset inside a multi-byte UTF-8 sequence, or
// past the end / negative, must never throw and must never split the
// sequence (which would paint two U+FFFD cells and shift the line).
TEST_CASE("composer cursor safety") {
    std::println("=== test_composer_cursor_safety ===");

    // "café" — the é is 2 bytes (0xC3 0xA9) at offsets 3..4.
    for (int cur : {-5, 0, 3, 4, 5, 999}) {
        Composer::Config cfg;
        cfg.text   = "caf\xc3\xa9";
        cfg.cursor = cur;
        auto r = render_at(Composer{cfg}.build(), 60);
        assert(r.content_h > 0);
        // The '?' mapping in get_row covers any non-ASCII glyph, so we
        // can't string-match é — but a split sequence would render TWO
        // replacement cells where one glyph should be. Just assert no
        // row overflows and the body row still contains "caf".
        bool has_caf = false;
        for (const auto& row : r.rows)
            if (row.find("caf") != std::string::npos) has_caf = true;
        assert(has_caf && "composer body must render the text intact");
    }

    // User text CONTAINING the block glyph: the caret styling must key
    // on the cursor's byte offset, not the first █ found. Render with
    // the cursor at the end — must not throw / mis-place.
    {
        Composer::Config cfg;
        cfg.text   = "progress \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88 done";
        cfg.cursor = static_cast<int>(cfg.text.size());
        auto r = render_at(Composer{cfg}.build(), 60);
        assert(r.content_h > 0);
    }

    std::println("  PASS\n");
}

// ContextGauge: percent must not overflow int for token counts past
// ~21.4M, and the token field must be a constant width across the
// <1000 / k / M ranges (stable-width slot contract).
TEST_CASE("context gauge stability") {
    std::println("=== test_context_gauge_stability ===");

    // Overflow: 30M used of 30M max = 100%, not a negative garbage pct.
    {
        ContextGauge::Config cfg;
        cfg.used = 30'000'000;
        cfg.max  = 30'000'000;
        auto r = render_at(ContextGauge{cfg}.build(), 80);
        bool has_100 = false;
        for (const auto& row : r.rows)
            if (row.find("100%") != std::string::npos) has_100 = true;
        assert(has_100 && "30M/30M must render 100%");
    }

    // Constant total width across magnitude boundaries + placeholder.
    auto width_at = [](int used) {
        ContextGauge::Config cfg;
        cfg.used = used;
        cfg.max  = 200'000;
        auto r = render_at(ContextGauge{cfg}.build(), 200);
        assert(r.content_h == 1);
        return static_cast<int>(r.rows[0].size());
    };
    const int w_small = width_at(999);        // "   999"
    const int w_kilo  = width_at(1'000);      // "  1.0k"
    const int w_big   = width_at(199'999);    // "200.0k"
    assert(w_small == w_kilo && w_kilo == w_big
           && "token field must be constant width across magnitudes");
    const int w_zero  = width_at(0);          // placeholder ——/——
    assert(w_zero == w_small
           && "placeholder must occupy the same columns as live");

    // No gap after the slash: the MAX denominator is left-trimmed, so a 1M
    // window reads "409.8k/1.0M", never "409.8k/  1.0M" (the constant-width
    // right-justification used to leave two spaces after the '/').
    {
        ContextGauge::Config cfg;
        cfg.used = 409'800; cfg.max = 1'000'000;
        auto r = render_at(ContextGauge{cfg}.build(), 200);
        bool joined = false, gapped = false;
        for (const auto& row : r.rows) {
            if (row.find("/1.0M") != std::string::npos) joined = true;
            if (row.find("/  1.0M") != std::string::npos
                || row.find("/ 1.0M") != std::string::npos) gapped = true;
        }
        assert(joined && "denominator must join the slash: 409.8k/1.0M");
        assert(!gapped && "no leading-space gap after the slash");
    }

    // The 1M denominator survives inside a real StatusBar at a normal width:
    // either the whole "used/max" count shows (with 1.0M intact) or the
    // ladder drops the counts entirely — it must NEVER show a half-clipped
    // "409.8k/" with the denominator sheared off.
    {
        auto ctx_bar = [](int w) {
            StatusBar::Config cfg;
            cfg.phase_color      = Color::cyan();
            cfg.phase.verb       = "Write";
            cfg.phase.verb_width = 6;
            cfg.model_badge      = ModelBadge{{.label = "Sonnet 4.5"}}.build();
            cfg.context.used     = 409'800;
            cfg.context.max      = 1'000'000;
            cfg.context.cells    = 10;
            auto r = render_at(StatusBar{cfg}.build(), w);
            std::string joined;
            for (const auto& row : r.rows) joined += row + "\n";
            return joined;
        };
        for (int w = 30; w <= 200; ++w) {
            const std::string s = ctx_bar(w);
            const bool has_used = s.find("409.8k/") != std::string::npos;
            const bool has_max  = s.find("1.0M") != std::string::npos;
            // If the numerator is shown, the denominator must be too.
            assert((!has_used || has_max)
                   && "denominator must never be clipped while numerator shows");
        }
    }

    std::println("  PASS\n");
}

// ToolBodyPreview: tail_only line numbers must show TRUE source
// positions, not restart at 1.
TEST_CASE("tool body tail line numbers") {
    std::println("=== test_tool_body_tail_line_numbers ===");

    std::string body;
    for (int i = 1; i <= 50; ++i)
        body += "line number " + std::to_string(i) + "\n";

    // CodeBlock, tail_only (default): tail budget = max(head, tail) = 4
    // → rows 47..50, numbered 47..50.
    {
        ToolBodyPreview::Config cfg;
        cfg.kind = ToolBodyPreview::Kind::CodeBlock;
        cfg.text = body;
        auto r = render_at(ToolBodyPreview{cfg}.build(), 80);
        bool has_true_num = false, has_row_one = false;
        for (const auto& row : r.rows) {
            if (row.find(" 47") != std::string::npos
                && row.find("line number 47") != std::string::npos)
                has_true_num = true;
            if (row.find("  1 \xe2\x94\x82") == 0) has_row_one = true;
        }
        (void)has_row_one;
        assert(has_true_num
               && "tail_only code_block gutter must show true line numbers");
    }

    // FileWrite, streaming tail: gutter anchored past the hidden lines.
    {
        ToolBodyPreview::Config cfg;
        cfg.kind = ToolBodyPreview::Kind::FileWrite;
        cfg.text = body;
        cfg.show_footer_stats = false;
        auto r = render_at(ToolBodyPreview{cfg}.build(), 80);
        bool has_true_num = false;
        for (const auto& row : r.rows)
            if (row.find("50") != std::string::npos
                && row.find("line number 50") != std::string::npos)
                has_true_num = true;
        assert(has_true_num
               && "tail_only file_write gutter must show true line numbers");
    }

    std::println("  PASS\n");
}

// BashOutput distils cargo / pytest / jest summaries to a ✓/✗ verdict line,
// gated so prose can't mis-fire. Mirrors the gtest/ctest path already shipped.
TEST_CASE("tool body test-runner summaries") {
    std::println("=== test_tool_body_test_summaries ===");

    // get_row() maps non-ASCII glyphs (✓/✗) to '?', so we can't search for
    // the glyph bytes — assert on the ASCII verdict TEXT the widget renders
    // ("N/N tests passed" / "N/N tests failed"), which survives verbatim.
    auto verdict = [](const std::string& out) -> std::string {
        ToolBodyPreview::Config cfg;
        cfg.kind = ToolBodyPreview::Kind::BashOutput;
        cfg.text = out;
        auto r = render_at(ToolBodyPreview{cfg}.build(), 80);
        for (const auto& row : r.rows)
            if (row.find("tests passed") != std::string::npos
                || row.find("tests failed") != std::string::npos)
                return row;
        return {};
    };

    // cargo: all green.
    {
        auto v = verdict("running 4 tests\n....\n"
                         "test result: ok. 4 passed; 0 failed; 0 ignored\n");
        assert(v.find("4/4 tests passed") != std::string::npos
               && "cargo all-pass renders a 4/4 passed verdict");
    }
    // cargo: with a failure.
    {
        auto v = verdict("test result: FAILED. 3 passed; 1 failed; 0 ignored\n");
        assert(v.find("1/4 tests failed") != std::string::npos
               && "cargo failure renders a 1/4 failed verdict");
    }
    // pytest: the ' in <time>s' duration tail is the gate.
    {
        auto v = verdict("===== 12 passed, 1 failed in 0.30s =====\n");
        assert(v.find("1/13 tests failed") != std::string::npos
               && "pytest mixed renders a 1/13 failed verdict");
    }
    // jest: "Tests:" marker.
    {
        auto v = verdict("Tests:       1 failed, 3 passed, 4 total\n");
        assert(v.find("1/4 tests failed") != std::string::npos
               && "jest mixed renders a 1/4 failed verdict");
    }
    // gtest (tier 1, unchanged): still works.
    {
        auto v = verdict("[==========] 4 tests passed.\n");
        assert(v.find("4/4 tests passed") != std::string::npos
               && "gtest all-pass still renders a 4/4 passed verdict");
    }
    // FALSE-POSITIVE GATE: prose with a bare 'passed'/'failed' and no
    // runner context must NOT produce a verdict line.
    {
        auto v = verdict("The review passed after 2 rounds; the build "
                         "failed once but recovered. See notes.\n");
        assert(v.empty()
               && "bare prose 'passed'/'failed' must not fake a test verdict");
    }

    std::println("  PASS\n");
}

// small_caps must letter-space at UTF-8 boundaries, not bytes — a
// multi-byte label must survive intact (no mojibake / no width blowup).
TEST_CASE("small caps utf8") {
    std::println("=== test_small_caps_utf8 ===");

    AgentTimeline::Config cfg;
    cfg.title = " ACTIONS ";
    cfg.stats = {{"r\xc3\xa9vision", 2, Color::blue()}};   // révision
    AgentTimelineEvent ev;
    ev.name   = "Bash";
    ev.detail = "ok";
    ev.status = AgentEventStatus::Done;
    cfg.events.push_back(ev);
    auto r = render_at(AgentTimeline{cfg}.build(), 60);
    assert(r.content_h > 0);
    // The stats row letter-spaces to "R É V I S I O N"; get_row maps the
    // intact 2-byte é to a single '?'. A byte-split é would decode as
    // TWO invalid glyphs ('??') — assert exactly one '?' between R and V.
    bool ok = false;
    for (const auto& row : r.rows)
        if (row.find("R ? V I S I O N") != std::string::npos) ok = true;
    assert(ok && "small_caps must not split multi-byte sequences");

    std::println("  PASS\n");
}

// Picker with multi-row raw items: the auto-scroll clamp must work in
// row space. Selecting the last of several multi-row items must scroll
// far enough that the item's rows are inside the viewport.
TEST_CASE("picker multirow autoscroll") {
    std::println("=== test_picker_multirow_autoscroll ===");

    ScrollState scroll;
    Panel::Config cfg;
    cfg.title      = " Test ";
    cfg.viewport_h = 4;
    cfg.scroll     = &scroll;
    // Four 3-row items → 12 content rows, viewport 4.
    for (int i = 0; i < 4; ++i) {
        using namespace dsl;
        cfg.prebuilt.push_back(v(
            text("item" + std::to_string(i) + "-a"),
            text("item" + std::to_string(i) + "-b"),
            text("item" + std::to_string(i) + "-c")
        ).build());
    }
    cfg.selected = 3;   // starts at row 9, ends at row 12
    auto r = render_at(Panel{cfg}.build(), 50);
    (void)r;
    // Row-space clamp: sel_end(12) - vh(4) = 8. The old index-space
    // clamp computed y = 3 - 4 + 1 = 0 — selection entirely off-view.
    assert(scroll.y == 8
           && "multi-row picker items must auto-scroll in row space");

    std::println("  PASS\n");
}

// Structured panel rows use one consistent, unmistakable selection band.
// Lock the cursor treatment so future chrome changes cannot regress it.
//
// This asserts PANEL's contract, not the deleted Picker's. Picker painted the
// cursor as inverse video (bright-white bg, black fg) across the row; Panel
// paints a subtle wash (theme.row_bg) plus an edge bar in column 0, so a
// picker row and a form row line up and a selected row stays readable. The
// selection must still be unmistakable and must still span the full row
// including the slack past the text — that part of the old test was right and
// is what is kept.
TEST_CASE("panel selected row highlight") {
    std::println("=== test_panel_selected_row_highlight ===");

    ScrollState scroll;
    scroll.auto_dispatch = false;
    Panel::Config cfg;
    cfg.title = " Picker ";
    cfg.min_width = 30;
    cfg.viewport_h = 2;
    cfg.scroll = &scroll;
    // Config::selected is the ONE owner of where the cursor is; Row::selected
    // is derived from it by the widget and ignored as an input.
    cfg.selected = 0;
    cfg.items = {
        Panel::Item{.leading = "Selected", .trailing = "first"},
        Panel::Item{.leading = "Unselected", .trailing = "second"},
    };
    const Color wash = cfg.theme.row_bg;

    StylePool pool;
    Canvas canvas(40, 10, &pool);
    render_tree(Panel{std::move(cfg)}.build(), canvas, pool, theme::native);

    int selected_y = -1;
    int selected_x = -1;
    int unselected_x = -1;
    int unselected_y = -1;
    for (int y = 0; y < canvas.height(); ++y) {
        for (int x = 0; x < canvas.width(); ++x) {
            const auto ch = canvas.get(x, y).character;
            if (ch == U'S' && selected_y < 0) { selected_x = x; selected_y = y; }
            if (ch == U'U' && unselected_y < 0) { unselected_x = x; unselected_y = y; }
        }
    }
    assert(selected_y >= 0 && unselected_y >= 0);

    const Style selected = pool.get(canvas.get(selected_x, selected_y).style_id);
    const Style unselected = pool.get(canvas.get(unselected_x, unselected_y).style_id);
    assert(selected.bg == wash
           && "the cursor row carries the theme's row wash");
    // A non-cursor row must not carry THE WASH. Deliberately not "carries no
    // background at all": row_bg is Color::slot(Surface), and under native
    // that slot resolves to Default — SGR 49, the terminal's own background.
    // So an unwashed row can legitimately hold a bg whose VALUE is "defer to
    // the terminal", which is the same pixels as holding none. What would
    // actually be a bug is the two rows becoming indistinguishable, so that
    // is what is asserted.
    assert(unselected.bg != wash
           && "a non-cursor row must not carry the cursor wash");
    assert((!unselected.bg.has_value()
            || unselected.bg->kind() == Color::Kind::Default)
           && "a non-cursor row states no colour of its own");

    // The wash must cover the row to its END, not stop at the last glyph: a
    // band that ends with the text reads as a broken stripe. Assert the SPAN
    // (contiguous from the first washed cell to the last) and that it reaches
    // past the trailing text, rather than a magic cell count — the row's width
    // depends on min_width, padding and the scrollbar column.
    int first_wash = -1, last_wash = -1, washed = 0;
    for (int x = 0; x < canvas.width(); ++x)
        if (pool.get(canvas.get(x, selected_y).style_id).bg == wash) {
            if (first_wash < 0) first_wash = x;
            last_wash = x;
            ++washed;
        }
    assert(first_wash >= 0 && "the cursor row must be washed at all");
    assert(washed == last_wash - first_wash + 1
           && "the wash must be contiguous across the row");

    // It must extend past the trailing cell's TEXT, i.e. cover the slack
    // between the two columns and up to the row's own right edge. Bound the
    // search to the washed span: everything right of it is frame chrome (the
    // scrollbar column, the border) which is not part of the row and is not
    // supposed to be washed.
    int last_glyph = -1;
    for (int x = first_wash; x <= last_wash; ++x) {
        const auto ch = canvas.get(x, selected_y).character;
        if (ch && ch != U' ') last_glyph = x;
    }
    assert(last_glyph >= 0 && "the cursor row must contain its text");
    assert(last_wash >= last_glyph
           && "selected row background must span the row, including slack");

    // And the band must be WIDE — covering the gap between the leading and
    // trailing columns, not just the two runs of text. That gap is where the
    // wash used to break into two stripes with a hole in the middle.
    int glyph_cells = 0;
    for (int x = first_wash; x <= last_wash; ++x) {
        const auto ch = canvas.get(x, selected_y).character;
        if (ch && ch != U' ') ++glyph_cells;
    }
    assert(washed > glyph_cells
           && "the wash must fill the slack between cells, not only the text");

    std::println("  PASS\n");
}

// A LARGE list takes the virtualized path: only the rows intersecting the
// viewport are built, the rest are reserved by spacers. Output must be
// identical to rendering everything — the selected row, deep in the list,
// still renders its real content and its wash at the right viewport slot.
TEST_CASE("panel virtualized large list renders selection") {
    std::println("=== test_panel_virtualized_large_list ===");

    ScrollState scroll;
    scroll.auto_dispatch = false;
    Panel::Config cfg;
    cfg.title = " Big ";
    cfg.min_width = 30;
    cfg.viewport_h = 8;
    cfg.scroll = &scroll;
    cfg.selected = 137;                 // deep in the list
    for (int i = 0; i < 200; ++i)
        cfg.items.push_back(Panel::Item{
            .leading = "row" + std::to_string(i), .trailing = "t"});
    const Color wash = cfg.theme.row_bg;

    StylePool pool;
    Canvas canvas(40, 14, &pool);
    render_tree(Panel{std::move(cfg)}.build(), canvas, pool, theme::native);

    // The selected row must be on-screen with its REAL leading text — proving
    // the windowed row was built and placed, not left as a blank spacer.
    int hl_y = -1;
    for (int y = 0; y < canvas.height() && hl_y < 0; ++y)
        for (int x = 0; x < canvas.width(); ++x)
            if (pool.get(canvas.get(x, y).style_id).bg == wash) {
                hl_y = y; break;
            }
    assert(hl_y >= 0 && "selected row must be visible + highlighted");
    std::string line;
    for (int x = 0; x < canvas.width(); ++x) {
        char32_t ch = canvas.get(x, hl_y).character;
        if (ch && ch < 128) line.push_back(static_cast<char>(ch));
    }
    assert(line.find("row137") != std::string::npos
           && "virtualized selected row must show its real content");
    std::println("  PASS\n");
}

// The preview is a framed block of a fixed height whatever it holds: the
// label on the top border, facts flush right on the title row, the hint on
// the bottom border, and the transcript filling exactly transcript_rows().
TEST_CASE("peek card is a fixed, framed block") {
    std::println("=== test_peek_card_fixed ===");
    for (int n : {0, 1, 30}) {
        PeekCard::Config pc;
        pc.title = "TITLE";
        pc.facts = "FACTS";
        pc.subtitle = "SUB";
        pc.asked = "ASKED";
        pc.hint = "HINT";
        pc.rows = 12;
        for (int i = 0; i < n; ++i)
            pc.lines.push_back({"you", "line" + std::to_string(i), PeekCard::Kind::User});
        const auto r = render_at(PeekCard{pc}.build(), 60);
        auto row_of = [&](std::string_view s) {
            for (int y = 0; y < static_cast<int>(r.rows.size()); ++y)
                if (r.rows[static_cast<std::size_t>(y)].find(s) != std::string::npos) return y;
            return -1;
        };
        assert(r.content_h == 12 && "the card never changes height");
        assert(row_of("Preview") == 0 && "label on the top border");
        assert(row_of("TITLE") == 1 && row_of("FACTS") == 1 && "facts on the title row");
        assert(row_of("HINT") == 11 && "hint on the bottom border");
        // Facts end against the right border: "FACTS ?" with one pad space.
        const auto& t = r.rows[1];
        assert(t.find("FACTS") + 5 + 2 == t.find_last_not_of(' ') + 1 && "facts flush right");
        const int fit = PeekCard::transcript_rows(pc);
        assert(fit == 12 - 2 - 4 && "border, title, sub, asked, divider leave 6");
        const int want = std::min(n, fit);
        assert((want == 0 || row_of("line" + std::to_string(want - 1)) >= 0) && "fills its rows");
        assert(row_of("line" + std::to_string(want)) < 0 && "and no more");
    }
    std::println("  PASS\n");
}

// Structured rows must remain exactly one row tall even when badge, leading,
// and trailing cells all compete for a phone-sized terminal. Wrapping changes
// row indices and makes the cursor highlight the wrong visual item.
TEST_CASE("picker rows responsive") {
    std::println("=== test_picker_rows_responsive ===");

    auto render = [](int width) {
        static ScrollState scroll = [] {
            ScrollState s;
            s.auto_dispatch = false;
            return s;
        }();
        scroll.y = 0;
        Panel::Config cfg;
        cfg.title = " Tool Outputs ";
        cfg.min_width = 1;
        cfg.viewport_h = 3;
        cfg.scroll = &scroll;
        cfg.selected = 0;
        cfg.items = {
            Panel::Item{
                .badge = "Git Commit", .leading = "A very long commit description",
                .trailing = "ok · 203 KB", .selected = true},
            Panel::Item{
                .badge = "Diagnostics", .leading = "cmake --build everything",
                .trailing = "failed · 41.2s"},
            Panel::Item{
                .badge = "● LIVE", .leading = "Bash gh run watch",
                .trailing = "running · 1 MB"},
        };
        return render_at(Panel{std::move(cfg)}.build(), width);
    };

    const int expected_h = render(80).content_h;
    for (int width = 8; width <= 120; ++width) {
        auto r = render(width);
        assert_fits(r, width, "responsive picker rows");
        assert(r.content_h == expected_h
               && "structured picker rows must never wrap at narrow widths");
    }

    std::println("  PASS\n");
}

// Input.handle_paste: a multi-line bracketed paste must KEEP its newlines
// when the Input is multiline (0x0A would otherwise be scrubbed as a control
// byte and the block collapses onto one line). A single-line Input still
// strips them.
TEST_CASE("input multiline paste") {
    std::println("=== test_input_multiline_paste ===");

    // Multiline: newlines survive, embedded control bytes (\t=0x09) drop.
    {
        Input<InputConfig{.multiline = true}> inp;
        inp.handle_paste(PasteEvent{"line1\nline2\tX\nline3"});
        const std::string& v = inp.value()();
        assert(v == "line1\nline2X\nline3"
               && "multiline paste must keep newlines, drop other controls");
    }

    // Single-line: newlines stripped (block flattened by design).
    {
        Input<> inp;
        inp.handle_paste(PasteEvent{"a\nb\nc"});
        const std::string& v = inp.value()();
        assert(v == "abc" && "single-line paste flattens newlines");
    }

    std::println("  PASS\n");
}

// Word-wrap must treat combining marks as ZERO width. `à` = 'a' + U+0300
// occupies one column, so N accented chars wrap into ceil(N/width) lines, not
// double that. Regression for the emit_lines width bug (marks counted as 1
// forced a premature break after every base+mark pair).
TEST_CASE("word wrap combining") {
    std::println("=== test_word_wrap_combining ===");
    std::string accented;
    for (int i = 0; i < 40; ++i) accented += "a\xcc\x80";  // 'a' + combining grave
    auto lines = maya::word_wrap(accented, 10);
    // 40 display columns at width 10 => 4 lines. (Pre-fix: 8.)
    MAYA_TEST_CHECK(lines.size() == 4,
                    "combining marks are zero-width in word_wrap");
    // A pure combining-mark string (no base) must not hang or explode either.
    std::string marks;
    for (int i = 0; i < 20; ++i) marks += "\xcc\x80";
    auto ml = maya::word_wrap(marks, 4);
    MAYA_TEST_CHECK(!ml.empty(), "pure combining-mark wrap does not hang");
    std::println("PASS");
}

TEST_CASE("model badge renders what the host decoded") {
    std::println("=== test_model_badge_labels ===");
    auto rendered = [](maya::ModelBadge mb) {
        auto r = render_at(mb.build(), 80);
        std::string joined;
        for (const auto& row : r.rows) joined += row;
        return joined;
    };

    // The widget is PRESENTATION ONLY: it renders the label the host
    // decoded, and never parses a model id itself. (The taxonomy that used
    // to live here — substring family matching, digit-run version scanning —
    // now lives in the host's model_name SSOT, where it has catalog access
    // and consteval proofs. See model_badge.hpp's header comment.)
    MAYA_TEST_CHECK(rendered(maya::ModelBadge{{.label = "Sonnet"}})
                        .find("Sonnet") != std::string::npos,
                    "label rendered verbatim");

    // A version rides after the label, dim. Crucially it is NOT dropped in
    // any mode: the old widget returned early in compact mode before
    // appending it, so a compact badge could not distinguish 4.5 from 4.8.
    {
        const std::string s =
            rendered(maya::ModelBadge{{.label = "Opus", .version = "4.8"}});
        MAYA_TEST_CHECK(s.find("Opus") != std::string::npos,
                        "label present alongside a version");
        MAYA_TEST_CHECK(s.find("4.8") != std::string::npos,
                        "version is never silently dropped");
    }

    // No version supplied: nothing invented.
    {
        const std::string s = rendered(maya::ModelBadge{{.label = "Haiku"}});
        MAYA_TEST_CHECK(s.find("Haiku") != std::string::npos,
                        "label-only badge renders");
        // get_row() renders the canvas as ASCII, mapping U+25CF ● to '@'
        // (see its glyph table). So the dot is asserted via that stand-in
        // AND its POSITION — leading, ahead of the label.
        MAYA_TEST_CHECK(s.starts_with("@ Haiku"),
                        "dot shown by default, ahead of the label");
    }

    // The dot is suppressible for composition into a larger chip.
    {
        const std::string s = rendered(
            maya::ModelBadge{{.label = "Opus", .show_dot = false}});
        MAYA_TEST_CHECK(s.starts_with("Opus"),
                        "show_dot=false: label leads, no dot glyph");
    }

    // A raw id passed as a label is rendered as-is — the widget does not
    // second-guess the host. Callers hand it decoded text.
    MAYA_TEST_CHECK(rendered(maya::ModelBadge{"mystery-model"})
                        .find("mystery-model") != std::string::npos,
                    "label-only ctor renders its string");
    std::println("PASS");
}

TEST_CASE("reasoning stream: live vs settled chrome, and no fold") {
    auto joined = [](const RenderResult& r) {
        std::string s;
        for (auto& row : r.rows) { s += row; s.push_back('\n'); }
        return s;
    };

    const std::string body =
        "First I inspect the FIRST_MARKER path.\n"
        "Then I confirm the SECOND_MARKER stays visible.";

    // LIVE: animated "Thinking" header, and the widget reports it's still
    // animating (the reveal cursor walks the body over subsequent frames —
    // exactly like normal streamed text, so we don't assert full body on the
    // first paint).
    {
        maya::ReasoningStream rs;
        rs.set_live(true);
        rs.set_content(body);
        const std::string s = joined(render_at(rs.build(), 44, 12));
        MAYA_TEST_CHECK(s.find("Thinking") != std::string::npos,
                        "live reasoning shows a 'Thinking' header");
        MAYA_TEST_CHECK(rs.is_animating(),
                        "live reasoning reports it is still animating");
    }

    // SETTLED: 'Reasoned' + token count, and the FULL body stays — NO fold.
    {
        maya::ReasoningStream rs;
        rs.set_content(body);
        rs.finish();
        rs.set_live(false);
        const std::string s = joined(render_at(rs.build(), 44, 12));
        MAYA_TEST_CHECK(s.find("Reasoned") != std::string::npos,
                        "settled reasoning shows a 'Reasoned' header");
        MAYA_TEST_CHECK(s.find("token") != std::string::npos,
                        "settled header names an approximate token count");
        MAYA_TEST_CHECK(s.find("FIRST_MARKER") != std::string::npos,
                        "settled reasoning keeps its first line");
        MAYA_TEST_CHECK(s.find("SECOND_MARKER") != std::string::npos,
                        "settled reasoning keeps ALL lines — it never folds");
    }

    // build_with_body: host supplies its own (cache-owned) body; the widget
    // still frames it with the same chrome.
    {
        maya::ReasoningStream rs;
        rs.set_live(false);
        Element host_body = maya::markdown("HOST_OWNED_BODY line.");
        const std::string s =
            joined(render_at(rs.build_with_body(std::move(host_body)), 44, 12));
        MAYA_TEST_CHECK(s.find("Reasoned") != std::string::npos,
                        "chrome wraps a host-supplied body");
        MAYA_TEST_CHECK(s.find("HOST_OWNED_BODY") != std::string::npos,
                        "host-supplied body renders inside the block");
    }

    // Live gradient body (stream-of-consciousness fade): the gradient recolor
    // pass must render cleanly. (Live reasoning reveals progressively, so we
    // assert the chrome renders rather than full body on the first frame —
    // same discipline as the live test above.)
    {
        maya::ReasoningStream::Config cfg;
        cfg.gradient_body = true;
        cfg.pulse = true;
        cfg.live_tail_rows = 3;    // ticker: only the last few rows
        maya::ReasoningStream rs{cfg};
        rs.set_live(true);
        rs.set_content("ALPHA_LINE\n\nBETA_LINE\n\nGAMMA_LINE");
        const auto r = render_at(rs.build(), 44, 16);
        const std::string s = joined(r);
        MAYA_TEST_CHECK(r.content_h > 0,
                        "gradient+pulse+tail live reasoning renders the block");
        MAYA_TEST_CHECK(s.find("Thinking") != std::string::npos,
                        "gradient live reasoning still shows the animated header");
        MAYA_TEST_CHECK(s.find("tok") != std::string::npos,
                        "live header shows the ticking token meter");
    }
    // Settled gradient config falls back to the flat recolor and keeps the
    // FULL body (the gradient is live-only, so settle is unaffected).
    {
        maya::ReasoningStream::Config cfg;
        cfg.gradient_body = true;
        maya::ReasoningStream rs{cfg};
        rs.set_content("ALPHA_LINE\n\nGAMMA_LINE");
        rs.finish();
        rs.set_live(false);
        const std::string s = joined(render_at(rs.build(), 44, 16));
        MAYA_TEST_CHECK(s.find("ALPHA_LINE") != std::string::npos
                        && s.find("GAMMA_LINE") != std::string::npos,
                        "settled gradient reasoning keeps the full body");
    }

    // Structured beats: decision-marker paragraphs render as waypoints. We
    // can't assert color via the text harness, but the render must keep the
    // beat lines (exercises is_beat + the waypoint recolor path end to end).
    {
        maya::ReasoningStream::Config cfg;
        cfg.structured = true;
        maya::ReasoningStream rs{cfg};
        rs.set_content("Connective filler prose here.\n\n"
                       "Let me check the auth path.\n\n"
                       "Actually, the token refresh is the bug.");
        rs.finish();
        rs.set_live(false);
        const std::string s = joined(render_at(rs.build(), 60, 20));
        MAYA_TEST_CHECK(s.find("Let me check") != std::string::npos
                        && s.find("Actually") != std::string::npos,
                        "structured reasoning keeps beat lines in the body");
    }

    // Dynamic-height faded tail: grows with short content, caps + fades when
    // long, always showing the LAST lines (newest at the bottom).
    {
        // Short: fits under the cap → shows ALL lines (no drop, no padding).
        maya::ReasoningStream::Config cfg;
        cfg.body_prestyled = true;
        maya::ReasoningStream rs{cfg};
        rs.set_live(true);
        auto body = maya::ReasoningStream::faded_tail(
            "aaa\nbbb\nccc", 8,
            maya::Color::rgb(0x20,0x20,0x28), maya::Color::rgb(0xb0,0xb0,0xb8));
        const std::string s = joined(render_at(rs.build_with_body(std::move(body)), 50, 20));
        MAYA_TEST_CHECK(s.find("aaa") != std::string::npos
                        && s.find("ccc") != std::string::npos,
                        "short reasoning shows every line (grows, no cap)");
    }
    {
        // Long: exceeds the cap → last lines only, oldest dropped.
        std::string src;
        for (int i = 1; i <= 40; ++i) src += "line" + std::to_string(i) + "\n";
        maya::ReasoningStream::Config cfg;
        cfg.body_prestyled = true;
        maya::ReasoningStream rs{cfg};
        rs.set_live(true);
        auto body = maya::ReasoningStream::faded_tail(
            src, 8, maya::Color::rgb(0x20,0x20,0x28), maya::Color::rgb(0xb0,0xb0,0xb8));
        const auto r = render_at(rs.build_with_body(std::move(body)), 50, 40);
        const std::string s = joined(r);
        MAYA_TEST_CHECK(s.find("line40") != std::string::npos,
                        "capped tail shows the newest line at the bottom");
        MAYA_TEST_CHECK(s.find("line1\n") == std::string::npos
                        && s.find("line5\n") == std::string::npos,
                        "capped tail drops the oldest lines");
    }
    std::println("PASS");
}


TEST_CASE("search result outlives the widget that built it") {
    // SearchResult::build() used to return an adapt() element capturing
    // `this`. A transcript builds the widget as a local and keeps only the
    // element, so layout read a destroyed widget: examples/chat crashed
    // (SIGSEGV in build_at) once the Grep block scrolled into a frame. The
    // element must carry its own copy. Under ASan this was
    // stack-use-after-scope.
    auto make = [] {
        SearchResult r{SearchKind::Grep, "needle"};
        r.set_status(SearchStatus::Done);
        r.set_expanded(true);
        r.add_group({"src/a/really/long/path/to/some/file.cpp", {{3, "needle = 1;"}}});
        return r.build();                 // r is gone after this
    };
    std::vector<Element> kept;
    for (int i = 0; i < 3; ++i) kept.push_back(make());
    std::vector<std::string> scribble(64, std::string(512, 'z'));   // reuse freed memory
    (void)scribble;
    const std::string out = render_to_string(dsl::v(kept), 60);
    CHECK(out.find("needle") != std::string::npos);
    CHECK(out.find("file.cpp") != std::string::npos);
}
