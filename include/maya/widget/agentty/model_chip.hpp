#pragma once
// maya::ModelChip — the provider tab and the model badge beside it, with an
// optional reasoning-effort and update chip:
//
//   [ Anthropic ] Opus 4.8  ✦ xhigh  ⬆ v0.9.66
//
// The provider gets a BACKGROUND (a label for the thing beside it, not
// another peer in a dim ·-separated run); the model is ModelBadge. When
// nothing is signed in, the tab is dimmed and "— sign in" stands where the
// model would be; with no model yet, a muted note says why.

#include <optional>
#include <string>
#include <utility>

#include "../../dsl.hpp"
#include "../../element/builder.hpp"
#include "../../style/color.hpp"
#include "../../style/style.hpp"
#include "../../style/theme.hpp"
#include "model_badge.hpp"

namespace maya {

class ModelChip {
public:
    enum class State : uint8_t { Model, SignedOut, NoModel, ProviderOnly };

    struct Config {
        State       state = State::Model;
        std::string provider;           // shown in the filled tab
        Style       provider_style{};   // the tab's band (host picks the hue)
        ModelBadge::Config badge{};     // State::Model
        std::string note;               // State::SignedOut / NoModel
        std::string effort;             // "xhigh"; empty = no effort chip
        Color       effort_sigil = Color::slot(ThemeSlot::Accent);
        // Update chip: "↺ v…" (restart pending) or "⬆ v…" (newer known).
        std::string update;             // full text incl. glyph; empty = none
        Color       update_color = Color::slot(ThemeSlot::Success);
        Color       muted = Color::slot(ThemeSlot::Muted);
        // A dim note after everything ("  ↵ queues"): what Enter will do.
        std::string trailing;
    };

    explicit ModelChip(Config c) : cfg_(std::move(c)) {}
    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;
        if (cfg_.trailing.empty()) return core();
        return h(core(), text(cfg_.trailing, Style{}.with_fg(cfg_.muted).with_dim())).build();
    }

private:
    [[nodiscard]] Element core() const {
        using namespace dsl;
        const auto& c = cfg_;
        auto tab = text(" " + c.provider + " ", c.provider_style);
        switch (c.state) {
            case State::ProviderOnly:
                return std::move(tab).build();
            case State::SignedOut:
            case State::NoModel:
                return h(std::move(tab), text(" "), text(c.note, dim(c.muted))).build();
            case State::Model: break;
        }
        Element effort = text("");
        if (!c.effort.empty())
            // The sigil keeps the accent and carries the eye; the tier only
            // has to be READABLE once the eye arrives, so plain muted (not
            // dimmed muted, which vanishes on dark themes).
            effort = h(text("  "),
                       text("\xe2\x9c\xa6 ", Style{}.with_fg(c.effort_sigil)),
                       text(c.effort, Style{}.with_fg(c.muted))).build();
        Element update = text("");
        if (!c.update.empty()) update = text("  " + c.update, Style{}.with_fg(c.update_color));
        // One UNFILLED space between tab and model: the tab's own trailing
        // space is background-filled, so the gap has to be outside the fill.
        return h(std::move(tab), text(" "), ModelBadge{c.badge}.build(),
                 std::move(effort), std::move(update)).build();
    }

    static Style dim(Color x) {
        return theme::is_muted(x) ? Style{}.with_fg(x) : Style{}.with_fg(x).with_dim();
    }
    Config cfg_;
};

} // namespace maya
