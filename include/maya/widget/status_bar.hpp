#pragma once
// maya::widget::StatusBar — bottom-of-app activity / status / shortcuts row.
//
// Five-row panel composed of nested sub-widget Configs. Hosts fill
// each sub-Config (or one Config builder per sub-widget — the moha
// adapters do this) and StatusBar drives the layout:
//
//   ▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔   ← PhaseAccent (top, dim)
//    TitleChip ·  PhaseChip   …    TokenStreamSparkline   ●Model · ContextGauge
//   ▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁   ← PhaseAccent (bottom)
//
// When `status_banner.text` is non-empty the activity row is replaced
// by a full-width toast in the same vertical slot — bright foreground
// + accent background span the whole row so transient status ("context
// compacted", "retrying in 3s…", "error: rate limited") is impossible
// to miss. The toast collapses back to the regular activity row the
// moment its TTL expires; height stays constant either way.
//
// Width-adaptive by MEASUREMENT: the activity row builds its real
// fragments at progressively leaner shapes and takes the richest one
// whose measured width fits — no thresholds, nothing to tune.
//
//   maya::StatusBar{{
//       .phase_color   = phase_color(m.s.phase),
//       .breadcrumb    = title_chip_config(m),
//       .phase         = phase_chip_config(m),
//       .token_stream  = token_stream_sparkline_config(m),
//       .model_badge   = model_badge_config(m).build(),
//       .context       = context_gauge_config(m),
//       .status_banner = status_banner_config(m),  // toast — hides activity row when present
//   }}.build();

#include <algorithm>
#include <utility>
#include <vector>

#include "../dsl.hpp"
#include "../element/element.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"

#include "context_gauge.hpp"
#include "cache_gauge.hpp"
#include "phase_accent.hpp"
#include "phase_chip.hpp"
#include "status_banner.hpp"
#include "title_chip.hpp"
#include "token_stream_sparkline.hpp"
#include "../style/theme.hpp"

namespace maya {

class StatusBar {
public:
    // The separator between activity-row groups, and its column width.
    //
    // ONE definition, and the width is DERIVED from the literal rather than
    // restated beside it. The width is consumed by three fit calculations
    // (the title chip's leftover budget, the adaptive stream slot's minimum,
    // and the right group's shed ladder) that must all agree with what is
    // actually painted — and it used to be a literal repeated at four sites
    // plus a hand-copied `kSepW = 7`. Narrowing the separator left that
    // constant reserving four columns nobody drew, so the title over-shed on
    // exactly the narrow terminals the ladder exists for. Now the compiler
    // keeps them in step: edit kSep and kSepW follows.
    //
    // " · " rather than "   ·   ": the activity row is a dense ambient
    // strip, and 7 columns per join is table typography in a place that is
    // not a table.
    static constexpr std::string_view kSep  = " \xc2\xb7 ";
    static constexpr int              kSepW = unicode::str_width(kSep);

    struct Config {
        // Frame coloring — drives the top/bottom PhaseAccent strips
        // and the leading rail glyph next to the phase chip.
        Color phase_color = Color::slot(ThemeSlot::Info);

        // Activity row sub-widget configs.
        TitleChip::Config            breadcrumb;       // empty title = hide
        PhaseChip::Config            phase;
        TokenStreamSparkline::Config token_stream;
        Element                      model_badge;     // default-empty = hide
        ContextGauge::Config         context;          // max=0 = hide
        // Prompt-cache verdict, just left of the ctx gauge. state=Hidden =
        // hide. Sheds after the sparkline (its detail first, before even the
        // ctx token counts), and before the phase or the ctx gauge ever do.
        CacheGauge::Config           cache;

        // Status row — takes over the activity_row slot when present.
        StatusBanner::Config         status_banner;

        // Identity of everything above, for the cross-frame component cache.
        //
        // The activity row is a measured degradation ladder (see below): it
        // builds each sub-widget as a real styled fragment and MEASURES it,
        // up to eight times, to find the richest shape that fits. That is
        // the right way to lay it out and a poor thing to redo 30 times a
        // second — profiling a streaming session put activity_row at 53% of
        // all render time, because the row is rebuilt every frame even when
        // nothing in it changed.
        //
        // When this is non-zero the built row is cached under it, so the
        // ladder runs only when the key moves. Leave it 0 (the default) and
        // nothing changes — every frame rebuilds, exactly as before.
        //
        // OPT-IN, and deliberately so. A key that misses an input paints a
        // stale status bar, which is worse than the cost of rebuilding: the
        // row would silently stop tracking the thing it exists to report.
        // So the host states the identity, because the host is what knows
        // when `model_badge` — an opaque Element this widget cannot hash —
        // actually changed.
        std::uint64_t content_key = 0;

        // NO width thresholds. The activity row is a measured
        // degradation ladder: every piece is built as its real styled
        // fragment and the row sheds detail step by step — breadcrumb
        // → ctx token counts → token-stream chip → phase elapsed → ctx
        // bar → phase verb → ctx gauge → model badge — until the
        // measured total fits the width the row was actually given.
        // The phase chip (what's happening now) is the last survivor.
    };

    explicit StatusBar(Config c) : cfg_(std::move(c)) {}

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;

        const Color pcolor = cfg_.phase_color;

        // Toast takeover: when a status message is set, the middle row
        // becomes a full-width banner instead of the regular activity
        // strip. Same height, same surrounding accents — just the
        // payload swaps. Maximally visible: bright fg + accent bg span
        // the entire content row so the message reads as a screen-wide
        // alert, not a tucked-in subtitle. Once the TTL expires the
        // host clears `status_banner.text` and the activity row
        // returns. Five-row layout collapses to three rows now that
        // ShortcutRow is gone (shortcuts moved to the welcome screen).
        std::vector<Element> rows;
        rows.reserve(3);
        rows.push_back(PhaseAccent{{.color = pcolor,
                                    .position = PhaseAccent::Position::Top}}.build());
        // The middle row (activity strip or toast) is hard-locked to
        // exactly one cell tall AND clipped. A squeezed sub-widget (the
        // context gauge, phase chip, model badge, breadcrumb) can word-
        // wrap its text to a second line when the terminal is narrow;
        // that 2-line child would otherwise paint past the bottom accent
        // and grow the whole status bar from 3 rows to 4. height(1)
        // fixes the slot to one cell and overflow:Hidden clips anything
        // the (already width-pruned) content can't fit — so the bar is a
        // stable single line at every width, truncated at the edge
        // rather than wrapped onto a phantom row.
        rows.push_back((cfg_.status_banner.text.empty()
                            ? activity_row()
                            : toast_row())
                       | height(1) | overflow(Overflow::Hidden));
        rows.push_back(PhaseAccent{{.color = pcolor,
                                    .position = PhaseAccent::Position::Bottom}}.build());
        return v(std::move(rows)).build();
    }

private:
    Config cfg_;

    [[nodiscard]] Element activity_row() const {
        using namespace dsl;
        Element el = component([cfg = cfg_](int w, int /*h*/) -> Element {
            using namespace dsl;
            if (w <= 0) return blank().build();

            const Color muted = Color::slot(ThemeSlot::Muted);
            const Color pcolor = cfg.phase_color;
            const bool  active = cfg.phase.breathing;

            auto width_of = [](const Element& el) {
                // NATURAL width (unbounded measure). Measuring at the slot
                // width clamps: a too-wide candidate reports ≤ w and the
                // ladder accepts a shape that does NOT actually fit — the
                // row then paints past the edge and overflow:Hidden clips
                // the right group (the "CTX gauge cut mid-word" artifact).
                return measure_element(el, /*max_width=*/1 << 14).width.value;
            };

            // ── Measured degradation ladder ──
            // Build the row's REAL fragments at progressively leaner
            // shapes and take the richest one whose measured width
            // fits. No thresholds, no estimates — a relabeled model
            // badge or a longer verb re-decides by itself. Shed order
            // (first → last): cache detail · ctx token counts ·
            // token-stream chip · cache gauge · phase elapsed · ctx bar ·
            // phase verb · ctx gauge · model badge. The breadcrumb (most
            // expendable of all) is added LAST from whatever measured
            // leftover remains.
            struct Shape {
                bool cdetail, tokens, stream, cache, elapsed, bar, verb, gauge, badge;
            };
            static constexpr Shape kLadder[] = {
                {true,  true,  true,  true,  true,  true,  true,  true,  true },
                {false, true,  true,  true,  true,  true,  true,  true,  true },
                {false, false, true,  true,  true,  true,  true,  true,  true },
                {false, false, false, true,  true,  true,  true,  true,  true },
                {false, false, false, false, true,  true,  true,  true,  true },
                {false, false, false, false, false, true,  true,  true,  true },
                {false, false, false, false, false, false, true,  true,  true },
                {false, false, false, false, false, false, false, true,  true },
                {false, false, false, false, false, false, false, false, true },
                {false, false, false, false, false, false, false, false, false},
            };

            Element right, phase_el, middle;
            bool stream_on = false;
            int fixed_w = 0;
            // Which fragments the CHOSEN rung actually kept. The title is
            // admitted below only when the richest fragments survived — see
            // the note there.
            bool kept_verb = false, kept_gauge = false;
            for (const Shape& s : kLadder) {
                PhaseChip::Config pc = cfg.phase;
                pc.verb_width = s.verb ? 10 : 0;
                if (!s.elapsed) pc.elapsed_secs = -1.0f;

                ContextGauge::Config ctx = cfg.context;
                ctx.show_bar    = s.bar;
                ctx.show_tokens = s.tokens;

                std::vector<Element> rparts;
                bool emitted_right = false;
                // Separators are emitted BEFORE each segment that follows
                // something, never trailing a segment speculatively. The
                // old code appended "   ·   " right after the sparkline on
                // the assumption that a badge or gauge would follow it —
                // true while the model badge lived here, but once the badge
                // moved to the composer footer (and any time the badge is
                // empty) the sparkline's trailing separator collided with
                // the gauge's leading one, painting "·   ·" with a dead
                // gap between them.
                auto sep = [&]() {
                    if (emitted_right)
                        rparts.push_back(text(kSep, fg_dim_(muted)));
                };
                if (s.stream && !cfg.token_stream.adaptive) {
                    rparts.push_back(TokenStreamSparkline{cfg.token_stream}.build());
                    emitted_right = true;
                }
                if (s.badge && width_of(cfg.model_badge) > 0) {
                    sep();
                    rparts.push_back(cfg.model_badge);
                    emitted_right = true;
                }
                if (s.cache && cfg.cache.state != CacheGauge::State::Hidden) {
                    CacheGauge::Config cc = cfg.cache;
                    cc.detail = s.cdetail;
                    sep();
                    rparts.push_back(CacheGauge{cc}.build());
                    emitted_right = true;
                }
                if (cfg.context.max > 0 && s.gauge) {
                    sep();
                    rparts.push_back(ContextGauge{ctx}.build());
                    emitted_right = true;
                }
                rparts.push_back(text(" "));
                Element cand_right = h(std::move(rparts)).build();
                Element cand_phase = PhaseChip{pc}.build();

                // The adaptive stream chip rides the middle slot and
                // shrinks by itself — it only needs a nominal minimum.
                //
                // When it is NOT riding, the middle is a bare spacer(), which
                // is a grow:1 box with a natural width of ZERO. A spacer can
                // therefore be squeezed to nothing, and the fit test used to
                // ask only whether phase + right fit — never that anything
                // separated them. At the exact width where `need == w` the
                // spacer collapsed to 0 and the phase chip's elapsed tail sat
                // flush against the gauge's leading "CTX": "1.9sCTX".
                //
                // Two touching segments read as one token, so the strip must
                // reserve the gap the same way it reserves the glyphs: one
                // column that belongs to the seam, not to either neighbour.
                // If that column cannot be paid for, the rung is too rich and
                // the ladder sheds — collision is never the cheaper option.
                // Does anything actually sit to the right? ONE predicate,
                // consumed by both the reservation below and the element
                // built from it — if these two ever disagreed, the strip
                // would either reserve a column it never draws (a phantom
                // gap) or draw one it never reserved (the collision again).
                const bool has_right = width_of(cand_right) > 0;

                const int middle_min = (s.stream && cfg.token_stream.adaptive)
                    ? 8 + kSepW   // spark floor + separator
                    : (has_right ? 1 : 0);  // seam gap

                const int need = 1 /*lead sp*/ + 1 /*rail*/ + 1 /*sp*/
                    + width_of(cand_phase) + middle_min
                    + width_of(cand_right);
                const bool last = &s == &kLadder[std::size(kLadder) - 1];
                if (need <= w || last) {
                    right      = std::move(cand_right);
                    phase_el   = std::move(cand_phase);
                    stream_on  = s.stream;
                    fixed_w    = need;
                    kept_verb  = s.verb;
                    kept_gauge = s.gauge && cfg.context.max > 0;
                    // A grow:1 spacer distributes SLACK, so it cannot be
                    // relied on to hold the seam open at the tight width —
                    // pair it with one hard column that no layout pass can
                    // squeeze out. Emitted only when something is actually to
                    // the right, so a bare strip grows no phantom trailing
                    // space (that would be the dangling-separator bug in
                    // whitespace form).
                    middle = (s.stream && cfg.token_stream.adaptive)
                        ? (h(TokenStreamSparkline{cfg.token_stream}.build(),
                             text(kSep, fg_dim_(muted))) | grow(1.0f)).build()
                        : (has_right
                            ? (h(spacer(), text(" ")) | grow(1.0f)).build()
                            : Element{spacer().build()});
                    break;
                }
            }
            (void)stream_on;

            // ── Left group: breadcrumb + ▌ rail + phase chip. The
            //    breadcrumb is the most expendable fragment, so it is
            //    added LAST — and by MEASUREMENT, like every other
            //    fragment: build the real chip at the candidate cap and
            //    include it only if its measured width (chip glyphs +
            //    separator) provably fits the leftover. The previous
            //    hand-arithmetic (`max_chars = leftover`) ignored the
            //    chip's own ▎+space overhead and the ellipsis, so a
            //    2-3 col overflow shoved the right group off the edge
            //    exactly when space was tightest.
            std::vector<Element> lparts;
            lparts.push_back(text(" "));
            //
            //    The title is admitted ONLY when the chosen rung kept the
            //    phase verb and the context gauge. Fitting in the leftover
            //    columns is necessary but NOT sufficient: the ladder sheds
            //    by VALUE, and a title outranking the verb it sits next to
            //    inverts that order — you would learn what the thread is
            //    called but not whether it is running. Leftover space alone
            //    could satisfy the width test on a rung that had already
            //    dropped both, which is exactly what a narrower separator
            //    made reachable.
            const bool title_allowed = kept_verb && kept_gauge;
            if (!cfg.breadcrumb.title.empty() && title_allowed) {
                constexpr int kChipDecor = 2;   // ▎ + space
                constexpr int kMinTitle  = 12;  // below this a title is noise
                const int leftover = w - fixed_w - kSepW;
                if (leftover >= kChipDecor + kMinTitle) {
                    TitleChip::Config bc = cfg.breadcrumb;
                    const std::size_t cfg_cap =
                        cfg.breadcrumb.max_chars > 0 ? cfg.breadcrumb.max_chars : 28;
                    bc.max_chars = std::min<std::size_t>(
                        static_cast<std::size_t>(leftover - kChipDecor), cfg_cap);
                    Element chip = TitleChip{bc}.build();
                    // Proof pass: accept only if the REAL rendered chip fits.
                    if (width_of(chip) + kSepW <= w - fixed_w) {
                        lparts.push_back(std::move(chip));
                        lparts.push_back(text(kSep, fg_dim_(muted)));
                    }
                }
            }
            Style rail_style = active
                ? Style{}.with_fg(pcolor).with_bold()
                : Style{}.with_fg(pcolor).with_dim();
            lparts.push_back(text("\xe2\x96\x8c", rail_style));             // ▌
            // ONE space after the rail, owned here. PhaseChip's internal pad
            // sits between its glyph and its verb, not ahead of the glyph —
            // so dropping this made the spinner touch the rail. The chip's
            // pad is now tied to the glyph's presence, which is what makes
            // this space unambiguously ours: the chip contributes nothing
            // before its own first character, whether or not it has a glyph.
            lparts.push_back(text(" "));
            lparts.push_back(std::move(phase_el));
            auto left = h(std::move(lparts));

            // overflow:Hidden guarantees the activity row is ALWAYS
            // exactly one line: if even the leanest measured shape can't
            // fit an extremely narrow terminal, the content is clipped
            // at the right edge rather than wrapping onto a phantom
            // second row that would shove the Thread above it and
            // trigger a full-viewport repaint.
            return (h(left, std::move(middle), std::move(right))
                    | overflow(Overflow::Hidden)).build();
        });
        // Hand the renderer a stable identity so the ladder above is run
        // once per distinct row rather than once per frame. The key is
        // MIXED with a tag rather than used raw: cache ids are global, and
        // a bare host counter could collide with an unrelated component
        // that happened to pick the same number.
        if (cfg_.content_key != 0) {
            if (auto* comp = std::get_if<ComponentElement>(&el.inner)) {
                comp->hash_id = CacheIdBuilder{}
                                    .add(std::string_view{"maya.status_bar.activity"})
                                    .add(cfg_.content_key)
                                    .build();
            }
        }
        return el;
    }

    static Style fg_dim_(Color c) {
        const bool already_muted = theme::is_muted(c);
        return already_muted
            ? Style{}.with_fg(c)
            : Style{}.with_fg(c).with_dim();
    }

    // Full-width toast row — the activity slot's high-visibility twin.
    // A leading rail glyph + message text, painted on an RGB-anchored
    // background that spans the entire row.
    //
    // We deliberately bypass the user's ANSI palette for the band's
    // bg/fg pair: terminal themes remap `bright_yellow`/`bright_cyan`
    // wildly (Gruvbox makes bright_yellow look like dark amber, Solarized
    // makes bright_cyan look teal), which means a luminance-based
    // contrast pick computed against the *default* xterm palette stops
    // matching reality. RGB triples render the same everywhere a
    // truecolor terminal is in use, so the band is guaranteed visible.
    //
    // Severity→palette:
    //   Error → deep red bg     + bright white fg
    //   Warn  → amber bg         + near-black fg
    //   Info  → desaturated indigo bg + bright white fg
    [[nodiscard]] Element toast_row() const {
        using namespace dsl;
        using Kind = StatusBanner::Kind;
        const Kind kind = cfg_.status_banner.effective_kind();

        // Theme-independent RGB palette. WCAG contrast ratios computed
        // against the chosen fg:
        //   error: #b91c1c on #ffffff → 7.6:1  (AAA)
        //   warn : #f59e0b on #0b0b0b → 9.4:1  (AAA)
        //   info : #3b4a6b on #ffffff → 8.9:1  (AAA)
        // THEME SLOTS. The contrast ratios noted above were measured
        // against ONE canvas, which is the tell: they hold on the terminal
        // they were sampled on and nowhere else. Under theme::native — all
        // Named/Default, so the user's own palette reaches the screen — a
        // hardcoded crimson-on-white banner overrode colours they had
        // already contrast-checked for themselves.
        //
        // error/warning/info are the slots that MEAN these three states,
        // every scheme ships them contrast-checked, and on native they map
        // to the ANSI red/yellow/cyan every terminal has meant since the
        // seventies.
        //
        // The band stays on every theme; the INK comes from on_band(),
        // which measures a real RGB band but uses REVERSE VIDEO for a
        // palette one. Under native these are ANSI red/yellow/cyan, whose
        // actual values only the terminal knows — measuring the standard
        // table picked white ink and the toast came out unreadable on a
        // remapped palette.
        const Theme& th = theme::live();

        const Color state = (kind == Kind::Error) ? Color::slot(ThemeSlot::Error)
                          : (kind == Kind::Warn)  ? Color::slot(ThemeSlot::Warning)
                                                  : Color::slot(ThemeSlot::Info);
        const Style band = on_band(th.resolve(state));

        const std::string& msg = cfg_.status_banner.text;
        const char* glyph =
            (kind == Kind::Error) ? "\xe2\x9c\x97"   // ✗
          : (kind == Kind::Warn)  ? "\xe2\x9a\xa0"   // ⚠
                                  : "\xe2\x96\xb6"; // ▶

        // Render as a two-segment row: a 1-cell rail in `rail` color,
        // then the body in `fg`-on-`bg`. The rail is painted on the
        // same bg so it reads as a brighter accent stripe inside the
        // band rather than an external chip floating on the panel.
        return component([band, msg, glyph](int w, int /*h*/) -> Element {
            using namespace dsl;
            if (w <= 0) return blank().build();

            // Layout: "▎ ▶  message..........."
            //          ^   ^^^                  trailing bg-painted pad
            //          |   glyph + 2sp
            //          rail (▎, 1 col)
            //
            // Reserved cells: rail(1) + sp(1) + glyph(1) + sp(2) = 5
            const std::string rail_g = "\xe2\x96\x8e";   // ▎ left-7eighths
            const std::string g_str  = glyph;
            const int reserved       = 5;

            std::string body;
            int rail_w = 0;
            int pre_w  = 0;   // width of everything before the message

            if (w >= reserved + 1) {
                // Full layout.
                body  = " " + g_str + "  ";
                rail_w = 1;
                pre_w  = 4;   // sp + glyph + 2sp (rail is rendered separately)
                const int avail = w - reserved;
                body += (string_width(msg) <= avail) ? msg : truncate_end(msg, avail);
            } else if (w >= 4) {
                // Tight: rail + glyph + sp + 1+ char of message.
                body  = " " + g_str + " ";
                rail_w = 1;
                pre_w  = 3;
                body += truncate_end(msg, w - 4);
            } else if (w >= 2) {
                // Very tight: just rail + truncated message.
                rail_w = 1;
                pre_w  = 0;
                body   = truncate_end(msg, w - 1);
            } else {
                // 1 cell — just paint the rail and stop.
                rail_w = 1;
            }

            // Pad body to fill the remaining columns with bg-painted spaces.
            const int used = string_width(body);
            const int total_after_rail = w - rail_w;
            if (used < total_after_rail)
                body.append(static_cast<std::size_t>(total_after_rail - used), ' ');

            // Two elements composed horizontally: the rail and the body,
            // both on the same band style, both BOLD. A toast is an
            // interruption — it should read at a glance without the user
            // hunting for it, and bold is the one emphasis that survives
            // reverse video (where we cannot pick a brighter ink because
            // the terminal owns both colours).
            auto rail_el = (rail_w > 0)
                ? text(rail_g, band.with_bold()).build()
                : blank().build();
            auto body_el = text(std::move(body), band.with_bold()).build();
            (void)pre_w;   // reserved for future right-aligned timestamp slot
            return h(std::move(rail_el), std::move(body_el)).build();
        });
    }

};

} // namespace maya
