#pragma once
// maya::widget::CodeView — beautiful read-only syntax-highlighted code panel
//
// A gorgeous, self-contained code viewer: a syntax-highlighted source pane
// with a line-number gutter, a git-diff change ribbon, an active-line marker,
// faint indent guides, and foreground-only styling so the user's own terminal
// background always shows through. It reuses maya's built-in syntax engine
// (maya::syntax) so it speaks C/C++/Python/Rust/Go/JS/TS/Shell/JSON out of the
// box.
//
// Pure *view* widget — no cursor, no input — a perfect building block for an
// editor viewport, a diff hunk, a search preview, or a docs sample.
//
// Usage:
//   CodeView cv{source, {.lang = syntax::Lang::Cpp}};
//   cv.set_active_line(42);
//   cv.mark(10, LineMark::Added);
//   Element ui = cv;                       // implicit -> build()

#include <cstdint>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"
#include "../text/unicode_width.hpp"
#include "agentty/markdown/highlight.hpp"

namespace maya {

// ── Per-line change marker (git ribbon on the far left) ─────────────────────
enum class LineMark : uint8_t { None, Added, Modified, Deleted, Warning, Error };

// ── Theme — foreground colours only; the terminal owns the background ───────
struct CodeViewTheme {
    syntax::HighlightTheme syntax = syntax::themes::github_dark;

    Color gutter_fg     = Color::slot(ThemeSlot::Muted); // idle line numbers
    Color gutter_active = Color::slot(ThemeSlot::Text); // current line number
    Color active_accent = Color::slot(ThemeSlot::Primary); // current-line gutter rule
    Color gutter_rule   = Color::slot(ThemeSlot::Surface); // idle vertical gutter rule
    Color indent_guide  = Color::slot(ThemeSlot::Border); // faint │ at each tab stop
    Color active_shade  = Color::slot(ThemeSlot::Surface); // subtle current-line wash

    Color mark_added    = Color::slot(ThemeSlot::Success); // green ribbon
    Color mark_modified = Color::slot(ThemeSlot::Warning); // amber ribbon
    Color mark_deleted  = Color::slot(ThemeSlot::Error); // red ribbon
    Color mark_warning  = Color::slot(ThemeSlot::Warning);
    Color mark_error    = Color::slot(ThemeSlot::Error);
};

struct CodeViewConfig {
    syntax::Lang  lang          = syntax::Lang::Generic;
    CodeViewTheme theme         = {};
    bool          line_numbers  = true;
    bool          indent_guides = true;
    bool          relative      = false; // relative line numbers (vim style)
    int           first_line    = 1;     // absolute number of the first row
    int           tab_width     = 4;
};

class CodeView {
public:
    CodeView() = default;
    explicit CodeView(std::string source, CodeViewConfig cfg = {})
        : cfg_(std::move(cfg)) { set_source(std::move(source)); }

    // ── Mutators (fluent) ───────────────────────────────────────────────────
    CodeView& set_source(std::string source) {
        src_ = std::move(source);
        rehighlight();
        return *this;
    }
    CodeView& set_active_line(int abs_line) { active_ = abs_line; return *this; }
    CodeView& set_first_line(int n)         { cfg_.first_line = n; return *this; }
    CodeView& set_lang(syntax::Lang l)      { cfg_.lang = l; rehighlight(); return *this; }
    CodeView& mark(int abs_line, LineMark m) { marks_[abs_line] = m; return *this; }
    CodeView& clear_marks()                  { marks_.clear(); return *this; }

    // A block caret at (line, col) — col is 0-based. Replaces any existing carets.
    CodeView& set_caret(int line, int col) {
        carets_.clear(); carets_.push_back({line, col}); active_ = line; return *this;
    }
    // Add an additional caret (multi-cursor).
    CodeView& add_caret(int line, int col) { carets_.push_back({line, col}); return *this; }
    // A selection from (l0,c0) to (l1,c1). Replaces any existing selections.
    CodeView& set_selection(int l0, int c0, int l1, int c1) {
        sels_.clear(); return add_selection(l0, c0, l1, c1);
    }
    CodeView& add_selection(int l0, int c0, int l1, int c1) {
        if (l1 < l0 || (l1 == l0 && c1 < c0)) { std::swap(l0, l1); std::swap(c0, c1); }
        sels_.push_back({l0, c0, l1, c1}); return *this;
    }
    CodeView& clear_selection() { sels_.clear(); return *this; }

    [[nodiscard]] int line_count() const { return static_cast<int>(lines_.size()); }

    // ── Node concept ────────────────────────────────────────────────────────
    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        const auto& th = cfg_.theme;
        const int   count = static_cast<int>(lines_.size());
        const int   last  = cfg_.first_line + count - 1;
        const int   gutter_w =
            cfg_.line_numbers ? std::max(3, digits(std::max(1, last))) : 0;

        std::vector<Element> rows;
        rows.reserve(lines_.size());

        for (int i = 0; i < count; ++i) {
            const int  abs = cfg_.first_line + i;
            const bool active = std::any_of(carets_.begin(), carets_.end(),
                                            [&](const auto& c) { return c.first == abs; });
            const Style plain{}; // terminal-default fg/bg

            // ── git change ribbon (1 col, far left) ─────────────────────────
            std::string ribbon = " ";
            Style ribbon_st = plain;
            if (auto it = marks_.find(abs); it != marks_.end()) {
                ribbon = "\xe2\x96\x8e"; // ▎ left three-eighths block
                switch (it->second) {
                    case LineMark::Added:    ribbon_st = plain.with_fg(th.mark_added);    break;
                    case LineMark::Modified: ribbon_st = plain.with_fg(th.mark_modified); break;
                    case LineMark::Deleted:  ribbon = "\xe2\x96\x94"; // ▔
                                             ribbon_st = plain.with_fg(th.mark_deleted);  break;
                    case LineMark::Warning:  ribbon_st = plain.with_fg(th.mark_warning);  break;
                    case LineMark::Error:    ribbon_st = plain.with_fg(th.mark_error);    break;
                    default: break;
                }
            }

            // ── gutter (line number, right-aligned) ─────────────────────────
            std::string num;
            if (cfg_.line_numbers) {
                int shown = abs;
                if (cfg_.relative && !active && active_ > 0)
                    shown = std::abs(abs - active_);
                num = std::to_string(shown);
                if (static_cast<int>(num.size()) < gutter_w)
                    num.insert(num.begin(), gutter_w - num.size(), ' ');
            }
            Style num_st = active
                ? plain.with_fg(th.gutter_active).with_bold()
                : plain.with_fg(th.gutter_fg);

            // ── gutter rule doubles as the current-line indicator ───────────
            std::string rule = active ? "\xe2\x96\x8e"    // ▎ solid bar
                                      : "\xe2\x96\x95";   // ▕ faint hairline
            Style rule_st = active ? plain.with_fg(th.active_accent)
                                   : plain.with_fg(th.gutter_rule);

            // ── assemble the row payload ────────────────────────────────────
            RowParts p;
            p.push(ribbon, ribbon_st);
            if (cfg_.line_numbers) {
                p.push(num, num_st);
                p.push(" ", plain);
                p.push(rule, rule_st);
                p.push(" ", plain);
            } else {
                p.push(rule, rule_st);
                p.push(" ", plain);
            }

            emit_code(p, lines_[static_cast<size_t>(i)], plain, abs);
            rows.push_back(active ? finish_active_row(std::move(p), th.active_shade)
                                  : finish_row(std::move(p)));
        }

        return dsl::v(std::move(rows)).build();
    }

private:
    // A rendered line: display text (tabs expanded) + syntax runs over it.
    struct Line {
        std::string            text;
        std::vector<StyledRun> runs;
        int                    indent_cols = 0; // leading whitespace width
        // Maps a SOURCE byte offset within the (untab-expanded) line to the
        // corresponding byte offset in `text` (tab-expanded). carets/selections
        // arrive in source coordinates; this remaps them onto the display.
        std::vector<int>       src2disp;
    };

    // Accumulates (text, style) segments then flattens to a TextElement.
    struct RowParts {
        std::string            s;
        std::vector<StyledRun> runs;
        void push(std::string_view t, Style st) {
            if (t.empty()) return;
            runs.push_back({s.size(), t.size(), st});
            s += t;
        }
    };

    CodeViewConfig                    cfg_{};
    std::string                       src_;
    std::vector<Line>                 lines_;
    std::unordered_map<int, LineMark> marks_;
    int                               active_ = -1;
    std::vector<std::pair<int,int>>   carets_;                 // (abs line, 0-based col)
    std::vector<std::array<int,4>>    sels_;                   // (l0,c0,l1,c1), 1-based lines

    static int digits(int n) {
        int d = 1;
        for (n = n < 0 ? -n : n; n >= 10; n /= 10) ++d;
        return d;
    }

    // Current-line row: same content as any other line (syntax colours, git
    // ribbon, indent guides, caret) but washed with a subtle background shade
    // and padded full-width so the wash spans the whole row. A gentle tint,
    // not a hard reverse — readable on top of the code's own colours.
    Element finish_active_row(RowParts p, Color shade) const {
        const Style fill = Style{}.with_bg(shade);
        // carry the shade under every run (leave a reverse caret cell alone)
        for (auto& r : p.runs)
            if (!r.style.inverse) r.style = r.style.with_bg(shade);

        std::vector<StyledRun> in = std::move(p.runs);
        std::sort(in.begin(), in.end(),
                  [](const StyledRun& a, const StyledRun& b) {
                      return a.byte_offset < b.byte_offset;
                  });
        std::string base = std::move(p.s);

        return Element{ComponentElement{
            .render = [in = std::move(in), base = std::move(base), fill]
                      (int w, int) -> Element {
                std::string s = base;
                const int used = string_width(s);
                if (used < w) s.append(static_cast<size_t>(w - used), ' ');

                std::vector<StyledRun> runs;
                runs.reserve(in.size() * 2 + 1);
                size_t cursor = 0;
                for (const auto& r : in) {
                    if (r.byte_offset > cursor)
                        runs.push_back({cursor, r.byte_offset - cursor, fill});
                    runs.push_back(r);
                    cursor = r.byte_offset + r.byte_length;
                }
                if (cursor < s.size())
                    runs.push_back({cursor, s.size() - cursor, fill});

                return Element{TextElement{ .content = std::move(s), .style = fill,
                                            .wrap = TextWrap::NoWrap,
                                            .runs = std::move(runs) }};
            },
        }};
    }

    // Append the code, indent guides, and per-token colours to the row.
    void emit_code(RowParts& p, const Line& ln, Style plain, int abs) const {
        const auto& th = cfg_.theme;

        // Carets + selection ranges on this line. Incoming columns are SOURCE
        // byte offsets; remap onto the tab-expanded display line.
        std::vector<int> carets;
        for (const auto& c : carets_) if (c.first == abs) carets.push_back(remap_col(ln, c.second));
        std::vector<std::pair<int,int>> sels;
        for (const auto& s : sels_)
            if (abs >= s[0] && abs <= s[2]) {
                int a = (abs == s[0]) ? remap_col(ln, s[1]) : 0;
                int b = (abs == s[2]) ? remap_col(ln, s[3]) : static_cast<int>(ln.text.size());
                sels.push_back({a, b});
            }
        if (!carets.empty() || !sels.empty()) { emit_code_cells(p, ln, carets, sels); return; }

        // Indent guides: a faint │ at every tab stop inside the leading run.
        if (cfg_.indent_guides && ln.indent_cols >= cfg_.tab_width) {
            const Style guide = plain.with_fg(th.indent_guide);
            std::string lead;
            for (int c = 0; c < ln.indent_cols; ++c) {
                if (c % cfg_.tab_width == 0) {
                    p.push(lead, plain);
                    lead.clear();
                    p.push("\xe2\x94\x82", guide); // │
                } else {
                    lead += ' ';
                }
            }
            p.push(lead, plain);
            const size_t base_off = p.s.size();
            std::string_view rest{ln.text};
            rest.remove_prefix(std::min<size_t>(ln.indent_cols, rest.size()));
            p.s += rest;
            for (const auto& r : ln.runs) {
                if (r.byte_offset + r.byte_length <= static_cast<size_t>(ln.indent_cols))
                    continue;
                size_t start = std::max(r.byte_offset, static_cast<size_t>(ln.indent_cols));
                size_t end   = r.byte_offset + r.byte_length;
                p.runs.push_back({base_off + (start - ln.indent_cols), end - start, r.style});
            }
            return;
        }

        const size_t base_off = p.s.size();
        p.s += ln.text;
        for (const auto& r : ln.runs)
            p.runs.push_back({base_off + r.byte_offset, r.byte_length, r.style});
    }

    // Per-cell path: draws each column so a caret (reverse video) and a
    // selection (underline) can overlay the syntax colour. Byte == column is
    // assumed (source is tab-expanded ASCII); multibyte comments degrade
    // gracefully. No custom background — inverse uses the terminal's colours.
    void emit_code_cells(RowParts& p, const Line& ln, const std::vector<int>& carets,
                         const std::vector<std::pair<int,int>>& sels) const {
        const auto& th = cfg_.theme;
        const int n = static_cast<int>(ln.text.size());
        std::vector<Style> cs(static_cast<size_t>(n), Style{});
        for (const auto& r : ln.runs)
            for (size_t b = r.byte_offset;
                 b < r.byte_offset + r.byte_length && b < static_cast<size_t>(n); ++b)
                cs[b] = r.style;

        auto in_sel   = [&](int c) { for (auto& s : sels) if (c >= s.first && c < s.second) return true; return false; };
        auto is_caret = [&](int c) { for (int cc : carets) if (c == cc) return true; return false; };

        // Walk by CODEPOINT so multibyte glyphs stay intact (a byte-by-byte
        // loop would split UTF-8 and mojibake any accented / CJK / emoji line
        // that has a caret or selection on it). Caret/selection cols are byte
        // offsets; we test the offset at each codepoint's START.
        std::string_view t{ln.text};
        size_t i = 0;
        int indent_seen = 0;
        while (i < t.size()) {
            size_t start = i;
            unsigned char lead = static_cast<unsigned char>(t[i]);
            int len = (lead < 0x80) ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3
                    : (lead >> 3) == 0x1E ? 4 : 1;
            if (start + static_cast<size_t>(len) > t.size()) len = 1;
            std::string glyph = std::string(t.substr(start, static_cast<size_t>(len)));
            i += static_cast<size_t>(len);

            Style st = cs[start];
            // indent guide: only meaningful in the ASCII indentation region
            if (cfg_.indent_guides && static_cast<int>(start) < ln.indent_cols &&
                (indent_seen % cfg_.tab_width == 0) && glyph == " ") {
                glyph = "\xe2\x94\x82";
                st = Style{}.with_fg(th.indent_guide);
            }
            if (static_cast<int>(start) < ln.indent_cols) ++indent_seen;

            if (in_sel(static_cast<int>(start)))   st = st.with_underline();
            if (is_caret(static_cast<int>(start)))  st = st.with_inverse();
            p.push(glyph, st);
        }
        for (int cc : carets) if (cc >= n) { p.push(" ", Style{}.with_inverse()); break; }
    }

    // TextElement requires runs to tile the whole content with no gaps, so we
    // sort the collected runs and fill any uncovered span with plain style.
    static Element finish_row(RowParts p) {
        std::vector<StyledRun>& in = p.runs;
        std::sort(in.begin(), in.end(),
                  [](const StyledRun& a, const StyledRun& b) {
                      return a.byte_offset < b.byte_offset;
                  });
        std::vector<StyledRun> runs;
        runs.reserve(in.size() * 2 + 1);
        size_t cursor = 0;
        for (const auto& r : in) {
            if (r.byte_offset > cursor)
                runs.push_back({cursor, r.byte_offset - cursor, Style{}});
            runs.push_back(r);
            cursor = r.byte_offset + r.byte_length;
        }
        if (cursor < p.s.size())
            runs.push_back({cursor, p.s.size() - cursor, Style{}});

        return Element{TextElement{
            .content = std::move(p.s),
            .style   = Style{},
            .wrap    = TextWrap::NoWrap,
            .runs    = std::move(runs),
        }};
    }

    void rehighlight() {
        lines_.clear();

        // Expand tabs up-front so byte offsets == display columns and the
        // highlighter sees stable, terminal-safe source. We also record, for
        // each SOURCE byte, the display byte it maps to (g_src2disp) so that
        // carets/selections — which arrive in source coordinates — land on the
        // right cell even on tab-indented / CR-containing lines.
        std::string disp;
        disp.reserve(src_.size());
        std::vector<int> g_src2disp(src_.size() + 1, 0);
        // source line-start byte offsets, in BOTH coordinate spaces.
        std::vector<size_t> src_line_start{0};
        int col = 0;
        for (size_t bi = 0; bi < src_.size(); ++bi) {
            char c = src_[bi];
            g_src2disp[bi] = static_cast<int>(disp.size());
            if (c == '\t') {
                int n = cfg_.tab_width - (col % cfg_.tab_width);
                disp.append(static_cast<size_t>(n), ' ');
                col += n;
            } else if (c == '\n') {
                disp += '\n';
                col = 0;
                src_line_start.push_back(bi + 1);
            } else if (c == '\r') {
                // drop CR (does not advance display)
            } else {
                disp += c;
                ++col;
            }
        }
        g_src2disp[src_.size()] = static_cast<int>(disp.size());

        std::vector<syntax::Span> spans;
        syntax::highlight(disp, cfg_.lang, spans);

        size_t pos = 0, si = 0;
        size_t disp_line_start = 0;
        int line_idx = 0;
        while (pos <= disp.size()) {
            size_t nl = disp.find('\n', pos);
            size_t end = (nl == std::string::npos) ? disp.size() : nl;

            Line ln;
            ln.text = disp.substr(pos, end - pos);
            while (ln.indent_cols < static_cast<int>(ln.text.size()) &&
                   ln.text[static_cast<size_t>(ln.indent_cols)] == ' ')
                ++ln.indent_cols;

            // Build this line's source-byte → display-byte map (line-local),
            // from the global map, relative to both line starts.
            {
                size_t ssrc = (static_cast<size_t>(line_idx) < src_line_start.size())
                                  ? src_line_start[static_cast<size_t>(line_idx)] : src_.size();
                // source bytes for this line run until the next '\n' in src_.
                size_t ssrc_end = ssrc;
                while (ssrc_end < src_.size() && src_[ssrc_end] != '\n') ++ssrc_end;
                ln.src2disp.resize(ssrc_end - ssrc + 1);
                for (size_t k = ssrc; k <= ssrc_end; ++k)
                    ln.src2disp[k - ssrc] = g_src2disp[k] - static_cast<int>(disp_line_start);
            }

            while (si < spans.size() &&
                   spans[si].start + spans[si].len <= pos)
                ++si;
            for (size_t k = si; k < spans.size() && spans[k].start < end; ++k) {
                const auto& sp = spans[k];
                size_t s0 = std::max<size_t>(sp.start, pos);
                size_t s1 = std::min<size_t>(sp.start + sp.len, end);
                if (s1 <= s0) continue;
                ln.runs.push_back({s0 - pos, s1 - s0,
                                   cfg_.theme.syntax.style_for(sp.cap)});
            }
            lines_.push_back(std::move(ln));

            if (nl == std::string::npos) break;
            pos = nl + 1;
            disp_line_start = pos;
            ++line_idx;
        }
    }

    // Remap a source-byte caret/selection column onto the display line.
    static int remap_col(const Line& ln, int src_col) {
        if (src_col < 0) return 0;
        if (src_col < static_cast<int>(ln.src2disp.size())) return ln.src2disp[static_cast<size_t>(src_col)];
        // past the mapped range: clamp to the display line end + overflow
        int base = ln.src2disp.empty() ? 0 : ln.src2disp.back();
        return base + (src_col - (static_cast<int>(ln.src2disp.size()) - 1));
    }
};

} // namespace maya
