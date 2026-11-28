#pragma once
// maya::html — interpret a safe subset of HTML into styled terminal Elements.
//
// A standalone, themable widget: feed it an HTML fragment and it tokenizes,
// builds a tolerant DOM, and renders a maya Element tree (block elements
// stack vertically; phrasing content collapses into word-wrapped, styled
// text). It powers the markdown widget's raw-HTML handling but is usable on
// its own:
//
//   using namespace maya;
//   print(html::render("<p>Hello <b>world</b> &amp; <code>code</code></p>"));
//   auto ui = dsl::v( html::Html("<ul><li>one</li><li>two</li></ul>") );
//
// Supported blocks:    p div section article header footer main aside figure
//                      figcaption h1–h6 ul ol li blockquote pre hr table thead
//                      tbody tr th td dl dt dd details summary
// Supported phrasing:  b strong i em u ins s del strike mark code kbd samp var
//                      tt small sub sup a span abbr cite q br wbr
// Everything else degrades gracefully: unknown tags render their children,
// HTML entities are decoded, and malformed markup never throws.
//
// HTML semantics live here (one source of truth); colour is the renderer's
// choice — render() uses a Theme, while the markdown widget maps the same
// Role classification onto its own palette via parse_tag()/inline_role().

#include <optional>
#include <string>
#include <string_view>

#include "../../element/builder.hpp"
#include "../../style/style.hpp"
#include "../../style/theme.hpp"

namespace maya::html {

// ── Block rendering ──────────────────────────────────────────────────────

/// Parse `source` as HTML and render it to a (block-level) Element, coloured
/// from `theme`. Never throws; malformed input degrades to best-effort text.
[[nodiscard]] Element render(std::string_view source,
                             const Theme& theme = theme::native);

/// True when `source` would render to nothing a reader can see — no glyphs,
/// no styled runs, no visible structural children (a lone unknown/empty tag
/// like `<shell>`, `<div></div>`, or an HTML comment). Lets the markdown
/// layer drop a content-less kind-6/7 HTML block so it neither takes a row
/// nor earns the surrounding block gap. Same verdict as inspecting
/// `render(source, theme)`, without materialising the result for the caller.
[[nodiscard]] bool renders_empty(std::string_view source,
                                 const Theme& theme = theme::native);

/// Widget wrapper (maya house style: pipeable Node via implicit Element).
class Html {
public:
    explicit Html(std::string source, Theme theme = theme::native)
        : source_(std::move(source)), theme_(theme) {}

    operator Element() const { return build(); }
    [[nodiscard]] Element build() const { return render(source_, theme_); }

private:
    std::string source_;
    Theme       theme_;
};

// ── Inline tag semantics (shared with the markdown widget) ───────────────
//
// The markdown inline scanner emits each raw HTML tag as its own fragment,
// interleaved with already-parsed markdown spans. To interpret `<b>…</b>`
// the caller walks those spans with a style stack; these helpers tell it
// what each tag means without dictating colour.

/// The display role of a phrasing element — what it does, not how it looks.
enum class Role : std::uint8_t {
    None,       // unknown / not a phrasing element we interpret
    Bold,       // b, strong
    Italic,     // i, em, cite, var, dfn
    Underline,  // u, ins
    Strike,     // s, del, strike
    Code,       // code, samp, tt
    KeyCap,     // kbd
    Mark,       // mark (highlight)
    Small,      // small
    Sub,        // sub
    Sup,        // sup
    Link,       // a
    Break,      // br (hard line break); wbr is a no-op break opportunity
};

/// A single parsed HTML tag fragment.
struct Tag {
    std::string name;            // lowercased element name
    bool        is_close = false;
    bool        self_closing = false;
    std::string href;            // <a href>
    std::string title;           // title= (e.g. <abbr title>)
    std::string style;           // style="..." (inline CSS, entity-decoded)
    std::string color;           // color= presentational attribute
    std::string bgcolor;         // bgcolor= presentational attribute
};

/// Parse one tag fragment (e.g. a markdown RawInline). nullopt if `fragment`
/// is not a single well-formed start/end tag.
[[nodiscard]] std::optional<Tag> parse_tag(std::string_view fragment);

/// Classify a (lowercased) element name's phrasing role. Role::None for
/// unknown or block-level names.
[[nodiscard]] Role inline_role(std::string_view name);

/// True when `name` is a tag the HTML renderer recognizes (a phrasing,
/// block, void, or raw-text element). Model pseudo-tags like `<shell>` or
/// `<thinking>` are NOT HTML and return false — the markdown engine uses
/// this to avoid swallowing them (and the lines under them) as raw HTML
/// blocks, letting them render as ordinary markdown instead.
[[nodiscard]] bool is_known_tag(std::string_view name);

/// Overlay a tag's presentational + inline-CSS styling (color=, bgcolor=,
/// style="...") onto `base`, following the CSS cascade (attributes first,
/// then `style`). Returns `base` unchanged when the tag carries no styling.
/// Lets the markdown widget honour `<span style="color:red">` and
/// `<font color=...>` with the same engine the standalone renderer uses.
[[nodiscard]] Style tag_style(const Tag& tag, Style base);

} // namespace maya::html
