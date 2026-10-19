#pragma once
// maya::widget::PeekCard — a raised preview card for a list row.
//
// A bold-bordered box on the theme's Surface colour, titled "◉ PREVIEW ·
// <title> · <facts>", with a hint on the bottom border. The body is a short
// transcript: each line has a speaker column and its text. The host passes
// the lines it wants shown, already windowed; the card pads to `rows`.
//
// Panel::Config::peek floats one of these over the rows above the cursor.

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
        std::string       title;
        std::string       facts;    // "48 msgs · 2h ago"
        std::string       hint;     // bottom border, right
        std::vector<Line> lines;
        int               rows = 8; // whole card height, borders included
    };

    Config cfg;

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        const Color surface = Color::slot(ThemeSlot::Surface);
        const Color accent  = Color::slot(ThemeSlot::Accent);
        const Color text    = Color::slot(ThemeSlot::Text);
        const Color muted   = Color::slot(ThemeSlot::Muted);
        const Color info    = Color::slot(ThemeSlot::Info);

        constexpr std::size_t kWho = 9;
        const int body_rows = cfg.rows > 2 ? cfg.rows - 2 : 0;
        std::vector<Element> body;
        body.reserve(static_cast<std::size_t>(body_rows));
        for (const auto& l : cfg.lines) {
            if (static_cast<int>(body.size()) >= body_rows) break;
            std::string who = l.who;
            if (who.size() < kWho) who.resize(kWho, ' ');
            Style who_st = Style{}.with_fg(muted).with_bg(surface);
            Style txt_st = Style{}.with_fg(text).with_bg(surface);
            switch (l.kind) {
                case Kind::User: who_st = Style{}.with_fg(info).with_bold().with_bg(surface);
                                 txt_st = txt_st.with_bold(); break;
                case Kind::Tool: txt_st = Style{}.with_fg(muted).with_dim().with_bg(surface); break;
                case Kind::Note: txt_st = Style{}.with_fg(muted).with_italic().with_bg(surface); break;
                case Kind::Gap:
                case Kind::Reply: break;
            }
            std::string content = who + l.text;
            std::vector<StyledRun> runs{{0, who.size(), who_st},
                                        {who.size(), l.text.size(), txt_st}};
            body.push_back(Element{TextElement{.content = std::move(content),
                                               .style   = txt_st,
                                               .wrap    = TextWrap::TruncateEnd,
                                               .runs    = std::move(runs)}});
        }
        while (static_cast<int>(body.size()) < body_rows)
            body.push_back(Element{TextElement{.content = "",
                                               .style = Style{}.with_bg(surface)}});

        std::string top = " \xe2\x97\x89 PREVIEW \xc2\xb7 " + cfg.title;   // ◉
        if (!cfg.facts.empty()) top += " \xc2\xb7 " + cfg.facts;
        top += " ";
        auto box = detail::vstack()
            .border(BorderStyle::Bold)
            .border_color(accent)
            .border_text(top, BorderTextPos::Top, BorderTextAlign::Start)
            .padding(0, 1)
            .bg(surface)
            .width(Dimension::percent(100));
        if (!cfg.hint.empty())
            box.border_text_end(" " + cfg.hint + " ", BorderTextPos::Bottom, BorderTextAlign::End);
        return box(std::move(body));
    }
};

}  // namespace maya
