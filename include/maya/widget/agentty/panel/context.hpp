#pragma once
// maya::panel::ItemCtx — what an item widget may know while rendering.
//
// Deliberately tiny. An item renders from its own value plus this context,
// and NOTHING else — no Config, no row list, no layout. That is what keeps
// "one item kind = one widget file" honest: a widget that could reach the
// whole panel would grow panel logic.

#include <cstddef>
#include <vector>

#include "../../../element/text.hpp"   // StyledRun
#include "../../../style/style.hpp"
#include "theme.hpp"

namespace maya::panel {

struct ItemCtx {
    const Theme& theme;

    // Choice only: is this row's dropdown currently open?
    bool open = false;

    // Text/Path: columns the edited value may occupy before it scrolls
    // horizontally under its caret (Panel::edit_budget()). 0 = unmeasured.
    int edit_budget = 0;

    // Columns a DRAWN control may occupy — the width budget for kinds whose
    // glyphs are a picture rather than a word: a bar, a meter, a strip.
    //
    // Every such kind currently hardcodes its own size (Slider's kCells =
    // 12) because a control had no way to learn how much room it was in.
    // The result is a picture that stays the same size on a 40-column split
    // pane and a 200-column terminal — responsive in neither direction,
    // which is the same defect agentty's stat sheet works around with a
    // hand-counted reserve constant.
    //
    // Derived from min_width like edit_budget, and for the same reason,
    // which the sibling's comment states precisely: a scroll viewport
    // measures its children against an unbounded width to discover the
    // content extent, so a control that asked the LAYOUT how wide it is
    // would answer "2^24" during measure, publish that as the panel's
    // horizontal extent, and dirty the scroll state on every resize. The
    // budget is a fact known BEFORE layout, so it cannot participate in
    // that loop.
    //
    // A hint, never a grant. The layout still clips whatever exceeds the
    // cell; the budget only tells a drawn control what size picture is
    // worth drawing. 0 = unmeasured, and every kind must keep working at 0
    // — which is what the per-kind floor below is for.
    int draw_budget = 0;

    // ── The table's shared geometry ─────────────────────────────────
    //
    // Columns the VALUE cell occupies, as a max over every item in the
    // panel. 0 = unmeasured.
    //
    // This is the one fact a row genuinely cannot know about itself, and
    // the reason a table is not just a list of independent rows: every bar
    // must start at the same column and every number must end at the same
    // column, or the eye cannot compare them down the page. That is a
    // property of the SET, so only the thing that can see the whole set can
    // supply it — the panel, which already walks every item to measure
    // heights for virtualisation.
    //
    // A control that lays itself out uses this as a flex `basis`, so rows
    // align because they share a number rather than because each one padded
    // a string to the same length. Alignment by arithmetic is what put a
    // value in a different column on every row when the arithmetic drifted.
    int value_basis = 0;

    // OUT: byte offset of the painted caret glyph in the returned string
    // (npos = no live caret). Written by the editable kinds; the panel
    // uses it to anchor the HARDWARE cursor on that cell — terminal-side
    // blink, IME composition at the right spot, screen readers. Null when
    // the host didn't ask.
    std::size_t* caret_out = nullptr;

    // OUT: per-span styling for the returned string, when one style for
    // the whole cell is not enough.
    //
    // render() answers with ONE (string, Style) because that is all a word
    // needs. A picture needs more: a bar's filled head and its unfilled
    // track are two hues, and they are the whole point — a bar drawn in a
    // single colour is a rectangle, not a scale.
    //
    // Offsets are BYTES into the returned string, matching the caret_out
    // convention above (and StyledRun itself). Left empty, the panel paints
    // the cell in the single returned Style exactly as before, so every
    // existing kind is unaffected.
    //
    // The panel still owns placement, truncation and the cursor-row tint;
    // this says only "these bytes are a different colour", which is the
    // least a drawn control can say and still be drawn.
    std::vector<StyledRun>* runs_out = nullptr;

    // The width a drawn control should use, given what it wants and what
    // it can survive.
    //
    // One helper rather than a clamp expression per kind: "pref, floor,
    // ceiling" is the whole vocabulary a drawn control needs, and putting
    // it here means adding a kind cannot get the degradation rules subtly
    // different from every existing one. An unmeasured budget yields the
    // preference, so a control built outside a panel still draws.
    [[nodiscard]] int drawn_cells(int pref, int floor, int ceiling) const noexcept {
        if (draw_budget <= 0) return pref;
        const int lo = floor < 1 ? 1 : floor;
        const int hi = ceiling < lo ? lo : ceiling;
        return draw_budget < lo ? lo : (draw_budget > hi ? hi : draw_budget);
    }
};

} // namespace maya::panel
