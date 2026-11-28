#pragma once
// maya::report — command-line report output (`agentty paste status`, help
// text, settings listings) printed to a plain terminal, not the TUI.
//
// A report is a column of rows built from a small vocabulary: styled words,
// a run of words with single spaces between them, a label in a fixed gutter
// with its value wrapping beside it, an indent, a blank row. The host picks
// the words; how each kind looks is decided here.

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "../../dsl.hpp"
#include "../../element/builder.hpp"
#include "../../style/color.hpp"
#include "../../style/style.hpp"

namespace maya::report {

struct Palette {
    Style title, heading, dim, ok, warn, bad, code, plain;
};

[[nodiscard]] inline Element words(std::string s, Style st = {}) { return dsl::text(std::move(s), st).build(); }
[[nodiscard]] inline Element blank() { return dsl::text("").build(); }

// A box exactly `w` columns wide: a gutter column.
[[nodiscard]] inline Element fixed(Element e, int w) {
    auto box = hstack();
    box.width(Dimension::fixed(w));
    box.min_width(Dimension::fixed(w));
    box.max_width(Dimension::fixed(w));
    return box(std::move(e));
}

// A label in a fixed gutter and its value wrapping beside it.
[[nodiscard]] inline Element row(Element label, Element value, int gutter) {
    auto right = hstack();
    right.grow();
    auto r = hstack();
    return r(fixed(std::move(label), gutter), right(std::move(value)));
}

// Words side by side, one space between.
[[nodiscard]] inline Element line(std::vector<Element> parts) {
    auto h = hstack();
    h.gap(1);
    return h(std::move(parts));
}

[[nodiscard]] inline Element indent(Element e, int n = 2) {
    auto h = hstack();
    return h(fixed(dsl::text("").build(), n), std::move(e));
}

[[nodiscard]] inline Element stack(std::vector<Element> rows) {
    auto v = vstack();
    return v(std::move(rows));
}

// A flush column (no gap between rows): the --help page.
[[nodiscard]] inline Element doc(std::vector<Element> rows) {
    auto v = vstack();
    v.gap(0);
    return v(std::move(rows));
}

// --help row: the name in a gutter, its description wrapping beside it.
// The gutter is exact (min = max), so every description starts on one
// column. A name that doesn't fit the gutter gets its own line, with the
// description indented underneath, rather than colliding with it.
[[nodiscard]] inline Element entry(std::string name, std::string desc, int gutter) {
    if (string_width(name) >= gutter) {
        auto rest = hstack();
        rest.grow();
        auto second = hstack();
        auto st = vstack();
        return st(words(std::move(name)),
                  second(fixed(words(""), gutter), rest(words(std::move(desc)))));
    }
    return row(words(std::move(name)), words(std::move(desc)), gutter);
}

// A warning in a double-ruled box with a centred title, e.g. the
// "SANDBOX IS OFF" banner printed before the TUI starts.
[[nodiscard]] inline Element banner(std::string title, std::vector<Element> body, Color color) {
    return vstack()
        .padding(1, 2)
        .border(BorderStyle::Double)
        .border_color(color)
        .border_text(std::move(title), BorderTextPos::Top, BorderTextAlign::Center)(std::move(body));
}

// ── listings ─────────────────────────────────────────────────────────────────────
//
// A CLI listing is a table: each row a list of cells, each column as wide
// as its widest cell (display columns, so non-ASCII aligns) up to a cap.
// The LAST column takes the rest of the row and wraps. Columns are two
// spaces apart and the table sits `indent` columns in.

struct Cell {
    std::string text;
    Style       style{};
};

// The outcome marks a listing uses, so every subcommand says "ok" the same.
enum class Mark { None, Ok, Bad, Warn, Info, On, Off };

[[nodiscard]] inline Cell mark(Mark m) {
    switch (m) {
        case Mark::Ok:   return {"\xe2\x9c\x93", Style{}.with_fg(Color::slot(ThemeSlot::Success))};  // ✓
        case Mark::Bad:  return {"\xe2\x9c\x97", Style{}.with_fg(Color::slot(ThemeSlot::Error))};    // ✗
        case Mark::Warn: return {"!",            Style{}.with_fg(Color::slot(ThemeSlot::Warning)).with_bold()};
        case Mark::Info: return {"\xc2\xb7",     Style{}.with_fg(Color::slot(ThemeSlot::Muted))};    // ·
        case Mark::On:   return {"\xe2\x97\x8f", Style{}.with_fg(Color::slot(ThemeSlot::Success))};  // ●
        case Mark::Off:  return {"\xe2\x97\x8b", Style{}.with_fg(Color::slot(ThemeSlot::Muted))};    // ○
        case Mark::None: break;
    }
    return {" "};
}

// One line: an outcome mark then a sentence. "  ✓ config parses".
[[nodiscard]] inline Element check(Mark m, std::string sentence, int indent_cols = 2) {
    Cell c = mark(m);
    auto h = hstack();
    h.gap(1);
    return report::indent(h(words(std::move(c.text), c.style), words(std::move(sentence))), indent_cols);
}

struct Table {
    std::vector<std::vector<Cell>> rows;
    int indent  = 2;
    int max_col = 32;     // cap for every column but the last
    // Fixed widths for the leading columns (display columns, without the
    // two-space gap). A listing printed one item at a time, with detail
    // lines between items, pins its widths so every item lines up.
    std::vector<int> widths{};

    [[nodiscard]] Element build() const {
        std::size_t ncols = 0;
        for (const auto& r : rows) ncols = std::max(ncols, r.size());
        std::vector<int> w(ncols, 0);
        for (const auto& r : rows)
            for (std::size_t c = 0; c + 1 < r.size(); ++c)
                w[c] = std::min(max_col, std::max(w[c], string_width(r[c].text)));
        for (std::size_t c = 0; c < widths.size() && c < w.size(); ++c) w[c] = widths[c];
        std::vector<Element> out;
        out.reserve(rows.size());
        for (const auto& r : rows) {
            std::vector<Element> parts;
            for (std::size_t c = 0; c < r.size(); ++c) {
                Element cell = words(r[c].text, r[c].style);
                if (c + 1 < r.size()) {
                    parts.push_back(fixed(std::move(cell), w[c] + 2));
                } else {
                    auto rest = hstack();
                    rest.grow();
                    parts.push_back(rest(std::move(cell)));
                }
            }
            auto h = hstack();
            out.push_back(report::indent(h(std::move(parts)), indent));
        }
        return doc(std::move(out));
    }
    operator Element() const { return build(); }  // NOLINT(google-explicit-constructor)
};

// A heading that must not be missed: "━━━ REVIEW THIS ONE ━━━".
[[nodiscard]] inline Element alarm(std::string title, Color color = Color::slot(ThemeSlot::Error)) {
    return report::indent(words("\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81 " + std::move(title)
                                + " \xe2\x94\x81\xe2\x94\x81\xe2\x94\x81",
                                Style{}.with_fg(color).with_bold()));
}

// A rule-framed block: "── title ──" / body / "────────".
// For content that must reach the user byte-for-byte (a file they are about
// to approve), print framed_open, write the bytes yourself, then
// framed_close: nothing in between is wrapped or restyled.
[[nodiscard]] inline Element framed_open(std::string title) {
    return words("\xe2\x94\x80\xe2\x94\x80 " + std::move(title) + " \xe2\x94\x80\xe2\x94\x80",
                 Style{}.with_fg(Color::slot(ThemeSlot::Muted)));
}
[[nodiscard]] inline Element framed_close() {
    std::string close;
    for (int i = 0; i < 8; ++i) close += "\xe2\x94\x80";
    return words(std::move(close), Style{}.with_fg(Color::slot(ThemeSlot::Muted)));
}
[[nodiscard]] inline Element framed(std::string title, std::vector<Element> body) {
    std::vector<Element> rows;
    rows.push_back(framed_open(std::move(title)));
    for (auto& e : body) rows.push_back(std::move(e));
    rows.push_back(framed_close());
    return doc(std::move(rows));
}

// "label  → target": an arrow continuation under a row.
[[nodiscard]] inline Element arrow(std::string target, int indent_cols = 4) {
    return report::indent(words("\xe2\x86\x92 " + std::move(target),
                                Style{}.with_fg(Color::slot(ThemeSlot::Muted))), indent_cols);
}

// ── output ──────────────────────────────────────────────────────────────────────────
// A report to a plain stream: the terminal's width (clamped 50..110) on a
// tty, 80 otherwise; styled only on a colour tty (NO_COLOR, TERM=dumb and
// pipes get plain text); row padding trimmed so a copy gets clean lines.
[[nodiscard]] bool        painted(std::FILE* f);
[[nodiscard]] std::string render(const Element& e, std::FILE* f);
void                      print(const Element& e, std::FILE* f = stdout);

} // namespace maya::report
