#pragma once
// maya::widget::PhaseAccent — soft-edge horizontal rule in a state color.
//
// A row of ▔ (top edge) or ▁ (bottom edge) half-block glyphs spanning
// the full width, dim, in a phase color. Reads as a "soft shelf"
// rather than a hard line — modern app vibe, and the color carries
// app-state info without extra chrome characters.
//
//   maya::PhaseAccent{{ .color=Color::slot(ThemeSlot::Info), .position=Position::Top }}.build();

#include <cstdint>
#include <string>

#include "../../dsl.hpp"
#include "../../element/element.hpp"
#include "../../style/color.hpp"
#include "../../style/style.hpp"

namespace maya {

class PhaseAccent {
public:
    enum class Position : std::uint8_t { Top, Bottom };

    struct Config {
        Color    color    = Color::slot(ThemeSlot::Info);
        Position position = Position::Top;
    };

    explicit PhaseAccent(Config c) : cfg_(c) {}

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;
        return component([c = cfg_.color, p = cfg_.position]
                         (int w, int /*h*/) -> Element {
            if (w <= 0) return blank().build();
            // ─ (U+2500) — single thin horizontal line, one row tall,
            // inked across the full cell. Same glyph for top and bottom
            // so the status bar sits flush against the composer with no
            // empty half-row of breathing space (the old ▔/▁ half-blocks
            // left a transparent half-row above/below their ink).
            const char* glyph = (p == Position::Top)
                ? "\xe2\x94\x80"   // ─
                : "\xe2\x94\x80";  // ─
            std::string line;
            line.reserve(static_cast<std::size_t>(w) * 3);
            for (int i = 0; i < w; ++i) line += glyph;
            return text(std::move(line),
                        Style{}.with_fg(c).with_dim()).build();
        });
    }

private:
    Config cfg_;
};

} // namespace maya
