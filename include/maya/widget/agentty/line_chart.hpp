#pragma once
// maya::widget::line_chart — Braille-dot line chart
//
// Non-interactive chart that renders a data series using braille characters
// (U+2800-U+28FF) for 2x4 sub-cell resolution per terminal cell.
//
// Usage:
//   LineChart chart({1.0f, 3.5f, 2.0f, 7.0f, 4.5f, 6.0f});
//   chart.set_height(8);
//   auto ui = chart.build();

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "../../dsl.hpp"
#include "../../style/style.hpp"

namespace maya {

// ============================================================================
// LineChart — braille-dot line chart
// ============================================================================

class LineChart {
    std::vector<float> data_;
    int height_ = 8;
    std::string label_;
    Color color_ = Color::slot(ThemeSlot::Primary);
    std::optional<float> floor_{};
    bool fill_ = false;

public:
    LineChart() = default;

    explicit LineChart(std::vector<float> data, int height = 8)
        : data_(std::move(data)), height_(height) {}

    void set_data(std::vector<float> data) { data_ = std::move(data); }
    void set_height(int h) { height_ = h; }
    void set_label(std::string_view l) { label_ = std::string{l}; }
    void set_color(Color c) { color_ = c; }
    // Pin the bottom of the axis.
    //
    // The range is the data's own min..max, which is right for a series
    // whose SHAPE is the question and wrong for one whose LEVEL is: a
    // context window sitting at 2400 tokens drew an axis of 2400..2401 and
    // a flat line through the middle of it, all noise and no magnitude.
    // Pinning the floor at zero makes the height mean the quantity.
    void set_floor(float v) { floor_ = v; }
    // Shade the area under the curve.
    //
    // A single trace across a tall plot is mostly empty space, and the eye
    // reads the EMPTINESS as the data. Filling underneath turns the same
    // series into an area chart, where the quantity is the mass and the
    // plot is full at the scale it is drawn.
    void set_fill(bool on) { fill_ = on; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        return Element{ComponentElement{
            .render = [self = *this](int w, int h) -> Element {
                return self.build_chart(w, h);
            },
            // Without a measure callback the engine auto-measures by running
            // render at whatever width it probes with, and then pins the
            // component to whatever that produced — so the curve sat at a
            // fraction of its column with dead space beside it, while the
            // histogram under it filled the same card. Report the OFFER, so
            // the chart takes the column it is given; fall back to a nominal
            // width for the unbounded probe, which must not be taken
            // literally.
            .measure = [self = *this](int max_width) -> Size {
                constexpr int kProbe = 4096;
                constexpr int kNominal = 48;
                const int w = (max_width > 0 && max_width < kProbe) ? max_width
                                                                    : kNominal;
                return Size{Columns{std::max(1, w)},
                            Rows{std::max(1, self.height_)}};
            },
            .layout = {},
        }};
    }

private:
    // Braille dot positions within a 2x4 cell:
    //   Col 0  Col 1
    //   0x01   0x08   row 0
    //   0x02   0x10   row 1
    //   0x04   0x20   row 2
    //   0x40   0x80   row 3
    static constexpr uint8_t braille_dots[4][2] = {
        {0x01, 0x08},
        {0x02, 0x10},
        {0x04, 0x20},
        {0x40, 0x80},
    };

    [[nodiscard]] Element build_chart(int total_width, int total_height = -1) const {
        if (data_.empty()) {
            return Element{TextElement{.content = ""}};
        }

        // Responsive height: fill the vertical space the container allocates
        // when it is a sane, definite value. The layout engine hands a
        // component the "unconstrained" sentinel (~1<<24) during auto-height
        // measure passes — in that case (or when no height was supplied)
        // fall back to the configured height_. A 1-row floor keeps the grid
        // valid. This is what lets a chart in a grow=1 / stretched row expand
        // and shrink with the terminal instead of being pinned to height_.
        constexpr int kMaxChartHeight = 256;
        int chart_rows = height_;
        if (total_height > 0 && total_height <= kMaxChartHeight) {
            chart_rows = total_height;
        }
        if (chart_rows < 1) chart_rows = 1;

        // Find data range
        float min_val = *std::min_element(data_.begin(), data_.end());
        float max_val = *std::max_element(data_.begin(), data_.end());
        if (floor_) min_val = std::min(*floor_, min_val);
        if (max_val == min_val) {
            max_val = min_val + 1.0f;
        }

        // Y-axis labels: show min and max
        char min_buf[16], max_buf[16];
        std::snprintf(max_buf, sizeof(max_buf), "%.1f", static_cast<double>(max_val));
        std::snprintf(min_buf, sizeof(min_buf), "%.1f", static_cast<double>(min_val));

        std::string max_label = max_buf;
        std::string min_label = min_buf;
        int label_width = static_cast<int>(std::max(max_label.size(), min_label.size()));

        // Chart area: 2 columns for each braille cell
        int y_axis_width = label_width + 2;  // label + " │"
        // Cap total_width against the layout engine's "unconstrained"
        // measure sentinel (~1<<24). A chart asked to size itself for an
        // auto-height / auto-width measure pass receives that sentinel as
        // its width; without this cap chart_cols becomes astronomical, the
        // data is spread across a multi-million-column grid, and only the
        // leftmost sliver lands in the visible region — the rest of the
        // panel reads as empty. 4096 columns is far past any real terminal
        // yet keeps the grid finite so the visible width fills normally.
        constexpr int kMaxChartWidth = 4096;
        int effective_width = std::min(total_width, kMaxChartWidth);
        int chart_cols = effective_width - y_axis_width;
        if (chart_cols < 4) chart_cols = 4;

        // Grid dimensions: each braille cell is 2 dots wide, 4 dots tall
        int grid_w = chart_cols * 2;
        int grid_h = chart_rows * 4;

        // Map data points to grid x-coordinates, compute y for each grid x
        // by linear interpolation of the data
        auto sample = [&](int gx) -> float {
            if (data_.size() == 1) return data_[0];
            float t = static_cast<float>(gx) / static_cast<float>(grid_w - 1);
            float idx = t * static_cast<float>(data_.size() - 1);
            int lo = static_cast<int>(idx);
            int hi = std::min(lo + 1, static_cast<int>(data_.size()) - 1);
            float frac = idx - static_cast<float>(lo);
            return data_[static_cast<size_t>(lo)] * (1.0f - frac)
                 + data_[static_cast<size_t>(hi)] * frac;
        };

        // Build braille grid: chart_rows rows x chart_cols columns
        // Each cell is a uint8_t bitmask for the 8 braille dots
        std::vector<std::vector<uint8_t>> grid(
            static_cast<size_t>(chart_rows),
            std::vector<uint8_t>(static_cast<size_t>(chart_cols), 0)
        );

        // Plot: for each grid-x, find grid-y and draw a SEGMENT from the
        // previous point.
        //
        // Setting one dot per column draws the samples, not the line: where
        // consecutive values differ by more than a dot the marks stop
        // touching, and a spiky series (tokens per turn) renders as a field
        // of specks with no shape to read. Filling the span between the
        // previous y and this one is what makes it a LINE chart — and it
        // costs nothing, since the dots are already there to be set.
        auto dot_at = [&](int gx, int gy) {
            const int cell_col = gx / 2;
            const int dot_col  = gx % 2;
            const int cell_row = gy / 4;
            const int dot_row  = gy % 4;
            grid[static_cast<size_t>(cell_row)][static_cast<size_t>(cell_col)]
                |= braille_dots[dot_row][dot_col];
        };

        const bool self_fill = fill_;
        int prev_gy = -1;
        for (int gx = 0; gx < grid_w; ++gx) {
            float val = sample(gx);
            float norm = (val - min_val) / (max_val - min_val);
            norm = std::clamp(norm, 0.0f, 1.0f);

            // Map to grid row (0 = top, grid_h-1 = bottom)
            int gy = grid_h - 1 - static_cast<int>(std::round(norm * static_cast<float>(grid_h - 1)));
            gy = std::clamp(gy, 0, grid_h - 1);

            if (prev_gy >= 0 && prev_gy != gy) {
                const int step = prev_gy < gy ? 1 : -1;
                for (int y = prev_gy + step; y != gy; y += step) dot_at(gx, y);
            }
            dot_at(gx, gy);
            // Area fill: every dot from the curve down to the baseline.
            if (self_fill) for (int y = gy + 1; y < grid_h; ++y) dot_at(gx, y);
            prev_gy = gy;
        }

        // Render rows
        std::vector<Element> rows;
        rows.reserve(static_cast<size_t>(chart_rows));

        for (int row = 0; row < chart_rows; ++row) {
            std::string content;
            std::vector<StyledRun> runs;

            // Y-axis label (only on first and last row)
            std::string y_label;
            if (row == 0) {
                y_label = max_label;
            } else if (row == chart_rows - 1) {
                y_label = min_label;
            }

            // Right-pad label to label_width
            while (static_cast<int>(y_label.size()) < label_width) {
                y_label = " " + y_label;
            }

            runs.push_back(StyledRun{
                content.size(), y_label.size(),
                Style{}.with_dim(),
            });
            content += y_label;

            // Separator
            std::string sep = " \xe2\x94\x82";  // " │"
            runs.push_back(StyledRun{
                content.size(), sep.size(),
                Style{}.with_fg(Color::slot(ThemeSlot::Muted)),
            });
            content += sep;

            // Braille characters
            std::string braille_str;
            for (int col = 0; col < chart_cols; ++col) {
                uint8_t bits = grid[static_cast<size_t>(row)][static_cast<size_t>(col)];
                // Braille base: U+2800
                char32_t cp = U'\u2800' + bits;
                // Encode UTF-8
                if (cp <= 0x7F) {
                    braille_str += static_cast<char>(cp);
                } else if (cp <= 0x7FF) {
                    braille_str += static_cast<char>(0xC0 | (cp >> 6));
                    braille_str += static_cast<char>(0x80 | (cp & 0x3F));
                } else if (cp <= 0xFFFF) {
                    braille_str += static_cast<char>(0xE0 | (cp >> 12));
                    braille_str += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    braille_str += static_cast<char>(0x80 | (cp & 0x3F));
                }
            }

            runs.push_back(StyledRun{
                content.size(), braille_str.size(),
                Style{}.with_fg(color_),
            });
            content += braille_str;

            rows.push_back(Element{TextElement{
                .content = std::move(content),
                .style = {},
                .wrap = TextWrap::NoWrap,
                .runs = std::move(runs),
            }});
        }

        // Optional label at the bottom
        if (!label_.empty()) {
            std::string pad(static_cast<size_t>(y_axis_width), ' ');
            std::string label_line = pad + label_;

            std::vector<StyledRun> label_runs;
            label_runs.push_back(StyledRun{
                static_cast<size_t>(y_axis_width), label_.size(),
                Style{}.with_dim(),
            });

            rows.push_back(Element{TextElement{
                .content = std::move(label_line),
                .style = {},
                .wrap = TextWrap::NoWrap,
                .runs = std::move(label_runs),
            }});
        }

        return dsl::v(std::move(rows)).build();
    }
};

} // namespace maya
