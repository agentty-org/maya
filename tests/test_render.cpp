// Headless render test - verifies layout + paint produce correct canvas output
#include <maya/maya.hpp>
#include <maya/print.hpp>
#include <maya/render/frame.hpp>
#include "check.hpp"
#include "agtest.hpp"
#include <print>
#if !defined(_WIN32)
#include <fcntl.h>    // ::open for the writer test
#endif
#include <string>
#include <vector>

#if defined(_WIN32)
namespace {
// The writer test needs a writable sink it can open and close around the
// flush. POSIX names /dev/null; windows names NUL, and the Writer takes a
// NativeHandle (HANDLE here, not an fd), so the open is CreateFileW.
[[nodiscard]] maya::platform::NativeHandle open_null_sink() noexcept {
    return ::CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr,
                         OPEN_EXISTING, 0, nullptr);
}
}  // namespace
#endif

using namespace maya;
using namespace maya::detail;
using namespace maya::dsl;

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

void dump(const Canvas& canvas, int rows = -1) {
    if (rows < 0) rows = canvas.height();
    for (int y = 0; y < rows && y < canvas.height(); ++y) {
        std::println("  {:2}|{}|", y, get_row(canvas, y));
    }
}

// ============================================================================
// Test 1: bare text element fills root
// ============================================================================
TEST_CASE("bare text") {
    std::println("--- test_bare_text ---");
    StylePool pool;
    Canvas canvas(30, 3, &pool);
    render_tree(text("hello"), canvas, pool, theme::native);
    dump(canvas);
    assert(get_row(canvas, 0).starts_with("hello"));
    std::println("PASS\n");
}

// ============================================================================
// Test 2: column box with padding
// ============================================================================
TEST_CASE("column padding") {
    std::println("--- test_column_padding ---");
    StylePool pool;
    Canvas canvas(30, 8, &pool);
    render_tree(
        box().direction(Column).padding(1)(
            text("AAA"),
            text("BBB")
        ), canvas, pool, theme::native);
    dump(canvas);
    // padding=1 → content starts at (1,1)
    assert(get_row(canvas, 0).empty()); // top padding row
    assert(get_row(canvas, 1).find("AAA") != std::string::npos);
    assert(get_row(canvas, 2).find("BBB") != std::string::npos);
    std::println("PASS\n");
}

// ============================================================================
// Test 3: row layout places children horizontally
// ============================================================================
TEST_CASE("row layout") {
    std::println("--- test_row_layout ---");
    StylePool pool;
    Canvas canvas(30, 3, &pool);
    render_tree(
        box().direction(Row)(
            text("LEFT"),
            text("RIGHT")
        ), canvas, pool, theme::native);
    dump(canvas);
    std::string r = get_row(canvas, 0);
    auto lpos = r.find("LEFT");
    auto rpos = r.find("RIGHT");
    std::println("  LEFT@{} RIGHT@{}", lpos == std::string::npos ? -1 : (int)lpos,
                 rpos == std::string::npos ? -1 : (int)rpos);
    assert(lpos != std::string::npos);
    assert(rpos != std::string::npos);
    assert(rpos > lpos);
    std::println("PASS\n");
}

// ============================================================================
// Test 4: flex-grow spacer pushes content to bottom
// ============================================================================
TEST_CASE("spacer") {
    std::println("--- test_spacer ---");
    StylePool pool;
    Canvas canvas(20, 10, &pool);
    render_tree(
        box().direction(Column)(
            text("TOP"),
            spacer(),
            text("BOT")
        ), canvas, pool, theme::native);
    dump(canvas);
    assert(get_row(canvas, 0).find("TOP") != std::string::npos);
    assert(get_row(canvas, 9).find("BOT") != std::string::npos);
    std::println("PASS\n");
}

// ============================================================================
// Test 5: border draws box-drawing characters
// ============================================================================
TEST_CASE("border") {
    std::println("--- test_border ---");
    StylePool pool;
    Canvas canvas(20, 5, &pool);
    render_tree(
        box().direction(Column).border(BorderStyle::Single)(
            text("hi")
        ), canvas, pool, theme::native);
    dump(canvas);
    Cell tl = canvas.get(0, 0);
    std::println("  corner U+{:04X}", (uint32_t)tl.character);
    assert(tl.character != U' '); // should be a box-drawing char
    // "hi" should be inside the border at (1,1)
    assert(get_row(canvas, 1).find("hi") != std::string::npos);
    std::println("PASS\n");
}

// ============================================================================
// Test 6: nested boxes
// ============================================================================
TEST_CASE("nested") {
    std::println("--- test_nested ---");
    StylePool pool;
    Canvas canvas(40, 10, &pool);
    render_tree(
        box().direction(Column).padding(1)(
            text("title"),
            box().direction(Row).gap(1)(
                text("A"),
                text("B")
            )
        ), canvas, pool, theme::native);
    dump(canvas, 5);
    assert(get_row(canvas, 1).find("title") != std::string::npos);
    std::string r2 = get_row(canvas, 2);
    assert(r2.find("A") != std::string::npos);
    assert(r2.find("B") != std::string::npos);
    std::println("PASS\n");
}

// ============================================================================
// Test 7: counter example element tree
// ============================================================================
TEST_CASE("counter tree") {
    std::println("--- test_counter_tree ---");
    StylePool pool;
    Canvas canvas(40, 10, &pool);
    render_tree(
        box().direction(Column).padding(1)(
            text("Counter: 0", bold_style),
            text("[+/-] change  [q] quit", dim_style)
        ), canvas, pool, theme::native);
    dump(canvas, 5);
    assert(get_row(canvas, 1).find("Counter: 0") != std::string::npos);
    assert(get_row(canvas, 2).find("[+/-]") != std::string::npos);
    std::println("PASS\n");
}

// ============================================================================
// Test 8: diff produces ops for changed cells
// ============================================================================
TEST_CASE("diff") {
    std::println("--- test_diff ---");
    StylePool pool;
    Canvas old_canvas(20, 3, &pool);
    Canvas new_canvas(20, 3, &pool);

    // Write different content to each
    render_tree(text("before"), old_canvas, pool, theme::native);
    render_tree(text("after"), new_canvas, pool, theme::native);

    // Verify cells are actually different
    std::println("  old[0]={:c} new[0]={:c}",
        (char)old_canvas.get(0, 0).character,
        (char)new_canvas.get(0, 0).character);
    std::println("  old packed={:#x} new packed={:#x}",
        old_canvas.get_packed(0, 0), new_canvas.get_packed(0, 0));
    auto dmg = new_canvas.damage();
    std::println("  damage: ({},{}) {}x{}",
        dmg.pos.x.value, dmg.pos.y.value,
        dmg.size.width.value, dmg.size.height.value);

    std::string result;
    diff(old_canvas, new_canvas, pool, result);
    std::println("  diff bytes: {}", result.size());
    assert(!result.empty());
    std::println("PASS\n");
}

// ============================================================================
// Test 9: FrameBuffer render + diff cycle
// ============================================================================
TEST_CASE("framebuffer") {
    std::println("--- test_framebuffer ---");
    FrameBuffer fb(30, 5);

    // First frame: full repaint
    const auto& out1 = fb.render(text("frame1"), theme::native);
    std::println("  frame1 bytes: {}", out1.size());
    assert(!out1.empty());

    // Second frame: same content → should produce fewer bytes
    const auto& out2 = fb.render(text("frame1"), theme::native);
    std::println("  frame2 (same) bytes: {}", out2.size());

    // Third frame: different content → should produce output
    const auto& out3 = fb.render(text("frame2"), theme::native);
    std::println("  frame3 (changed) bytes: {}", out3.size());
    assert(!out3.empty());
    std::println("PASS\n");
}

// ============================================================================
// Test 10: input parser basics
// ============================================================================
TEST_CASE("input parser") {
    std::println("--- test_input_parser ---");
    InputParser parser;

    // Regular character
    auto events = parser.feed("a");
    std::println("  'a' -> {} events", events.size());
    assert(!events.empty());
    auto* ke = std::get_if<KeyEvent>(&events[0]);
    assert(ke != nullptr);
    auto* ck = std::get_if<CharKey>(&ke->key);
    assert(ck != nullptr && ck->codepoint == 'a');

    // Escape sequence for arrow up: ESC [ A
    events = parser.feed("\x1b[A");
    std::println("  ESC[A -> {} events", events.size());
    if (!events.empty()) {
        ke = std::get_if<KeyEvent>(&events[0]);
        if (ke) {
            auto* sk = std::get_if<SpecialKey>(&ke->key);
            if (sk) std::println("  special key: {}", static_cast<int>(*sk));
        }
    }

    std::println("PASS\n");
}

// ============================================================================
// Test 11: signal reactivity
// ============================================================================
TEST_CASE("signal") {
    std::println("--- test_signal ---");
    Signal<int> count{0};
    assert(count.get() == 0);

    count.set(5);
    assert(count.get() == 5);

    count.update([](int& n) { n += 10; });
    assert(count.get() == 15);

    // Computed
    auto doubled = computed([&] { return count.get() * 2; });
    assert(doubled.get() == 30);

    count.set(3);
    assert(doubled.get() == 6);

    std::println("  count={} doubled={}", count.get(), doubled.get());
    std::println("PASS\n");
}

// ============================================================================
// Test 12: style SGR generation
// ============================================================================
TEST_CASE("style sgr") {
    std::println("--- test_style_sgr ---");
    auto s = Style{}.with_bold().with_fg(Color::red());
    std::string sgr = s.to_sgr();
    std::println("  bold+red: '{}'", sgr);
    assert(sgr.find("1") != std::string::npos);  // bold = SGR 1
    assert(sgr.find("31") != std::string::npos);  // red fg = SGR 31

    auto empty_sgr = Style{}.to_sgr();
    assert(empty_sgr.empty());

    std::println("PASS\n");
}

// ============================================================================
// Test 13: StyleApplier transition
// ============================================================================
TEST_CASE("style transition") {
    std::println("--- test_style_transition ---");
    Style a = Style{}.with_bold();
    Style b = Style{}.with_bold().with_fg(Color::green());
    std::string t = ansi::StyleApplier::transition(a, b);
    std::println("  bold -> bold+green: '{}'", t);
    assert(!t.empty()); // should emit green fg
    assert(t.find("32") != std::string::npos); // green = 32

    // Same style -> empty
    std::string same = ansi::StyleApplier::transition(a, a);
    assert(same.empty());
    std::println("PASS\n");
}

// ============================================================================
// Test 14: writer batches and flushes
// ============================================================================
TEST_CASE("writer") {
    std::println("--- test_writer ---");
    // Write to the null device to test the pipeline doesn't crash
#if defined(_WIN32)
    const auto fd = open_null_sink();
    assert(fd != maya::platform::invalid_handle);
#else
    int fd = ::open("/dev/null", O_WRONLY);
    assert(fd >= 0);
#endif
    Writer w(fd);
    w.push(render_op::CursorHide{});
    w.push(render_op::Write{"hello"});
    w.push(render_op::Write{" world"});  // should merge with previous
    w.push(render_op::StyleStr{"\x1b[1m"});
    w.push(render_op::CursorShow{});
    auto status = w.flush();
#if defined(_WIN32)
    ::CloseHandle(fd);
#else
    ::close(fd);
#endif
    assert(status.has_value());
    std::println("PASS\n");
}

// ============================================================================
// Test 15: canvas regrow keys on layout height, not painted rows
// ============================================================================
// 480 painted rows + 30 blank rows + 5 painted "chrome" rows. Layout
// needs 515 > 500 (the seed canvas height), but the max PAINTED row
// inside the 500-row canvas is 479 — a `content_height >= height`
// regrow precondition never trips and the tail rows (the app chrome:
// composer, status bar) are silently clipped. The gate must key on the
// layout's computed height instead.
TEST_CASE("regrow blank boundary") {
    std::println("--- test_regrow_blank_boundary ---");
    std::vector<Element> rows;
    for (int i = 0; i < 480; ++i)
        rows.push_back(text("body row " + std::to_string(i)).build());
    for (int i = 0; i < 30; ++i)
        rows.push_back(blank().build());
    for (int i = 0; i < 5; ++i)
        rows.push_back(text("CHROME " + std::to_string(i)).build());
    std::string out = render_to_string(v(rows).build(), 80);
    MAYA_TEST_CHECK(out.find("CHROME 4") != std::string::npos,
                    "frame tail clipped: regrow gate missed because the "
                    "canvas-boundary rows are blank");
    std::println("PASS\n");
}

// ============================================================================
// Test 16: text inherits the enclosing box's bg (ambient background)
// ============================================================================
// The terminal cell model doesn't composite — write_text replaces cells
// wholesale, and a style with no bg resets to the terminal default at
// SGR time. Before ambient-bg inheritance, a box's bgc() fill survived
// only in cells no text touched: every fg-only glyph punched a
// default-bg hole in the strip (selected-row strips, filled buttons).
// Now paint_element records the nearest enclosing box bg and folds it
// into any text run that doesn't declare its own; explicit run bgs win.
TEST_CASE("ambient bg inheritance") {
    std::println("--- test_ambient_bg_inheritance ---");
    StylePool pool;
    Canvas canvas(30, 3, &pool);
    const Color strip = Color::hex(0x2c2c44);
    const Color chip  = Color::hex(0x113355);

    // Row: fg-only text + a run with its own bg, on a bg-filled h-box.
    std::vector<StyledRun> runs;
    runs.push_back({0, 3, Style{}.with_fg(Color::green())});          // "abc"
    runs.push_back({3, 3, Style{}.with_bg(chip)});                    // "def"
    Element styled{TextElement{.content = "abcdef", .style = {},
                               .wrap = TextWrap::NoWrap,
                               .runs = std::move(runs)}};
    render_tree((h(text("hi"), std::move(styled)) | bgc(strip)).build(),
                canvas, pool, theme::native);
    dump(canvas, 1);

    auto bg_at = [&](int x) -> std::optional<Color> {
        return pool.get(canvas.get(x, 0).style_id).bg;
    };
    // "hi" (cols 0-1): fg-only text must carry the strip bg, not default.
    MAYA_TEST_CHECK(bg_at(0) == strip,
                    "fg-only text glyph lost the enclosing box bg");
    // "abc" run (cols 2-4): fg-only run inherits the strip too.
    MAYA_TEST_CHECK(bg_at(2) == strip,
                    "fg-only styled run lost the enclosing box bg");
    // "def" run (cols 5-7): explicit run bg must WIN over the ambient.
    MAYA_TEST_CHECK(bg_at(5) == chip,
                    "explicit run bg was clobbered by the ambient bg");
    // Untouched fill cell (col 20): plain strip fill.
    MAYA_TEST_CHECK(bg_at(20) == strip,
                    "box fill missing outside the text span");
    // And text OUTSIDE any bg box must stay default-bg.
    Canvas c2(10, 1, &pool);
    render_tree(text("zz"), c2, pool, theme::native);
    MAYA_TEST_CHECK(!pool.get(c2.get(0, 0).style_id).bg.has_value(),
                    "ambient bg leaked outside its box scope");

    // Hash-keyed component cell cache must be ambient-aware: capture
    // cells under NO ambient, then paint the SAME hash_id inside a
    // bg strip — the cached (default-bg) cells must NOT be blitted;
    // the entry re-renders and the glyphs carry the strip bg.
    auto make_comp = [] {
        ComponentElement ce;
        ce.render = [](int, int) -> Element {
            return dsl::text("cc").build();
        };
        ce.hash_id = CacheIdBuilder{}.add("ambient-cache-probe").build();
        return Element{std::move(ce)};
    };
    Canvas c3(10, 1, &pool);
    render_tree(make_comp(), c3, pool, theme::native);          // capture, no ambient
    render_tree(make_comp(), c3, pool, theme::native);          // warm blit path
    Canvas c4(10, 1, &pool);
    render_tree((h(make_comp()) | bgc(strip)).build(), c4, pool, theme::native);
    MAYA_TEST_CHECK(pool.get(c4.get(0, 0).style_id).bg == strip,
                    "cached component cells blitted under a DIFFERENT ambient "
                    "(stale default-bg cells punched through the strip)");
    std::println("PASS\n");
}

// ============================================================================
// Test 17: hit-region registry — `| hit(id)` resolves mouse coordinates
// ============================================================================
// The painter records each hit-tagged box's ABSOLUTE painted rect;
// hit_test(x,y) returns the topmost (last painted) id; the registry
// resets every render so removed targets stop hit-testing.
TEST_CASE("hit regions") {
    std::println("--- test_hit_regions ---");
    StylePool pool;
    Canvas canvas(40, 6, &pool);

    constexpr HitId kLeft  = hit_id(1);
    constexpr HitId kRight = hit_id(2);
    constexpr HitId kRow   = hit_id(3, 7);   // parameterized: kind 3, row 7

    // Row: [left 10 cols][right 10 cols]; a nested row-target inside right.
    render_tree(
        h(
            v(text("L")) | width(10) | hit(kLeft),
            (v(
                text("head"),
                v(text("row")) | hit(kRow)
            ) | width(10) | hit(kRight))
        ).build(),
        canvas, pool, theme::native);

    // Left box owns columns 0-9.
    MAYA_TEST_CHECK(hit_test(0, 0) == std::optional<HitId>{kLeft},
                    "click at (0,0) must resolve to the left box");
    MAYA_TEST_CHECK(hit_test(9, 0) == std::optional<HitId>{kLeft},
                    "click at (9,0) must resolve to the left box");
    // Right box owns columns 10-19; its row 1 is the nested target and
    // must WIN over the enclosing box (painted later = topmost).
    MAYA_TEST_CHECK(hit_test(10, 0) == std::optional<HitId>{kRight},
                    "click at (10,0) must resolve to the right box");
    MAYA_TEST_CHECK(hit_test(10, 1) == std::optional<HitId>{kRow},
                    "nested target must beat its enclosing box (z-order)");
    MAYA_TEST_CHECK(hit_kind(*hit_test(10, 1)) == 3
                    && hit_index(*hit_test(10, 1)) == 7,
                    "hit_id kind/index round-trip");
    // Outside everything → no id.
    MAYA_TEST_CHECK(!hit_test(30, 0).has_value(),
                    "click outside all targets must miss");
    // Reverse query: where did kRight land?
    auto rr = hit_rect(kRight);
    MAYA_TEST_CHECK(rr && rr->x == 10 && rr->y == 0 && rr->w == 10,
                    "hit_rect must report the painted rect");

    // A new frame WITHOUT the targets clears them.
    render_tree(text("empty"), canvas, pool, theme::native);
    MAYA_TEST_CHECK(!hit_test(0, 0).has_value(),
                    "registry must reset on the next render");
    std::println("PASS\n");
}

