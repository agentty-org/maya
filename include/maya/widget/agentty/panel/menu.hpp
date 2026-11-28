#pragma once
// maya::panel::Menu — the inline option list a Choice row opens.
//
// Enums only. There is no query field: a set large enough to need searching
// belongs in its own overlay, reached by a `Pick` row. Keeping that boundary
// sharp is what stops this from slowly growing into a second, worse picker.
//
// TWO markers, never conflated — ◉ is the COMMITTED value, ❯ is the CURSOR.
// They coincide on open and diverge the moment you move.

#include <string>
#include <vector>

namespace maya::panel {

struct Menu {
    std::vector<std::string> options;
    std::vector<std::string> hints;
    int  highlighted = 0;
    int  current     = -1;
    int  scroll      = 0;
    int  viewport    = 8;
};

} // namespace maya::panel
