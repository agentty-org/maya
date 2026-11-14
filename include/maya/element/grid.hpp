#pragma once
// maya::grid — responsive layout with ONE number.
//
//   grid({cpu, mem, net, disk}, 26);
//
// "Each cell wants about 26 columns." That is the entire API. maya fits as
// many cells per row as the REAL slot width allows, wraps the rest into new
// rows, and stacks everything in one column on a narrow terminal — re-solved
// live on every resize. No breakpoints to memorize, no span arithmetic, no
// per-tier declarations that drift apart: the cell count per row falls out
// of the width you already know (how wide does one cell need to be to look
// right?).
//
//   sidebar(stats, table, 42);
//
// The other layout every dashboard needs: a fixed-width rail next to a main
// pane that takes the rest — and the pair stacks vertically the moment the
// terminal is too narrow for both. Again one number: the rail's width.
//
//   col({ row({cpu, mem, net, disk}), table });
//
// And the GTK mental model for whole pages: row() puts cells side by side
// sharing the width equally — and wraps, then stacks, by itself when the
// slot narrows. col() stacks cells, each stretched to the full width.
// Everything fills automatically; nothing needs a hand-computed width.
//
//   columns({sec1, sec2, sec3, …}, 34);
//
// The OTHER flow: newspaper columns. grid() wraps left-to-right (cell 2 sits
// beside cell 1); columns() runs top-to-bottom (cell 2 sits BELOW cell 1,
// and the sequence moves to column 2 only when column 1 has had its share of
// the height). Reading order is down-then-across, which is what a document
// wants and what a dashboard does not. One number again: how wide a column
// needs to be before splitting is worth it.
//
// Compose them and you have a full three-shape dashboard in two lines:
//
//   sidebar(grid({cpu, mem, net, disk}, 24), table, {.width = 42});
//
//   * ultrawide  — 42-cell rail (stats stacked 1-across inside it, because
//                  the grid re-solves from its SLOT width, not the screen),
//                  table fills the rest
//   * medium    — stats flow 2-, 3-, 4-across over a full-width table
//   * narrow    — everything in one column
//
// Semantics worth knowing:
//   * Cells in a row share the width EXACTLY (largest-remainder split — no
//     ragged right edge from integer division).
//   * A short last row keeps the same cell width as the full rows above it,
//     so columns line up down the whole grid.
//   * grid re-solves from the width of the slot it SITS IN (adapt() under
//     the hood) — a grid inside a sidebar collapses independently of one in
//     the main pane.
//   * Anything fancier (a cell that spans two columns, tier-specific
//     hiding) is what adapt()/responsive() are for. The grid stays simple.

#include "builder.hpp"
#include "../core/render_context.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace maya {

// ============================================================================
// grid()
// ============================================================================

struct GridOpts {
    int  min       = 24;     ///< a cell's comfortable minimum width (columns)
    int  max_cols  = 0;      ///< cap cells-per-row; 0 = as many as fit
    int  gap_x     = 1;      ///< blank columns between cells
    int  gap_y     = 0;      ///< blank rows between rows
    bool grow_rows = false;  ///< rows share surplus height (definite slot)
};

/// Auto-flow grid: as many `min`-wide cells per row as fit, wrap the rest,
/// one column when narrow. `grid(cells, 26)` is the whole call.
[[nodiscard]] inline auto grid(std::vector<Element> cells, GridOpts opts = {})
    -> ComponentBuilder
{
    return detail::adapt([cells = std::move(cells), opts](int w) -> Element {
        const int n = static_cast<int>(cells.size());
        if (n == 0) return Element{ElementList{}};

        const int min   = std::max(1, opts.min);
        const int gap_x = std::max(0, opts.gap_x);

        // How many min-wide cells (plus gaps between them) fit in w?
        int cols = (w + gap_x) / (min + gap_x);
        cols = std::clamp(cols, 1, n);
        if (opts.max_cols > 0) cols = std::min(cols, opts.max_cols);

        // Split the row width exactly: base + largest-remainder spread, so
        // the last cell ends flush with the slot edge.
        const int total = std::max(cols, w - gap_x * (cols - 1));
        const int base  = total / cols;
        const int rem   = total % cols;
        std::vector<int> cw(static_cast<std::size_t>(cols));
        for (int i = 0; i < cols; ++i)
            cw[static_cast<std::size_t>(i)] = base + (i < rem ? 1 : 0);

        // Wrap into rows. A short last row keeps the same cell widths so
        // columns line up down the grid.
        std::vector<Element> row_els;
        row_els.reserve(static_cast<std::size_t>((n + cols - 1) / cols));
        for (int start = 0; start < n; start += cols) {
            const int count = std::min(cols, n - start);
            std::vector<Element> cell_els;
            cell_els.reserve(static_cast<std::size_t>(count));
            for (int i = 0; i < count; ++i) {
                // A Column-direction box with a fixed width: the default
                // cross-axis Stretch hands the content the FULL cell width,
                // and the row's own Stretch hands it the row height.
                auto cb = detail::vstack();
                cb.width(Dimension::fixed(cw[static_cast<std::size_t>(i)]));
                cell_els.push_back(
                    cb(Element{cells[static_cast<std::size_t>(start + i)]}));
            }
            auto rb = detail::hstack();
            if (gap_x > 0) rb.gap(gap_x);
            if (opts.grow_rows) rb.grow(1.0f);
            row_els.push_back(rb(std::move(cell_els)));
        }

        if (row_els.size() == 1) return std::move(row_els.front());
        auto vb = detail::vstack();
        if (opts.gap_y > 0) vb.gap(opts.gap_y);
        return vb(std::move(row_els));
    });
}

/// Sugar: `grid(cells, 26)` — "each cell wants about 26 columns".
[[nodiscard]] inline auto grid(std::vector<Element> cells, int min_width)
    -> ComponentBuilder
{
    return grid(std::move(cells), GridOpts{.min = min_width});
}

// ============================================================================
// sidebar()
// ============================================================================

struct SidebarOpts {
    int  width       = 32;    ///< the rail's fixed width (columns)
    int  stack_below = 0;     ///< stack when slot < this; 0 = auto (2×width:
                              ///< side-by-side only while main ≥ the rail)
    int  gap         = 1;     ///< blank columns between rail and main
    bool right       = false; ///< rail on the right instead of the left
};

/// Fixed-width rail beside a main pane that takes the rest; the pair stacks
/// vertically (reading order preserved) when the slot is too narrow.
[[nodiscard]] inline auto sidebar(Element rail, Element main,
                                  SidebarOpts opts = {}) -> ComponentBuilder
{
    return detail::adapt(
        [rail = std::move(rail), main = std::move(main), opts](int w) -> Element {
            const int rail_w = std::max(1, opts.width);
            const int threshold =
                opts.stack_below > 0 ? opts.stack_below : rail_w * 2;

            if (w >= threshold) {
                auto rb = detail::vstack();
                rb.width(Dimension::fixed(rail_w));
                auto mb = detail::vstack();
                mb.grow(1.0f);
                auto rowb = detail::hstack();
                if (opts.gap > 0) rowb.gap(opts.gap);
                std::vector<Element> kids;
                if (opts.right) {
                    kids.push_back(mb(Element{main}));
                    kids.push_back(rb(Element{rail}));
                } else {
                    kids.push_back(rb(Element{rail}));
                    kids.push_back(mb(Element{main}));
                }
                return rowb(std::move(kids));
            }

            // Too narrow: stack, preserving reading order (left → top).
            auto mainb = detail::vstack();
            mainb.grow(1.0f);
            auto colb = detail::vstack();
            std::vector<Element> kids;
            if (opts.right) {
                kids.push_back(mainb(Element{main}));
                kids.push_back(Element{rail});
            } else {
                kids.push_back(Element{rail});
                kids.push_back(mainb(Element{main}));
            }
            return colb(std::move(kids));
        });
}

/// Sugar: `sidebar(rail, main, 42)` — "42-column rail, main takes the rest".
[[nodiscard]] inline auto sidebar(Element rail, Element main, int width)
    -> ComponentBuilder
{
    return sidebar(std::move(rail), std::move(main), SidebarOpts{.width = width});
}

// ============================================================================
// row() / col() — boxes that keep themselves correct
// ============================================================================

/// Cells side by side, sharing the width equally and exactly — wrapping,
/// then stacking, by itself as the slot narrows. Same engine as grid();
/// the name reads better when composing pages: col({ row({a, b}), table }).
[[nodiscard]] inline auto row(std::vector<Element> cells, int min_width = 24)
    -> ComponentBuilder
{
    return grid(std::move(cells), GridOpts{.min = min_width});
}

/// Cells stacked top to bottom, each stretched to the full width (flex
/// cross-stretch — the GTK "fill"). Pipe `| grow(1)` onto the child that
/// should take the leftover height.
[[nodiscard]] inline Element col(std::vector<Element> cells, int gap = 0)
{
    auto vb = detail::vstack();
    if (gap > 0) vb.gap(gap);
    return vb(std::move(cells));
}

// ============================================================================
// columns() — newspaper flow
// ============================================================================

struct ColumnsOpts {
    // The WIDEST a column may get before splitting again. Not a minimum:
    // the question a document asks is "how long may a line be before it is
    // tiring to read", and the answer forces the count — a 200-column slot
    // with max 60 gives three columns because two would be 99 wide.
    //
    // Stating the CEILING rather than the floor is what makes the result
    // exact. A minimum leaves a remainder nobody owns (fit as many as pay
    // for themselves, then live with the slack); a maximum picks the count
    // first and then divides the WHOLE slot among them, so every cell of
    // the width is claimed by construction.
    int max_width = 60;
    // Ceiling on the count, for slots wide enough to split further than the
    // content deserves. 0 = as many as the width implies.
    int max_cols = 0;
    // The narrowest a column may be. A CEILING alone cannot express "do not
    // split below what a row needs": at max_width 64 a 76-column slot splits
    // into two columns of 36, and a label─→value row that wanted 40 starts
    // truncating its labels. That trades one unreadable layout for another.
    //
    // So the two bounds do different jobs and both are needed. max_width is
    // about READING (how long a line may get); min_width is about FITTING
    // (how short a column may get before its content stops working). A split
    // happens only where both are satisfied; where they conflict, min wins,
    // because a too-wide line is awkward while a truncated one has lost
    // information.
    int min_width = 0;
    // Blank columns between neighbours. Two: one reads as a wrapped line
    // rather than a gutter once the cells have ragged right edges.
    int gap = 2;
    // Balance the column HEIGHTS rather than packing strictly in sequence.
    bool balance = true;
    // The widest slot the caller can actually be given. 0 = trust the
    // offered width completely.
    //
    // A BOUND, not an override, and the distinction is the whole point.
    // adapt() reports the slot a component is MEASURED in, which is not
    // always the slot it is PAINTED in: a sibling that flex has not
    // subtracted yet (a scrollbar gutter) is invisible at measure time, so
    // the offer can be a column or two generous. Laying out for it puts the
    // tail of every row — which is where a value sits — under whatever that
    // sibling draws.
    //
    // Taking the MINIMUM of the two is what makes it safe in both
    // directions: it can hand back an over-generous offer, but it can never
    // pin the body narrower than the width it really has. Replacing the
    // offer outright was tried and left a constant strip of dead space down
    // the right edge at every terminal size.
    //
    // It also covers the measure-outside-a-render-pass case for free: the
    // renderer's no-context fallback is just another over-generous offer.
    int width = 0;
};

/// Cells flowed top-to-bottom into columns, each no wider than `max_width`.
///
/// The sibling of grid(): grid wraps ACROSS (cell 2 beside cell 1), columns
/// runs DOWN (cell 2 below cell 1, moving right only when the column has had
/// its share of the height). Reading order is down-then-across — a document,
/// not a dashboard.
///
/// Two properties, both load-bearing:
///
///   * NO SLACK. The count is the smallest k whose columns fit under
///     max_width, and the slot is then divided among exactly those k with a
///     largest-remainder spread. The columns plus gaps sum to the slot
///     WIDTH, not to something near it — there is no strip left over,
///     because no step ever rounds a width down and keeps the change.
///
///   * RESPONSIVE INSIDE. Each column is a fixed-width box whose cross-axis
///     Stretch hands its content that full width, so a cell laid out with
///     adapt()/grid()/a meter re-solves against the COLUMN — not against
///     the screen. Nesting columns() inside a column therefore works, and a
///     figure sized to the screen inside a third of it is impossible.
///
/// Balanced by MEASURED HEIGHT, never by cell count: cells differ wildly in
/// length (a two-row table against a nine-bucket chart), and splitting on
/// count leaves one column twice the height of its neighbour, which reads as
/// a layout bug rather than as a choice.
///
/// Like grid(), this measures the REAL slot width via adapt() — so the count
/// is re-decided on every resize and no caller has to know what a frame, a
/// padding or a scrollbar gutter cost. A caller that computes its own column
/// width against a guessed "usable" figure is the bug this replaces: the
/// cell measures one width, flex resolves another, and the difference comes
/// off the end of the string.
[[nodiscard]] inline auto columns(std::vector<Element> cells, ColumnsOpts opts)
    -> ComponentBuilder
{
    return detail::adapt([cells = std::move(cells), opts](int offered) -> Element {
        // The offer bounds us and so does the caller's width; take the
        // smaller. Never lay out wider than the slot we will be painted in,
        // never narrower than the width we actually have.
        const int w = opts.width > 0 ? std::min(offered, opts.width) : offered;
        const int n = static_cast<int>(cells.size());
        if (n == 0) return Element{ElementList{}};

        const int gap  = std::max(0, opts.gap);
        const int most = std::max(1, opts.max_width);

        // The smallest k whose columns come in at or under max_width.
        //
        // k columns leave w - gap*(k-1) to divide, so the widest column is
        // ceil((w - gap*(k-1)) / k). Solving that for "<= most" directly is
        // fiddly and easy to get wrong by one; counting up from 1 is exact,
        // obviously right, and bounded by n.
        int cols = 1;
        while (cols < n) {
            const int span = w - gap * (cols - 1);
            if (span <= 0) break;
            if ((span + cols - 1) / cols <= most) break;   // ceil <= most
            // Would the NEXT column take us below the floor? Then stop here
            // — splitting further would satisfy the ceiling by breaking the
            // content, which is not an improvement.
            if (opts.min_width > 0) {
                const int next = w - gap * cols;
                if (next / (cols + 1) < opts.min_width) break;
            }
            ++cols;
        }
        cols = std::clamp(cols, 1, n);
        if (opts.max_cols > 0) cols = std::min(cols, opts.max_cols);

        // One column: hand back the plain stack, at the FULL slot width.
        //
        // Not an optimisation — it is the guarantee that enabling flow
        // cannot disturb a narrow layout, because at k == 1 there is no
        // wrapper here to disturb it.
        //
        // And no cap, ever. max_width's job is to decide HOW MANY columns
        // there are; it is not a margin. Once the answer is one there is no
        // second column for withheld width to go to, so capping leaves a
        // strip of the slot belonging to nobody — which is the exact thing
        // this primitive exists to prevent, just arriving from the other
        // side. An earlier version capped whenever the body could not split
        // (a single section, or max_cols == 1), and it cost a one-section
        // stats tab 19 columns at width 90: the row stopped at 65 with the
        // value stranded mid-row, AND the label truncated, because the lane
        // it was sharing had been shrunk to a width the slot never asked
        // for. A bound that costs width without buying a split is not a
        // ceiling.
        if (cols == 1) {
            auto vb = detail::vstack();
            // Claim the slot EXPLICITLY rather than trusting the stack to
            // stretch into it. Inside adapt() the component reports the size
            // it measures, and a stack of rows measures to its NATURAL width
            // — the longest row — not to the slot it was offered. The body
            // then reports 67 on an 83-column slot and the 16 columns nobody
            // claimed show up as dead space down the right edge, which is
            // the same "strip belonging to nobody" this primitive exists to
            // prevent, one level up from where it was fixed before.
            //
            // max_width bounds it as well as the offer does. A row whose
            // cells shrink under pressure (a label that truncates so a value
            // survives) only does so when the width it is given is the width
            // it actually has — handed the parent's larger slot instead, the
            // row sees no pressure, never truncates, and its tail runs off
            // the end. That is a VALUE silently disappearing, which is worse
            // than any truncated label.
            //
            // But it must still YIELD. A fixed width with the default
            // shrink of 1 is a preference; the same width with shrink(0) is
            // a demand, and a demand overflows the moment a sibling takes a
            // column — a scrollbar gutter does exactly that, one column at a
            // time, and the overflow comes off the row's tail. Leaving
            // shrink alone is what lets the column be 33 when it can and 32
            // when the gutter is there, rather than 33 or broken.
            vb.width(Dimension::fixed(w));
            vb.max_width(Dimension::fixed(w));
            return vb(std::move(cells));
        }

        // Divide the slot EXACTLY: base + largest-remainder spread. The
        // first `rem` columns take one extra cell, so the widths sum to the
        // full span and the last column ends flush with the slot edge.
        const int span = std::max(cols, w - gap * (cols - 1));
        const int base = span / cols;
        const int rem  = span % cols;
        std::vector<int> cw(static_cast<std::size_t>(cols));
        for (int i = 0; i < cols; ++i)
            cw[static_cast<std::size_t>(i)] = base + (i < rem ? 1 : 0);

        // Measure every cell at the NARROWEST column. A cell's height must
        // not depend on which column it lands in, or the balance below would
        // be computed from heights the layout then contradicts.
        std::vector<int> h(static_cast<std::size_t>(n), 1);
        int total = 0;
        for (int i = 0; i < n; ++i) {
            const int m = measure_element(cells[static_cast<std::size_t>(i)],
                                          std::max(1, base)).height.value;
            h[static_cast<std::size_t>(i)] = std::max(1, m);
            total += h[static_cast<std::size_t>(i)];
        }

        // Fill column by column, re-deriving the target from what is LEFT.
        //
        // A fixed target of total/cols does not survive contact with atomic
        // cells: once one column overshoots (it must, unless the heights
        // divide evenly) every later column inherits the error, and the last
        // one collects it. Recomputing rem_total/rem_cols after each column
        // spreads that error instead of accumulating it.
        //
        // The `(n - i) > (rem_cols - 1)` guard reserves one cell for each
        // column still to come, so a tall early cell can never starve the
        // right-hand columns into being empty.
        std::vector<std::vector<Element>> col_cells(static_cast<std::size_t>(cols));
        int i = 0, rem_total = total;
        for (int c = 0; c < cols; ++c) {
            const int rem_cols = cols - c;
            const int target   = (rem_total + rem_cols - 1) / rem_cols;
            // The LAST column takes everything that is left, unconditionally.
            // Deriving its contents from the same target arithmetic would
            // make "every cell is placed" a property of the rounding rather
            // than of the loop, and a cell silently dropped by a layout is
            // the worst failure this file could have.
            const bool last = (c == cols - 1);
            int used = 0;
            while (i < n && (last || (n - i) > (rem_cols - 1))) {
                if (!last && opts.balance && used > 0 && used >= target) break;
                col_cells[static_cast<std::size_t>(c)]
                    .push_back(std::move(cells[static_cast<std::size_t>(i)]));
                used      += h[static_cast<std::size_t>(i)];
                rem_total -= h[static_cast<std::size_t>(i)];
                ++i;
            }
        }

        // A Column-direction box with a fixed width: the default cross-axis
        // Stretch hands the content the FULL column width, which is what
        // makes a cell inside re-solve against its column.
        std::vector<Element> col_els;
        col_els.reserve(static_cast<std::size_t>(cols));
        for (int ci = 0; ci < cols; ++ci) {
            auto cb = detail::vstack();
            cb.width(Dimension::fixed(cw[static_cast<std::size_t>(ci)]));
            col_els.push_back(cb(std::move(col_cells[static_cast<std::size_t>(ci)])));
        }

        auto rb = detail::hstack();
        if (gap > 0) rb.gap(gap);
        return rb(std::move(col_els));
    });
}

/// Sugar: `columns(cells, 60)` — "no column wider than 60".
[[nodiscard]] inline auto columns(std::vector<Element> cells, int max_width)
    -> ComponentBuilder
{
    return columns(std::move(cells), ColumnsOpts{.max_width = max_width});
}

// ============================================================================
// viewport() — count-split responsive columns
// ============================================================================
//
//   viewport({a, b, c, d, e, f}, 30);
//
// The count comes from a column CEILING, exactly like columns(): a slot at
// or under max_width is one column, over max_width is two, over 2×max_width
// is three, and so on. But where columns() flows DOWN a column until it has
// had its share of the HEIGHT (newspaper reading order, balanced by measured
// height), viewport() fills HORIZONTALLY first: cells go left-to-right across
// the columns, then wrap to the next row — a real grid. Six cells over two
// columns are rows [0,1] [2,3] [4,5]; four over three are [0,1,2] [3]. Each
// row is a horizontal band whose height is its tallest cell, so a cell that
// wraps to a new row starts at a clean, aligned baseline rather than stacking
// under the first column with a ragged gap. Every column that exists holds
// ceil(n/cols) or floor(n/cols) cells — differing by at most one.
//
// When there are FEWER cells than the width affords, the columns are held at
// max_width (not stretched to fill), so a lone card stays its natural size
// and two cards sit as two evenly-spaced cards rather than two half-screen
// ones — the surplus width is left empty on the right.
//
// Everything inside stays responsive to the COLUMN width, not the screen:
// each column is a fixed-width box whose cross-axis Stretch hands its content
// the full column width, so an adapt()/grid()/table/meter re-solves against
// the column it landed in. Shrink the terminal and the columns collapse
// 3 → 2 → 1, and at every count the children reflow to the new column width.
//
// Use viewport() when the cells are peers of similar size and you want a
// predictable even split (a card grid, a settings sheet); use columns() when
// the cells vary wildly in height and you want the columns to end level.

// Fill order for viewport(): how cells map onto the chosen columns.
enum class Flow {
    Row,     // left-to-right, then wrap to the next row (a grid, aligned rows)
    Column,  // top-to-bottom down each column, then across (independent columns)
};

struct ViewportOpts {
    // The WIDEST a column may get before another column is added. The count
    // is chosen first (smallest k whose columns fit under this), then the
    // whole slot is divided among exactly that many — no leftover strip.
    int max_width = 40;
    // Ceiling on the column count. 0 = as many as the width implies.
    int max_cols = 0;
    // The narrowest a column may be. A split that would push a column below
    // this is not taken — a too-wide line is awkward, a truncated one has
    // lost information, so fitting wins over reading. 0 = no floor.
    int min_width = 0;
    // Blank columns between neighbours.
    int gap = 2;
    // Vertical gap between stacked cells inside a column.
    int gap_y = 0;
    // A BOUND on the offered width (not an override): min(offer, width) is
    // laid out, so a not-yet-subtracted sibling (a scrollbar gutter) can
    // never pin the body wider than the space it will actually be painted
    // in. 0 = trust the offered width.
    int width = 0;
    // Fill order once the column count is chosen. Row (default) fills
    // left-to-right then wraps to the next row — a real grid with aligned
    // rows. Column fills top-to-bottom down column 0, then column 1, … —
    // independent columns, order runs down then across.
    Flow flow = Flow::Row;
    // Make every cell in a Row-flow row the SAME height as its tallest
    // sibling, so the grid reads as a clean matrix instead of scattered
    // boxes of different heights. The cell is stretched on the cross axis
    // and told to grow, so a bordered card fills the row band. No effect in
    // Flow::Column (columns are independent by design). Default off so the
    // primitive stays purely additive.
    bool equal_rows = false;
    // Size columns to their CONTENT instead of splitting evenly (Row flow).
    // Each cell is measured at two wide offers: one that keeps its width is
    // rigid (text, a donut) and gets its natural width; one that grows with
    // the offer is elastic (a histogram, a line chart) and shares the space
    // left over. If the row is all rigid, the leftover is split evenly so it
    // reads as even spacing. A row that can't fit its cells takes fewer.
    // max_width is not used; min_width is an elastic cell's floor and the
    // least a rigid cell is squeezed to.
    bool fit = false;
    // With fit: when a row has nothing elastic, hand the spare width to
    // the cells so the row spans the slot. Off packs cells at their natural
    // width, which a viewport nested inside another must do, or it inks
    // the whole offer and the outer one sees it as elastic.
    bool spread = true;
    // With fit: the host's content key. The plan (how many per row, each
    // width) costs a few trial paints per cell, so it is kept per
    // (fit_key, width) and reused until either changes. Change the key when
    // the cells' content changes in a way that could move a width. 0 means
    // no caching: the plan is worked out on every call.
    std::uint64_t fit_key = 0;
};

/// Cells split EVENLY BY COUNT into 1/2/3/… columns chosen from a width
/// ceiling, round-robin so reading order is preserved, every column stays
/// responsive to its own width.
namespace detail {

// ViewportOpts::fit. Widths come from measuring the real cells.
struct FitPlan { int k = 1; std::vector<int> want; std::vector<char> elastic; };

[[nodiscard]] inline FitPlan plan_fit(const std::vector<Element>& cells,
                                     const ViewportOpts& opts, int w) {
    const int n     = static_cast<int>(cells.size());
    const int gap   = std::max(0, opts.gap);
    const int floor = std::max(1, opts.min_width);
    // A cell that keeps inking edge to edge on a canvas far wider than the
    // slot stretches (a chart). Anything else, text even when it wraps at
    // the slot, stops at its own length and has a natural width.
    std::vector<int>  want(static_cast<std::size_t>(n));
    std::vector<char> elastic(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const auto& cell = cells[static_cast<std::size_t>(i)];
        const int big = std::max(400, 2 * w);
        const bool fills = painted_width(cell, big) >= big - 1;
        elastic[static_cast<std::size_t>(i)] = fills;
        int a = fills ? floor : painted_width(cell, w);
        if (!fills) {
            // The cell's width is the smallest at which it keeps the layout
            // it has at the full slot: as short, and inking as much. Ink
            // alone is too tight: a card's right padding isn't ink (text
            // wraps), and a donut needs room past its legend to keep the
            // legend beside the ring. Binary search: wider never hurts.
            const int h_full = measure_element(cell, w).height.value;
            auto keeps = [&](int x) {
                return measure_element(cell, x).height.value <= h_full
                    && painted_width(cell, x) >= a;
            };
            // The slack a cell needs past its ink is small (padding, a
            // legend's margin), so search a short window, not the slot.
            int lo = a, hi = std::min(w, a + 12);
            if (!keeps(hi)) hi = lo = w;
            while (lo < hi) {
                const int mid = lo + (hi - lo) / 2;
                if (keeps(mid)) hi = mid; else lo = mid + 1;
            }
            a = lo;
        }
        want[static_cast<std::size_t>(i)] = fills ? floor : std::max(1, a);
    }

    // The most cells per row such that every row fits what it wants.
    int k = opts.max_cols > 0 ? std::min(n, opts.max_cols) : n;
    for (; k > 1; --k) {
        bool ok = true;
        for (int s = 0; s < n && ok; s += k) {
            int sum = gap * (std::min(n, s + k) - s - 1);
            for (int i = s; i < std::min(n, s + k); ++i) sum += want[static_cast<std::size_t>(i)];
            ok = sum <= w;
        }
        if (ok) break;
    }
    return FitPlan{k, std::move(want), std::move(elastic)};
}

[[nodiscard]] inline Element fit_rows(const std::vector<Element>& cells,
                                      const ViewportOpts& opts, int w) {
    const int n     = static_cast<int>(cells.size());
    const int gap   = std::max(0, opts.gap);
    const int floor = std::max(1, opts.min_width);

    // A few recent plans. Measure and paint both run this, often more than
    // once a frame, and the plan only moves when the content or width does.
    struct Entry { std::uint64_t key; int w, n; FitPlan plan; };
    thread_local std::vector<Entry> cache;
    FitPlan local;
    const FitPlan* hit = nullptr;
    if (opts.fit_key == 0) {
        local = plan_fit(cells, opts, w);
        hit = &local;
    } else {
        for (auto& e : cache)
            if (e.key == opts.fit_key && e.w == w && e.n == n) { hit = &e.plan; break; }
        if (!hit) {
            if (cache.size() >= 16) cache.erase(cache.begin());
            cache.push_back(Entry{opts.fit_key, w, n, plan_fit(cells, opts, w)});
            hit = &cache.back().plan;
        }
    }
    const int k = hit->k;
    const auto& want = hit->want;
    const auto& elastic = hit->elastic;

    std::vector<Element> rows;
    for (int s = 0; s < n; s += k) {
        const int e = std::min(n, s + k), m = e - s;
        const int span = std::max(m, w - gap * (m - 1));
        std::vector<int> cw(static_cast<std::size_t>(m));
        int used = 0, n_el = 0;
        for (int i = 0; i < m; ++i) {
            cw[static_cast<std::size_t>(i)] = want[static_cast<std::size_t>(s + i)];
            used += cw[static_cast<std::size_t>(i)];
            n_el += elastic[static_cast<std::size_t>(s + i)] ? 1 : 0;
        }
        if (used > span) {
            // Too wide (a lone cell, or k hit 1): squeeze the rigid cells
            // toward the floor in proportion to what they have over it.
            int over = used - span, slack = 0;
            for (int i = 0; i < m; ++i) slack += std::max(0, cw[static_cast<std::size_t>(i)] - floor);
            for (int i = 0; i < m && slack > 0; ++i) {
                auto& c = cw[static_cast<std::size_t>(i)];
                const int give = std::max(0, c - floor);
                const int take = std::min(give, (over * give + slack - 1) / slack);
                c -= take;
            }
            used = 0;
            for (int v : cw) used += v;
            for (int i = m - 1; i >= 0 && used > span; --i) {   // rounding
                auto& c = cw[static_cast<std::size_t>(i)];
                const int t = std::min(used - span, c - 1);
                c -= t; used -= t;
            }
        } else if (used < span && (n_el > 0 || opts.spread)) {
            // Leftover goes to the elastic cells, else evenly to everyone.
            const int left = span - used;
            const int share_n = n_el > 0 ? n_el : m;
            int given = 0, seen = 0;
            for (int i = 0; i < m; ++i) {
                if (n_el > 0 && !elastic[static_cast<std::size_t>(s + i)]) continue;
                ++seen;
                const int add = left * seen / share_n - given;
                cw[static_cast<std::size_t>(i)] += add;
                given += add;
            }
        }
        std::vector<Element> in_row;
        for (int i = 0; i < m; ++i) {
            auto cb = vstack();
            cb.width(Dimension::fixed(cw[static_cast<std::size_t>(i)]));
            cb.max_width(Dimension::fixed(cw[static_cast<std::size_t>(i)]));
            in_row.push_back(cb(cells[static_cast<std::size_t>(s + i)]));
        }
        auto rb = hstack();
        if (gap > 0) rb.gap(gap);
        rows.push_back(rb(std::move(in_row)));
    }
    if (rows.size() == 1) return std::move(rows.front());
    auto colb = vstack();
    if (opts.gap_y > 0) colb.gap(opts.gap_y);
    return colb(std::move(rows));
}

} // namespace detail

[[nodiscard]] inline auto viewport(std::vector<Element> cells, ViewportOpts opts)
    -> ComponentBuilder
{
    return detail::adapt([cells = std::move(cells), opts](int offered) -> Element {
        const int w = opts.width > 0 ? std::min(offered, opts.width) : offered;
        const int n = static_cast<int>(cells.size());
        if (n == 0) return Element{ElementList{}};
        if (opts.fit && opts.flow == Flow::Row)
            return detail::fit_rows(cells, opts, w);

        const int gap  = std::max(0, opts.gap);
        const int most = std::max(1, opts.max_width);

        // How many columns the WIDTH alone affords — the ceiling rule, run
        // WITHOUT reference to how many cells we have. This is the number of
        // column SLOTS the viewport is wide enough for; a slot is at most
        // max_width. Counted up from 1 so it is exact.
        int slots = 1;
        while (true) {
            const int span = w - gap * (slots - 1);
            if (span <= 0) break;
            if ((span + slots - 1) / slots <= most) break;   // ceil(span/slots) <= most
            if (opts.min_width > 0) {
                const int next = w - gap * slots;
                if (next / (slots + 1) < opts.min_width) break;
            }
            ++slots;
        }
        if (opts.max_cols > 0) slots = std::min(slots, opts.max_cols);
        slots = std::max(1, slots);

        // Never more columns than cells: an empty column is not "even". With
        // FEWER cells than slots the count drops to n, and — crucially — each
        // column is then held at the SLOT width (≤ max_width) rather than
        // stretched to fill the viewport, so two cards read as two evenly
        // spaced cards, not two half-screen-wide ones.
        const int cols = std::clamp(slots, 1, n);
        const bool underfull = (n < slots);   // fewer cells than the width affords

        // One column, FULL cells: hand back the plain stack at the full slot
        // width, so enabling the flow cannot disturb a narrow layout. (Only
        // when the width truly affords one column — a lone card in a wide
        // viewport is the underfull case below, not this one.)
        if (cols == 1 && !underfull) {
            auto vb = detail::vstack();
            if (opts.gap_y > 0) vb.gap(opts.gap_y);
            vb.width(Dimension::fixed(w));
            vb.max_width(Dimension::fixed(w));
            return vb(std::move(cells));
        }

        // Column widths. Normally divide the slot EXACTLY (base +
        // largest-remainder) so the columns sum to the full width and the
        // last ends flush. But when UNDERFULL — fewer cells than the width
        // affords — hold every column at the slot width (≤ max_width) so the
        // cards stay their natural size and sit evenly, rather than each
        // stretching to swallow the surplus.
        std::vector<int> cw(static_cast<std::size_t>(cols));
        if (underfull) {
            const int slot = std::min(most, (w - gap * (slots - 1)) / slots);
            for (int i = 0; i < cols; ++i)
                cw[static_cast<std::size_t>(i)] = std::max(1, slot);
        } else {
            const int span = std::max(cols, w - gap * (cols - 1));
            const int base = span / cols;
            const int rem  = span % cols;
            for (int i = 0; i < cols; ++i)
                cw[static_cast<std::size_t>(i)] = base + (i < rem ? 1 : 0);
        }

        if (opts.flow == Flow::Row) {
            // Fill HORIZONTALLY first: cells go left-to-right across the
            // columns, then wrap to the next row — a real grid, not
            // independent columns. Four cells over three columns are
            // row0 = [0,1,2], row1 = [3]; the fourth cell starts a fresh row
            // aligned to the top, instead of stacking under the first with a
            // ragged gap. Every row is an hstack of fixed-width cells, and
            // the rows stack vertically, so cells in a row share a baseline
            // (the tallest cell sets the row height).
            std::vector<Element> row_els;
            row_els.reserve(static_cast<std::size_t>((n + cols - 1) / cols));
            for (int start = 0; start < n; start += cols) {
                std::vector<Element> cells_in_row;
                const int end = std::min(n, start + cols);
                for (int i = start; i < end; ++i) {
                    Element cell_el = cells[static_cast<std::size_t>(i)];
                    const int this_w = cw[static_cast<std::size_t>(i - start)];
                    if (opts.equal_rows) {
                        // No wrapper: set the fixed width on the card itself
                        // and let the row's align_items:Stretch size its
                        // HEIGHT to the tallest card in the row. A bordered
                        // box stretched on the cross axis grows its border to
                        // the row band, so the grid reads as a clean matrix.
                        if (auto* bx = as_box(cell_el)) {
                            bx->layout.width      = Dimension::fixed(this_w);
                            bx->layout.align_self = Align::Stretch;
                        }
                        cells_in_row.push_back(std::move(cell_el));
                    } else {
                        auto cb = detail::vstack();
                        cb.width(Dimension::fixed(this_w));
                        // max_width as well as width: a fixed width sizes the
                        // BOX, but a fill()/adapt() child is measured against
                        // whatever the parent chain offers — which, under a
                        // scroll or a prebuilt body, is an unbounded probe.
                        // The ceiling is what that child's measure actually
                        // clamps to, so the column width reaches it.
                        cb.max_width(Dimension::fixed(this_w));
                        cells_in_row.push_back(cb(std::move(cell_el)));
                    }
                }
                auto rb = detail::hstack();
                if (gap > 0) rb.gap(gap);
                if (opts.equal_rows) rb.align_items(Align::Stretch);
                row_els.push_back(rb(std::move(cells_in_row)));
            }

            auto colb = detail::vstack();
            if (opts.gap_y > 0) colb.gap(opts.gap_y);
            return colb(std::move(row_els));
        }

        // Flow::Column — fill VERTICALLY first: fill column 0 top-to-bottom,
        // then column 1, and so on. Each column holds a CONTIGUOUS run of
        // cells, so reading order is down-then-across. The columns are
        // independent vstacks (no shared row baseline); every column gets
        // ceil(n/cols) or floor(n/cols) cells, differing by at most one, with
        // the taller columns first.
        const int per      = n / cols;
        const int tall_cnt = n % cols;   // first `tall_cnt` columns get one extra
        std::vector<std::vector<Element>> col_cells(static_cast<std::size_t>(cols));
        int idx = 0;
        for (int c = 0; c < cols; ++c) {
            const int take = per + (c < tall_cnt ? 1 : 0);
            for (int k = 0; k < take; ++k)
                col_cells[static_cast<std::size_t>(c)]
                    .push_back(std::move(cells[static_cast<std::size_t>(idx++)]));
        }

        std::vector<Element> col_els;
        col_els.reserve(static_cast<std::size_t>(cols));
        for (int c = 0; c < cols; ++c) {
            auto cb = detail::vstack();
            if (opts.gap_y > 0) cb.gap(opts.gap_y);
            cb.width(Dimension::fixed(cw[static_cast<std::size_t>(c)]));
            col_els.push_back(cb(std::move(col_cells[static_cast<std::size_t>(c)])));
        }

        auto rb = detail::hstack();
        if (gap > 0) rb.gap(gap);
        return rb(std::move(col_els));
    });
}

/// Sugar: `viewport(cells, 30)` — "a column is about 30 wide; split evenly".
[[nodiscard]] inline auto viewport(std::vector<Element> cells, int max_width)
    -> ComponentBuilder
{
    return viewport(std::move(cells), ViewportOpts{.max_width = max_width});
}

} // namespace maya

