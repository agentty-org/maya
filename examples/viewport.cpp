// examples/viewport.cpp — the viewport() layout widget, exercised with real widgets,
// now scrollable.
//
// viewport(cells, max_width) is PURELY layout. It chooses a column count from
// a width CEILING and then splits the cells EVENLY BY COUNT into that many
// columns, round-robin so reading order is preserved:
//
//   slot width <= max_width          → 1 column   (the plain stack)
//   max_width  <  width <= 2·max     → 2 columns   cells go [0,2,4…] [1,3,5…]
//   2·max      <  width <= 3·max     → 3 columns   cells go [0,3…] [1,4…] [2,5…]
//   …and so on.
//
// Every column is a fixed-width box, so ANYTHING inside stays responsive to
// its OWN column width — the gauges, bar charts and wrapped text below all
// re-solve when the column count (and therefore the column width) changes.
//
// SCROLL. Twelve cards rarely fit a screen at one or two columns, so the
// whole grid lives in a vertical scroll viewport with a scrollbar. As the
// window widens the cards fan into more columns, the grid gets SHORTER, and
// the scrollbar shrinks (or vanishes) on its own — the scroll extent falls
// out of the layout, we never compute a content height by hand.
//
// Keys:  ↑/↓ · j/k · PgUp/PgDn · Home/End · mouse wheel — scroll
//        +/-  change the column ceiling      q  quit
//        resize the terminal to watch the columns reflow

#include <string>
#include <vector>

#include <maya/host/run.hpp>
#include <maya/maya.hpp>
#include <maya/element/grid.hpp>          // viewport(), columns(), grid()
#include <maya/widget/agentty/bar_chart.hpp>
#include <maya/widget/gauge.hpp>
#include <maya/widget/agentty/scrollbar.hpp>      // scrollbar_y, ScrollbarStyle

using namespace maya;
using namespace maya::dsl;

namespace {

// The data behind one card. Pure values — no layout, no widths.
struct Metric {
    std::string title;
    float       load;
    Color       accent;
    std::string blurb;
};

// The dozen cards. Distinct hues so you can track which card went to which
// column as they fan out.
const std::vector<Metric>& metrics() {
    static const std::vector<Metric> m = {
        {"CPU",       0.72f, Color::rgb(100, 180, 255),
         "Utilisation across all cores. This paragraph rewraps to the column "
         "it lands in, so the card stays legible at any column count."},
        {"Memory",    0.55f, Color::rgb(160, 220, 120),
         "Resident set and cache pressure. Nothing here is measured against "
         "the screen — only against this column's width."},
        {"Disk",      0.88f, Color::rgb(255, 180, 90),
         "Throughput on the primary volume. Shrink the window and the meters "
         "redraw shorter as the column narrows."},
        {"Network",   0.34f, Color::rgb(200, 140, 255),
         "Ingress and egress. Widen the window and another column appears; the "
         "cards redistribute evenly and the grid gets shorter."},
        {"GPU",       0.61f, Color::rgb(120, 220, 220),
         "Compute occupancy. Even by count: each column gets the same number "
         "of cards, give or take one."},
        {"Battery",   0.47f, Color::rgb(240, 200, 100),
         "Charge and draw. Reading order is preserved left to right as the "
         "cells fan out into more columns."},
        {"Temp",      0.66f, Color::rgb(255, 130, 130),
         "Package temperature. The bars below are their own responsive widget "
         "nested inside the column."},
        {"Fan",       0.40f, Color::rgb(140, 200, 255),
         "Cooling duty cycle. When only one column fits, this whole grid is a "
         "plain vertical stack — byte for byte."},
        {"Swap",      0.18f, Color::rgb(190, 170, 255),
         "Backing store pressure. Idle most of the time, which is exactly what "
         "you want to see here."},
        {"IO wait",   0.52f, Color::rgb(255, 200, 140),
         "Time blocked on the disk. Scroll down — the twelfth card is only "
         "reachable by scrolling at one or two columns."},
        {"Load avg",  0.71f, Color::rgb(150, 230, 180),
         "One / five / fifteen minute pressure. The scrollbar to the right "
         "shrinks as more columns make the grid shorter."},
        {"Uptime",    0.95f, Color::rgb(220, 220, 120),
         "Days since the last reboot. The last card — press End to jump here, "
         "Home to jump back to CPU."},
    };
    return m;
}

// One responsive card: a bordered panel holding a gauge, a small bar chart,
// and a paragraph that wraps to whatever width its column gives it. None of
// these know the terminal width — they only ever see their column's width.
Element card(const Metric& mt) {
    BarChart chart({
        {"in",  mt.load * 0.9f, mt.accent},
        {"out", mt.load * 0.5f},
        {"err", mt.load * 0.2f, Color::red()},
    }, 1.0f);

    auto body = vstack();
    body.gap(0);
    body.padding(1);
    body.border(BorderStyle::Round, mt.accent);
    return body(
        text(mt.title, Style{}.with_fg(mt.accent).with_bold()),
        blank(),
        Gauge(mt.load, "load").build(),
        blank(),
        chart.build(),
        blank(),
        text(mt.blurb, Style{}.with_dim())     // wraps to the column width
    );
}

struct Viewport {
    struct Model {
        int ceiling = 32;   // per-column max width; drives the split
        int term_w  = 120;  // live terminal width,  from ResizeEvent
        int term_h  = 40;   // live terminal height, from ResizeEvent
        Flow flow   = Flow::Row;   // row-major grid vs column-major columns

        // Scroll state is mutable: view() takes Model by const ref, but the
        // renderer writes max_y / viewport bounds back after layout, and the
        // scroll pipe needs a mutable reference to wire that writeback.
        mutable ScrollState scroll;

        Model() = default;
    };

    struct Wider {};
    struct Narrower {};
    struct ToggleFlow {};                 // swap row-major ↔ column-major
    struct Scroll { KeyEvent key; };      // a scroll key → ScrollState::handle
    struct Wheel  { MouseEvent mouse; };  // a wheel event → ScrollState::handle
    struct Resize { Size size; };
    struct Quit {};
    using Msg = std::variant<Wider, Narrower, ToggleFlow, Scroll, Wheel, Resize, Quit>;

    using Cmd = jaal::Cmd<Msg>;

    // The scroll viewport height in rows: the terminal minus this app's own
    // chrome (title + status + border + padding). One number, used by both
    // the scroll pipe and the scrollbar so they always agree.
    static int viewport_h(const Model& m) {
        return std::max(3, m.term_h - 6);
    }

    // The width the GRID may occupy: the terminal, minus this app's 1-cell
    // padding on each side, minus the scrollbar column and its 1-cell gutter.
    // Passing this as viewport()'s `.width` bound makes it lay its columns
    // into exactly the space LEFT of the scrollbar — so no card border can
    // ever land in, or under, the bar column, whatever the flex solver does.
    static constexpr int kBar = 1, kGap = 1, kPad = 2;
    static int grid_w(const Model& m) {
        return std::max(1, m.term_w - kPad - kGap - kBar);
    }

    static Cmd update(Model& m, Wider)    { m.ceiling = std::min(m.ceiling + 4, 80); return {}; }
    static Cmd update(Model& m, Narrower) { m.ceiling = std::max(m.ceiling - 4, 12); return {}; }
    static Cmd update(Model& m, ToggleFlow) { m.flow = (m.flow == Flow::Row) ? Flow::Column : Flow::Row;
                                              return {}; }
    static Cmd update(Model& m, Scroll s) { (void)m.scroll.handle(s.key, viewport_h(m)); return {}; }
    static Cmd update(Model& m, Wheel w)  { (void)m.scroll.handle(w.mouse);               return {}; }
    static Cmd update(Model& m, Resize r) { m.term_w = r.size.width.value;
                                            m.term_h = r.size.height.value;         return {}; }
    static Cmd update(Model& m, Quit)     { m = Model{}; return Cmd::quit(0); }

    static Element view(const Model& m) {
        // Build every card, then hand the whole set to viewport(). It spreads
        // them evenly across however many columns the width + ceiling allow.
        std::vector<Element> cards;
        cards.reserve(metrics().size());
        for (const auto& mt : metrics()) cards.push_back(card(mt));

        // Bound the grid to the space LEFT of the scrollbar (via viewport()'s
        // `.width`), so adapt() lays its columns into exactly that width and
        // the last column's border can never reach the bar column.
        Element grid = viewport(std::move(cards),
                                ViewportOpts{.max_width = m.ceiling,
                                             .gap = 2, .gap_y = 1,
                                             .width = grid_w(m),
                                             .flow = m.flow,
                                             .equal_rows = true});

        const int vh = viewport_h(m);
        auto& sc = m.scroll;   // mutable member; ok through const Model&

        // Scroll viewport + scrollbar. Give the scroll box a FIXED viewport
        // width (grid_w) so overflow:Scroll clips the grid to exactly the
        // space left of the bar; the grid is bounded to the same width, so
        // nothing overflows horizontally and no border reaches the bar.
        auto sb = hstack();
        sb.gap(1);
        Element scroller = sb(
            std::move(grid) | scroll(sc, grid_w(m), vh),
            scrollbar_y(m.scroll, vh, ScrollbarStyle::block())
        );

        std::string status =
            std::string("flow=") + (m.flow == Flow::Row ? "row" : "col") +
            "   ceiling=" + std::to_string(m.ceiling) +
            "   y=" + std::to_string(m.scroll.y) +
            "/"     + std::to_string(m.scroll.max_y) +
            "   ·  [f] flow   [-]/[+] ceiling   ↑/↓·wheel scroll   [q] quit";

        auto root = vstack();
        root.gap(0);
        root.padding(1);
        return root(
            text("viewport() — even, responsive, scrollable columns",
                 Style{}.with_bold()),
            text(status, Style{}.with_dim()),
            blank(),
            std::move(scroller)
        );
    }

    // +/- and q are ordinary keys. Everything the ScrollState recognises
    // (arrows, j/k, PgUp/PgDn, Home/End) is forwarded verbatim as a
    // Scroll message; the wheel goes through as Wheel. Resize keeps the
    // viewport height current.
    static std::optional<Msg> on(const Model&, const KeyEvent& k) {
        if (key_is(k, 'q')) return Quit{};
        if (key_is(k, '+') || key_is(k, '=')) return Wider{};
        if (key_is(k, '-')) return Narrower{};
        if (key_is(k, 'f')) return ToggleFlow{};
        return Scroll{k};   // let ScrollState decide if it's a scroll key
    }
    static std::optional<Msg> on(const Model&, const MouseEvent& me) {
        return Wheel{me};
    }
    static std::optional<Msg> on(const Model&, const ResizeEvent& r) {
        return Resize{Size{r.width, r.height}};
    }
};

static_assert(Program<Viewport>, "Viewport must satisfy the App concept");

} // namespace

int main() {
    return run<Viewport>({.title = "viewport", .mouse = true});
}
