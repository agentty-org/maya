#pragma once
// maya::widget::Scrollable — backwards-compat wrapper over the framework
// scroll primitive (FlexStyle::scroll_x/y + ScrollState + dsl::scroll pipe).
//
// New code should prefer the primitive directly:
//
//   ScrollState s;
//   auto ui = my_content | dsl::scroll(s, /*viewport_h=*/8);
//   // In event_fn:
//   if (auto* k = as_key(ev)) s.handle(*k, /*viewport=*/8);
//   if (auto* m = as_mouse(ev)) s.handle(*m);
//
// This class stays for existing callers (and the agent-UI widgets that
// embed it) and lets you keep the imperative set_content / build pattern.
// Internally it just owns a ScrollState and assembles the same primitive
// the pipe form does — no negative-margin hacks, no flex-shrink games.

#include "../core/scroll_state.hpp"
#include "../dsl.hpp"
#include "../element/element.hpp"
#include "../style/style.hpp"
#include "../style/color.hpp"
#include "../terminal/input.hpp"
#include "agentty/scrollbar.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace maya {

struct ScrollConfig {
    int  height          = 10;     ///< Visible viewport height in rows (y axis)
    int  width           = 0;      ///< Visible viewport width in cols (0 = use parent width)
    int  scroll_amount   = 1;      ///< Rows per scroll step (vertical)
    bool show_indicator  = true;   ///< Show scroll position indicator (y axis only, for compat)
    Color indicator_color  = Color::slot(ThemeSlot::Muted);
    Color indicator_active = Color::slot(ThemeSlot::Muted);
};

class Scrollable {
    ScrollConfig cfg_;
    ScrollState  state_;
    Element      content_{TextElement{}};

public:
    explicit Scrollable(ScrollConfig cfg = {}) : cfg_(cfg) {
        state_.step_y = std::max(1, cfg.scroll_amount);
    }

    // -- Content --
    void set_content(Element elem) { content_ = std::move(elem); }

    // The pre-shim API required the caller to set this so scroll_down/up
    // would clamp correctly even before the first render. The renderer
    // now writes back max_y after layout, so this is only needed for
    // pre-render scroll calls. Treat it as a primer for state_.max_y;
    // the writeback will overwrite with the authoritative value on the
    // next paint.
    void set_content_height(int h) {
        state_.max_y = std::max(0, h - cfg_.height);
    }

    // -- Scroll API (delegates to ScrollState) --
    void scroll_up(int n = 0) {
        state_.scroll_by(0, -(n > 0 ? n : cfg_.scroll_amount));
    }
    void scroll_down(int n = 0) {
        state_.scroll_by(0, +(n > 0 ? n : cfg_.scroll_amount));
    }
    void scroll_to_top()    { state_.scroll_to_top(); }
    void scroll_to_bottom() { state_.scroll_to_bottom(); }

    [[nodiscard]] int offset()         const noexcept { return state_.y; }
    [[nodiscard]] int height()         const noexcept { return cfg_.height; }
    [[nodiscard]] int content_height() const noexcept { return state_.max_y + cfg_.height; }
    [[nodiscard]] ScrollState&       state()       noexcept { return state_; }
    [[nodiscard]] const ScrollState& state() const noexcept { return state_; }

    // -- Event handlers --
    bool handle(const KeyEvent& ev) {
        return state_.handle(ev, cfg_.height, cfg_.width);
    }
    bool handle_mouse(const MouseEvent& me) {
        return state_.handle(me);
    }

    // -- Build --
    [[nodiscard]] Element build() const {
        // Mutable reference for the DSL pipe (it needs &state to wire the
        // writeback pointer). The widget owns the state, so this cast is
        // safe — Scrollable::build() being const is a backwards-compat
        // promise to existing callers, not a real invariant on this class.
        auto& s = const_cast<ScrollState&>(state_);

        Element viewport = (cfg_.width > 0)
            ? (dsl::v(content_) | dsl::scroll(s, cfg_.width, cfg_.height)).build()
            : (dsl::v(content_) | dsl::scroll(s, cfg_.height)).build();

        if (!cfg_.show_indicator || state_.max_y == 0)
            return viewport;

        // Reuse the scrollbar widget — single source of truth for the
        // thumb math and glyph repertoire. Colors map onto ScrollbarStyle
        // (only the foreground; bg follows whatever parent paints).
        ScrollbarStyle st;
        st.track_color = cfg_.indicator_color;
        st.thumb_color = cfg_.indicator_active;
        return dsl::h(
            std::move(viewport),
            scrollbar_y(state_, cfg_.height, st)
        ).build();
    }

    operator Element() const { return build(); }
};

} // namespace maya
