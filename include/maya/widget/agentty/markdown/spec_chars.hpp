#pragma once
// spec_chars.hpp — compile-time character classification for the markdown
// engine. One source of truth for the byte-class tables that both the
// parser (block + inline) and the syntax highlighter consult in their
// hot inner loops.
//
// Each table is a 256-byte rodata lookup built at compile time via a
// fold over a non-type template parameter pack, so membership is an
// O(1) array index with no branching. The `proofs` namespace pins the
// membership of every table with static_asserts — change a table and a
// wrong cell breaks the build, not a test.
//
// Private to the markdown implementation; NOT installed.

#include <cstdint>

namespace maya::md_detail::chars {

// 256-entry boolean lookup table, constructed at compile time.
struct CharTable {
    bool v[256]{};
    [[nodiscard]] constexpr bool operator[](unsigned char c) const noexcept {
        return v[c];
    }
    [[nodiscard]] constexpr bool operator[](char c) const noexcept {
        return v[static_cast<unsigned char>(c)];
    }
};

template <unsigned char... Cs>
[[nodiscard]] consteval CharTable make_table() noexcept {
    CharTable t{};
    ((t.v[Cs] = true), ...);
    return t;
}

// ── escapable punctuation (current engine's set) ────────────────────
// NOTE: behavior-preserving carve — this mirrors the set parser.cpp
// used before the split. CommonMark §2.4 actually escapes ALL ASCII
// punctuation; widening this set is a deliberate, separately-measured
// step in the engine rebuild (it moves the Backslash-escapes section),
// not part of the file carve. `kAsciiPunct` below is the full spec set
// for the rewrite to switch to.
inline constexpr CharTable kEscapable = make_table<
    '\\', '`', '*', '_', '{', '}', '[', ']', '(', ')', '#', '+',
    '-', '.', '!', '|', '~', '<', '>', '"', '\'', '^'>();

// Full CommonMark ASCII-punctuation set (spec §6.1). The rewrite's
// backslash-escape and flanking logic switch to this.
inline constexpr CharTable kAsciiPunct = make_table<
    '!', '"', '#', '$', '%', '&', '\'', '(', ')', '*', '+', ',',
    '-', '.', '/', ':', ';', '<', '=', '>', '?', '@', '[', '\\',
    ']', '^', '_', '`', '{', '|', '}', '~'>();

// ── inline-scanner trigger bytes (current engine's set) ─────────────
// Mirrors parser.cpp's kInlineSpecial pre-split. URLs, mentions,
// entities and emoji are handled in a separate Text post-pass, so
// their leads (h/w/@/#/&/:) are deliberately absent here.
inline constexpr CharTable kInlineSpecial = make_table<
    '`', '*', '_', '~', '[', '!', '\\', '<', '$',
    '=', '^'>();

// ── syntax-highlighter classes ──────────────────────────────────────
inline constexpr CharTable kPunct = make_table<
    '{', '}', '[', ']', '(', ')', '.', ',', ';', ':',
    '<', '>', '?', '~', '%', '@', '\\'>();

inline constexpr CharTable kOp = make_table<
    '+', '-', '*', '/', '=', '!', '&', '|', '^'>();

// ── ASCII helpers used across block + inline scanning ───────────────
[[nodiscard]] constexpr bool is_space_or_tab(char c) noexcept {
    return c == ' ' || c == '\t';
}
[[nodiscard]] constexpr bool is_ascii_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}
[[nodiscard]] constexpr bool is_ascii_alpha(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
[[nodiscard]] constexpr bool is_ascii_alnum(char c) noexcept {
    return is_ascii_alpha(c) || is_ascii_digit(c);
}
// CommonMark "ASCII punctuation" (spec §6.1), used by flanking rules.
[[nodiscard]] constexpr bool is_ascii_punct(char c) noexcept {
    return kAsciiPunct[c];
}
// Unicode-whitespace approximation at the ASCII level (flanking rules
// only need to distinguish whitespace / punctuation / other at byte
// granularity for the ASCII delimiters maya supports).
[[nodiscard]] constexpr bool is_ascii_ws(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           c == '\f' || c == '\v';
}

// ── compile-time proofs ─────────────────────────────────────────────
namespace proofs {
    static_assert(kEscapable['*'] && kEscapable['\\'] && kEscapable['~'] &&
                  kEscapable['"'] && kEscapable['_'],
                  "escapable set must include core CommonMark punctuation");
    static_assert(!kEscapable['a'] && !kEscapable['0'] && !kEscapable[' '],
                  "escapable set is punctuation-only");

    static_assert(kAsciiPunct['&'] && kAsciiPunct['/'] && kAsciiPunct['@'] &&
                  kAsciiPunct['*'] && kAsciiPunct[','],
                  "full ASCII-punctuation set");
    static_assert(!kAsciiPunct['a'] && !kAsciiPunct[' '],
                  "ASCII-punctuation set is punctuation-only");

    static_assert(kInlineSpecial['*'] && kInlineSpecial['['] &&
                  kInlineSpecial['`'] && kInlineSpecial['\\'] &&
                  kInlineSpecial['<'],
                  "inline scanner must stop on every emphasis/link/code lead");
    static_assert(!kInlineSpecial['a'] && !kInlineSpecial[' '] &&
                  !kInlineSpecial['@'],
                  "inline scanner must NOT stop on prose bytes");

    static_assert(is_ascii_digit('0') && is_ascii_digit('9') &&
                  !is_ascii_digit('a'), "digit class");
    static_assert(is_ascii_alpha('z') && is_ascii_alpha('A') &&
                  !is_ascii_alpha('_'), "alpha class");
}

} // namespace maya::md_detail::chars
