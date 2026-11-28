#pragma once
// maya::panel item widget: Slider — a bounded real, drawn as a bar.
//
// For ratios judged visually (an MMR lambda, a temperature): ←/→ step the
// value, the bar shows roughly-where-in-range at a glance. A quantity you'd
// rather TYPE is a Number, not a Slider.

#include <algorithm>
#include <string>
#include <utility>

#include "../../../../style/style.hpp"
#include "../context.hpp"

namespace maya::panel {

struct Slider {
    double value = 0.0, min = 0.0, max = 1.0;
    int    decimals = 2;
};

[[nodiscard]] inline std::pair<std::string, Style>
render(const Slider& c, const ItemCtx& ctx) {
    // The bar takes the panel's drawn-control budget.
    //
    // It was `constexpr int kCells = 12` — the same twelve on a 40-column
    // split pane and a 200-column terminal. A bar's whole job is to make a
    // ratio comparable at a glance, and its resolution IS its width: at 12
    // cells the smallest visible step is 8%, so two settings a twentieth
    // apart draw identically. Freezing the size froze the information.
    //
    // 8 is the floor because below it the bar stops reading as a scale and
    // becomes texture. 24 is the ceiling because a bounded ratio is judged
    // by proportion, not measured — past that the eye starts counting cells
    // and the number beside it is the better answer anyway.
    const int cells = ctx.drawn_cells(/*pref=*/12, /*floor=*/8, /*ceiling=*/24);
    const double span = (c.max > c.min) ? (c.max - c.min) : 1.0;
    const double t    = std::clamp((c.value - c.min) / span, 0.0, 1.0);
    const int    on   = static_cast<int>(t * cells + 0.5);
    std::string bar;
    for (int i = 0; i < cells; ++i)
        bar += (i < on) ? "\xe2\x96\x86" : "\xe2\x96\x81";       // ▆ / ▁

    // Fixed-decimal without <format>/<sstream>: these are bounded ratios,
    // so integer scaling is exact and allocation-free.
    const int dec = std::clamp(c.decimals, 0, 4);
    int scale = 1;
    for (int i = 0; i < dec; ++i) scale *= 10;
    const long long scaled =
        static_cast<long long>(c.value * scale + (c.value < 0 ? -0.5 : 0.5));
    std::string num = std::to_string(scaled / scale);
    if (dec > 0) {
        long long frac = scaled % scale;
        if (frac < 0) frac = -frac;
        std::string fs = std::to_string(frac);
        fs.insert(fs.begin(), static_cast<std::size_t>(dec) - fs.size(), '0');
        num += "." + fs;
    }
    return {bar + "  " + num, Style{}.with_fg(ctx.theme.value)};
}

} // namespace maya::panel
