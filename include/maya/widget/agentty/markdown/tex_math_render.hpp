#pragma once
// tex_math_render.hpp — flatten a typeset math Box into a maya Element.
//
// Split out from tex_math.hpp so the pure typesetter (Box layout, symbol
// tables) has ZERO dependency on the Element/builder machinery and can be
// unit-tested in isolation against a plain string grid. This header is the
// thin bridge that the markdown renderer actually calls.

#include "tex_math.hpp"
#include "../../../element/builder.hpp"

#include <string>
#include <vector>

namespace maya::texmath {

// Render one grid row of a Box into a styled TextElement. Continuation cells
// (len==0, the second column of a wide glyph) are skipped — their leading cell
// already carries the full grapheme and occupies two display columns.
inline TextElement row_to_text(const Box& b, int r) {
    TextElement te;
    te.wrap = TextWrap::NoWrap;
    std::vector<StyledRun> runs;
    std::string& out = te.content;
    for (int c = 0; c < b.cols; ++c) {
        const Cell& cell = b.at(r, c);
        std::size_t start = out.size();
        if (cell.len == 0) {
            // continuation of a wide glyph: nothing to emit (the wide cell's
            // own bytes already rendered and the terminal advances 2 cols).
            continue;
        }
        out.append(cell.bytes.data(), cell.len);
        if (out.size() > start)
            runs.push_back({start, out.size() - start, cell.style});
    }
    if (out.empty()) { out = " "; }  // keep the row present (height stable)
    te.runs = std::move(runs);
    return te;
}

// Flatten a Box into a vertical stack of styled TextElements — one per grid
// row. A single-row box collapses to a bare TextElement (no container) so
// inline math slots into a paragraph without adding vertical chrome.
[[nodiscard]] inline Element box_to_element(const Box& b) {
    if (b.rows <= 1) return Element{row_to_text(b, 0)};
    std::vector<Element> rows;
    rows.reserve(static_cast<std::size_t>(b.rows));
    for (int r = 0; r < b.rows; ++r) rows.push_back(Element{row_to_text(b, r)});
    // Fully-qualify: inside namespace maya::texmath, a bare `detail::vstack`
    // would bind to the typesetter's box-stacker, not the Element builder.
    return ::maya::detail::vstack()(std::move(rows));
}

// Typeset + flatten in one call. This is the public convenience the markdown
// renderer uses. `display` selects \displaystyle (limits above/below) vs
// \textstyle (inline) conventions.
[[nodiscard]] inline Element render_math(std::string_view latex,
                                         const MathPalette& pal, bool display) {
    return box_to_element(typeset(latex, pal, display));
}

} // namespace maya::texmath
