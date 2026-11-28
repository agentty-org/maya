#pragma once
// maya::panel item widget: Toggle — a boolean. Enter/Space/←/→ all flip it.

#include <string>
#include <utility>

#include "../../../../style/style.hpp"
#include "../context.hpp"

namespace maya::panel {

struct Toggle { bool on = false; };

[[nodiscard]] inline std::pair<std::string, Style>
render(const Toggle& c, const ItemCtx& ctx) {
    return {c.on ? "\xe2\x97\x8f on" : "\xe2\x97\x8b off",              // ● / ○
            Style{}.with_fg(c.on ? ctx.theme.on : ctx.theme.off)};
}

} // namespace maya::panel
