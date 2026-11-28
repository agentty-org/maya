// test_spacing_invariants — the proof that killed the screenshot loop.
//
// ── Why this file exists ─────────────────────────────────────────────────
//
// A run of spacing bugs shipped, each found by a human squinting at a
// screenshot, each fixed in isolation:
//
//   • "Anthropic Opus" rendered with the two words TOUCHING, because a
//     separator was removed on the reasoning that the chip's own trailing
//     space would serve — but that space is background-FILLED, so it reads
//     as the chip's padding, not as separation.
//   • the status bar painted "·   ·" with a dead gap between them, because
//     a separator was baked onto the END of one segment on the assumption
//     that another always followed it. When the model badge moved out, the
//     assumption broke and the orphan stayed.
//   • the composer read "    1 words" — four leading spaces from tabular
//     padding that bought nothing in a right-aligned cluster.
//   • the sparkline read "⚡  0.0" — TWO owners each contributed spacing
//     (the "⚡ " literal and a %5.1f right-pad), neither aware of the other,
//     and ⚡ is a 2-column wide glyph on top of that.
//
// One root cause: SPACING WAS ENCODED AS CONTENT. `text(" ")` is not a gap,
// it is a string that happens to be blank — invisible to the layout engine,
// invisible to measurement, and invisible to the shed ladder, so a separator
// inside segment N cannot know segment N+1 was dropped.
//
// Fixing instances one at a time is a treadmill: each fix is invisible until
// someone hits the exact terminal width that reveals it. This file is the
// mechanical version of that squinting. It renders the real widgets across
// every width they must survive and asserts the properties a human was
// checking by eye:
//
//   1. no run of 2+ spaces between visible content (double-gap)
//   2. no separator glyph adjacent to another separator (orphaned sep)
//   3. no separator left dangling at either end of a row
//   4. content never touches where a gap was intended (regression guard)
//
// A widget that hand-rolls spacing will eventually violate one of these at
// SOME width. That is the point: the test sweeps widths so it does not
// depend on anyone guessing which one.

#include <maya/maya.hpp>
#include <maya/widget/agentty/status_bar.hpp>
#include <maya/widget/agentty/composer.hpp>
#include <maya/widget/agentty/model_badge.hpp>
#include <maya/widget/shortcut_row.hpp>
#include <maya/widget/agentty/token_stream_sparkline.hpp>
#include <maya/text/unicode_width.hpp>

#include <print>
#include <string>
#include <vector>

#include "check.hpp"
#include "agtest.hpp"

using namespace maya;

namespace {

// Render a row to a plain string. Non-ASCII collapses to a single sentinel
// so the scanners below reason about STRUCTURE (content / space /
// separator) rather than glyphs.
constexpr char kSepMark = '\x01';   // any separator-ish glyph (·)
constexpr char kGlyph   = '\x02';   // any other non-ASCII glyph
constexpr char kRailMark = '\x03';  // the structural rail (▌)

std::string structural_row(const Canvas& c, int y) {
    std::string s;
    for (int x = 0; x < c.width(); ++x) {
        const Cell e = c.get(x, y);
        const char32_t cp = e.character;
        if (cp == 0 || cp == U' ')      s += ' ';
        else if (cp == U'\u00B7')       s += kSepMark;      // ·
        else if (cp == U'\u258C')       s += kRailMark;     // ▌
        else if (cp < 0x80)             s += static_cast<char>(cp);
        else                            s += kGlyph;
        // A wide glyph occupies 2 cells; the trailing cell reads as 0 and
        // would otherwise look like a space. Mark it as glyph continuation.
        if (unicode::char_width(cp) == 2 && x + 1 < c.width()) {
            s += kGlyph;
            ++x;
        }
    }
    // Trailing blank cells are the row's slack, not a gap between content.
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

// Index of the first / last non-space, or npos.
std::size_t first_content(const std::string& s) {
    return s.find_first_not_of(' ');
}

// ── Invariant 1: no HARDCODED double gap between content ────────────────
//
// Two or more spaces between visible content means two owners each added
// spacing without knowing about the other ("⚡  0.0", "▌  Streaming"), or a
// fixed-width pad was spent where it bought nothing ("CTX   28%").
//
// The hard part is telling those from a LAYOUT SPACER — the flexible gap the
// engine opens between the left and right clusters. Both look like a run of
// spaces in the rendered row, and at tight widths a spacer can be as narrow
// as one or two columns, so a run-length threshold cannot separate them.
//
// The discriminator is GROWTH. Render the same element twice, at w and at
// w + kProbeDelta:
//
//   • a spacer ABSORBS the extra width — its run grows by kProbeDelta;
//   • a hardcoded gap is baked into a string — its run is identical.
//
// So we compare the two rows' gap runs in order and flag only the ones that
// did not move. That is exactly the property we care about ("this gap is
// content, not layout") rather than a proxy for it.
constexpr int kProbeDelta = 8;

struct Gap { std::size_t at; int run; };

std::vector<Gap> interior_gaps(const std::string& s) {
    std::vector<Gap> out;
    const auto b = first_content(s);
    if (b == std::string::npos) return out;
    for (std::size_t i = b; i < s.size();) {
        if (s[i] != ' ') { ++i; continue; }
        std::size_t j = i;
        while (j < s.size() && s[j] == ' ') ++j;
        if (j < s.size())   // interior only; trailing slack already trimmed
            out.push_back({i, static_cast<int>(j - i)});
        i = j;
    }
    return out;
}

// Compare the gap sequences of the narrow and wide renders. A gap that is
// present in both, at the same ordinal position, with the SAME run length,
// and wider than kMaxOwnedPad, is a hardcoded double gap.
//
// Why a tolerance rather than ">= 2": a SINGLE owner may legitimately hold
// several columns — a fixed-width numeric field pads to its width so a
// neighbour doesn't twitch as the value grows (the sparkline's rate, the
// gauge's percent). That is one owner doing one job, and it does not grow
// with the row, so the growth probe cannot distinguish it from a bug.
//
// The bug shape is TWO owners each contributing to the same gap, which in
// practice means a small run that no single field would have produced. A
// fixed field is sized to the values it displays; past ~4 columns of dead
// space between two fragments, no plausible field is responsible.
constexpr int kMaxOwnedPad = 4;

bool has_hardcoded_double_gap(const std::string& narrow,
                              const std::string& wide,
                              std::string& why) {
    const auto gn = interior_gaps(narrow);
    const auto gw = interior_gaps(wide);
    // Different fragment counts mean the two widths shed differently; the
    // comparison is meaningless, so skip rather than guess.
    if (gn.size() != gw.size()) return false;
    for (std::size_t k = 0; k < gn.size(); ++k) {
        if (gn[k].run <= kMaxOwnedPad) continue;
        if (gn[k].run != gw[k].run) continue;   // grew -> it is a spacer
        why = "hardcoded gap of " + std::to_string(gn[k].run)
            + " cols at col " + std::to_string(gn[k].at)
            + " (unchanged when the row grew " + std::to_string(kProbeDelta)
            + " cols, so it is baked in, not layout)";
        return true;
    }
    return false;
}

// ── Invariant 2: separators are never orphaned ──────────────────────────
//
// A separator must sit BETWEEN content. Two separators with only spaces
// between them means one of them lost the segment it was introducing —
// exactly the "·   ·" the status bar painted once the model badge moved to
// the composer.
bool has_orphan_separator(const std::string& s, std::string& why) {
    std::size_t prev = std::string::npos;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != kSepMark) continue;
        if (prev != std::string::npos) {
            bool only_space = true;
            for (std::size_t k = prev + 1; k < i; ++k)
                if (s[k] != ' ') { only_space = false; break; }
            if (only_space) {
                why = "adjacent separators at cols " + std::to_string(prev)
                    + " and " + std::to_string(i) + " (nothing between)";
                return true;
            }
        }
        prev = i;
    }
    return false;
}

// ── Invariant 3: no dangling separator at either end ────────────────────
//
// A separator before the first content or after the last is introducing
// nothing. This is the shape a trailing "baked-in" separator leaves when
// the following segment sheds out.
bool has_dangling_separator(const std::string& s, std::string& why) {
    const auto b = s.find_first_not_of(' ');
    if (b == std::string::npos) return false;
    const auto e = s.find_last_not_of(' ');
    if (s[b] == kSepMark) { why = "row starts with a separator"; return true; }
    if (s[e] == kSepMark) { why = "row ends with a separator";   return true; }
    return false;
}

// ── Invariant 4: distinct fragments never TOUCH ───────────────────────
//
// The mirror image of the double-gap rule, and the half it kept missing:
// invariants 1-3 all detect too MUCH space, so a fix that removes a gap
// that was actually load-bearing sails through. That is exactly how a
// spinner ended up flush against the rail — the double-gap fix removed the
// rail's separator, and nothing was watching the other direction.
//
// A structural rail glyph (▌) introduces the group beside it, so it must
// never be adjacent to that group's first character. Checked as a specific
// shape rather than a general "two fragments" rule, because the renderer
// has no notion of fragment boundaries — only the widget does, and the rail
// is the boundary the status bar actually draws.
bool has_touching_rail(const std::string& s, std::string& why) {
    for (std::size_t i = 0; i + 1 < s.size(); ++i) {
        if (s[i] != kRailMark) continue;
        const char next = s[i + 1];
        if (next != ' ' && next != kRailMark) {
            why = std::string{"rail glyph touches content ('"} + next
                + "') at col " + std::to_string(i);
            return true;
        }
    }
    return false;
}

// Two ADJACENT segments must never touch: a seam with zero columns between
// its neighbours reads as a single token. This is the "1.9sCTX" bug, where
// the phase chip's elapsed tail butted straight against the context gauge's
// leading label because the middle spacer (grow:1, natural width 0) was
// squeezed to nothing at the tight width and nothing had RESERVED the gap.
//
// Detected as a case/kind transition that no word in the strip's vocabulary
// contains: a lowercase letter or digit immediately followed by two or more
// uppercase letters ("sCTX", "9CTX"). Real labels are either whole words
// ("Streaming") or fully-capitalised tokens ("CTX", "WRITE"), so this shape
// only ever arises where two segments have collided. Deliberately narrow:
// it must not fire on legitimate CamelCase or on a units suffix like "9m05s".
bool has_touching_segments(const std::string& s, std::string& why) {
    auto lower_or_digit = [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    };
    auto upper = [](unsigned char c) { return c >= 'A' && c <= 'Z'; };

    for (std::size_t i = 0; i + 2 < s.size(); ++i) {
        if (!lower_or_digit(static_cast<unsigned char>(s[i]))) continue;
        if (upper(static_cast<unsigned char>(s[i + 1]))
            && upper(static_cast<unsigned char>(s[i + 2]))) {
            why = "segments collide ('" + s.substr(i, 4)
                + "') at col " + std::to_string(i)
                + " \u2014 no gap reserved between neighbours";
            return true;
        }
    }
    return false;
}

// Run every invariant over one rendered row. `wide` is the same row rendered
// kProbeDelta columns wider, used to tell a baked-in gap from a spacer.
void check_row(const std::string& row, const std::string& wide,
               int w, const char* what) {
    std::string why;
    MAYA_TEST_CHECK(!has_hardcoded_double_gap(row, wide, why),
        std::string{what} + " @w=" + std::to_string(w) + ": " + why
        + "\n    narrow |" + row + "|\n    wide   |" + wide + "|");
    why.clear();
    MAYA_TEST_CHECK(!has_orphan_separator(row, why),
        std::string{what} + " @w=" + std::to_string(w) + ": " + why
        + "  |" + row + "|");
    why.clear();
    MAYA_TEST_CHECK(!has_dangling_separator(row, why),
        std::string{what} + " @w=" + std::to_string(w) + ": " + why
        + "  |" + row + "|");
    why.clear();
    MAYA_TEST_CHECK(!has_touching_rail(row, why),
        std::string{what} + " @w=" + std::to_string(w) + ": " + why
        + "  |" + row + "|");
    why.clear();
    MAYA_TEST_CHECK(!has_touching_segments(row, why),
        std::string{what} + " @w=" + std::to_string(w) + ": " + why
        + "  |" + row + "|");
}

// Render every non-empty row of `el` at one width.
std::vector<std::string> rows_at(const Element& el, int w) {
    StylePool pool;
    Canvas cv(w, 8, &pool);
    render_tree(el, cv, pool, theme::native, true);
    std::vector<std::string> out;
    for (int y = 0; y < 8; ++y) {
        std::string r = structural_row(cv, y);
        if (r.find_first_not_of(' ') != std::string::npos)
            out.push_back(std::move(r));
    }
    return out;
}

// Sweep a width range, checking each row against its wider twin.
void sweep(const Element& el, const char* what, int lo, int hi) {
    for (int w = lo; w <= hi; ++w) {
        const auto narrow = rows_at(el, w);
        const auto wide   = rows_at(el, w + kProbeDelta);
        // Only compare when both widths produced the same row structure;
        // otherwise a shed changed which rows exist and the pairing is
        // meaningless.
        if (narrow.size() != wide.size()) continue;
        for (std::size_t i = 0; i < narrow.size(); ++i)
            check_row(narrow[i], wide[i], w, what);
    }
}

} // namespace

TEST_CASE("spacing: the status bar never double-gaps or orphans a separator") {
    std::println("=== test_spacing_status_bar ===");
    // The status bar is where the "·   ·" bug lived: its right group sheds
    // segments by width, and a separator baked onto a segment's tail
    // outlives the segment that was supposed to follow it.
    //
    // The phase chip is swept BOTH with and without a spinner glyph. Its
    // internal pad sits between glyph and verb, so the two cases put a
    // different first character against the rail's separator — and a fix
    // that balances one can unbalance the other. Sweeping only the empty
    // case is how "▌⠀Streaming" (spinner touching the rail) shipped after
    // "▌  Streaming" (double gap) was fixed.
    // The elapsed tail is swept too. Without it the phase chip ends in a
    // letter ("Streaming") and the seam against the right group never puts
    // a digit-run against "CTX" — which is exactly how "1.9sCTX" survived
    // this suite. The tail is the chip's rightmost fragment and the last
    // thing to shed before the verb, so it owns the tightest seam on the row.
    for (bool with_glyph : {false, true}) {
        for (bool with_badge : {false, true}) {
            for (float elapsed : {-1.0f, 1.9f, 12.3f, 234.0f, 605.0f}) {
                StatusBar::Config c;
                c.breadcrumb.title   = "understand this project deeply";
                c.phase.verb         = "Streaming";
                c.phase.glyph        = with_glyph ? "\xe2\xa0\x8b" : "";   // ⠋
                c.phase.elapsed_secs = elapsed;
                c.context.max        = 1'000'000;
                c.context.used       = 286'100;
                if (with_badge)
                    c.model_badge =
                        ModelBadge{{.label = "Opus", .version = "4.8"}}.build();
                sweep(StatusBar{c}.build(),
                      with_glyph ? (with_badge ? "status_bar+glyph+badge"
                                               : "status_bar+glyph")
                                 : (with_badge ? "status_bar+badge" : "status_bar"),
                      20, 200);
            }
        }
    }
    std::println("  clean across widths 20..200, glyph x badge x elapsed");

    // The right group RIGHT-ALIGNS. The activity row is lead | rail | phase
    // | grow-slack | right-group, so the throughput+CTX cluster sits flush
    // against the row's right edge with the slack opening up after the phase
    // chip. This shipped BROKEN: a fix for a tight-width collision wrapped
    // the middle spacer in an h() but dropped its `| grow(1.0f)`, so the
    // wrapped box measured to its one hard column and stopped distributing
    // slack — the whole cluster floated mid-row with a growing void to its
    // right at every width past the point things fit. The gap sweep above
    // could not see it: nothing collided or double-gapped, the cluster was
    // just in the wrong place. Assert the position directly.
    //
    // “Flush right” = at most one trailing blank column (the row keeps a
    // one-column bleed guard on its right so a wide glyph in the last cell
    // cannot wrap). Swept wide, where the bug is visible; a too-narrow row
    // that sheds to nothing is skipped.
    for (bool with_badge : {false, true}) {
        StatusBar::Config c;
        c.phase.verb         = "Ready";
        c.phase.elapsed_secs = 1.9f;
        c.context.max        = 1'000'000;
        c.context.used       = 317'800;
        c.token_stream.rate    = 0.0f;
        c.token_stream.history = {1, 2, 3, 2, 4};
        c.token_stream.live    = true;
        if (with_badge)
            c.model_badge =
                ModelBadge{{.label = "Opus", .version = "4.8"}}.build();
        for (int w = 80; w <= 200; w += 10) {
            const auto rows = rows_at(StatusBar{c}.build(), w);
            if (rows.empty()) continue;
            // The activity row is the one carrying "CTX".
            const std::string* row = nullptr;
            for (const auto& r : rows)
                if (r.find("CTX") != std::string::npos) { row = &r; break; }
            if (!row) continue;
            const auto last = row->find_last_not_of(' ');
            const std::size_t trailing =
                last == std::string::npos ? w : (w - 1 - last);
            MAYA_TEST_CHECK(trailing <= 1,
                "status bar right group is flush right @w=" + std::to_string(w)
                + " (trailing=" + std::to_string(trailing) + ")  |" + *row + "|");
        }
    }
    std::println("  right group flush-right across widths 80..200");
    std::println("  PASS");
}

TEST_CASE("spacing: the composer footer never double-gaps") {
    std::println("=== test_spacing_composer ===");
    // The composer footer held the "    1 words" tabular padding and the
    // "Anthropic Opus" touching-words bug. Sweep with and without a host
    // status slot, since that slot displaces the built-in key hints.
    for (bool with_status : {false, true}) {
        Composer::Config c;
        c.text   = "hello world";
        c.cursor = 11;
        c.word_estimate  = 2;
        c.token_estimate = 3;
        c.profile = {.label = "write"};
        if (with_status) {
            c.status = dsl::text("Anthropic Opus 4.8").build();
            c.show_key_hints = false;
        }
        sweep(Composer{c}.build(),
              with_status ? "composer+status" : "composer", 30, 200);
    }
    std::println("  clean across widths 30..200, with and without a status slot");
    std::println("  PASS");
}

TEST_CASE("spacing: the sparkline keeps its unit tight to its number") {
    std::println("=== test_spacing_sparkline ===");
    // This chip has twice been the spacing bug-of-record, so it is swept
    // rather than eyeballed. First a leading "⚡ " literal and a %5.1f
    // right-pad both contributed spacing without knowing about each other
    // (and ⚡ was 2 columns wide on top of it); then, with the glyph gone,
    // the field's own right-pad became the number↔unit gap and grew as the
    // number shrank. Sweep the rate magnitudes, since the format switches
    // at 999.5 / 9999.5.
    std::size_t unit_col = std::string::npos;
    std::size_t spark_col = std::string::npos;
    for (float rate : {0.0f, 9.9f, 105.2f, 999.4f, 1500.0f, 25000.0f}) {
        TokenStreamSparkline::Config c;
        c.rate    = rate;
        c.history = {rate, rate, rate, rate};
        c.live    = true;
        sweep(TokenStreamSparkline{c}.build(), "sparkline", 20, 60);

        // The unit belongs to its number: EXACTLY one space between them,
        // identical at every magnitude. The field's slack is spent as a LEAD
        // on the number, so a short rate must not open a wider hole before
        // the unit ("23.4  t/s") than a long one ("105.2 t/s").
        const auto rows = rows_at(TokenStreamSparkline{c}.build(), 60);
        MAYA_TEST_CHECK(!rows.empty(), "sparkline rendered");
        const std::string& r = rows.front();
        const auto u = r.find("t/s");
        MAYA_TEST_CHECK(u != std::string::npos && u >= 2,
            "sparkline shows its unit @rate=" + std::to_string(rate));
        MAYA_TEST_CHECK(r[u - 1] == ' ' && r[u - 2] != ' ',
            "unit is exactly one space from its number @rate="
            + std::to_string(rate) + "  |" + r + "|");

        // …and the whole token must not SLIDE. A one-space gap is not enough
        // on its own: pad the token on its tail and the gap stays correct
        // while the number and unit drift left together as the value shrinks
        // ("0.0 t/s" put the label two columns left of "105.2 t/s"). The rate
        // updates every frame, so a label that moves with the digit count
        // flickers under the reader's eye — exactly what the fixed-width
        // field exists to prevent. Pin the ABSOLUTE column of both the unit
        // and the spark: neither may depend on the magnitude.
        if (unit_col == std::string::npos) {
            unit_col  = u;
            spark_col = r.find_first_not_of(' ', u + 3);
        } else {
            MAYA_TEST_CHECK(u == unit_col,
                "unit holds its column across magnitudes (was col "
                + std::to_string(unit_col) + ", now " + std::to_string(u)
                + ") @rate=" + std::to_string(rate) + "  |" + r + "|");
            MAYA_TEST_CHECK(r.find_first_not_of(' ', u + 3) == spark_col,
                "spark holds its column across magnitudes @rate="
                + std::to_string(rate) + "  |" + r + "|");
        }
    }
    std::println("  clean across 6 rate magnitudes x widths 20..60");
    std::println("  unit glued one space to its number at every magnitude");
    std::println("  unit + spark hold fixed columns as the rate ticks");
    std::println("  PASS");
}

TEST_CASE("spacing: the shortcut row sheds without stranding spacing") {
    std::println("=== test_spacing_shortcut_row ===");
    ShortcutRow::Config c;
    c.bindings = {
        {.key = "^K", .label = "palette", .priority = 100},
        {.key = "^J", .label = "threads", .priority = 90},
        {.key = "^R", .label = "review",  .priority = 80},
        {.key = "^S", .label = "smart",   .priority = 70},
        {.key = "^C", .label = "quit",    .priority = 60},
    };
    sweep(ShortcutRow{c}.build(), "shortcut_row", 20, 200);
    std::println("  clean across widths 20..200");
    std::println("  PASS");
}

// ── The width-derivation proof ───────────────────────────────────────────
//
// Tier 2 of the fix: a spacing literal's WIDTH must be computed from the
// literal, never restated as an integer beside it. status_bar.hpp carried
// `kSepW = 7` next to `"   ·   "`; narrowing the separator left the constant
// reserving four columns nobody painted, and the title chip over-shed on
// exactly the narrow terminals the shed ladder exists for.
//
// These are compile-time facts, asserted at runtime so the intent is
// discoverable from the test suite rather than only from a static_assert.
TEST_CASE("spacing: declared widths are derived, not hand-copied") {
    std::println("=== test_spacing_derived_widths ===");

    // str_width is the primitive that makes derivation possible.
    static_assert(unicode::str_width(" ") == 1);
    static_assert(unicode::str_width("   ") == 3);
    static_assert(unicode::str_width(" \xc2\xb7 ") == 3, "· is 1 column");
    static_assert(unicode::str_width("\xe2\x9a\xa1") == 2, "⚡ is WIDE");
    static_assert(unicode::str_width("") == 0);

    // Each widget's declared width matches its painted literal, by
    // construction — these would be tautologies if the constants were
    // derived and REAL assertions if someone reverts one to a literal int.
    MAYA_TEST_CHECK(StatusBar::kSepW == unicode::str_width(StatusBar::kSep),
                    "status bar separator width is derived");
    // The sparkline chip's fixed overhead is its rate token PLUS the two
    // columns the removed ⚡ used to occupy. The glyph is gone from the paint
    // but the chip still spends its width, so that dropping a decorative
    // icon did not reflow every segment to its right (the spark, the ·, the
    // CTX gauge) two columns leftward. Still derived rather than hand-copied
    // — that is what made the reservation a one-line change.
    MAYA_TEST_CHECK(
        TokenStreamSparkline::kFixedCols
            == TokenStreamSparkline::kRateTokCols
             + TokenStreamSparkline::kBoltCols,
        "sparkline fixed overhead is derived");
    // The rate token holds the widest number, its single space, the unit,
    // and one trailing column — so the unit is never flush against either
    // the number or the spark.
    MAYA_TEST_CHECK(
        TokenStreamSparkline::kRateTokCols
            == TokenStreamSparkline::kRateNumCols + 1
             + unicode::str_width(TokenStreamSparkline::kUnit) + 1,
        "sparkline rate token width is derived");
    MAYA_TEST_CHECK(
        unicode::str_width(TokenStreamSparkline::kUnit) == 3,
        "the unit literal carries no baked-in padding");

    // Malformed UTF-8 degrades rather than crashing — ids and titles reach
    // these widgets from provider APIs.
    MAYA_TEST_CHECK(unicode::str_width("\xff\xfe") == 2,
                    "stray bytes count as one column each");
    MAYA_TEST_CHECK(unicode::str_width("\xe2\x9a") == 2,
                    "truncated sequence does not overrun");
    std::println("  PASS");
}
