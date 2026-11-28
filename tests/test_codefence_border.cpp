// Reproduce: code-fence right border inside Turn rail, multi-frame
// streaming-then-settled path with the per-instance prefix
// ComponentElement hash_id active.
//
// What we're targeting: in the live moha session, code-fence cards
// inside a settled assistant message render with the RIGHT BORDER
// COLUMN missing (visible gap on the right side of the box). The
// static one-shot render is correct (see probe_codeborder), so the
// failure path must involve the StreamingMarkdown commit ⇒ prefix
// ComponentElement (hash_id "strmd-prefix:…") rebuild cycle, the
// cross-frame component_cache cells capture, or the inline-frame
// native-scrollback path that the renderer uses for fully-rendered
// output.

#include <maya/maya.hpp>
#include <maya/widget/agentty/markdown.hpp>
#include <maya/widget/agentty/turn.hpp>
#include <cassert>
#include <print>
#include <string>
#include <vector>

using namespace maya;

static char32_t cell_ch(const Canvas& c, int x, int y) {
    return c.get(x, y).character;
}

// Locate the topmost row that opens a code fence (the ╭ character),
// then scan rows down until ╰ at the same column. Returns
// {top, bot, left, right} of the box. {-1,-1,-1,-1} if not found.
struct BoxRect { int top, bot, left, right; };

static BoxRect find_codefence(const Canvas& c, int W, int H) {
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            // ╭ = U+256D
            if (cell_ch(c, x, y) == U'\u256D') {
                int left = x;
                // Find ╮ at U+256E on same row
                int right = -1;
                for (int xr = x + 1; xr < W; ++xr) {
                    if (cell_ch(c, xr, y) == U'\u256E') { right = xr; break; }
                }
                if (right < 0) continue;
                // Find ╰ at U+2570 below
                int bot = -1;
                for (int yb = y + 1; yb < H; ++yb) {
                    if (cell_ch(c, left, yb) == U'\u2570') { bot = yb; break; }
                }
                if (bot < 0) continue;
                return {y, bot, left, right};
            }
        }
    }
    return {-1, -1, -1, -1};
}

static void assert_box_intact(const Canvas& c, const BoxRect& b,
                              const char* ctx) {
    // top row should be: ╭ ── … ── ╮ (with optional " cpp " label)
    // bottom row: ╰ ── … ── ╯
    // middle rows: │ … │
    int fail = 0;
    auto report = [&](int x, int y, char32_t got, const char* expected) {
        std::println("  FAIL [{}]: ({},{}) U+{:04X} expected {}",
                     ctx, x, y, static_cast<uint32_t>(got), expected);
        ++fail;
    };
    if (cell_ch(c, b.left, b.top) != U'\u256D')
        report(b.left, b.top, cell_ch(c, b.left, b.top), "╭ U+256D");
    if (cell_ch(c, b.right, b.top) != U'\u256E')
        report(b.right, b.top, cell_ch(c, b.right, b.top), "╮ U+256E");
    if (cell_ch(c, b.left, b.bot) != U'\u2570')
        report(b.left, b.bot, cell_ch(c, b.left, b.bot), "╰ U+2570");
    if (cell_ch(c, b.right, b.bot) != U'\u256F')
        report(b.right, b.bot, cell_ch(c, b.right, b.bot), "╯ U+256F");
    for (int y = b.top + 1; y < b.bot; ++y) {
        if (cell_ch(c, b.left, y) != U'\u2502')
            report(b.left, y, cell_ch(c, b.left, y), "│ U+2502 (left)");
        if (cell_ch(c, b.right, y) != U'\u2502')
            report(b.right, y, cell_ch(c, b.right, y), "│ U+2502 (right)");
    }
    if (fail) {
        std::println("  context: box rows {}..{}, cols {}..{}",
                     b.top, b.bot, b.left, b.right);
        std::abort();
    }
}

static Element build_turn_with_md(StreamingMarkdown& md) {
    Turn::Config cfg{
        .glyph      = "*",
        .label      = "Sonnet",
        .rail_color = Color::blue(),
        .meta       = "12:34",
        .body       = {},
    };
    cfg.body.emplace_back(Turn::BodySlot{md.build()});
    return Turn{cfg}.build();
}

static void render_to(Canvas& canvas, StylePool& pool, const Element& root) {
    Theme theme{};
    canvas.clear();
    render_tree(root, canvas, pool, theme, /*auto_height=*/false);
}

// Drive a streaming sequence: feed bytes incrementally; render between.
// Reuse the SAME canvas + style pool across frames so we exercise the
// cross-frame component_cache that the real renderer uses.
static void test_codefence_right_border_streaming_to_settled() {
    std::println("--- test_codefence_right_border_streaming_to_settled ---");

    const int W = 80, H = 40;
    StylePool pool;
    Canvas canvas(W, H, &pool);

    auto md = std::make_shared<StreamingMarkdown>();

    // Stream in chunks. Pause and render between each.
    std::vector<std::string> chunks = {
        "**Coherence type-state** in the Runtime ",
        "(`app.hpp`):\n\n",
        "```cpp\n",
        "struct FullscreenSynced { Canvas front; };\n",
        "struct InlineSynced     { InlineFrameState state; };\n",
        "struct Divergent        {};   // no front buffer member at all\n",
        "```\n\n",
        "std::variant-encoded — Divergent physically lacks the front member.\n",
    };

    std::string acc;
    int frame = 0;
    for (auto& chunk : chunks) {
        acc += chunk;
        md->set_content(acc);
        Element root = build_turn_with_md(*md);
        render_to(canvas, pool, root);
        ++frame;
        // If a code fence is on canvas, assert its box is intact.
        auto box = find_codefence(canvas, W, H);
        if (box.top >= 0) {
            char ctx[64];
            std::snprintf(ctx, sizeof(ctx), "stream frame %d", frame);
            assert_box_intact(canvas, box, ctx);
        }
    }

    // Now settle: finish + render
    md->finish();
    Element root = build_turn_with_md(*md);
    render_to(canvas, pool, root);
    auto box = find_codefence(canvas, W, H);
    if (box.top < 0) {
        std::println("  FAIL settled: no code fence found");
        std::abort();
    }
    assert_box_intact(canvas, box, "settled");

    // Render again (idempotent, exercises the fast-path cache hit)
    render_to(canvas, pool, root);
    box = find_codefence(canvas, W, H);
    assert_box_intact(canvas, box, "settled-again");

    std::println("PASS");
}

int main() {
    test_codefence_right_border_streaming_to_settled();
    std::println("\n=== codefence right-border test passed ===");
    return 0;
}
