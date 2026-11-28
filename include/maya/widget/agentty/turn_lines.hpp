#pragma once
// maya::turn_lines — the dense one-row lines inside agentty's synthetic turn
// cards, described as DATA. Each is exactly one terminal row at any width
// (a single truncating text node, so it can never wrap onto a second row);
// the styling of every segment is decided here, the host only fills fields.
//
//   ForkNote       prior transcript · ~/.agentty/threads/…md
//   RouteLine      → claude-sonnet-4  · effort high  · complex   <note>
//   LayerChips     ● orchestrate   ○ subagents
//   RetrievalMeta  ▰▰▰▰▱▱▱▱ 46%·moderate · 2 doc · 1 mem  (3)
//   SourceRow        └ docs · RUST-CRITIQUE.md:42  ×3  “Where a Rust…”

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../../element/element.hpp"
#include "../../style/color.hpp"
#include "../../style/style.hpp"
#include "line.hpp"

namespace maya {

namespace turn_lines_detail {

// Accumulates segments into one string + its per-byte style runs.
struct Runs {
    std::string            text;
    std::vector<StyledRun> runs;
    void push(std::string_view part, Style st) {
        if (part.empty()) return;
        runs.push_back(StyledRun{text.size(), part.size(), st});
        text.append(part);
    }
    [[nodiscard]] Element build() && {
        return Line::clipped(std::move(text), std::move(runs)).build();
    }
};

inline Style ink(Color c) { return Style{}.with_fg(c); }
inline Color muted()     { return Color::slot(ThemeSlot::Muted); }
inline Color link()      { return Color::slot(ThemeSlot::Link); }

} // namespace turn_lines_detail

// "prior transcript · <path>": where a fork's parent conversation lives.
struct ForkNote {
    std::string path;

    [[nodiscard]] Element build() const {
        using namespace turn_lines_detail;
        Runs r;
        r.push("prior transcript \xc2\xb7 ", ink(muted()));
        r.push(path, ink(link()));
        return std::move(r).build();
    }
    operator Element() const { return build(); }  // NOLINT(google-explicit-constructor)
};

// "→ <model>  · effort <e>  · <complexity>   <note>": one routing decision.
// Complexity reads on the status axis: complex = warning, standard = info,
// anything simpler recedes.
struct RouteLine {
    std::string model;
    std::string effort;       // "" = "off"
    std::string complexity;
    std::string note;

    [[nodiscard]] Element build() const {
        using namespace turn_lines_detail;
        const Color cx = complexity == "complex"  ? Color::slot(ThemeSlot::Warning)
                       : complexity == "standard" ? Color::slot(ThemeSlot::Info)
                                                  : muted();
        Runs r;
        r.push("\xe2\x86\x92 ", ink(muted()));                        // →
        r.push(model, ink(link()).with_bold());
        r.push("  \xc2\xb7 effort ", ink(muted()));
        r.push(effort.empty() ? "off" : effort, ink(Color::slot(ThemeSlot::Text)));
        r.push("  \xc2\xb7 ", ink(muted()));
        r.push(complexity, ink(cx));
        if (!note.empty()) {
            r.push("   ", Style{});
            r.push(note, ink(muted()).with_italic());
        }
        return std::move(r).build();
    }
    operator Element() const { return build(); }  // NOLINT(google-explicit-constructor)
};

// "● orchestrate   ○ subagents": which layers are on (filled, success) and
// which are off (hollow, muted).
struct LayerChips {
    struct Chip { std::string name; bool on = false; };
    std::vector<Chip> chips;

    [[nodiscard]] Element build() const {
        using namespace turn_lines_detail;
        Runs r;
        for (const auto& c : chips) {
            const std::size_t s = r.text.size();
            r.text += c.on ? "\xe2\x97\x8f " : "\xe2\x97\x8b ";   // ● / ○
            r.text += c.name;
            r.text += "   ";
            r.runs.push_back(StyledRun{s, r.text.size() - s,
                ink(c.on ? Color::slot(ThemeSlot::Success) : muted())});
        }
        return std::move(r).build();
    }
    operator Element() const { return build(); }  // NOLINT(google-explicit-constructor)
};

// The retrieval card's meta strip: a confidence gauge then a by-kind tally.
//   ▰▰▰▰▱▱▱▱ 46%·moderate · 2 doc · 1 mem  (3)
// Strength is on the status axis (≥60% success / ≥35% warning / else error)
// so the gauge's colour answers "trust this grounding?" at a glance. The
// gauge sits leftmost so a narrow row clips the tally, never the gauge.
struct RetrievalMeta {
    std::optional<double> confidence;                  // 0..1; none = no gauge
    std::vector<std::pair<std::string, int>> kinds;    // ("docs", 2), ("memory", 1)
    int passages = 0;                                  // total passages

    [[nodiscard]] static std::string short_kind(std::string k) {
        if (k == "memory")                   return "mem";
        if (k == "docs" || k == "doc")       return "doc";
        if (k == "skill" || k == "skills")   return "skill";
        if (k.size() > 5) k.resize(5);
        return k;
    }

    [[nodiscard]] Element build() const {
        using namespace turn_lines_detail;
        Runs r;
        if (confidence) {
            const double c = std::clamp(*confidence, 0.0, 1.0);
            constexpr int kCells = 8;
            const int filled = static_cast<int>(c * kCells + 0.5);
            const char* word = c >= 0.60 ? "strong" : c >= 0.35 ? "moderate" : "weak";
            const Color tint = c >= 0.60 ? Color::slot(ThemeSlot::Success)
                             : c >= 0.35 ? Color::slot(ThemeSlot::Warning)
                                         : Color::slot(ThemeSlot::Error);
            std::string on, off;
            for (int i = 0; i < filled; ++i)      on  += "\xe2\x96\xb0";   // ▰
            for (int i = filled; i < kCells; ++i) off += "\xe2\x96\xb1";   // ▱
            r.push(on,  ink(tint));
            r.push(off, ink(muted()));
            r.push(" " + std::to_string(static_cast<int>(c * 100.0 + 0.5)) + "%", ink(tint).with_bold());
            r.push(std::string("\xc2\xb7") + word, ink(tint).with_dim());
        }
        if (!r.text.empty()) r.push(" \xc2\xb7 ", ink(muted()));
        if (!kinds.empty()) {
            for (std::size_t i = 0; i < kinds.size(); ++i) {
                if (i) r.push(" \xc2\xb7 ", ink(muted()));
                r.push(std::to_string(kinds[i].second) + " ", ink(link()));
                r.push(short_kind(kinds[i].first), ink(muted()));
            }
            if (kinds.size() > 1 && passages > 0)
                r.push("  (" + std::to_string(passages) + ")", ink(muted()));
        } else {
            const int n = passages > 0 ? passages : 1;
            r.push(std::to_string(n) + " ", ink(link()));
            r.push(n == 1 ? "passage" : "passages", ink(muted()));
        }
        return std::move(r).build();
    }
    operator Element() const { return build(); }  // NOLINT(google-explicit-constructor)
};

// One provenance row: "  └ docs · path:42  ×3  “preview…”".
struct SourceRow {
    std::string kind;
    std::string path;      // "" = show the kind
    std::string line;      // "" = no :line
    int         chunks = 1;
    std::string preview;   // already quoted by the host when it wants quotes

    [[nodiscard]] Line line_of() const {
        using namespace turn_lines_detail;
        Runs r;
        r.push("  \xe2\x94\x94 ", ink(muted()));                     // └
        r.push(kind + " \xc2\xb7 ", ink(muted()));
        std::string p = path.empty() ? kind : path;
        if (!line.empty()) p += ":" + line;
        r.push(p, ink(link()));
        if (chunks > 1) r.push("  \xc3\x97" + std::to_string(chunks), ink(Color::slot(ThemeSlot::Warning)));
        if (!preview.empty()) {
            r.push("  ", ink(muted()));
            r.push(preview, ink(muted()));
        }
        return Line::clipped(std::move(r.text), std::move(r.runs));
    }
    operator Line() const { return line_of(); }  // NOLINT(google-explicit-constructor)
};

} // namespace maya
