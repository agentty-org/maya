#pragma once
// maya::widget::Minimap — VSCode-style code minimap (background-free)
//
// A compressed bird's-eye view of a whole file. Each terminal cell packs two
// source lines and keeps the terminal's own background: it picks a glyph by
// which halves have code — █ (both), ▀ (top), ▄ (bottom), space (neither) —
// so nothing paints a cell background. Every line is drawn as its own
// indent-aware silhouette, and colour alone marks the visible viewport
// (brighter ink) and the active line (accent).
//
// Pure view widget: give it the line metrics + viewport range and it draws.
//
// Usage:
//   Minimap mm;
//   mm.set_source(source_text);            // or set_lines(lengths, indents)
//   mm.set_viewport(top_line, bottom_line);
//   mm.set_active_line(cursor_line);
//   Element ui = mm | dsl::width(16) | dsl::height(24);

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"
#include "agentty/markdown/highlight.hpp"

namespace maya {

struct MinimapTheme {
    Color ink_idle = Color::slot(ThemeSlot::Muted); // code, outside viewport
    Color ink_view = Color::slot(ThemeSlot::Link); // code, inside viewport
    Color active   = Color::slot(ThemeSlot::Warning); // active line glow
    Color slider   = Color::slot(ThemeSlot::Primary); // viewport slider on the left edge
};

struct MinimapConfig {
    int                    width  = 16; // columns
    MinimapTheme           theme  = {};
    syntax::Lang           lang   = syntax::Lang::Generic; // Generic => flat ink
    syntax::HighlightTheme syntax = syntax::themes::github_dark;
};

class Minimap {
public:
    Minimap() = default;
    explicit Minimap(MinimapConfig cfg) : cfg_(cfg) {}

    // ── Feed it a whole document ────────────────────────────────────────────
    Minimap& set_source(std::string_view src) {
        lens_.clear();
        indents_.clear();
        std::vector<std::pair<size_t, size_t>> ranges; // per-line [start,end)
        size_t pos = 0;
        while (pos <= src.size()) {
            size_t nl = src.find('\n', pos);
            size_t end = (nl == std::string_view::npos) ? src.size() : nl;
            std::string_view line = src.substr(pos, end - pos);
            int indent = 0;
            while (indent < static_cast<int>(line.size()) &&
                   (line[indent] == ' ' || line[indent] == '\t'))
                indent += (line[indent] == '\t') ? 4 : 1;
            lens_.push_back(rstrip_width(line));
            indents_.push_back(indent);
            ranges.push_back({pos, end});
            if (nl == std::string_view::npos) break;
            pos = nl + 1;
        }
        recompute_max();
        recompute_colors(src, ranges);
        return *this;
    }

    // ── Or hand it precomputed metrics (from a rope/piece-table) ─────────────
    Minimap& set_lines(std::vector<int> lengths, std::vector<int> indents) {
        lens_ = std::move(lengths);
        indents_ = std::move(indents);
        indents_.resize(lens_.size(), 0);
        recompute_max();
        return *this;
    }

    Minimap& set_viewport(int top_line, int bottom_line) {
        view_top_ = top_line; view_bot_ = bottom_line; return *this;
    }
    Minimap& set_active_line(int abs_line) { active_ = abs_line; return *this; }

    [[nodiscard]] int line_count() const { return static_cast<int>(lens_.size()); }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        return Element{ComponentElement{
            .render = [self = *this](int w, int h) -> Element {
                return self.paint(w, h);
            },
            .measure = [](int max_w) -> Size {
                return Size{Columns(max_w), Rows(1)};
            },
        }};
    }

private:
    MinimapConfig      cfg_{};
    std::vector<int>   lens_;
    std::vector<int>   indents_;
    std::vector<Color> line_col_;   // per-line dominant syntax colour (optional)
    int                max_len_  = 1;
    int                view_top_ = -1;
    int                view_bot_ = -1;
    int                active_   = -1;

    static int rstrip_width(std::string_view s) {
        int end = static_cast<int>(s.size());
        while (end > 0 && (s[end - 1] == ' ' || s[end - 1] == '\t' ||
                           s[end - 1] == '\r'))
            --end;
        return end;
    }
    void recompute_max() {
        max_len_ = 1;
        for (int l : lens_) max_len_ = std::max(max_len_, l);
    }

    // Dominant syntax colour per line (skips whitespace/punctuation noise).
    void recompute_colors(std::string_view src,
                          const std::vector<std::pair<size_t, size_t>>& ranges) {
        line_col_.clear();
        if (cfg_.lang == syntax::Lang::Generic) return;
        std::vector<syntax::Span> spans;
        syntax::highlight(src, cfg_.lang, spans);
        line_col_.resize(ranges.size(), cfg_.theme.ink_idle);
        size_t si = 0;
        for (size_t li = 0; li < ranges.size(); ++li) {
            auto [ls, le] = ranges[li];
            long best_len = 0; syntax::Capture best = syntax::Capture::None;
            while (si < spans.size() && spans[si].start + spans[si].len <= ls) ++si;
            for (size_t k = si; k < spans.size() && spans[k].start < le; ++k) {
                if (spans[k].cap == syntax::Capture::None ||
                    spans[k].cap == syntax::Capture::Punctuation) continue;
                long s0 = std::max<long>(spans[k].start, ls);
                long s1 = std::min<long>(spans[k].start + spans[k].len, le);
                if (s1 - s0 > best_len) { best_len = s1 - s0; best = spans[k].cap; }
            }
            if (best != syntax::Capture::None)
                line_col_[li] = cfg_.syntax.style_for(best).fg.value_or(cfg_.theme.ink_idle);
        }
    }

    Color line_color(int li, bool view) const {
        if (!line_col_.empty() && li >= 0 && li < static_cast<int>(line_col_.size()))
            return line_col_[static_cast<size_t>(li)];
        return view ? cfg_.theme.ink_view : cfg_.theme.ink_idle;
    }

    struct Px { bool ink = false; bool view = false; bool active = false; int li = -1; };

    Px pixel(int subrow, int total_sub, int col, int cols) const {
        const int n = static_cast<int>(lens_.size());
        if (n == 0) return {};
        int li = static_cast<int>(static_cast<long long>(subrow) * n / total_sub);
        li = std::clamp(li, 0, n - 1);

        const int len    = lens_[static_cast<size_t>(li)];
        const int indent = std::min(indents_[static_cast<size_t>(li)], len);
        int c0 = indent * cols / max_len_;
        int c1 = (len   * cols + max_len_ - 1) / max_len_;
        c1 = std::max(c1, c0 + (len > indent ? 1 : 0));

        Px px;
        px.li     = li;
        px.ink    = (len > indent) && col >= c0 && col < c1;
        px.view   = (view_top_ > 0 && li + 1 >= view_top_ && li + 1 <= view_bot_);
        px.active = (li + 1 == active_);
        return px;
    }

    // Foreground style for a stacked (top,bottom) cell.
    Style cell_style(const Px& a, const Px& b) const {
        const auto& t = cfg_.theme;
        const bool active = (a.ink && a.active) || (b.ink && b.active);
        const bool view   = (a.ink && a.view)   || (b.ink && b.view);
        if (active) return Style{}.with_fg(t.active);
        const int li = a.ink ? a.li : b.li;
        Style s = Style{}.with_fg(line_color(li, view));
        if (!view) s = s.with_dim(); // outside the viewport recedes
        return s;
    }

    Element paint(int w, int h) const {
        // Reserve the first column for the viewport slider bar.
        const int slider_w = 1;
        const int cols = std::clamp(std::min(w, cfg_.width) - slider_w,
                                    1, std::max(1, w - slider_w));
        const int total_sub = std::max(1, h * 2);

        std::vector<Element> rows;
        rows.reserve(static_cast<size_t>(h));
        for (int r = 0; r < h; ++r) {
            std::string content;
            std::vector<StyledRun> runs;

            // Left-edge viewport slider: a solid bar next to visible rows.
            {
                Px a = pixel(r * 2,     total_sub, 0, cols);
                Px b = pixel(r * 2 + 1, total_sub, 0, cols);
                bool vis = a.view || b.view;
                const size_t off = content.size();
                content += vis ? "\xe2\x96\x8f" : " "; // ▏ left one-eighth block
                runs.push_back({off, vis ? 3u : 1u,
                                vis ? Style{}.with_fg(cfg_.theme.slider) : Style{}});
            }

            for (int c = 0; c < cols; ++c) {
                Px top = pixel(r * 2,     total_sub, c, cols);
                Px bot = pixel(r * 2 + 1, total_sub, c, cols);
                const char* glyph =
                    (top.ink && bot.ink) ? "\xe2\x96\x88"          // █
                    : top.ink            ? "\xe2\x96\x80"          // ▀
                    : bot.ink            ? "\xe2\x96\x84"          // ▄
                                         : " ";
                const size_t off = content.size();
                content += glyph;
                const size_t len = std::strlen(glyph);
                Style st = (top.ink || bot.ink) ? cell_style(top, bot) : Style{};
                runs.push_back({off, len, st});
            }
            rows.push_back(Element{TextElement{
                .content = std::move(content),
                .style   = Style{},
                .wrap    = TextWrap::NoWrap,
                .runs    = std::move(runs),
            }});
        }
        return dsl::v(std::move(rows)).build();
    }
};

} // namespace maya
