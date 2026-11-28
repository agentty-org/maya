#pragma once
// maya::panel::Item — the ONE item type of the panel family.
//
// What used to distinguish a picker row from a form row is per-cell
// decoration, not structure:
//
//     edge · badge · leading [· highlight] · gap · trailing [· origin]
//
// A list item fills `leading`/`trailing`; a settings item additionally sets
// `control` and `origin`. Nothing about the frame, the scroll, the selection
// or the width arithmetic differs, because there is only one of each.
//
// ("Item", not "Row": the help line and the error line render UNDER it, so
// an item is 1–3 terminal rows — calling it a row was a presentation lie.)

#include <string>
#include <vector>

#include "../../../style/color.hpp"
#include "../../../style/style.hpp"
#include "control.hpp"

namespace maya::panel {

struct Item {
    // Column 0-1: a status glyph (● active, ⚠ pending-delete, tree elbow).
    std::string badge;
    Style       badge_style{};

    std::string leading;
    Style       leading_style{};

    // Optional match-highlight: byte offsets into `leading` to render in
    // the accent hue (bold) — the characters a fuzzy query matched. Empty
    // ⇒ leading is painted as one span. The highlight keeps its hue even
    // on the cursor row, so "which chars matched" stays legible.
    std::vector<int> highlight;
    Color            highlight_fg = Color::slot(ThemeSlot::Link);

    // Optional colour sample, painted as solid blocks between the leading
    // text and the trailing cell.
    //
    // For a row that IS a set of colours — a theme in the scheme browser —
    // where the name is the least informative thing about it. "Rose Pine
    // Dawn" tells you nothing; six blocks of its actual palette tell you
    // whether it is light, whether the accent is warm, and whether the
    // whole thing is muted, at a glance and without selecting it.
    //
    // A vector of colours rather than a pre-built Element because Item is a
    // DESCRIPTION the panel renders — giving it a child Element would let a
    // caller smuggle in arbitrary layout and break the row geometry every
    // other field is careful to keep uniform. Empty (the default) renders
    // nothing at all, so no existing row changes.
    std::vector<Color> swatch;

    // The trailing cell. `Label` covers every picker row; the other
    // alternatives are the editable controls a settings row carries.
    Control control = Label{};

    // Convenience for the common case: a plain trailing string. Setting
    // these is equivalent to `control = Label{text, style}`, and exists so
    // a list row reads as a list row rather than wrapping every string in
    // a variant alternative. `control` wins if both are set.
    std::string trailing;
    Style       trailing_style{};

    // Dim provenance after the value ("default", "env: X", "pinned").
    std::string origin;

    // A one-line description shown ONLY while the cursor is on this row.
    // Painting it under every row doubles a list's height — a 22-setting
    // pane became 50-odd rows in a 14-row viewport.
    std::string help;

    // Non-empty ⇒ invalid; rendered under the row in the error tone.
    std::string error;

    // DERIVED from Config::selected, never set by a caller. It lives on
    // the row only because the renderer needs it per-row; writing it from
    // outside is silently ignored. One owner for "where is the cursor".
    bool selected = false;
    bool active   = false;   // "currently in use" — a persistent marker
    bool locked   = false;   // visible, not editable
    std::string locked_reason;

    // A non-selectable SECTION HEADER: upper-cased, with a rule to the
    // right edge. DERIVED from the control's kind — a header is what the
    // item IS, not a flag beside an unrelated control (the bool let both be
    // set at once, a state with no meaning).
    [[nodiscard]] bool is_header() const noexcept {
        return std::holds_alternative<Header>(control);
    }

    // The trailing cell is secondary and yields space FIRST (a command
    // palette's description must never eat its command name). Default:
    // the LEADING cell yields first, so a long label truncates before it
    // can push the value off the row.
    bool trailing_secondary = false;

    // The CONTROL is the row's content, not a value beside a label: it
    // hugs the (short) leading and grows leftward-anchored instead of
    // right-aligning. For prompt-style rows — a lone › with an input —
    // where right-alignment reads as the value drifting away from its
    // prompt. Rows with a real label keep the label─→value column layout.
    bool value_primary = false;
};

} // namespace maya::panel

namespace maya::panel {
// Compatibility spelling — the type was called Row before the rename.
using Row = Item;
} // namespace maya::panel
