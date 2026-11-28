#pragma once
// maya::panel item widget: Choice — one of a SMALL, CLOSED set (an enum).
//
// ←/→ cycle in place; Enter opens the inline Menu (menu.hpp). There is no
// search box and no filtering, because the moment a list needs either it is
// not an enum any more: it belongs in a full picker, reached by a Pick item.
// Keeping that boundary sharp is what stops this widget from slowly growing
// into a second, worse picker.

#include <string>
#include <utility>

#include "../../../../style/style.hpp"
#include "../context.hpp"

namespace maya::panel {

// The option list travels separately (Config::menu) and only while open.
struct Choice { std::string label; };

[[nodiscard]] inline std::pair<std::string, Style>
render(const Choice& c, const ItemCtx& ctx) {
    // The chevron points the way the list will move: ▾ opens downward,
    // ▴ collapses. While open the value itself is redundant — it is
    // marked ◉ in the list right below — so the row shows only the
    // affordance and lets the list carry the information.
    if (ctx.open) return {"\xe2\x96\xb4", Style{}.with_fg(ctx.theme.cursor)};
    return {c.label + "  \xe2\x96\xbe", Style{}.with_fg(ctx.theme.value)};
}

} // namespace maya::panel
