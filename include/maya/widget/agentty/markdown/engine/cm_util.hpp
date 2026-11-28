#pragma once
// cm_util.hpp — engine-private utilities for the CommonMark core.
//
// Spec-precise character classification, UTF-8 decode, percent/entity
// normalization, and small string helpers shared by the block- and
// inline-structure phases. All free functions in the engine namespace;
// no state. Private to the markdown implementation; NOT installed.

#include <cstdint>
#include <string>
#include <string_view>

#include "maya/widget/agentty/markdown/spec_chars.hpp"
#include "maya/widget/agentty/markdown/engine/cm_entities_table.hpp"
#include "maya/widget/agentty/markdown/engine/cm_unicode_table.hpp"

namespace maya::md_detail::engine {

namespace chars = ::maya::md_detail::chars;

using chars::is_ascii_alpha;
using chars::is_ascii_alnum;
using chars::is_ascii_digit;
using chars::is_ascii_punct;

constexpr char NUL = '\0';

[[nodiscard]] inline bool is_space_or_tab(char c) noexcept {
    return c == ' ' || c == '\t';
}

// ASCII-level whitespace test, used where only ASCII matters (link
// destinations, the reference-definition scanner).
[[nodiscard]] inline bool is_unicode_ws_byte(unsigned char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' ||
           c == '\f' || c == '\r';
}

// ── UTF-8 code-point decode ───────────────────────────────────────────────
// Decode the code point beginning at byte `i`; sets `len` to its byte length
// (0 at end of input). Malformed lead bytes decode as the single byte.
[[nodiscard]] inline char32_t decode_cp_at(std::string_view s, std::size_t i,
                                           std::size_t& len) noexcept {
    if (i >= s.size()) { len = 0; return 0; }
    auto b = [&](std::size_t k) { return static_cast<unsigned char>(s[k]); };
    unsigned char c = b(i);
    if (c < 0x80) { len = 1; return c; }
    if ((c >> 5) == 0x6 && i + 1 < s.size()) {
        len = 2; return ((c & 0x1Fu) << 6) | (b(i + 1) & 0x3Fu);
    }
    if ((c >> 4) == 0xE && i + 2 < s.size()) {
        len = 3;
        return ((c & 0x0Fu) << 12) | ((b(i + 1) & 0x3Fu) << 6) | (b(i + 2) & 0x3Fu);
    }
    if ((c >> 3) == 0x1E && i + 3 < s.size()) {
        len = 4;
        return ((c & 0x07u) << 18) | ((b(i + 1) & 0x3Fu) << 12) |
               ((b(i + 2) & 0x3Fu) << 6) | (b(i + 3) & 0x3Fu);
    }
    len = 1; return c;
}

// Decode the code point ending just before byte `i` (the previous character).
[[nodiscard]] inline char32_t decode_cp_before(std::string_view s,
                                               std::size_t i) noexcept {
    if (i == 0) return 0;
    std::size_t j = i - 1;
    // back up over UTF-8 continuation bytes (10xxxxxx), at most 3
    while (j > 0 && (static_cast<unsigned char>(s[j]) & 0xC0) == 0x80 &&
           (i - j) < 4)
        --j;
    std::size_t len;
    return decode_cp_at(s, j, len);
}

// ── Unicode classification (CommonMark §2.1) ──────────────────────────────
[[nodiscard]] inline bool in_cp_ranges(char32_t cp, const CpRange* r,
                                       int n) noexcept {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (cp < r[mid].first) hi = mid - 1;
        else if (cp > r[mid].last) lo = mid + 1;
        else return true;
    }
    return false;
}

// "Unicode whitespace character": Zs category plus tab/LF/FF/CR/space.
[[nodiscard]] inline bool is_unicode_whitespace_cp(char32_t cp) noexcept {
    if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\f' || cp == '\r')
        return true;
    return in_cp_ranges(cp, kWhitespaceRanges, kWhitespaceRangesCount);
}

// "Unicode punctuation character": general category P* or S*.
[[nodiscard]] inline bool is_unicode_punct_cp(char32_t cp) noexcept {
    return in_cp_ranges(cp, kPunctRanges, kPunctRangesCount);
}

// ── line helpers ────────────────────────────────────────────────────────────

[[nodiscard]] inline std::string_view rstrip(std::string_view s) {
    while (!s.empty() &&
           (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' ||
            s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}

[[nodiscard]] inline std::string_view lstrip(std::string_view s) {
    while (!s.empty() && is_space_or_tab(s.front())) s.remove_prefix(1);
    return s;
}

[[nodiscard]] inline std::string_view strip(std::string_view s) {
    return rstrip(lstrip(s));
}

[[nodiscard]] inline bool is_blank(std::string_view s) {
    for (char c : s)
        if (!is_space_or_tab(c) && c != '\n' && c != '\r') return false;
    return true;
}

// Count leading indentation in columns: space = 1, tab = advance to next
// multiple of 4. Returns columns and sets `bytes` to chars consumed.
[[nodiscard]] inline int leading_indent(std::string_view s, std::size_t& bytes) {
    int col = 0;
    std::size_t i = 0;
    for (; i < s.size(); ++i) {
        if (s[i] == ' ') ++col;
        else if (s[i] == '\t') col += 4 - (col % 4);
        else break;
    }
    bytes = i;
    return col;
}

// ── case folding ────────────────────────────────────────────────────────────

[[nodiscard]] inline std::string ascii_lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        out += c;
    }
    return out;
}

// Full Unicode case fold of a single code point (CaseFolding.txt C+F). Falls
// back to the code point's own bytes when it has no fold mapping.
[[nodiscard]] inline std::string_view lookup_case_fold(char32_t cp) noexcept {
    int lo = 0, hi = kCaseFoldCount - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (cp < kCaseFold[mid].cp) hi = mid - 1;
        else if (cp > kCaseFold[mid].cp) lo = mid + 1;
        else return kCaseFold[mid].folded;
    }
    return {};
}

// CommonMark reference-label normalization (§6.3): strip, collapse internal
// whitespace runs to one space, and perform a full Unicode case fold (so
// `[ΑΓΩ]` matches `[αγω]` and `[ẞ]` matches `[SS]`).
[[nodiscard]] inline std::string normalize_label(std::string_view label) {
    std::string out;
    out.reserve(label.size());
    bool prev_ws = false;
    bool started = false;
    std::size_t i = 0;
    while (i < label.size()) {
        std::size_t len;
        char32_t cp = decode_cp_at(label, i, len);
        if (len == 0) break;
        bool ws = (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r');
        if (ws) {
            prev_ws = true;
            i += len;
            continue;
        }
        if (prev_ws && started) out += ' ';
        prev_ws = false;
        started = true;
        std::string_view folded = lookup_case_fold(cp);
        if (!folded.empty()) out += folded;
        else out += label.substr(i, len);
        i += len;
    }
    return out;
}

// ── UTF-8 ───────────────────────────────────────────────────────────────────

// Append code point `cp` as UTF-8. Invalid / zero → U+FFFD per spec.
inline void append_utf8(std::string& out, uint32_t cp) {
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        cp = 0xFFFD;
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// ── entity resolution (§6.2) ────────────────────────────────────────────────

// Look up a named entity (without leading & / trailing ;). Returns the UTF-8
// bytes, or empty if unknown.
[[nodiscard]] inline std::string_view lookup_entity(std::string_view name) {
    int lo = 0, hi = kEntityCount - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        auto cmp = kEntities[mid].name.compare(name);
        if (cmp == 0) return kEntities[mid].utf8;
        if (cmp < 0) lo = mid + 1;
        else hi = mid - 1;
    }
    return {};
}

// Attempt to decode an entity / numeric character reference beginning at
// text[i] (text[i] == '&'). On success appends the decoded UTF-8 to `out`
// and returns the number of input bytes consumed (incl. & and ;). On
// failure returns 0 (caller emits '&' literally).
[[nodiscard]] inline std::size_t decode_entity(std::string_view text,
                                               std::size_t i, std::string& out) {
    if (i >= text.size() || text[i] != '&') return 0;
    std::size_t j = i + 1;
    if (j < text.size() && text[j] == '#') {
        // numeric
        ++j;
        uint32_t cp = 0;
        bool hex = false;
        std::size_t digits = 0;
        if (j < text.size() && (text[j] == 'x' || text[j] == 'X')) {
            hex = true;
            ++j;
            while (j < text.size() && digits < 7 &&
                   std::isxdigit(static_cast<unsigned char>(text[j]))) {
                char c = text[j];
                int v = (c >= '0' && c <= '9') ? c - '0'
                        : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                                                 : c - 'A' + 10;
                cp = cp * 16 + static_cast<uint32_t>(v);
                ++j;
                ++digits;
            }
        } else {
            while (j < text.size() && digits < 7 && is_ascii_digit(text[j])) {
                cp = cp * 10 + static_cast<uint32_t>(text[j] - '0');
                ++j;
                ++digits;
            }
        }
        (void)hex;
        if (digits == 0 || j >= text.size() || text[j] != ';') return 0;
        ++j;  // consume ;
        append_utf8(out, cp);
        return j - i;
    }
    // named: &name;
    std::size_t k = j;
    while (k < text.size() && is_ascii_alnum(text[k])) ++k;
    if (k == j || k >= text.size() || text[k] != ';') return 0;
    auto name = text.substr(j, k - j);
    auto utf8 = lookup_entity(name);
    if (utf8.empty()) return 0;
    out += utf8;
    return (k + 1) - i;
}

// Decode all entity / numeric character references in a run of text,
// leaving everything else verbatim. Used for link destinations, titles,
// and code-fence info strings (§6.2 applies there too).
[[nodiscard]] inline std::string decode_entities(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        if (s[i] == '&') {
            std::string dec;
            std::size_t used = decode_entity(s, i, dec);
            if (used) { out += dec; i += used; continue; }
        }
        out += s[i++];
    }
    return out;
}

} // namespace maya::md_detail::engine
