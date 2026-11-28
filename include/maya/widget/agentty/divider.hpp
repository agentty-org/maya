#pragma once
// maya::widget::divider — Horizontal/vertical separator with optional label
//
// Usage:
//   Divider()                          // plain line
//   Divider("Section")                 // ── Section ──────
//   Divider("Title", {.line = Round})  // ╭─ Title ─╮ style

#include <string>
#include <string_view>

#include "../../dsl.hpp"
#include "../../style/border.hpp"
#include "../../style/style.hpp"

namespace maya {

struct DividerConfig {
    BorderStyle line    = BorderStyle::Single;
    Style line_style    = Style{}.with_fg(Color::slot(ThemeSlot::Muted));
    Style label_style   = Style{}.with_dim();
};

class Divider {
    std::string label_;
    DividerConfig cfg_;

public:
    Divider() = default;
    explicit Divider(std::string_view label, DividerConfig cfg = {})
        : label_(label), cfg_(cfg) {}
    explicit Divider(DividerConfig cfg) : cfg_(cfg) {}

    void set_label(std::string_view l) { label_ = std::string{l}; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        auto ch = get_border_chars(cfg_.line).top;
        if (ch.empty()) ch = "─";

        // Use ComponentElement to fill available width
        return Element{ComponentElement{
            .render = [label = label_, cfg = cfg_, ch = std::string{ch}]
                      (int w, int /*h*/) -> Element {
                if (label.empty()) {
                    std::string line;
                    for (int i = 0; i < w; ++i) line += ch;
                    return Element{TextElement{
                        .content = std::move(line),
                        .style = cfg.line_style,
                    }};
                }

                // "── Label ──────"
                int label_w = static_cast<int>(label.size()) + 2; // " Label "
                int remaining = w - label_w;
                int left_len = 2;
                int right_len = std::max(0, remaining - left_len);

                std::string left_line;
                for (int i = 0; i < left_len; ++i) left_line += ch;

                std::string right_line;
                for (int i = 0; i < right_len; ++i) right_line += ch;

                return dsl::h(
                    Element{TextElement{
                        .content = std::move(left_line),
                        .style = cfg.line_style,
                    }},
                    Element{TextElement{
                        .content = " " + label + " ",
                        .style = cfg.label_style,
                    }},
                    Element{TextElement{
                        .content = std::move(right_line),
                        .style = cfg.line_style,
                    }}
                ).build();
            },
            .layout = {},
        }};
    }
};

} // namespace maya
