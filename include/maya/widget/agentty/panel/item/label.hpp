#pragma once
// maya::panel item widget: Label — plain text.
//
// Every list row (a palette command, a thread, a symbol) is this. The
// degenerate item kind: no state, no interaction, just a string + style.

#include <string>
#include <utility>

#include "../../../../style/style.hpp"
#include "../context.hpp"

namespace maya::panel {

struct Label {
    std::string text;
    Style       style{};
};

[[nodiscard]] inline std::pair<std::string, Style>
render(const Label& c, const ItemCtx&) {
    return {c.text, c.style};
}

} // namespace maya::panel
