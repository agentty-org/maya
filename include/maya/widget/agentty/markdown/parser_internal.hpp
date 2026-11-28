// parser_internal.hpp — small helpers shared between parser_inline.cpp and
// parser_block.cpp. TU-private to the parser module; NOT installed.
//
// The reference-link TLS slot (tls_ref_defs) is DEFINED in parser_inline.cpp
// (alongside the RefDefsScope ctor/dtor that the public internal.hpp
// declares) and declared extern here so the block parser's lookup path and
// collect_ref_defs see the same slot.

#pragma once

#include <cctype>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>

#include "maya/widget/agentty/markdown.hpp"
#include "maya/widget/agentty/markdown/internal.hpp"
#include "maya/widget/agentty/markdown/spec_chars.hpp"

namespace maya {
namespace md_detail {

// Active reference-link map for the current parse. Set by RefDefsScope.
extern thread_local const std::unordered_map<std::string, md::LinkRef>*
    tls_ref_defs;

namespace parser_detail {

namespace chars = ::maya::md_detail::chars;

[[nodiscard]] inline std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
    return s;
}

[[nodiscard]] inline bool starts_with(std::string_view s,
                                      std::string_view prefix) {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

[[nodiscard]] inline bool is_escapable(char c) {
    return chars::kEscapable[static_cast<unsigned char>(c)];
}

[[nodiscard]] inline std::string ascii_lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

[[nodiscard]] inline const md::LinkRef* lookup_ref(std::string_view label) {
    if (!tls_ref_defs) return nullptr;
    auto key = ascii_lower(label);
    // Collapse whitespace runs to a single space (CommonMark normalization).
    std::string norm;
    norm.reserve(key.size());
    bool prev_space = false;
    for (char c : key) {
        bool sp = std::isspace(static_cast<unsigned char>(c));
        if (sp) {
            if (!prev_space && !norm.empty()) norm += ' ';
            prev_space = true;
        } else {
            norm += c;
            prev_space = false;
        }
    }
    while (!norm.empty() && norm.back() == ' ') norm.pop_back();
    auto it = tls_ref_defs->find(norm);
    return (it == tls_ref_defs->end()) ? nullptr : &it->second;
}

// ── block helpers ──────────────────────────────────────────────────────────

// Remove up to `n` spaces of indentation (tab counts as 4).
[[nodiscard]] inline std::string_view dedent(std::string_view line, int n) {
    int removed = 0;
    std::size_t i = 0;
    while (i < line.size() && removed < n) {
        if (line[i] == ' ') { ++removed; ++i; }
        else if (line[i] == '\t') { removed += 4; ++i; }
        else break;
    }
    return line.substr(i);
}

// Starting number of an ordered-list line.
[[nodiscard]] inline int ol_start_num(std::string_view line) {
    auto t = trim(line);
    std::size_t d = 0;
    while (d < t.size() && std::isdigit(static_cast<unsigned char>(t[d]))) ++d;
    int num = 0;
    for (std::size_t k = 0; k < d; ++k) num = num * 10 + (t[k] - '0');
    return num;
}

// Task-list checkbox: -1 = not a task, 0 = unchecked, 1 = checked.
[[nodiscard]] inline int parse_task_checkbox(std::string_view content) {
    if (content.size() >= 4 && content[0] == '[' && content[2] == ']' &&
        content[3] == ' ') {
        if (content[1] == ' ') return 0;
        if (content[1] == 'x' || content[1] == 'X') return 1;
    }
    return -1;
}

// SIMD-accelerated newline search via memchr.
[[nodiscard]] inline std::size_t find_eol(const char* data, std::size_t start,
                                          std::size_t end) noexcept {
    if (start >= end) return end;
    auto* p = static_cast<const char*>(
        std::memchr(data + start, '\n', end - start));
    return p ? static_cast<std::size_t>(p - data) : end;
}

} // namespace parser_detail
} // namespace md_detail
} // namespace maya
