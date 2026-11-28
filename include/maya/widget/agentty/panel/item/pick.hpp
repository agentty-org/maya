#pragma once
// maya::panel item widget: Pick — a value owned by ANOTHER panel.
//
// Enter hands off to a real picker rather than opening an inline list; the
// item holds only the display label. This is the escape hatch that keeps
// Choice honest: the searchable case has its own kind, so Choice's dropdown
// never needs a filter.

#include <string>
#include <utility>

#include "../../../../style/style.hpp"
#include "../context.hpp"

namespace maya::panel {

struct Pick {
    std::string label;
    std::string placeholder = "\xe2\x80\x94";   // —
};

[[nodiscard]] inline std::pair<std::string, Style>
render(const Pick& c, const ItemCtx& ctx) {
    // → marks "this opens a full picker", deliberately different from ▾ so
    // an inline enum and a hand-off never read as the same thing.
    const std::string shown = c.label.empty() ? c.placeholder : c.label;
    return {shown + "  \xe2\x86\x92",
            Style{}.with_fg(c.label.empty() ? ctx.theme.off : ctx.theme.value)};
}

} // namespace maya::panel
