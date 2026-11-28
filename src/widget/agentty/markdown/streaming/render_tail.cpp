// render_tail.cpp — StreamingMarkdown's opaque-tail renderer.
//
// Renders the uncommitted bytes since the last block boundary as a
// monotonic in-progress element: inside-fence literal, open-fence code
// block, ATX heading, eager list/blockquote/table for fully-terminated
// rows, and an inline-only sliding-cache paragraph fallback for the live
// line. The monotonic-height invariant lives here — see the body.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <chrono>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "maya/element/builder.hpp"
#include "maya/element/text.hpp"        // string_width
#include "maya/render/cache_id.hpp"
#include "maya/style/border.hpp"
#include "maya/style/style.hpp"
#include "maya/widget/agentty/markdown.hpp"
#include "maya/widget/agentty/markdown/internal.hpp"
#include "maya/widget/agentty/html.hpp"

#include "maya/widget/agentty/markdown/streaming_internal.hpp"

namespace maya {

using ::maya::md_detail::parse_markdown_impl;
using ::maya::md_detail::parse_inlines;
using ::maya::md_detail::flatten_inline;
using ::maya::md_detail::build_inline_row;
using ::maya::md_detail::highlight_code;
using ::maya::md_detail::ul_marker_len;
using ::maya::md_detail::ol_marker_len;
using ::maya::md_detail::count_indent;
using ::maya::md_detail::streaming::fnv1a64;

namespace {

// Strip block-quote markers ('>' after ≤3 leading spaces, plus one optional
// following space) from the front of a line, repeatedly (nested quotes).
// `depth` (optional) receives the number of markers stripped.
std::string_view dequote_line(std::string_view ln, int* depth = nullptr) noexcept {
    int d = 0;
    for (;;) {
        std::size_t k = 0;
        while (k < 4 && k < ln.size() && ln[k] == ' ') ++k;
        if (k < ln.size() && ln[k] == '>') {
            ln.remove_prefix(k + 1);
            if (!ln.empty() && ln.front() == ' ') ln.remove_prefix(1);
            ++d;
            continue;
        }
        break;
    }
    if (depth) *depth = d;
    return ln;
}

// True while a LIVE (unterminated) line is a plausible prefix of an HTML
// block opener whose committed render shows NO raw tag bytes. A committed
// HtmlBlock renders through html::render, where the tag markup itself is
// consumed (a lone `<div>` / `</div>` / comment line produces ZERO rows) —
// so painting the raw `<div>` bytes as a literal row while live guarantees
// a 1-row shrink the instant the '\n' lands and the line reclassifies.
// Render nothing instead: hidden live == hidden committed, height
// continuous. The test must stay true for every prefix of the hidden final
// shape (else the row would pop in then back out mid-typing): bare `<`,
// `<!`, `<?`, `</`, a growing tag name, an open tag still collecting
// attributes, and a completed tag followed only by whitespace are all
// "still plausibly a bare tag line". The first byte that disproves it —
// the ':' of `<https://…` autolinks, or CONTENT after the closing '>'
// (that content renders in the committed html block, so it must show) —
// flips to visible: a 0→N GROWTH, which monotonicity permits.
bool html_block_live_prefix(std::string_view line) noexcept {
    if (md_detail::count_indent(line) >= 4) return false;  // indented code
    std::size_t k = 0;
    while (k < 4 && k < line.size() && line[k] == ' ') ++k;
    if (k >= line.size() || line[k] != '<') return false;
    std::size_t p = k + 1;
    if (p >= line.size()) return true;               // bare '<' — ambiguous
    char c = line[p];
    if (c == '!' || c == '?') return true;           // comment / PI / decl
    if (c == '/') {
        ++p;
        if (p >= line.size()) return true;
        c = line[p];
    }
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) return false;
    std::size_t name_lo = p;
    while (p < line.size()) {
        char t = line[p];
        if ((t >= 'a' && t <= 'z') || (t >= 'A' && t <= 'Z')
            || (t >= '0' && t <= '9') || t == '-') { ++p; continue; }
        break;
    }
    // Only KNOWN HTML tag names start a raw HTML block (see html_block_start
    // in cm_block.cpp — kind 7 is gated on html::is_known_tag). Unknown model
    // pseudo-tags (<shell>, <thinking>, …) commit as ordinary VISIBLE text, so
    // hiding them live would render 0 rows live but 1 row committed — a height
    // mismatch that snaps / overlaps the canvas at commit. Keep the live tail
    // in lockstep with the parser: never hide an unknown tag line. (A name
    // still mid-stream, `p == line.size()`, stays hidden below — we don't yet
    // know what it is, and it carries no visible content yet either way.)
    if (p < line.size()) {
        std::string_view name = line.substr(name_lo, p - name_lo);
        if (!html::is_known_tag(name)) return false;
    }
    if (p >= line.size()) return true;               // tag name still growing
    char t = line[p];
    if (!(t == ' ' || t == '\t' || t == '>' || t == '/' || t == '\r'))
        return false;                                // `<https:` etc — prose
    // Tag shape confirmed. Hidden only while it's a BARE tag line: the tag
    // is still open (no '>' yet — attributes streaming) or the '>' is
    // followed by whitespace only. Content after '>' renders in the
    // committed html block, so it must stay visible.
    std::size_t gt = line.find('>', p);
    if (gt == std::string_view::npos) return true;   // tag still open
    for (std::size_t q = gt + 1; q < line.size(); ++q) {
        char w = line[q];
        if (w != ' ' && w != '\t' && w != '\r') return false;
    }
    return true;
}

// True while a LIVE line is a plausible prefix of a link-reference
// definition (`[label]: url`). The committed parse EXTRACTS ref-def lines
// into the ref map and renders ZERO rows for them (collect_ref_defs /
// cm_block leading-refs extraction), so a literal live render vanishes at
// the terminating '\n' — the 2-row link_ref composer bounce. Footnote defs
// (`[^label]: …`) stay visible blocks and are excluded. Prefix-stable: `[`,
// `[lab`, `[label]`, `[label]:` all hold; the first byte proving it is NOT
// a ref def (anything but ':' after the ']') flips to visible — growth.
bool link_ref_live_prefix(std::string_view line) noexcept {
    if (md_detail::count_indent(line) >= 4) return false;  // indented code
    std::size_t k = 0;
    while (k < 4 && k < line.size() && line[k] == ' ') ++k;
    if (k >= line.size() || line[k] != '[') return false;
    ++k;
    if (k < line.size() && line[k] == '^') return false;   // footnote def
    std::size_t close = line.find(']', k);
    if (close == std::string_view::npos) return true;      // label growing
    std::size_t after = close + 1;
    if (after >= line.size()) return true;                  // ']' just landed
    return line[after] == ':';
}

// True while a LIVE line is nothing but a HALF-TYPED list-marker prefix
// ("-", "- ", "  -", "12", "12.", "12. " — a prefix of "marker + space"
// with no content yet). When the last terminated block is a list, folding
// such a line into it can TRANSIENTLY reparse as something taller —
// "- outer\n  -" is a setext-h2 INSIDE the item (3 rows incl. the rule row)
// until the first content byte turns it into a nested item (2 rows): a
// mid-typing shrink. Suppressing the marker-only phase renders the list
// without the live line; the first content byte makes it appear — growth
// only. Note "12 " (digits + space, no '.'/')') is NOT a marker prefix —
// no continuation can turn it into a marker — so it stays visible.
bool half_list_marker(std::string_view t) noexcept {
    std::size_t k = 0;
    while (k < t.size() && (t[k] == ' ' || t[k] == '\t')) ++k;
    if (k >= t.size()) return false;                 // blank — not ours
    std::string_view m = t.substr(k);
    if (m[0] == '-' || m[0] == '*' || m[0] == '+') {
        if (m.size() == 1) return true;              // "-"
        return m.size() == 2 && m[1] == ' ';         // "- "
    }
    std::size_t d = 0;
    while (d < m.size() && m[d] >= '0' && m[d] <= '9') ++d;
    if (d == 0 || d > 9) return false;
    if (d == m.size()) return true;                  // "12"
    std::size_t p = d;
    if (m[p] != '.' && m[p] != ')') return false;
    ++p;
    if (p == m.size()) return true;                  // "12."
    return p + 1 == m.size() && m[p] == ' ';         // "12. "
}

// ── Paragraph-interruption classifiers for the block-boundary walk ──
//
// CommonMark §5.1/§5.2/§4.5: some block openers interrupt a paragraph
// with NO blank line between — `Summary:\n- item` is two blocks. The
// walk in render_tail uses these to split at such seams so the paragraph
// freezes into the stable prefix instead of being silently dropped by
// the last-block-only render (blocks.back()).
//
// Deliberately conservative — anything not recognised keeps the old
// cohesive verdict (no split), which can never DROP content, only skip
// an optimisation:
//   • thematic breaks are EXCLUDED: `---` under a paragraph is a setext
//     underline (it transforms the paragraph, doesn't interrupt it);
//   • ordered markers interrupt only with start number 1 (spec rule);
//   • markers must carry CONTENT after the space (empty items don't
//     interrupt);
//   • html openers / tables are left cohesive (tables can't interrupt
//     a paragraph in GFM; html has its own live-line hide).

// True when `ln`, at a block START, reads as plain paragraph text — no
// block-level opener. Fence lines never reach the caller's else-branch
// (fence_scan_line intercepts), so they're not tested here.
bool para_line_shape(std::string_view ln) noexcept {
    if (md_detail::count_indent(ln) >= 4) return false;   // indented code
    std::size_t k = 0;
    while (k < 4 && k < ln.size() && ln[k] == ' ') ++k;
    if (k >= ln.size()) return false;                      // blank
    std::string_view t = ln.substr(k);
    if (t[0] == '>') return false;                         // quote
    if (t[0] == '|') return false;                         // table-ish
    if (t[0] == '#') {                                     // ATX heading?
        std::size_t h = 0;
        while (h < t.size() && h < 7 && t[h] == '#') ++h;
        if (h >= 1 && h <= 6
            && (h == t.size() || t[h] == ' ' || t[h] == '\t'))
            return false;
    }
    if (ul_marker_len(t) > 0 || ol_marker_len(t) > 0) return false;
    return true;
}

// Would `ln` interrupt an open paragraph? (§5.1 conservative subset.)
bool interrupts_paragraph(std::string_view ln) noexcept {
    if (md_detail::count_indent(ln) >= 4) return false;    // code can't
    std::size_t k = 0;
    while (k < 4 && k < ln.size() && ln[k] == ' ') ++k;
    if (k >= ln.size()) return false;
    std::string_view t = ln.substr(k);
    if (t[0] == '>') return true;                          // blockquote
    if (t[0] == '#') {                                     // ATX heading
        std::size_t h = 0;
        while (h < t.size() && h < 7 && t[h] == '#') ++h;
        if (h >= 1 && h <= 6
            && (h == t.size() || t[h] == ' ' || t[h] == '\t'))
            return true;
    }
    // Unordered list item WITH content (`- foo`, not bare `-`).
    if (int ml = ul_marker_len(t); ml > 0) {
        std::string_view after = t.substr(static_cast<std::size_t>(ml));
        for (char c : after)
            if (c != ' ' && c != '\t' && c != '\r') return true;
        return false;
    }
    // Ordered item: only `1.` / `1)` with content interrupts (spec).
    if (int ml = ol_marker_len(t); ml > 0) {
        std::size_t d = 0;
        while (d < t.size() && t[d] >= '0' && t[d] <= '9') ++d;
        if (!(d == 1 && t[0] == '1')) return false;
        std::string_view after = t.substr(static_cast<std::size_t>(ml));
        for (char c : after)
            if (c != ' ' && c != '\t' && c != '\r') return true;
        return false;
    }
    return false;
}

} // namespace

void StreamingMarkdown::render_eager_slice(std::string_view slice,
                                           std::vector<Element>& kids) const {
    // Memoize on (source_version_, slice length, slice hash). The slice
    // is the proven-complete prefix of the eager block; it only grows
    // when a new row terminates, so between row-commits this is a hit
    // and we re-emit the cached block Elements by shared_ptr copy —
    // skipping parse_markdown_impl + md_block_to_element entirely.
    //
    // Each cached block is also wrapped in a hash-keyed ComponentElement
    // (key = slice hash + block index) so the RENDERER's cell cache
    // blits it (memcpy/row) instead of re-laying-out the whole table or
    // list every frame. Without this the parse is cached but render_tree
    // still re-runs layout::compute over the full block each frame — the
    // dominant cost for a big streaming table. The key changes only when
    // a new row lands (slice hash moves), so a growing block re-lays-out
    // once per committed row, not once per frame.
    std::uint64_t slice_hash = eager_cache_slice_hash_;
    bool hit = false;
    if (eager_cache_version_ == source_version_
        && eager_cache_slice_len_ == slice.size()
        && !eager_cache_blocks_.empty()) {
        hit = true;
    } else {
        const std::uint64_t h = fnv1a64(slice);
        slice_hash = h;
        if (eager_cache_slice_len_ == slice.size()
            && eager_cache_slice_hash_ == h
            && !eager_cache_blocks_.empty()) {
            hit = true;
        } else {
            ::maya::md_detail::RefDefsScope guard(
                const_cast<std::unordered_map<std::string, md::LinkRef>*>(&ref_defs_));
            auto parsed = parse_markdown_impl(std::string{slice}, 0);
            eager_cache_blocks_.clear();
            eager_cache_blocks_.reserve(parsed.blocks.size());
            for (auto& block : parsed.blocks) {
                // Reserve final column widths for a live table so it does
                // not reflow as later rows reveal (no-op for non-tables).
                apply_live_table_floor_(block);
                eager_cache_blocks_.push_back(
                    std::make_shared<const Element>(md_block_to_element(block)));
            }
            eager_cache_slice_hash_ = h;
            eager_cache_slice_len_  = slice.size();
            eager_cache_version_    = source_version_;
        }
    }
    (void)hit;
    for (std::size_t i = 0; i < eager_cache_blocks_.size(); ++i) {
        auto blk = eager_cache_blocks_[i];
        ComponentElement comp;
        comp.hash_id = ::maya::CacheIdBuilder{}
            .add(std::string_view{"strmd-eager"})
            .add(static_cast<std::uint64_t>(instance_id_))
            .add(slice_hash)
            .add(static_cast<std::uint64_t>(i))
            .build();
        comp.render = [blk](int, int) -> Element { return *blk; };
        kids.push_back(Element{std::move(comp)});
    }
}

// ── Live-table column-width floor ───────────────────────────────────────────
//
// See markdown.hpp (live_table_floor_). When the uncommitted tail begins with
// a GFM table, scan source_ over EVERY already-arrived, fully-terminated row
// of that table and record each column's final width. The eager / canonical
// table renders fold it in (md::Table::min_col_widths) so the visible table
// sits at its FINAL widths from the first revealed row instead of reflowing
// horizontally as wider rows/cells stream in. Cached on (source_version_,
// committed_): recomputed only when bytes arrive or the commit base moves.
void StreamingMarkdown::refresh_live_table_floor_() const {
    if (live_table_floor_version_ == source_version_
        && live_table_floor_base_ == committed_)
        return;
    live_table_floor_version_ = source_version_;

    // The floor only matters when the reveal cursor can hide arrived rows
    // (reveal_fx live). Otherwise the displayed tail already holds every
    // arrived row, so its natural ideal == the floor — computing it would be
    // a pure-overhead parse. Skip.
    if (!(reveal_fx_ && live_)) {
        live_table_floor_.clear();
        live_table_floor_base_     = committed_;
        live_table_floor_rows_end_ = static_cast<std::size_t>(-1);
        return;
    }

    // Escape hatch for A/B measurement / emergencies: MAYA_NO_TABLE_FLOOR
    // disables the reservation (table reverts to per-revealed-row widths).
    // The old AGENTTY_ spelling is still honoured.
    static const bool disabled = [] {
        const char* e = std::getenv("MAYA_NO_TABLE_FLOOR");
        if (!e) e = std::getenv("AGENTTY_NO_TABLE_FLOOR");
        return e && e[0] && e[0] != '0';
    }();
    if (disabled) { live_table_floor_.clear(); return; }

    // Start of the live tail's first non-blank line.
    std::size_t base = committed_;
    while (base < source_.size()
           && (source_[base] == '\n' || source_[base] == '\r'))
        ++base;
    if (base >= source_.size()) {
        live_table_floor_.clear();
        live_table_floor_base_     = committed_;
        live_table_floor_rows_end_ = static_cast<std::size_t>(-1);
        return;
    }

    // line [ls, le): does it start with '|' after ≤3 leading spaces (GFM)?
    auto first_pipe = [&](std::size_t ls, std::size_t le) -> bool {
        std::size_t k = ls, lim = std::min(ls + 4, le);
        while (k < lim && source_[k] == ' ') ++k;
        return k < le && source_[k] == '|';
    };

    // Any early-out that means "the live tail is not a (valid) table" clears
    // the floor and forgets the incremental cursor.
    auto not_a_table = [&]() {
        live_table_floor_.clear();
        live_table_floor_base_     = committed_;
        live_table_floor_rows_end_ = static_cast<std::size_t>(-1);
    };

    // Header line must be terminated and pipe-led.
    std::size_t he = source_.find('\n', base);
    if (he == std::string::npos || !first_pipe(base, he)) { not_a_table(); return; }
    // Delimiter line must be terminated, pipe-led, and well-formed.
    std::size_t ds = he + 1;
    std::size_t de = source_.find('\n', ds);
    if (de == std::string::npos || !first_pipe(ds, de)) { not_a_table(); return; }
    {
        bool has_dash = false, ok = true;
        std::size_t k = ds, lim = std::min(ds + 4, de);
        while (k < lim && source_[k] == ' ') ++k;
        for (; k < de; ++k) {
            char c = source_[k];
            if (c == '-') { has_dash = true; continue; }
            if (c == '|' || c == ':' || c == ' ' || c == '\t' || c == '\r')
                continue;
            ok = false; break;
        }
        if (!ok || !has_dash) { not_a_table(); return; }
    }

    // Walk arrived data rows: each fully-terminated '|'-led line after the
    // delimiter is in the table. Stop at the first non-pipe line OR the
    // first UNTERMINATED (live) line — the half-typed live row must NOT
    // drive the floor (its growing cell would re-introduce the very jitter
    // we remove; it reflows by at most itself when it finally terminates).
    std::size_t end = de + 1;
    for (std::size_t p = de + 1; p < source_.size();) {
        std::size_t e = source_.find('\n', p);
        if (e == std::string::npos || !first_pipe(p, e)) break;
        p = e + 1;
        end = p;
    }

    // ── Row-gated recompute ─────────────────────────────────────
    // The floor is a monotone function of the terminated rows: it changes
    // ONLY when a new row terminates (advances `end`). source_version_ bumps
    // on every byte, so keying on it recomputed the whole floor per token =
    // O(rows)/token = O(rows^2)/stream. Gate on (base, end) instead: same
    // table base with no new terminated row since we last folded → the
    // cached floor is exact, return without parsing.
    const bool same_table = (live_table_floor_base_ == committed_
                             && !live_table_floor_.empty()
                             && live_table_floor_rows_end_
                                    != static_cast<std::size_t>(-1));
    if (same_table && end <= live_table_floor_rows_end_) {
        return;  // no new terminated row — floor unchanged, O(1)
    }
    live_table_floor_base_ = committed_;

    // ── TRUE incremental floor — measure ONLY the newly-terminated rows ──
    //
    // The floor is a per-column max over every terminated row's flattened
    // cell width, computed EXACTLY as the renderer computes ideal (same
    // split_table_cells + parse_inlines + flatten_inline + string_width),
    // so the floor equals the committed table's natural width: continuous
    // at the seam. It is monotone (widths only grow), so re-folding an
    // already-seen row is harmless — which means we never NEED to re-measure
    // a row we've already folded.
    //
    // Previously this re-parsed the WHOLE [base, end) slice with
    // parse_markdown_impl on every row termination — O(all rows) per
    // termination = O(rows^2) over the table's life. The parser was only
    // needed to RECOGNISE the table (header + delimiter), not to measure a
    // data row: a GFM row's cell widths come straight from
    // split_table_cells + parse_inlines on that one line. So we measure
    // each row exactly once, scanning only the delta window
    // [row_scan_start, end) — turning the whole stream into O(total rows)
    // = amortised O(1) per termination.
    //
    // The header/delimiter recognition already happened above (first_pipe +
    // the dash-scan on the delimiter line), so here we know [base, end) is a
    // valid table body. Column count is fixed by the header row.
    ::maya::md_detail::RefDefsScope guard(
        const_cast<std::unordered_map<std::string, md::LinkRef>*>(&ref_defs_));

    // Local GFM pipe-cell splitter. Splits `line` on UNESCAPED `|`,
    // drops the leading/trailing pipe delimiters, and trims each cell
    // — matching the engine's split_pipe_cells (cm_block.cpp) so the
    // measured widths equal what the committed table renders. Kept local
    // (the engine's splitter is file-static + not exported); the logic is
    // small and stable (GFM cell delimiting is a fixed grammar).
    auto split_cells = [](std::string_view t) {
        std::vector<std::string_view> cells;
        // Strip one leading and one trailing pipe (GFM edge delimiters).
        std::size_t lo = 0, hi = t.size();
        while (lo < hi && (t[lo] == ' ' || t[lo] == '\t')) ++lo;
        while (hi > lo && (t[hi-1] == ' ' || t[hi-1] == '\t'
                           || t[hi-1] == '\r' || t[hi-1] == '\n')) --hi;
        t = t.substr(lo, hi - lo);
        if (!t.empty() && t.front() == '|') t.remove_prefix(1);
        if (!t.empty() && t.back()  == '|') t.remove_suffix(1);
        auto trim = [](std::string_view s) {
            std::size_t a = 0, b = s.size();
            while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
            while (b > a && (s[b-1] == ' ' || s[b-1] == '\t')) --b;
            return s.substr(a, b - a);
        };
        std::size_t start = 0;
        for (std::size_t i = 0; i < t.size(); ++i) {
            if (t[i] == '\\') { ++i; continue; }  // escaped char
            if (t[i] == '|') { cells.push_back(trim(t.substr(start, i - start)));
                               start = i + 1; }
        }
        cells.push_back(trim(t.substr(start)));
        return cells;
    };

    auto row_cell_widths = [&](std::string_view line, std::vector<int>& w) {
        auto cells = split_cells(line);
        if (w.empty()) w.assign(cells.size(), 0);
        for (std::size_t c = 0; c < cells.size() && c < w.size(); ++c) {
            std::string content;
            std::vector<StyledRun> runs;
            const Style base_st{};
            for (const auto& sp : ::maya::md_detail::parse_inlines(cells[c]))
                flatten_inline(sp, base_st, content, runs);
            w[c] = std::max(w[c], std::max(1, string_width(content)));
        }
    };

    // Seed from the existing floor when extending the same table (monotone
    // widening) and resume the row scan just past the last-folded row.
    // Start fresh for a new table base (row_scan_start = header line).
    std::vector<int> w;
    std::size_t row_scan_start;
    if (same_table && !live_table_floor_.empty()
        && live_table_floor_rows_end_ != static_cast<std::size_t>(-1)
        && live_table_floor_rows_end_ >= base
        && live_table_floor_rows_end_ <= end) {
        w = live_table_floor_;
        row_scan_start = live_table_floor_rows_end_;  // only the NEW rows
    } else {
        // Fresh table: fold the header, SKIP the delimiter line (`|:--|`
        // carries no content width), then fold every data row below.
        std::size_t hl_end = source_.find('\n', base);
        if (hl_end == std::string::npos) return;
        row_cell_widths(
            std::string_view{source_}.substr(base, hl_end - base), w);
        std::size_t dl_end = source_.find('\n', hl_end + 1);
        if (dl_end == std::string::npos) return;
        row_scan_start = dl_end + 1;  // first data row
    }

    // Fold each fully-terminated data row in [row_scan_start, end).
    for (std::size_t p = row_scan_start; p < end;) {
        std::size_t e = source_.find('\n', p);
        if (e == std::string::npos || e >= end) break;
        row_cell_widths(
            std::string_view{source_}.substr(p, e - p), w);
        p = e + 1;
    }

    if (w.empty()) return;
    live_table_floor_ = std::move(w);
    live_table_floor_rows_end_ = end;
}

void StreamingMarkdown::apply_live_table_floor_(md::Block& block) const {
    if (live_table_floor_.empty()) return;
    if (auto* t = std::get_if<md::Table>(&block.inner))
        if (t->min_col_widths.empty())
            t->min_col_widths = live_table_floor_;
}

Element StreamingMarkdown::render_tail(std::string_view tail) const {
    // Keep the live-table column-width floor current for THIS frame before
    // any table render below reads it (cheap: cached on version+committed).
    refresh_live_table_floor_();

    // ── Canonical-render memo (the anti-"stuck" guard) ─────────────────
    // The body below full-parses the tail every call; build() calls this
    // every animation frame. Memoize on (source_version_, len, hash,
    // in_code_fence_) so a tail that hasn't changed since the last frame
    // is returned by shared_ptr copy with zero parsing. See header.
    //
    // CRITICAL — skip the hash when source_version_ matches. fnv1a64(tail)
    // is O(tail) and used to run EVERY frame just to TEST the memo, so a
    // long live tail (e.g. reveal_fx defers commits, leaving the whole
    // turn in the tail) paid O(N) per animation frame even on a frame
    // where nothing changed — the long-turn streaming stutter. Every
    // mutator of source_/committed_ bumps source_version_, so an unchanged
    // version PROVES the tail bytes are identical: short-circuit on
    // (version, len, in_fence) alone and never touch the bytes. The inner
    // term/eager caches below already use this same version gate; the
    // outer memo was the one O(N) hold-out. The hash is still computed (and
    // cached) on the cold path so a version-bumped-but-identical-bytes case
    // (rare: a mutation that round-trips the tail) still hits by hash.
    const bool version_match =
        tail_canon_cache_el_
        && tail_canon_cache_version_  == source_version_
        && tail_canon_cache_len_      == tail.size()
        && tail_canon_cache_in_fence_ == in_code_fence_;
    if (version_match) return *tail_canon_cache_el_;

    // Slow-path bytes-equality fallback: only reachable when the version
    // bumped but the tail bytes might be identical (a mutation that
    // round-trips the tail). A DIFFERENT length can never be a hit, so
    // skip the O(tail) hash entirely unless the lengths already match —
    // this keeps the common streaming frame (tail grew, version bumped)
    // off the whole-tail hash, which is otherwise the dominant per-frame
    // cost on a long uncommitted tail (reveal cursor lagging tens of KB).
    std::uint64_t tail_hash = 0;
    if (tail_canon_cache_el_
        && tail_canon_cache_len_      == tail.size()
        && tail_canon_cache_in_fence_ == in_code_fence_) {
        tail_hash = fnv1a64(tail);
        if (tail_canon_cache_hash_ == tail_hash) {
            // Bytes are identical despite a version bump — refresh the
            // version stamp so subsequent frames take the cheap
            // version_match path.
            tail_canon_cache_version_ = source_version_;
            return *tail_canon_cache_el_;
        }
    }

    Element result = [&]() -> Element {
    // The 100% monotonicity funnel.
    //
    // render_tail_inner picks an eager/inline shape for the in-progress
    // bytes. That shape can DISAGREE with the canonical block layout the
    // bytes will commit to, and the disagreement shows up as a height
    // snap — either mid-stream (a reclassification: quote→alert,
    // paragraph→setext, blank-terminated quote collapse) or at the
    // COMMIT SEAM (the eager tail is taller than the canonical block,
    // so when commit_range moves the bytes into the prefix the total
    // height drops by the difference). A padding floor (min_height)
    // can't fix it: the host sizes the inline frame by content_height,
    // which ignores trailing blank rows.
    //
    // The fix that holds across BOTH the mid-stream snap and the commit
    // seam: render the fully-terminated portion of the tail in its
    // CANONICAL committed shape — exactly what commit_range will emit
    // (parse_markdown_impl + md_block_to_element). The live partial last
    // line (no trailing `\n`) can still misparse as a block, so it stays
    // on the eager/inline path; canonical handles only the proven-
    // complete prefix. Because the canonical render IS the committed
    // shape, height at the seam is continuous, and because canonical is
    // a stable function of terminated bytes it never shrinks as more
    // bytes arrive.
    //
    // NOTE: we do NOT early-return for in_code_fence_. When the tail
    // starts inside an open fence, render_tail_inner renders the literal
    // body — but if the CLOSING fence has arrived in the tail (it sits
    // there until the trailing blank line commits the whole block), the
    // canonical parse below balances the fence into a proper CodeBlock
    // whose height matches what commit_range will produce, eliminating
    // the 2-row collapse at the closing-fence commit seam. If the fence
    // is still open (no closing ```), the canonical parse of the
    // terminated rows yields the same in-progress CodeBlock shape.
    std::size_t s = 0;
    while (s < tail.size() && tail[s] == '\n') ++s;
    if (s >= tail.size()) return render_tail_inner(tail);
    std::string_view body = tail.substr(s);

    // Absolute source offset of `body`'s start. render_tail is handed
    // tail = source_[committed_, visible_end]; `s` skips the leading
    // newlines. Absolute offsets are used to key the terminated caches
    // below so they survive the commit-behind-cursor shifts that move
    // committed_ forward every frame (source_version_ bumps every frame
    // and would defeat the memo).
    const std::size_t body_abs_start = committed_ + s;

    // Split the tail at the last newline: [terminated_prefix][live_line].
    // Only the terminated prefix is safe to render canonically — the
    // live line may still be a half-typed marker that parse_markdown_impl
    // would misclassify (and then re-classify next byte, snapping).
    auto last_nl = body.rfind('\n');
    if (last_nl == std::string_view::npos) {
        // No terminated rows yet — nothing canonical to anchor. The tail
        // IS one live line, starting at a block boundary (commit_range
        // only advances committed_ across proven boundaries, so a blank
        // line above is implied). If that line will render as ZERO rows
        // once committed — an HTML block-opener tag (`<div>`: html::render
        // consumes bare tag markup) or a link-reference definition
        // (`[d]: url`: extracted into the ref map, never rendered) —
        // painting its raw bytes now guarantees a 1-row shrink the instant
        // the '\n' lands. Render nothing instead: hidden live == hidden
        // committed. Both tests are prefix-stable, so a disproving byte
        // only ever REVEALS the line (growth).
        if (!in_code_fence_
            && (html_block_live_prefix(body) || link_ref_live_prefix(body)))
            return Element{TextElement{}};
        // Otherwise the eager inline render is the floor.
        return render_tail_inner(tail);
    }
    std::string_view terminated = body.substr(0, last_nl + 1);
    std::string_view live_line   = body.substr(last_nl + 1);

    // When the committed prefix ended INSIDE an open code fence, the
    // opening ``` lives in the already-committed bytes, so a fresh parse
    // of the tail wouldn't know it's code. Re-open the fence with a
    // synthetic opener so parse_markdown_impl reconstructs the CodeBlock
    // (and balances it if the closing ``` is present in the tail). The
    // opener is stripped from the rendered output by md_block_to_element
    // exactly as a real fence opener would be.
    std::string fence_prefix;
    if (in_code_fence_) fence_prefix = "```\n";

    // Canonical parse of a slice — byte-identical to what commit_range
    // will eventually produce for these bytes.
    auto canonical_blocks = [this](std::string_view src) {
        ::maya::md_detail::RefDefsScope guard(
            const_cast<std::unordered_map<std::string, md::LinkRef>*>(&ref_defs_));
        return parse_markdown_impl(std::string{src}, 0).blocks;
    };

    // Memoized RENDER of the terminated prefix to shared block Elements,
    // each wrapped in a hash-keyed ComponentElement so the RENDERER's
    // cell cache blits it (memcpy/row) instead of re-laying-out +
    // re-highlighting the whole block every frame. Two layers of caching
    // are needed: this memo skips the PARSE + md_block_to_element when the
    // terminated prefix is unchanged, and the ComponentElement key (stable
    // per terminated-hash + block index) makes render_tree blit the cells
    // rather than recompute layout::compute over the block. Without the
    // wrapper the parse is cached but the per-frame render_tree still
    // re-highlights a growing code fence — O(tail) per frame, the actual
    // "animation stops past N bytes" stall (the parse was never the
    // dominant cost; the layout/highlight re-run was). The key moves only
    // when a new row terminates, so a growing block re-lays-out once per
    // committed row, not once per frame. Mirrors render_eager_slice.
    //
    // Forward-scan the byte offset where the LAST top-level block in
    // `terminated` begins. Everything in [0, last_block_start) is frozen
    // (closed by a blank-line / fence / paragraph-interruption boundary a
    // later byte can't reach back across); only the final block
    // [last_block_start, end) can still grow as more rows terminate.
    // Branch/newline scan only — no markdown parse — cheap relative to the
    // parse it gates, and it keeps both the incremental stable-prefix memo
    // (below) and the live-line merge test (further down) O(last block)
    // instead of O(terminated).
    //
    // PARAGRAPH INTERRUPTION (the vanishing-paragraph bug): blocks are NOT
    // only separated by blank lines. A list item with content, a `1.`
    // ordered item, a blockquote `>`, an ATX heading, or a fence opener
    // INTERRUPTS a paragraph directly (CommonMark §5.1/§5.2/§4.5) —
    // `Summary:\n- item` is TWO blocks with no blank between. The old
    // blank-only walk left last_block_start at the paragraph, so the
    // "last block" slice held para+list and last_block_rendered — which
    // renders blocks.back() ONLY — silently DROPPED the paragraph from
    // the frame the moment the first list row terminated; it popped back
    // when the list committed. Track whether the current block is
    // paragraph-shaped and split at interrupter lines so the paragraph
    // lands in the frozen prefix instead.
    std::size_t last_block_start = 0;
    {
        md_detail::streaming::FenceState fst{
            in_code_fence_, fence_open_ch_, fence_open_len_};
        std::size_t p = 0;
        std::size_t block_start = 0;
        bool prev_blank = false;
        bool cur_para   = false;  // block at block_start is paragraph-shaped
        bool have_line  = false;  // any content line seen yet
        while (p < terminated.size()) {
            std::size_t e = terminated.find('\n', p);
            if (e == std::string_view::npos) e = terminated.size();
            // Spec-faithful ``` / ~~~ parity via the shared classifier so
            // this block-boundary scan agrees with committed_/in_code_fence_.
            const bool was_in = fst.in_fence;
            const bool fence_line = md_detail::streaming::fence_scan_line(
                fst, terminated, p, e);
            std::string_view ln = terminated.substr(p, e - p);
            const bool blank =
                ln.find_first_not_of(" \t\r") == std::string_view::npos;
            if (fence_line) {
                if (!was_in) {
                    // OPENER. Starts a new block after a blank line (the
                    // old walk missed this — the fence rode the previous
                    // block's slice) and also interrupts a paragraph
                    // directly (§4.5).
                    if (prev_blank || (have_line && cur_para))
                        block_start = p;
                    cur_para = false;
                }
                // CLOSER (was_in): the fence block continues through its
                // closing line; no boundary decision here. (A block
                // starting right after the closer without a blank line
                // keeps the old cohesive behaviour — splitting there
                // would also split fences nested in list items.)
                have_line  = true;
                prev_blank = false;
            }
            else if (!was_in && blank) {
                // Intra-list blank (loose-list separator) is NOT a block
                // boundary — same verdict find_block_boundary takes via
                // classify_blank_line. Splitting the loose list here made
                // the tail render it as per-item blocks joined by the
                // outer gap(1) vstack (a blank row between items) while
                // the committed whole-list parse renders the items with
                // NO gap rows — a 1-row shrink per loose blank at the
                // commit seam (the loose_list composer bounce). Holding
                // the slice cohesive makes the live render byte-identical
                // to the committed one.
                if (ln.empty() && p > 0
                    && md_detail::streaming::classify_blank_line(
                           terminated, p)
                           == md_detail::streaming::IntraBlank::Yes) {
                    // step over without arming the boundary
                } else {
                    prev_blank = true;
                }
            }
            else if (was_in) {
                // Fence content — never a boundary.
                prev_blank = false;
            }
            else {
                if (prev_blank) {
                    block_start = p;
                    cur_para = para_line_shape(ln);
                } else if (!have_line) {
                    cur_para = para_line_shape(ln);
                } else if (cur_para && interrupts_paragraph(ln)) {
                    block_start = p;
                    cur_para = para_line_shape(ln);
                }
                // Lazy continuation lines keep cur_para as-is.
                have_line  = true;
                prev_blank = false;
            }
            p = e + 1;
        }
        last_block_start = block_start;
    }

    // Absolute source offsets pinning the terminated slice + its last
    // (still-growing) block. Stable across commit shifts — the memo keys.
    const std::size_t term_abs_end   = body_abs_start + terminated.size();
    const std::size_t last_blk_abs   = body_abs_start + last_block_start;

    // term_slice_hash keys the still-growing last block's ComponentElement
    // wrapper (it must rotate when a new row lands = when term_abs_end
    // moves). The frozen-prefix wrappers key on last_blk_abs instead.
    const std::uint64_t term_slice_hash =
        (term_abs_end * 0x9E3779B97F4A7C15ull) ^ terminated.size();
    // Rendered blocks for the FROZEN prefix [0, last_block_start) only —
    // NOT the still-growing last block. The last block is rendered exactly
    // once below (either merged with the live line, or standalone),
    // eliminating the double last-block render (terminated_rendered used
    // to render it AND the merge test re-rendered it every frame). Keyed
    // on absolute offsets so it survives commit shifts.
    auto stable_rendered = [&]() -> const std::vector<std::shared_ptr<const Element>>& {
        const bool stable_hit =
            tail_stable_cache_in_fence_ == in_code_fence_
            && tail_stable_cache_abs_end_  == last_blk_abs
            && tail_stable_cache_len_      == last_block_start
            // Self-heal: a cache entry whose KEY matches but whose block
            // vector is empty while the frozen prefix is non-empty would
            // silently drop the prefix (e.g. "Notes..." above a list)
            // for the frame the key first lands — the reported "line was
            // missing while the list rendered, then came back". Treat an
            // empty-but-should-be-populated entry as a MISS so it
            // recomputes this frame instead of returning nothing.
            && !(last_block_start > 0 && tail_stable_cache_blocks_.empty());
        if (stable_hit) return tail_stable_cache_blocks_;
        tail_stable_cache_blocks_.clear();
        if (last_block_start > 0) {
            // Parse the frozen prefix [0, last_block_start) once. It ends
            // on a blank-line separator (block boundary), so it parses to
            // a clean set of complete blocks with no dangling open block.
            std::string_view frozen = terminated.substr(0, last_block_start);
            auto blocks = canonical_blocks(fence_prefix + std::string{frozen});
            tail_stable_cache_blocks_.reserve(blocks.size());
            for (auto& blk : blocks) {
                apply_live_table_floor_(blk);
                tail_stable_cache_blocks_.push_back(
                    std::make_shared<const Element>(md_block_to_element(blk)));
            }
        }
        tail_stable_cache_len_      = last_block_start;
        tail_stable_cache_in_fence_ = in_code_fence_;
        tail_stable_cache_abs_end_  = last_blk_abs;
        return tail_stable_cache_blocks_;
    };

    // Render the still-growing LAST terminated block (without the live
    // line). Memoized on the terminated slice's absolute end. This is the
    // block the merge test may replace with a live-line-merged version;
    // when the live line does NOT continue it, this standalone render is
    // used. Cheap re-parse of just the last block on the miss frame.
    auto last_block_rendered = [&]() -> std::shared_ptr<const Element> {
        if (tail_term_cache_blocks_.size() == 1
            && tail_term_cache_in_fence_ == in_code_fence_
            && tail_term_cache_abs_end_  == term_abs_end
            && tail_term_cache_len_      == terminated.size())
            return tail_term_cache_blocks_.front();
        std::string last_src;
        if (last_block_start == 0) last_src = fence_prefix;
        last_src.append(terminated.data() + last_block_start,
                        terminated.size() - last_block_start);
        auto blocks = canonical_blocks(last_src);
        std::shared_ptr<const Element> el;
        if (!blocks.empty()) {
            apply_live_table_floor_(blocks.back());
            el = std::make_shared<const Element>(
                md_block_to_element(blocks.back()));
        }
        tail_term_cache_blocks_.clear();
        if (el) tail_term_cache_blocks_.push_back(el);
        tail_term_cache_len_      = terminated.size();
        tail_term_cache_in_fence_ = in_code_fence_;
        tail_term_cache_abs_end_  = term_abs_end;
        return el;
    };

    // Wrap a cached FROZEN block in a hash-keyed ComponentElement so the
    // renderer blits its cells. Key = (tag, instance, STABLE-prefix abs
    // end, block index): stable while the frozen prefix is unchanged (it
    // only advances once per block boundary), so the renderer keeps the
    // painted cells warm across the many drain frames where only the last
    // block / live line move. Keying on the terminated abs_end instead
    // would rotate the key every frame and force a needless re-render of
    // the whole frozen prefix.
    auto wrap_term_block = [&](const std::shared_ptr<const Element>& blk,
                               std::size_t idx) -> Element {
        ComponentElement comp;
        comp.hash_id = ::maya::CacheIdBuilder{}
            .add(std::string_view{"strmd-term"})
            .add(static_cast<std::uint64_t>(instance_id_))
            .add(static_cast<std::uint64_t>(last_blk_abs))
            .add(static_cast<std::uint64_t>(idx))
            .build();
        comp.render = [blk](int, int) -> Element { return *blk; };
        return Element{std::move(comp)};
    };

    // FROZEN-prefix blocks [0, last_block_start) as a SINGLE cached vstack
    // Element. Each block inside is still its own hash-keyed
    // ComponentElement (so the renderer blits per block — no single-wrapper
    // cells-height bug), but they're pre-assembled into one vstack once per
    // block-boundary and cached, so the caller splices ONE Element handle
    // instead of copying ~130 per-block handles + re-running the vstack
    // builder over them EVERY frame (the residual O(frozen blocks)
    // per-frame cost during a burst drain). Safe against scrollback: the
    // frozen prefix sits BEHIND the reveal cursor, so commit-behind-cursor
    // commits it (into build()'s per-block committed children) before it
    // can scroll off as a tail sub-tree — the oracle/reveal_scrollback
    // tests gate this. Memoized on the frozen prefix's absolute end.
    auto stable_vstack = [&](const std::vector<std::shared_ptr<const Element>>& stable)
            -> const Element& {
        const std::size_t n = stable.size();
        const bool hit =
            tail_wrap_cache_abs_end_  == last_blk_abs
            && tail_wrap_cache_in_fence_ == in_code_fence_
            && tail_wrap_cache_block_count_ == n
            && tail_wrap_cache_blocks_.size() == 1;
        if (hit) return tail_wrap_cache_blocks_.front();
        std::vector<Element> kids;
        kids.reserve(n);
        for (std::size_t i = 0; i < n; ++i)
            kids.push_back(wrap_term_block(stable[i], i));
        Element vs = (kids.size() == 1)
            ? std::move(kids.front())
            : detail::vstack().gap(1)(std::move(kids)).build();
        tail_wrap_cache_blocks_.clear();
        tail_wrap_cache_blocks_.push_back(std::move(vs));
        tail_wrap_cache_abs_end_     = last_blk_abs;
        tail_wrap_cache_len_         = last_block_start;
        tail_wrap_cache_in_fence_    = in_code_fence_;
        tail_wrap_cache_block_count_ = n;
        return tail_wrap_cache_blocks_.front();
    };

    // Wrap the still-growing LAST terminated block for the non-continuing
    // path. Its hash_id keys on the terminated slice's absolute end so it
    // re-renders (renderer blits from cache) only when a new row lands.
    auto wrap_last_block = [&](const std::shared_ptr<const Element>& blk)
            -> Element {
        ComponentElement comp;
        comp.hash_id = ::maya::CacheIdBuilder{}
            .add(std::string_view{"strmd-term-last"})
            .add(static_cast<std::uint64_t>(instance_id_))
            .add(term_slice_hash)
            .build();
        comp.render = [blk](int, int) -> Element { return *blk; };
        return Element{std::move(comp)};
    };

    // Decide how to render the live partial last line. If folding it
    // into the block above (by appending a synthetic newline) keeps the
    // SAME block count, the live line is a CONTINUATION of the last
    // terminated block (another `>` quote row, another list item, another
    // table row) — render it canonically inside that block so it doesn't
    // pop from a separate inline row into the block when its real `\n`
    // lands (the 2-row collapse that shows up at the commit seam). If it
    // STARTS A NEW block (count grows) or isn't terminable cleanly, keep
    // it as a separate inline row below — a half-typed new-block marker
    // must stay literal so it can't snap shape.
    std::vector<Element> rendered;
    {
        std::string trimmed_live{live_line};
        while (!trimmed_live.empty()
               && (trimmed_live.back() == '\n' || trimmed_live.back() == '\r'))
            trimmed_live.pop_back();

        // A live line that is only fence characters (`\`` / `~`) is a
        // half-typed CLOSING fence — rendering it as a row (either inline
        // or as a code line inside the open fence) adds a row that
        // vanishes the instant the third backtick lands and the block
        // commits. Suppress it, but ONLY when we're genuinely inside an
        // open fence (otherwise a bare `` ` `` starting inline code in
        // prose would wrongly disappear).
        {
            bool all_fence = !trimmed_live.empty();
            for (char c : trimmed_live)
                if (c != '`' && c != '~') { all_fence = false; break; }
            if (all_fence) {
                // Fence parity over committed-side state + terminated
                // rows: an opener/closer is a line whose first non-space
                // run is >=3 of the same fence char. Odd parity = the
                // last terminated row left us INSIDE a fence, so this
                // all-fence live line is the closing delimiter. Uses the
                // shared spec-faithful classifier (char + run-length
                // matched close) so the verdict agrees with in_code_fence_.
                md_detail::streaming::FenceState fst{
                    in_code_fence_, fence_open_ch_, fence_open_len_};
                std::size_t p = 0;
                while (p < terminated.size()) {
                    std::size_t e = terminated.find('\n', p);
                    if (e == std::string_view::npos) e = terminated.size();
                    (void)md_detail::streaming::fence_scan_line(
                        fst, terminated, p, e);
                    p = e + 1;
                }
                if (fst.in_fence) trimmed_live.clear();
            }
        }

        // Same rule for a fence nested INSIDE a blockquote ("> ``"). The
        // top-level test above can't see it: the '>' marker defeats the
        // all-fence check and in_code_fence_ tracks only top-level fences.
        // While a quote-nested fence is OPEN, a live line whose DEQUOTED
        // content is empty (">" / "> ") or all fence chars ("> ``") is a
        // code row that exists only until the closing "> ```" completes —
        // rendering it adds a row that vanishes at the close (the
        // quote_code composer bounce). Compute the nested parity over the
        // terminated rows: dequote each quote line and feed the shared
        // classifier; any top-level (depth-0) line closes the quote and
        // kills the nested fence (lazy continuation doesn't extend a
        // fenced block). Lines inside an open TOP-LEVEL fence are content
        // and never touch the nested state.
        if (!trimmed_live.empty()) {
            int live_depth = 0;
            std::string_view dq = dequote_line(trimmed_live, &live_depth);
            bool candidate = live_depth > 0;
            if (candidate)
                for (char c : dq)
                    if (c != '`' && c != '~') { candidate = false; break; }
            if (candidate) {
                md_detail::streaming::FenceState top{
                    in_code_fence_, fence_open_ch_, fence_open_len_};
                md_detail::streaming::FenceState qst{};
                std::size_t p = 0;
                while (p < terminated.size()) {
                    std::size_t e = terminated.find('\n', p);
                    if (e == std::string_view::npos) e = terminated.size();
                    if (top.in_fence) {
                        (void)md_detail::streaming::fence_scan_line(
                            top, terminated, p, e);
                    } else {
                        std::string_view ln = terminated.substr(p, e - p);
                        int d = 0;
                        std::string_view inner = dequote_line(ln, &d);
                        if (d > 0) {
                            (void)md_detail::streaming::fence_scan_line(
                                qst, inner, 0, inner.size());
                        } else {
                            qst = {};   // left the quote — nested fence dies
                            (void)md_detail::streaming::fence_scan_line(
                                top, terminated, p, e);
                        }
                    }
                    p = e + 1;
                }
                if (qst.in_fence) trimmed_live.clear();
            }
        }

        // Half-typed list-marker live line under a LIST block: "  -"
        // transiently reparses as a setext underline INSIDE the item
        // (title + full-width rule row — a garbled taller frame) until the
        // first content byte turns it into a nested item — a shrink plus
        // one frame of visual garbage (the nested_list bounce). Hide the
        // marker-only phase; the first content byte reveals the item
        // (growth only). Gated on the last terminated block being a list
        // so a "-" under a paragraph keeps its legitimate setext reading.
        if (!trimmed_live.empty() && half_list_marker(trimmed_live)) {
            std::string_view blk0 = terminated.substr(last_block_start);
            std::size_t fe = blk0.find('\n');
            std::string_view fl0 = (fe == std::string_view::npos)
                ? blk0 : blk0.substr(0, fe);
            if (ul_marker_len(fl0) > 0 || ol_marker_len(fl0) > 0)
                trimmed_live.clear();
        }

        // Live item whose CONTENT is itself a half-typed marker — "- *"
        // en route to "- **bold**…" (the dominant LLM bullet shape).
        // Parsed as-is, the "*" content reads as a NESTED empty list:
        // one row TALLER than the eventual bold/emphasis reading — a
        // guaranteed shrink on the disambiguating byte. Truncate to the
        // bare marker while ambiguous: "- " renders as an empty item
        // (1 row); the next byte either turns it into literal content
        // ("**" — same row) or a real nested item ("* x" — grows).
        // Monotonic both ways.
        if (!trimmed_live.empty() && !in_code_fence_) {
            std::string_view lv = trimmed_live;
            int ml = ul_marker_len(lv);
            if (ml == 0) ml = ol_marker_len(lv);
            if (ml > 0 && static_cast<std::size_t>(ml) < lv.size()) {
                std::string_view rest = lv.substr(
                    static_cast<std::size_t>(ml));
                if (half_list_marker(rest))
                    trimmed_live.resize(static_cast<std::size_t>(ml));
            }
        }

        // Live line that will render as ZERO rows once committed — painting
        // it now guarantees a shrink at its '\n':
        //   • an HTML block-opener tag line (`<div>` …): html::render
        //     consumes bare tag markup — hidden when committed. Kind-6
        //     openers interrupt paragraphs too, so no blank-above gate.
        //   • a link-reference definition (`[label]: url`): extracted into
        //     the ref map, never rendered. Only recognised at a PARAGRAPH
        //     START (a ref-def can't interrupt a paragraph), so gate on a
        //     blank line above — mid-paragraph `[d]: …` stays visible text.
        // Both tests are prefix-stable (hidden from the first byte, flip to
        // visible only on a disproving byte — a growth), so no pop-in/out.
        if (!trimmed_live.empty() && !in_code_fence_) {
            const bool blank_above_live =
                terminated.size() >= 2
                && terminated[terminated.size() - 2] == '\n';
            if (html_block_live_prefix(trimmed_live)
                || (blank_above_live && link_ref_live_prefix(trimmed_live)))
                trimmed_live.clear();
        }

        bool live_continues = false;
        bool list_chunked   = false;
        bool table_chunked  = false;

        // ── Incremental chunked render for a TIGHT-LIST tail ────────────
        //
        // A tight list (no \n\n between items — the dominant LLM reply
        // shape) never reaches a commit boundary, so the WHOLE list rides
        // the live tail for its entire stream. The merge test below keys
        // on the live-line hash, which moves EVERY reveal frame, so it
        // re-parsed + re-rendered the whole list block per frame — cost
        // rising linearly with list length (10 ms/frame at ~3000 items:
        // the "long turn gets stuck, then bursts" stall).
        //
        // Lists are chunk-splittable at TOP-LEVEL ITEM boundaries with
        // cell-identical output: md_block_to_element(List) is a gap-0
        // vstack of item rows, nested content binds to its parent item,
        // and ordered numbering is forced continuous across chunks by
        // overriding each chunk's start_num. So seal every kChunkItems
        // closed items into a hash-keyed ComponentElement ONCE (the
        // renderer blits it from cache thereafter) and re-parse only
        // [unsealed closed items + live line] per frame — O(chunk), not
        // O(list).
        //
        // Engage rules (all must hold; otherwise the general path below
        // is authoritative):
        //   • not inside an open code fence;
        //   • the LAST block of the terminated prefix starts with a list
        //     marker at its own base indent.
        // Loose lists (blank lines between items) engage too: since the
        // loose-list cohesion fix, classify_blank_line keeps the WHOLE
        // loose list as the last block, so without chunking every frame
        // re-parsed + re-rendered the full list — the exact O(list)/frame
        // cost this chunker was built to kill. Chunking at top-level item
        // boundaries is render-safe for loose lists because render_list
        // never reads List::loose (loose/tight only affects <p> wrapping
        // in HTML renderers): a chunk that re-parses as tight renders
        // cell-identically to its rows inside the whole-list parse
        // (proven by the chunk-equivalence probe across ul/ol/multi-para/
        // nested/task/wrapped shapes). The per-chunk single-List parse
        // check below remains the bail-out for any shape where the
        // engine disagrees.
        static const bool chunk_disabled = [] {
            const char* e = std::getenv("MAYA_NO_LIST_CHUNK");
            return e && e[0] && e[0] != '0';
        }();
        if (!chunk_disabled && !in_code_fence_) {
            std::string_view blk = terminated.substr(last_block_start);
            const std::size_t blk_abs = body_abs_start + last_block_start;
            // First line must be a list marker; capture its indent as the
            // top-level base.
            std::size_t fl_end = blk.find('\n');
            std::string_view fl = (fl_end == std::string_view::npos)
                ? blk : blk.substr(0, fl_end);
            const bool fl_is_item =
                ul_marker_len(fl) > 0 || ol_marker_len(fl) > 0;
            const int base_indent = fl_is_item ? count_indent(fl) : 0;
            if (fl_is_item) {
                // Offsets (relative to blk) of every TOP-LEVEL item start.
                std::vector<std::size_t> item_starts;
                std::size_t p = 0;
                while (p < blk.size()) {
                    std::size_t e = blk.find('\n', p);
                    if (e == std::string_view::npos) e = blk.size();
                    std::string_view ln = blk.substr(p, e - p);
                    if ((ul_marker_len(ln) > 0 || ol_marker_len(ln) > 0)
                        && count_indent(ln) == base_indent)
                        item_starts.push_back(p);
                    p = e + 1;
                }
                if (!item_starts.empty()) {
                    constexpr std::size_t kChunkItems = 16;
                    // Reset the chunk cache when the block base moved
                    // (commit fired / clip rebase / different list).
                    if (tail_list_chunks_abs_start_ != blk_abs) {
                        tail_list_chunks_.clear();
                        tail_list_chunks_abs_start_ = blk_abs;
                        tail_list_chunks_abs_end_   = blk_abs;  // sealed end
                        tail_list_next_ord_         = -1;
                    }
                    std::size_t sealed_off =
                        tail_list_chunks_abs_end_ - blk_abs;   // rel to blk
                    // Defensive: sealed_off must sit ON an item boundary
                    // within the current slice; otherwise resync from 0.
                    if (sealed_off > blk.size()) {
                        tail_list_chunks_.clear();
                        tail_list_chunks_abs_end_ = blk_abs;
                        tail_list_next_ord_       = -1;
                        sealed_off = 0;
                    }
                    // Count closed items: every top-level item start
                    // strictly BEFORE the last one owns fully-terminated
                    // rows. items in [first_unsealed_idx, closed_count)
                    // are sealable.
                    const std::size_t last_item_off = item_starts.back();
                    // index of first item at/after sealed_off
                    std::size_t idx = 0;
                    while (idx < item_starts.size()
                           && item_starts[idx] < sealed_off) ++idx;
                    // Seal in blocks of kChunkItems while enough CLOSED
                    // items (strictly before the last item) remain.
                    while (idx + kChunkItems < item_starts.size()
                           && item_starts[idx + kChunkItems] <= last_item_off) {
                        std::size_t a = item_starts[idx];
                        std::size_t b = item_starts[idx + kChunkItems];
                        auto blocks = canonical_blocks(
                            std::string{blk.substr(a, b - a)});
                        if (blocks.size() != 1) break;  // engine disagrees — bail
                        if (auto* l = std::get_if<md::List>(&blocks.front().inner)) {
                            if (l->ordered) {
                                if (tail_list_next_ord_ < 0)
                                    tail_list_next_ord_ = l->start_num;
                                l->start_num =
                                    static_cast<int>(tail_list_next_ord_);
                                tail_list_next_ord_ +=
                                    static_cast<long>(l->items.size());
                            }
                        } else {
                            break;                       // not a list — bail
                        }
                        auto el = std::make_shared<const Element>(
                            md_block_to_element(blocks.front()));
                        ComponentElement comp;
                        comp.hash_id = ::maya::CacheIdBuilder{}
                            .add(std::string_view{"strmd-listchunk"})
                            .add(static_cast<std::uint64_t>(instance_id_))
                            .add(static_cast<std::uint64_t>(blk_abs + a))
                            .add(static_cast<std::uint64_t>(b - a))
                            .build();
                        comp.render = [el](int, int) -> Element { return *el; };
                        tail_list_chunks_.push_back(Element{std::move(comp)});
                        tail_list_chunks_abs_end_ = blk_abs + b;
                        idx += kChunkItems;
                        sealed_off = b;
                    }

                    // Per-frame slice: unsealed closed items + last item
                    // + live line. Bounded at < 2*kChunkItems items.
                    std::string pending;
                    pending.append(blk.data() + sealed_off,
                                   blk.size() - sealed_off);
                    const bool has_live = !trimmed_live.empty();
                    if (has_live) { pending += trimmed_live; pending += '\n'; }
                    auto pblocks = canonical_blocks(pending);
                    // The pending slice must still be ONE list block —
                    // the live line either continues the list (another
                    // item / lazy continuation) or the whole thing is
                    // list-shaped without it. If the live line opened a
                    // NEW block (heading, fence…), render the closed part
                    // as the list and let the standard live-line handling
                    // below place the new block's eager/inline render.
                    bool pending_is_list =
                        pblocks.size() == 1
                        && std::holds_alternative<md::List>(pblocks.front().inner);
                    bool live_folded = pending_is_list;
                    if (!pending_is_list && has_live) {
                        // Retry without the live line: closed items only.
                        pending.resize(pending.size() - trimmed_live.size() - 1);
                        pblocks = canonical_blocks(pending);
                        pending_is_list =
                            pblocks.size() == 1
                            && std::holds_alternative<md::List>(
                                   pblocks.front().inner);
                        live_folded = false;
                    }
                    if (pending_is_list) {
                        auto& pl = std::get<md::List>(pblocks.front().inner);
                        if (pl.ordered && tail_list_next_ord_ >= 0)
                            pl.start_num =
                                static_cast<int>(tail_list_next_ord_);
                        Element pend_el =
                            md_block_to_element(pblocks.front());
                        // Assemble the full list as ONE gap-0 vstack —
                        // identical rows to the whole-list render.
                        std::vector<Element> lk;
                        lk.reserve(tail_list_chunks_.size() + 1);
                        for (const auto& c : tail_list_chunks_)
                            lk.push_back(c);
                        lk.push_back(std::move(pend_el));
                        Element list_el = (lk.size() == 1)
                            ? std::move(lk.front())
                            : detail::vstack()(std::move(lk)).build();
                        if (last_block_start > 0)
                            rendered.push_back(
                                stable_vstack(stable_rendered()));
                        rendered.push_back(std::move(list_el));
                        list_chunked   = true;
                        live_continues = live_folded;
                        if (live_folded) trimmed_live.clear();
                        // A live line that did NOT fold falls through to
                        // the standard "live line starts a new block"
                        // handling below (eager / inline).
                    }
                }
            }
        }

        if (!list_chunked) {
            // ── Incremental chunked render for a TABLE tail ────────────
            //
            // Mirror of the list chunker above for a long GFM table riding
            // the live tail. A table only commits when a BREAKING line
            // lands below it (find_table_end), so a long streaming table
            // re-rendered wholesale per frame — O(rows) of cell layout,
            // the md_cache_probe table O(N) escape. Terminated data rows
            // seal into fixed kChunkRows chunks: each chunk re-parses
            // [header+delim+rows] so the engine recognises the slice as a
            // table, then renders with md::Table::omit_top/omit_bottom so
            // only its own rows paint (header/top border owned by chunk
            // 0, bottom border by the pending remainder, row-separator
            // seams emitted by omit_top chunks). The live-table column
            // floor pins every chunk to identical column widths — the
            // floor is the per-column max over EVERY arrived row incl.
            // the header, so each chunk's own ideal folds to exactly the
            // floor and the concatenation is cell-identical to the
            // whole-table render (and to the committed render, whose
            // natural ideal equals the floor by construction). Chunks are
            // hash-keyed ComponentElements (renderer blits); per frame
            // only [unsealed rows + live row] re-parse — O(chunk). The
            // floor hash keys the reset: a new widest cell re-seals
            // everything at the new widths (rare; amortised O(1)).
            //
            // Engage rules: not in a fence, floor present (reveal live —
            // the floor is what guarantees width equality), the last
            // block is a well-formed table shape, EVERY terminated line
            // after the delimiter is a `|` row (a plain continuation line
            // is legal GFM but changes the parse — bail to the standard
            // path), and there are enough closed rows to seal at least
            // one chunk.
            if (!in_code_fence_ && !live_table_floor_.empty()) {
                std::string_view blk = terminated.substr(last_block_start);
                const std::size_t blk_abs = body_abs_start + last_block_start;
                auto first_pipe_ln = [](std::string_view s) noexcept -> bool {
                    std::size_t k = 0;
                    while (k < 4 && k < s.size() && s[k] == ' ') ++k;
                    return k < s.size() && s[k] == '|';
                };
                std::size_t l1 = blk.find('\n');
                bool shape = l1 != std::string_view::npos
                             && first_pipe_ln(blk.substr(0, l1));
                std::size_t delim_end = std::string_view::npos;
                if (shape) {
                    delim_end = blk.find('\n', l1 + 1);
                    shape = delim_end != std::string_view::npos;
                    if (shape) {
                        std::string_view dl = blk.substr(l1 + 1,
                                                         delim_end - l1 - 1);
                        std::size_t dk = 0;
                        while (dk < 4 && dk < dl.size() && dl[dk] == ' ') ++dk;
                        bool has_dash = false;
                        bool ok = dk < dl.size() && dl[dk] == '|';
                        for (std::size_t q = dk; ok && q < dl.size(); ++q) {
                            char c = dl[q];
                            if (c == '-') { has_dash = true; continue; }
                            if (c == '|' || c == ':' || c == ' '
                                || c == '\t' || c == '\r') continue;
                            ok = false;
                        }
                        shape = ok && has_dash;
                    }
                }
                if (shape) {
                    // Terminated data-row offsets (rel to blk). Bail if any
                    // terminated line after the delimiter is not a pipe row.
                    std::vector<std::size_t> row_starts;
                    std::size_t p = delim_end + 1;
                    bool all_pipe = true;
                    while (p < blk.size()) {
                        std::size_t e = blk.find('\n', p);
                        if (e == std::string_view::npos) break; // live remainder
                        if (!first_pipe_ln(blk.substr(p, e - p))) {
                            all_pipe = false;
                            break;
                        }
                        row_starts.push_back(p);
                        p = e + 1;
                    }
                    constexpr std::size_t kChunkRows = 24;
                    if (all_pipe && row_starts.size() > kChunkRows) {
                        std::uint64_t fh = 1469598103934665603ull;
                        for (int w : live_table_floor_)
                            fh = (fh ^ static_cast<std::uint64_t>(
                                      static_cast<unsigned>(w)))
                                 * 1099511628211ull;
                        if (tail_table_chunks_abs_start_ != blk_abs
                            || tail_table_floor_hash_ != fh
                            || tail_table_chunks_abs_end_ < blk_abs
                            || tail_table_chunks_abs_end_ - blk_abs > blk.size()) {
                            tail_table_chunks_.clear();
                            tail_table_chunks_abs_start_ = blk_abs;
                            tail_table_chunks_abs_end_   = blk_abs;
                            tail_table_floor_hash_       = fh;
                        }
                        std::string_view head = blk.substr(0, delim_end + 1);
                        std::size_t sealed_off =
                            tail_table_chunks_abs_end_ - blk_abs;
                        std::size_t idx = 0;
                        while (idx < row_starts.size()
                               && row_starts[idx] < sealed_off) ++idx;
                        // Seal while at least one closed row remains
                        // unsealed (the pending remainder must own ≥1 row
                        // so it carries the bottom border under real rows).
                        while (idx + kChunkRows < row_starts.size()) {
                            std::size_t a = row_starts[idx];
                            std::size_t b = row_starts[idx + kChunkRows];
                            std::string csrc{head};
                            csrc.append(blk.data() + a, b - a);
                            auto blocks = canonical_blocks(csrc);
                            if (blocks.size() != 1) break;
                            auto* t = std::get_if<md::Table>(
                                &blocks.front().inner);
                            if (!t) break;
                            t->min_col_widths = live_table_floor_;
                            t->omit_top    = !tail_table_chunks_.empty();
                            t->omit_bottom = true;
                            auto el = std::make_shared<const Element>(
                                md_block_to_element(blocks.front()));
                            ComponentElement comp;
                            comp.hash_id = ::maya::CacheIdBuilder{}
                                .add(std::string_view{"strmd-tblchunk"})
                                .add(static_cast<std::uint64_t>(instance_id_))
                                .add(static_cast<std::uint64_t>(blk_abs + a))
                                .add(static_cast<std::uint64_t>(b - a))
                                .add(fh)
                                .build();
                            comp.render = [el](int, int) -> Element {
                                return *el;
                            };
                            tail_table_chunks_.push_back(
                                Element{std::move(comp)});
                            tail_table_chunks_abs_end_ = blk_abs + b;
                            idx += kChunkRows;
                            sealed_off = b;
                        }
                        if (!tail_table_chunks_.empty()) {
                            // Pending remainder: header+delim + unsealed
                            // terminated rows. Bounded < 2*kChunkRows rows.
                            std::string psrc{head};
                            psrc.append(blk.data() + sealed_off,
                                        blk.size() - sealed_off);
                            auto pblocks = canonical_blocks(psrc);
                            if (pblocks.size() == 1) {
                                if (auto* pt = std::get_if<md::Table>(
                                        &pblocks.front().inner)) {
                                    pt->min_col_widths = live_table_floor_;
                                    pt->omit_top    = true;
                                    pt->omit_bottom = false;
                                    auto pel = std::make_shared<const Element>(
                                        md_block_to_element(pblocks.front()));
                                    ComponentElement pcomp;
                                    pcomp.hash_id = ::maya::CacheIdBuilder{}
                                        .add(std::string_view{"strmd-tblpend"})
                                        .add(static_cast<std::uint64_t>(
                                                 instance_id_))
                                        .add(static_cast<std::uint64_t>(
                                                 term_abs_end))
                                        .add(fh)
                                        .build();
                                    pcomp.render = [pel](int, int) -> Element {
                                        return *pel;
                                    };
                                    std::vector<Element> tk;
                                    tk.reserve(tail_table_chunks_.size() + 1);
                                    for (const auto& c : tail_table_chunks_)
                                        tk.push_back(c);
                                    tk.push_back(Element{std::move(pcomp)});
                                    Element tbl_el = detail::vstack()(
                                        std::move(tk)).build();
                                    if (last_block_start > 0)
                                        rendered.push_back(
                                            stable_vstack(stable_rendered()));
                                    rendered.push_back(std::move(tbl_el));
                                    table_chunked = true;
                                    // Live partial row renders inline below
                                    // via the standard live-line handling
                                    // (not a list/quote marker → inline).
                                }
                            }
                        }
                    }
                }
            }
        }

        if (!list_chunked && !table_chunked && !trimmed_live.empty()) {
            // ── Big-table fold bypass ──
            //
            // Folding the live partial row into the last block re-renders
            // that block EVERY frame the live row grows a byte. For most
            // blocks that's cheap, but a TABLE's md_block_to_element is
            // O(rows) of column measurement + cell layout — on a table
            // that has streamed hundreds of rows without a blank line the
            // fold costs multiple ms per frame (the long-table "stuck then
            // burst" stall; the merge TEST itself is an O(rows) parse per
            // frame too). Above a row threshold, skip the fold entirely:
            // the standalone last_block_rendered() render is memoized per
            // row-commit (re-parses once per '\n', not once per byte) and
            // the live row renders inline below, exactly like the eager
            // table path in render_tail_inner. Costs a 1-row seam jitter
            // as each row folds in — invisible next to a multi-ms stall —
            // and only engages on tables larger than any test/doc shape
            // (threshold 60 lines; the render_tail_inner eager table path
            // behaves this way from row one: live row inline below).
            constexpr std::size_t kBigTableLines = 60;
            bool big_table_bypass = false;
            if (!in_code_fence_) {
                std::string_view blk = terminated.substr(last_block_start);
                // Cheap shape probe: first line starts with '|' (≤3 sp),
                // second line is a delimiter row, and the block exceeds
                // the line threshold. No parse.
                std::size_t k = 0;
                while (k < 4 && k < blk.size() && blk[k] == ' ') ++k;
                if (k < blk.size() && blk[k] == '|') {
                    std::size_t l1 = blk.find('\n');
                    if (l1 != std::string_view::npos && l1 + 1 < blk.size()) {
                        std::string_view d = blk.substr(l1 + 1);
                        std::size_t l2 = d.find('\n');
                        std::string_view dl = (l2 == std::string_view::npos)
                            ? d : d.substr(0, l2);
                        std::size_t dk = 0;
                        while (dk < 4 && dk < dl.size() && dl[dk] == ' ') ++dk;
                        bool has_dash = false, ok = dk < dl.size() && dl[dk] == '|';
                        for (std::size_t q = dk; ok && q < dl.size(); ++q) {
                            char c = dl[q];
                            if (c == '-') { has_dash = true; continue; }
                            if (c == '|' || c == ':' || c == ' '
                                || c == '\t' || c == '\r') continue;
                            ok = false;
                        }
                        if (ok && has_dash) {
                            std::size_t lines = 0;
                            for (std::size_t q = 0;
                                 q < blk.size() && lines <= kBigTableLines; ++q)
                                if (blk[q] == '\n') ++lines;
                            big_table_bypass = lines > kBigTableLines;
                        }
                    }
                }
            }
            if (!big_table_bypass) {
            // Continuation test: does the live line fold INTO the last
            // terminated block (making it one taller) or START a new one?
            //
            // The naive form re-parsed `fence_prefix + terminated +
            // live_line` in full every frame the live line grew — O(the
            // whole uncommitted tail) per frame, which after a big burst
            // delta (the reveal cursor lagging tens of KB behind the live
            // edge) is the dominant streaming cost. But the ONLY block the
            // live line can extend is the LAST terminated one; every block
            // before it is closed by a blank-line / fence boundary that the
            // live line cannot reach back across. So parse only the last
            // terminated block's span + the live line, and compare against
            // 1 (did the live line stay inside that single block?) instead
            // of re-deriving the whole prefix's block count. That makes the
            // per-frame merge test O(last block), not O(tail).
            //
            // last_block_start (hoisted above, shared with the incremental
            // terminated-render memo): byte offset in `terminated` where
            // the last top-level block begins.

            // Parse only [last_block_start, end) + the live line. The
            // fence_prefix re-opener is only needed when the LAST block
            // itself begins inside the committed-side open fence, i.e. the
            // whole terminated prefix is that one open-fence block
            // (last_block_start == 0). Otherwise the last block is a fresh
            // top-level block whose own opener (if any) is inside the slice.
            //
            // Memoized: keyed on (last block abs start, terminated abs end,
            // live-line hash, fence). When none moved since last frame the
            // continuation verdict + merged render are byte-identical, so
            // reuse them and skip the parse + md_block_to_element entirely
            // — the repeated last-block render was the dominant per-frame
            // cost during a burst drain.
            const std::uint64_t live_hash = fnv1a64(trimmed_live);
            // Hash-keyed wrapper for the merged render. Without it the
            // merged Element is spliced RAW into the tail vstack, so
            // render_tree re-runs layout::compute + paint over the whole
            // merged block EVERY FRAME — even on memo-hit frames where
            // nothing changed. For a paragraph that's noise; for a
            // 100-row table it's milliseconds per frame (the md_cache_probe
            // table O(N) escape). The key moves exactly when the merged
            // content can differ (block base / terminated end / live-line
            // bytes / fence), so unchanged frames blit cells from the
            // renderer's component cache instead.
            auto wrap_merged = [&](const std::shared_ptr<const Element>& el)
                    -> Element {
                ComponentElement comp;
                comp.hash_id = ::maya::CacheIdBuilder{}
                    .add(std::string_view{"strmd-merged"})
                    .add(static_cast<std::uint64_t>(instance_id_))
                    .add(static_cast<std::uint64_t>(last_blk_abs))
                    .add(static_cast<std::uint64_t>(term_abs_end))
                    .add(live_hash)
                    .add(static_cast<std::uint64_t>(in_code_fence_ ? 1 : 0))
                    .build();
                comp.render = [el](int, int) -> Element { return *el; };
                return Element{std::move(comp)};
            };
            if (tail_merge_cache_el_
                && tail_merge_cache_last_blk_abs_ == last_blk_abs
                && tail_merge_cache_term_abs_end_ == term_abs_end
                && tail_merge_cache_live_hash_    == live_hash
                && tail_merge_cache_in_fence_     == in_code_fence_) {
                if (tail_merge_cache_continues_) {
                    live_continues = true;
                    if (last_block_start > 0)
                        rendered.push_back(stable_vstack(stable_rendered()));
                    rendered.push_back(wrap_merged(tail_merge_cache_el_));
                }
                // else: verdict was "does not continue" — fall through to
                //       the !live_continues path below (cheap, no parse).
            } else {
                std::string merged;
                if (last_block_start == 0) merged = fence_prefix;
                merged.append(terminated.data() + last_block_start,
                              terminated.size() - last_block_start);
                merged += trimmed_live;
                merged += '\n';
                auto merged_blocks = canonical_blocks(merged);
                // The live line folded into the last block iff the
                // last-block slice + live line still parses to exactly ONE
                // block.
                const bool continues = merged_blocks.size() == 1;
                if (continues) {
                    live_continues = true;
                    if (last_block_start > 0)
                        rendered.push_back(stable_vstack(stable_rendered()));
                    apply_live_table_floor_(merged_blocks.back());
                    tail_merge_cache_el_ = std::make_shared<const Element>(
                        md_block_to_element(merged_blocks.back()));
                    rendered.push_back(wrap_merged(tail_merge_cache_el_));
                } else {
                    tail_merge_cache_el_ = nullptr;
                }
                // Store the memo.
                tail_merge_cache_last_blk_abs_ = last_blk_abs;
                tail_merge_cache_term_abs_end_ = term_abs_end;
                tail_merge_cache_live_hash_    = live_hash;
                tail_merge_cache_in_fence_     = in_code_fence_;
                tail_merge_cache_continues_    = continues;
            }
            }
        }
        if (!list_chunked && !table_chunked && !live_continues) {
            // Frozen prefix (one cached vstack) + the standalone last block.
            if (last_block_start > 0)
                rendered.push_back(stable_vstack(stable_rendered()));
            if (auto last_blk = last_block_rendered())
                rendered.push_back(wrap_last_block(last_blk));
        }

        // Live line that STARTS a new block stays inline below. Use the
        // SUPPRESSION-ADJUSTED live text (trimmed_live), not the raw
        // live_line — the half-marker truncation / hide rules above must
        // hold on this path too or the taller transient reading leaks
        // back in through the eager render.
        if (!live_continues && !trimmed_live.empty()) {
            std::string_view ll = trimmed_live;
            if (!ll.empty()) {
                // A live line that OPENS a fresh list item / blockquote row
                // (no prior same-kind block to continue — so the merge test
                // above returned "does not continue") would otherwise render
                // as RAW inline text (`- item`, `1. item`, `> quote`) in body
                // colour until its '\n' lands, then SNAP into the styled block
                // when it commits — the reported "list item renders
                // unformatted, then jumps to the formatted block and takes
                // the composer up" artefact. Render it eagerly in committed
                // shape instead: synthesize the terminating '\n' and route
                // through the SAME render_eager_slice (parse_markdown_impl +
                // md_block_to_element) pipeline commit_range uses, so the
                // live single-item block is byte- and row-identical to what
                // eventually commits — no unformatted phase, no height snap.
                // Only the last (live) line is affected; the commit
                // watermark and reveal clip are untouched.
                bool is_ll_list  = ul_marker_len(ll) > 0 || ol_marker_len(ll) > 0;
                bool is_ll_quote = false;
                if (!is_ll_list) {
                    std::size_t k = 0;
                    while (k < 4 && k < ll.size() && ll[k] == ' ') ++k;
                    if (k < ll.size() && ll[k] == '>')
                        is_ll_quote = (k + 1 == ll.size()
                                       || ll[k + 1] == ' ' || ll[k + 1] == '\t');
                }
                bool eager_done = false;
                if (is_ll_list || is_ll_quote) {
                    std::string synth;
                    synth.reserve(ll.size() + 1);
                    synth.assign(ll);
                    while (!synth.empty()
                           && (synth.back() == '\n' || synth.back() == '\r'))
                        synth.pop_back();
                    synth.push_back('\n');
                    std::vector<Element> ekids;
                    ekids.reserve(1);
                    render_eager_slice(synth, ekids);
                    if (ekids.size() == 1) {
                        rendered.push_back(std::move(ekids.front()));
                        eager_done = true;
                    } else if (!ekids.empty()) {
                        rendered.push_back(
                            detail::vstack().gap(1)(std::move(ekids)).build());
                        eager_done = true;
                    }
                }
                if (!eager_done) {
                    auto spans = parse_inlines(ll);
                    std::string content;
                    std::vector<StyledRun> runs;
                    const Style base = Style{}.with_fg(colors::text());
                    for (const auto& sp : spans)
                        flatten_inline(sp, base, content, runs);
                    if (!runs.empty()) {
                        if (runs.size() == 1)
                            rendered.push_back(Element{TextElement{
                                .content = std::move(content), .style = runs[0].style}});
                        else
                            rendered.push_back(Element{TextElement{
                                .content = std::move(content),
                                .style   = Style{}.with_fg(colors::text()),
                                .runs    = std::move(runs)}});
                    }
                }
            }
        }
    }
    if (rendered.empty()) {
        // Terminated rows exist but produced NOTHING to render. If the
        // canonical parse of the terminated slice yields zero blocks the
        // rows are pure link-ref / footnote-def definitions — invisible in
        // their committed shape — so falling back to render_tail_inner
        // (which would paint the raw `[d]: url` bytes as literal text)
        // re-introduces the very row the canonical hide removed, and it
        // vanishes again at commit: render nothing instead.
        if (!last_block_rendered()) return Element{TextElement{}};
        return render_tail_inner(tail);
    }

    Element canonical = (rendered.size() == 1)
        ? std::move(rendered.front())
        : detail::vstack().gap(1)(std::move(rendered)).build();

    // Emit the canonical render. The terminated rows are byte-identical
    // to what commit_range will produce (continuous across the commit
    // seam) and the live line is either folded into its continuing block
    // (no pop when it terminates) or held as a literal inline row that
    // can only grow. Both cases keep height monotonic.
    return canonical;
    }();

    // Store the memo so an unchanged tail next frame returns by
    // shared_ptr copy with zero parsing.
    tail_canon_cache_el_       = std::make_shared<const Element>(result);
    tail_canon_cache_version_  = source_version_;
    tail_canon_cache_len_      = tail.size();
    tail_canon_cache_hash_     = tail_hash;
    tail_canon_cache_in_fence_ = in_code_fence_;
    return result;
}

Element StreamingMarkdown::render_tail_inner(std::string_view tail) const {
    // Skip leading newlines (whitespace from a prior commit boundary).
    std::size_t start = 0;
    while (start < tail.size() && tail[start] == '\n') ++start;
    if (start >= tail.size()) {
        return Element{TextElement{}};
    }
    std::string_view body = tail.substr(start);

    if (in_code_fence_) {
        // Inside an open code fence: render the literal bytes (including
        // the opening fence marker if it's still in this tail).  Wrap
        // mode keeps height monotonic in byte count.
        return Element{TextElement{
            .content = std::string{body},
            .style   = Style{}.with_fg(colors::code_fg()),
            .wrap    = TextWrap::Wrap,
        }};
    }

    // ── Open code fence at tail start: render in its committed shape
    //    (round border + language label + syntax-highlighted body) so
    //    the user sees the formatted code block from the moment the
    //    opening ``` arrives, not after the closing ``` lands.  The
    //    closing fence hasn't arrived (find_block_boundary would have
    //    committed past it), so the tail is a verbatim opener line +
    //    in-progress body.  Monotonic in byte count: body grows row by
    //    row inside the same border frame.
    if (body.size() >= 3 &&
        ((body[0] == '`' && body[1] == '`' && body[2] == '`') ||
         (body[0] == '~' && body[1] == '~' && body[2] == '~')))
    {
        auto eol = body.find('\n');
        std::string lang;
        std::string code;
        if (eol == std::string_view::npos) {
            // Opener line not yet terminated: treat the post-fence
            // characters as the language hint and the body as empty.
            // The ``` itself is hidden so the row doesn't flicker the
            // marker bytes before the line break arrives.
            std::string_view lsv = body.substr(3);
            while (!lsv.empty() && (lsv.back() == ' ' || lsv.back() == '\r'))
                lsv.remove_suffix(1);
            while (!lsv.empty() && lsv.front() == ' ')
                lsv.remove_prefix(1);
            lang = std::string{lsv};
        } else {
            std::string_view lsv = body.substr(3, eol - 3);
            while (!lsv.empty() && (lsv.back() == ' ' || lsv.back() == '\r'))
                lsv.remove_suffix(1);
            while (!lsv.empty() && lsv.front() == ' ')
                lsv.remove_prefix(1);
            lang = std::string{lsv};
            code = std::string{body.substr(eol + 1)};
            // Mirror the strip md_block_to_element applies post-commit: drop
            // one trailing '\n' on the live body so the live tail and the
            // committed render produce the SAME row count. Without this,
            // the closing fence commits and the renderer drops one row
            // (the trailing-newline phantom row) — a 1-row shrink at the
            // commit seam that the height-monotonicity test catches.
            if (!code.empty() && code.back() == '\n') code.pop_back();
        }
        // Match md_block_to_element's CodeBlock styling exactly — same
        // builder shape, same align_self(Stretch) so the in-flight tail
        // render and the committed block render produce identical
        // borders. Without the stretch the streaming-tail border tracks
        // the longest emitted line, drifting frame-to-frame.
        //
        // Empty body (opener landed but no content yet, OR opener+closer
        // with nothing between): render a 1-row dim placeholder that
        // mirrors md_block_to_element's empty-CodeBlock rendering.
        // Constant 1 row keeps live tail and committed render in
        // agreement (no shrink at commit seam) AND avoids reading as a
        // stray empty bordered box.
        if (code.empty()) {
            std::string label = lang.empty()
                ? std::string{"\xe2\x97\x8b empty code block"}
                : std::string{"\xe2\x97\x8b "} + lang + " (empty)";
            return Element{TextElement{
                .content = std::move(label),
                .style   = Style{}.with_fg(colors::strike_fg()).with_dim(),
            }};
        }
        auto builder = detail::vstack()
            .align_self(Align::Stretch)
            .border(BorderStyle::Round)
            .border_color(colors::code_border())
            .padding(0, 1, 0, 1);
        if (!lang.empty()) {
            std::string label = " " + lang + " ";
            if (lang == "mermaid" || lang == "matrix" ||
                lang == "math"    || lang == "latex"  ||
                lang == "dot"     || lang == "graphviz")
            {
                label = " " + lang + " (diagram) ";
            }
            builder = std::move(builder).border_text(
                std::move(label),
                BorderTextPos::Top,
                BorderTextAlign::Start);
        }
        return builder(highlight_code(code, lang));
    }

    // ── ATX heading at tail start: render the first line in its
    //    committed shape so the heading style appears as soon as `# ` is
    //    typed instead of waiting for the next blank line to commit.
    //
    //    HEIGHT-CONTINUITY (the composer-jump fix). The canonical committed
    //    render (md_block_to_element, render_block.cpp) gives h1/h2 a
    //    leading ▍ accent AND a full-width ═/─ underline RULE ROW below the
    //    title — so a committed h1/h2 is TWO rows tall. If the live tail
    //    rendered the heading as a bare one-row TextElement (as it used to),
    //    the total turn height jumped +1 the instant the heading committed,
    //    shoving the composer/status bar up by a row — the reported
    //    "headings move the composer" artefact. Fix: render a RECOGNISED
    //    h1/h2 through the SAME md_block_to_element pipeline commit_range
    //    uses, so the accent + rule are present from `# ` onward and the
    //    height is 2 rows before AND after commit — byte-identical, zero
    //    snap. h3–h6 have no rule row (one row canonical), so their bare
    //    text render already matches; they keep the cheap inline path.
    //    Monotonic: the title line grows char-by-char (multi-line wrap only
    //    ADDS rows); the rule row is reserved from the start.
    int hashes = 0;
    while (hashes < 6 && static_cast<size_t>(hashes) < body.size() &&
           body[hashes] == '#') ++hashes;
    if (hashes > 0 && static_cast<size_t>(hashes) < body.size() &&
        body[hashes] == ' ')
    {
        auto eol = body.find('\n');
        std::string_view first = (eol == std::string_view::npos)
                                ? body
                                : body.substr(0, eol);
        std::string_view rest  = (eol == std::string_view::npos)
                                ? std::string_view{}
                                : body.substr(eol + 1);
        std::string_view text  = first.substr(static_cast<size_t>(hashes) + 1);

        // Strip a trailing CR the wire may carry so the synthesized span
        // matches what parse_markdown_impl would store.
        while (!text.empty() && (text.back() == '\r')) text.remove_suffix(1);

        // Build the heading Element the SAME way commit does: a real
        // md::Heading fed to md_block_to_element. This carries the ▍ accent
        // (h1/h2) + underline rule (h1/h2) so the live render is byte- and
        // row-identical to the committed block — no height snap at the
        // commit seam. Inline-parse the title exactly as the engine does on
        // commit (so `**bold**` etc. in a heading render identically).
        // Reserve the title row with a single space when the text is still
        // empty (`# ` typed, no glyphs yet) so the row never collapses to
        // zero and flickers.
        md::Heading hd;
        hd.level = hashes;
        if (text.empty()) {
            hd.spans.push_back(md::Inline{md::Text{" "}});
        } else {
            hd.spans = parse_inlines(text);
            if (hd.spans.empty())
                hd.spans.push_back(md::Inline{md::Text{" "}});
        }
        Element heading_el = md_block_to_element(md::Block{std::move(hd)});

        while (!rest.empty() && rest.front() == '\n') rest.remove_prefix(1);
        if (rest.empty()) return heading_el;
        auto spans = parse_inlines(rest);
        return detail::vstack().gap(1)(
            std::move(heading_el),
            build_inline_row(spans)
        );
    }

    // ── Thematic break (---, ***, ___) at tail start: render the
    //    committed HRule the instant the line terminates. The line must
    //    be `\n`-terminated (a complete rule) so height is fixed at one
    //    row and can't snap. At tail start there's no open paragraph in
    //    this slice, so a `-` line is unambiguously an HR, not a setext
    //    underline. Any content after the rule's newline falls through
    //    to the inline parse below.
    {
        auto eol = body.find('\n');
        if (eol != std::string_view::npos) {
            std::string_view rule_line = body.substr(0, eol);
            auto strip_ws = [](std::string_view s) {
                while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
                    s.remove_prefix(1);
                while (!s.empty() && (s.back() == ' ' || s.back() == '\t'
                                      || s.back() == '\r'))
                    s.remove_suffix(1);
                return s;
            };
            std::string_view t = strip_ws(rule_line);
            bool is_hr = !t.empty();
            if (is_hr) {
                char c = t[0];
                if (c != '-' && c != '_' && c != '*') is_hr = false;
                else {
                    int count = 0;
                    for (char ch : t) {
                        if (ch == c) ++count;
                        else if (ch == ' ' || ch == '\t') continue;
                        else { is_hr = false; break; }
                    }
                    if (count < 3) is_hr = false;
                }
            }
            if (is_hr) {
                Element rule = md_block_to_element(md::Block{md::HRule{}});
                std::string_view rest = body.substr(eol + 1);
                while (!rest.empty() && rest.front() == '\n')
                    rest.remove_prefix(1);
                if (rest.empty()) return rule;
                auto spans = parse_inlines(rest);
                return detail::vstack().gap(1)(
                    std::move(rule),
                    build_inline_row(spans)
                );
            }
        }
    }

    // Inline-only parse for the rest — block markers (|, -, *, >) stay
    // literal until a real boundary advances past them.  This is the
    // monotonicity floor: a paragraph can only GROW row count as bytes
    // arrive, never shrink, so the renderer never has to clear stale
    // rows and ghosting is structurally impossible.  Lists / tables /
    // blockquotes pay a one-frame "snap" when the trailing blank line
    // commits the whole thing as one cohesive block.
    //
    // ── Eager list / blockquote rendering for completed rows ──
    //
    // Exception to the rule above: when the tail STARTS with a list
    // marker (`- `, `* `, `+ `, `\d+. `, `\d+) `) or a blockquote
    // marker (`> `), we can render each FULLY-TERMINATED row in its
    // committed shape immediately. Long lists are the worst offender
    // for the perceived "wait for the page" lag: a 12-item bullet list
    // stays in classify_blank_line's intra-blank limbo for the entire
    // duration of the stream, rendered as raw `* item` text in body
    // colour, and snaps to a styled list only when the model emits
    // its first non-list line. Same for blockquotes — `> ` markers
    // sit literal until end-of-quote.
    //
    // Monotonicity argument: each fully-terminated row has a height
    // determined by its bytes alone (text wrap inside its column).
    // The LIVE row (last line, no trailing `\n`) stays literal so
    // further bytes can extend it without snapping shape. Once a `\n`
    // lands on the live row, it joins the rendered rows above on the
    // next frame — a one-row monotonic add, identical in cost to
    // committing one extra line of an inline paragraph.
    //
    // We feed the rows through parse_markdown_impl so the rendered
    // Element is byte-identical to what commit_range would eventually
    // produce — no snap when commit finally fires. The live row falls
    // through to the inline-only path below as a separate Element
    // sitting under the rendered list.
    auto is_list_row_start = [](std::string_view line) noexcept -> bool {
        return ul_marker_len(line) > 0 || ol_marker_len(line) > 0;
    };
    auto is_quote_row_start = [](std::string_view line) noexcept -> bool {
        // Skip up to 3 leading spaces (GFM tolerance), then `> ` or `>`
        // at end-of-line.
        std::size_t k = 0;
        while (k < 4 && k < line.size() && line[k] == ' ') ++k;
        if (k >= line.size() || line[k] != '>') return false;
        // Bare `>` or `> ...` both qualify.
        return k + 1 == line.size() || line[k + 1] == ' ' || line[k + 1] == '\t';
    };

    // Probe the first line of the tail to decide whether to engage the
    // eager renderer. We deliberately don't engage on a tail whose
    // first line is plain prose followed by list rows — the prose
    // already prints inline and treating mid-buffer list rows as a
    // separate list would split the tail into two visually distinct
    // sections that the eventual commit would re-merge differently.
    auto first_line_end = body.find('\n');
    std::string_view first_line = (first_line_end == std::string_view::npos)
        ? body
        : body.substr(0, first_line_end);
    const bool tail_is_list  = is_list_row_start(first_line);
    const bool tail_is_quote = !tail_is_list && is_quote_row_start(first_line);

    // ── Live-first-row eager render ───────────────────────────────────
    //
    // When the tail's first line is a list/quote marker that has NOT yet
    // received its terminating '\n', the terminated-row walk below finds
    // nothing to commit and we historically fell through to the inline
    // path — which paints the raw marker bytes (`- `, `1. `, `> `) in
    // body colour until the newline lands, then SNAPS into a styled
    // block. That snap is the "rows start unformatted then jump into the
    // block" artefact.
    //
    // Fix: mirror the heading / code-fence eager paths — synthesize a
    // terminated single-row slice and render it in its committed shape
    // right now. render_eager_slice feeds it through the same
    // parse_markdown_impl + md_block_to_element pipeline commit_range
    // uses, so the Element is byte-identical to what eventually commits:
    // no second snap when the '\n' finally arrives.
    //
    // Monotonicity: a single list item / quote line occupies a height
    // determined by its own bytes (wrap inside its column). As bytes
    // extend the live row the item can only grow taller; once the '\n'
    // lands the row joins the terminated-rows walk above unchanged.
    if ((tail_is_list || tail_is_quote)
        && first_line_end == std::string_view::npos
        && !first_line.empty())
    {
        std::string synth;
        synth.reserve(first_line.size() + 1);
        synth.assign(first_line);
        // Item content that is itself a half-typed marker — "- *" en
        // route to "- **bold**…". Parsed as-is it reads as a NESTED
        // empty list (2 rows) until the next byte disambiguates to
        // literal content (1 row) — a guaranteed shrink. Truncate to the
        // bare marker while ambiguous (renders as a 1-row empty item);
        // the disambiguating byte either keeps the row ("**") or grows
        // it ("* x" nested item). Same rule as the canonical path.
        if (tail_is_list) {
            int ml = ul_marker_len(synth);
            if (ml == 0) ml = ol_marker_len(synth);
            if (ml > 0 && static_cast<std::size_t>(ml) < synth.size()
                && half_list_marker(std::string_view{synth}.substr(
                       static_cast<std::size_t>(ml))))
                synth.resize(static_cast<std::size_t>(ml));
        }
        synth.push_back('\n');
        std::vector<Element> kids;
        kids.reserve(1);
        render_eager_slice(synth, kids);
        if (kids.size() == 1) return std::move(kids.front());
        if (!kids.empty())
            return detail::vstack().gap(1)(std::move(kids)).build();
        // render_eager_slice produced nothing (defensive) — fall through.
    }

    if ((tail_is_list || tail_is_quote) && first_line_end != std::string_view::npos) {
        // Walk lines forward, greedily consuming everything that still
        // belongs to the same block (list rows + their continuations,
        // or `> `-prefixed rows). Stop at the first line that breaks
        // the pattern OR at the live row (the trailing partial line
        // with no `\n`).
        std::size_t cursor = 0;
        std::size_t last_committed_end = 0;   // end of last fully-terminated in-block row
        while (cursor < body.size()) {
            auto eol = body.find('\n', cursor);
            if (eol == std::string_view::npos) {
                // Live row — leave for the inline fallback.
                break;
            }
            std::string_view line = body.substr(cursor, eol - cursor);
            bool in_block;
            if (tail_is_list) {
                // A list "row" is either a new item marker OR a
                // continuation (indented or blank, where the blank's
                // intra-list semantics already kept us cohesive).
                if (is_list_row_start(line)) {
                    in_block = true;
                } else if (line.empty()) {
                    // Blank inside the list (loose-list separator).
                    // Keep going — classify_blank_line's verdict at
                    // commit time will decide whether this terminates;
                    // for rendering we just need bytes that
                    // parse_markdown_impl will recognise.
                    in_block = true;
                } else if (count_indent(line) >= 2) {
                    // Indented continuation of the previous item.
                    in_block = true;
                } else {
                    in_block = false;
                }
            } else {
                // Blockquote: lines are `> ...` or `>` (lazy
                // continuations are CommonMark legal but we conservatively
                // require the marker to keep the eager path simple).
                in_block = is_quote_row_start(line);
            }
            if (!in_block) break;
            cursor = eol + 1;
            last_committed_end = cursor;
        }

        if (last_committed_end > 0) {
            // Parse the proven-complete rows as a real markdown
            // document (cached on the slice bytes — re-emits the block
            // Elements without re-parsing when the slice is unchanged).
            std::string_view rendered_slice = body.substr(0, last_committed_end);
            std::vector<Element> kids;
            kids.reserve(2);
            render_eager_slice(rendered_slice, kids);

            // Live row (post-last-`\n` partial) renders as inline text,
            // same monotonicity rules as the paragraph fallback below.
            // We don't fold it into the parsed list because doing so
            // would change the parsed list's shape on the next frame
            // when the row terminates — the snap we're trying to
            // avoid.
            std::string_view live_row = body.substr(last_committed_end);
            // Trim leading blank lines so a separator between the
            // rendered list and the live row doesn't paint a stray
            // empty row.
            while (!live_row.empty() && live_row.front() == '\n')
                live_row.remove_prefix(1);
            if (!live_row.empty()) {
                auto live_spans = parse_inlines(live_row);
                std::string content;
                std::vector<StyledRun> runs;
                const Style base = Style{}.with_fg(colors::text());
                for (const auto& s : live_spans) {
                    flatten_inline(s, base, content, runs);
                }
                Element live_el;
                if (runs.empty()) {
                    live_el = Element{TextElement{}};
                } else if (runs.size() == 1) {
                    live_el = Element{TextElement{
                        .content = std::move(content),
                        .style   = runs[0].style,
                    }};
                } else {
                    live_el = Element{TextElement{
                        .content = std::move(content),
                        .style   = Style{}.with_fg(colors::text()),
                        .runs    = std::move(runs),
                    }};
                }
                kids.push_back(std::move(live_el));
            }

            if (kids.size() == 1) return std::move(kids.front());
            return detail::vstack().gap(1)(std::move(kids)).build();
        }
        // Fell through (no fully-terminated rows yet) — the inline
        // fallback below renders the partial markers as literal text,
        // same as the historical behaviour. Once the first row gets
        // its `\n` we'll take the eager path on the next frame.
    }

    // ── Eager table rendering ─────────────────────────────────────────
    //
    // Tables are the worst offender for "wait for the page to format".
    // Without this branch the user sees raw `|` / `---` text from the
    // moment the model emits the header until either (a) a blank line
    // lands below the table (the boundary scanner commits) or (b) a
    // non-`|` line lands below (find_table_end commits). On a long
    // table that's many seconds of unstyled pipes scrolling past.
    //
    // Eager rule:
    //   * tail starts with `|` (after ≤3 spaces tolerance — GFM)
    //   * the header line is fully terminated by `\n`
    //   * the delimiter line is fully terminated AND well-formed
    //     (only `|`, `-`, `:`, space, `\r`, with at least one `-`)
    //
    // When all three hold we feed [header, delim, body-rows…] through
    // parse_markdown_impl exactly the way commit_range would, then
    // render via md_block_to_element. Any partial trailing row (no
    // closing `\n`) is held back and rendered as inline literal text
    // BELOW the formatted table, same monotonicity trick the eager
    // list path uses.
    //
    // Monotonicity: each fully-terminated body row contributes a
    // fixed number of visual rows to the rendered table (column-
    // wrapped). New `\n`-terminated rows can only ADD rows. The live
    // partial row stays inline below until its `\n` lands, at which
    // point it folds into the table on the next frame (one-row add,
    // identical Element identity for the rows already shown — the
    // table's ComponentElement keys on `data` shared_ptr identity,
    // not on this temporary fresh build, so the inner cells cache
    // misses cleanly on the new row count without ghosting).
    if (body.size() >= 1 && first_line_end != std::string_view::npos) {
        auto first_non_space_off = [](std::string_view s) -> std::size_t {
            std::size_t k = 0;
            while (k < 4 && k < s.size() && s[k] == ' ') ++k;
            return k;
        };
        std::size_t hoff = first_non_space_off(first_line);
        const bool header_pipe = hoff < first_line.size()
                                 && first_line[hoff] == '|';
        if (header_pipe) {
            // Need a fully-terminated delimiter line.
            std::size_t delim_start = first_line_end + 1;
            std::size_t delim_end = body.find('\n', delim_start);
            if (delim_end != std::string_view::npos) {
                std::string_view delim_line =
                    body.substr(delim_start, delim_end - delim_start);
                std::size_t doff = first_non_space_off(delim_line);
                bool delim_pipe = doff < delim_line.size()
                                  && delim_line[doff] == '|';
                bool delim_has_dash = false;
                bool delim_ok = delim_pipe;
                if (delim_ok) {
                    for (std::size_t k = doff;
                         k < delim_line.size(); ++k) {
                        char c = delim_line[k];
                        if (c == '-') { delim_has_dash = true; continue; }
                        if (c == '|' || c == ':' || c == ' '
                            || c == '\t' || c == '\r') continue;
                        delim_ok = false; break;
                    }
                }
                if (delim_ok && delim_has_dash) {
                    // Walk body rows: every fully-terminated `|`-prefixed
                    // line after the delimiter. Stop at the first non-
                    // `|` line OR at the live (unterminated) row.
                    std::size_t cursor = delim_end + 1;
                    std::size_t last_row_end = cursor; // end of header+delim+complete rows
                    while (cursor < body.size()) {
                        std::size_t eol = body.find('\n', cursor);
                        if (eol == std::string_view::npos) break;
                        std::string_view line =
                            body.substr(cursor, eol - cursor);
                        std::size_t off = first_non_space_off(line);
                        if (off >= line.size() || line[off] != '|')
                            break;
                        cursor = eol + 1;
                        last_row_end = cursor;
                    }

                    // Parse the proven-complete slice as a real markdown
                    // document — yields exactly one md::Table block
                    // matching what commit_range would eventually emit.
                    // Cached on the slice bytes so an unchanged table
                    // re-emits without re-parsing every frame.
                    std::string_view table_slice =
                        body.substr(0, last_row_end);
                    std::vector<Element> kids;
                    kids.reserve(2);
                    render_eager_slice(table_slice, kids);

                    // Live trailing row (partial, no `\n`). Render as
                    // inline literal text so further bytes extend it
                    // without snapping the table's shape.
                    std::string_view live_row =
                        body.substr(last_row_end);
                    while (!live_row.empty() && live_row.front() == '\n')
                        live_row.remove_prefix(1);
                    if (!live_row.empty()) {
                        auto live_spans = parse_inlines(live_row);
                        std::string content;
                        std::vector<StyledRun> runs;
                        const Style base =
                            Style{}.with_fg(colors::text());
                        for (const auto& s : live_spans) {
                            flatten_inline(s, base, content, runs);
                        }
                        Element live_el;
                        if (runs.empty()) {
                            live_el = Element{TextElement{}};
                        } else if (runs.size() == 1) {
                            live_el = Element{TextElement{
                                .content = std::move(content),
                                .style   = runs[0].style,
                            }};
                        } else {
                            live_el = Element{TextElement{
                                .content = std::move(content),
                                .style   = Style{}.with_fg(colors::text()),
                                .runs    = std::move(runs),
                            }};
                        }
                        kids.push_back(std::move(live_el));
                    }

                    if (kids.size() == 1) return std::move(kids.front());
                    return detail::vstack().gap(1)(std::move(kids)).build();
                }
            }
        }
        // Header+delim not both ready yet → fall through to inline
        // path; the literal `|` text stays visible until the next
        // frame lands more bytes and we can try the eager path again.
    }

    // ── Sliding cache: parse only the live line ──
    //
    // Split body at the last '\n'. The "stable prefix" (everything
    // through and including the last '\n') is cache-keyed by its bytes;
    // the "live line" (post-newline tail) is re-parsed every frame.
    // Markdown emphasis / code-span / link delimiters never cross
    // newline boundaries in well-formed input — when the model emits
    // `**bold**\nNext line`, the parser state at the '\n' is "fresh,
    // no open delimiters" — so the split is parser-state-safe.
    //
    // Cost transition: prior code re-parsed the full body (O(tail)
    // per frame). Now per-frame work is O(live_line_length) on the
    // common case, with an O(tail) refresh once per newline boundary
    // (rare: bytes arrive faster than line breaks during streaming).
    auto last_nl = body.rfind('\n');
    std::string_view stable_prefix = (last_nl == std::string_view::npos)
        ? std::string_view{}
        : body.substr(0, last_nl + 1);
    std::string_view live_line = (last_nl == std::string_view::npos)
        ? body
        : body.substr(last_nl + 1);

    // Refresh the prefix cache only if it differs from what's stored.
    // Fast path: source_version_ matches what populated the cache —
    // bytes are guaranteed unchanged, no hash needed.  Slow path:
    // version moved, fall back to the (hash, length) key to catch the
    // case where commit_range shifted the tail base but the stable-
    // prefix slice happened to round-trip to the same bytes.
    if (tail_inline_cache_version_ != source_version_) {
        const std::uint64_t prefix_hash = fnv1a64(stable_prefix);
        if (tail_inline_cache_prefix_len_  != stable_prefix.size()
            || tail_inline_cache_prefix_hash_ != prefix_hash) {
            tail_inline_cache_content_.clear();
            tail_inline_cache_runs_.clear();
            if (!stable_prefix.empty()) {
                auto prefix_spans = parse_inlines(stable_prefix);
                const Style base = Style{}.with_fg(colors::text());
                for (const auto& s : prefix_spans) {
                    flatten_inline(s, base, tail_inline_cache_content_,
                                   tail_inline_cache_runs_);
                }
            }
            tail_inline_cache_prefix_len_  = stable_prefix.size();
            tail_inline_cache_prefix_hash_ = prefix_hash;
        }
        tail_inline_cache_version_ = source_version_;
    }

    // Build the result: cached prefix runs + freshly-parsed live-line
    // runs. flatten_inline writes run offsets relative to `out.size()`
    // at write time, so seeding `content` / `runs` from the cache
    // keeps the appended-line offsets globally correct.
    std::string content                  = tail_inline_cache_content_;
    std::vector<StyledRun> runs          = tail_inline_cache_runs_;
    if (!live_line.empty()) {
        auto live_spans = parse_inlines(live_line);
        const Style base = Style{}.with_fg(colors::text());
        for (const auto& s : live_spans) {
            flatten_inline(s, base, content, runs);
        }
    }

    if (runs.empty()) return Element{TextElement{}};
    if (runs.size() == 1) {
        return Element{TextElement{
            .content = std::move(content),
            .style   = runs[0].style,
        }};
    }
    return Element{TextElement{
        .content = std::move(content),
        .style   = Style{}.with_fg(colors::text()),
        .runs    = std::move(runs),
    }};
}

} // namespace maya
