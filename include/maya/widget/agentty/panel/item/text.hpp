#pragma once
// maya::panel item widget: Text — a free single-line string.

#include <string>
#include <utility>

#include "../../../../style/style.hpp"
#include "../caret.hpp"
#include "../context.hpp"

namespace maya::panel {

struct Text {
    std::string value;
    std::size_t caret = std::string::npos;   // npos ⇒ not being edited
    std::string placeholder;
};

[[nodiscard]] inline std::pair<std::string, Style>
render(const Text& c, const ItemCtx& ctx) {
    const bool editing = c.caret != std::string::npos;
    if (c.value.empty() && !editing)
        return {c.placeholder.empty() ? "\xe2\x80\x94" : c.placeholder,
                Style{}.with_fg(ctx.theme.off)};
    // An EMPTY field being edited is just a caret. On its own that is a lone
    // block glyph in the value column — indistinguishable from a rendering
    // artefact — so it keeps its placeholder beside it and reads as
    // "type here".
    if (c.value.empty())
        return {[&] {
                    if (ctx.caret_out) *ctx.caret_out = 0;
                    return std::string{"\xe2\x96\x88"}
                        + (c.placeholder.empty() ? "" : " " + c.placeholder);
                }(),
                Style{}.with_fg(ctx.theme.value_edit)};
    return {detail::with_caret(c.value, c.caret, ctx.edit_budget,
                               ctx.caret_out),
            Style{}.with_fg(ctx.theme.value_edit)};
}

} // namespace maya::panel
