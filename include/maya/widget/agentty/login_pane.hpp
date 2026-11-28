#pragma once
// maya::LoginPane — agentty's sign-in modal, described as data.
//
// Two shapes:
//   * a prose pane: a list of blocks (title, prose, options, a URL box, key
//     hints) inside the rounded " Sign in to agentty " frame;
//   * a Panel pane: a maya::Panel (input field, account list) with prose
//     blocks above and below it. The Panel draws its own frame.
//
// The host picks the words and which blocks appear; this file decides how
// every block looks.

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../../dsl.hpp"
#include "../../element/builder.hpp"
#include "../../element/text.hpp"
#include "../../style/border.hpp"
#include "../../style/color.hpp"
#include "../../style/style.hpp"
#include "../../style/theme.hpp"
#include "panel.hpp"

namespace maya {

class LoginPane {
public:
    struct Block {
        enum class Kind : uint8_t {
            Gap,        // an empty row
            Title,      // bold, one line
            Prose,      // wrapping, muted
            Danger,     // wrapping, error colour
            Url,        // the URL in its own rounded box -- the thing to copy
            Option,     // "1) Title" + hanging muted subtitle, or one row with a note
            Code,       // muted lead-in, then a highlighted code
            Hints,      // "key value · key value", wrapping
        };
        Kind        kind = Kind::Gap;
        std::string text;                 // Title / Prose / Danger / Url / Option title / Code lead
        std::string num;                  // Option: "1) "
        std::string sub;                  // Option subtitle (own row) / Code: the code
        std::string note;                 // Option: inline note on the same row
        std::vector<std::pair<std::string, std::string>> hints;   // Hints
    };

    struct Config {
        // Prose pane.
        std::string        frame_title = " Sign in to agentty ";
        std::vector<Block> blocks;
        // Panel pane (set = this shape).
        std::optional<Panel::Config> panel;
        std::vector<Block>           panel_header, panel_footer;

        Color accent    = Color::slot(ThemeSlot::Accent);
        Color text      = Color::slot(ThemeSlot::Text);
        Color muted     = Color::slot(ThemeSlot::Muted);
        Color danger    = Color::slot(ThemeSlot::Error);
        Color highlight = Color::slot(ThemeSlot::Highlight);
        Color code_path = Color::slot(ThemeSlot::Link);
    };

    explicit LoginPane(Config c) : cfg_(std::move(c)) {}
    operator Element() const { return build(); }

    [[nodiscard]] static Element empty() { return dsl::nothing(); }

    // Block helpers, so a host reads as a description of the modal.
    static Block gap()                               { return {}; }
    static Block title(std::string s)                { Block b; b.kind = Block::Kind::Title;  b.text = std::move(s); return b; }
    static Block prose(std::string s)                { Block b; b.kind = Block::Kind::Prose;  b.text = std::move(s); return b; }
    static Block danger(std::string s)               { Block b; b.kind = Block::Kind::Danger; b.text = std::move(s); return b; }
    static Block url(std::string s)                  { Block b; b.kind = Block::Kind::Url;    b.text = std::move(s); return b; }
    static Block code(std::string lead, std::string code) {
        Block b; b.kind = Block::Kind::Code; b.text = std::move(lead); b.sub = std::move(code); return b;
    }
    static Block option(std::string num, std::string title, std::string sub, std::string note = {}) {
        Block b; b.kind = Block::Kind::Option; b.num = std::move(num); b.text = std::move(title);
        b.sub = std::move(sub); b.note = std::move(note); return b;
    }
    static Block hints(std::vector<std::pair<std::string, std::string>> h) {
        Block b; b.kind = Block::Kind::Hints; b.hints = std::move(h); return b;
    }

    [[nodiscard]] Element build() const {
        using namespace dsl;
        if (cfg_.panel) {
            Panel::Config pc = *cfg_.panel;
            for (const auto& b : cfg_.panel_header) append(pc.header, b);
            for (const auto& b : cfg_.panel_footer) append(pc.footer, b);
            return Panel{std::move(pc)}.build();
        }
        std::vector<Element> rows;
        for (const auto& b : cfg_.blocks) append(rows, b);
        // min_width floors the modal on tiny terminals; the Overlay's default
        // Stretch lets it fill wider ones. Every text node wraps, so URLs and
        // prose reflow to whatever width the terminal gives.
        return vstack()
            .padding(1, 2)
            .min_width(Dimension::fixed(48))
            .border(BorderStyle::Round)
            .border_color(cfg_.accent)
            .border_text(cfg_.frame_title, BorderTextPos::Top, BorderTextAlign::Center)
            (v(std::move(rows)).build());
    }

private:
    Style fg(Color c) const   { return Style{}.with_fg(c); }
    Style bold(Color c) const { return Style{}.with_fg(c).with_bold(); }
    Style dim(Color c) const {
        return theme::is_muted(c) ? Style{}.with_fg(c) : Style{}.with_fg(c).with_dim();
    }

    // Wrap-mode body text: long URLs / explanations reflow, never overflow.
    static Element wrapped(std::string s, Style st) {
        return Element{TextElement{.content = std::move(s), .style = st, .wrap = TextWrap::Wrap}};
    }

    void append(std::vector<Element>& rows, const Block& b) const {
        using namespace dsl;
        using K = Block::Kind;
        switch (b.kind) {
            case K::Gap:    rows.push_back(text("")); break;
            case K::Title:  rows.push_back(text(b.text, bold(cfg_.text))); break;
            case K::Prose:  rows.push_back(wrapped(b.text, dim(cfg_.muted))); break;
            case K::Danger: rows.push_back(wrapped(b.text, fg(cfg_.danger))); break;
            case K::Url:
                // Bordered and wrapping: every character readable without
                // horizontal scrolling, set apart as the thing to copy.
                rows.push_back((v(Element{TextElement{.content = b.text,
                                                      .style = Style{}.with_fg(cfg_.code_path),
                                                      .wrap = TextWrap::Wrap}})
                                | padding(0, 1)
                                | border(BorderStyle::Round)
                                | bcolor(cfg_.code_path)).build());
                break;
            case K::Option:
                // Numbered items are flush-left; the subtitle hangs under the
                // title by exactly the badge width.
                if (!b.note.empty()) {
                    rows.push_back(h(text(b.num, bold(cfg_.highlight)),
                                     text(b.text, bold(cfg_.text)),
                                     text(b.note, dim(cfg_.muted))).build());
                } else {
                    rows.push_back(h(text(b.num, bold(cfg_.highlight)),
                                     text(b.text, bold(cfg_.text))).build());
                    rows.push_back(h(text("   ", fg(cfg_.text)),
                                     wrapped(b.sub, dim(cfg_.muted))).build());
                }
                break;
            case K::Code:
                rows.push_back(h(text(b.text, dim(cfg_.muted)),
                                 text(b.sub, bold(cfg_.highlight))).build());
                break;
            case K::Hints: {
                // One wrapping text with styled runs, so a long footer reflows
                // across rows on a narrow terminal instead of overflowing.
                std::string content;
                std::vector<StyledRun> runs;
                auto push = [&](std::string_view s, Style st) {
                    if (s.empty()) return;
                    runs.push_back({content.size(), s.size(), st});
                    content.append(s);
                };
                const Style key = Style{}.with_fg(cfg_.text).with_bold();
                const Style sep = dim(cfg_.muted);
                bool first = true;
                for (const auto& [k, val] : b.hints) {
                    if (!first) push("   \xc2\xb7   ", sep);   //   ·
                    first = false;
                    push(k, key);
                    push(" ", sep);
                    push(val, sep);
                }
                rows.push_back(Element{TextElement{.content = std::move(content),
                                                   .style = Style{}.with_fg(cfg_.text),
                                                   .wrap = TextWrap::Wrap,
                                                   .runs = std::move(runs)}});
                break;
            }
        }
    }

    Config cfg_;
};

} // namespace maya
