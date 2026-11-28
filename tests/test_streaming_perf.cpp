// test_streaming_perf — perf-regression oracle for the streaming markdown
// pipeline. Guards the invariants the long-turn CPU fixes established:
//
//   1. FLAT STREAMING: per-frame cost of append()+build()+render_tree()
//      must NOT grow with stream progress (the O(N)/frame cache-escape
//      class: ingest-seam memcmp, loose-list chunker gate, prefix
//      re-verify). Oracle: median cost of the last stream quartile vs
//      the first — ratio-based, so machine speed doesn't matter.
//
//   2. O(1) SETTLED BUILD: after finish(), build() must be a cache hit —
//      orders of magnitude cheaper than the paint. Oracle: settled build
//      median vs settled full-frame median.
//
//   3. BOUNDED IDLE REPAINT vs TRANSCRIPT SIZE: repainting a settled
//      transcript 8x larger must not cost proportionally more (the
//      epoch-skip / preserved-prefix guarantee). Oracle: idle repaint
//      median at 8x doc vs 1x doc.
//
// All assertions are RATIOS with generous slack (3-4x headroom over the
// measured values) so a loaded CI box passes while a genuine O(N) escape
// (which shows up as 10-100x) still fails deterministically.

#include "agtest.hpp"

static int g_failed = 0;

#include <maya/maya.hpp>
#include <maya/widget/agentty/markdown.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace maya;

namespace {


void check(bool ok, const char* what) {
    std::printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++g_failed;
}

std::string make_doc(int sections) {
    std::string d;
    for (int i = 0; i < sections; ++i) {
        d += "## Section " + std::to_string(i) + "\n\n";
        d += "Paragraph with **bold**, `code`, and a [link](https://x.y/" +
             std::to_string(i) + ") plus some longer prose that wraps across "
             "multiple terminal rows when rendered at width 120.\n\n";
        if (i % 3 == 0)
            d += "```cpp\nint f" + std::to_string(i) +
                 "() {\n    return " + std::to_string(i) + ";\n}\n```\n\n";
        if (i % 4 == 0)
            d += "- item one about " + std::to_string(i) +
                 "\n- item two\n  - nested\n\n";
        if (i % 7 == 0)
            d += "| col a | col b |\n|---|---|\n| " + std::to_string(i) +
                 " | val |\n\n";
        if (i % 5 == 0)
            d += "> a quote about section " + std::to_string(i) + "\n\n";
    }
    return d;
}

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v.empty() ? 0.0 : v[v.size() / 2];
}

struct StreamResult {
    double q1_ms;        // median frame cost, first stream quartile
    double q4_ms;        // median frame cost, last stream quartile
    double idle_ms;      // median settled full-frame (build+render)
    double idle_build_ms;// median settled build() alone
};

StreamResult stream_doc(const std::string& doc, std::size_t chunk) {
    StylePool pool;
    Canvas canvas(120, 400, &pool);
    StreamingMarkdown md;

    std::vector<double> frame_ms;
    frame_ms.reserve(doc.size() / chunk + 2);
    std::size_t fed = 0;
    while (fed < doc.size()) {
        const std::size_t n = std::min(chunk, doc.size() - fed);
        auto t0 = std::chrono::steady_clock::now();
        md.append(std::string_view(doc).substr(fed, n));
        Element root = md.build();
        render_tree(root, canvas, pool, theme::native, /*auto_height=*/true);
        auto t1 = std::chrono::steady_clock::now();
        frame_ms.push_back(
            std::chrono::duration<double, std::milli>(t1 - t0).count());
        fed += n;
    }
    md.finish();
    (void)md.build();  // absorb the finish() rebuild
    render_tree(md.build(), canvas, pool, theme::native, true);

    const std::size_t N = frame_ms.size();
    std::vector<double> q1(frame_ms.begin(), frame_ms.begin() + N / 4);
    std::vector<double> q4(frame_ms.begin() + (N * 3) / 4, frame_ms.end());

    std::vector<double> idle, idle_build;
    for (int f = 0; f < 200; ++f) {
        auto t0 = std::chrono::steady_clock::now();
        Element root = md.build();
        auto tb = std::chrono::steady_clock::now();
        render_tree(root, canvas, pool, theme::native, true);
        auto t1 = std::chrono::steady_clock::now();
        idle.push_back(
            std::chrono::duration<double, std::milli>(t1 - t0).count());
        idle_build.push_back(
            std::chrono::duration<double, std::milli>(tb - t0).count());
    }
    return {median(std::move(q1)), median(std::move(q4)),
            median(std::move(idle)), median(std::move(idle_build))};
}

}  // namespace

TEST_CASE("streaming_perf") {
    std::printf("=== test_streaming_perf ===\n");

    // 1x doc ≈ 33 KB, 8x ≈ 267 KB. 24-byte chunks ≈ one LLM token burst.
    const std::string doc1 = make_doc(160);
    const std::string doc8 = make_doc(1280);

    const StreamResult r1 = stream_doc(doc1, 24);
    const StreamResult r8 = stream_doc(doc8, 24);

    std::printf("  [1x %zu B] q1=%.3f q4=%.3f idle=%.4f build=%.4f ms\n",
                doc1.size(), r1.q1_ms, r1.q4_ms, r1.idle_ms, r1.idle_build_ms);
    std::printf("  [8x %zu B] q1=%.3f q4=%.3f idle=%.4f build=%.4f ms\n",
                doc8.size(), r8.q1_ms, r8.q4_ms, r8.idle_ms, r8.idle_build_ms);

    // (1) Flat streaming: measured ~1.2x on the 8x doc; an O(N) escape
    //     lands at 10-100x. Gate at 3x, and only when q4 is slow enough
    //     to matter (guards against noise on sub-0.5ms frames).
    check(!(r8.q4_ms > 1.0 && r8.q4_ms > 3.0 * r8.q1_ms),
          "per-frame cost flat vs stream progress (8x doc)");

    // (2) Settled build() is a cache hit: measured ~2% of the frame; a
    //     broken build cache re-parses and lands at ~100%. Gate at 25%.
    check(r8.idle_build_ms < 0.25 * r8.idle_ms || r8.idle_build_ms < 0.05,
          "settled build() is O(1) cache hit");

    // (3) Idle repaint bounded by viewport, not transcript: measured
    //     1.2x across an 8x size jump; a preserved-prefix regression
    //     re-blits the whole transcript and lands at ~8x. Gate at 3x.
    check(r8.idle_ms < 3.0 * r1.idle_ms || r8.idle_ms < 0.5,
          "settled idle repaint bounded by viewport (8x vs 1x)");

    CHECK(g_failed == 0);
}
