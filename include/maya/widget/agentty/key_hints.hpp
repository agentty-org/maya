#pragma once
// maya::KeyHints / maya::FilterLine — the footer and filter rows every
// agentty picker and pane shares.
//
// KeyHints: "key label   key label …" on ONE line. When the width cannot fit
// them all, the lowest-priority hints drop out (rightmost loses ties) and the
// survivors keep their order; it never wraps. A hint with an empty label is
// "does not apply here" and is never drawn, so a caller can write
// `cond ? "" : "label"` instead of building two lists. The key carries an
// optional accent (success for keep, error for undo) so a destructive key
// never looks like a benign one.
//
// FilterLine: a magnifier, then the query or a greyed "type to filter …"
// prompt; optionally a blinking caret, for pickers you type INTO.

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../../dsl.hpp"
#include "../../element/builder.hpp"
#include "../../style/color.hpp"
#include "../../style/style.hpp"
#include "../../style/theme.hpp"

namespace maya {

struct KeyHint {
    std::string key;
    std::string label;
    int         priority = 0;                           // higher = kept longer
    Color       key_color = Color::slot(ThemeSlot::Text);
};

class KeyHints {
public:
    struct Config {
        std::vector<KeyHint> hints;
        int   gap   = 3;                                      // columns between hints
        Color label = Color::slot(ThemeSlot::Muted);
    };

    explicit KeyHints(Config c) : cfg_(std::move(c)) {}
    explicit KeyHints(std::vector<KeyHint> h) : cfg_{std::move(h)} {}
    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;
        return detail::component([cfg = cfg_](int w, int) -> Element {
            const auto& hints = cfg.hints;
            if (w <= 0 || hints.empty()) return nothing();
            auto pair_w = [](const KeyHint& hn) {
                return string_width(hn.key) + 1 + string_width(hn.label);
            };
            std::vector<bool> keep(hints.size(), true);
            for (std::size_t i = 0; i < hints.size(); ++i)
                if (hints[i].label.empty()) keep[i] = false;
            auto total = [&] {
                int sum = 0, shown = 0;
                for (std::size_t i = 0; i < hints.size(); ++i)
                    if (keep[i]) { sum += pair_w(hints[i]); ++shown; }
                if (shown > 1) sum += cfg.gap * (shown - 1);
                return sum;
            };
            while (total() > w) {
                int victim = -1;
                for (std::size_t i = 0; i < hints.size(); ++i) {
                    if (!keep[i]) continue;
                    if (victim < 0 || hints[i].priority <=
                            hints[static_cast<std::size_t>(victim)].priority)
                        victim = static_cast<int>(i);
                }
                if (victim < 0) break;
                keep[static_cast<std::size_t>(victim)] = false;
            }
            std::vector<Element> parts;
            bool first = true;
            for (std::size_t i = 0; i < hints.size(); ++i) {
                if (!keep[i]) continue;
                if (!first) parts.push_back(text(std::string(static_cast<std::size_t>(cfg.gap), ' ')));
                first = false;
                parts.push_back(text(hints[i].key + " ", Style{}.with_fg(hints[i].key_color)));
                parts.push_back(text(hints[i].label, dim_(cfg.label)));
            }
            if (parts.empty()) return nothing();
            return h(std::move(parts)).build();
        });
    }

private:
    static Style dim_(Color c) {
        return theme::is_muted(c) ? Style{}.with_fg(c) : Style{}.with_fg(c).with_dim();
    }
    Config cfg_;
};

// The blinking insertion point after a query: concealed block that anchors
// the terminal cursor.
[[nodiscard]] inline Element query_caret(Color tint) {
    return dsl::text("\xe2\x96\x88",
                     Style{}.with_conceal().with_caret_anchor().with_caret_shape(1).with_fg(tint))
        .build();
}

class FilterLine {
public:
    struct Config {
        std::string          query;
        std::string          noun;          // "providers" -> "type to filter providers…"
        std::optional<Color> caret;         // set = a prompt you type into
        std::string          glyph = "\xf0\x9f\x94\x8d ";   // 🔍
        Color                text  = Color::slot(ThemeSlot::Text);
        Color                muted = Color::slot(ThemeSlot::Muted);
    };

    explicit FilterLine(Config c) : cfg_(std::move(c)) {}
    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;
        std::string prompt = "type to filter";
        if (!cfg_.noun.empty()) { prompt += ' '; prompt += cfg_.noun; }
        prompt += "\xe2\x80\xa6";                      // …
        std::vector<Element> parts;
        parts.push_back(text(cfg_.glyph, cfg_.caret ? Style{}.with_fg(*cfg_.caret).with_bold()
                                                    : Style{}.with_fg(cfg_.muted)));
        if (cfg_.query.empty()) {
            // Caret BEFORE the prompt: it marks where the first keystroke
            // lands, and the prompt is placeholder text to its right.
            if (cfg_.caret) parts.push_back(query_caret(*cfg_.caret));
            parts.push_back(text(prompt, Style{}.with_fg(cfg_.muted).with_italic()));
        } else {
            parts.push_back(text(cfg_.query, Style{}.with_fg(cfg_.text)));
            if (cfg_.caret) parts.push_back(query_caret(*cfg_.caret));
        }
        return h(std::move(parts)).build();
    }

private:
    Config cfg_;
};

} // namespace maya
