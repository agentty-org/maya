#pragma once
// maya::widget::PeekCard — a framed, fixed-height preview section.
//
// Sits in a Panel's header (Panel::Config::header), between the filter and
// the list, at a fixed height so the list below never jumps as the cursor
// moves:
//
//   ╭─ Preview ─────────────────────────────────────────────────────╮
//   │ Fix windows sandbox cancel                   2h ago · 48 msgs │
//   │ agentty · ⑂ fork                                              │
//   │ asked   cancel takes 20s on windows when ping runs inside cmd │
//   │ ───────────────────────────────────────────────────────────── │
//   │ you     merge it and fix the issues                           │
//   │ agentty merged locally, three fixes on top.                   │
//   │         ▸ shell                                               │
//   ╰──────────────────────────────────── ← → turns 26/26 · Enter ──╯
//
// The title is bold with the facts right-aligned on its row, and every
// transcript line shares one speaker column so text lines up. The host
// passes the transcript already windowed to transcript_rows() lines.

#include <algorithm>
#include <string>
#include <vector>

#include "../dsl.hpp"
#include "../element/builder.hpp"
#include "../element/text.hpp"
#include "../style/border.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

struct PeekCard {
    enum class Kind { User, Reply, Tool, Gap, Note };
    struct Line {
        std::string who;   // speaker; "" on a continuation line
        std::string text;
        Kind        kind = Kind::Reply;
    };
    struct Config {
        std::string       label = "Preview";   // on the top border
        std::string       title;
        std::string       facts;      // right of the title: "2h ago · 48 msgs"
        std::string       subtitle;   // second row: "agentty · ⑂ fork"
        std::string       asked;      // the first question, pinned
        std::vector<Line> lines;      // the transcript, already windowed
        std::string       hint;       // on the bottom border, right
        int               rows = 10;  // the whole card, borders included
    };

    Config cfg;

    // How many transcript lines fit: the card's height minus its borders,
    // the title, the subtitle and asked rows when set, and the divider.
    [[nodiscard]] static int transcript_rows(const Config& c) {
        return std::max(0, c.rows - 2 - 1 - (c.subtitle.empty() ? 0 : 1)
                                   - (c.asked.empty() ? 0 : 1) - 1);
    }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        const Color frame  = Color::slot(ThemeSlot::Accent);
        const Color text_c = Color::slot(ThemeSlot::Text);
        const Color muted  = Color::slot(ThemeSlot::Muted);
        const Color info   = Color::slot(ThemeSlot::Info);
        const Color rule   = Color::slot(ThemeSlot::Border);

        // One speaker column for every transcript line, so text lines up.
        constexpr int kWho = 8;
        auto who_col = [&](std::string who) {
            if (string_width(who) < kWho)
                who.append(static_cast<std::size_t>(kWho - string_width(who)), ' ');
            return who;
        };

        // A row laid out at its real width: an optional lead (speaker), the
        // body ellipsised to fit, and an optional tail flush to the right.
        auto row = [](std::string lead, Style lead_st, std::string body, Style body_st,
                      std::string tail = {}, Style tail_st = {}) {
            return Element{ComponentElement{
                .render = [=](int w, int) -> Element {
                    std::string s;
                    std::vector<StyledRun> runs;
                    int used = 0;
                    if (!lead.empty()) {
                        runs.push_back({s.size(), lead.size(), lead_st});
                        s += lead;
                        used += string_width(lead);
                    }
                    const int tw = tail.empty() ? 0 : string_width(tail) + 2;
                    const int avail = std::max(0, w - used - tw);
                    std::string b = string_width(body) > avail ? truncate_end(body, avail) : body;
                    runs.push_back({s.size(), b.size(), body_st});
                    s += b;
                    used += string_width(b);
                    if (!tail.empty() && w - used >= tw) {
                        s.append(static_cast<std::size_t>(w - used - string_width(tail)), ' ');
                        runs.push_back({s.size(), tail.size(), tail_st});
                        s += tail;
                    }
                    return Element{TextElement{.content = std::move(s), .style = {},
                                               .wrap = TextWrap::NoWrap, .runs = std::move(runs)}};
                },
                .measure = [](int mw) -> Size { return {Columns(mw), Rows(1)}; },
            }};
        };

        const int inner = std::max(0, cfg.rows - 2);
        std::vector<Element> rows;
        rows.reserve(static_cast<std::size_t>(inner));

        // Identity first: title with facts, then where/what, then the ask.
        rows.push_back(row({}, {}, cfg.title, Style{}.with_fg(text_c).with_bold(),
                           cfg.facts, Style{}.with_fg(muted)));
        if (!cfg.subtitle.empty())
            rows.push_back(row({}, {}, cfg.subtitle, Style{}.with_fg(muted)));
        if (!cfg.asked.empty())
            rows.push_back(row(who_col("asked"), Style{}.with_fg(info).with_bold(),
                               cfg.asked, Style{}.with_fg(text_c)));
        rows.push_back(Element{ComponentElement{
            .render = [=](int w, int) -> Element {
                std::string s;
                for (int i = 0; i < w; ++i) s += "\xe2\x94\x80";   // ─
                return Element{TextElement{.content = std::move(s),
                                           .style = Style{}.with_fg(rule),
                                           .wrap = TextWrap::NoWrap}};
            },
            .measure = [](int mw) -> Size { return {Columns(mw), Rows(1)}; },
        }});

        // The transcript fills the rest; blank rows keep the height fixed.
        const int room = transcript_rows(cfg);
        int shown = 0;
        for (const auto& l : cfg.lines) {
            if (shown >= room) break;
            Style who_st = Style{}.with_fg(muted);
            Style txt_st = Style{}.with_fg(text_c);
            switch (l.kind) {
                case Kind::User: who_st = Style{}.with_fg(info).with_bold(); break;
                case Kind::Tool: txt_st = Style{}.with_fg(muted); break;
                case Kind::Note: txt_st = Style{}.with_fg(muted).with_italic(); break;
                case Kind::Gap:
                case Kind::Reply: break;
            }
            rows.push_back(row(who_col(l.who), who_st, l.text, txt_st));
            ++shown;
        }
        while (static_cast<int>(rows.size()) < inner)
            rows.push_back(Element{TextElement{.content = ""}});

        auto box = detail::vstack()
            .border(BorderStyle::Round)
            .border_color(frame)
            .border_text(" " + cfg.label + " ", BorderTextPos::Top, BorderTextAlign::Start)
            .padding(0, 1)
            .height(Dimension::fixed(cfg.rows))
            .min_height(Dimension::fixed(cfg.rows))
            .shrink(0.0f);
        if (!cfg.hint.empty())
            box.border_text_end(" " + cfg.hint + " ", BorderTextPos::Bottom, BorderTextAlign::End);
        return box(std::move(rows));
    }
};

}  // namespace maya
