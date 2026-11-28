#pragma once
// maya::widget::CheckpointDivider — full-width "↺ Restore checkpoint" rule.
//
// Lives ABOVE a turn, outside the rail. Marks a snapshot point that the
// user can restore to:
//
//   ─── [↺ Restore checkpoint] ───────────────────────
//
//   maya::CheckpointDivider{{
//       .label = "Restore checkpoint",
//       .color = Color::slot(ThemeSlot::Warning),
//   }}.build();

#include <string>
#include <utility>

#include "../../dsl.hpp"
#include "../../element/element.hpp"
#include "../../style/color.hpp"
#include "../../style/style.hpp"

namespace maya {

class CheckpointDivider {
public:
    struct Config {
        std::string label = "Restore checkpoint";
        Color       color = Color::slot(ThemeSlot::Warning);
    };

    explicit CheckpointDivider(Config c) : cfg_(std::move(c)) {}

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;
        const Style chrome = Style{}.with_fg(Color::slot(ThemeSlot::Muted)).with_dim();
        return h(
            text("\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80 ", chrome),       // ───
            text("[",                                     chrome),
            text("\xe2\x86\xba " + cfg_.label,            Style{}.with_fg(cfg_.color)),  // ↺
            text("] ",                                    chrome),
            text("\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80",  chrome),       // ───
            spacer()
        ).build();
    }

private:
    Config cfg_;
};

} // namespace maya
