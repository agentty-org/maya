// theme_discipline_test — the theme system's own guard rail.
//
// The rule: a widget names a ROLE, the theme decides what the role looks
// like. Every regression in this area has been the same shape — a colour
// literal in a Config default, evaluated at static-init with no theme in
// scope, silently pinning that widget to the palette it was compiled with.
// You cannot see it in review and you cannot see it until someone picks a
// theme and half the screen ignores them.
//
// So it is checked mechanically, against the source, on every build.
#include <maya/maya.hpp>
#undef NDEBUG
#include "agtest.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <print>
#include <regex>
#include <string>
#include <vector>

using namespace maya;
namespace fs = std::filesystem;

namespace {

// Literals that are legitimately NOT theme slots.
//
// A theme assigns colour to ROLES. These name something else, so a slot
// would be the wrong answer, not merely a stricter one:
//
//   * markdown/highlight.hpp — the named syntax decks (Monokai, ...). A
//     deck IS a palette; resolving it through the theme would make every
//     deck identical.
//   * file_tree / editor_tab_bar — language BRAND colours (Rust orange, Go
//     cyan). They identify a language, not a UI role, and are the same hue
//     in every editor precisely because they are not themeable.
//
// Each entry is a repo-relative SUFFIX, matched at a path boundary — see
// allowed(). Prefer Themed::brand(colour, "why") at the site over a new
// entry here: an exemption that travels with the colour cannot go stale,
// and states its reason where the next reader is already looking.
const std::vector<std::string> kAllowed = {
    "widget/markdown/highlight.hpp",
    "widget/markdown/render/highlight.cpp",
    "widget/file_tree.hpp",
    "widget/editor_tab_bar.hpp",
};

// True when `path` ends with `suffix` AT A PATH BOUNDARY.
//
// A bare suffix compare (what this used to do) exempts by spelling rather
// than by identity: "file_tree.hpp" also matched my_file_tree.hpp,
// scratch_file_tree.hpp, anything ending in those bytes. Every other
// failure mode in this file was deliberately chosen to fail CLOSED — the
// prefilter rule below is a whole essay about exactly that — and then the
// exemption list quietly failed open. A new widget could inherit an
// exemption it never asked for by picking a name ending the right way.
//
// So the match must consume a whole path component: either the suffix IS
// the path, or the character before it is a separator.
[[nodiscard]] bool ends_at_boundary(const std::string& path,
                                    const std::string& suffix) {
    if (path.size() < suffix.size()) return false;
    const std::size_t at = path.size() - suffix.size();
    if (path.compare(at, suffix.size(), suffix) != 0) return false;
    return at == 0 || path[at - 1] == '/' || path[at - 1] == '\\';
}

// Which kAllowed entries actually exempted a file this run. Populated by
// allowed(); checked for completeness after the walk, so an entry naming a
// file that no longer exists fails instead of rotting.
std::vector<std::string> matched_allowed;

[[nodiscard]] bool allowed(const std::string& path) {
    // Windows hands back backslashes; the entries are written with forward
    // slashes because that is how the repo spells them.
    std::string norm = path;
    for (char& c : norm)
        if (c == '\\') c = '/';
    for (const auto& a : kAllowed)
        if (ends_at_boundary(norm, a)) {
            if (!std::any_of(matched_allowed.begin(), matched_allowed.end(),
                             [&](const std::string& m) { return m == a; }))
                matched_allowed.push_back(a);
            return true;
        }
    return false;
}

// ── Prefilters ───────────────────────────────────────────────────────
//
// libstdc++'s std::regex is an interpreted backtracker with no literal
// prefix optimisation: it walks every position of every line even when the
// line cannot possibly match. Over a whole-tree sweep that is seconds, in
// the debug build these tests run in. A cheap find() first turns the
// common case (no match) into one memchr-backed scan.
//
// CORRECTNESS RULE: a prefilter must be strictly WEAKER than its regex —
// it may admit lines the regex rejects, NEVER the reverse. One that
// rejects a real hit silently switches the rule off and the suite stays
// green forever, which is the exact failure this file exists to catch.
[[nodiscard]] inline bool may_read_channel(const std::string& l) noexcept {
    return l.find(".r()") != std::string::npos
        || l.find(".g()") != std::string::npos
        || l.find(".b()") != std::string::npos;
}

[[nodiscard]] inline bool may_to_rgb(const std::string& l) noexcept {
    return l.find(".to_rgb()") != std::string::npos;
}

// ── Is this channel read guarded? ───────────────────────────────────────
//
// has_channels() establishes a fact about a colour that holds for the REST
// OF THE ENCLOSING BLOCK, not for one line. Checking a single line forces
// the marker onto the arithmetic itself, which is unreadable and fails open
// on the natural spelling:
//
//     if (!c.has_channels()) return c;
//     return LitColor::rgb(c.r() / 2, ...);   // correct, but flagged
//
// So track brace depth: a guard seen at depth d covers every line until the
// depth drops back below d, which is the scope where the fact still holds.
//
// A heuristic, not a parser — braces inside strings or comments skew the
// depth. Deliberately biased toward FALSE POSITIVES (a skew expires the
// guard early and flags a safe line): a nuisance failure costs a minute,
// a missed one ships #45 again.
class GuardScope {
public:
    void observe(const std::string& line) {
        // A guard is CODE. Reading it out of a comment is not a detail:
        // every one of these functions carries a comment explaining the
        // has_channels hazard, so honouring comments disarmed the scanner
        // in exactly the files that needed it most. anim::lerp sat under a
        // 20-line comment headed "Why this checks has_channels()" and was
        // waved through with the guard deleted.
        const auto first = line.find_first_not_of(" \t");
        const bool comment =
            first != std::string::npos
            && (line.compare(first, 2, "//") == 0
                || line.compare(first, 2, "/*") == 0
                || line.compare(first, 1, "*") == 0);

        // What counts as establishing "this colour has channels".
        //
        // has_channels() is the name, but a proven Kind::Rgb is the same
        // fact stated the long way, and the emission paths use it because
        // they must switch on the kind anyway:
        //
        //     case ColorKind::Rgb:            // canvas.cpp, grid_emit.cpp
        //     if (c.kind() == ColorKind::Rgb) // serialize.cpp
        //
        // Inside those, r()/g()/b() ARE channels. Recognising the branch
        // keeps the rule honest — the alternative is an allowlist of file
        // paths, which grants the exemption to the whole file forever
        // rather than to the one branch that earned it.
        const bool establishes =
            line.find("has_channels") != std::string::npos
            || line.find("ColorKind::Rgb") != std::string::npos
            || line.find("Kind::Rgb") != std::string::npos;

        if (!comment && establishes)
            guarded_depth_ = depth_;

        if (comment) return;   // braces in prose are not scope, either

        for (char c : line) {
            if (c == '{') ++depth_;
            else if (c == '}') {
                --depth_;
                if (guarded_depth_ >= 0 && depth_ < guarded_depth_)
                    guarded_depth_ = -1;
            }
        }
    }
    [[nodiscard]] bool guarded() const noexcept { return guarded_depth_ >= 0; }
private:
    int depth_ = 0;
    int guarded_depth_ = -1;
};

}  // namespace

// The scanner's own unit test.
//
// A source-grep guard is itself code, and a guard that silently stops
// guarding is worse than none — it reads as green forever. The line-scoped
// version of this check failed exactly that way: it accepted only a marker
// on the arithmetic line, so every correctly-written guard was a false
// positive and the obvious fix ("add // has_channels to the line") taught
// the wrong model of the predicate. Pin the scope rules directly.
TEST_CASE("theme discipline: the guard tracker is block-scoped") {
    auto scan = [](std::initializer_list<const char*> lines) {
        GuardScope s;
        std::vector<bool> guarded;
        for (const char* l : lines) {
            const std::string line{l};
            s.observe(line);
            guarded.push_back(s.guarded());
        }
        return guarded;
    };

    // A guard covers the rest of its block — the case that forced the ugly
    // inline marker in ui_theme.hpp.
    {
        const auto g = scan({
            "bool f(LitColor c) {",              // 0: no guard yet
            "    if (!c.has_channels()) return false;", // 1: guard established
            "    return c.r() + c.g() > 10;",    // 2: STILL guarded
            "}",                                 // 3: block closed
        });
        CHECK(!g[0], "no guard before has_channels is seen");
        CHECK(g[1],  "the guard line itself counts");
        CHECK(g[2],  "the guard must survive to the next line — this is the "
                     "whole point of tracking scope rather than lines");
        CHECK(!g[3], "the guard expires when its block closes");
    }

    // A guard inside a NESTED block does not leak out of it.
    {
        const auto g = scan({
            "void f() {",                        // 0
            "    if (x) {",                      // 1
            "        if (c.has_channels()) {}",  // 2: guarded at depth 2
            "    }",                             // 3: depth 1 — expired
            "    return c.r() * 2;",             // 4: unguarded again
            "}",                                 // 5
        });
        CHECK(g[2],  "guarded inside the nested block");
        CHECK(!g[3], "leaving the block drops the guard");
        CHECK(!g[4], "a sibling statement is NOT covered by it");
    }

    // Two functions in a row: the first one's guard must not cover the
    // second. This is the fail-open case that would hide a real bug.
    {
        const auto g = scan({
            "int a(LitColor c) {",
            "    if (!c.has_channels()) return 0;",
            "    return c.r() / 2;",
            "}",
            "int b(LitColor c) {",
            "    return c.r() / 2;",             // 5: MUST be flagged
            "}",
        });
        CHECK(g[2],  "the first function is guarded");
        CHECK(!g[5], "the next function must NOT inherit the guard");
    }

    // A guard in a COMMENT is not a guard — the load-bearing one. Every
    // function near this hazard documents it, so honouring comments
    // disarmed the scanner in precisely the files that needed it: lerp sat
    // under "// Why this checks has_channels()" and was waved through with
    // the guard itself deleted.
    {
        const auto g = scan({
            "// Why this checks has_channels() ...",
            "LitColor lerp(LitColor a, LitColor b, double t) {",
            "    return LitColor::rgb(mix(a.r(), b.r()));",
            "}",
        });
        CHECK(!g[0], "a comment mentioning has_channels establishes nothing");
        CHECK(!g[2], "so the body is UNGUARDED and must be flagged");
    }

    // A proven Kind::Rgb branch is the same fact stated the long way.
    {
        const auto g = scan({
            "switch (c.kind()) {",
            "    case ColorKind::Rgb:",
            "        return write(p, c.r(), c.g(), c.b());",
            "}",
        });
        CHECK(g[1], "case ColorKind::Rgb establishes channels");
        CHECK(g[2], "and covers the emission inside it");
    }
}

TEST_CASE("theme discipline: widgets name roles, not colours") {
    std::println("--- test_theme_discipline ---");

    // Every file that IMPLEMENTS a widget, not just the ones that declare
    // it. This used to be `include/maya/widget` and `.hpp` only, which left
    // the whole of src/widget unscanned — panel.cpp is 71K, and
    // markdown/render/ builds palettes, so a literal there is both likely
    // and completely unenforced. It was clean when this was widened, which
    // is the good case: the rule now HOLDS that, rather than hoping.
    //
    // include/maya/style is deliberately NOT here. It is where literals are
    // DEFINED (schemes.hpp is 57 themes of nothing but hex), so scanning it
    // would be a rule against the thing that file exists to do.
    const std::vector<fs::path> roots = {
        fs::path{MAYA_SOURCE_DIR} / "include" / "maya" / "widget",
        fs::path{MAYA_SOURCE_DIR} / "src" / "widget",
    };
    for (const auto& r : roots) REQUIRE(fs::exists(r));

    // A bare named/hex/indexed colour, anywhere it is USED rather than
    // described. Color::slot(), Color::default_color() and computed
    // Color::rgb(expr) are all fine — the first is the point, the second is
    // "the terminal's own", and the third is arithmetic (gradients, hashes).
    // NOTE the rgb/hsl arms. This pattern used to list hex( and indexed(
    // but NOT rgb(, which is the spelling most widgets actually reach for
    // — so tool_body_preview's diff greens and status_bar's crimson rail
    // sat here unflagged through every run of this test. A rule that names
    // the constructors by hand has to name ALL of them; the omission is
    // invisible precisely because the test still passes.
    const std::regex lit{
        R"(Color::(black|red|green|yellow|blue|magenta|cyan|white|bright_\w+)\(\))"
        R"(|Color::hex\(0x)"
        R"(|Color::rgb\(\s*[0-9])"
        R"(|Color::rgb\(\s*0x)"
        R"(|Color::hsl\(\s*[0-9])"
        R"(|Color::indexed\()"};

    std::vector<std::string> offenders;
    int scanned = 0;

    // A slot in a BACKGROUND position must be a surface slot.
    //
    // Ink slots (text, accents) move opposite to the canvas when a theme
    // flips polarity, so using one as a fill inverts: a tint that reads as a
    // subtle wash on a dark theme becomes a near-black slab on a light one.
    // The bulk conversion introduced five of these and none were visible in
    // review — both sides look deliberate in the diff.
    const std::regex ink_as_bg{
        R"((with_bg|_bg|\bbg\b|bgc)\s*[=(][^;]*ThemeSlot::)"
        R"((Text|Secondary|Muted|Primary|Accent|Info|Link|Success|Warning|Error|Placeholder|Cursor)\b)"};

    // Reading a Color's CHANNELS without resolving it first.
    //
    // A slot carries its enum in the red byte, so to_rgb() on an unresolved
    // slot reads the slot NUMBER as an ANSI index and returns a plausible,
    // wrong colour — never a diagnostic. Anything built on those bytes
    // (equality, luminance, interpolation, hashing) quietly answers nonsense.
    //
    // This is the second-order hazard of slots and it bit four times: the
    // sigil's colour comparison (every half-block took the mixed branch and
    // painted a dark slab on light themes), its cache key (all slots hashed
    // alike), the gradient ramp, and the grid backend's wire encoder. All
    // four were invisible until a light theme made them obvious.
    //
    // Matches `.to_rgb()` on a line that does not also resolve — the one
    // call that silently fabricates channels.
    const std::regex unresolved_to_rgb{R"(\.to_rgb\(\))"};

    for (const auto& root : roots)
    for (const auto& e : fs::recursive_directory_iterator(root)) {
        if (!e.is_regular_file()) continue;
        const auto ext = e.path().extension();
        if (ext != ".hpp" && ext != ".cpp") continue;
        const std::string path = e.path().string();
        if (allowed(path)) continue;
        ++scanned;

        std::ifstream in{e.path()};
        std::string line;
        int n = 0;
        GuardScope scope;
        while (std::getline(in, line)) {
            ++n;
            scope.observe(line);
            // Skip comments — examples in docs are not code.
            const auto first = line.find_first_not_of(" \t");
            if (first != std::string::npos
                && (line.compare(first, 2, "//") == 0
                    || line.compare(first, 1, "*") == 0)) continue;
            if (std::regex_search(line, lit))
                offenders.push_back(e.path().filename().string() + ":"
                                    + std::to_string(n) + "  " + line);
            if (std::regex_search(line, ink_as_bg))
                offenders.push_back(e.path().filename().string() + ":"
                                    + std::to_string(n)
                                    + "  (ink slot as background)  " + line);
            if (may_to_rgb(line)
                && std::regex_search(line, unresolved_to_rgb)
                && line.find("resolve") == std::string::npos)
                offenders.push_back(e.path().filename().string() + ":"
                                    + std::to_string(n)
                                    + "  (to_rgb without resolve)  " + line);
        }
    }

    // Guard the guard: if the walk found nothing to scan, the test is
    // passing for the wrong reason. The floor tracks the widget count
    // loosely — high enough that a broken root or a bad extension filter
    // trips it, low enough not to churn when a widget is added or removed.
    assert(scanned > 200);

    // An exemption that matches nothing is a rule that has quietly switched
    // itself off. The file it named was renamed, moved, or deleted, and the
    // entry now sits there looking like coverage while exempting nobody —
    // or worse, waits to exempt the next file that happens to land at that
    // path. Same failure shape as a prefilter that is stronger than its
    // regex: the suite stays green and the rule is gone.
    {
        std::vector<std::string> unused;
        for (const auto& a : kAllowed)
            if (!std::any_of(matched_allowed.begin(), matched_allowed.end(),
                             [&](const std::string& m) { return m == a; }))
                unused.push_back(a);
        if (!unused.empty()) {
            std::println("stale kAllowed entr{}:",
                         unused.size() == 1 ? "y" : "ies");
            for (const auto& u : unused)
                std::println("   {}  (matches no file under widget/)", u);
            std::println("\nThe file moved or went away. Drop the entry, or fix");
            std::println("its path — an exemption matching nothing exempts nobody");
            std::println("and silently waits to exempt whatever lands there next.");
        }
        assert(unused.empty());
    }

    if (!offenders.empty()) {
        std::println("{} widget colour literal(s) that should be theme slots:",
                     offenders.size());
        for (std::size_t i = 0; i < offenders.size() && i < 25; ++i)
            std::println("   {}", offenders[i]);
        std::println("\nUse Color::slot(ThemeSlot::X) so the colour follows the");
        std::println("user's theme. If the literal is genuinely not a UI role");
        std::println("(a language brand colour, a named syntax deck), say so at");
        std::println("the site with Themed::brand(colour, \"why\") — that cannot");
        std::println("go stale the way a path in kAllowed can.");
    }
    assert(offenders.empty());

    std::println("PASS ({} widget headers scanned)\n", scanned);
}

// Channel arithmetic, across the WHOLE tree.
//
// This is a separate pass from the one above because it has a different
// scope, and getting that wrong is how #45 survived its own fix. The
// colour-literal rule is genuinely widget-shaped: a widget must name a
// role. But reading a palette index as a colour channel is a hazard
// wherever a colour is touched — and the ORIGINAL bug was in
// include/maya/core/animation.hpp, which the widget-only walk never looked
// at. A guard that cannot see the file the bug was in is not a guard.
//
// The rule: resolving a slot gets you a LitColor, which is PAINTABLE but
// not necessarily NUMERIC. Only Kind::Rgb has channels; Named and Indexed
// keep a PALETTE INDEX in the r_ byte with g_/b_ zero, and Default has
// nothing at all. theme::native states every slot as Named or Default on
// purpose, so that the user's own palette reaches the screen — which makes
// every unguarded blend wrong under precisely the theme nobody tests.
//
//     bright_black -> Named(8) -> lerp -> rgb(8,0,0) -> "38;2;8;0;0"
//
// Legitimate reads are EMISSION of a palette index, which spells itself
// index(), and arithmetic guarded by has_channels() — tracked across the
// enclosing block by GuardScope.
TEST_CASE("theme discipline: no unguarded channel arithmetic anywhere") {
    std::println("--- channel arithmetic sweep ---");

    // Every directory that touches a colour, not just the widgets.
    const std::vector<fs::path> roots = {
        fs::path{MAYA_SOURCE_DIR} / "include" / "maya",
        fs::path{MAYA_SOURCE_DIR} / "src",
    };

    // ANY channel read — not just one adjacent to an operator.
    //
    // The first version of this regex required a [-+*/] next to the read.
    // That felt precise and was useless: the original #45 bug passes its
    // channels to a FUNCTION,
    //
    //     return LitColor::rgb(mix(a.r(), b.r()), ...);
    //
    // so no operator is in sight and the scanner walked straight past the
    // very line it was written for. Verified by deleting the lerp guard and
    // re-running the sweep: zero hits on animation.hpp.
    //
    // So flag EVERY read and make the legitimate ones say so. There are
    // exactly two legitimate shapes and both are already explicit:
    //
    //   * EMISSION of a palette index — spells itself index().
    //   * Arithmetic proven safe — carries has_channels() in its block.
    //
    // Anything else reads bytes whose meaning depends on a kind it never
    // checked.
    const std::regex channel_read{R"(\.[rgb]\(\))"};

    // color.hpp is the ONE file exempt, and only because it is the type
    // itself: its static_asserts read channels off a literal hex() (whose
    // kind is Rgb by construction, at compile time) and CanReadChannels is
    // a concept probe that never runs. A blend cannot hide here — there is
    // nothing in this file to blend WITH.
    const std::string self = "color.hpp";

    std::vector<std::string> offenders;
    int scanned = 0;

    for (const auto& root : roots) {
        if (!fs::exists(root)) continue;
        for (const auto& e : fs::recursive_directory_iterator(root)) {
            if (!e.is_regular_file()) continue;
            const auto ext = e.path().extension();
            if (ext != ".hpp" && ext != ".cpp") continue;
            if (e.path().filename().string() == self) continue;
            ++scanned;

            std::ifstream in{e.path()};
            std::string line;
            int n = 0;
            GuardScope scope;
            while (std::getline(in, line)) {
                ++n;
                scope.observe(line);
                const auto first = line.find_first_not_of(" \t");
                if (first != std::string::npos
                    && (line.compare(first, 2, "//") == 0
                        || line.compare(first, 1, "*") == 0))
                    continue;
                if (may_read_channel(line) && !scope.guarded()
                    && std::regex_search(line, channel_read))
                    offenders.push_back(
                        fs::relative(e.path(), MAYA_SOURCE_DIR).string() + ":"
                        + std::to_string(n) + "  " + line);
            }
        }
    }

    // Guard the guard: the widget-only version of this check scanned a
    // directory that did not contain the bug and passed forever.
    assert(scanned > 100);

    if (!offenders.empty()) {
        std::println("{} unguarded channel read(s):", offenders.size());
        for (std::size_t i = 0; i < offenders.size() && i < 25; ++i)
            std::println("   {}", offenders[i]);
        std::println("\nOnly Kind::Rgb has channels. Guard with has_channels(),");
        std::println("or spell a palette-index read index(). See #45.");
    }
    assert(offenders.empty());

    std::println("PASS ({} files scanned)\n", scanned);
}

// ── The Themed gate, checked by COMPILING ────────────────────────────────
//
// Every other rule in this file is a source scan. This one is different
// because the property is "does this fail to compile", and no scan and no
// static_assert can answer it:
//
//   - a `requires`-expression asks whether an expression is WELL-FORMED,
//     and Themed's literal gate is a consteval THROW. A throw is perfectly
//     well-formed right up until it is evaluated, so requires{} answers
//     true for exactly the case the gate rejects.
//   - the scanners above find literals by SPELLING. They cannot see whether
//     the type system would have caught one.
//
// So the instrument is a compiler. Each probe is a tiny TU that must
// compile, or must not, and the test asserts which.
//
// These are the executable half of the "WHERE Themed BELONGS" comment in
// style/color.hpp: that comment states the scope, the static_asserts there
// pin the type's shape, and these prove the behaviour at the boundary.
TEST_CASE("theme discipline: the Themed gate admits and refuses correctly") {
    std::println("--- test_themed_gate ---");

#if !defined(MAYA_CXX_COMPILER) || !defined(MAYA_CXX_GNULIKE)
    // Needs a GCC/Clang-style driver (-fsyntax-only, -I, exit code as the
    // answer). Skipped rather than faked: a probe that cannot run must not
    // report that the gate holds.
    std::println("SKIP (needs a gnu-like driver; MSVC spells these flags "
                 "differently)\n");
#else
    const fs::path cxx = MAYA_CXX_COMPILER;
    const fs::path inc = fs::path{MAYA_SOURCE_DIR} / "include";
    const fs::path tu  = fs::temp_directory_path() / "maya_themed_probe.cpp";

    // A host token shaped exactly like agentty's ui::Slot: it reaches the
    // live theme through a function pointer, so it is not constant-foldable.
    // And an impostor that opts IN to the same trait while pinning a
    // literal — the case the opt-in alone cannot distinguish.
    const std::string preamble = R"(
#include <maya/style/schemes.hpp>
#include <maya/style/theme.hpp>
using namespace maya;

struct RealToken {
    LitColor (*read)() noexcept;
    operator LitColor() const noexcept { return read(); }
    operator Color()   const noexcept { return read(); }
};
inline constexpr RealToken tok{
    +[]() noexcept -> LitColor { return theme::live().accent; }};

struct Impostor {
    constexpr operator Color() const noexcept { return Color::hex(0xDEADBE); }
};

template <> struct maya::is_theme_token<RealToken> : std::true_type {};
template <> struct maya::is_theme_token<Impostor>  : std::true_type {};

Color runtime_colour();
)";

    struct Probe {
        const char* name;
        bool        should_compile;
        const char* body;
    };

    const Probe probes[] = {
        // The type admits what it exists to admit.
        {"a slot-valued colour is accepted", true,
         "Themed t = Color::slot(ThemeSlot::Accent);"},

        // ...and refuses the thing agentty #45 was.
        {"a hex literal is refused", false,
         "Themed t = Color::hex(0x112233);"},
        {"a named literal is refused", false,
         "Themed t = Color::red();"},

        // The sanctioned exit, and its price.
        {"brand() admits a literal with a reason", true,
         R"(Themed t = Themed::brand(Color::hex(0xCE422B), "Rust brand orange");)"},
        {"brand() refuses a literal without one", false,
         "Themed t = Themed::brand(Color::hex(0xCE422B));"},

        // SCOPE (issue #2). The gate is consteval, so a runtime colour
        // cannot reach it. This is the property the reverted migration
        // (4b78928) ran into 22 files deep, and the reason maya's own
        // widget internals stay on Color. If this probe ever flips to
        // compiling, Themed has been widened into something that accepts
        // what it cannot judge.
        {"a RUNTIME colour cannot be Themed", false,
         "Themed t = runtime_colour();"},

        // TOKENS (issue #3). A genuine token passes; an impostor that
        // specialised the same trait but pins a literal does not.
        {"a genuine theme token is accepted", true,
         "Themed t = tok;"},
        {"an impostor token is refused", false,
         "Themed t = Impostor{};"},
    };

    int checked = 0;
    std::vector<std::string> wrong;

    for (const auto& p : probes) {
        {
            std::ofstream out{tu};
            REQUIRE(out.good());
            out << preamble << "\n" << p.body << "\n";
        }

        // -fsyntax-only: we are asking a question about the type system,
        // so there is no reason to pay for codegen.
#if defined(MAYA_CXX_SYSROOT)
        const std::string sysroot = std::string{" -isysroot \""} + MAYA_CXX_SYSROOT + "\" ";
#else
        const std::string sysroot = " ";
#endif
        // 2>/dev/null is a shell redirection; cmd.exe has no /dev/null, and
        // std::system on windows runs cmd — the probe would fail for a
        // syntax reason that has nothing to do with the type system under
        // test. NUL is the same device on windows.
#if defined(_WIN32)
        const std::string nullsink = "2>NUL";
#else
        const std::string nullsink = "2>/dev/null";
#endif
#if defined(_WIN32)
        // std::system runs `cmd /c <string>`. A string that STARTS with a
        // quote trips cmd's /C quote handling: it strips the outer pair and
        // then treats "C:\...\clang++.exe" as a quoted spec it cannot parse
        // after the second pair closes — every probe "fails" without the
        // compiler ever running. The known-good shape for cmd is the whole
        // command wrapped in ONE extra outer quote pair (what cmd /S exists
        // for); every inner path keeps its quotes and forward slashes keep
        // the C++ string escapes readable.
        const std::string inner = "\"" + cxx.string() + "\"" + sysroot
                                + "-std=c++26 -fsyntax-only -I \"" + inc.string()
                                + "\" \"" + tu.string() + "\" " + nullsink;
        const std::string cmd = "\"" + inner + "\"";
#else
        const std::string cmd = "\"" + cxx.string() + "\"" + sysroot
                              + "-std=c++26 -fsyntax-only -I \"" + inc.string()
                              + "\" \"" + tu.string() + "\" " + nullsink;
#endif
        const bool compiled = std::system(cmd.c_str()) == 0;
        ++checked;

        if (compiled != p.should_compile)
            wrong.push_back(std::string{p.name} + "  (expected "
                            + (p.should_compile ? "accept" : "REFUSE")
                            + ", got "
                            + (compiled ? "accept" : "REFUSE") + ")");
    }

    std::error_code ec;
    fs::remove(tu, ec);

    if (!wrong.empty()) {
        std::println("{} Themed gate probe(s) behaved wrongly:", wrong.size());
        for (const auto& w : wrong) std::println("   {}", w);
        std::println("\nThe gate is the compile-time half of theme discipline.");
        std::println("See the 'WHERE Themed BELONGS' comment in style/color.hpp");
        std::println("and commit 4b78928 before changing what it accepts.");
    }
    assert(wrong.empty());

    // A probe that never ran is not a passing probe. This also catches a
    // compiler path that exists but cannot build maya's headers at all —
    // in which case every probe would "REFUSE" and the refusal-expecting
    // ones would pass for entirely the wrong reason. The accept-expecting
    // probes are what make that impossible to miss.
    assert(checked == static_cast<int>(std::size(probes)));

    std::println("PASS ({} gate probes)\n", checked);
#endif
}
