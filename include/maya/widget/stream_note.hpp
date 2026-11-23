#pragma once
// maya::widget::StreamNote — the activity row's live narration of a stream.
//
// The phase chip says WHAT the app is doing in one verb ("retrying",
// "stalled", "Streaming"). A StreamNote says the rest, inline, right after
// it — the reason, the attempt, and how long until the next thing happens:
//
//   ▌ ⠋ retrying     HTTP 503 · service unavailable · 2/6  ━━━━━━╸───  7s
//   ▌ ⠹ stalled      no data for 47s · reconnecting
//   ▌ ⠼ Streaming    waiting for the first token  ·  4.2s
//
// It lives IN the activity row instead of replacing it the way a toast
// does, so the gauges and the chip stay put while the stream explains
// itself. Hosts route stream-lifecycle messages here and keep the toast
// band for things the user did ("copied", "saved").
//
// `progress` (0..1, < 0 = none) draws a DRAIN bar: full when the wait
// starts, empty when the next attempt fires. Half-cell resolution
// (━ ╸ ─) so a 10-cell bar moves every 5% — smooth at countdown speed
// without needing eighth-block glyphs most fonts render unevenly.
//
// Width-adaptive by measurement, like the row around it: given w columns
// it sheds the bar, then truncates the text (the tail — the number the
// user is waiting on — goes last), and draws nothing below kMinWidth.

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "../dsl.hpp"
#include "../element/element.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"
#include "../style/theme.hpp"

namespace maya {

class StreamNote {
public:
    static constexpr int kMinWidth = 8;

    struct Config {
        std::string text;                                  // empty = hidden
        std::string tail;                                  // "7s", "4.2s" — the number
        Color       tone      = Color::slot(ThemeSlot::Warning);
        float       progress  = -1.0f;                     // 0..1 remaining; < 0 = no bar
        int         bar_cells = 10;
    };

    explicit StreamNote(Config c) : cfg_(std::move(c)) {}
    operator Element() const { return build(); }

    [[nodiscard]] bool visible() const noexcept { return !cfg_.text.empty(); }

    // Columns for the richest shape (text + bar + tail).
    [[nodiscard]] int natural_width() const {
        if (!visible()) return 0;
        return string_width(cfg_.text)
             + (cfg_.progress >= 0.0f && cfg_.bar_cells > 0 ? cfg_.bar_cells + 2 : 0)
             + (cfg_.tail.empty() ? 0 : string_width(cfg_.tail) + 2);
    }

    // Fills whatever width the layout hands it.
    [[nodiscard]] Element build() const {
        using namespace dsl;
        return component([c = cfg_](int w, int /*h*/) -> Element {
            return StreamNote{c}.build_at(w);
        });
    }

    // The note at its richest shape that fits `w` columns.
    [[nodiscard]] Element build_at(int w) const {
        using namespace dsl;
        if (!visible() || w < kMinWidth) return blank().build();

        const Color muted = Color::slot(ThemeSlot::Muted);
        const Style text_st = Style{}.with_fg(muted);
        const Style tail_st = Style{}.with_fg(cfg_.tone).with_bold();

        const int tw    = string_width(cfg_.text);
        const int tailw = cfg_.tail.empty() ? 0 : string_width(cfg_.tail) + 2;   // "  7s"
        const bool has_bar = cfg_.progress >= 0.0f && cfg_.bar_cells > 0;
        const int barw  = has_bar ? cfg_.bar_cells + 2 : 0;                       // "  ━━…"

        std::vector<Element> parts;
        auto push_tail = [&] {
            if (tailw == 0) return;
            parts.push_back(text("  "));
            parts.push_back(text(cfg_.tail, tail_st));
        };

        if (tw + barw + tailw <= w) {
            parts.push_back(text(cfg_.text, text_st));
            if (has_bar) {
                parts.push_back(text("  "));
                append_bar(parts, muted);
            }
            push_tail();
        } else if (tw + tailw <= w) {
            parts.push_back(text(cfg_.text, text_st));
            push_tail();
        } else if (w - tailw >= kMinWidth) {
            parts.push_back(text(truncate_end(cfg_.text, w - tailw), text_st));
            push_tail();
        } else {
            parts.push_back(text(truncate_end(cfg_.text, w), text_st));
        }
        return h(std::move(parts)).build();
    }

private:
    Config cfg_;

    void append_bar(std::vector<Element>& parts, Color muted) const {
        using namespace dsl;
        const int n      = cfg_.bar_cells;
        const float p    = std::clamp(cfg_.progress, 0.0f, 1.0f);
        const int halves = static_cast<int>(std::lround(p * static_cast<float>(n * 2)));
        std::string lit, rest;
        for (int i = 0; i < n; ++i) {
            if (halves >= 2 * (i + 1))      lit  += "\xe2\x94\x81";   // ━
            else if (halves == 2 * i + 1)   lit  += "\xe2\x95\xb8";   // ╸
            else                            rest += "\xe2\x94\x80";   // ─
        }
        if (!lit.empty())  parts.push_back(text(std::move(lit),  Style{}.with_fg(cfg_.tone)));
        if (!rest.empty()) parts.push_back(text(std::move(rest), Style{}.with_fg(muted).with_dim()));
    }
};

} // namespace maya
