#pragma once
// maya::widget::CacheChip — the prompt cache, legible at a glance.
//
// A provider's prompt (KV) cache is the difference between re-reading a
// 100k-token prefix in milliseconds and re-pricing it cold, which is 10-20x
// slower to the first token and far more expensive. The numbers exist on
// every usage frame, but a bare hit ratio cannot tell an EXPECTED cold start
// (the first request, a compaction, a long idle) from the one that matters:
// a cache that should have hit and did not. The host makes that call and
// hands this widget a verdict; the widget only draws it.
//
//   CACHE 94%  115k       Hit       — % of the prefix served from cache
//   CACHE warming  +120k  Warming   — first write; expected, not a failure
//   CACHE expired  idle 12m          — the cache outlived its TTL
//   CACHE MISS  120k cold Miss      — should have hit; the prefix changed
//   CACHE n/a             Unreported — the provider says nothing about it
//
// Same typography as ContextGauge (bold muted label, value carries the
// colour) so the two read as one family on the status bar. `detail` adds the
// trailing token/idle field; the status bar sheds it first under width
// pressure, then the whole chip.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../dsl.hpp"
#include "../element/element.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"
#include "../style/theme.hpp"

namespace maya {

class CacheChip {
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
        State state     = State::Hidden;
        int   hit_pct   = 0;    // Hit: 0..100 of the prefix read from cache
        int   tokens    = 0;    // Hit: read · Warming: written · Miss: re-priced cold
        int   idle_secs = 0;    // Expired: the gap that outlived the cache
        bool  detail    = true; // false → label + verdict only (narrow rungs)
    };

    explicit CacheChip(Config c) : cfg_(c) {}
    operator Element() const { return build(); }

    [[nodiscard]] bool visible() const noexcept { return cfg_.state != State::Hidden; }

    [[nodiscard]] Element build() const {
        using namespace dsl;
        if (!visible()) return blank().build();
        const Color muted = Color::slot(ThemeSlot::Muted);

        std::vector<Element> parts;
        // Like "CTX": no trailing space; each following segment owns its gap.
        parts.push_back(text("CACHE", Style{}.with_fg(muted).with_bold()));

        std::string verdict, more;
        Style vstyle;
        switch (cfg_.state) {
        case State::Hit: {
            const int pct = std::clamp(cfg_.hit_pct, 0, 100);
            verdict = " " + std::to_string(pct) + "%";
            vstyle  = Style{}.with_fg(hit_color(pct)).with_bold();
            if (cfg_.tokens > 0) more = " " + compact_tokens(cfg_.tokens);
            break;
        }
        case State::Warming:
            verdict = " warming";
            vstyle  = Style{}.with_fg(Color::slot(ThemeSlot::Info));
            if (cfg_.tokens > 0) more = " +" + compact_tokens(cfg_.tokens);
            break;
        case State::Expired:
            // Muted, not amber: the cache did exactly what it promised.
            verdict = " expired";
            vstyle  = fg_dim_(Color::slot(ThemeSlot::Text));
            if (cfg_.idle_secs > 0) more = " idle " + compact_duration(cfg_.idle_secs);
            break;
        case State::Miss:
            verdict = " MISS";
            vstyle  = Style{}.with_fg(Color::slot(ThemeSlot::Warning)).with_bold();
            if (cfg_.tokens > 0) more = " " + compact_tokens(cfg_.tokens) + " cold";
            break;
        case State::Unreported:
            verdict = " n/a";
            vstyle  = fg_dim_(muted);
            break;
        case State::Hidden:
            break;
        }
        parts.push_back(text(std::move(verdict), vstyle));
        if (cfg_.detail && !more.empty())
            parts.push_back(text(std::move(more), fg_dim_(muted)));
        return h(std::move(parts)).build();
    }

    // "850", "12k", "1.2k", "115k", "1.4M" — short and unpadded. Unlike the
    // ctx gauge this field is not tabular: it changes once per request, not
    // per frame, so there is no twitch to guard against.
    [[nodiscard]] static std::string compact_tokens(int n) {
        char buf[16];
        if (n >= 1'000'000)
            std::snprintf(buf, sizeof buf, "%.1fM", n / 1'000'000.0);
        else if (n >= 10'000)
            std::snprintf(buf, sizeof buf, "%dk", (n + 500) / 1000);
        else if (n >= 1000)
            std::snprintf(buf, sizeof buf, "%.1fk", n / 1000.0);
        else
            std::snprintf(buf, sizeof buf, "%d", n);
        return buf;
    }

    [[nodiscard]] static std::string compact_duration(int secs) {
        if (secs < 60)   return std::to_string(secs) + "s";
        if (secs < 3600) return std::to_string(secs / 60) + "m";
        return std::to_string(secs / 3600) + "h" +
               (secs % 3600 >= 60 ? std::to_string((secs % 3600) / 60) + "m" : "");
    }

    // Green when nearly the whole prefix is warm, plain text in between,
    // amber when most of it was re-read cold.
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
