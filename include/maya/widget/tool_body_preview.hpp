#pragma once
// maya::widget::ToolBodyPreview — compact body content for one timeline event.
//
// Sits under the AgentTimeline event's `│` connector and carries the
// glanceable "what did the tool actually do" detail per ToolKind:
//
//     Kind             use it for             rendering
//     ───────────────  ─────────────────────  ────────────────────────────
//     None             empty body             nothing
//     CodeBlock        generic head+tail      dim text with elision marker
//     Failure          generic head+tail      red text with elision marker
//     EditDiff         struct EditHunk[]      −/+ blocks with per-hunk header
//     GitDiff          unified diff text      +/-/@@ per-line coloring
//     TodoList         struct TodoItem[]      checkbox list with status icons
//     BashOutput       terminal output        dim head + emphasized failing tail
//     FileRead         file contents          line-numbered gutter + code
//     FileWrite        new file body          dim "+" prefix + lines/bytes footer
//     Json             JSON response          tokenized pretty-print w/ kv colors
//     GrepMatches      grep results           path:line:text columnar rendering
//
// Discriminated by `kind`; only the fields relevant to that kind are
// read. Returns an empty Element when there's nothing useful to show
// (Kind::None, empty text, etc.) so callers can drop it into a slot
// list unconditionally.
//
//   maya::ToolBodyPreview{{
//       .kind        = ToolBodyPreview::Kind::BashOutput,
//       .text        = stdout,
//       .failed      = exit_code != 0,
//       .exit_code   = exit_code,
//   }}.build();

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../dsl.hpp"
#include "../element/element.hpp"
#include "../element/text.hpp"   // string_width, truncate_end
#include "../style/color.hpp"
#include "../style/style.hpp"
#include "../style/theme.hpp"
#include "markdown.hpp"   // syntax_line_runs

namespace maya {

// Defined in render/canvas.cpp — the pipeline's active color capability
// (3 = truecolor, 2 = 256, 1 = 16). Declared here (not #include'd from
// render/canvas.hpp) to keep this widget header light.
[[nodiscard]] int terminal_color_level() noexcept;


// A byte range of a diff line that changed vs its partner line, and the
// brighter band it sits on. Namespace scope so it can be a default arg.
struct DiffChange { std::size_t off = 0, len = 0; Color bg{}; };

class ToolBodyPreview {
public:
    enum class Kind : std::uint8_t {
        None,         // empty body
        CodeBlock,    // generic head+tail preview, dim'd in `text_color`
        Failure,      // same shape as CodeBlock but in `Color::slot(ThemeSlot::Error)`
        EditDiff,     // multi-hunk edit preview (per-hunk header + −/+ lines)
        GitDiff,      // unified diff with per-line +/-/@@ coloring
        TodoList,     // checkbox list with status icons
        BashOutput,   // terminal output: dim head, red tail when failed
        FileRead,     // code with line-number gutter
        FileWrite,    // new file body: subtle + prefix, lines/bytes footer
        Json,         // pretty-printed JSON with key/value colors
        GrepMatches,  // path:line:text matches
    };

    struct EditHunk {
        std::string old_text;
        std::string new_text;
    };

    struct TodoItem {
        enum class Status : std::uint8_t { Pending, InProgress, Completed };
        std::string content;
        Status      status = Status::Pending;
    };

    struct Config {
        Kind kind = Kind::None;

        // CodeBlock / Failure / GitDiff / BashOutput / FileRead / FileWrite /
        // Json / GrepMatches: free-text body
        std::string text;
        Color       text_color = Color::slot(ThemeSlot::Text);

        // Color used for the BODY CHROME (line-number gutter, pipe
        // separator `│`, the trailing "⋯ N more" elision marker). Host
        // typically passes the tool's category color so a Bash body's
        // chrome reads cyan, an Edit body's chrome reads magenta, etc.
        // — visually framing the dim body content in the category hue.
        // Defaults to ANSI 7 white (legible mid-gray).
        Color       chrome_color = Color::slot(ThemeSlot::Text);

        // EditDiff
        std::vector<EditHunk> hunks;

        // TodoList
        std::vector<TodoItem> todos;

        // BashOutput: when failed, append `· exit N` to the last visible
        // line. We deliberately do NOT recolor the tail — the surrounding
        // timeline card already paints its border red and its connector
        // red on failure; coloring the body too would be double-flagging.
        bool failed    = false;
        int  exit_code = 0;

        // FileRead: gutter starts at this line number.
        int  start_line  = 1;

        // FileRead: cross-tool semantics. When the assistant has previously
        // located interesting lines via Grep, the caller can pass those
        // line numbers here. The rendered Read body then (a) summarises
        // them in a header row `▸ matches: 1, 42, 61`, and (b) for any
        // listed line that falls within the rendered head, replaces the
        // gutter's leading space with `▸` and brightens the code style —
        // so the user's eye lands on the relevant line instead of having
        // to scan and cross-reference. std::set keeps lookup logarithmic
        // and gives the deterministic ascending order the summary needs.
        std::set<int> highlight_lines;

        // Language of the code in the body: a file extension or fence tag
        // ("cpp", "py", "rs"). Read / write / edit / diff rows are syntax
        // coloured with it. Empty = flat colour, as before.
        std::string lang;

        // The literal text a search was LOOKING FOR, highlighted in place
        // wherever it appears in a GrepMatches body.
        //
        // Without it a match row is one flat span and the user re-scans each
        // line hunting for the thing they just searched for -- the structure
        // (grouped by file, line-numbered) was already right, but every
        // character carried equal weight. maya's own SearchResults widget
        // renders before/MATCH/after for exactly this reason; this brings
        // that emphasis to the body a tool card can actually use.
        //
        // LITERAL, not a regex. A caller passing a pattern gets highlighting
        // only where the pattern happens to be literal text -- which is the
        // common case (`cfg.status`, `TextWrap::Truncate`) and costs nothing
        // when it is not. Running a regex engine per row, per frame, on a
        // pattern the user may have mistyped is not worth it.
        std::string match_needle;

        // Suppress placeholder rendering. Default behaviour for an empty
        // body in a Running tool is to render NOTHING — the timeline's
        // own spinner conveys "in flight" already, and an additional
        // "awaiting…" row just adds column flicker as bodies pop in.
        // Set true to opt back into the placeholder.
        bool show_streaming_placeholder = false;
        bool is_streaming               = false;

        // FileWrite: append `12 lines · 482 B` footer. The byte count is
        // the reason a Write event has a body at all (the path is in the
        // header), so this stays on by default.
        bool show_footer_stats = true;

        // When true, the body renderer skips all head/tail elision and
        // shows EVERY line / hunk / item. Used for Write and Edit where
        // the user wants to see exactly what was written or changed, not
        // a head-only preview with "⋯ N more". Affects code_block,
        // file_write, edit_diff, git_diff, todo_list.
        bool show_all = false;

        // When true (default), every line-oriented body renders ONLY the
        // last N lines (live tail) — no head section, no "⋯ N more"
        // elision marker in the middle. The user wants a single
        // continuous tail of fresh output, not a head+tail split. Affects
        // CodeBlock, FileRead, GitDiff, EditDiff per-side, BashOutput,
        // Json. `show_all` overrides this (renders everything).
        bool tail_only = true;

        // Tunables. Defaults are tuned for the timeline body slot, not a
        // standalone preview pane:
        //  - code_head/tail: a head+tail elision profile for free text
        //    (CodeBlock, Failure, GitDiff)
        //  - bash_tail: terminal output is tail-heavy (summaries live at
        //    the end), so BashOutput renders only the last N lines
        //  - read_head: file reads are head-heavy (the user wants to see
        //    where the file starts), so FileRead shows only the first N
        int code_head           = 4;
        int code_tail           = 3;
        int bash_tail           = 4;
        int read_head           = 5;
        int edit_head_per_side  = 6;
        int edit_tail_per_side  = 2;
        int max_edit_hunks_shown = 4;
        int max_todos_shown     = 8;
        int max_matches_shown   = 8;

        // EditDiff, streaming only: ordinal of the hunk currently
        // landing (1-based). Streaming previews window to the NEWEST
        // hunk (the host slices earlier ones away — they'd balloon the
        // live card and their headers would rewrite committed rows as
        // totals tick), which loses the sense that earlier edits already
        // applied. This tags the stat chip "edit N · −2 / +5" so the
        // count of landed hunks stays visible at ZERO extra rows — the
        // streaming body budget (host-side) is row-exact and an extra
        // "⋯ applied" row would blow the 18-row proof. The ticking N is
        // seam-safe: the chip row is part of the streaming body, which
        // the budget keeps inside the viewport by construction. 0 = no
        // tag (settled renders use the "edit i/N" indexed form instead).
        int stream_hunk_no = 0;
    };

    explicit ToolBodyPreview(Config c) : cfg_(std::move(c)) {
        // A tab has no cell width, so tab-indented code (C, Go, Makefiles)
        // lost its indentation. Expand to 4-column stops up front.
        expand_tabs(cfg_.text);
        for (auto& h : cfg_.hunks) {
            expand_tabs(h.old_text);
            expand_tabs(h.new_text);
        }
    }

    static void expand_tabs(std::string& s) {
        if (s.find('\t') == std::string::npos) return;
        std::string out;
        out.reserve(s.size() + 16);
        int col = 0;
        for (char c : s) {
            if (c == '\t') {
                const int n = 4 - (col % 4);
                out.append(static_cast<std::size_t>(n), ' ');
                col += n;
            } else {
                out += c;
                if (c == '\n') col = 0;
                else if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++col;
            }
        }
        s = std::move(out);
    }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;
        switch (cfg_.kind) {
            case Kind::None:        return blank();
            case Kind::CodeBlock:   return code_block(cfg_.text, cfg_.text_color);
            case Kind::Failure:     return code_block(cfg_.text, danger());
            case Kind::EditDiff:    return edit_diff();
            case Kind::GitDiff:     return git_diff();
            case Kind::TodoList:    return todo_list();
            case Kind::BashOutput:  return bash_output();
            case Kind::FileRead:    return file_read();
            case Kind::FileWrite:   return file_write();
            case Kind::Json:        return json_block();
            case Kind::GrepMatches: return grep_matches();
        }
        return blank();
    }

private:
    Config cfg_;

    // `muted()` is the LEGIBLE-secondary tier — line gutters, footer
    // summaries, elision markers, hint labels. ANSI 7 ("white") renders
    // as a clearly readable mid-gray on dark themes (typical ~#C0C0C0)
    // and as a slightly-recessed gray on light themes — visibly
    // subordinate to bright_white prose but never collapsing below the
    // readable floor the way bright_black (ANSI 8, ~#5C5C5C on dark
    // themes) does on low-contrast terminals.
    static constexpr Color muted()   { return Color::slot(ThemeSlot::Text); }
    static constexpr Color success() { return Color::slot(ThemeSlot::Success); }
    static constexpr Color danger()  { return Color::slot(ThemeSlot::Error); }
    static constexpr Color info()    { return Color::slot(ThemeSlot::Primary); }
    static constexpr Color accent()  { return Color::slot(ThemeSlot::Info); }
    static constexpr Color number()  { return Color::slot(ThemeSlot::Warning); }

    // ── Line-level helpers ────────────────────────────────────────────────

    static std::vector<std::string_view> split_lines(std::string_view s) {
        std::vector<std::string_view> out;
        std::size_t start = 0;
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\n') {
                out.emplace_back(s.data() + start, i - start);
                start = i + 1;
            }
        }
        if (start < s.size()) out.emplace_back(s.data() + start, s.size() - start);
        return out;
    }

    struct ElidedPreview {
        std::vector<std::string_view> lines;
        int elided = 0;
        // Whether index `i` in `lines` is the position where the elision
        // marker should be inserted ABOVE (i.e., between i-1 and i).
        int elision_at = -1;
        // Lines silently dropped BEFORE lines[0] with NO marker row — the
        // tail_only path. Line-numbered renderers must offset their
        // gutter by this so the tail shows TRUE source positions (509,
        // 510, 511 of a 512-line output) instead of restarting at 1.
        // Distinct from `elided`, which drives the visible "⋯ N more"
        // marker; tail_only deliberately renders no marker.
        int hidden_above = 0;
    };

    static ElidedPreview head_tail(std::string_view s, int head, int tail) {
        auto all = split_lines(s);
        const int total = static_cast<int>(all.size());
        ElidedPreview out;
        if (total <= head + tail) {
            out.lines = std::move(all);
            return out;
        }
        out.lines.reserve(static_cast<std::size_t>(head + tail));
        for (int i = 0; i < head; ++i)
            out.lines.push_back(all[static_cast<std::size_t>(i)]);
        out.elision_at = head;
        out.elided = total - head - tail;
        for (int i = total - tail; i < total; ++i)
            out.lines.push_back(all[static_cast<std::size_t>(i)]);
        return out;
    }

    // Show every line with no elision. Used when cfg_.show_all is set
    // (Write, Edit) so the user sees the full content rather than a
    // head/tail preview.
    static ElidedPreview all_lines(std::string_view s) {
        ElidedPreview out;
        out.lines = split_lines(s);
        out.elision_at = -1;
        out.elided     = 0;
        return out;
    }

    // Top-level elision dispatcher. Honors cfg_.show_all (render every
    // line) and cfg_.tail_only (render only the last `tail` lines, no
    // head, no "⋯ N more" marker). This is the single funnel every
    // body renderer should call instead of head_tail() directly so the
    // user-facing knobs stay in one place.
    [[nodiscard]] ElidedPreview
    elide(std::string_view s, int head, int tail) const {
        if (cfg_.show_all) return all_lines(s);
        if (cfg_.tail_only) {
            // Pick the larger of head/tail as the tail budget so we
            // honor whatever the caller meant by "how much body to
            // show" — head-heavy tools (FileRead, GitDiff with head>tail)
            // still get a meaningfully sized tail.
            const int t = (head > tail) ? head : tail;
            auto p = head_tail(s, 0, t);
            // No marker row in tail mode — but PRESERVE the drop count
            // as hidden_above so gutter renderers keep true line
            // numbers. Zeroing it (the old behaviour) made a 512-line
            // output's tail read "1, 2, 3, 4" — actively misleading
            // when the user cross-references an error's line number.
            p.hidden_above = p.elided;
            p.elision_at = -1;     // suppress marker emit at boundary
            p.elided     = 0;       // suppress "⋯ N more" row
            return p;
        }
        return head_tail(s, head, tail);
    }

    static int count_lines(std::string_view s) noexcept {
        if (s.empty()) return 0;
        int n = 0;
        for (char c : s) if (c == '\n') ++n;
        // A trailing line without a newline still counts; a trailing newline
        // does not introduce a new empty line.
        if (s.back() != '\n') ++n;
        return n;
    }

    static std::string format_bytes(std::size_t n) {
        char buf[32];
        if      (n < 1024)         std::snprintf(buf, sizeof(buf), "%zu B", n);
        else if (n < 1024UL*1024)  std::snprintf(buf, sizeof(buf), "%.1f KB",
                                                 static_cast<double>(n)/1024.0);
        else                       std::snprintf(buf, sizeof(buf), "%.2f MB",
                                                 static_cast<double>(n)/(1024.0*1024.0));
        return buf;
    }

    // ── Streaming placeholder ─────────────────────────────────────────────
    //
    // Off by default — the timeline header already shows a spinner for any
    // Running tool, so an additional "awaiting…" row would just add visual
    // pop-in as bodies appear. Opt-in via `show_streaming_placeholder`.
    [[nodiscard]] Element placeholder_or_blank(std::string_view verb) const {
        if (!cfg_.show_streaming_placeholder) return dsl::blank();
        return dsl::text(std::string{verb} + "\xe2\x80\xa6",
                         Style{}.with_fg(muted()).with_italic()).build();
    }

    // ── CodeBlock: row-numbered head+tail preview (uniform contract).
    //    Marker = right-aligned row number. Used by grep / glob /
    //    list_dir / web_search / git_status / git_log / git_commit —
    //    tools where the body is line-oriented free text and the
    //    "what row of output am I on" cue helps scanning.
    [[nodiscard]] Element code_block(std::string_view body, Color c) const {
        using namespace dsl;
        if (body.empty()) {
            return cfg_.is_streaming ? placeholder_or_blank("awaiting output")
                                     : blank();
        }
        const auto p = elide(body, cfg_.code_head, cfg_.code_tail);
        // Gutter (row numbers) renders in dim mid-gray so it recedes
        // behind the content's category-colored chrome (pipe). Numbers
        // are reference; the pipe carries the category band.
        const Style marker_st = Style{}.with_fg(muted());
        const Style content_st = Style{}.with_fg(c);

        std::vector<Element> rows;
        rows.reserve(p.lines.size() + 1);

        // Track TRUE line number in the original content. hidden_above
        // accounts for the tail_only silent drop (no marker row); the
        // elision-marker jump below handles the head+tail split.
        int line_num = 1 + p.hidden_above;
        for (int i = 0; i < static_cast<int>(p.lines.size()); ++i) {
            if (i == p.elision_at && p.elided > 0) {
                rows.push_back(elision_marker(p.elided));
                line_num += p.elided;
            }
            // Generic tool output is messages, not a file: numbering it
            // (`1 │ switched to branch master`) is noise. Keep the column
            // so the rail lines up with numbered bodies.
            (void)line_num++;
            rows.push_back(make_row("   ", marker_st,
                                    p.lines[std::size_t(i)], content_st));
        }
        return v(rows).build();
    }

    // ── BashOutput: structured extraction → tail-only fallback ────────────
    //
    // For terminal output the user is asking ONE QUESTION ("did the command
    // do what it was supposed to?"), and for several common output shapes —
    // unit-test runners, compiler diagnostics — we can answer it in 1-3
    // rows instead of dumping 4 lines of `[==========]` noise.  We try
    // each known shape in order of specificity; if none match, we fall
    // back to the previous tail-only rendering.  The fallback path stays
    // intact for unknown / freeform output (which is most of bash usage).
    //
    // Failure is NOT recoloured.  The surrounding AgentTimeline event
    // already paints its connector and status icon red on failure;
    // colouring the body too would be double-flagging.  The failed path's
    // only addition is an inline `· exit N` suffix on the final tail line.
    [[nodiscard]] Element bash_output() const {
        using namespace dsl;
        if (cfg_.text.empty()) {
            return cfg_.is_streaming ? placeholder_or_blank("awaiting output")
                                     : blank();
        }
        if (auto e = try_render_test_summary())     return *e;
        if (auto e = try_render_compiler_errors())  return *e;
        return bash_output_tail();
    }

    // Fallback for bash: row-marker is "  >" so each tail line reads as
    // a shell-prompt entry. Last row appends ` · exit N` in red when
    // the command failed (preserves the post-content failure cue).
    [[nodiscard]] Element bash_output_tail() const {
        using namespace dsl;
        const auto p = elide(cfg_.text, /*head=*/0, cfg_.bash_tail);

        const Style marker_st = Style{}.with_fg(cfg_.chrome_color);
        const Style line_st   = Style{}.with_fg(cfg_.text_color);

        std::vector<Element> rows;
        rows.reserve(p.lines.size() + 1);

        constexpr std::string_view kPrompt = "  >";   // right-aligned 3-col

        const int n = static_cast<int>(p.lines.size());
        for (int i = 0; i < n; ++i) {
            if (i == p.elision_at && p.elided > 0)
                rows.push_back(elision_marker(p.elided));

            std::string_view ln = p.lines[std::size_t(i)];
            const bool last = (i + 1 == n);

            if (last && cfg_.failed && cfg_.exit_code != 0) {
                // Compose content with the inline exit-code suffix, then
                // emit through make_row so the row's chrome stays uniform.
                char suf[24];
                std::snprintf(suf, sizeof(suf),
                              "  \xc2\xb7 exit %d", cfg_.exit_code);
                std::string content;
                content.reserve(ln.size() + std::string{suf}.size());
                content += ln;
                const std::size_t exit_off = content.size();
                content += suf;
                (void)exit_off;
                // For simplicity we color the whole content (line + suffix)
                // in line_st; the failure cue carries on the timeline
                // border + ✗ icon. Inline `· exit N` stays readable as
                // tail text; double-coloring would crowd the row.
                rows.push_back(make_row(kPrompt, marker_st, content, line_tone(ln, line_st)));
            } else {
                rows.push_back(make_row(kPrompt, marker_st, ln, line_tone(ln, line_st)));
            }
        }

        if (rows.empty()) return blank();
        return v(rows).build();
    }

    // Colour for one line of shell output by what it says: errors and
    // failures red, warnings yellow, passes green, everything else `base`.
    [[nodiscard]] static Style line_tone(std::string_view ln, Style base) {
        auto has = [&](std::string_view w) {
            return ln.find(w) != std::string_view::npos;
        };
        auto has_ci = [&](std::string_view w) {
            if (w.size() > ln.size()) return false;
            for (std::size_t i = 0; i + w.size() <= ln.size(); ++i) {
                std::size_t k = 0;
                while (k < w.size()
                       && std::tolower(static_cast<unsigned char>(ln[i + k])) == w[k]) ++k;
                if (k == w.size()) return true;
            }
            return false;
        };
        if (has("error:") || has("error[") || has("FAILED") || has("FAIL ")
            || has("panicked") || has("Traceback") || has("fatal:")
            || has("Segmentation fault") || has("Assertion") || has("\xe2\x9c\x97"))
            return Style{}.with_fg(Color::slot(ThemeSlot::Error));
        if (has("warning:") || has("WARN") || has("deprecated"))
            return Style{}.with_fg(Color::slot(ThemeSlot::Warning));
        const bool some_failed = has_ci("failed") && !has_ci(" 0 failed");
        if ((has_ci(" passed") && !has_ci(" 0 passed") && !some_failed)
            || has("Status: SUCCESS") || has("100% tests passed")
            || has("\xe2\x9c\x93") || has("PASS"))
            return Style{}.with_fg(Color::slot(ThemeSlot::Success));
        return base;
    }

    // ── Test-runner summary detection ─────────────────────────────────────
    //
    // Distils a runner's output to a one-line ✓/✗ verdict. Two tiers:
    //
    //   TIER 1 — the literal "N tests passed" / "N tests failed" shape
    //     (gtest "[==========] 4 tests passed.", ctest "100% tests
    //     passed, 0 tests failed out of 4"). Unambiguous, always trusted.
    //
    //   TIER 2 — bare-word runner summaries, only trusted when a runner
    //     CONTEXT marker is present so prose like "the test passed review"
    //     never mis-fires:
    //       cargo:  "test result: ok. 4 passed; 1 failed; 0 ignored"
    //       pytest: "===== 4 passed, 1 failed in 0.12s ====="
    //       jest:   "Tests:  1 failed, 3 passed, 4 total"
    //     The gate requires one of: "test result:" (cargo), "Tests:"
    //     (jest), a pytest " in <time>s" tail next to the word, or both
    //     "passed" and "failed"/"total" co-occurring on the summary.
    [[nodiscard]] std::optional<Element> try_render_test_summary() const {
        using namespace dsl;
        std::optional<int> passed = count_before(cfg_.text, "tests passed");
        std::optional<int> failed = count_before(cfg_.text, "tests failed");

        // TIER 2: bare-word shapes, gated on a runner context marker.
        if (!passed && !failed) {
            const auto& b = cfg_.text;
            const bool has_cargo  = b.find("test result:") != std::string_view::npos;
            const bool has_jest   = b.find("Tests:") != std::string_view::npos;
            // pytest hallmark: a "passed"/"failed" tally followed by an
            // " in <number>s" duration tail on the summary line.
            const bool has_pytest = b.find(" passed") != std::string_view::npos
                                 && b.find(" in ") != std::string_view::npos;
            auto bp = count_before_word(b, "passed");
            auto bf = count_before_word(b, "failed");
            // Co-occurrence gate: a lone "5 passed" with no runner marker
            // and no companion failed/total is too weak — require context.
            const bool companion =
                bf.has_value()
                || count_before_word(b, "total").has_value();
            if ((has_cargo || has_jest || (has_pytest && bp) || (bp && companion))) {
                passed = bp;
                failed = bf;
            }
        }

        if (!passed && !failed) return std::nullopt;

        const int p = passed.value_or(0);
        const int f = failed.value_or(0);
        const int total = p + f;
        if (total == 0) return std::nullopt;

        char buf[96];
        std::vector<StyledRun> runs;
        std::string content;

        const Style ok_st  = Style{}.with_fg(success()).with_bold();
        const Style bad_st = Style{}.with_fg(danger()).with_bold();
        const Style txt_st = Style{}.with_fg(cfg_.text_color);

        if (f == 0) {
            std::snprintf(buf, sizeof(buf), "\xe2\x9c\x93 %d/%d tests passed", p, total);
            const std::string s{buf};
            // Glyph + space = 4 bytes UTF-8 + ' '
            const std::size_t glyph_len = std::string{"\xe2\x9c\x93 "}.size();
            runs.push_back({0, glyph_len, ok_st});
            runs.push_back({glyph_len, s.size() - glyph_len, txt_st});
            content = std::move(buf);
        } else {
            std::snprintf(buf, sizeof(buf), "\xe2\x9c\x97 %d/%d tests failed", f, total);
            const std::string s{buf};
            const std::size_t glyph_len = std::string{"\xe2\x9c\x97 "}.size();
            runs.push_back({0, glyph_len, bad_st});
            runs.push_back({glyph_len, s.size() - glyph_len, txt_st});
            content = std::move(buf);
        }

        std::vector<Element> rows;
        rows.push_back(Element{TextElement{
            .content = std::move(content),
            .style   = {},
            .wrap    = TextWrap::TruncateEnd,
            .runs    = std::move(runs),
        }});

        // On failure, list up to 3 failing test names if we can extract
        // them via the conventional `[  FAILED  ]` marker.  Each row stays
        // 1 line tall to honour AgentTimeline's per-child stripe contract.
        if (f > 0) {
            const std::vector<std::string_view> names =
                extract_failing_test_names(cfg_.text, /*max=*/3);
            const Style fail_row_st = Style{}.with_fg(danger());
            for (const auto& name : names) {
                std::string row;
                row.reserve(name.size() + 4);
                row += "    ";
                row += name;
                rows.push_back(text_row(std::move(row), fail_row_st));
            }
            if (static_cast<int>(names.size()) < f) {
                std::string more;
                more.reserve(32);
                more += "    \xe2\x8b\xaf ";
                more += std::to_string(f - static_cast<int>(names.size()));
                more += " more failing";
                rows.push_back(text_row(std::move(more),
                    Style{}.with_fg(muted()).with_italic()));
            }
        }
        return v(rows).build();
    }

    // ── Compiler / linter diagnostic detection ────────────────────────────
    //
    // Looks for lines matching `path:line(:col)?: error:` or similar and
    // renders them as a structured chip:
    //
    //   ✗ 2 errors in src/foo.cpp
    //        42:7  undeclared identifier 'bar'
    //        43:1  expected ';'
    //
    // When errors span multiple files we drop the per-file grouping and
    // just show the first three lines verbatim — diff-quality across-file
    // error rendering is a rabbit hole we don't need to enter for the
    // common single-file build-error case.
    [[nodiscard]] std::optional<Element> try_render_compiler_errors() const {
        using namespace dsl;
        std::vector<Diag> diags = parse_compiler_diags(cfg_.text);
        if (diags.empty()) return std::nullopt;

        // Determine whether all the errors live in the same file.  When
        // they do we can use the tighter "N errors in path" header; if
        // not we fall back to a generic "N issues" header so we don't lie
        // about scope.
        bool single_file = true;
        for (std::size_t i = 1; i < diags.size(); ++i)
            if (diags[i].path != diags[0].path) { single_file = false; break; }

        const int n_err = static_cast<int>(std::count_if(
            diags.begin(), diags.end(),
            [](const Diag& d) { return d.severity == Diag::Error; }));
        const int n_total = static_cast<int>(diags.size());
        const Style hdr_glyph_st = Style{}.with_fg(danger()).with_bold();
        const Style hdr_txt_st   = Style{}.with_fg(cfg_.text_color);
        const Style coord_st     = Style{}.with_fg(muted());
        const Style msg_st       = Style{}.with_fg(cfg_.text_color);

        // Header
        std::string header;
        std::vector<StyledRun> hdr_runs;
        const std::string glyph = "\xe2\x9c\x97 ";   // ✗
        hdr_runs.push_back({0, glyph.size(), hdr_glyph_st});
        header += glyph;

        const std::size_t txt_off = header.size();
        if (single_file) {
            header += std::to_string(n_err > 0 ? n_err : n_total);
            header += (n_err == 1 || n_total == 1) ? " issue in "
                                                   : " issues in ";
            header += diags[0].path;
        } else {
            header += std::to_string(n_total);
            header += (n_total == 1) ? " issue across files"
                                     : " issues across files";
        }
        hdr_runs.push_back({txt_off, header.size() - txt_off, hdr_txt_st});

        std::vector<Element> rows;
        rows.reserve(diags.size() + 2);
        rows.push_back(Element{TextElement{
            .content = std::move(header),
            .style   = {},
            .wrap    = TextWrap::TruncateEnd,
            .runs    = std::move(hdr_runs),
        }});

        // Up to 3 diagnostic rows.  Each: "    42:7  msg" with "42:7"
        // dim-muted (chrome) and msg in default fg.
        const int shown = std::min<int>(3, static_cast<int>(diags.size()));
        for (int i = 0; i < shown; ++i) {
            const Diag& d = diags[std::size_t(i)];
            char coord[24];
            if (d.col > 0) std::snprintf(coord, sizeof(coord), "%d:%d", d.line, d.col);
            else           std::snprintf(coord, sizeof(coord), "%d",    d.line);
            std::string row;
            std::vector<StyledRun> rrun;
            row += "    ";
            const std::size_t coff = row.size();
            row += coord;
            rrun.push_back({coff, std::string{coord}.size(), coord_st});
            row += "  ";
            const std::size_t moff = row.size();
            row += d.msg;
            rrun.push_back({moff, d.msg.size(), msg_st});
            rows.push_back(Element{TextElement{
                .content = std::move(row),
                .style   = {},
                .wrap    = TextWrap::TruncateEnd,
                .runs    = std::move(rrun),
            }});
        }
        if (shown < static_cast<int>(diags.size())) {
            std::string more;
            more.reserve(32);
            more += "    \xe2\x8b\xaf ";
            more += std::to_string(static_cast<int>(diags.size()) - shown);
            more += " more";
            rows.push_back(text_row(std::move(more),
                Style{}.with_fg(muted()).with_italic()));
        }
        return v(rows).build();
    }

    // Pull the integer that immediately precedes `tag` in `body`.
    // Returns nullopt if `tag` is absent or not preceded by a digit run.
    [[nodiscard]] static std::optional<int>
    count_before(std::string_view body, std::string_view tag) noexcept {
        auto pos = body.find(tag);
        if (pos == std::string_view::npos) return std::nullopt;
        std::size_t i = pos;
        while (i > 0 && body[i - 1] == ' ') --i;
        const std::size_t end = i;
        while (i > 0 && body[i - 1] >= '0' && body[i - 1] <= '9') --i;
        if (i == end) return std::nullopt;
        int n = 0;
        for (auto j = i; j < end; ++j) n = n * 10 + (body[j] - '0');
        return n;
    }

    // Like count_before, but `word` must be a WHOLE word (bounded by a
    // non-alnum on both sides) and only whitespace may sit between the
    // digit run and the word. Catches the bare-word runner shapes:
    //   cargo:  "test result: ok. 12 passed; 0 failed; 3 ignored"
    //   pytest: "===== 12 passed, 1 failed in 0.30s ====="
    //   jest:   "Tests:  1 failed, 11 passed, 12 total"
    // Scans ALL occurrences and takes the LAST (runner summaries live at the
    // end of the output, after any prose that might contain the word).
    [[nodiscard]] static std::optional<int>
    count_before_word(std::string_view body, std::string_view word) noexcept {
        std::optional<int> found;
        std::size_t from = 0;
        while (true) {
            auto pos = body.find(word, from);
            if (pos == std::string_view::npos) break;
            from = pos + word.size();
            // Whole-word: char after `word` must not be alphanumeric.
            if (from < body.size()) {
                char c = body[from];
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                    || (c >= '0' && c <= '9')) continue;
            }
            std::size_t i = pos;
            while (i > 0 && body[i - 1] == ' ') --i;
            const std::size_t end = i;
            while (i > 0 && body[i - 1] >= '0' && body[i - 1] <= '9') --i;
            if (i == end) continue;               // no digits before the word
            // Whole-word on the LEFT too: the char before the digits must
            // not be alphanumeric (so "unpassed" / "x12passed" can't match).
            if (i > 0) {
                char c = body[i - 1];
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) continue;
            }
            int n = 0;
            for (auto j = i; j < end; ++j) n = n * 10 + (body[j] - '0');
            found = n;   // keep scanning; last match wins
        }
        return found;
    }

    // Up to `max` failing test names extracted from gtest-style markers.
    [[nodiscard]] static std::vector<std::string_view>
    extract_failing_test_names(std::string_view body, int max) noexcept {
        constexpr std::string_view kMarker = "[  FAILED  ] ";
        std::vector<std::string_view> out;
        std::size_t i = 0;
        while (out.size() < std::size_t(max)) {
            auto p = body.find(kMarker, i);
            if (p == std::string_view::npos) break;
            const std::size_t name_start = p + kMarker.size();
            // Skip any leading whitespace just in case
            std::size_t name_end = name_start;
            while (name_end < body.size()
                   && body[name_end] != '\n'
                   && body[name_end] != ',') ++name_end;
            if (name_end > name_start) {
                std::string_view name = body.substr(name_start, name_end - name_start);
                // Trim trailing space and parenthesised duration.
                while (!name.empty() && name.back() == ' ') name.remove_suffix(1);
                if (!name.empty() && name.back() == ')') {
                    auto op = name.rfind('(');
                    if (op != std::string_view::npos) {
                        std::string_view trimmed = name.substr(0, op);
                        while (!trimmed.empty() && trimmed.back() == ' ')
                            trimmed.remove_suffix(1);
                        if (!trimmed.empty()) name = trimmed;
                    }
                }
                // Skip the gtest summary line which lists every failing
                // test name on a single comma-separated row.
                if (name.find(' ') == std::string_view::npos)
                    out.push_back(name);
            }
            i = name_end;
        }
        return out;
    }

    // ── Compiler diagnostic line parser ───────────────────────────────────

    struct Diag {
        enum Sev : std::uint8_t { Error, Warning, Note };
        std::string path;
        int         line = 0;
        int         col  = 0;
        Sev         severity = Error;
        std::string msg;
    };

    [[nodiscard]] static std::vector<Diag>
    parse_compiler_diags(std::string_view body) {
        std::vector<Diag> out;
        std::size_t pos = 0;
        while (pos < body.size()) {
            const auto nl = body.find('\n', pos);
            const std::string_view ln = body.substr(
                pos, (nl == std::string_view::npos ? body.size() : nl) - pos);
            if (auto d = parse_diag_line(ln)) out.push_back(std::move(*d));
            if (nl == std::string_view::npos) break;
            pos = nl + 1;
        }
        return out;
    }

    [[nodiscard]] static std::optional<Diag>
    parse_diag_line(std::string_view ln) {
        // Required shape: <path>:<line>(:<col>)?: <severity>: <msg>
        // Severities: error, warning, note, fatal error
        // Reject lines that don't look like file paths (no slash, no dot).
        const auto p1 = ln.find(':');
        if (p1 == std::string_view::npos || p1 == 0) return std::nullopt;
        const std::string_view path = ln.substr(0, p1);
        if (path.find('/') == std::string_view::npos
         && path.find('.') == std::string_view::npos) return std::nullopt;

        const auto p2 = ln.find(':', p1 + 1);
        if (p2 == std::string_view::npos) return std::nullopt;
        const std::string_view linum_sv = ln.substr(p1 + 1, p2 - p1 - 1);
        if (linum_sv.empty()) return std::nullopt;
        int linum = 0;
        for (char c : linum_sv) {
            if (c < '0' || c > '9') return std::nullopt;
            linum = linum * 10 + (c - '0');
        }

        std::size_t rest_start = p2 + 1;
        int col = 0;
        const auto p3 = ln.find(':', rest_start);
        if (p3 != std::string_view::npos) {
            const std::string_view col_sv = ln.substr(rest_start, p3 - rest_start);
            bool digits = !col_sv.empty();
            for (char c : col_sv) if (c < '0' || c > '9') { digits = false; break; }
            if (digits) {
                for (char c : col_sv) col = col * 10 + (c - '0');
                rest_start = p3 + 1;
            }
        }

        std::string_view rest = ln.substr(rest_start);
        while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);

        Diag::Sev sev = Diag::Error;
        std::string_view sev_tag;
        if      (rest.starts_with("error:"))         { sev = Diag::Error;   sev_tag = "error:"; }
        else if (rest.starts_with("fatal error:"))   { sev = Diag::Error;   sev_tag = "fatal error:"; }
        else if (rest.starts_with("warning:"))       { sev = Diag::Warning; sev_tag = "warning:"; }
        else if (rest.starts_with("note:"))          { sev = Diag::Note;    sev_tag = "note:"; }
        else                                          return std::nullopt;

        rest.remove_prefix(sev_tag.size());
        while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);

        return Diag{
            std::string{path}, linum, col, sev, std::string{rest},
        };
    }

    // ── FileRead: head + line gutter (+ optional highlight_lines) ─────────
    //
    // File reads are head-heavy: the assistant just opened a file and the
    // user wants to confirm it's the right one.  Showing 5 leading lines
    // with a line-number gutter anchors "yes, that's the file."
    //
    // When the caller supplies `highlight_lines` (typically the line
    // numbers a previous Grep flagged on the same path), we
    //   1. emit a leading `▸ matches: 1, 42, 61` summary header row so the
    //      reader knows the relevant lines even if they're outside the
    //      rendered head, and
    //   2. for any highlighted line that DOES fall in the rendered range,
    //      replace the gutter's leading space with `▸` and brighten the
    //      code style — so the eye lands on it immediately.
    //
    // The summary row ALWAYS appears when highlight_lines is non-empty,
    // because the most common case in a long real file is that the
    // matches don't all fit in the head budget; without the summary the
    // reader would never know lines 42 and 61 exist.
    [[nodiscard]] Element file_read() const {
        using namespace dsl;
        if (cfg_.text.empty()) {
            return cfg_.is_streaming ? placeholder_or_blank("reading file")
                                     : blank();
        }
        const auto p = elide(cfg_.text, cfg_.read_head, /*tail=*/0);
        const auto syn = syntax_lines(p.lines);

        // Gutter (line numbers) dim; pipe carries the category band.
        const Style gutter_st = Style{}.with_fg(muted());
        const Style pipe_st   = Style{}.with_fg(cfg_.chrome_color);
        const Style code_st   = Style{}.with_fg(cfg_.text_color);
        // Highlighted lines: arrow gutter + bright code.  We use a brighter
        // fg via the with_bold() weight rather than a separate accent
        // colour so the rest of the body's tonal balance is preserved.
        const Style hi_arrow_st = Style{}.with_fg(accent()).with_bold();
        const Style hi_code_st  = Style{}.with_fg(cfg_.text_color).with_bold();

        std::vector<Element> rows;
        rows.reserve(p.lines.size() + 2);

        // 1. Optional matches header.
        if (!cfg_.highlight_lines.empty())
            rows.push_back(highlight_summary_row());

        // 2. Gutter rows. hidden_above offsets past the tail_only drop
        //    so the numbers stay anchored to the file's real lines.
        int line_num = cfg_.start_line + p.hidden_above;
        for (int i = 0; i < static_cast<int>(p.lines.size()); ++i) {
            std::string_view ln = p.lines[std::size_t(i)];
            const bool hi = cfg_.highlight_lines.count(line_num) > 0;

            // Gutter: "▸42 │ " when highlighted, " 42 │ " otherwise.  Both
            // shapes are exactly the same display width so consecutive
            // rows stay column-aligned regardless of which are flagged.
            char numbuf[8];
            std::snprintf(numbuf, sizeof(numbuf), "%3d", line_num);
            const std::size_t numlen = std::char_traits<char>::length(numbuf);
            constexpr std::string_view kArrow = "\xe2\x96\xb8";   // ▸  (3 bytes, 1 col)
            constexpr std::string_view kSpace = " ";

            std::string content;
            content.reserve(numlen + 8 + ln.size());
            const std::size_t lead_off = content.size();
            content += hi ? kArrow : kSpace;
            const std::size_t lead_len = hi ? kArrow.size() : kSpace.size();
            const std::size_t num_off = content.size();
            content += numbuf;
            const std::size_t pipe_off = content.size();
            content += " \xe2\x94\x82 ";   // " │ "  (5 bytes)

            std::vector<StyledRun> runs;
            runs.push_back({lead_off, lead_len, hi ? hi_arrow_st : gutter_st});
            runs.push_back({num_off,  numlen,   gutter_st});
            runs.push_back({pipe_off, 5,        pipe_st});
            const std::size_t code_off = content.size();
            content += ln;
            if (!ln.empty()) {
                for (auto r : paint_runs(ln, hi ? hi_code_st : code_st,
                                         hit_style(), syn_at(syn, i))) {
                    r.byte_offset += code_off;
                    runs.push_back(r);
                }
            }

            rows.push_back(Element{TextElement{
                .content = std::move(content),
                .style   = {},
                .wrap    = TextWrap::TruncateEnd,
                .runs    = std::move(runs),
            }});
            ++line_num;
        }

        // 3. Trailing elision row.
        if (p.elided > 0) rows.push_back(elision_marker(p.elided));

        return v(rows).build();
    }

    // Summary header for FileRead's highlight_lines: `▸ matches: 1, 42, 61`.
    // Truncated to 5 line numbers + "+N more" to keep it on one line.
    [[nodiscard]] Element highlight_summary_row() const {
        constexpr int kMaxNums = 5;
        std::vector<int> nums(cfg_.highlight_lines.begin(),
                              cfg_.highlight_lines.end());
        const int total = static_cast<int>(nums.size());

        std::string content = "\xe2\x96\xb8 matches: ";
        const int shown = std::min(kMaxNums, total);
        for (int i = 0; i < shown; ++i) {
            if (i > 0) content += ", ";
            content += std::to_string(nums[std::size_t(i)]);
        }
        if (shown < total) {
            content += " +";
            content += std::to_string(total - shown);
            content += " more";
        }
        return Element{TextElement{
            .content = std::move(content),
            .style   = Style{}.with_fg(accent()).with_dim(),
            .wrap    = TextWrap::TruncateEnd,
        }};
    }

    // ── FileWrite: green add-band + lines/bytes footer ────────────────────
    //
    // For a Write tool the BYTE COUNT is a big part of why the user looks at
    // the body — the path is in the timeline header, this is a brand-new
    // file. The body renders as a clearly green "add" diff band (same visual
    // language as a +-side hunk) so a Write reads as one big addition, and
    // always ends with a `12 lines · 482 B` footer.
    [[nodiscard]] Element file_write() const {
        using namespace dsl;
        if (cfg_.text.empty()) {
            return cfg_.is_streaming ? placeholder_or_blank("awaiting content")
                                     : blank();
        }
        // Tail-anchored preview: the user is glancing at a Write event
        // mid-stream or post-finish to confirm "yes, this looks like
        // the file I asked for" — the END of the body is what carries
        // the most signal (the trailing brace closing the function the
        // model just wrote, the final return statement, the last test
        // case in a fixture). The opening lines are usually license
        // headers and #includes that read the same across every Write.
        //
        // So: with show_all=false (default) we render the LAST
        // cfg_.code_tail lines, with a single "⋯ N more above" marker
        // on top accounting for everything that was elided. Line
        // numbers reflect true positions in the file (e.g. 198, 199,
        // 200 for a 200-line write) so the slice doesn't read as if
        // it started at line 1.
        //
        // show_all=true keeps the full numbered render for callers
        // that want it (no elision, line numbers from 1).
        const auto p = elide(cfg_.text, /*head=*/0, cfg_.code_tail);

        // Render the body as a green "add" diff band so a Write event reads
        // in the same visual language as a +-side hunk in git_diff /
        // edit_diff. The whole-file write is conceptually a single large
        // addition; matching palette keeps the timeline coherent when a Write
        // sits next to an Edit or a git_diff. The band is a DARK, saturated
        // green with BRIGHT same-hue green text on top (GitHub-dark-diff
        // style) — the wide luminance gap reads with strong contrast even on a
        // phone / low-gamma SSH screen, and the dark band keeps its green hue
        // instead of washing out to grey the way the old pale bands did. The
        // band is DARK but highly SATURATED (low red/blue) — on a low-gamma
        // screen it's channel separation, not lightness, that carries hue, so
        // a dark-but-greyish band reads grey while a dark-but-vivid one reads
        // green.
        // The band is the THEME's diff_added, not a literal. A hardcoded
        // green here is the widget stating its own palette over the user's:
        // under theme::native (Named/Default only, so the terminal's own
        // colours reach the screen) this painted GitHub-dark greens on top
        // of whatever they had chosen. Every scheme already carries a
        // contrast-checked diff_added, and native maps it to the plain
        // green every terminal has meant for forty years.
        const Color add_bg    = diff_palette::add_bg;
        const Color add_fg_br = diff_palette::add_fg_hi;
        const Color num_fg    = diff_palette::add_fg;

        // Line-number gutter rides the same green band as the code so the
        // whole row is one solid rectangle (no dim stripe cutting through);
        // the number reads in a medium green, the code in bright green.
        // 16-color fallback: plain green fg, no band (see diff_bands_ok).
        const bool  bands   = diff_bands_ok();
        const Style num_st  = bands
            ? Style{}.with_bg(add_bg).with_fg(diff_palette::add_fg_hi).with_bold()
            : Style{}.with_fg(Color::slot(ThemeSlot::Success)).with_dim();
        // Same weight as edit_diff's `+` side: plain add_fg on the band, no
        // bold. A write is one big addition, so it should read identically.
        const Style code_st = bands
            ? Style{}.with_bg(add_bg).with_fg(diff_palette::add_fg)
            : Style{}.with_fg(Color::slot(ThemeSlot::Success));
        // Needle hits brighten + bold inside the band; bg stays put.
        const Style hit_st = bands
            ? Style{}.with_bg(add_bg).with_fg(Color::hex(0xFFFFFF)).with_bold()
            : Style{}.with_fg(Color::slot(ThemeSlot::Text)).with_bold();

        std::vector<Element> rows;
        rows.reserve(p.lines.size() + 2);

        // Elision marker on top — the rendered tail starts at line
        // (elided + 1) of the source. Pushed BEFORE the line loop so
        // the visual order is "⋯ N more above" then the tail rows.
        if (p.elided > 0) rows.push_back(elision_marker(p.elided));

        // Base at cfg_.start_line so a HOST-SIDE tail slice (write_body
        // streams only the last N lines of the content written so far)
        // still numbers rows with their TRUE position in the file being
        // written, not a restart at 1. Default start_line=1 keeps the
        // classic behaviour for full bodies.
        int line_num = cfg_.start_line + p.elided + p.hidden_above;
        const auto syn = syntax_lines(p.lines);
        for (std::size_t i = 0; i < p.lines.size(); ++i) {
            char numbuf[8];
            std::snprintf(numbuf, sizeof(numbuf), "%3d ", line_num++);
            if (bands) {
                rows.push_back(band_row_hl(numbuf, num_st,
                                           std::string{p.lines[i]}, code_st,
                                           hit_st, add_bg, syn_at(syn, int(i))));
            } else {
                std::string out = numbuf;
                const std::size_t gl = out.size();
                out += p.lines[i];
                std::vector<StyledRun> runs;
                runs.push_back({0, gl, num_st});
                for (auto r : paint_runs(p.lines[i], code_st, hit_st,
                                         syn_at(syn, int(i)))) {
                    r.byte_offset += gl;
                    runs.push_back(r);
                }
                rows.push_back(Element{TextElement{
                    .content = std::move(out),
                    .style   = {},
                    .wrap    = TextWrap::TruncateEnd,
                    .runs    = std::move(runs),
                }});
            }
        }

        if (cfg_.show_footer_stats) {
            // Trailing summary row — italic, chrome-colored, NOT part of
            // the marker contract (different visual role from per-row
            // markers; this is a post-content fact about the file).
            const int total = count_lines(cfg_.text);
            char buf[64];
            std::snprintf(buf, sizeof(buf), "    %d line%s \xc2\xb7 %s",
                          total, total == 1 ? "" : "s",
                          format_bytes(cfg_.text.size()).c_str());
            rows.push_back(dsl::text(std::string{buf},
                Style{}.with_fg(cfg_.chrome_color).with_italic()).build());
        }

        return v(rows).build();
    }

    // ── Diff-band capability gate ──
    // The dark, saturated RGB row bands (GitHub-dark-diff style) are a
    // truecolor/256 design. On a 16-color terminal (TERM=xterm over SSH,
    // vt100, linux console) the emit-time degrade quantizes the dark
    // vivid bg to the nearest ANSI cell — FULL bright green / red / blue
    // blocks with washed-out text: the "garish solid bands over SSH"
    // report. There is no 16-color bg that reads as a subtle band, so
    // below 256 colors we drop the bands entirely and fall back to the
    // classic fg-colored diff (green/red +/- text, blue @@ headers) —
    // exactly what `git diff` itself looks like on such terminals.
    //
    // This is a TERMINAL question only. It briefly also asked the theme and
    // turned the bands off under native — the wrong fix for a real bug. The
    // bug was the INK: native's diff_added resolves to the same ANSI green
    // as success, so the band painted green text on a green block. The band
    // was never the problem, and dropping it threw away the whole visual
    // language of a diff. Both sides of every band are now stated together
    // in diff_palette, which is the only way to guarantee the pair.
public:
    // Test hook: a test needs to tell "the colours are right" from "no band
    // ever drew", and those look identical from the outside.
    [[nodiscard]] static bool diff_bands_ok_for_test() noexcept {
        return diff_bands_ok();
    }
private:
    [[nodiscard]] static bool diff_bands_ok() noexcept {
        return terminal_color_level() >= 2;
    }

    // Helper: build a styled-text row with NoWrap so AgentTimeline's per-
    // child stripe contract (1 visible row per child) holds.
    [[nodiscard]] static Element text_row(std::string content, Style st) {
        return Element{TextElement{
            .content = std::move(content),
            .style   = st,
            .wrap    = TextWrap::TruncateEnd,
        }};
    }

    // ── Full-width diff band ──────────────────────────────────────────────
    // Render `gutter + body` as ONE solid colored rectangle that fills the
    // row to the card's right edge. A width-aware component pads the line
    // with bg-colored spaces to the width the layout hands it, so the band
    // has no ragged tail and no gap where the old ` │ ` separator used to
    // punch a hole through the tint.
    //
    // Why a component (vs a plain padded TextElement): the renderer paints a
    // TextElement's per-run styles and IGNORES the base style when runs are
    // present, and a static row can't know the terminal width to pad to. The
    // component receives its allocated width at paint time; we then tile the
    // whole line with CONTIGUOUS bg-carrying runs (gutter → body → pad) so
    // every cell — glyph, gap, and trailing fill — shares the band bg. grow
    // makes AgentTimeline's `h(connector, row)` stretch the band across the
    // remaining width after the tree connector.
    [[nodiscard]] Element band_row(std::string gutter, Style gutter_st,
                                   std::string body, Style body_st,
                                   Color bg) const {
        using namespace dsl;
        return component(
            [gutter = std::move(gutter), gutter_st,
             body = std::move(body), body_st, bg](int w, int) -> Element {
                std::string out = gutter + body;
                const int used  = string_width(out);
                // Clamp pathological widths (the layout's kUnconstrained
                // sentinel) back to content width so we never allocate a
                // multi-thousand-space pad.
                const int width = (w > 0 && w < 2000) ? w : used;
                if (used < width)
                    out.append(static_cast<std::size_t>(width - used), ' ');

                const std::size_t gl = gutter.size();
                const std::size_t bl = body.size();
                std::vector<StyledRun> runs;
                runs.reserve(3);
                if (gl) runs.push_back({0, gl, gutter_st});
                if (bl) runs.push_back({gl, bl, body_st});
                if (out.size() > gl + bl)
                    runs.push_back({gl + bl, out.size() - gl - bl,
                                    Style{}.with_bg(bg)});

                return Element{TextElement{
                    .content = std::move(out),
                    .style   = Style{}.with_bg(bg),
                    .wrap    = TextWrap::TruncateEnd,
                    .runs    = std::move(runs),
                }};
            }).grow(1.0f);
    }

    using Changed = DiffChange;

    // Same as band_row, but the body gets syntax colours (`syn`) and needle
    // hits painted over it. Both only change fg/weight, never the bg, so the
    // band stays solid.
    [[nodiscard]] Element band_row_hl(std::string gutter, Style gutter_st,
                                      std::string body, Style body_st,
                                      Style hit_st, Color bg,
                                      const std::vector<StyledRun>* syn = nullptr,
                                      Changed chg = {}) const {
        using namespace dsl;
        auto body_runs = paint_runs(body, body_st, hit_st, syn, chg);
        if (body_runs.empty()
            || (body_runs.size() == 1 && body_runs[0].style == body_st))
            return band_row(std::move(gutter), gutter_st,
                            std::move(body), body_st, bg);
        return component(
            [gutter = std::move(gutter), gutter_st,
             body = std::move(body), bg,
             body_runs = std::move(body_runs)](int w, int) -> Element {
                std::string out = gutter + body;
                const int used  = string_width(out);
                const int width = (w > 0 && w < 2000) ? w : used;
                if (used < width)
                    out.append(static_cast<std::size_t>(width - used), ' ');

                const std::size_t gl = gutter.size();
                const std::size_t bl = body.size();
                std::vector<StyledRun> runs;
                runs.reserve(body_runs.size() + 2);
                if (gl) runs.push_back({0, gl, gutter_st});
                for (auto r : body_runs) {
                    r.byte_offset += gl;
                    runs.push_back(r);
                }
                if (out.size() > gl + bl)
                    runs.push_back({gl + bl, out.size() - gl - bl,
                                    Style{}.with_bg(bg)});

                return Element{TextElement{
                    .content = std::move(out),
                    .style   = Style{}.with_bg(bg),
                    .wrap    = TextWrap::TruncateEnd,
                    .runs    = std::move(runs),
                }};
            }).grow(1.0f);
    }

    // ── Syntax colouring for code rows ───────────────────────────────────────
    using LineRuns = std::vector<std::vector<StyledRun>>;

    // A file path with the directory receding and the filename standing
    // out, so the eye lands on the file: `src/runtime/` + **turn.cpp**.
    [[nodiscard]] static Element path_row(std::string_view path,
                                          std::string_view prefix = {}) {
        std::string out{prefix};
        const auto slash = path.find_last_of('/');
        const std::size_t cut = slash == std::string_view::npos ? 0 : slash + 1;
        const std::size_t p0 = out.size();
        out += path;
        std::vector<StyledRun> runs;
        if (p0) runs.push_back({0, p0, Style{}.with_fg(muted())});
        if (cut) runs.push_back({p0, cut, Style{}.with_fg(accent()).with_dim()});
        runs.push_back({p0 + cut, path.size() - cut,
                        Style{}.with_fg(accent()).with_bold()});
        return Element{TextElement{
            .content = std::move(out),
            .style   = {},
            .wrap    = TextWrap::TruncateEnd,
            .runs    = std::move(runs),
        }};
    }

    // WCAG relative luminance / contrast ratio.
    static double rel_lum(int r, int g, int b) {
        auto f = [](int v) {
            const double s = v / 255.0;
            return s <= 0.03928 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
        };
        return 0.2126 * f(r) + 0.7152 * f(g) + 0.0722 * f(b);
    }

    // `fg` adjusted so it reaches `target` contrast on `bg`: same hue and
    // saturation, lightness moved away from the background (up on dark,
    // down on light). Unchanged when it already reads, or when either
    // colour is a palette entry we can't measure.
    static Color readable_on(Color fg, Color bg, double target) {
        const auto& th = theme::live();
        const auto f = th.resolve(fg);
        const auto b = th.resolve(bg);
        if (!f.has_channels() || !b.has_channels()) return fg;
        const double lb = rel_lum(b.r(), b.g(), b.b());
        auto ratio = [&](double lf) {
            return lf > lb ? (lf + 0.05) / (lb + 0.05) : (lb + 0.05) / (lf + 0.05);
        };
        if (ratio(rel_lum(f.r(), f.g(), f.b())) >= target) return fg;

        // rgb -> hsl
        const double r = f.r() / 255.0, g = f.g() / 255.0, bl = f.b() / 255.0;
        const double mx = std::max({r, g, bl}), mn = std::min({r, g, bl});
        double h = 0, s = 0, l = (mx + mn) / 2;
        if (mx != mn) {
            const double d = mx - mn;
            s = l > 0.5 ? d / (2 - mx - mn) : d / (mx + mn);
            if (mx == r)      h = (g - bl) / d + (g < bl ? 6 : 0);
            else if (mx == g) h = (bl - r) / d + 2;
            else              h = (r - g) / d + 4;
            h /= 6;
        }
        auto hue = [](double p, double q, double t) {
            if (t < 0) t += 1;
            if (t > 1) t -= 1;
            if (t < 1.0 / 6) return p + (q - p) * 6 * t;
            if (t < 0.5)     return q;
            if (t < 2.0 / 3) return p + (q - p) * (2.0 / 3 - t) * 6;
            return p;
        };
        auto to_rgb = [&](double L) {
            if (s == 0) {
                const auto v = static_cast<uint8_t>(L * 255 + 0.5);
                return Color::rgb(v, v, v);
            }
            const double q = L < 0.5 ? L * (1 + s) : L + s - L * s;
            const double p = 2 * L - q;
            return Color::rgb(static_cast<uint8_t>(hue(p, q, h + 1.0 / 3) * 255 + 0.5),
                              static_cast<uint8_t>(hue(p, q, h) * 255 + 0.5),
                              static_cast<uint8_t>(hue(p, q, h - 1.0 / 3) * 255 + 0.5));
        };
        const bool dark_bg = lb < 0.18;
        Color out = fg;
        for (int step = 0; step < 20; ++step) {
            l = dark_bg ? std::min(0.95, l + 0.03) : std::max(0.05, l - 0.03);
            const auto c = to_rgb(l);
            out = c;
            if (ratio(rel_lum(c.r(), c.g(), c.b())) >= target) break;
            if (l <= 0.05 || l >= 0.95) break;
        }
        return out;
    }

    // A plain (no band) row: gutter, then code with syntax + hit colours.
    [[nodiscard]] Element code_row(std::string gutter, Style gutter_st,
                                   std::string_view code, Style base,
                                   Style hit,
                                   const std::vector<StyledRun>* syn) const {
        std::string out = std::move(gutter);
        const std::size_t gl = out.size();
        out += code;
        std::vector<StyledRun> runs;
        if (gl) runs.push_back({0, gl, gutter_st});
        for (auto r : paint_runs(code, base, hit, syn)) {
            r.byte_offset += gl;
            runs.push_back(r);
        }
        return Element{TextElement{
            .content = std::move(out),
            .style   = {},
            .wrap    = TextWrap::TruncateEnd,
            .runs    = std::move(runs),
        }};
    }

    // Tokenise the visible lines as one block, so multi-line comments and
    // strings carry across rows. Empty when cfg_.lang is unset/unknown.
    [[nodiscard]] LineRuns syntax_lines(
            const std::vector<std::string_view>& lines) const {
        if (cfg_.lang.empty() || lines.empty()) return {};
        std::string joined;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (i) joined += '\n';
            joined += lines[i];
        }
        return syntax_line_runs(joined, cfg_.lang);
    }
    [[nodiscard]] static const std::vector<StyledRun>*
    syn_at(const LineRuns& r, int i) {
        return (i >= 0 && std::size_t(i) < r.size()) ? &r[std::size_t(i)]
                                                     : nullptr;
    }

    // Theme text, bold: the searched-for literal reads white over code.
    [[nodiscard]] static Style hit_style() {
        return Style{}.with_fg(Color::slot(ThemeSlot::Text)).with_bold();
    }

    // Word-level diff of a removed line `a` against its added partner `b`:
    // strip the common prefix and suffix, snapped out to word boundaries so
    // an identifier is never split. Returns {a_off, a_len, b_off, b_len},
    // all zero when the lines are too different for it to help.
    struct WordChange { std::size_t ao = 0, al = 0, bo = 0, bl = 0; };
    [[nodiscard]] static WordChange word_change(std::string_view a,
                                                std::string_view b) {
        auto word = [](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '_'
                || (static_cast<unsigned char>(c) & 0x80);
        };
        std::size_t pre = 0;
        while (pre < a.size() && pre < b.size() && a[pre] == b[pre]) ++pre;
        std::size_t suf = 0;
        while (suf < a.size() - pre && suf < b.size() - pre
               && a[a.size() - 1 - suf] == b[b.size() - 1 - suf]) ++suf;
        // Snap the prefix back to a word start, the suffix forward to a word end.
        while (pre > 0 && word(a[pre - 1]) && pre < a.size() && word(a[pre])) --pre;
        while (pre > 0 && pre <= b.size() && word(b[pre - 1]) && pre < b.size() && word(b[pre])) --pre;
        auto snap_suf = [&](std::string_view s) {
            while (suf > 0 && word(s[s.size() - suf]) && s.size() - suf > 0
                   && word(s[s.size() - suf - 1])) --suf;
        };
        snap_suf(a); snap_suf(b);
        if (pre + suf > a.size()) suf = a.size() - pre;
        if (pre + suf > b.size()) suf = b.size() - pre;
        WordChange w{pre, a.size() - pre - suf, pre, b.size() - pre - suf};
        // Only worth showing when part of the line stayed the same.
        const std::size_t kept = pre + suf;
        const std::size_t lead = std::min(a.find_first_not_of(' '), a.size());
        if (kept <= lead + 1 || (w.al == 0 && w.bl == 0)) return {};
        if (w.al * 10 > a.size() * 7 && w.bl * 10 > b.size() * 7) return {};
        return w;
    }

    // Runs for one line of code: syntax colours over `base`, then needle
    // hits on top. Token fg wins, but bg/dim/bold from `base` are kept so a
    // diff band stays solid. `hit` likewise only changes fg + bold. The
    // `chg` range (word-level diff) gets its own, brighter bg.
    [[nodiscard]] std::vector<StyledRun> paint_runs(
            std::string_view line, Style base, Style hit,
            const std::vector<StyledRun>* syn, Changed chg = {}) const {
        const std::size_t n = line.size();
        std::vector<Style> per;   // style per byte, only built when needed
        const auto spans = needle_spans(line);
        if ((!syn || syn->empty()) && spans.empty() && chg.len == 0)
            return n ? std::vector<StyledRun>{{0, n, base}}
                     : std::vector<StyledRun>{};
        per.assign(n, base);
        const std::size_t ce = std::min(n, chg.off + chg.len);
        auto in_chg = [&](std::size_t k) { return k >= chg.off && k < ce; };
        for (std::size_t k = chg.off; k < ce; ++k) per[k].bg = chg.bg;
        // Keep each token's theme colour when it already reads on its
        // background; otherwise raise only its lightness (saturation kept)
        // until it does. Diff bands want more contrast than the panel.
        const Color against = base.bg ? *base.bg
                                      : Color::slot(ThemeSlot::Background);
        const double target = base.bg ? 6.0 : 4.5;
        auto lift = [&](Color c, std::size_t k) {
            return readable_on(c, in_chg(k) ? chg.bg : against, target);
        };
        if (syn) {
            for (const auto& r : *syn) {
                const std::size_t e = std::min(n, r.byte_offset + r.byte_length);
                for (std::size_t k = r.byte_offset; k < e; ++k) {
                    Style s = per[k];
                    if (r.style.fg) s.fg = lift(*r.style.fg, k);
                    s.italic = r.style.italic;
                    s.bold   = base.bold || r.style.bold;
                    per[k] = s;
                }
            }
        }
        for (auto [off, len] : spans)
            for (std::size_t k = off; k < std::min(n, off + len); ++k) {
                Style s = per[k];
                s.fg = hit.fg; s.bold = true; s.dim = false; s.italic = false;
                per[k] = s;
            }
        std::vector<StyledRun> out;
        for (std::size_t k = 0; k < n;) {
            std::size_t j = k + 1;
            while (j < n && per[j] == per[k]) ++j;
            out.push_back({k, j - k, per[k]});
            k = j;
        }
        return out;
    }

    // Byte spans of every case-insensitive occurrence of cfg_.match_needle in
    // `hay`. Empty when no needle is set, so callers pay nothing by default.
    // match_needle may hold SEVERAL alternatives separated by '\n' (a literal
    // alternation like `foo|bar` split host-side); each is matched and the
    // leftmost-longest wins, so spans never overlap.
    [[nodiscard]] std::vector<std::pair<std::size_t, std::size_t>>
    needle_spans(std::string_view hay) const {
        std::vector<std::pair<std::size_t, std::size_t>> out;
        if (cfg_.match_needle.empty() || hay.empty()) return out;
        auto fold = [](char c) -> char {
            return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
        };
        auto matches_at = [&](std::string_view n, std::size_t i) {
            if (i + n.size() > hay.size()) return false;
            for (std::size_t k = 0; k < n.size(); ++k)
                if (fold(hay[i + k]) != fold(n[k])) return false;
            return true;
        };

        const std::string_view all{cfg_.match_needle};
        for (std::size_t i = 0; i < hay.size();) {
            std::size_t best = 0;
            for (std::size_t s = 0; s <= all.size();) {
                const auto e = all.find('\n', s);
                const auto n = all.substr(s, (e == std::string_view::npos)
                                                 ? std::string_view::npos
                                                 : e - s);
                if (!n.empty() && n.size() > best && matches_at(n, i))
                    best = n.size();
                if (e == std::string_view::npos) break;
                s = e + 1;
            }
            if (best > 0) {
                out.push_back({i, best});
                i += best;
            } else {
                ++i;
            }
        }
        return out;
    }

    // ── EditDiff: per-hunk header + −/+ lines with head+tail per side ─────
    [[nodiscard]] Element edit_diff() const {
        using namespace dsl;
        if (cfg_.hunks.empty()) return blank();
        if (cfg_.is_streaming) return edit_diff_streaming();

        const int total_hunks = static_cast<int>(cfg_.hunks.size());
        const int shown = cfg_.show_all
            ? total_hunks
            : std::min(total_hunks, cfg_.max_edit_hunks_shown);

        std::vector<Element> rows;
        rows.reserve(static_cast<std::size_t>(shown) * 12);

        for (int i = 0; i < shown; ++i) {
            const auto& h = cfg_.hunks[static_cast<std::size_t>(i)];
            const int minus = count_lines(h.old_text);
            const int plus  = count_lines(h.new_text);

            // Per-hunk header. For multi-hunk edits we tag with
            // `edit i/N  ·  −2 / +3` so the user can locate which
            // splice they're looking at. For single-hunk edits the
            // `edit 1/1` tag would be pure noise, so we keep just the
            // stat chip (`−2 / +3`) — still useful at a glance, gone
            // are the days where you have to count rows to know the
            // net edit size.
            std::string header;
            if (total_hunks > 1) {
                // "edit i/N" only when SETTLED (streaming routed to
                // edit_diff_streaming above). While streaming, hunks
                // arrive one by one, so N ticks 2→3→4 — but an earlier
                // hunk's header row may already sit in native scrollback
                // (immutable) as later hunks grew the card; rewriting its
                // "/N" is a committed-row rewrite → the host renderer's
                // scrollback gate → destructive HardReset. The full
                // indexed form appears only on the settled re-render,
                // which paints below the seam.
                header  = "edit " + std::to_string(i + 1) + "/"
                       + std::to_string(total_hunks) + "  \xc2\xb7  ";
            }
            header += "\xe2\x88\x92" + std::to_string(minus)
                   + " / +" + std::to_string(plus);
            // Hunk header sits between the bg-tinted - and + sides; give
            // it its own subtle slate band so the diff region reads as a
            // continuous tri-color stack (slate header → red removes →
            // green adds) instead of a jagged "tint, blank, tint" strip.
            // Full-width band so the header rule spans the same rectangle
            // as the −/+ sides below it. 16-color fallback: plain dim
            // blue text, no band (see push_diff_side).
            if (diff_bands_ok()) {
                // Slots, not literals: see the diff-band note above.
                const Color hdr_bg = diff_palette::hunk_bg;
                const Color hdr_fg = diff_palette::hunk_fg;
                rows.push_back(band_row("   ", Style{}.with_bg(hdr_bg).with_fg(diff_palette::hunk_fg),
                    std::move(header),
                    Style{}.with_bg(hdr_bg).with_fg(diff_palette::hunk_fg).with_bold(), hdr_bg));
            } else {
                rows.push_back(text_row("   " + header,
                    Style{}.with_fg(Color::slot(ThemeSlot::Primary)).with_bold()));
            }

            push_diff_side(rows, h.old_text, '-', danger());
            push_diff_side(rows, h.new_text, '+', success());
        }

        if (shown < total_hunks) {
            rows.push_back(text(
                "\xe2\x8b\xaf " + std::to_string(total_hunks - shown) + " more edits",
                Style{}.with_fg(muted()).with_italic()
            ).build());
        }

        if (rows.empty()) return blank();
        return v(rows).build();
    }

    // ── EditDiff, STREAMING: pinned chip + row-level live tail ───────────
    //
    // While the edit streams, hunks grow line-by-line and NEW hunks pop
    // into the args array mid-token with (almost) no content. Windowing
    // the preview to the newest hunk therefore made the card height jump:
    // the instant a fresh hunk opened, the body collapsed to the chip row
    // (zero diff rows — the card visibly shrank upwards) and then re-grew
    // as its text arrived. Instead:
    //
    //   * ONE chip row pinned at the top — "edit N · −a / +b", the stats
    //     of the hunk currently landing (stream_hunk_no ordinal).
    //   * A LIVE TAIL of the last K diff rows across ALL hunks. The
    //     window fills from whatever content exists anywhere in the edit,
    //     so it stays full across hunk boundaries: height is
    //     monotonically non-decreasing (grows to 1+K, then constant) and
    //     the user always watches the newest K rows stream past — older
    //     hunks' rows scroll out of the window as new ones land.
    //
    // K = 2×edit_tail_per_side, which the host derives from the terminal
    // height so 1+K ≤ the streaming body budget — the event HEADER row
    // stays inside the viewport while the card streams (the committed-
    // row-rewrite invariant the scrollback oracle proves).
    [[nodiscard]] Element edit_diff_streaming() const {
        using namespace dsl;
        const int K = std::max(2, cfg_.edit_tail_per_side * 2);

        // Chip: ordinal + stats of the NEWEST hunk (the one landing).
        const auto& last = cfg_.hunks.back();
        std::string header;
        if (cfg_.stream_hunk_no > 0)
            header = "edit " + std::to_string(cfg_.stream_hunk_no)
                   + "  \xc2\xb7  ";
        header += "\xe2\x88\x92" + std::to_string(count_lines(last.old_text))
               + " / +" + std::to_string(count_lines(last.new_text));
        // Slots, not literals: see the diff-band note above.
        const Color hdr_bg = diff_palette::hunk_bg;
        const Color hdr_fg = diff_palette::hunk_fg;
        const bool  bands  = diff_bands_ok();

        // Find the earliest hunk that can still contribute to the last-K
        // window (each side contributes at most K rows after its per-side
        // tail elide) so we only build O(K) elements, not O(total hunks).
        std::size_t first = cfg_.hunks.size();
        int have = 0;
        while (first > 0 && have < K) {
            const auto& h = cfg_.hunks[first - 1];
            have += std::min(count_lines(h.old_text), K)
                  + std::min(count_lines(h.new_text), K);
            --first;
        }

        std::vector<Element> body;
        body.reserve(static_cast<std::size_t>(K) * 2);
        for (std::size_t i = first; i < cfg_.hunks.size(); ++i) {
            const auto& h = cfg_.hunks[i];
            push_diff_side(body, h.old_text, '-', danger(),  K);
            push_diff_side(body, h.new_text, '+', success(), K);
        }
        if (static_cast<int>(body.size()) > K)
            body.erase(body.begin(),
                       body.end() - static_cast<std::ptrdiff_t>(K));

        std::vector<Element> rows;
        rows.reserve(body.size() + 1);
        if (bands)
            rows.push_back(band_row("   ",
                Style{}.with_bg(hdr_bg).with_fg(diff_palette::hunk_fg),
                std::move(header),
                Style{}.with_bg(hdr_bg).with_fg(diff_palette::hunk_fg).with_bold(), hdr_bg));
        else
            rows.push_back(text_row("   " + header,
                Style{}.with_fg(Color::slot(ThemeSlot::Primary)).with_bold()));
        for (auto& e : body) rows.push_back(std::move(e));
        return v(rows).build();
    }

    // Emit a diff-side (removed or added) as full-width solid bands: a
    // leading sign gutter (` - ` / ` + `, bold in the diff hue) followed by
    // the line content, the whole row painted as one colored rectangle that
    // reaches the card's right edge. Matches git_diff's palette so a
    // multi-hunk Edit and a unified git_diff read in the same visual
    // language.
    // `tail_override >= 0` bypasses the cfg elision profile and keeps just
    // the last N lines of the side, marker-free — the streaming live-tail
    // path slices at ROW level afterwards, so per-side elision only needs
    // to cap the element count, never to annotate what it dropped.
    void push_diff_side(std::vector<Element>& rows, std::string_view body,
                        char marker, Color c, int tail_override = -1) const {
        using namespace dsl;
        if (body.empty()) return;
        ElidedPreview p;
        if (tail_override >= 0) {
            p = head_tail(body, 0, tail_override);
            p.elision_at = -1;   // suppress "⋯ N more" — row-tail follows
            p.elided     = 0;
        } else {
            p = elide(body, cfg_.edit_head_per_side,
                            cfg_.edit_tail_per_side);
        }

        const bool is_add = (marker == '+');
        // Dark, highly SATURATED green / red bands with BRIGHT same-hue text
        // on top (GitHub-dark-diff style) so the change reads with a wide
        // luminance gap even on a phone / low-gamma SSH screen. The band is
        // dark but vivid (low off-channels) — on a low-gamma screen it's
        // channel separation, not lightness, that carries hue, so a greyish
        // dark band reads grey while a vivid dark one reads green/red. The
        // " + " / " - " gutter rides a brighter, even more saturated rail of
        // the same hue so the change marker pops like a GitHub / GitLab line
        // indicator.
        //
        // 16-color fallback (diff_bands_ok false): no bg at all — classic
        // fg-colored ± lines, like `git diff` on the same terminal. The
        // degrade would otherwise turn the dark band into a FULL bright
        // green/red block per row (the "garish bands over SSH" report).
        const bool bands = diff_bands_ok();
        // Slots, not literals. These were the last hardcoded diff colours in
        // the file — the Edit path, which I missed when the git_diff and
        // Write paths were done, because Themed only gates Config FIELDS
        // and these are locals.
        // Slots, not literals — the Edit path, missed when the git_diff and
        // Write paths were done because Themed only gates Config FIELDS and
        // these are locals. This is the band in agentty #45's screenshots.
        const Color bg      = is_add ? diff_palette::add_bg   : diff_palette::rem_bg;
        const Color rail_bg = is_add ? diff_palette::add_rail : diff_palette::rem_rail;
        const Color fg_br   = is_add ? diff_palette::add_fg   : diff_palette::rem_fg;
        const Color fg_hi   = is_add ? diff_palette::add_fg_hi: diff_palette::rem_fg_hi;
        (void)c;   // c was the legacy fg; kept in signature for callers

        const Style sign_st = bands
            ? Style{}.with_bg(rail_bg).with_fg(fg_hi).with_bold()
            : Style{}.with_fg(is_add ? Color::slot(ThemeSlot::Success) : Color::slot(ThemeSlot::Error)).with_bold();
        const Style body_st = bands
            ? Style{}.with_bg(bg).with_fg(fg_br)
            : Style{}.with_fg(is_add ? Color::slot(ThemeSlot::Success) : Color::slot(ThemeSlot::Error));
        // Needle hits stay INSIDE the band: brighten + bold the span, never
        // change its bg, so the red/green rectangle stays unbroken.
        const Style hit_st = bands
            ? Style{}.with_bg(bg).with_fg(Color::hex(0xFFFFFF)).with_bold()
            : Style{}.with_fg(Color::slot(ThemeSlot::Text)).with_bold();

        std::string gutter = " ";
        gutter += marker;
        gutter += " ";          // " + " / " - " — 3-col sign gutter
        const auto syn = syntax_lines(p.lines);

        for (int i = 0; i < static_cast<int>(p.lines.size()); ++i) {
            if (i == p.elision_at && p.elided > 0)
                rows.push_back(elision_marker(p.elided));
            if (bands) {
                rows.push_back(band_row_hl(gutter, sign_st,
                                           std::string{p.lines[std::size_t(i)]},
                                           body_st, hit_st, bg, syn_at(syn, i)));
            } else {
                // Plain fg row: " + content" — sign gutter + body, no band.
                std::string out = gutter;
                out += p.lines[std::size_t(i)];
                std::vector<StyledRun> runs;
                runs.push_back({0, gutter.size(), sign_st});
                for (auto r : paint_runs(p.lines[std::size_t(i)], body_st,
                                         hit_st, syn_at(syn, i))) {
                    r.byte_offset += gutter.size();
                    runs.push_back(r);
                }
                rows.push_back(Element{TextElement{
                    .content = std::move(out),
                    .style   = {},
                    .wrap    = TextWrap::TruncateEnd,
                    .runs    = std::move(runs),
                }});
            }
        }
    }

    // ── GitDiff: unified diff through the uniform row contract.
    //    Marker carries the diff signal (`-`/`+`/` ` in red/green/chrome);
    //    content stays in the dim body tier. `+++`/`---`/`diff` header
    //    lines + `@@` hunk separators render through make_row with a `~`
    //    marker in muted so they read as section breaks.
    //
    //    Diff coloring: a dark, saturated green / red whole-line BACKGROUND
    //    band with BRIGHT same-hue text on top (GitHub-dark-diff style) carries
    //    the change signal with a wide luminance gap, so it reads at a glance
    //    even on a phone / low-gamma SSH screen while the dark band keeps its
    //    hue instead of washing to grey. The +/- gutter rail is a brighter,
    //    more saturated shade of the same hue so the change marker pops like a
    //    GitHub / GitLab indicator; context + file metadata stay plain (no
    //    band) so the colored bands read as changes floating over unchanged
    //    code.
    [[nodiscard]] Element git_diff() const {
        using namespace dsl;
        if (cfg_.text.empty()) {
            return cfg_.is_streaming ? placeholder_or_blank("awaiting diff")
                                     : blank();
        }
        if (cfg_.text == "no changes") return blank();

        const auto p = elide(cfg_.text, cfg_.code_head, cfg_.code_tail);

        // Dark, highly SATURATED bands with BRIGHT same-hue text so the diff
        // regions read with a wide luminance gap on a phone / low-gamma SSH
        // screen. The bands are dark but vivid (low off-channels) — on a
        // low-gamma screen it's channel separation, not lightness, that carries
        // hue, so a greyish dark band reads grey while a vivid dark one reads
        // green/red. The +/- gutter gets a brighter, even more saturated rail
        // than the body band so the change marker pops. Context + file metadata
        // get NO band (plain text).
        // THEME SLOTS, not literals — see the note on the Write path above.
        // A widget that states its own RGB paints over the user's palette,
        // and under theme::native (which states no RGB precisely so their
        // terminal's colours reach the screen) it wins every time. The rail
        // and body band share a slot; the rail separates by BOLD rather
        // than by a second invented hue, which also survives a 16-colour
        // terminal where two shades of the same green are one colour.
        const Color add_bg     = diff_palette::add_bg;
        const Color rem_bg     = diff_palette::rem_bg;
        const Color hunk_bg    = diff_palette::hunk_bg;
        const Color add_rail   = diff_palette::add_rail;
        const Color rem_rail   = diff_palette::rem_rail;
        const Color add_fg_br  = diff_palette::add_fg;
        const Color rem_fg_br  = diff_palette::rem_fg;
        const Color hunk_fg    = diff_palette::hunk_fg;

        // Bands (full-width solid rectangles) for the lines that carry the
        // diff signal: + adds, - removes, @@ hunk headers. Git plumbing
        // (diff --git / index / mode / rename chatter) is dropped, and the
        // `--- a/X` + `+++ b/X` pair collapses into ONE clean accent-tinted
        // filename header — so the colored bands read as changes floating
        // over unchanged code, the classic GitHub/delta hierarchy, with no
        // leaked `a//tmp/x` double-slash plumbing.
        //
        // 16-color fallback (diff_bands_ok false): classic fg-colored diff
        // (green/red ± text, blue @@), no backgrounds — the degrade would
        // otherwise render full bright-color blocks per row.
        const bool bands = diff_bands_ok();
        const Style add_sign_st = bands
            ? Style{}.with_bg(add_rail).with_fg(diff_palette::add_fg_hi).with_bold()
            : Style{}.with_fg(Color::slot(ThemeSlot::Success)).with_bold();
        const Style add_body_st = bands
            ? Style{}.with_bg(add_bg).with_fg(diff_palette::add_fg)
            : Style{}.with_fg(Color::slot(ThemeSlot::Success));
        const Style rem_sign_st = bands
            ? Style{}.with_bg(rem_rail).with_fg(diff_palette::rem_fg_hi).with_bold()
            : Style{}.with_fg(Color::slot(ThemeSlot::Error)).with_bold();
        const Style rem_body_st = bands
            ? Style{}.with_bg(rem_bg).with_fg(diff_palette::rem_fg)
            : Style{}.with_fg(Color::slot(ThemeSlot::Error));
        // Needle hits ride the same band, brightened + bold, so the span the
        // tool searched for pops without breaking the red/green rectangle.
        const Style add_hit_st = bands
            ? Style{}.with_bg(add_bg).with_fg(Color::hex(0xFFFFFF)).with_bold()
            : Style{}.with_fg(Color::slot(ThemeSlot::Text)).with_bold();
        const Style rem_hit_st = bands
            ? Style{}.with_bg(rem_bg).with_fg(Color::hex(0xFFFFFF)).with_bold()
            : Style{}.with_fg(Color::slot(ThemeSlot::Text)).with_bold();
        const Style hunk_st     = bands
            ? Style{}.with_fg(hunk_fg).with_bg(hunk_bg).with_bold()
            : Style{}.with_fg(Color::slot(ThemeSlot::Primary)).with_bold();
        const Style file_st     = Style{}.with_fg(accent()).with_bold();
        const Style ctx_st      = Style{}.with_fg(cfg_.text_color);

        // Strip the `a/` / `b/` diff sentinel so an ABSOLUTE path renders as
        // `/tmp/x` instead of the ugly `a//tmp/x` the prefix + leading slash
        // produce.
        auto strip_ab = [](std::string_view s) -> std::string_view {
            if (s.starts_with("a/") || s.starts_with("b/")) s.remove_prefix(2);
            return s;
        };
        std::string pending_from;   // `--- a/<from>` awaiting its `+++ b/<to>`

        // Real line numbers, tracked from `@@ -a,b +c,d @@` hunk headers:
        // '+' and context rows advance the NEW-file counter, '-' rows the
        // OLD-file counter; the gutter shows the side the row belongs to.
        // When elision drops rows mid-hunk the counters go stale, so they
        // are invalidated at the elision boundary and resume at the next
        // hunk header — numbers are only ever shown when they are TRUE.
        int  old_ln = 0, new_ln = 0;
        bool have_nums = false;
        auto parse_hunk_header = [&](std::string_view h) {
            // Standard `@@ -<a>[,b] +<c>[,d] @@`, or the compact `@@ L<n> @@`
            // anchor the replace/rewrite_structural preview bodies emit
            // (one true line number for both sides of a single-line splice).
            old_ln = new_ln = 0;
            have_nums = false;
            std::size_t i = h.find('-');
            if (i == std::string_view::npos) {
                // Try the `@@ L<n> @@` compact form.
                std::size_t k = h.find('L');
                if (k == std::string_view::npos) return;
                int n = 0; bool got = false;
                for (++k; k < h.size() && h[k] >= '0' && h[k] <= '9'; ++k) {
                    n = n * 10 + (h[k] - '0'); got = true;
                }
                if (got && n > 0) {
                    old_ln = new_ln = n;
                    have_nums = true;
                }
                return;
            }
            for (++i; i < h.size() && h[i] >= '0' && h[i] <= '9'; ++i)
                old_ln = old_ln * 10 + (h[i] - '0');
            std::size_t j = h.find('+', i);
            if (j == std::string_view::npos) return;
            for (++j; j < h.size() && h[j] >= '0' && h[j] <= '9'; ++j)
                new_ln = new_ln * 10 + (h[j] - '0');
            have_nums = old_ln > 0 && new_ln > 0;
        };
        auto num_gutter = [&](int n, char sign) -> std::string {
            char buf[16];
            if (have_nums)
                std::snprintf(buf, sizeof(buf), "%4d %c ", n, sign);
            else
                std::snprintf(buf, sizeof(buf), " %c ", sign);
            return buf;
        };

        std::vector<Element> rows;
        rows.reserve(p.lines.size() + 1);
        // Code text of every content row (sign stripped), tokenised as one
        // block so syntax colours line up by row index.
        std::vector<std::string_view> code_lines(p.lines.size());
        for (std::size_t k = 0; k < p.lines.size(); ++k) {
            const auto l = p.lines[k];
            if (l.starts_with("@@") || l.starts_with("+++ ") || l.starts_with("--- "))
                continue;
            if (!l.empty() && (l[0] == '+' || l[0] == '-' || l[0] == ' '))
                code_lines[k] = l.substr(1);
            else
                code_lines[k] = l;
        }
        const auto syn = syntax_lines(code_lines);
        // Word-level diff: pair each run of `-` lines with the `+` run right
        // after it, line by line, and mark what changed on a brighter band.
        std::vector<Changed> chg(p.lines.size());
        if (bands) {
            auto sign = [&](std::size_t k) {
                const auto l = p.lines[k];
                if (l.starts_with("+++ ") || l.starts_with("--- ") || l.empty()) return ' ';
                return l[0];
            };
            for (std::size_t k = 0; k < p.lines.size();) {
                if (sign(k) != '-') { ++k; continue; }
                std::size_t r0 = k;
                while (k < p.lines.size() && sign(k) == '-') ++k;
                std::size_t a0 = k;
                while (k < p.lines.size() && sign(k) == '+') ++k;
                const std::size_t pairs = std::min(a0 - r0, k - a0);
                for (std::size_t j = 0; j < pairs; ++j) {
                    const auto w = word_change(code_lines[r0 + j], code_lines[a0 + j]);
                    if (w.al) chg[r0 + j] = {w.ao, w.al, diff_palette::rem_rail};
                    if (w.bl) chg[a0 + j] = {w.bo, w.bl, diff_palette::add_rail};
                }
            }
        }
        for (int i = 0; i < static_cast<int>(p.lines.size()); ++i) {
            if (i == p.elision_at && p.elided > 0) {
                rows.push_back(elision_marker(p.elided));
                have_nums = false;   // counters stale across the gap
            }
            std::string_view ln = p.lines[std::size_t(i)];

            // Git plumbing we never surface — pure machine metadata that just
            // eats rows (precious on a phone). Every real diff CONTENT line
            // carries a +/-/space prefix, so a column-0 keyword here is
            // unambiguously plumbing, never code.
            if (ln.starts_with("diff ")      || ln.starts_with("index ")
             || ln.starts_with("new file")   || ln.starts_with("deleted file")
             || ln.starts_with("old mode")   || ln.starts_with("new mode")
             || ln.starts_with("similarity ")|| ln.starts_with("rename ")
             || ln.starts_with("copy ")) {
                continue;
            }
            // `--- a/X` — the "from" path. Remember it (to pair with the
            // `+++ b/Y` below) and suppress the row. Only counts as a header
            // when the path looks like a diff path; otherwise it's a removed
            // content line (e.g. a `-- sql comment`) and falls through.
            if (ln.starts_with("--- ")) {
                std::string_view rest = ln.substr(4);
                if (rest.starts_with("a/") || rest == "/dev/null") {
                    pending_from = std::string{strip_ab(rest)};
                    continue;
                }
            }
            // `+++ b/Y` — render the ONE clean filename header: a thin left
            // rule + the bare path in accent, an `old → new` form for renames,
            // a `(deleted)` tag when the target is /dev/null.
            if (ln.starts_with("+++ ")) {
                std::string_view rest = ln.substr(4);
                if (rest.starts_with("b/") || rest == "/dev/null") {
                    std::string to{strip_ab(rest)};
                    std::string label;
                    if (to == "/dev/null")
                        label = pending_from.empty()
                              ? std::string{"(deleted)"}
                              : pending_from + "  (deleted)";
                    else if (!pending_from.empty() && pending_from != to
                          && pending_from != "/dev/null")
                        label = pending_from + "  \xe2\x86\x92  " + to;
                    else
                        label = to;
                    pending_from.clear();
                    if (label.find("  \xe2\x86\x92  ") == std::string::npos)
                        rows.push_back(path_row(label, "\xe2\x96\x8f "));
                    else
                        rows.push_back(text_row("\xe2\x96\x8f " + label, file_st));
                    continue;
                }
            }

            if (ln.starts_with("@@")) {
                parse_hunk_header(ln);
                if (bands)
                    rows.push_back(band_row("   ", hunk_st, std::string{ln},
                                            hunk_st, hunk_bg));
                else
                    rows.push_back(text_row("   " + std::string{ln}, hunk_st));
            } else if (!ln.empty() && ln[0] == '+') {
                auto g = num_gutter(new_ln, '+');
                if (have_nums) ++new_ln;
                if (bands)
                    rows.push_back(band_row_hl(std::move(g), add_sign_st,
                                               std::string{ln.substr(1)},
                                               add_body_st, add_hit_st, add_bg,
                                               syn_at(syn, i), chg[std::size_t(i)]));
                else
                    rows.push_back(code_row(std::move(g), add_sign_st, ln.substr(1),
                                            add_body_st, add_hit_st, syn_at(syn, i)));
            } else if (!ln.empty() && ln[0] == '-') {
                auto g = num_gutter(old_ln, '-');
                if (have_nums) ++old_ln;
                if (bands)
                    rows.push_back(band_row_hl(std::move(g), rem_sign_st,
                                               std::string{ln.substr(1)},
                                               rem_body_st, rem_hit_st, rem_bg,
                                               syn_at(syn, i), chg[std::size_t(i)]));
                else
                    rows.push_back(code_row(std::move(g), rem_sign_st, ln.substr(1),
                                            rem_body_st, rem_hit_st, syn_at(syn, i)));
            } else {
                // Context line — unchanged code, plain dim, NEW-side line
                // number in the gutter so the eye can track position
                // between +/− runs. Aligned with the band content column.
                std::string_view body = ln;
                if (!ln.empty() && ln[0] == ' ') body = ln.substr(1);
                std::string g;
                if (have_nums) {
                    char buf[16];
                    std::snprintf(buf, sizeof(buf), "%4d   ", new_ln);
                    ++new_ln; ++old_ln;
                    g = buf;
                } else {
                    g = "   ";
                }
                rows.push_back(code_row(std::move(g), ctx_st, body, ctx_st,
                                        hit_style(), syn_at(syn, i)));
            }
        }
        return v(rows).build();
    }

    // ── TodoList: checkbox list ───────────────────────────────────────────
    // TodoList: uniform contract with status glyph as marker. Glyph
    // carries the status semantics (✓ done = green, ◍ in-progress =
    // info, ○ pending = chrome), content reads in body color tier.
    [[nodiscard]] Element todo_list() const {
        using namespace dsl;
        if (cfg_.todos.empty()) return blank();

        const int total = static_cast<int>(cfg_.todos.size());
        const int shown = std::min(total, cfg_.max_todos_shown);

        std::vector<Element> rows;
        rows.reserve(static_cast<std::size_t>(shown) + 1);

        for (int i = 0; i < shown; ++i) {
            const auto& td = cfg_.todos[std::size_t(i)];
            std::string_view marker_glyph;
            Style marker_st;
            Style body_st = Style{}.with_fg(cfg_.text_color);
            switch (td.status) {
                case TodoItem::Status::Completed:
                    marker_glyph = "  \xe2\x9c\x93";   // ✓ right-aligned 3-col
                    marker_st    = Style{}.with_fg(success()).with_bold();
                    break;
                case TodoItem::Status::InProgress:
                    marker_glyph = "  \xe2\x97\x8d";   // ◍
                    marker_st    = Style{}.with_fg(info()).with_bold();
                    break;
                case TodoItem::Status::Pending:
                default:
                    marker_glyph = "  \xe2\x97\x8b";   // ○
                    marker_st    = Style{}.with_fg(cfg_.chrome_color);
                    break;
            }
            rows.push_back(make_row(marker_glyph, marker_st, td.content, body_st));
        }
        if (shown < total) rows.push_back(elision_marker(total - shown));
        return v(rows).build();
    }

    // ── GrepMatches: grouped path → match-row layout ──────────────────────
    //
    // Mirrors `git grep --heading` and ripgrep's default: each unique path
    // becomes a header line, with its match rows indented beneath as a
    // right-aligned line-number column followed by the matched text.
    //
    //     tests/test_auth.cpp
    //         1   // Flaky in CI — investigate.
    //        42       // FIXME: timeout under load
    //        61       EXPECT_NO_TIMEOUT(refresh_pair());
    //     tests/CMakeLists.txt
    //        18       test_auth
    //
    // Two structural wins over the previous flat layout: (a) the path
    // shows up once per group instead of repeating as wallpaper, and
    // (b) the right-aligned line column gives Grep a TABLE silhouette
    // that the eye recognises without reading the contents — the kind of
    // distinctive shape signature the body needs to feel different from
    // BashOutput at peripheral-vision range.
    //
    // Lines that don't fit the canonical `path:line:text` shape pass
    // through as plain text in the default fg.
    [[nodiscard]] Element grep_matches() const {
        using namespace dsl;
        if (cfg_.text.empty()) {
            return cfg_.is_streaming ? placeholder_or_blank("searching")
                                     : blank();
        }

        // Pass 1: parse into typed records, preserving order.
        struct Match {
            std::string_view path;
            std::string_view linum;     // raw, for column alignment
            std::string_view rest;
            bool             parsed = false;   // false → render as raw row
            std::string_view raw;
        };
        const auto all = split_lines(cfg_.text);
        std::vector<Match> matches;
        matches.reserve(all.size());
        for (auto ln : all) {
            const auto p1 = ln.find(':');
            const auto p2 = (p1 == std::string_view::npos)
                          ? std::string_view::npos
                          : ln.find(':', p1 + 1);
            bool digits = (p1 != std::string_view::npos)
                       && (p2 != std::string_view::npos)
                       && (p2 > p1 + 1);
            for (std::size_t j = p1 + 1; j < p2 && digits; ++j)
                if (ln[j] < '0' || ln[j] > '9') digits = false;
            if (digits) {
                matches.push_back(Match{
                    ln.substr(0, p1),
                    ln.substr(p1 + 1, p2 - p1 - 1),
                    ln.substr(p2 + 1),
                    true,
                    {}
                });
            } else {
                matches.push_back(Match{{}, {}, {}, false, ln});
            }
        }

        // Compute the line-number column width across ALL parsed matches
        // so the indented column stays uniform — even when the first few
        // groups have shorter line numbers than the later ones.
        std::size_t num_col_w = 1;
        for (const auto& m : matches)
            if (m.parsed) num_col_w = std::max(num_col_w, m.linum.size());

        // Limit total displayed records to max_matches_shown (counting
        // both parsed matches and pass-through rows; the limit is about
        // visual budget, not a strict match-count).
        const int total = static_cast<int>(matches.size());
        const int shown = std::min(total, cfg_.max_matches_shown);

        const Style num_st  = Style{}.with_fg(muted());                 // gutter
        const Style body_st = Style{}.with_fg(cfg_.text_color);         // match
        const Style raw_st  = Style{}.with_fg(cfg_.text_color);
        // The searched-for text itself: the theme's text colour, bold, so it
        // reads white against the dim body.
        const Style hit_st  = Style{}.with_fg(Color::slot(ThemeSlot::Text))
                                     .with_bold();

        std::vector<Element> rows;
        rows.reserve(static_cast<std::size_t>(shown) + 4);

        std::string_view current_path;
        for (int i = 0; i < shown; ++i) {
            const auto& m = matches[std::size_t(i)];
            if (!m.parsed) {
                rows.push_back(text(std::string{m.raw}, raw_st).build());
                current_path = {};   // a stray row breaks the group
                continue;
            }
            if (m.path != current_path) {
                rows.push_back(path_row(m.path));
                current_path = m.path;
            }
            // "    42   match text" — leading 4-space indent, right-aligned
            // line number padded to num_col_w, two-space gap, match.
            std::string content;
            std::vector<StyledRun> runs;
            content += "    ";
            const std::size_t pad = num_col_w - m.linum.size();
            content.append(pad, ' ');
            const std::size_t num_off = content.size();
            content += m.linum;
            runs.push_back({num_off, m.linum.size(), num_st});
            content += "  ";
            const std::size_t rest_off = content.size();
            content += m.rest;
            // Emphasise what the search was LOOKING FOR, in place.
            //
            // A grep's whole job is to point at a thing; a row painted one
            // flat colour points at nothing and makes the reader redo the
            // search by eye. The structure here (grouped by file, aligned
            // line numbers) was already right -- only the emphasis was
            // missing. maya's SearchResults widget renders before/HIT/after
            // for the same reason.
            //
            // EVERY occurrence, not just the first: a line like
            // `wrap == Truncate || node.wrap == Truncate` is exactly where
            // this earns its keep.
            const std::string& needle = cfg_.match_needle;
            if (!needle.empty()) {
                // needle_spans folds case (grep defaults to insensitive) and
                // handles a '\n'-separated alternation, so `foo|bar` lights up
                // both. Spans are exact byte offsets into the original text.
                const auto spans = needle_spans(m.rest);
                if (spans.empty()) {
                    runs.push_back({rest_off, m.rest.size(), body_st});
                } else {
                    std::size_t at = 0;
                    for (auto [off, len] : spans) {
                        if (off > at)
                            runs.push_back({rest_off + at, off - at, body_st});
                        runs.push_back({rest_off + off, len, hit_st});
                        at = off + len;
                    }
                    if (at < m.rest.size())
                        runs.push_back({rest_off + at, m.rest.size() - at, body_st});
                }
            } else {
                runs.push_back({rest_off, m.rest.size(), body_st});
            }
            rows.push_back(Element{TextElement{
                .content = std::move(content),
                .style   = {},
                .wrap    = TextWrap::TruncateEnd,
                .runs    = std::move(runs),
            }});
        }

        if (shown < total) {
            // These are rows (a match and its context lines), not matches:
            // a 1-match grep with 15 lines of context used to read
            // "⋯ 23 more matches".
            const int more = total - shown;
            rows.push_back(text(
                "\xe2\x8b\xaf " + std::to_string(more) + (more == 1 ? " more line" : " more lines"),
                Style{}.with_fg(muted()).with_italic()
            ).build());
        }
        return v(rows).build();
    }

    // ── Json: pretty-printed with key/value coloring ──────────────────────
    //
    // Single-pass tokenizer: walk the input, classify each lexeme
    // (string / number / keyword / structural) and append it to an output
    // buffer with the correct style.  Pretty-print on the fly:
    // open-brace pushes a new indent and a newline, comma stays on the
    // current line and emits a newline, close-brace pops indent.  Strings
    // that immediately precede a `:` are tagged as keys.
    //
    // If the body doesn't start with `{` or `[` after whitespace we treat
    // it as not-JSON and fall back to the dim CodeBlock — better to render
    // it readable than to colour-vomit a parser failure midway through.
    [[nodiscard]] Element json_block() const {
        using namespace dsl;
        if (cfg_.text.empty()) {
            return cfg_.is_streaming ? placeholder_or_blank("awaiting response")
                                     : blank();
        }
        // Sniff: first non-whitespace must be `{` or `[`.
        std::size_t i0 = 0;
        while (i0 < cfg_.text.size()
               && (cfg_.text[i0] == ' ' || cfg_.text[i0] == '\n'
                || cfg_.text[i0] == '\t' || cfg_.text[i0] == '\r')) ++i0;
        if (i0 >= cfg_.text.size()
         || (cfg_.text[i0] != '{' && cfg_.text[i0] != '[')) {
            return code_block(cfg_.text, cfg_.text_color);
        }

        // Tokenize and emit per-line with run styling. We accumulate into
        // `lines`, where each line is a (string, runs) pair; at the end
        // we wrap each in a TextElement.
        struct Run { std::size_t off, len; Style st; };
        struct Line { std::string content; std::vector<StyledRun> runs; };

        // Discipline: ONE accent (cyan) for keys; everything else (values,
        // structural punctuation, keywords, numbers) shares default-fg or
        // dim. A 4-color rainbow looks like a syntax-highlighter; in a
        // 70-col timeline body the user just wants to scan for which keys
        // are present. Tinting just the keys gives the same scannability
        // without the visual chatter.
        const Style key_st    = Style{}.with_fg(accent());
        const Style str_st    = Style{}.with_fg(cfg_.text_color);
        const Style num_st    = Style{}.with_fg(cfg_.text_color);
        const Style kw_st     = Style{}.with_fg(cfg_.text_color).with_dim();
        const Style struct_st = Style{}.with_fg(muted());
        const Style err_st    = Style{}.with_fg(danger()).with_dim();

        std::vector<Line> lines;
        lines.emplace_back();
        int depth = 0;
        auto indent = [&](Line& l) {
            for (int k = 0; k < depth; ++k) l.content += "  ";
        };

        auto append = [&](std::string_view s, Style st) {
            Line& l = lines.back();
            std::size_t off = l.content.size();
            l.content += s;
            l.runs.push_back({off, s.size(), st});
        };
        auto newline = [&] {
            lines.emplace_back();
            indent(lines.back());
        };

        const std::string& src = cfg_.text;
        std::size_t i = i0;
        // Track whether the next string literal we see is a key (we just
        // emitted a `{` or a `,` and are inside an object).
        std::vector<bool> in_object;       // depth stack: true=object, false=array
        bool expect_key = false;

        while (i < src.size()) {
            char c = src[i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { ++i; continue; }

            if (c == '{' || c == '[') {
                append(std::string{c}, struct_st);
                in_object.push_back(c == '{');
                expect_key = (c == '{');
                ++depth;
                ++i;
                // Peek: empty container collapses on one line.
                std::size_t j = i;
                while (j < src.size()
                       && (src[j]==' '||src[j]=='\t'||src[j]=='\n'||src[j]=='\r')) ++j;
                char close = (c == '{') ? '}' : ']';
                if (j < src.size() && src[j] == close) {
                    append(std::string{close}, struct_st);
                    --depth;
                    if (!in_object.empty()) in_object.pop_back();
                    expect_key = false;
                    i = j + 1;
                    continue;
                }
                newline();
                continue;
            }
            if (c == '}' || c == ']') {
                --depth;
                if (!in_object.empty()) in_object.pop_back();
                expect_key = false;
                newline();
                append(std::string{c}, struct_st);
                ++i;
                continue;
            }
            if (c == ',') {
                append(",", struct_st);
                ++i;
                expect_key = !in_object.empty() && in_object.back();
                newline();
                continue;
            }
            if (c == ':') {
                append(": ", struct_st);
                ++i;
                expect_key = false;
                continue;
            }
            if (c == '"') {
                // String: scan to closing quote with backslash awareness.
                std::size_t j = i + 1;
                while (j < src.size()) {
                    if (src[j] == '\\' && j + 1 < src.size()) { j += 2; continue; }
                    if (src[j] == '"') break;
                    ++j;
                }
                std::size_t end = (j < src.size()) ? j + 1 : src.size();
                std::string_view sv(src.data() + i, end - i);
                append(sv, expect_key ? key_st : str_st);
                i = end;
                expect_key = false;
                continue;
            }
            if (c == '-' || (c >= '0' && c <= '9')) {
                std::size_t j = i + 1;
                while (j < src.size()
                       && (src[j] == '.' || src[j] == 'e' || src[j] == 'E'
                        || src[j] == '+' || src[j] == '-'
                        || (src[j] >= '0' && src[j] <= '9'))) ++j;
                append(std::string_view(src.data() + i, j - i), num_st);
                i = j;
                expect_key = false;
                continue;
            }
            if (c == 't' || c == 'f' || c == 'n') {
                std::size_t j = i;
                while (j < src.size() && src[j] >= 'a' && src[j] <= 'z') ++j;
                append(std::string_view(src.data() + i, j - i), kw_st);
                i = j;
                expect_key = false;
                continue;
            }
            // Anything else is unexpected — emit as red so it's visibly
            // wrong rather than silently swallowed.
            append(std::string{c}, err_st);
            ++i;
        }

        // Render each accumulated line as a TextElement.  Trim a trailing
        // empty last line (artifact of newline-after-close).
        while (!lines.empty() && lines.back().content.empty()) lines.pop_back();

        // Apply head/tail elision at the line level — honoring
        // cfg_.show_all (render every line) and cfg_.tail_only (render
        // only the last N lines, no head, no "⋯ N more" marker).
        const int total = static_cast<int>(lines.size());
        const int head  = cfg_.code_head;
        const int tail  = cfg_.code_tail;
        std::vector<Element> rows;
        rows.reserve(static_cast<std::size_t>(total) + 1);

        auto push_line = [&](Line& ln) {
            rows.push_back(Element{TextElement{
                .content = std::move(ln.content),
                .style   = {},
                .wrap    = TextWrap::TruncateEnd,
                .runs    = std::move(ln.runs),
            }});
        };
        if (cfg_.show_all || total <= head + tail) {
            for (auto& ln : lines) push_line(ln);
        } else if (cfg_.tail_only) {
            // Tail-only: render only the last max(head, tail) lines,
            // no head section, no elision marker.
            const int t = (head > tail) ? head : tail;
            const int start = (total > t) ? (total - t) : 0;
            for (int k = start; k < total; ++k)
                push_line(lines[std::size_t(k)]);
        } else {
            for (int k = 0; k < head; ++k)
                push_line(lines[std::size_t(k)]);
            rows.push_back(elision_marker(total - head - tail));
            for (int k = total - tail; k < total; ++k)
                push_line(lines[std::size_t(k)]);
        }
        return v(rows).build();
    }

    // ── Shared helpers ────────────────────────────────────────────────────

    // Compact elision row. The previous decorative frame
    // (`· · ·  N hidden  · · ·`) competed with AgentTimeline's own stripe
    // for visual weight; a single horizontal-ellipsis glyph + count, left-
    // aligned at a 4-col indent, reads as a quiet "more here" without
    // pulling the eye.
    // Instance method (not static) so it picks up cfg_.chrome_color —
    // each tool's elision marker reads in the tool's category hue,
    // visually tying the "⋯ N more" tag to the surrounding body chrome.
    [[nodiscard]] Element elision_marker(int hidden) const {
        // U+22EF MIDLINE HORIZONTAL ELLIPSIS — single-cell wide.
        return dsl::text("    \xe2\x8b\xaf " + std::to_string(hidden) + " more",
                         Style{}.with_fg(cfg_.chrome_color).with_italic()).build();
    }

    // ── Uniform row contract ──────────────────────────────────────────────
    // Every tool body's per-row layout follows the same shape:
    //
    //    "<marker> │ <content>"
    //
    //   * marker  — short kind-specific identifier (line number, `>`, `+`,
    //               `☐`, etc.). Caller decides width / glyph; helper just
    //               styles + concatenates.
    //   * ` │ `   — separator, painted in `cfg_.chrome_color` so the
    //               whole tool's chrome reads as one band.
    //   * content — body text, painted in `content_style` (typically
    //               dim text_tertiary).
    //
    // This is what gives a turn-with-mixed-tools its tabular feel: the
    // pipe column lines up at the same horizontal offset for every kind,
    // and the leading marker tells you what kind of row it is (a file
    // line, a shell-prompt output line, an added line of code, a todo,
    // etc.) at the same horizontal position.
    [[nodiscard]] Element
    make_row(std::string_view marker, Style marker_style,
             std::string_view content, Style content_style) const {
        const Style pipe_style = Style{}.with_fg(cfg_.chrome_color);

        std::string out;
        out.reserve(marker.size() + content.size() + 8);

        const std::size_t marker_off = 0;
        out.append(marker);
        const std::size_t pipe_off = out.size();
        out += " \xe2\x94\x82 ";   // " │ "  (UTF-8 5 bytes, 3 cols)
        const std::size_t content_off = out.size();
        out.append(content);

        std::vector<StyledRun> runs;
        if (!marker.empty())
            runs.push_back({marker_off, marker.size(), marker_style});
        runs.push_back({pipe_off, 5, pipe_style});
        if (!content.empty()) {
            // Light up the searched-for literal wherever it appears in the
            // row. Falls back to one flat run when there's no needle.
            const auto spans = needle_spans(content);
            if (spans.empty()) {
                runs.push_back({content_off, content.size(), content_style});
            } else {
                const Style hit_style =
                    Style{}.with_fg(Color::slot(ThemeSlot::Text)).with_bold();
                std::size_t at = 0;
                for (auto [off, len] : spans) {
                    if (off > at)
                        runs.push_back({content_off + at, off - at,
                                        content_style});
                    runs.push_back({content_off + off, len, hit_style});
                    at = off + len;
                }
                if (at < content.size())
                    runs.push_back({content_off + at, content.size() - at,
                                    content_style});
            }
        }

        return Element{TextElement{
            .content = std::move(out),
            .style   = {},
            .wrap    = TextWrap::TruncateEnd,
            .runs    = std::move(runs),
        }};
    }

    // Project elided lines into a vector of styled text Elements,
    // inserting the "… N hidden" marker at the elision position.
    // Instance method (not static) so the elision marker can read
    // cfg_.chrome_color via the now-non-static elision_marker.
    [[nodiscard]] std::vector<Element>
    each_with_elision(const ElidedPreview& p, Style st) const {
        using namespace dsl;
        std::vector<Element> out;
        out.reserve(p.lines.size() + 1);
        for (int i = 0; i < static_cast<int>(p.lines.size()); ++i) {
            if (i == p.elision_at && p.elided > 0)
                out.push_back(elision_marker(p.elided));
            out.push_back(
                text(std::string{p.lines[static_cast<std::size_t>(i)]}, st).build());
        }
        return out;
    }

    // bright_black is already the muted tone — stacking SGR `dim` on it
    // collapses below readability on some themes; suppress in that case.
    static Style fg_dim_(Color c) {
        const bool already_muted = theme::is_muted(c);
        return already_muted
            ? Style{}.with_fg(c)
            : Style{}.with_fg(c).with_dim();
    }
};

} // namespace maya
