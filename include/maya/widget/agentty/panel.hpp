#pragma once
// maya::Panel — THE overlay widget. One container, one Row, one renderer.
//
// Picker and Form were two implementations of the same thing: a bordered,
// padded, titled box holding a scrollable list of rows, each row a leading
// cell and a trailing cell with a cursor bar in column 0. Every fix to one had
// to be repeated in the other, and the ones that were not became bugs — the
// form spilled past its border, painted a theme-coloured selection slab, and
// lost its help line off the bottom edge, all of which the picker had solved
// years earlier.
//
// An earlier attempt kept both row renderers and shared only the frame. That
// was worse: it needed a `scroll_resolved` flag so two auto-scroll
// implementations would not fight over the same offset — a flag whose whole
// job is to say "don't run your logic, mine already ran". A referee between
// two owners is not single ownership.
//
// So there is ONE Row. What used to distinguish a picker row from a form row
// is per-cell decoration, not structure:
//
//     edge · badge · leading [· highlight] · gap · trailing [· origin]
//
// A picker row fills `leading`/`trailing`; a form row additionally sets
// `control` and `origin`. Nothing about the frame, the scroll, the selection
// or the width arithmetic differs, because there is only one of each.
//
// ── Two invariants the layout depends on ────────────────────────────────
//
// 1. ONE SCREEN ROW PER ENTRY. maya applies scroll offsets in rows, so if an
//    entry could be two rows tall the cursor index and the scroll offset would
//    be in different units and auto-scroll would drift. Rows that own extra
//    lines (a help line, an error, an open dropdown) emit them as their own
//    entries and report the span.
//
// 2. FLEX, NEVER ARITHMETIC. Cells are laid out by the flex box at
//    `width(percent(100))`. Every previous version measured the row and padded
//    it — `w - text - reserve` — and each reserve constant was a second owner
//    of a width the layout already knew. They drifted, values slid under the
//    scrollbar, and "fix the reserve" became a sequence of magic numbers.

// ── Modularity ──────────────────────────────────────────────────────────
// The family lives in widget/panel/, one file per concern:
//
//   panel/theme.hpp    — the palette
//   panel/context.hpp  — ItemCtx: the ONLY facts an item widget may know
//   panel/caret.hpp    — caret splicing for line-edited items
//   panel/item/*.hpp   — ONE WIDGET PER ITEM KIND (value struct + renderer)
//   panel/control.hpp  — the closed variant over the item kinds
//   panel/item.hpp     — the one Item
//   panel/menu.hpp     — the inline Choice dropdown
//   panel/config.hpp   — everything a host supplies
//
// This header is the umbrella: the Panel class (frame, viewport, scroll,
// selection — the CONTAINER concerns) plus compatibility aliases so
// `maya::Panel::Row` and friends keep meaning what they always did.

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "../../dsl.hpp"
#include "../../element/builder.hpp"
#include "../../element/element.hpp"
#include "line.hpp"
#include "../../element/text.hpp"
#include "../../style/color.hpp"
#include "../../style/style.hpp"
#include "panel/config.hpp"
#include "scrollbar.hpp"

namespace maya {

namespace panel::detail {
// The terminal's width, or 0 when it cannot be known. Defined in panel.cpp
// beside the min-width clamp that also needs it, so ONE place decides how
// this is answered (ioctl, then COLUMNS when there is no tty).
[[nodiscard]] int terminal_cols() noexcept;
} // namespace panel::detail

class Panel {
public:
    // The family's types. `Item` is the name; `Row` is the pre-rename
    // compatibility spelling and new code should not use it.
    using Item   = panel::Item;
    using Row    = panel::Item;
    using Menu   = panel::Menu;
    using Config = panel::Config;

    explicit Panel(Config c) : cfg_(std::move(c)) {}
    operator Element() const { return build(); }

    // No pane this frame: an empty element, so a host never builds one.
    [[nodiscard]] static Element empty() { return detail::nothing(); }

    // Build `c`, never taller than `budget` rows at `cols`: the list gives
    // up rows first (down to 3), then the frame's blank rows, then the
    // footer, then the last rows of the list.
    [[nodiscard]] static Element fit(Config c, int budget, int cols) {
        Element out;
        for (int pass = 0; pass < 8; ++pass) {
            Config t = c;
            out = Panel{std::move(t)}.build();
            const int h = measure_element(out, cols).height.value;
            if (h <= budget) break;
            const int over = h - budget;
            if (c.viewport_h > 3)    c.viewport_h = std::max(3, c.viewport_h - over);
            else if (!c.tight)       c.tight = true;
            else if (!c.footer.empty()) c.footer.clear();
            else if (c.viewport_h > 1) c.viewport_h = std::max(1, c.viewport_h - over);
            else break;
        }
        return out;
    }

    [[nodiscard]] Element build() const;

    // U+258E LEFT ONE QUARTER BLOCK — the cursor bar in column 0.
    static constexpr const char* kEdgeBar = "\xe2\x96\x8e";

    // Columns an EDITED value may occupy before it scrolls horizontally under
    // its caret.
    //
    // From the SURFACE, not from min_width. min_width is a FLOOR and the
    // panel stretches past it to fill its container, so a budget derived
    // from it answers the same number on a 60-column pane and a 200-column
    // one: this used to pin the window at 34 columns however wide the
    // terminal was, so a long URL arrived cut off with empty pane beside
    // it (#75).
    //
    // surface_cols() and not the flex layout: a scroll viewport measures
    // its children against an unbounded width, so a cell that asked the
    // layout would get 2^24. The terminal width is known before layout, so
    // it cannot join that loop.
    //
    // The deduction is the rest of the row — marker lane, label and gaps.
    // The layout still clips whatever exceeds the cell; the budget only
    // decides where the window sits inside it.
    [[nodiscard]] int edit_budget() const noexcept {
        return std::max(34, surface_cols() - 26);
    }

    // The width this panel is being drawn at, for the budgets to divide up.
    //
    // ONE place answers it, because two budgets disagreeing about how wide
    // the panel is would put a drawn control and an edited one on different
    // ideas of the same row.
    //
    // The terminal and not available_width(): build() assembles the tree
    // BEFORE the render pass publishes a context, so there is no slot to
    // ask yet. min_width is the last resort for a panel measured with no
    // terminal at all.
    [[nodiscard]] int surface_cols() const noexcept {
        const int term = panel::detail::terminal_cols();
        return term > 0 ? term : cfg_.min_width;
    }

    // Columns a DRAWN control (a bar, a meter, a strip) may occupy.
    //
    // Same derivation as edit_budget: surface_cols(). NOT min_width — that
    // is a FLOOR and a panel stretches past it, so a budget derived from it
    // answers the same number on a 60-column pane and a 200-column one,
    // which is the frozen-picture bug this exists to fix.
    //
    // Still never measured inside the flex layout, which is the trap: a
    // scroll viewport measures its children against an unbounded width, so
    // a control that asked the LAYOUT would answer 2^24, publish that as
    // the panel's horizontal extent, and dirty the scroll state on every
    // resize. The terminal width is known before layout starts, so it
    // cannot join that loop.
    //
    // The deduction is larger than edit_budget's because a drawn control
    // shares its line with a label AND a value, where an edited one has the
    // rest of the row. The floor keeps a narrow panel drawing SOMETHING
    // rather than nothing — the per-kind clamp in ItemCtx::drawn_cells
    // decides what that means for each picture.
    [[nodiscard]] int draw_budget() const noexcept {
        const int screen = surface_cols();

        // NOT flowing: the established answer, byte for byte. Every panel
        // that does not opt into column flow must be unaffected by it, and
        // the cheapest way to guarantee that is to not touch this path.
        if (cfg_.col_max_width <= 0) {
            const int usable = screen - kDrawnRowChrome;
            return usable < 8 ? 8 : usable;
        }

        // Flowing. A split happens only where BOTH bounds agree: the body
        // exceeds the ceiling AND a second column would still clear the
        // floor. That is columns()' own rule, restated because the budget
        // must be known before layout while columns() decides during it —
        // the two must not disagree, so this mirrors it literally rather
        // than approximating with "body > ceiling".
        const int inner  = Config::content_width(screen);
        const int second = (inner - cfg_.col_gap) / 2;
        // Both bounds AND something to divide. columns() caps the count at
        // the number of cells it was given — it will not make a column it
        // cannot fill — and the panel's cells are its SECTIONS, one per
        // header. A single-section tab therefore never splits however wide
        // the terminal is, and a budget that assumed otherwise sized the
        // bar for a 64-column column while the row was painted at 145:
        // a 22-cell bar stranded against the right edge with a dead
        // expanse after the label, which is what the Tokens tab looked
        // like on a wide pane.
        //
        // Counting headers here rather than asking columns() because the
        // budget must be known BEFORE layout, which is the same reason the
        // rest of this test is a restatement rather than a call.
        int sections = 0;
        for (const auto& it : cfg_.items) if (it.is_header()) ++sections;
        if (sections == 0 && !cfg_.items.empty()) sections = 1;

        const bool split = inner > cfg_.col_max_width
                        && second >= cfg_.col_min_width
                        && sections >= 2;

        // SPLIT: the row is one column wide, so the ceiling IS the row and
        // the picture must be sized to it or it paints past its column.
        //
        // UNSPLIT: the row is the body, and the answer must be the one the
        // branch above gives — byte for byte, from `screen`. Deriving it
        // from `inner` instead looks more principled and is wrong by
        // exactly the chrome: 43 where the unflowed path gives 50. The
        // meter divides the row it is handed (bar, gap, value) against that
        // number, so seven missing columns came off the tail, and the tail
        // is the VALUE — a bar running the full width of the row with no
        // track and no number beside it.
        //
        // A flowed panel that has not split must be INDISTINGUISHABLE from
        // an unflowed one, because it is one. That is the invariant; the
        // unit-matching argument this comment used to make was a different,
        // plausible-sounding rule that quietly broke it.
        const int avail = split ? cfg_.col_max_width : screen;
        const int usable = avail - kDrawnRowChrome;
        return usable < 8 ? 8 : usable;
    }

    // What a drawn row spends on everything that is not the picture: the
    // marker lane, the label, both gaps and the value cell. Unchanged from
    // when this was a literal 40 — named, not retuned. Retuning it would
    // move every meter in every panel, which is a separate decision from
    // fixing which WIDTH it is deducted from.
    static constexpr int kDrawnRowChrome = 40;

    // Columns the VALUE cell occupies, as a max over every item.
    //
    // The one fact a row cannot know about itself, and the reason a table
    // is not a list of independent rows: numbers that do not end in the
    // same column cannot be compared down the page. Only the thing that
    // can see every row can answer it.
    //
    // A MAX, so a row whose own value is longer still gets its full width
    // — the kind takes max(basis, own) as its floor.
    [[nodiscard]] int value_basis() const noexcept;

private:
    Config cfg_;

    // The body is MEASURED before it is rendered, so only the rows inside the
    // viewport are ever turned into Elements.
    //
    // The panel backs the thread list, which is thousands of rows behind a
    // fourteen-row window. Building every row's Element and letting the scroll
    // viewport clip the other 4986 cost ~82ms a frame at 5k rows — twelve fps
    // on a list whose whole job is to be flicked through. Both halves of that
    // are per-ROW work (constructing the styled runs, then laying out N flex
    // children), and neither is needed for a row that cannot be seen.
    //
    // So: a measure pass that counts LINES without building anything, then a
    // render pass over the intersecting rows only. Content height is preserved
    // exactly by two zero-child spacers standing in for the skipped rows above
    // and below, which keeps the renderer's own max_y writeback — and so the
    // scrollbar thumb and the wheel hit-testing — honest.
    struct Body {
        // offsets[i] = absolute first line of row i; offsets.back() = total.
        // Always non-empty (a single 0 for an empty body), so offsets[ra] is
        // safe for any ra a window computation can produce.
        std::vector<int> offsets{0};
        int total       = 0;
        int cursor_line = 0;
        int cursor_span = 1;
        int selected    = -1;   // resolved: in range, and never a header

        // True when the lines came from Config::items — caller-supplied
        // Elements that the panel did not build. They are measured (so the
        // scroll arithmetic is honest) but never WINDOWED: the panel cannot
        // re-render half of somebody else's Element, and callers with huge
        // item lists already hand it only the visible slice.
        bool opaque     = false;

        // True when measurement stopped early at the height cap, so `total`
        // is a floor rather than the real content height. The panel must not
        // publish max_y from a floor — the renderer's writeback owns it there.
        bool capped     = false;
    };

    [[nodiscard]] Body measure_body() const;

    // The items grouped into sections and flowed into columns — ONE Element,
    // so it scrolls as a unit and every column moves together. Empty unless
    // cfg_.col_max_width says to flow and the panel is a document.
    //
    // Flow breaks the one-screen-row-per-entry invariant this header opens
    // with: across two columns item k is no longer on line k, so a cursor
    // index and a scroll offset stop being the same unit. A flowed body is
    // therefore rendered whole and OPAQUE — exactly what `prebuilt` already
    // does — and is refused for any panel that has a cursor or an open menu,
    // where per-item line positions still have to mean something.
    [[nodiscard]] std::optional<Element> flowed_body() const;

    // Half-open row range [first, last) covering absolute lines [y, y + vh).
    [[nodiscard]] static std::pair<int, int> visible_rows(const Body& b, int y,
                                                          int vh);

    // Elements for rows [first, last) — the ONLY place rows become Elements.
    [[nodiscard]] std::vector<Element> render_range(const Body& b, int first,
                                                    int last) const;

    // Painted line count of one row / one open menu, WITHOUT building it.
    // These must agree with render_item / render_menu exactly: measure decides
    // where the scroll sits and render decides what is under it, so a drift of
    // one line is a row that scrolls off its own help text. panel_test walks a
    // matrix of row shapes asserting render_item(r).size() == item_lines(r).
    [[nodiscard]] int item_lines(const Item& r, int index, bool on_row) const;
    [[nodiscard]] static int menu_lines(const Menu& m);

    // Rows a wrapped row-error occupies at the current width. Its own
    // helper because both the measure pass and the paint pass must get the
    // same answer from the same place.
    [[nodiscard]] int error_lines(const std::string& err) const;

    // A zero-child box that occupies `n` rows: the skipped rows' geometry
    // without their cost.
    [[nodiscard]] static Element spacer_rows(int n);

    [[nodiscard]] std::vector<Element> render_item(const Item& r, int index) const;
    [[nodiscard]] std::vector<Element> render_menu(const Menu& m) const;
    [[nodiscard]] std::pair<std::string, Style> render_control(const Item& r,
                                                               int index) const;
    // Same, reporting where the painted caret glyph landed (byte offset
    // into the returned string; npos = none) so the item renderer can
    // anchor the hardware cursor on that cell.
    [[nodiscard]] std::pair<std::string, Style> render_control(
        const Item& r, int index, std::size_t* caret_at) const;
    // Same again, also collecting the control's OWN styled runs when it has
    // more than one hue to express — a meter's filled head against its
    // unfilled track. Empty when the kind styles itself uniformly, which is
    // every text kind.
    [[nodiscard]] std::pair<std::string, Style> render_control(
        const Item& r, int index, std::size_t* caret_at,
        std::vector<StyledRun>* runs_at) const;

    // The facts an item may know. Built by BOTH the measure pass and the
    // render pass so the two cannot disagree about how tall a control is.
    [[nodiscard]] panel::ItemCtx item_ctx(int index) const;

    // The shared row idiom: a leading cell that grows, a gap, a trailing cell.
    // FLEX — the layout owns the width, so there is nothing to get wrong.
    [[nodiscard]] static Element row_line(Element lead, Element trail,
                                          bool trailing_secondary,
                                          Style gap_style,
                                          bool value_primary = false);
    [[nodiscard]] static Element right_line(Element content);

    // A drawn control with a note beside it. See the definition for why
    // the note goes in FRONT of the control and why it takes two bounds.
    [[nodiscard]] Element annotated(Element control, const std::string& note,
                                    Style note_style) const;

    // What a row's note may cost, as a share of the picture's budget.
    //
    // A third is about the slack a label→value row has between its two
    // ends, so a note that stays under it never makes the label compete.
    static constexpr int kNoteShare = 3;

    // Floor for the above, so a narrow pane shows a word or two of the
    // note rather than an ellipsis standing on its own.
    static constexpr int kMinNoteCols = 8;

    // How readily the note gives ground once the row is over-subscribed.
    // Weighted far above the other cells because it is the only one the
    // row can lose and still be read.
    static constexpr float kNoteShrink = 12.0f;
};
} // namespace maya
