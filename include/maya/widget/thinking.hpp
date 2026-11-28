#pragma once
// maya::widget::thinking — Collapsible thinking/reasoning block
//
// Zed's left-gutter bordered content + Claude Code's spinner and streaming.
//
//   Thinking...  ⠋  ▼              (active, with spinner)
//   │ Analyzing the request...
//   │ Reading project structure...
//
//   Thinking  ▶                    (collapsed)

#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../style/style.hpp"
#include "agentty/spinner.hpp"

namespace maya {

class ThinkingBlock {
    std::string content_;
    bool active_ = false;
    bool expanded_ = true;
    Spinner<SpinnerStyle::Dots> spinner_{Style{}.with_dim()};
    int max_visible_lines_ = 0;  // 0 = unlimited

public:
    ThinkingBlock() = default;

    void set_active(bool active) { active_ = active; }
    void set_expanded(bool e) { expanded_ = e; }
    void toggle() { expanded_ = !expanded_; }
    void set_max_visible_lines(int n) { max_visible_lines_ = n; }

    void set_content(std::string_view text) { content_ = std::string{text}; }
    void append(std::string_view text) { content_ += text; }

    [[nodiscard]] bool is_active() const { return active_; }
    [[nodiscard]] bool is_expanded() const { return expanded_; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        auto header = build_header();

        if (!expanded_ || content_.empty()) {
            return header;
        }

        // Expanded: header + gutter-prefixed content lines
        std::vector<Element> elems;
        elems.push_back(std::move(header));

        auto gutter_style = Style{}.with_fg(Color::slot(ThemeSlot::Muted));
        auto text_style = Style{}.with_italic().with_dim();

        std::string_view sv = content_;
        int line_count = 0;

        while (!sv.empty()) {
            auto nl = sv.find('\n');
            auto line = (nl == std::string_view::npos) ? sv : sv.substr(0, nl);
            sv = (nl == std::string_view::npos) ? std::string_view{} : sv.substr(nl + 1);

            if (max_visible_lines_ > 0 && line_count >= max_visible_lines_) {
                elems.push_back(Element{TextElement{
                    .content = "\xe2\x94\x82 \xe2\x80\xa6",  // "│ …"
                    .style = gutter_style,
                }});
                break;
            }

            std::string bordered = "\xe2\x94\x82 ";  // "│ "
            std::vector<StyledRun> runs;

            runs.push_back(StyledRun{0, 4, gutter_style});  // "│ " = 3+1 bytes

            std::size_t text_start = bordered.size();
            bordered += line;

            if (!line.empty()) {
                runs.push_back(StyledRun{text_start, line.size(), text_style});
            }

            elems.push_back(Element{TextElement{
                .content = std::move(bordered),
                .style = {},
                .wrap = TextWrap::Wrap,
                .runs = std::move(runs),
            }});
            ++line_count;
        }

        return dsl::v(std::move(elems)).build();
    }

private:
    [[nodiscard]] Element build_header() const {
        std::string chevron = expanded_
            ? "  \xe2\x96\xbc"   // "  ▼"
            : "  \xe2\x96\xb6";  // "  ▶"

        auto label_style = Style{}.with_dim();
        auto chev_style = Style{}.with_dim();

        if (active_) {
            // Active: "Thinking..." + spinner + chevron
            return dsl::h(
                Element{TextElement{
                    .content = "Thinking... ",
                    .style = label_style,
                    .wrap = TextWrap::NoWrap,
                }},
                spinner_.build(),
                Element{TextElement{
                    .content = chevron,
                    .style = chev_style,
                    .wrap = TextWrap::NoWrap,
                }}
            ).build();
        }

        // Inactive: single text element
        std::string content = "Thinking" + chevron;
        std::vector<StyledRun> runs;
        runs.push_back(StyledRun{0, 8, label_style});  // "Thinking"
        runs.push_back(StyledRun{8, chevron.size(), chev_style});

        return Element{TextElement{
            .content = std::move(content),
            .style = {},
            .wrap = TextWrap::NoWrap,
            .runs = std::move(runs),
        }};
    }
};

} // namespace maya
