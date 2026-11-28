#pragma once
// maya::widget::Overlay — base layer + an anchored floating element.
//
// Z-stack composition for modal-style UIs: the `base` always renders;
// when `overlay` is present (set `present=true`) it floats over the base,
// pinned to one of nine anchor positions, with a solid default background
// so the base content underneath doesn't bleed through. Pass
// `present=false` and the widget collapses to just the base — callers
// don't need an `if` branch.
//
//   maya::Overlay{{
//       .base    = main_view_element,
//       .overlay = modal_element,
//       .present = m.ui.has_modal_open,
//       .anchor  = Overlay::Anchor::BottomCenter,
//       .inset   = {0, 0, 2, 0},   // 2 rows up from the base bottom
//   }}.build();
//
// `anchor` chooses where the float sits; `inset` (top,right,bottom,left
// cells) nudges it INWARD from that edge, applied on the OUTER alignment
// wrapper so it sits outside the float's own bg fill. This is what lets a
// host pin a picker N rows above the base box bottom — keeping the float's
// painted extent within the base's extent so toggling it never changes the
// frame height (and so never pushes rows across the inline viewport edge
// into native scrollback).

#include <utility>

#include "../../core/types.hpp"
#include "../../element/builder.hpp"
#include "../../element/element.hpp"
#include "../../style/color.hpp"

namespace maya {

class Overlay {
public:
    // Nine standard anchor positions (which edge/corner the float pins to).
    enum class Anchor : uint8_t {
        TopLeft,      TopCenter,    TopRight,
        CenterLeft,   Center,       CenterRight,
        BottomLeft,   BottomCenter, BottomRight,
    };

    struct Config {
        Element     base;       // default-empty
        Element     overlay;    // default-empty
        bool        present = false;
        Anchor      anchor  = Anchor::BottomCenter;
        Edges<int>  inset{};    // inward nudge from the anchored edge(s)
    };

    explicit Overlay(Config c) : cfg_(std::move(c)) {}

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        if (!cfg_.present) return cfg_.base;

        // Cross-axis (horizontal) alignment and main-axis (vertical)
        // distribution derived from the anchor. The outer wrapper is a
        // full-screen vstack; align_items places the float horizontally,
        // justify places it vertically.
        const auto [ai, jc] = resolve(cfg_.anchor);

        // The float itself: a full-width bg-filled box so the base doesn't
        // bleed through. (Width percent(100) + default align_items=Stretch
        // means a caller's natural sizing genuinely stretches to the wrapper
        // content width; explicit fixed widths still override.)
        //
        // The fill is the THEME's background, not `default_color()`. An
        // overlay's job is to occlude what is under it, and occluding with
        // "the terminal's own colour" punches a hole straight through a
        // themed canvas — the panel lands as a rectangle of the user's
        // terminal background sitting inside an otherwise themed frame,
        // with the chrome above and below it painted and the panel not.
        //
        // Under `native` the theme's background IS default_color(), so this
        // is byte-for-byte the old behaviour: nothing is stated, and the
        // terminal shows through exactly as it should.
        auto floated = detail::vstack()
            .width(Dimension::percent(100))
            .padding(0, 2)
            .bg(theme::live().background)(cfg_.overlay);

        // Outer alignment wrapper. The inset lives HERE — outside the
        // bg-filled box — so it nudges the float without painting bg over
        // the inset rows (which is why a plain padding inside the box can't
        // express a "2 rows above the bottom" pin).
        return detail::zstack({
            cfg_.base,
            detail::vstack()
                .align_items(ai)
                .justify(jc)
                .padding(cfg_.inset.top, cfg_.inset.right,
                         cfg_.inset.bottom, cfg_.inset.left)(std::move(floated)),
        });
    }

private:
    Config cfg_;

    // anchor → (cross-axis align_items, main-axis justify) for a column wrapper.
    [[nodiscard]] static std::pair<Align, Justify> resolve(Anchor a) noexcept {
        Align   ai = Align::Center;
        Justify jc = Justify::End;
        switch (a) {
            case Anchor::TopLeft:      ai = Align::Start;  jc = Justify::Start;  break;
            case Anchor::TopCenter:    ai = Align::Center; jc = Justify::Start;  break;
            case Anchor::TopRight:     ai = Align::End;    jc = Justify::Start;  break;
            case Anchor::CenterLeft:   ai = Align::Start;  jc = Justify::Center; break;
            case Anchor::Center:       ai = Align::Center; jc = Justify::Center; break;
            case Anchor::CenterRight:  ai = Align::End;    jc = Justify::Center; break;
            case Anchor::BottomLeft:   ai = Align::Start;  jc = Justify::End;    break;
            case Anchor::BottomCenter: ai = Align::Center; jc = Justify::End;    break;
            case Anchor::BottomRight:  ai = Align::End;    jc = Justify::End;    break;
        }
        return {ai, jc};
    }
};

} // namespace maya
