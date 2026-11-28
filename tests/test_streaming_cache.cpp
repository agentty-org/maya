// test_streaming_cache.cpp — invariants on StreamingMarkdown's per-frame
// cache shape. These are not parser / layout / rendering tests; they pin
// the relationship between (live_, source_, committed_, build_dirty_) and
// what build() returns, which is where the "ghost empty box at end of
// turn" / "prose rendered as code" / "stuck cached tree after settle"
// class of bugs lived.
//
// Pattern: each test asserts a SHAPE invariant on the Element tree
// returned by build(). Shape = (top-level child count, kind of each
// child). We don't render — these are pure invariants on the tree the
// renderer consumes, and they fail fast in a few ms each.
//
// Two of the tests below are the unit repros for the recent bugs:
//   - settle_drops_empty_tail_slot  (the "stray empty bordered box")
//   - byte_by_byte_settle_matches_full_feed (cache-shape regression sweep)

#include "maya/widget/agentty/markdown.hpp"
#include "maya/element/element.hpp"
#include "maya/element/text.hpp"   // string_width
#include "check.hpp"

#include <chrono>
#include <cstdio>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

using namespace maya;

// ── shape introspection ────────────────────────────────────────────────────

// Top-level shape: a description of cached_build_'s outer BoxElement's
// children — enough to catch every cache-shape regression observed so far.
struct Shape {
    std::vector<std::string> kinds;   // one entry per top-level child
};

static std::string element_kind(const Element& e) {
    if (std::get_if<TextElement>(&e.inner))      return "Text";
    if (std::get_if<BoxElement>(&e.inner))       return "Box";
    if (std::get_if<ComponentElement>(&e.inner)) return "Component";
    return "Other";
}

static Shape top_shape(const Element& root) {
    Shape s;
    if (auto* b = std::get_if<BoxElement>(&root.inner)) {
        for (auto& c : b->children) s.kinds.push_back(element_kind(c));
    } else {
        s.kinds.push_back(element_kind(root));
    }
    return s;
}

static std::string shape_str(const Shape& s) {
    std::string out = "[";
    for (std::size_t i = 0; i < s.kinds.size(); ++i) {
        if (i) out += ",";
        out += s.kinds[i];
    }
    out += "]";
    return out;
}

// True if the trailing child of `root` is an empty TextElement. That's
// the failure mode the recent screenshot bug printed as an empty
// bordered box at the end of the message.
static bool has_empty_trailing_text(const Element& root) {
    auto* b = std::get_if<BoxElement>(&root.inner);
    if (!b || b->children.empty()) return false;
    auto& last = b->children.back();
    auto* t = std::get_if<TextElement>(&last.inner);
    return t && t->content.empty();
}

// Recursive: does `e`'s subtree contain ANY visible bytes? "Visible" =
// at least one TextElement with non-empty content somewhere below. Used
// to detect empty bordered boxes — a Box with a border but no visible
// descendant renders as a stray empty rectangle (the screenshot bug
// class: trailing-slot AND empty-fence both manifest this way).
static bool has_visible_content(const Element& e) {
    if (auto* t = std::get_if<TextElement>(&e.inner)) {
        return !t->content.empty();
    }
    if (auto* b = std::get_if<BoxElement>(&e.inner)) {
        for (auto& c : b->children) if (has_visible_content(c)) return true;
        return false;
    }
    if (auto* c = std::get_if<ComponentElement>(&e.inner)) {
        if (!c->render) return false;
        Element m = c->render(80, 80);
        return has_visible_content(m);
    }
    if (auto* l = std::get_if<ElementList>(&e.inner)) {
        for (auto& it : l->items) if (has_visible_content(it)) return true;
        return false;
    }
    return false;
}

// Render-equivalent of an empty bordered box: a Box with a non-None
// border style whose ENTIRE subtree contains zero visible characters.
// Walks the whole tree (descending through Components too) and returns
// a short description of the first offender, or empty string if clean.
static std::string find_empty_bordered_box(const Element& e, std::string path = "") {
    if (auto* b = std::get_if<BoxElement>(&e.inner)) {
        const bool bordered = (b->border.style != BorderStyle::None);
        if (bordered && !has_visible_content(e)) {
            return path + "Box(border)";
        }
        for (std::size_t i = 0; i < b->children.size(); ++i) {
            auto r = find_empty_bordered_box(
                b->children[i],
                path + "Box[" + std::to_string(i) + "]/");
            if (!r.empty()) return r;
        }
        return {};
    }
    if (auto* c = std::get_if<ComponentElement>(&e.inner)) {
        if (!c->render) return {};
        Element m = c->render(80, 80);
        return find_empty_bordered_box(m, path + "Component/");
    }
    if (auto* l = std::get_if<ElementList>(&e.inner)) {
        for (std::size_t i = 0; i < l->items.size(); ++i) {
            auto r = find_empty_bordered_box(
                l->items[i],
                path + "List[" + std::to_string(i) + "]/");
            if (!r.empty()) return r;
        }
    }
    return {};
}

// ── harness ────────────────────────────────────────────────────────────────

static int g_passed = 0;
static int g_failed = 0;

template <typename F>
static void run(const char* name, F&& fn) {
    using namespace std::chrono;
    auto t0 = steady_clock::now();
    try {
        fn();
        auto ms = duration_cast<milliseconds>(steady_clock::now() - t0).count();
        std::println("  {:<54} ok    [{} ms]", name, ms);
        ++g_passed;
    } catch (const std::exception& e) {
        ++g_failed;
        std::println("  {:<54} FAIL", name);
        std::println("      {}", e.what());
    }
}

#define REQUIRE(cond, msg) do { \
    if (!(cond)) throw std::runtime_error(std::string("REQUIRE(") + #cond + "): " + (msg)); \
} while (0)

// ── tests ──────────────────────────────────────────────────────────────────

// T1. The exact bug from the screenshot. Live streaming completed; host
// called set_live(false) BEFORE finish() (mirrors finalize-ramp completion
// path). build() must NOT return a tree with a dangling empty TextElement
// — that renders as an empty bordered box at the end of the turn.
static void settle_drops_empty_tail_slot() {
    std::string src =
"or stream live:\n\n"
"```\n"
"gh run watch 27098608184\n"
"```\n";

    StreamingMarkdown md;
    md.set_live(true);
    md.set_content(src);
    auto& b1 = md.build();
    REQUIRE(!top_shape(b1).kinds.empty(), "live build produced empty tree");

    // The bug: dropping live_ alone (without finish) used to leave the
    // empty Text[0] reserved-tail slot in cached_build_. set_live() must
    // bump build_dirty_ so the next build rebuilds with the correct shape.
    md.set_live(false);
    auto& b2 = md.build();
    if (has_empty_trailing_text(b2)) {
        throw std::runtime_error(
            "set_live(false) left an empty trailing TextElement in cached_build_ — "
            "renders as a stray empty bordered box at the end of the turn. "
            "Shape: " + shape_str(top_shape(b2)));
    }
}

// T2. The build cache must NOT serve a stale tree across a live_ transition.
// After set_live(false) or finish(), build() must NOT return a tree with an
// empty trailing Text — that's the screenshot bug. We DON'T check the
// mid-live frame: build.cpp intentionally reserves an empty tail slot while
// live_ is true (so the inter-block gap doesn't bounce as commit_range walks
// past a block boundary). The invariant is post-transition, not mid-stream.
static void no_stale_tree_across_live_transitions() {
    auto fixture = [](const std::string& src) {
        struct Frame { const char* op; bool empty_tail; std::string shape; };
        std::vector<Frame> frames;
        StreamingMarkdown md;
        md.set_live(true);
        md.set_content(src);
        (void)md.build();           // live build — invariant doesn't apply yet
        md.set_live(false);
        {
            auto& b = md.build();
            frames.push_back({"set_live(false)", has_empty_trailing_text(b), shape_str(top_shape(b))});
        }
        md.finish();
        {
            auto& b = md.build();
            frames.push_back({"finish()", has_empty_trailing_text(b), shape_str(top_shape(b))});
        }
        return frames;
    };

    // Sources that all end on a closing structural boundary (the "no
    // trailing prose" case where the empty tail bug previously appeared).
    const std::vector<std::string> sources = {
        "plain paragraph only\n",
        "two\n\nparagraphs\n",
        "```\nfenced\n```\n",
        "intro\n\n```\nfenced\n```\n",
        "# heading\n\n```\nblock\n```\n\nafter\n",
        "- a\n- b\n- c\n",
        "| a | b |\n|---|---|\n| 1 | 2 |\n",
    };
    for (const auto& src : sources) {
        auto frames = fixture(src);
        for (const auto& f : frames) {
            if (f.empty_tail) {
                std::string preview = src.size() > 40 ? src.substr(0, 40) + "..." : src;
                for (auto& c : preview) if (c == '\n') c = '|';
                throw std::runtime_error(
                    std::string("empty trailing Text after ") + f.op
                    + " on source '" + preview + "': shape=" + f.shape);
            }
        }
    }
}

// T3. The invariant that catches the WHOLE class structurally: feeding
// `source` all at once and feeding it byte-by-byte must produce the same
// settled Element tree shape after set_live(false) + finish(). Any
// incremental-state leak into the cache shows up here.
static void byte_by_byte_settle_matches_full_feed() {
    const std::vector<std::string> sources = {
        "hello\n",
        "para one\n\npara two\n",
        "```cpp\nint main() { return 0; }\n```\n",
        "intro\n\n```\nx\n```\n\nouttro\n",
        "# heading\n\n- one\n- two\n\n> quote\n",
        "| a | b |\n|---|---|\n| 1 | 2 |\n\nafter\n",
    };
    for (const auto& src : sources) {
        // Reference: full feed.
        StreamingMarkdown ref;
        ref.set_content(src);
        ref.finish();
        auto ref_shape = top_shape(ref.build());

        // Incremental: feed one byte at a time, live the whole time, then
        // set_live(false) + finish() at the end (mirrors agentty's path).
        StreamingMarkdown inc;
        inc.set_live(true);
        for (char c : src) inc.append(std::string_view(&c, 1));
        inc.set_live(false);
        inc.finish();
        auto inc_shape = top_shape(inc.build());

        if (ref_shape.kinds != inc_shape.kinds) {
            std::string preview = src.size() > 40 ? src.substr(0, 40) + "..." : src;
            for (auto& c : preview) if (c == '\n') c = '|';
            throw std::runtime_error(
                std::string("byte-by-byte settled shape diverges from full-feed shape on '")
                + preview + "': ref=" + shape_str(ref_shape)
                + " inc=" + shape_str(inc_shape));
        }
    }
}

// T4. block_count() must equal the parser's view at all times, INDEPENDENT
// OF FEED CHUNKING. If the resumable boundary scanner is sensitive to
// where chunk boundaries land vs. block boundaries, different chunk sizes
// produce different block counts — a real parser bug that corrupts the
// rendered tree (very likely the "prose rendered as code block" /
// "empty bordered box" symptoms).
//
// KNOWN OPEN: chunk-size-dependent boundary scanning currently fails this
// for several chunk sizes on the source below. Pinned baseline keeps the
// bug RECORDED (and visible in the test output) without blocking CI.
// Lower the baseline as the scanner stabilises; never raise.
static void block_count_consistent_with_finish() {
    const std::string src =
"Try it:\n\n"
"```\n"
"cmd1\n"
"```\n\n"
"Three outcomes:\n\n"
"1. one\n"
"2. two\n"
"3. three\n\n"
"If hit:\n\n"
"```\n"
"awk something\n"
"```\n\n"
"trailing prose.\n";

    StreamingMarkdown ref;
    ref.set_content(src);
    ref.finish();
    const std::size_t ref_blocks = ref.block_count();

    // Sweep a wide range of chunk sizes. Record disagreements; fail if
    // ANY chunk size produces a block count that differs from the
    // whole-buffer reference. The resumable boundary scanner used to
    // drift on chunks 7/13/17 (baseline was 3); that's since been fixed
    // and verified drift-free across an exhaustive chunk-size sweep, so
    // the guard is now tight at 0 to lock the fix in — never raise it.
    constexpr std::size_t kBaselineDrift = 0;
    std::size_t drifts = 0;
    std::string detail;
    for (std::size_t chunk : {std::size_t{1}, std::size_t{3}, std::size_t{7},
                              std::size_t{13}, std::size_t{17}, std::size_t{29},
                              std::size_t{50}, std::size_t{100}}) {
        StreamingMarkdown inc;
        inc.set_live(true);
        for (std::size_t i = 0; i < src.size(); i += chunk) {
            inc.append(std::string_view(src).substr(i, std::min(chunk, src.size() - i)));
        }
        inc.set_live(false);
        inc.finish();
        if (inc.block_count() != ref_blocks) {
            ++drifts;
            detail += " chunk=" + std::to_string(chunk)
                   +  "→" + std::to_string(inc.block_count());
        }
    }
    if (drifts > kBaselineDrift) {
        throw std::runtime_error(
            "block_count chunk-size drift REGRESSED: " + std::to_string(drifts)
            + " sizes diverge from ref=" + std::to_string(ref_blocks)
            + " (baseline allowed: " + std::to_string(kBaselineDrift) + ")."
            + detail
            + " — the resumable boundary scanner is producing chunk-dependent"
            " output, which corrupts the rendered tree.");
    }
}

// T5. clear() must drop the empty-tail invariant too. A widget that's
// been cleared and not yet fed should not return a tree with an empty
// trailing Text masquerading as content.
static void clear_resets_cache_shape() {
    StreamingMarkdown md;
    md.set_live(true);
    md.set_content("some content\n");
    (void)md.build();

    md.clear();
    auto& b = md.build();
    if (has_empty_trailing_text(b)) {
        // Empty-content widget should render as either an empty TextElement
        // OR an empty Box — never a Box containing only an empty Text.
        auto sh = top_shape(b);
        if (sh.kinds.size() == 1 && sh.kinds[0] == "Text") {
            // OK: lone empty TextElement is the canonical empty form.
        } else {
            throw std::runtime_error(
                "clear() left a stale shape with empty trailing Text: "
                + shape_str(sh));
        }
    }
}

// T6. Calling build() repeatedly without mutation must return the same
// shape every time (no per-frame drift, no animation-state leaking into
// the structural tree).
static void idle_build_stable() {
    StreamingMarkdown md;
    md.set_content("para one\n\npara two\n");
    md.finish();

    auto shape0 = top_shape(md.build());
    for (int i = 0; i < 50; ++i) {
        auto shape_i = top_shape(md.build());
        if (shape_i.kinds != shape0.kinds) {
            throw std::runtime_error(
                "idle build() drifted at iteration " + std::to_string(i)
                + ": baseline=" + shape_str(shape0)
                + " now=" + shape_str(shape_i));
        }
    }
}

// T7. request_finalize() must not corrupt cache shape. After a finalize
// ramp completes (cursor reaches edge → widget auto-flips live_=false +
// build_dirty_=true), the next build's shape must match what finish()
// would produce.
static void finalize_ramp_completion_matches_finish() {
    const std::string src = "intro\n\n```\nblock\n```\n";

    // Reference: live → finish path.
    StreamingMarkdown ref;
    ref.set_live(true);
    ref.set_content(src);
    (void)ref.build();
    ref.set_live(false);
    ref.finish();
    auto ref_shape = top_shape(ref.build());

    // Subject: live → request_finalize(0) (instant ramp) → build builds the
    // ramp completes inside it on the next call → finish.
    StreamingMarkdown sub;
    sub.set_live(true);
    sub.set_reveal_fx(true);   // ramp only runs with reveal_fx on
    sub.set_content(src);
    (void)sub.build();
    sub.request_finalize(0);   // instant deadline
    // Two builds: first advances the cursor to edge and flips live_=false,
    // second sees live_=false and returns settled cached_build_.
    (void)sub.build();
    auto sub_shape_after_ramp = top_shape(sub.build());
    sub.finish();
    auto sub_shape_after_finish = top_shape(sub.build());

    if (sub_shape_after_finish.kinds != ref_shape.kinds) {
        throw std::runtime_error(
            "post-finalize-ramp finish() shape differs from ref: ramp+finish="
            + shape_str(sub_shape_after_finish) + " ref=" + shape_str(ref_shape));
    }
    // Post-ramp (pre-finish) shape MUST already be free of empty trailing Text —
    // that was the exact bug. We don't require it to match ref_shape
    // structurally (finish may add/coalesce), but it must not have the
    // stray empty bordered box.
    if (sub_shape_after_ramp.kinds.size() > 0
        && sub_shape_after_ramp.kinds.back() == "Text")
    {
        // Verify it's not the empty-Text bug specifically.
        if (has_empty_trailing_text(sub.build())) {
            throw std::runtime_error(
                "ramp completion left empty trailing Text: "
                + shape_str(sub_shape_after_ramp));
        }
    }
}

// T8. The exact regression source from the screenshot — repro embedded as
// a permanent test. Feed it through every realistic path (full, live+settle,
// live+ramp+settle, byte-by-byte) and assert no empty bordered box ever
// appears.
static void screenshot_repro_no_ghost_box() {
    const std::string src =
"Running. `prepare release` already completed (12s — created/refreshed the v0.2.0 release + uploaded source tarball). All six build jobs are in flight in parallel.\n\n"
"Expected timing based on the last run (47m14s total):\n"
"- linux x86_64, i686, macos, windows → ~5-15 min each\n"
"- linux aarch64 (QEMU emulation) → ~13 min, usually the longest pole\n\n"
"Check again in ~5 min:\n\n"
"```\n"
"gh run view 27098608184\n"
"```\n\n"
"or stream live:\n\n"
"```\n"
"gh run watch 27098608184\n"
"```\n";

    auto check = [&](const char* path_name, const Element& el) {
        if (has_empty_trailing_text(el)) {
            throw std::runtime_error(
                std::string("screenshot repro: empty trailing Text on path '") + path_name
                + "': shape=" + shape_str(top_shape(el)));
        }
    };

    // Path A: full feed + finish.
    {
        StreamingMarkdown md;
        md.set_content(src);
        md.finish();
        check("full+finish", md.build());
    }
    // Path B: live → set_live(false) (no finish).
    {
        StreamingMarkdown md;
        md.set_live(true);
        md.set_content(src);
        (void)md.build();
        md.set_live(false);
        check("live→!live", md.build());
    }
    // Path C: live → finish (no explicit set_live(false)).
    {
        StreamingMarkdown md;
        md.set_live(true);
        md.set_content(src);
        (void)md.build();
        md.finish();
        check("live→finish", md.build());
    }
    // Path D: byte-by-byte live, then settle.
    {
        StreamingMarkdown md;
        md.set_live(true);
        for (char c : src) md.append(std::string_view(&c, 1));
        (void)md.build();
        md.set_live(false);
        md.finish();
        check("by1+settle", md.build());
    }
    // Path E: finalize ramp (instant) + finish.
    {
        StreamingMarkdown md;
        md.set_live(true);
        md.set_reveal_fx(true);
        md.set_content(src);
        (void)md.build();
        md.request_finalize(0);
        (void)md.build();
        (void)md.build();
        md.finish();
        check("ramp+finish", md.build());
    }
}

// T9. The reveal-fx animation (scramble/gradient/caret) must reach the
// live tail even when it is rendered as an eager block — table, list,
// blockquote, code block. Earlier the find_last_text helper bailed at the
// first ComponentElement on the rightmost spine with NO fallback; result:
// a streaming table got NO animated edge at all ("animation doesn't show
// when things like tables are streaming").
//
// The CURRENT production contract (reveal_fx.cpp) is two-armed:
//   • flat tails expose a non-empty TextElement on the rightmost spine
//     (production find_last_text decorates it in place);
//   • eager tails surface a ComponentElement on that spine — production
//     does NOT materialize it at descent time (render(0,0) yields a
//     width-0 layout: one-cell tables, one-glyph-per-line code — the
//     char-chopped-table bug), it wraps the component's render() and
//     animates the materialised rows at paint time (eager_render branch).
// So the invariant this test pins is: for every shape, the rightmost
// spine reaches EITHER a non-empty TextElement (walking through Boxes
// only) OR a ComponentElement. Reaching neither — an empty leaf / bare
// dead end — means reveal_fx has nothing to animate: a frozen tail.
static void rightmost_leaf_reachable_in_eager_renders() {
    // Walk the rightmost spine through Boxes ONLY — mirroring production
    // find_last_text, which never materializes a ComponentElement during
    // descent. A ComponentElement terminates the walk as SUCCESS (the
    // eager overlay branch animates it); a non-empty TextElement is the
    // flat-tail success; anything else is the frozen-tail failure.
    auto tail_is_animatable = [](const Element& root) -> bool {
        const Element* cur = &root;
        for (int depth = 0; depth < 64; ++depth) {
            if (auto* t = std::get_if<TextElement>(&cur->inner))
                return !t->content.empty();
            if (auto* b = std::get_if<BoxElement>(&cur->inner)) {
                if (b->children.empty()) return false;
                cur = &b->children.back();
                continue;
            }
            if (std::holds_alternative<ComponentElement>(cur->inner))
                return true;   // eager tail: overlay wraps its render()
            return false;
        }
        return false;  // depth exceeded
    };

    struct Case { const char* name; const char* src; };
    Case cases[] = {
        {"table",
         "Stats:\n\n"
         "| Col A | Col B | Col C |\n"
         "|-------|-------|-------|\n"
         "| row1a | row1b | row1c |\n"
         "| row2a | row2b | row2"},   // mid-cell
        {"list",
         "Items:\n\n"
         "- alpha\n"
         "- beta\n"
         "- gamma\n"
         "- delt"},                    // mid-item
        {"blockquote",
         "> first quoted line\n"
         "> second quoted line\n"
         "> third quoted lin"},        // mid-line
        {"code block",
         "```c\n"
         "int main() {\n"
         "    return 0"},              // open fence
        {"paragraph",
         "This is some prose that streams in over multiple lines and is"
         " the inline-fallback shape \u2014 reveal_fx already worked here"
         " before the eager fix."},
    };

    for (auto& c : cases) {
        StreamingMarkdown md;
        md.set_live(true);
        // Do NOT enable reveal_fx here — we want to inspect the
        // underlying cached_build_ tree (the input the overlay walks),
        // not the overlay's per-frame mutated output. With reveal_fx
        // off, build() returns cached_build_ unmodified.
        md.set_content(c.src);
        Element snapshot = md.build();
        if (!tail_is_animatable(snapshot)) {
            throw std::runtime_error(
                std::string("no animatable tail (text leaf or eager "
                            "component) reachable for shape '")
                + c.name + "' — reveal_fx has nothing to animate. Source: "
                + c.src);
        }
    }
}

// T10. Structural invariant across the WHOLE rendered tree: no bordered
// Box may have a subtree containing zero visible characters. This is the
// general form of the screenshot bug class — a bordered Box with no
// visible descendant renders as an empty rectangle. Previous tests only
// checked the top-level last-child; this scanner descends through every
// Box, ComponentElement (by calling render()), and ElementList. Catches:
//   • the trailing-slot bug (build.cpp's reserved empty Text after settle)
//   • the empty-fence bug (```\n``` rendering a bordered empty box)
//   • any future cache/parse path that emits a bordered empty Box.
static void no_empty_bordered_boxes_anywhere() {
    struct Case { const char* name; const char* src; };
    Case cases[] = {
        // The empty-fence cases (the new screenshot bug).
        {"empty fence",            "```\n```"},
        {"empty tagged fence",     "```sh\n```"},
        {"empty fence with blank", "```\n\n```"},
        // Sandwich shapes: prose, empty fence, more prose. Matches the
        // exact pattern the user's screenshot circled — stray empty
        // boxes between paragraphs.
        {"prose / empty fence / prose",
         "before paragraph\n\n```\n```\n\nafter paragraph"},
        {"two empty fences",
         "alpha\n\n```\n```\n\n```\n```\n\nomega"},
        // Regular non-empty fences must NOT trip the scanner (no false
        // positives — they have visible content inside the border).
        {"normal fence",           "```\nint main() { return 0; }\n```"},
        {"normal tagged fence",    "```cpp\nint main() { return 0; }\n```"},
        // Mixed: empty fence between two real ones.
        {"real / empty / real",
         "```\nfirst\n```\n\n```\n```\n\n```\nthird\n```"},
        // Streaming-shaped cases that the existing tests cover.
        {"streaming code",
         "or stream live:\n\n```\ngh run watch 27098608184\n```\n"},
    };

    for (auto& c : cases) {
        // Static parse path (settled message rendering).
        Element settled = markdown(c.src);
        if (auto where = find_empty_bordered_box(settled); !where.empty()) {
            throw std::runtime_error(
                std::string("empty bordered box in STATIC parse for '")
                + c.name + "' at " + where + "; source: " + c.src);
        }
        // Streaming + finish path (assistant turn render).
        StreamingMarkdown md;
        md.set_content(c.src);
        md.finish();
        Element streamed = md.build();
        if (auto where = find_empty_bordered_box(streamed); !where.empty()) {
            throw std::runtime_error(
                std::string("empty bordered box in STREAMING parse for '")
                + c.name + "' at " + where + "; source: " + c.src);
        }
    }
}

// Walk the tree (descending through Components and Boxes) and concatenate
// every TextElement's content. Used to assert which text DOES appear.
static std::string concat_text(const Element& e) {
    if (auto* t = std::get_if<TextElement>(&e.inner)) return t->content;
    if (auto* b = std::get_if<BoxElement>(&e.inner)) {
        std::string out;
        for (auto& c : b->children) out += concat_text(c);
        return out;
    }
    if (auto* c = std::get_if<ComponentElement>(&e.inner)) {
        if (!c->render) return {};
        return concat_text(c->render(80, 80));
    }
    if (auto* l = std::get_if<ElementList>(&e.inner)) {
        std::string out;
        for (auto& it : l->items) out += concat_text(it);
        return out;
    }
    return {};
}

// T11. Empty fences must render the dim one-row placeholder, not nothing
// and not a bordered box. Pins the EXACT rendering choice so a future
// "collapse to 0 rows" refactor (which would re-trip the streaming
// height-monotonicity test) doesn't accidentally land. Also pins that
// the lang label, when present, is included in the placeholder text.
static void empty_fence_renders_dim_placeholder() {
    struct Case { const char* name; const char* src; const char* must_contain; };
    Case cases[] = {
        {"plain empty fence",     "```\n```",     "empty code block"},
        {"tagged empty fence",    "```sh\n```",   "sh"},
        {"tagged empty (cpp)",    "```cpp\n```",  "cpp"},
    };
    for (auto& c : cases) {
        Element el = markdown(c.src);
        std::string text = concat_text(el);
        if (text.find(c.must_contain) == std::string::npos) {
            throw std::runtime_error(
                std::string("empty fence '") + c.name +
                "' did not render the placeholder; expected to find '" +
                c.must_contain + "' in rendered text. Got: '" + text + "'");
        }
    }
}

// T12. Streaming table column-width FLOOR (md::Table::min_col_widths). The
// live StreamingMarkdown reserves each column's final width from EVERY
// already-arrived row so a streaming table doesn't reflow horizontally as
// wider rows reveal. Here we pin the RENDERER half of that contract: a table
// carrying min_col_widths renders its columns at least that wide. (The
// streaming integration — burst-arrival then reveal stays one width — is
// proven by scratch/diag_table_floor; this keeps the renderer fold honest.)
//
// Materialize the table ComponentElement at a fixed width and take the widest
// TextElement on the tree (the box-border rows). With the floor set, that
// width must jump well past the natural (1-char-cell) render.
static int max_text_width(const Element& e) {
    if (auto* t = std::get_if<TextElement>(&e.inner))
        return string_width(t->content);
    if (auto* b = std::get_if<BoxElement>(&e.inner)) {
        int m = 0;
        for (auto& c : b->children) m = std::max(m, max_text_width(c));
        return m;
    }
    if (auto* c = std::get_if<ComponentElement>(&e.inner))
        return c->render ? max_text_width(c->render(80, 80)) : 0;
    if (auto* l = std::get_if<ElementList>(&e.inner)) {
        int m = 0;
        for (auto& it : l->items) m = std::max(m, max_text_width(it));
        return m;
    }
    return 0;
}

static void table_floor_widens_columns() {
    auto cell = [](const char* s) {
        return md::TableCell{
            std::vector<md::Inline>{ md::Inline{md::Text{std::string(s)}} } };
    };
    md::Table tbl;
    tbl.header.cells = { cell("a"), cell("b") };
    tbl.rows.push_back(
        md::TableRow{ std::vector<md::TableCell>{ cell("x"), cell("y") } });
    tbl.aligns = { md::TableAlign::Left, md::TableAlign::Left };

    const int nat_w = max_text_width(md_block_to_element(md::Block{tbl}));

    md::Table wide = tbl;
    wide.min_col_widths = { 18, 18 };   // reserve wide columns
    const int flo_w = max_text_width(md_block_to_element(md::Block{wide}));

    if (!(nat_w > 0)) throw std::runtime_error("natural table did not render");
    if (!(flo_w > nat_w)) throw std::runtime_error(
        "min_col_widths did not widen the table (flo_w="
        + std::to_string(flo_w) + " nat_w=" + std::to_string(nat_w) + ")");
    if (!(flo_w >= 2 * 18)) throw std::runtime_error(
        "floored table narrower than the reserved column widths (flo_w="
        + std::to_string(flo_w) + ")");

    // An EMPTY floor must be a no-op (static / committed tables): identical
    // render to the natural table.
    md::Table same = tbl;
    same.min_col_widths = {};
    if (max_text_width(md_block_to_element(md::Block{same})) != nat_w)
        throw std::runtime_error("empty min_col_widths changed the render");
}

// ── main ────────────────────────────────────────────────────────────────────────

int main() {
    std::println("=== test_streaming_cache ===");

    run("settle drops empty tail slot",            settle_drops_empty_tail_slot);
    run("no stale tree across live transitions",   no_stale_tree_across_live_transitions);
    run("byte-by-byte settle matches full feed",   byte_by_byte_settle_matches_full_feed);
    run("block_count consistent across chunkings", block_count_consistent_with_finish);
    run("clear resets cache shape",                clear_resets_cache_shape);
    run("idle build stable",                       idle_build_stable);
    run("finalize ramp completion matches finish", finalize_ramp_completion_matches_finish);
    run("screenshot repro no ghost box",           screenshot_repro_no_ghost_box);
    run("reveal-fx leaf reachable in eager renders", rightmost_leaf_reachable_in_eager_renders);
    run("no empty bordered boxes anywhere",        no_empty_bordered_boxes_anywhere);
    run("empty fence renders dim placeholder",     empty_fence_renders_dim_placeholder);
    run("streaming table column-width floor",      table_floor_widens_columns);

    std::println("\n── summary ────────────────────────────────────────────────────────");
    std::println("  passed: {}   failed: {}", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
