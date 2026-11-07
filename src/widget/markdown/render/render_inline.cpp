// render_inline.cpp — inline AST → Element (the hot per-frame path).
//
// flatten_inline collapses inline spans into one TextElement with styled
// runs so word-wrap operates on the full flow; md_inline_to_element wraps a
// single span; build_inline_row / measure_inline_width / render_list build
// the paragraph and list scaffolding. The committed-block renderer
// (render_block.cpp) calls into these via maya::md_detail.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "maya/core/overload.hpp"
#include "maya/element/builder.hpp"
#include "maya/style/border.hpp"
#include "maya/style/style.hpp"
#include "maya/widget/html.hpp"
#include "maya/widget/markdown.hpp"
#include "maya/widget/markdown/internal.hpp"
#include "maya/widget/markdown/tex_math.hpp"

namespace maya {

// md_block_to_element lives in render_block.cpp; declared here so render_list
// (which recurses into nested block children) can call it.
Element md_block_to_element(const md::Block& block);

namespace {

// Build the math typesetter palette from the markdown colour scheme so
// typeset formulae sit in the same visual language as the surrounding prose.
// Variables read as body text (italic by TeX convention), operators pick up a
// touch of bold, numbers stay plain, fraction bars / radical strokes reuse the
// dim rule colour, delimiters the link accent.
[[nodiscard]] static texmath::MathPalette math_palette(const Style& inherited) {
    texmath::MathPalette pal;
    pal.normal = inherited;
    pal.op     = inherited.with_bold();
    pal.num    = inherited;
    pal.rule   = inherited.with_fg(colors::strike_fg());
    pal.delim  = inherited.with_fg(colors::link_fg());
    pal.text   = inherited;
    return pal;
}

// loads + an &&-chain that the compiler folds well). Most flatten_inline
// recursion chains feed `inherited` from a prior merge, so `base` is
// rarely empty in steady state — but the helper keeps the common case
// fast and the cold case correct, and it's a single inline that the
// optimiser collapses into the merge body when `base.empty()` would
// otherwise be a constant.
[[nodiscard]] static inline Style fold_style(const Style& base, Style over) noexcept {
    if (base.empty()) return over;
    return base.merge(over);
}

static void flatten_inline(const md::Inline& span, const Style& inherited,
                           std::string& out, std::vector<StyledRun>& runs) {
    std::visit(overload{
        [&](const md::Text& t) {
            runs.push_back({out.size(), t.content.size(), inherited});
            out += t.content;
        },
        [&](const md::Bold& b) {
            auto sty = fold_style(inherited, Style{}.with_bold().with_fg(colors::bold_fg()));
            for (auto& child : b.children) flatten_inline(child, sty, out, runs);
        },
        [&](const md::Italic& it) {
            auto sty = fold_style(inherited, Style{}.with_italic().with_fg(colors::italic_fg()));
            for (auto& child : it.children) flatten_inline(child, sty, out, runs);
        },
        [&](const md::BoldItalic& bi) {
            auto sty = fold_style(inherited, Style{}.with_bold().with_italic().with_fg(colors::bold_fg()));
            for (auto& child : bi.children) flatten_inline(child, sty, out, runs);
        },
        [&](const md::Code& c) {
            // Inline code: bright_cyan fg on a faint bg — a chip, not
            // just colored text. Pure-color was indistinguishable from
            // a regularly-styled word on themes where bright_cyan
            // collapses toward the body fg, so identifiers like
            // `Domain` / `StreamState` blended into prose. The bg fills
            // strictly the code RUN (no padding spaces) so wrapping
            // behaves identically to plain text — the historical
            // "chip with spaces" wrap bug doesn't reappear. On terminals
            // without true-color the ANSI black slot still produces a
            // visible step against default bg.
            auto sty = Style{}.with_fg(colors::code_fg())
                              .with_bg(colors::code_bg());
            runs.push_back({out.size(), c.content.size(), sty});
            out += c.content;
        },
        [&](const md::Link& l) {
            auto sty = Style{}.with_fg(colors::link_fg()).with_underline();
            runs.push_back({out.size(), l.text.size(), sty});
            out += l.text;
        },
        [&](const md::Image& img) {
            // Pick the visible label:
            //   1. The author-provided alt text, when present.
            //   2. The URL's last path segment ("icon.png" out of
            //      `https://x/a/b/icon.png`), so `![](url)` shows
            //      something more useful than a lonely 🖼 glyph.
            //   3. The URL title, when even the URL is empty (e.g.
            //      reference image whose ref-def carries a title).
            //   4. "image" — absolute last resort, so layout never
            //      collapses to zero-width.
            std::string label = img.alt;
            if (label.empty()) {
                std::string_view url = img.url;
                // Strip query / fragment first so they don't leak into
                // the label when the URL has no trailing filename
                // (`https://x/?foo=1`).
                auto q = url.find_first_of("?#");
                if (q != std::string_view::npos) url = url.substr(0, q);
                auto slash = url.find_last_of('/');
                std::string_view tail =
                    (slash == std::string_view::npos) ? url
                                                      : url.substr(slash + 1);
                if (!tail.empty()) label.assign(tail);
            }
            if (label.empty()) label = img.title;
            if (label.empty()) label = "image";

            std::string display = "\xf0\x9f\x96\xbc " + label;
            auto sty = Style{}.with_fg(colors::image_fg()).with_italic();
            runs.push_back({out.size(), display.size(), sty});
            out += display;
        },
        [&](const md::Strike& s) {
            auto sty = fold_style(inherited, Style{}.with_strikethrough().with_fg(colors::strike_fg()));
            for (auto& child : s.children) flatten_inline(child, sty, out, runs);
        },
        [&](const md::Highlight& h) {
            auto sty = fold_style(inherited,
                Style{}.with_bg(colors::highlight_bg()).with_fg(colors::highlight_fg()));
            for (auto& child : h.children) flatten_inline(child, sty, out, runs);
        },
        [&](const md::Sub& sb) {
            size_t start = out.size();
            auto sty = fold_style(inherited, Style{}.with_fg(colors::strike_fg()));
            out += "_{";
            for (auto& child : sb.children) flatten_inline(child, sty, out, runs);
            out += "}";
            runs.push_back({start, 2, sty});
            runs.push_back({out.size() - 1, 1, sty});
        },
        [&](const md::Sup& sp) {
            size_t start = out.size();
            auto sty = fold_style(inherited, Style{}.with_fg(colors::strike_fg()));
            out += "^{";
            for (auto& child : sp.children) flatten_inline(child, sty, out, runs);
            out += "}";
            runs.push_back({start, 2, sty});
            runs.push_back({out.size() - 1, 1, sty});
        },
        [&](const md::Kbd& k) {
            // Render <kbd>X</kbd> as a key-cap chip:
            //
            //   ╶ X ╴
            //
            // The bracket glyphs (U+2576 ╶ "box drawings light right" /
            // U+2574 ╴ "box drawings light left") read as a chip
            // outline at terminal resolution without the legibility
            // hit of `[`/`]` (which collide with reference-link
            // bracketed text in scanned prose). The inner text is
            // bold + bright; the chrome rides in `kbd_border` and is
            // dimmed so the chip outline is visible without
            // out-competing the key glyphs themselves.
            auto bracket_sty =
                Style{}.with_fg(colors::kbd_border()).with_dim();
            auto inner_sty = fold_style(inherited,
                Style{}.with_bold().with_fg(colors::kbd_fg()));
            static constexpr std::string_view kOpen  = "\xe2\x95\xb6 "; // ╶ + space
            static constexpr std::string_view kClose = " \xe2\x95\xb4"; // space + ╴
            runs.push_back({out.size(), kOpen.size(), bracket_sty});
            out.append(kOpen);
            for (auto& child : k.children) flatten_inline(child, inner_sty, out, runs);
            runs.push_back({out.size(), kClose.size(), bracket_sty});
            out.append(kClose);
        },
        [&](const md::Abbr& a) {
            auto sty = fold_style(inherited, Style{}.with_underline());
            for (auto& child : a.children) flatten_inline(child, sty, out, runs);
            if (!a.title.empty()) {
                std::string suffix = " (" + a.title + ")";
                auto suf_sty = Style{}.with_fg(colors::footnote_fg()).with_italic();
                runs.push_back({out.size(), suffix.size(), suf_sty});
                out += suffix;
            }
        },
        [&](const md::Mention& m) {
            auto sty = Style{}.with_fg(colors::mention_fg());
            runs.push_back({out.size(), m.display.size(), sty});
            out += m.display;
        },
        [&](const md::FootnoteRef& f) {
            auto content = "[" + f.label + "]";
            auto sty = Style{}.with_fg(colors::footnote_fg()).with_italic();
            runs.push_back({out.size(), content.size(), sty});
            out += content;
        },
        [&](const md::HardBreak&) {
            runs.push_back({out.size(), 1, inherited});
            out += '\n';
        },
        [&](const md::SoftBreak&) {
            // A soft break renders as a single space in flowed terminal
            // text (the layout engine re-wraps); the spec maps it to a
            // newline in HTML but for the TUI a space is the right join.
            runs.push_back({out.size(), 1, inherited});
            out += ' ';
        },
        [&](const md::RawInline& r) {
            // Verbatim inline HTML / passthrough — show the literal text
            // dimmed so it's visibly "raw" without dominating prose.
            auto sty = fold_style(inherited, Style{}.with_fg(colors::footnote_fg()));
            runs.push_back({out.size(), r.content.size(), sty});
            out += r.content;
        },
        [&](const md::MathInline& m) {
            // Inline `$…$` / `\(…\)`: typeset the TeX to a single baseline row
            // (Unicode sub/superscripts, symbols, greek; fractions collapse to
            // a/b) so it flows inside the paragraph. Display `$$…$$` keeps the
            // real 2-D box via the block path in render_block.cpp.
            std::string glyphs = texmath::linearize(m.latex, math_palette(inherited));
            if (glyphs.empty()) glyphs = m.latex;  // degrade to raw TeX
            runs.push_back({out.size(), glyphs.size(), inherited});
            out += glyphs;
        },
    }, span.inner);
}

// Map an HTML phrasing role onto the markdown palette so inline raw HTML
// (`<b>`, `<code>`, `<kbd>`, …) looks identical to the equivalent markdown
// construct. Mirrors the per-variant styling in flatten_inline above.
[[nodiscard]] static Style html_role_style(html::Role r, Style cur) {
    using R = html::Role;
    switch (r) {
        case R::Bold:      return cur.with_bold().with_fg(colors::bold_fg());
        case R::Italic:    return cur.with_italic().with_fg(colors::italic_fg());
        case R::Underline: return cur.with_underline();
        case R::Strike:    return cur.with_strikethrough().with_fg(colors::strike_fg());
        case R::Code:      return cur.with_fg(colors::code_fg()).with_bg(colors::code_bg());
        case R::KeyCap:    return cur.with_bold().with_fg(colors::kbd_fg());
        case R::Mark:      return cur.with_bg(colors::highlight_bg())
                                     .with_fg(colors::highlight_fg());
        case R::Small:     return cur.with_dim();
        case R::Sub:
        case R::Sup:       return cur.with_fg(colors::strike_fg());
        case R::Link:      return cur.with_fg(colors::link_fg()).with_underline();
        case R::None:
        case R::Break:     break;
    }
    return cur;
}

// Flatten a run of inline spans, interpreting interleaved raw-HTML tags
// (md::RawInline) as a style stack: `<b>`…`</b>` toggles bold across the
// sibling spans between them, `<br>` is a hard break, unknown tags fall back
// to the dimmed-literal passthrough. This is the markdown widget delegating
// HTML *semantics* to maya::html while keeping ownership of the span walk.
static void flatten_inlines(const std::vector<md::Inline>& spans,
                            const Style& base, std::string& out,
                            std::vector<StyledRun>& runs) {
    struct Open { std::string name; Style prev; };
    std::vector<Open> stack;
    Style cur = base;

    // BALANCE PRE-PASS. A styling tag emits no literal text — correct for
    // `<b>bold</b>`, catastrophic for prose that merely MENTIONS a tag:
    // "Set <var> to 3" would render "Set  to 3", silently eating the word
    // the sentence was about. Models talk about tags constantly (agentty's
    // own prompts are full of <environment>, <cwd>, <output>, <summary>),
    // and the ones that collide with real HTML phrasing/container names
    // (var, cite, code, kbd, div, p, span, font, …) were the only ones
    // that vanished — a data-loss bug keyed on a name collision.
    //
    // So: an opening tag only opens a STYLE SCOPE when a matching close
    // exists later in the same block. Unbalanced ones fall through to the
    // dimmed-literal passthrough and stay visible. Well-formed HTML is
    // unaffected; a lone mention is preserved verbatim.
    //
    // Counting closes up front and decrementing as we pass them keeps this
    // one linear walk. The map is only built when the block actually
    // CONTAINS raw inline HTML — the overwhelmingly common case is prose
    // with none, and paying a heap allocation per paragraph to look up an
    // always-empty table shows up in the render loop for nothing.
    std::unordered_map<std::string, int> closes_ahead;
    const bool has_raw = std::ranges::any_of(spans, [](const md::Inline& s) {
        return std::holds_alternative<md::RawInline>(s.inner);
    });
    if (has_raw)
        for (const auto& span : spans)
            if (const auto* r = std::get_if<md::RawInline>(&span.inner))
                if (auto t = html::parse_tag(r->content); t && t->is_close)
                    ++closes_ahead[t->name];

    for (const auto& span : spans) {
        const auto* raw = std::get_if<md::RawInline>(&span.inner);
        if (!raw) { flatten_inline(span, cur, out, runs); continue; }

        auto tag = html::parse_tag(raw->content);
        if (tag) {
            if (tag->is_close) {
                if (auto it = closes_ahead.find(tag->name);
                    it != closes_ahead.end() && it->second > 0)
                    --it->second;
            }
            html::Role role = html::inline_role(tag->name);
            if (role == html::Role::Break) {
                runs.push_back({out.size(), 1, cur});
                out += '\n';
                continue;
            }
            if (tag->name == "wbr") continue;  // zero-width break opportunity

            // A tag is "styling" if it maps to a phrasing role OR carries
            // inline CSS / presentational color attributes (span, font, or
            // any element with style="..."). span/font have Role::None but
            // still open a style scope so `<span style="color:red">…</span>`
            // colours its children instead of dumping literal tag text.
            bool has_css = !tag->style.empty() || !tag->color.empty() ||
                           !tag->bgcolor.empty();
            bool container = tag->name == "span" || tag->name == "font" ||
                             tag->name == "div" || tag->name == "p";
            // An OPEN tag must be closed later to earn its silence; a CLOSE
            // tag must actually pop a scope this walk opened. A stray
            // `</div>` in prose belongs to no scope, so it stays visible
            // rather than deleting itself. Self-closing tags style nothing
            // and so never need a partner.
            const bool pops =
                tag->is_close &&
                std::any_of(stack.begin(), stack.end(),
                            [&](const Open& o) { return o.name == tag->name; });
            const bool balanced = pops || tag->self_closing ||
                                  (!tag->is_close && closes_ahead[tag->name] > 0);
            if ((role != html::Role::None || has_css || container) && balanced) {
                if (tag->is_close) {
                    for (int k = static_cast<int>(stack.size()) - 1; k >= 0; --k)
                        if (stack[static_cast<std::size_t>(k)].name == tag->name) {
                            cur = stack[static_cast<std::size_t>(k)].prev;
                            stack.erase(stack.begin() + k, stack.end());
                            break;
                        }
                } else if (!tag->self_closing) {
                    stack.push_back({tag->name, cur});
                    Style styled = html_role_style(role, cur);
                    styled = html::tag_style(*tag, styled);  // CSS overrides role
                    cur = styled;
                }
                continue;  // styling tags emit no literal text
            }
        }
        // not a recognized tag → dimmed-literal passthrough (flatten_inline's
        // RawInline branch), using the current effective style.
        flatten_inline(span, cur, out, runs);
    }
}

} // anonymous

Element md_inline_to_element(const md::Inline& span) {
    std::string content;
    std::vector<StyledRun> runs;
    flatten_inline(span, Style{}.with_fg(colors::text()), content, runs);
    if (runs.size() <= 1 && !runs.empty()) {
        return Element{TextElement{.content = std::move(content), .style = runs[0].style}};
    }
    return Element{TextElement{
        .content = std::move(content),
        .style = Style{}.with_fg(colors::text()),
        .runs = std::move(runs),
    }};
}

// Measure the display width of a vector of inline spans.
static int measure_inline_width(const std::vector<md::Inline>& spans) {
    std::string content;
    std::vector<StyledRun> runs;
    Style base = Style{}.with_fg(colors::text());
    flatten_inlines(spans, base, content, runs);
    return string_width(content);
}

namespace {
static Element build_inline_row(const std::vector<md::Inline>& spans) {
    if (spans.empty()) return Element{TextElement{}};

    std::string content;
    std::vector<StyledRun> runs;
    Style base = Style{}.with_fg(colors::text());

    flatten_inlines(spans, base, content, runs);

    // Optimize: single run → use simple TextElement
    if (runs.size() == 1) {
        return Element{TextElement{
            .content = std::move(content),
            .style = runs[0].style,
        }};
    }

    return Element{TextElement{
        .content = std::move(content),
        .style = base,
        .runs = std::move(runs),
    }};
}

// Render an md::List at a given nesting depth (0 = top-level).
static Element render_list(const md::List& l, int depth) {
    std::vector<Element> items;
    items.reserve(l.items.size());
    int num = l.start_num;
    // Two columns per level, for every kind of list (ordered and task items
    // used to ignore depth, so a nested `1.` sat flush with its parent).
    const std::string lead(static_cast<std::size_t>(2 + 2 * depth), ' ');
    // Right-align numbers so `9.` and `10.` share a column.
    const int last_num = l.start_num + static_cast<int>(l.items.size()) - 1;
    const std::size_t num_w = std::to_string(std::max(last_num, l.start_num)).size();

    for (auto& item : l.items) {
        std::string prefix;
        Style prefix_style;

        if (item.checked.has_value()) {
            // Zed style: ✓/○ for task lists
            if (*item.checked) {
                prefix = lead + "\xe2\x9c\x93 ";  // "✓ "
                prefix_style = Style{}.with_fg(colors::checkbox_fg());
            } else {
                prefix = lead + "\xe2\x97\x8b ";  // "○ "
                prefix_style = Style{}.with_fg(colors::checkbox_off());
            }
        } else if (l.ordered) {
            std::string n = std::to_string(num++);
            prefix = lead + std::string(num_w - n.size(), ' ') + n + ". ";
            prefix_style = depth == 0
                ? Style{}.with_fg(colors::list_num()).with_bold()
                : Style{}.with_fg(colors::list_num());
        } else if (depth == 0) {
            // ▸ (U+25B8, black right-pointing small triangle) reads as
            // a directional "item start" cue — heavier than • (which
            // disappears against dense prose on most themes) and
            // unambiguously punctuation, not stray text. Bold + bullet
            // color so it's the visual anchor for each list item.
            prefix = "  \xe2\x96\xb8 ";  // "  ▸ "
            prefix_style = Style{}.with_fg(colors::list_bullet()).with_bold();
        } else {
            // Nested items step down in visual weight: hollow ◦
            // signals "sub-item" without competing with the parent
            // ▸. Extra two-space indent reinforces the hierarchy.
            prefix = lead + "\xe2\x97\xa6 ";  // "◦ "
            prefix_style = Style{}.with_fg(colors::list_bullet());
        }

        // Hanging-indent layout: render the bullet/number marker as its own
        // fixed-width column on the left, and the body content in a flexing
        // column on the right. When the body wraps, continuation lines stay
        // aligned under the first character of the body — they do not bleed
        // back to column 0. Same hstack(prefix, body) pattern blockquote uses.
        auto prefix_elem = Element{TextElement{
            .content = prefix,
            .style = prefix_style,
        }};

        std::string body;
        std::vector<StyledRun> body_runs;
        Style base = Style{}.with_fg(colors::text());
        flatten_inlines(item.spans, base, body, body_runs);
        Element body_elem = (body_runs.size() == 1)
            ? Element{TextElement{
                .content = std::move(body),
                .style   = body_runs[0].style,
              }}
            : Element{TextElement{
                .content = std::move(body),
                .style   = base,
                .runs    = std::move(body_runs),
              }};

        auto item_row = detail::hstack()(std::move(prefix_elem),
                                         std::move(body_elem));

        if (item.children.empty()) {
            items.push_back(std::move(item_row));
        } else {
            // Item with sub-content (nested lists, multi-para)
            std::vector<Element> sub;
            sub.reserve(item.children.size() + 1);
            sub.push_back(std::move(item_row));
            for (auto& child : item.children) {
                // If this child is a list, render it at increased depth
                if (auto* nested = std::get_if<md::List>(&child.inner)) {
                    sub.push_back(render_list(*nested, depth + 1));
                } else {
                    sub.push_back(md_block_to_element(child));
                }
            }
            items.push_back(detail::vstack()(std::move(sub)));
        }
    }
    return detail::vstack()(std::move(items));
}

} // anonymous

// ── Cross-TU bridge ──────────────────────────────────────────────────
namespace md_detail {
void flatten_inline(const md::Inline& span,
                    const Style& inherited,
                    std::string& out,
                    std::vector<StyledRun>& runs) {
    ::maya::flatten_inline(span, inherited, out, runs);
}
void flatten_inlines(const std::vector<md::Inline>& spans,
                     const Style& base,
                     std::string& out,
                     std::vector<StyledRun>& runs) {
    ::maya::flatten_inlines(spans, base, out, runs);
}
Element build_inline_row(const std::vector<md::Inline>& spans) {
    return ::maya::build_inline_row(spans);
}
int measure_inline_width(const std::vector<md::Inline>& spans) {
    return ::maya::measure_inline_width(spans);
}
Element render_list(const md::List& l, int depth) {
    return ::maya::render_list(l, depth);
}
} // namespace md_detail

} // namespace maya
