#pragma once
// maya::widget::AppLayout — top-level chat-app frame.
//
// Composes the four canonical chat-app rows + overlay slot:
//
//   ┌─────────────────────────────────────────┐  ↑
//   │                                         │  │
//   │  Thread (grows to fill)                 │  │
//   │                                         │  │
//   │ ┌─────────────────────────────────────┐ │  │ pad<1>
//   │ │  ChangesStrip (optional)            │ │  │
//   │ ├─────────────────────────────────────┤ │  │
//   │ │  Composer                           │ │  │
//   │ └─────────────────────────────────────┘ │  │
//   │  StatusBar                              │  │
//   └─────────────────────────────────────────┘  ↓
//
// Every section is a nested widget Config — host apps construct one
// `AppLayout::Config` per frame and `AppLayout::build()` invokes the
// sub-widgets internally. No Element construction in the host.
//
// `overlay_present == true` floats `overlay` (centered, bottom-pinned,
// opaque background) over the base. Empty Element + present=false
// collapses cleanly to just the base — host doesn't need an `if`.
//
//   maya::AppLayout{{
//       .thread        = thread_cfg(m),
//       .changes_strip = changes_strip_cfg(m),
//       .composer      = composer_cfg(m),
//       .status_bar    = status_bar_cfg(m),
//       .overlay         = pick_overlay(m),       // Element (modal etc.)
//       .overlay_present = m.has_modal_open,
//   }}.build();

#include <optional>
#include <utility>

#include "../../core/render_context.hpp"
#include "../../dsl.hpp"
#include "../../element/element.hpp"

#include "changes_strip.hpp"
#include "composer.hpp"
#include "overlay.hpp"
#include "status_bar.hpp"
#include "thread.hpp"

namespace maya {

class AppLayout {
public:
    struct Config {
        Thread::Config        thread;
        ChangesStrip::Config  changes_strip;
        // Floats over the bottom of the thread, its last row sitting right
        // on the composer's top border (an inline completion menu). It never
        // takes a row in the column, so opening it moves nothing.
        // Empty element = nothing.
        Element               above_composer;
        Composer::Config      composer;
        StatusBar::Config     status_bar;

        // Overlay slot — nullopt collapses cleanly to just the base.
        std::optional<Element> overlay;
    };

    explicit AppLayout(Config c) : cfg_(std::move(c)) {}

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;

        // Structural shape — mirrors maya/examples/agent_session.cpp,
        // the reference inline-mode app. Children sit DIRECTLY in the
        // outer vstack at their natural heights. No per-child wrapper
        // boxes, no flex grow/shrink on children, no overflow setting
        // anywhere in this widget.
        //
        // Why this is the only correct shape for inline mode:
        //   The maya inline renderer commits overflow row-by-row into
        //   the terminal's native scrollback (see serialize.cpp). For
        //   that to work, the layout MUST be allowed to grow past the
        //   viewport — any ancestor that clips (overflow:Hidden) or
        //   compresses (shrink>0) the thread forces content to pile
        //   up inside the same viewport slot, and stale cells from a
        //   previously-taller frame leak through the new shorter slot
        //   at the thread↔composer seam. That was the live-streaming
        //   ghosting bug.
        //
        // Thread grows internally (Conversation::build() sets
        // grow(1.0f) + trailing spacer), which eats slack on short
        // content. Composer + StatusBar are natural-height by
        // construction (their build()s don't set grow), so they sit
        // at the end of the vstack and ride the viewport bottom.
        //
        // DO NOT wrap any child in a vstack/box here. DO NOT add
        // overflow(). DO NOT set shrink on the outer or any child.
        // The min_height + padding below are the only outer flex
        // properties this widget owns.
        const int term_h = available_height();
        auto composer   = Composer{cfg_.composer}.build();
        auto status_bar = StatusBar{cfg_.status_bar}.build();
        auto base = (vstack()
            .min_height(Dimension::fixed(term_h))
            .padding(1)
            (
                Thread{cfg_.thread}.build(),
                ChangesStrip{cfg_.changes_strip}.build(),
                composer,
                status_bar,
                // Composer anti-bounce pad. The composer rides the
                // tree's content_height; a transient 1-row dip in the
                // content above (welcome→conversation subtree swap, the
                // thinking-indicator handoff, a tool card collapsing)
                // would otherwise bounce the whole chrome up then back
                // down. maya autonomously sets inline_min_content to the
                // bridge height (Runtime::render) and decays it once the
                // shrink proves real, so this carries NO dead space at
                // idle. Emitted via a LAZY component so the in-render
                // re-layout picks up the freshly-computed pad. Dim space
                // interns a non-default style so the canvas counts the
                // row (plain blank() is invisible to content_height).
                // LAST child so the pad lands at the very bottom and
                // directly raises content_height — anywhere higher gets
                // absorbed by the Thread's internal grow/spacer.
                component([](int /*w*/, int /*h*/) -> Element {
                    const int pad = available_inline_min_content();
                    if (pad <= 0) return blank().build();
                    std::vector<Element> prows;
                    prows.reserve(pad);
                    for (int p = 0; p < pad; ++p)
                        prows.push_back((text(" ") | Dim).build());
                    return v(prows).build();
                })
            )).build();

        // The composer float: pinned so its bottom edge meets the composer's
        // top border. Below that edge sit the composer, the status bar, the
        // anti-bounce pad and the outer padding row, all measured at the
        // width they really get. Same z-stack shape as Overlay, minus the
        // full-width fill: the menu paints its own box.
        if (!is_empty_text(cfg_.above_composer)) {
            const int inner_w = std::max(1, available_width() - 2);
            // The column's bottom padding row adds 1; the float's own last
            // row then sits right above the composer's top border.
            const int below = measure_element(composer, inner_w).height.value
                            + measure_element(status_bar, inner_w).height.value
                            + std::max(0, available_inline_min_content())
                            + 2;
            // Opaque, like Overlay: the menu's own blank cells (the gap
            // between a label and its detail) must hide the thread under it.
            auto floated = detail::vstack()
                .bg(theme::live().background)(cfg_.above_composer);
            base = detail::zstack({
                std::move(base),
                detail::vstack()
                    .justify(Justify::End)
                    .padding(0, 1, below, 1)(std::move(floated)),
            });
        }

        Overlay::Config oc;
        oc.base = std::move(base);
        if (cfg_.overlay) {
            oc.overlay = *cfg_.overlay;
            oc.present = true;
        }
        return Overlay{std::move(oc)}.build();
    }

private:
    Config cfg_;

    [[nodiscard]] static bool is_empty_text(const Element& e) noexcept {
        const auto* t = std::get_if<TextElement>(&e.inner);
        return t && t->content.empty();
    }
};

} // namespace maya
