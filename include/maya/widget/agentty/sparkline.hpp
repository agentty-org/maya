#pragma once
// maya::widget::sparkline — Inline mini chart using Unicode block characters
//
// Renders a single-line bar chart from numeric data.
// Maps each value to one of 8 Unicode block levels: ▁▂▃▄▅▆▇█
//
//   CPU  ▂▃▅▇█▆▄▃▂▁▃▅▇█▆▄  12% / 98%
//
// Usage:
//   Sparkline spark({0.1, 0.3, 0.5, 0.8, 1.0, 0.6});
//   spark.set_label("CPU");
//   auto ui = spark.build();

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../../element/builder.hpp"
#include "../../style/style.hpp"

namespace maya {

// ============================================================================
// SparklineConfig — appearance configuration
// ============================================================================

struct SparklineConfig {
    Color color           = Color::slot(ThemeSlot::Primary);
    Style label_style     = Style{};
    Style value_style     = Style{}.with_dim();
    bool show_min_max     = false;
    bool show_last        = false;
    // Colour for the part of the column the value did NOT fill.
    //
    // A block glyph draws the value from the bottom and leaves the rest as
    // background, so a series that is a SHARE of something (a cache hit
    // rate) shows only the hit half and the miss half is invisible. Setting
    // this paints the cell's background, so each column reads as the two
    // parts of the whole rather than as a bar floating in space.
    //
    // Unset by default: for a series that is not a share of anything (a
    // latency trend) there is no "other half" to colour, and a background
    // behind every cell would just be a box around the trend.
    std::optional<Color> rest_color{};
};

// ============================================================================
// Sparkline — inline mini chart widget
// ============================================================================

class Sparkline {
    std::vector<float> data_;
    std::string label_;
    std::optional<float> min_override_;
    std::optional<float> max_override_;
    SparklineConfig cfg_;

    // Unicode block elements U+2581 through U+2588
    // Each is a 3-byte UTF-8 sequence: E2 96 81..88
    static constexpr const char* blocks[] = {
        "\xe2\x96\x81",  // ▁
        "\xe2\x96\x82",  // ▂
        "\xe2\x96\x83",  // ▃
        "\xe2\x96\x84",  // ▄
        "\xe2\x96\x85",  // ▅
        "\xe2\x96\x86",  // ▆
        "\xe2\x96\x87",  // ▇
        "\xe2\x96\x88",  // █
    };

public:
    explicit Sparkline(std::vector<float> data, SparklineConfig cfg = {})
        : data_(std::move(data)), cfg_(std::move(cfg)) {}

    // -- Mutators --
    void set_data(std::vector<float> data) { data_ = std::move(data); }
    void set_label(std::string_view lbl)   { label_ = std::string{lbl}; }
    void set_min(float v)                  { min_override_ = v; }
    void set_max(float v)                  { max_override_ = v; }
    void set_color(Color c)                { cfg_.color = c; }
    void set_show_min_max(bool b)          { cfg_.show_min_max = b; }
    void set_show_last(bool b)             { cfg_.show_last = b; }

    // -- Accessors --
    [[nodiscard]] const std::vector<float>& data() const { return data_; }

    // -- Node concept: build into Element --
    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        if (data_.empty()) {
            return Element{TextElement{
                .content = label_.empty() ? "(no data)" : label_ + "  (no data)",
                .style = cfg_.value_style,
            }};
        }
        // A spark is ONE CELL PER SAMPLE, so a long series is a very long
        // line — a 250-point history asked for 258 columns and the tail was
        // clipped by whatever slot it landed in. Make it width-aware: render
        // against the allocated width and show the most RECENT samples that
        // fit, which is the window a reader of a live series wants anyway.
        return Element{ComponentElement{
            .render = [self = *this](int w, int /*h*/) -> Element {
                return self.build_line(w);
            },
            .measure = [self = *this](int max_width) -> Size {
                // Natural width: the label, the gap, the samples and any
                // suffix — but never more than offered, and bounded so an
                // unbounded probe cannot be taken literally.
                const int lbl = self.label_.empty()
                    ? 0 : static_cast<int>(unicode::str_width(self.label_)) + 2;
                const int pts = static_cast<int>(self.data_.size());
                const int suffix = (self.cfg_.show_last || self.cfg_.show_min_max) ? 10 : 0;
                // Report the OFFER, not the sample count. A spark stretches
                // to fill its slot now, so pinning the natural width to the
                // number of points left a nine-point trace nine cells wide in
                // a column three times that. Still bounded, so an unbounded
                // probe cannot be taken literally.
                const int natural = lbl + std::min(pts, 64) + suffix;
                // Take the OFFER when it is a real width, so the trace
                // fills whatever column it lands in; fall back to the sample
                // count only for the unbounded measure probe, which must not
                // be taken literally.
                constexpr int kProbe = 4096;
                const int w = (max_width > 0 && max_width < kProbe) ? max_width
                                                                   : natural;
                return Size{Columns{std::max(1, w)}, Rows{1}};
            },
            .layout = {},
        }};
    }

private:
    [[nodiscard]] Element build_line(int avail) const {
        // Compute range
        float data_min = min_override_.value_or(*std::min_element(data_.begin(), data_.end()));
        float data_max = max_override_.value_or(*std::max_element(data_.begin(), data_.end()));
        float range = data_max - data_min;
        if (range < std::numeric_limits<float>::epsilon()) range = 1.0f;

        // Reserve the label and the suffix first; whatever is left is how
        // many samples we can actually draw. Both are measured EXACTLY (the
        // suffix is composed up-front) — a guessed reserve is how a block
        // ended up one cell past the edge.
        const int lbl_cells = label_.empty()
            ? 0 : static_cast<int>(unicode::str_width(label_)) + 2;

        std::string suffix;
        if (cfg_.show_min_max)
            suffix += "  " + format_float(data_min) + " / " + format_float(data_max);
        if (cfg_.show_last) {
            suffix += "  ";
            suffix += format_float(data_.back());
        }
        const int suffix_cells = static_cast<int>(unicode::str_width(suffix));

        int room = avail - lbl_cells - suffix_cells;
        if (room < 1) room = 1;

        // Fill the room, in whichever direction the data needs.
        //
        // More samples than cells: window to the MOST RECENT `room`, which
        // is the part of a live series a reader wants.
        //
        // FEWER samples than cells: stretch, drawing each sample across
        // several columns. Without this a nine-turn history drew nine cells
        // and stopped, so the trace sat as a stub in the corner of a column
        // three times its width while the histogram beside it filled the
        // same space — the two charts of one card disagreeing about how wide
        // the card was.
        std::size_t begin = 0;
        if (data_.size() > static_cast<std::size_t>(room))
            begin = data_.size() - static_cast<std::size_t>(room);
        const std::size_t shown = data_.size() - begin;
        // Spread the room across the samples EXACTLY: base cells each, with
        // the remainder handed out one at a time. Integer division alone
        // wasted the remainder — eleven cells over nine samples is one each
        // and two columns of nothing, so the trace still fell short of its
        // column by the part that did not divide.
        const int base = shown > 0 ? room / static_cast<int>(shown) : 1;
        const int extra = shown > 0 ? room % static_cast<int>(shown) : 0;

        // Build the spark characters
        std::string spark;
        spark.reserve(static_cast<std::size_t>(room + 8) * 3);
        for (std::size_t i = begin; i < data_.size(); ++i) {
            float norm = (data_[i] - data_min) / range;
            norm = std::clamp(norm, 0.0f, 1.0f);
            int level = static_cast<int>(norm * 7.0f + 0.5f);
            level = std::clamp(level, 0, 7);
            const int n = std::max(1, base)
                        + ((static_cast<int>(i - begin) < extra) ? 1 : 0);
            for (int r = 0; r < n; ++r) spark += blocks[level];
        }

        // Compose the full line with runs
        std::string content;
        std::vector<StyledRun> runs;

        // Label
        if (!label_.empty()) {
            size_t lbl_start = content.size();
            content += label_;
            content += "  ";
            runs.push_back(StyledRun{lbl_start, label_.size(), cfg_.label_style});
        }

        // Spark data. The block draws the value from the bottom in `color`;
        // when `rest_color` is set the cell BACKGROUND carries the part the
        // value did not reach, so a share reads as two parts of a whole.
        size_t spark_start = content.size();
        content += spark;
        Style spark_style = Style{}.with_fg(cfg_.color);
        if (cfg_.rest_color) spark_style = spark_style.with_bg(*cfg_.rest_color);
        runs.push_back(StyledRun{spark_start, spark.size(), spark_style});

        // Min/max/last values — composed above so the window could reserve
        // its exact width.
        if (!suffix.empty()) {
            size_t suffix_start = content.size();
            content += suffix;
            runs.push_back(StyledRun{suffix_start, suffix.size(), cfg_.value_style});
        }

        return Element{TextElement{
            .content = std::move(content),
            .style = {},
            .wrap = TextWrap::NoWrap,
            .runs = std::move(runs),
        }};
    }

private:
    static std::string format_float(float v) {
        // Simple formatting: up to 1 decimal place
        if (v == static_cast<float>(static_cast<int>(v))) {
            return std::to_string(static_cast<int>(v));
        }
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f", static_cast<double>(v));
        return buf;
    }
};

} // namespace maya
