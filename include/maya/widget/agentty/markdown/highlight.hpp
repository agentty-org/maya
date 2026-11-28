#pragma once
// maya::syntax — themeable syntax highlighting with a constexpr HighlightTheme
//
// Architecture (the part tree-sitter plugs into):
//
//   source text ──▶  Tokenizer  ──▶  [Span{byte_range, Capture}]  ──▶  styled
//                    (backend)        canonical capture names          spans
//                                                  │
//                                       HighlightTheme: Capture → Style
//                                       (constexpr, themeable)
//
// `Capture` is the canonical set of highlight *capture names* that tree-sitter
// query files use (`@keyword`, `@function`, `@string`, `@comment`, …). A
// `HighlightTheme` is a compile-time array mapping each Capture to a maya
// `Style`; swap themes by passing a different one. Themes are `constexpr` so
// the palette is baked into the binary with zero runtime construction.
//
// The TOKENIZER is pluggable:
//   * Default: a fast, allocation-light built-in lexer (`builtin_highlight`)
//     that recognises strings / comments / numbers / keywords / identifiers
//     across C-family, Python, Rust, Go, shell, JSON, etc. This keeps maya
//     dependency-free and "blazing fast" — no grammar blobs, no FFI.
//   * Opt-in: define MAYA_TREE_SITTER and provide a `TSHighlightBackend` that
//     runs a real tree-sitter parser + highlight query and emits the SAME
//     `Span` stream. The theme/render half is identical; only the tokenizer
//     swaps. (Wiring the FFI is left to the host build that opts in — the
//     architecture and the contract live here.)
//
// Output is a flat, sorted, non-overlapping `std::vector<Span>` covering the
// whole input (gaps are implicitly `Capture::None` / plain text). Render with
// `style_for(theme, span.capture)`.

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

#include "maya/style/style.hpp"

namespace maya::syntax {

// ============================================================================
// Capture — canonical highlight capture names (tree-sitter compatible)
// ============================================================================
// Mirrors the common subset of nvim-treesitter / helix capture names. Keep
// `Count` last; it sizes the theme table.

enum class Capture : uint8_t {
    None = 0,     // plain text (theme default)
    Keyword,      // @keyword           if/for/return/def/fn/...
    KeywordCtrl,  // @keyword.control   control-flow subset (optional accent)
    Type,         // @type              int / String / MyClass
    Function,     // @function          call + definition names
    String,       // @string            "..." '...' `...`
    Number,       // @number / @float   42  3.14  0xFF
    Comment,      // @comment           // ... # ... /* ... */
    Constant,     // @constant          true / false / null / NULL / PI
    Operator,     // @operator          + - * / == => ...
    Punctuation,  // @punctuation       (){}[];,.
    Preproc,      // @preproc           #include / #define / decorators
    Attribute,    // @attribute         @decorator / #[derive] / annotations
    Variable,     // @variable          $VAR / general identifiers
    Property,     // @property          obj.field
    Count
};

[[nodiscard]] constexpr std::size_t capture_index(Capture c) noexcept {
    return static_cast<std::size_t>(c);
}

// ============================================================================
// Span — a styled byte range produced by the tokenizer
// ============================================================================

struct Span {
    std::uint32_t start = 0;   // byte offset into the source
    std::uint32_t len   = 0;   // byte length
    Capture       cap   = Capture::None;
};

// ============================================================================
// HighlightTheme — constexpr Capture → Style map
// ============================================================================

struct HighlightTheme {
    std::array<Style, static_cast<std::size_t>(Capture::Count)> styles{};

    [[nodiscard]] constexpr Style style_for(Capture c) const noexcept {
        return styles[capture_index(c)];
    }

    // Builder: set one capture's style at compile time and return a new theme.
    [[nodiscard]] constexpr HighlightTheme with(Capture c, Style s) const noexcept {
        HighlightTheme t = *this;
        t.styles[capture_index(c)] = s;
        return t;
    }
};

[[nodiscard]] constexpr Style style_for(const HighlightTheme& t, Capture c) noexcept {
    return t.style_for(c);
}

// ── Built-in themes ─────────────────────────────────────────────────────────
namespace themes {

// Default 16-colour terminal theme — matches the existing markdown palette so
// nothing regresses visually when a block routes through this path.
inline constexpr HighlightTheme terminal = [] {
    HighlightTheme t{};
    using S = Style;
    t.styles[capture_index(Capture::None)]        = S{}.with_fg(Color::slot(ThemeSlot::Text));
    t.styles[capture_index(Capture::Keyword)]     = S{}.with_fg(Color::slot(ThemeSlot::Accent));
    t.styles[capture_index(Capture::KeywordCtrl)] = S{}.with_fg(Color::slot(ThemeSlot::Accent)).with_bold();
    t.styles[capture_index(Capture::Type)]        = S{}.with_fg(Color::slot(ThemeSlot::Info));
    t.styles[capture_index(Capture::Function)]    = S{}.with_fg(Color::slot(ThemeSlot::Primary));
    t.styles[capture_index(Capture::String)]      = S{}.with_fg(Color::slot(ThemeSlot::Success));
    t.styles[capture_index(Capture::Number)]      = S{}.with_fg(Color::slot(ThemeSlot::Warning));
    t.styles[capture_index(Capture::Comment)]     = S{}.with_fg(Color::slot(ThemeSlot::Muted)).with_italic();
    t.styles[capture_index(Capture::Constant)]    = S{}.with_fg(Color::slot(ThemeSlot::Warning));
    t.styles[capture_index(Capture::Operator)]    = S{}.with_fg(Color::slot(ThemeSlot::Error));
    t.styles[capture_index(Capture::Punctuation)] = S{}.with_fg(Color::slot(ThemeSlot::Muted));
    t.styles[capture_index(Capture::Preproc)]     = S{}.with_fg(Color::slot(ThemeSlot::Warning));
    t.styles[capture_index(Capture::Attribute)]   = S{}.with_fg(Color::slot(ThemeSlot::Warning));
    t.styles[capture_index(Capture::Variable)]    = S{}.with_fg(Color::slot(ThemeSlot::Link));
    t.styles[capture_index(Capture::Property)]    = S{}.with_fg(Color::slot(ThemeSlot::Text));
    return t;
}();

// Monokai (truecolor). The roadmap's named example.
inline constexpr HighlightTheme monokai = [] {
    HighlightTheme t{};
    using S = Style;
    t.styles[capture_index(Capture::None)]        = S{}.with_fg(Color::hex(0xF8F8F2));
    t.styles[capture_index(Capture::Keyword)]     = S{}.with_fg(Color::hex(0xF92672));
    t.styles[capture_index(Capture::KeywordCtrl)] = S{}.with_fg(Color::hex(0xF92672)).with_bold();
    t.styles[capture_index(Capture::Type)]        = S{}.with_fg(Color::hex(0x66D9EF)).with_italic();
    t.styles[capture_index(Capture::Function)]    = S{}.with_fg(Color::hex(0xA6E22E));
    t.styles[capture_index(Capture::String)]      = S{}.with_fg(Color::hex(0xE6DB74));
    t.styles[capture_index(Capture::Number)]      = S{}.with_fg(Color::hex(0xAE81FF));
    t.styles[capture_index(Capture::Comment)]     = S{}.with_fg(Color::hex(0x75715E)).with_italic();
    t.styles[capture_index(Capture::Constant)]    = S{}.with_fg(Color::hex(0xAE81FF));
    t.styles[capture_index(Capture::Operator)]    = S{}.with_fg(Color::hex(0xF92672));
    t.styles[capture_index(Capture::Punctuation)] = S{}.with_fg(Color::hex(0xF8F8F2));
    t.styles[capture_index(Capture::Preproc)]     = S{}.with_fg(Color::hex(0xA6E22E));
    t.styles[capture_index(Capture::Attribute)]   = S{}.with_fg(Color::hex(0xA6E22E));
    t.styles[capture_index(Capture::Variable)]    = S{}.with_fg(Color::hex(0xF8F8F2));
    t.styles[capture_index(Capture::Property)]    = S{}.with_fg(Color::hex(0x66D9EF));
    return t;
}();

// GitHub Dark (truecolor).
inline constexpr HighlightTheme github_dark = [] {
    HighlightTheme t{};
    using S = Style;
    t.styles[capture_index(Capture::None)]        = S{}.with_fg(Color::slot(ThemeSlot::Text));
    t.styles[capture_index(Capture::Keyword)]     = S{}.with_fg(Color::hex(0xFF7B72));
    t.styles[capture_index(Capture::KeywordCtrl)] = S{}.with_fg(Color::hex(0xFF7B72));
    t.styles[capture_index(Capture::Type)]        = S{}.with_fg(Color::hex(0xFFA657));
    t.styles[capture_index(Capture::Function)]    = S{}.with_fg(Color::hex(0xD2A8FF));
    t.styles[capture_index(Capture::String)]      = S{}.with_fg(Color::hex(0xA5D6FF));
    t.styles[capture_index(Capture::Number)]      = S{}.with_fg(Color::hex(0x79C0FF));
    t.styles[capture_index(Capture::Comment)]     = S{}.with_fg(Color::hex(0x8B949E)).with_italic();
    t.styles[capture_index(Capture::Constant)]    = S{}.with_fg(Color::hex(0x79C0FF));
    t.styles[capture_index(Capture::Operator)]    = S{}.with_fg(Color::hex(0xFF7B72));
    t.styles[capture_index(Capture::Punctuation)] = S{}.with_fg(Color::slot(ThemeSlot::Text));
    t.styles[capture_index(Capture::Preproc)]     = S{}.with_fg(Color::hex(0xFF7B72));
    t.styles[capture_index(Capture::Attribute)]   = S{}.with_fg(Color::hex(0xD2A8FF));
    t.styles[capture_index(Capture::Variable)]    = S{}.with_fg(Color::slot(ThemeSlot::Text));
    t.styles[capture_index(Capture::Property)]    = S{}.with_fg(Color::hex(0x79C0FF));
    return t;
}();

} // namespace themes

// ============================================================================
// Lang — language selector for the built-in tokenizer
// ============================================================================

enum class Lang : uint8_t {
    Generic,   // C-family-ish defaults; works passably for unknown langs
    C, Cpp, Python, Rust, Go, JavaScript, TypeScript, Shell, Json,
};

// Map a fenced-code language tag to a Lang. Case-insensitive on the common
// aliases. Unknown ⇒ Generic.
[[nodiscard]] Lang lang_from_tag(std::string_view tag) noexcept;

// ============================================================================
// Tokenizer entry points
// ============================================================================

// Built-in lexer. Appends sorted, non-overlapping spans covering `src` into
// `out` (cleared first). Always available, dependency-free.
void builtin_highlight(std::string_view src, Lang lang, std::vector<Span>& out);

// Public façade. Routes to the tree-sitter backend when compiled in and a
// grammar is available for `lang`; otherwise the built-in lexer. Identical
// span contract either way.
[[nodiscard]] std::vector<Span> highlight(std::string_view src, Lang lang);

inline void highlight(std::string_view src, Lang lang, std::vector<Span>& out) {
#ifdef MAYA_TREE_SITTER
    if (ts_highlight(src, lang, out)) return;   // backend filled `out`
#endif
    builtin_highlight(src, lang, out);
}

#ifdef MAYA_TREE_SITTER
// Host-provided tree-sitter backend. Returns true and fills `out` if a grammar
// for `lang` is available; false to fall through to the built-in lexer. The
// host links the tree-sitter runtime + grammars and defines this symbol.
[[nodiscard]] bool ts_highlight(std::string_view src, Lang lang,
                                std::vector<Span>& out);
#endif

} // namespace maya::syntax
