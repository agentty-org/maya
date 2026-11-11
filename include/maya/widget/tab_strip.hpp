#pragma once
// maya::TabStrip — one horizontal strip of selectable views.
//
// The chrome a panel wears when it shows SEVERAL views of one subject: the
// stats viewer's areas, the diff reviewer's changed files. Not a general
// "row of labels" — a strip whose job is to answer "which view am I in, and
// what else is there" without ever pushing the answer off-screen.
//
// ── Why this is a widget and not a helper in each host ───────────────────
//
// The naive version is ten lines and every host can write it. The version
// that survives contact with a real changeset is not, and three properties
// are what separate them:
//
//   1. It SCROLLS. A 40-file review is wider than any pane, so the strip
//      slides to keep the active tab visible and collapses what it passed
//      into a leading ellipsis. Without this, moving to a tab can move
//      focus to something rendered entirely off-screen — the user is
//      navigating blind.
//   2. Tabs carry STATUS. A file is accepted / rejected / pending, and the
//      dot that says so is part of the tab, not a second widget beside it.
//   3. Tabs carry a TRAILING detail (a diffstat, a count). It has to
//      participate in the same width arithmetic, or the scroll maths is
//      wrong by exactly the part the host added itself.
//
// Hand-rolled per host, those three drift — and the two that drift silently
// (scrolling and width) are the two that break at exactly the size where a
// user needs the strip most.
//
// Usage:
//   TabStrip s;
//   s.tab("Smart Mode").tab("Tools");
//   s.active(0);
//   Element ui = s;
//
//   // with status + detail (the diff reviewer's shape)
//   TabStrip files;
//   files.tab("rope.cpp").dot(Color::green()).detail("+12 -3", …);
//   files.active(2);

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"
#include "../text/unicode_width.hpp"

namespace maya {

// Color, not Themed, throughout: a HOST assigns these at runtime (Panel
// pushes its own cfg_.accent in), and Themed's literal gate is consteval.
// The defaults are still slots, and the compile-time guarantee lives at
// whatever Config the host's colour was named in.
struct TabStripTheme {
    Color active   = Color::slot(ThemeSlot::Text);   // the selected label
    Color idle     = Color::slot(ThemeSlot::Muted);  // every other label
    Color accent   = Color::slot(ThemeSlot::Info);   // the underline / marker
    Color detail   = Color::slot(ThemeSlot::Muted);  // trailing count / diffstat
    Color ellipsis = Color::slot(ThemeSlot::Muted);  // the "…" scrolled-past chip
    Color divider  = Color::slot(ThemeSlot::Muted);  // │ between tabs, and the rule
    // Optional FILL behind the active label — the tab becomes a chip,
    // " label " painted in this colour with `active` as its text. Unset
    // (the default) keeps every mark background-free, so a strip still
    // drops onto any surface.
    //
    // A fill is the strongest mark available and it does not need help:
    // when it is on, the active label drops its bold and the idle labels
    // stay plain. Weight on top of a fill is emphasis applied twice, and
    // dimming the neighbours makes a two-tab strip read as one live tab
    // and one disabled one.
    std::optional<Color> active_bg;
};

// How the selected tab is marked.
//
// A FAMILY, not one house style. The three differ in what they cost and what
// they emphasise, and a host picks by what its surface can afford:
enum class TabMark : std::uint8_t {
    // A solid rule under the active label. Reads as "this one" without
    // competing with a row cursor for the same visual channel — a filled
    // block does, and in a 16-colour terminal the two are easy to confuse.
    // Costs a second row. The default, and what a panel's tabs use.
    Underline,
    // A leading ◆ on the active tab. One row total, for a strip that has to
    // share a line budget with content.
    Dot,
    // The editor style: " │ " dividers between tabs, and the active tab
    // marked on the label itself — a filled chip when the host sets
    // `theme.active_bg`, otherwise bold in `theme.active`. One row, no
    // rule: the dividers already say "separate tabs", so a rule under
    // them would be a second marker for the same fact.
    //
    // Denser than Underline and more emphatic than Dot — it says "these are
    // PEERS you switch between", which is what an open-file strip is, as
    // against "these are views of one thing". The dividers cost columns, so
    // it wants a wide surface.
    Editor,
    // The whole active tab (dot, label and detail) is a rounded chip:
    // half-block ends in `theme.active_bg` (accent when unset), so it reads
    // as one object, not a filled label with loose parts beside it. Every
    // tab reserves the chip's padding, so switching tabs never moves the
    // others. One row, no dividers: the chip is the separation. Half
    // blocks are in every terminal font, unlike powerline glyphs.
    Pill,
};

struct TabStrip {
    struct Tab {
        std::string label;
        // Optional leading status dot. Empty glyph = no dot. The COLOUR is
        // the signal; the glyph is a knob so a host can use ● / ○ / ✓ to
        // stay legible without colour.
        std::string dot_glyph;
        // RUNTIME data, not a Config default: dot()/detail() take a colour
        // from the caller, so this cannot be Themed (whose gate is
        // consteval). The caller's colour still comes from a slot in
        // practice; the compile-time guarantee lives at their Config.
        Color        dot_color = Color::slot(ThemeSlot::Muted);
        // Optional trailing detail: a diffstat, a count, a size. Rendered
        // dim after the label and INCLUDED in the width arithmetic, which
        // is the part a host doing this itself gets wrong.
        std::string detail;
        Color        detail_color = Color::slot(ThemeSlot::Muted);
        // Detail in several colours (e.g. "+12" green, "−3" red), joined by
        // spaces. Used instead of `detail` when non-empty. Dimmed on idle
        // tabs under Pill, so the active chip stays the loudest thing.
        std::vector<std::pair<std::string, Color>> parts;
    };

    std::vector<Tab> tabs;
    int              active_index = 0;
    TabStripTheme    theme;
    TabMark          mark = TabMark::Underline;
    // Columns of lead-in, so the strip lines up with whatever is above it
    // (the panel family indents its subtitle by 2).
    int              indent = 2;
    // Columns between tabs.
    int              gap = 3;

    TabStrip& tab(std::string label) {
        tabs.push_back({std::move(label), {}, {}, {}, {}, {}});
        return *this;
    }
    // Attach a status dot to the most recently added tab.
    TabStrip& dot(Color c, std::string glyph = "\xe2\x97\x8f") {   // ●
        if (!tabs.empty()) {
            tabs.back().dot_glyph = std::move(glyph);
            tabs.back().dot_color = c;
        }
        return *this;
    }
    // Attach a trailing detail to the most recently added tab.
    TabStrip& detail(std::string text, Color c) {
        if (!tabs.empty()) {
            tabs.back().detail       = std::move(text);
            tabs.back().detail_color = c;
        }
        return *this;
    }
    // Add one coloured piece to the most recently added tab's detail.
    TabStrip& part(std::string text, Color c) {
        if (!tabs.empty() && !text.empty()) tabs.back().parts.emplace_back(std::move(text), c);
        return *this;
    }
    TabStrip& active(int i)          { active_index = i; return *this; }
    TabStrip& marker(TabMark m)      { mark = m;         return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;
        if (tabs.empty()) return Element{TextElement{}};

        // Width of one whole tab, in DISPLAY COLUMNS — dot, label and
        // detail together. Columns, not bytes: a label with any non-ASCII
        // in it would otherwise scroll and underline at the wrong place.
        // A filled tab is a CHIP, so it also carries a column of air on
        // each side; that padding has to be in the arithmetic or the strip
        // scrolls by the wrong amount and pushes the active tab off-edge.
        const bool pill = mark == TabMark::Pill;
        const bool chip = theme.active_bg.has_value() && !pill;
        // Capture BY VALUE. This lambda is copied into the component closure
        // below, which outlives build() — and `mark` is a member of the
        // TabStrip, so a [&] capture left it reading a reference to an
        // object that is gone by the time the component renders. That is
        // undefined behaviour and it behaved like one: the strip computed a
        // different scroll window between renders of identical input, so the
        // same tab row came out starting at "Models" on one frame and
        // "Smart" on the next.
        auto detail_width = [](const Tab& t) {
            if (t.parts.empty()) return t.detail.empty() ? 0 : unicode::str_width(t.detail);
            int w = 0;
            for (const auto& [s, c] : t.parts) w += unicode::str_width(s) + 1;
            return w - 1;
        };
        auto tab_width = [chip, pill, mark = mark, detail_width](const Tab& t, bool on) {
            int w = unicode::str_width(t.label);
            if (!t.dot_glyph.empty()) w += unicode::str_width(t.dot_glyph) + 1;
            if (const int d = detail_width(t); d > 0) w += d + (pill ? 2 : 1);
            if (mark == TabMark::Dot)    w += 2;   // "◆ " / "  "
            if (chip && on)              w += 2;   // " label "
            if (pill)                    w += 4;   // "▐ " ... " ▌", on every tab
            return w;
        };

        // Width-aware, because the whole point is fitting the ACTIVE tab.
        // The strip is a single text row (two with an underline), so the
        // component's cost is a few string appends — not a subtree rebuild.
        const int n = static_cast<int>(tabs.size());
        const int act = active_index < 0 ? 0 : (active_index >= n ? n - 1 : active_index);

        return component([tabs = tabs, theme = theme, mark = mark,
                          indent = indent, gap = gap, act, n, chip, pill,
                          tab_width, detail_width](int avail_w, int) -> Element {
            // Clamp the MEASURE-PASS sentinel before it reaches the scroll
            // window.
            //
            // A component with no measure callback is auto-measured by
            // invoking render at whatever width the layout probes with, and
            // a scroll viewport or a prebuilt body probes with an unbounded
            // one (1<<14). The scroll loop below turns `avail` into a
            // starting INDEX, so an absurd probe answers "everything fits,
            // start at zero" while the real paint answers "scroll" — and the
            // strip then renders differently between the two passes. The
            // symptom is a strip that shows "Session | Models | …" on one
            // render and "… Models | Smart | …" on the next, from identical
            // input; the panel's rule row already guards the same sentinel
            // one line below this in panel.cpp.
            constexpr int kMaxWidth = 4096;
            if (avail_w > kMaxWidth) avail_w = kMaxWidth;
            if (avail_w < 0)         avail_w = 0;
            const int avail = avail_w > indent ? avail_w - indent : 0;

            // Slide the window right until the ACTIVE tab's right edge fits.
            // This is the property a hand-rolled strip loses: without it,
            // selecting a tab can select something drawn off-screen.
            // Editor mode separates tabs with a visible divider rather than
            // whitespace: its tabs are PEERS, and a rule between them reads
            // as "another one of the same kind" where a gap reads as "a
            // different thing".
            const int sep_w = mark == TabMark::Editor ? 5 : pill ? 1 : gap;
            auto span = [&](int lo, int hi) {
                int t = 0;
                for (int i = lo; i <= hi; ++i)
                    t += tab_width(tabs[static_cast<std::size_t>(i)], i == act)
                       + (i > lo ? sep_w : 0);
                return t;
            };
            int start = 0;
            // 2 columns reserved for the "… " chip once anything is hidden.
            while (start < act && span(start, act) > avail - (start > 0 ? 2 : 0))
                ++start;

            std::string labels;
            std::vector<StyledRun> label_runs;
            std::string rule;
            // Column cursor, so each piece advances the underline in step.
            int col = 0;
            auto put = [&](std::string_view s, Style st) {
                if (s.empty()) return;
                label_runs.push_back({labels.size(), s.size(), st});
                labels += s;
            };
            // The underline row is built in lockstep with the label row so
            // the two cannot disagree about where a tab starts and ends.
            auto rule_pad = [&](int cols, bool solid) {
                for (int i = 0; i < cols; ++i) rule += solid ? "\xe2\x94\x81" : " ";
            };

            for (int i = 0; i < indent; ++i) { labels += ' '; rule += ' '; ++col; }
            if (start > 0) {
                put("\xe2\x80\xa6 ", Style{}.with_fg(theme.ellipsis));
                rule_pad(2, false);
                col += 2;
            }

            for (int i = start; i < n; ++i) {
                const auto& t = tabs[static_cast<std::size_t>(i)];
                const bool on = (i == act);
                if (pill) {
                    if (i > start) put(" ", Style{});
                    const Color fill = theme.active_bg.value_or(theme.accent);
                    // Inside the chip everything shares its fill. No fill set:
                    // the ends alone mark it, as brackets in the accent.
                    const bool filled = theme.active_bg.has_value();
                    const Style base = on && filled ? Style{}.with_bg(fill) : Style{};
                    // The ends are half blocks in the fill colour over the
                    // surface, so the chip's sides read rounded.
                    put(on ? (filled ? "\xe2\x96\x90" : "\xe2\x9d\xae") : " ", Style{}.with_fg(fill));   // ▐ or ❮
                    put(" ", base);
                    if (!t.dot_glyph.empty()) {
                        put(t.dot_glyph, base.with_fg(t.dot_color));
                        put(" ", base);
                    }
                    put(t.label, on ? base.with_fg(theme.active).with_bold()
                                    : base.with_fg(theme.idle));
                    if (detail_width(t) > 0) {
                        put("  ", base);
                        if (t.parts.empty()) {
                            put(t.detail, base.with_fg(t.detail_color).with_dim());
                        } else {
                            for (std::size_t k = 0; k < t.parts.size(); ++k) {
                                if (k) put(" ", base);
                                Style ps = base.with_fg(t.parts[k].second);
                                if (!on) ps = ps.with_dim();
                                put(t.parts[k].first, ps);
                            }
                        }
                    }
                    put(" ", base);
                    put(on ? (filled ? "\xe2\x96\x8c" : "\xe2\x9d\xaf") : " ", Style{}.with_fg(fill));   // ▌ or ❯
                    continue;
                }
                if (i > start) {
                    if (mark == TabMark::Editor) {
                        // "  │  " — symmetric air around the divider. The
                        // separator has to read as a boundary BETWEEN two
                        // labels, and padding it on one side only makes it
                        // look attached to the tab on that side.
                        put("  ", Style{});
                        put("\xe2\x94\x82", Style{}.with_fg(theme.divider));
                        put("  ", Style{});
                    } else {
                        put(std::string(static_cast<std::size_t>(gap), ' '), Style{});
                    }
                    rule_pad(sep_w, false);
                    col += sep_w;
                }

                if (mark == TabMark::Dot) {
                    put(on ? "\xe2\x97\x86 " : "  ", Style{}.with_fg(theme.accent));
                    rule_pad(2, false);
                    col += 2;
                }
                if (!t.dot_glyph.empty()) {
                    put(t.dot_glyph, Style{}.with_fg(t.dot_color));
                    put(" ", Style{});
                    const int w = unicode::str_width(t.dot_glyph) + 1;
                    rule_pad(w, false);
                    col += w;
                }
                Style ls = Style{}.with_fg(on ? theme.active : theme.idle);
                // A fill already says "this one" as loudly as a strip can.
                // Bold on top of it is the same claim twice, and it makes
                // the chip's text sit a shade off the others' — so weight
                // is only the mark when there is no fill to carry it.
                if (on && !chip) ls = ls.with_bold();
                if (on && chip) {
                    ls = ls.with_bg(*theme.active_bg);
                    put(" ", ls);
                    rule_pad(1, false);
                    ++col;
                }
                put(t.label, ls);
                // ONLY the label is underlined. Extending the rule under a
                // status dot and a trailing diffstat made one tab wear three
                // disjoint underlines — it read as three marks rather than
                // one, which is the opposite of what a selection indicator
                // is for.
                rule_pad(unicode::str_width(t.label), on);
                col += unicode::str_width(t.label);
                if (on && chip) {
                    put(" ", ls);
                    rule_pad(1, false);
                    ++col;
                }

                if (!t.parts.empty()) {
                    for (const auto& [s, c] : t.parts) {
                        put(" ", Style{});
                        put(s, Style{}.with_fg(c));
                    }
                    const int w = detail_width(t) + 1;
                    rule_pad(w, false);
                    col += w;
                } else if (!t.detail.empty()) {
                    put(" ", Style{});
                    put(t.detail, Style{}.with_fg(t.detail_color).with_dim());
                    const int w = unicode::str_width(t.detail) + 1;
                    rule_pad(w, false);
                    col += w;
                }
            }

            Element label_row{TextElement{.content = std::move(labels),
                                          .wrap    = TextWrap::TruncateEnd,
                                          .runs    = std::move(label_runs)}};
            // Dot and Editor are single-row marks, and so is ANY strip
            // wearing a chip: a fill plus a rule beneath it is one fact
            // claimed twice, and the rule would sit under a chip that is
            // two columns wider than its own label.
            if (mark == TabMark::Dot || mark == TabMark::Editor || chip || pill)
                return label_row;

            return v(std::move(label_row),
                     Element{TextElement{.content = std::move(rule),
                                         .style   = Style{}.with_fg(theme.accent),
                                         .wrap    = TextWrap::TruncateEnd}}).build();

        }).build();
    }
};

}  // namespace maya
