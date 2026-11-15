// Tests for maya style system: Style attributes, SGR generation, merge, operator|
#include <maya/maya.hpp>
#include <maya/widget/markdown.hpp>   // markdown_palette_from
#include <maya/style/schemes.hpp>       // theme::schemes
// NDEBUG guard: CMake builds tests in Release (-O3 -DNDEBUG), which strips
// assert(). Undefine it here so this file's runtime asserts actually fire.
#undef NDEBUG
#include "agtest.hpp"
#include <print>
#include <algorithm>    // std::find, for the contrast exemption list
#include <cmath>        // std::pow, for WCAG relative luminance
#include <string>
#include <string_view>

using namespace maya;

TEST_CASE("style empty sgr") {
    std::println("--- test_style_empty_sgr ---");
    assert(Style{}.to_sgr().empty());
    std::println("PASS\n");
}

TEST_CASE("style bold sgr") {
    std::println("--- test_style_bold_sgr ---");
    auto s = Style{}.with_bold();
    auto sgr = s.to_sgr();
    // Bold = SGR param "1", wrapped in ESC[...m
    assert(sgr.find("1") != std::string::npos);
    assert(s.bold == true);
    std::println("  bold sgr: '{}'", sgr);
    std::println("PASS\n");
}

TEST_CASE("style dim sgr") {
    std::println("--- test_style_dim_sgr ---");
    auto s = Style{}.with_dim();
    assert(s.dim == true);
    assert(s.to_sgr().find("2") != std::string::npos);
    std::println("PASS\n");
}

TEST_CASE("style italic sgr") {
    std::println("--- test_style_italic_sgr ---");
    auto s = Style{}.with_italic();
    assert(s.italic == true);
    assert(s.to_sgr().find("3") != std::string::npos);
    std::println("PASS\n");
}

TEST_CASE("style underline sgr") {
    std::println("--- test_style_underline_sgr ---");
    auto s = Style{}.with_underline();
    assert(s.underline == true);
    assert(s.to_sgr().find("4") != std::string::npos);
    std::println("PASS\n");
}

TEST_CASE("style inverse sgr") {
    std::println("--- test_style_inverse_sgr ---");
    auto s = Style{}.with_inverse();
    assert(s.inverse == true);
    assert(s.to_sgr().find("7") != std::string::npos);
    std::println("PASS\n");
}

TEST_CASE("style strikethrough sgr") {
    std::println("--- test_style_strikethrough_sgr ---");
    auto s = Style{}.with_strikethrough();
    assert(s.strikethrough == true);
    assert(s.to_sgr().find("9") != std::string::npos);
    std::println("PASS\n");
}

TEST_CASE("style fg color sgr") {
    std::println("--- test_style_fg_color_sgr ---");
    auto s = Style{}.with_fg(Color::green());
    assert(s.fg.has_value());
    auto sgr = s.to_sgr();
    assert(sgr.find("32") != std::string::npos); // green fg = 32
    std::println("  fg(green) sgr: '{}'", sgr);
    std::println("PASS\n");
}

TEST_CASE("style bg color sgr") {
    std::println("--- test_style_bg_color_sgr ---");
    auto s = Style{}.with_bg(Color::blue());
    assert(s.bg.has_value());
    auto sgr = s.to_sgr();
    assert(sgr.find("44") != std::string::npos); // blue bg = 44
    std::println("  bg(blue) sgr: '{}'", sgr);
    std::println("PASS\n");
}

TEST_CASE("style rgb fg sgr") {
    std::println("--- test_style_rgb_fg_sgr ---");
    auto s = Style{}.with_fg(Color::rgb(100, 150, 200));
    auto sgr = s.to_sgr();
    assert(sgr.find("38;2;100;150;200") != std::string::npos);
    std::println("PASS\n");
}

TEST_CASE("style combined sgr") {
    std::println("--- test_style_combined_sgr ---");
    auto s = Style{}.with_bold().with_fg(Color::red());
    auto sgr = s.to_sgr();
    assert(sgr.find("1")  != std::string::npos); // bold
    assert(sgr.find("31") != std::string::npos); // red fg
    std::println("  bold+red sgr: '{}'", sgr);
    std::println("PASS\n");
}

TEST_CASE("style merge additive") {
    std::println("--- test_style_merge_additive ---");
    Style a = Style{}.with_bold();
    Style b = Style{}.with_italic().with_fg(Color::cyan());
    Style merged = a.merge(b);
    auto sgr = merged.to_sgr();
    assert(merged.bold == true);
    assert(merged.italic == true);
    assert(merged.fg.has_value());
    assert(sgr.find("36") != std::string::npos); // cyan fg
    std::println("PASS\n");
}

TEST_CASE("style merge second wins fg") {
    std::println("--- test_style_merge_second_wins_fg ---");
    Style a = Style{}.with_fg(Color::red());
    Style b = Style{}.with_fg(Color::blue());
    Style merged = a.merge(b);
    // Second wins for fg color
    assert(merged.fg.has_value());
    // Resolve before asking for bytes: Style::fg is a Color (it may hold a
    // slot), and only a resolved colour has an SGR spelling.
    assert(theme::live().resolve(*merged.fg).fg_sgr() == "34"); // blue
    std::println("PASS\n");
}

TEST_CASE("style operator pipe") {
    std::println("--- test_style_operator_pipe ---");
    Style a = Style{}.with_bold();
    Style b = Style{}.with_fg(Color::magenta());
    Style combined = a | b;
    assert(combined.bold == true);
    assert(combined.fg.has_value());
    assert(theme::live().resolve(*combined.fg).fg_sgr() == "35"); // magenta
    std::println("PASS\n");
}

TEST_CASE("style pipe equals merge") {
    std::println("--- test_style_pipe_equals_merge ---");
    Style a = Style{}.with_bold().with_fg(Color::red());
    Style b = Style{}.with_italic().with_bg(Color::blue());
    assert((a | b).to_sgr() == a.merge(b).to_sgr());
    std::println("PASS\n");
}

TEST_CASE("style empty predicate") {
    std::println("--- test_style_empty_predicate ---");
    assert( Style{}.empty());
    assert(!Style{}.with_bold().empty());
    assert(!Style{}.with_fg(Color::red()).empty());
    assert(!Style{}.with_bg(Color::red()).empty());
    std::println("PASS\n");
}

TEST_CASE("style predefined bold") {
    std::println("--- test_style_predefined_bold ---");
    assert(bold_style.bold == true);
    assert(bold_style.to_sgr().find("1") != std::string::npos);
    std::println("PASS\n");
}

TEST_CASE("style predefined dim") {
    std::println("--- test_style_predefined_dim ---");
    assert(dim_style.dim == true);
    std::println("PASS\n");
}

TEST_CASE("style predefined fg colors") {
    std::println("--- test_style_predefined_fg_colors ---");
    assert(fg_red.fg.has_value()   && theme::live().resolve(*fg_red.fg).fg_sgr()   == "31");
    assert(fg_green.fg.has_value() && theme::live().resolve(*fg_green.fg).fg_sgr() == "32");
    std::println("PASS\n");
}

TEST_CASE("theme owns_canvas: native defers, a scheme claims") {
    std::println("--- test_theme_owns_canvas ---");
    // The whole native/scheme split, decided by data rather than by name:
    // a theme owns the canvas exactly when it states a real background.
    assert(!theme::owns_canvas(theme::native));
    assert(theme::native.background.kind() == Color::Kind::Default);

    Theme scheme = theme::native;
    scheme.background = Color::rgb(0x28, 0x2A, 0x36);
    assert(theme::owns_canvas(scheme));
    std::println("PASS\n");
}

TEST_CASE("theme canvas fill is inline-safe") {
    std::println("--- test_theme_canvas_fill_inline_safe ---");
    // A host in Mode::Inline paints a window onto live scrollback, not the
    // whole screen. apply_theme_canvas() is only sound if the fill (a)
    // reaches the right edge, so there is no ragged tear where the
    // terminal's own background shows through the gaps, and (b) stops at
    // the content, so it never repaints rows that belong to the user's
    // history. Both are asserted because both are invisible in review and
    // only show up as someone's recoloured scrollback.
    using namespace maya::dsl;

    auto tree = [] { return v(text("hello"), text("a longer line")); };

    // Paint under the theme actually PUBLISHED as live.
    //
    // The fill is not a property of the style structs — a bg-less style is
    // the normal, correct output of most widgets. It is a property of the
    // BYTES: build_sgr() renders the live canvas background for any style
    // that does not name one. So the assertion has to read the emitted SGR,
    // and the theme has to be live while it is read, or the pool's cached
    // strings were built under a different canvas than the one under test.
    auto paint = [&](const Theme& t, auto&& fn) {
        const Theme* saved = &theme::live();
        theme::set_live(t);
        {
            StylePool pool;
            Canvas c{40, 6, &pool};
            render_tree(detail::apply_theme_canvas(Element{tree()}.build(), t,
                                                   /*term_width=*/40),
                        c, pool, t, /*auto_height=*/true);
            pool.retheme();
            fn(c, pool);
        }
        theme::set_live(*saved);
    };

    // Does the cell's emitted SGR set a REAL background? That is the
    // question the terminal answers, and the only one that matters here.
    // `49` (default background) is explicitly not a fill: it is the code
    // that hands the cell back to the terminal's own colour.
    auto has_bg = [](const Canvas& c, const StylePool& pool, int x, int y) {
        const std::string_view sgr = pool.sgr(c.get(x, y).style_id);
        return sgr.find("48;") != std::string_view::npos;
    };

    // native: the seam is a no-op. Nothing is filled, so the terminal's own
    // background — including transparency and wallpaper — survives.
    paint(theme::native, [&](const Canvas& c, const StylePool& pool) {
        for (int y = 0; y < c.height(); ++y)
            for (int x = 0; x < c.width(); ++x)
                assert(!has_bg(c, pool, x, y));
    });

    Theme scheme = theme::native;
    scheme.background = Color::rgb(0x28, 0x2A, 0x36);
    paint(scheme, [&](const Canvas& c, const StylePool& pool) {
        const int last = c.max_content_row();
        assert(last == 1);
        // (a) full width on every content row, including past the text.
        for (int y = 0; y <= last; ++y)
            for (int x = 0; x < c.width(); ++x)
                assert(has_bg(c, pool, x, y));
        // (b) nothing below the content — that is scrollback.
        //
        // Asserted as the EMITTER'S BOUND, not as a property of the cells
        // down there. Style 0 means two different things on a canvas —
        // "unpainted" and "painted with no opinion" — and since a style
        // with no opinion now renders the canvas colour (that is what makes
        // the gaps in (a) fill), the bytes for an untouched cell are
        // indistinguishable from a legitimately themed one. What keeps
        // those rows out of the user's scrollback is that nothing ever asks
        // for them: max_content_row() is the last row the emitter walks.
        assert(last < c.height() - 1);   // there ARE rows below
        assert(c.max_content_row() == last);
    });

    std::println("PASS\n");
}

TEST_CASE("theme reaches markdown, not just the chrome") {
    std::println("--- test_theme_markdown_projection ---");
    // Markdown is most of what is on screen in a chat TUI — prose, code
    // spans, tables. A theme that repaints the borders but leaves the body
    // on its launch-time palette is the "it barely changed anything" bug,
    // so this asserts the projection actually moves.

    const MarkdownPalette nat = markdown_palette_from(theme::native);
    // native must stay terminal-native: body prose is the terminal's own
    // foreground, and the code background is whatever the terminal is.
    // Anything literal here would defeat the point of native.
    assert(nat.text.kind() == Color::Kind::Default);
    assert(nat.code_bg.kind() == Color::Kind::Default);

    Theme scheme = theme::native;
    scheme.text       = Color::rgb(0xF8, 0xF8, 0xF2);
    scheme.surface    = Color::rgb(0x34, 0x36, 0x41);
    scheme.border     = Color::rgb(0x56, 0x57, 0x5F);
    scheme.info       = Color::rgb(0x8B, 0xE9, 0xFD);
    scheme.link       = Color::rgb(0x6C, 0xB6, 0xFF);
    scheme.muted      = Color::rgb(0x6E, 0x73, 0x81);
    scheme.background = Color::rgb(0x28, 0x2A, 0x36);

    // The mapping is authored ONCE, in MAYA_MD_PALETTE. It used to be
    // written twice — the internal.hpp defaults and markdown_palette_from()
    // — disagreeing on nine of the thirty-seven roles, and this test pinned
    // the projection's half of the disagreement (code_fg=info,
    // table_border=border) while markdown actually RENDERED with the other
    // (code_fg=link, table_border=muted) until the first theme swap. Assert
    // the single mapping now, so the test cannot drift from what paints.
    const MarkdownPalette p = markdown_palette_from(scheme);
    assert(p.text == scheme.text);
    assert(p.code_fg == scheme.link);
    assert(p.table_border == scheme.muted);
    // The code background is the SURFACE slot, never a literal black: a
    // hardcoded black is a hole punched through a light scheme.
    assert(p.code_bg == scheme.surface);
    assert(p.code_bg != Color::black());

    std::println("PASS\n");
}

TEST_CASE("theme slot survives the runtime move") {
    std::println("--- test_theme_slot_survives_move ---");
    // THE bug behind "the theme recolours text but never the background".
    //
    // The slot used to be published inside Runtime::create(), against a
    // local that is then moved into Result<Runtime> and moved AGAIN into
    // the caller. The published pointer named storage that was dead before
    // the first frame, so every app_set_theme() wrote into freed memory and
    // rt.theme() — which is what the canvas fill is keyed on — never left
    // its startup value. Foreground colour still changed, because hosts
    // read their palette from their own published copy, which is exactly
    // why it looked like "the theme half works".
    //
    // This reproduces the shape without a terminal: publish against an
    // object, move it, and require that writes land where reads happen.
    struct Holder { Theme t = theme::native; };

    auto make = [] { return Holder{}; };
    Holder h = make();          // moved into place, as Runtime is

    Theme* slot = &h.t;         // published AFTER the move — the fix
    Theme scheme = theme::native;
    scheme.background = Color::rgb(0x28, 0x2A, 0x36);
    *slot = scheme;

    // A write through the slot must be visible to the object the renderer
    // actually reads, and must flip the canvas decision with it.
    assert(h.t.background == scheme.background);
    assert(theme::owns_canvas(h.t));
    assert(!theme::owns_canvas(theme::native));

    std::println("PASS\n");
}

TEST_CASE("theme slots resolve, and literals do not") {
    std::println("--- test_theme_slot_resolution ---");
    // A widget Config default is written once, at static-init, with no theme
    // in scope. Color::slot() lets it name a ROLE instead of a hue and be
    // answered at paint time — which is what stops ~250 such defaults from
    // pinning the UI to whatever palette it was compiled with.
    Theme t = theme::native;
    t.accent  = Color::rgb(0xBD, 0x93, 0xF9);
    t.muted   = Color::rgb(0x62, 0x72, 0xA4);
    t.error   = Color::rgb(0xFF, 0x55, 0x55);

    assert(t.resolve(Color::slot(ThemeSlot::Accent)) == t.accent);
    assert(t.resolve(Color::slot(ThemeSlot::Muted))  == t.muted);
    assert(t.resolve(Color::slot(ThemeSlot::Error))  == t.error);

    // A literal is an explicit host override and must survive untouched —
    // otherwise a caller could never escape the theme.
    const Color lit = Color::rgb(1, 2, 3);
    assert(t.resolve(lit) == lit);
    assert(t.resolve(Color::red()) == Color::red());
    assert(t.resolve(Color::default_color()) == Color::default_color());

    // Under native every slot lands on the terminal's own palette, so the
    // default look is unchanged by all of this.
    assert(theme::native.resolve(Color::slot(ThemeSlot::Muted))
           == theme::native.muted);

    std::println("PASS\n");
}

TEST_CASE("theme discipline: ink slots are never backgrounds") {
    std::println("--- test_theme_slot_axis ---");
    // A slot belongs to one of two AXES, and the axes move in opposite
    // directions when a theme flips polarity:
    //
    //   INK      — text and accents. Dark on a light theme, light on a dark
    //               one, because it must contrast the canvas.
    //   SURFACE  — fills and tints. Light on a light theme, dark on a dark
    //               one, because it IS (a step off) the canvas.
    //
    // Using an ink slot as a background inverts on the opposite polarity: a
    // "dark grey" fill written against a dark theme becomes a near-black bar
    // on a light one. That is invisible in review — both sides look
    // deliberate — and only shows up as a dark slab across someone's light
    // terminal. The bulk slot conversion introduced five of exactly this.
    //
    // So the rule is checked against the theme DATA, which is the only place
    // it can be checked honestly: for a light theme and a dark theme, every
    // surface slot must sit on the same side as its background, and every
    // ink slot on the opposite side.
    // LitColor: a Theme's slots are resolved by construction, so luminance
    // is a fair question to ask of them directly.
    auto lum = [](LitColor c) {
        const LitColor rgb = c.to_rgb();
        return 0.2126 * rgb.r() + 0.7152 * rgb.g() + 0.0722 * rgb.b();
    };

    // WCAG 2.x relative luminance and contrast ratio. Distinct from `lum`
    // above: that one is a plain weighted average used to ask which SIDE of
    // the canvas a fill sits on, which is the right question for surface
    // slots. Legibility is a different question and needs the gamma-corrected
    // form, because perceived difference is not linear in channel value.
    auto rel_lum = [](LitColor c) {
        const LitColor rgb = c.to_rgb();
        auto ch = [](double v) {
            v /= 255.0;
            return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
        };
        return 0.2126 * ch(rgb.r()) + 0.7152 * ch(rgb.g()) + 0.0722 * ch(rgb.b());
    };
    auto contrast_ratio = [&](LitColor a, LitColor b) {
        double hi = rel_lum(a), lo = rel_lum(b);
        if (hi < lo) std::swap(hi, lo);
        return (hi + 0.05) / (lo + 0.05);
    };

    int checked = 0;
    for (const auto& s : theme::schemes) {
        const Theme& t = *s.theme;
        if (t.background.kind() != Color::Kind::Rgb) continue;
        const bool light_bg = lum(t.background) > 128.0;
        ++checked;

        // Surface slots sit on the canvas's side. `shadow` is exempt: it is
        // meant to be darker than the canvas on BOTH polarities.
        for (const auto& [name, c] : {
                 std::pair{"surface", t.surface},
                 std::pair{"overlay", t.overlay},
                 std::pair{"inverse_text", t.inverse_text},
                 std::pair{"diff_added", t.diff_added},
                 std::pair{"diff_removed", t.diff_removed},
                 std::pair{"diff_changed", t.diff_changed}}) {
            if (c.kind() != Color::Kind::Rgb) continue;
            CHECK_MESSAGE((lum(c) > 128.0) == light_bg,
                          s.name << ": " << name
                                 << " is on the wrong side of the canvas");
        }

        // Text must contrast the canvas — the one guarantee legibility rests
        // on, and the thing a mis-assigned slot destroys.
        //
        // CONTRAST, not which side of 128 the text falls on. The midpoint test
        // this replaces failed 23 themes, and 13 of them were perfectly
        // legible: Alien Blood puts text at luminance 119 on a canvas at 20,
        // six times brighter and obviously readable, but both land below 128
        // so a same-side check called it broken. A rule that fires on legible
        // themes is not a legibility rule, and a permanently-red assertion
        // gets scrolled past — this one had been failing every maya CI run
        // since the 615-scheme import, which is how the MSVC break went
        // unnoticed for six commits behind it.
        //
        // WCAG relative luminance + the standard ratio, so the threshold means
        // something outside this file. 3.0:1 is WCAG AA for large text; the
        // floor is deliberately not 4.5 because a handful of shipped themes
        // (Matrix, HaX0R R3D, C64) are low-contrast BY AUTHORIAL INTENT and
        // rewriting them to satisfy a lint would be a worse bug than the lint.
        // What 3.0 still catches is the real failure this guards: an ink slot
        // used as a background, which collapses the ratio toward 1.0.
        if (t.text.kind() == Color::Kind::Rgb) {
            // Named exemptions, because a floor with no escape hatch gets
            // LOWERED until it passes, and then it guards nothing. These ten
            // are low-contrast on purpose — phosphor-CRT and glitch themes
            // whose whole character is dim text on near-black — so they are
            // listed individually rather than allowed for by a weaker global
            // threshold. Adding a name here is a deliberate, reviewable act;
            // a NEW theme that trips the floor is a bug until someone says
            // otherwise in a diff.
            static constexpr std::string_view kLowContrastByDesign[] = {
                "C64", "Crayon Pony Fish", "Darkermatrix", "Darkmatrix",
                "HaX0R R3D", "Matrix", "Poimandres White", "Royal",
                "Shaman", "base16-icy",
            };
            const std::string_view nm{s.name};
            const bool exempt =
                std::find(std::begin(kLowContrastByDesign),
                          std::end(kLowContrastByDesign), nm)
                != std::end(kLowContrastByDesign);

            const double r = contrast_ratio(t.text, t.background);
            if (!exempt) {
                CHECK_MESSAGE(r >= 3.0,
                              std::string{s.name}
                                  << ": text/background contrast is only " << r
                                  << ":1 (want >= 3.0) — check for an ink slot "
                                     "used as a background");
            } else {
                // An exemption must stay EARNED. If someone fixes one of
                // these, the name has to leave the list, or the list slowly
                // becomes a place where regressions hide.
                CHECK_MESSAGE(r < 3.0,
                              std::string{s.name}
                                  << " now meets 3.0:1 (" << r
                                  << ":1) — remove it from kLowContrastByDesign");
            }
        }
    }
    CHECK(checked > 0);
    std::println("PASS ({} schemes checked)\n", checked);
}

TEST_CASE("theme: a style with no background means THE CANVAS") {
    std::println("--- test_theme_bg_default ---");
    // The structural end of "some frame lines still show the terminal
    // through".
    //
    // Widgets produce bg-less styles constantly and legitimately — a border
    // glyph, a divider rule, a bare label. Those styles used to emit no
    // background SGR, which a terminal reads as "reset to MY default", so
    // every one of them punched a hole in a themed canvas. Fixing them
    // widget-by-widget never converged, because the DEFAULT was wrong
    // rather than any one widget: the next bg-less style anyone wrote
    // brought the bug straight back.
    //
    // Inverting the default ends the class. build_sgr and
    // write_transition_sgr are the ONLY two places a cell becomes bytes,
    // and both now render "no opinion" as the canvas colour. A widget can
    // no longer forget, because there is nothing left to remember.
    using namespace maya::dsl;

    auto emit_for = [](const Theme& t) {
        theme::set_live(t);
        StylePool pool;
        Canvas c{40, 4, &pool};
        pool.retheme();
        render_tree(v(text("header"), sep, text("row")).build(),
                    c, pool, t, /*auto_height=*/true);
        std::string out;
        serialize(c, pool, out);
        return out;
    };

    // A themed canvas: every row carries it, including the separator's
    // border glyphs and the blank tail past the text.
    Theme light = theme::native;
    light.background = Color::rgb(0xEA, 0xEA, 0xEA);
    const std::string themed = emit_for(light);
    assert(themed.find("48;2;234;234;234") != std::string::npos);

    // native states no background, so nothing is emitted and the terminal
    // shows through — transparency, background image and all. This is the
    // case an unconditional fill would have destroyed, and it is the whole
    // reason the rule is keyed on owns_canvas rather than applied always.
    const std::string nat = emit_for(theme::native);
    assert(nat.find("48;2") == std::string::npos);
    assert(nat.find("48;5") == std::string::npos);

    theme::set_live(theme::native);
    std::println("PASS\n");
}

TEST_CASE("theme: a swap re-derives cached SGR, and says that it did") {
    std::println("--- test_theme_retheme_detects_swap ---");
    // The "I picked a new scheme and the background stayed the old colour"
    // bug, at both of the layers that produced it.
    //
    // (1) DETECTION. retheme() used to compare `&theme::live()` against the
    //     pointer it cached last time, on the stated premise that the live
    //     slot is re-seated rather than mutated. It is not: app_set_theme()
    //     assigns THROUGH the slot, so the address is fixed for the life of
    //     the runtime. The pointer matched on every frame after the first
    //     and retheme() became a permanent no-op — every cached SGR string
    //     kept the first theme's background forever. Comparing by VALUE is
    //     what makes a swap observable at all.
    //
    // (2) REPORTING. A retheme changes no Style and no canvas cell — only
    //     what the ids RENDER as. Both inline emitters diff packed
    //     (glyph, style_id) cells against a shadow of the last wire frame,
    //     so after a swap every row compares equal, nothing is emitted, and
    //     the terminal keeps the old paint. The bool return is the signal
    //     those emitters use to drop their shadow; without it the fix in
    //     (1) refreshes a cache nobody ever transmits.
    using namespace maya::dsl;

    Theme a = theme::native;
    a.background = Color::rgb(0x10, 0x20, 0x30);
    Theme b = theme::native;
    b.background = Color::rgb(0xEE, 0xDD, 0xCC);

    // THE SLOT. A host owns exactly one Theme object (Runtime::theme_) and
    // publishes its address once; app_set_theme() then assigns THROUGH it
    // (`*slot = t`) and hands the RESULT to set_live() on every change.
    // Modelling it as one mutated object is what makes this test reproduce
    // the real bug — with two distinct Theme locals the addresses differ and
    // even the broken pointer compare "works", which is exactly why this
    // shipped.
    //
    // Note the live slot INTERNS, so `&theme::live()` is not `&slot` and a
    // mutation of `slot` alone is not a swap — the host has to publish it,
    // which is precisely what app_set_theme() does on the next line after
    // `*slot = t`. That is why the re-publish below is not test scaffolding:
    // it is the production sequence.
    Theme slot = a;
    theme::set_live(slot);

    StylePool pool;
    Canvas c{20, 3, &pool};
    pool.retheme();
    // Rendered against `native` on purpose: native names no background, so
    // no element interns an EXPLICIT one and every background byte in the
    // output can only have come from build_sgr()'s canvas fallback — which
    // reads the LIVE theme. That isolates the thing under test. A style that
    // names its own colour (a selection strip, a diff tint) is not
    // theme-derived and is deliberately left alone by retheme().
    render_tree(v(text("hello")).build(), c, pool, theme::native,
                /*auto_height=*/true);

    std::string first;
    serialize(c, pool, first);
    assert(first.find("48;2;16;32;48") != std::string::npos);

    // Idempotent: nothing changed, so nothing is re-derived and no caller
    // is told to repaint. This is the every-frame case and it must stay
    // cheap and quiet.
    assert(pool.retheme() == false);

    // Swap the live theme exactly as app_set_theme() does: assign through
    // the published slot, then publish. The ORIGINAL bug was retheme()
    // comparing `&theme::live()` against the pointer it cached last time —
    // and because the host mutates one object in place, that address never
    // moves, so every cached SGR string kept the old background forever.
    // Only a VALUE compare can observe this, which is what the assert below
    // pins down. Interning means live() no longer even aliases `slot`, but
    // the identity `&live_before == &live_after` would STILL hold for a
    // repeat of the same theme, so the value compare is still the only
    // thing standing between this and the bug.
    slot = b;
    theme::set_live(slot);
    assert(theme::live() == b);        // the new value is live
    assert(pool.retheme() == true);

    // The same cells, unchanged and un-repainted, now serialize in the new
    // canvas colour — the old one must be gone entirely.
    std::string second;
    serialize(c, pool, second);
    assert(second.find("48;2;238;221;204") != std::string::npos);
    assert(second.find("48;2;16;32;48") == std::string::npos);

    theme::set_live(theme::native);
    std::println("PASS\n");
}

TEST_CASE("theme: a swap survives the fullscreen double-buffer too") {
    std::println("--- test_theme_swap_fullscreen ---");
    // The inline path needs retheme()'s bool because its wire shadow diffs
    // packed (glyph, style_id) cells and a swap changes neither. Fullscreen
    // discards that bool — and the reason has to be checked, not assumed,
    // because write_diff() compares the back canvas against the presented
    // front one, which is the same shape of shadow.
    //
    // It is safe because clear() zeroes every back cell to style 0 first, so
    // the frame is re-interned from nothing under the new theme and an
    // unchanged cell still differs from front in the only way diff() looks
    // at. This test pins that: if an incremental fullscreen repaint ever
    // stops clearing, the reasoning dies and this goes red rather than
    // someone rediscovering it as "my theme only half-applies".
    using namespace maya::dsl;

    Theme a = theme::native;
    a.background = Color::rgb(0x11, 0x22, 0x33);
    Theme b = theme::native;
    b.background = Color::rgb(0xFA, 0xFB, 0xFC);

    Theme slot = a;                 // one slot, mutated in place
    theme::set_live(slot);

    StylePool pool;
    Canvas front{40, 4, &pool};
    Canvas back{40, 4, &pool};
    auto tree = [] { return v(text("hello"), text("world")); };

    std::string out1;
    RenderPipeline<stage::Idle>::start(back, pool, slot, out1)
        .clear().paint(Element{tree()}.build())
        .open_frame(false).write_diff(front).close_frame(false);
    assert(out1.find("48;2;17;34;51") != std::string::npos);

    std::swap(front, back);         // present
    slot = b;                       // swap through the published slot
    theme::set_live(slot);          // ...and publish it, as app_set_theme does

    // This pipeline is handed `slot` directly, so the swap would land even
    // without the publish above. It is here so the sequence matches the
    // production one — the live slot interns, and a test that mutates a
    // local without publishing is asserting against a theme the rest of the
    // process cannot see.
    // The SAME tree, so every glyph and every style id is identical.
    std::string out2;
    RenderPipeline<stage::Idle>::start(back, pool, slot, out2)
        .clear().paint(Element{tree()}.build())
        .open_frame(false).write_diff(front).close_frame(false);
    assert(out2.find("48;2;250;251;252") != std::string::npos);
    assert(out2.find("48;2;17;34;51") == std::string::npos);

    theme::set_live(theme::native);
    std::println("PASS\n");
}

TEST_CASE("theme: markdown tables and code blocks carry the canvas") {
    std::println("--- test_theme_markdown_render ---");
    // Table rules and code-block frames are styled fg-only — with_fg(border)
    // and nothing else — because a border has no reason to name a
    // background. Under the old default that made every one of them a hole
    // in a themed canvas, which is what "even in md" looked like: a light
    // panel with the terminal showing through every rule and frame.
    //
    // This renders the two worst offenders through the real markdown
    // pipeline rather than a stand-in, because the bug was never in the
    // widgets — it was in what a bg-less style MEANT.
    Theme t   = theme::native;
    t.background = Color::rgb(0xEA, 0xEA, 0xEA);
    t.text       = Color::rgb(0x23, 0x23, 0x22);
    t.border     = Color::rgb(0xBE, 0xBE, 0xBE);
    t.surface    = Color::rgb(0xDE, 0xDE, 0xDE);
    t.info       = Color::rgb(0x0E, 0x71, 0x7C);
    theme::set_live(t);
    set_markdown_palette(markdown_palette_from(t));

    StylePool pool;
    Canvas c{60, 20, &pool};
    pool.retheme();

    StreamingMarkdown md;
    md.feed("| col | val |\n|---|---|\n| a | 1 |\n\n```cpp\nint x = 1;\n```\n");
    md.finish();
    render_tree(md.build(), c, pool, t, /*auto_height=*/true);

    std::string out;
    serialize(c, pool, out);

    // Walk the emitted bytes tracking background state; every glyph must
    // land on a stated background, never the terminal's.
    int holes = 0, painted = 0;
    bool def = true;
    for (std::size_t i = 0; i < out.size();) {
        if (out[i] == '\x1b') {
            const std::size_t j = out.find('m', i);
            if (j != std::string::npos && i + 1 < out.size() && out[i + 1] == '[') {
                const std::string ps = out.substr(i + 2, j - i - 2);
                if (ps.find("48;2") != std::string::npos) def = false;
                else if (ps.empty() || ps == "0"
                         || ps.find("49") != std::string::npos) def = true;
                i = j + 1;
                continue;
            }
            i += 2;
            continue;
        }
        if (out[i] != '\n' && out[i] != '\r' && out[i] != ' ') {
            if (def) ++holes; else ++painted;
        }
        ++i;
    }
    assert(painted > 0);
    assert(holes == 0);

    set_markdown_palette(markdown_palette_from(theme::native));
    theme::set_live(theme::native);
    std::println("PASS\n");
}

TEST_CASE("style equality") {
    std::println("--- test_style_equality ---");
    Style a = Style{}.with_bold().with_fg(Color::red());
    Style b = Style{}.with_bold().with_fg(Color::red());
    Style c = Style{}.with_bold().with_fg(Color::green());
    assert(a == b);
    assert(a != c);
    std::println("PASS\n");
}

TEST_CASE("theme: the live slot owns what it publishes") {
    std::println("--- test_live_slot_owns_its_theme ---");

    // set_live() used to store `&t`, so the lifetime obligation lived in
    // the CALLER and was invisible at the signature. The obvious call --
    // set_live(a temporary) -- compiled and left live() dereferencing a
    // dead object on the next paint. ASan called it stack-use-after-scope;
    // without ASan it is a palette that is subtly, intermittently wrong.
    //
    // The slot interns now, so a temporary is safe. This is the regression
    // guard: it reads every slot back through live() AFTER the argument is
    // gone, which is exactly the window the old code got wrong.
    {
        theme::set_live(Theme{theme::dracula});   // rvalue; dead on return
        const Theme& l = theme::live();
        assert(l.complete());
        assert(l == theme::dracula);
    }

    // Interning dedups by VALUE, which is what bounds the never-freed
    // table. A host resolving `auto` per frame calls this every frame, and
    // a background detect that flaps across an ssh hop alternates forever;
    // neither may allocate per call.
    {
        const Theme* first = &theme::live();
        for (int i = 0; i < 200; ++i) {
            theme::set_live(Theme{theme::dracula});
            assert(&theme::live() == first);
        }
    }

    // Reset-to-native reuses the static rather than copying it.
    theme::set_live(theme::native);
    assert(&theme::live() == &theme::native);

    std::println("PASS\n");
}

TEST_CASE("theme: an incomplete theme cannot go live") {
    std::println("--- test_live_slot_rejects_holes ---");

    // complete() is a static_assert for the 57 built-in schemes and for
    // anything constexpr. But a theme parsed from a user config, built
    // field-by-field, or derive()d at runtime reaches the slot without ever
    // meeting that assert -- and an Unset slot paints as "inherit", so the
    // failure is a widget quietly adopting the terminal's foreground
    // instead of its role colour. Legible often enough to ship; wrong on
    // exactly the light-background terminal nobody tested.
    theme::set_live(theme::dracula);
    const Theme* before   = &theme::live();
    const unsigned epoch0 = theme::live_epoch();

    Theme holed = theme::dracula;
    holed.warning = LitColor{};            // back to ColorKind::Unset
    assert(!holed.complete());
    assert(holed.first_unset().has_value());
    assert(std::string_view{slot_field_name(*holed.first_unset())} == "warning");

    assert(theme::try_set_live(holed) == false);

    // A refused set changes NOTHING. Not the theme -- a half-themed surface
    // looks deliberate and is worse than a refusal -- and not the epoch,
    // which would spuriously re-derive every projected palette.
    assert(&theme::live() == before);
    assert(theme::live_epoch() == epoch0);

    // A complete theme still goes through, and still bumps the epoch: the
    // projection cache keys off it, so a swap that failed to bump would
    // leave every projected<P> palette permanently stale.
    assert(theme::try_set_live(theme::nord) == true);
    assert(theme::live() == theme::nord);
    assert(theme::live_epoch() > epoch0);

    theme::set_live(theme::native);
    std::println("PASS\n");
}

