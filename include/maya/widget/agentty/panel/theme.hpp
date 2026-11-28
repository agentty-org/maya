#pragma once
// maya::panel::Theme — the panel family's palette, one place.
//
// ANSI-relative, never hex. A widget that hardcodes #181825 looks correct on
// exactly one terminal theme and wrong everywhere else. The panel paints NO
// background; it inherits the terminal's, and only the cursor row is tinted.

#include "../../../style/color.hpp"

namespace maya::panel {

struct Theme {
    Color title      = Color::slot(ThemeSlot::Text);
    Color label      = Color::slot(ThemeSlot::Text);
    Color help       = Color::slot(ThemeSlot::Muted);
    Color value      = Color::slot(ThemeSlot::Info);
    Color value_edit = Color::slot(ThemeSlot::Primary);
    Color on         = Color::slot(ThemeSlot::Success);
    Color off        = Color::slot(ThemeSlot::Muted);
    Color origin     = Color::slot(ThemeSlot::Muted);
    Color locked     = Color::slot(ThemeSlot::Muted);
    Color error      = Color::slot(ThemeSlot::Error);
    Color good       = Color::slot(ThemeSlot::Success);
    Color busy       = Color::slot(ThemeSlot::Warning);
    Color cursor     = Color::slot(ThemeSlot::Primary);      // the edge bar
    Color active     = Color::slot(ThemeSlot::Accent);
    Color match      = Color::slot(ThemeSlot::Info);      // fuzzy-match highlight

    // Cursor-row wash. A tint, not a reverse-video slab: ANSI bright-white is
    // a cream/yellow tone in several popular palettes, and a full-width band
    // of it across a wide settings panel reads as a rendering fault.
    //
    // Hex, not an ANSI slot, because this is the ONE place a literal is
    // right: it must sit a hair above the terminal's background on both dark
    // and light themes, and every ANSI slot is either invisible against one
    // of them or loud against the other. Verified by asserting the emitted
    // SGR code, which is what caught an earlier "invisible black".
    Color row_bg     = Color::slot(ThemeSlot::Surface);
};

} // namespace maya::panel

namespace maya {
// Compatibility spelling — the type predates the panel/ folder.
using PanelTheme = panel::Theme;
} // namespace maya
