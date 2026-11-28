// test_card_border.cpp — Render-pipeline regression tests for tool-card
// border integrity across status transitions.
//
// The bug being pinned:
//
//   In the user's live agentty session, AgentTimeline cards render
//   correctly while they're streaming (Running), but the LEFT BORDER
//   COLUMN goes missing once events transition to terminal status
//   (Done / Failed / Rejected) — the moment per-event cache_id
//   activates and ComponentElement::cache_id-keyed cell blitting
//   takes over from the recursive paint walk.
//
//   The unit tests in test_scrollback.cpp + test_scrollback_vt.cpp
//   pass because they exercise compose_inline_frame in isolation
//   against hand-built canvases — never going through the
//   ComponentElement blit path. This file closes that gap.
//
// What "left border" means structurally:
//
//   AgentTimeline wraps its content in a vstack().border(BorderStyle::Round).
//   That puts a Round-style border glyph at column 0 of every row of
//   the card's bounding box:
//     row top     → ╭ (U+256D)
//     row middle  → │ (U+2502)
//     row bottom  → ╰ (U+2570)
//
//   If column 0 contains anything else after render_tree completes
//   (blank, stale content, a non-border codepoint), the border is
//   corrupt and the user sees the missing-left-edge from the
//   screenshot.
//
// Test shape:
//
//   1. Build an AgentTimeline with N events that progressively
//      transition from Running → Done across frames, mimicking the
//      live moha session.
//   2. Each event has cache_id set on its terminal status — exactly
//      what activates the ComponentElement blit path in renderer.cpp.
//   3. After every frame, walk column 0 of the card's bounding box
//      and assert the glyph is one of {╭, │, ╰, space-but-only-
//      outside-card}.
//
// If this test fails: the rendered cells at column 0 reveal what
// the blit path actually deposited. The failure dump shows the
// codepoint that landed there instead of the border, which points
// at either:
//   • blit_packed_row clipping the leftmost cell away
//   • component_cache populating cells without the border glyph
//     (cache_id captured a sub-tree that didn't include the chrome)
//   • a stale-cell survivor from before the cache activated.

#include <maya/maya.hpp>
#include <maya/widget/agentty/agent_timeline.hpp>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <print>
#include <string>
#include <vector>
#include "check.hpp"

using namespace maya;

#undef assert
#define assert(x) MAYA_TEST_CHECK((x), #x)
// Pre-existing CHECK call sites use the local name; alias to the shared one.
#undef CHECK
#define CHECK(cond, msg) MAYA_TEST_CHECK((cond), (msg))

// Codepoints that are LEGAL for column 0 of a Round-border card.
// Anything else at column 0 inside the card's row range is corruption.
constexpr char32_t kBorderTL    = U'\u256D';   // ╭
constexpr char32_t kBorderMid   = U'\u2502';   // │
constexpr char32_t kBorderBL    = U'\u2570';   // ╰
constexpr char32_t kSpace       = U' ';

static const char* cp_name(char32_t cp) {
    switch (cp) {
        case kBorderTL:  return "╭ (top-left)";
        case kBorderMid: return "│ (middle)";
        case kBorderBL:  return "╰ (bottom-left)";
        case kSpace:     return "<space>";
        case 0:          return "<null>";
        default:         return "<other>";
    }
}

// ── Helpers ─────────────────────────────────────────────────────────────────

// Find the FIRST card's rows: top = first ╭, bot = first ╰ at or after
// top. Returns {-1, -1} if no card chrome found at column 0. Tests that
// render multiple cards use the explicit walker in test 4.
static std::pair<int, int> find_card_rows(const Canvas& c) {
    int top = -1, bot = -1;
    const int max_y = c.max_content_row();
    for (int y = 0; y <= max_y; ++y) {
        char32_t cp = c.get(0, y).character;
        if (top < 0 && cp == kBorderTL) top = y;
        else if (top >= 0 && cp == kBorderBL) { bot = y; break; }
    }
    return {top, bot};
}

// Walk column 0 from row `top` to row `bot` and assert every cell is a
// valid border glyph. On failure, prints a column-0 dump for diagnosis.
static void assert_left_border_intact(const Canvas& c, int top, int bot,
                                      const char* ctx)
{
    int bad = -1;
    char32_t bad_cp = 0;
    for (int y = top; y <= bot; ++y) {
        char32_t cp = c.get(0, y).character;
        const bool ok = (y == top) ? (cp == kBorderTL)
                      : (y == bot) ? (cp == kBorderBL)
                                   : (cp == kBorderMid);
        if (!ok) { bad = y; bad_cp = cp; break; }
    }
    if (bad < 0) return;

    std::println("  FAIL [{}]: column 0 of row {} = {} (U+{:04X}); expected border glyph",
                 ctx, bad, cp_name(bad_cp), static_cast<uint32_t>(bad_cp));
    std::println("  ── column-0 dump [{}..{}] ──", top, bot);
    for (int y = top; y <= bot; ++y) {
        char32_t cp = c.get(0, y).character;
        const char* mark = (y == bad) ? " <-- corrupt" : "";
        std::println("    row {:>3}: U+{:04X}  {}{}", y,
                     static_cast<uint32_t>(cp), cp_name(cp), mark);
    }
    CHECK(false, "left border corrupted at column 0");
}

// Walk column W-1 of the card's row range and assert the right border
// is intact too — a symmetric check, because the bug being chased
// could in principle hit either edge.
static void assert_right_border_intact(const Canvas& c, int top, int bot,
                                       int W, const char* ctx)
{
    constexpr char32_t kRoundTR  = U'\u256E';  // ╮
    constexpr char32_t kRoundBR  = U'\u256F';  // ╯
    constexpr char32_t kRoundMid = U'\u2502';  // │ (same as left)

    int bad = -1;
    char32_t bad_cp = 0;
    for (int y = top; y <= bot; ++y) {
        char32_t cp = c.get(W - 1, y).character;
        const bool ok = (y == top) ? (cp == kRoundTR)
                      : (y == bot) ? (cp == kRoundBR)
                                   : (cp == kRoundMid);
        if (!ok) { bad = y; bad_cp = cp; break; }
    }
    if (bad < 0) return;
    std::println("  FAIL [{}]: column {} of row {} = U+{:04X}; expected right border",
                 ctx, W - 1, bad, static_cast<uint32_t>(bad_cp));
    CHECK(false, "right border corrupted");
}

// Build a "done" event matching what real agentty produces.
static AgentTimelineEvent make_done_event(int idx, bool with_hash_id) {
    AgentTimelineEvent ev;
    ev.name = "read";
    ev.detail = "src/file_" + std::to_string(idx) + ".cpp";
    ev.elapsed_seconds = 0.42f;
    ev.category_color = Color::cyan();
    ev.status = AgentEventStatus::Done;
    ev.body.kind = ToolBodyPreview::Kind::FileRead;
    std::string text;
    for (int j = 0; j < 4; ++j)
        text += "line " + std::to_string(j) + " of file " + std::to_string(idx) + "\n";
    ev.body.text = std::move(text);
    if (with_hash_id) {
        ev.hash_id = CacheIdBuilder{}
            .add(std::string_view{"tool:done"})
            .add(idx)
            .build();
    }
    return ev;
}

static AgentTimelineEvent make_running_event(int idx) {
    AgentTimelineEvent ev;
    ev.name = "bash";
    ev.detail = "running cmd " + std::to_string(idx);
    ev.category_color = Color::green();
    ev.status = AgentEventStatus::Running;
    ev.body.kind = ToolBodyPreview::Kind::BashOutput;
    ev.body.text = "intermediate...\n";
    return ev;
}

// Render an AgentTimeline at width W into an auto-height canvas, return
// the canvas (StylePool is created fresh — borders use intern'd styles
// but that's fine, the cells we inspect are codepoints).
static void render_timeline(const AgentTimeline::Config& cfg,
                            int W,
                            Canvas& canvas,
                            StylePool& pool)
{
    AgentTimeline tl{cfg};
    Element root = tl.build();
    std::vector<layout::LayoutNode> layout_nodes;
    canvas.clear();
    render_tree(root, canvas, pool, theme::native, layout_nodes,
                /*auto_height=*/true);
}

// ── Test 1: single frame, all done — left border must be there ─────────────
//
// The minimal case. If this fails, the bug is in the FIRST cache_id
// engagement — the cache miss path that captures cells doesn't include
// the border glyph.

void test_card_border_single_frame_all_done_with_hash_id() {
    std::println("--- test_card_border_single_frame_all_done_with_hash_id ---");
    constexpr int W = 80;
    StylePool pool;
    Canvas canvas(W, 200, &pool);

    AgentTimeline::Config cfg;
    cfg.title = " ACTIONS · 1/1 · 754ms ";
    for (int i = 0; i < 3; ++i) cfg.events.push_back(make_done_event(i, true));
    cfg.footer = AgentTimelineFooter{
        .glyph = "\xe2\x9c\x93", .text = "done",
        .color = Color::green(), .summary = "3 actions  1.4s"
    };
    render_timeline(cfg, W, canvas, pool);

    auto [top, bot] = find_card_rows(canvas);
    std::println("  card rows: [{}..{}]", top, bot);
    CHECK(top >= 0 && bot > top, "no card chrome found at column 0");

    assert_left_border_intact(canvas, top, bot, "single-frame-all-done");
    assert_right_border_intact(canvas, top, bot, W, "single-frame-all-done");

    std::println("PASS\n");
}

// ── Test 2: status transition (Running → Done with cache_id) preserves border ─
//
// This is the most direct reproducer of the screenshot. Frame 1: card
// with one running event (no cache_id, recursive paint every frame).
// Frame 2: same event flipped to Done with cache_id set (first miss,
// captures cells). Frame 3: same Done event again (cache hit, blit
// path). After frame 3, the border at column 0 must still be intact —
// the symptom in the user's screenshot is that this is where it goes
// missing.

void test_card_border_survives_running_to_done_transition() {
    std::println("--- test_card_border_survives_running_to_done_transition ---");
    constexpr int W = 80;
    StylePool pool;
    Canvas canvas(W, 200, &pool);

    // Frame 1: running.
    {
        AgentTimeline::Config cfg;
        cfg.title = " ACTIONS · 1/1 ";
        AgentTimelineEvent ev = make_running_event(0);
        cfg.events.push_back(ev);
        render_timeline(cfg, W, canvas, pool);

        auto [top, bot] = find_card_rows(canvas);
        CHECK(top >= 0, "frame 1 (running): card chrome missing");
        assert_left_border_intact(canvas, top, bot, "frame-1-running");
    }

    // Frames 2..6: same event transitioned to Done with cache_id.
    // Frame 2 populates the cache (miss). Frames 3+ blit (hits).
    // The border must survive both code paths.
    for (int f = 2; f <= 6; ++f) {
        AgentTimeline::Config cfg;
        cfg.title = " ACTIONS · 1/1 · 754ms ";
        AgentTimelineEvent ev = make_done_event(0, /*with_hash_id=*/true);
        cfg.events.push_back(ev);
        cfg.footer = AgentTimelineFooter{
            .glyph = "\xe2\x9c\x93", .text = "done",
            .color = Color::green(), .summary = "1 action  754ms"
        };
        render_timeline(cfg, W, canvas, pool);

        auto [top, bot] = find_card_rows(canvas);
        CHECK(top >= 0, "frame N: card chrome missing");
        std::string ctx = "frame-" + std::to_string(f) + "-done-cached";
        assert_left_border_intact(canvas, top, bot, ctx.c_str());
        assert_right_border_intact(canvas, top, bot, W, ctx.c_str());
    }

    std::println("PASS\n");
}

// ── Test 3: MULTIPLE cards stacked, each one transitioning at a different
//    frame. This is what the user's session actually does — many tool
//    calls fire in sequence, transition Done, the card list grows.
//
// Every transition is an opportunity for the cache to engage. After
// each transition, ALL prior cards' borders must remain intact.

void test_card_borders_intact_across_cascade_of_transitions() {
    std::println("--- test_card_borders_intact_across_cascade_of_transitions ---");
    constexpr int W = 80;
    constexpr int N_EVENTS = 5;
    StylePool pool;
    Canvas canvas(W, 500, &pool);

    // Frame `f`: events [0..f-1] are Done (with cache_id), event f is
    // Running, events [f+1..N_EVENTS-1] are Pending.
    for (int f = 0; f <= N_EVENTS; ++f) {
        AgentTimeline::Config cfg;
        cfg.title = " ACTIONS · " + std::to_string(f) + "/" +
                    std::to_string(N_EVENTS) + " ";
        for (int i = 0; i < N_EVENTS; ++i) {
            if (i < f) {
                cfg.events.push_back(make_done_event(i, /*with_hash_id=*/true));
            } else if (i == f) {
                cfg.events.push_back(make_running_event(i));
            } else {
                AgentTimelineEvent ev = make_running_event(i);
                ev.status = AgentEventStatus::Pending;
                cfg.events.push_back(ev);
            }
        }
        if (f == N_EVENTS) {
            cfg.footer = AgentTimelineFooter{
                .glyph = "\xe2\x9c\x93", .text = "done",
                .color = Color::green(),
                .summary = std::to_string(N_EVENTS) + " actions"
            };
        }
        cfg.frame = f;
        render_timeline(cfg, W, canvas, pool);

        auto [top, bot] = find_card_rows(canvas);
        CHECK(top >= 0, "card chrome missing");

        std::string ctx = "cascade-frame-" + std::to_string(f);
        assert_left_border_intact(canvas, top, bot, ctx.c_str());
        assert_right_border_intact(canvas, top, bot, W, ctx.c_str());
    }

    std::println("PASS\n");
}

// ── Test 4: TWO independent AgentTimelines (two turns) stacked.
//
// Simulates two consecutive turns each with their own card. The bug
// shape we're chasing could span cards: e.g. the second card's
// component_cache lookup hits an entry from the first card (cache_id
// collision, address reuse, …) and blits cells from card A into
// card B's region, clobbering its left border.

void test_two_stacked_card_panels_borders_independent() {
    std::println("--- test_two_stacked_card_panels_borders_independent ---");
    constexpr int W = 80;
    StylePool pool;
    Canvas canvas(W, 500, &pool);

    using namespace maya::dsl;

    auto card_a = [] {
        AgentTimeline::Config cfg;
        cfg.title = " ACTIONS · A ";
        for (int i = 0; i < 2; ++i) cfg.events.push_back(make_done_event(100 + i, true));
        cfg.footer = AgentTimelineFooter{
            .glyph = "\xe2\x9c\x93", .text = "done", .color = Color::green(),
            .summary = "2 actions"
        };
        return AgentTimeline{cfg}.build();
    };
    auto card_b = [] {
        AgentTimeline::Config cfg;
        cfg.title = " ACTIONS · B ";
        for (int i = 0; i < 2; ++i) cfg.events.push_back(make_done_event(200 + i, true));
        cfg.footer = AgentTimelineFooter{
            .glyph = "\xe2\x9c\x93", .text = "done", .color = Color::green(),
            .summary = "2 actions"
        };
        return AgentTimeline{cfg}.build();
    };

    // Render twice — first frame populates caches, second blits.
    for (int frame = 0; frame < 3; ++frame) {
        Element root = vstack()(card_a(), card_b()).build();
        std::vector<layout::LayoutNode> layout_nodes;
        canvas.clear();
        render_tree(root, canvas, pool, theme::native, layout_nodes,
                    /*auto_height=*/true);

        // Find BOTH cards. Walk column 0; a card starts at ╭ and ends
        // at ╰. Two stacked cards share an edge with NO blank row
        // between them — the boundary is ╰ immediately followed by ╭,
        // not a non-border run.
        const int max_y = canvas.max_content_row();
        std::vector<std::pair<int, int>> cards;
        int cur_top = -1;
        for (int y = 0; y <= max_y; ++y) {
            char32_t cp = canvas.get(0, y).character;
            if (cp == kBorderTL) {
                if (cur_top >= 0) {
                    // Implicit close of previous card at y-1.
                    cards.emplace_back(cur_top, y - 1);
                }
                cur_top = y;
            } else if (cp == kBorderBL && cur_top >= 0) {
                cards.emplace_back(cur_top, y);
                cur_top = -1;
            } else if (cp != kBorderMid && cur_top >= 0) {
                // Non-border interrupted an open card — close it at y-1.
                cards.emplace_back(cur_top, y - 1);
                cur_top = -1;
            }
        }
        if (cur_top >= 0) cards.emplace_back(cur_top, max_y);

        std::println("  frame {}: detected {} card(s) at column 0", frame, cards.size());
        for (auto [t, b] : cards) std::println("    rows {}..{}", t, b);

        CHECK(cards.size() == 2,
            "expected two separate cards at column 0 — fewer means a card's "
            "left border vanished entirely (the screenshot symptom)");

        for (std::size_t i = 0; i < cards.size(); ++i) {
            std::string ctx = "two-stacked-frame-" + std::to_string(frame) +
                              "-card-" + std::to_string(i);
            assert_left_border_intact(canvas, cards[i].first, cards[i].second,
                                      ctx.c_str());
        }
    }

    std::println("PASS\n");
}

// ── Test 5: card border survives even when the previous frame's canvas
//    has been cleared and re-painted with DIFFERENT content beneath.
//
// The cache_id blit copies a sub-tree's cells into the live canvas.
// If between frames the surrounding layout shifts the card's
// (content_x, content_y) origin, the blit must STILL deposit the
// border at column 0 of the new origin — not at the old position.

void test_card_border_survives_layout_shift_above() {
    std::println("--- test_card_border_survives_layout_shift_above ---");
    constexpr int W = 80;
    StylePool pool;
    Canvas canvas(W, 500, &pool);
    using namespace maya::dsl;

    AgentTimeline::Config cfg;
    cfg.title = " ACTIONS · shift ";
    for (int i = 0; i < 3; ++i) cfg.events.push_back(make_done_event(300 + i, true));
    cfg.footer = AgentTimelineFooter{
        .glyph = "\xe2\x9c\x93", .text = "done",
        .color = Color::green(), .summary = "3 actions"
    };

    for (int frame = 0; frame < 4; ++frame) {
        // Each frame prepends a different number of header lines
        // ABOVE the card, shifting its content_y origin.
        std::vector<Element> header_rows;
        for (int j = 0; j <= frame; ++j)
            header_rows.push_back(text("header line " + std::to_string(j)));

        AgentTimeline tl{cfg};
        std::vector<Element> all = std::move(header_rows);
        all.push_back(tl.build());
        Element root = vstack()(all).build();

        std::vector<layout::LayoutNode> layout_nodes;
        canvas.clear();
        render_tree(root, canvas, pool, theme::native, layout_nodes,
                    /*auto_height=*/true);

        auto [top, bot] = find_card_rows(canvas);
        CHECK(top >= 0, "card chrome missing after layout shift");
        std::string ctx = "shift-frame-" + std::to_string(frame);
        assert_left_border_intact(canvas, top, bot, ctx.c_str());
        assert_right_border_intact(canvas, top, bot, W, ctx.c_str());
    }

    std::println("PASS\n");
}

// ── main ────────────────────────────────────────────────────────────────────

int main() {
    test_card_border_single_frame_all_done_with_hash_id();
    test_card_border_survives_running_to_done_transition();
    test_card_borders_intact_across_cascade_of_transitions();
    test_two_stacked_card_panels_borders_independent();
    test_card_border_survives_layout_shift_above();

    std::println("\n=== All 5 card-border tests passed ===");
    return 0;
}
