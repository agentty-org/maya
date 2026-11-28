// The markdown palette: one authored mapping, published as a value.
//
// Two defects lived here, and both were invisible to review:
//
//   1. DIVERGENCE. The theme-slot mapping was authored TWICE — the defaults
//      in markdown/internal.hpp and markdown_palette_from() in
//      render_block.cpp — and the two disagreed on NINE of thirty-seven
//      roles (heading1/2/3, table_header, highlight_fg, code_border,
//      hrule_fg, code_fg, mention_fg). Markdown rendered under mapping A
//      until the first theme swap and mapping B forever after, so headings,
//      code spans and links shifted colour on a swap that changed nothing
//      else — and swapping to the SAME theme still moved them.
//
//   2. A DATA RACE. The palette was ~35 plain mutable globals, written by
//      the UI thread and read by the DETACHED streaming parse worker across
//      ~165 sites. The header asked callers to only re-theme while nothing
//      was streaming, which the one caller that matters (a live theme
//      preview) cannot honour.
//
// Both are now structural: MAYA_MD_PALETTE is the single statement of the
// mapping, and the palette is an immutable snapshot published by pointer
// swap. These cases pin that, including the concurrency shape.

#include "agtest.hpp"

#include <maya/style/schemes.hpp>
#include <maya/style/theme.hpp>
#include <maya/widget/agentty/markdown.hpp>
#include <maya/widget/agentty/markdown/internal.hpp>

#include <atomic>
#include <print>
#include <string_view>
#include <thread>
#include <vector>

using namespace maya;

namespace {

[[nodiscard]] const Theme* scheme_named(std::string_view want) {
    for (const auto& s : theme::schemes)
        if (std::string_view{s.name} == want) return s.theme;
    return nullptr;
}

// Count roles where two palettes disagree.
[[nodiscard]] int divergences(const colors::Palette& a, const colors::Palette& b) {
    int n = 0;
#define X(f, SLOT) if (!(a.f == b.f)) ++n;
    MAYA_MD_PALETTE(X)
#undef X
    return n;
}

// Count roles where two palettes PAINT differently under `t`.
//
// The internal palette is late-bound, so two palettes can hold different
// representations of the same colour: a slot and the literal that slot
// resolves to. For anything about what the user SEES, resolving first is the
// only comparison that means something.
[[nodiscard]] int painted_divergences(const colors::Palette& a,
                                      const colors::Palette& b,
                                      const Theme& t) {
    int n = 0;
#define X(f, SLOT) if (!(t.resolve(a.f) == t.resolve(b.f))) ++n;
    MAYA_MD_PALETTE(X)
#undef X
    return n;
}

}  // namespace

TEST_CASE("markdown palette: the boot palette IS the projection") {
    std::println("--- test_md_palette_no_divergence ---");
    // The regression, stated directly. Before the merge these differed in 9
    // of 37 roles, so the first theme swap repainted headings/code/links
    // even when it swapped to the theme already in force.
    theme::set_live(theme::native);
    const colors::Palette boot = colors::live();
    const colors::Palette proj = colors::project(theme::native);
    CHECK(divergences(boot, proj) == 0);
    std::println("PASS\n");
}

TEST_CASE("markdown palette: publishing lands exactly on the projection") {
    std::println("--- test_md_palette_publish ---");
    // A light scheme on purpose: polarity is what makes a stale palette
    // visible (dark ink on a light canvas), and it is the case a user hits.
    const Theme* rose = scheme_named("Rose Pine Dawn");
    REQUIRE(rose != nullptr);

    theme::set_live(*rose);
    set_markdown_palette(markdown_palette_from(*rose));
    // Compared by what it PAINTS, not by representation. The override path
    // hands back concrete colours (markdown_palette_from resolves against
    // the theme the caller named) while the default palette holds slots, so
    // the two are equal in effect and unequal in form. What has to hold is
    // that publishing a theme's own palette changes nothing on screen.
    CHECK(painted_divergences(colors::live(), colors::project(*rose), *rose) == 0);

    // Every built-in scheme round-trips through the public API onto its own
    // projection — the mapping cannot be partial or order-dependent.
    int checked = 0;
    for (const auto& s : theme::schemes) {
        theme::set_live(*s.theme);
        set_markdown_palette(markdown_palette_from(*s.theme));
        CHECK(painted_divergences(colors::live(),
                                  colors::project(*s.theme), *s.theme) == 0);
        ++checked;
    }
    CHECK(checked > 50);

    theme::set_live(theme::native);
    set_markdown_palette(markdown_palette_from(theme::native));
    std::println("PASS ({} schemes round-tripped)\n", checked);
}

TEST_CASE("markdown palette: every role is a LATE-BOUND slot") {
    std::println("--- test_md_palette_total ---");
    // The palette is symbolic: every role is a Color::slot resolved at PAINT
    // time, never a literal decided when the palette was projected.
    //
    // This used to assert the opposite — that no role is a slot — because
    // the palette held LitColor. That was the bug. md_block_to_element()
    // stores a fully RENDERED Element per committed block, so resolving here
    // baked the live palette into that block at COMMIT time and no later
    // theme switch could reach it: a settled transcript kept the outgoing
    // scheme while anything rebuilt per frame followed the new one.
    //
    // Slots cost the worker nothing — Color::slot() is a tag plus an index
    // and touches no Theme — so the old reason for resolving early
    // ("a parse worker has no theme") no longer applies.
    for (const auto& s : theme::schemes) {
        const colors::Palette p = colors::project(*s.theme);
#define X(f, SLOT) CHECK(p.f.kind() == ColorKind::Slot); \
                   CHECK(p.f.is_set());                  \
                   CHECK(p.f.theme_slot() == ThemeSlot::SLOT);
        MAYA_MD_PALETTE(X)
#undef X
    }
    std::println("PASS\n");
}

TEST_CASE("markdown palette: a slot resolves to the live theme's colour") {
    std::println("--- test_md_palette_resolves ---");
    // The other half of late binding: symbolic is only useful if resolving
    // it at paint yields the right colour. Same palette value, two themes,
    // two results — with nothing re-projected in between.
    const Theme* rose = scheme_named("Rose Pine Dawn");
    const Theme* drac = scheme_named("Dracula");
    REQUIRE(rose != nullptr);
    REQUIRE(drac != nullptr);

    const colors::Palette p = colors::project(theme::native);

    int differing = 0;
#define X(f, SLOT) if (!(rose->resolve(p.f) == drac->resolve(p.f))) ++differing;
    MAYA_MD_PALETTE(X)
#undef X
    // Two schemes this unalike must disagree on a good number of roles; if
    // they agreed everywhere the resolve would be ignoring the theme.
    CHECK(differing > 5);
    std::println("PASS ({} roles differ between schemes)\n", differing);
}

TEST_CASE("markdown palette: a theme change alone refreshes it") {
    std::println("--- test_md_palette_epoch_pull ---");
    // THE forgotten-subscriber regression.
    //
    // A projected palette has to be re-derived when the theme moves. That
    // used to be a push — on_theme_changed(fn), registered once per
    // subsystem — and markdown was the subsystem that forgot. The failure is
    // invisible by construction: a projection that never re-derives looks
    // exactly like a projection whose theme never changed, so it surfaced
    // only as a user reporting that picking a scheme repainted the chrome
    // while prose, code spans and tables kept the old colours.
    //
    // Deriving is the read path now, so this test registers NOTHING and
    // publishes NOTHING. It only changes the theme. If someone reintroduces
    // a push-based design, this fails.
    const Theme* rose = scheme_named("Rose Pine Dawn");
    const Theme* drac = scheme_named("Dracula");
    REQUIRE(rose != nullptr);
    REQUIRE(drac != nullptr);

    theme::set_live(*rose);
    CHECK(divergences(colors::live(), colors::project(*rose)) == 0);

    // A second change, to catch a palette that refreshes once and then
    // latches — which is what a stale epoch would do.
    theme::set_live(*drac);
    CHECK(divergences(colors::live(), colors::project(*drac)) == 0);

    // Every scheme, so no ordering or caching quirk hides.
    int checked = 0;
    for (const auto& s : theme::schemes) {
        theme::set_live(*s.theme);
        CHECK(divergences(colors::live(), colors::project(*s.theme)) == 0);
        ++checked;
    }
    CHECK(checked > 50);

    // The epoch is what makes it work, and it must actually move — a
    // pointer compare could not see this, because the live theme slot is
    // assigned THROUGH and its address never changes.
    const unsigned before = theme::live_epoch();
    theme::set_live(theme::native);
    CHECK(theme::live_epoch() > before);
    CHECK(divergences(colors::live(), colors::project(theme::native)) == 0);
    std::println("PASS ({} schemes tracked with no subscriber)\n", checked);
}

TEST_CASE("markdown palette: an explicit override stands, then yields") {
    std::println("--- test_md_palette_override ---");
    // set_markdown_palette() is the host escape hatch: an explicit palette
    // rather than a derived one. It has to WIN over the projection...
    const Theme* rose = scheme_named("Rose Pine Dawn");
    REQUIRE(rose != nullptr);
    theme::set_live(*rose);

    MarkdownPalette custom = markdown_palette_from(*rose);
    custom.heading1 = Color::rgb(1, 2, 3);
    set_markdown_palette(custom);
    CHECK(colors::live().heading1 == Color::rgb(1, 2, 3));

    // ...but only until the theme changes, because the theme is the source
    // of truth and an override is a deliberate exception to it. Anything
    // else would be the old bug wearing a different hat: a palette pinned
    // to a scheme the user has already left.
    const Theme* drac = scheme_named("Dracula");
    REQUIRE(drac != nullptr);
    theme::set_live(*drac);
    CHECK(divergences(colors::live(), colors::project(*drac)) == 0);

    theme::set_live(theme::native);
    std::println("PASS\n");
}

TEST_CASE("markdown palette: readers never observe a torn palette") {
    std::println("--- test_md_palette_snapshot ---");
    // The concurrency shape of the real thing: the UI thread previewing
    // themes (the appearance panel has no streaming gate) while detached
    // parse workers read the palette mid-render.
    //
    // The property is that a reader sees a COHERENT palette — one theme's
    // projection, never a mix of two. Under the old mutable globals a worker
    // could observe half the roles updated; that is what "not published
    // atomically" meant in practice, and it is unrepresentable now because a
    // reader holds a pointer to a frozen value.
    //
    // Run this under TSan for the stronger statement (verified clean:
    // 114k publishes against 3.9M reads, zero reports).
    std::atomic<bool> stop{false};
    std::atomic<unsigned long> reads{0};
    std::atomic<unsigned long> torn{0};

    std::vector<std::thread> workers;
    for (int i = 0; i < 4; ++i) {
        workers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                // One acquire load, then read a frozen object — exactly what
                // the render path does.
                //
                // BY VALUE, deliberately. Holding a `const Palette&` and
                // re-reading it across the loop below would compare fields
                // from DIFFERENT snapshots once a publish lands mid-scan —
                // a bug in the test, not in the palette, and one that
                // reported ~3 spurious tears per 2000 reads. A copy is what
                // "I took a snapshot" actually means.
                const colors::Palette p = colors::live();
                // A coherent snapshot matches SOME theme's projection.
                //
                // Compared by what it PAINTS under that theme, not by
                // representation: the publisher hands over concrete colours
                // (markdown_palette_from resolves against the theme it was
                // given) while the default palette holds slots, so a
                // perfectly coherent snapshot can differ in form from the
                // projection it came from. Only the painted value is
                // evidence of tearing; comparing form would flag every read.
                //
                // native is in the candidate set because the reader can
                // start before the first publish and legitimately observe
                // the boot palette — which is native's projection, and is
                // not equal to any of the 57 schemes. Leaving it out made
                // exactly 3 valid reads per run look like tears.
                bool coherent = painted_divergences(
                    p, colors::project(theme::native), theme::native) == 0;
                for (const auto& s : theme::schemes) {
                    if (coherent) break;
                    if (painted_divergences(p, colors::project(*s.theme),
                                            *s.theme) == 0) {
                        coherent = true;
                        break;
                    }
                }
                if (!coherent) torn.fetch_add(1, std::memory_order_relaxed);
                reads.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (int n = 0; n < 40; ++n)
        for (const auto& s : theme::schemes)
            set_markdown_palette(markdown_palette_from(*s.theme));

    stop.store(true);
    for (auto& t : workers) t.join();

    CHECK(reads.load() > 0);
    CHECK(torn.load() == 0);

    theme::set_live(theme::native);
    set_markdown_palette(markdown_palette_from(theme::native));
    std::println("PASS ({} concurrent reads, {} torn)\n", reads.load(), torn.load());
}
