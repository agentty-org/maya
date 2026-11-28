#pragma once
// maya::widget::CompletionMenu — LSP autocomplete popup (background-free)
//
// The completion list that pops under the caret: each item shows its kind
// glyph, label (with fuzzy-matched characters highlighted), and a dim
// right-aligned type/detail. The selected row is marked and accented, the list
// windows around the selection with a scroll hint, and an optional doc line for
// the selected item hangs below a divider.
//
// Foreground-only; a thin rounded border frames it.
//
// Usage:
//   CompletionMenu m;
//   m.item({SymKind::Method, "concat", "(Rope*, Rope*) -> Rope*", "Join two ropes."})
//    .item({SymKind::Field,  "weight", ": std::size_t"})
//    .select(0);
//   Element ui = m | dsl::width(46);

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../../dsl.hpp"
#include "../../element/builder.hpp"
#include "../../element/text.hpp"
#include "../../style/color.hpp"
#include "../../style/style.hpp"
#include "../sym_kind.hpp"

namespace maya {

struct CompletionMenuTheme {
    Color border   = Color::slot(ThemeSlot::Surface);
    Color label    = Color::slot(ThemeSlot::Secondary); // item label
    Color sel_label = Color::slot(ThemeSlot::Text); // selected label
    Color match     = Color::slot(ThemeSlot::Warning); // fuzzy-matched chars
    Color detail    = Color::slot(ThemeSlot::Muted); // type / signature
    Color pointer   = Color::slot(ThemeSlot::Primary); // ▸ selection pointer
    Color sel_shade  = Color::slot(ThemeSlot::Surface); // subtle wash on the selected row
    Color scroll     = Color::slot(ThemeSlot::Muted); // scroll hint
    Color doc        = Color::slot(ThemeSlot::Muted); // doc string
};

struct CompletionMenu {
    struct Item {
        SymKind          kind = SymKind::Variable;
        std::string      label;
        std::string      detail;         // type or signature, right-aligned
        std::string      doc;            // shown for the selected item
        std::vector<int> match;          // byte offsets into label to highlight
    };

    std::vector<Item>    items;
    int                  selected_ = 0;
    int                  max_rows  = 8;
    CompletionMenuTheme  theme;

    CompletionMenu& item(Item it)  { items.push_back(std::move(it)); return *this; }
    CompletionMenu& select(int i)  { selected_ = i; return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        const int n = static_cast<int>(items.size());
        const int sel = std::clamp(selected_, 0, std::max(0, n - 1));
        const int rows = std::min(max_rows, n);

        // window around the selection
        int top = std::clamp(sel - rows / 2, 0, std::max(0, n - rows));
        int bot = std::min(n, top + rows);

        std::vector<Element> list;
        for (int i = top; i < bot; ++i)
            list.push_back(row(items[static_cast<size_t>(i)], i == sel));

        std::vector<Element> col;
        // top scroll hint
        if (top > 0) col.push_back(hint("\xe2\x96\xb2 " + std::to_string(top) + " more"));
        for (auto& e : list) col.push_back(std::move(e));
        if (bot < n) col.push_back(hint("\xe2\x96\xbc " + std::to_string(n - bot) + " more"));

        // doc for the selected item
        const std::string& doc = items[static_cast<size_t>(sel)].doc;
        if (!doc.empty()) {
            col.push_back(Element{TextElement{ .content = "", .style = Style{} }});
            col.push_back(Element{TextElement{
                .content = doc,
                .style   = Style{}.with_fg(theme.doc).with_italic(),
                .wrap    = TextWrap::Wrap,
            }});
        }

        return maya::detail::box()
            .border(BorderStyle::Round)
            .border_color(theme.border)
            .padding(0, 1, 0, 1)
            (dsl::v(std::move(col)).build());
    }

private:
    Element hint(std::string text) const {
        return Element{TextElement{ .content = std::move(text),
                                    .style = Style{}.with_fg(theme.scroll),
                                    .wrap = TextWrap::NoWrap }};
    }

    Element row(const Item& it, bool sel) const {
        std::string s; std::vector<StyledRun> r;
        auto put = [&](std::string_view t, Style st) {
            if (t.empty()) return;
            r.push_back({s.size(), t.size(), st});
            s += t;
        };

        const Color shade = sel ? theme.sel_shade : Color{};
        auto tint = [&](Style st) { return sel ? st.with_bg(shade) : st; };

        put(sel ? "\xe2\x96\xb8 " : "  ", tint(Style{}.with_fg(theme.pointer))); // ▸
        put(std::string(sym_glyph(it.kind)) + " ", tint(Style{}.with_fg(sym_color(it.kind))));

        const Style base = tint(Style{}.with_fg(sel ? theme.sel_label : theme.label));
        const Style hit  = tint(Style{}.with_fg(theme.match).with_bold());
        size_t mi = 0;
        for (size_t i = 0; i < it.label.size(); ++i) {
            bool m = (mi < it.match.size() && static_cast<int>(i) == it.match[mi]);
            if (m) ++mi;
            Style st = m ? hit : base;
            if (sel && !m) st = st.with_bold();
            put(std::string_view{it.label}.substr(i, 1), st);
        }

        std::string left = std::move(s);
        std::vector<StyledRun> lruns = std::move(r);
        std::string right = it.detail;

        // Selected: pad full-width so the shade spans the whole row.
        if (sel) {
            return Element{ComponentElement{
                .render = [left = std::move(left), lruns = std::move(lruns),
                           right = std::move(right), shade, dcol = theme.detail]
                          (int w, int) -> Element {
                    const Style fill = Style{}.with_bg(shade);
                    std::string s = left;
                    std::vector<StyledRun> runs = lruns;
                    int lw = string_width(left), rw = string_width(right);
                    int gap = std::max(1, w - lw - rw);
                    runs.push_back({s.size(), static_cast<size_t>(gap), fill});
                    s.append(static_cast<size_t>(gap), ' ');
                    if (!right.empty()) {
                        runs.push_back({s.size(), right.size(),
                                        Style{}.with_fg(dcol).with_bg(shade)});
                        s += right;
                    }
                    int used = string_width(s);
                    if (used < w) {
                        runs.push_back({s.size(), static_cast<size_t>(w - used), fill});
                        s.append(static_cast<size_t>(w - used), ' ');
                    }
                    return Element{TextElement{ .content = std::move(s), .style = fill,
                                                .wrap = TextWrap::NoWrap,
                                                .runs = std::move(runs) }};
                },
            }};
        }

        if (right.empty())
            return Element{TextElement{ .content = std::move(left), .style = Style{},
                                        .wrap = TextWrap::NoWrap, .runs = std::move(lruns) }};

        // Pad to the full width like the selected row, so the detail sits
        // flush right and never butts against the label.
        return Element{ComponentElement{
            .render = [left = std::move(left), lruns = std::move(lruns),
                       right = std::move(right), dcol = theme.detail](int w, int) -> Element {
                std::string s = left;
                std::vector<StyledRun> runs = lruns;
                const int lw = string_width(left), rw = string_width(right);
                s.append(static_cast<size_t>(std::max(2, w - lw - rw)), ' ');
                runs.push_back({s.size(), right.size(), Style{}.with_fg(dcol)});
                s += right;
                return Element{TextElement{ .content = std::move(s), .style = Style{},
                                            .wrap = TextWrap::NoWrap,
                                            .runs = std::move(runs) }};
            },
        }};
    }
};

} // namespace maya
