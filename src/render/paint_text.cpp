// src/render/paint_text.cpp — a text run: wrapping, truncation, styles.
#include "render_internal.hpp"

namespace maya {
namespace render_detail {

namespace {
// Display width of a styled-run chunk. Runs are short (a word or a few), so
// the general string_width's SIMD setup costs more than it saves; a plain
// byte loop answers the all-printable-ASCII case (the norm) and anything
// else defers to string_width, so the two can never disagree.
[[nodiscard]] inline int chunk_width(std::string_view s) noexcept {
    for (unsigned char b : s)
        if (b >= 0x80 || b < 0x20 || b == 0x7F) return string_width(s);
    return static_cast<int>(s.size());
}
}  // namespace

void paint_text(const PaintCtx& c, const TextElement& node) {
    Canvas& canvas = c.canvas;
    StylePool& pool = c.pool;
    const int ax = c.ax;
    const int ay = c.ay;
    const int aw = c.aw;
    const int ah = c.ah;
    const auto& lines = node.format(aw);

    // Ambient-bg fold: a run with no explicit bg inherits the
    // nearest enclosing box's bg (see AmbientBgScope). Without
    // this, the SGR for a bg-less style resets the cell to the
    // terminal default and the glyph punches a hole in the box's
    // fill. An explicit bg — including Color::Kind::Default as
    // the deliberate "terminal bg" opt-out — always wins.
    auto intern_bg = [&](const Style& s) -> uint16_t {
        const auto& amb = ambient_bg();
        if (amb.has_value() && !s.bg.has_value()) {
            Style folded = s;
            return pool.intern(folded.with_bg(*amb));
        }
        return pool.intern(s);
    };

    if (node.runs.empty()) {
        uint16_t style_id = intern_bg(node.style);
        // Plain counter instead of std::views::enumerate — libc++
        // (Android/Termux) doesn't ship enumerate even at C++23.
        int row = 0;
        for (const auto& line : lines) {
            if (row >= ah) break;
            canvas.write_text(ax, ay + row, line.view(node.content), style_id);
            ++row;
        }
        return;
    }

    // Truncation modes produce a single line that differs from
    // content (ellipsis appended/prepended). The word-wrap path
    // below aligns runs to wrapped output via the byte offset
    // cached in WrappedLine; truncated lines hold synthetic
    // ellipsis bytes that don't appear in content, so they take
    // this explicit-mapping branch instead.
    const bool truncates = (node.wrap == TextWrap::TruncateEnd ||
                            node.wrap == TextWrap::TruncateStart ||
                            node.wrap == TextWrap::TruncateMiddle);
    if (truncates && lines.size() == 1) {
        const std::string_view line = lines[0].view(node.content);
        const int y = ay;
        constexpr std::string_view kEll = "\xe2\x80\xa6";

        auto run_sid_at = [&](std::size_t pos) -> uint16_t {
            std::size_t ri = 0;
            while (ri + 1 < node.runs.size() &&
                   pos >= node.runs[ri].byte_offset +
                          node.runs[ri].byte_length)
                ++ri;
            return intern_bg(
                node.runs[std::min(ri, node.runs.size() - 1)].style);
        };

        // Paint line bytes [ls..le) with runs, where ls maps to
        // content byte cs. Returns x after the painted segment.
        auto paint_seg = [&](std::string_view ln,
                             std::size_t ls, std::size_t le,
                             std::size_t cs,
                             int x0, int yy) -> int {
            int xc = x0;
            std::size_t ri = 0;
            while (ri + 1 < node.runs.size() &&
                   cs >= node.runs[ri].byte_offset +
                         node.runs[ri].byte_length)
                ++ri;
            for (std::size_t b = ls; b < le; ) {
                std::size_t cb = cs + (b - ls);
                while (ri + 1 < node.runs.size() &&
                       cb >= node.runs[ri].byte_offset +
                             node.runs[ri].byte_length)
                    ++ri;
                const auto& r = node.runs[
                    std::min(ri, node.runs.size() - 1)];
                std::size_t re = r.byte_offset + r.byte_length;
                std::size_t rem = (re > cb) ? (re - cb) : 1;
                std::size_t ce = std::min(le, b + rem);
                if (ce <= b) ce = b + 1;
                // Extend to the end of the codepoint that `ce` lands
                // inside. The run walk advances by BYTES, and the
                // `rem = 1` fallback above (a run that ends at or
                // before the cursor) is exactly one byte — so a
                // multi-byte glyph got cut into pieces and each
                // fragment was written as its own cell, which the
                // terminal draws as U+FFFD. Invisible on ASCII;
                // immediate on box-drawing or CJK, e.g. a styled
                // ─── rule tearing into replacement characters at
                // the first run boundary.
                while (ce < le &&
                       (static_cast<unsigned char>(ln[ce]) & 0xC0) == 0x80)
                    ++ce;
                auto sv = std::string_view(ln).substr(b, ce - b);
                canvas.write_text(xc, yy, sv, intern_bg(r.style));
                xc += string_width(sv);
                b = ce;
            }
            return xc;
        };

        if (line == node.content) {
            paint_seg(line, 0, line.size(), 0, ax, y);
            return;
        }

        // Back up to a UTF-8 START byte. These paths slice `line`
        // by BYTES to make room for the ellipsis, and a cut landing
        // inside a multi-byte sequence emits a torn codepoint that
        // the terminal draws as U+FFFD — a line of replacement
        // characters where box-drawing was intended. ASCII text
        // never noticed; any run of ─/│/CJK does immediately.
        auto utf8_floor = [&](std::size_t pos) -> std::size_t {
            while (pos > 0 && (static_cast<unsigned char>(line[pos]) & 0xC0) == 0x80)
                --pos;
            return pos;
        };

        if (node.wrap == TextWrap::TruncateEnd &&
            line.size() >= kEll.size())
        {
            std::size_t plen = utf8_floor(line.size() - kEll.size());
            int xc = paint_seg(line, 0, plen, 0, ax, y);
            canvas.write_text(xc, y, kEll, run_sid_at(plen));
            return;
        }

        if (node.wrap == TextWrap::TruncateStart &&
            line.size() >= kEll.size())
        {
            std::size_t slen = line.size() - kEll.size();
            std::size_t M = node.content.size() - slen;
            canvas.write_text(ax, y, kEll, run_sid_at(M));
            paint_seg(line, kEll.size(), line.size(), M,
                      ax + 1, y);
            return;
        }

        uint16_t sid = intern_bg(node.style);
        canvas.write_text(ax, y, line, sid);
        return;
    }

    // Word-wrap / NoWrap with styled runs.
    //
    // Each WrappedLine carries its byte_offset into content
    // (populated by format(); see element/text.cpp). Run
    // alignment is therefore O(runs_per_line) per line instead
    // of the legacy O(content_size × lines) substring search.
    // run_idx is monotonic across lines because both lines and
    // runs are sorted by content offset.
    {
        std::size_t run_idx = 0;

        // Plain counter instead of std::views::enumerate — libc++
        // (Android/Termux) doesn't ship enumerate even at C++23.
        int row = 0;
        for (const auto& line : lines) {
            if (row >= ah) break;
            int y = ay + row;
            ++row;
            const std::string_view line_text = line.view(node.content);
            const std::size_t content_byte = line.byte_offset;

            int x_cursor = ax;
            std::size_t line_byte = 0;
            while (line_byte < line_text.size()) {
                std::size_t abs_byte = content_byte + line_byte;
                while (run_idx + 1 < node.runs.size() &&
                       abs_byte >= node.runs[run_idx].byte_offset +
                                   node.runs[run_idx].byte_length) {
                    ++run_idx;
                }

                const auto& run = node.runs[std::min(run_idx, node.runs.size() - 1)];
                std::size_t run_end = run.byte_offset + run.byte_length;
                std::size_t chunk_end;
                if (run_end > abs_byte) {
                    chunk_end = std::min(line_text.size(),
                                         line_byte + (run_end - abs_byte));
                } else {
                    chunk_end = line_byte + 1;
                }
                if (chunk_end <= line_byte) chunk_end = line_byte + 1;

                auto chunk = line_text
                                 .substr(line_byte, chunk_end - line_byte);
                uint16_t sid = intern_bg(run.style);
                canvas.write_text(x_cursor, y, chunk, sid);
                x_cursor += chunk_width(chunk);
                line_byte = chunk_end;
            }
        }
    }
}

} // namespace render_detail
} // namespace maya
