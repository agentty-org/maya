#pragma once
// maya::panel item widget: Secret — a credential.
//
// No plaintext — by construction. The value is a CHARACTER COUNT, not a
// string, so no render path (present or future) can leak a credential to the
// screen: there is nothing to leak. Masking is type-level, not a flag someone
// can forget to check.

#include <algorithm>
#include <string>
#include <utility>

#include "../../../../style/style.hpp"
#include "../context.hpp"

namespace maya::panel {

struct Secret {
    std::size_t filled = 0;
    std::size_t caret  = std::string::npos;
    // Format hint for the EMPTY field ("sk-ant-…"). A placeholder is not
    // secret — it exists precisely to be shown — so it is the one string
    // this kind may carry.
    std::string placeholder;
};

[[nodiscard]] inline std::pair<std::string, Style>
render(const Secret& c, const ItemCtx& ctx) {
    // Capped, so the rendered width never discloses the real length.
    const bool editing = c.caret != std::string::npos;
    if (c.filled == 0) {
        if (editing) {
            if (ctx.caret_out) *ctx.caret_out = 0;
            return {std::string{"\xe2\x96\x88"}
                      + (c.placeholder.empty() ? " empty" : " " + c.placeholder),
                    Style{}.with_fg(ctx.theme.value_edit)};
        }
        return {c.placeholder.empty() ? "not set" : c.placeholder,
                Style{}.with_fg(ctx.theme.off)};
    }
    // The mask is width-RESPONSIVE: stars up to the panel's edit budget
    // (the same width Text values get), then ·N chars for the overflow.
    // The cap is layout, not privacy — filled is a count the OWNER typed;
    // the render must answer "did my whole paste land?" at a glance, and
    // 12 fixed stars for a 40-char code read as "only part arrived".
    const std::size_t cap = ctx.edit_budget > 8
                                ? static_cast<std::size_t>(ctx.edit_budget - 2)
                                : 12;
    std::string dots(std::min<std::size_t>(c.filled, cap), '*');
    if (editing) {
        if (ctx.caret_out) *ctx.caret_out = dots.size();
        dots += "\xe2\x96\x88";
    }
    if (c.filled > cap)
        dots += " \xc2\xb7 " + std::to_string(c.filled) + " chars";
    return {dots, Style{}.with_fg(editing ? ctx.theme.value_edit
                                          : ctx.theme.value)};
}

} // namespace maya::panel
