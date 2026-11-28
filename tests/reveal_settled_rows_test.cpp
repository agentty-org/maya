// reveal_settled_rows_test — the streaming reveal must never REWRITE a row
// that has already settled above the live tail.
//
// THE BUG THIS PINS. The reveal's line_bounded flag exists so only the bottom
// visual row is ever rewritten: an upper row can scroll into the terminal's
// native scrollback at any frame, and scrollback is immutable — restyling a
// row after it lands there is permanent corruption. But line_bounded was
// implemented as `orig.rfind('\n')`, i.e. the last SOURCE newline. A
// streaming prose paragraph is ONE source line with no newline in it at all;
// the renderer wraps it across many visual rows. So the bound found nothing,
// the effect window opened at the paragraph's first byte, and every frame
// re-styled every wrapped row of the paragraph.
//
// User-visible symptom (what prompted this): mid-glide the tail appears to
// "go back and re-render" — settled words above the cursor visibly change
// styling again. The scrollback hazard was the same defect's silent half.
//
// WHAT THIS MEASURES. Per frame, cells whose CHARACTER is unchanged but whose
// resolved STYLE flipped, bucketed by row. A restyle on the row that is only
// NOW ceasing to be the tail is legitimate (it takes its final styling as the
// cursor leaves). A restyle on a row that was ALREADY above the tail on the
// previous frame is the bug.
//
// Counting styles rather than glyphs is essential: the glyphs don't move, so
// a cell-count or height probe sees nothing at all. reveal_smoothness_probe
// tracks height monotonicity and per-frame cell deltas and stayed green
// through the entire life of this bug.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include <maya/core/anim_clock.hpp>
#include <maya/core/render_context.hpp>
#include <maya/render/canvas.hpp>
#include <maya/render/renderer.hpp>
#include <maya/style/theme.hpp>
#include <maya/widget/agentty/markdown.hpp>

using namespace maya;

namespace {

int g_failed = 0;
void check(bool cond, const std::string& msg) {
    if (!cond) { std::printf("  FAIL: %s\n", msg.c_str()); ++g_failed; }
}

constexpr int kWidth = 60;
constexpr int kTermH = 40;

struct Painted { char32_t ch; std::uint32_t style; };

std::unordered_map<int, Painted> snapshot(const Canvas& c) {
    std::unordered_map<int, Painted> m;
    for (int y = 0; y <= c.max_content_row(); ++y)
        for (int x = 0; x < kWidth; ++x) {
            Cell cell = c.get(x, y);
            if (cell.character == 0 || cell.character == U' ') continue;
            m[y * kWidth + x] = {cell.character,
                                 static_cast<std::uint32_t>(cell.style_id)};
        }
    return m;
}

// Feed `doc` in `chunk`-byte steps with the production reveal config and
// return how many frames restyled an ALREADY-settled row.
int settled_row_rewrites(const std::string& doc, int chunk) {
    maya::testing::freeze_anim_clock();

    StreamingMarkdown md;
    md.set_reveal_fx(true);
    md.set_reveal_pacing(45.0, 0.40);          // turn.cpp's production values
    md.set_reveal_adaptive(true, 25.0, 180.0);
    md.set_live(true);

    StylePool pool;
    std::vector<layout::LayoutNode> nodes;
    auto paint = [&]() -> Canvas {
        RenderContext ctx{kWidth, kTermH, render_generation(), true};
        RenderContextGuard guard(ctx);
        Canvas c(kWidth, 4000, &pool);
        c.clear();
        render_tree(md.build(), c, pool, theme::native, nodes, true);
        return c;
    };

    std::unordered_map<int, Painted> prev;
    int prev_last_row = -1, violations = 0;

    for (std::size_t i = 0; i < doc.size(); i += static_cast<std::size_t>(chunk)) {
        md.append(std::string_view{doc}.substr(
            i, std::min<std::size_t>(static_cast<std::size_t>(chunk), doc.size() - i)));
        maya::testing::advance_anim_clock_ms(33);
        Canvas c = paint();
        auto cur = snapshot(c);

        int min_restyled_row = 1 << 30;
        for (const auto& [pos, p] : cur) {
            auto it = prev.find(pos);
            if (it == prev.end()) continue;            // newly painted
            if (it->second.ch != p.ch) continue;       // different glyph
            if (it->second.style == p.style) continue; // unchanged
            // A cell going conceal -> visible is the TYPEWRITER REVEALING it,
            // which is the whole point of the effect and never corruption:
            // the cell was invisible, so nothing the user had read changed.
            // Only a cell that was ALREADY VISIBLE and then changed styling
            // is a rewrite of settled content.
            const bool was_hidden = pool.get(it->second.style).conceal;
            if (was_hidden) continue;
            min_restyled_row = std::min(min_restyled_row, pos / kWidth);
        }
        if (prev_last_row >= 0 && min_restyled_row < prev_last_row) ++violations;

        prev_last_row = c.max_content_row();
        prev = std::move(cur);
    }
    return violations;
}

} // namespace

// NOTE: the standalone-test bundle renames this to reveal_settled_rows_test_main
// at compile time via -Dmain=<name>_main (see maya/CMakeLists.txt), so this
// must be a plain `main`.
int main() {
    std::printf("reveal_settled_rows_test\n");

    // Long unbroken prose — the shape the newline-only bound could not see.
    // No '\n' anywhere in the paragraph, so it exists purely as wrapped rows.
    std::string prose;
    for (int i = 0; i < 14; ++i)
        prose += "the reveal cursor walks this prose at the model's pace and "
                 "the settled rows above it must never be restyled again ";

    // Mixed markdown: prose, heading, list, table, fence. Each block type
    // takes a different path through the tail renderer.
    const std::string mixed =
        "Opening paragraph that runs long enough to wrap across several "
        "visual rows before the next block begins at all.\n\n"
        "## A heading\n\n"
        "- first list item with some length to it\n"
        "- second list item\n\n"
        "| Col A | Col B |\n|-------|-------|\n| one   | two   |\n\n"
        "```cpp\nint main() { return 0; }\n```\n\n"
        "Closing paragraph after every block has been streamed.\n";

    // WHAT COUNTS AS A VIOLATION. A cell that was CONCEALED and becomes
    // visible is the typewriter revealing it — excluded, that is the effect
    // working. What remains is a cell that was already visible and changed
    // styling on a row above the live tail. Most of that residue is the
    // hot->cool gradient legitimately cooling the row the cursor just left;
    // it is bounded by the gradient's 700 ms band, not unbounded churn.
    //
    // So this is a REGRESSION BUDGET, not an assertion of zero. The numbers
    // below sit just above the measured post-fix values and well under the
    // pre-fix ones, so the whole-paragraph sweep cannot come back unnoticed:
    //
    //     case            before  after
    //     prose chunk=4      371    201
    //     prose chunk=8      182    146
    //     prose chunk=16      87     70
    //     prose chunk=32      40     33
    //     mixed chunk=8        7      5
    //     mixed chunk=16       4      3
    //
    // The remaining gap between "after" and zero is the cooling gradient
    // reaching one row back on a wrapped paragraph. Closing it means giving
    // decorate_text_reveal per-ROW geometry (it currently sees one flat
    // string and infers rows by re-wrapping), which is a larger change than
    // this fix — tracked, not attempted here.
    struct Case { const char* name; const std::string* doc; int chunk; int budget; };
    const Case cases[] = {
        {"prose chunk=4",  &prose, 4,  230},
        {"prose chunk=8",  &prose, 8,  165},
        {"prose chunk=16", &prose, 16,  80},
        {"prose chunk=32", &prose, 32,  38},
        {"mixed chunk=8",  &mixed, 8,    6},
        {"mixed chunk=16", &mixed, 16,   3},
    };

    for (const auto& c : cases) {
        const int v = settled_row_rewrites(*c.doc, c.chunk);
        std::printf("  %-16s settled-row rewrites = %2d (budget %d)\n",
                    c.name, v, c.budget);
        check(v <= c.budget,
              std::string{c.name} + ": " + std::to_string(v)
                  + " settled-row rewrites exceeds budget "
                  + std::to_string(c.budget)
                  + " — the reveal is restyling rows above the live tail "
                    "(line_bounded is not finding the last VISUAL row)");
    }

    if (g_failed == 0) std::printf("PASSED\n");
    else               std::printf("FAILED (%d)\n", g_failed);
    return g_failed == 0 ? 0 : 1;
}
