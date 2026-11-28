// syntax_internal.hpp — types + style palette shared between the language
// tables (syntax_lang.cpp) and the highlighter (syntax_highlight.cpp).
// Internal to the markdown render module; NOT installed for public use.

#pragma once

#include <cstdint>
#include <initializer_list>
#include <string_view>

#include "maya/style/style.hpp"
#include "maya/widget/agentty/markdown/internal.hpp"

namespace maya {
namespace syntax_detail {

// ── Language identification ──────────────────────────────────────────────────
enum class LangId {
    Unknown,
    C, Cpp, Python, Rust, JavaScript, TypeScript, Go, Java, Kotlin, Swift,
    Ruby, Shell, Fish, SQL, HTML, XML, CSS, SCSS,
    JSON, YAML, TOML, Lua, Zig, Haskell, Elixir, Erlang, PHP, Perl, R,
    Diff, Makefile, CMake, Dockerfile, Markdown,
};

struct WordClass { bool keyword; bool type; bool constant; };

struct CommentStyle {
    const char* line;         // "//" or "#" or "--" or nullptr
    const char* block_open;   // "/*" or "{-" or nullptr
    const char* block_close;  // "*/" or "-}" or nullptr
    bool hash_comment;        // '#' as a line comment
};

struct LangFeatures {
    bool triple_quote_strings;  // Python """...""", '''...'''
    bool backtick_strings;      // JS `...`, Go raw strings
    bool preprocessor;          // C/C++ #include, #define
    bool decorators;            // Python/Java @, Rust #[...]
    bool shell_vars;            // $VAR, ${VAR}
    bool char_literals;         // 'c' is a char, not a string
    bool lifetime;              // Rust 'a lifetime annotations
    bool colon_atom;            // Ruby/Elixir :symbol
};

// Defined in syntax_lang.cpp.
[[nodiscard]] LangId       detect_lang(std::string_view tag);
[[nodiscard]] WordClass    classify_word(std::string_view word, LangId lang);
[[nodiscard]] CommentStyle comment_style_for(LangId lang);
[[nodiscard]] LangFeatures features_for(LangId lang);

[[nodiscard]] inline bool in_list(
    std::string_view word,
    std::initializer_list<std::string_view> list) {
    for (auto& k : list) if (word == k) return true;
    return false;
}

} // namespace syntax_detail

// ── Token style palette ─────────────────────────────────────────────
// Used by both the diff highlighter and the language tokeniser.
//
// These were `static const`, which meant they were constructed on first use
// and then frozen for the life of the process — so code blocks kept their
// launch-time colours no matter what theme you picked afterwards. In a chat
// TUI code blocks are a large share of what is on screen, which is most of
// why a theme change looked like it "barely did anything".
//
// They are now derived from the live markdown palette, which the runtime
// re-projects from the Theme on every swap. Each accessor is a cheap struct
// copy off values that only change when the theme does — the tokeniser calls
// these per token, so they must not allocate or lock, and they don't.
//
// The MAPPING is semantic, not literal: keyword=accent, string=success,
// number/constant=warning, comment/punct=muted, and so on. That keeps the
// familiar terminal look under `native` (whose slots are the named ANSI
// colours) while letting a scheme recolour code in its own hues.
namespace syntax {
    inline Style kw()       { return Style{}.with_fg(colors::heading3()); }
    inline Style ctrl()     { return Style{}.with_fg(colors::heading3()).with_bold(); }
    inline Style type()     { return Style{}.with_fg(colors::code_fg()); }
    inline Style fn()       { return Style{}.with_fg(colors::heading1()); }
    inline Style str()      { return Style{}.with_fg(colors::checkbox_fg()); }
    inline Style num()      { return Style{}.with_fg(colors::quote_bar()); }
    inline Style comment()  { return Style{}.with_fg(colors::code_lang()).with_italic(); }
    inline Style constant() { return Style{}.with_fg(colors::quote_bar()); }
    inline Style preproc()  { return Style{}.with_fg(colors::alert_warning()); }
    inline Style attr()     { return Style{}.with_fg(colors::alert_warning()); }
    inline Style op()       { return Style{}.with_fg(colors::alert_caution()); }
    inline Style punct()    { return Style{}.with_fg(colors::code_lang()); }
    inline Style plain()    { return Style{}.with_fg(colors::text()); }
    inline Style shellvar() { return Style{}.with_fg(colors::mention_fg()); }
    inline Style gutter()   { return Style{}.with_fg(colors::code_lang()).with_dim(); }

    inline Style diff_add()  { return Style{}.with_fg(colors::checkbox_fg()); }
    inline Style diff_del()  { return Style{}.with_fg(colors::alert_caution()); }
    inline Style diff_hunk() { return Style{}.with_fg(colors::code_fg()); }
    inline Style diff_meta() { return Style{}.with_fg(colors::code_lang()).with_bold(); }
}

} // namespace maya
