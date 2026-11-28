#pragma once
// maya::DiffReview — the "review changes" pane: a bordered pane that walks
// pending file edits one change at a time.
//
//   ╭ Review changes ────────────────────────────────────────────────╮
//   │  Already saved. Keep or undo each change.     2 files  +5 −2   │
//   │  ▐ ○ login.cpp  +3 −1 ▌   README.md  +2                        │
//   │  src/auth/login.cpp                              change 1 of 2 │
//   │  ───────────────────────────────────────────────────────────── │
//   │  ▎ ○ line 4   +2 −1   f4                             to review │
//   │       3   3   int f3() { return 3; }                           │
//   │       4     - int f4() { return 4; }                           │
//   │           4 + int f4() { return -1; }                          │
//   │  ━━━━━━━━━━━━━━━━━━━━━━━━  0 of 3 reviewed   ✓ 0 kept   ✗ 0 …  │
//   │  y keep   n undo   ↑↓ change   …                               │
//   ╰────────────────────────────────────────────────────────────────╯
//
// Pure presentation. The host decides WHAT is shown -- which file, which
// window of changes, which lines of the focused change and which bytes of
// them changed -- and hands it over as data. HOW it looks (bands, gutters,
// syntax colour, the progress bar, the tabs, the border) is decided here.

#include <algorithm>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include "../../dsl.hpp"
#include "../../element/builder.hpp"
#include "../../style/border.hpp"
#include "../../style/color.hpp"
#include "../../style/style.hpp"
#include "../../style/theme.hpp"
#include "key_hints.hpp"
#include "markdown/highlight.hpp"
#include "tab_strip.hpp"
#include "tool_body_preview.hpp"   // diff_palette, terminal_color_level

namespace maya {

class DiffReview {
public:
    // The edits are already on disk, so the choice is keep or undo.
    enum class Status : uint8_t { Pending, Kept, Undone };

    struct Line {
        char sign = ' ';               // '+', '-', or ' '
        std::string code;              // sign stripped
        int old_no = 0, new_no = 0;    // 0 = none on that side
        // Byte ranges in `code` that changed against the paired line.
        std::vector<std::pair<std::size_t, std::size_t>> changed;
    };

    struct Change {                    // one row of the change list
        Status      status = Status::Pending;
        std::string where;             // "line 4" / "lines 4–9"
        int         added = 0, removed = 0;
        std::string scope;             // enclosing function, if any
        bool        focused = false;
        // Focused change only: the window of its lines, and what's past it.
        std::vector<Line> lines;
        int above = 0, below = 0;
    };

    struct File {                      // one tab
        std::string name;              // basename
        Status      roll = Status::Pending;
        int         added = 0, removed = 0;
    };

    struct Config {
        int files_n = 0, added = 0, removed = 0;   // totals across every file

        std::vector<File> files;
        int               current = 0;

        // The focused file.
        std::string dir, base;         // the path as the user thinks of it
        std::string kind;              // "new file" / "deleted" / ""
        bool        kind_deletes = false;
        int         changes_n = 0, focused_index = 0;
        std::string lang;              // syntax tag (extension)
        int         gutter_w = 2;

        // The window onto this file's changes.
        int                 hidden_above = 0, hidden_below = 0;
        std::vector<Change> changes;

        int  total = 0, kept = 0, undone = 0;
        bool all_done = false;

        bool                 confirm_undo_all = false;   // the two-press guard
        std::vector<KeyHint> hints;

        Color accent    = Color::slot(ThemeSlot::Accent);
        Color text      = Color::slot(ThemeSlot::Text);
        Color muted     = Color::slot(ThemeSlot::Muted);
        Color success   = Color::slot(ThemeSlot::Success);
        Color danger    = Color::slot(ThemeSlot::Error);
        Color warn      = Color::slot(ThemeSlot::Warning);
    };

    explicit DiffReview(Config c) : cfg_(std::move(c)) {}
    operator Element() const { return build(); }

    // Nothing to review: an empty element, so a host never builds one.
    [[nodiscard]] static Element empty() { return dsl::nothing(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;
        const auto& c = cfg_;
        std::vector<Element> rows;

        // ── What this is, in one line, and the size of it ──
        {
            const std::string size = plural(c.files_n, "file", "files")
                                   + std::format("  +{} \xe2\x88\x92{}", c.added, c.removed);
            rows.push_back(detail::component([c, size](int w, int) -> Element {
                const std::string longer = "Already saved. Keep or undo each change.";
                const std::string shorter = "Keep or undo each change.";
                const int sw = string_width(size) + 2;
                const std::string& msg = string_width(longer) + sw <= w ? longer : shorter;
                std::vector<Element> p;
                if (string_width(msg) + sw <= w) p.push_back(text(msg, fg(c.text)));
                p.push_back(spacer());
                p.push_back(text(plural(c.files_n, "file", "files") + "  ", dim(c.muted)));
                p.push_back(text(std::format("+{}", c.added), fg(c.success)));
                p.push_back(text(" "));
                p.push_back(text(std::format("\xe2\x88\x92{}", c.removed), fg(c.danger)));
                return h(std::move(p)).build();
            }));
        }
        rows.push_back(text(""));

        // ── One tab per file ──
        // The open file is a soft pill holding its status dot, name and
        // diffstat; the rest sit plain beside it. On a 16-colour terminal the
        // selection is the bold name between the chip's ends instead.
        {
            TabStrip tabs;
            tabs.marker(TabMark::Pill);
            tabs.indent = 0;
            tabs.theme.active = c.text;
            tabs.theme.idle   = c.muted;
            tabs.theme.accent = c.accent;
            if (bands_on()) tabs.theme.active_bg = Color::slot(ThemeSlot::Selection);
            for (const auto& f : c.files)
                tabs.tab(f.name)
                    .dot(status_color(f.roll), std::string(status_dot(f.roll)))
                    .part(f.added ? std::format("+{}", f.added) : std::string{}, c.success)
                    .part(f.removed ? std::format("\xe2\x88\x92{}", f.removed) : std::string{}, c.danger);
            tabs.active(c.current);
            rows.push_back(tabs.build());
        }
        rows.push_back(text(""));

        // ── The current file: full path, then its changes ──
        rows.push_back(h(
            text(c.dir, dim(c.muted)),
            text(c.base, bold(c.text)),
            text(c.kind.empty() ? std::string{} : "  " + c.kind,
                 bold(c.kind_deletes ? c.danger : c.success)),
            spacer(),
            text(c.changes_n > 1 ? std::format("change {} of {}", c.focused_index + 1, c.changes_n)
                                 : std::string{"1 change"},
                 dim(c.muted))
        ).build());
        rows.push_back(rule(c.muted));

        if (c.hidden_above > 0)
            rows.push_back(text(std::format("  \xe2\x86\x91 {} more {} above", c.hidden_above,
                                            c.hidden_above == 1 ? "change" : "changes"),
                                dim(c.muted)));
        const auto lang = syntax::lang_from_tag(c.lang);
        for (const auto& ch : c.changes) {
            // One line per change: where it is, how big, and what you decided.
            rows.push_back(h(
                text(ch.focused ? "\xe2\x96\x8e " : "  ", bold(c.accent)),
                text(status_dot(ch.status), fg(status_color(ch.status))),
                text(" "),
                text(ch.where, ch.focused ? bold(c.text) : fg(c.muted)),
                text("   "),
                text(std::format("+{}", ch.added), ch.added ? fg(c.success) : dim(c.muted)),
                text(" "),
                text(std::format("\xe2\x88\x92{}", ch.removed), ch.removed ? fg(c.danger) : dim(c.muted)),
                text(ch.scope.empty() ? "" : "   " + ch.scope, dim(c.muted)) | clip | shrink(1.0f),
                spacer(),
                text(status_word(ch.status), ch.focused || ch.status != Status::Pending
                                                 ? fg(status_color(ch.status))
                                                 : dim(c.muted))
            ).build());
            if (!ch.focused) continue;

            // The focused change's lines (the host sized the window).
            rows.push_back(text(""));
            if (ch.above > 0)
                rows.push_back(text(std::format("    \xe2\x86\x91 {} more above", ch.above), dim(c.muted)));
            for (const auto& ln : ch.lines) {
                auto row = h(code_line(ln, lang, c.gutter_w)) | grow(1.0f);
                if (bands_on() && ln.sign == '+')      row = std::move(row) | bgc(diff_palette::add_bg);
                else if (bands_on() && ln.sign == '-') row = std::move(row) | bgc(diff_palette::rem_bg);
                rows.push_back((h(std::move(row)) | padding(0, 0, 0, 2)).build());
            }
            if (ch.below > 0)
                rows.push_back(text(std::format("    \xe2\x86\x93 {} more below", ch.below), dim(c.muted)));
            rows.push_back(text(""));
        }
        if (c.hidden_below > 0)
            rows.push_back(text(std::format("  \xe2\x86\x93 {} more {} below", c.hidden_below,
                                            c.hidden_below == 1 ? "change" : "changes"),
                                dim(c.muted)));

        rows.push_back(progress());
        rows.push_back(text(""));

        // ── Keys ──
        if (c.confirm_undo_all) {
            rows.push_back(h(
                text("\xe2\x9a\xa0 ", bold(c.danger)),
                text("press ^X again to undo every change in every file", bold(c.danger)),
                text("  \xc2\xb7  any other key cancels", dim(c.muted))
            ).build());
        } else {
            rows.push_back(KeyHints{KeyHints::Config{.hints = c.hints, .label = c.muted}}.build());
        }

        return (v(std::move(rows))
                | padding(1, 2)
                | border(BorderStyle::Round)
                | bcolor(c.all_done ? c.success : c.accent)
                | btext(" Review changes ", BorderTextPos::Top, BorderTextAlign::Start)
               ).build();
    }

private:
    static Style fg(Color x)   { return Style{}.with_fg(x); }
    static Style bold(Color x) { return Style{}.with_fg(x).with_bold(); }
    static Style dim(Color x) {
        return theme::is_muted(x) ? Style{}.with_fg(x) : Style{}.with_fg(x).with_dim();
    }
    static std::string plural(int n, const char* one, const char* many) {
        return std::format("{} {}", n, n == 1 ? one : many);
    }
    static Element rule(Color x) {
        return detail::component([x](int w, int) -> Element {
            std::string s;
            for (int i = 0; i < w; ++i) s += "\xe2\x94\x80";   // ─
            return dsl::text(std::move(s), dim(x)).build();
        });
    }
    // Red/green row bands, like the edit cards. Off below 256 colours.
    [[nodiscard]] static bool bands_on() noexcept { return terminal_color_level() >= 2; }

    [[nodiscard]] static const char* status_dot(Status s) {
        switch (s) {
            case Status::Kept:    return "\xe2\x9c\x93";   // ✓
            case Status::Undone:  return "\xe2\x9c\x97";   // ✗
            case Status::Pending: return "\xe2\x97\x8b";   // ○
        }
        return " ";
    }
    [[nodiscard]] static const char* status_word(Status s) {
        switch (s) {
            case Status::Kept:    return "kept";
            case Status::Undone:  return "undone";
            case Status::Pending: return "to review";
        }
        return "";
    }
    [[nodiscard]] Color status_color(Status s) const {
        switch (s) {
            case Status::Kept:    return cfg_.success;
            case Status::Undone:  return cfg_.danger;
            case Status::Pending: return cfg_.warn;
        }
        return cfg_.muted;
    }

    // ── Progress: a bar that takes what the counts leave, and plain counts ──
    [[nodiscard]] Element progress() const {
        return detail::component([c = cfg_](int w, int) -> Element {
            using namespace dsl;
            const int decided = c.kept + c.undone;
            std::string counts = std::format("{} of {} reviewed", decided, c.total);
            const std::string ks = std::format("\xe2\x9c\x93 {} kept", c.kept);
            const std::string us = std::format("\xe2\x9c\x97 {} undone", c.undone);
            const int full = string_width(counts) + 3 + string_width(ks) + 3 + string_width(us);
            const bool tallies = full + 2 + 8 <= w;
            const int used = (tallies ? full : string_width(counts)) + 2;
            const int bar_w = std::clamp(w - used, 0, 24);
            const int fill_k = c.total ? (c.kept * bar_w + c.total / 2) / c.total : 0;
            const int fill_u = c.total ? std::min(bar_w - fill_k, (c.undone * bar_w + c.total / 2) / c.total) : 0;
            std::string bk, bu, br;
            for (int i = 0; i < fill_k; ++i) bk += "\xe2\x94\x81";   // ━
            for (int i = 0; i < fill_u; ++i) bu += "\xe2\x94\x81";
            for (int i = fill_k + fill_u; i < bar_w; ++i) br += "\xe2\x94\x81";
            std::vector<Element> p;
            if (bar_w > 0) {
                p.push_back(text(bk, fg(c.success)));
                p.push_back(text(bu, fg(c.danger)));
                p.push_back(text(br, dim(c.muted)));
                p.push_back(text("  "));
            }
            p.push_back(text(std::move(counts), fg(c.text)));
            if (tallies) {
                p.push_back(text("   "));
                p.push_back(text(ks, c.kept ? fg(c.success) : dim(c.muted)));
                p.push_back(text("   "));
                p.push_back(text(us, c.undone ? fg(c.danger) : dim(c.muted)));
            }
            return h(std::move(p)).build();
        });
    }

    // One code line as a SINGLE text element with per-byte styled runs, so
    // it clips its tail cleanly at the pane width instead of squeezing spans.
    // Added/removed lines keep syntax colours on a band; context is dimmed.
    [[nodiscard]] Element code_line(const Line& dl, syntax::Lang lang, int gutter_w) const {
        const bool add = dl.sign == '+', del = dl.sign == '-';
        const Color sign_c = add ? cfg_.success : del ? cfg_.danger : cfg_.muted;
        std::string content;
        std::vector<StyledRun> runs;

        auto num = [gutter_w](int n) {
            std::string s = n > 0 ? std::to_string(n) : "";
            s.insert(s.begin(), static_cast<std::size_t>(std::max(0, gutter_w - static_cast<int>(s.size()))), ' ');
            return s;
        };
        const std::string gut = num(dl.old_no) + " " + num(dl.new_no) + " ";
        const bool on_band = bands_on() && (add || del);
        const Color rail    = add ? diff_palette::add_rail  : diff_palette::rem_rail;
        const Color rail_fg = add ? diff_palette::add_fg_hi : diff_palette::rem_fg_hi;
        runs.push_back({content.size(), gut.size(),
                        on_band ? Style{}.with_fg(rail_fg).with_bg(rail).with_bold()
                                : Style{}.with_fg(cfg_.muted).with_dim()});
        content += gut;
        const std::string sign_s = add ? "+ " : del ? "- " : "  ";
        runs.push_back({content.size(), sign_s.size(),
                        on_band ? Style{}.with_fg(rail_fg).with_bg(rail).with_bold()
                                : Style{}.with_fg(sign_c)});
        content += sign_s;

        const std::size_t code_off = content.size();
        content += dl.code;
        const auto spans = syntax::highlight(dl.code, lang);
        const auto& theme = syntax::themes::terminal;
        const auto& lt = theme::live();
        const LitColor band  = lt.resolve(add ? diff_palette::add_bg : diff_palette::rem_bg);
        const LitColor plain = lt.resolve(add ? diff_palette::add_fg : diff_palette::rem_fg);
        auto push = [&](std::size_t from, std::size_t to, syntax::Capture cap) {
            if (to <= from) return;
            Style st = theme.style_for(cap);
            if (on_band) {
                LitColor fgc = st.fg ? theme::measure(lt.resolve(*st.fg)) : plain;
                if (!fgc.has_channels()) fgc = plain;
                st = st.with_fg(readable_on(fgc, band, 6.0)).with_bg(band);
            } else if (dl.sign == ' ') {
                st = st.with_dim();
            }
            runs.push_back({code_off + from, to - from, st});
        };
        std::size_t pos = 0;
        for (const auto& sp : spans) {
            if (sp.start > pos) push(pos, sp.start, syntax::Capture::None);
            push(sp.start, std::min<std::size_t>(sp.start + sp.len, dl.code.size()), sp.cap);
            pos = sp.start + sp.len;
        }
        if (pos < dl.code.size()) push(pos, dl.code.size(), syntax::Capture::None);

        // The words that changed get the strong band on top, like an
        // editor's inline diff; below 256 colours, bold + underline.
        if (!dl.changed.empty()) {
            const LitColor hot = lt.resolve(add ? diff_palette::add_rail : diff_palette::rem_rail);
            auto changed_at = [&](std::size_t b) {
                for (const auto& [o, l] : dl.changed)
                    if (b >= code_off + o && b < code_off + o + l) return true;
                return false;
            };
            std::vector<StyledRun> split;
            split.reserve(runs.size() + dl.changed.size() * 2);
            for (const auto& r : runs) {
                if (r.byte_offset < code_off) { split.push_back(r); continue; }
                std::size_t s = r.byte_offset;
                const std::size_t e = r.byte_offset + r.byte_length;
                while (s < e) {
                    const bool hit = changed_at(s);
                    std::size_t t = s + 1;
                    while (t < e && changed_at(t) == hit) ++t;
                    StyledRun piece = r;
                    piece.byte_offset = s;
                    piece.byte_length = t - s;
                    if (hit) {
                        if (on_band) {
                            LitColor f = piece.style.fg ? theme::measure(lt.resolve(*piece.style.fg)) : plain;
                            if (!f.has_channels()) f = plain;
                            piece.style = piece.style.with_bg(hot)
                                              .with_fg(readable_on(f, hot, 6.0)).with_bold();
                        } else {
                            piece.style = piece.style.with_bold().with_underline();
                        }
                    }
                    split.push_back(piece);
                    s = t;
                }
            }
            runs = std::move(split);
        }

        return Element{TextElement{.content = std::move(content),
                                   .wrap = TextWrap::TruncateEnd,
                                   .runs = std::move(runs)}};
    }

    Config cfg_;
};

} // namespace maya
