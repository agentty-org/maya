#pragma once
// maya::ToolViewer — the body stage of agentty's tool-output viewer (^O):
// one tool call's (or thinking block's) output, scrolling inside a Panel.
//
//   ╭─ Shell · 3/7 ──────────────────────────────────────────╮
//   │  Shell  ran on scripts/build.sh            ok · 4.2s   │
//   │ ───────────────────────────────────────────────────────│
//   │ $ scripts/build.sh -- --target agentty                 │
//   │  1 │ [1/446] Building CXX object …                   ▐ │
//   │  2 │ [2/446] Building CXX object …                   ▐ │
//   │                                          40% · 446 lines│
//   │  ↑↓ scroll   PgUp/PgDn page   / find   y copy   Esc back│
//   ╰────────────────────────────────────────────────────────╯
//
// The host owns WHAT: the rows (one screen row each, as Lines or rows a
// widget produced), where the search hits are, how far the typewriter has
// got, the hints. This widget owns HOW: the header row's fitting, the
// typewriter decoration of the row at the cursor, the scroll window and its
// scrollbar, the status line, and shrinking the whole pane until it fits
// the terminal.

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../../anim/text_reveal.hpp"
#include "../../core/anim_clock.hpp"
#include "../../dsl.hpp"
#include "../../element/builder.hpp"
#include "../../element/text.hpp"
#include "../../style/style.hpp"
#include "../../style/theme.hpp"
#include "key_hints.hpp"
#include "line.hpp"
#include "panel.hpp"
#include "scrollbar.hpp"
#include "tool_body_preview.hpp"

namespace maya {
namespace tool_viewer_detail {
using namespace dsl;   // the drawing DSL, inside maya only

class ToolViewer {
public:
    struct Header {                 // "Shell  ran on …   ok · 4.2s"
        std::string head, detail, trailing;
        std::vector<std::string> mid;    // facts that join `detail` while they fit
        Style head_style{}, trail_style{};
        Color text = Color::slot(ThemeSlot::Text);
    };
    struct Tail {                   // thinking: "✦ Reasoned · 216 tokens · 4.3s"
        bool        live = false;
        std::string meta;
        Color brand     = Color::slot(ThemeSlot::Accent);
        Color secondary = Color::slot(ThemeSlot::Muted);
    };
    struct Reveal {                 // the typewriter, on text arriving live
        bool        on = false;
        std::size_t typed = 0;      // codepoints typed so far
        std::size_t row0  = 0;      // first row the spans below describe
        const std::vector<std::size_t>* cp_begin = nullptr;
        const std::vector<std::size_t>* cp_len   = nullptr;
        double      rate = 0.0;     // the cursor's current chars/s
        bool        decorate = false;
    };
    // What fit last frame, kept by the host across frames.
    struct FitMemo { int term_rows = -1, term_cols = -1, start = -1, index = -1, vh = -1;
                     bool thinking = false, hints = true, header = true, tight = false; };

    struct Config {
        Panel::Config base;                  // title, accent, viewport_h, min_width
        std::optional<Header> header;        // none on a tiny terminal
        const std::vector<Element>* rows = nullptr;   // one screen row each
        std::optional<Tail> tail;
        Reveal reveal;

        // Find.
        std::string query;
        bool typing = false;
        int  hits = 0, current_hit = -1, current_row = -1;
        bool jump_to_hit = false;

        ScrollState* scroll = nullptr;
        int  y_before = 0;
        bool follow   = false;               // pin to the newest row

        bool        compact = false;         // no status, no hints
        std::string count;                   // "446 lines" / "1,203 words"
        bool        following = false;
        Color       where_color = Color::slot(ThemeSlot::Muted);
        int         fresh_above = 0;
        std::vector<KeyHint> hints;

        // Fitting: the room the overlay has, and what fit last frame.
        int budget = 20, cols = 80;
        FitMemo* memo = nullptr;
        int term_rows = 0, index = 0;
        bool thinking_only = false;

        Color text = Color::slot(ThemeSlot::Text), muted = Color::slot(ThemeSlot::Muted),
              warn = Color::slot(ThemeSlot::Warning), danger = Color::slot(ThemeSlot::Error),
              highlight = Color::slot(ThemeSlot::Highlight);
    };

    explicit ToolViewer(Config c) : cfg_(std::move(c)) {}
    operator Element() const { return build(); }

    // A structured preview (diff, file, todo) split into one-row elements,
    // so the viewer's window can address rows and the scroll stays exact.
    [[nodiscard]] static std::vector<Element> preview_rows(ToolBodyPreview::Config bp) {
        std::vector<Element> out;
        Element body = ToolBodyPreview{std::move(bp)}.build();
        if (auto* box = as_box(body);
            box && box->layout.direction == FlexDirection::Column && !box->children.empty()) {
            out.reserve(box->children.size());
            for (auto& child : box->children)
                out.push_back(std::move(child) | height(1) | overflow(Overflow::Hidden));
        } else {
            out.push_back(std::move(body) | height(1) | overflow(Overflow::Hidden));
        }
        return out;
    }

    [[nodiscard]] Element build() const {
        const auto& c = cfg_;
        static const std::vector<Element> kNone;
        const auto& rows = c.rows ? *c.rows : kNone;
        std::vector<Element> tail_rows;
        if (c.tail) {
            tail_rows.push_back(text("") | height(1));
            tail_rows.push_back(hstack()(
                text("\xe2\x9c\xa6 ", c.tail->live ? fg(c.tail->brand) : dim(c.tail->brand)),   // ✦
                text(c.tail->live ? "Thinking" : "Reasoned", bold(c.tail->secondary)),
                text(c.tail->meta, Style{}.with_fg(c.tail->secondary).with_italic()) | clip | shrink(1.0f)
            ).build() | height(1) | overflow(Overflow::Hidden));
        }
        const int cached_rows = static_cast<int>(rows.size());
        const int total_rows  = cached_rows + static_cast<int>(tail_rows.size());

        // Never more viewport than there is output, so each fit pass removes
        // rows that are really there. Start from what fit last frame for THIS
        // entry at this size; a memo with room to spare is re-fit from the top.
        const int start_vh = std::clamp(c.base.viewport_h, 1, std::max(1, total_rows));
        int  vh = start_vh;
        bool hints = true, header = true, tight = false;
        bool from_memo = false;
        FitMemo* fit = c.memo;
        if (fit && fit->term_rows == c.term_rows && fit->term_cols == c.cols && fit->start == start_vh
            && fit->index == c.index && fit->thinking == c.thinking_only && fit->vh > 0) {
            vh = std::min(fit->vh, start_vh);
            hints = fit->hints; header = fit->header; tight = fit->tight;
            from_memo = true;
        }
        Element out;
        for (int pass = 0; pass < 8; ++pass) {
            auto pc = assemble(rows, tail_rows, total_rows, vh, hints, header);
            pc.tight = tight;
            out = Panel{std::move(pc)}.build();
            const int h = measure_element(out, c.cols).height.value;
            if (h <= c.budget) {
                if (from_memo && h < c.budget && (vh < start_vh || !hints || !header || tight)) {
                    from_memo = false;
                    vh = start_vh; hints = true; header = true; tight = false;
                    continue;
                }
                if (fit) *fit = {c.term_rows, c.cols, start_vh, c.index, vh, c.thinking_only, hints, header, tight};
                break;
            }
            from_memo = false;
            const int over = h - c.budget;
            // Keep at least 3 rows of output while there's chrome to drop.
            if (vh > 3)          vh = std::max(3, vh - over);
            else if (!tight)     tight = true;                  // then the frame's blank rows
            else if (hints)      hints = false;                 // then the key hints
            else if (header)     header = false;                // then the header
            else if (vh > 1)     vh = std::max(1, vh - over);   // last, the output itself
            else break;
        }
        return out;
    }

private:
    static Style fg(Color x)   { return Style{}.with_fg(x); }
    static Style bold(Color x) { return Style{}.with_fg(x).with_bold(); }
    static Style dim(Color x) {
        return theme::is_muted(x) ? Style{}.with_fg(x) : Style{}.with_fg(x).with_dim();
    }

    // Byte offset of codepoint `cp` in `s` (s.size() when past the end).
    static std::size_t cp_offset(std::string_view s, std::size_t cp) {
        std::size_t n = 0;
        for (std::size_t i = 0; i < s.size(); ++i)
            if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) {
                if (n == cp) return i;
                ++n;
            }
        return s.size();
    }

    // Header: name, what it ran on, how it went. Built at the row's real
    // width so the facts that fit are chosen against what's there: whole
    // ones, in order, never a cut word. The status never clips.
    [[nodiscard]] Element header_row() const {
        const Header hd = *cfg_.header;
        return detail::component([hd](int w, int) -> Element {
            std::string d = hd.detail;
            if (!hd.mid.empty()) {
                const int room = w - string_width(" " + hd.head) - string_width("  " + hd.trailing + " ");
                for (const auto& item : hd.mid) {
                    const std::string next = d.empty() ? item : d + " \xc2\xb7 " + item;
                    if (string_width("  " + next) > room) break;
                    d = next;
                }
            }
            return hstack().width(Dimension::percent(100))(
                text(" " + hd.head, hd.head_style) | clip | shrink(1.0f),
                text(d.empty() ? "" : "  " + d, fg(hd.text))
                    | clip | grow(1.0f) | shrink(50.0f),
                text("  " + hd.trailing + " ", hd.trail_style) | clip | shrink(1.0f)
            ).build();
        });
    }

    // The typewriter: rows past the cursor are blank; the row AT it has its
    // text leaf clipped and decorated with the transcript's reveal effect.
    [[nodiscard]] Element reveal_row(int i, Element row) const {
        const auto& r = cfg_.reveal;
        if (!r.on) return row;
        const auto k = static_cast<std::size_t>(i) - r.row0;
        if (static_cast<std::size_t>(i) < r.row0 || k >= r.cp_begin->size()) return row;
        const std::size_t b = (*r.cp_begin)[k], n = (*r.cp_len)[k];
        if (n == 0 || b + n <= r.typed) return row;              // fully typed
        if (b >= r.typed) return text("") | height(1);           // not yet
        TextElement* leaf = nullptr;
        std::function<void(Element&)> find = [&](Element& el) {
            std::visit([&](auto& node) {
                using T = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<T, TextElement>) leaf = &node;
                else if constexpr (std::is_same_v<T, BoxElement>)
                    for (auto& ch : node.children) find(ch);
            }, el.inner);
        };
        find(row);
        if (!leaf) return row;
        const std::size_t keep_cp = r.typed - b;
        const std::size_t cut = cp_offset(leaf->content, keep_cp);
        std::erase_if(leaf->runs, [&](const StyledRun& run) { return run.byte_offset >= cut; });
        for (auto& run : leaf->runs)
            run.byte_length = std::min(run.byte_length, cut - run.byte_offset);
        anim::TextRevealParams rp;
        rp.ms_total     = anim_now_ms();
        rp.edge_age_ms  = 0;
        rp.revealed_cp  = keep_cp;
        rp.total_cp     = n;
        rp.clip_active  = true;
        rp.clipped_unrevealed_cp = n - keep_cp;
        rp.line_bounded = true;
        rp.scramble_glyph_safe = true;
        rp.wrap_width   = 0;
        // Speed-scaled like the transcript, so the shimmer covers the same
        // spatial window at any rate.
        const double scale = std::clamp(r.rate > 1.0 ? 45.0 / r.rate : 1.0, 0.35, 2.5);
        rp.scramble_ms  = static_cast<std::int64_t>(rp.scramble_ms * scale);
        rp.char_step_ms = static_cast<std::int64_t>(rp.char_step_ms * scale);
        if (r.decorate) (void)anim::decorate_text_reveal(*leaf, rp);
        else            leaf->content.resize(cut);
        leaf->cached_width = -1;
        return row;
    }

    [[nodiscard]] Panel::Config assemble(const std::vector<Element>& rows,
                                         const std::vector<Element>& tail_rows,
                                         int total_rows, int vh, bool hints, bool header) const {
        const auto& c = cfg_;
        Panel::Config pc = c.base;
        if (c.header && header) {
            pc.header.push_back(header_row());
            pc.header.push_back(sep);
        }
        pc.scroll = nullptr;
        const int cached_rows = static_cast<int>(rows.size());
        auto& sc = *c.scroll;
        sc.max_y = std::max(0, total_rows - vh);
        sc.viewport_bounds.h = vh;   // page size for PgUp / PgDn
        sc.y = c.jump_to_hit ? c.current_row - vh / 3 : c.y_before;
        if (c.follow) sc.y = sc.max_y;
        sc.y = std::clamp(sc.y, 0, sc.max_y);
        const int first = sc.y;
        const int last  = std::min(total_rows, first + vh);
        std::vector<Element> win;
        for (int i = first; i < last; ++i) {
            if (i >= cached_rows) {
                win.push_back(tail_rows[static_cast<std::size_t>(i - cached_rows)]);
                continue;
            }
            // While searching, a one-column margin marks the current match.
            if (!c.query.empty())
                win.push_back(hstack().width(Dimension::percent(100))(
                    text(i == c.current_row ? "\xe2\x96\xb8" : " ", bold(c.warn)),   // ▸
                    rows[static_cast<std::size_t>(i)] | grow(1.0f) | shrink(1.0f)
                ).build() | height(1) | overflow(Overflow::Hidden));
            else
                win.push_back(reveal_row(i, rows[static_cast<std::size_t>(i)]));
        }
        if (total_rows == 0)
            win.push_back(text("  waiting for output\xe2\x80\xa6", Style{}.with_fg(c.muted).with_italic())
                          | height(1) | overflow(Overflow::Hidden));
        // The scrollbar column is always there, so text never reflows when
        // the output grows past the pane; it only gets a thumb then.
        const int win_h = std::max(1, static_cast<int>(win.size()));
        ScrollbarStyle bar = ScrollbarStyle::overlay();
        bar.thumb_color = c.base.accent;
        Element gutter = total_rows > vh
            ? scrollbar_y(sc, win_h, bar)
            : (text("") | width(1) | height(win_h));
        pc.prebuilt.push_back(hstack()(
            vstack()(std::move(win)) | grow(1.0f) | shrink(1.0f),
            std::move(gutter)).build());
        if (c.compact) return pc;

        // Status line: the find prompt or result, then where we are.
        std::vector<Element> status;
        if (c.typing || !c.query.empty()) {
            status.push_back(text("  /", bold(c.warn)) | clip);
            status.push_back(text(c.query, fg(c.text)) | clip | shrink(5.0f));
            if (c.typing) status.push_back(text("\xe2\x96\x8f", fg(c.warn)) | clip);   // ▏ caret
            std::string res = c.query.empty() ? ""
                : c.hits == 0 ? "  no matches"
                : "  " + std::to_string(c.current_hit + 1) + " of " + std::to_string(c.hits);
            status.push_back(text(res, c.hits == 0 && !c.query.empty() ? fg(c.danger) : dim(c.muted)) | clip);
        }
        std::string where;
        if (total_rows > vh) {
            const int pct = sc.max_y == 0 ? 100 : (100 * sc.y) / sc.max_y;
            where = sc.y == 0 ? "top" : sc.y >= sc.max_y ? "end" : std::to_string(pct) + "%";
        }
        if (!c.count.empty()) {
            if (!where.empty()) where += " \xc2\xb7 ";
            where += c.count;
        }
        if (c.following) where += where.empty() ? "following" : " \xc2\xb7 following";
        status.push_back(spacer());
        // New entries landed above while you read. Say so; never jump.
        if (c.fresh_above > 0)
            status.push_back(text(" \xe2\x86\x91 " + std::to_string(c.fresh_above) + " new ",   // ↑
                                  bold(c.highlight)) | clip);
        status.push_back(text(where + "  ", dim(c.where_color)) | clip);
        pc.footer.push_back(hstack().width(Dimension::percent(100))(std::move(status)).build()
                            | height(1) | overflow(Overflow::Hidden));
        if (!hints) return pc;
        pc.footer.push_back(KeyHints{KeyHints::Config{.hints = c.hints, .label = c.muted}}.build());
        return pc;
    }

    Config cfg_;
};

} // namespace tool_viewer_detail
using tool_viewer_detail::ToolViewer;
} // namespace maya
