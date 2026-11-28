#pragma once
// maya::ModelBadge — a colored model chip.
//
// PRESENTATION ONLY. This widget renders a label, an optional dim version
// run, and an optional leading dot in a caller-chosen colour. It does NOT
// know what a "model" is: no family taxonomy, no id parsing, no colour
// policy.
//
// That is a deliberate correction. This widget used to take a raw model id
// and parse vendor taxonomy out of it — `model_.find("opus")` for the family
// and a digit-run scan for the version. Both were wrong to live here:
//
//   • A rendering library has no access to the host's model catalog, so it
//     duplicated a classifier the host already owned — and drifted from it.
//     The unanchored substring match also misfired on aggregator ids
//     (`openrouter/anthropic/claude-opus-4`, `my-opus-finetune`), a failure
//     mode the host's positional tokeniser had already fixed.
//   • Compact mode returned BEFORE the version was appended, so a compact
//     badge silently dropped it — you could not tell Opus 4.5 from 4.8.
//   • The colour table here disagreed with the host's turn-header table
//     (Haiku green vs cyan), and green collided with the host's "status ok"
//     hue.
//
// maya owns every pixel; the host owns every meaning. The host decodes the
// id once (agentty: `domain/model_name.hpp`) and passes the result in.
//
//   ModelBadge{{ .label = "Opus", .version = "4.8",
//                .color = Color::slot(ThemeSlot::Accent) }}

#include <string>
#include <utility>

#include "../../dsl.hpp"
#include "../../style/style.hpp"

namespace maya {

class ModelBadge {
public:
    struct Config {
        // The model's display name. Rendered verbatim, bold, in `color`.
        std::string label;

        // Optional version run ("4.8"), rendered dim in `color` after the
        // label. Empty = omitted. Unlike the old widget-side extractor this
        // is never guessed, and `compact` no longer silently drops it.
        std::string version;

        // Family/identity hue. The caller owns colour POLICY (which family
        // is which hue, and which hues are reserved for status); the widget
        // just paints what it is given.
        Color color = Color::slot(ThemeSlot::Text);

        // Leading "● ". Off when the badge is composed into a larger chip
        // that places its own marker, or sits beside a filled provider tab
        // where a third marker is noise.
        bool show_dot = true;
    };

    explicit ModelBadge(Config c) : cfg_(std::move(c)) {}

    // Convenience for the common label-only case.
    explicit ModelBadge(std::string label)
        : cfg_{.label = std::move(label)} {}

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;

        std::vector<Element> parts;
        parts.reserve(3);

        if (cfg_.show_dot)
            parts.push_back(text("\xe2\x97\x8f ", Style{}.with_fg(cfg_.color)));  // ●
        parts.push_back(
            text(cfg_.label, Style{}.with_fg(cfg_.color).with_bold()));
        if (!cfg_.version.empty())
            parts.push_back(text(" " + cfg_.version,
                                 Style{}.with_fg(cfg_.color).with_dim()));

        return h(std::move(parts)).build();
    }

private:
    Config cfg_;
};

} // namespace maya
