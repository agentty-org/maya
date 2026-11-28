// test_math.cpp — coverage for the LaTeX math markdown extension.
//
// Two layers:
//   1. The standalone TeX typesetter (widget/agentty/markdown/tex_math.hpp): assert on
//      the exact typeset cell grid for representative formulae — deterministic,
//      no layout engine involved.
//   2. Integration: `$…$`, `\(…\)`, `$$…$$` and ```math fences flow through the
//      full markdown() path and produce a renderable Element without crashing.
//
// Uses MAYA_TEST_CHECK (not assert) because CMake builds tests -DNDEBUG, which
// strips assert() — see tests/check.hpp.

#include "agtest.hpp"

#include "check.hpp"

#include "maya/widget/agentty/markdown/tex_math.hpp"
#include "maya/widget/agentty/markdown.hpp"
#include "maya/text/unicode_width.hpp"
#include "maya/render/canvas.hpp"
#include "maya/render/renderer.hpp"
#include "maya/render/serialize.hpp"
#include "maya/style/theme.hpp"

#include <print>
#include <string>
#include <string_view>
#include <vector>

using namespace maya;

// Render a typeset Box to a vector of UTF-8 row strings (skipping wide-glyph
// continuation cells, trimming trailing blanks) for exact comparison.
static std::vector<std::string> grid(std::string_view latex, bool display) {
    texmath::MathPalette pal;
    texmath::Box b = texmath::typeset(latex, pal, display);
    std::vector<std::string> rows;
    for (int r = 0; r < b.rows; ++r) {
        std::string line;
        for (int c = 0; c < b.cols; ++c) {
            const auto& cell = b.at(r, c);
            if (cell.len) line.append(cell.bytes.data(), cell.len);
        }
        while (!line.empty() && line.back() == ' ') line.pop_back();
        rows.push_back(std::move(line));
    }
    return rows;
}

static std::string joined(std::string_view latex, bool display) {
    auto rows = grid(latex, display);
    std::string out;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (i) out += '\n';
        out += rows[i];
    }
    return out;
}

// ── 1. typesetter unit tests ─────────────────────────────────────────────────

TEST_CASE("inline scripts") {
    // Simple exponent / index fold to Unicode super/subscript, staying 1 row.
    CHECK(joined("x^2", false)     == "x\u00b2",       "x^2 -> x super2");
    CHECK(joined("a_1", false)     == "a\u2081",       "a_1 -> a sub1");
    // Binary operators now breathe (TeX-style thin space around '+').
    CHECK(joined("x^2 + y^2", false) == "x\u00b2 + y\u00b2", "sum of squares spaced");
    // A multi-char exponent that can't fold stays legible (superscript box).
    auto rows = grid("e^{x+1}", false);
    CHECK(rows.size() >= 1, "e^{x+1} produced rows");
}

TEST_CASE("escaped punctuation prints itself") {
    // TeX reserves these five, so printing one means backslashing it. They
    // stand for THEMSELVES — dropping the backslash is the whole job.
    //
    // They used to fall through to the unknown-control-word arm and render
    // as a literal "\\_", which is worse than no math support: the escape is
    // invisible to whoever wrote the source (\_ reads as an underscore) and
    // the reader sees a stray backslash they cannot account for. A real
    // agentty answer rendered `bright\_black` verbatim, backslash included.
    CHECK(joined("bright\\_black", false) == "bright_black", "\\_ is _");
    CHECK(joined("50\\%", false)          == "50%",          "\\% is %");
    CHECK(joined("a\\&b", false)          == "a&b",          "\\& is &");
    CHECK(joined("\\#1", false)           == "#1",           "\\# is #");
    CHECK(joined("x\\$y", false)          == "x$y",          "\\$ is $");

    // \_ is the common one because _ is TeX's subscript operator and our
    // own output is full of snake_case C++ identifiers. Escaping it must
    // NOT disturb the unescaped operator.
    CHECK(joined("a_1", false) == "a\u2081", "bare _ still subscripts");
}

TEST_CASE("greek and symbols") {
    CHECK(joined("\\alpha", false) == "\u03b1", "alpha");
    CHECK(joined("\\Sigma", false) == "\u03a3", "Sigma");
    CHECK(joined("\\infty", false) == "\u221e", "infty");
    CHECK(joined("\\pi", false)    == "\u03c0", "pi");
    // relation gets air around it: α ≤ β
    CHECK(joined("\\alpha \\leq \\beta", false) == "\u03b1 \u2264 \u03b2", "alpha <= beta spaced");
    CHECK(joined("x \\in \\mathbb{R}", false).find("\u2208") != std::string::npos,
          "in operator present");
    CHECK(joined("x \\in \\mathbb{R}", false).find("\u211d") != std::string::npos,
          "blackboard R present");
}

TEST_CASE("fraction") {
    // \frac{a}{b} typesets to 3 rows: num / bar / den, axis on the bar.
    auto rows = grid("\\frac{a}{b}", true);
    CHECK(rows.size() == 3, "fraction is 3 rows");
    CHECK(rows[0] == "a", "numerator row");
    CHECK(rows[1] == "\u2500", "bar row is box-drawing horizontal");
    CHECK(rows[2] == "b", "denominator row");
    // The bar widens to the max of num/den width.
    auto wide = grid("\\frac{n+1}{2}", true);
    CHECK(wide[1].find("\u2500") != std::string::npos, "wide bar drawn");
}

TEST_CASE("sqrt") {
    // \sqrt{x} has a vinculum row above and a radical stroke on the baseline.
    auto rows = grid("\\sqrt{x}", true);
    CHECK(rows.size() == 2, "sqrt is 2 rows");
    CHECK(rows[0].find("\u203e") != std::string::npos, "vinculum overline present");
    CHECK(rows[1].find("\u221a") != std::string::npos, "radical sign present");
    CHECK(rows[1].find("x") != std::string::npos, "radicand under the stroke");
}

TEST_CASE("bigop limits") {
    // Display \sum with limits stacks upper / operator / lower (3 rows).
    auto rows = grid("\\sum_{i=1}^{n}", true);
    CHECK(rows.size() == 3, "display sum has 3 rows (upper/op/lower)");
    CHECK(rows[0].find("n") != std::string::npos, "upper limit on top");
    CHECK(rows[1].find("\u2211") != std::string::npos, "sigma on the axis row");
    CHECK(rows[2].find("i") != std::string::npos, "lower limit on bottom");

    // \int keeps limits as scripts even in display mode (integral convention).
    auto integ = grid("\\int_0^1", true);
    CHECK(integ[integ.size()/2].find("\u222b") != std::string::npos,
          "integral sign present");
}

TEST_CASE("matrix") {
    // 2x2 pmatrix: 2 rows of content wrapped in tall parens.
    auto rows = grid("\\begin{pmatrix} a & b \\\\ c & d \\end{pmatrix}", true);
    CHECK(rows.size() == 2, "2x2 matrix has 2 content rows");
    CHECK(rows[0].find("a") != std::string::npos && rows[0].find("b") != std::string::npos,
          "first matrix row has a,b");
    CHECK(rows[1].find("c") != std::string::npos && rows[1].find("d") != std::string::npos,
          "second matrix row has c,d");
    // tall paren pieces present
    CHECK(rows[0].find("\u239b") != std::string::npos || rows[0].find("(") != std::string::npos,
          "left bracket present");
}

TEST_CASE("array rules") {
    // array with a column rule {c|c}: the vertical bar must render as a real
    // box-drawing │ separating the columns, NOT leak as literal "c|c".
    auto rows = grid(
        "\\left[\\begin{array}{c|c} A & B \\\\ C & D \\end{array}\\right]", true);
    std::string all;
    for (auto& r : rows) all += r + "\n";
    CHECK(all.find("c|c") == std::string::npos, "column spec not leaked as text");
    CHECK(all.find("\u2502") != std::string::npos, "vertical column rule drawn");
    CHECK(all.find("A") != std::string::npos && all.find("D") != std::string::npos,
          "array cells present");
    CHECK(all.find("hline") == std::string::npos, "no leaked hline macro");

    // \hline must render as a horizontal box-drawing rule, not literal text.
    auto hl = grid(
        "\\begin{array}{cc} a & b \\\\ \\hline c & d \\end{array}", true);
    std::string allh;
    for (auto& r : hl) allh += r + "\n";
    CHECK(allh.find("hline") == std::string::npos, "hline consumed, not printed");
    CHECK(allh.find("\u2500") != std::string::npos, "horizontal rule drawn");

    // The partitioned-matrix stress case from the docs: a c|c array with an
    // \hline, wrapped in \left[…\right]. Must not leak spec/macro text.
    auto blk = joined(
        "M = \\left[\\begin{array}{c|c} A & B \\\\ \\hline C & D \\end{array}\\right]",
        true);
    CHECK(blk.find("c|c")   == std::string::npos, "block array spec not leaked");
    CHECK(blk.find("hline") == std::string::npos, "block array hline not leaked");
    CHECK(blk.find("begin") == std::string::npos, "block array begin not leaked");
    CHECK(blk.find("\u2502") != std::string::npos, "block array has a vertical rule");
    CHECK(blk.find("\u2500") != std::string::npos, "block array has a horizontal rule");
}

TEST_CASE("quadratic formula") {
    // The canonical stress case: real fraction + radical + scripts + \pm.
    auto rows = grid("\\frac{-b \\pm \\sqrt{b^2-4ac}}{2a}", true);
    CHECK(rows.size() == 4, "quadratic formula lays out in 4 rows");
    // \pm glyph and the radical both survive.
    std::string all;
    for (auto& r : rows) all += r;
    CHECK(all.find("\u00b1") != std::string::npos, "plus-minus present");
    CHECK(all.find("\u221a") != std::string::npos, "radical present");
    CHECK(all.find("\u2500") != std::string::npos, "fraction bar present");
}

TEST_CASE("unknown macro degrades") {
    // An unrecognised control word must NOT crash and should surface its name.
    auto rows = grid("\\foobarbaz", false);
    CHECK(!rows.empty(), "unknown macro still produces output");
    CHECK(rows[0].find("foobarbaz") != std::string::npos, "unknown macro shows its name");
}

TEST_CASE("linearize inline") {
    texmath::MathPalette pal;
    // inline fraction collapses to a/b, parenthesizing compound parts
    CHECK(texmath::linearize("\\frac{a}{b}", pal) == "a/b", "a/b");
    CHECK(texmath::linearize("\\frac{a+b}{2}", pal) == "(a + b)/2",
          "compound numerator parenthesized");
    CHECK(texmath::linearize("\\frac{\\sqrt{2}}{2}", pal) == "\u221a2/2",
          "radical numerator collapses");
    // scripts fold to Unicode
    CHECK(texmath::linearize("x^2", pal) == "x\u00b2", "inline x^2 folds");
    // unfoldable script uses caret notation, stays one row
    CHECK(texmath::linearize("e^{i\\pi}", pal) == "e^i\u03c0", "caret fallback");
}

TEST_CASE("spacing quality") {
    // Binary operators and relations get one cell of air.
    CHECK(joined("a+b", false)   == "a + b", "binary op spaced");
    CHECK(joined("a=b", false)   == "a = b", "relation spaced");
    // Implicit multiplication stays tight.
    CHECK(joined("2x", false)    == "2x",    "implicit mult tight");
    CHECK(joined("abc", false)   == "abc",   "adjacent letters tight");
    // ASCII '-' becomes a real minus sign U+2212.
    CHECK(joined("a-b", false)   == "a \u2212 b", "minus is U+2212");
    // A comma gets a trailing space, no leading.
    CHECK(joined("a,b", false)   == "a, b",  "comma spacing");
}

TEST_CASE("radical join") {
    // The radical stroke sits immediately left of the radicand (no gap).
    auto rows = grid("\\sqrt{x}", true);
    CHECK(rows.size() == 2, "sqrt is 2 rows");
    // baseline row is exactly "\u221ax" (stroke then radicand, adjacent)
    CHECK(rows[1] == "\u221ax", "radical hugs radicand");
}

TEST_CASE("norm and braces") {
    // Norm delimiters: \|, \lVert/\rVert, \Vert all fold to ‖; the single-bar
    // \lvert/\rvert/\vert fold to ∣. Previously these leaked as raw LaTeX.
    CHECK(joined("\\|x\\|", false) == "\u2016x\u2016", "\\| folds to ‖");
    CHECK(joined("\\lVert v \\rVert", false) == "\u2016v\u2016", "lVert/rVert fold");
    CHECK(joined("\\lvert a \\rvert", false) == "\u2223a\u2223", "lvert/rvert fold");

    // \overline draws a real top rule spanning the whole body (multi-char).
    auto ol = grid("\\overline{AB}", true);
    CHECK(ol.size() == 2, "overline is 2 rows");
    CHECK(ol[0] == "\u203e\u203e", "overline rule spans body width");
    CHECK(ol[1] == "AB", "overline body below the rule");

    // \underline draws a bottom rule.
    auto ul = grid("\\underline{xy}", true);
    CHECK(ul.size() == 2, "underline is 2 rows");
    CHECK(ul[0] == "xy", "underline body above the rule");

    // \underbrace renders a brace + label instead of dumping the macro name.
    auto ub = joined("\\underbrace{a+b}_{S}", true);
    CHECK(ub.find("underbrace") == std::string::npos, "underbrace macro consumed");
    CHECK(ub.find('S') != std::string::npos, "underbrace label rendered");
}

TEST_CASE("text fonts and arrows") {
    // \texttt / \textrm / \textbf etc. must render their body as text, NOT
    // leak the macro name (previously `\texttt{expected}` showed "texttt"
    // + italic body).
    CHECK(joined("\\texttt{expected}", false) == "expected",
          "texttt renders body, no leaked macro name");
    CHECK(joined("\\textbf{ok}", false) == "ok", "textbf body rendered");
    CHECK(joined("\\text{parse}", false).find("text") == std::string::npos
          || joined("\\text{parse}", false) == "parse", "text body only");
    // The exact failing case from the report: a labeled-arrow pipeline with
    // \texttt operands must not spill raw LaTeX. In display mode the label
    // sits ABOVE the arrow, so check across all rows.
    auto pipe_rows = grid(
        "\\texttt{parse} \\xrightarrow{expected} \\texttt{render}", true);
    std::string pipe;
    for (const auto& r : pipe_rows) { pipe += r; pipe += '\n'; }
    CHECK(pipe.find("xrightarrow") == std::string::npos,
          "xrightarrow macro consumed, not literal");
    CHECK(pipe.find("texttt") == std::string::npos, "texttt consumed");
    CHECK(pipe.find("parse") != std::string::npos
          && pipe.find("render") != std::string::npos, "operands present");
    CHECK(pipe.find("\u2192") != std::string::npos, "arrow glyph rendered");
    CHECK(pipe.find("expected") != std::string::npos, "arrow label rendered");

    // Inline (textstyle) xrightarrow collapses to one row: `label─→`.
    auto inl = grid("a \\xrightarrow{f} b", false);
    CHECK(inl.size() == 1, "inline labeled arrow stays one row");
    CHECK(inl[0].find("\u2192") != std::string::npos, "inline arrow head present");
    CHECK(inl[0].find('f') != std::string::npos, "inline arrow label present");

    // Left variant points the other way.
    CHECK(joined("\\xleftarrow{g}", false).find("\u2190") != std::string::npos,
          "xleftarrow renders a left arrow");
}

// Measure a UTF-8 string's terminal display width the way maya's layout does.
static int display_width(const std::string& line) {
    int w = 0;
    std::size_t i = 0;
    while (i < line.size()) {
        unsigned char c0 = static_cast<unsigned char>(line[i]);
        std::size_t len = c0 >= 0xF0 ? 4 : c0 >= 0xE0 ? 3 : c0 >= 0xC0 ? 2 : 1;
        char32_t cp = c0;
        if (len == 2) cp = ((c0 & 0x1Fu) << 6) | (line[i+1] & 0x3Fu);
        else if (len == 3) cp = ((c0 & 0x0Fu) << 12) | ((line[i+1] & 0x3Fu) << 6) | (line[i+2] & 0x3Fu);
        else if (len == 4) cp = ((c0 & 0x07u) << 18) | ((line[i+1] & 0x3Fu) << 12) | ((line[i+2] & 0x3Fu) << 6) | (line[i+3] & 0x3Fu);
        w += unicode::char_width(cp, unicode::WidthMode::Modern);
        i += len;
    }
    return w;
}

// The load-bearing invariant: EVERY grid row of a typeset box measures to the
// SAME display width as the box's declared `cols`. When this breaks (e.g. a
// combining mark counted as a column, or a wide glyph as one), rows come out
// ragged and a surrounding border leaks into the margin.
TEST_CASE("width invariant") {
    const char* cases[] = {
        "\\vec{v}", "\\hat{x} + \\bar{y}", "\\dot{x} = \\tilde{v}",
        "\\begin{pmatrix} a & b \\\\ c & d \\end{pmatrix}",
        "\\begin{pmatrix} a & b \\\\ c & d \\end{pmatrix} \\cdot \\vec{v} = \\lambda \\vec{v}",
        "\\frac{-b \\pm \\sqrt{b^2-4ac}}{2a}",
        "\\sum_{i=1}^{n} i", "\\int_0^\\infty e^{-x^2} dx",
        "\\alpha \\leq \\beta \\in \\mathbb{R}",
        "\\|x\\|_2 \\leq \\lVert x \\rVert_1",
        "\\overline{AB} + \\underline{cd}",
        "\\underbrace{a+b+c}_{3} = \\overbrace{x+y}^{2}",
        "\\boxed{E = mc^2}",
        "\\texttt{parse} \\xrightarrow{expected} \\texttt{render}",
        "a \\xrightarrow[below]{above} b",
        "x \\xleftarrow{g} y",
        "\\left[\\begin{array}{c|c} A & B \\\\ \\hline C & D \\end{array}\\right]",
        "\\begin{array}{cc} a & b \\\\ \\hline c & d \\end{array}",
    };
    for (const char* tex : cases) {
        for (bool disp : {false, true}) {
            texmath::Box b = texmath::typeset(tex, texmath::MathPalette{}, disp);
            for (int r = 0; r < b.rows; ++r) {
                std::string line;
                for (int c = 0; c < b.cols; ++c) {
                    const auto& cell = b.at(r, c);
                    if (cell.len) line.append(cell.bytes.data(), cell.len);
                }
                CHECK(display_width(line) == b.cols,
                      "grid row display width must equal box.cols");
            }
        }
    }
}

// ── 2. markdown integration ──────────────────────────────────────────────────

// Full render at a realistic width; asserts it doesn't crash and produces
// non-zero height.
static int render_height(std::string_view src) {
    Element el = markdown(src);
    StylePool pool;
    Canvas canvas(80, /*h=*/2000, &pool);
    render_tree(el, canvas, pool, theme::native, /*auto_height=*/true);
    return content_height(canvas);
}

TEST_CASE("markdown inline math") {
    // $…$ and \(…\) both parse and render.
    CHECK(render_height("The value $x^2 + 1$ is positive.") > 0, "dollar inline renders");
    CHECK(render_height("Euler: \\(e^{i\\pi} + 1 = 0\\) is elegant.") > 0,
          "\\(...\\) inline renders");
    // Currency must NOT be eaten as math: "$5 and $10" stays prose.
    CHECK(render_height("It costs $5 and then $10 total.") > 0, "currency doesn't crash");
}

TEST_CASE("markdown display math") {
    // $$…$$ block typesets as a multi-row bordered box → taller than one line.
    int h = render_height("Before\n\n$$\n\\frac{n(n+1)}{2}\n$$\n\nAfter");
    CHECK(h >= 5, "display fraction block adds vertical rows");
    // ```math fence path
    int h2 = render_height("```math\n\\sum_{i=1}^{n} i\n```");
    CHECK(h2 >= 3, "math fence block renders multi-row");
    // SINGLE-LINE `$$ … $$` form (the shape chat models emit inline). Must
    // typeset as a real 2-D box, NOT fall through to a raw-LaTeX paragraph.
    int h3 = render_height("Before\n\n$$x = \\frac{-b \\pm \\sqrt{b^2-4ac}}{2a}$$\n\nAfter");
    CHECK(h3 >= 5, "one-line $$…$$ fraction typesets multi-row");
    // A single-line $$…$$ with no 2-D content still renders as a math box
    // (bordered), so it's taller than the same text as a bare paragraph.
    int hm = render_height("$$a^2 + b^2 = c^2$$");
    int hp = render_height("a^2 + b^2 = c^2");
    CHECK(hm > hp, "one-line $$…$$ renders as a bordered math block");
    // Guard rails: `$5` currency and inline `$$a$$ b $$c$$` must NOT become
    // a display block (no crash / no swallow). Just assert they render.
    CHECK(render_height("price is $5 today") >= 1, "currency not math");
    CHECK(render_height("$$a$$ and $$b$$ inline") >= 1, "ambiguous defers");
}

// ── driver ───────────────────────────────────────────────────────────────────

