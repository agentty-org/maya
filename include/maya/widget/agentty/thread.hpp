#pragma once
// maya::widget::Thread — top-level conversation viewport.
//
// Owns the empty-vs-populated branch:
//   - Empty thread → maya::WelcomeScreen (brand splash, starters, hints)
//   - Non-empty    → maya::Conversation  (turns + dividers + in-flight)
//
// Caller fills `welcome` when `is_empty == true`, `conversation` when
// `is_empty == false`. The widget invokes the matching sub-widget.
//
//   maya::Thread{{
//       .is_empty     = m.d.current.messages.empty(),
//       .welcome      = welcome_screen_config(m),
//       .conversation = conversation_config(m),
//   }}.build();

#include <utility>

#include "../../element/element.hpp"

#include "conversation.hpp"
#include "welcome_screen.hpp"

namespace maya {

class Thread {
public:
    struct Config {
        bool                  is_empty = false;
        WelcomeScreen::Config welcome;        // when is_empty
        Conversation::Config  conversation;   // when !is_empty
    };

    explicit Thread(Config c) : cfg_(std::move(c)) {}

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        if (cfg_.is_empty)
            return WelcomeScreen{cfg_.welcome}.build();
        return Conversation{cfg_.conversation}.build();
    }

private:
    Config cfg_;
};

} // namespace maya
