// maya::Panel — implementation. See widget/panel.hpp for the design.
//
// A real .cpp, not a header: Panel is the whole overlay family (every picker
// and every settings form), and its renderer instantiates std::visit over a
// ten-alternative control variant. Inlining that into every TU that merely
// NAMES a Panel is what maya already avoids for its other heavyweight widget
// (src/widget/markdown.cpp); the other 178 widgets are small enough to stay
// header-only.

#include "maya/widget/panel.hpp"
#include "maya/widget/tab_strip.hpp"   // the shared strip the tabs field renders
#include "maya/platform/io.hpp"       // query_terminal_size, for the width clamp

#include <algorithm>
#include <cstdlib>

#include "maya/style/theme.hpp"

namespace maya {

namespace panel::detail {

int terminal_cols() noexcept {
    const auto sz = platform::query_terminal_size(platform::stdout_handle());
    int term = sz.width.value;
    // No tty (a pipe, a test harness, a render-to-canvas probe): the query
    // returns a hardcoded fallback rather than a real width, so consult
    // COLUMNS the way every other size lookup here does. Without this the
    // answer is silently 80 in exactly the environments that render at
    // other widths.
    if (!platform::is_tty(platform::stdout_handle())) {
        if (const char* c = std::getenv("COLUMNS")) {
            if (const int n = std::atoi(c); n > 0) term = n;
        }
    }
    return term > 0 ? term : 0;
}

} // namespace panel::detail

// Columns the VALUE cell occupies, as a max over every item.
//
// Only the drawn kinds carry a value of their own — a text kind's trailing
// cell is the item's `trailing` string, which the existing row idiom
// already aligns by flex. So this measures exactly the rows that need a
// shared column and ignores the rest.
int Panel::value_basis() const noexcept {
    // The drawn value kinds (Meter/Spark) that owned a right-aligned value
    // column are standalone widgets now, dropped into an item's prebuilt
    // cell rather than laid out by the panel. Nothing left to align on, so
    // there is no shared value column.
    return 0;
}

namespace {

// The panel's min-width, clamped to something the terminal can actually
// show.
//
// A floor wider than the screen is not a minimum, it is an overflow: the
// overlay is centred, so the excess is split off BOTH edges and the frame's
// own border is the first thing clipped. Every row then renders without a
// right edge, which is the "content overflows the frame" report.
//
// kChromeCols is subtracted so the clamp leaves room for the border, the
// padding and the scrollbar gutter rather than for the body alone — the
// whole point is that the FRAME fits, and the frame is what the chrome is.
[[nodiscard]] int clamped_min_width_for(int requested) {
    const auto sz = platform::query_terminal_size(platform::stdout_handle());
    int term = sz.width.value;
    // No tty (a pipe, a test harness, a render-to-canvas probe): the query
    // returns a hardcoded fallback rather than a real width, so consult
    // COLUMNS the way every other size lookup here does. Without this the
    // clamp silently uses 80 in exactly the environments that render at
    // other widths.
    if (!platform::is_tty(platform::stdout_handle())) {
        if (const char* c = std::getenv("COLUMNS")) {
            if (const int n = std::atoi(c); n > 0) term = n;
        }
    }
    if (term <= 0) return std::max(20, requested);

    const int usable = term - Panel::Config::kChromeCols;
    return std::max(20, std::min(requested, std::max(20, usable)));
}

} // namespace

// ============================================================================
//  Row layout — flex, never arithmetic
// ============================================================================

Element Panel::row_line(Element lead, Element trail,
                               bool trailing_secondary, Style gap_style,
                               bool value_primary) {
    // Which side yields under width pressure. The default is that the LEADING
    // cell gives way (shrink 3×), so a long label truncates before it can push
    // the value off the row. `trailing_secondary` flips it: a command
    // palette's description must never eat its command name.
    const float lead_shrink  = trailing_secondary ? 1.0f : 3.0f;
    const float trail_shrink = trailing_secondary ? 4.0f : 1.0f;

    std::vector<Element> cells;
    if (value_primary) {
        // Prompt-style row (a lone › + an input): the control hugs the
        // prompt and the SLACK goes after it, instead of the label─→value
        // column layout that right-aligns the value a screen-width away
        // from its two-character prompt.
        cells.push_back(std::move(lead) | dsl::shrink(lead_shrink));
        cells.push_back(Element{TextElement{.content = "  ", .style = gap_style}});
        cells.push_back(std::move(trail) | dsl::shrink(trail_shrink));
        cells.push_back(dsl::spacer().build());   // grow=1: takes the slack
    } else {
        cells.push_back(std::move(lead) | dsl::grow(1.0f) | dsl::shrink(lead_shrink));
        cells.push_back(Element{TextElement{.content = "  ", .style = gap_style}});
        cells.push_back(std::move(trail) | dsl::shrink(trail_shrink));
    }

    auto row = maya::detail::hstack().width(Dimension::percent(100));
    // The cursor wash belongs to the ROW, not to its text. A text run only
    // paints the cells it occupies, so styling the cells left the SLACK
    // between the leading and trailing columns unwashed — the band appeared
    // as two stripes with a hole in the middle, widest on exactly the wide
    // terminals where the cursor most needs to be findable. The flex box owns
    // the full row width, so it is the only thing that can fill it.
    if (gap_style.bg.has_value()) row.style(Style{}.with_bg(*gap_style.bg));
    return row(std::move(cells));
}

// A drawn control with a note beside it, in the row's slack.
//
// THE SHAPE OF THE PROBLEM. A row is three things on one line, and they
// are not equally important:
//
//     the LABEL   says which row this is    — must survive
//     the CONTROL carries the number        — must survive, IN ITS COLUMN
//     the NOTE    explains the row          — yields first
//
// The column matters as much as the survival. Values in a section are
// read against each other, so they share one right-aligned column, and a
// single annotated row that pushes its value out of that column costs
// the whole section its comparability. That is the bug this exists to
// prevent, and the reason the note goes BEFORE the control rather than
// after it: appended after, it extends a right-aligned cell rightward
// and the flex box shoves the whole cell left to make room — "2.5k"
// landing mid-row while 1h, 2h and 5 stay flush right above it.
//
// In front, it occupies the gap between the label and the control, which
// is space that belonged to nobody, and nothing else moves.
//
// WHY BOTH BOUNDS, since either alone looks sufficient and is not:
//
//   shrink decides who gives ground once the row is over-subscribed. By
//   then the label has already been asked to truncate, so a note that
//   merely yields slowly still drags "Measured turns" down to "Measu…".
//
//   the cap decides how much the note may ASK for in the first place.
//   Under it the label is not competing with the note at all.
//
// Together: the note cannot ask for more than the row's natural slack,
// and if the row tightens anyway it is the first thing to give.
//
// fit_row() reads like the right primitive here and is not. It drops by
// priority, which is the right policy, but sizes the row to its natural
// width — abandoning the shared column this whole arrangement exists to
// protect. Measured: values scattered and labels truncated at every
// width below 140.
Element Panel::annotated(Element control, const std::string& note,
                         Style note_style) const {
    // About a row's natural slack. Floored so a narrow pane still shows a
    // word or two rather than an ellipsis standing on its own.
    const int cap = std::max(kMinNoteCols, draw_budget() / kNoteShare);

    BoxElement box;
    box.layout.direction = FlexDirection::Row;
    box.layout.max_width = Dimension::fixed(cap);
    box.layout.shrink    = kNoteShrink;
    box.children.push_back(Element{TextElement{
        .content = "  " + note,
        .style   = note_style,
        .wrap    = TextWrap::TruncateEnd}});

    // The note goes AFTER the control, not before it.
    //
    // Before, it sits between the label and the picture — so a row that
    // has one starts its bar further right than a row that does not, and
    // a column of bars stops sharing the baseline that makes them
    // comparable. The note is a gloss on the number; it belongs past it,
    // where a varying width costs nothing.
    return dsl::h(std::move(control) | dsl::shrink(0.0f),
                  Element{std::move(box)}).build();
}

Element Panel::right_line(Element content) {
    std::vector<Element> cells;
    cells.push_back(dsl::spacer());
    cells.push_back(std::move(content) | dsl::shrink(1.0f));
    // TWO columns, matching the gap a row leaves before its own trailing cell
    // (see row_line). One column left the option list a character further
    // right than the value it replaces, so opening a dropdown nudged the
    // column it belongs to.
    cells.push_back(Element{TextElement{.content = "  "}});
    return maya::detail::hstack()
               .width(Dimension::percent(100))
               (std::move(cells));
}

// ============================================================================
//  Controls -> text
// ============================================================================

std::pair<std::string, Style>
Panel::render_control(const Item& r, int index) const {
    return render_control(r, index, nullptr);
}

std::pair<std::string, Style>
Panel::render_control(const Item& r, int index, std::size_t* caret_at) const {
    return render_control(r, index, caret_at, nullptr);
}

std::pair<std::string, Style>
Panel::render_control(const Item& r, int index, std::size_t* caret_at,
                      std::vector<StyledRun>* runs_at) const {
    // One widget per item kind (widget/panel/item/*.hpp). The panel's only
    // jobs here are assembling the ItemCtx — the ONLY facts an item may
    // know — and dispatching. Everything glyph-level lives with the kind.
    auto ctx = item_ctx(index);
    ctx.caret_out = caret_at;
    ctx.runs_out  = runs_at;
    return panel::render(r.control, ctx);
}

// The facts an item may know, in ONE place.
//
// Built by both the measure pass and the render pass, which is the point:
// item_lines() decides a row's height without painting it, and if the two
// assembled different contexts they could disagree about how tall a
// control is — a row measured at one height and painted at another is the
// scroll-accounting bug this family has already had once.
panel::ItemCtx Panel::item_ctx(int index) const {
    return panel::ItemCtx{
        .theme       = cfg_.theme,
        .open        = cfg_.menu && cfg_.menu_row == index,
        .edit_budget = edit_budget(),
        .draw_budget = draw_budget(),
        .value_basis = value_basis(),
    };
}

// ============================================================================
//  Rows
// ============================================================================

std::vector<Element> Panel::render_item(const Item& r, int index) const {
    const auto& th = cfg_.theme;
    std::vector<Element> out;

    // A section header carries structure a row never has: upper-case and a
    // rule to the right edge. It used to be dim bold text, which is exactly
    // how a locked row renders — so "Endpoint  auto-detected" read as a title.
    //
    // The rule is drawn only when the panel asks for it. A settings pane is
    // a long single column of interchangeable rows, and a full-width rule is
    // what stops one section running into the next. A stats tab is not: its
    // sections are already separated by blank rows and by the column split,
    // and a rule across every heading turns a page of figures into a form.
    // Worse at width — in two columns each rule runs to its own column edge,
    // so the eye reads four horizontal lines where there are two groups.
    if (r.is_header()) {
        std::string caps = r.leading;
        for (auto& ch : caps)
            if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');

        const bool ruled = cfg_.header_rule;
        out.push_back(Element{ComponentElement{
            .render = [caps, th, ruled](int w, int) -> Element {
                std::string s = "  " + caps + " ";
                std::vector<StyledRun> runs{
                    {0, s.size(), Style{}.with_fg(ruled ? th.help : th.active)
                                         .with_bold()}};
                const int rule = ruled ? std::max(0, w - string_width(s) - 2) : 0;
                if (rule > 0) {
                    const std::size_t at = s.size();
                    for (int i = 0; i < rule; ++i) s += "\xe2\x94\x80";
                    runs.push_back({at, s.size() - at, Style{}.with_fg(th.origin)});
                }
                return Element{TextElement{.content = std::move(s),
                                           .wrap = TextWrap::NoWrap,
                                           .runs = std::move(runs)}};
            },
            // The header FILLS its line when painted, but its NATURAL size is
            // just the label — the rule is decoration that expands into
            // whatever is offered, not content that demands room.
            //
            // Returning Columns(mw) conflated the two, and mw is not always a
            // real terminal width: a scroll viewport measures its children
            // against an unbounded width to discover the content extent, so
            // the header answered "I need 2^24 columns". That became the
            // panel's max_x, and since the value depended on the offered
            // width, every RESIZE changed it and dirtied the scroll state —
            // making the run loop redraw the whole overlay a second time on
            // 25 of 31 widths. It also told the scrollbar machinery the panel
            // was horizontally scrollable by sixteen million columns.
            //
            // A vertical list has no horizontal extent; measuring to the label
            // says so, and the render lambda still paints the full rule.
            .measure = [caps](int mw) -> Size {
                const int natural = string_width("  " + caps + " ");
                return Size{Columns(std::min(mw, natural)), Rows(1)};
            },
        }});
        return out;
    }

    const bool on_row = r.selected;

    // The cursor row carries a subtle wash across its whole span. Applied
    // per-run AND as the base style, because runs must cover the content in
    // order and cannot overlap — a tint laid over the top would leave the gaps
    // between cells untinted and read as broken stripes.
    const auto tint = [&](Style s) {
        return (on_row && !s.inverse) ? s.with_bg(th.row_bg) : s;
    };
    const Style base = on_row ? Style{}.with_bg(th.row_bg) : Style{};

    // Leading: edge bar, badge, label (+ fuzzy-match accents).
    std::string left;
    std::vector<StyledRun> lruns;
    const auto put = [&](std::string_view t, Style st) {
        if (t.empty()) return;
        lruns.push_back({left.size(), t.size(), tint(st)});
        left += t;
    };

    // Column 0 is the marker lane. The CURSOR wins over the ACTIVE marker on
    // overlap — where you are outranks where you were — and both use the same
    // bar glyph so a picker row and a form row line up exactly.
    // While the row is being EDITED the bar takes the caret's hue: the mode
    // is visible at the left edge, in the same colour as the value being
    // typed and the footer hint. One hue = one mode, three places.
    if (on_row)          put(kEdgeBar, Style{}.with_fg(panel::is_editing(r.control)
                                                           ? th.value_edit
                                                           : th.cursor));
    else if (r.active)   put(kEdgeBar, Style{}.with_fg(cfg_.active_color));
    else                 put(" ", Style{});
    put(" ", Style{});
    if (!r.badge.empty()) {
        put(r.badge, r.badge_style.fg.has_value() ? r.badge_style
                                                  : Style{}.with_fg(th.active));
        put(" ", Style{});
    }

    // A locked row's LABEL stays fully readable — it is still information,
    // it just is not editable, and dimming it made it look like a header.
    const Style label_style =
        r.leading_style.fg.has_value() ? r.leading_style
      : on_row                        ? Style{}.with_fg(th.title).with_bold()
                                      : Style{}.with_fg(th.label);
    const std::size_t label_at = left.size();
    put(r.leading, label_style);

    // Fuzzy-match accents: the matched CHARACTERS keep their hue even on the
    // cursor row, so "which chars matched" stays legible under the tint. Runs
    // must be ordered and non-overlapping, so the label's own run is replaced
    // by an alternating sequence rather than having accents laid over it.
    if (!r.highlight.empty() && !r.leading.empty()) {
        std::vector<bool> hot(r.leading.size(), false);
        for (int p : r.highlight)
            if (p >= 0 && p < static_cast<int>(r.leading.size())) hot[static_cast<std::size_t>(p)] = true;

        lruns.pop_back();                       // drop the single label run
        const Style hot_style =
            tint(label_style.with_fg(r.highlight_fg).with_bold());
        std::size_t i = 0;
        while (i < r.leading.size()) {
            const bool h = hot[i];
            std::size_t j = i;
            while (j < r.leading.size() && hot[j] == h) ++j;
            lruns.push_back({label_at + i, j - i, h ? hot_style : tint(label_style)});
            i = j;
        }
    }

    // Trailing: the control, then its origin. A plain `trailing` string is
    // the same thing as a Label control — resolved here so the renderer below
    // has exactly one path.
    std::size_t caret_at = std::string::npos;
    std::vector<StyledRun> control_runs;
    auto [right, rstyle] = render_control(r, index, &caret_at, &control_runs);
    if (right.empty() && !r.trailing.empty()) {
        right  = r.trailing;
        rstyle = r.trailing_style;
    }
    // Colour sample, on the TRAILING side.
    //
    // It belongs here rather than after the label because the trailing cell
    // is the panel's value column: it is right-aligned, so every row's
    // sample starts at the same x no matter how long the name is. Hung off
    // the label instead, the swatches staircase with name length — which is
    // exactly what makes a list of colours hard to compare, and comparing
    // them is the only reason to show them.
    //
    // A colour strip is RESPONSIVE: it sheds samples rather than clipping.
    //
    // Truncation is the right failure for text — an ellipsised name is still
    // a name — but half a swatch is not half a palette, it is a lie about
    // which colours the scheme has. So the strip is sized to what the row
    // can actually spare and drops whole entries from the RIGHT, keeping the
    // leading ones (primary, accent, info), which are the hues you see most.
    //
    // Sized HERE rather than inside a component, because the trailing cell's
    // width is what the flex split negotiates against: a component measures
    // as flexible, so it swallowed the row and truncated the scheme NAME —
    // the one thing the row exists to show — while the strip drifted left.
    // The panel's own width is the honest budget, and the label keeps first
    // claim on it.
    if (!r.swatch.empty()) {
        const int panel_w  = cfg_.min_width > 0 ? cfg_.min_width : 48;
        const std::string right_prefix = right;
        const int prefix_w = string_width(right_prefix);
        // Chrome: edge bar, indents, the two-space gap, the border.
        constexpr int kChrome = 12;
        // Leave the longest name room to breathe before the strip starts.
        const int reserved  = std::max(16, panel_w / 3);
        int room = panel_w - kChrome - prefix_w - reserved;
        int keep = std::clamp(room / 2, 0, static_cast<int>(r.swatch.size()));

        std::string s = right_prefix;
        std::vector<StyledRun> runs;
        if (keep > 0) {
            if (!s.empty()) { runs.push_back({0, s.size(), rstyle}); s += "  "; }
            for (int i = 0; i < keep; ++i) {
                runs.push_back({s.size(), 6,        // ██ = 2 x 3 bytes
                                Style{}.with_fg(r.swatch[static_cast<std::size_t>(i)])});
                s += "\xe2\x96\x88\xe2\x96\x88";
            }
        } else if (!s.empty()) {
            runs.push_back({0, s.size(), rstyle});
        }
        right        = std::move(s);
        control_runs = std::move(runs);
    }
    if (r.locked) rstyle = Style{}.with_fg(th.locked);
    const std::string origin =
        r.locked && !r.locked_reason.empty() ? r.locked_reason : r.origin;

    // TruncateEnd, not NoWrap: when the flex box takes space back from a cell,
    // the cell must ellipsise inside its allotment rather than spill past it.
    Element lead{TextElement{.content = std::move(left),
                             .style   = base,
                             .wrap    = TextWrap::TruncateEnd,
                             .runs    = std::move(lruns)}};

    std::string tail = right;
    std::vector<StyledRun> truns;
    const Style rrun = tint(on_row ? rstyle.with_bold() : rstyle);
    if (!control_runs.empty()) {
        // The control styled itself. A drawn kind (a meter's filled head
        // against its unfilled track) needs more than one hue, and the
        // boundary between them IS the datum.
        //
        // Tinted here rather than in the widget: the cursor-row wash is
        // the panel's decision, and an item widget that knew about it
        // would be growing panel logic.
        for (const auto& run : control_runs)
            truns.push_back({run.byte_offset, run.byte_length, tint(run.style)});
    } else if (caret_at != std::string::npos && caret_at < right.size()) {
        // Split the control's run so the caret cell carries the
        // caret_anchor meta-bit: the serializer puts the HARDWARE cursor
        // there (terminal-native blink, IME composition at the right spot,
        // screen readers find the edit point).
        constexpr std::size_t kBarLen = 3;   // █ is 3 bytes in UTF-8
        // Same caret as the composer and the palette: the cell paints
        // nothing and the terminal's blinking block cursor sits on it. A
        // painted █ under it hid the blink, since both phases looked solid.
        const Style caret_style = tint(Style{}.with_fg(th.value_edit))
                                      .with_conceal()
                                      .with_caret_anchor()
                                      .with_caret_shape(1);
        if (caret_at > 0) truns.push_back({0, caret_at, rrun});
        truns.push_back({caret_at, kBarLen, caret_style});
        if (caret_at + kBarLen < right.size())
            truns.push_back({caret_at + kBarLen,
                             right.size() - caret_at - kBarLen, rrun});
    } else {
        truns.push_back({0, right.size(), rrun});
    }
    if (!origin.empty()) {
        tail += "  ";
        truns.push_back({tail.size(), origin.size(),
                         tint(Style{}.with_fg(th.origin))});
        tail += origin;
    }
    Element trail{TextElement{.content = std::move(tail),
                              .style   = base,
                              .wrap    = TextWrap::TruncateEnd,
                              .runs    = std::move(truns)}};

    // A control that lays itself out replaces the string cell entirely.
    //
    // Everything above builds ONE concatenated string, which is the right
    // answer for a word and the wrong one for a table: a string cannot hold
    // columns, so a kind wanting them has to pad and shrink by hand, and
    // that hand-rolled flex is where this family's sizing bugs came from.
    // A laid-out control hands back real siblings and lets maya's engine
    // size them.
    //
    // The origin note still rides along, appended as a sibling rather than
    // concatenated — same reason.
    if (panel::lays_itself_out(r.control)) {
        Element cell = panel::control_element(r.control, item_ctx(index));

        // A PICTURE takes the whole row.
        //
        // The shared row idiom is label · gap · trailing, which is right for
        // a value that annotates a name. A ring or a curve annotates
        // nothing — it IS the content — and squeezing it into the trailing
        // cell would leave it half the width beside an empty label lane.
        // Its heading is the item ABOVE it, which is how a section already
        // reads.
        if (panel::owns_full_row(r.control, item_ctx(index))) {
            out.push_back(std::move(cell));
            return out;
        }

        if (!origin.empty())
            cell = annotated(std::move(cell), origin,
                             tint(Style{}.with_fg(th.origin)));
        trail = std::move(cell);
    }

    out.push_back(row_line(std::move(lead), std::move(trail),
                           r.trailing_secondary, base, r.value_primary));

    // Only the FOCUSED row shows its help. One setting is one row; the
    // description appears where the cursor is, which is the only row it can be
    // describing. TruncateEnd, not NoWrap: on a narrow terminal a long help
    // line must ellipsise INSIDE the border — NoWrap let it paint straight
    // through the panel frame (seen at width≄44).
    if (on_row && !r.help.empty())
        out.push_back(Element{TextElement{
            .content = "    " + r.help,
            .style   = Style{}.with_fg(th.help),
            .wrap    = TextWrap::TruncateEnd}});

    // An error WRAPS. Every other prose line in this family truncates,
    // because a cut hint still points the right way — but an error is the
    // one string in a pane whose tail is the part you need: "connect
    // failed: no route to 10.0.0.5:11434 (check the endpoint…)" truncated
    // to the colon tells you something broke and nothing about what to do.
    // Reported on #75 with a cut error and blank rows underneath it.
    if (!r.error.empty())
        out.push_back(Element{TextElement{
            .content = "      \xe2\x9a\xa0 " + r.error,
            .style   = Style{}.with_fg(th.error),
            .wrap    = TextWrap::Wrap}});

    return out;
}

// ============================================================================
//  The inline option list
// ============================================================================
//
// Plain rows in the panel's own flow, RIGHT-ALIGNED under the value column
// they replace. It was a z-stacked float, which had to invent an opaque
// background, a width, a flip and a clamp — and every one of those was a bug.
// An enum needs none of it: the list expands in place and pushes the rows
// below it down.

std::vector<Element> Panel::render_menu(const Menu& m) const {
    const auto& th = cfg_.theme;
    std::vector<Element> out;

    const int total = static_cast<int>(m.options.size());
    if (total == 0) {
        out.push_back(right_line(Element{TextElement{
            .content = "(no options)",
            .style   = Style{}.with_fg(th.help)}}));
        return out;
    }

    // The window. `scroll`/`viewport` are host HINTS; the invariant that the
    // highlight is visible is guaranteed HERE, so a stale hint can never hide
    // the cursor.
    const int hi   = std::clamp(m.highlighted, 0, total - 1);
    const int view = std::max(1, std::min(m.viewport, total));
    int first = std::clamp(m.scroll, 0, std::max(0, total - view));
    if (hi < first)         first = hi;
    if (hi >= first + view) first = hi - view + 1;
    first = std::clamp(first, 0, std::max(0, total - view));

    // ONE width for the whole list, so every option's radio sits in the SAME
    // column. Right-aligning each line independently — which is what happens
    // when each is its own right_line — put the radios in as many columns as
    // there were distinct label lengths, and a radio group whose radios do not
    // line up does not read as a group at all: it reads as loose text that
    // happens to sit near the row. Padding to the widest option turns the same
    // lines into a rectangular block hanging under the value it replaces,
    // which is the shape a dropdown is supposed to have.
    //
    // Measured over the WHOLE option set rather than the visible window, so
    // scrolling the list does not re-flow its left edge.
    int widest = 0;
    for (const auto& opt : m.options)
        widest = std::max(widest, string_width(opt));

    // Bounded, because the panel may be narrower than the longest option. The
    // option LINES truncate gracefully (TruncateEnd puts an … on them), but the
    // rules are plain NoWrap text: an over-wide rule is clipped from the right,
    // which silently eats the corner and the connector that make it a bracket.
    // The result reads as two stray lines rather than a frame — worst exactly
    // on the narrow terminals where the grouping cue matters most.
    //
    // The cap is intentionally generous: this only bites when the block cannot
    // fit, and clamping it early would narrow the block on wide terminals too.
    // `min_width` is the floor the panel is built to, minus the frame, padding
    // and the marker/scrollbar columns the row idiom reserves.
    widest = std::min(widest, std::max(8, cfg_.min_width - 16));

    // The frame. Without one the list has no boundary of its own — it is just
    // more rows in a panel already made of rows, so "these four belong to that
    // field" is left entirely to the reader.
    //
    // The bracket is anchored to the CHEVRON. An open Choice row collapses to
    // a bare ▴ sitting one gap column in from the right edge (see
    // render_control), and right_line leaves that same two-column gap — so a
    // rule that simply spans the block ends two columns short of the chevron
    // and the ┬ points at empty space. Padding the rules by that gap puts the
    // corner directly beneath the ▴, which is what makes the list read as
    // hanging FROM the control rather than merely sitting near it.
    const int block_w = widest + 4;           // "❯ " + "◉ " columns
    // The bottom rule is an ordinary right_line, so it shares the options'
    // right edge and their left edge follows from the common width. Making it
    // one wider to "match" the top rule moves its LEFT edge instead —
    // right_line pins the right — which un-squares the very block this is
    // meant to square.
    const auto bottom_rule = [&] {
        std::string s = "\xe2\x95\xb0";                       // ╰
        for (int i = 0; i < block_w - 2; ++i) s += "\xe2\x94\x80";
        s += "\xe2\x95\xaf";                                 // ╯
        return right_line(Element{TextElement{
            .content = std::move(s),
            .style   = Style{}.with_fg(th.origin),
            .wrap    = TextWrap::NoWrap}});
    };
    // The TOP rule has to reach the chevron, which sits INSIDE the two-column
    // gap right_line() reserves — so it cannot use right_line and is built
    // flush-right itself. That corner under the ▴ is the whole point: it is
    // what makes the list read as hanging FROM the control rather than merely
    // sitting near it.
    const auto top_rule = [&] {
        std::string s = "\xe2\x95\xad";                       // ╭
        for (int i = 0; i < block_w - 1; ++i) s += "\xe2\x94\x80";
        s += "\xe2\x94\xb4";                                 // ┴ under the ▴
        std::vector<Element> cells;
        cells.push_back(dsl::spacer());
        cells.push_back(Element{TextElement{
            .content = std::move(s),
            .style   = Style{}.with_fg(th.origin),
            .wrap    = TextWrap::NoWrap}} | dsl::shrink(1.0f));
        // ONE trailing column, not right_line's two: the chevron occupies the
        // first of that pair, so the corner has to land one column further
        // right than an option line's edge.
        cells.push_back(Element{TextElement{.content = " "}});
        return maya::detail::hstack().width(Dimension::percent(100))
                   (std::move(cells));
    };

    out.push_back(top_rule());

    for (int i = first; i < first + view && i < total; ++i) {
        const bool sel = (i == hi);
        const bool cur = (i == m.current);
        const auto k   = static_cast<std::size_t>(i);

        std::string s;
        std::vector<StyledRun> runs;
        const auto put = [&](std::string_view t, Style st) {
            if (t.empty()) return;
            runs.push_back({s.size(), t.size(), st});
            s += t;
        };

        // The cursor chevron sits OUTSIDE the radio column so the radios line
        // up whether or not a row is focused. Without the placeholder the
        // whole list shifted by two columns as the cursor moved, which reads
        // as the options twitching rather than the cursor moving.
        put(sel ? "\xe2\x9d\xaf " : "  ", Style{}.with_fg(th.cursor));
        put(cur ? "\xe2\x97\x89 " : "\xe2\x97\x8b ",
            Style{}.with_fg(cur ? th.on : th.off));
        put(m.options[k], sel ? Style{}.with_fg(th.title).with_bold()
                              : Style{}.with_fg(cur ? th.value : th.label));
        // Pad to the common width so the block's right edge is straight and
        // the highlight below covers a rectangle, not a ragged silhouette.
        const int pad = widest - string_width(m.options[k]);
        if (pad > 0) put(std::string(static_cast<std::size_t>(pad), ' '), Style{});

        // The focused option carries the same wash the cursor ROW does, so the
        // one thing Enter will choose is unmistakable. Runs must cover the
        // content in order and cannot overlap, so the wash is folded into each
        // run rather than laid over the top.
        if (sel)
            for (auto& r : runs) r.style = r.style.with_bg(th.row_bg);

        out.push_back(right_line(Element{TextElement{
            .content = std::move(s),
            .wrap    = TextWrap::TruncateEnd,
            .runs    = std::move(runs)}}));
    }

    out.push_back(bottom_rule());

    // The focused option's blurb, once, under the list — rather than a hint on
    // every row. Same discipline as Row::help: describe what has focus.
    //
    // `hints` may be shorter than `options`, or absent: a caller that supplies
    // three options and one hint gets a hint on the first and none on the rest.
    if (hi >= 0 && hi < static_cast<int>(m.hints.size())
        && !m.hints[static_cast<std::size_t>(hi)].empty())
        out.push_back(right_line(Element{TextElement{
            .content = m.hints[static_cast<std::size_t>(hi)],
            .style   = Style{}.with_fg(th.help).with_italic()}}));

    // Only when the list is actually windowed — a truncated list that says
    // nothing reads as the whole list.
    if (total > view)
        out.push_back(right_line(Element{TextElement{
            .content = std::to_string(hi + 1) + "/" + std::to_string(total),
            .style   = Style{}.with_fg(th.origin)}}));

    return out;
}

// ============================================================================
//  Body + frame
// ============================================================================

// ============================================================================
//  Measure -> window -> render
// ============================================================================

// A row's painted height, decided WITHOUT painting it. Mirrors render_item's
// own control flow; the two are pinned together by a test.
int Panel::item_lines(const Item& r, int index, bool on_row) const {
    if (r.is_header()) return 1;
    // A control that paints a picture states its own height. Everything
    // else is one line, which is what every text kind is.
    int n = panel::control_rows(r.control, item_ctx(index));
    if (on_row && !r.help.empty()) ++n;
    // The error wraps, so its height is whatever the width makes it — not
    // the 1 this assumed. Measure it the way render_item will lay it out
    // (same prefix, same body width), because a row measured at one height
    // and painted at another is this family's scroll-accounting bug.
    if (!r.error.empty()) n += error_lines(r.error);
    return n;
}

int Panel::error_lines(const std::string& err) const {
    const int w = Config::content_width(surface_cols());
    if (w <= 0) return 1;
    const auto lines = word_wrap("      \xe2\x9a\xa0 " + err, w);
    return std::max<int>(1, static_cast<int>(lines.size()));
}

int Panel::menu_lines(const Menu& m) {
    const int total = static_cast<int>(m.options.size());
    if (total == 0) return 1;                         // "(no options)"
    const int hi   = std::clamp(m.highlighted, 0, total - 1);
    const int view = std::max(1, std::min(m.viewport, total));
    int n = view + 2;                                 // + the block's two rules
    if (hi < static_cast<int>(m.hints.size())
        && !m.hints[static_cast<std::size_t>(hi)].empty()) ++n;
    if (total > view) ++n;
    return n;
}

Panel::Body Panel::measure_body() const {
    Body b;

    // Pre-built rows: the caller owns its own rendering, and an item may be a
    // MULTI-ROW Element — the todo picker pushes a single PlanView covering
    // every task, the tool viewers push pre-windowed multi-line slices.
    //
    // So measure them for real. Assuming one line each collapsed the viewport
    // to items.size() rows (a one-Element body got a one-row viewport: the
    // "output not visible" bug) and put the auto-scroll clamp in index space,
    // where item 3 of four 3-row items sits at row 3 rather than row 9 — so
    // selecting it scrolled to 0 and left it off-screen entirely.
    //
    // Heights are capped: the sum only feeds a viewport min() and the scroll
    // clamp, so once past the cap the answers cannot change. Without that a
    // 5000-line body would pay a layout pass per line per frame.
    //
    // Items are NOT windowed — virtualisation is for `rows`, where the panel
    // builds each row and knows its height exactly. Callers with a huge
    // `items` list already hand the panel only the visible slice (the
    // code-block and tool-output pickers set scroll = nullptr and window
    // themselves), which is the same O(viewport) result by a different owner.
    // A FLOWED body is one Element covering every item, so it measures and
    // scrolls exactly like a prebuilt one: whole, opaque, clipped by the
    // viewport. That is the entire reason flow returns a SINGLE Element — a
    // second scroll topology would have to be kept in agreement with this
    // one forever, and "agrees today" is not a property that survives.
    if (const auto flowed = flowed_body()) {
        // Measure at the width it will be PAINTED at, not at 1<<14.
        //
        // An unbounded probe is right for content whose height does not
        // depend on its width, and wrong for everything else. columns()
        // CHOOSES ITS COLUMN COUNT from the width it is handed, so an
        // unbounded measure asks it the layout question with the wrong
        // input: the renderer clamps the probe to a no-context fallback of
        // 80 columns, columns() answers "one column" and reports that
        // height, and paint then lays out two columns at the real width.
        // The viewport is sized for a body twice as tall as the one drawn,
        // which is the dead strip below the content AND the rows running
        // under the scrollbar — one disagreement, two symptoms.
        //
        // content_width() is the same derivation build() uses for the body,
        // so measure and paint now ask the identical question.
        const int body_w = std::max(1, Config::content_width(
                                           panel::detail::terminal_cols() > 0
                                               ? panel::detail::terminal_cols()
                                               : cfg_.min_width));
        const int h = std::max(1, measure_element(*flowed, body_w).height.value);
        b.offsets     = {0, h};
        b.total       = h;
        b.opaque      = true;
        b.cursor_line = -1;        // a document has no cursor to keep in view
        b.cursor_span = 0;
        return b;
    }

    if (cfg_.items.empty()) {
        const int n = static_cast<int>(cfg_.prebuilt.size());
        b.offsets.reserve(static_cast<std::size_t>(n) + 1);
        const int cap = std::max(1, cfg_.viewport_h) * 4 + 64;
        int at = 0;
        for (int i = 0; i < n; ++i) {
            if (at <= cap)
                at += std::max(1, measure_element(
                          cfg_.prebuilt[static_cast<std::size_t>(i)],
                          1 << 14).height.value);
            else
                { at += 1; b.capped = true; }   // past the cap: a floor, not the truth
            b.offsets.push_back(at);
        }
        b.total  = at;
        b.opaque = true;            // rendered whole; the viewport clips
        // An EMPTY body has no cursor line, and neither does an unselected
        // one. -1 means "nothing to keep in view": clamping to 0 instead would
        // point at a row that may not exist and pin the view to the top.
        if (n == 0 || cfg_.selected < 0) {
            b.cursor_line = -1; b.selected = -1; return b;
        }
        const int sel = std::clamp(cfg_.selected, 0, n - 1);
        b.selected    = sel;
        // offsets[i] is where item i BEGINS; offsets[i+1] where it ends.
        b.cursor_line = b.offsets[static_cast<std::size_t>(sel)];
        b.cursor_span = std::max(1, b.offsets[static_cast<std::size_t>(sel) + 1]
                                  - b.cursor_line);
        return b;
    }

    // A selection outside the row set is a caller bug, but it must not paint a
    // cursor on an arbitrary row: out of range means NO row is focused, which
    // is also what `selected < 0` deliberately expresses (a picker with an
    // empty match set still draws its frame and its "no matches" line).
    const int n   = static_cast<int>(cfg_.items.size());
    const int sel = (cfg_.selected >= 0 && cfg_.selected < n) ? cfg_.selected : -1;

    // "Nowhere" is a real answer, and it is NOT line 0. When nothing holds the
    // cursor — no selection, an out-of-range one, or a `selected` pointing at
    // a header, which can never take focus — the scroll must simply leave the
    // view where the user put it. Defaulting cursor_line to 0 made the clamp
    // below dutifully "keep line 0 in view" and yanked a scrolled list back to
    // the top: navigating onto a section header snapped the palette to row one
    // and lost the user's place.
    b.cursor_line = -1;

    b.offsets.reserve(static_cast<std::size_t>(n) + 1);
    int at = 0;
    for (int i = 0; i < n; ++i) {
        const auto& src = cfg_.items[static_cast<std::size_t>(i)];
        // `Config::selected` is the ONE owner of where the cursor is. A row's
        // own `selected` flag is derived from it, never read — having both be
        // inputs meant a caller could set them inconsistently and the widget
        // would silently believe one and paint the other. A header can never
        // be the cursor: it carries no action, so landing on it is a dead
        // keypress the widget refuses to render as focus.
        const bool on_row = (i == sel) && !src.is_header();
        if (on_row) { b.cursor_line = at; b.selected = i; }

        int h = item_lines(src, i, on_row);
        // An open enum list expands directly beneath its own row, so it reads
        // as belonging to that row and needs no geometry at all. Guarded on
        // the row actually BEING a choice: a menu pinned to a toggle row would
        // render an option list under a control that cannot use it.
        if (cfg_.menu && cfg_.menu_row == i
            && std::holds_alternative<panel::Choice>(src.control))
            h += menu_lines(*cfg_.menu);

        at += h;
        b.offsets.push_back(at);

        // Everything this row contributes — label, help, error, open menu — so
        // the scroll can keep the whole group visible rather than just its
        // first line. Scrolling to the row alone left a field's help text one
        // row past the bottom edge.
        if (on_row) b.cursor_span = std::max(1, h);
    }
    b.total = at;
    return b;
}

std::pair<int, int> Panel::visible_rows(const Body& b, int y, int vh) {
    const int n = static_cast<int>(b.offsets.size()) - 1;
    if (n <= 0) return {0, 0};

    const int top = std::max(0, y);
    const int bot = top + std::max(1, vh);

    // offsets is sorted, so the window is two binary searches: the first row
    // whose END is past the top edge, and the first row whose START is at or
    // past the bottom edge. A row STRADDLING an edge must be rendered in full
    // (the scroll offset then clips it), which is why this bounds on ends for
    // `first` and on starts for `last`.
    const auto beg = b.offsets.begin();
    const int first = static_cast<int>(
        std::upper_bound(beg + 1, b.offsets.end(), top) - (beg + 1));
    const int last = static_cast<int>(
        std::lower_bound(beg, b.offsets.end(), bot) - beg);

    return {std::clamp(first, 0, n), std::clamp(last, first, n)};
}

std::vector<Element> Panel::render_range(const Body& b, int first,
                                         int last) const {
    std::vector<Element> lines;
    if (last <= first) return lines;

    lines.reserve(static_cast<std::size_t>(
        b.offsets[static_cast<std::size_t>(last)]
      - b.offsets[static_cast<std::size_t>(first)]));

    // Flowed: one entry, one Element, already carrying every item. Built
    // here rather than cached on Body so that measure and render go through
    // the SAME function — the two disagreeing about a body's height is the
    // bug class this panel has had once already.
    if (const auto flowed = flowed_body()) {
        lines.push_back(*flowed);
        return lines;
    }

    if (cfg_.items.empty()) {
        for (int i = first; i < last; ++i)
            lines.push_back(cfg_.prebuilt[static_cast<std::size_t>(i)]);
        return lines;
    }

    for (int i = first; i < last; ++i) {
        Item row = cfg_.items[static_cast<std::size_t>(i)];
        row.selected = (i == b.selected);
        for (auto& line : render_item(row, i)) lines.push_back(std::move(line));

        if (cfg_.menu && cfg_.menu_row == i
            && std::holds_alternative<panel::Choice>(row.control))
            for (auto& line : render_menu(*cfg_.menu))
                lines.push_back(std::move(line));
    }
    return lines;
}

// ============================================================================
//  Column flow
// ============================================================================

std::optional<Element> Panel::flowed_body() const {
    if (cfg_.col_max_width <= 0) return std::nullopt;   // off: the default

    // Refuse rather than assert. Flow makes item k stop being on line k, so
    // anything that reads a per-item line position has to be excluded: a
    // cursor to keep in view, a dropdown anchored to its row. A caller that
    // sets col_max_width on a picker gets the ordinary single column, which
    // is correct-but-unflowed rather than subtly mis-scrolled.
    if (cfg_.selected >= 0)     return std::nullopt;
    if (cfg_.menu)              return std::nullopt;
    if (cfg_.items.empty())     return std::nullopt;

    const int n = static_cast<int>(cfg_.items.size());

    // Group into sections on HEADER boundaries, and hand columns() one cell
    // per section rather than one per item. A section is the unit that must
    // not be broken: a heading stranded at the foot of a column with its
    // rows at the head of the next is worse than an uneven split, and it is
    // the only structural boundary the panel has.
    //
    // The column width, computed BEFORE the sections so they can be built
    // to it. See the note on `columns(...width)` below for the derivation.
    const int frame = panel::detail::terminal_cols() > 0
                          ? panel::detail::terminal_cols()
                          : cfg_.min_width;
    const int bound = std::max(1, Config::content_width(frame)
                                      - Config::kScrollbarCols);

    // A section is a stack that STRETCHES to its column, never one that
    // measures itself — and never one pinned to a number either.
    //
    // A plain vstack takes the width of its widest child, and one of those
    // children is deliberately narrow: a section header reports only its
    // LABEL width so that a vertical list has no horizontal extent (a
    // component that asked for its full rule inside a scroll viewport is
    // how this family once published 2^24 as its width). That is right for
    // a flex parent which stretches its children — and wrong here, because
    // columns() sizes a column from its cells: the header's fourteen
    // columns become the section's width, every row is squeezed into it,
    // and a squeezed row loses its tail, which is the VALUE.
    //
    // grow(1) is the fix rather than a fixed width. A pin says "I am
    // exactly N" and cannot answer what happens when the column turns out
    // to be N-1; grow says "take whatever the column has", which is the
    // only true statement available here and needs no number kept in step
    // with the one columns() computes.
    auto section = [](std::vector<Element> rows) {
        auto vb = dsl::vstack();
        vb.grow(1.0f);
        return vb(std::move(rows));
    };

    // The banner, if any: rendered full width ABOVE the flow rather than
    // handed to columns() as a cell. See Config::lead_items — a headline
    // is the cheapest cell to move when balancing by height, so left in
    // the flow it reliably lands where nobody looks for it.
    std::vector<Element> lead;
    const int lead_n = std::clamp(cfg_.lead_items, 0, n);
    for (int i = 0; i < lead_n; ++i)
        for (auto& line : render_item(cfg_.items[static_cast<std::size_t>(i)], i))
            lead.push_back(std::move(line));

    std::vector<Element> cells;
    std::vector<Element> cur;
    for (int i = lead_n; i < n; ++i) {
        const auto& it = cfg_.items[static_cast<std::size_t>(i)];
        if (it.is_header() && !cur.empty()) {
            cells.push_back(section(std::move(cur)));
            cur.clear();
        }
        for (auto& line : render_item(it, i)) cur.push_back(std::move(line));
    }
    if (!cur.empty()) cells.push_back(section(std::move(cur)));
    if (cells.empty() && lead.empty()) return std::nullopt;

    // Everything geometric belongs to columns(): the count the ceiling
    // implies, dividing the slot exactly, balancing by measured height. The
    // panel contributes the ceiling, the gap, and a BOUND on the width.
    //
    // The bound exists because of ONE fact adapt() cannot see. build() puts
    // this body and the scrollbar gutter side by side in an h(), and a
    // component is measured BEFORE flex subtracts its sibling — so the body
    // is offered a slot one column wider than it will be painted in. Its
    // last column then sizes to the frame edge and the row's tail, which is
    // the VALUE, lands under the scrollbar and is gone.
    //
    // It is the FULL chrome, not just the gutter. The offer this component
    // receives is the frame width, not the padded interior — measured
    // before the border, the padding and the gutter have each taken their
    // share — so all three have to come off. Reserving only the gutter
    // still left the body six columns too wide at 40 columns: the label
    // stopped shrinking under pressure and the value fell off the end.
    //
    // And it is a MINIMUM against the offer rather than a replacement for
    // it: a bound can only ever take back width the body does not have,
    // never pin it narrower than the width it does. Overriding instead was
    // what left a constant strip of dead space down the right edge.
    //
    // ONE SECTION still goes through columns(): it cannot split, but the
    // ceiling must still decide.
    //
    // The gutter comes off TWICE on purpose, and it is not a mistake being
    // papered over. content_width() removes it once as part of the frame's
    // chrome — that is the body's share of the panel. But build() then puts
    // this element and the scrollbar side by side in an h(), and a
    // component is measured BEFORE flex subtracts its sibling, so the slot
    // this body is offered still has the gutter's column in it. Laid out
    // for 33 and handed 32, the row loses exactly one cell off its tail:
    // the value "40ms" painted as "40m".
    //
    // One column, one character, and nothing else in the frame looks wrong
    // — which is why this survived several rounds of looking at it.
    //
    // max_width on the result is the other half. The bound above governs
    // what columns() lays out INSIDE itself; it does not change what the
    // component reports UPWARD, and flex sizes a child from that report.
    // The body kept answering 40 — the frame width adapt() was offered —
    // inside a 34-column slot that also had to hold a 2-column gutter, so
    // the solver saw hypo=42 against avail=34 and took the 8-column
    // difference off the end of every row. Capping the element makes the
    // report and the layout agree, which is the invariant this whole
    // feature rests on.
    const bool cells_empty = cells.empty();
    auto flowed = columns(std::move(cells),
                          ColumnsOpts{.max_width = cfg_.col_max_width,
                                      .min_width = cfg_.col_min_width,
                                      .gap       = cfg_.col_gap,
                                      .width     = bound})
                      .build();
    BoxElement cap;
    cap.layout.direction = FlexDirection::Column;
    cap.layout.max_width = Dimension::fixed(bound);
    // The banner first, then the columns. Both are children of the same
    // vertical box, so the banner gets the FULL width while everything
    // after it flows — which is the whole point of holding it out.
    for (auto& l : lead) cap.children.push_back(std::move(l));
    if (!cells_empty) cap.children.push_back(std::move(flowed));
    return Element{std::move(cap)};
}

// A box with no children and a fixed height. `height` alone is not enough:
// the parent vstack's default align_items is Stretch, and a zero-child box
// with no basis can still be collapsed by shrink — which would silently give
// back the very rows this exists to reserve.
//
// min_height rather than basis does the reserving, and the distinction is
// load-bearing. `basis` is the MAIN-AXIS size, and which axis that is
// depends on the parent: in a Column it means height (what this wants), but
// in a Row it means WIDTH. build() puts one of these in a row as the
// scrollbar gutter — `spacer_rows(vh) | width(1)` — where basis=vh made a
// one-column gutter report vh columns wide. The solver then charged the row
// for a 4-column gutter beside a 32-column body in a 34-column slot, and
// since the gutter is shrink(0) the whole overflow came off the body: two
// columns for every extra viewport row, taken from the end of every line.
//
// min_height says the same thing in an axis that cannot be reinterpreted.
Element Panel::spacer_rows(int n) {
    BoxElement box;
    box.layout.height     = Dimension::fixed(n);
    box.layout.min_height = Dimension::fixed(n);
    box.layout.shrink     = 0.0f;
    return Element{std::move(box)};
}

// ============================================================================
//  Frame
// ============================================================================

Element Panel::build() const {
    using namespace dsl;

    std::vector<Element> stack;

    if (!cfg_.subtitle.empty()) {
        // A STATUS line, set in the same plain text as the rows it summarises.
        // It was dim italic — this family's styling for help prose — which
        // made a factual line look like a hint.
        //
        // TruncateEnd, not NoWrap. Both refuse to wrap; the difference is
        // what happens to the part that does not fit. NoWrap lets the clip
        // take it, so on a narrow terminal the line simply STOPS — "how
        // much of the prefix was served from cache" became "how much of the
        // prefix was serv", with nothing to say a word had been cut. An
        // ellipsis is the difference between a sentence the reader knows is
        // abbreviated and one they have no reason to doubt.
        //
        // Found by Canvas::on_clip_overflow, which reported nine of these
        // at 40 columns that the golden hash had been happily pinning.
        stack.push_back(Element{TextElement{
            .content = "  " + cfg_.subtitle,
            .style   = Style{}.with_fg(cfg_.theme.label),
            .wrap    = TextWrap::TruncateEnd}});
        stack.push_back(Element{TextElement{}});
    }
    // The pane's error, under the subtitle and WRAPPED — see Config::error
    // for why this is not just another subtitle.
    if (!cfg_.error.empty()) {
        stack.push_back(Element{TextElement{
            .content = "  \xe2\x9a\xa0 " + cfg_.error,
            .style   = Style{}.with_fg(cfg_.theme.error),
            .wrap    = TextWrap::Wrap}});
        stack.push_back(Element{TextElement{}});
    }
    // The tab strip sits between the subtitle and the header: the subtitle
    // describes the PANEL, the strip selects a view of it, and the header
    // belongs to whichever view is selected. Reading order matches that
    // nesting.
    //
    // Delegated to maya::TabStrip rather than drawn here — it is the same
    // strip the diff reviewer's file rail needs, and the scrolling it does
    // (keep the active tab on screen, collapse what it passed into a "…")
    // is the part that must not be reimplemented per host.
    if (!cfg_.tabs.empty()) {
        TabStrip strip;
        for (const auto& t : cfg_.tabs) strip.tab(t);
        strip.active(cfg_.tab_active);
        if (cfg_.tab_mark) strip.marker(*cfg_.tab_mark);
        // The ACTIVE tab wears the accent, not the plain label colour. With
        // the accent only in the underline, a strip read as ordinary bold
        // text with a stray rule under it — and with a single tab there was
        // nothing to contrast against at all, so it looked unstyled.
        //
        // With `tab_fill` the accent moves to the BACKGROUND and the label
        // is painted black on it — the same treatment the status bar's
        // model badge uses, and the one mark that still reads on a strip
        // of a single tab. Black rather than theme.label because an accent
        // is chosen to carry white panel text at ARM's length; with the
        // text sitting directly on it, only the dark side has contrast.
        if (cfg_.tab_fill) {
            strip.theme.active_bg = cfg_.accent;
            // inverse_text only makes sense against a canvas the theme owns.
            // Under native it is Default — the same colour as ordinary text
            // — because there is no second canvas to invert against, so a
            // filled tab would paint normal foreground on an ANSI-magenta
            // block. Fall back to the terminal's background as the ink,
            // which is the actual inverse of its own foreground.
            const Theme& th = theme::live();
            const bool owns_canvas =
                th.resolve(Color::slot(ThemeSlot::Background)).kind()
                    != ColorKind::Default;
            strip.theme.active = owns_canvas
                ? Color::slot(ThemeSlot::InverseText)
                : Color::slot(ThemeSlot::Background);
        } else {
            strip.theme.active = cfg_.accent;
        }
        strip.theme.idle   = cfg_.theme.help;
        strip.theme.accent = cfg_.accent;
        stack.push_back(strip.build());
        // A rule under the strip. The tabs are chrome and what follows is
        // content, and without a boundary the first content row reads as a
        // fourth tab that happens to be on its own line — which is exactly
        // what a headingless section does look like. Dim, and full width:
        // the strip's own marks carry the selection, so this line's only
        // job is to say "the chrome ends here".
        stack.push_back(component([help = cfg_.theme.help](int w, int) {
            constexpr int kMaxWidth = 4096;   // measure-pass sentinel guard
            int n = w > kMaxWidth ? kMaxWidth : (w < 0 ? 0 : w);
            // Pay the scrollbar gutter the BODY pays.
            //
            // This rule sits outside the scrollable, so it is handed the
            // full content width while every row beneath it is one column
            // narrower — the gutter is reserved unconditionally now. Drawn
            // at `w` the rule runs one column long, and since a Canvas
            // clips rather than spills it lands on the frame's own right
            // border and erases it. The reported symptom ("content
            // overflows the frame") is this single row on the narrowest
            // panes, and it is the last one left after the width clamp.
            n -= Config::kScrollbarCols;
            if (n < 0) n = 0;
            std::string rule;
            for (int i = 0; i < n; ++i) rule += "\xe2\x94\x80";   // ─
            return Element{TextElement{.content = std::move(rule),
                                       .style   = Style{}.with_fg(help),
                                       .wrap    = TextWrap::TruncateEnd}};
        }).build());
        stack.push_back(Element{TextElement{}});
    }

    for (const auto& h_row : cfg_.header) stack.push_back(h_row);

    const Body body = measure_body();
    const int content = body.total;
    // Shrink-wrap to the content, UNLESS the caller asked for a fixed body.
    //
    // min() is right for a picker: a list of three matches should be three
    // rows tall, not a fourteen-row box with eleven empty ones under it.
    // The panel is sized to what it holds and the frame follows.
    //
    // It is wrong for a DOCUMENT with tabs, and visibly so. The stats
    // viewer's tabs differ in length, so min() made the frame resize every
    // time the reader switched view — measured at 13 rows on one tab and 30
    // on another, the whole box jumping as they pressed tab. A reader
    // navigating a document is entitled to have the document stay still;
    // the thing that changes should be what is written in it.
    //
    // fixed_viewport says which of the two this panel is. Default false
    // keeps every picker exactly as it was.
    const int vh = cfg_.fixed_viewport
                       ? std::max(1, cfg_.viewport_h)
                       : std::max(1, std::min(cfg_.viewport_h, content));

    // Keep the cursor's WHOLE span in view. ONE owner: only the panel knows
    // both the painted row count and the viewport.
    if (cfg_.scroll) {
        auto& s = *cfg_.scroll;

        // The scrollable extent is knowable HERE, and the renderer must not be
        // the one to discover it. `scrollbar_y` sizes its thumb from s.max_y,
        // but the renderer only writes max_y back AFTER layout — so a bar
        // built before that writeback is drawn from the PREVIOUS frame's
        // content height. Whenever the height changes between frames (the
        // focused row's help line appearing as the cursor moves onto it), the
        // first frame painted a thumb for the old height, the writeback then
        // noticed the disagreement and dirtied the scroll state, and the run
        // loop rendered the whole overlay a second time to correct it.
        //
        // That cost a doubled input->photon on ~40% of arrow presses, and on
        // some of them the correction was VISIBLE: the user saw one frame of
        // wrong thumb before it snapped. Measuring already told us the answer,
        // so publish it and let the writeback agree instead of arbitrate.
        //
        // Only when the measurement is COMPLETE: an items body past the cap
        // stopped measuring, so its total is a floor rather than the truth.
        // There the renderer knows better — leave max_y to the writeback
        // rather than publish a number that is wrong.
        if (!body.capped) s.max_y = std::max(0, content - vh);

        if (content <= vh) {
            s.y = 0;                        // fits: never paint-shifted
        } else if (body.cursor_line < 0) {
            // Nothing holds the cursor, so there is nothing to keep in view.
            // Leave the offset alone (the user may have scrolled here by
            // wheel) and only re-clamp it against the current content.
            s.y = std::clamp(s.y, 0, content - vh);
        } else {
            // Clamp the span to what actually exists before using it. A row
            // whose help + error + open menu ran past the end would otherwise
            // scroll to an offset with nothing under it.
            const int line = std::clamp(body.cursor_line, 0, content - 1);
            const int span = std::clamp(body.cursor_span, 1, content - line);
            const int end  = line + span;
            if (line < s.y)          s.y = line;
            else if (end > s.y + vh) s.y = end - vh;
            // A capped measurement has no trustworthy upper bound of its own;
            // fall back to the one the renderer wrote back last frame.
            s.y = body.capped ? std::clamp(s.y, 0, std::max(s.max_y, 0))
                              : std::clamp(s.y, 0, content - vh);
        }
    }

    if (cfg_.scroll) {
        auto& s = *cfg_.scroll;
        // The scroll offset must be resolved BEFORE windowing — it is what
        // decides which rows are on screen — but the viewport box still gets
        // the same s.y it always did, because the spacers put the rendered
        // rows back at their true absolute positions.
        const int n_entries = static_cast<int>(body.offsets.size()) - 1;
        const auto [first, last] = body.opaque
            ? std::pair{0, n_entries}          // opaque: never windowed
            : visible_rows(body, s.y, vh);

        std::vector<Element> lines;
        const int above = body.offsets[static_cast<std::size_t>(first)];
        const int below = content - body.offsets[static_cast<std::size_t>(last)];
        if (!body.opaque && above > 0) lines.push_back(spacer_rows(above));
        for (auto& line : render_range(body, first, last))
            lines.push_back(std::move(line));
        if (!body.opaque && below > 0) lines.push_back(spacer_rows(below));

        Element scrollable = vstack()(lines) | scroll(s, vh) | grow(1.0f);
        // The scrollbar's column is reserved WHETHER OR NOT a bar is drawn.
        //
        // It used to be appended only when `content > vh`, which made the
        // body one column wider in the fits case than in the overflows
        // case. Two costs, both real:
        //
        //   • The body reflows the instant content crosses the viewport —
        //     a layout that shifts under the reader with no visible cause.
        //
        //   • It is circular, and that is why callers could not compensate
        //     for it correctly. Whether the bar exists depends on `vh`;
        //     `vh` depends on the width; the width is what the bar's
        //     presence would decide. agentty's stats panel hand-counted a
        //     reserve for exactly this and got a number that is right at
        //     some widths and wrong at others — not because the count was
        //     careless, but because no single count can be right.
        //
        // A one-column gutter costs one column. Reflowing costs the
        // reader's place on the page.
        Element gutter = content > vh
            ? scrollbar_y(s, vh, cfg_.scrollbar_style)
            // Same width, no ink. A full-height thumb beside a list that
            // FITS reads as a stray ┃ hugging the border and communicates
            // nothing — there is no hidden content to indicate.
            : (spacer_rows(vh) | width(1));
        stack.push_back(h(std::move(scrollable), std::move(gutter)).build());
    } else {
        // No scroll state: nothing can be off-screen, so nothing is skipped.
        for (auto& line : render_range(body, 0,
                                       static_cast<int>(body.offsets.size()) - 1))
            stack.push_back(std::move(line));
    }

    // Fixed chrome below the body — outside the scroll region, so a validation
    // note never scrolls away from the row that needs it.
    //
    // MODE-AWARE: while a field is being edited (live caret) or a dropdown is
    // open, the note line becomes the mode's key hint instead. This is the
    // "where did my arrow keys go" fix at the chrome level — a mode you can
    // see is a mode you can leave. Derived from the SAME state that renders
    // the caret/menu, so the hint and the mode cannot disagree; hosts write
    // only the idle note and get the mode line for free.
    const bool menu_open = cfg_.menu.has_value();
    const bool editing = !menu_open && [&] {
        if (cfg_.selected < 0
            || cfg_.selected >= static_cast<int>(cfg_.items.size()))
            return false;
        return panel::is_editing(
            cfg_.items[static_cast<std::size_t>(cfg_.selected)].control);
    }();
    std::string note = cfg_.note;
    Style note_style = Style{}.with_fg(cfg_.theme.help);
    if (menu_open) {
        note = "\xe2\x86\x91\xe2\x86\x93 choose \xc2\xb7 \xe2\x86\xb5 select \xc2\xb7 esc cancel";
    } else if (editing) {
        // value_edit, not help: the hint shares the caret's hue, visually
        // pairing "this row is live" with "these keys end it".
        note = cfg_.editing_note.empty()
                   ? "editing \xc2\xb7 \xe2\x86\xb5 done \xc2\xb7 \xe2\x86\x91\xe2\x86\x93 next field"
                   : cfg_.editing_note;
        note_style = Style{}.with_fg(cfg_.theme.value_edit);
    }
    if (!note.empty()) {
        stack.push_back(Element{TextElement{}});
        stack.push_back(Element{TextElement{
            .content = "  " + note,
            .style   = note_style,
            // TruncateEnd, not NoWrap.
            //
            // The note is a caller-supplied key hint ("tab switch view · ↑↓
            // scroll · esc close") sized for a comfortable terminal, and
            // NoWrap on a string the widget does not control is a promise
            // it cannot keep. A Canvas clips rather than spills, so on a
            // phone-sized pane the overrun did not run off the screen — it
            // ran over the frame's own right border and erased it, leaving
            // one row of the panel with no edge.
            //
            // Wrapping is wrong here (a hint that reflows to two rows
            // shifts everything below it), but truncating is not: an
            // ellipsised hint still reads, and the frame stays a frame.
            .wrap    = TextWrap::TruncateEnd}});
    }
    for (const auto& f_row : cfg_.footer) stack.push_back(f_row);

    return maya::detail::vstack()
        .padding(1, 2)
        // The panel sits ON the canvas, so it carries the canvas colour.
        //
        // Without this the frame box states no background at all, and every
        // cell it owns — the border glyphs, the padding ring, the gaps
        // between rows — falls back to the TERMINAL's colour. On a themed
        // canvas that reads as a rectangular hole punched through the frame:
        // the chrome above and below the panel is painted, the panel is not.
        //
        // Background, not Surface: a picker is not a raised card, it is the
        // same plane with a border drawn on it. Under `native` this resolves
        // to default_color() and nothing is stated, which is the behaviour
        // this replaces.
        .bg(maya::theme::live().background)
        // A min-width below a usable floor makes the panel narrower than its
        // own title, and the border text then overflows the frame.
        //
        // And a min-width ABOVE the terminal is not a minimum at all — it is
        // an overflow. The panel is centred, so the excess is split off both
        // edges: the frame's right border is clipped away and every row
        // inside it loses its own edge. That is what a "broken frame" looks
        // like on a phone-sized pane, and a probe across agentty's panels
        // found 13 of 14 doing it below 60 columns.
        //
        // This used to be the CALLER's job, stated in prose ("the caller
        // clamps it down anyway") and honoured by exactly one of twelve —
        // form_common.cpp caps at max_width - 6; the other eleven assign
        // kPanelStandard raw and overflow the moment the terminal is
        // narrower than the floor they picked. An obligation delegated to
        // every caller is an obligation nobody owns.
        //
        // The widget knows the terminal and knows its own chrome, so it
        // clamps itself. A floor that cannot be honoured is not a floor.
        .min_width(Dimension::fixed(clamped_min_width_for(cfg_.min_width)))
        .border(BorderStyle::Round)
        .border_color(cfg_.accent)
        .border_text(cfg_.title, BorderTextPos::Top, BorderTextAlign::Center)
        (std::vector<Element>{v(std::move(stack)).build()});
}

} // namespace maya
