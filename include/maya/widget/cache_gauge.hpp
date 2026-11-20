#pragma once
// maya::widget::CacheGauge — the prompt cache, legible at a glance.
//
// A provider's prompt (KV) cache is the difference between re-reading a
// 100k-token prefix in milliseconds and re-pricing it cold, which is 10-20x
// slower to the first token and far more expensive. The numbers exist on
// every usage frame, but a bare hit ratio cannot tell an EXPECTED cold start
// (the first request, a compaction, a long idle) from the one that matters:
// a cache that should have hit and did not. The host makes that call and
// hands this widget a verdict; the widget only draws it.
//
// One number, like the ctx gauge's percentage: how much of the prefix the
// last request left warm was served from cache. A healthy hit is ONLY the
// number. Anything else gets one word after it, because a number cannot say
// whether 0% is a fresh thread or a broken cache, and colour alone is lost on
// monochrome themes and to colour-blind eyes:
//
//   CACHE 94%        green   Hit        — reused (≥80% green, else plain text)
//   CACHE 0% new     info    Warming    — expected cold: new thread, compaction,
//                                         model switch
//   CACHE 3% idle    dim     Expired    — the idle gap outlived the cache
//   CACHE 2% miss    amber   Miss       — should have hit; the prefix changed
//   CACHE n/a        dim     Unreported — the provider never says
//
// Same typography as ContextGauge (bold muted label, value carries the
// colour, no background, no edge bar) — it is the gauge's neighbour and reads
// as one readout with it, not as a chip. Under width pressure the status bar
// drops the word first (the colour still carries it), then the whole gauge.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "../dsl.hpp"
#include "../element/element.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"
#include "../style/theme.hpp"

namespace maya {

class CacheGauge {
public:
    enum class State : std::uint8_t {
        Hidden,       // nothing to say (no data yet, or a prefix too small to cache)
        Hit,
        Warming,
        Expired,
        Miss,
        Unreported,
    };

    struct Config {
        State state = State::Hidden;
        int   pct   = 0;     // 0..100 of the warm prefix served from cache
        bool  note  = true;  // false → drop the explaining word (narrow rungs)
    };

    explicit CacheGauge(Config c) : cfg_(c) {}
    operator Element() const { return build(); }

    [[nodiscard]] bool visible() const noexcept { return cfg_.state != State::Hidden; }

    [[nodiscard]] Element build() const {
        using namespace dsl;
        if (!visible()) return blank().build();
        const Color muted = Color::slot(ThemeSlot::Muted);

        std::vector<Element> parts;
        // Like "CTX": no trailing space; each following segment owns its gap.
        parts.push_back(text("CACHE", Style{}.with_fg(muted).with_bold()));

        if (cfg_.state == State::Unreported) {
            parts.push_back(text(" n/a", fg_dim_(muted)));
            return h(std::move(parts)).build();
        }
        const int pct = std::clamp(cfg_.pct, 0, 100);
        Style vstyle;
        const char* word = nullptr;
        switch (cfg_.state) {
        case State::Hit:
            vstyle = Style{}.with_fg(hit_color(pct)).with_bold();
            break;
        case State::Warming:
            vstyle = Style{}.with_fg(Color::slot(ThemeSlot::Info));
            word = " new";
            break;
        case State::Expired:
            // Dim, not amber: the cache did exactly what it promised.
            vstyle = fg_dim_(Color::slot(ThemeSlot::Text));
            word = " idle";
            break;
        case State::Miss:
            vstyle = Style{}.with_fg(Color::slot(ThemeSlot::Warning)).with_bold();
            word = " miss";
            break;
        default: break;
        }
        parts.push_back(text(" " + std::to_string(pct) + "%", vstyle));
        if (word && cfg_.note) {
            // The miss keeps its alarm colour on the word; the expected
            // states explain themselves quietly.
            parts.push_back(text(word, cfg_.state == State::Miss
                                           ? vstyle
                                           : fg_dim_(muted)));
        }
        return h(std::move(parts)).build();
    }

    // Green when nearly the whole prefix is warm, plain text in between,
    // amber below that. (A Hit is always ≥50% reuse, so amber is for Miss.)
    [[nodiscard]] static Color hit_color(int pct) {
        if (pct >= 80) return Color::slot(ThemeSlot::Success);
        if (pct >= 50) return Color::slot(ThemeSlot::Text);
        return Color::slot(ThemeSlot::Warning);
    }

private:
    Config cfg_;

    static Style fg_dim_(Color c) {
        return theme::is_muted(c) ? Style{}.with_fg(c) : Style{}.with_fg(c).with_dim();
    }
};

} // namespace maya
