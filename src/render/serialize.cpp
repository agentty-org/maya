// src/render/serialize.cpp — serialize(): a whole frame's cells as VT bytes.
#include "serialize_internal.hpp"

namespace maya {

using namespace serialize_detail;

void serialize(const Canvas& canvas, const StylePool& pool,
               std::string& out, int rows, int start_row) {
    const int W = canvas.width();
    const int total_rows = (rows > 0) ? std::min(rows, canvas.height()) : canvas.height();
    const int y_begin = std::clamp(start_row, 0, total_rows);
    const int y_end = total_rows;

    if (W <= 0 || y_begin >= y_end) return;

    uint16_t current_style = UINT16_MAX; // sentinel: no SGR emitted yet

    // Disable auto-wrap (DECAWM reset) so that characters extending past
    // the right margin don't wrap to the next line. Re-enabled at the end
    // of this function — every early return must restore it.
    out += "\x1b[?7l";

    for (int y = y_begin; y < y_end; ++y) {
        if (y > y_begin) out += "\r\n";

        // Trim trailing blanks: only emit through the last visible cell.
        // Styled spaces (sid != 0) are treated as content — they may carry
        // background color, inverse, etc. last_content_col() is O(1) —
        // canvas maintains it incrementally.
        const int last_col = canvas.last_content_col(y);
        if (last_col >= 0) {
            emit_cell_run(canvas, pool, y, 0, last_col + 1, current_style, out);
        }
        // EL 0 cleans up any stale content from a prior frame whose row
        // was wider than the current one. Reset SGR first so the erased
        // cells don't inherit attributes from the last emitted cell —
        // particularly underline / inverse, which most modern terminals
        // (alacritty, kitty, vte-based, iTerm 3.5+) apply to EL'd cells
        // per the spec. Without this, a row whose last cell is styled
        // (e.g. a [link](url) at row end) would visually extend its
        // underline to end-of-row.
        //
        // BUT the background must survive that reset when the theme owns
        // the canvas. EL fills with the CURRENT background, so a bare
        // SGR 0 here erases to the TERMINAL's colour and every row ends in
        // a ragged stripe of un-themed cells running to the right edge —
        // the "hard edges where the fill stops" artifact. Re-assert the
        // theme background after clearing attributes so the erase lands in
        // the theme's own colour.
        //
        // Under `native` there is no background to re-assert (owns_canvas
        // is false), so this stays a plain reset and the terminal shows
        // through exactly as it should.
        if (current_style != 0) {
            pool.close_link(current_style, out); out.append(pool.sgr(0));
            current_style = 0;
        }
        const Theme& th = theme::live();
        const bool themed_canvas = theme::owns_canvas(th);
        if (themed_canvas) {
            // Degraded the same way painted cells are, so the erase colour
            // cannot drift from the fill it is continuing.
            out += "\x1b[";
            th.background.degrade(terminal_color_level()).append_bg_sgr(out);
            out += "m";
        }
        // Skip EL when the row filled through col W-1: DECAWM-off leaves
        // the cursor AT col W-1 (no advance past the right edge,
        // ECMA-48 §8.3.118), and \x1b[K from there would erase that
        // rightmost cell — the last-column corruption symptom on
        // full-width content (code-block right borders, full-width
        // rules). Nothing past W-1 exists to erase anyway.
        if (last_col < W - 1) {
            out += "\x1b[K";
        }
        // Leave the style bookkeeping honest: we emitted a bg outside the
        // pool, so the next row must re-emit its style rather than assume
        // style 0 is still in force.
        if (themed_canvas) current_style = UINT16_MAX;
    }

    out += "\x1b[?7h";   // re-enable auto-wrap
    if (links_used()) out += "\x1b]8;;\x1b\\";   // close any OSC 8 link
    out += ansi::reset;  // reset SGR at end of frame
}

} // namespace maya
