#pragma once
// maya::panel item widget: Path — a filesystem path with a liveness verdict.
//
// The ✓/✗ comes from a probe the HOST runs (the widget never touches the
// filesystem): "file not found" appears while you type, not after you commit.

#include <cstdint>
#include <string>
#include <utility>

#include "../../../../style/style.hpp"
#include "../caret.hpp"
#include "../context.hpp"

namespace maya::panel {

struct Path {
    std::string value;
    std::size_t caret = std::string::npos;
    enum class State : std::uint8_t { Unknown, Exists, Missing };
    State       state = State::Unknown;
    std::string placeholder;
};

[[nodiscard]] inline std::pair<std::string, Style>
render(const Path& c, const ItemCtx& ctx) {
    const bool editing = c.caret != std::string::npos;
    if (c.value.empty() && !editing)
        return {c.placeholder.empty() ? "\xe2\x80\x94" : c.placeholder,
                Style{}.with_fg(ctx.theme.off)};
    if (c.value.empty())
        return {[&] {
                    if (ctx.caret_out) *ctx.caret_out = 0;
                    return std::string{"\xe2\x96\x88"}
                        + (c.placeholder.empty() ? "" : " " + c.placeholder);
                }(),
                Style{}.with_fg(ctx.theme.value_edit)};
    std::string s = detail::with_caret(c.value, c.caret, ctx.edit_budget,
                                       ctx.caret_out);
    if (c.state == Path::State::Missing)      s += "  \xe2\x9c\x97";   // ✗
    else if (c.state == Path::State::Exists)  s += "  \xe2\x9c\x93";   // ✓
    Style st = Style{}.with_fg(editing ? ctx.theme.value_edit
                                       : ctx.theme.value);
    if (c.state == Path::State::Missing) st = Style{}.with_fg(ctx.theme.error);
    return {std::move(s), st};
}

} // namespace maya::panel
