// examples/agent_stats.cpp — a tabbed, animated "AI agent stats" dashboard laid out
// with viewport().
//
// The star of the show is maya::viewport(): a purely-layout responsive grid
// that fans its cards into 1/2/3/… columns as the terminal widens, keeps
// every card capped at max_width, and stays fully responsive inside each
// column. Here it lays out cards built from maya's real visual widgets —
// Sparkline, LineChart, Heatmap, Gauge, BarChart, ProgressRing,
// ContextGauge, ModelBadge, TokenStreamSparkline, Table — across eight tabs,
// each covering a different family of stats an AI agent produces.
//
// It ANIMATES: a 120 ms tick advances rolling history buffers and a global
// phase, so every sparkline, line chart and meter is alive.
//
//   Tabs:   Tab / Shift-Tab (or ←/→, or 1..8) switch tab
//   Scroll: ↑/↓ · j/k · PgUp/PgDn · wheel
//   Layout: f toggle row/column flow · [-]/[+] column ceiling
//   Quit:   q

#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include <maya/host/run.hpp>
#include <maya/maya.hpp>
#include <maya/element/grid.hpp>
#include <maya/widget/agentty/bar_chart.hpp>
#include <maya/widget/agentty/context_gauge.hpp>
#include <maya/widget/agentty/donut.hpp>
#include <maya/widget/empty_state.hpp>
#include <maya/widget/gauge.hpp>
#include <maya/widget/heatmap.hpp>
#include <maya/widget/agentty/histogram.hpp>
#include <maya/widget/agentty/line_chart.hpp>
#include <maya/widget/agentty/model_badge.hpp>
#include <maya/widget/progress_ring.hpp>
#include <maya/widget/agentty/scrollbar.hpp>
#include <maya/widget/agentty/sparkline.hpp>
#include <maya/widget/agentty/tab_strip.hpp>
#include <maya/widget/table.hpp>
#include <maya/widget/agentty/token_stream_sparkline.hpp>

using namespace maya;
using namespace maya::dsl;

namespace {

// ── palette ──────────────────────────────────────────────────────────────
namespace hue {
const Color cyan   = Color::rgb(120, 220, 232);
const Color green  = Color::rgb(150, 230, 160);
const Color amber  = Color::rgb(245, 200, 110);
const Color red    = Color::rgb(255, 130, 130);
const Color violet = Color::rgb(190, 160, 255);
const Color blue   = Color::rgb(120, 180, 255);
const Color pink   = Color::rgb(240, 150, 200);
const Color teal   = Color::rgb(120, 220, 200);
const Color muted  = Color::rgb(140, 150, 170);
}  // namespace hue

// ── a rolling ring of floats, for the live charts ─────────────────────────
struct Roll {
    std::deque<float> v;
    std::size_t cap = 48;
    void push(float x) { v.push_back(x); while (v.size() > cap) v.pop_front(); }
    std::vector<float> vec() const { return {v.begin(), v.end()}; }
    float back() const { return v.empty() ? 0.f : v.back(); }
    float avg() const {
        if (v.empty()) return 0.f;
        float s = 0; for (float x : v) s += x; return s / static_cast<float>(v.size());
    }
    float max() const {
        float m = 0; for (float x : v) m = std::max(m, x); return m;
    }
};

// A cheap deterministic wobble so the demo animates without any RNG plumbing.
float wobble(float t, float base, float amp, float freq, float phase = 0.f) {
    float s = std::sin(t * freq + phase) * 0.6f + std::sin(t * freq * 2.3f + phase) * 0.4f;
    return std::max(0.f, base + amp * s);
}

// ── the animated model ─────────────────────────────────────────────────────
struct Stats {
    float t = 0.f;             // seconds-ish clock
    long  turns = 0;

    // token throughput
    Roll tok_rate;             // tokens/sec
    long tok_in = 0, tok_out = 0;
    Roll ctx_used;             // context window fill, absolute tokens

    // latency (ms)
    Roll ttft;                 // time to first token
    Roll gen_ms;               // generation time
    Roll tool_ms;              // tool round-trip

    // tools
    std::array<float, 6> tool_calls{{0,0,0,0,0,0}};

    // cost (USD)
    Roll cost_rate;            // $/min
    float cost_total = 0.f;

    // cache
    float cache_hits = 0, cache_miss = 0;

    // errors
    std::array<float, 5> err_kinds{{0,0,0,0,0}};

    // per-model share
    std::array<float, 4> model_share{{0.55f, 0.25f, 0.12f, 0.08f}};

    // heatmap: 7 rows (models) × 12 cols (recent windows) of activity
    std::vector<std::vector<float>> activity;

    void init() {
        for (int i = 0; i < 48; ++i) tick(0.12f);   // warm the buffers
        activity.assign(5, std::vector<float>(12, 0.f));
    }

    void tick(float dt) {
        t += dt;
        ++turns;
        tok_rate.push(wobble(t, 42, 30, 1.7f));
        tok_in  += static_cast<long>(wobble(t, 120, 80, 0.9f));
        tok_out += static_cast<long>(tok_rate.back() * dt * 10.f);
        ctx_used.push(wobble(t, 90000, 60000, 0.3f));

        ttft.push(wobble(t, 380, 180, 2.1f, 1.f));
        gen_ms.push(wobble(t, 1400, 700, 1.1f, 2.f));
        tool_ms.push(wobble(t, 220, 160, 2.7f, 0.5f));

        for (std::size_t i = 0; i < tool_calls.size(); ++i)
            tool_calls[i] = wobble(t, 30 + 8.f * i, 20, 0.7f + 0.2f * i, i);

        float cr = wobble(t, 0.8f, 0.5f, 0.6f);
        cost_rate.push(cr);
        cost_total += cr * dt / 60.f * 40.f;

        cache_hits = wobble(t, 72, 12, 0.4f);
        cache_miss = 100 - cache_hits;

        for (std::size_t i = 0; i < err_kinds.size(); ++i)
            err_kinds[i] = wobble(t, 2 + i, 3, 0.5f + 0.3f * i, i * 2.f);

        if (!activity.empty()) {
            // shift heatmap left, append a fresh column
            for (auto& row : activity) {
                row.erase(row.begin());
                row.push_back(0.f);
            }
            for (std::size_t r = 0; r < activity.size(); ++r)
                activity[r].back() =
                    std::clamp(wobble(t, 0.5f, 0.5f, 0.8f + 0.3f * r, r * 1.3f), 0.f, 1.f);
        }
    }
};

// ── small card helpers ─────────────────────────────────────────────────────

// A titled section — NO border box, just the accent title over the content.
// viewport() spaces the sections into columns; the title carries the colour.
Element card(std::string title, Color accent, std::vector<Element> body) {
    auto b = vstack();
    b.gap(0);
    b.padding(0, 1);
    std::vector<Element> kids;
    kids.push_back(text(std::move(title), Style{}.with_fg(accent).with_bold()));
    kids.push_back(blank());
    for (auto& e : body) kids.push_back(std::move(e));
    return b(std::move(kids));
}

// A big number with a caption, for the "stat tiles".
Element stat(std::string value, std::string caption, Color accent) {
    return v(
        text(std::move(value), Style{}.with_fg(accent).with_bold()),
        text(std::move(caption), Style{}.with_dim())
    ).build();
}

std::string fmt_int(long n) {
    std::string s = std::to_string(n), out;
    int c = 0;
    for (auto it = s.rbegin(); it != s.rend(); ++it) {
        if (c && c % 3 == 0) out.push_back(',');
        out.push_back(*it); ++c;
    }
    return {out.rbegin(), out.rend()};
}
std::string fmt1(float x) { char b[32]; std::snprintf(b, sizeof b, "%.1f", x); return b; }
std::string fmt2(float x) { char b[32]; std::snprintf(b, sizeof b, "%.2f", x); return b; }

Element spark(const Roll& r, Color c, std::string label) {
    Sparkline s(r.vec(), SparklineConfig{.color = c, .show_last = true});
    s.set_label(std::move(label));
    return s.build();
}

Element line(const Roll& r, Color c, int h = 6) {
    LineChart lc(r.vec(), h);
    lc.set_color(c);
    return lc.build();
}

// ── tabs ────────────────────────────────────────────────────────────────
enum Tab { Overview, Tokens, Latency, Tools, Models, Cost, Errors, Cache, kTabCount };
const char* kTabNames[kTabCount] =
    {"Overview", "Tokens", "Latency", "Tools", "Models", "Cost", "Errors", "Cache"};
const Color kTabHue[kTabCount] =
    {hue::cyan, hue::blue, hue::amber, hue::green, hue::violet, hue::teal, hue::red, hue::pink};

// Build the cards for a tab. Returns a vector<Element> for viewport().
std::vector<Element> tab_cards(int tab, const Stats& s) {
    std::vector<Element> cards;
    switch (tab) {

    case Overview: {
        cards.push_back(card("Session", hue::cyan, {
            stat(fmt_int(s.turns) + " turns", "since start", hue::cyan),
            blank(),
            stat(fmt_int(s.tok_in + s.tok_out), "total tokens", hue::blue),
        }));
        cards.push_back(card("Throughput", hue::blue, {
            spark(s.tok_rate, hue::blue, "tok/s"),
            blank(),
            stat(fmt1(s.tok_rate.back()) + " t/s", "current rate", hue::blue),
        }));
        cards.push_back(card("Context window", hue::violet, {
            ContextGauge({.used = static_cast<int>(s.ctx_used.back()),
                          .max = 200000, .cells = 10, .show_tokens = false}).build(),
            blank(),
            stat(fmt1(s.ctx_used.back() / 1000.f) + "k / 200k", "context used", hue::violet),
            blank(),
            spark(s.ctx_used, hue::violet, "fill"),
        }));
        cards.push_back(card("Cost", hue::teal, {
            stat("$" + fmt2(s.cost_total), "session total", hue::teal),
            blank(),
            stat("$" + fmt2(s.cost_rate.back()) + "/min", "burn rate", hue::teal),
        }));
        cards.push_back(card("Latency (gen)", hue::amber, {
            line(s.gen_ms, hue::amber, 5),
        }));
        cards.push_back(card("Cache hit rate", hue::green, {
            Gauge(s.cache_hits / 100.f, "hits").build(),
            blank(),
            stat(fmt1(s.cache_hits) + "%", "of lookups", hue::green),
        }));
        break;
    }

    case Tokens: {
        cards.push_back(card("Token rate", hue::blue, {
            TokenStreamSparkline({.rate = s.tok_rate.back(),
                                  .total = static_cast<int>(s.tok_out),
                                  .history = s.tok_rate.vec(),
                                  .color = hue::blue, .live = true}).build(),
            blank(),
            line(s.tok_rate, hue::blue, 6),
        }));
        cards.push_back(card("Input vs output", hue::cyan, {
            BarChart({{"input",  static_cast<float>(s.tok_in)},
                      {"output", static_cast<float>(s.tok_out)}},
                     static_cast<float>(std::max(s.tok_in, s.tok_out))).build(),
            blank(),
            stat(fmt_int(s.tok_in), "prompt tokens", hue::cyan),
            stat(fmt_int(s.tok_out), "completion tokens", hue::blue),
        }));
        cards.push_back(card("Context fill", hue::violet, {
            line(s.ctx_used, hue::violet, 6),
            blank(),
            ContextGauge({.used = static_cast<int>(s.ctx_used.back()),
                          .max = 200000, .cells = 10, .show_tokens = false}).build(),
            blank(),
            stat(fmt1(s.ctx_used.back() / 1000.f) + "k / 200k", "context used", hue::violet),
        }));
        cards.push_back(card("Peak throughput", hue::green, {
            stat(fmt1(s.tok_rate.max()) + " t/s", "session peak", hue::green),
            blank(),
            stat(fmt1(s.tok_rate.avg()) + " t/s", "rolling average", hue::muted),
        }));
        break;
    }

    case Latency: {
        cards.push_back(card("Time to first token", hue::amber, {
            line(s.ttft, hue::amber, 6),
            blank(),
            stat(fmt_int(static_cast<long>(s.ttft.back())) + " ms", "current TTFT", hue::amber),
        }));
        cards.push_back(card("Generation time", hue::blue, {
            line(s.gen_ms, hue::blue, 6),
            blank(),
            stat(fmt_int(static_cast<long>(s.gen_ms.back())) + " ms", "last turn", hue::blue),
        }));
        cards.push_back(card("Tool round-trip", hue::green, {
            spark(s.tool_ms, hue::green, "ms"),
            blank(),
            stat(fmt_int(static_cast<long>(s.tool_ms.avg())) + " ms", "avg round-trip", hue::green),
        }));
        cards.push_back(card("Latency budget", hue::red, {
            Gauge(std::clamp(s.gen_ms.back() / 3000.f, 0.f, 1.f), "gen").build(),
            blank(),
            Gauge(std::clamp(s.ttft.back() / 800.f, 0.f, 1.f), "ttft").build(),
        }));
        cards.push_back(card("Gen-time distribution", hue::violet, {
            Histogram{}
                .bucket("<1s",  wobble(s.t, 6, 4, 0.5f, 0))
                .bucket("1-2s", wobble(s.t, 14, 6, 0.5f, 1))
                .bucket("2-3s", wobble(s.t, 9, 5, 0.5f, 2))
                .bucket("3-4s", wobble(s.t, 4, 3, 0.5f, 3))
                .bucket(">4s",  wobble(s.t, 2, 2, 0.5f, 4))
                .rows(6).caption("turns per bucket")
                .bar_color(hue::violet).label_color(hue::muted).build(),
        }));
        break;
    }

    case Tools: {
        const char* names[6] = {"read", "edit", "shell", "grep", "search", "write"};
        std::vector<Bar> bars;
        float mx = 1;
        for (int i = 0; i < 6; ++i) mx = std::max(mx, s.tool_calls[i]);
        for (int i = 0; i < 6; ++i)
            bars.push_back({names[i], s.tool_calls[i], kTabHue[(i) % kTabCount]});
        cards.push_back(card("Calls by tool", hue::green, {
            BarChart(bars, mx).build(),
        }));

        Table tbl({{"tool", 8, ColumnAlign::Left, kKeepAlways},
                   {"calls", 6, ColumnAlign::Right, kKeepAlways},
                   {"ms", 5, ColumnAlign::Right, kKeepAlways}},
                  TableConfig{.stripe_rows = true, .show_header = true});
        for (int i = 0; i < 6; ++i)
            tbl.add_row({names[i], fmt_int(static_cast<long>(s.tool_calls[i])),
                         fmt_int(static_cast<long>(wobble(s.t, 120 + 30.f * i, 40, 1.f, i)))});
        cards.push_back(card("Tool ledger", hue::cyan, { tbl.build() }));

        cards.push_back(card("Success rate", hue::blue, {
            ProgressRing{}.value(0.94f).label("94% ok").build(),
            blank(),
            stat("6", "tools available", hue::muted),
        }));
        break;
    }

    case Models: {
        const char* mnames[4] = {"Opus 4.5", "Sonnet 4.5", "Haiku 4", "Gemini"};
        const Color mhue[4]   = {hue::violet, hue::blue, hue::green, hue::amber};
        std::vector<Element> badges;
        for (int i = 0; i < 4; ++i)
            badges.push_back(ModelBadge({.label = mnames[i], .color = mhue[i]}).build());
        cards.push_back(card("Active models", hue::violet, std::move(badges)));

        std::vector<Bar> share;
        for (int i = 0; i < 4; ++i)
            share.push_back({mnames[i], s.model_share[i], mhue[i]});
        cards.push_back(card("Traffic share", hue::blue, {
            BarChart(share, 1.0f).build(),
            blank(),
            Donut{}
                .segment(mnames[0], s.model_share[0], mhue[0])
                .segment(mnames[1], s.model_share[1], mhue[1])
                .segment(mnames[2], s.model_share[2], mhue[2])
                .segment(mnames[3], s.model_share[3], mhue[3])
                .center("4").caption("by request").rows(6)
                .label_color(hue::muted).build(),
        }));

        Heatmap hm(s.activity);
        hm.set_y_labels({"Opus", "Sonnet", "Haiku", "Gemini", "local"});
        hm.set_high_color(hue::violet);
        hm.set_low_color(Color::rgb(40, 44, 60));
        cards.push_back(card("Activity heatmap", hue::teal, {
            hm.build(),
            blank(),
            text("recent request windows →", Style{}.with_dim()),
        }));
        break;
    }

    case Cost: {
        cards.push_back(card("Spend rate", hue::teal, {
            line(s.cost_rate, hue::teal, 6),
            blank(),
            stat("$" + fmt2(s.cost_rate.back()) + "/min", "current burn", hue::teal),
        }));
        cards.push_back(card("Session total", hue::green, {
            stat("$" + fmt2(s.cost_total), "spent so far", hue::green),
            blank(),
            stat("$" + fmt2(s.cost_total / std::max(1L, s.turns) * 1000), "per 1k turns", hue::muted),
        }));
        cards.push_back(card("Cost by model", hue::violet, {
            BarChart({{"Opus",   0.62f, hue::violet},
                      {"Sonnet", 0.28f, hue::blue},
                      {"Haiku",  0.10f, hue::green}}, 1.0f).build(),
        }));
        cards.push_back(card("Budget", hue::amber, {
            ContextGauge({.used = static_cast<int>(s.cost_total * 100),
                          .max = 5000, .cells = 10, .show_tokens = false}).build(),
            blank(),
            stat("$" + fmt2(s.cost_total) + " / $50.00", "monthly cap", hue::amber),
        }));
        break;
    }

    case Errors: {
        const char* ek[5] = {"timeout", "rate-limit", "parse", "tool-fail", "refusal"};
        std::vector<Bar> bars;
        float mx = 1;
        for (int i = 0; i < 5; ++i) mx = std::max(mx, s.err_kinds[i]);
        for (int i = 0; i < 5; ++i)
            bars.push_back({ek[i], s.err_kinds[i], hue::red});
        cards.push_back(card("Errors by kind", hue::red, {
            BarChart(bars, mx).build(),
        }));
        float total_err = 0; for (float e : s.err_kinds) total_err += e;
        cards.push_back(card("Error rate", hue::amber, {
            Gauge(std::clamp(total_err / 40.f, 0.f, 1.f), "err").build(),
            blank(),
            stat(fmt1(total_err) + "/min", "current errors", hue::red),
        }));
        cards.push_back(card("Retries", hue::blue, {
            ProgressRing{}.value(0.12f).label("12% retried").build(),
            blank(),
            stat("2.1", "avg attempts", hue::muted),
        }));
        cards.push_back(card("Recovery", hue::green, {
            stat("98.4%", "eventually succeeded", hue::green),
        }));
        break;
    }

    case Cache: {
        cards.push_back(card("Hit rate", hue::green, {
            Gauge(s.cache_hits / 100.f, "hits").build(),
            blank(),
            stat(fmt1(s.cache_hits) + "%", "cache hits", hue::green),
        }));
        cards.push_back(card("Hits vs misses", hue::blue, {
            BarChart({{"hits",   s.cache_hits, hue::green},
                      {"misses", s.cache_miss, hue::red}}, 100.f).build(),
        }));
        cards.push_back(card("Tokens saved", hue::violet, {
            stat(fmt_int(static_cast<long>(s.tok_in * s.cache_hits / 100.f)),
                 "cached prompt tokens", hue::violet),
            blank(),
            stat("$" + fmt2(s.cost_total * s.cache_hits / 100.f * 0.9f), "estimated savings", hue::teal),
        }));
        cards.push_back(card("Cache warmth", hue::amber, {
            ProgressRing{}.value(s.cache_hits / 100.f).label("warm").build(),
        }));
        break;
    }

    } // switch
    return cards;
}

// ── the program ────────────────────────────────────────────────────────────
struct AgentStats {
    struct Model {
        Stats stats;
        int   tab     = Overview;
        int   ceiling = 34;
        Flow  flow    = Flow::Row;
        int   term_w  = 120;
        int   term_h  = 40;
        mutable ScrollState scroll;
        Model() { stats.init(); }
    };

    struct Tick {};
    struct NextTab {};
    struct PrevTab {};
    struct GotoTab { int i; };
    struct Wider {};
    struct Narrower {};
    struct ToggleFlow {};
    struct Scroll { KeyEvent key; };
    struct Wheel  { MouseEvent mouse; };
    struct Resize { Size size; };
    struct Quit {};
    using Msg = std::variant<Tick, NextTab, PrevTab, GotoTab, Wider, Narrower,
                             ToggleFlow, Scroll, Wheel, Resize, Quit>;

    using Cmd = jaal::Cmd<Msg>;
    using Sub = jaal::Sub<Msg>;

    static int viewport_h(const Model& m) { return std::max(4, m.term_h - 7); }
    static constexpr int kBar = 1, kGap = 1, kPad = 2;
    static int grid_w(const Model& m) { return std::max(1, m.term_w - kPad - kGap - kBar); }

    static Cmd update(Model& m, Tick)      { m.stats.tick(0.12f); return {}; }
    static Cmd update(Model& m, NextTab)   { m.tab = (m.tab + 1) % kTabCount; m.scroll.y = 0; return {}; }
    static Cmd update(Model& m, PrevTab)   { m.tab = (m.tab + kTabCount - 1) % kTabCount; m.scroll.y = 0; return {}; }
    static Cmd update(Model& m, GotoTab g) { if (g.i >= 0 && g.i < kTabCount) { m.tab = g.i; m.scroll.y = 0; } return {}; }
    static Cmd update(Model& m, Wider)     { m.ceiling = std::min(m.ceiling + 4, 90); return {}; }
    static Cmd update(Model& m, Narrower)  { m.ceiling = std::max(m.ceiling - 4, 16); return {}; }
    static Cmd update(Model& m, ToggleFlow){ m.flow = (m.flow == Flow::Row) ? Flow::Column : Flow::Row; return {}; }
    static Cmd update(Model& m, Scroll sm) { (void)m.scroll.handle(sm.key, viewport_h(m)); return {}; }
    static Cmd update(Model& m, Wheel w)   { (void)m.scroll.handle(w.mouse); return {}; }
    static Cmd update(Model& m, Resize r)  { m.term_w = r.size.width.value; m.term_h = r.size.height.value; return {}; }
    static Cmd update(Model& m, Quit)      { m = Model{}; return Cmd::quit(0); }

    static Element view(const Model& m) {
        // Header: title + tab strip.
        TabStrip strip;
        for (int i = 0; i < kTabCount; ++i) strip.tab(kTabNames[i]);
        strip.active(m.tab);
        strip.theme.accent = kTabHue[m.tab];

        // Body: this tab's cards, laid out by viewport(). A tab with no data
        // (viewport() renders nothing for an empty set) gets a friendly
        // empty state instead of a blank void.
        const int vh = viewport_h(m);
        auto cards = tab_cards(m.tab, m.stats);

        Element body;
        if (cards.empty()) {
            body = (EmptyState{}
                        .glyph("◇")
                        .title(std::string(kTabNames[m.tab]) + " — no data yet")
                        .hint("Stats appear here once the agent produces them.")
                        .action("Tab", "next tab")
                        .action("q", "quit")
                    | grow(1)).build();
        } else {
            Element grid = viewport(std::move(cards),
                                    ViewportOpts{.max_width = m.ceiling,
                                                 .gap = 3, .gap_y = 2,
                                                 .width = grid_w(m),
                                                 .flow = m.flow});
            auto& sc = m.scroll;
            auto sb = hstack();
            sb.gap(1);
            body = sb(
                std::move(grid) | scroll(sc, grid_w(m), vh),
                scrollbar_y(m.scroll, vh, ScrollbarStyle::block())
            );
        }

        std::string status =
            std::string("flow=") + (m.flow == Flow::Row ? "row" : "col") +
            "  ceiling=" + std::to_string(m.ceiling) +
            "  y=" + std::to_string(m.scroll.y) + "/" + std::to_string(m.scroll.max_y) +
            "   ·  Tab/1-8 switch · ↑↓·wheel scroll · f flow · +/- width · q quit";

        auto root = vstack();
        root.gap(0);
        root.padding(1);
        return root(
            h(
                text("◆ AI Agent Stats", Style{}.with_fg(hue::cyan).with_bold()),
                text("   live", Style{}.with_fg(hue::green)),
                text(" ● ", Style{}.with_fg(hue::green))
            ).build(),
            strip.build(),
            blank(),
            std::move(body),
            (text(status, Style{}.with_dim()) | nowrap)
        );
    }

    static std::optional<Msg> on(const Model&, const KeyEvent& k) {
        if (key_is(k, 'q')) return Quit{};
        if (key_is(k, 'f')) return ToggleFlow{};
        if (key_is(k, '+') || key_is(k, '=')) return Wider{};
        if (key_is(k, '-')) return Narrower{};
        if (key_is(k, SpecialKey::Tab)) return NextTab{};
        if (key_is(k, SpecialKey::Right)) return NextTab{};
        if (key_is(k, SpecialKey::Left))  return PrevTab{};
        for (char c = '1'; c <= '8'; ++c)
            if (key_is(k, c)) return GotoTab{c - '1'};
        return Scroll{k};
    }
    static std::optional<Msg> on(const Model&, const MouseEvent& me) { return Wheel{me}; }
    static std::optional<Msg> on(const Model&, const ResizeEvent& r) {
        return Resize{Size{r.width, r.height}};
    }

    static Sub subscribe(const Model&) {
        return Sub::every(std::chrono::milliseconds{120}, Tick{});
    }
};

static_assert(Program<AgentStats>, "AgentStats must satisfy the App concept");

} // namespace

int main() {
    return run<AgentStats>({.title = "agent stats", .mouse = true});
}
