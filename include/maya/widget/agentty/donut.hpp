#pragma once
// maya::Donut — a composition drawn as a braille ring (a pie/donut chart).
//
// The parts of a whole, drawn around a closed loop so the eye compares
// ANGLES rather than segment lengths — a thin wedge against a circle is
// obvious in a way a thin segment against a bar is not. Use it when the
// composition IS the answer.
//
// Braille rather than half-blocks: a braille cell is 2 dots wide and 4
// tall, and against a terminal cell's ~1:2 aspect that makes the dots very
// nearly SQUARE, so a circle in dot space is a circle on screen with no
// correction — and the ring is four times denser.
//
// Usage:
//   Donut d;
//   d.segment("cache", 72, Color::green())
//    .segment("miss",  28, Color::red())
//    .center("72%").caption("cache hits").rows(7);
//   Element ui = d.build();
//
// Standalone: sizes to whatever width the layout gives it (via fill()), and
// occupies `rows` terminal rows (+1 for a caption).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "../../dsl.hpp"
#include "../../element/element.hpp"
#include "../../element/text.hpp"
#include "../../style/color.hpp"
#include "../../style/style.hpp"
#include "../../style/theme.hpp"
#include "../../text/unicode_width.hpp"

namespace maya {

class Donut {
public:
    struct Seg {
        std::string label;
        double      value = 0;
        Color       hue;
    };

    Donut() = default;
    explicit Donut(std::vector<Seg> segs) : segments_(std::move(segs)) {}

    Donut& segment(std::string label, double value, Color hue) {
        segments_.push_back({std::move(label), value, hue});
        return *this;
    }
    Donut& rows(int r)              { rows_ = r; return *this; }
    Donut& caption(std::string c)   { caption_ = std::move(c); return *this; }
    Donut& center(std::string c)    { center_ = std::move(c); return *this; }
    Donut& label_color(Color c)     { label_color_ = c; return *this; }
    Donut& center_color(Color c)    { center_color_ = c; return *this; }

    operator Element() const { return build(); }

    // Rows this figure paints — a caller sizing a slot can ask ahead.
    [[nodiscard]] int height() const {
        if (segments_.empty()) return 1;
        const int r = std::clamp(rows_, 3, 16);
        return caption_.empty() ? r : r + 1;
    }

    [[nodiscard]] Element build() const {
        double total = 0;
        for (const auto& s : segments_) total += s.value > 0 ? s.value : 0;
        if (segments_.empty() || total <= 0) return Element{TextElement{}};

        const int   rows = std::clamp(rows_, 3, 16);
        const Color help = label_color_;
        const Color val  = center_color_;

        std::vector<Element> lines;

        if (!caption_.empty())
            lines.push_back(Element{TextElement{
                .content = caption_,
                .style   = Style{}.with_fg(help),
                .wrap    = TextWrap::TruncateEnd}});

        auto segs = segments_;
        const std::string center = center_;
        const int nseg = static_cast<int>(segs.size());

        lines.push_back(detail::adapt([segs, total, rows, center, help, val, nseg](int w) {
            // Layout picks itself from the width, in three tiers:
            //   HORIZONTAL  full ring beside the legend            (wide)
            //   VERTICAL    full ring on top, legend stacked below  (narrow)
            //   LEGEND-ONLY just the labels                        (tiny)
            // The ring only shrinks radius when even the vertical stack's
            // width can't hold it; it never cramps beside the legend.
            int legend_min = 0;
            for (const auto& s : segs)
                legend_min = std::max(legend_min, unicode::str_width(s.label) + 4);

            int ring_rows = rows;
            auto ring_w = [&] { return ring_rows * 2; };

            // Stay HORIZONTAL only when the column is wide enough for a
            // GENEROUS ring AND the legend side by side. A small ring
            // crammed next to the labels reads worse than a big ring stacked
            // over them, so unless there is room for a comfortably large ring
            // (≈ rows+2 tall) plus the legend and a gap, flow VERTICAL.
            const int horiz_ring_w = (rows + 2) * 2;
            const int horiz_need   = horiz_ring_w + 4 + legend_min;
            const bool horizontal  = horiz_need <= w;

            bool with_ring = true;
            if (horizontal) {
                // Use the room the horizontal test reserved: grow the ring to
                // fill the space left of the legend, so it isn't a tiny hoop
                // beside big labels.
                const int cap = std::max(rows + 2, rows);
                while ((ring_rows + 1) * 2 + 4 + legend_min <= w && ring_rows < cap)
                    ++ring_rows;
            } else {
                // VERTICAL: the ring has the whole width to itself, so GROW
                // it to fill the column (a ring cell is 2 wide, so the radius
                // rises one row per two columns), capped so a tall terminal
                // doesn't get a giant circle. Then shrink back if somehow
                // still too wide, and drop below a legible minimum.
                const int cap = std::max(rows, 9);
                while (ring_w() + 2 <= w && ring_rows < cap) ++ring_rows;
                while (ring_w() > w && ring_rows > 3) --ring_rows;
                with_ring = (ring_w() <= w && ring_rows >= 3);
                if (!with_ring) { ring_rows = 0; }
            }
            const int cw = ring_w();

            // Cumulative segment edges as fractions of the circle.
            std::vector<double> edge;
            edge.reserve(segs.size() + 1);
            double acc = 0;
            edge.push_back(0.0);
            for (const auto& s : segs) {
                acc += (s.value > 0 ? s.value : 0) / total;
                edge.push_back(acc);
            }

            constexpr double kPi = 3.14159265358979323846;
            const int dot_w = cw * 2;
            const int dot_h = ring_rows * 4;
            const double cx = (dot_w - 1) / 2.0;
            const double cy = (dot_h - 1) / 2.0;
            const double r_out = static_cast<double>(dot_h) / 2.0;
            // A constant-ISH band thickness rather than a constant ratio: a
            // fixed 0.62 ratio makes the hole grow with the ring, so a large
            // ring is mostly empty. Instead take a generous fraction of the
            // radius as the band and keep the hole just big enough for the
            // centre text — clamp the inner radius so the ring stays a fat
            // annulus at any size.
            const double ctr_need = center.empty()
                ? 0.0
                : (unicode::str_width(center) + 2) * 2.0;   // dots to fit text
            double r_in = r_out * 0.45;                      // thick band
            r_in = std::max(r_in, ctr_need / 2.0);           // but fit the label
            r_in = std::min(r_in, r_out - 2.0);              // never a hairline

            auto seg_at = [&](int dx, int dy) -> int {
                const double x = dx - cx, y = dy - cy;
                const double dist = std::sqrt(x * x + y * y);
                if (dist < r_in || dist > r_out) return -1;
                double a = std::atan2(x, -y) / (2 * kPi);
                if (a < 0) a += 1.0;
                for (std::size_t i = 0; i + 1 < edge.size(); ++i)
                    if (a >= edge[i] && a < edge[i + 1]) return static_cast<int>(i);
                return static_cast<int>(segs.size()) - 1;
            };

            static constexpr std::uint8_t kDot[4][2] = {
                {0x01, 0x08}, {0x02, 0x10}, {0x04, 0x20}, {0x40, 0x80},
            };
            const int mid = with_ring ? ring_rows / 2 : -1;

            // One ring row (cyc in [0, ring_rows)) as text + runs.
            auto ring_row = [&](int cyc, std::string& s, std::vector<StyledRun>& runs) {
                const bool is_mid = (cyc == mid) && !center.empty();
                const int ctr_w = is_mid ? unicode::str_width(center) : 0;
                const int ctr_x = is_mid ? (cw - ctr_w) / 2 : -1;
                const int ctr_lo = is_mid ? ctr_x - 1 : -1;
                const int ctr_hi = is_mid ? ctr_x + ctr_w + 1 : -1;
                for (int cxc = 0; cxc < cw; ++cxc) {
                    if (is_mid && cxc >= ctr_lo && cxc < ctr_hi) {
                        if (cxc == ctr_x) {
                            const std::size_t at = s.size();
                            s += center;
                            runs.push_back({at, s.size() - at,
                                            Style{}.with_fg(val).with_bold()});
                        } else if (cxc < ctr_x || cxc >= ctr_x + ctr_w) {
                            s += ' ';
                        }
                        continue;
                    }
                    std::uint8_t bits = 0;
                    int votes[16] = {0};
                    for (int dy = 0; dy < 4; ++dy)
                        for (int dx = 0; dx < 2; ++dx) {
                            const int sg = seg_at(cxc * 2 + dx, cyc * 4 + dy);
                            if (sg < 0) continue;
                            bits |= kDot[dy][dx];
                            if (sg < 16) ++votes[sg];
                        }
                    if (!bits) { s += ' '; continue; }
                    int best = 0;
                    for (int i = 1; i < 16; ++i) if (votes[i] > votes[best]) best = i;
                    const char32_t cp = 0x2800u + bits;
                    const std::size_t at = s.size();
                    s += static_cast<char>(0xE0 | (cp >> 12));
                    s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    s += static_cast<char>(0x80 | (cp & 0x3F));
                    runs.push_back({at, s.size() - at,
                                    Style{}.with_fg(segs[static_cast<std::size_t>(best)
                                                         % segs.size()].hue)});
                }
            };

            // One legend row (■ + label) appended to s.
            auto legend_row = [&](int i, std::string& s, std::vector<StyledRun>& runs) {
                const auto& sg = segs[static_cast<std::size_t>(i)];
                const std::size_t at = s.size();
                s += "\xe2\x96\xa0 ";                                  // ■
                runs.push_back({at, s.size() - at, Style{}.with_fg(sg.hue)});
                const std::size_t tat = s.size();
                s += sg.label;
                runs.push_back({tat, s.size() - tat, Style{}.with_fg(help)});
            };

            auto emit = [](std::string s, std::vector<StyledRun> runs) {
                return Element{TextElement{.content = std::move(s),
                                           .wrap    = TextWrap::TruncateEnd,
                                           .runs    = std::move(runs)}};
            };

            std::vector<Element> out;

            if (horizontal) {
                // Ring beside legend, each on the same row, legend column
                // padded to the ring gutter so it aligns.
                const int total_rows = std::max(ring_rows, nseg);
                for (int cyc = 0; cyc < total_rows; ++cyc) {
                    std::string s; std::vector<StyledRun> runs;
                    if (with_ring && cyc < ring_rows) ring_row(cyc, s, runs);
                    if (cyc < nseg) {
                        while (static_cast<int>(unicode::str_width(s)) < cw) s += ' ';
                        s += "  ";
                        legend_row(cyc, s, runs);
                    }
                    out.push_back(emit(std::move(s), std::move(runs)));
                }
            } else {
                // VERTICAL: ring block on top (centred), then the legend
                // rows beneath it. Nothing is cramped side by side.
                if (with_ring) {
                    for (int cyc = 0; cyc < ring_rows; ++cyc) {
                        std::string s; std::vector<StyledRun> runs;
                        ring_row(cyc, s, runs);
                        out.push_back(emit(std::move(s), std::move(runs)));
                    }
                }
                for (int i = 0; i < nseg; ++i) {
                    std::string s; std::vector<StyledRun> runs;
                    legend_row(i, s, runs);
                    out.push_back(emit(std::move(s), std::move(runs)));
                }
            }
            return dsl::v(std::move(out)).build();
        }).build());

        return dsl::v(std::move(lines)).build();
    }

private:
    std::vector<Seg> segments_;
    int              rows_ = 7;
    std::string      caption_;
    std::string      center_;
    Color            label_color_  = Color::slot(ThemeSlot::Muted);
    Color            center_color_ = Color::slot(ThemeSlot::Text);
};

} // namespace maya
