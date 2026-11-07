#pragma once
// maya::widget::AgentTimeline — bordered Actions panel for agent tool turns.
//
// CI-pipeline-style log of tool events inside a single bordered card. Each
// event has a tree glyph, status icon, name + detail line + duration, an
// optional indented body under a `│` connector, and (when settled) a footer
// summary. The widget OWNS the chrome — borders, tree glyphs, status icons,
// duration formatting, body stripe, footer. Callers supply pure data:
// per-tool body element, category color, rich event status.
//
//   maya::AgentTimeline{{
//       .title        = " ACTIONS  ·  3/5  ·  Bash ",
//       .border_color = Color::slot(ThemeSlot::Info),
//       .frame        = spinner_frame,
//       .stats        = {{"INSPECT", 3, Color::blue()},
//                        {"MUTATE",  2, Color::magenta()}},
//       .events       = {{.name="Bash", .detail="npm test  ·  exit 0",
//                         .elapsed_seconds=1.2f,
//                         .category_color=Color::slot(ThemeSlot::Success),
//                         .status=AgentEventStatus::Done,
//                         .body=preview_block_element}},
//       .footer       = {{.glyph="\xe2\x9c\x93", .text="done",
//                         .color=Color::slot(ThemeSlot::Success), .summary="3 actions   1.4s"}},
//   }}.build();

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../dsl.hpp"
#include "../element/element.hpp"
#include "../style/border.hpp"
#include "spinner.hpp"   // shared Spinner<Dots> frame table (status_icon / footer)
#include "../style/color.hpp"
#include "../style/style.hpp"
#include "../style/theme.hpp"

#include "tool_body_preview.hpp"

namespace maya {

// Lifecycle status for a single timeline event. Maps directly to the model's
// tool-call discriminator. "Approved" in the source domain folds into Running
// for visual purposes — both are "in flight."
enum class AgentEventStatus : std::uint8_t {
    Pending,    // queued, args still streaming → bright_yellow spinner
    Running,    // executing → bright_cyan spinner
    Done,       // success → bright_green ✓
    Failed,     // exception or non-zero exit → bright_red ✗
    Rejected,   // user denied permission → bright_yellow ⊘
};

struct AgentTimelineEvent {
    std::string             name;                 // Display name e.g. "Bash"
    std::string             detail;               // One-line summary
    float                   elapsed_seconds = 0.0f;
    Color                   category_color  = Color::slot(ThemeSlot::Primary);
    AgentEventStatus        status          = AgentEventStatus::Pending;
    ToolBodyPreview::Config body;                 // empty by default; rendered under `│` stripe

    // Optional SHARED alias of `body` for the terminal-event fast path.
    //
    // A terminal tool's body is immutable, and the host already keeps it
    // behind a shared_ptr<const Config> in its body-config cache. When the
    // host sets this, append_event's terminal-event ComponentElement
    // captures THIS shared_ptr directly instead of deep-copying the whole
    // event (name+detail+body text) into a fresh make_shared every frame.
    // The deep copy was O(body bytes) per settled event per frame — paid
    // EVEN on a renderer cache HIT (the closure is built but the renderer
    // skips invoking it), so a long in-flight run with many tall settled
    // write/edit cards spent tens of ms/frame copying body text that never
    // reached a cell. With body_shared the terminal closure is a ref-bump.
    //
    // When null, the fast path falls back to the historical
    // make_shared<const AgentTimelineEvent>(ev) deep copy (correct, just
    // O(body)). `body` stays the source of truth for the uncached
    // (running/pending) render path.
    std::shared_ptr<const ToolBodyPreview::Config> body_shared;

    // Optional content-stable cache key (Witness Chain) for the
    // rendered event sub-tree (header row + striped body rows).
    //
    // When non-empty AND the event is in a TERMINAL state
    // (Done / Failed / Rejected), AgentTimeline::append_event wraps
    // the per-event rows in a ComponentElement whose hash_id is
    // this CacheId. The renderer's hash-keyed component_cache then
    // stores the rendered cells against this key and serves every
    // subsequent frame as a cell-blit — layout + paint of the tool
    // body is skipped entirely.
    //
    // Why "terminal" only: a Running / Pending tool's card carries
    // a live spinner frame in its status icon, and a Done card with
    // a frame-dependent state somewhere inside would lock a stale
    // spinner glyph into the cells. Terminal events render no
    // frame-driven state (status icon collapses to a static ✓ / ✗ /
    // ⊘), so their cell snapshot is byte-stable until the host
    // mutates the event itself (which it signals by changing the
    // hash_id, typically by mixing the tool's wire id into the
    // CacheIdBuilder).
    //
    // Default empty → every frame walks the event sub-tree, matching
    // the pre-cache behaviour for callers that haven't opted in.
    CacheId                 hash_id;
};

struct AgentTimelineStat {
    std::string label;                        // raw — widget small-caps's it
    int         count = 0;
    Color       color = Color::slot(ThemeSlot::Primary);
};

struct AgentTimelineFooter {
    std::string glyph;
    std::string text;
    Color       color = Color::slot(ThemeSlot::Success);
    std::string summary;
};

class AgentTimeline {
public:
    struct Config {
        std::string                          title;
        // Optional right-aligned segment on the same (top) border edge.
        // Used to pin a duration / status pill to the right side of the
        // panel header while the main title stays left-anchored — a
        // single `title` string alone is start-aligned and can't express
        // that split.
        std::string                          title_end;
        Color                                border_color = Color::slot(ThemeSlot::Muted);
        int                                  frame        = 0;
        std::vector<AgentTimelineStat>       stats;
        std::vector<AgentTimelineEvent>      events;
        std::optional<AgentTimelineFooter>   footer;

        // Optional PANEL-level cache key. When set, the whole built panel
        // (border box + stats + every event header/body + connectors +
        // footer) is wrapped in a single hash-keyed ComponentElement, so
        // the renderer blits the entire panel's cells across frames
        // instead of re-laying-out its rows. The host sets this ONLY for
        // an all-terminal batch whose bytes are immutable (no running
        // spinner / live elapsed in the chrome) — a settled tool panel
        // sitting in a still-streaming in-flight run, where it would
        // otherwise be re-laid-out every frame for the whole turn. Leave
        // empty for a live batch so the spinner/elapsed keep animating.
        CacheId                              hash_id;
    };

    explicit AgentTimeline(Config c) : cfg_(std::move(c)) {}

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;

        std::vector<Element> rows;
        rows.reserve(cfg_.events.size() * 4 + 4);

        // Stats row visibility is gated only on the host having
        // provided stats — NOT on the event count. The previous
        // `events.size() > 1` gate produced a +2-row height bump the
        // instant a second tool joined a panel mid-stream, which on
        // panels straddling the scrollback↔viewport seam stranded the
        // pre-bump top edge in native scrollback (visible as a
        // half-floating "A C T I O N S" header above the live panel).
        // Holding the row from frame 1 onward keeps the panel's
        // intrinsic row count monotone across its lifecycle: rows can
        // only be appended (more events, footer transition), never
        // inserted above already-emitted content. One extra row of
        // chrome on single-tool turns is the cost; visual stability
        // across multi-tool turns is the payoff.
        const bool show_stats = !cfg_.stats.empty();
        if (show_stats) {
            rows.push_back(stats_row());
            rows.push_back(blank());
        }
        for (std::size_t i = 0; i < cfg_.events.size(); ++i) append_event(rows, i);
        if (cfg_.footer) {
            rows.push_back(blank());
            rows.push_back(footer_row(*cfg_.footer));
        }

        // align_self(Stretch) forces the panel to claim the parent's
        // full cross-axis width regardless of how wide its rows
        // naturally measure. Without it, the bordered card hugs its
        // widest row (typically the stats row or the longest event
        // detail) and the right border lands mid-viewport — the
        // panel reads as "not responsive" as the terminal widens.
        // The inner rows already carry grow(1.0f) so they fill the
        // available width once the panel itself is stretched.
        //
        // Uses the runtime BoxBuilder (vstack()) instead of the
        // compile-time `v(...)` node because the DSL pipe layer
        // doesn't surface an align_self setter — the property has
        // to be set on the builder before children are accepted.
        // overflow:Hidden clips per-row content at the card's right
        // border so long file-write / file-read content can never
        // visibly bleed past the bordered rectangle. The row TextElement
        // already declares wrap=TruncateEnd which should fit the text
        // into the layout-allocated width, but with auto-width row
        // containers there's a small flex-shrink window where children
        // are measured before the parent's cross-stretch resolves the
        // row's definite width — long content from a write tool's
        // 100+ row body can land past the right border before the
        // shrink pass converges, especially when the AgentTimeline
        // gets stretched to a width that's narrower than the body
        // rows' natural widths. The canvas-level clip is the
        // structural guard against that escape hatch.
        auto box = vstack()
            .align_self(Align::Stretch)
            .border(BorderStyle::Round)
            .padding(0, 1, 0, 1)
            .border_color(cfg_.border_color)
            .border_text(cfg_.title, BorderTextPos::Top, BorderTextAlign::Start)
            .overflow(Overflow::Hidden);
        if (!cfg_.title_end.empty()) {
            box = std::move(box).border_text_end(
                cfg_.title_end, BorderTextPos::Top, BorderTextAlign::End);
        }
        Element built = std::move(box)(rows);

        // Panel-level cache wrap. When the host marked this batch settled
        // (all-terminal, immutable bytes) it set cfg_.hash_id. Wrap the
        // whole panel in a hash-keyed ComponentElement so the renderer
        // captures its cells once and BLITS them every later frame,
        // instead of re-laying-out the border box + N event header rows +
        // connectors + footer. This is what bounds a long in-flight
        // autopilot turn (many settled panels in the live tail) to
        // O(active panel) per frame instead of O(Σ settled panels).
        //
        // The render closure owns the built panel by shared_ptr<const>:
        // capturing by value would deep-copy the whole sub-tree on every
        // build() even on the cache-miss frame, and the inner per-event
        // ComponentElements already carry their own hash_ids so the nested
        // caches stay warm under this outer wrapper.
        if (!cfg_.hash_id.empty()) {
            ComponentElement comp;
            comp.hash_id = cfg_.hash_id;
            auto panel_sp = std::make_shared<const Element>(std::move(built));
            comp.render = [panel_sp](int, int) -> Element { return *panel_sp; };
            return Element{std::move(comp)};
        }
        return built;
    }

private:
    Config cfg_;

    // ── Stats row: small-caps category badges separated by mid-dots.
    //    Dots stay bright_black (truly ornamental — separators between
    //    badges). Counts use ANSI 7 (mid-gray, legible) so "INSPECT 2 ·
    //    MUTATE 1" doesn't collapse below readability on low-contrast
    //    terminal themes.
    [[nodiscard]] Element stats_row() const {
        using namespace dsl;
        // One small chip per category: `● inspect 3`, dot and label in the
        // category colour, count in text. Same single row as before.
        const Color count_color = Color::slot(ThemeSlot::Text);

        std::vector<Element> parts;
        parts.reserve(cfg_.stats.size() * 4);
        for (std::size_t i = 0; i < cfg_.stats.size(); ++i) {
            const auto& s = cfg_.stats[i];
            if (i > 0) parts.push_back(text("   "));
            parts.push_back(text("\xe2\x97\x8f ", Style{}.with_fg(s.color)));
            parts.push_back(text(lower(s.label), Style{}.with_fg(s.color)));
            parts.push_back(text(" " + std::to_string(s.count),
                                 Style{}.with_fg(count_color).with_bold()));
        }
        return (h(parts) | grow(1.0f)).build();
    }

    static std::string lower(std::string_view s) {
        std::string out{s};
        for (auto& c : out)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        return out;
    }

    // ── Footer row: `   ✓ done   3 actions   1.4s`. Glyph + word carry the
    //    status colour; the summary stays in the text colour.
    [[nodiscard]] static Element footer_row(const AgentTimelineFooter& f) {
        using namespace dsl;
        return h(
            text("   "),
            text(f.glyph + " ", Style{}.with_fg(f.color).with_bold()),
            text(lower(f.text), Style{}.with_fg(f.color).with_bold()),
            text("   "),
            text(f.summary, Style{}.with_fg(Color::slot(ThemeSlot::Text)))
        ).build();
    }

    // ── Per-event block: header row + (optional) body rows + (optional)
    //    inter-event connector. Pushed into `rows` so v() picks them up
    //    in order.
    //
    // Caching: when ev.hash_id is set AND the event is terminal
    // (Done / Failed / Rejected), the (header + body) sub-tree is
    // wrapped in a ComponentElement so the renderer's hash-keyed
    // cache blits cells across frames instead of re-walking
    // ToolBodyPreview's tree. The inter-event connector stays
    // outside the cached wrapper because its color depends on the
    // NEXT event's status — caching it under THIS event's id would
    // freeze the connector to whatever next-status existed at first
    // population, missing later transitions like "the running tool
    // below me just turned green."
    void append_event(std::vector<Element>& rows, std::size_t i) const {
        using namespace dsl;
        const auto& ev       = cfg_.events[i];
        const bool is_last   = (i + 1 == cfg_.events.size());
        const bool is_terminal = (ev.status == AgentEventStatus::Done
                               || ev.status == AgentEventStatus::Failed
                               || ev.status == AgentEventStatus::Rejected);

        if (is_terminal && !ev.hash_id.empty()) {
            ComponentElement comp;
            std::string glyph{tree_glyph(i, cfg_.events.size())};
            // The cache key must cover every byte the cached cells
            // depend on — and the tree glyph is a function of the
            // event's POSITION and the panel's EVENT COUNT, neither of
            // which the host's content-addressed hash_id can know
            // about. Without folding it in, a settled first tool cached
            // as "── Bash" keeps blitting "──" after a second tool
            // joins (its glyph should become "╭─"), and the panel
            // renders an impossible tree ("──" followed by a connector
            // and "╰─"). Deriving the key as (host id × glyph) keeps
            // the freeze handoff a pure hit — live and frozen builds of
            // the same event list produce the same glyph — while a
            // position/count change correctly mints a fresh entry and
            // re-renders with the right corner.
            comp.hash_id = CacheIdBuilder{}
                .add(ev.hash_id.hash())
                .add(std::string_view{glyph})
                .build();
            // FAST PATH: when the host supplied a shared body (terminal
            // tool bodies are immutable and already live behind a
            // shared_ptr in its cache), capture only the small header
            // fields by value + the body by ref-bump. This avoids the
            // O(body) deep copy of the whole event that
            // make_shared<const AgentTimelineEvent>(ev) performs EVERY
            // frame — a cost paid even on a renderer cache HIT (the
            // closure is constructed but never invoked), which on a long
            // in-flight run with many tall settled cards dominated the
            // frame (tens of ms/frame of pure std::string copies).
            if (ev.body_shared) {
                comp.render = [name = ev.name, detail = ev.detail,
                               cc = ev.category_color, status = ev.status,
                               body = ev.body_shared,
                               glyph_copy = std::move(glyph)]
                              (int /*w*/, int /*h*/) -> Element {
                    AgentTimelineEvent e;
                    e.name = name; e.detail = detail;
                    e.category_color = cc; e.status = status;
                    e.body = *body;   // materialized ONLY on a cache miss
                    std::vector<Element> sub;
                    sub.reserve(4);
                    build_event_body(sub, e, /*is_terminal=*/true,
                                     /*frame=*/0, glyph_copy);
                    return v(std::move(sub)).build();
                };
                rows.push_back(Element{std::move(comp)});
                if (!is_last) {
                    const Color next_cc =
                        event_connector_color(cfg_.events[i + 1].status);
                    rows.push_back(h(
                        text("   "),
                        text("\xe2\x94\x82", Style{}.with_fg(next_cc).with_dim())
                    ).build());
                }
                return;
            }
            // Capture the event by shared_ptr<const> so the lambda owns
            // a stable view of its inputs without per-frame deep-copy
            // of body text. Prior by-value capture cost
            // sizeof(AgentTimelineEvent) (strings + ToolBodyPreview body
            // text) on every AgentTimeline::build() call — even when
            // the cache fast-path won't invoke the lambda. For a
            // settled write/edit body with hundreds of lines that's
            // hundreds of KB of std::string copies per frame per event,
            // which made the cached path measurably SLOWER than the
            // uncached path on small bodies (see test_render_scaling
            // test_agent_timeline_per_event_hash_id_bounds_cost).
            // shared_ptr capture is one ref-bump per frame, fixed cost.
            auto ev_sp = std::make_shared<const AgentTimelineEvent>(ev);
            comp.render = [ev_sp = std::move(ev_sp),
                           glyph_copy = std::move(glyph)]
                          (int /*w*/, int /*h*/) -> Element {
                std::vector<Element> sub;
                sub.reserve(4);
                build_event_body(sub, *ev_sp, /*is_terminal=*/true,
                                 /*frame=*/0, glyph_copy);
                return v(std::move(sub)).build();
            };
            rows.push_back(Element{std::move(comp)});
        } else {
            build_event_body(rows, ev, is_terminal, cfg_.frame,
                             std::string{tree_glyph(i, cfg_.events.size())});
        }

        if (!is_last) {
            const Color next_cc = event_connector_color(cfg_.events[i + 1].status);
            rows.push_back(h(
                text("   "),
                text("\xe2\x94\x82", Style{}.with_fg(next_cc).with_dim())
            ).build());
        }
    }

    // Build the header row + body rows for a single event into `out`.
    // Pure function of its inputs — caller passes the live `frame` for
    // active spinner glyphs and the precomputed `glyph` so the cached
    // path can capture a position at lambda construction time.
    static void build_event_body(std::vector<Element>& out,
                                 const AgentTimelineEvent& ev,
                                 bool is_terminal,
                                 int frame,
                                 const std::string& glyph) {
        using namespace dsl;
        const bool is_active = (ev.status == AgentEventStatus::Pending
                             || ev.status == AgentEventStatus::Running);

        // `detail` is clipped (TruncateEnd) — when a tool's args don't fit
        // (long `powershell -c "…"` commands, paths past the right edge),
        // default Wrap would let the text bleed into N visual rows while
        // the parent h() still measures the row as 1 line, so the next
        // event's body / connector renders on top of the wrapped overflow.
        // The body preview below is the place for full content; the
        // header stays a 1-line summary with an ellipsis at the cut.
        //
        // The header is one two-tone pill: the tool name on a solid chip,
        // then the detail on a soft band of the same hue running to the
        // card's edge. Without measurable colours (theme::native) the band
        // can't be mixed, so the detail stays plain text as before.
        const auto band = band_bg(ev.status, ev.category_color);
        const Style dst = band
            ? Style{}.with_fg(Color::slot(ThemeSlot::Text)).with_italic()
            : detail_style(ev.category_color, is_active);
        Element detail_el = is_active && frame > 0
            ? shimmer_text(ev.detail, dst, frame)
            : detail_text(ev.detail, dst);
        Element detail_cell = band
            ? (h(text("  "), std::move(detail_el), text(" ")) | bgc(*band)
                   | grow(1.0f) | shrink(1.0f)).build()
            : (h(text("  "), h(std::move(detail_el)) | shrink(1.0f))
                   | shrink(1.0f)).build();
        out.push_back((h(
            text(glyph, tree_style(ev.category_color, is_active)),
            text(" "),
            status_icon(ev.status, frame),
            text("  "),
            text(" " + ev.name + " ", chip_style(ev.status, ev.category_color))
                | nowrap | shrink(0.0f),
            std::move(detail_cell)
            // NOTE: no per-event elapsed cell here. The duration of a
            // tool lives in the card's title-end (running tool / total)
            // and the DONE footer. A per-row right-aligned elapsed used
            // to render here `when(is_terminal)`, but it only appeared on
            // the LIVE (uncached) render: the FROZEN render wraps each
            // event in a hash-cached ComponentElement laid out in an
            // isolated sub-tree, where the grow-spacer + right-aligned
            // nowrap cell resolves differently and the elapsed dropped
            // out. That made the same settled tool row render WITH the
            // elapsed live and WITHOUT it once frozen — a committed-row
            // rewrite at the freeze instant, i.e. the scrollback
            // duplication seam. Removing the per-row cell makes live and
            // frozen byte-identical (and matches agent_session, which
            // carries duration only in the footer).
        ) | grow(1.0f)).build());

        // Build the body via ToolBodyPreview, then stripe each child row
        // with the `│` connector. The body widget owns its own line layout
        // (vector<row> in a vstack); we walk its children to give each row
        // its own stripe, keeping every rendered row at exactly 1 line of
        // measured height (inline mode prefers that).
        const Color cc        = event_connector_color(ev.status);
        const Style stripe_st = is_active ? Style{}.with_fg(cc)
                                          : Style{}.with_fg(cc).with_dim();
        Element body_rule = h(
            text("   "),
            text("\xe2\x94\x82  ", stripe_st)
        ).build();

        // Body rows sit on a panel a shade off the canvas, faintly tinted
        // with the row's hue. Diff bands carry their own bg and win over it.
        const Color panel = panel_bg(ev.status, ev.category_color);
        Element body_el = ToolBodyPreview{ev.body}.build();
        if (auto* bx = as_box(body_el)) {
            for (const auto& child : bx->children)
                out.push_back((h(body_rule,
                                 h(child) | bgc(panel) | grow(1.0f))
                               | grow(1.0f)).build());
        } else if (auto* t = as_text(body_el); t && !t->content.empty()) {
            out.push_back((h(body_rule, h(body_el) | bgc(panel) | grow(1.0f))
                           | grow(1.0f)).build());
        }
    }

    // ── Style helpers ──────────────────────────────────────────────────

    static Style tree_style(Color cat, bool is_active) {
        return is_active ? Style{}.with_fg(cat)
                         : Style{}.with_fg(cat).with_dim();
    }

    static Style name_style(AgentEventStatus s, Color cat, bool is_active) {
        if (s == AgentEventStatus::Failed)
            return Style{}.with_fg(Color::slot(ThemeSlot::Error)).with_bold();
        if (s == AgentEventStatus::Rejected)
            return Style{}.with_fg(Color::slot(ThemeSlot::Warning)).with_bold();
        return is_active ? Style{}.with_fg(cat).with_bold()
                         : Style{}.with_fg(cat).with_dim();
    }

    // The body panel: a shade off the canvas (6% toward the text colour, so
    // dark themes lift and light themes dip), then 5% toward the row's hue
    // so the body is a faint sibling of the header band. Under theme::native
    // the colours can't be measured, so no panel is painted there.
    static Color panel_bg(AgentEventStatus s, Color cat) {
        const Theme& th = theme::live();
        const auto bg = th.resolve(Color::slot(ThemeSlot::Background));
        const auto fg = th.resolve(Color::slot(ThemeSlot::Text));
        if (!bg.has_channels() || !fg.has_channels())
            return Color::default_color();
        auto step = [](int a, int b, int pct) {
            return a + (b - a) * pct / 100;
        };
        int r = step(bg.r(), fg.r(), 6), g = step(bg.g(), fg.g(), 6),
            b = step(bg.b(), fg.b(), 6);
        if (const auto hc = th.resolve(chip_hue(s, cat)); hc.has_channels()) {
            r = step(r, hc.r(), 5); g = step(g, hc.g(), 5); b = step(b, hc.b(), 5);
        }
        return Color::rgb(static_cast<uint8_t>(r), static_cast<uint8_t>(g),
                          static_cast<uint8_t>(b));
    }

    // The hue a row is drawn in: its category, red when failed, yellow when
    // the user rejected it.
    static Color chip_hue(AgentEventStatus s, Color cat) {
        Color hue = cat;
        if (s == AgentEventStatus::Failed)   hue = Color::slot(ThemeSlot::Error);
        if (s == AgentEventStatus::Rejected) hue = Color::slot(ThemeSlot::Warning);
        return hue;
    }

    // The soft band behind a header's detail: the canvas mixed 22% toward
    // the row's hue, so it reads as the chip's lighter (or, on a light
    // theme, paler) sibling. nullopt when either colour can't be measured.
    static std::optional<Color> band_bg(AgentEventStatus s, Color cat) {
        const Theme& th = theme::live();
        const auto hc = th.resolve(chip_hue(s, cat));
        const auto bg = th.resolve(Color::slot(ThemeSlot::Background));
        if (!hc.has_channels() || !bg.has_channels()) return std::nullopt;
        auto mix = [](int a, int b) {
            return static_cast<uint8_t>(a + (b - a) * 22 / 100);
        };
        return Color::rgb(mix(bg.r(), hc.r()), mix(bg.g(), hc.g()), mix(bg.b(), hc.b()));
    }

    // The tool name as a chip: bold ink on a solid fill of the row's hue.
    // A named scheme gets an explicit fill with the theme's inverse text; a
    // palette colour (theme::native) can't be measured, so let the terminal
    // swap its own fg/bg with SGR 7 instead.
    static Style chip_style(AgentEventStatus s, Color cat) {
        const Color hue = chip_hue(s, cat);
        if (theme::live().resolve(hue).has_channels())
            return Style{}.with_bg(hue)
                          .with_fg(Color::slot(ThemeSlot::InverseText))
                          .with_bold();
        return Style{}.with_fg(hue).with_inverse().with_bold();
    }

    static Style detail_style(Color cat, bool /*is_active*/) {
        // Tool detail (file path / args / command) renders in the
        // tool's category color + italic. The whole header row
        // ("✓ Name detail elapsed") now flows in the category hue —
        // strong visual identity per tool.
        return Style{}.with_fg(cat).with_italic();
    }

    // ── Pure helpers (formatting / glyphs / colors) ────────────────────

    // In-flight detail text with a soft highlight sweeping left to right,
    // so a running tool reads as alive. Driven by the spinner frame: where
    // the spinner is frozen (frame 0, committed rows) this never runs.
    static Element shimmer_text(const std::string& d, Style st, int frame) {
        // Code-point start offsets, so a run never splits a UTF-8 sequence.
        std::vector<std::size_t> cps;
        for (std::size_t i = 0; i < d.size(); ++i)
            if ((static_cast<unsigned char>(d[i]) & 0xC0) != 0x80) cps.push_back(i);
        const int n = static_cast<int>(cps.size());
        if (n == 0)
            return Element{TextElement{.content = d, .style = st,
                                       .wrap = TextWrap::TruncateEnd}};
        constexpr int kWidth = 8, kGap = 24;
        const int period = n + kWidth + kGap;
        const int head = (frame * 2) % period - kWidth;
        auto at = [&](int k) { return k >= n ? d.size() : cps[std::size_t(k)]; };
        const Style mid  = Style{}.with_fg(Color::slot(ThemeSlot::Text)).with_italic();
        const Style peak = mid.with_bold();
        std::vector<StyledRun> runs;
        auto push = [&](int a, int b, Style s) {
            a = std::clamp(a, 0, n); b = std::clamp(b, 0, n);
            if (b > a) runs.push_back({at(a), at(b) - at(a), s});
        };
        push(0, head, st);
        push(head, head + 2, mid);
        push(head + 2, head + kWidth - 2, peak);
        push(head + kWidth - 2, head + kWidth, mid);
        push(head + kWidth, n, st);
        return Element{TextElement{.content = d, .style = st,
                                   .wrap = TextWrap::TruncateEnd,
                                   .runs = std::move(runs)}};
    }

    // The header detail, with a trailing "+A −R" change stat painted
    // green / red instead of the detail colour.
    static Element detail_text(const std::string& d, Style st) {
        const auto p = d.rfind("  \xc2\xb7  +");
        if (p == std::string::npos)
            return Element{TextElement{.content = d, .style = st,
                                       .wrap = TextWrap::TruncateEnd}};
        const std::size_t plus = p + 6;            // after "  ·  "
        auto m = d.find(" \xe2\x88\x92", plus);
        if (m == std::string::npos) m = d.size();
        // Only a stat if the run after "+" is digits up to the minus/end.
        for (std::size_t k = plus + 1; k < m; ++k)
            if (d[k] < '0' || d[k] > '9')
                return Element{TextElement{.content = d, .style = st,
                                           .wrap = TextWrap::TruncateEnd}};
        std::vector<StyledRun> runs{
            {0, plus, st},
            {plus, m - plus, Style{}.with_fg(Color::slot(ThemeSlot::Success)).with_bold()},
        };
        if (m < d.size())
            runs.push_back({m, d.size() - m,
                            Style{}.with_fg(Color::slot(ThemeSlot::Error)).with_bold()});
        return Element{TextElement{.content = d, .style = st,
                                   .wrap = TextWrap::TruncateEnd,
                                   .runs = std::move(runs)}};
    }

    static dsl::RuntimeTextNode<std::string_view>
    status_icon(AgentEventStatus s, int frame) {
        // Same braille glyphs as Spinner<Dots> — index the shared frame
        // table rather than a second hand-copied array, so every animated
        // surface (Spinner, status-bar chips, this) stays in lockstep off
        // the one clock the host advances.
        constexpr auto sp = detail::get_spinner_frames<SpinnerStyle::Dots>();
        const auto spin = [&](int f) {
            return sp.frames[((f % sp.count) + sp.count) % sp.count];
        };
        // frame < 0 = seam-frozen live panel: the host has determined the
        // panel's TOP rows (title + first event headers) may already sit in
        // native scrollback, where any byte change is a committed-row
        // rewrite (gate → HardReset/soft-repaint → stranded chrome). An
        // animated braille glyph on such a row would tick every frame, so
        // the host passes -1 and we render ONE static in-flight glyph —
        // ● bright_cyan — for BOTH Pending and Running. Not just static
        // per status: the Pending→Running transition itself must be a
        // byte+style no-op, because a queued tool's header can commit
        // while an earlier tool's output grows the panel, and the later
        // kick to Running would rewrite that committed row (oracle
        // write/edit turn, t*-e-run at 60x18). Liveness is carried by the
        // panel FOOTER (a bottom append — never above the seam while the
        // panel is live), which the host animates. frame >= 0 keeps the
        // classic spinner + distinct pending hue for callers whose panels
        // never straddle the seam.
        switch (s) {
            case AgentEventStatus::Running:
                if (frame < 0)
                    return dsl::text(std::string_view{"\xe2\x97\x8f"},   // ●
                                     Style{}.with_fg(Color::slot(ThemeSlot::Link)).with_bold());
                return dsl::text(std::string_view{spin(frame)},
                                 Style{}.with_fg(Color::slot(ThemeSlot::Link)).with_bold());
            case AgentEventStatus::Pending:
                if (frame < 0)
                    return dsl::text(std::string_view{"\xe2\x97\x8f"},   // ● (same as Running)
                                     Style{}.with_fg(Color::slot(ThemeSlot::Link)).with_bold());
                return dsl::text(std::string_view{spin(frame)},
                                 Style{}.with_fg(Color::slot(ThemeSlot::Warning)).with_bold());
            case AgentEventStatus::Done:
                return dsl::text(std::string_view{"\xe2\x9c\x93"},
                                 Style{}.with_fg(Color::slot(ThemeSlot::Success)).with_bold());
            case AgentEventStatus::Failed:
                return dsl::text(std::string_view{"\xe2\x9c\x97"},
                                 Style{}.with_fg(Color::slot(ThemeSlot::Error)).with_bold());
            case AgentEventStatus::Rejected:
                return dsl::text(std::string_view{"\xe2\x8a\x98"},
                                 Style{}.with_fg(Color::slot(ThemeSlot::Warning)).with_bold());
        }
        return dsl::text(std::string_view{"\xe2\x97\x8b"},
                         Style{}.with_fg(Color::slot(ThemeSlot::Muted)));
    }

    static std::string_view tree_glyph(std::size_t idx, std::size_t total) {
        if (total == 1)        return "\xe2\x94\x80\xe2\x94\x80";  // ──
        if (idx == 0)          return "\xe2\x95\xad\xe2\x94\x80";  // ╭─
        if (idx + 1 == total)  return "\xe2\x95\xb0\xe2\x94\x80";  // ╰─
        return                        "\xe2\x94\x9c\xe2\x94\x80";  // ├─
    }

    static std::string format_duration(float secs) {
        // The elapsed glyphs render right-aligned (after a grow spacer),
        // nowrap, clipped at the card border. While a tool is running
        // this re-renders every spinner tick; an unpadded result lets
        // the token's width change between ticks ("9.9s"->"10.0s",
        // "999ms"->"1.0s"), which slides the whole right-aligned cell
        // one column sideways per tick -- a visible horizontal jitter
        // on terminals without synchronized output. Left-pad to a fixed
        // width so the right edge stays pinned and the digits never
        // shift; the pad is leading spaces in the gap before the value,
        // so the rendered number/unit looks identical.
        char buf[24];
        if      (secs < 1.0f)
            std::snprintf(buf, sizeof(buf), "%.0fms", static_cast<double>(secs) * 1000.0);
        else if (secs < 60.0f)
            std::snprintf(buf, sizeof(buf), "%.1fs", static_cast<double>(secs));
        else {
            int mins   = static_cast<int>(secs) / 60;
            float rest = secs - static_cast<float>(mins * 60);
            std::snprintf(buf, sizeof(buf), "%dm%.0fs",
                          mins, static_cast<double>(rest));
        }
        std::string out{buf};
        constexpr std::size_t kWidth = 6;   // "999ms", "59.9s", "1m05s"
        if (out.size() < kWidth)
            out.insert(out.begin(), kWidth - out.size(), ' ');
        return out;
    }

    static Color duration_color(float secs) {
        if (secs < 0.25f) return Color::slot(ThemeSlot::Success);
        if (secs < 2.0f)  return Color::slot(ThemeSlot::Muted);
        if (secs < 15.0f) return Color::slot(ThemeSlot::Warning);
        return Color::slot(ThemeSlot::Error);
    }

    static Color event_connector_color(AgentEventStatus s) {
        switch (s) {
            case AgentEventStatus::Failed:   return Color::slot(ThemeSlot::Error);
            case AgentEventStatus::Rejected: return Color::slot(ThemeSlot::Warning);
            case AgentEventStatus::Running:  return Color::slot(ThemeSlot::Primary);
            case AgentEventStatus::Pending:
            case AgentEventStatus::Done:     return Color::slot(ThemeSlot::Muted);
        }
        return Color::slot(ThemeSlot::Muted);
    }

    static std::string small_caps(std::string_view s) {
        std::string out;
        out.reserve(s.size() * 2);
        for (std::size_t i = 0; i < s.size(); ++i) {
            char c = s[i];
            out.push_back(static_cast<char>(
                (c >= 'a' && c <= 'z') ? (c - 32) : c));
            // Letter-space only at UTF-8 sequence boundaries: inserting
            // after every BYTE splits multi-byte sequences ("R \xc3 \xa9
            // …" mojibake for any non-ASCII label). The next byte is a
            // continuation iff (b & 0xC0) == 0x80 — skip the space then.
            if (i + 1 < s.size()
                && (static_cast<unsigned char>(s[i + 1]) & 0xC0) != 0x80)
                out.push_back(' ');
        }
        return out;
    }
};

} // namespace maya
