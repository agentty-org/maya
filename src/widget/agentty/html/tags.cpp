// tags.cpp — HTML element classification + the public tag helpers shared
// with the markdown widget (parse_tag / inline_role).

#include "maya/widget/agentty/html.hpp"
#include "maya/widget/agentty/html/internal.hpp"

#include <initializer_list>
#include <string_view>

namespace maya::html {
namespace detail {

namespace {
// Sorted lookup over a small string_view set.
[[nodiscard]] bool in_set(std::string_view name,
                          std::initializer_list<std::string_view> set) {
    for (auto s : set)
        if (s == name) return true;
    return false;
}
} // namespace

bool is_void_element(std::string_view name) {
    return in_set(name, {"area", "base", "br", "col", "embed", "hr", "img",
                         "input", "link", "meta", "param", "source", "track",
                         "wbr"});
}

bool is_raw_text_element(std::string_view name) {
    return in_set(name, {"script", "style", "textarea", "title"});
}

bool is_block_element(std::string_view name) {
    return in_set(name, {"address", "article", "aside", "blockquote", "details",
                         "dialog", "dd", "div", "dl", "dt", "fieldset",
                         "figcaption", "figure", "footer", "form", "h1", "h2",
                         "h3", "h4", "h5", "h6", "header", "hgroup", "hr", "li",
                         "main", "nav", "ol", "p", "pre", "section", "summary",
                         "table", "tbody", "td", "tfoot", "th", "thead", "tr",
                         "ul"});
}

} // namespace detail

// ── public: phrasing-role classification ─────────────────────────────────
Role inline_role(std::string_view name) {
    if (name == "b" || name == "strong") return Role::Bold;
    if (name == "i" || name == "em" || name == "cite" || name == "var" ||
        name == "dfn" || name == "address")
        return Role::Italic;
    if (name == "u" || name == "ins") return Role::Underline;
    if (name == "s" || name == "del" || name == "strike") return Role::Strike;
    if (name == "code" || name == "samp" || name == "tt") return Role::Code;
    if (name == "kbd") return Role::KeyCap;
    if (name == "mark") return Role::Mark;
    if (name == "small") return Role::Small;
    if (name == "sub") return Role::Sub;
    if (name == "sup") return Role::Sup;
    if (name == "a") return Role::Link;
    if (name == "br") return Role::Break;
    return Role::None;
}

// ── public: single-fragment tag parse ────────────────────────────────────
std::optional<Tag> parse_tag(std::string_view fragment) {
    auto toks = detail::tokenize(fragment);
    // Expect exactly one start/end tag token (text-only or multiple → not a tag)
    const detail::Token* tag = nullptr;
    for (const auto& t : toks) {
        if (t.kind == detail::Token::Kind::Text) {
            // ignore pure-whitespace text around the tag; bail on real text
            for (char c : t.text)
                if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
                    return std::nullopt;
            continue;
        }
        if (tag) return std::nullopt;  // more than one tag
        tag = &t;
    }
    if (!tag) return std::nullopt;

    Tag out;
    out.name = tag->name;
    out.is_close = (tag->kind == detail::Token::Kind::EndTag);
    out.self_closing = tag->self_closing;
    out.href = std::string(detail::attr_of(tag->attrs, "href"));
    out.title = std::string(detail::attr_of(tag->attrs, "title"));
    out.style = std::string(detail::attr_of(tag->attrs, "style"));
    out.color = std::string(detail::attr_of(tag->attrs, "color"));
    out.bgcolor = std::string(detail::attr_of(tag->attrs, "bgcolor"));
    return out;
}

// ── public: tag styling overlay ──────────────────────────────────────────
Style tag_style(const Tag& tag, Style base) {
    Style out = base;
    // Presentational attributes first (color=, bgcolor=), then inline CSS.
    if (!tag.color.empty())
        if (auto c = detail::parse_css_color(tag.color)) out = out.with_fg(*c);
    if (!tag.bgcolor.empty())
        if (auto c = detail::parse_css_color(tag.bgcolor)) out = out.with_bg(*c);
    if (!tag.style.empty())
        out = detail::parse_inline_style(tag.style, out);
    return out;
}

// ── public: is this a recognized HTML element name? ──────────────────────
// True when `name` (case-insensitive) is a tag the renderer actually knows —
// a phrasing element (inline_role != None), a block element, a void element,
// or a raw-text element. Model "pseudo-tags" (<shell>, <thinking>,
// <tool_call>, …) are NOT HTML and return false, so the markdown engine can
// let them fall through to normal paragraph/list parsing instead of eating
// the following lines as a raw HTML block.
bool is_known_tag(std::string_view name) {
    return inline_role(name) != Role::None
        || detail::is_block_element(name)
        || detail::is_void_element(name)
        || detail::is_raw_text_element(name);
}

} // namespace maya::html
