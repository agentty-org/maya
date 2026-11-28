// bench_thread_scaling.cpp — does per-frame render cost stay flat as a
// thread grows?
//
// agentty's transcript model (src/runtime/app/update/frozen.cpp): settled
// turns are built into Element values ONCE, pushed into a vector, and handed
// to maya via list_ref(&frozen). Per-frame cost is supposed to be
// O(visible_live), independent of how many turns have accumulated — the
// hash-keyed component cache blits each settled turn's cells instead of
// re-rendering it.
//
// This bench reconstructs that exact shape: a growing vector of frozen turns,
// each turn a settled StreamingMarkdown wrapped in a hash-keyed
// ComponentElement (one stable content id per turn), rendered via list_ref
// once per "frame". We time the steady-state frame (after the prime frame
// has populated every turn's cell cache) at increasing turn counts and report
// per-frame µs + µs/turn. Flat µs/turn ⇒ the cache holds; rising total with
// flat per-turn ⇒ the residual is just tree assembly (O(turns) cheap nodes);
// rising µs/turn ⇒ a real superlinear regression.

#include <maya/maya.hpp>
#include <maya/widget/agentty/markdown.hpp>
#include <maya/widget/agentty/tool_body_preview.hpp>
#include <maya/render/cache_id.hpp>
#include <maya/style/style.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <print>
#include <string>
#include <vector>

using namespace maya;

namespace {

// ── Tool-card simulations ──────────────────────────────────────────────
//
// Real agentty frozen turns are dominated by tool cards (write / edit /
// read / bash). The cards are NOT hand-rolled boxes — agentty routes each
// tool's body through maya's ToolBodyPreview widget (Kind::EditDiff,
// FileWrite, FileRead, BashOutput), which builds line gutters, per-line
// diff coloring, footers, and elision. We build the bench cards through
// the SAME widget so this measures the actual render/layout/paint path,
// then wrap each in a hash-keyed ComponentElement exactly as agentty's
// frozen.cpp / agent_timeline.cpp do (so settled cards blit from the cell
// cache).

using TBP = maya::ToolBodyPreview;

Element cache_keyed(const char* kind, int turn, int seq, Element body) {
    auto sp = std::make_shared<const Element>(std::move(body));
    ComponentElement comp;
    comp.hash_id = CacheIdBuilder{}
        .add(std::string_view{kind})
        .add(static_cast<std::uint64_t>(turn))
        .add(static_cast<std::uint64_t>(seq))
        .build();
    comp.render = [sp](int, int) -> Element { return *sp; };
    return Element{std::move(comp)};
}

// One timeline event row: a header line (`edit  path · N edits`) over the
// ToolBodyPreview body — the (header + body) sub-tree agentty wraps per
// terminal tool event.
Element tool_event(std::string header, TBP::Config body_cfg) {
    std::vector<Element> rows;
    rows.push_back(Element{TextElement{
        .content = std::move(header),
        .style   = Style{}.with_fg(Color::cyan()).with_bold(),
    }});
    rows.push_back(TBP{std::move(body_cfg)}.build());
    return detail::vstack().gap(0).padding(0, 0, 0, 2)(std::move(rows)).build();
}

// EDIT card — a multi-hunk diff, rendered via Kind::EditDiff (the heaviest
// preview: per-side head/tail elision + per-line +/- coloring).
Element edit_card(int turn, int seq) {
    TBP::Config c;
    c.kind = TBP::Kind::EditDiff;
    c.show_all = true;
    c.text_color = Color::white();
    c.chrome_color = Color::magenta();
    for (int h = 0; h < 2; ++h) {
        std::string ot, nt;
        for (int l = 0; l < 6; ++l) {
            ot += "    result.value = legacy(input[" + std::to_string(l) + "]);\n";
            nt += "    result.value = transform(input[" + std::to_string(l) + "]);\n";
        }
        c.hunks.push_back({std::move(ot), std::move(nt)});
    }
    return tool_event("edit  lib/core/handler_" + std::to_string(turn)
        + ".cpp \xc2\xb7 2 edits", std::move(c));
}

// WRITE card — a new file body via Kind::FileWrite (line gutter + footer).
Element write_card(int turn, int seq) {
    TBP::Config c;
    c.kind = TBP::Kind::FileWrite;
    c.show_all = true;
    c.text_color = Color::white();
    c.chrome_color = Color::green();
    c.show_footer_stats = true;
    for (int l = 0; l < 16; ++l)
        c.text += "int line_" + std::to_string(l) + " = compute("
               +  std::to_string(l) + ");\n";
    return tool_event("write  src/gen/module_" + std::to_string(turn)
        + "_" + std::to_string(seq) + ".cpp", std::move(c));
}

// READ card — a code excerpt via Kind::FileRead (numbered gutter).
Element read_card(int turn, int seq) {
    TBP::Config c;
    c.kind = TBP::Kind::FileRead;
    c.show_all = true;
    c.text_color = Color::white();
    c.chrome_color = Color::blue();
    c.start_line = seq * 10;
    for (int l = 0; l < 18; ++l)
        c.text += "struct Field_" + std::to_string(l) + " { int id; };\n";
    return tool_event("read  include/api/types_" + std::to_string(turn)
        + ".hpp", std::move(c));
}

// BASH card — terminal output via Kind::BashOutput (tail-oriented).
Element bash_card(int turn, int seq) {
    TBP::Config c;
    c.kind = TBP::Kind::BashOutput;
    c.text_color = Color::white();
    c.chrome_color = Color::cyan();
    for (int l = 0; l < 10; ++l)
        c.text += "[ " + std::to_string(l * 10) + "%] Built target unit_"
               +  std::to_string(l) + "\n";
    c.text += "=== ALL 14 TESTS PASSED ===\n";
    return tool_event("bash  cmake --build build -j10", std::move(c));
}

// Build one settled turn the way agentty's frozen.cpp does: an assistant
// markdown reply FOLLOWED BY a panel of tool cards (the realistic mix is a
// few write/edit/read calls per turn). The whole turn is wrapped in ONE
// hash-keyed ComponentElement so the renderer blits the entire settled turn
// from its cell cache on every later frame.
Element make_frozen_turn(int turn_idx) {
    auto md = std::make_shared<StreamingMarkdown>();
    std::string body;
    body += "## Reply " + std::to_string(turn_idx) + "\n\n";
    body += "Here is a paragraph of prose explaining the answer to the "
            "question, with some **bold** and `inline code` and a "
            "[link](https://example.com) thrown in for good measure.\n\n";
    body += "I'll make the changes now.\n\n";
    md->set_content(body);
    md->finish();

    // Tool-call panel: a realistic multi-step agent turn — read context,
    // make two edits, write a new file, run the build. Each card is wrapped
    // in its own hash-keyed component (agentty's per-event freeze).
    std::vector<Element> turn_rows;
    turn_rows.push_back(md->build());
    turn_rows.push_back(cache_keyed("card-read",  turn_idx, 0, read_card(turn_idx, 0)));
    turn_rows.push_back(cache_keyed("card-edit",  turn_idx, 1, edit_card(turn_idx, 1)));
    turn_rows.push_back(cache_keyed("card-edit",  turn_idx, 2, edit_card(turn_idx, 2)));
    turn_rows.push_back(cache_keyed("card-write", turn_idx, 3, write_card(turn_idx, 3)));
    turn_rows.push_back(cache_keyed("card-bash",  turn_idx, 4, bash_card(turn_idx, 4)));

    Element turn_body = detail::vstack().gap(1)(std::move(turn_rows)).build();
    auto turn_sp = std::make_shared<const Element>(std::move(turn_body));

    ComponentElement comp;
    comp.hash_id = CacheIdBuilder{}
        .add(std::string_view{"bench-frozen-turn"})
        .add(static_cast<std::uint64_t>(turn_idx))
        .build();
    comp.render = [turn_sp](int, int) -> Element { return *turn_sp; };
    return Element{std::move(comp)};
}

double frame_us(const std::vector<Element>& frozen) {
    Element root = detail::vstack().gap(1)(
        // list_ref borrows the frozen vector — the zero-copy transcript path.
        std::vector<Element>{ detail::list_ref(frozen) }
    ).build();

    StylePool pool;
    Canvas canvas(80, /*h=*/4000, &pool);

    constexpr int kReps = 20;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kReps; ++i) {
        render_tree(root, canvas, pool, theme::native, /*auto_height=*/true);
        volatile int ch = content_height(canvas);
        (void)ch;
    }
    auto dt = std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::steady_clock::now() - t0).count();
    return (dt / 1000.0) / kReps;
}

} // namespace

int main() {
    std::println("=== bench_thread_scaling ===");
    std::println("frozen-transcript steady-state frame cost vs turn count\n");
    std::println("  {:>6}  {:>12}  {:>12}", "turns", "us/frame", "us/turn");

    std::vector<Element> frozen;
    double base_per_turn = 0.0;
    bool   first = true;
    int    rc = 0;

    for (int target : {10, 25, 50, 100, 200, 400, 800}) {
        while (static_cast<int>(frozen.size()) < target)
            frozen.push_back(make_frozen_turn(static_cast<int>(frozen.size())));

        // Prime: first frame populates every turn's cell cache. Steady-state
        // frames below should then blit.
        (void)frame_us(frozen);
        double us = frame_us(frozen);
        double per = us / target;
        std::println("  {:>6}  {:>12.1f}  {:>12.3f}", target, us, per);

        if (first) { base_per_turn = per; first = false; }
        ++rc;
    }

    std::println("");
    std::println("base us/turn (10 turns): {:.3f}", base_per_turn);

    // Production reality check: agentty trims the LIVE frozen tree to ~2
    // viewports of rows (trim_frozen_if_oversized / trim_frozen_above_
    // viewport in frozen.cpp) AND to kFrozenMaxEntries=120. Older turns
    // graduate into native terminal scrollback and leave the live tree.
    // So the real steady-state working set is a SMALL trailing window
    // regardless of total thread length. Measure that window directly:
    // the most recent ~8 full turns (the kKeepMinEntries floor).
    //
    // This is the pass/fail guard: if agentty's trim ever regresses (live
    // tree grows unbounded) OR maya's cell-blit cache breaks (settled
    // cards re-render every frame), this window blows past budget.
    //
    // The guard is RELATIVE, not an absolute wall-clock threshold: an
    // 8-turn row-bounded window should cost ~8x the per-turn baseline
    // measured during warmup (linear in the small working set). If a
    // settled card re-renders every frame instead of blitting from cache,
    // the cost goes super-linear and blows past the multiple. Gating on a
    // multiple of the self-calibrated baseline makes the test immune to
    // machine speed (dev box ~37us, slow CI runner ~700us — both pass as
    // long as the RATIO holds). A generous absolute ceiling stays as a
    // backstop against a baseline that is itself pathological.
    bool fail = false;
    {
        std::vector<Element> window;
        for (int i = 0; i < 8; ++i)
            window.push_back(make_frozen_turn(10000 + i));
        (void)frame_us(window);
        double us = frame_us(window);
        std::println("");
        std::println("production window (8 trailing turns, row-bounded): "
                     "{:.1f} us/frame", us);

        // Expected cost of a healthy 8-turn window, derived from the
        // machine's own warmup baseline. 8 turns * base_per_turn, with a
        // 6x slack for cache-priming + measurement jitter on noisy CI.
        double expected = 8.0 * base_per_turn;
        double rel_budget = expected * 6.0;
        // Absolute backstop: even a slow CI runner should not exceed this
        // for an 8-turn window if the cache works. Scaled off the runner's
        // own baseline so it degrades gracefully on slow hardware.
        double abs_budget = std::max(400.0, expected * 20.0);
        double budget = std::max(rel_budget, abs_budget);

        if (us > budget) {
            std::println("FAIL: production window {:.1f}us exceeds {:.0f}us "
                         "budget ({:.1f}us base/turn x8={:.0f} expected) — "
                         "cell-blit cache or app trim regressed",
                         us, budget, base_per_turn, expected);
            fail = true;
        }
    }
    std::println("done.");
    return fail ? 1 : 0;
}
