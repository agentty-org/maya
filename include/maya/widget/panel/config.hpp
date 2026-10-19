#pragma once
// maya::panel::Config — everything a host supplies to build one panel.
//
// Pure data; the widget (widget.hpp) turns it into an Element. Hosts fill a
// Config and never touch layout — the widget owns every chrome decision:
// border, viewport clipping, scrollbar glyphs, keep-selection-in-view.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "../../element/element.hpp"
#include "../../style/color.hpp"
#include "../scrollbar.hpp"
#include "item.hpp"
#include "menu.hpp"
#include "theme.hpp"

namespace maya {
// Forward-declared rather than pulling in tab_strip.hpp: Config is pure data
// and this is the one field that names the strip's style. The enum has a
// fixed underlying type, so a declaration is enough to hold one by value.
enum class TabMark : std::uint8_t;
}  // namespace maya

namespace maya::panel {

struct Config {
    std::string title;        // centred on the top border
    std::string subtitle;     // status line above the body

    // A failure the whole pane is in, shown under the subtitle.
    //
    // Separate from `subtitle` because the two have different contracts.
    // A subtitle is a one-line situation report and TRUNCATES: cutting
    // "connected \xc2\xb7 14 tools" loses nothing you needed. An error WRAPS,
    // because its tail is the part that tells you what to do — "connect
    // failed: no route to 10.0.0.5:11434 (check the endpoint)" cut at the
    // colon says only that something broke. Hosts used to put errors in
    // `subtitle` and inherit the truncation (#75).
    std::string error;

    std::vector<Item> items;
    // Index into `items` (or `prebuilt`) of the cursor. <0 = no selection.
    int               selected = -1;

    // Pre-built Elements, for callers that own their own item rendering
    // (the thread list's virtualisation). Ignored when `items` is set.
    std::vector<Element> prebuilt;

    std::optional<Menu> menu;
    int                 menu_row = -1;

    std::vector<Element> header;   // above the body, never scrolls
    std::string          note;     // below the body
    // Overrides the DERIVED editing hint ("editing · ↵ done · ↑↓ next
    // field") while a field is live. For panels where Enter means
    // something else — a single-field form whose Enter SUBMITS — the
    // default would teach the wrong key. Empty = use the default.
    std::string          editing_note;
    std::vector<Element> footer;   // key hints

    // ── Tabs ─────────────────────────────────────────────────────────
    //
    // A panel that shows SEVERAL views of one subject renders a tab strip
    // between the subtitle and the header. Empty = no strip, which is every
    // existing panel: this is additive, and a host that never sets `tabs`
    // builds exactly the frame it built before.
    //
    // It lives in the widget rather than in the one host that needed it
    // first, because a tab strip is CHROME. Every chrome decision in this
    // family — border, viewport clipping, scrollbar glyphs, keeping the
    // selection in view — belongs to the widget, and hosts supply data. A
    // strip hand-rolled into `header` by each host would drift in padding,
    // in the selected-tab treatment, and in how it degrades when the panel
    // is narrower than its labels; three hosts would mean three strips that
    // look almost alike.
    //
    // `tab_active` indexes `tabs`. Out of range renders every tab inactive
    // rather than asserting: a panel is a view, and a bad index should show
    // a slightly wrong strip, not take down the frame.
    std::vector<std::string> tabs;
    int                      tab_active = 0;

    // How the active tab is marked. Defaults to TabMark::Underline (the
    // value 0), which is what a panel's tabs have always used; a host that
    // wants the denser editor treatment — " │ " dividers, no rule — sets
    // TabMark::Editor. Spelled as an optional so the default stays owned
    // by TabStrip rather than duplicated here.
    std::optional<TabMark>   tab_mark;

    // Paint the active tab as a FILLED CHIP in the panel's accent instead
    // of marking it with weight or a rule. Off by default.
    //
    // Worth its own flag rather than being folded into TabMark because it
    // is orthogonal to the mark: it answers "how loud" where the mark
    // answers "what shape". A one-tab strip is the case that needs it —
    // there is no dim neighbour to contrast against, so colour alone says
    // nothing and a lone underline reads as a stray rule.
    bool                     tab_fill = false;

    // Borrowed; must outlive the built Element. Null disables scrolling.
    ScrollState* scroll     = nullptr;
    int          viewport_h = 14;

    // The HOST clamps this to the terminal: a min-width wider than the
    // screen is not a minimum but an overflow, and the overlay centres the
    // panel so the excess is split off both edges and the labels vanish.
    int   min_width = 60;

    // ── What the frame costs, as ONE fact ────────────────────────────────
    //
    // Panel wraps its body in `.padding(1, 2).border(Round)`: one border
    // column and two pad columns on each side. Content laid out at the
    // panel's OUTER width is therefore six columns too wide, and a Canvas
    // clips rather than spills — so the excess does not paint past the
    // frame, it paints OVER the frame's own right border and stops. The
    // visible damage is a row that simply has no right edge.
    //
    // This was a number every caller had to know and none was told. The
    // one caller that noticed hand-counted it (agentty's stats panel:
    // `sheet.reserve_right(7)`, derived by measuring at three widths), and
    // a hand-counted constant describing someone else's internals goes
    // stale the moment the padding changes — silently, because nothing
    // references anything. A probe across all 14 agentty panels found 13
    // losing their border below 60 columns.
    //
    // So the widget states its own cost. content_width() below is the
    // width a caller may actually paint in, and it is derived from these
    // rather than repeated.
    static constexpr int kBorderCols  = 2;   // one each side
    static constexpr int kPaddingCols = 4;   // two each side
    // The vertical scrollbar rides in the right pad when the body
    // overflows. Charged UNCONDITIONALLY: it appears exactly when content
    // crosses the viewport, and a body whose width changes at that moment
    // is a layout that shifts under the reader for a reason they cannot
    // see. It is also the only way to break the circularity — whether the
    // bar exists depends on the viewport, which depends on the width the
    // bar's presence would determine.
    static constexpr int kScrollbarCols = 1;
    static constexpr int kChromeCols =
        kBorderCols + kPaddingCols + kScrollbarCols;

    // The width content may paint in, given the panel's OUTER width.
    // Never negative; a panel narrower than its own chrome has no body.
    [[nodiscard]] static constexpr int content_width(int outer) noexcept {
        const int inner = outer - kChromeCols;
        return inner > 0 ? inner : 0;
    }

    // ── Column flow ──────────────────────────────────────────────────────
    //
    // The widest a column of items may get before the body flows into
    // another column. 0 (the default) disables it: one column, whatever the
    // width, which is what a picker wants — splitting a ranked list puts
    // rank 1 beside rank 9 and reading order stops meaning anything.
    //
    // A DOCUMENT wants the opposite. On a 200-column terminal a single
    // column of rows is a narrow ribbon with two thirds of the screen blank
    // and the reader scrolling for content that would have fitted.
    //
    // The panel supplies only the two facts it alone owns — the body width
    // (it owns the border, the padding and the scrollbar gutter) and where
    // the section boundaries are. Everything geometric is maya::columns():
    // how many columns the ceiling implies, dividing the slot exactly so no
    // strip is left over, and balancing by measured height. That split is
    // deliberate; a host that computed its own column width had to guess
    // the body width, and the guess was wrong by exactly the amount that
    // clipped a stats value mid-string.
    int col_max_width = 0;

    // The narrowest a column may be. A ceiling alone cannot say "do not
    // split below what a row needs" — at a 64 ceiling a 76-column body
    // splits into two columns of 36 and label─→value rows begin truncating
    // their labels, which trades one unreadable layout for another. The two
    // bounds do different jobs: max is about reading, min is about fitting.
    int col_min_width = 0;

    // Blank columns between neighbouring columns.
    int col_gap = 2;

    // Draw the rule that runs from a section header to the right edge.
    //
    // On by default because a settings pane needs it: a long single column
    // of interchangeable rows has nothing else marking where one section
    // ends. A panel whose sections are already separated — by blank rows,
    // by a column split, by figures that are visibly different objects —
    // turns off, because a rule across every heading reads as a form
    // rather than as a page of statistics.
    bool header_rule = true;

    // How many leading items sit ABOVE the column split, full width.
    //
    // A banner is not a section. The column flow balances cells by height,
    // so a two-line headline is the cheapest thing to move — and it gets
    // moved, landing at the foot of the left column or the top of the
    // right, which is exactly where a reader does not look for the tab's
    // answer. Reading order in a two-column layout is a property of the
    // layout, not of the content, so the content cannot express "me
    // first" any other way.
    //
    // 0 (the default) flows everything, which is right for a body that is
    // all of one kind.
    int lead_items = 0;

    // Keep the body at `viewport_h` rows even when the content is shorter.
    //
    // The default (false) shrink-wraps: a picker showing three matches is
    // three rows tall, which is what a list of alternatives should be.
    //
    // A DOCUMENT wants the opposite, and the difference is visible. Tabs
    // whose contents differ in length make a shrink-wrapping frame resize
    // every time the reader switches view — the stats viewer measured 13
    // rows on one tab and 30 on another, the whole box jumping under the
    // cursor. A reader navigating a document is entitled to have the
    // document hold still; what changes should be what is written in it,
    // not the shape of the thing holding it.
    bool fixed_viewport = false;

    // Color, not Themed: hosts pass this at RUNTIME (agentty picks a per-
    // panel accent from its own tokens), and Themed's gate is consteval.
    // The compile-time guarantee lives where the caller's colour is
    // named, which is their own Config.
    Color accent    = Color::slot(ThemeSlot::Primary);

    // Colour of the edge bar on the ACTIVE row (the persistent "currently
    // in use" marker, distinct from the cursor). The cursor wins on
    // overlap — where you ARE outranks where you were.
    Color active_color = Color::slot(ThemeSlot::Accent);

    Theme          theme{};
    ScrollbarStyle scrollbar_style = ScrollbarStyle::neon();
};

} // namespace maya::panel

// NOTE: no compatibility accessors for the renamed members (rows→items,
// items→prebuilt) — the compiler finds every stale spelling, which is the
// point of a hard rename.
