#pragma once
// maya::widget::Conversation — vertical list of turns with dim dividers
// between them and an optional trailing in-flight indicator.
//
// Owned:
//   - Width-aware dim ─ separator between consecutive turns.
//   - Trailing in-flight slot.
//   - Outer padding(0,1) + grow(1.0f) so the panel fills its column.
//
// NOT owned:
//   - Per-turn rendering — caller passes pre-built turn Elements
//     (typically maya::Turn instances).
//   - Empty-state / welcome — caller decides what to render when the
//     list is empty.
//
//   maya::Conversation{{
//       .turns     = built_turn_elements,
//       .in_flight = activity_indicator_element,    // optional
//   }}.build();

#include <optional>
#include <utility>
#include <vector>

#include "../../dsl.hpp"
#include "../../element/element.hpp"
#include "../../render/scrollback_ledger.hpp"
#include "../../style/color.hpp"
#include "../../style/style.hpp"

#include "activity_indicator.hpp"
#include "turn.hpp"

namespace maya {

class Conversation {
public:
    // A pre-built turn Element + the continuation flag. The host
    // (moha) maintains its own (thread, message) → Element cache and
    // hands settled turns straight through as Element values, so a
    // long session's per-frame cost stays O(visible_live_turns)
    // instead of O(visible_total_turns × tool_cards_per_turn). This
    // is the same pattern the agent_session example uses with
    // `m.frozen` + `list_ref` — settled rendering is computed once
    // per turn lifetime, not once per frame. The continuation bit
    // drives the inter-turn rule below so callers don't have to
    // recreate that layout policy in their host.
    struct PreBuilt {
        Element element;
        bool    continuation = false;
    };

    struct Config {
        // Original config-based path. Kept for callers that haven't
        // adopted the Element cache yet — every Turn::Config in this
        // vector gets `Turn{cfg}.build()` called on it once per frame.
        std::vector<Turn::Config>                turns;
        // Middle path: when non-empty, takes precedence over `turns`.
        // Each entry is a fully-built turn Element; build() lays them
        // out with dividers between non-continuation entries.
        //
        // Per-frame cost: build() copies each `element` into the rows
        // vector once. To make the ComponentElements inside survive
        // those copies, hosts that want render-cache-stable settled
        // turns should hand in Elements whose ComponentElement
        // wrappers carry a `hash_id` (see element.hpp). Maya keeps
        // the rendered result indexed by hash rather than by pointer,
        // so the freshly-copied wrapper at this slot still hits the
        // cache produced by the original.
        std::vector<PreBuilt>                    built_turns;

        // Fastest AND safest path (Witness Chain — Trim Accounting).
        // When `ledger != nullptr`, takes precedence over everything
        // below. Renders the ledger's sealed blocks through
        // `ledger_ref` — zero-copy like `frozen`, plus the paint pass
        // records every block's laid-out height back into the ledger,
        // so the host's front-trims mint their scrollback commit
        // counts from maya's OWN measurements (ScrollbackLedger::
        // harvest → the commit_scrollback effect). A host
        // that trims a sealed prefix MUST use this path; the bare
        // `frozen` pointer remains for hosts that never trim.
        const ScrollbackLedger*                  ledger           = nullptr;

        // Fastest path (agent_session pattern). When `frozen != nullptr`,
        // takes precedence over both `built_turns` and `turns`.
        //
        // `frozen` is a BORROWED pointer to the host's append-only
        // vector of fully-built scrollback Elements (one row per entry:
        // turn body, or a divider/gap pushed by the host). build()
        // hands maya a `list_ref(*frozen)` — zero-copy across frames,
        // regardless of how large the prefix grows. Every Element
        // inside benefits from maya's hash_id-keyed layout + cell
        // cache, so per-frame cost stays O(visible_live) instead of
        // O(total_turns).
        //
        // The host owns inter-turn dividers: push them into `*frozen`
        // at the moments you want them (typically a one-row gap
        // before each fresh-speaker turn). This widget adds NO
        // dividers when the frozen path is active — what you see is
        // exactly the rows in `*frozen`, followed by `live_tail` (if
        // non-empty), followed by `in_flight` (if set).
        //
        // Lifetime: the pointer must outlive build(). Typical use is
        // a member of the host's model that lives across frames.
        const std::vector<Element>*              frozen           = nullptr;

        // The live tail — one entry per in-flight or unfrozen Turn.
        // Rendered after the frozen prefix in order. The host is
        // responsible for any leading divider/gap between the last
        // frozen row and the first live entry — push that into
        // `*frozen` at the time it freezes the previous turn, OR
        // prepend a gap as the first `live_tail` entry. Same rule
        // for between live entries: include the gap yourself.
        std::vector<Element>                     live_tail;

        std::optional<ActivityIndicator::Config> in_flight;
    };

    explicit Conversation(Config c) : cfg_(std::move(c)) {}

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;

        // ── Fastest path: borrowed sealed/frozen prefix + live tail. ─
        // The host owns dividers inside the prefix and between prefix
        // and live_tail; we just concatenate. `ledger` (measured,
        // trim-safe) wins over the bare `frozen` pointer.
        if (cfg_.ledger != nullptr || cfg_.frozen != nullptr) {
            std::vector<Element> rows;
            rows.reserve(cfg_.live_tail.size() + 3);
            rows.push_back(cfg_.ledger != nullptr
                               ? ledger_ref(*cfg_.ledger)
                               : list_ref(*cfg_.frozen));
            for (const auto& e : cfg_.live_tail) rows.push_back(e);
            if (cfg_.in_flight)
                rows.push_back(ActivityIndicator{*cfg_.in_flight}.build());
            // Trailing flex spacer absorbs the leftover vertical space
            // so the last Turn doesn't stretch its rail downward into
            // the gap above the composer. (The composer anti-bounce pad
            // lives in AppLayout::build as the vstack's LAST child — a
            // pad here would be absorbed by this spacer's grow and never
            // reach content_height.)
            rows.push_back(spacer().build());
            return (v(rows) | padding(0, 1) | grow(1.0f)).build();
        }

        std::vector<Element> rows;
        const bool use_built = !cfg_.built_turns.empty();
        const std::size_t n = use_built ? cfg_.built_turns.size()
                                        : cfg_.turns.size();
        rows.reserve(n * 2 + 2);
        for (std::size_t i = 0; i < n; ++i) {
            // Skip the inter-turn rule before a continuation turn so the
            // rail flows uninterrupted through a same-speaker run.
            const bool continuation = use_built ? cfg_.built_turns[i].continuation
                                                : cfg_.turns[i].continuation;
            if (i > 0 && !continuation)
                rows.push_back(divider_rule());
            rows.push_back(use_built ? cfg_.built_turns[i].element
                                     : Turn{cfg_.turns[i]}.build());
        }
        if (cfg_.in_flight)
            rows.push_back(ActivityIndicator{*cfg_.in_flight}.build());
        rows.push_back(spacer().build());
        return (v(rows) | padding(0, 1) | grow(1.0f)).build();
    }

    // Build the standard width-aware dim inter-turn rule. Exposed so
    // hosts using the `frozen` path can push the same divider Element
    // into their frozen vector at turn boundaries, matching the look
    // of the legacy auto-dividers from the built_turns path.
    [[nodiscard]] static Element divider() { return divider_rule(); }

    // The inter-turn gap: the rule alone (compact), or blank, rule, blank.
    // Its height is `gap_rows(compact)`; a host sealing it into scrollback
    // must use that number, never a constant.
    [[nodiscard]] static Element gap(bool compact) {
        using namespace dsl;
        if (compact) return divider_rule();
        return v(blank(), divider_rule(), blank()).build();
    }
    [[nodiscard]] static constexpr int gap_rows(bool compact) noexcept { return compact ? 1 : 3; }

    // The gap minus its trailing blank: what replayed history adds above a
    // live frame whose top padding row stands in for that blank.
    [[nodiscard]] static Element gap_lead(bool compact) {
        using namespace dsl;
        if (compact) return divider_rule();
        return v(blank(), divider_rule()).build();
    }

    // Replayed history sits on the columns the live prefix had: AppLayout's
    // padding(1) plus Conversation's padding(0, 1), two columns each side.
    [[nodiscard]] static Element scrollback_wrap(Element e) {
        using namespace dsl;
        return (v(std::move(e)) | padding(0, 2)).build();
    }

    // Prose held to a reading measure: a column at most `cap` wide, the
    // rest of the row an empty flexible cell. Left-aligned, so it stays on
    // the column the rail and every turn above start at. A CEILING: a
    // narrower terminal gives the row less and the column shrinks with it.
    // cap <= 0 = no cap.
    [[nodiscard]] static Element prose_capped(Element body, int cap) {
        using namespace dsl;
        if (cap <= 0) return body;
        return h(v(std::move(body)) | width(cap), v() | grow_<1>).build();
    }

    // A one-row labelled section break, centred between ┈ rules:
    //   ┈┈┈┈┈  ≡ Conversation compacted  ┈┈┈┈┈
    //   ┈┈┈┈┈  → continued in agentty · from Codex  ┈┈┈┈┈
    // Narrower than the label plus 4 columns of rule, it sits left-aligned.
    // Exactly ONE row by construction: both the frozen and the live builder
    // emit it at the boundary, and a height delta there ghosts scrollback.
    // `from` is the dim trailing source (empty = none); `cache_tag` is the
    // stable hash id (all copies at a width render the same cells).
    struct SectionBreak {
        std::string glyph, label, from;
        std::vector<std::string_view> cache_tag;
        Color accent    = Color::slot(ThemeSlot::Accent);
        Color secondary = Color::slot(ThemeSlot::Muted);
        Color muted     = Color::slot(ThemeSlot::Muted);
    };
    [[nodiscard]] static Element section_break(SectionBreak sb) {
        using namespace dsl;
        CacheIdBuilder id;
        for (auto t : sb.cache_tag) id.add(t);
        const auto hash = id.build();
        return component([sb = std::move(sb)](int w, int /*h*/) -> Element {
            if (w <= 0) return blank().build();
            const Style dim_m = Style{}.with_fg(sb.muted).with_dim();
            const Style lab   = Style{}.with_fg(sb.secondary).with_dim();
            int label_cells = 2 + static_cast<int>(sb.label.size());
            if (!sb.from.empty()) label_cells += 3 + string_width(sb.from);
            const int rule_total = w - label_cells - 4;
            if (rule_total < 4)
                return h(text("   "),
                         text(sb.glyph + " ", Style{}.with_fg(sb.accent)),
                         text(sb.label, lab)).build();
            const int left = rule_total / 2, right = rule_total - left;
            std::string lrule, rrule;
            lrule.reserve(static_cast<std::size_t>(left) * 3);
            rrule.reserve(static_cast<std::size_t>(right) * 3);
            for (int i = 0; i < left; ++i)  lrule += "\xe2\x94\x88";   // ┈
            for (int i = 0; i < right; ++i) rrule += "\xe2\x94\x88";
            std::vector<Element> p;
            p.push_back(text(std::move(lrule), dim_m));
            p.push_back(text("  "));
            p.push_back(text(sb.glyph, Style{}.with_fg(sb.accent)));
            p.push_back(text(" "));
            p.push_back(text(sb.label, lab));
            if (!sb.from.empty()) {
                p.push_back(text(" \xc2\xb7 ", dim_m));
                p.push_back(text(sb.from, dim_m));
            }
            p.push_back(text("  "));
            p.push_back(text(std::move(rrule), dim_m));
            return h(std::move(p)).build();
        })
        .hash_id(hash);
    }

private:
    Config cfg_;

    // Width-aware thin dim rule, indented 3 columns. The "next turn
    // starts here" handhold without the heaviness of a full-weight rule.
    //
    // Stable hash_id: every divider at a given terminal width renders
    // to byte-identical cells, so all N dividers in a long frozen
    // prefix can share one cached cells-blit. The renderer's hash
    // cache stores (cells, width) per entry and rejects width
    // mismatches automatically, so a single id is correct across
    // resizes (resize misses, re-paints, re-caches).
    static Element divider_rule() {
        using namespace dsl;
        return component([](int w, int /*h*/) -> Element {
            if (w <= 0) return blank().build();
            std::string line;
            constexpr int kIndent = 3;
            for (int i = 0; i < kIndent; ++i) line += ' ';
            for (int i = kIndent; i < w; ++i) line += "\xe2\x94\x80"; // ─
            return text(std::move(line),
                        Style{}.with_fg(Color::slot(ThemeSlot::Muted)).with_dim()).build();
        })
        .hash_id(CacheIdBuilder{}
            .add(std::string_view{"maya.conversation.divider"})
            .build());
    }
};

} // namespace maya
