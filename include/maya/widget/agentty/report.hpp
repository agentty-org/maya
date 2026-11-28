#pragma once
// maya::report — command-line report output (`agentty paste status`, help
// text, settings listings) printed to a plain terminal, not the TUI.
//
// A report is a column of rows built from a small vocabulary: styled words,
// a run of words with single spaces between them, a label in a fixed gutter
// with its value wrapping beside it, an indent, a blank row. The host picks
// the words; how each kind looks is decided here.

#include <string>
#include <utility>
#include <vector>

#include "../../dsl.hpp"
#include "../../element/builder.hpp"
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

} // namespace maya::report
