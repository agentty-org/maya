#pragma once
// maya::widget::InlayHintLine — code line with inlay-hint chips (background-free)
//
// A single source line built from interleaved segments: real code and dim inlay
// hints (inferred types, parameter names) rendered as subtle chips between the
// tokens — the LSP inlay-hint decoration.
//
// Usage:
//   InlayHintLine l;
//   l.code("auto ").code("x").hint(": int").code(" = add(").hint("a:").code("1, ")
//    .hint("b:").code("2);");
//   Element ui = l;

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"
#include "agentty/markdown/highlight.hpp"

namespace maya {

struct InlayHintTheme {
    syntax::HighlightTheme syntax = syntax::themes::github_dark;
    Color code = Color::slot(ThemeSlot::Text);
    Color hint = Color::slot(ThemeSlot::Muted); // inlay chip text
    Color edge = Color::slot(ThemeSlot::Border);
};

struct InlayHintLine {
    struct Seg { std::string text; bool hint; };
    std::vector<Seg> segs;
    syntax::Lang     lang = syntax::Lang::Generic;
    InlayHintTheme   theme;

    InlayHintLine& code(std::string t) { segs.push_back({std::move(t), false}); return *this; }
    InlayHintLine& hint(std::string t) { segs.push_back({std::move(t), true}); return *this; }
    InlayHintLine& set_lang(syntax::Lang l) { lang = l; return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::string s; std::vector<StyledRun> r;
        auto put=[&](std::string_view t, Style st){ if(t.empty())return;
            r.push_back({s.size(),t.size(),st}); s+=t; };
        for (const auto& seg : segs) {
            if (seg.hint) {
                // a subtle chip: a hint rendered dim italic with thin edges
                put("\xe2\x9d\xb4", Style{}.with_fg(theme.edge));      // ❴
                put(seg.text, Style{}.with_fg(theme.hint).with_italic());
                put("\xe2\x9d\xb5", Style{}.with_fg(theme.edge));      // ❵
            } else {
                put(seg.text, Style{}.with_fg(theme.code));
            }
        }
        return Element{TextElement{ .content=std::move(s), .style=Style{},
                                    .wrap=TextWrap::NoWrap, .runs=std::move(r) }};
    }
};

} // namespace maya
