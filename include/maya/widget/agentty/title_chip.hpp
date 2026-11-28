#pragma once
// maya::widget::TitleChip — leading-edge marker + bold title with
// middle-truncation. The simpler cousin of `Breadcrumb` (which holds
// a path of segments); `TitleChip` is a single label preceded by a
// colored ▎ edge bar.
//
//   ▎ implement /loop dynamic m…
//
// Used in the status bar's activity row to show the current thread
// title. Renders nothing when `title` is empty.
//
//   maya::TitleChip{{
//       .title       = m.d.current.title,
//       .edge_color  = phase_color(m.s.phase),
//       .text_color  = Color::slot(ThemeSlot::Text),
//       .max_chars   = 28,
//   }}.build();

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include "../../dsl.hpp"
#include "../../element/element.hpp"
#include "../../style/color.hpp"
#include "../../style/style.hpp"

namespace maya {

class TitleChip {
public:
    struct Config {
        std::string title;                          // empty = blank
        Color       edge_color = Color::slot(ThemeSlot::Info);
        Color       text_color = Color::slot(ThemeSlot::Text);
        std::size_t max_chars  = 28;
    };

    explicit TitleChip(Config c) : cfg_(std::move(c)) {}

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;
        if (cfg_.title.empty()) return text("");

        return h(
            text("\xe2\x96\x8e", Style{}.with_fg(cfg_.edge_color)),     // ▎
            text(" " + truncate(cfg_.title, cfg_.max_chars),
                 Style{}.with_fg(cfg_.text_color).with_bold())
        ).build();
    }

private:
    Config cfg_;

    static std::string truncate(std::string_view s, std::size_t max_cols) {
        // UTF-8 + wide-char-safe via the framework helper. The previous
        // implementation indexed by byte count, splitting multi-byte
        // sequences in the middle (CJK / emoji titles → garbage).
        return truncate_end(s, static_cast<int>(max_cols));
    }
};

} // namespace maya
