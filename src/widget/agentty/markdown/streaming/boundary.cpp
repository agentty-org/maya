// boundary.cpp — StreamingMarkdown's resumable block-boundary scanner.
//
// Owns three things, all driving the "where does the committed prefix end"
// decision:
//   • classify_blank_line — is a \n\n separator a real boundary or just
//     intra-list whitespace?
//   • find_table_end      — is a `| … |` run a complete GFM table yet?
//   • StreamingMarkdown::find_block_boundary — the resumable line scanner
//     that persists its cursor / fence parity across calls so each frame
//     pays O(bytes-since-last-call), not O(committed_to_EOS).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "maya/widget/agentty/markdown.hpp"
#include "maya/widget/agentty/markdown/internal.hpp"

#include "maya/widget/agentty/markdown/streaming_internal.hpp"

namespace maya {

using ::maya::md_detail::ul_marker_len;
using ::maya::md_detail::ol_marker_len;
using ::maya::md_detail::count_indent;

namespace md_detail { namespace streaming {

IntraBlank classify_blank_line(std::string_view src, std::size_t i) noexcept {
    // i is the position of the second '\n' in a \n\n pair (line-start
    // sentinel). The previous character must be '\n', otherwise this
    // wasn't called from the blank-line branch — defensive.
    if (i == 0 || src[i - 1] != '\n') return IntraBlank::No;

    // ── Look back: walk to the start of the line ending at src[i-1].
    //
    // Bounded scan: a list-marker line is at most a few dozen bytes
    // ("  123. body" plus modest body). Pathological inputs (a streamed
    // reply with hundreds of intra-list blanks, each prior "line"
    // happening to be very long) used to make this an O(n) walk per
    // blank, called O(n) times per frame from find_block_boundary —
    // O(n²) on long streamed lists. Cap at kMaxListMarkerLineLen
    // bytes: if no '\n' is found within that window the prior "line"
    // is far too long to be a list-marker line, so the IntraBlank::No
    // verdict is correct without ever inspecting the bytes.
    constexpr std::size_t kMaxListMarkerLineLen = 256;
    std::size_t prev_end = i - 1;            // exclusive of the '\n' at i-1
    std::size_t prev_start = 0;
    std::size_t scan_floor = (prev_end > kMaxListMarkerLineLen)
                                ? (prev_end - kMaxListMarkerLineLen)
                                : 0;
    bool found_start = false;
    for (std::size_t k = prev_end; k > scan_floor; --k) {
        if (src[k - 1] == '\n') { prev_start = k; found_start = true; break; }
    }
    if (!found_start && scan_floor == 0) {
        // Reached the buffer start without seeing a '\n' — prev_start
        // really is 0, treat the entire prefix as the previous line.
        prev_start = 0;
        found_start = true;
    }
    if (!found_start) {
        // Hit the look-back cap without finding a line start. The
        // prior "line" is longer than kMaxListMarkerLineLen and so
        // cannot be a list marker; the blank line therefore is not
        // an intra-list separator.
        return IntraBlank::No;
    }
    std::string_view prev_line = src.substr(prev_start, prev_end - prev_start);
    bool prev_is_ol = ol_marker_len(prev_line) > 0;
    bool prev_is_ul = ul_marker_len(prev_line) > 0;
    int prev_indent = count_indent(prev_line);
    if (!prev_is_ol && !prev_is_ul) {
        // The line directly above the blank isn't a marker line — but it
        // may be a CONTINUATION line of a list item: an indented body
        // line, a definition-list `: def`, or a LAZY continuation (plain
        // prose at any indent hugging the item — CommonMark absorbs it
        // into the item's paragraph). Only a line that itself opens a
        // block-level structure (ATX heading, HR, fence, blockquote)
        // provably CLOSES the list; for those the blank is a real
        // boundary. For any other shape, walk back through the cohesive
        // run (no blank lines) looking for the nearest marker line; if
        // one exists the blank still separates two items of the SAME
        // list and splitting there re-numbers / re-flows the second half.
        auto is_structural_line = [](std::string_view ln) noexcept -> bool {
            std::size_t k = 0;
            while (k < 4 && k < ln.size() && ln[k] == ' ') ++k;
            if (k >= ln.size()) return false;
            char c = ln[k];
            if (c == '>') return true;                       // blockquote
            if (c == '#') {                                  // ATX heading
                std::size_t h = 0;
                while (h < 7 && k + h < ln.size() && ln[k + h] == '#') ++h;
                if (h >= 1 && h <= 6 &&
                    (k + h >= ln.size() || ln[k + h] == ' ' || ln[k + h] == '\t'))
                    return true;
            }
            if (c == '`' || c == '~') {                      // fence marker
                std::size_t run = 0;
                while (k + run < ln.size() && ln[k + run] == c) ++run;
                if (run >= 3) return true;
            }
            if (c == '-' || c == '*' || c == '_') {          // HR
                std::size_t markers = 0;
                bool hr = true;
                for (std::size_t q = k; q < ln.size(); ++q) {
                    char cc = ln[q];
                    if (cc == c) { ++markers; continue; }
                    if (cc != ' ' && cc != '\t' && cc != '\r') { hr = false; break; }
                }
                if (hr && markers >= 3) return true;
            }
            return false;
        };
        if (!is_structural_line(prev_line)) {
            // Walk back (bounded: kMaxLookbackLines lines of
            // kMaxListMarkerLineLen bytes each), stopping at blanks and
            // structural lines — both close the run.
            constexpr int kMaxLookbackLines = 24;
        std::size_t line_end2 = prev_start;   // exclusive end of line above
        for (int back_lines = 0;
             back_lines < kMaxLookbackLines && line_end2 > 0; ++back_lines) {
            // line_end2 points just past a '\n'; step to the previous line.
            std::size_t e = line_end2 - 1;    // the '\n'
            std::size_t s2 = e;
            std::size_t floor2 = (e > kMaxListMarkerLineLen)
                                     ? (e - kMaxListMarkerLineLen) : 0;
            bool found2 = false;
            for (std::size_t k = e; k > floor2; --k) {
                if (src[k - 1] == '\n') { s2 = k; found2 = true; break; }
            }
            if (!found2) {
                if (floor2 == 0) { s2 = 0; found2 = true; }
                else break;      // over-long line: not a marker line
            }
            std::string_view ln = src.substr(s2, e - s2);
            if (ln.empty()) break;             // blank — run boundary
            if (is_structural_line(ln)) break; // closes any list above it
            if (ol_marker_len(ln) > 0) { prev_is_ol = true; }
            else if (ul_marker_len(ln) > 0) { prev_is_ul = true; }
            if (prev_is_ol || prev_is_ul) {
                prev_indent = count_indent(ln);
                break;
            }
            line_end2 = s2;
        }
        }
    }
    // Same structural gate applies to the LINE ABOVE THE BLANK itself:
    // if it is a structural line the run above is closed and the blank is
    // a real boundary regardless of any marker found by the walkback.
    // (The walkback only runs when prev_line has no marker, so reaching
    // here with prev_is_* set via prev_line means prev_line IS a marker
    // line — never structural.)
    if (!prev_is_ol && !prev_is_ul) return IntraBlank::No;
    //                  the next line. Loose lists sometimes carry two
    //                  blank lines between items; treat the whole run
    //                  as one separator.
    std::size_t j = i;
    while (j < src.size() && src[j] == '\n') ++j;
    if (j >= src.size()) return IntraBlank::Unknown;  // next line not here yet

    std::size_t next_end = src.find('\n', j);
    bool next_complete = (next_end != std::string_view::npos);
    if (!next_complete) next_end = src.size();
    std::string_view next_line = src.substr(j, next_end - j);

    bool next_is_ol = ol_marker_len(next_line) > 0;
    bool next_is_ul = ul_marker_len(next_line) > 0;

    if (!next_is_ol && !next_is_ul) {
        // Next line has no marker. If it's still incomplete we can't
        // tell — it might become "2. ..." once a space + body arrives,
        // or it might become "Plain paragraph". Wait.
        if (!next_complete) return IntraBlank::Unknown;
        // A line indented to (or past) the item's CONTENT column is a
        // continuation of the item across the blank ("- nested\n\n
        //     indented code" is ONE loose list item in the full parse —
        // the 4-space line is item content, possibly indented code
        // INSIDE the item). Splitting there re-parsed the continuation
        // as a top-level indented code block: different render. The
        // content column for a bullet is marker col + 2; ordered
        // markers are wider, so +2 is a safe LOWER bound — anything
        // less indented is a lazy-continuation/paragraph shape that
        // the blank genuinely terminates.
        if (count_indent(next_line) >= prev_indent + 2)
            return IntraBlank::Yes;
        return IntraBlank::No;
    }

    // Both prev and next are list markers. Same KIND is sufficient: in
    // CommonMark a same-kind marker after a blank line CONTINUES the list
    // regardless of indent — a shallower marker is a sibling of an outer
    // level ("  - nested\n\n- item" is ONE loose list) and a deeper one
    // is nested content of the previous item. The old ±1 indent
    // tolerance split such lists into separate commits whose isolated
    // re-parses render differently (tight vs loose, extra gap row).
    // Holding cohesive is always safe: it only DELAYS the commit to the
    // next real boundary, where the whole run parses together.
    bool same_kind = (prev_is_ol && next_is_ol) || (prev_is_ul && next_is_ul);
    if (same_kind) return IntraBlank::Yes;
    return IntraBlank::No;
}

TableScanResult find_table_end(std::string_view src,
                               std::size_t line_start) noexcept
{
    // Skip leading spaces (up to 3 — GFM allows). A leading tab or
    // ≥4-space indent makes this a code line, not a table.
    auto first_non_space = [](std::string_view s) -> std::size_t {
        std::size_t k = 0;
        while (k < 4 && k < s.size() && s[k] == ' ') ++k;
        return k;
    };

    // Header row: if not yet terminated by '\n' we can't even check
    // that it starts with '|' meaningfully (the line is still being
    // built) — defer.
    std::size_t header_end = src.find('\n', line_start);
    if (header_end == std::string_view::npos) {
        // The caller already knows source_[line_start] == '|', so the
        // line at least PURPORTS to be a table row. Hold here.
        return {TableScan::Incomplete, 0};
    }
    auto header_line = src.substr(line_start, header_end - line_start);
    std::size_t h_off = first_non_space(header_line);
    if (h_off >= header_line.size() || header_line[h_off] != '|')
        return {TableScan::NotATable, 0};

    // Delimiter row.
    std::size_t delim_start = header_end + 1;
    if (delim_start >= src.size()) {
        // Header is terminated but delimiter hasn't started arriving.
        // Could still become a table — defer.
        return {TableScan::Incomplete, 0};
    }
    std::size_t delim_end = src.find('\n', delim_start);
    if (delim_end == std::string_view::npos) {
        // Delimiter line in progress — defer.
        return {TableScan::Incomplete, 0};
    }
    auto delim_line = src.substr(delim_start, delim_end - delim_start);
    std::size_t d_off = first_non_space(delim_line);
    if (d_off >= delim_line.size() || delim_line[d_off] != '|')
        return {TableScan::NotATable, 0};
    bool has_dash = false;
    bool ok = true;
    for (std::size_t k = d_off; k < delim_line.size(); ++k) {
        char c = delim_line[k];
        if (c == '-') { has_dash = true; continue; }
        if (c == '|' || c == ':' || c == ' ' || c == '\t' || c == '\r')
            continue;
        ok = false; break;
    }
    if (!ok || !has_dash) return {TableScan::NotATable, 0};

    // Walk body rows. GFM (and the engine's open_leaf) continue the table
    // through ANY non-blank line — a plain-text line is a 1-cell row, not
    // the end of the table (spec example: "| bar | baz |\nbar" keeps `bar`
    // as a row). The table only breaks at:
    //   • a blank line, or
    //   • a line that opens another block-level structure first in the
    //     engine's open_leaf / try_open_new_blocks order: ≥4-space indent
    //     (indented code), blockquote, list marker, thematic break, ATX
    //     heading, code fence, HTML block.
    // The old rule ("first non-`|` line ends the table") committed a
    // SHORTER table than the full parse produces whenever a plain line
    // hugged the last row — the committed re-parse then dropped that row
    // into its own paragraph, a settled-scrollback rewrite.
    auto is_blank_ln = [](std::string_view ln) noexcept {
        for (char c : ln)
            if (c != ' ' && c != '\t' && c != '\r') return false;
        return true;
    };
    auto is_hr_shape = [](std::string_view ln) noexcept {
        std::size_t k = 0;
        while (k < 4 && k < ln.size() && ln[k] == ' ') ++k;
        if (k >= ln.size()) return false;
        char m = ln[k];
        if (m != '-' && m != '*' && m != '_') return false;
        std::size_t markers = 0;
        for (std::size_t q = k; q < ln.size(); ++q) {
            char c = ln[q];
            if (c == m) { ++markers; continue; }
            if (c != ' ' && c != '\t' && c != '\r') return false;
        }
        return markers >= 3;
    };
    auto is_atx_shape = [](std::string_view ln) noexcept {
        std::size_t k = 0;
        while (k < 4 && k < ln.size() && ln[k] == ' ') ++k;
        std::size_t h = 0;
        while (h < 7 && k + h < ln.size() && ln[k + h] == '#') ++h;
        if (h < 1 || h > 6) return false;
        return k + h >= ln.size() || ln[k + h] == ' ' || ln[k + h] == '\t';
    };
    auto breaks_table = [&](std::string_view ln) noexcept -> bool {
        if (is_blank_ln(ln)) return true;
        if (count_indent(ln) >= 4) return true;          // indented code
        std::size_t k = 0;
        while (k < 4 && k < ln.size() && ln[k] == ' ') ++k;
        if (k >= ln.size()) return true;                 // ws-only (blank)
        char c = ln[k];
        if (c == '>') return true;                       // blockquote
        if (ul_marker_len(ln) > 0 || ol_marker_len(ln) > 0) return true;
        if (is_hr_shape(ln) || is_atx_shape(ln)) return true;
        if (c == '`' || c == '~') {                      // code fence opener
            md_detail::streaming::FenceState probe;
            if (md_detail::streaming::fence_scan_line(
                    probe, ln, 0, ln.size()))
                return true;
        }
        if (c == '<' && k + 1 < ln.size()) {             // HTML block
            char n = ln[k + 1];
            if ((n >= 'a' && n <= 'z') || (n >= 'A' && n <= 'Z') ||
                n == '/' || n == '!' || n == '?')
                return true;
        }
        return false;
    };
    std::size_t pos = delim_end + 1;
    while (pos < src.size()) {
        std::size_t eol = src.find('\n', pos);
        if (eol == std::string_view::npos) {
            // Last line of the buffer isn't terminated. Can't prove
            // finality — might become a table row when more bytes
            // arrive. Defer (but the table SHAPE is proven, so this
            // is Incomplete, not NotATable).
            return {TableScan::Incomplete, 0};
        }
        auto line = src.substr(pos, eol - pos);
        if (breaks_table(line)) {
            // Finality boundary. The table's header + delimiter row are
            // already verified above, so the bytes [line_start, pos) are
            // PROVABLY a complete table and can commit now — even if N
            // more tables (each with their own trailing blanks) follow,
            // they will each commit at their own boundary instead of
            // queueing behind the first breaking line in the buffer.
            return {TableScan::EndsAt, pos};
        }
        pos = eol + 1;
    }
    // Reached end of buffer with all lines `|`-prefixed and terminated.
    // Still incomplete: a future byte could be more table or a closing
    // line.
    return {TableScan::Incomplete, 0};
}

}} // namespace md_detail::streaming

// find_block_boundary lives on StreamingMarkdown; pull the helpers it calls
// into scope unqualified so the verbatim body below is unchanged.
using ::maya::md_detail::streaming::IntraBlank;
using ::maya::md_detail::streaming::classify_blank_line;
using ::maya::md_detail::streaming::TableScan;
using ::maya::md_detail::streaming::find_table_end;

size_t StreamingMarkdown::find_block_boundary() noexcept {
    // Resumable scan — see the scan_* / in_code_fence_ comments in the
    // header for the full design. The body below is byte-for-byte the
    // same boundary detection logic the prior const version had; only
    // the bookkeeping around it (cursor / fence parity / last-boundary)
    // is now persisted across calls instead of re-derived from
    // committed_ every time.
    //
    // The scan walks ONE LINE START at a time: i points at the byte
    // following a '\n' (or i == 0).  Detecting the blank-line separator
    // looks BACKWARD at line start: source_[i] == '\n' implies
    // source_[i-1] was also '\n' (or i == 0), i.e. we just crossed a
    // \n\n separator. Same correction applies to the heading marker.
    size_t i             = scan_cursor_;
    bool   in_fence      = scan_in_fence_;
    char   fence_ch      = scan_fence_open_ch_;
    size_t fence_len     = scan_fence_open_len_;
    bool   fence_safe    = scan_fence_safe_;
    size_t last_boundary = scan_last_boundary_;

    // Ledger recorder: every boundary the scanner discovers is ALSO
    // appended to scan_boundaries_ (strictly increasing, > committed_) so
    // the reveal-paced commit gate (commit_revealed_) can later commit up
    // to the largest boundary the typewriter cursor has swept — an
    // INTERMEDIATE boundary on a multi-block tail, which the single
    // last_boundary return value can't express.
    auto record = [&](size_t b) {
        last_boundary = b;
        if (b > committed_
            && (scan_boundaries_.empty() || scan_boundaries_.back() < b))
            scan_boundaries_.push_back(b);
    };

    // A line-start `pos` has a blank line above when pos == 0 or the
    // previous line is empty (walking back past the '\n' at pos-1 and any
    // '\r's lands on another '\n' or the buffer start). Eager commits that
    // SPLIT a run of non-blank lines are only safe when this holds —
    // otherwise the preceding block's shape is not provably final (lazy
    // paragraph continuation, setext target, open HTML block) and the
    // isolated re-parse of the committed range can differ cell-for-cell
    // from the full-document parse. See the ATX branch below for the
    // original rationale; the same gate now covers tables, math fences
    // and code-fence openers.
    auto has_blank_above = [&](std::size_t pos) noexcept -> bool {
        if (pos == 0) return true;
        std::size_t back = pos;
        if (back > 0 && source_[back - 1] == '\n') --back;
        while (back > 0 && source_[back - 1] == '\r') --back;
        return back == 0 || source_[back - 1] == '\n';
    };

    while (i < source_.size()) {
        bool at_line_start = (i == 0 || source_[i - 1] == '\n');

        if (at_line_start) {
            // Spec-faithful fence detection (≤3 indent, same-char + run-length
            // matched close) via the shared classifier. Peek the parity flip
            // on a probe copy so the existing eager-commit control flow below
            // (which needs the eol / marker-only handling) stays intact.
            std::size_t probe_eol = source_.find('\n', i);
            std::size_t line_end = (probe_eol == std::string::npos)
                                       ? source_.size() : probe_eol;
            md_detail::streaming::FenceState fprobe{in_fence, fence_ch, fence_len};
            bool is_code_fence =
                md_detail::streaming::fence_scan_line(
                    fprobe, std::string_view{source_}, i, line_end);
            // Math fence ($$): treat as a block fence ONLY when the
            // opener line is exactly "$$" (optionally with trailing
            // whitespace and/or \r), terminated by '\n', AND the
            // parity is coherent:
            //   • while a ``` / ~~~ code fence is open, a $$ line is
            //     CONTENT (LaTeX inside a code example) — treating it
            //     as a fence flipped the scanner's parity against the
            //     parser's and mis-committed everything after;
            //   • an OPENER additionally requires a blank line above.
            //     parse_markdown_impl has NO math block: a $$ hugging
            //     a paragraph is lazy continuation ("para $$ x $$" is
            //     ONE paragraph), so splitting there froze a different
            //     render than the full parse produces.
            // Plain `$$` followed by any content is paragraph-level
            // inline math (KaTeX inline) or just dollar signs in prose.
            bool is_math_fence = false;
            if (!is_code_fence && i + 2 <= source_.size() &&
                source_[i] == '$' && source_[i+1] == '$' &&
                (in_fence ? fence_ch == '$' : has_blank_above(i))) {
                std::size_t eol = source_.find('\n', i);
                if (eol != std::string::npos) {
                    bool only_ws = true;
                    for (std::size_t q = i + 2; q < eol; ++q) {
                        char cc = source_[q];
                        if (cc != ' ' && cc != '\t' && cc != '\r') {
                            only_ws = false; break;
                        }
                    }
                    is_math_fence = only_ws;
                }
                // If the opener line isn't terminated yet we can't
                // prove it's $$-only — leave is_math_fence false and
                // fall through to the normal line-step; a later call
                // with the '\n' present will re-evaluate.
            }
            if (is_code_fence || is_math_fence) {
                // Opening fence commits any prose that preceded it — but
                // ONLY when a blank line already separates them. Without
                // one the preceding block's shape isn't final: a kind-6
                // HTML block swallows fence-looking lines until a blank
                // line ("<div>…</div>\n```py" is ONE HtmlBlock in the
                // parser), so splitting there committed a phantom code
                // block. With the gate the whole run stays cohesive and
                // commits at the fence CLOSE (code) or the next real
                // blank line — the isolated re-parse then agrees with
                // the full parse.
                if (!in_fence && has_blank_above(i)) record(i);
                // A fence OPENER without a blank line above may not be a
                // fence at all in the full parse (content of a kind-6 HTML
                // block, paragraph interruption ambiguity). Track parity so
                // the scanner doesn't desync, but remember the opener was
                // UNSAFE: its close must not record a boundary either — the
                // whole run commits at the next real blank line, where the
                // isolated re-parse provably matches the full parse.
                if (!in_fence) fence_safe = has_blank_above(i);
                // Whether this line CLOSES a math fence (needed after the
                // probe adopt below overwrites the descriptor): a closed
                // math block, unlike a closed code fence, can still be
                // CONTINUED by the next line (it's paragraph text to the
                // parser), so no boundary is recorded at its close — the
                // blank-line rule commits it when a real separator lands.
                const bool math_close = is_math_fence && in_fence;
                // A math ($$) fence isn't handled by fence_scan_line (which
                // only knows ``` / ~~~); flip the probe manually so the
                // shared post-line adopt below is correct for both kinds.
                // $$ carries no char/len descriptor — leave it sentinel.
                if (is_math_fence) {
                    fprobe.in_fence = !in_fence;
                    fprobe.open_ch  = fprobe.in_fence ? '$' : '\0';
                    fprobe.open_len = fprobe.in_fence ? 2 : 0;
                }
                size_t eol = source_.find('\n', i);
                if (eol == std::string::npos) {
                    // The fence line isn't newline-terminated. A CLOSING
                    // fence (in_fence) at end-of-buffer is still provably
                    // complete when the line is fence-marker-only (no
                    // info string is permitted on a closing fence): the
                    // block can't grow further within these bytes, so
                    // commit past it. This is the common Claude ending
                    // "…```" with no trailing newline.
                    //
                    // Load-bearing (verified, do NOT "simplify" away):
                    // without this eager commit the closed block sits in
                    // the tail, where render_tail's canonical path parses
                    // the still-OPEN fence (the closing ``` is the
                    // suppressed live line) and emits a phantom trailing
                    // body row — 6 rows where the committed closed block is
                    // 5. That 1-row shrink at settle is a height-
                    // monotonicity break (scratch/diag_fence: live_h=6 vs
                    // committed_h=5). Committing here makes the live render
                    // identical to the committed one.
                    //
                    // The cost is a benign, invisible metadata nit: the
                    // committed block's source_end excludes the closing
                    // fence's trailing '\n' when a chunk happens to split
                    // exactly at "...```" (streamed source_end 627 vs the
                    // one-shot 628). Proven to change ZERO rendered cells
                    // across every chunking (scratch/repro_issueA), so the
                    // trailing '\n' simply lands in the inter-block gap
                    // instead of the code block's range — no fold / lookup /
                    // render consequence.
                    //
                    // An OPENING fence (!in_fence) with no terminator stays
                    // in the tail: its language / first line are still
                    // arriving.
                    if (in_fence) {
                        // Skip the full fence-marker run and require the
                        // closer to match the opener (same char, run ≥
                        // opener) with only whitespace after it — same rule
                        // as fence_scan_line, applied to the unterminated
                        // final line.
                        char close_ch = source_[i];
                        std::size_t q = i;
                        while (q < source_.size() && source_[q] == close_ch) ++q;
                        std::size_t close_run = q - i;
                        bool marker_only =
                            close_ch == fence_ch && close_run >= fence_len;
                        for (; marker_only && q < source_.size(); ++q) {
                            char cc = source_[q];
                            if (cc != ' ' && cc != '\t' && cc != '\r') {
                                marker_only = false; break;
                            }
                        }
                        if (marker_only) {
                            const bool close_safe = fence_safe;
                            in_fence = false;
                            fence_ch = '\0';
                            fence_len = 0;
                            fence_safe = true;
                            i = source_.size();
                            if (close_safe) record(i);
                            continue;
                        }
                    }
                    break;
                }
                // Real fence open/close: adopt the probe's post-line state
                // (parity + opener descriptor).
                in_fence  = fprobe.in_fence;
                fence_ch  = fprobe.open_ch;
                fence_len = fprobe.open_len;
                i = eol + 1;
                if (!in_fence) {
                    const bool close_safe = fence_safe;
                    fence_safe = true;
                    if (close_safe && !math_close) record(i);
                }
                continue;
            }

            if (!in_fence) {
                // Blank line: we're at the second '\n' of a \n\n pair
                // (or source begins with '\n' at i == 0).  Default
                // behaviour: commit up to and including the second '\n';
                // the next block starts at i + 1.  Exception: intra-list
                // whitespace (loose-list blanks between two same-kind
                // list-item lines at the same indent) must NOT split
                // the list — see classify_blank_line for the rationale.
                if (source_[i] == '\n') {
                    auto verdict = classify_blank_line(
                        std::string_view{source_}, i);
                    if (verdict == IntraBlank::Yes) {
                        // Step past the blank without advancing
                        // last_boundary; the list stays cohesive in
                        // the tail until it genuinely ends.
                        i = i + 1;
                        continue;
                    }
                    if (verdict == IntraBlank::Unknown) {
                        // Next line hasn't fully arrived. Stop the
                        // scan here without consuming the blank so
                        // the next call resumes from this position
                        // when more bytes are present.
                        break;
                    }
                    record(i + 1);
                    i = i + 1;
                    continue;
                }
                // ATX heading at line start.  Commit the prose before
                // the heading; then, once the heading line itself is
                // fully terminated by '\n', advance the boundary past
                // it so the heading is rendered in its committed shape
                // immediately — without waiting for a trailing blank
                // line that may not arrive until the next paragraph
                // lands (and during long streamed answers, may take
                // seconds to do so).
                //
                // The eager-commit mirrors the table / closing-fence
                // behaviour: a structural block that's *provably*
                // complete commits at the start of the following
                // line; a still-arriving line stays in the tail where
                // render_tail's ATX special case renders it in the
                // committed style so the user sees no styling pop.
                //
                // Why this isn't extended to list / blockquote rows:
                // a list/blockquote is a single cohesive block whose
                // shape is only knowable once the block ENDS
                // (intra-list blanks vs. terminating blanks).
                // Committing each row separately would stretch a
                // tight list into singletons separated by the inter-
                // block gap — same trade we make for tables until
                // find_table_end proves them complete.
                if (source_[i] == '#') {
                    // Verify it's an ATX heading shape (1–6 '#' then
                    // a space) before treating it as a boundary —
                    // bare '#' / '#tag' at line start is a plain
                    // paragraph in CommonMark, not a heading.
                    std::size_t hashes = 0;
                    while (hashes < 6 && i + hashes < source_.size()
                           && source_[i + hashes] == '#') ++hashes;
                    bool is_atx = hashes >= 1 && hashes <= 6
                        && i + hashes < source_.size()
                        && (source_[i + hashes] == ' '
                            || source_[i + hashes] == '\t');
                    if (is_atx) {
                        // Setext/tight-block safety gate (mirrors the HR
                        // branch below). Eager-committing at the heading
                        // start splits whatever precedes it into an
                        // ISOLATED commit range. That is only safe when a
                        // blank line already separates the heading from
                        // the block above — a true block boundary. When
                        // there is NO blank line (the loose-GFM shape an
                        // LLM emits: "- item\n## Header", or
                        // "paragraph\n## Header"), the preceding block's
                        // shape is not yet provably final: a list keeps
                        // absorbing lines, a paragraph could still be a
                        // setext target. Committing [committed_, i) here
                        // re-parses that block WITHOUT the following bytes,
                        // and its isolated re-flow can differ cell-for-cell
                        // from how the live tail rendered it — a rewrite of
                        // already-settled scrollback (see
                        // st_committed_cells_stable's list->heading case).
                        // So only eager-commit when the line ABOVE is
                        // blank; otherwise fall through to the normal
                        // blank-line commit path, keeping the whole run
                        // cohesive in the tail until a genuine \n\n lands.
                        bool blank_above = has_blank_above(i);
                        if (blank_above) {
                            record(i);
                            // If the heading line has fully arrived,
                            // anchor the boundary past it so the next
                            // paragraph/heading/etc. starts a fresh
                            // commit. If it hasn't (`eol == npos`), fall
                            // through to the line-step `break` below;
                            // the resumable scan will retry once more
                            // bytes land.
                            std::size_t heading_eol = source_.find('\n', i);
                            if (heading_eol != std::string::npos) {
                                i = heading_eol + 1;
                                record(i);
                                continue;
                            }
                        }
                        // No blank line above: the heading and the block
                        // it hugs commit together at the next real blank-
                        // line boundary. Fall through to the line-step.
                    }
                }
                // Table finality gate — only when a blank line separates
                // the `|` row from whatever precedes it. The parser forms
                // a table ONLY when the header row is a single-line
                // paragraph; a `|a|b|` row hugging prose above is lazy
                // continuation ("para |a|b| |-|-|" is ONE paragraph), so
                // eagerly committing a "proven" table there rewrote the
                // paragraph as a table in settled scrollback.
                if (source_[i] == '|' && has_blank_above(i)) {
                    auto r = find_table_end(
                        std::string_view{source_}, i);
                    if (r.kind == TableScan::EndsAt) {
                        // Commit the prose-before-table at the table's
                        // start (the table itself commits at r.pos
                        // — we set last_boundary there and fast-
                        // forward i past the proven table bytes).
                        record(i);
                        i = r.pos;
                        record(r.pos);
                        continue;
                    }
                    if (r.kind == TableScan::Incomplete) {
                        // Looks like a table but finality not yet
                        // proven. STOP the scan here without
                        // advancing past this line — the scanner is
                        // resumable, and we MUST re-enter this same
                        // `|` check on the next call once more bytes
                        // have landed. Advancing past the header now
                        // would leak the table into the
                        // blank-line-only commit path.
                        break;
                    }
                    // TableScan::NotATable — fall through to the
                    // normal line-step loop. The `|` is just a
                    // literal pipe in prose.
                }

                // Horizontal rule at line start. CommonMark HR: a
                // line containing ONLY repeated `*`, `_`, or `-`
                // (≥3 of the same char), with optional interior
                // spaces, optionally indented ≤3 spaces. Like ATX
                // headings, an HR is a single fully-proven block as
                // soon as the line terminates by '\n' — we can
                // commit the prose before it AND the HR itself in
                // one transition.
                //
                // Setext ambiguity: `---` (and `===`, which we never
                // treat as HR) on a line directly under a non-blank
                // text line is a setext h2 underline, not an HR.
                // The blank-line rule above commits prose at every
                // `\n\n`, so by the time we get HERE inspecting a
                // line at `i`, the previous line is EITHER part of
                // an uncommitted run (could be a setext target) OR
                // already committed (separated by `\n\n`). Only the
                // FORMER case poses setext risk.
                //
                // Conservative gate: only eager-commit `*`/`_` HRs
                // unconditionally (setext doesn't apply — it only
                // uses `=` and `-`). For `-`-style HRs, require the
                // PREVIOUS line to be empty (blank-line separator
                // already in place, so no setext attachment is
                // possible). The fall-through for `-` after text
                // preserves the existing commit-on-blank-line
                // behaviour, which correctly resolves to either HR
                // or setext at commit time via parse_markdown_impl.
                {
                    char c0 = source_[i];
                    if (c0 == '*' || c0 == '_' || c0 == '-') {
                        std::size_t hr_eol = source_.find('\n', i);
                        if (hr_eol != std::string::npos) {
                            // Inspect the line bytes (excluding the
                            // trailing '\n').
                            std::string_view ln{source_.data() + i,
                                                hr_eol - i};
                            // Allow ≤3 leading spaces (GFM).
                            std::size_t k = 0;
                            while (k < 4 && k < ln.size() && ln[k] == ' ') ++k;
                            // Must be the same marker char throughout
                            // the line, with only spaces / '\r'
                            // allowed between markers, and ≥3 marker
                            // chars total.
                            char marker = (k < ln.size()) ? ln[k] : '\0';
                            bool is_hr_shape = (marker == '*'
                                             || marker == '_'
                                             || marker == '-');
                            std::size_t markers = 0;
                            if (is_hr_shape) {
                                for (std::size_t q = k; q < ln.size(); ++q) {
                                    char cc = ln[q];
                                    if (cc == marker) { ++markers; continue; }
                                    if (cc == ' ' || cc == '\t' || cc == '\r')
                                        continue;
                                    is_hr_shape = false;
                                    break;
                                }
                            }
                            if (is_hr_shape && markers >= 3) {
                                // Setext-safety gate for `-`: only
                                // eager-commit when the previous
                                // line is blank (so this can't be
                                // an h2 underline of the line
                                // above). `*` and `_` are always
                                // safe.
                                bool safe = (marker != '-') || has_blank_above(i);
                                if (safe) {
                                    record(i);
                                    i = hr_eol + 1;
                                    record(i);
                                    continue;
                                }
                            }
                        }
                        // No '\n' yet — the line is still arriving,
                        // fall through to the normal line-step.
                        // Next call resumes here.
                    }
                }
            }
        }

        size_t eol = source_.find('\n', i);
        if (eol == std::string::npos) break;
        i = eol + 1;
    }

    // Persist where the scanner stopped so the next call resumes here.
    // i is either == source_.size() (drained), or pointing at a byte
    // we couldn't advance past (find('\n', i) returned npos because
    // the current line hasn't terminated yet) — both correct resume
    // points; new bytes appended to source_ will be visible on the
    // next call without re-walking the prefix.
    scan_cursor_        = i;
    scan_in_fence_      = in_fence;
    scan_fence_open_ch_ = fence_ch;
    scan_fence_open_len_ = fence_len;
    scan_fence_safe_    = fence_safe;
    scan_last_boundary_ = last_boundary;
    return last_boundary;
}

} // namespace maya
