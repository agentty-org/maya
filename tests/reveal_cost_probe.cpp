// reveal_cost_probe — measure the PER-FRAME WORK of the streaming widget,
// not just how the result looks.
//
// reveal_smoothness_probe answers "did content appear evenly?". This probe
// answers WHY: it counts the engine-level work each frame costs. A frame
// that re-renders the whole document is invisible to a cell-delta metric
// until it grows big enough to blow the pacing deadline — by which point
// the regression is already shipped and reads as "streaming feels janky".
//
// ── The bug this exists to prevent ────────────────────────────────────
//
// maya 8e765ce wrapped StreamingMarkdown's whole document in a width-aware
// component() so a 2-column pad could yield on an ultra-narrow surface:
//
//     cached_build_ = component([kids = ...](int avail_w, int) {
//         const int pad = avail_w >= 4 ? 2 : 0;
//         return vstack().padding(0,0,0,pad)(kids).build();
//     }).build();
//
// That is a one-line, obviously-correct-looking fix. It cost 6.2x more
// stall frames (18 -> 112 on reveal_smoothness_probe) because of a
// property of the engine that is not visible at the call site:
//
//   A ComponentElement is a DEFERRED subtree. With no `measure` callback
//   the engine AUTO-MEASURES it by literally invoking render() during
//   layout and counting rows (renderer.cpp, "Auto-measure"), so render
//   runs TWICE per frame. The cross-frame cache that normally absorbs
//   this is keyed on hash_id; a component with an EMPTY hash_id gets
//   pointer-keyed caching, and pointer-keyed cross-frame hits are
//   deliberately REJECTED (the closure may capture mutated state). So an
//   un-keyed component re-renders its entire subtree every single frame.
//
// Wrapping ONE cheap decoration (a 2-column pad) therefore dragged the
// ENTIRE document — every committed block plus the live tail — behind a
// per-frame rebuild, and the reveal cursor lost its frame budget.
//
// Note the non-obvious part, learned by measuring: hoisting the built
// tree OUT of the lambda so the builder is O(1) does NOT fix it. The
// deferral itself is the cost. Only removing the wrapper (or giving it a
// stable hash_id) restores the budget.
//
// ── What this probe asserts ───────────────────────────────────────────
//
// Per streamed frame, the widget must cost a BOUNDED number of component
// render() calls, and that bound must not scale with how much document
// has already settled. A regression of the 8e765ce class shows up here
// as render calls climbing with committed-block count.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <maya/core/anim_clock.hpp>
#include <maya/core/render_context.hpp>
#include <maya/render/canvas.hpp>
#include <maya/render/renderer.hpp>
#include <maya/style/theme.hpp>
#include <maya/widget/agentty/markdown.hpp>

using namespace maya;

static constexpr int kWidth   = 100;
static constexpr int kTermH   = 40;
static constexpr int kFrameMs = 16;

// A document with MANY settled blocks: the whole point is to detect cost
// that scales with what is already committed.
static std::string make_doc(int blocks) {
    std::string s;
    for (int i = 0; i < blocks; ++i) {
        s += "## Section " + std::to_string(i) + "\n\n";
        s += "Prose paragraph number " + std::to_string(i) +
             " with enough words in it to wrap across more than a single "
             "visual row at the probe's width, so the layout has real work "
             "to do rather than a trivial one-line measure.\n\n";
        s += "- list item one\n- list item two\n\n";
        s += "```\ncode line A\ncode line B\n```\n\n";
    }
    return s;
}

struct Sample {
    int    frame;
    std::size_t committed;   // bytes already settled
    std::uint64_t renders;   // component render() calls this frame
    std::uint64_t build_ns;  // build_layout_tree
    std::uint64_t layout_ns; // layout::compute
    std::uint64_t paint_ns;  // paint_element
    std::uint64_t blit_skip; // rows proven unchanged by write-epoch
    std::uint64_t blit_cmp;  // rows that needed a real compare
};

int main() {
    maya::testing::freeze_anim_clock(0);

    const std::string doc = make_doc(12);

    StreamingMarkdown md;
    // LIVE is what makes this the streaming path: it enables the reveal
    // cursor and the commit/settle machinery. Without it the widget never
    // commits a block and the probe measures a completely different
    // (and much cheaper) code path than production.
    md.set_live(true);
    md.set_reveal_fx(true);
    md.set_reveal_pacing(/*floor_cps=*/90.0, /*drain_secs=*/0.3);

    StylePool pool;
    std::vector<layout::LayoutNode> nodes;

    // ONE canvas for the whole run, reused every frame — this is what the
    // real app does, and it is load-bearing for the measurement. The
    // renderer's per-row write-epoch skip (the fast path that avoids
    // re-blitting rows nothing touched) is armed on
    // (canvas uid, x, y, w, clip) matching the PREVIOUS blit. A fresh
    // Canvas per frame changes the uid, disarms the skip on every entry,
    // and the probe would report a 0% skip ratio that says nothing about
    // production. Allocating per frame would also hide any per-frame
    // allocation cost we might want to see.
    Canvas canvas(kWidth, 4000, &pool);

    auto paint = [&]() {
        RenderContext ctx{kWidth, kTermH, render_generation(), /*auto_height=*/true};
        RenderContextGuard guard(ctx);
        render_tree(md.build(), canvas, pool, theme::native, nodes, /*auto_height=*/true);
    };

    std::vector<Sample> samples;
    std::size_t fed = 0;
    // Feed fast enough that the reveal always has a backlog: the widget is
    // continuously live, which is the state we care about.
    constexpr std::size_t kBytesPerFrame = 8;

    for (int frame = 0; frame < 400; ++frame) {
        if (fed < doc.size()) {
            const std::size_t n = std::min(kBytesPerFrame, doc.size() - fed);
            md.append(std::string_view{doc}.substr(fed, n));
            fed += n;
        }
        maya::testing::advance_anim_clock_ms(kFrameMs);

        const std::uint64_t before  = render_detail::component_render_calls();
        const std::uint64_t b_build  = render_detail::rt_build_ns();
        const std::uint64_t b_layout = render_detail::rt_layout_ns();
        const std::uint64_t b_paint  = render_detail::rt_paint_ns();
        const std::uint64_t b_skip   = render_detail::blit_rows_epoch_skip();
        const std::uint64_t b_cmp    = render_detail::blit_rows_compared();

        paint();

        samples.push_back({
            frame, md.debug_committed(),
            render_detail::component_render_calls() - before,
            render_detail::rt_build_ns()           - b_build,
            render_detail::rt_layout_ns()          - b_layout,
            render_detail::rt_paint_ns()           - b_paint,
            render_detail::blit_rows_epoch_skip()  - b_skip,
            render_detail::blit_rows_compared()    - b_cmp,
        });
    }

    // ── Report ────────────────────────────────────────────────────────
    std::uint64_t total = 0, peak = 0;
    for (const auto& s : samples) { total += s.renders; peak = std::max(peak, s.renders); }
    const double mean = samples.empty() ? 0.0
                      : static_cast<double>(total) / static_cast<double>(samples.size());

    std::printf("frames=%zu  total_renders=%llu  mean=%.2f  peak=%llu\n",
                samples.size(), (unsigned long long)total, mean,
                (unsigned long long)peak);

    // Correlation with settled size is the REGRESSION SIGNAL. Compare the
    // first and last quartiles: if per-frame cost grows as blocks settle,
    // something un-keyed is being re-rendered.
    const std::size_t q = samples.size() / 4;
    std::uint64_t early = 0, late = 0;
    for (std::size_t i = 0; i < q; ++i) early += samples[i].renders;
    for (std::size_t i = samples.size() - q; i < samples.size(); ++i)
        late += samples[i].renders;
    const double early_mean = q ? static_cast<double>(early) / (double)q : 0.0;
    const double late_mean  = q ? static_cast<double>(late)  / (double)q : 0.0;

    std::printf("early_quartile_mean=%.2f  late_quartile_mean=%.2f  growth=%.2fx\n",
                early_mean, late_mean,
                early_mean > 0 ? late_mean / early_mean : 0.0);

    // Phase breakdown — WHERE the frame time goes. build/layout are the
    // phases an un-keyed component inflates (it re-renders during measure,
    // which is inside layout); paint is dominated by how many rows the
    // blit could NOT prove unchanged.
    std::uint64_t tb = 0, tl = 0, tp = 0, sk = 0, cm = 0;
    for (const auto& s : samples) {
        tb += s.build_ns; tl += s.layout_ns; tp += s.paint_ns;
        sk += s.blit_skip; cm += s.blit_cmp;
    }
    const double n = static_cast<double>(samples.size());
    std::printf("\nper-frame phase means (us):  build=%.1f  layout=%.1f  paint=%.1f\n",
                tb / n / 1000.0, tl / n / 1000.0, tp / n / 1000.0);
    std::printf("blit rows/frame: epoch_skip=%.1f  compared=%.1f  (skip ratio %.0f%%)\n",
                sk / n, cm / n,
                (sk + cm) ? 100.0 * (double)sk / (double)(sk + cm) : 0.0);

    std::printf("\nworst frames:\n");
    std::vector<Sample> worst = samples;
    std::sort(worst.begin(), worst.end(),
              [](const Sample& a, const Sample& b) { return a.renders > b.renders; });
    for (std::size_t i = 0; i < std::min<std::size_t>(8, worst.size()); ++i)
        std::printf("  frame %4d  committed=%6zu  renders=%llu\n",
                    worst[i].frame, worst[i].committed,
                    (unsigned long long)worst[i].renders);

    // ── GATE ────────────────────────────────────────────────────
    //
    // Two budgets, because the 8e765ce class of bug shows up in both and
    // the second is the one with teeth.
    //
    // Measured with the document tree correctly cached:
    //   renders/frame 0.33   layout 21.6us  paint 23.8us  blit skip 99%
    // Reintroducing the un-keyed component() wrapper:
    //   renders/frame 0.72   layout 42.8us  paint 75.2us  blit skip 4%
    //
    // The render count doubles, but the REAL damage is the blit: a
    // component that re-renders produces a fresh Element every frame, so
    // the renderer's per-row write-epoch skip is disarmed and rows that
    // nothing touched get re-blitted anyway — 0.3 rows/frame becomes 60.7,
    // a 202x increase in redundant work. That is why the symptom is
    // stutter rather than a small slowdown: it lands on the same thread as
    // the reveal cursor's deadline.
    //
    // Budgets sit between measured-good and measured-bad with headroom:
    constexpr double kMeanRenderBudget = 0.50;   // good 0.33, bad 0.72
    constexpr double kMinBlitSkipRatio = 80.0;   // good 99%,  bad 4%

    const double skip_ratio =
        (sk + cm) ? 100.0 * (double)sk / (double)(sk + cm) : 100.0;

    int fails = 0;
    if (mean > kMeanRenderBudget) {
        std::printf("\nFAIL: mean %.2f component render()/frame exceeds the "
                    "budget of %.2f.\n"
                    "      A component() with no hash_id re-renders its whole "
                    "subtree every frame\n"
                    "      (measure + paint). See the COST CONTRACT in "
                    "element/builder.hpp.\n",
                    mean, kMeanRenderBudget);
        ++fails;
    }
    if (skip_ratio < kMinBlitSkipRatio) {
        std::printf("\nFAIL: blit epoch-skip ratio %.0f%% is below the floor "
                    "of %.0f%%.\n"
                    "      Rows that nothing touched are being re-blitted. "
                    "Usual cause: a subtree\n"
                    "      is being rebuilt every frame, so its cached cells "
                    "never survive to be\n"
                    "      skipped. Same root cause as the render budget "
                    "above.\n",
                    skip_ratio, kMinBlitSkipRatio);
        ++fails;
    }

    std::printf("\n%s\n", fails ? "FAILED" : "PASSED");
    return fails ? 1 : 0;
}
