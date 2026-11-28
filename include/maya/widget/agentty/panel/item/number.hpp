#pragma once
// maya::panel item widget: Number — a bounded integer, edited digit-wise.

#include <cstdint>
#include <string>
#include <utility>

#include "../../../../style/style.hpp"
#include "../context.hpp"

namespace maya::panel {

struct Number {
    std::int64_t value = 0;
    // npos when not editing. Digits are typed at the end, so any other value
    // draws the caret after them.
    std::size_t  caret = std::string::npos;
};

[[nodiscard]] inline std::pair<std::string, Style>
render(const Number& c, const ItemCtx& ctx) {
    std::string s = std::to_string(c.value);
    if (c.caret == std::string::npos)
        return {std::move(s), Style{}.with_fg(ctx.theme.value)};
    // Digits are typed at the end, so the caret always sits there.
    if (ctx.caret_out) *ctx.caret_out = s.size();
    s += "\xe2\x96\x88";
    return {std::move(s), Style{}.with_fg(ctx.theme.value_edit)};
}

} // namespace maya::panel
