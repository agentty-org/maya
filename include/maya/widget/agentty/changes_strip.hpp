#pragma once
// maya::widget::ChangesStrip — bordered "session has pending changes" banner.
//
// Header row + file list inside a rounded border. The header carries the
// "Changes (N files)" label and key hints for review / accept / reject.
// The body is a list of FileChange rows. When `changes` is empty the
// widget renders to an empty Element so callers can drop it into a
// vertical stack without a conditional.
//
//   maya::ChangesStrip{{
//       .changes      = file_change_vector,
//       .border_color = Color::slot(ThemeSlot::Warning),
//   }}.build();

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../../dsl.hpp"
#include "../../element/element.hpp"
#include "../../element/text.hpp"
#include "../../style/border.hpp"
#include "../../style/color.hpp"
#include "../../style/style.hpp"
#include "../../style/theme.hpp"
#include "file_change.hpp"   // the record only, not the FileChanges widget
#include "scrollbar.hpp"

namespace maya {

class ChangesStrip {
public:
    struct Config {
        std::vector<FileChange> changes;

        // One line instead of a box: "▎ ✎ 26 files  +1048 −966   ^R review
        // ^A accept  ^X reject". The banner exists to say THAT edits await
        // review and how to act on them; the per-file list is what the
        // review pane (^R) is for, and 26 rows of it pushed the transcript
        // off the screen.
        bool compact = false;
        // Full form: the list is a WINDOW of at most this many rows onto all
        // the files, scrolled by `scroll` (the first row shown), with an
        // overlay scrollbar when it doesn't fit. 0 = no window (every row).
        int max_rows = 8;
        int scroll   = 0;
        // The row the host's selection is on (-1 = none): drawn with a ▸
        // marker and kept inside the window by the host's scroll.
        int selected = -1;
        // Paths are shown relative to this when it is a prefix ("" = as is).
        std::string root;
        // Advertise the scroll keys in the footer (the host binds them while
        // the strip is focused; omit when it doesn't).
        bool scroll_keys = true;

        // Brand palette
        Color border_color  = Color::slot(ThemeSlot::Warning);
        Color text_color    = Color::slot(ThemeSlot::Text);
        Color accept_color  = Color::slot(ThemeSlot::Success);
        Color reject_color  = Color::slot(ThemeSlot::Error);
    };

    explicit ChangesStrip(Config c) : cfg_(std::move(c)) {}

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;

        if (cfg_.changes.empty()) return text("");

        const Color muted = Color::slot(ThemeSlot::Muted);

        if (cfg_.compact) return build_compact(muted);

        // Header is a fit_row: the "Changes (N files)" label and the grow
        // spacer are essential; the key hints shed lowest-keep first
        // (review → reject → accept) when the strip is too narrow —
        // measured, so a relabeled hint re-decides by itself.
        //
        // The chords are Ctrl-held because accept/reject-all are bulk
        // decisions over files already written to disk; the host binds the
        // same ^A/^X inside its review pane, so the hint must not promise a
        // bare letter that does nothing.
        int all_add = 0, all_rm = 0;
        for (const auto& c : cfg_.changes) { all_add += c.lines_added; all_rm += c.lines_removed; }
        std::vector<Element> label;
        label.push_back(text("Changes ", Style{}.with_fg(cfg_.border_color).with_bold()));
        label.push_back(text("(" + std::to_string(cfg_.changes.size()) + " files)",
                             Style{}.with_fg(muted)));
        if (all_add > 0)
            label.push_back(text("  +" + std::to_string(all_add), Style{}.with_fg(cfg_.accept_color)));
        if (all_rm > 0)
            label.push_back(text("  \xe2\x88\x92" + std::to_string(all_rm), Style{}.with_fg(cfg_.reject_color)));
        std::vector<FitItem> header;
        header.push_back({h(std::move(label)).build()});
        header.push_back({Element{spacer().build()}});
        header.push_back({h(
            text("Ctrl+R", Style{}.with_fg(cfg_.text_color)),
            text(" review  ", Style{}.with_fg(muted))).build(), 1});
        header.push_back({h(
            text("Ctrl+A", Style{}.with_fg(cfg_.accept_color)),
            text(" accept  ", Style{}.with_fg(muted))).build(), 3});
        header.push_back({h(
            text("Ctrl+X", Style{}.with_fg(cfg_.reject_color)),
            text(" reject", Style{}.with_fg(muted))).build(), 2});

        const int n   = static_cast<int>(cfg_.changes.size());
        const int win = (cfg_.max_rows > 0) ? std::min(n, cfg_.max_rows) : n;
        const int top = std::clamp(cfg_.scroll, 0, std::max(0, n - win));
        const bool scrolls = win < n;

        // The rows are laid out at the width the strip is given, so paths
        // are shortened in the MIDDLE (the file name is what you scan for)
        // and the +/- counts sit in one right-aligned column.
        int add_w = 0, rm_w = 0;
        for (const auto& c : cfg_.changes) {
            add_w = std::max(add_w, digits(c.lines_added));
            rm_w  = std::max(rm_w,  digits(c.lines_removed));
        }
        auto rows = component([cfg = cfg_, top, win, add_w, rm_w, muted](int w, int /*h*/) -> Element {
            std::vector<Element> out;
            out.reserve(static_cast<std::size_t>(win));
            const int counts_w = 2 + 1 + add_w + 2 + 1 + rm_w;   // "  +NNN  −MM"
            for (int i = top; i < top + win; ++i) {
                const auto& c = cfg.changes[static_cast<std::size_t>(i)];
                const bool sel = i == cfg.selected;
                std::string path = c.path;
                if (!cfg.root.empty() && path.starts_with(cfg.root)) {
                    path.erase(0, cfg.root.size());
                    if (!path.empty() && path.front() == '/') path.erase(0, 1);
                }
                const int avail_w = std::max(4, w - 4 - counts_w);
                // Counts sit one gap after the LONGEST path, not at the far
                // edge: on a wide terminal the eye had to cross 70 columns of
                // nothing to pair a file with its +/-.
                int longest = 0;
                for (int j = top; j < top + win; ++j) {
                    std::string pj = cfg.changes[static_cast<std::size_t>(j)].path;
                    if (!cfg.root.empty() && pj.starts_with(cfg.root)) {
                        pj.erase(0, cfg.root.size());
                        if (!pj.empty() && pj.front() == '/') pj.erase(0, 1);
                    }
                    longest = std::max(longest, string_width(pj));
                }
                const int path_w = std::min(avail_w, longest);
                if (string_width(path) > path_w) path = truncate_middle(path, path_w);
                const int pad = std::max(0, path_w - string_width(path));
                const Color kc = kind_color(c.kind);
                std::vector<Element> line;
                line.push_back(text(sel ? "\xe2\x96\xb8 " : "  ",                     // ▸
                                    Style{}.with_fg(cfg.border_color).with_bold()));
                line.push_back(text(kind_glyph(c.kind), Style{}.with_fg(kc).with_bold()));
                line.push_back(text(" "));
                // The directory dim, the file name bright: the eye finds the
                // name without reading the path.
                const auto slash = path.rfind('/');
                if (slash != std::string::npos && slash + 1 < path.size()) {
                    line.push_back(text(path.substr(0, slash + 1), Style{}.with_fg(muted)));
                    line.push_back(text(path.substr(slash + 1),
                                        sel ? Style{}.with_fg(cfg.text_color).with_bold()
                                            : Style{}.with_fg(cfg.text_color)));
                } else {
                    line.push_back(text(path, Style{}.with_fg(cfg.text_color)));
                }
                line.push_back(text(std::string(static_cast<std::size_t>(pad), ' ')));
                line.push_back(text("  +" + rpad(c.lines_added, add_w),
                                    c.lines_added ? Style{}.with_fg(cfg.accept_color)
                                                  : Style{}.with_fg(muted).with_dim()));
                line.push_back(text("  \xe2\x88\x92" + rpad(c.lines_removed, rm_w),
                                    c.lines_removed ? Style{}.with_fg(cfg.reject_color)
                                                    : Style{}.with_fg(muted).with_dim()));
                out.push_back(h(std::move(line)).build());
            }
            return v(std::move(out)).build();
        });

        // The scrollbar: the same overlay style as maya's panels (half-cell
        // thumb, no track line beside the border), in the strip's colour.
        // Only when there is somewhere to scroll; the column is reserved
        // either way so rows never reflow when the list grows past the
        // window.
        Element gutter = text("") | width(1) | height(win);
        if (scrolls) {
            // scrollbar_y BORROWS its state for the frame (the renderer writes
            // the bar's hit-rect back into it during paint), so it can't be a
            // local here. A component owns one for exactly as long as the
            // Element lives; the strip has no mouse, so the write-back is
            // just discarded with it.
            ScrollbarStyle bar = ScrollbarStyle::overlay();
            bar.thumb_color = cfg_.border_color;
            bar.thumb_follows_accent = false;
            auto st = std::make_shared<ScrollState>();
            st->max_y = n - win;
            st->y     = top;
            gutter = component([st, win, bar](int, int) -> Element {
                return scrollbar_y(*st, win, bar);
            }) | width(1) | height(win);
        }

        std::vector<Element> body;
        body.push_back(detail::fit_row(std::move(header)));
        body.push_back(h(std::move(rows) | grow(1.0f), std::move(gutter)).build());
        if (scrolls) {
            // Where you are, and how to move: the window says nothing about
            // what's above or below it on its own.
            std::vector<FitItem> foot;
            const int above = top, below = n - top - win;
            std::string where = std::to_string(top + 1) + "\xe2\x80\x93"               // –
                              + std::to_string(top + win) + " of " + std::to_string(n);
            std::vector<Element> pos;
            pos.push_back(text(above ? "\xe2\x86\x91" : " ", Style{}.with_fg(cfg_.border_color)));   // ↑
            pos.push_back(text(below ? "\xe2\x86\x93" : " ", Style{}.with_fg(cfg_.border_color)));   // ↓
            pos.push_back(text(" " + where, Style{}.with_fg(muted)));
            foot.push_back({h(std::move(pos)).build()});
            foot.push_back({Element{spacer().build()}});
            if (cfg_.scroll_keys)
                foot.push_back({h(
                    text("Alt+\xe2\x86\x91\xe2\x86\x93", Style{}.with_fg(cfg_.text_color)),  // ↑↓
                    text(" scroll  ", Style{}.with_fg(muted)),
                    text("Ctrl+R", Style{}.with_fg(cfg_.text_color)),
                    text(" review all", Style{}.with_fg(muted))).build(), 1});
            body.push_back(detail::fit_row(std::move(foot)));
        }
        return (v(std::move(body))
                | border(BorderStyle::Round)
                | bcolor(cfg_.border_color)
                | padding(0, 1)
               ).build();
    }

private:
    Config cfg_;

    static int digits(int v) { int d = 1; for (v = std::max(0, v); v >= 10; v /= 10) ++d; return d; }
    static std::string rpad(int v, int w) {
        std::string s = std::to_string(v);
        return std::string(static_cast<std::size_t>(std::max(0, w - static_cast<int>(s.size()))), ' ') + s;
    }
    static const char* kind_glyph(FileChangeKind k) {
        switch (k) {
            case FileChangeKind::Created:  return "+";
            case FileChangeKind::Deleted:  return "\xe2\x88\x92";   // −
            case FileChangeKind::Renamed:  return "\xe2\x86\x92";   // →
            case FileChangeKind::Modified: break;
        }
        return "~";
    }
    static Color kind_color(FileChangeKind k) {
        switch (k) {
            case FileChangeKind::Created:  return Color::slot(ThemeSlot::Success);
            case FileChangeKind::Deleted:  return Color::slot(ThemeSlot::Error);
            case FileChangeKind::Renamed:  return Color::slot(ThemeSlot::Info);
            case FileChangeKind::Modified: break;
        }
        return Color::slot(ThemeSlot::Warning);
    }

    // The one-line form. A fit_row like the full header: the count and the
    // totals are essential, the key hints shed (review last) when narrow.
    [[nodiscard]] Element build_compact(Color muted) const {
        using namespace dsl;
        int add = 0, rm = 0;
        for (const auto& c : cfg_.changes) { add += c.lines_added; rm += c.lines_removed; }
        const std::size_t n = cfg_.changes.size();

        // Same grammar as the composer's hint row just below it: bold key,
        // dim label, " · " between clusters, all of it in one run from the
        // left. Boxed like the composer (rounded, its own colour, the same
        // width) so it reads as a panel of its own rather than loose text
        // hanging off the transcript; one content row, three rows in all.
        // The hints read the review pane's words, so a chord means the same
        // thing on both sides of ^R.
        auto dim = [muted] {
            return theme::is_muted(muted) ? Style{}.with_fg(muted)
                                          : Style{}.with_fg(muted).with_dim();
        };
        auto sep  = [&] { return text(" \xc2\xb7 ", dim()); };              // ·
        // Key hints sit flush RIGHT, in the review pane's footer grammar
        // (key, label, three spaces apart); the left side is what changed.
        auto hint = [&](const char* k, const char* l, Color kc) {
            return h(text("   "),
                     text(k, Style{}.with_fg(kc).with_bold()),
                     text(l, dim())).build();
        };

        // Only the glyph and the count are essential. Narrowing sheds, in
        // order: undo all, keep all, the word "changed", review, the totals
        // — so the row never wraps, down to "✎ 26 files".
        auto lead = h(text("\xe2\x9c\x8e ", Style{}.with_fg(cfg_.border_color)),   // ✎
                      text(std::to_string(n), Style{}.with_fg(cfg_.text_color).with_bold()),
                      text(n == 1 ? " file" : " files", dim())).build();
        std::vector<Element> totals;
        if (add > 0 || rm > 0) totals.push_back(sep());
        if (add > 0)
            totals.push_back(text("+" + std::to_string(add), Style{}.with_fg(cfg_.accept_color)));
        if (add > 0 && rm > 0) totals.push_back(text(" "));
        if (rm > 0)
            totals.push_back(text("\xe2\x88\x92" + std::to_string(rm), Style{}.with_fg(cfg_.reject_color)));

        std::vector<FitItem> row;
        row.push_back({std::move(lead)});
        row.push_back({text(" changed", dim()).build(), 4});
        if (!totals.empty()) row.push_back({h(std::move(totals)).build(), 6});
        row.push_back({Element{spacer().build()}});   // pushes the keys right
        row.push_back({hint("^R", " review",   cfg_.text_color),   5});
        row.push_back({hint("^A", " keep all", cfg_.accept_color), 3});
        row.push_back({hint("^X", " undo all", cfg_.reject_color), 2});
        return (v(detail::fit_row(std::move(row)).build())
                | border(BorderStyle::Round)
                | bcolor(cfg_.border_color)
                | padding(0, 1)
               ).build();
    }
};

} // namespace maya
