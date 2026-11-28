// streaming_internal.hpp — declarations shared across the StreamingMarkdown TUs.
//
// streaming.cpp was carved into focused TUs (memo / boundary / commit /
// render_tail / build / folding / async). The helpers that were file-scope
// anonymous-namespace functions in the monolith — but are now called from
// more than one of those TUs — live here in the internal
// `maya::md_detail::streaming` namespace. Private to the implementation;
// NOT installed, NOT included by public consumers.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <atomic>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

#include "maya/element/element.hpp"
#include "maya/widget/agentty/markdown.hpp"
#include "maya/widget/agentty/markdown/ast.hpp"

namespace maya {

// assemble_markdown: Document → Element (vstack of block elements with the
// standard 2-col indent). Defined in markdown_memo.cpp; used there by the
// memoised markdown() entry point and re-exported via md_detail.
[[nodiscard]] Element assemble_markdown(md::Document&& doc);

namespace md_detail {

// The big-document parse runs on maya::exec (core/executor.hpp): on jaal's
// pool under maya::run, joined before run() returns; inline with no runtime.
// The AsyncResult slot is a shared_ptr the job co-owns, so a
// StreamingMarkdown destroyed mid-parse just drops its copy.

namespace streaming {

// ── FNV-1a 64-bit ─────────────────────────────────────────────────────────
// Branch-free, memory-bound content hash. Used by the markdown() LRU memo
// and by StreamingMarkdown's per-frame tail/prefix equality short-circuits.
[[nodiscard]] inline std::uint64_t fnv1a64(const char* data,
                                           std::size_t n) noexcept {
    constexpr std::uint64_t kOffset = 0xcbf29ce484222325ULL;
    constexpr std::uint64_t kPrime  = 0x100000001b3ULL;
    std::uint64_t h = kOffset;
    for (std::size_t k = 0; k < n; ++k) {
        h ^= static_cast<unsigned char>(data[k]);
        h *= kPrime;
    }
    return h;
}

[[nodiscard]] inline std::uint64_t fnv1a64(std::string_view s) noexcept {
    return fnv1a64(s.data(), s.size());
}

// ── Intra-list blank-line classifier ──────────────────────────────────────
// Classify a blank-line position `i` (src[i] == '\n' at line start) as a
// real block boundary (No), intra-list whitespace that must NOT split the
// list (Yes), or "next line not here yet, defer" (Unknown). Defined in
// boundary.cpp; the only caller is find_block_boundary in the same TU, but
// it is published here so the predicate has a single authoritative home.
enum class IntraBlank : std::uint8_t { No, Yes, Unknown };
[[nodiscard]] IntraBlank classify_blank_line(std::string_view src,
                                             std::size_t i) noexcept;

// ── GFM table finality predicate ───────────────────────────────────────────
// NotATable: the `|` is just literal prose. Incomplete: looks like a table
// but finality unproven (caller must not advance past this line).
// EndsAt: table proven complete; .pos is the byte index of the first
// non-`|` line after it. Defined in boundary.cpp.
enum class TableScan : std::uint8_t { NotATable, Incomplete, EndsAt };
struct TableScanResult {
    TableScan   kind;
    std::size_t pos;  // only meaningful for EndsAt
};
[[nodiscard]] TableScanResult find_table_end(std::string_view src,
                                             std::size_t line_start) noexcept;

// ── Code-fence line classifier (single source of truth) ────────────────────
// The streaming widget tracks ``` / ~~~ fence parity in five places (the
// boundary scanner, commit_range's seg walker and its parity walker, the
// async worker, and render_tail's closer-suppression probe). Each used to
// hand-inline a bare 3-char test, which diverged from the real engine parser
// (engine/cm_block.cpp code_fence / append_to_leaf) on three axes — so
// committed_ / in_code_fence_ could describe a DIFFERENT document than
// parse_markdown_impl produced, committing prose as code (or vice versa)
// depending on chunk boundaries. This predicate mirrors the engine exactly:
//   • up to 3 leading spaces are allowed before the marker (spec §4.5);
//   • a marker is ≥3 of the SAME char (backtick or tilde);
//   • an OPENER records its (char, run length); a CLOSER must be the same
//     char AND run length ≥ the opener's, with only whitespace after it
//     (backtick openers additionally may not carry a backtick in the info
//     string — that makes it an inline code span, not a fence).
struct FenceState {
    bool        in_fence = false;  // parity BEFORE the line being classified
    char        open_ch  = '\0';   // fence char of the currently-open fence
    std::size_t open_len = 0;      // marker run length of the open fence
};

// Advance `st` across one line [line_start, line_end) (line_end excludes the
// terminating '\n'). Returns true when the line was a fence open/close (parity
// flipped). A non-fence line leaves `st` unchanged and returns false.
[[nodiscard]] inline bool fence_scan_line(FenceState& st, std::string_view src,
                                          std::size_t line_start,
                                          std::size_t line_end) noexcept {
    if (line_end > src.size()) line_end = src.size();
    std::size_t k = line_start;
    int sp = 0;
    while (k < line_end && src[k] == ' ' && sp < 4) { ++k; ++sp; }
    if (sp >= 4 || k >= line_end) return false;   // indented code, not a fence
    char c = src[k];
    if (c != '`' && c != '~') return false;
    std::size_t run = 0;
    while (k + run < line_end && src[k + run] == c) ++run;
    if (run < 3) return false;

    if (!st.in_fence) {
        // Opener. Backtick fences forbid a backtick anywhere in the info
        // string (that would be an inline code span); tilde fences allow it.
        if (c == '`') {
            for (std::size_t q = k + run; q < line_end; ++q)
                if (src[q] == '`') return false;
        }
        st.in_fence = true;
        st.open_ch  = c;
        st.open_len = run;
        return true;
    }
    // Potential closer: same char, run ≥ opener, only trailing whitespace.
    if (c != st.open_ch || run < st.open_len) return false;
    for (std::size_t q = k + run; q < line_end; ++q) {
        char cc = src[q];
        if (cc != ' ' && cc != '\t' && cc != '\r') return false;
    }
    st.in_fence = false;
    st.open_ch  = '\0';
    st.open_len = 0;
    return true;
}

} // namespace streaming
} // namespace md_detail
} // namespace maya
