#pragma once
// maya::widget::ChangesStrip — bordered "session has pending changes" banner.
//
// Header row + file list inside a rounded border. The header carries the
// "Changes (N files)" label and key hints for review / accept / reject.
// The body is a maya::FileChanges list. When `changes` is empty the
// widget renders to an empty Element so callers can drop it into a
// vertical stack without a conditional.
//
//   maya::ChangesStrip{{
//       .changes      = file_change_vector,
//       .border_color = Color::slot(ThemeSlot::Warning),
//   }}.build();

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "../dsl.hpp"
#include "../element/element.hpp"
#include "../style/border.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"
#include "file_changes.hpp"

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
        // Full form only: list at most this many files, then "… N more".
        // 0 = no cap.
        int max_rows = 8;

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

        FileChanges fc;
        // The totals are the header's (all files); the list below may be a
        // subset, so its own "N files changed" line would state the wrong N.
        fc.set_summary(false);
        const std::size_t n = cfg_.changes.size();
        const std::size_t shown = (cfg_.max_rows > 0)
            ? std::min(n, static_cast<std::size_t>(cfg_.max_rows)) : n;
        for (std::size_t i = 0; i < shown; ++i) fc.add(cfg_.changes[i]);

        std::vector<Element> body;
        body.push_back(detail::fit_row(std::move(header)));
        body.push_back(fc.build());
        if (shown < n)
            body.push_back(text("  \xe2\x80\xa6 " + std::to_string(n - shown)
                                + " more \xc2\xb7 Ctrl+R to review all",
                                Style{}.with_fg(muted)));
        return (v(std::move(body))
                | border(BorderStyle::Round)
                | bcolor(cfg_.border_color)
                | padding(0, 1)
               ).build();
    }

private:
    Config cfg_;

    // The one-line form. A fit_row like the full header: the count and the
    // totals are essential, the key hints shed (review last) when narrow.
    [[nodiscard]] Element build_compact(Color muted) const {
        using namespace dsl;
        int add = 0, rm = 0;
        for (const auto& c : cfg_.changes) { add += c.lines_added; rm += c.lines_removed; }
        const std::size_t n = cfg_.changes.size();

        std::vector<FitItem> row;
        std::vector<Element> lead;
        lead.push_back(text("\xe2\x96\x8e ", Style{}.with_fg(cfg_.border_color)));       // ▎
        lead.push_back(text("\xe2\x9c\x8e ", Style{}.with_fg(cfg_.border_color).with_bold()));  // ✎
        lead.push_back(text(std::to_string(n) + (n == 1 ? " file" : " files"),
                            Style{}.with_fg(cfg_.text_color).with_bold()));
        if (add > 0)
            lead.push_back(text("  +" + std::to_string(add), Style{}.with_fg(cfg_.accept_color)));
        if (rm > 0)
            lead.push_back(text("  \xe2\x88\x92" + std::to_string(rm), Style{}.with_fg(cfg_.reject_color)));
        row.push_back({h(std::move(lead)).build()});
        row.push_back({Element{spacer().build()}});
        row.push_back({h(
            text("^R", Style{}.with_fg(cfg_.text_color)),
            text(" review  ", Style{}.with_fg(muted))).build(), 3});
        row.push_back({h(
            text("^A", Style{}.with_fg(cfg_.accept_color)),
            text(" accept  ", Style{}.with_fg(muted))).build(), 2});
        row.push_back({h(
            text("^X", Style{}.with_fg(cfg_.reject_color)),
            text(" reject ", Style{}.with_fg(muted))).build(), 1});
        return detail::fit_row(std::move(row));
    }
};

} // namespace maya
