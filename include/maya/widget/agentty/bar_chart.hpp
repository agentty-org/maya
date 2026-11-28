#pragma once
// maya::widget::bar_chart — Horizontal bar chart with proportional block bars
//
// Non-interactive chart that renders labeled horizontal bars using █ characters.
//
// Usage:
//   BarChart chart({
//       {"CPU",    0.75f},
//       {"Memory", 0.45f},
//       {"Disk",   0.90f, Color::red()},
//   });
//   auto ui = chart.build();

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "../../dsl.hpp"
#include "../../style/style.hpp"

namespace maya {

// ============================================================================
// Bar — a single data bar
// ============================================================================

struct Bar {
    std::string label;
    float value = 0.0f;
    std::optional<Color> color{};
};

// ============================================================================
// BarChart — non-interactive horizontal bar chart
// ============================================================================

// Format a bar's value: no decimal when the number is whole.
//
// "%.1f" on a tally of 5 calls prints "5.0", which invites the reader to
// wonder what the tenth of a call was. A fraction still shows one place.
inline std::string bar_value_text(float v) {
    char buf[32];
    const double d = static_cast<double>(v);
    if (d == static_cast<double>(static_cast<long long>(d)))
        std::snprintf(buf, sizeof(buf), "  %lld", static_cast<long long>(d));
    else
        std::snprintf(buf, sizeof(buf), "  %.1f", d);
    return buf;
}

class BarChart {
    std::vector<Bar> bars_;
    float max_value_ = 0.0f;  // 0 = auto-detect from data
    Color default_color_ = Color::slot(ThemeSlot::Primary);

public:
    BarChart() = default;

    explicit BarChart(std::vector<Bar> bars, float max_value = 0.0f)
        : bars_(std::move(bars)), max_value_(max_value) {}

    void set_bars(std::vector<Bar> bars) { bars_ = std::move(bars); }
    void set_max_value(float v) { max_value_ = v; }
    void set_default_color(Color c) { default_color_ = c; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        // Use ComponentElement for dynamic width
        return Element{ComponentElement{
            .render = [self = *this](int w, int /*h*/) -> Element {
                return self.build_chart(w);
            },
            // WITHOUT a measure callback the engine auto-measures by running
            // render() at whatever it probes with — and a scroll/prebuilt
            // body probes with an UNBOUNDED width (1<<14). This widget fills
            // its slot, so it answered "sixteen thousand columns wide", and
            // when it was then painted in the real ~36-column body the value
            // suffix at the far end fell off the edge.
            //
            // So state a bounded NATURAL width: what the chart wants is its
            // longest label, the gap, a bar worth looking at, and the value.
            // It still FILLS a wider slot (render uses the allocated w) —
            // this only stops an unbounded probe from being taken literally.
            .measure = [self = *this](int max_width) -> Size {
                std::size_t longest = 0;
                for (const auto& b : self.bars_)
                    longest = std::max(longest, b.label.size());
                constexpr int kGap = 2, kValue = 8, kNiceBar = 24;
                const int natural =
                    static_cast<int>(longest) + kGap + kNiceBar + kValue;
                const int w = max_width > 0 ? std::min(max_width, natural) : natural;
                return Size{Columns{w},
                            Rows{static_cast<int>(self.bars_.size())}};
            },
            .layout = {},
        }};
    }

private:
    [[nodiscard]] Element build_chart(int width) const {
        if (bars_.empty()) {
            return Element{TextElement{.content = ""}};
        }

        // Find max value for scaling
        float max_val = max_value_;
        if (max_val <= 0.0f) {
            for (const auto& b : bars_) {
                max_val = std::max(max_val, b.value);
            }
        }
        if (max_val <= 0.0f) max_val = 1.0f;

        // Find longest label for right-alignment
        std::size_t max_label = 0;
        for (const auto& b : bars_) {
            max_label = std::max(max_label, b.label.size());
        }

        // Value suffix width. Measure the WIDEST value actually formatted
        // rather than assuming 8: a guessed reserve is how a bar ended up a
        // cell or two past the slot edge, and the block at the end of the
        // longest row was what got clipped.
        int value_suffix_width = 0;
        for (const auto& b : bars_) {
            value_suffix_width =
                std::max(value_suffix_width,
                         static_cast<int>(bar_value_text(b.value).size()));
        }
        if (value_suffix_width < 4) value_suffix_width = 4;
        constexpr int label_gap = 2;  // gap between label and bar
        constexpr int min_bar = 4;

        // The label column must YIELD so the row fits. Flooring bar_width at
        // 4 without also capping the label let
        //     label + gap + 4 + value
        // exceed `width` whenever the label was long (a model id is ~17
        // cells), and the value suffix — the number the chart exists to
        // report — was what fell off the right edge. Cap the label to what
        // is left after the bar and the value, and truncate into it.
        const int budget = width - label_gap - min_bar - value_suffix_width;
        const int label_cap = std::max(0, budget);
        if (static_cast<int>(max_label) > label_cap)
            max_label = static_cast<std::size_t>(label_cap);

        int bar_width = width - static_cast<int>(max_label) - label_gap - value_suffix_width;
        // If even the floor does not fit, the row must still not overrun:
        // a 4-cell bar forced into a column that has 2 left is how a block
        // ended up painted past the edge. Give the bar whatever remains —
        // possibly nothing — and let the label and value carry the row.
        if (bar_width < min_bar)
            bar_width = std::max(0, width - static_cast<int>(max_label)
                                        - label_gap - value_suffix_width);

        std::vector<Element> rows;
        rows.reserve(bars_.size());

        for (const auto& b : bars_) {
            Color bar_color = b.color.value_or(default_color_);
            float ratio = std::clamp(b.value / max_val, 0.0f, 1.0f);
            int filled = static_cast<int>(std::round(ratio * static_cast<float>(bar_width)));

            // Right-align label, truncating to the capped column so a long
            // name can never push the value off the row.
            std::string lbl = b.label;
            if (lbl.size() > max_label) {
                // Truncate on a UTF-8 boundary, leaving room for an ellipsis.
                std::size_t cut = max_label > 1 ? max_label - 1 : 0;
                while (cut > 0 && (static_cast<unsigned char>(lbl[cut]) & 0xC0) == 0x80) --cut;
                lbl = lbl.substr(0, cut);
                if (max_label > 0) lbl += "\xe2\x80\xa6";   // …
            }
            const std::size_t lbl_cells = lbl.size() > max_label ? max_label : lbl.size();
            std::string padded_label(max_label - std::min(max_label, lbl_cells), ' ');
            padded_label += lbl;

            // Build the line: "  label  ████████────  value"
            std::string content;
            std::vector<StyledRun> runs;

            // Label
            runs.push_back(StyledRun{
                content.size(), padded_label.size(),
                Style{},
            });
            content += padded_label;

            // Gap
            content += "  ";

            // Filled bar
            std::string bar_str;
            for (int i = 0; i < filled; ++i)
                bar_str += "\xe2\x96\x88";  // █
            if (!bar_str.empty()) {
                runs.push_back(StyledRun{
                    content.size(), bar_str.size(),
                    Style{}.with_fg(bar_color),
                });
                content += bar_str;
            }

            // Empty track
            int empty = bar_width - filled;
            if (empty > 0) {
                std::string track;
                for (int i = 0; i < empty; ++i)
                    track += "\xe2\x94\x80";  // ─
                runs.push_back(StyledRun{
                    content.size(), track.size(),
                    Style{}.with_fg(Color::slot(ThemeSlot::Muted)),
                });
                content += track;
            }

            // Value suffix
            std::string val_str = bar_value_text(b.value);
            runs.push_back(StyledRun{
                content.size(), val_str.size(),
                Style{}.with_dim(),
            });
            content += val_str;

            rows.push_back(Element{TextElement{
                .content = std::move(content),
                .style = {},
                .wrap = TextWrap::NoWrap,
                .runs = std::move(runs),
            }});
        }

        return dsl::v(std::move(rows)).build();
    }
};

} // namespace maya
