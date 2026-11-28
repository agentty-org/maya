#pragma once
// maya::Line — one row of a pane, described as data.
//
// A host fills a Panel's header / footer / prebuilt rows, or any pane's
// prose, with Lines instead of building Elements: a run of styled spans, a
// blank row, or a rule. Line converts to Element, so it drops straight into
// any `std::vector<Element>` a widget Config takes.
//
//   cfg.header.push_back(maya::Line{{"$ ", bold_accent}, {cmd, plain}});
//   cfg.footer.push_back(maya::Line::blank());
//   cfg.header.push_back(maya::Line::rule());
//
// A single span is a plain text row (wraps at the pane width); several sit
// side by side, each wrapping in what the ones before it leave -- which is
// how a hanging margin works: {{"  "}, {sentence, st}}.

#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "../../dsl.hpp"
#include "../../element/builder.hpp"
#include "../../element/text.hpp"
#include "../../style/style.hpp"

namespace maya {

struct Span {
    std::string text;
    Style       style{};
    TextWrap    wrap     = TextWrap::Wrap;
    int         pad_left = 0;    // columns of margin before a wrapping span
    bool        fill     = false; // take the row's leftover width, clipped
    // Per-byte styles over `text` (search hits, output tokens, inline bold):
    // only honoured on a `fill` span of a Line::row.
    std::vector<StyledRun> runs{};
};

class Line {
public:
    Line() = default;                                    // a blank row
    Line(std::string t, Style s = {}, TextWrap w = TextWrap::Wrap)
        : spans_{Span{std::move(t), s, w}} {}
    Line(std::initializer_list<Span> s) : spans_(s) {}
    explicit Line(std::vector<Span> s) : spans_(std::move(s)) {}

    [[nodiscard]] static Line blank() { return {}; }
    [[nodiscard]] static Line rule()  { Line l; l.rule_ = true; return l; }
    // Exactly ONE terminal row, full width: a span marked `fill` takes the
    // leftover columns and its tail is clipped. For line-numbered output
    // where a long line must never wrap into the next number's row.
    [[nodiscard]] static Line row(std::vector<Span> s) { Line l{std::move(s)}; l.one_row_ = true; return l; }
    // A non-wrapping prefix and a detail that wraps beside it, hanging two
    // columns in: "  !! line 12  the detail, wrapping under itself".
    [[nodiscard]] static Line hanging(std::string prefix, std::string detail, Style s) {
        return Line{Span{std::move(prefix), s, TextWrap::NoWrap},
                    Span{std::move(detail), s, TextWrap::Wrap, 2}};
    }
    // One row whose tail is cut at the width, styled per byte range: a dense
    // provenance or metadata line that must never take a second row.
    [[nodiscard]] static Line clipped(std::string t, std::vector<StyledRun> runs = {}, Style s = {}) {
        Line l;
        l.spans_.push_back(Span{std::move(t), s, TextWrap::TruncateEnd, 0, false, std::move(runs)});
        l.clipped_ = true;
        return l;
    }
    // Several Lines as one flush block (no gap between them).
    [[nodiscard]] static Element stack(std::vector<Line> lines) {
        std::vector<Element> rows;
        rows.reserve(lines.size());
        for (auto& l : lines) rows.push_back(l.build());
        return detail::vstack().gap(0)(std::move(rows)).build();
    }

    Line& add(std::string t, Style s = {}) { spans_.push_back({std::move(t), s}); return *this; }
    Line& add(Span s) { spans_.push_back(std::move(s)); return *this; }
    [[nodiscard]] bool empty() const noexcept { return spans_.empty() && !rule_; }

    operator Element() const { return build(); }   // NOLINT(google-explicit-constructor)

    [[nodiscard]] Element build() const {
        using namespace dsl;
        if (rule_) return sep.build();
        if (spans_.empty()) return text("").build();
        if (clipped_)
            return Element{TextElement{.content = spans_[0].text, .style = spans_[0].style,
                                       .wrap = TextWrap::TruncateEnd, .runs = spans_[0].runs}};
        if (one_row_) {
            std::vector<Element> parts;
            for (const auto& s : spans_) {
                if (s.fill)
                    parts.push_back((Element{TextElement{.content = s.text, .style = s.style,
                                                         .wrap = TextWrap::TruncateEnd,
                                                         .runs = s.runs}}
                                     | grow(1.0f) | shrink(1.0f)).build());
                else        parts.push_back(text(s.text, s.style).build());
            }
            if (spans_.size() == 1 && !spans_[0].fill)
                return (text(spans_[0].text, spans_[0].style) | height(1) | overflow(Overflow::Hidden)).build();
            return (hstack().width(Dimension::percent(100))(std::move(parts)).build()
                    | height(1) | overflow(Overflow::Hidden)).build();
        }
        auto one = [](const Span& s) -> Element {
            if (s.pad_left > 0) return (text(s.text, s.style, s.wrap) | padding(0, 0, 0, s.pad_left)).build();
            return text(s.text, s.style, s.wrap).build();
        };
        if (spans_.size() == 1 && spans_[0].pad_left == 0) return one(spans_[0]);
        std::vector<Element> parts;
        parts.reserve(spans_.size());
        for (const auto& s : spans_) parts.push_back(one(s));
        return h(std::move(parts)).build();
    }

private:
    std::vector<Span> spans_;
    bool rule_ = false;
    bool one_row_ = false;
    bool clipped_ = false;
};

} // namespace maya
