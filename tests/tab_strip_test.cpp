// tab_strip_test — the shared tab strip.
//
// The strip exists because the naive version (a row of styled labels) breaks
// at exactly the size where it matters most. These pin the properties that
// separate it from that version, because they are the ones a host
// re-implementing the strip would get wrong:
//
//   1. the ACTIVE tab is always rendered, whatever the width
//   2. what scrolled past is announced, not silently dropped
//   3. widths are DISPLAY COLUMNS, so non-ASCII labels still align
//
// Rendering note: this file reads the canvas through the same row extractor
// the rest of the widget tests use, which folds every non-ASCII glyph to '?'
// and the full block to '#'. So assertions are about ASCII text, COUNTS of
// marker glyphs, and row shape — never about a specific box-drawing byte.

#include "doctest/doctest.h"

#include <maya/render/canvas.hpp>
#include <maya/render/renderer.hpp>
#include <maya/style/theme.hpp>
#include <maya/widget/tab_strip.hpp>

#include <cstdio>
#include <string>
#include <vector>

using namespace maya;

namespace {

struct Rendered {
    std::vector<std::string> rows;
};

// One row of the canvas as text. Non-ASCII folds to '?' (see the file note),
// which is enough to COUNT marker glyphs without asserting on their bytes.
std::string row_text(const Canvas& c, int y, int width) {
    std::string s;
    for (int x = 0; x < width; ++x) {
        const auto cell = c.get(x, y);
        const char32_t ch = cell.character;
        if (ch >= 0x20 && ch < 0x7F) s += static_cast<char>(ch);
        else if (ch == 0 || ch == U' ') s += ' ';
        else s += '?';
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

Rendered render(const TabStrip& s, int width) {
    StylePool pool;
    Canvas canvas(width, 16, &pool);
    render_tree(s.build(), canvas, pool, theme::native, /*auto_height=*/true);
    Rendered out;
    for (int y = 0; y < 16; ++y) {
        auto r = row_text(canvas, y, width);
        if (r.empty() && out.rows.empty()) continue;   // leading blanks
        out.rows.push_back(std::move(r));
    }
    while (!out.rows.empty() && out.rows.back().empty()) out.rows.pop_back();
    return out;
}

bool has(const Rendered& r, std::string_view needle) {
    for (const auto& row : r.rows)
        if (row.find(needle) != std::string::npos) return true;
    return false;
}

// Count non-ASCII glyph cells in a row — the marker/rule glyphs all fold to
// '?', so this measures the underline's WIDTH without naming its bytes.
int glyphs(const std::string& row) {
    int n = 0;
    for (char c : row) if (c == '?') ++n;
    return n;
}

}  // namespace

TEST_CASE("tab strip: an empty strip renders nothing") {
    TabStrip s;
    const auto r = render(s, 40);
    CHECK(r.rows.empty());
}

TEST_CASE("tab strip: every tab shows when they all fit") {
    TabStrip s;
    s.tab("Smart Mode").tab("Tools").tab("Wire");
    s.active(0);
    const auto r = render(s, 60);
    CHECK(has(r, "Smart Mode"));
    CHECK(has(r, "Tools"));
    CHECK(has(r, "Wire"));
}

TEST_CASE("tab strip: the active tab is visible however narrow the strip") {
    // THE property. A hand-rolled strip renders left-to-right and truncates,
    // so selecting a late tab selects something drawn off-screen — the user
    // is then navigating blind.
    const char* names[] = {"alpha", "bravo", "charlie", "delta",
                           "echo", "foxtrot", "golf", "hotel"};
    TabStrip s;
    for (const char* n : names) s.tab(n);

    for (int active = 0; active < 8; ++active) {
        s.active(active);
        for (int w : {20, 30, 45}) {
            const auto r = render(s, w);
            CHECK_MESSAGE(has(r, names[active]),
                          "active tab must render (active=" << active
                          << " width=" << w << ")");
        }
    }
}

TEST_CASE("tab strip: a narrow strip drops EARLIER tabs, not the active one") {
    // Silently omitting the tail would make the strip lie about how much
    // there is; dropping the active tab would make it lie about where you
    // are. Scrolling from the left is the only option that does neither.
    const char* names[] = {"alpha", "bravo", "charlie", "delta",
                           "echo", "foxtrot", "golf", "hotel"};
    TabStrip s;
    for (const char* n : names) s.tab(n);
    s.active(7);
    const auto r = render(s, 24);
    CHECK(has(r, "hotel"));      // the active tab is in view
    CHECK(!has(r, "alpha"));    // and the ones it scrolled past are not
}

TEST_CASE("tab strip: the underline tracks the active label's width") {
    // The rule row is built in lockstep with the label row, so the two
    // cannot disagree about where a tab starts and ends.
    TabStrip s;
    s.tab("aa").tab("bbbb");
    s.active(1);
    const auto r = render(s, 40);
    REQUIRE(r.rows.size() >= 2);
    // The rule matches 'bbbb' — not 'aa', not the whole row.
    CHECK(glyphs(r.rows[1]) == 4);

    TabStrip t;
    t.tab("aa").tab("bbbb");
    t.active(0);
    const auto r2 = render(t, 40);
    REQUIRE(r2.rows.size() >= 2);
    CHECK(glyphs(r2.rows[1]) == 2);   // and it follows the selection
}

TEST_CASE("tab strip: Dot marking is a single row") {
    // A strip sharing a line budget with content cannot afford the rule.
    TabStrip s;
    s.tab("one").tab("two");
    s.active(0).marker(TabMark::Dot);
    const auto r = render(s, 40);
    CHECK(r.rows.size() == 1);   // Dot mode costs one row
    CHECK(has(r, "one"));
    CHECK(has(r, "two"));
}

TEST_CASE("tab strip: status dots and details render") {
    // The diff reviewer's shape: a per-tab status and a trailing diffstat.
    TabStrip s;
    s.tab("rope.cpp").dot(Color::green()).detail("+12 -3", Color::cyan());
    s.tab("node.hpp");
    s.active(0);
    const auto r = render(s, 60);
    CHECK(has(r, "rope.cpp"));
    CHECK(has(r, "+12 -3"));   // the detail is part of the tab
}

TEST_CASE("tab strip: a detail participates in the width arithmetic") {
    // The subtle one. A host appending its own trailing text AFTER the
    // widget measured would scroll by the wrong amount and push the active
    // tab off the edge — which is precisely why detail is a field and not
    // something the caller concatenates into the label.
    TabStrip s;
    for (int i = 0; i < 8; ++i)
        s.tab("file" + std::to_string(i)).detail("+100 -100", Color::cyan());
    s.active(7);
    const auto r = render(s, 30);
    CHECK(has(r, "file7"));   // the active tab still fits once details count
}

TEST_CASE("tab strip: the three marks are visibly different") {
    // A family, not one house style — so this both PINS each mode's shape
    // and prints them together, because "do these read as three
    // deliberate styles" is a judgement no assertion makes for you.
    auto build = [](TabMark m) {
        TabStrip s;
        s.tab("rope.cpp").tab("node.hpp").tab("notes.md");
        s.active(1).marker(m);
        return s;
    };

    const auto und = render(build(TabMark::Underline), 60);
    const auto dot = render(build(TabMark::Dot), 60);
    const auto edt = render(build(TabMark::Editor), 60);

    std::printf("\n--- Underline ---\n");
    for (const auto& r : und.rows) std::printf("|%s|\n", r.c_str());
    std::printf("--- Dot ---\n");
    for (const auto& r : dot.rows) std::printf("|%s|\n", r.c_str());
    std::printf("--- Editor ---\n");
    for (const auto& r : edt.rows) std::printf("|%s|\n", r.c_str());

    CHECK(und.rows.size() == 2);   // label + rule under the active label
    CHECK(dot.rows.size() == 1);   // one row, marker inline
    CHECK(edt.rows.size() == 1);   // one row, dividers + colour only

    // Every mode shows every tab when they fit.
    for (const auto* r : {&und, &dot, &edt}) {
        CHECK(has(*r, "rope.cpp"));
        CHECK(has(*r, "node.hpp"));
        CHECK(has(*r, "notes.md"));
    }

    // The underline rule covers only the active label.
    CHECK(glyphs(und.rows[1]) == 8);   // "node.hpp"
}

TEST_CASE("tab strip: editor mode still keeps the active tab in view") {
    // The dividers cost columns, so the scroll arithmetic has to account
    // for them — a mode that measured gaps but rendered dividers would
    // overflow by 1 column per tab and push the active one off the edge.
    TabStrip s;
    for (int i = 0; i < 10; ++i) s.tab("buffer" + std::to_string(i) + ".cpp");
    s.marker(TabMark::Editor);
    for (int active : {0, 4, 9}) {
        s.active(active);
        for (int w : {28, 40, 70}) {
            const auto r = render(s, w);
            CHECK_MESSAGE(has(r, "buffer" + std::to_string(active) + ".cpp"),
                          "editor mode keeps the active tab (i=" << active
                          << " w=" << w << ")");
        }
    }
}

TEST_CASE("tab strip: the underline marks the label and nothing else") {
    // A tab can carry a status dot and a trailing detail. Underlining those
    // too gave ONE tab three disjoint rules, which reads as three marks
    // rather than as one selection — the opposite of the indicator's job.
    TabStrip s;
    s.tab("rope.cpp").dot(Color::green()).detail("+12 -3", Color::cyan());
    s.tab("node.hpp");
    s.active(0);
    const auto r = render(s, 60);
    REQUIRE(r.rows.size() >= 2);
    CHECK(glyphs(r.rows[1]) == 8);   // exactly "rope.cpp"
}

TEST_CASE("tab strip: no mark paints a background by default") {
    // Every mark is background-free UNLESS the host asks for a fill, so a
    // strip still drops onto any surface without one. The opt-in matters:
    // a fill is the loudest mark available, and paying it by default would
    // put three signals (fill + dividers + rule) on one fact.
    for (auto m : {TabMark::Underline, TabMark::Dot, TabMark::Editor}) {
        StylePool pool;
        Canvas canvas(52, 8, &pool);
        TabStrip s;
        s.tab("alpha").tab("bravo").tab("charlie");
        s.active(1).marker(m);
        render_tree(s.build(), canvas, pool, theme::native, /*auto_height=*/true);
        bool any_bg = false;
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 52; ++x)
                if (pool.get(canvas.get(x, y).style_id).bg.has_value())
                    any_bg = true;
        CHECK(!any_bg);
    }
}

TEST_CASE("tab strip: active_bg fills the active tab and nothing else") {
    // The chip is what a ONE-TAB strip needs: marks that work by contrast
    // (bold against dim) say nothing with no neighbour to contrast against,
    // and an underline under a lone tab reads as a stray rule.
    //
    // Two properties, and the second is the one a naive fill gets wrong:
    // the fill covers the active label plus a column of air each side, and
    // it covers NOTHING else — not a divider, not a neighbour.
    for (auto m : {TabMark::Underline, TabMark::Dot, TabMark::Editor}) {
        StylePool pool;
        Canvas canvas(52, 8, &pool);
        TabStrip s;
        s.tab("alpha").tab("bravo").tab("charlie");
        s.active(1).marker(m);
        s.theme.active_bg = Color::magenta();
        render_tree(s.build(), canvas, pool, theme::native, /*auto_height=*/true);

        int filled = 0;
        std::string under_fill;
        for (int y = 0; y < 3; ++y) {
            for (int x = 0; x < 52; ++x) {
                const auto cell = canvas.get(x, y);
                if (!pool.get(cell.style_id).bg.has_value()) continue;
                ++filled;
                const char32_t ch = cell.character;
                under_fill += (ch >= 0x20 && ch < 0x7F)
                                ? static_cast<char>(ch)
                                : ' ';
            }
        }
        // "bravo" (5) plus one column of air on each side, and no more.
        CHECK(filled == 7);
        CHECK(under_fill == " bravo ");
    }
}

TEST_CASE("tab strip: a filled tab drops the underline rule") {
    // A fill and a rule beneath it are the same claim made twice — and the
    // rule is sized to the label while the chip is two columns wider, so
    // the two marks would not even agree on where the tab ends.
    TabStrip s;
    s.tab("alpha").tab("bravo");
    s.active(1).marker(TabMark::Underline);
    s.theme.active_bg = Color::magenta();

    const auto r = render(s, 52);
    REQUIRE(!r.rows.empty());
    // One row only: the rule row is gone, not merely blank.
    CHECK(r.rows.size() == 1u);
}

TEST_CASE("tab strip: an out-of-range active index never crashes") {
    // A panel is a VIEW: a bad index should show a slightly wrong strip,
    // not take the frame down.
    TabStrip s;
    s.tab("one").tab("two");
    for (int bad : {-5, -1, 2, 99}) {
        s.active(bad);
        const auto r = render(s, 40);
        CHECK(has(r, "one"));
        CHECK(has(r, "two"));
    }
}

TEST_CASE("tab strip: non-ASCII labels underline by display columns") {
    // Measuring bytes would underline ~3x too long on these.
    TabStrip s;
    s.tab("\xe6\x97\xa5\xe6\x9c\xac");   // 日本 — 2 chars, 4 columns
    s.active(0);
    const auto r = render(s, 40);
    REQUIRE(r.rows.size() >= 2);
    // Two wide glyphs underline as four columns.
    CHECK(glyphs(r.rows[1]) == 4);
}

namespace {
TabStrip pills(int active) {
    TabStrip s;
    s.marker(TabMark::Pill);
    s.theme.active_bg = Color::blue();
    s.tab("login.cpp").dot(Color::yellow(), "o").part("+2", Color::green()).part("-2", Color::red());
    s.tab("http.cpp").dot(Color::yellow(), "o").part("+9", Color::green());
    s.tab("README.md").part("-1", Color::red());
    s.active(active);
    return s;
}
}  // namespace

TEST_CASE("tab strip: Pill is one row and tabs don't move when you switch") {
    const auto a = render(pills(0), 80), b = render(pills(1), 80);
    REQUIRE(a.rows.size() == 1);
    REQUIRE(b.rows.size() == 1);
    for (const char* label : {"login.cpp", "http.cpp", "README.md"})
        CHECK(a.rows[0].find(label) == b.rows[0].find(label));
    // Every coloured part is there, joined by a space.
    CHECK(has(a, "+2 -2"));
    CHECK(has(a, "+9"));
}

TEST_CASE("tab strip: the Pill chip covers the whole active tab") {
    StylePool pool;
    Canvas canvas(80, 4, &pool);
    render_tree(pills(1).build(), canvas, pool, theme::native, true);
    const std::string row = row_text(canvas, 0, 80);
    const auto from = row.find("o http.cpp"), to = row.find("+9") + 2;
    REQUIRE(from != std::string::npos);
    // The dot, the name and the diffstat all sit on the fill...
    for (auto x = from; x < to; ++x)
        CHECK(pool.get(canvas.get(static_cast<int>(x), 0).style_id).bg.has_value());
    // ...the idle tabs don't...
    const auto idle = row.find("login.cpp");
    CHECK_FALSE(pool.get(canvas.get(static_cast<int>(idle), 0).style_id).bg.has_value());
    // ...and a coloured part keeps its own colour inside the chip.
    const auto plus = row.find("+9");
    CHECK(pool.get(canvas.get(static_cast<int>(plus), 0).style_id).fg == Color::green());
}
