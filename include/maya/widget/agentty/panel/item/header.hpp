#pragma once
// maya::panel item widget: Header — a SECTION TITLE, not a value.
//
// A real kind, not a bool on the item: `is_header = true` next to a live
// `control` was two fields describing one mutually-exclusive fact, and the
// old fake (a locked Text) leaked a stray "—" placeholder into the value
// column. As a kind, a header structurally CANNOT also carry a control.
//
// Rendering is whole-item (upper-cased leading + a rule to the right edge),
// so the panel's item renderer branches on this kind before the cell layout;
// the render() overload below exists only to keep the Control visit total.

#include <string>
#include <utility>

#include "../../../../style/style.hpp"
#include "../context.hpp"

namespace maya::panel {

struct Header {};

[[nodiscard]] inline std::pair<std::string, Style>
render(const Header&, const ItemCtx&) {
    return {std::string{}, Style{}};   // headers have no trailing cell
}

} // namespace maya::panel
