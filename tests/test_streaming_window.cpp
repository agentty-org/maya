// test_streaming_window.cpp — PROVE the committed-prefix windowing keeps the
// OUTER renderer's per-frame work bounded (O(window)) as a streaming turn
// grows to hundreds of committed blocks, and that the collapsed head wrapper
// hits the renderer's hash-keyed cache instead of re-rendering every frame.
//
// The instrument is render_detail::component_render_calls(): a process-wide
// monotone tally bumped ONLY on a ComponentElement render() cache MISS (at
// either the measure site or the paint site). A cache HIT never bumps it.
// So the delta across a window of steady frames (no new bytes, no commit) is
// the number of components that failed to blit from cache that frame.
//
// Invariants asserted:
//   1. STEADY-STATE HITS: once a long transcript is built and settled, a
//      pure repaint frame (nothing changed) must incur ZERO render calls —
//      every committed block AND the head wrapper blit from cache.
//   2. BOUNDED COMMIT COST: appending one new block (a commit) costs a
//      bounded number of render calls — O(new block + at most one head
//      re-capture on a window slide), NOT O(transcript). Measured as the
//      max per-commit delta over a long stream; it must not scale with N.
//   3. WINDOW SLIDE IS RARE + BOUNDED: the head wrapper's hash changes only
//      on a chunk boundary; when it does, it re-renders exactly once. The
//      total render calls over the whole stream must be ~O(blocks), not
//      O(blocks²) — the O(N²) signature of a per-frame full re-render.

#include "agtest.hpp"

static int g_failed = 0;

#include "maya/widget/agentty/markdown.hpp"
#include "maya/core/render_context.hpp"
#include "maya/render/renderer.hpp"
#include "maya/render/canvas.hpp"
#include "maya/style/style.hpp"
#include "maya/style/theme.hpp"
#include "check.hpp"

#include <cstdint>
#include <cstdio>
#include <print>
#include <string>
#include <vector>

using namespace maya;

static int g_passed = 0;

template <class F>
static void run(const char* name, F&& f) {
    try {
        f();
        std::println("  {:52} ok", name);
        ++g_passed;
    } catch (const std::exception& e) {
        std::println("  {:52} FAIL  {}", name, e.what());
        ++g_failed;
    }
}

// One render of `md` into a canvas, returning the number of component
// render() cache-misses that frame.
//
// auto_height=true + a canvas TALLER than the content mirrors the real
// inline streaming path: maya grows the canvas to the full content height
// and paints every row (rows scroll into native scrollback via compose,
// not via an off-screen clip). This is the mode the production widget runs
// in — a fixed short viewport (auto_height=false) instead pushes committed
// blocks off-screen-below in a way the inline path never does, which would
// exercise a code path the product doesn't hit.
static std::uint64_t render_frame(StreamingMarkdown& md, Canvas& canvas,
                                  StylePool& pool) {
    const std::uint64_t before = render_detail::component_render_calls();
    canvas.clear();
    // build() UNDER a sized RenderContext, which is what the streaming path
    // always has (the app paints inside one).
    //
    // Without it, have_render_context() is false and StreamingMarkdown takes
    // its documented fallback: it cannot know the width, so it defers the
    // outer pad decision into a component() with NO hash_id. A component
    // with an empty hash_id gets pointer keying, whose cross-frame hits are
    // deliberately rejected, so it re-renders its subtree every single frame
    // — forever. That is exactly one un-cached component per frame, which is
    // the `worst == 1` this test was failing on: a fixture artifact, not a
    // windowing regression. The fallback is correct and deliberate (see the
    // DO NOT wrap this in a component() block in streaming/build.cpp); it is
    // simply not the path the product runs, so asserting cache behaviour
    // through it measures the wrong thing.
    RenderContext ctx{canvas.width(), 24, render_generation(),
                      /*auto_height=*/true};
    RenderContextGuard guard(ctx);
    render_tree(md.build(), canvas, pool, theme::native, /*auto_height=*/true);
    return render_detail::component_render_calls() - before;
}

// A distinct settled markdown block per index (paragraph + a fenced code
// block so blocks are chunky and varied). Feeding it with a trailing blank
// line forces commit_range to finalize it into the prefix.
static std::string block(int i) {
    return "## Section " + std::to_string(i) + "\n\n"
         + "Paragraph " + std::to_string(i) + " explains the change and "
           "threads the result through every caller.\n\n"
         + "```cpp\nint f" + std::to_string(i) + "() { return "
         + std::to_string(i) + "; }\n```\n\n";
}

// ── Test 1: steady-state repaint incurs ZERO render calls ──────────────────
// Build a long settled transcript, then repaint it N times with nothing
// changing. Every committed block blits; the collapsed head wrapper blits.
// If ANY frame re-renders a component, windowing/measure caching is broken.
void steady_state_zero_rerenders() {
    StreamingMarkdown md;
    md.set_reveal_fx(false);           // no per-frame tip mutation
    for (int i = 0; i < 200; ++i) md.feed(block(i));
    md.finish();                       // settle: all blocks committed

    StylePool pool;
    Canvas canvas(100, 8000, &pool);  // taller than content: inline auto-height

    // Warm: first two frames populate measure + paint cells.
    render_frame(md, canvas, pool);
    render_frame(md, canvas, pool);

    // Now steady state: no mutation. Each frame must be all-hits.
    std::uint64_t worst = 0;
    for (int f = 0; f < 20; ++f) {
        std::uint64_t d = render_frame(md, canvas, pool);
        if (d > worst) worst = d;
    }
    std::println("    settled 200-block repaint: worst per-frame render calls = {}",
                 worst);
    MAYA_TEST_CHECK(worst == 0,
        "settled transcript re-rendered a component on a no-change frame");
}

// ── Test 2: per-commit render cost is bounded (not O(transcript)) ──────────
// Stream 300 blocks one commit at a time, rendering after each. The render
// calls attributable to each commit must stay bounded as N grows — the
// window collapses the old prefix so a commit never re-renders more than
// (the new block) + (occasionally, one head wrapper on a chunk slide).
void per_commit_cost_bounded() {
    StreamingMarkdown md;
    md.set_reveal_fx(false);
    md.set_live(true);

    StylePool pool;
    Canvas canvas(100, 8000, &pool);  // taller than content: inline auto-height

    // Warm the empty widget.
    render_frame(md, canvas, pool);

    std::vector<std::uint64_t> per_commit;
    per_commit.reserve(300);
    for (int i = 0; i < 300; ++i) {
        md.feed(block(i));
        // Two renders: the first absorbs the commit (measure+paint miss on
        // the genuinely-new content), the second must be all-hits. We charge
        // the commit the FIRST frame's delta and assert the SECOND is 0.
        std::uint64_t commit_delta = render_frame(md, canvas, pool);
        std::uint64_t settle_delta = render_frame(md, canvas, pool);
        per_commit.push_back(commit_delta);
        MAYA_TEST_CHECK(settle_delta == 0,
            "frame after a commit still re-rendered a cached component");
    }

    // Compare the cost of commits late in the stream (N large) to early
    // ones. If the design were O(transcript)/commit, late commits would
    // cost proportionally more. Windowed, they must be flat.
    auto avg = [&](std::size_t lo, std::size_t hi) {
        std::uint64_t s = 0;
        for (std::size_t i = lo; i < hi; ++i) s += per_commit[i];
        return double(s) / double(hi - lo);
    };
    const double early = avg(10, 40);       // blocks 10..40
    const double late  = avg(260, 290);     // blocks 260..290
    std::uint64_t worst = 0;
    for (auto v : per_commit) if (v > worst) worst = v;
    std::println("    per-commit render calls: early avg={:.2f} late avg={:.2f} worst={}",
                 early, late, worst);

    // Late commits must not be materially more expensive than early ones.
    // Allow a small constant slack for the periodic head re-capture.
    MAYA_TEST_CHECK(late <= early + 3.0,
        "per-commit render cost grew with transcript length (windowing failed)");
    // A single commit must never re-render an unbounded slice. The window is
    // 32 individual blocks + at most one head wrapper; a genuine per-frame
    // full re-render of a 300-block transcript would show 100s here.
    MAYA_TEST_CHECK(worst < 40,
        "a single commit re-rendered more than the bounded window");
}

// ── Test 3: total render calls over the stream is ~O(blocks), not O(N²) ────
// The definitive anti-regression: sum every render call across a full
// stream. Windowed, each block renders ~once (its own commit) plus the head
// wrapper re-renders ~N/chunk times, each covering a growing prefix — but
// only ONCE per slide, so the total is O(N + N/chunk * (avg head size)).
// The O(N²) failure mode (re-render the whole prefix every frame) would be
// orders of magnitude larger. We assert a generous linear-ish ceiling that
// still rejects quadratic blow-up.
void total_render_calls_subquadratic() {
    const std::uint64_t start = render_detail::component_render_calls();

    StreamingMarkdown md;
    md.set_reveal_fx(false);
    md.set_live(true);

    StylePool pool;
    Canvas canvas(100, 8000, &pool);  // taller than content: inline auto-height

    constexpr int N = 300;
    render_frame(md, canvas, pool);
    for (int i = 0; i < N; ++i) {
        md.feed(block(i));
        render_frame(md, canvas, pool);   // one frame per commit
        render_frame(md, canvas, pool);   // steady repaint (all hits)
    }
    md.finish();
    render_frame(md, canvas, pool);

    const std::uint64_t total =
        render_detail::component_render_calls() - start;

    // Ceiling: per-block commit renders (~1 each) + head-wrapper slides.
    // With chunk=16 and window=32 the head slides ~N/16 ≈ 19 times over
    // 300 blocks. Each slide re-renders the head's inner blocks once. Even
    // pessimistically bounding that at N per slide gives ~N + (N/16)*N/2.
    // A hard multiple of N that still fails loudly on true O(N²) per-frame
    // (which would be ~N * N frames = 90000+ for N=300) is 60*N.
    std::println("    total render calls over {} blocks × 2 frames each = {} "
                 "(O(N) ceiling {})", N, total, 60 * N);
    MAYA_TEST_CHECK(total < static_cast<std::uint64_t>(60 * N),
        "total render calls scaled quadratically — windowing/caching broke");
}

// ── Test 4: reveal_fx live streaming also stays bounded ────────────────────
// The production path uses reveal_fx (typewriter). Its tail mutates every
// frame (expected — one render for the live tail), but the committed head
// must still blit. Assert the steady per-frame cost with a long committed
// prefix + a live tail is small and flat.
void live_reveal_prefix_stays_cached() {
    StreamingMarkdown md;
    md.set_reveal_fx(true);
    md.set_reveal_pacing(/*floor_cps=*/1e9, /*lead_secs=*/0.0); // reveal instantly
    md.set_live(true);

    StylePool pool;
    Canvas canvas(100, 8000, &pool);  // taller than content: inline auto-height

    for (int i = 0; i < 150; ++i) {
        md.feed(block(i));
        render_frame(md, canvas, pool);
    }
    // Long committed prefix now exists. Append a growing live tail and
    // measure per-frame render calls. This drives render_tree directly
    // into a fresh-cleared canvas each frame with the widget as the flex
    // ROOT (no stretch parent), so the outer vstack self-sizes and its
    // resolved content width can jitter ±1 column frame-to-frame as the
    // live-reveal tail nudges its natural width. The renderer's paint
    // fast-path absorbs that sub-cell jitter (width-tolerant hash-keyed
    // blit: cached cells whose real content fits within both the stored
    // and current width are blitted without a re-render), so the committed
    // prefix stays cached through the wobble. Before that fix this frame
    // re-rendered ~188 segment-inner blocks on every jitter frame.
    std::uint64_t worst = 0;
    for (int f = 0; f < 30; ++f) {
        md.feed("more live tail text " + std::to_string(f) + " ");
        std::uint64_t d = render_frame(md, canvas, pool);
        if (d > worst) worst = d;
    }
    std::println("    live-tail over 150-block prefix: worst per-frame render calls = {}",
                 worst);
    // Bounded by a small constant (the live tail + at most one segment's
    // worth on a rare >1-column jitter), NOT the O(prefix) full re-render
    // (~150 blocks × sub-elements = many hundreds) it was before.
    MAYA_TEST_CHECK(worst < 40,
        "live-tail frame re-rendered an unbounded slice of the committed prefix");
}

TEST_CASE("streaming_window") {
    std::println("=== test_streaming_window ===");
    run("steady-state repaint: zero re-renders", steady_state_zero_rerenders);
    run("per-commit cost bounded (not O(N))",    per_commit_cost_bounded);
    run("total render calls sub-quadratic",      total_render_calls_subquadratic);
    run("live-reveal prefix stays cached",       live_reveal_prefix_stays_cached);

    std::println("\n── summary ──");
    std::println("  passed: {}   failed: {}", g_passed, g_failed);
}
