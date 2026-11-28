#pragma once
// tex_math.hpp — a state-of-the-art LaTeX→terminal math typesetter.
//
// This is the "KaTeX for a cell grid" that powers maya's math markdown
// extension. It is provider-agnostic by construction: Claude, OpenAI, Gemini
// and every other model emit the SAME TeX source ($…$, $$…$$, \(…\), \[…\],
// or ```math fences), so a renderer that understands TeX understands them all.
//
// ── Why a 2D box model (and not string substitution) ────────────────────────
// Naive terminal math renderers flatten everything to one line: `a/b`, `x^2`,
// `sqrt(x)`. That falls apart the moment real math shows up — a fraction, a
// summation with limits, a matrix, a nested radical. maya instead ports TeX's
// box-and-glue idea to a character grid:
//
//   • Every sub-expression typesets into a `Box`: a rectangular grid of
//     styled cells plus an `axis` — the row index that sits on the math
//     baseline. Ascent = axis, descent = rows - axis - 1.
//   • Boxes compose. Horizontal concatenation aligns two boxes on their
//     shared axis (padding the shorter one above/below). A fraction stacks
//     numerator / rule / denominator and puts the axis ON the rule. A
//     superscript raises a box and shrinks the parent's implied axis; a
//     subscript lowers it. Roots draw a radical that grows with its radicand.
//
// The result: `\frac{-b\pm\sqrt{b^2-4ac}}{2a}` renders as a real three-row
// fraction with a real radical, in monochrome-safe Unicode, at any width.
//
// ── Scope ───────────────────────────────────────────────────────────────────
// Header-only, no deps beyond maya's Element/Style. Covers the LaTeX subset
// that actually appears in chat-model output: Greek, operators, relations,
// arrows, set/logic symbols, blackboard/cal letters, \frac \dfrac \tfrac,
// \sqrt (incl. \sqrt[n]), ^ _ groups, \sum \int \prod \lim with limits,
// \left…\right auto-sizing delimiters, \begin{matrix|pmatrix|bmatrix|cases},
// \text{…}, spacing, accents (\hat \bar \vec …), and ~200 named symbols.
// Unknown macros degrade gracefully to their name rather than aborting.

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../../../core/types.hpp"
#include "../../../style/style.hpp"
#include "../../../text/unicode_width.hpp"

namespace maya::texmath {

// ── Palette hooks ────────────────────────────────────────────────────────────
// The renderer resolves colors lazily so it stays decoupled from the markdown
// palette. Callers may override; defaults are terminal-safe.
struct MathPalette {
    Style normal;                         // variables / plain glyphs
    Style op       = Style{}.with_bold(); // operators + big operators
    Style num;                            // numbers
    Style rule;                           // fraction bars, radical strokes
    Style delim;                          // parens / brackets / braces
    Style text;                           // \text{…} escape hatch
};

// One typeset cell: a UTF-8 grapheme (1–4 bytes) and its style.
struct Cell {
    std::array<char, 4> bytes{};
    std::uint8_t        len = 0;
    Style               style;
    std::uint8_t        width = 1;  // display columns (2 for wide glyphs)
};

// A rectangular grid of cells plus the baseline (axis) row. Empty cells are
// rendered as spaces. This is the universal currency of the layout engine.
struct Box {
    int               rows = 1;
    int               cols = 0;
    int               axis = 0;               // baseline row (0-based)
    std::vector<Cell> grid;                   // rows*cols, row-major

    // TeX-style atom class of the WHOLE box, consulted by the sequence layout
    // to insert inter-atom spacing (a relation gets air around it, a binary
    // operator a thinner air, an open/close delimiter hugs its operand). Only
    // meaningful for leaf/simple boxes; composites reset to Ord.
    enum class Cls : std::uint8_t { Ord, Op, Bin, Rel, Open, Close, Punct, Inner };
    Cls klass = Cls::Ord;

    [[nodiscard]] int ascent()  const noexcept { return axis; }
    [[nodiscard]] int descent() const noexcept { return rows - axis - 1; }

    Cell&       at(int r, int c)       noexcept { return grid[static_cast<std::size_t>(r) * cols + c]; }
    const Cell& at(int r, int c) const noexcept { return grid[static_cast<std::size_t>(r) * cols + c]; }
};

// ── Public API ───────────────────────────────────────────────────────────────
// Typeset a LaTeX math string into a Box. `display=false` uses inline
// conventions (\textstyle: limits go to sub/superscript position, smaller
// fraction bars); `display=true` uses \displaystyle (limits above/below).
[[nodiscard]] Box typeset(std::string_view latex, const MathPalette& pal,
                          bool display);

// (render_math — typeset + flatten into a maya Element — lives in the sibling
// header tex_math_render.hpp, which pulls in the Element/builder machinery.
// This header stays dependency-light so the typesetter can be unit-tested
// against a plain cell grid.)

// ─────────────────────────────────────────────────────────────────────────────
//                              IMPLEMENTATION
// ─────────────────────────────────────────────────────────────────────────────
namespace detail {

inline Cell make_cell(std::string_view g, Style s, int w = 1) {
    Cell c;
    c.len = static_cast<std::uint8_t>(g.size() > 4 ? 4 : g.size());
    for (std::uint8_t i = 0; i < c.len; ++i) c.bytes[i] = g[i];
    c.style = s;
    c.width = static_cast<std::uint8_t>(w < 1 ? 1 : w);
    return c;
}

// Blank box of a given shape (spaces on the given axis).
inline Box blank(int rows, int cols, int axis, Style s) {
    Box b;
    b.rows = rows < 1 ? 1 : rows;
    b.cols = cols < 0 ? 0 : cols;
    b.axis = axis < 0 ? 0 : (axis >= b.rows ? b.rows - 1 : axis);
    b.grid.assign(static_cast<std::size_t>(b.rows) * b.cols, make_cell(" ", s));
    return b;
}

// A single-row box from a UTF-8 string. Each grapheme is one cell whose
// display width comes from maya's authoritative unicode::char_width, so the
// box's `cols` ALWAYS matches what the terminal (and maya's own layout /
// measure pass) will render. Combining marks (width 0) fold onto the
// preceding base cell rather than occupying a column of their own — without
// this, an accented glyph (v⃗, x̂) would over-count by one and push every
// following atom (and any surrounding border) one cell to the right.
inline Box hbox(std::string_view s, Style style) {
    std::vector<Cell> cells;
    std::size_t i = 0;
    while (i < s.size()) {
        unsigned char c0 = static_cast<unsigned char>(s[i]);
        std::size_t len = 1;
        if (c0 >= 0xF0) len = 4;
        else if (c0 >= 0xE0) len = 3;
        else if (c0 >= 0xC0) len = 2;
        if (i + len > s.size()) len = 1;
        std::string_view g = s.substr(i, len);
        // decode the codepoint for the width lookup
        char32_t cp = c0;
        if (len == 2) cp = ((c0 & 0x1Fu) << 6) |
                           (static_cast<unsigned char>(s[i + 1]) & 0x3Fu);
        else if (len == 3) cp = ((c0 & 0x0Fu) << 12) |
                           ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 6) |
                           (static_cast<unsigned char>(s[i + 2]) & 0x3Fu);
        else if (len == 4) cp = ((c0 & 0x07u) << 18) |
                           ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 12) |
                           ((static_cast<unsigned char>(s[i + 2]) & 0x3Fu) << 6) |
                           (static_cast<unsigned char>(s[i + 3]) & 0x3Fu);
        int w = maya::unicode::char_width(cp, maya::unicode::WidthMode::Modern);
        if (w == 0 && !cells.empty()) {
            // combining mark: append its bytes to the previous cell's grapheme
            // (up to the 4-byte Cell budget) so it renders composed and adds
            // no column. Overflow marks are simply dropped (rare in math).
            Cell& prev = cells.back();
            for (std::size_t k = 0; k < len && prev.len < 4; ++k)
                prev.bytes[prev.len++] = g[k];
            i += len;
            continue;
        }
        if (w == 0) w = 1;  // a leading combining mark with no base: show it
        cells.push_back(make_cell(g, style, w));
        i += len;
    }
    Box b;
    b.rows = 1;
    b.axis = 0;
    b.cols = 0;
    for (auto& c : cells) b.cols += c.width;
    // Re-expand wide cells: a width-2 cell occupies 2 grid columns (2nd blank).
    b.grid.reserve(static_cast<std::size_t>(b.cols));
    for (auto& c : cells) {
        b.grid.push_back(c);
        if (c.width == 2) b.grid.push_back(make_cell("", c.style, 1)); // continuation
    }
    return b;
}

// Stack `a` left of `b`, aligning on the shared axis. The joined axis is the
// max of the two ascents; rows below is the max of the two descents.
inline Box hcat(const Box& a, const Box& b) {
    if (a.cols == 0) return b;
    if (b.cols == 0) return a;
    int asc = std::max(a.ascent(), b.ascent());
    int desc = std::max(a.descent(), b.descent());
    int rows = asc + desc + 1;
    Box out = blank(rows, a.cols + b.cols, asc, Style{});
    auto blit = [&](const Box& src, int col0) {
        int top = asc - src.ascent();
        for (int r = 0; r < src.rows; ++r)
            for (int c = 0; c < src.cols; ++c)
                out.at(top + r, col0 + c) = src.at(r, c);
    };
    blit(a, 0);
    blit(b, a.cols);
    return out;
}

inline Box hcat3(const Box& a, const Box& b, const Box& c) {
    return hcat(hcat(a, b), c);
}

// Vertically stack boxes (top→bottom), centering narrower ones, and place the
// axis on row `axis_row`. Used by fractions, matrices, big operators.
inline Box vstack(const std::vector<Box>& parts, int axis_row, Style fill) {
    int cols = 0, rows = 0;
    for (auto& p : parts) { cols = std::max(cols, p.cols); rows += p.rows; }
    Box out = blank(rows, cols, axis_row, fill);
    int r0 = 0;
    for (auto& p : parts) {
        int c0 = (cols - p.cols) / 2;
        for (int r = 0; r < p.rows; ++r)
            for (int c = 0; c < p.cols; ++c)
                out.at(r0 + r, c0 + c) = p.at(r, c);
        r0 += p.rows;
    }
    return out;
}

// ── Named-symbol table ───────────────────────────────────────────────────────
// Maps a LaTeX control word (without the backslash) to its Unicode glyph.
// Covers the set that actually shows up in model output. Lookup is a linear
// scan over a constexpr sorted-ish array; the table is small and only touched
// once per macro during parse, so a hash map isn't worth the header weight.
struct SymEntry { std::string_view name; std::string_view glyph; };

inline constexpr SymEntry kSymbols[] = {
    // lowercase Greek
    {"alpha","\u03b1"},{"beta","\u03b2"},{"gamma","\u03b3"},{"delta","\u03b4"},
    {"epsilon","\u03b5"},{"varepsilon","\u03b5"},{"zeta","\u03b6"},{"eta","\u03b7"},
    {"theta","\u03b8"},{"vartheta","\u03d1"},{"iota","\u03b9"},{"kappa","\u03ba"},
    {"lambda","\u03bb"},{"mu","\u03bc"},{"nu","\u03bd"},{"xi","\u03be"},
    {"pi","\u03c0"},{"varpi","\u03d6"},{"rho","\u03c1"},{"varrho","\u03f1"},
    {"sigma","\u03c3"},{"varsigma","\u03c2"},{"tau","\u03c4"},{"upsilon","\u03c5"},
    {"phi","\u03c6"},{"varphi","\u03d5"},{"chi","\u03c7"},{"psi","\u03c8"},
    {"omega","\u03c9"},
    // uppercase Greek
    {"Gamma","\u0393"},{"Delta","\u0394"},{"Theta","\u0398"},{"Lambda","\u039b"},
    {"Xi","\u039e"},{"Pi","\u03a0"},{"Sigma","\u03a3"},{"Upsilon","\u03a5"},
    {"Phi","\u03a6"},{"Psi","\u03a8"},{"Omega","\u03a9"},
    // binary operators
    {"times","\u00d7"},{"div","\u00f7"},{"pm","\u00b1"},{"mp","\u2213"},
    {"cdot","\u00b7"},{"ast","\u2217"},{"star","\u22c6"},{"circ","\u2218"},
    {"bullet","\u2219"},{"oplus","\u2295"},{"ominus","\u2296"},{"otimes","\u2297"},
    {"oslash","\u2298"},{"odot","\u2299"},{"cap","\u2229"},{"cup","\u222a"},
    {"sqcap","\u2293"},{"sqcup","\u2294"},{"wedge","\u2227"},{"land","\u2227"},
    {"vee","\u2228"},{"lor","\u2228"},{"setminus","\u2216"},{"wr","\u2240"},
    {"amalg","\u2a3f"},{"dagger","\u2020"},{"ddagger","\u2021"},
    // relations
    {"leq","\u2264"},{"le","\u2264"},{"geq","\u2265"},{"ge","\u2265"},
    {"neq","\u2260"},{"ne","\u2260"},{"equiv","\u2261"},{"approx","\u2248"},
    {"cong","\u2245"},{"simeq","\u2243"},{"sim","\u223c"},{"propto","\u221d"},
    {"ll","\u226a"},{"gg","\u226b"},{"subset","\u2282"},{"supset","\u2283"},
    {"subseteq","\u2286"},{"supseteq","\u2287"},{"sqsubseteq","\u2291"},
    {"sqsupseteq","\u2292"},{"in","\u2208"},{"ni","\u220b"},{"notin","\u2209"},
    {"vdash","\u22a2"},{"dashv","\u22a3"},{"models","\u22a8"},{"perp","\u22a5"},
    {"mid","\u2223"},{"parallel","\u2225"},{"asymp","\u224d"},{"doteq","\u2250"},
    {"prec","\u227a"},{"succ","\u227b"},{"preceq","\u2aaf"},{"succeq","\u2ab0"},
    // arrows
    {"leftarrow","\u2190"},{"gets","\u2190"},{"rightarrow","\u2192"},{"to","\u2192"},
    {"leftrightarrow","\u2194"},{"Leftarrow","\u21d0"},{"Rightarrow","\u21d2"},
    {"implies","\u27f9"},{"impliedby","\u27f8"},{"iff","\u27fa"},
    {"Leftrightarrow","\u21d4"},{"mapsto","\u21a6"},{"uparrow","\u2191"},
    {"downarrow","\u2193"},{"updownarrow","\u2195"},{"nearrow","\u2197"},
    {"searrow","\u2198"},{"swarrow","\u2199"},{"nwarrow","\u2196"},
    {"longrightarrow","\u27f6"},{"longleftarrow","\u27f5"},{"hookrightarrow","\u21aa"},
    {"hookleftarrow","\u21a9"},
    // misc symbols
    {"infty","\u221e"},{"partial","\u2202"},{"nabla","\u2207"},{"forall","\u2200"},
    {"exists","\u2203"},{"nexists","\u2204"},{"emptyset","\u2205"},{"varnothing","\u2205"},
    {"neg","\u00ac"},{"lnot","\u00ac"},{"top","\u22a4"},{"bot","\u22a5"},
    {"angle","\u2220"},{"triangle","\u25b3"},{"square","\u25a1"},{"Box","\u25a1"},
    {"diamond","\u22c4"},{"Diamond","\u25c7"},{"aleph","\u2135"},{"hbar","\u210f"},
    {"ell","\u2113"},{"Re","\u211c"},{"Im","\u2111"},{"wp","\u2118"},
    {"prime","\u2032"},{"degree","\u00b0"},{"deg","\u00b0"},{"surd","\u221a"},
    {"flat","\u266d"},{"sharp","\u266f"},{"natural","\u266e"},{"clubsuit","\u2663"},
    {"diamondsuit","\u2662"},{"heartsuit","\u2661"},{"spadesuit","\u2660"},
    {"dots","\u2026"},{"ldots","\u2026"},{"cdots","\u22ef"},{"vdots","\u22ee"},
    {"ddots","\u22f1"},{"therefore","\u2234"},{"because","\u2235"},{"checkmark","\u2713"},
    {"complement","\u2201"},{"backslash","\\"},
    {"Vert","\u2016"},{"lVert","\u2016"},{"rVert","\u2016"},
    {"vert","\u2223"},{"lvert","\u2223"},{"rvert","\u2223"},
    {"nmid","\u2224"},{"nsubseteq","\u2288"},{"nsupseteq","\u2289"},
    {"subsetneq","\u228a"},{"supsetneq","\u228b"},{"leqslant","\u2a7d"},
    {"geqslant","\u2a7e"},{"coloneqq","\u2254"},{"triangleq","\u225c"},
    {"nleq","\u2270"},{"ngeq","\u2271"},{"asymp","\u224d"},{"bowtie","\u22c8"},
    // blackboard-bold shortcuts for common sets
    {"mathbb{R}","\u211d"},{"R","\u211d"},{"mathbb{N}","\u2115"},{"N","\u2115"},
    {"mathbb{Z}","\u2124"},{"Z","\u2124"},{"mathbb{Q}","\u211a"},{"Q","\u211a"},
    {"mathbb{C}","\u2102"},{"C","\u2102"},{"mathbb{P}","\u2119"},{"mathbb{H}","\u210d"},
    // named functions render as themselves (upright) — handled by parser, but
    // a few common ones alias here for completeness of the fallback path.
    {"quad","  "},{"qquad","    "},{","," "},{":"," "},{";"," "},{"!",""},
    {" "," "},{"lbrace","{"},{"rbrace","}"},{"langle","\u27e8"},{"rangle","\u27e9"},
    {"lceil","\u2308"},{"rceil","\u2309"},{"lfloor","\u230a"},{"rfloor","\u230b"},
};

inline std::string_view lookup_symbol(std::string_view name) {
    for (const auto& e : kSymbols) if (e.name == name) return e.glyph;
    return {};
}

// Upright multi-letter operator names (\sin, \log, …). These render as roman
// text, unlike single-letter variables which render italic-by-convention.
inline bool is_named_op(std::string_view w) {
    static constexpr std::string_view kOps[] = {
        "sin","cos","tan","cot","sec","csc","sinh","cosh","tanh","coth",
        "arcsin","arccos","arctan","log","ln","lg","exp","det","dim","ker",
        "deg","gcd","hom","arg","max","min","sup","inf","lim","limsup",
        "liminf","mod","bmod","Pr","tr","rank",
    };
    for (auto o : kOps) if (o == w) return true;
    return false;
}

// Unicode sub/superscript forms for digits, +, -, =, ( ), and a few letters —
// used for inline (\textstyle) exponents/indices that fit on one row so we
// don't force a 3-row box for `x^2`.
inline std::string_view sup_glyph(char c) {
    switch (c) {
        case '0': return "\u2070"; case '1': return "\u00b9"; case '2': return "\u00b2";
        case '3': return "\u00b3"; case '4': return "\u2074"; case '5': return "\u2075";
        case '6': return "\u2076"; case '7': return "\u2077"; case '8': return "\u2078";
        case '9': return "\u2079"; case '+': return "\u207a"; case '-': return "\u207b";
        case '=': return "\u207c"; case '(': return "\u207d"; case ')': return "\u207e";
        case 'n': return "\u207f"; case 'i': return "\u2071";
        default:  return {};
    }
}
inline std::string_view sub_glyph(char c) {
    switch (c) {
        case '0': return "\u2080"; case '1': return "\u2081"; case '2': return "\u2082";
        case '3': return "\u2083"; case '4': return "\u2084"; case '5': return "\u2085";
        case '6': return "\u2086"; case '7': return "\u2087"; case '8': return "\u2088";
        case '9': return "\u2089"; case '+': return "\u208a"; case '-': return "\u208b";
        case '=': return "\u208c"; case '(': return "\u208d"; case ')': return "\u208e";
        case 'a': return "\u2090"; case 'e': return "\u2091"; case 'i': return "\u1d62";
        case 'o': return "\u2092"; case 'x': return "\u2093"; case 'j': return "\u2c7c";
        default:  return {};
    }
}

// Combining accents for \hat \bar \vec \dot \tilde etc. (applied to the last
// cell of the accented box).
inline std::string_view accent_combining(std::string_view name) {
    if (name == "hat"  || name == "widehat")   return "\u0302";
    if (name == "bar")                          return "\u0304";
    if (name == "vec")                          return "\u20d7";
    if (name == "dot")                          return "\u0307";
    if (name == "ddot")                         return "\u0308";
    if (name == "tilde"|| name == "widetilde") return "\u0303";
    if (name == "check")                        return "\u030c";
    if (name == "acute")                        return "\u0301";
    if (name == "grave")                        return "\u0300";
    if (name == "breve")                        return "\u0306";
    return {};
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
//  Parser / typesetter
// ─────────────────────────────────────────────────────────────────────────────
namespace detail {

struct Parser {
    std::string_view s;
    std::size_t      i = 0;
    const MathPalette& pal;
    bool             display;
    // When true, parse_seq/parse_group suppress inter-atom spacing. Set while
    // typesetting a superscript/subscript/limit, where TeX also uses a
    // cramped style — `e^{-x}` should read `e⁻ˣ`, not `e⁻ ˣ`.
    bool             tight = false;

    Parser(std::string_view src, const MathPalette& p, bool disp)
        : s(src), pal(p), display(disp) {}

    bool eof() const { return i >= s.size(); }
    char peek() const { return i < s.size() ? s[i] : '\0'; }
    void skip_ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n')) ++i; }

    // Read a control word: backslash already consumed. Returns the name; a
    // control SYMBOL (\{, \,, \|) returns a 1-char name.
    std::string_view read_ctrl() {
        std::size_t start = i;
        if (i < s.size() && !std::isalpha(static_cast<unsigned char>(s[i]))) {
            ++i;
            return s.substr(start, 1);
        }
        while (i < s.size() && std::isalpha(static_cast<unsigned char>(s[i]))) ++i;
        return s.substr(start, i - start);
    }

    // Parse a {...}-group or a single token into a Box. Used by \frac args,
    // ^ _ operands, \sqrt radicand, accents.
    Box parse_group() {
        skip_ws();
        if (peek() == '{') {
            ++i;
            Box b = parse_seq('}');
            if (peek() == '}') ++i;
            return b;
        }
        // single token
        return parse_atom();
    }

    // Parse a sequence of atoms until `stop` (or eof), concatenating boxes
    // with TeX-style inter-atom spacing (air around relations and binary
    // operators) so the result reads like typeset math, not a run-on string.
    Box parse_seq(char stop) {
        Box acc;  // empty
        bool have_prev = false;
        Box::Cls prev_cls = Box::Cls::Ord;
        while (!eof() && peek() != stop) {
            // \right / \end terminate a sub-parse regardless of `stop`.
            if (peek() == '\\') {
                std::size_t save = i;
                ++i;
                std::string_view w = read_ctrl();
                if (w == "right" || w == "end" || w == "\\") { i = save; break; }
                i = save;
            }
            if (peek() == '&') break;  // matrix column sep
            Box a = parse_atom();
            Box::Cls cur_cls = a.klass;
            a = maybe_scripts(std::move(a));
            if (have_prev) {
                int sp = tight ? 0 : inter_atom_space(prev_cls, cur_cls);
                if (sp > 0) acc = hcat(acc, blank(1, sp, 0, pal.normal));
            }
            acc = hcat(acc, a);
            have_prev = true;
            prev_cls = cur_cls;
        }
        return acc;
    }

    // TeX-inspired inter-atom spacing table (in cells). We keep it modest for
    // a character grid: 1 cell of air around relations and binary operators,
    // none inside tight pairs (Ord-Ord “implicit multiplication”, Open-Ord,
    // Ord-Close, before Punct). Big-operator (Op) gets a leading cell so
    // `\sum i` doesn't glue.
    static int inter_atom_space(Box::Cls a, Box::Cls b) {
        using C = Box::Cls;
        // no space hugging delimiters / before punctuation
        if (a == C::Open || b == C::Close || b == C::Punct) return 0;
        if (a == C::Rel  || b == C::Rel)  return 1;   // relations breathe
        if (a == C::Bin  || b == C::Bin)  return 1;   // + − ± × …
        if (a == C::Op   || b == C::Op)   return 1;   // ∑ ∫ ∏ \sin …
        if (a == C::Punct)                return 1;   // after a comma
        return 0;                                     // Ord–Ord: implicit mult
    }

    // After an atom, attach ^ and _ scripts if present.
    Box maybe_scripts(Box base) {
        Box sup, sub;
        bool has_sup = false, has_sub = false;
        bool save_tight = tight;
        tight = true;   // scripts are set cramped: e^{-x} not e^{- x}
        for (;;) {
            skip_ws();
            if (peek() == '^') { ++i; sup = parse_group(); has_sup = true; }
            else if (peek() == '_') { ++i; sub = parse_group(); has_sub = true; }
            else break;
        }
        tight = save_tight;
        if (!has_sup && !has_sub) return base;
        return attach_scripts(std::move(base), sup, sub, has_sup, has_sub);
    }

    // Compose a base with raised superscript and lowered subscript.
    Box attach_scripts(Box base, Box sup, Box sub, bool has_sup, bool has_sub) {
        // Fast path: single-row single-cell scripts fold into Unicode
        // sub/superscript glyphs (x^2 → x²) so simple exponents stay 1 row.
        auto try_unicode = [](const Box& b, bool super) -> std::string {
            if (b.rows != 1) return {};
            std::string out;
            for (int c = 0; c < b.cols; ++c) {
                const Cell& cell = b.at(0, c);
                if (cell.len == 0) continue;
                if (cell.len != 1) return {};
                std::string_view g = super ? sup_glyph(cell.bytes[0])
                                           : sub_glyph(cell.bytes[0]);
                if (g.empty()) return {};
                out += g;
            }
            return out;
        };
        if (has_sup && !has_sub) {
            std::string u = try_unicode(sup, true);
            if (!u.empty()) return hcat(base, hbox(u, pal.normal));
        }
        if (has_sub && !has_sup) {
            std::string u = try_unicode(sub, false);
            if (!u.empty()) return hcat(base, hbox(u, pal.normal));
        }
        // Inline fallback: in \textstyle (inline `$…$`) a script that can't
        // Unicode-fold would force the whole formula multi-row, which the
        // inline linearizer then has to drop. Keep it on one row with caret/
        // underscore notation instead — `e^{i\pi}` reads as `e^(i\u03c0)`,
        // legible and lossless. Display math (\displaystyle) keeps the real
        // stacked box so `\sum_{i=1}^{n}` typesets properly.
        if (!display) {
            auto flat = [](const Box& b) {
                std::string out;
                for (int r = 0; r < b.rows; ++r)
                    for (int c = 0; c < b.cols; ++c) {
                        const Cell& cell = b.at(r, c);
                        if (cell.len) out.append(cell.bytes.data(), cell.len);
                    }
                return out;
            };
            auto wrap = [](const std::string& s) {
                bool multi = s.size() > 1 &&
                             s.find_first_of(" +-") != std::string::npos;
                return multi ? "(" + s + ")" : s;
            };
            Box out = base;
            if (has_sup) out = hcat(out, hbox("^" + wrap(flat(sup)), pal.normal));
            if (has_sub) out = hcat(out, hbox("_" + wrap(flat(sub)), pal.normal));
            return out;
        }
        // General display path: build a 2-or-3 row box beside the base.
        // Superscript sits above the axis, subscript below.
        int width = std::max(has_sup ? sup.cols : 0, has_sub ? sub.cols : 0);
        std::vector<Box> parts;
        int axis_row;
        if (has_sup && has_sub) {
            parts = {sup, blank(1, width, 0, pal.normal), sub};
            axis_row = sup.rows;  // middle spacer row
        } else if (has_sup) {
            parts = {sup, blank(1, width, 0, pal.normal)};
            axis_row = sup.rows;
        } else {
            parts = {blank(1, width, 0, pal.normal), sub};
            axis_row = 0;
        }
        Box scripts = vstack(parts, axis_row, pal.normal);
        return hcat(base, scripts);
    }

    // ── one atom ──────────────────────────────────────────────────────────
    Box parse_atom() {
        skip_ws();
        if (eof()) return Box{};
        char c = peek();
        if (c == '\\') { ++i; return parse_ctrl(); }
        if (c == '{')  { return parse_group(); }
        if (c == '(' || c == '[') {
            ++i; Box b = hbox(std::string_view(&s[i - 1], 1), pal.delim);
            b.klass = Box::Cls::Open; return b;
        }
        if (c == ')' || c == ']') {
            ++i; Box b = hbox(std::string_view(&s[i - 1], 1), pal.delim);
            b.klass = Box::Cls::Close; return b;
        }
        if (c == '|') {
            ++i; return hbox("\u2502", pal.delim);  // proper vertical bar
        }
        if (c == ',' || c == ';') {
            ++i; Box b = hbox(std::string_view(&s[i - 1], 1), pal.normal);
            b.klass = Box::Cls::Punct; return b;
        }
        // digit run
        if (std::isdigit(static_cast<unsigned char>(c))) {
            std::size_t start = i;
            while (i < s.size() &&
                   (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.'))
                ++i;
            return hbox(s.substr(start, i - start), pal.num);
        }
        // binary operators
        if (c=='+'||c=='-'||c=='*') {
            ++i;
            std::string_view g = (c=='-') ? std::string_view("\u2212")   // real minus
                               : (c=='*') ? std::string_view("\u2217")   // asterisk op
                                          : std::string_view(&s[i - 1], 1);
            Box b = hbox(g, pal.op);
            b.klass = Box::Cls::Bin; return b;
        }
        // relations
        if (c=='='||c=='<'||c=='>') {
            ++i;
            Box b = hbox(std::string_view(&s[i - 1], 1), pal.op);
            b.klass = Box::Cls::Rel; return b;
        }
        if (c=='/') {
            ++i; return hbox("/", pal.op);
        }
        // a letter → italic variable (single char)
        if (std::isalpha(static_cast<unsigned char>(c))) {
            ++i;
            return hbox(std::string_view(&s[i - 1], 1),
                        pal.normal.with_italic());
        }
        // anything else: literal
        ++i;
        return hbox(std::string_view(&s[i - 1], 1), pal.normal);
    }

    // ── control sequences ────────────────────────────────────────────────
    Box parse_ctrl() {
        std::string_view w = read_ctrl();

        // Control SYMBOLS (single non-alpha char after the backslash).
        if (w == "|") { Box b = hbox("\u2016", pal.delim); return b; }  // \| → ‖
        if (w == "{") { Box b = hbox("{", pal.delim); b.klass = Box::Cls::Open;  return b; }
        if (w == "}") { Box b = hbox("}", pal.delim); b.klass = Box::Cls::Close; return b; }

        // Escaped punctuation — the characters TeX reserves, which a source
        // must backslash to print. They stand for THEMSELVES, so dropping
        // the backslash is the whole job.
        //
        // Without this they fell through to the unknown-control-word arm
        // and rendered as a literal "\_", which is worse than not
        // supporting math at all: the escape is invisible in the source
        // (\_ looks like an underscore to whoever wrote it) and the reader
        // sees a stray backslash they cannot account for. `bright\_black`
        // came out as `bright\_black` in a rendered agentty answer.
        //
        // \_ is by far the most common of these in our own output, because
        // snake_case identifiers are everywhere and _ is TeX's subscript
        // operator, so anything naming a C++ symbol inside math has to
        // escape it.
        if (w == "_" || w == "%" || w == "$" || w == "&" || w == "#")
            return hbox(w, pal.normal);

        if (w == "frac" || w == "dfrac" || w == "tfrac" || w == "binom") {
            Box top = parse_group();
            Box bot = parse_group();
            if (w == "binom") return make_binom(top, bot);
            return make_frac(top, bot);
        }
        if (w == "sqrt") {
            Box index;
            bool has_index = false;
            skip_ws();
            if (peek() == '[') {
                ++i; index = parse_seq(']'); if (peek() == ']') ++i; has_index = true;
            }
            Box rad = parse_group();
            return make_sqrt(rad, index, has_index);
        }
        if (w == "left") {
            char open = eat_delim();
            Box inner = parse_seq('\0');  // until \right
            char close = '\0';
            if (peek() == '\\') {
                std::size_t save = i; ++i;
                if (read_ctrl() == "right") close = eat_delim(); else i = save;
            }
            return wrap_delims(inner, open, close);
        }
        if (w == "text" || w == "mathrm" || w == "operatorname" ||
            w == "mathbf" || w == "mathit" || w == "mathsf" || w == "mathtt" ||
            w == "texttt" || w == "textrm" || w == "textbf" ||
            w == "textit" || w == "textsf" || w == "textsc" ||
            w == "textnormal" || w == "emph") {
            skip_ws();
            if (peek() == '{') {
                ++i;
                std::size_t start = i;
                int depth = 1;
                while (i < s.size() && depth) {
                    if (s[i] == '{') ++depth;
                    else if (s[i] == '}') { if (--depth == 0) break; }
                    ++i;
                }
                std::string_view body = s.substr(start, i - start);
                if (peek() == '}') ++i;
                Style st = (w == "mathbf" || w == "textbf") ? pal.text.with_bold()
                         : (w == "mathit" || w == "textit"
                            || w == "emph") ? pal.text.with_italic()
                         : pal.text;
                return hbox(body, st);
            }
            return Box{};
        }
        if (w == "begin") return parse_env();

        // \overline / \underline: full-width rule (multi-char aware).
        if (w == "overline" || w == "underline")
            return make_line(parse_group(), w == "overline");

        // \boxed{…}: framed body.
        if (w == "boxed") return make_boxed(parse_group());

        // \overbrace{body}^{lbl} / \underbrace{body}_{lbl}.
        if (w == "overbrace" || w == "underbrace") {
            bool over = (w == "overbrace");
            Box body = parse_group();
            Box label; bool has_label = false;
            bool save_tight = tight; tight = true;
            skip_ws();
            char want = over ? '^' : '_';
            if (peek() == want) { ++i; label = parse_group(); has_label = true; }
            tight = save_tight;
            return make_brace(body, label, over, has_label);
        }

        // extensible arrows: \xrightarrow[under]{over}, \xleftarrow…, etc.
        // The label(s) sit over/under a long arrow. Optional [..] then {..}.
        if (w == "xrightarrow" || w == "xleftarrow"
         || w == "xRightarrow" || w == "xLeftarrow"
         || w == "xleftrightarrow" || w == "xmapsto"
         || w == "xhookrightarrow" || w == "xhookleftarrow") {
            Box under; bool has_under = false;
            skip_ws();
            if (peek() == '[') {   // optional under-label
                ++i;
                under = parse_seq(']');
                if (peek() == ']') ++i;
                has_under = true;
            }
            Box over; bool has_over = false;
            skip_ws();
            if (peek() == '{') { over = parse_group(); has_over = true; }
            std::string_view glyph =
                  (w == "xleftarrow")        ? "\u2190"
                : (w == "xRightarrow")       ? "\u21d2"
                : (w == "xLeftarrow")        ? "\u21d0"
                : (w == "xleftrightarrow")   ? "\u2194"
                : (w == "xmapsto")           ? "\u21a6"
                : (w == "xhookrightarrow")   ? "\u21aa"
                : (w == "xhookleftarrow")    ? "\u21a9"
                :                              "\u2192";   // xrightarrow
            return make_labeled_arrow(glyph, over, under, has_over, has_under);
        }

        // accents
        if (!accent_combining(w).empty()) {
            Box b = parse_group();
            apply_accent(b, accent_combining(w));
            return b;
        }

        // big operators with limits (\sum \int \prod \lim \bigcup …)
        if (auto big = big_operator(w); !big.empty())
            return parse_bigop(big, w);

        // named function (\sin \log …): upright, keep following space
        if (is_named_op(w)) {
            Box b = hbox(w, pal.op);
            b.klass = Box::Cls::Op;
            return b;
        }

        // \mathbb{X} etc. — try the combined key first (e.g. "mathbb{R}").
        if (w == "mathbb" || w == "mathcal" || w == "mathfrak") {
            skip_ws();
            if (peek() == '{') {
                std::size_t save = i; ++i;
                std::size_t start = i;
                while (i < s.size() && s[i] != '}') ++i;
                std::string_view inner = s.substr(start, i - start);
                if (peek() == '}') ++i;
                std::string combined = std::string(w) + "{" + std::string(inner) + "}";
                if (auto g = lookup_symbol(combined); !g.empty())
                    return hbox(g, pal.normal);
                // fallback: render the letters upright/bold
                i = save;
                Box b = parse_group();
                return b;
            }
        }

        // named symbol
        if (auto g = lookup_symbol(w); !g.empty()) {
            Box b = hbox(g, pal.normal);
            b.klass = classify_symbol(w);
            if (b.klass == Box::Cls::Bin || b.klass == Box::Cls::Rel ||
                b.klass == Box::Cls::Op)
                for (auto& cell : b.grid) cell.style = pal.op;
            return b;
        }

        // spacing commands already in table as " "; unknown → dim literal name
        return hbox(std::string("\\") + std::string(w),
                    pal.text.with_dim());
    }

    // Classify a named LaTeX symbol so the sequence layout can space it. This
    // is the coarse TeX math-class of the control word (relations breathe,
    // binary operators breathe a little, delimiters hug).
    static Box::Cls classify_symbol(std::string_view w) {
        using C = Box::Cls;
        // relations
        for (auto r : {"leq","le","geq","ge","neq","ne","equiv","approx","cong",
                       "simeq","sim","propto","ll","gg","subset","supset",
                       "subseteq","supseteq","sqsubseteq","sqsupseteq","in",
                       "ni","notin","vdash","dashv","models","perp","mid",
                       "parallel","asymp","doteq","prec","succ","preceq",
                       "succeq","to","rightarrow","leftarrow","gets",
                       "leftrightarrow","Rightarrow","Leftarrow","iff",
                       "implies","impliedby","mapsto","Leftrightarrow"})
            if (w == r) return C::Rel;
        // binary operators
        for (auto b : {"times","div","pm","mp","cdot","ast","star","circ",
                       "bullet","oplus","ominus","otimes","oslash","odot",
                       "cap","cup","sqcap","sqcup","wedge","land","vee","lor",
                       "setminus","wr","amalg","dagger","ddagger"})
            if (w == b) return C::Bin;
        return C::Ord;
    }

    // Big-operator glyph for a control word, or empty.
    static std::string_view big_operator(std::string_view w) {
        if (w == "sum")      return "\u2211";
        if (w == "prod")     return "\u220f";
        if (w == "coprod")   return "\u2210";
        if (w == "int")      return "\u222b";
        if (w == "iint")     return "\u222c";
        if (w == "iiint")    return "\u222d";
        if (w == "oint")     return "\u222e";
        if (w == "bigcup")   return "\u22c3";
        if (w == "bigcap")   return "\u22c2";
        if (w == "bigvee")   return "\u22c1";
        if (w == "bigwedge") return "\u22c0";
        if (w == "bigoplus") return "\u2a01";
        if (w == "bigotimes")return "\u2a02";
        if (w == "lim")      return "lim";
        return {};
    }

    // \sum_{i=0}^{n}: in display mode put limits above/below, else as scripts.
    Box parse_bigop(std::string_view glyph, std::string_view name) {
        Box lower, upper;
        bool has_lo = false, has_up = false;
        bool save_tight = tight;
        tight = true;   // limits are cramped too
        for (;;) {
            skip_ws();
            if (peek() == '_') { ++i; lower = parse_group(); has_lo = true; }
            else if (peek() == '^') { ++i; upper = parse_group(); has_up = true; }
            else break;
        }
        tight = save_tight;
        Box op = hbox(glyph, pal.op.with_bold());
        bool limits_above = display && name != "int" && name != "iint" &&
                            name != "iiint" && name != "oint";
        if (!has_lo && !has_up) return op;
        if (limits_above) {
            std::vector<Box> parts;
            int axis_row = 0;
            if (has_up) { parts.push_back(upper); ++axis_row; }
            parts.push_back(op);
            if (has_lo) parts.push_back(lower);
            return vstack(parts, axis_row, pal.normal);
        }
        return attach_scripts(op, upper, lower, has_up, has_lo);
    }

    // ── environments: matrix / pmatrix / bmatrix / cases / array ────────────
    Box parse_env() {
        skip_ws();
        std::string kind;
        if (peek() == '{') {
            ++i;
            std::size_t start = i;
            while (i < s.size() && s[i] != '}') ++i;
            kind = std::string(s.substr(start, i - start));
            if (peek() == '}') ++i;
        }
        // `array` (and array-like) carries a SECOND {…} column spec, e.g.
        // {c|c} or {l r r}. Consume it and record which inter-column gaps get a
        // vertical rule (a `|` between/around column letters). vrules[k] = a
        // rule sits to the LEFT of column k; vrules[ncol] = a trailing rule.
        std::vector<bool> vrules;
        if (kind == "array") {
            skip_ws();
            if (peek() == '{') {
                ++i;
                int col = 0;
                vrules.push_back(false);   // slot before column 0
                while (i < s.size() && s[i] != '}') {
                    char c = s[i++];
                    if (c == '|') { vrules[col] = true; }
                    else if (c == 'l' || c == 'c' || c == 'r') {
                        ++col; vrules.push_back(false);
                    }
                    // ignore p{..}, @{..}, spaces, etc. (best-effort)
                }
                if (peek() == '}') ++i;
            }
        }
        // collect rows of cells; hrules[r] = a horizontal rule sits ABOVE row r
        // (\hline seen before that row); hrules[nrow] = a trailing rule.
        std::vector<std::vector<Box>> matrix;
        std::vector<bool> hrules;
        matrix.emplace_back();
        hrules.push_back(false);
        for (;;) {
            skip_ws();
            if (eof()) break;
            if (peek() == '\\') {
                std::size_t save = i; ++i;
                std::string_view w = read_ctrl();
                if (w == "end") {
                    skip_ws();
                    if (peek() == '{') { while (i < s.size() && s[i] != '}') ++i; if (peek()=='}') ++i; }
                    break;
                }
                if (w == "\\") { matrix.emplace_back(); hrules.push_back(false); continue; }  // row break
                if (w == "hline" || w == "midrule" || w == "toprule"
                    || w == "bottomrule") {
                    // A rule attaches to the boundary at the current row. If the
                    // current row is still empty it's the top border of that
                    // row; otherwise (rare: \hline mid-row) treat as trailing.
                    if (matrix.back().empty()) hrules.back() = true;
                    else { hrules.push_back(true); matrix.emplace_back(); hrules.push_back(false); }
                    continue;
                }
                i = save;
            }
            if (peek() == '&') { ++i; matrix.back().emplace_back(); continue; }
            Box cell = parse_seq_cell();
            if (matrix.back().empty()) matrix.back().push_back(cell);
            else matrix.back().back() = hcat(matrix.back().back(), cell);
        }
        return assemble_matrix(matrix, kind, vrules, hrules);
    }

    // parse one matrix cell: until & , \\ , or \end
    Box parse_seq_cell() {
        Box acc;
        bool have_prev = false;
        Box::Cls prev_cls = Box::Cls::Ord;
        while (!eof()) {
            skip_ws();
            if (peek() == '&') break;
            if (peek() == '\\') {
                std::size_t save = i; ++i;
                std::string_view w = read_ctrl();
                i = save;
                if (w == "\\" || w == "end") break;
            }
            Box a = parse_atom();
            Box::Cls cur_cls = a.klass;
            a = maybe_scripts(std::move(a));
            if (have_prev) {
                int sp = inter_atom_space(prev_cls, cur_cls);
                if (sp > 0) acc = hcat(acc, blank(1, sp, 0, pal.normal));
            }
            acc = hcat(acc, a);
            have_prev = true;
            prev_cls = cur_cls;
        }
        return acc;
    }

    char eat_delim() {
        skip_ws();
        if (peek() == '\\') { ++i; std::string_view w = read_ctrl();
            if (w == ".") return '\0';
            if (w == "{") return '{';
            if (w == "}") return '}';
            if (w == "langle") return '<';
            if (w == "rangle") return '>';
            if (w == "|" || w == "Vert" || w == "lVert" || w == "rVert")
                return '|';
            if (w == "lvert" || w == "rvert" || w == "vert") return '|';
            return '\0';
        }
        char c = peek();
        if (c) ++i;
        if (c == '.') return '\0';
        return c;
    }

    // ── constructors that build multi-row boxes ────────────────────────────
    Box make_frac(const Box& num, const Box& den) {
        int w = std::max(num.cols, den.cols);
        w = std::max(w, 1);
        Box bar = blank(1, w, 0, pal.rule);
        for (int c = 0; c < w; ++c) bar.at(0, c) = make_cell("\u2500", pal.rule);
        Box stacked = vstack({num, bar, den}, num.rows, pal.normal);
        // axis on the bar row
        stacked.axis = num.rows;
        return stacked;
    }

    // \xrightarrow{over}[under]: a long arrow at least as wide as its labels,
    // with the over-label centered above and the under-label below. In tight
    // (inline / script) context we can't stack rows, so collapse to a single
    // row: a short arrow rule carrying the label, e.g. `─parse→`.
    Box make_labeled_arrow(std::string_view glyph, const Box& over,
                           const Box& under, bool has_over, bool has_under) {
        int lblw = std::max(has_over ? over.cols : 0,
                            has_under ? under.cols : 0);
        // Determine arrow direction from the glyph to know which end gets the
        // head; default (right/`→`) draws `──→`, left (`←`) draws `←──`.
        const bool leftish = glyph == "\u2190" || glyph == "\u21d0";
        // Arrow body width: label width + a little air, min 3 so the shaft
        // reads as an arrow even with a 1-char label.
        int shaft = std::max(3, lblw + 2);

        auto build_arrow_row = [&]() -> Box {
            Box row = blank(1, shaft, 0, pal.op);
            for (int c = 0; c < shaft; ++c) {
                std::string_view g = "\u2500";                 // shaft ─
                if (leftish && c == 0)            g = glyph;   // head at left
                else if (!leftish && c == shaft-1) g = glyph;  // head at right
                row.at(0, c) = make_cell(g, pal.op);
            }
            row.klass = Box::Cls::Rel;   // an arrow is a relation — give it air
            return row;
        };

        if (!display || (!has_over && !has_under)) {
            // Inline (textstyle) or unlabeled: keep it to a single row so it
            // never inflates line height. Layout: right arrows read
            // `label─→`, left arrows `←─label`.
            Box arrow = build_arrow_row();
            if (!has_over && !has_under) return arrow;
            const Box& lbl = has_over ? over : under;
            if (lbl.rows == 1) {
                Box a = build_arrow_row();
                Box res = leftish ? hcat(a, lbl) : hcat(lbl, a);
                res.klass = Box::Cls::Rel;
                return res;
            }
            return arrow;
        }

        // Display: stack over-label / arrow / under-label, arrow on the axis.
        Box arrow = build_arrow_row();
        std::vector<Box> parts;
        int axis_row = 0;
        if (has_over)  { parts.push_back(over); ++axis_row; }
        parts.push_back(arrow);
        if (has_under)   parts.push_back(under);
        Box out = vstack(parts, axis_row, pal.normal);
        out.klass = Box::Cls::Rel;
        return out;
    }

    // \overline{X} / \underline{X}: draw a full-width rule above / below the
    // body (a real bar, not a per-cell combining mark, so it works for
    // multi-character bodies like \overline{AB}). The body keeps the axis.
    Box make_line(const Box& body, bool over) {
        int w = std::max(1, body.cols);
        Box rule = blank(1, w, 0, pal.rule);
        for (int c = 0; c < w; ++c)
            rule.at(0, c) = make_cell(over ? "\u203e" : "\u2500", pal.rule);
        Box out = over ? vstack({rule, body}, 1, pal.normal)
                       : vstack({body, rule}, 0, pal.normal);
        out.axis = over ? 1 + body.axis : body.axis;
        return out;
    }

    // \overbrace{…}^{lbl} / \underbrace{…}_{lbl}: a horizontal brace above /
    // below the body with an optional label. The brace is drawn from the
    // box-drawing set so it scales to the body width.
    Box make_brace(const Box& body, const Box& label, bool over, bool has_label) {
        int w = std::max(2, body.cols);
        Box brace = blank(1, w, 0, pal.rule);
        for (int c = 0; c < w; ++c) {
            std::string_view g;
            if (over)  g = (c == 0) ? "\u2570" : (c == w-1) ? "\u256f"
                        : (c == w/2) ? "\u252c" : "\u2500";
            else       g = (c == 0) ? "\u256d" : (c == w-1) ? "\u256e"
                        : (c == w/2) ? "\u2534" : "\u2500";
            brace.at(0, c) = make_cell(g, pal.rule);
        }
        std::vector<Box> parts;
        int axis_row;
        if (over) {
            if (has_label) parts.push_back(label);
            parts.push_back(brace);
            parts.push_back(body);
            axis_row = static_cast<int>(parts.size()) - 1;
        } else {
            parts.push_back(body);
            parts.push_back(brace);
            if (has_label) parts.push_back(label);
            axis_row = 0;
        }
        Box out = vstack(parts, axis_row, pal.normal);
        out.axis = over ? (has_label ? 2 : 1) : 0;
        return out;
    }

    // \boxed{…}: draw a light box-drawing frame around the body.
    Box make_boxed(const Box& body) {
        int w = body.cols, h = body.rows;
        Box out = blank(h + 2, w + 2, body.axis + 1, pal.normal);
        for (int r = 0; r < h; ++r)
            for (int c = 0; c < w; ++c)
                out.at(r + 1, c + 1) = body.at(r, c);
        out.at(0, 0)         = make_cell("\u250c", pal.delim);
        out.at(0, w + 1)     = make_cell("\u2510", pal.delim);
        out.at(h + 1, 0)     = make_cell("\u2514", pal.delim);
        out.at(h + 1, w + 1) = make_cell("\u2518", pal.delim);
        for (int c = 1; c <= w; ++c) {
            out.at(0, c)     = make_cell("\u2500", pal.delim);
            out.at(h + 1, c) = make_cell("\u2500", pal.delim);
        }
        for (int r = 1; r <= h; ++r) {
            out.at(r, 0)     = make_cell("\u2502", pal.delim);
            out.at(r, w + 1) = make_cell("\u2502", pal.delim);
        }
        return out;
    }

    Box make_binom(const Box& top, const Box& bot) {
        Box stacked = vstack({top, bot}, top.rows - 1 + (bot.rows>0?0:0), pal.normal);
        stacked.axis = (stacked.rows - 1) / 2;
        return wrap_delims(stacked, '(', ')');
    }

    Box make_sqrt(const Box& rad, const Box& index, bool has_index) {
        int h = rad.rows;
        int w = rad.cols;
        // Radical layout — the stroke sits directly left of the radicand and
        // the vinculum runs across the top, joined at the hook:
        //
        //       ‾‾‾‾‾‾‾        row 0: vinculum over the radicand (+1 lead col)
        //      √b²−4ac         rows 1..: '√' stroke immediately left of radicand
        //
        int stroke_col = 0;
        int rad_col    = 1;          // radicand right after the stroke, no gap
        int extra_top  = 1;          // vinculum row
        int total_w    = rad_col + w;
        Box out = blank(h + extra_top, total_w, rad.axis + extra_top, pal.normal);
        // vinculum over the radicand columns (and one lead col above the hook)
        for (int c = 0; c < total_w; ++c)
            out.at(0, c) = make_cell("\u203e", pal.rule);
        // radical stroke on the baseline row of the radicand region
        out.at(extra_top + std::max(0, h - 1), stroke_col) =
            make_cell("\u221a", pal.rule);
        // blit radicand
        for (int r = 0; r < h; ++r)
            for (int c = 0; c < w; ++c)
                out.at(extra_top + r, rad_col + c) = rad.at(r, c);
        if (has_index && index.cols > 0) {
            // small index tucked to the upper-left of the stroke (best effort)
            return hcat(index, out);
        }
        return out;
    }

    Box wrap_delims(const Box& inner, char open, char close) {
        int h = inner.rows;
        Box out = inner;
        auto tall = [&](char d, bool left) -> Box {
            if (h <= 1) {
                std::string_view g(&d, 1);
                std::string_view sub = (d=='\0') ? std::string_view(" ") : g;
                return hbox((d=='\0')?" ":sub, pal.delim);
            }
            // build a multi-row bracket
            Box col = blank(h, 1, inner.axis, pal.delim);
            std::string_view top, mid, bot, ext;
            switch (d) {
                case '(': top="\u239b"; ext="\u239c"; bot="\u239d"; mid="\u239c"; break;
                case ')': top="\u239e"; ext="\u239f"; bot="\u23a0"; mid="\u239f"; break;
                case '[': top="\u23a1"; ext="\u23a2"; bot="\u23a3"; mid="\u23a2"; break;
                case ']': top="\u23a4"; ext="\u23a5"; bot="\u23a6"; mid="\u23a5"; break;
                case '{': top="\u23a7"; ext="\u23a8"; bot="\u23a9"; mid="\u23aa"; break;
                case '}': top="\u23ab"; ext="\u23ac"; bot="\u23ad"; mid="\u23aa"; break;
                case '|': top="\u2502"; ext="\u2502"; bot="\u2502"; mid="\u2502"; break;
                default:  return blank(h, 1, inner.axis, pal.delim);
            }
            for (int r = 0; r < h; ++r) {
                std::string_view g = (r == 0) ? top : (r == h - 1) ? bot
                                    : (r == h/2) ? mid : ext;
                col.at(r, 0) = make_cell(g, pal.delim);
            }
            (void)left;
            return col;
        };
        if (open  != '\0') out = hcat(tall(open, true),  out);
        if (close != '\0') out = hcat(out, tall(close, false));
        return out;
    }

    Box assemble_matrix(const std::vector<std::vector<Box>>& m, const std::string& kind,
                        const std::vector<bool>& vrules = {},
                        const std::vector<bool>& hrules = {}) {
        // drop trailing empty row (but keep a matching trailing hrule intent)
        std::vector<std::vector<Box>> rows = m;
        std::vector<bool> hr = hrules;
        bool trailing_hrule = false;
        while (!rows.empty() && rows.back().empty()) {
            if (hr.size() == rows.size() + 1 && hr.back()) trailing_hrule = true;
            rows.pop_back();
            if (!hr.empty()) hr.pop_back();
        }
        if (rows.empty()) return Box{};
        std::size_t ncol = 0;
        for (auto& r : rows) ncol = std::max(ncol, r.size());
        std::vector<int> colw(ncol, 0);
        std::vector<int> rowh(rows.size(), 1), rowasc(rows.size(), 0);
        for (std::size_t r = 0; r < rows.size(); ++r)
            for (std::size_t c = 0; c < rows[r].size(); ++c) {
                colw[c]  = std::max(colw[c], rows[r][c].cols);
                rowh[r]  = std::max(rowh[r], rows[r][c].rows);
                rowasc[r]= std::max(rowasc[r], rows[r][c].ascent());
            }
        const bool is_array = (kind == "array");
        int gap = 1;
        // Vertical rules (array only): a `|` between columns occupies one extra
        // grid column in the gap. vcol[k] true ⇒ a rule sits left of column k;
        // vcol[ncol] ⇒ a trailing rule after the last column.
        auto vcol = [&](std::size_t k) -> bool {
            return is_array && k < vrules.size() && vrules[k];
        };
        // Column x-origin (in the output grid) accounting for gaps + vrules.
        std::vector<int> colx(ncol, 0);
        int totalw = 0;
        for (std::size_t c = 0; c < ncol; ++c) {
            if (vcol(c)) totalw += 1;                 // rule glyph column
            else if (c > 0) totalw += gap;            // normal inter-col gap
            colx[c] = totalw;
            totalw += colw[c];
        }
        if (vcol(ncol)) totalw += 1;                  // trailing rule
        // Horizontal rules (array only): each \hline occupies one extra grid
        // row. hrow_before[r] ⇒ a rule row precedes row r.
        auto hrow = [&](std::size_t r) -> bool {
            return is_array && r < hr.size() && hr[r];
        };
        std::vector<int> rowy(rows.size(), 0);
        int totalh = 0;
        for (std::size_t r = 0; r < rows.size(); ++r) {
            if (hrow(r)) totalh += 1;                  // rule row
            rowy[r] = totalh;
            totalh += rowh[r];
        }
        if (is_array && trailing_hrule) totalh += 1;
        Box out = blank(totalh, totalw, totalh / 2, pal.normal);
        // paint cells
        for (std::size_t r = 0; r < rows.size(); ++r) {
            for (std::size_t c = 0; c < ncol; ++c) {
                if (c < rows[r].size()) {
                    const Box& cell = rows[r][c];
                    int coff = colx[c] + (colw[c] - cell.cols) / 2;
                    int roff = rowy[r] + (rowasc[r] - cell.ascent());
                    for (int rr = 0; rr < cell.rows; ++rr)
                        for (int cc = 0; cc < cell.cols; ++cc)
                            out.at(roff + rr, coff + cc) = cell.at(rr, cc);
                }
            }
        }
        // paint vertical rules down the full height
        if (is_array) {
            auto paint_vrule = [&](int x) {
                if (x < 0 || x >= out.cols) return;
                for (int r = 0; r < out.rows; ++r)
                    out.at(r, x) = make_cell("\u2502", pal.delim);
            };
            for (std::size_t c = 0; c < ncol; ++c)
                if (vcol(c)) paint_vrule(colx[c] - 1);
            if (vcol(ncol)) paint_vrule(totalw - 1);
            // paint horizontal rules across the full width
            auto paint_hrule = [&](int y) {
                if (y < 0 || y >= out.rows) return;
                for (int c = 0; c < out.cols; ++c) {
                    // keep a vrule crossing as a cross glyph for legibility
                    std::string_view cur;
                    const Cell& ex = out.at(y, c);
                    if (ex.len && std::string_view(ex.bytes.data(), ex.len) == "\u2502")
                        cur = "\u253c";
                    else cur = "\u2500";
                    out.at(y, c) = make_cell(cur, pal.delim);
                }
            };
            for (std::size_t r = 0; r < rows.size(); ++r)
                if (hrow(r)) paint_hrule(rowy[r] - 1);
            if (trailing_hrule) paint_hrule(out.rows - 1);
        }
        // brackets. Add one pad column on each side so the entries don't
        // touch the delimiters (⎛ a  b ⎞ reads better than ⎛a b⎞).
        char open = '\0', close = '\0';
        if (kind == "pmatrix") { open='('; close=')'; }
        else if (kind == "bmatrix") { open='['; close=']'; }
        else if (kind == "Bmatrix") { open='{'; close='}'; }
        else if (kind == "vmatrix") { open='|'; close='|'; }
        else if (kind == "Vmatrix") { open='|'; close='|'; }
        else if (kind == "cases")   { open='{'; close='\0'; }
        // `array` intentionally has NO auto-brackets — a surrounding
        // \left[…\right] (or \left(…) supplies them, matching LaTeX.
        if (open || close) {
            Box padded = blank(out.rows, out.cols + 2, out.axis, pal.normal);
            for (int r = 0; r < out.rows; ++r)
                for (int c = 0; c < out.cols; ++c)
                    padded.at(r, c + 1) = out.at(r, c);
            out = wrap_delims(padded, open, close);
        }
        return out;
    }

    static void apply_accent(Box& b, std::string_view combining) {
        if (b.cols == 0 || b.rows == 0) return;
        // append the combining mark to the top-center cell of the top row.
        int c = b.cols / 2;
        Cell& cell = b.at(0, c);
        std::string merged;
        merged.assign(cell.bytes.data(), cell.len);
        merged += combining;
        cell = make_cell(merged, cell.style, cell.width);
    }
};

} // namespace detail

// ── public entry points ──────────────────────────────────────────────────────
inline Box typeset(std::string_view latex, const MathPalette& pal, bool display) {
    detail::Parser p(latex, pal, display);
    Box b = p.parse_seq('\0');
    if (b.cols == 0) b = detail::blank(1, 0, 0, pal.normal);
    return b;
}

// Linearize a typeset Box to a single UTF-8 string for INLINE math that must
// live inside a flowed paragraph run. The typeset already uses \textstyle
// (display=false), so most inline math is ALREADY one row (x^2, a_i,
// \alpha+\beta, \sin x) and passes straight through. The only common
// multi-row inline construct is a fraction, which collapses to `num/den`
// (the universal inline convention). Any other multi-row box (a stray inline
// matrix, nested radical) falls back to its baseline row so we never emit the
// scrambled row-interleaving a naive join would produce.
inline std::string linearize(std::string_view latex, const MathPalette& pal) {
    Box b = typeset(latex, pal, /*display=*/false);
    auto row_text = [&](int r) {
        std::string line;
        for (int c = 0; c < b.cols; ++c) {
            const Cell& cell = b.at(r, c);
            if (cell.len) line.append(cell.bytes.data(), cell.len);
        }
        while (!line.empty() && line.back() == ' ') line.pop_back();
        // also strip a leading run of spaces so centered rows align left
        std::size_t nb = line.find_first_not_of(' ');
        if (nb != std::string::npos && nb) line.erase(0, nb);
        return line;
    };
    if (b.rows == 1) return row_text(0);

    // Find a fraction bar: the FIRST full-width run of the box-drawing bar
    // '\u2500'. A top-level fraction's bar spans (nearly) the whole width; a
    // bar that only covers part of the row is a sub-fraction and we leave it
    // to the raw-latex fallback below.
    auto bar_row = [&]() -> int {
        for (int r = 0; r < b.rows; ++r) {
            int bars = 0, other = 0;
            for (int c = 0; c < b.cols; ++c) {
                const Cell& cell = b.at(r, c);
                if (cell.len == 0) continue;
                std::string_view g(cell.bytes.data(), cell.len);
                if (g == "\u2500") ++bars;
                else if (g != " ") ++other;
            }
            if (bars >= 1 && other == 0 && bars >= b.cols - 2) return r;
        }
        return -1;
    };
    int br = bar_row();
    if (br >= 0) {
        // Collapse everything above the bar into num, below into den, each
        // flattened to one line (drops any inner 2-D structure like a radical
        // vinculum — acceptable inline; the display form keeps it).
        auto region = [&](int lo, int hi) {
            std::string best;
            int best_weight = -1;
            for (int r = lo; r < hi; ++r) {
                std::string line = row_text(r);
                // weight = count of glyphs that aren't space or vinculum
                int weight = 0;
                for (int c = 0; c < b.cols; ++c) {
                    const Cell& cell = b.at(r, c);
                    if (cell.len == 0) continue;
                    std::string_view g(cell.bytes.data(), cell.len);
                    if (g != " " && g != "\u203e") ++weight;
                }
                if (weight > best_weight) { best_weight = weight; best = line; }
            }
            // strip a leading/trailing vinculum run and surrounding spaces
            std::string cleaned;
            for (std::size_t k = 0; k < best.size(); ) {
                if (best.compare(k, 3, "\u203e") == 0) { k += 3; continue; }
                cleaned += best[k++];
            }
            std::size_t a = cleaned.find_first_not_of(' ');
            std::size_t z = cleaned.find_last_not_of(' ');
            if (a == std::string::npos) return std::string{};
            return cleaned.substr(a, z - a + 1);
        };
        std::string num = region(0, br), den = region(br + 1, b.rows);
        if (!num.empty() && !den.empty()) {
            auto needs_parens = [](const std::string& s) {
                return s.find(' ') != std::string::npos ||
                       s.find('+') != std::string::npos ||
                       s.find('-') != std::string::npos ||
                       s.find("\u2212") != std::string::npos ||
                       s.find("\u00b1") != std::string::npos;
            };
            std::string out;
            out += needs_parens(num) ? "(" + num + ")" : num;
            out += "/";
            out += needs_parens(den) ? "(" + den + ")" : den;
            return out;
        }
    }
    // General multi-row inline (a fraction whose numerator/denominator is
    // itself tall, a nested radical, an inline matrix, or a fraction embedded
    // beside other terms like `x = \frac{…}{…}`): extracting a single row is
    // misleading — a fraction bar on the baseline would read as a stray `───`.
    // When the baseline row carries ANY bar glyph, return the raw LaTeX
    // (unambiguous, lossless); the DISPLAY (`$$…$$`) path is where full 2-D
    // layout belongs.
    for (int c = 0; c < b.cols; ++c) {
        const Cell& cell = b.at(b.axis, c);
        if (cell.len && std::string_view(cell.bytes.data(), cell.len) == "\u2500")
            return std::string(latex);
    }
    return row_text(b.axis);
}

} // namespace maya::texmath
