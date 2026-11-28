// wire_bytes_bench — measure BYTES-ON-THE-WIRE per frame for the emit path.
//
// The audit established the CPU per-frame cost is at its floor (sub-ms). But
// on a REMOTE link (SSH / mosh / Tailscale) smoothness is governed not by CPU
// but by how many BYTES each frame pushes onto the wire: a frame that emits
// 4 KB of re-styled ANSI stutters over a congested link where a 200-byte frame
// glides. Nobody currently measures or gates this — this bench does.
//
// It drives the REAL streaming-markdown pipeline through the REAL FrameBuffer
// diff-emitter (the exact ANSI a terminal receives, including differential
// SGR, ASCII batching, EL-clear coalescing, CUP elision) and reports:
//
//   bytes/frame   median / p99 ANSI bytes emitted per streaming frame
//   bytes/cell    amortised bytes per CHANGED cell (the encoding efficiency
//                 metric — 1.0 would be "one byte per changed glyph, zero
//                 overhead"; ANSI's cursor moves + SGR push this above 1)
//   total KB      total wire volume for the whole stream
//
// Run:  ./maya_tests wire_bytes_bench     (doctest picks it by name)
// Env:  WIRE_VERBOSE=1  → per-scenario detail
//
// This is a BASELINE-and-gate harness: it prints numbers a human reads, and
// (when WIRE_ASSERT=1) fails if bytes/frame blows past a generous ceiling —
// the regression guard for the wire format.

#undef NDEBUG
#include "agtest.hpp"

#include <maya/maya.hpp>
#include <maya/render/frame.hpp>
#include <maya/render/grid_emit.hpp>
#include <maya/render/renderer.hpp>
#include <maya/render/serialize.hpp>
#include <maya/widget/agentty/markdown.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <string>
#include <vector>

using namespace maya;

namespace {

// A realistic assistant answer: prose that wraps, bold/code spans (style
// churn), a code fence (solid style block), a list, a table. This is the
// content whose per-frame emit cost we care about.
std::string make_answer(int paragraphs) {
    std::string d;
    for (int i = 0; i < paragraphs; ++i) {
        d += "Here is paragraph " + std::to_string(i) +
             " with **bold text**, some `inline_code`, and prose that wraps "
             "across several terminal rows at width 100 so the diff has real "
             "multi-row work to do each frame as tokens arrive.\n\n";
        if (i % 3 == 0)
            d += "```cpp\nint compute_" + std::to_string(i) +
                 "(int x) {\n    return x * " + std::to_string(i + 1) +
                 ";\n}\n```\n\n";
        if (i % 4 == 0)
            d += "- first point on topic " + std::to_string(i) +
                 "\n- second point\n- third point\n\n";
    }
    return d;
}

struct WireStats {
    double   bytes_per_frame_median = 0;
    double   bytes_per_frame_p99    = 0;
    double   bytes_per_changed_cell = 0;
    std::size_t total_bytes         = 0;
    std::size_t frames              = 0;
    std::size_t total_changed_cells = 0;
    std::size_t total_suffix_cells  = 0;   // grid: cells emitted col_lo..width
};

double pct(std::vector<std::size_t> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    const auto idx = std::min(v.size() - 1,
        static_cast<std::size_t>(p * (v.size() - 1)));
    return static_cast<double>(v[idx]);
}

// Count cells that differ between two canvases (the "changed cells" the diff
// had to encode this frame). Cheap O(w*h); only run in the bench.
std::size_t changed_cells(const Canvas& a, const Canvas& b) {
    const int w = std::min(a.width(), b.width());
    const int h = std::min(a.height(), b.height());
    std::size_t n = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (a.get(x, y).pack() != b.get(x, y).pack()) ++n;
    return n;
}

// Stream `doc` in `chunk`-byte increments through a real FrameBuffer, one
// diff frame per chunk, measuring the ANSI bytes each frame emits.
//
// `coalesce` > 1 batches that many chunk-appends before rendering ONE
// cumulative diff frame — modelling backpressure-driven frame coalescing.
// The content that reaches the wire is identical (the diff is cumulative);
// only the per-frame CUP+SGR navigation overhead is amortised across more
// changed cells. This is the ASCII-native smoothness lever.
WireStats stream_wire(const std::string& doc, std::size_t chunk,
                      int width, int height, int coalesce = 1) {
    FrameBuffer fb(width, height);
    StreamingMarkdown md;

    StylePool shadow_pool;
    Canvas shadow(width, height, &shadow_pool);
    shadow.clear();

    std::vector<std::size_t> per_frame;
    WireStats st;

    std::size_t fed = 0;
    int pending = 0;
    while (fed < doc.size()) {
        const std::size_t n = std::min(chunk, doc.size() - fed);
        md.append(std::string_view{doc}.substr(fed, n));
        fed += n;
        if (++pending < coalesce && fed < doc.size()) continue;
        pending = 0;

        const std::string& wire = fb.render(md.build(), theme::native);

        const std::size_t changed = changed_cells(shadow, fb.back().canvas);
        st.total_changed_cells += changed;

        const Canvas& src = fb.back().canvas;
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
                shadow.set(x, y, src.get(x, y).character,
                           src.get(x, y).style_id);

        fb.commit();

        per_frame.push_back(wire.size());
        st.total_bytes += wire.size();
        ++st.frames;
    }

    st.bytes_per_frame_median = pct(per_frame, 0.5);
    st.bytes_per_frame_p99    = pct(per_frame, 0.99);
    st.bytes_per_changed_cell = st.total_changed_cells > 0
        ? static_cast<double>(st.total_bytes) /
          static_cast<double>(st.total_changed_cells)
        : 0.0;
    return st;
}

struct Scenario { const char* name; int paragraphs; std::size_t chunk; };

// Stream `doc` through the GRID emit path (render_tree → canvas → per-row diff
// → emit_diff binary frame), the cooperating-host backend. Measures the same
// bytes-on-the-wire metric as stream_wire so grid and ANSI are directly
// comparable. Mirrors render_grid_frame's row-diff logic (changed row + first
// changed column), minus the scrollback-commit machinery the bench doesn't
// exercise.
WireStats stream_grid(const std::string& doc, std::size_t chunk,
                      int width, int height, bool use_dict = false) {
    StylePool pool;
    Canvas canvas(width, height, &pool);
    StreamingMarkdown md;
    maya::render::StyleAckSet ack;
    ack.varint_runs = use_dict;   // v3 cooperating-host: dict + varint runs

    std::vector<std::uint64_t> prev;  // packed cells of the last emitted frame
    int prev_rows = 0;

    std::vector<std::size_t> per_frame;
    WireStats st;

    std::size_t fed = 0;
    while (fed < doc.size()) {
        const std::size_t n = std::min(chunk, doc.size() - fed);
        md.append(std::string_view{doc}.substr(fed, n));
        fed += n;

        canvas.clear();
        render_tree(md.build(), canvas, pool, theme::native,
                    /*auto_height=*/true);
        const int rows = std::max(1, content_height(canvas));

        // Per-row diff vs the previous emitted frame (same shape as
        // render_grid_frame's growth+diff branch).
        std::vector<int> changed, changed_cols;
        std::size_t changed_cell_count = 0;
        std::size_t suffix_cell_count  = 0;   // cells emitted (col_lo..width)
        for (int y = 0; y < rows; ++y) {
            if (y >= prev_rows) {                    // fresh growth row
                changed.push_back(y);
                changed_cols.push_back(0);
                suffix_cell_count += static_cast<std::size_t>(width);
                for (int x = 0; x < width; ++x)
                    if (canvas.get_packed(x, y) != 0) ++changed_cell_count;
                continue;
            }
            const std::uint64_t* pr = &prev[static_cast<std::size_t>(y) * width];
            int first_diff = -1;
            for (int x = 0; x < width; ++x) {
                if (pr[x] != canvas.get_packed(x, y)) {
                    if (first_diff < 0) first_diff = x;
                    ++changed_cell_count;
                }
            }
            if (first_diff >= 0) {
                changed.push_back(y);
                changed_cols.push_back(first_diff);
                suffix_cell_count +=
                    static_cast<std::size_t>(width - first_diff);
            }
        }

        std::string wire;
        if (!changed.empty()) {
            maya::render::emit_diff(canvas, pool, changed, /*base_row=*/0,
                                    /*cursor=*/nullptr, wire, &changed_cols,
                                    use_dict ? &ack : nullptr,
                                    use_dict && !prev.empty() ? prev.data() : nullptr,
                                    use_dict ? width : 0,
                                    use_dict ? prev_rows : 0);
        }

        // Snapshot for next frame's diff.
        prev.assign(static_cast<std::size_t>(rows) * width, 0);
        for (int y = 0; y < rows; ++y)
            for (int x = 0; x < width; ++x)
                prev[static_cast<std::size_t>(y) * width + x] =
                    canvas.get_packed(x, y);
        prev_rows = rows;

        st.total_changed_cells += changed_cell_count;
        st.total_suffix_cells  += suffix_cell_count;
        per_frame.push_back(wire.size());
        st.total_bytes += wire.size();
        ++st.frames;
    }

    st.bytes_per_frame_median = pct(per_frame, 0.5);
    st.bytes_per_frame_p99    = pct(per_frame, 0.99);
    st.bytes_per_changed_cell = st.total_changed_cells > 0
        ? static_cast<double>(st.total_bytes) /
          static_cast<double>(st.total_changed_cells)
        : 0.0;
    return st;
}

} // namespace

TEST_CASE("wire_bytes_bench") {
    const bool verbose = std::getenv("WIRE_VERBOSE") != nullptr;
    // The byte ceilings below are DETERMINISTIC (bytes on the wire don't vary
    // with CPU load), so unlike a timing budget this gates unconditionally in
    // the default parallel lane — no `perf` label, no flake. WIRE_NOGATE=1
    // downgrades it to measurement-only for local exploration.
    const bool assert_gate = std::getenv("WIRE_NOGATE") == nullptr;
    constexpr int W = 100, H = 400;

    const Scenario scenarios[] = {
        {"token stream (4B chunks)",  24, 4},
        {"word stream  (8B chunks)",  24, 8},
        {"line stream  (40B chunks)", 24, 40},
        {"burst stream (200B chunks)",48, 200},
    };

    std::printf("\nwire_bytes_bench — ANSI bytes on the wire per streaming frame"
                "  (W=%d H=%d)\n", W, H);
    std::printf("%-28s | %10s | %10s | %11s | %9s\n",
                "scenario", "B/frame", "B/frame p99", "B/cell", "total KB");
    std::printf("-----------------------------+------------+------------"
                "+-------------+----------\n");

    for (const auto& s : scenarios) {
        const std::string doc = make_answer(s.paragraphs);
        const WireStats st = stream_wire(doc, s.chunk, W, H);

        std::printf("%-28s | %10.0f | %10.0f | %11.2f | %8.1f\n",
                    s.name, st.bytes_per_frame_median, st.bytes_per_frame_p99,
                    st.bytes_per_changed_cell,
                    static_cast<double>(st.total_bytes) / 1024.0);

        if (verbose) {
            std::printf("    frames=%zu total_changed_cells=%zu doc=%zuB\n",
                        st.frames, st.total_changed_cells, doc.size());
        }

        // Sanity: the emitter must produce SOMETHING and stay bounded.
        CHECK(st.frames > 0, "  %s produced no frames\n", s.name);

        if (assert_gate) {
            // Generous ceiling: a streaming frame that pushes > 8 KB median
            // is a wire regression (style churn / lost EL-clear / CUP storm).
            CHECK(st.bytes_per_frame_median < 8192.0,
                  "  %s median B/frame=%.0f exceeds 8192 ceiling\n",
                  s.name, st.bytes_per_frame_median);
        }
    }

    std::printf("\nnote: B/cell is total wire bytes / total changed cells — the\n"
                "      encoding efficiency. 1.0 = one byte per changed glyph with\n"
                "      zero cursor/SGR overhead; ANSI sits above that. This is the\n"
                "      metric a compact binary frame format would drive toward 1.\n\n");

    // ── Coalesce sweep: the ASCII-native smoothness lever ─────────────────
    // Batch N token-appends into one cumulative diff frame. The CONTENT that
    // reaches the wire is identical; only the per-frame CUP+SGR navigation
    // overhead is amortised. Works on ANY terminal — no protocol change.
    std::printf("coalesce sweep (token stream, 4B chunks) — total wire volume"
                " vs frames coalesced per diff:\n");
    std::printf("%-10s | %8s | %10s | %9s | %8s\n",
                "coalesce", "frames", "total KB", "B/frame", "B/cell");
    std::printf("-----------+----------+------------+-----------+---------\n");
    const std::string sweep_doc = make_answer(24);
    double base_kb = 0;
    double prev_kb = 1e18;
    bool   monotone = true;
    for (int c : {1, 2, 4, 8, 16, 32}) {
        const WireStats st = stream_wire(sweep_doc, 4, W, H, c);
        const double kb = static_cast<double>(st.total_bytes) / 1024.0;
        if (c == 1) base_kb = kb;
        // Coalescing must never INCREASE total wire volume — the diff is
        // cumulative, so batching more appends per frame can only remove
        // per-frame overhead, never add content. A violation means the
        // diff/emit path lost its append-only cumulativity (a real bug).
        if (kb > prev_kb + 0.5) monotone = false;
        prev_kb = kb;
        std::printf("%-10d | %8zu | %10.1f | %9.0f | %7.2f   (%.2f× less wire)\n",
                    c, st.frames, kb, st.bytes_per_frame_median,
                    st.bytes_per_changed_cell,
                    base_kb > 0 ? base_kb / kb : 1.0);
    }
    std::printf("\n");

    if (assert_gate) {
        CHECK(monotone,
              "  coalescing INCREASED total wire volume — the cumulative diff\n"
              "  lost its append-only property (emit/diff regression)\n");
        // The whole premise: coalescing 32 appends/frame must cut total wire
        // by at least 2x vs no coalescing. If this stops holding, the
        // per-frame overhead we're amortising has changed shape — re-profile.
        const double kb1  = static_cast<double>(
            stream_wire(sweep_doc, 4, W, H, 1).total_bytes) / 1024.0;
        const double kb32 = static_cast<double>(
            stream_wire(sweep_doc, 4, W, H, 32).total_bytes) / 1024.0;
        CHECK(kb1 / kb32 > 2.0,
              "  coalescing win eroded: 1x=%.1fKB 32x=%.1fKB ratio=%.2f (< 2)\n",
              kb1, kb32, kb1 / kb32);
    }

    // ── Grid backend vs ANSI: the cooperating-host encoding ─────────────
    // The grid backend emits a compact binary cell-run frame instead of
    // ANSI. Same content, no cursor-navigation text, no SGR re-encode — the
    // B/cell it achieves is the tier-2 north star (drive it toward ~1).
    std::printf("grid backend vs ANSI — same streamed content, bytes on the wire:\n");
    std::printf("%-28s | %8s | %8s | %11s | %9s\n",
                "scenario", "ANSI KB", "grid KB", "grid v3 KB", "v3 save");
    std::printf("-----------------------------+----------+----------+-------------+---------\n");
    for (const auto& s : scenarios) {
        const std::string doc = make_answer(s.paragraphs);
        const WireStats a  = stream_wire(doc, s.chunk, W, H);
        const WireStats g  = stream_grid(doc, s.chunk, W, H, /*v3=*/false);
        const WireStats gd = stream_grid(doc, s.chunk, W, H, /*v3=*/true);
        const double akb  = static_cast<double>(a.total_bytes)  / 1024.0;
        const double gkb  = static_cast<double>(g.total_bytes)  / 1024.0;
        const double gdkb = static_cast<double>(gd.total_bytes) / 1024.0;
        std::printf("%-28s | %8.1f | %8.1f | %11.1f | %6.2fx\n",
                    s.name, akb, gkb, gdkb, gdkb > 0 ? gkb / gdkb : 1.0);
        // Interior-waste probe: how many cells the grid EMITS (col_lo..width
        // suffix) vs how many actually CHANGED. A ratio near 1.0 means the
        // changes ARE contiguous suffixes — interior-span run splitting would
        // save nothing. Well above 1.0 means real interior waste to reclaim.
        const double waste = g.total_changed_cells > 0
            ? double(g.total_suffix_cells) / double(g.total_changed_cells)
            : 0.0;
        std::printf("    ↳ grid emits %zu cells for %zu changed (%.2fx — "
                    "interior-split headroom)\n",
                    g.total_suffix_cells, g.total_changed_cells, waste);
    }
    std::printf("\n");
}
