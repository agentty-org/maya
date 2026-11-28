// render_block.cpp — md_block_to_element: block AST → Element.
//
// One std::visit over every md::Block variant: paragraphs, headings, code
// blocks (via highlight_code), blockquotes, lists, rules, tables (incl. the
// inline column-layout machinery), footnotes, alerts, definition lists,
// <details>, and raw HTML. Calls flatten_inline / build_inline_row /
// render_list (render_inline.cpp) and highlight_code (syntax.cpp) via
// maya::md_detail.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "maya/core/overload.hpp"
#include "maya/element/builder.hpp"
#include "maya/style/border.hpp"
#include "maya/style/style.hpp"
#include "maya/style/theme.hpp"
#include "maya/widget/agentty/html.hpp"
#include "maya/widget/agentty/markdown.hpp"
#include "maya/widget/agentty/markdown/internal.hpp"
#include "maya/widget/agentty/markdown/tex_math_render.hpp"

namespace maya {

using ::maya::md_detail::highlight_code;
using ::maya::md_detail::flatten_inlines;
using ::maya::md_detail::build_inline_row;
using ::maya::md_detail::render_list;

Element md_block_to_element(const md::Block& block) {
    return std::visit(overload{
        [](const md::Paragraph& p) -> Element {
            return build_inline_row(p.spans);
        },
        [](const md::Heading& h) -> Element {
            Style sty = Style{}.with_bold();
            switch (h.level) {
                case 1: sty = sty.with_fg(colors::heading1()); break;
                case 2: sty = sty.with_fg(colors::heading2()); break;
                case 3: sty = sty.with_fg(colors::heading3()); break;
                default: sty = sty.with_fg(colors::heading3()).with_dim(); break;
            }

            // h4 / h5 / h6 carry no underline rule and reuse the h3
            // foreground at half luminance — visually they collapse
            // into a single bold-grey blob in long docs. Stamp a
            // small marker glyph in front of the text so the level is
            // legible at a glance:
            //
            //   h4 → §  (section sign — "subsection")
            //   h5 → ›  (single guillemet — "deeper")
            //   h6 → ‣  (triangular bullet — "deepest")
            //
            // The marker is rendered in `list_bullet` so it reads as
            // structural punctuation, not part of the heading body.
            std::string_view marker;
            switch (h.level) {
                case 4: marker = "\xc2\xa7 ";       break;   // §
                case 5: marker = "\xe2\x80\xba ";   break;   // ›
                case 6: marker = "\xe2\x80\xa3 ";   break;   // ‣
                default: break;
            }

            std::string content;
            std::vector<StyledRun> runs;

            // h1 / h2 get a leading three-quarter-block (▍, U+258D)
            // in the heading color: a vertical accent bar fused to the
            // title. Reads as a section anchor without consuming a
            // separate row — the underline rule below still owns the
            // row-spend. h3 stays clean (already bold + heading_blue),
            // h4–h6 keep their existing marker glyph (§ › ‣). The
            // block is in heading_fg + bold so it tracks whichever
            // level is active, not a fixed accent.
            if (h.level == 1 || h.level == 2) {
                static constexpr std::string_view kAccent =
                    "\xe2\x96\x8d "; // ▍ + space
                Style accent_sty = sty;       // same fg + bold
                runs.push_back({0, kAccent.size(), accent_sty});
                content.append(kAccent);
            }

            if (!marker.empty()) {
                runs.push_back({content.size(), marker.size(),
                                Style{}.with_fg(colors::list_bullet()).with_bold()});
                content.append(marker);
            }
            flatten_inlines(h.spans, sty, content, runs);
            Element heading_text = Element{TextElement{
                .content = std::move(content),
                .style = sty,
                .runs = std::move(runs),
            }};

            // Underline rule for # / ## only — gives the heading
            // typographic weight without compromising monotonicity:
            // headings commit atomically, so the +1 row arrives in
            // a single snap and never reflows. h3+ stay
            // single-line because they're already visually distinct
            // via the bold + heading_dim treatment, and adding a
            // rule under every h3 in a tutorial-style doc would be
            // visual noise.
            //
            // Glyph choice: U+2550 (═) for h1 — heaviest, matches
            // the "section break" feel; U+2500 (─) for h2 —
            // lighter, matches the "subsection" feel. Both rendered
            // in heading_rule (= bright_black) so they sit visually
            // *under* the heading text without competing with it.
            if (h.level == 1 || h.level == 2) {
                const char* rule_glyph =
                    (h.level == 1) ? "\xe2\x95\x90"   // ═ U+2550
                                   : "\xe2\x94\x80"; // ─ U+2500
                const Style rule_style =
                    Style{}.with_fg(colors::heading_rule()).with_dim();
                Element rule = Element{ComponentElement{
                    .render = [rule_glyph, rule_style]
                              (int w, int /*h*/) -> Element {
                        if (w <= 0) return Element{TextElement{}};
                        std::string line;
                        line.reserve(static_cast<std::size_t>(w) * 3);
                        for (int i = 0; i < w; ++i) line.append(rule_glyph);
                        return Element{TextElement{
                            .content = std::move(line),
                            .style   = rule_style,
                        }};
                    },
                }};
                return detail::vstack()(
                    std::move(heading_text), std::move(rule)
                ).build();
            }
            return heading_text;
        },
        [](const md::CodeBlock& c) -> Element {
            // Empty fence: render a minimal one-row dim placeholder instead
            // of a multi-row bordered box. A bordered empty rectangle
            // reads as a parsing artifact (stray pair of fences with
            // nothing between); a single dim row reads as "intentionally
            // minimal" while still keeping the rendered tree honest about
            // the AST (the empty CodeBlock IS present — spec requires
            // <pre><code></code></pre> for HTML conformance).
            //
            // Height is constant 1 row (with or without the language
            // tag), matching render_tail_inner's open-fence empty-body
            // path — so the live tail and the eventual committed render
            // agree on height (no shrink at the commit seam).
            if (c.content.empty()) {
                std::string label = c.lang.empty()
                    ? std::string{"\xe2\x97\x8b empty code block"}     // ○
                    : std::string{"\xe2\x97\x8b "} + c.lang + " (empty)";
                return Element{TextElement{
                    .content = std::move(label),
                    .style   = Style{}.with_fg(colors::strike_fg()).with_dim(),
                }};
            }

            // Display math: `$$…$$` and ```math / ```latex fences typeset via
            // the terminal TeX engine into a real 2-D box (fractions, roots,
            // matrices, big operators with limits above/below). Wrapped in the
            // same round border as code so it reads as a distinct block, with
            // a subtle " math " tag. This is the DISPLAY path — inline `$…$`
            // is linearised separately in render_inline.cpp.
            if (c.lang == "math" || c.lang == "latex") {
                texmath::MathPalette pal;
                auto body = Style{}.with_fg(colors::text());
                pal.normal = body;
                pal.op     = body.with_bold();
                pal.num    = body;
                pal.rule   = Style{}.with_fg(colors::code_border());
                pal.delim  = Style{}.with_fg(colors::link_fg());
                pal.text   = body;
                Element formula =
                    texmath::render_math(c.content, pal, /*display=*/true);
                return detail::vstack()
                    .align_self(Align::Stretch)
                    .border(BorderStyle::Round)
                    .border_color(colors::code_border())
                    .padding(0, 1, 0, 1)
                    .border_text(" math ", BorderTextPos::Top,
                                 BorderTextAlign::Start)(
                        detail::center()(std::move(formula)));
            }

            // Zed style: round border, subtle bg, language label top-left.
            // align_self(Stretch) anchors the right border at the parent's
            // available width instead of the code's natural width.
            // Without it, the border tracks the longest line — which
            // changes during streaming as new lines arrive — so the
            // right border drifts column-to-column between frames
            // (visible later as phantom borders left in scrollback) and
            // the panel reads as "not responsive" once the message
            // settles.
            auto builder = detail::vstack()
                .align_self(Align::Stretch)
                .border(BorderStyle::Round)
                .border_color(colors::code_border())
                .padding(0, 1, 0, 1)
                // NoWrap inside (syntax.cpp) means long lines extend
                // past the available width; clip them at the right
                // border instead of letting the terminal hard-wrap
                // continuation bytes into column 0 of the next row
                // (which lands them under — or through — the line-
                // number gutter).
                .overflow(Overflow::Hidden);

            if (!c.lang.empty()) {
                std::string label = " " + c.lang + " ";
                // Diagram fences we can't layout: flag them in the label so
                // users know the content is a textual fallback, not the
                // intended rendering. (math/latex are handled above via the
                // real typesetter and never reach here.)
                if (c.lang == "mermaid" || c.lang == "matrix" ||
                    c.lang == "dot"     || c.lang == "graphviz") {
                    label = " " + c.lang + " (diagram) ";
                }
                builder = std::move(builder).border_text(
                    std::move(label),
                    BorderTextPos::Top,
                    BorderTextAlign::Start);
            }

            return builder(highlight_code(c.content, c.lang));
        },
        [](const md::Blockquote& bq) -> Element {
            // Zed style: thin │ gutter, muted italic text
            std::vector<Element> rows;
            rows.reserve(bq.children.size());
            auto bar_style = Style{}.with_fg(colors::quote_bar());
            auto text_style = Style{}.with_italic().with_fg(colors::quote_text());

            for (auto& child : bq.children) {
                auto child_elem = md_block_to_element(child);
                // Extract text and prefix each visual line with "│ "
                rows.push_back(detail::hstack()(
                    Element{TextElement{
                        .content = "\xe2\x94\x82 ",  // "│ "
                        .style = bar_style,
                    }},
                    std::move(child_elem) | text_style
                ));
            }

            return detail::vstack()(std::move(rows));
        },
        [](const md::List& l) -> Element {
            return render_list(l, 0);
        },
        [](const md::HRule&) -> Element {
            // Zed style: thin, subtle separator
            return Element{ComponentElement{
                .render = [](int w, int /*h*/) -> Element {
                    std::string rule;
                    rule.reserve(static_cast<size_t>(w) * 3);
                    for (int k = 0; k < w; ++k) rule += "\xe2\x94\x80"; // ─
                    return Element{TextElement{
                        .content = std::move(rule),
                        .style = Style{}.with_fg(colors::hrule_fg()),
                    }};
                },
                .layout = {},
            }};
        },
        [](const md::Table& tbl) -> Element {
            int ncols = static_cast<int>(tbl.header.cells.size());
            if (ncols == 0) return Element{TextElement{}};

            // ── Pre-flatten every cell's inline content into (content, runs).
            // This is the parse-time work: the inline spans are converted
            // once into a flat string + StyledRun list which the render-
            // time wrapper can then slice into wrapped lines based on the
            // ACTUAL available width. Keeps the runtime path cheap (just
            // wrap + pad) instead of re-walking the inline AST every frame.
            struct FlatCell { std::string content; std::vector<StyledRun> runs; };

            std::vector<FlatCell> header_flat;
            header_flat.reserve(static_cast<size_t>(ncols));
            auto header_base = Style{}.with_bold().with_fg(colors::table_header());
            for (int c = 0; c < ncols; ++c) {
                FlatCell f;
                flatten_inlines(tbl.header.cells[static_cast<size_t>(c)].spans,
                                header_base, f.content, f.runs);
                header_flat.push_back(std::move(f));
            }

            std::vector<std::vector<FlatCell>> rows_flat;
            rows_flat.reserve(tbl.rows.size());
            auto cell_base = Style{}.with_fg(colors::text());
            for (auto& row : tbl.rows) {
                std::vector<FlatCell> rf;
                rf.reserve(static_cast<size_t>(ncols));
                for (int c = 0; c < ncols; ++c) {
                    FlatCell f;
                    if (static_cast<size_t>(c) < row.cells.size()) {
                        flatten_inlines(row.cells[static_cast<size_t>(c)].spans,
                                        cell_base, f.content, f.runs);
                    }
                    rf.push_back(std::move(f));
                }
                rows_flat.push_back(std::move(rf));
            }

            // ── Ideal width per column (longest cell content). Used to
            // decide the layout when the terminal is wide enough; shrunk
            // proportionally when not. No cap — we let the render-time
            // distributor handle it.
            std::vector<int> ideal(static_cast<size_t>(ncols), 0);
            for (int c = 0; c < ncols; ++c) {
                ideal[static_cast<size_t>(c)] =
                    std::max(1, string_width(header_flat[static_cast<size_t>(c)].content));
                for (auto& r : rows_flat) {
                    ideal[static_cast<size_t>(c)] = std::max(
                        ideal[static_cast<size_t>(c)],
                        string_width(r[static_cast<size_t>(c)].content));
                }
            }

            // ── Streaming width floor ────────────────────────────────────
            // The live StreamingMarkdown fills tbl.min_col_widths from the
            // columns' widths over EVERY already-arrived row (even ones the
            // reveal cursor hasn't exposed). Folding it in here keeps the
            // visible table at its final column widths from the first
            // revealed row, so it no longer reflows horizontally as wider
            // cells stream in. Empty for static / committed tables (the
            // arrived set == the rendered set, so the natural ideal already
            // equals the floor): a pure no-op there, layout unchanged.
            for (int c = 0; c < ncols
                     && static_cast<size_t>(c) < tbl.min_col_widths.size(); ++c)
                ideal[static_cast<size_t>(c)] = std::max(
                    ideal[static_cast<size_t>(c)],
                    tbl.min_col_widths[static_cast<size_t>(c)]);

            // ── Shared helpers for layout + render ───────────────────────
            // Both `measure` (called during build_layout_tree) and
            // `render` (called during paint) need to know how the table
            // lays out at a given width. The `measure` path needs only
            // the row count so the layout engine allocates the right
            // amount of vertical space; the `render` path needs the full
            // wrapped Elements. Both call distribute_cols + wrap_cell —
            // identical math, so the row count from measure exactly
            // matches what render produces.
            //
            // The table data lives in a shared_ptr so both the `measure`
            // and `render` closures can capture it by value (without
            // double-moving). Capturing by reference would leave both
            // callbacks holding dangling refs to function-local state
            // — segfault on the first paint after this function returns.

            auto distribute_cols = [](int avail_w, int ncols_,
                                       const std::vector<int>& ideal_)
                                       -> std::vector<int> {
                constexpr int pad = 1;
                int chrome = 1 + ncols_ + 2 * ncols_ * pad;
                int avail_cells = std::max(ncols_, avail_w - chrome);
                // Floor every column at kMinColPref so cell content
                // stays legible at typical widths. But when the
                // terminal itself can't accommodate even that floor
                // (very narrow window, narrow side panel), shrink the
                // floor toward 1 so the produced table is provably
                // ≤ avail_w — otherwise the renderer emits rows wider
                // than the viewport and the TERMINAL inserts its own
                // hard wrap at column 0, splitting the `│` borders
                // across visual lines (the symptom on screenshots:
                // continuation text like "paginated" landing in
                // column 0 with no leading separator).
                constexpr int kMinColPref = 6;
                int kMinCol = kMinColPref;
                if (kMinCol * ncols_ > avail_cells) {
                    kMinCol = std::max(1, avail_cells / ncols_);
                }
                int ideal_sum = 0;
                for (int v : ideal_) ideal_sum += v;
                std::vector<int> col_w(static_cast<size_t>(ncols_));
                if (ideal_sum <= avail_cells) {
                    for (int c = 0; c < ncols_; ++c)
                        col_w[static_cast<size_t>(c)] = ideal_[static_cast<size_t>(c)];
                } else {
                    int min_total = kMinCol * ncols_;
                    if (avail_cells <= min_total) {
                        for (int c = 0; c < ncols_; ++c)
                            col_w[static_cast<size_t>(c)] = kMinCol;
                    } else {
                        int budget_above_min = avail_cells - min_total;
                        int excess_total = 0;
                        for (int v : ideal_) excess_total += std::max(0, v - kMinCol);
                        for (int c = 0; c < ncols_; ++c) {
                            int excess = std::max(0,
                                ideal_[static_cast<size_t>(c)] - kMinCol);
                            int share = excess_total > 0
                                ? (excess * budget_above_min) / excess_total
                                : budget_above_min / ncols_;
                            col_w[static_cast<size_t>(c)] = kMinCol + share;
                        }
                        int sum = 0;
                        for (int v : col_w) sum += v;
                        int rem = avail_cells - sum;
                        for (int i = 0; i < rem; ++i) {
                            int best = 0;
                            for (int c = 1; c < ncols_; ++c)
                                if (ideal_[static_cast<size_t>(c)] - col_w[static_cast<size_t>(c)]
                                    > ideal_[static_cast<size_t>(best)] - col_w[static_cast<size_t>(best)])
                                    best = c;
                            ++col_w[static_cast<size_t>(best)];
                        }
                    }
                }
                return col_w;
            };

            // Word-aware wrap with force-break for over-long words. Used
            // by both the row-counter (measure path) and the cell
            // renderer (paint path); identical to keep counts exact.
            auto wrap_cell_lines = [](const std::string& content, int max_w) -> int {
                if (max_w <= 0) max_w = 1;
                if (content.empty()) return 1;
                auto cb = [](unsigned char c) -> int {
                    if (c < 0xC0) return 1;
                    if (c < 0xE0) return 2;
                    if (c < 0xF0) return 3;
                    return 4;
                };
                int lines = 0;
                size_t i = 0;
                int line_w = 0;
                bool any_in_line = false;
                auto end_line = [&]() { ++lines; line_w = 0; any_in_line = false; };
                while (i < content.size()) {
                    if (content[i] == '\n') { end_line(); ++i; continue; }
                    size_t tok_start = i;
                    bool ws = (content[i] == ' ' || content[i] == '\t');
                    int tok_w = 0;
                    while (i < content.size() && content[i] != '\n'
                           && ((content[i] == ' '
                                || content[i] == '\t') == ws)) {
                        int n = cb(static_cast<unsigned char>(content[i]));
                        // Display width, not codepoint count: a wide
                        // (CJK/emoji) glyph occupies 2 cells, matching the
                        // string_width() used for column padding below.
                        // Counting ++tok_w here overran wide-glyph cells.
                        std::size_t dp = i;
                        char32_t cp = decode_utf8(content, dp);
                        tok_w += is_wide_char(cp) ? 2 : 1;
                        i += static_cast<size_t>(n);
                    }
                    if (line_w + tok_w <= max_w) {
                        line_w += tok_w;
                        if (!ws) any_in_line = true;
                        continue;
                    }
                    if (ws) { end_line(); continue; }
                    if (any_in_line) { end_line(); i = tok_start; continue; }
                    size_t pos = tok_start;
                    int forced = 0;
                    while (pos < i && forced < max_w) {
                        int n = cb(static_cast<unsigned char>(content[pos]));
                        std::size_t dp = pos;
                        char32_t cp = decode_utf8(content, dp);
                        int cw = is_wide_char(cp) ? 2 : 1;
                        if (forced + cw > max_w && forced > 0) break;
                        pos += static_cast<size_t>(n);
                        forced += cw;
                    }
                    end_line();
                    i = pos;
                }
                if (any_in_line) ++lines;
                return std::max(1, lines);
            };

            // Bundle the per-table state into a heap-allocated struct so
            // both closures (render + measure) can hold it by shared_ptr
            // and survive past this function's return.
            struct TableData {
                int ncols;
                std::vector<FlatCell> header_flat;
                std::vector<std::vector<FlatCell>> rows_flat;
                std::vector<int> ideal;
                std::vector<md::TableAlign> aligns;
                Style header_base;
                Style cell_base;
                // Width-keyed render cache. Cell content is fixed at
                // parse time; only the wrapping (and therefore the row
                // count) depends on the available width. The closure
                // below is otherwise the most expensive
                // md_block_to_element output — every paint re-wraps
                // every cell and re-builds every line. Memoising on
                // `avail_w` collapses the steady-state cost to a
                // single Element copy: the renderer's component_cache
                // already short-circuits render() entirely on a hit
                // there, so this kicks in on the wider class of cache
                // misses (component_cache eviction, fresh
                // ComponentElement instance after a prefix
                // regeneration that re-materialised this same table
                // block at a new tree slot, …).
                //
                // mutable: the render lambda runs on a const-ref to
                // TableData via the shared_ptr; the cache slot is
                // logically const (same input always yields same
                // output) so the standard memoisation justification
                // for `mutable` applies.
                mutable int     cached_w = -1;
                mutable Element cached_render;
                // Chunk-seam flags (see md::Table). Fixed at construction.
                bool omit_top    = false;
                bool omit_bottom = false;
            };
            // Aligns come from the GFM delimiter row; if for any reason
            // the parser didn't fill them (truncated / legacy callers
            // building md::Table by hand), default every column to
            // Left. CommonMark spec default is Left, and silently
            // mis-aligning is worse than ignoring user intent.
            auto aligns_vec = tbl.aligns;
            if (aligns_vec.size() != static_cast<size_t>(ncols)) {
                aligns_vec.assign(static_cast<size_t>(ncols),
                                  md::TableAlign::Left);
            }
            auto data = std::make_shared<TableData>(TableData{
                ncols,
                std::move(header_flat),
                std::move(rows_flat),
                std::move(ideal),
                std::move(aligns_vec),
                header_base,
                cell_base,
                /*cached_w=*/-1,
                /*cached_render=*/{},
                tbl.omit_top,
                tbl.omit_bottom,
            });

            // Wrap into a ComponentElement so cell wrapping uses the
            // actual rendered width. The `measure` callback reports the
            // exact row count so the parent vstack allocates the right
            // amount of vertical space — without it the layout engine
            // defaults to {max_width, 1}, clipping the entire table to
            // a single row (the original symptom).
            return Element{ComponentElement{
                .render = [data, distribute_cols]
                          (int avail_w, int /*h*/) -> Element {
                    // Width-cache fast path. Returns a copy of the
                    // cached Element — cheap relative to re-running
                    // the wrap pipeline, and unavoidable because the
                    // signature returns by value. The component_cache
                    // ABOVE this catches the more common
                    // (instance, width) hit and skips render()
                    // entirely; this layer is the one that survives
                    // component_cache evictions and prefix-regen
                    // rebuilds (where the table's outer
                    // ComponentElement gets a new address but `data`
                    // — captured via shared_ptr — is preserved).
                    if (avail_w == data->cached_w) return data->cached_render;

                    int ncols = data->ncols;
                    const auto& header_flat = data->header_flat;
                    const auto& rows_flat   = data->rows_flat;
                    const auto& header_base = data->header_base;
                    const auto& cell_base   = data->cell_base;
                    constexpr int pad = 1;
                    auto col_w = distribute_cols(avail_w, ncols, data->ideal);

                    // ── Wrap a (content, runs) cell to a target width.
                    // Returns one entry per visual line; each carries its
                    // own sliced run list so per-character styling
                    // (bold, code, color) survives the wrap intact.
                    // Word-aware with force-break for words longer than
                    // the target width. Hard '\n' in content also breaks.
                    auto wrap_cell = [](const std::string& content,
                                        const std::vector<StyledRun>& runs,
                                        int max_w) -> std::vector<FlatCell> {
                        std::vector<FlatCell> out;
                        if (max_w <= 0) max_w = 1;
                        if (content.empty()) {
                            out.push_back({});
                            return out;
                        }

                        // Helper: bytes per UTF-8 char from leading byte.
                        auto cb = [](unsigned char c) -> int {
                            if (c < 0x80) return 1;
                            if (c < 0xC0) return 1;     // invalid lead, treat as 1
                            if (c < 0xE0) return 2;
                            if (c < 0xF0) return 3;
                            return 4;
                        };

                        // Find ranges [start, end) per output line. Then
                        // build content + runs from those ranges.
                        std::vector<std::pair<size_t, size_t>> lines;
                        size_t i = 0;
                        size_t line_start = 0;
                        int line_w = 0;

                        auto flush_line = [&](size_t end) {
                            // Trim trailing whitespace from the line.
                            size_t e = end;
                            while (e > line_start
                                   && (content[e - 1] == ' '
                                       || content[e - 1] == '\t'))
                                --e;
                            lines.push_back({line_start, e});
                        };

                        while (i < content.size()) {
                            // Hard newline → flush current line, start fresh.
                            if (content[i] == '\n') {
                                flush_line(i);
                                ++i;
                                line_start = i;
                                line_w = 0;
                                continue;
                            }
                            // Read next token (run of non-space OR run of
                            // space). Spaces collapse only across line
                            // breaks, not within a line.
                            size_t tok_start = i;
                            bool ws = (content[i] == ' ' || content[i] == '\t');
                            int tok_w = 0;
                            while (i < content.size()
                                   && content[i] != '\n'
                                   && ((content[i] == ' '
                                        || content[i] == '\t') == ws)) {
                                int n = cb(static_cast<unsigned char>(content[i]));
                                // Display width, not codepoint count — keep
                                // in lockstep with wrap_cell_lines (measure)
                                // so row counts match, and with the
                                // string_width() padding below.
                                std::size_t dp = i;
                                char32_t cp = decode_utf8(content, dp);
                                tok_w += is_wide_char(cp) ? 2 : 1;
                                i += static_cast<size_t>(n);
                            }

                            if (line_w + tok_w <= max_w) {
                                line_w += tok_w;
                                continue;
                            }
                            // Doesn't fit. If the token is whitespace,
                            // discard it at the line break.
                            if (ws) {
                                flush_line(tok_start);
                                line_start = i;
                                line_w = 0;
                                continue;
                            }
                            // Non-whitespace word that doesn't fit.
                            // Case A: line already has content → break here.
                            if (line_w > 0) {
                                flush_line(tok_start);
                                line_start = tok_start;
                                line_w = 0;
                                i = tok_start;   // re-process the word on new line
                                continue;
                            }
                            // Case B: a single word longer than the line.
                            // Force-break at max_w characters.
                            size_t pos = tok_start;
                            int forced_w = 0;
                            while (pos < i && forced_w < max_w) {
                                int n = cb(static_cast<unsigned char>(content[pos]));
                                std::size_t dp = pos;
                                char32_t cp = decode_utf8(content, dp);
                                int cw = is_wide_char(cp) ? 2 : 1;
                                if (forced_w + cw > max_w && forced_w > 0) break;
                                pos += static_cast<size_t>(n);
                                forced_w += cw;
                            }
                            flush_line(pos);
                            line_start = pos;
                            line_w = 0;
                            i = pos;             // continue word on next line
                        }
                        if (line_start < content.size() || lines.empty())
                            flush_line(content.size());

                        // Build output FlatCell per line range, slicing runs.
                        out.reserve(lines.size());
                        for (auto [a, b] : lines) {
                            FlatCell fc;
                            fc.content = content.substr(a, b - a);
                            for (auto& r : runs) {
                                size_t rs = r.byte_offset;
                                size_t re = r.byte_offset + r.byte_length;
                                if (re <= a || rs >= b) continue;
                                size_t s = std::max(rs, a);
                                size_t e = std::min(re, b);
                                if (e <= s) continue;
                                fc.runs.push_back(
                                    StyledRun{s - a, e - s, r.style});
                            }
                            out.push_back(std::move(fc));
                        }
                        return out;
                    };

                    // ── Build the visual rows for a logical row by wrapping
                    // each cell to its column width and then transposing
                    // — visual line v of the row is "cell[0].line[v] │
                    // cell[1].line[v] │ …", padded with empty lines
                    // when a cell is shorter than the row's tallest cell.
                    auto build_row_visuals = [&](
                        const std::vector<FlatCell>& cells,
                        const Style& base) -> std::vector<Element>
                    {
                        // Wrap each cell.
                        std::vector<std::vector<FlatCell>> wrapped(
                            static_cast<size_t>(ncols));
                        int max_lines = 1;
                        for (int c = 0; c < ncols; ++c) {
                            wrapped[static_cast<size_t>(c)] = wrap_cell(
                                cells[static_cast<size_t>(c)].content,
                                cells[static_cast<size_t>(c)].runs,
                                col_w[static_cast<size_t>(c)]);
                            max_lines = std::max(max_lines,
                                static_cast<int>(wrapped[static_cast<size_t>(c)].size()));
                        }
                        auto sep_style = Style{}.with_fg(colors::table_border());
                        const std::string sep = "\xe2\x94\x82";   // │

                        std::vector<Element> visuals;
                        visuals.reserve(static_cast<size_t>(max_lines));
                        for (int v = 0; v < max_lines; ++v) {
                            std::string line;
                            std::vector<StyledRun> line_runs;
                            // Leading │
                            line += sep;
                            line_runs.push_back(StyledRun{0, sep.size(), sep_style});
                            for (int c = 0; c < ncols; ++c) {
                                int cw = col_w[static_cast<size_t>(c)];
                                int total = cw + pad * 2;
                                md::TableAlign align =
                                    data->aligns[static_cast<size_t>(c)];
                                // Pull this cell's v-th line if present.
                                const FlatCell* fc = nullptr;
                                if (static_cast<int>(wrapped[static_cast<size_t>(c)].size()) > v)
                                    fc = &wrapped[static_cast<size_t>(c)][static_cast<size_t>(v)];

                                std::string content_part;
                                std::vector<StyledRun> runs_part;
                                if (fc) {
                                    content_part = fc->content;
                                    runs_part = fc->runs;
                                }
                                int content_w = string_width(content_part);
                                int slack = std::max(0, cw - content_w);

                                // Distribute slack according to alignment.
                                // `left_pad` and `right_pad` are the
                                // EXTRA cells inside the column body,
                                // ON TOP of the fixed `pad` cell on
                                // each side of every cell. The fixed
                                // pad keeps content visually clear of
                                // the `│` borders.
                                int left_pad  = 0;
                                int right_pad = slack;
                                if (align == md::TableAlign::Right) {
                                    left_pad  = slack;
                                    right_pad = 0;
                                } else if (align == md::TableAlign::Center) {
                                    left_pad  = slack / 2;
                                    right_pad = slack - left_pad;
                                }

                                // Left fixed pad + alignment slack
                                size_t cell_off = line.size();
                                line.append(static_cast<size_t>(pad + left_pad), ' ');
                                // Content
                                size_t content_off = line.size();
                                line += content_part;
                                for (auto& r : runs_part)
                                    line_runs.push_back(StyledRun{
                                        content_off + r.byte_offset,
                                        r.byte_length, r.style});
                                // Right alignment slack + fixed pad
                                int right_total = right_pad + pad;
                                line.append(static_cast<size_t>(right_total), ' ');
                                // Cell-spanning base style for the
                                // padding cells (so background-color
                                // styles, if any are added later, fill
                                // the whole cell instead of just text).
                                if (!runs_part.empty() || content_w == 0) {
                                    line_runs.push_back(StyledRun{
                                        cell_off,
                                        static_cast<size_t>(pad + left_pad), base});
                                    line_runs.push_back(StyledRun{
                                        content_off + content_part.size(),
                                        static_cast<size_t>(right_total), base});
                                }
                                // Trailing │
                                size_t s = line.size();
                                line += sep;
                                line_runs.push_back(StyledRun{s, sep.size(), sep_style});
                                (void)total;
                            }
                            visuals.push_back(Element{TextElement{
                                .content = std::move(line),
                                .style = base,
                                .runs = std::move(line_runs),
                            }});
                        }
                        return visuals;
                    };

                    // ── Border line builder. type: 0=top, 1=mid, 2=bottom.
                    auto make_border_line = [&](int type) -> Element {
                        const char* left[]  = {"\xe2\x94\x8c","\xe2\x94\x9c","\xe2\x94\x94"};
                        const char* mid[]   = {"\xe2\x94\xac","\xe2\x94\xbc","\xe2\x94\xb4"};
                        const char* right[] = {"\xe2\x94\x90","\xe2\x94\xa4","\xe2\x94\x98"};
                        std::string line;
                        line += left[type];
                        for (int c = 0; c < ncols; ++c) {
                            if (c > 0) line += mid[type];
                            int total = col_w[static_cast<size_t>(c)] + pad * 2;
                            for (int k = 0; k < total; ++k)
                                line += "\xe2\x94\x80";          // ─
                        }
                        line += right[type];
                        return Element{TextElement{
                            .content = std::move(line),
                            .style = Style{}.with_fg(colors::table_border()),
                        }};
                    };

                    // ── Soft inter-row separator. Same skeleton as the
                    // header rule (├ … ┼ … ┤) but with light-quadruple-
                    // dash horizontals (┈) and rendered at half luminance
                    // via `with_dim`. Dashed-not-solid + dim is the
                    // canonical "this is a row break, not a section
                    // break" treatment in pro TUIs (Helix, Lazygit).
                    auto make_row_separator = [&]() -> Element {
                        std::string line;
                        line += "\xe2\x94\x9c";          // ├
                        for (int c = 0; c < ncols; ++c) {
                            if (c > 0) line += "\xe2\x94\xbc";  // ┼
                            int total = col_w[static_cast<size_t>(c)] + pad * 2;
                            for (int k = 0; k < total; ++k)
                                line += "\xe2\x94\x88";  // ┈ (light quadruple dash)
                        }
                        line += "\xe2\x94\xa4";          // ┤
                        return Element{TextElement{
                            .content = std::move(line),
                            .style = Style{}.with_fg(colors::table_border()).with_dim(),
                        }};
                    };

                    // ── Compose the full table (or a chunk of one — see
                    // md::Table::omit_top/omit_bottom: a sealed streaming
                    // chunk renders only its own rows; the header/borders
                    // are owned by the first/last chunk respectively, and
                    // the seam to the chunk above is a row separator).
                    std::vector<Element> rows;
                    rows.reserve(rows_flat.size() * 3 + 4);
                    if (!data->omit_top) {
                        rows.push_back(make_border_line(0));
                        for (auto& v : build_row_visuals(header_flat, header_base))
                            rows.push_back(std::move(v));
                        rows.push_back(make_border_line(1));
                    }
                    for (std::size_t ri = 0; ri < rows_flat.size(); ++ri) {
                        if (ri > 0 || data->omit_top)
                            rows.push_back(make_row_separator());
                        for (auto& v : build_row_visuals(rows_flat[ri], cell_base))
                            rows.push_back(std::move(v));
                    }
                    if (!data->omit_bottom)
                        rows.push_back(make_border_line(2));
                    auto built = detail::vstack()(std::move(rows)).build();
                    // Memoise. Subsequent paints at the same width
                    // hand back the cached copy without re-walking
                    // any cell. Different widths overwrite the slot
                    // (a single-entry cache is enough — terminal
                    // resizes are rare relative to per-frame paints,
                    // and we only ever render at one width per pass).
                    data->cached_w      = avail_w;
                    data->cached_render = built;
                    return built;
                },
                // No custom measure — the renderer auto-derives the
                // table's height by running the render callback and
                // counting the rows of the produced tree.  This
                // removes the previous hand-rolled measure that had to
                // mirror render() exactly (and caused clipped rows
                // when the two formulas drifted).
                .layout = {},
            }};
        },
        [](const md::FootnoteDef& fn) -> Element {
            // Render as a hanging-indent list item:
            //
            //   [label]: first paragraph wraps under the label gutter.
            //            continuation paragraphs align with the body.
            //
            // The previous version put the label on its own row and then
            // indented every child by 2 spaces, leaving "[a]:" sitting
            // alone above the body — it read as an empty definition
            // rather than the start of one. Inlining the label with the
            // first child mirrors how academic footnotes look on paper
            // and how GitHub renders them in Markdown.
            std::string label_str = "[" + fn.label + "] ";
            auto label_style =
                Style{}.with_fg(colors::footnote_fg()).with_bold();
            Element label_elem{TextElement{
                .content = label_str,
                .style = label_style,
            }};

            if (fn.children.empty()) {
                return label_elem;
            }

            // Indent column whose width equals the visible label width.
            // Continuation children sit under this column; the first
            // child sits next to the label itself.
            std::string indent_col(label_str.size(), ' ');
            auto indent_elem = [&] {
                return Element{TextElement{.content = indent_col}};
            };

            std::vector<Element> rows;
            rows.reserve(fn.children.size());

            // First child: hstack(label, body) so wrap-continuation lines
            // of the body align under the body's first column.
            rows.push_back(detail::hstack()(
                label_elem,
                md_block_to_element(fn.children.front())
            ));
            // Remaining children: hstack(blank indent, body).
            for (std::size_t k = 1; k < fn.children.size(); ++k) {
                rows.push_back(detail::hstack()(
                    indent_elem(),
                    md_block_to_element(fn.children[k])
                ));
            }
            return detail::vstack()(std::move(rows));
        },
        [](const md::Alert& a) -> Element {
            // GitHub-style alert: colored gutter + kind header, children
            // indented under a single solid bar in the kind color.
            const char* label = "NOTE";
            const char* icon  = "\xe2\x84\xb9";  // ℹ
            Color bar = colors::alert_note();
            switch (a.kind) {
                case md::Alert::Kind::Note:
                    label = "NOTE";  icon = "\xe2\x84\xb9";  // ℹ
                    bar = colors::alert_note(); break;
                case md::Alert::Kind::Tip:
                    label = "TIP";   icon = "\xf0\x9f\x92\xa1"; // 💡
                    bar = colors::alert_tip(); break;
                case md::Alert::Kind::Important:
                    label = "IMPORTANT"; icon = "\xe2\x97\x86";  // ◆
                    bar = colors::alert_important(); break;
                case md::Alert::Kind::Warning:
                    label = "WARNING";   icon = "\xe2\x9a\xa0";  // ⚠
                    bar = colors::alert_warning(); break;
                case md::Alert::Kind::Caution:
                    label = "CAUTION";   icon = "\xe2\x9b\x94";  // ⛔
                    bar = colors::alert_caution(); break;
            }

            auto bar_style = Style{}.with_fg(bar).with_bold();

            std::vector<Element> rows;
            rows.reserve(a.children.size() + 1);

            // Header: " ▍ ICON KIND "
            std::string header;
            header.reserve(16);
            header += "\xe2\x96\x8d ";          // ▍
            header += icon;
            header += ' ';
            header += label;
            rows.push_back(Element{TextElement{
                .content = std::move(header),
                .style = bar_style,
            }});

            for (auto& child : a.children) {
                auto child_elem = md_block_to_element(child);
                rows.push_back(detail::hstack()(
                    Element{TextElement{
                        .content = "\xe2\x96\x8d ",     // ▍
                        .style = bar_style,
                    }},
                    std::move(child_elem)
                ));
            }

            return detail::vstack()(std::move(rows));
        },
        [](const md::DefList& d) -> Element {
            std::vector<Element> items;
            items.reserve(d.items.size() * 2);
            for (auto& item : d.items) {
                // Term: bolded
                std::string term_text;
                std::vector<StyledRun> runs;
                Style base = Style{}.with_bold().with_fg(colors::bold_fg());
                flatten_inlines(item.term, base, term_text, runs);
                items.push_back(Element{TextElement{
                    .content = std::move(term_text),
                    .style = base,
                    .runs = std::move(runs),
                }});

                // Definitions: indented with leading ":"
                for (auto& def : item.defs) {
                    std::vector<Element> def_rows;
                    def_rows.reserve(def.size());
                    for (auto& child : def) {
                        def_rows.push_back(md_block_to_element(child));
                    }
                    auto def_body = detail::vstack()(std::move(def_rows));
                    items.push_back(detail::hstack()(
                        Element{TextElement{
                            .content = "  : ",
                            .style = Style{}.with_fg(colors::list_bullet()),
                        }},
                        std::move(def_body)
                    ));
                }
            }
            return detail::vstack()(std::move(items));
        },
        [](const md::Details& d) -> Element {
            // Render as: "▸ summary" bolded, body indented beneath.
            //
            // Glyph note: ▸ (U+25B8) is THREE UTF-8 bytes (\xe2\x96\xb8),
            // not two. Styling only the first 2 bytes leaks the third
            // continuation byte into whatever style the summary text
            // starts with, and the terminal sees a torn codepoint —
            // visible as the `███ click` mojibake on screen. The run
            // length is the literal prefix size, computed at compile time.
            static constexpr std::string_view kPrefix = "\xe2\x96\xb8 "; // "▸ "
            std::string summary_text;
            std::vector<StyledRun> runs;
            Style base = Style{}.with_bold().with_fg(colors::bold_fg());
            runs.push_back({0, kPrefix.size(),
                            Style{}.with_fg(colors::list_bullet())});
            summary_text.append(kPrefix);
            flatten_inlines(d.summary, base, summary_text, runs);
            Element header{TextElement{
                .content = std::move(summary_text),
                .style = base,
                .runs = std::move(runs),
            }};

            std::vector<Element> body_rows;
            body_rows.reserve(d.body.size());
            for (auto& b : d.body) body_rows.push_back(md_block_to_element(b));

            std::vector<Element> out_rows;
            out_rows.reserve(2);
            out_rows.push_back(std::move(header));
            if (!body_rows.empty()) {
                // Indent body under the "▸ " gutter — the glyph is 1 cell
                // wide and is followed by a space, so 2 leading spaces puts
                // the body's first column directly under the summary's
                // first letter. (Previously "  " lined up with the glyph,
                // making the body look flush with the marker.)
                out_rows.push_back(detail::hstack()(
                    Element{TextElement{
                        .content = "  ",
                    }},
                    detail::vstack()(std::move(body_rows))
                ));
            }
            return detail::vstack()(std::move(out_rows));
        },
        [](const md::HtmlBlock& h) -> Element {
            // Delegate raw HTML blocks to the maya::html widget, which parses
            // and renders them as styled terminal Elements. The HTML widget is
            // themed, so map the markdown palette onto a Theme so embedded
            // HTML matches the surrounding markdown's look.
            //
            // NOT `static`. This used to be built once on first use from the
            // mutable colors:: globals, which froze embedded HTML into
            // whatever palette happened to be live at that moment — a theme
            // swap afterwards repainted the markdown around it and left the
            // HTML on the old colours. The type system found this: the
            // palette entries are Colors (they may be slots) and a Theme
            // holds LitColor, so the assignment does not compile without a
            // resolve, and resolving means naming a theme, and naming one
            // means saying WHEN — which is now, every time.
            const Theme& live = theme::live();
            Theme md_theme = live;
            md_theme.text         = live.resolve(colors::text());
            md_theme.primary      = live.resolve(colors::heading1());
            md_theme.accent       = live.resolve(colors::heading2());
            md_theme.info         = live.resolve(colors::heading3());
            md_theme.muted        = live.resolve(colors::footnote_fg());
            md_theme.link         = live.resolve(colors::link_fg());
            md_theme.surface      = live.resolve(colors::code_bg());
            md_theme.border       = live.resolve(colors::table_border());
            md_theme.highlight    = live.resolve(colors::highlight_bg());
            md_theme.inverse_text = live.resolve(colors::highlight_fg());
            return html::render(h.content, md_theme);
        },
    }, block.inner);
}

// ── themable palette ────────────────────────────────────────────────────────
MarkdownPalette default_markdown_palette() {
    return MarkdownPalette{
        colors::text(), colors::heading1(), colors::heading2(), colors::heading3(),
        colors::heading_dim(), colors::heading_rule(),
        colors::bold_fg(), colors::italic_fg(), colors::code_fg(), colors::code_bg(),
        colors::link_fg(), colors::image_fg(), colors::strike_fg(),
        colors::quote_bar(), colors::quote_text(), colors::list_bullet(),
        colors::list_num(), colors::checkbox_fg(), colors::checkbox_off(),
        colors::code_border(), colors::code_lang(), colors::hrule_fg(),
        colors::footnote_fg(), colors::table_border(), colors::table_header(),
        colors::highlight_bg(), colors::highlight_fg(), colors::mention_fg(),
        colors::kbd_fg(), colors::kbd_border(),
        colors::alert_note(), colors::alert_tip(), colors::alert_important(),
        colors::alert_warning(), colors::alert_caution(),
    };
}

// Project a Theme onto the markdown palette.
//
// Markdown is where most of the text in a TUI actually lives, so a theme
// that stops at the chrome is a theme you can barely see. Every slot below
// resolves through a SEMANTIC theme field rather than a literal, which is
// what makes the mapping work for both kinds of theme at once:
//
//   - `native`'s slots ARE the named ANSI colours, so it reproduces the
//     historical terminal look exactly — nothing regresses by default.
//   - an RGB scheme's slots are its own hues, so the whole document
//     repaints in that scheme rather than only its borders.
//
// Where a slot has no direct semantic twin, it is DERIVED rather than
// invented: chrome (rules, borders, footnotes) is the muted slot, and the
// code-block background is the surface slot, so it sits a step off the
// canvas on any polarity instead of being a hardcoded black that turns
// into a hole on a light scheme.
MarkdownPalette markdown_palette_from(const Theme& t) {
    // Projected through the ONE authored mapping (MAYA_MD_PALETTE in
    // markdown/internal.hpp). This function used to restate all 37 roles by
    // hand, and disagreed with the defaults on nine of them — markdown
    // rendered under one mapping until the first theme swap and the other
    // forever after, so headings, code spans and links visibly shifted on a
    // swap that changed nothing else.
    //
    // RESOLVED against `t`, not left symbolic. The internal palette is
    // late-bound (its entries are Color::slot, resolved at paint) precisely
    // so a stored Element follows a theme switch. This is the PUBLIC type,
    // and it means something different: "the colours theme `t` produces",
    // asked for by a host that names the theme explicitly. A slot here would
    // resolve against whatever is live at PAINT time instead of against `t`,
    // so `markdown_palette_from(A)` published while B is live would paint B
    // — silently ignoring the argument.
    //
    // So the boundary is: symbolic inside, concrete at the public edge,
    // resolved against the theme the caller actually named.
    const colors::Palette p = colors::project(t);
    MarkdownPalette out{};
#define X(f, SLOT) out.f = t.resolve(p.f);
    MAYA_MD_PALETTE(X)
#undef X
    return out;
}

// Publishes a new immutable palette snapshot; the render path picks it up on
// its next acquire load.
//
// This used to overwrite ~35 mutable globals in place, with a THREADING
// CONTRACT in the comment asking callers to only do it while no
// StreamingMarkdown was live: the async worker (spawn_async_worker_) parses
// on a background worker thread and reaches md_block_to_element, which reads the
// palette directly across ~165 sites. That contract could not be honoured by
// the one caller that matters — agentty's theme picker previews live, and the
// appearance panel has no streaming gate — so re-theming mid-response was a
// genuine race, and the best case was a worker observing a mix of old and new
// colours for one frame.
//
// The palette is now a VALUE published by pointer swap (colors::publish).
// Readers take one acquire load and then read a frozen object nobody will
// mutate, so there is no contract left to break and no lock on the hot path.
void set_markdown_palette(const MarkdownPalette& p) {
    colors::Palette next{};
    // Stored VERBATIM — no resolve here.
    //
    // The internal palette is late-bound: its entries are normally
    // Color::slot(...) and the resolve happens at paint, which is what lets
    // a committed markdown block follow a later theme switch. An override is
    // a deliberate exception to that: a host says "paint markdown with THESE
    // colours", and it has already decided what they are —
    // markdown_palette_from() resolves against the theme the caller named.
    //
    // Resolving again here would be wrong twice over. Against theme::live()
    // it second-guesses the caller with ambient state ("project theme T,
    // then publish it" is the obvious call order, and a host that has not
    // yet made T live would get its slots bound to the OUTGOING theme). And
    // it would flatten a host that deliberately passed a SLOT wanting the
    // late-bound behaviour — pinning it to today's palette forever, which is
    // the exact bug class late binding exists to remove.
    //
    // So: whatever the host handed us is what markdown paints with. A
    // literal stays literal, a slot stays late-bound.
#define X(f, SLOT) next.f = p.f;
    MAYA_MD_PALETTE(X)
#undef X
    colors::publish(next);
}

} // namespace maya
