#pragma once
// maya::panel item widget: Action — a row that DOES something and reports
// its outcome where the action lives ("Test connection" → "connected ✓").

#include <cstdint>
#include <string>
#include <utility>

#include "../../../../style/style.hpp"
#include "../context.hpp"

namespace maya::panel {

struct Action {
    enum class Tone : std::uint8_t { Neutral, Busy, Good, Bad };
    std::string status;
    std::string hint = "press Enter";
    Tone        tone = Tone::Neutral;
};

[[nodiscard]] inline std::pair<std::string, Style>
render(const Action& c, const ItemCtx& ctx) {
    if (c.status.empty()) return {c.hint, Style{}.with_fg(ctx.theme.off)};
    Color tone = ctx.theme.value;
    switch (c.tone) {
        case Action::Tone::Good: tone = ctx.theme.good;  break;
        case Action::Tone::Bad:  tone = ctx.theme.error; break;
        case Action::Tone::Busy: tone = ctx.theme.busy;  break;
        case Action::Tone::Neutral: break;
    }
    return {c.status, Style{}.with_fg(tone)};
}

} // namespace maya::panel
