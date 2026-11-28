// test_scroll.cpp — coverage for the framework scroll primitive:
//
//   - ScrollState (data + clamp + key/mouse handling)
//   - DSL scroll() pipe (sets overflow + scroll_x/y + writeback ptr)
//   - Renderer paint-time origin shift (visible content shifts with offset)
//   - max_x / max_y writeback after layout
//   - Stale-offset clamp on next frame
//   - scrollbar_y / scrollbar_x widgets (thumb position + proportional size)

// Maya's default build type is Release, which defines NDEBUG and turns
// assert() into a no-op. That makes silent "PASS" outputs meaningless.
// Force assertions on for this file before <cassert> is pulled in.
#ifdef NDEBUG
#undef NDEBUG
#endif

#include <maya/maya.hpp>
#include <maya/widget/scrollable.hpp>
#include <maya/widget/agentty/scrollbar.hpp>

#include "agtest.hpp"
#include <print>
#include <string>
#include <vector>

using namespace maya;
using namespace maya::detail;
using namespace maya::dsl;

// ── Canvas helpers (matched to test_render.cpp's idiom) ─────────────────────

static std::string get_row(const Canvas& canvas, int y) {
    std::string s;
    for (int x = 0; x < canvas.width(); ++x) {
        Cell c = canvas.get(x, y);
        if (c.character >= 0x20 && c.character < 0x7F)
            s += static_cast<char>(c.character);
        else if (c.character >= 0x2500)
            s += '#';
        else
            s += ' ';
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

static void dump(const Canvas& canvas, int rows = -1) {
    if (rows < 0) rows = canvas.height();
    for (int y = 0; y < rows && y < canvas.height(); ++y) {
        std::println("  {:2}|{}|", y, get_row(canvas, y));
    }
}

// Build a deterministic numbered-row column. Rows look like:
//   "row00", "row01", ..., "rowNN"
static std::vector<Element> make_rows(int n) {
    std::vector<Element> out;
    out.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "row%02d", i);
        out.push_back(text(std::string{buf}));
    }
    return out;
}

// Helper: make synthetic events
static KeyEvent key_event(SpecialKey sk, bool ctrl = false, bool shift = false) {
    KeyEvent ev;
    ev.key = sk;
    ev.mods.ctrl  = ctrl;
    ev.mods.shift = shift;
    return ev;
}
static MouseEvent wheel(MouseButton btn, bool shift = false, bool alt = false) {
    MouseEvent ev;
    ev.button = btn;
    ev.kind   = MouseEventKind::Press;
    ev.mods.shift = shift;
    ev.mods.alt   = alt;
    return ev;
}
static MouseEvent wheel_at(MouseButton btn, int col, int row) {
    MouseEvent ev;
    ev.button = btn;
    ev.kind   = MouseEventKind::Press;
    // SGR mouse coords are 1-based; ScrollState::handle subtracts 1.
    ev.x = Columns{col + 1};
    ev.y = Rows{row + 1};
    return ev;
}
static MouseEvent click_at(int col, int row) {
    MouseEvent ev;
    ev.button = MouseButton::Left;
    ev.kind   = MouseEventKind::Press;
    ev.x = Columns{col + 1};
    ev.y = Rows{row + 1};
    return ev;
}
static MouseEvent move_at(int col, int row) {
    MouseEvent ev;
    ev.button = MouseButton::Left;
    ev.kind   = MouseEventKind::Move;
    ev.x = Columns{col + 1};
    ev.y = Rows{row + 1};
    return ev;
}
static MouseEvent release_at(int col, int row) {
    MouseEvent ev;
    ev.button = MouseButton::Left;
    ev.kind   = MouseEventKind::Release;
    ev.x = Columns{col + 1};
    ev.y = Rows{row + 1};
    return ev;
}

// ============================================================================
// 1. ScrollState — pure data + clamp + accessors
// ============================================================================
TEST_CASE("scroll state basics") {
    std::println("--- test_scroll_state_basics ---");
    ScrollState s;
    assert(s.x == 0 && s.y == 0);
    assert(s.max_x == 0 && s.max_y == 0);
    assert(s.at_top() && s.at_bottom() && s.at_left() && s.at_right());

    s.max_y = 10;
    s.y = 5;
    assert(!s.at_top() && !s.at_bottom());
    s.clamp();
    assert(s.y == 5);

    // Out-of-range gets pulled in.
    s.y = 999;
    s.clamp();
    assert(s.y == 10);
    s.y = -3;
    s.clamp();
    assert(s.y == 0);

    std::println("PASS\n");
}

// ============================================================================
// 2. scroll_by / scroll_to_* clamp correctly on both axes
// ============================================================================
TEST_CASE("scroll state imperative") {
    std::println("--- test_scroll_state_imperative ---");
    ScrollState s;
    s.max_x = 20;
    s.max_y = 40;

    s.scroll_by(5, 7);
    assert(s.x == 5 && s.y == 7);

    s.scroll_by(-100, -100);   // clamped
    assert(s.x == 0 && s.y == 0);

    s.scroll_by(1000, 1000);   // clamped
    assert(s.x == 20 && s.y == 40);

    s.scroll_to_top();
    assert(s.y == 0 && s.x == 20);  // only y reset
    s.scroll_to_bottom();
    assert(s.y == 40);
    s.scroll_to_left();
    assert(s.x == 0);
    s.scroll_to_right();
    assert(s.x == 20);
    s.scroll_to_origin();
    assert(s.x == 0 && s.y == 0);

    s.scroll_to(7, 13);
    assert(s.x == 7 && s.y == 13);

    std::println("PASS\n");
}

// ============================================================================
// 3. Key handling: arrows, PgUp/Dn, Home/End, Ctrl variants
// ============================================================================
TEST_CASE("scroll state keys") {
    std::println("--- test_scroll_state_keys ---");
    ScrollState s;
    s.max_x = 50;
    s.max_y = 100;

    assert(s.handle(key_event(SpecialKey::Down))  && s.y == 1);
    assert(s.handle(key_event(SpecialKey::Down))  && s.y == 2);
    assert(s.handle(key_event(SpecialKey::Up))    && s.y == 1);
    assert(s.handle(key_event(SpecialKey::Right)) && s.x == 1);
    assert(s.handle(key_event(SpecialKey::Left))  && s.x == 0);

    // PgDn moves by viewport_h
    s.y = 0;
    assert(s.handle(key_event(SpecialKey::PageDown), /*viewport_h=*/10) && s.y == 10);
    assert(s.handle(key_event(SpecialKey::PageUp),   /*viewport_h=*/10) && s.y == 0);

    // Home/End without modifiers operate on vertical axis only
    s.x = 5; s.y = 7;
    assert(s.handle(key_event(SpecialKey::End)) && s.y == 100 && s.x == 5);
    assert(s.handle(key_event(SpecialKey::Home)) && s.y == 0 && s.x == 5);

    // Ctrl+End jumps to (max_x, max_y); Ctrl+Home to origin
    assert(s.handle(key_event(SpecialKey::End,  /*ctrl=*/true)) && s.x == 50 && s.y == 100);
    assert(s.handle(key_event(SpecialKey::Home, /*ctrl=*/true)) && s.x == 0  && s.y == 0);

    // Non-arrow/nav keys are not consumed (return false)
    KeyEvent k_char;
    k_char.key = CharKey{static_cast<char32_t>('a')};
    assert(!s.handle(k_char));

    std::println("PASS\n");
}

// ============================================================================
// 4. Mouse handling: wheel + Shift+wheel for horizontal
// ============================================================================
TEST_CASE("scroll state mouse") {
    std::println("--- test_scroll_state_mouse ---");
    ScrollState s;
    s.max_x = 50;
    s.max_y = 50;

    assert(s.handle(wheel(MouseButton::ScrollDown)) && s.y == 1);
    assert(s.handle(wheel(MouseButton::ScrollUp))   && s.y == 0);

    // Shift+wheel pans horizontally (fallback for terminals without
    // native horizontal wheel events).
    assert(s.handle(wheel(MouseButton::ScrollDown, /*shift=*/true)) && s.x == 1 && s.y == 0);
    assert(s.handle(wheel(MouseButton::ScrollUp,   /*shift=*/true)) && s.x == 0);

    // Alt+wheel is the same fallback for terminals that strip Shift
    // (gnome-terminal, xterm, older Konsole reserve Shift+wheel for
    // their own scrollback).
    assert(s.handle(wheel(MouseButton::ScrollDown, /*shift=*/false, /*alt=*/true)) && s.x == 1);
    assert(s.handle(wheel(MouseButton::ScrollUp,   /*shift=*/false, /*alt=*/true)) && s.x == 0);

    // Native horizontal wheel — SGR mouse codes 66/67. These come from
    // Kitty / WezTerm / iTerm2 / recent Konsole on a real trackpad pan.
    assert(s.handle(wheel(MouseButton::ScrollRight)) && s.x == 1);
    assert(s.handle(wheel(MouseButton::ScrollLeft))  && s.x == 0);
    // step_x is respected for horizontal wheel.
    s.step_x = 4;
    assert(s.handle(wheel(MouseButton::ScrollRight)) && s.x == 4);

    // Non-scroll buttons are not consumed
    MouseEvent click;
    click.button = MouseButton::Left;
    click.kind   = MouseEventKind::Press;
    assert(!s.handle(click));

    // Release events are not consumed
    MouseEvent release;
    release.button = MouseButton::ScrollDown;
    release.kind   = MouseEventKind::Release;
    assert(!s.handle(release));

    std::println("PASS\n");
}

// ============================================================================
// 5. step_x / step_y honored
// ============================================================================
TEST_CASE("scroll state step") {
    std::println("--- test_scroll_state_step ---");
    ScrollState s;
    s.max_x = 100; s.max_y = 100;
    s.step_x = 4;
    s.step_y = 3;
    assert(s.handle(key_event(SpecialKey::Down)) && s.y == 3);
    assert(s.handle(key_event(SpecialKey::Down)) && s.y == 6);
    assert(s.handle(key_event(SpecialKey::Right)) && s.x == 4);
    assert(s.handle(wheel(MouseButton::ScrollDown)) && s.y == 9);
    std::println("PASS\n");
}

// ============================================================================
// 6. DSL pipe sets BoxElement fields correctly (overflow / scroll_x/y / ptr)
// ============================================================================
TEST_CASE("dsl pipe sets fields") {
    std::println("--- test_dsl_pipe_sets_fields ---");
    ScrollState s;
    s.x = 3;
    s.y = 7;

    Element e = (v(text("a"), text("b")) | scroll(s, /*h=*/5)).build();
    auto* bx = as_box(e);
    assert(bx != nullptr);
    assert(bx->overflow == Overflow::Scroll);
    assert(bx->layout.scroll_x == 3);
    assert(bx->layout.scroll_y == 7);
    assert(bx->scroll_state == &s);
    // viewport h was 5, w was 0 (not set)
    assert(bx->layout.height.is_fixed());
    assert(static_cast<int>(bx->layout.height.value) == 5);
    assert(!bx->layout.width.is_fixed());

    // 2D form sets both
    Element e2 = (v(text("a")) | scroll(s, 40, 8)).build();
    auto* bx2 = as_box(e2);
    assert(bx2 != nullptr);
    assert(bx2->layout.width.is_fixed());
    assert(static_cast<int>(bx2->layout.width.value) == 40);
    assert(static_cast<int>(bx2->layout.height.value) == 8);

    std::println("PASS\n");
}

// ============================================================================
// 7. Renderer: vertical content shifts with scroll_y
// ============================================================================
TEST_CASE("renderer vertical scroll") {
    std::println("--- test_renderer_vertical_scroll ---");
    StylePool pool;

    auto build = [](ScrollState& s) {
        return (v(make_rows(30)) | scroll(s, /*h=*/8)).build();
    };

    // y = 0 → first row visible is row00
    {
        Canvas canvas(20, 8, &pool);
        ScrollState s;
        render_tree(build(s), canvas, pool, theme::native);
        dump(canvas);
        assert(get_row(canvas, 0).find("row00") != std::string::npos);
        assert(get_row(canvas, 7).find("row07") != std::string::npos);
    }
    // y = 5 → first row visible is row05
    {
        Canvas canvas(20, 8, &pool);
        ScrollState s;
        s.y = 5;
        render_tree(build(s), canvas, pool, theme::native);
        dump(canvas);
        assert(get_row(canvas, 0).find("row05") != std::string::npos);
        assert(get_row(canvas, 7).find("row12") != std::string::npos);
    }
    // y = 22 → last frame: row22..row29
    {
        Canvas canvas(20, 8, &pool);
        ScrollState s;
        s.y = 22;
        render_tree(build(s), canvas, pool, theme::native);
        dump(canvas);
        assert(get_row(canvas, 0).find("row22") != std::string::npos);
        assert(get_row(canvas, 7).find("row29") != std::string::npos);
    }

    std::println("PASS\n");
}

// ============================================================================
// 8. Renderer: horizontal content shifts with scroll_x
// ============================================================================
//
// Horizontal scroll requires content with a defined natural width — a
// row of cells, or a TextElement with wrap = NoWrap. (A default-wrap
// text in a narrow viewport just word-wraps; nothing to scroll past.)
TEST_CASE("renderer horizontal scroll") {
    std::println("--- test_renderer_horizontal_scroll ---");
    StylePool pool;

    // Row of 30 single-character cells in an h() container. Each cell is
    // naturally 1 column wide so no wrap question arises.
    auto wide_row = []() {
        std::vector<Element> cells;
        cells.reserve(30);
        for (int i = 0; i < 30; ++i) {
            char c = static_cast<char>('0' + (i % 10));
            cells.push_back(text(std::string(1, c)));
        }
        return cells;
    };

    // x = 0 → first 10 cells visible
    {
        Canvas canvas(10, 1, &pool);
        ScrollState s;
        Element ui = (h(wide_row()) | scrollx(s, /*w=*/10)).build();
        render_tree(ui, canvas, pool, theme::native);
        std::string row = get_row(canvas, 0);
        std::println("  x=0  row[0]='{}'", row);
        assert(row.starts_with("0123456789"));
    }

    // x = 5 → cells 5..14 visible, i.e. "5678901234"
    {
        Canvas canvas(10, 1, &pool);
        ScrollState s;
        s.x = 5;
        Element ui = (h(wide_row()) | scrollx(s, /*w=*/10)).build();
        render_tree(ui, canvas, pool, theme::native);
        std::string row = get_row(canvas, 0);
        std::println("  x=5  row[0]='{}'", row);
        assert(row.starts_with("5678901234"));
    }

    // x = 20 → last cells: 20..29 = "0123456789"
    {
        Canvas canvas(10, 1, &pool);
        ScrollState s;
        s.x = 20;
        Element ui = (h(wide_row()) | scrollx(s, /*w=*/10)).build();
        render_tree(ui, canvas, pool, theme::native);
        std::string row = get_row(canvas, 0);
        std::println("  x=20 row[0]='{}'", row);
        assert(row.starts_with("0123456789"));
    }

    std::println("PASS\n");
}

// ============================================================================
// 9. Renderer: both axes simultaneously
// ============================================================================
//
// Grid of single-char cells in h()-of-h() form so each cell has a known
// natural size — same reason as test_renderer_horizontal_scroll.
TEST_CASE("renderer 2d scroll") {
    std::println("--- test_renderer_2d_scroll ---");
    StylePool pool;

    // Build rows × cols grid. Each cell is two text elements ("Rn" "Cn")
    // so we can verify both axes at once from the cell's first char.
    auto make_grid = [](int rows, int cols) {
        std::vector<Element> grid_rows;
        grid_rows.reserve(static_cast<std::size_t>(rows));
        for (int r = 0; r < rows; ++r) {
            std::vector<Element> cells;
            cells.reserve(static_cast<std::size_t>(cols) * 2);
            for (int c = 0; c < cols; ++c) {
                cells.push_back(text(std::string(1, static_cast<char>('A' + (r % 26)))));
                cells.push_back(text(std::string(1, static_cast<char>('0' + (c % 10)))));
            }
            grid_rows.push_back(h(std::move(cells)).build());
        }
        return grid_rows;
    };

    Canvas canvas(8, 4, &pool);
    ScrollState s;
    s.x = 6;       // skip 3 col-pairs (each pair is 2 chars wide)
    s.y = 2;       // skip 2 rows

    Element ui = (v(make_grid(/*rows=*/10, /*cols=*/15)) | scroll(s, /*w=*/8, /*h=*/4)).build();
    render_tree(ui, canvas, pool, theme::native);
    dump(canvas);

    // Debug: dump canvas without trimming so we can see every cell.
    for (int y = 0; y < canvas.height(); ++y) {
        std::string s_row;
        for (int x = 0; x < canvas.width(); ++x) {
            Cell c = canvas.get(x, y);
            char ch = (c.character >= 0x20 && c.character < 0x7F)
                ? static_cast<char>(c.character) : '.';
            s_row += ch;
        }
        std::println("  raw[{}]='{}'", y, s_row);
    }

    // First visible row index = 2 → letter 'C'.
    // First visible column-pair = 6/2 = 3 (cols 0..2 hidden) → starts 'C3'.
    std::string r0 = get_row(canvas, 0);
    std::println("  r0='{}'", r0);
    assert(!r0.empty());
    assert(r0[0] == 'C');
    assert(r0[1] == '3');
    assert(get_row(canvas, 1)[0] == 'D');
    assert(get_row(canvas, 2)[0] == 'E');
    assert(get_row(canvas, 3)[0] == 'F');

    std::println("PASS\n");
}

// ============================================================================
// 10. max_x / max_y written back after layout
// ============================================================================
TEST_CASE("max offset writeback") {
    std::println("--- test_max_offset_writeback ---");
    StylePool pool;

    // Vertical
    {
        ScrollState s;
        Canvas canvas(20, 8, &pool);
        Element ui = (v(make_rows(30)) | scroll(s, /*h=*/8)).build();
        render_tree(ui, canvas, pool, theme::native);
        std::println("  vertical:   max_y={} (expected 22)", s.max_y);
        assert(s.max_y == 22);   // 30 rows - 8 viewport
        assert(s.max_x == 0);
    }

    // Horizontal (row of 30 single-char cells, viewport_w = 10)
    {
        ScrollState s;
        Canvas canvas(10, 1, &pool);
        std::vector<Element> cells;
        for (int i = 0; i < 30; ++i)
            cells.push_back(text(std::string(1, 'X')));
        Element ui = (h(std::move(cells)) | scrollx(s, /*w=*/10)).build();
        render_tree(ui, canvas, pool, theme::native);
        std::println("  horizontal: max_x={} (expected 20)", s.max_x);
        assert(s.max_x == 20);
    }

    std::println("PASS\n");
}

// ============================================================================
// 11. Stale offset is clamped after layout
// ============================================================================
TEST_CASE("stale offset clamps") {
    std::println("--- test_stale_offset_clamps ---");
    StylePool pool;
    ScrollState s;
    s.y = 9999;       // ridiculously past content end
    Canvas canvas(20, 8, &pool);
    Element ui = (v(make_rows(30)) | scroll(s, /*h=*/8)).build();
    render_tree(ui, canvas, pool, theme::native);
    std::println("  y after render: {} (expected <= 22)", s.y);
    assert(s.y == 22);   // clamped to max_y
    std::println("PASS\n");
}

// ============================================================================
// 12. scrollbar_y — thumb position and proportional size
// ============================================================================
TEST_CASE("scrollbar y") {
    std::println("--- test_scrollbar_y ---");
    StylePool pool;

    // 10-row viewport, 40-row content → max_y = 30
    // thumb_size = max(1, 10*10 / (10+30)) = max(1, 100/40) = 2
    // thumb_pos at y=0 should be 0; at y=30 should be 10-2 = 8.
    ScrollState s;
    s.max_y = 30;

    // y = 0 — thumb at top of column.
    {
        Canvas canvas(1, 10, &pool);
        s.y = 0;
        render_tree(scrollbar_y(s, 10), canvas, pool, theme::native);
        dump(canvas);
        std::string col;
        for (int y = 0; y < 10; ++y) col += get_row(canvas, y).empty() ? '.' : '#';
        std::println("  y=0  col='{}'", col);
        // First two rows are thumb (#), rest is track (#) — both render as
        // box-drawing chars and our get_row maps any >= U+2500 to '#'.
        // So we can't distinguish thumb vs track at the canvas level, but
        // we CAN verify that the cell at row 0 is non-empty and that the
        // total emitted column is 10 rows tall.
        assert(col.size() == 10);
        for (char c : col) assert(c == '#');
    }

    // y = max_y — thumb should sit at the bottom of the column. We verify
    // by inspecting the actual emitted Element's children sizes: with this
    // state, the thumb should be at index thumb_pos=8 of the 10-row track.
    {
        Canvas canvas(1, 10, &pool);
        s.y = 30;   // at the bottom
        Element bar = scrollbar_y(s, 10);
        render_tree(bar, canvas, pool, theme::native);
        dump(canvas);
        // The element is a vbox with up to 3 children: top track, thumb,
        // bottom track. With thumb_pos = 8 and thumb_size = 2, we expect
        // exactly 2 children: an 8-row track and a 2-row thumb (no bottom
        // track since 8+2 == viewport_h).
        auto* box = as_box(bar);
        assert(box != nullptr);
        std::println("  y=max  box children: {}", box->children.size());
        assert(box->children.size() == 2);
    }

    // Content fits (max_y = 0) — thumb fills the whole track.
    {
        ScrollState fit;
        fit.max_y = 0;
        Element bar = scrollbar_y(fit, 6);
        auto* box = as_box(bar);
        assert(box != nullptr);
        // thumb_size = max(1, 6*6 / (6+0)) = 6 → fills entire track,
        // and thumb_pos = 0, so only one child (the thumb).
        std::println("  fit    box children: {}", box->children.size());
        assert(box->children.size() == 1);
    }

    std::println("PASS\n");
}

// ============================================================================
// 13. scrollbar_x — symmetric to scrollbar_y on the x axis
// ============================================================================
TEST_CASE("scrollbar x") {
    std::println("--- test_scrollbar_x ---");
    StylePool pool;
    ScrollState s;
    s.max_x = 30;

    {
        s.x = 0;
        Element bar = scrollbar_x(s, 10);
        auto* box = as_box(bar);
        assert(box != nullptr);
        std::println("  x=0  children={}", box->children.size());
        // thumb at left (pos=0), size=2 → thumb + right-track == 2 children
        assert(box->children.size() == 2);
    }
    {
        s.x = 30;
        Element bar = scrollbar_x(s, 10);
        auto* box = as_box(bar);
        assert(box != nullptr);
        std::println("  x=max children={}", box->children.size());
        // thumb at right edge → left-track + thumb == 2 children
        assert(box->children.size() == 2);
    }
    {
        s.x = 15;
        Element bar = scrollbar_x(s, 10);
        auto* box = as_box(bar);
        assert(box != nullptr);
        std::println("  x=mid children={}", box->children.size());
        // thumb in middle → all 3 segments present
        assert(box->children.size() == 3);
    }

    std::println("PASS\n");
}

// ============================================================================
// 14. Backwards-compat: Scrollable widget still works (delegates to primitive)
// ============================================================================
TEST_CASE("scrollable widget compat") {
    std::println("--- test_scrollable_widget_compat ---");
    StylePool pool;
    Scrollable view({.height = 8, .show_indicator = false});
    view.set_content(v(make_rows(30)).build());
    // Legacy callers prime max_y via set_content_height so scroll calls
    // before the first render clamp correctly. After render, the writeback
    // takes over.
    view.set_content_height(30);
    view.scroll_down(5);

    Canvas canvas(20, 8, &pool);
    render_tree(view.build(), canvas, pool, theme::native);
    dump(canvas);
    assert(get_row(canvas, 0).find("row05") != std::string::npos);
    assert(view.offset() == 5);

    // After render, content_height reports back: max_y + viewport == 30
    assert(view.content_height() == 30);

    std::println("PASS\n");
}

// ============================================================================
// 14b. Hover over horizontal scrollbar → wheel pans horizontally
// ============================================================================
TEST_CASE("hover over hbar") {
    std::println("--- test_hover_over_hbar ---");
    StylePool pool;
    ScrollState state;
    state.step_x = 2;

    // Build content that's both wider AND taller than the viewport so
    // max_x AND max_y are both > 0 — needed to exercise both axes.
    auto wide_rows = []() {
        std::vector<Element> out;
        for (int i = 0; i < 20; ++i) {
            char buf[40];
            std::snprintf(buf, sizeof(buf), "row %02d — wide line content here", i);
            out.push_back(text(std::string{buf}));
        }
        return out;
    };

    // Render: 20×5 viewport with the bar directly underneath.
    Canvas canvas(30, 7, &pool);
    Element ui = v(
        v(wide_rows()) | scroll(state, /*w=*/20, /*h=*/5),
        scrollbar_x(state, /*w=*/20)
    ).build();
    render_tree(ui, canvas, pool, theme::native);

    std::println("  max_x={}  max_y={}", state.max_x, state.max_y);
    std::println("  bar_h_bounds: x={} y={} w={} h={}",
                 state.bar_h_bounds.x, state.bar_h_bounds.y,
                 state.bar_h_bounds.w, state.bar_h_bounds.h);
    assert(state.bar_h_bounds.h > 0);
    assert(state.max_x > 0 && state.max_y > 0);

    const int bar_row = state.bar_h_bounds.y;
    const int bar_col = state.bar_h_bounds.x + 2;

    // Wheel-down ON the horizontal scrollbar → horizontal pan, NOT vertical.
    state.x = 0;
    state.y = 0;
    assert(state.handle(wheel_at(MouseButton::ScrollDown, bar_col, bar_row)));
    std::println("  on h-bar:  x={} y={} (expect x=2 y=0)", state.x, state.y);
    assert(state.x == 2);
    assert(state.y == 0);

    // Wheel-down OUTSIDE the bar (e.g., row 0) → vertical pan (default).
    state.x = 0;
    state.y = 0;
    assert(state.handle(wheel_at(MouseButton::ScrollDown, 0, 0)));
    std::println("  off bar:   x={} y={} (expect x=0 y=1)", state.x, state.y);
    assert(state.x == 0);
    assert(state.y == 1);

    std::println("PASS\n");
}

// ============================================================================
// 14c. Click + drag on horizontal scrollbar — absolute jump + continuous drag
// ============================================================================
TEST_CASE("click drag h bar") {
    std::println("--- test_click_drag_h_bar ---");
    StylePool pool;
    ScrollState state;

    // Build a 2D viewport + h-bar so the renderer populates bar_h_bounds.
    auto wide_rows = []() {
        std::vector<Element> out;
        for (int i = 0; i < 20; ++i) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "row %02d — long line of content for scroll", i);
            out.push_back(text(std::string{buf}));
        }
        return out;
    };
    // Use a tight canvas so the outer v doesn't cross-stretch the bar
    // beyond its requested width.
    Canvas canvas(20, 7, &pool);
    Element ui = v(
        v(wide_rows()) | scroll(state, /*w=*/20, /*h=*/5),
        scrollbar_x(state, /*w=*/20)
    ).build();
    render_tree(ui, canvas, pool, theme::native);

    std::println("  bar_h_bounds: x={} y={} w={} h={}",
                 state.bar_h_bounds.x, state.bar_h_bounds.y,
                 state.bar_h_bounds.w, state.bar_h_bounds.h);
    assert(state.bar_h_bounds.w > 0);
    assert(state.max_x > 0);
    const int bar_y = state.bar_h_bounds.y;
    const int bar_x = state.bar_h_bounds.x;
    const int bar_w = state.bar_h_bounds.w;

    // Click at the left edge of the bar → x = 0.
    state.x = 5;   // anywhere
    assert(state.handle(click_at(bar_x, bar_y)));
    std::println("  click left:  x={} (expect 0), dragging_h={}", state.x, state.dragging_h);
    assert(state.x == 0);
    assert(state.dragging_h);

    // Move right while dragging → x increases.
    assert(state.handle(move_at(bar_x + bar_w / 2, bar_y)));
    std::println("  move mid:    x={} (expect roughly max_x/2)", state.x);
    assert(state.x > 0 && state.x < state.max_x);

    // Move all the way right → x = max_x.
    assert(state.handle(move_at(bar_x + bar_w - 1, bar_y)));
    std::println("  move right:  x={} (expect max_x={})", state.x, state.max_x);
    assert(state.x == state.max_x);

    // Release → drag ends.
    assert(state.handle(release_at(bar_x + bar_w - 1, bar_y)));
    assert(!state.dragging_h);

    // Move after release should NOT change x.
    const int x_before = state.x;
    assert(!state.handle(move_at(bar_x, bar_y)));
    assert(state.x == x_before);

    std::println("PASS\n");
}

// ============================================================================
// 14d. Click + drag on vertical scrollbar — same but on y axis
// ============================================================================
TEST_CASE("click drag v bar") {
    std::println("--- test_click_drag_v_bar ---");
    StylePool pool;
    ScrollState state;

    Canvas canvas(20, 8, &pool);
    Element ui = h(
        v(make_rows(30)) | scroll(state, /*h=*/8),
        scrollbar_y(state, /*h=*/8)
    ).build();
    render_tree(ui, canvas, pool, theme::native);

    std::println("  bar_v_bounds: x={} y={} w={} h={}",
                 state.bar_v_bounds.x, state.bar_v_bounds.y,
                 state.bar_v_bounds.w, state.bar_v_bounds.h);
    assert(state.bar_v_bounds.h > 0);
    assert(state.max_y > 0);
    const int bar_x = state.bar_v_bounds.x;
    const int bar_y = state.bar_v_bounds.y;
    const int bar_h = state.bar_v_bounds.h;

    // Click at top → y = 0.
    state.y = 10;
    assert(state.handle(click_at(bar_x, bar_y)));
    assert(state.y == 0);
    assert(state.dragging_v);

    // Click+drag to bottom → y = max_y.
    assert(state.handle(move_at(bar_x, bar_y + bar_h - 1)));
    assert(state.y == state.max_y);

    // Release.
    assert(state.handle(release_at(bar_x, bar_y + bar_h - 1)));
    assert(!state.dragging_v);

    std::println("PASS\n");
}

// ============================================================================
// 14d2. Lag-free property — dragging the thumb by N cells moves the thumb's
//       painted position by exactly N cells (cursor stays glued to the same
//       point on the thumb).
// ============================================================================
TEST_CASE("drag is lag free") {
    std::println("--- test_drag_is_lag_free ---");
    StylePool pool;
    ScrollState state;

    // Wide content so thumb is fat (max lag risk). 160-char rows in a
    // 40-wide viewport → thumb_w around 10. Lag would be ~30/39 cells
    // per cursor cell with proportional mapping; should be 1:1 now.
    auto wide_rows = []() {
        std::vector<Element> out;
        for (int i = 0; i < 20; ++i) {
            std::string line = "row " + std::to_string(i);
            while (line.size() < 160) line += " . X . X . X . X";
            out.push_back(text(line.substr(0, 160)));
        }
        return out;
    };

    Canvas canvas(40, 7, &pool);
    Element ui = v(
        v(wide_rows()) | scroll(state, /*w=*/40, /*h=*/5),
        scrollbar_x(state, /*w=*/40)
    ).build();
    render_tree(ui, canvas, pool, theme::native);

    assert(state.bar_h_bounds.w == 40);
    assert(state.max_x > 0);
    const int tw = state.thumb_w(state.bar_h_bounds);
    std::println("  bar_w={} thumb_w={} max_x={}", state.bar_h_bounds.w, tw, state.max_x);

    // Compute initial thumb position (with state.x = 0).
    auto thumb_x_in_bar = [&](int x_val) {
        const int track = std::max(1, state.bar_h_bounds.w - tw);
        return track * x_val / std::max(1, state.max_x);
    };

    const int initial_thumb_x = thumb_x_in_bar(state.x);
    // Click on the thumb at offset 3 (inside thumb).
    const int click_col = state.bar_h_bounds.x + initial_thumb_x + 3;
    const int click_row = state.bar_h_bounds.y;
    assert(state.handle(click_at(click_col, click_row)));
    assert(state.drag_offset_h == 3);   // recorded the offset
    // Cursor is on thumb → no jump, x stays at 0.
    assert(state.x == 0);

    // Drag right by 5 cells.
    assert(state.handle(move_at(click_col + 5, click_row)));
    const int new_thumb_x = thumb_x_in_bar(state.x);
    std::println("  after drag +5: thumb moved {} cells (expect 5)",
                 new_thumb_x - initial_thumb_x);
    assert(new_thumb_x - initial_thumb_x == 5);

    // Drag right by another 7 cells (total +12).
    assert(state.handle(move_at(click_col + 12, click_row)));
    const int next_thumb_x = thumb_x_in_bar(state.x);
    std::println("  after drag +12: thumb moved {} cells (expect 12)",
                 next_thumb_x - initial_thumb_x);
    assert(next_thumb_x - initial_thumb_x == 12);

    // Drag back left by 8 cells (net +4).
    assert(state.handle(move_at(click_col + 4, click_row)));
    const int back_thumb_x = thumb_x_in_bar(state.x);
    std::println("  after drag +4: thumb at offset {} (expect {})",
                 back_thumb_x - initial_thumb_x, 4);
    assert(back_thumb_x - initial_thumb_x == 4);

    (void)state.handle(release_at(click_col + 4, click_row));
    assert(!state.dragging_h);

    std::println("PASS\n");
}

// ============================================================================
// 14d2b. Stretched bar — natural extent, not painted outer width, drives
//        hit-test. Without this, putting a scrollbar inside a wider parent
//        causes the drag math to diverge from the rendered thumb position
//        because the bar widget paints `viewport_w` cells but flexbox
//        cross-stretches the container.
// ============================================================================
TEST_CASE("stretched bar uses natural extent") {
    std::println("--- test_stretched_bar_uses_natural_extent ---");
    StylePool pool;
    ScrollState state;

    auto wide_rows = []() {
        std::vector<Element> out;
        for (int i = 0; i < 20; ++i) {
            char buf[80];
            std::snprintf(buf, sizeof(buf),
                "row %02d a very long line so max_x > 0", i);
            out.push_back(text(std::string{buf}));
        }
        return out;
    };

    // Tile is 30 wide, scrollbar is requested at viewport_w=12. Without
    // the fix the bar gets cross-stretched to 30, hit-test uses 30, and
    // the rendered thumb (painted at viewport_w=12) ends up offset from
    // the cursor.
    Canvas canvas(30, 10, &pool);
    Element ui = v(
        v(wide_rows()) | scroll(state, /*w=*/12, /*h=*/6),
        scrollbar_x(state, /*w=*/12)
    ).build();
    render_tree(ui, canvas, pool, theme::native);

    std::println("  bar.w={} (expect 12)  max_x={}", state.bar_h_bounds.w, state.max_x);
    assert(state.bar_h_bounds.w == 12);
    assert(state.max_x > 0);

    std::println("PASS\n");
}

// ============================================================================
// 14d3. Multi-bar — clicking ANY of several bars all driven by one state
//        starts a drag on the correct bar (showcase pattern).
// ============================================================================
TEST_CASE("multiple bars one state") {
    std::println("--- test_multiple_bars_one_state ---");
    StylePool pool;
    ScrollState state;

    auto wide_rows = []() {
        std::vector<Element> out;
        for (int i = 0; i < 20; ++i) {
            char buf[80];
            std::snprintf(buf, sizeof(buf),
                "row %02d  very long line content for horizontal scroll", i);
            out.push_back(text(std::string{buf}));
        }
        return out;
    };

    // One viewport + THREE horizontal bars all sharing the state.
    Canvas canvas(40, 12, &pool);
    Element ui = v(
        v(wide_rows()) | scroll(state, /*w=*/40, /*h=*/5),
        scrollbar_x(state, /*w=*/40),   // bar 0
        scrollbar_x(state, /*w=*/40),   // bar 1
        scrollbar_x(state, /*w=*/40)    // bar 2
    ).build();
    render_tree(ui, canvas, pool, theme::native);

    std::println("  bars_h.size()={}  max_x={}", state.bars_h.size(), state.max_x);
    assert(state.bars_h.size() == 3);
    assert(state.max_x > 0);

    // Click the MIDDLE bar (bar 1). drag_bar should equal that bar.
    const auto& bar1 = state.bars_h[1];
    assert(state.handle(click_at(bar1.x + 5, bar1.y)));
    assert(state.dragging_h);
    assert(state.drag_bar.y == bar1.y);

    // Click the LAST bar (bar 2). drag_bar should switch to that bar.
    const auto& bar2 = state.bars_h[2];
    assert(state.handle(click_at(bar2.x + 8, bar2.y)));
    assert(state.dragging_h);
    assert(state.drag_bar.y == bar2.y);

    (void)state.handle(release_at(bar2.x + 8, bar2.y));
    assert(!state.dragging_h);

    std::println("PASS\n");
}

// ============================================================================
// 14e. Drag robustness — Right Release doesn't cancel an active Left drag
// ============================================================================
TEST_CASE("drag right release does not cancel") {
    std::println("--- test_drag_right_release_does_not_cancel ---");
    StylePool pool;
    ScrollState state;
    auto wide_rows = []() {
        std::vector<Element> out;
        for (int i = 0; i < 20; ++i) {
            char buf[80];
            std::snprintf(buf, sizeof(buf),
                "row %02d  this is a very long line that needs horizontal scroll", i);
            out.push_back(text(std::string{buf}));
        }
        return out;
    };
    Canvas canvas(20, 7, &pool);
    Element ui = v(
        v(wide_rows()) | scroll(state, /*w=*/20, /*h=*/5),
        scrollbar_x(state, /*w=*/20)
    ).build();
    render_tree(ui, canvas, pool, theme::native);

    const int bar_x = state.bar_h_bounds.x;
    const int bar_y = state.bar_h_bounds.y;

    // Start a Left drag.
    assert(state.handle(click_at(bar_x, bar_y)));
    assert(state.dragging_h);

    // Right-button release arrives mid-drag (user right-clicked).
    MouseEvent right_release;
    right_release.button = MouseButton::Right;
    right_release.kind   = MouseEventKind::Release;
    right_release.x = Columns{bar_x + 1};
    right_release.y = Rows{bar_y + 1};
    assert(!state.handle(right_release));
    assert(state.dragging_h);   // still dragging

    // Left Release ends drag.
    assert(state.handle(release_at(bar_x, bar_y)));
    assert(!state.dragging_h);

    std::println("PASS\n");
}

// ============================================================================
// 14f. Drag robustness — click on bar with max=0 doesn't start a stuck drag
// ============================================================================
TEST_CASE("drag skipped when no scroll room") {
    std::println("--- test_drag_skipped_when_no_scroll_room ---");
    StylePool pool;
    ScrollState state;
    // Tiny content that fits the viewport → max_x = 0.
    Canvas canvas(20, 7, &pool);
    Element ui = v(
        v(text("short")) | scroll(state, /*w=*/20, /*h=*/5),
        scrollbar_x(state, /*w=*/20)
    ).build();
    render_tree(ui, canvas, pool, theme::native);

    std::println("  max_x={}  bar_h_bounds.w={}", state.max_x, state.bar_h_bounds.w);
    assert(state.max_x == 0);
    assert(state.bar_h_bounds.w > 0);   // bar is painted but inert

    // Click on the bar — should NOT start a drag.
    const int bar_x = state.bar_h_bounds.x;
    const int bar_y = state.bar_h_bounds.y;
    assert(!state.handle(click_at(bar_x + 2, bar_y)));
    assert(!state.dragging_h);
    assert(!state.dragging_v);

    std::println("PASS\n");
}

// ============================================================================
// 14g. Drag robustness — Press on the other bar mid-drag switches axes cleanly
// ============================================================================
TEST_CASE("drag axis switch does not dual flag") {
    std::println("--- test_drag_axis_switch_does_not_dual_flag ---");
    StylePool pool;
    ScrollState state;
    auto wide_rows = []() {
        std::vector<Element> out;
        for (int i = 0; i < 30; ++i)
            out.push_back(text("row " + std::to_string(i) + " long content here"));
        return out;
    };
    Canvas canvas(22, 10, &pool);
    Element ui = v(
        h(
            v(wide_rows()) | scroll(state, /*w=*/20, /*h=*/8),
            scrollbar_y(state, /*h=*/8)
        ),
        scrollbar_x(state, /*w=*/20)
    ).build();
    render_tree(ui, canvas, pool, theme::native);

    assert(state.max_x > 0 && state.max_y > 0);

    // Start dragging the horizontal bar.
    assert(state.handle(click_at(state.bar_h_bounds.x, state.bar_h_bounds.y)));
    assert(state.dragging_h);
    assert(!state.dragging_v);

    // Press lands on the v-bar without releasing — axes must switch,
    // not both be true.
    assert(state.handle(click_at(state.bar_v_bounds.x, state.bar_v_bounds.y)));
    assert(state.dragging_v);
    assert(!state.dragging_h);   // h was cleared on the v-bar press

    (void)state.handle(release_at(state.bar_v_bounds.x, state.bar_v_bounds.y));
    assert(!state.is_dragging());

    std::println("PASS\n");
}

// ============================================================================
// 14h. Drag robustness — end_drag() unsticks a lost-release drag
// ============================================================================
TEST_CASE("end drag recovers from lost release") {
    std::println("--- test_end_drag_recovers_from_lost_release ---");
    StylePool pool;
    ScrollState state;
    auto wide_rows = []() {
        std::vector<Element> out;
        for (int i = 0; i < 20; ++i) {
            char buf[80];
            std::snprintf(buf, sizeof(buf),
                "row %02d  this is a very long line that needs horizontal scroll", i);
            out.push_back(text(std::string{buf}));
        }
        return out;
    };
    Canvas canvas(20, 7, &pool);
    Element ui = v(
        v(wide_rows()) | scroll(state, /*w=*/20, /*h=*/5),
        scrollbar_x(state, /*w=*/20)
    ).build();
    render_tree(ui, canvas, pool, theme::native);

    // Start drag, no release ever arrives.
    assert(state.handle(click_at(state.bar_h_bounds.x, state.bar_h_bounds.y)));
    assert(state.is_dragging());

    // Recovery: caller invokes end_drag() explicitly.
    state.end_drag();
    assert(!state.is_dragging());

    // After recovery, Move events do nothing.
    const int x_before = state.x;
    assert(!state.handle(move_at(state.bar_h_bounds.x + 5, state.bar_h_bounds.y)));
    assert(state.x == x_before);

    std::println("PASS\n");
}

// ============================================================================
// 15. Edge case: content fits viewport — max_x/y both 0, scroll is no-op
// ============================================================================
TEST_CASE("content fits viewport") {
    std::println("--- test_content_fits_viewport ---");
    StylePool pool;
    ScrollState s;
    // 3 rows of content in a 10-row viewport — content fits completely.
    Canvas canvas(20, 10, &pool);
    Element ui = (v(make_rows(3)) | scroll(s, /*h=*/10)).build();
    render_tree(ui, canvas, pool, theme::native);
    std::println("  max_y after render: {} (expected 0)", s.max_y);
    assert(s.max_y == 0);

    // Scroll attempts are no-ops when content fits.
    s.scroll_by(0, +5);
    assert(s.y == 0);
    (void)s.handle(key_event(SpecialKey::Down));
    assert(s.y == 0);
    assert(s.at_top() && s.at_bottom());

    std::println("PASS\n");
}

// ============================================================================
// 16. Edge case: empty content — bounds stay at zero, no UB
// ============================================================================
TEST_CASE("empty content") {
    std::println("--- test_empty_content ---");
    StylePool pool;
    ScrollState s;
    Canvas canvas(10, 5, &pool);
    Element ui = (v() | scroll(s, /*h=*/5)).build();
    render_tree(ui, canvas, pool, theme::native);
    assert(s.max_y == 0);
    assert(s.max_x == 0);
    // Scrollbar over zero-content should still produce a valid Element
    Element bar = scrollbar_y(s, 5);
    auto* box = as_box(bar);
    assert(box != nullptr);
    // 5 rows total, max_y=0 → full-height thumb (one segment).
    assert(box->children.size() == 1);
    std::println("PASS\n");
}

// ============================================================================
// 17. Edge case: stale state.y far past max_y — clamped on render
// ============================================================================
TEST_CASE("stale state far past max") {
    std::println("--- test_stale_state_far_past_max ---");
    StylePool pool;
    ScrollState s;
    s.y = 1'000'000;  // wildly out of range
    s.x = 1'000'000;
    Canvas canvas(20, 8, &pool);
    Element ui = (v(make_rows(30)) | scroll(s, /*h=*/8)).build();
    render_tree(ui, canvas, pool, theme::native);
    // After writeback + clamp, y must be == max_y (and x == max_x = 0).
    assert(s.y == s.max_y);
    assert(s.x == 0);
    assert(s.y == 22);
    std::println("PASS\n");
}

// ============================================================================
// 18. Edge case: viewport_h = 0 → scrollbar widget returns empty Element
// ============================================================================
TEST_CASE("zero viewport scrollbar") {
    std::println("--- test_zero_viewport_scrollbar ---");
    ScrollState s;
    s.max_y = 100;
    s.max_x = 100;
    Element bar_y = scrollbar_y(s, 0);
    Element bar_x = scrollbar_x(s, 0);
    // Both return a TextElement (the empty fallback), not a box.
    assert(as_box(bar_y) == nullptr);
    assert(as_box(bar_x) == nullptr);
    std::println("PASS\n");
}

// ============================================================================
// 19. Edge case: large content (10,000 rows) — writeback math doesn't overflow
// ============================================================================
TEST_CASE("large content") {
    std::println("--- test_large_content ---");
    StylePool pool;
    ScrollState s;
    Canvas canvas(20, 10, &pool);
    Element ui = (v(make_rows(10'000)) | scroll(s, /*h=*/10)).build();
    render_tree(ui, canvas, pool, theme::native);
    std::println("  max_y: {} (expected 9990)", s.max_y);
    assert(s.max_y == 9990);
    // Jumping to end should land at max_y exactly.
    s.scroll_to_bottom();
    assert(s.y == 9990);
    // Page scroll math also OK at scale.
    s.y = 0;
    (void)s.handle(key_event(SpecialKey::PageDown), 10);
    assert(s.y == 10);
    std::println("PASS\n");
}

// ============================================================================
// 20. Edge case: writeback dirty flag fires once per max-change, not idle
// ============================================================================
TEST_CASE("writeback dirty flag") {
    std::println("--- test_writeback_dirty_flag ---");
    StylePool pool;
    ScrollState s;
    // Clear from any earlier test.
    detail::scroll_writeback_dirty = false;

    // First render with 30 rows — max_y goes 0 → 22, dirty fires.
    Canvas canvas(20, 8, &pool);
    Element ui1 = (v(make_rows(30)) | scroll(s, /*h=*/8)).build();
    render_tree(ui1, canvas, pool, theme::native);
    assert(s.max_y == 22);
    assert(detail::scroll_writeback_dirty == true);

    // Re-render with same content — max stays at 22, dirty should NOT fire.
    detail::scroll_writeback_dirty = false;
    Element ui2 = (v(make_rows(30)) | scroll(s, /*h=*/8)).build();
    render_tree(ui2, canvas, pool, theme::native);
    assert(s.max_y == 22);
    assert(detail::scroll_writeback_dirty == false);

    // Render with different content size — max changes, dirty fires.
    Element ui3 = (v(make_rows(50)) | scroll(s, /*h=*/8)).build();
    render_tree(ui3, canvas, pool, theme::native);
    assert(s.max_y == 42);
    assert(detail::scroll_writeback_dirty == true);

    std::println("PASS\n");
}

// Regression: the renderer registers &state in a non-owning thread-local
// registry (detail::live_scroll_states) during paint, and the run() loop
// dispatches events to those pointers until the NEXT render clears the list.
// A ScrollState destroyed in that window (e.g. a scrollable removed inside
// update()) must remove itself from the registry, or the next input event is a
// use-after-free. Verify the dtor and move/copy-assign keep the registry clean.
TEST_CASE("registry unregisters on destroy") {
    std::println("--- test_registry_unregisters_on_destroy ---");
    auto& reg = detail::live_scroll_states();
    reg.clear();

    // Simulate a paint registering a heap state, then destroy it.
    {
        auto* s = new ScrollState();
        reg.push_back(s);
        assert(reg.size() == 1);
        delete s;                       // <- must self-unregister
        assert(reg.empty());            // no dangling pointer left behind
    }

    // Move-assignment into a registered target drops its registration.
    {
        ScrollState a, b;
        reg.push_back(&a);
        assert(reg.size() == 1);
        a = std::move(b);               // a was registered -> unregistered
        assert(reg.empty());
    }

    // Scope-exit destruction of a registered stack state also cleans up.
    {
        ScrollState local;
        reg.push_back(&local);
        assert(reg.size() == 1);
    }
    assert(reg.empty());

    std::println("PASS\n");
}

