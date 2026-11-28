#pragma once
// maya::widget::ContextGauge — fuel-gauge bar for context-window fullness.
//
// Stable-width slot showing how full the model's context is, with a
// gradient bar coloured by zone (green safe → amber squeeze → red
// cliff). When `used == 0` the slot still occupies the same display
// columns but renders a dim placeholder ("CTX  ────────  ──%") so
// the surrounding right-group chips don't shove leftward when the
// first usage event arrives mid-stream.
//
//   maya::ContextGauge{{
//       .used     = 18'432,
//       .max      = 200'000,
//       .show_bar = true,        // false on narrow widths
//   }}.build();

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../../dsl.hpp"
#include "../../element/element.hpp"
#include "../../element/text.hpp"
#include "../../style/color.hpp"
#include "../../style/style.hpp"
#include "../../style/theme.hpp"

namespace maya {

class ContextGauge {
public:
    struct Config {
        int  used        = 0;
        int  max         = 0;
        int  cells       = 10;       // bar width in cells
        bool show_bar    = true;     // false → drop the bar (percent only)
        bool show_tokens = true;     // false → drop the raw "used/max" counts
                                     //         (compact: bar graph + percent)
        // Of `used`, how much the provider served from its prompt cache. When
        // > 0 the filled bar splits: the cached stretch draws as ▓ (stored,
        // cheap), the fresh tail as █ (paid in full this request). Same width,
        // same zone colours — the split is a SHAPE, so it survives monochrome
        // themes. 0 = no split (provider silent, or nothing cached yet).
        int  cached      = 0;
    };

    explicit ContextGauge(Config c) : cfg_(c) {}

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;
        const Color muted = Color::slot(ThemeSlot::Muted);

        if (cfg_.max <= 0) return blank().build();

        const bool has_tokens = cfg_.used > 0;
        // 64-bit intermediate: `used * 100` overflows int32 past ~21.4M
        // tokens. Hosts feed raw byte counts through this path in some
        // adapters, so guard rather than assume the domain stays small.
        // Negative used is already excluded by has_tokens; clamp the
        // result to [0,100] for the bar/threshold math either way.
        const int  pct = has_tokens
            ? static_cast<int>(std::clamp<long long>(
                  static_cast<long long>(cfg_.used) * 100
                      / static_cast<long long>(cfg_.max),
                  0, 100))
            : 0;
        const Color zone = has_tokens ? threshold_color(pct) : muted;

        std::vector<Element> parts;
        // "CTX" carries NO trailing space. Every following segment supplies
        // its own LEADING space, so exactly one owner is responsible for
        // each gap.
        //
        // The old form baked a trailing space into "CTX " as well, on the
        // assumption that the bar always followed. When show_bar sheds on a
        // narrow status bar, that space collided with the percent's own
        // leading space AND the tabular pad inside it — painting
        // "CTX   28%" with a 3-column hole. Same shape as the status bar's
        // orphaned "·   ·": spacing baked onto segment N cannot know whether
        // segment N+1 survived.
        parts.push_back(text("CTX", Style{}.with_fg(muted).with_bold()));

        if (cfg_.show_bar) {
            // The raw "used/max" token counts are the most verbose part of
            // the gauge; show_tokens=false drops just those (keeping the bar
            // + percent) for a compact "CTX ████▌░░░░░ 46%" that fits a
            // narrow / phone-width status bar.
            if (has_tokens) {
                if (cfg_.show_tokens) {
                    // The USED count keeps a constant 6-col field so the
                    // right-group chips don't shift as it grows during a turn.
                    // The MAX (denominator) never changes within a session, so
                    // it's trimmed of leading pad — otherwise a right-justified
                    // "  1.0M" shows an ugly gap after the slash
                    // ("62.9k/  1.0M"). Trimmed reads tight: "62.9k/1.0M".
                    //
                    // The 6-col field is right-aligned, so it already opens
                    // with pad at small values; ltrim it and prepend exactly
                    // one leading space, keeping the field width via the
                    // trailing space before the bar.
                    std::string used_str = " " + format_tokens(cfg_.used) + "/"
                                         + ltrim_(format_tokens(cfg_.max))
                                         + " ";
                    parts.push_back(text(used_str, fg_dim_(muted)));
                } else {
                    parts.push_back(text(" ", fg_dim_(muted)));
                }
                const long long cached = std::clamp<long long>(cfg_.cached, 0, cfg_.used);
                const int cached_pct = static_cast<int>(
                    cached * 100 / static_cast<long long>(cfg_.max));
                parts.push_back(bar(pct, cfg_.cells, cached_pct));
            } else {
                // Placeholder numbers, same cols as live: the 6-col used
                // field + '/' + the (trimmed) max + trailing space. Mirrors
                // the live layout so the right-group chips stay pinned when
                // the first usage event lands.
                if (cfg_.show_tokens) {
                    std::string ph = std::string(" ") + "    \xe2\x80\x94\xe2\x80\x94/"
                                   + ltrim_(format_tokens(cfg_.max)) + " ";
                    parts.push_back(text(ph, fg_dim_(muted)));
                } else {
                    parts.push_back(text(" ", fg_dim_(muted)));
                }
                parts.push_back(bar(0, cfg_.cells));   // dim track only
            }
        }

        // The percent's gap and its stability pad are the SAME columns —
        // one owner, not two.
        //
        // A fixed field keeps the bar to its left from twitching as the
        // value grows during a turn, and that field INCLUDES the separating
        // space: 4 columns holds " 100" at the maximum and "   0" at the
        // minimum, so there is always exactly one owner for the gap and the
        // total width never moves. Emitting a separator on top of a 3-col
        // field was the two-owner bug ("████  28%", "████   0%").
        //
        // The dim placeholder mirrors the same 4+1 cells so the chips to
        // the right stay pinned when the first usage event lands.
        if (has_tokens) {
            parts.push_back(text(tabular_int_(pct, 4) + "%",
                                 Style{}.with_fg(zone).with_bold()));
        } else {
            // " ———%" — 4 cells + '%', same as live " 100%".
            parts.push_back(text(
                " \xe2\x80\x94\xe2\x80\x94\xe2\x80\x94%",
                Style{}.with_fg(zone).with_dim()));
        }

        return h(std::move(parts)).build();
    }

private:
    Config cfg_;

    // ── Smooth 1/8-gradation bar with per-cell color ──────────────────────
    //
    // Visual: `█████▆░░░░`  →  green green green green warn-amber dim dim dim dim
    // With a cached prefix: `▓▓▓▓█▆░░░░` — whole cells inside the cached share
    // draw textured; the partial tip is always fresh (it is the newest part).
    [[nodiscard]] static Element bar(int pct, int cells, int cached_pct = 0) {
        static constexpr std::string_view kPartials[8] = {
            "", "\xe2\x96\x8f", "\xe2\x96\x8e", "\xe2\x96\x8d",
            "\xe2\x96\x8c", "\xe2\x96\x8b", "\xe2\x96\x8a", "\xe2\x96\x89",
        };
        const Color muted = Color::slot(ThemeSlot::Muted);

        pct = std::clamp(pct, 0, 100);
        int total_eighths = pct * cells * 8 / 100;
        const int cached_eighths =
            std::clamp(cached_pct, 0, pct) * cells * 8 / 100;

        std::string content;
        std::vector<StyledRun> runs;
        runs.reserve(static_cast<std::size_t>(cells));
        content.reserve(static_cast<std::size_t>(cells) * 3);

        for (int i = 0; i < cells; ++i) {
            int filled = std::max(0, total_eighths - i * 8);
            std::string_view ch;
            const bool cached_cell = filled >= 8 && (i + 1) * 8 <= cached_eighths;
            if      (cached_cell) ch = "\xe2\x96\x93";  // ▓ dark shade: cached
            else if (filled >= 8) ch = "\xe2\x96\x88";  // █ full block: fresh
            else if (filled >  0) ch = kPartials[filled];
            else                  ch = "\xe2\x96\x91";  // ░ light shade

            // Threshold by cell position: [0,0.6) safe, [0.6,0.8) warn, rest danger.
            float cell_t = static_cast<float>(i + 1) / static_cast<float>(cells);
            Color cc;
            if      (filled == 0)    cc = muted;
            else if (cell_t <= 0.6f) cc = Color::slot(ThemeSlot::Success);
            else if (cell_t <= 0.8f) cc = Color::slot(ThemeSlot::Warning);
            else                     cc = Color::slot(ThemeSlot::Error);

            std::size_t off = content.size();
            content.append(ch);
            Style st = (filled == 0) ? Style{}.with_fg(cc).with_dim()
                                     : Style{}.with_fg(cc);
            runs.push_back(StyledRun{off, ch.size(), st});
        }
        return Element{TextElement{
            .content = std::move(content),
            .style   = {},
            .runs    = std::move(runs),
        }};
    }

    static Color threshold_color(int pct) {
        if (pct < 60)  return Color::slot(ThemeSlot::Success);
        if (pct <= 80) return Color::slot(ThemeSlot::Warning);
        return Color::slot(ThemeSlot::Error);
    }

    // Constant 6-char token field: " 999.9" / " 99.9k" / "  9.9M".
    // CONSTANT width is load-bearing — the widget's contract is a
    // stable-width slot, and the old 5-char <1000 branch vs 6-char k/M
    // branches shoved the right-group chips one column sideways the
    // moment `used` crossed 1000 (and mismatched the placeholder).
    static std::string format_tokens(int n) {
        char buf[16];
        // Thresholds are ROUNDING-AWARE: %.1f rounds 999.95 up to
        // "1000.0", which would overflow the 6-char field for
        // n ∈ [999950, 999999] under a plain >= 1'000'000 gate.
        if (n >= 999'950'000) {
            // "   2.1B" territory — without this branch %5.1fM prints
            // "1000.0M"+ (7 chars) for n ≥ 999.95M up to INT_MAX.
            std::snprintf(buf, sizeof(buf), "%5.1fB", static_cast<double>(n) / 1'000'000'000.0);
        } else if (n >= 999'950) {
            std::snprintf(buf, sizeof(buf), "%5.1fM", static_cast<double>(n) / 1'000'000.0);
        } else if (n >= 1000) {
            std::snprintf(buf, sizeof(buf), "%5.1fk", static_cast<double>(n) / 1000.0);
        } else {
            std::snprintf(buf, sizeof(buf), "%6d", n);
        }
        return buf;
    }

    static std::string tabular_int_(int n, int width) {
        std::string s = std::to_string(n);
        if (static_cast<int>(s.size()) >= width) return s;
        return std::string(static_cast<std::size_t>(width - static_cast<int>(s.size())), ' ')
             + s;
    }

    // Drop leading spaces from a fixed-width formatted field. Used on the MAX
    // denominator (which never changes within a session, so trimming it can't
    // cause the right-group chips to shift) to close the gap the constant
    // 6-col right-justification would otherwise leave after the '/'.
    static std::string ltrim_(std::string s) {
        std::size_t i = 0;
        while (i < s.size() && s[i] == ' ') ++i;
        return s.substr(i);
    }

    static Style fg_dim_(Color c) {
        const bool is_already_muted = theme::is_muted(c);
        return is_already_muted
            ? Style{}.with_fg(c)
            : Style{}.with_fg(c).with_dim();
    }
};

} // namespace maya
