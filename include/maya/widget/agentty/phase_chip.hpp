#pragma once
// maya::widget::PhaseChip — colored glyph + bold verb + optional elapsed.
//
// Phase indicator with no chip background — pro TUI style: glyph color
// carries urgency, weight (bold) tells the eye it's a state label.
// When `breathing` is true, the glyph alternates bold/dim on a slow
// 32-frame cycle so the indicator feels alive at rest.
//
// `verb_width` renders the verb in EXACTLY this many display columns
// (truncate-with-ellipsis if too long, right-pad if too short) so
// chips to the right of the phase chip don't slide horizontally as
// the verb changes between "Streaming" / "bash" / "find_definition".
// Pass 0 to drop the verb entirely.
//
// `elapsed_secs` ≥ 0 appends a fixed-5-column elapsed-time tail.
//
//   maya::PhaseChip{{
//       .glyph        = "\xe2\xa0\x8b",   // ⠋ spinner frame
//       .verb         = "Streaming",
//       .color        = Color::slot(ThemeSlot::Info),
//       .breathing    = true,
//       .frame        = spinner.frame_index(),
//       .verb_width   = 10,
//       .elapsed_secs = 4.2f,
//   }}.build();

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "../../dsl.hpp"
#include "../../element/element.hpp"
#include "../../style/color.hpp"
#include "../../style/style.hpp"

namespace maya {

class PhaseChip {
public:
    struct Config {
        std::string glyph;
        std::string verb;
        Color       color        = Color::slot(ThemeSlot::Info);
        bool        breathing    = false;
        int         frame        = 0;
        int         verb_width   = 10;     // 0 = drop verb
        float       elapsed_secs = -1.0f;  // < 0 = omit
    };

    explicit PhaseChip(Config c) : cfg_(std::move(c)) {}

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;

        Style glyph_style;
        if (cfg_.breathing) {
            // 32-frame cycle, bold for first half, dim for second.
            const bool inhale = ((cfg_.frame >> 4) & 1) == 0;
            glyph_style = inhale ? Style{}.with_fg(cfg_.color).with_bold()
                                 : Style{}.with_fg(cfg_.color).with_dim();
        } else {
            glyph_style = Style{}.with_fg(cfg_.color);
        }

        std::vector<Element> parts;
        // The glyph's trailing space belongs to the GLYPH, not to the verb:
        // emitted unconditionally it became a second owner of the gap that
        // separates this chip from whatever precedes it, painting
        // "▌  Streaming" whenever the glyph was empty. Tying it to the
        // glyph's presence means the chip contributes exactly one gap when
        // it has a glyph and none when it doesn't — so the caller's own
        // separator is always the sole owner of the boundary.
        if (!cfg_.glyph.empty()) {
            parts.push_back(text(cfg_.glyph, glyph_style));
            parts.push_back(text(" "));
        }

        if (cfg_.verb_width > 0) {
            // UTF-8 + wide-char-safe truncation. The previous version
            // substr'd by byte count, which split multi-byte sequences
            // (e.g. CJK verbs) and miscounted padding for wide glyphs.
            std::string out = (string_width(cfg_.verb) > cfg_.verb_width)
                ? truncate_end(cfg_.verb, cfg_.verb_width)
                : cfg_.verb;
            int dw = string_width(out);
            if (dw < cfg_.verb_width)
                out.append(static_cast<std::size_t>(cfg_.verb_width - dw), ' ');
            parts.push_back(text(std::move(out),
                                 Style{}.with_fg(cfg_.color).with_bold()));
        }

        if (cfg_.elapsed_secs >= 0.0f && cfg_.verb_width > 0) {
            parts.push_back(text(" "));
            parts.push_back(text(format_elapsed_5(cfg_.elapsed_secs),
                                 Style{}.with_fg(cfg_.color).with_dim()));
        }

        return h(std::move(parts)).build();
    }

private:
    Config cfg_;

    // Fixed-5-column elapsed-time formatter:
    //     0.0–9.9 s  →  " 4.2s"   (leading space)
    //   10.0–99.9 s  →  "12.3s"
    //      100–599 s →  " 234s"   (whole seconds)
    //        ≥600 s  →  " 9m05s"  (m/s)
    //        ≥3600 s →  " >1hr"
    static std::string format_elapsed_5(float secs) {
        char buf[16];
        if (secs < 10.0f) {
            std::snprintf(buf, sizeof(buf), "%4.1fs", static_cast<double>(secs));
        } else if (secs < 100.0f) {
            std::snprintf(buf, sizeof(buf), "%4.1fs", static_cast<double>(secs));
        } else if (secs < 600.0f) {
            std::snprintf(buf, sizeof(buf), "%4ds", static_cast<int>(secs));
        } else if (secs < 3600.0f) {
            int mins = static_cast<int>(secs) / 60;
            int rest = static_cast<int>(secs) - mins * 60;
            std::snprintf(buf, sizeof(buf), "%dm%02ds", mins, rest);
        } else {
            std::snprintf(buf, sizeof(buf), " >1hr");
        }
        return buf;
    }
};

} // namespace maya
