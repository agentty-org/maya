#pragma once
// html/internal.hpp — engine-private types shared across the HTML widget's
// translation units (tokenizer → tree builder → renderer). Private to the
// implementation; NOT installed.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "maya/style/color.hpp"
#include "maya/style/style.hpp"

namespace maya::html::detail {

// ── attributes ────────────────────────────────────────────────────────────
struct Attr {
    std::string name;   // lowercased
    std::string value;  // entity-decoded
};

[[nodiscard]] inline std::string_view attr_of(const std::vector<Attr>& attrs,
                                              std::string_view name) {
    for (const auto& a : attrs)
        if (a.name == name) return a.value;
    return {};
}

// ── token stream (tokenizer.cpp) ────────────────────────────────────────────
struct Token {
    enum class Kind : std::uint8_t { StartTag, EndTag, Text };
    Kind              kind = Kind::Text;
    std::string       name;              // lowercased element name (tags)
    std::vector<Attr> attrs;             // start-tag attributes
    bool              self_closing = false;
    std::string       text;              // Text token payload (entities decoded)
};

[[nodiscard]] std::vector<Token> tokenize(std::string_view src);

// Decode HTML entities / numeric refs in `s` (reuses the markdown engine's
// canonical entity table so the two stay in lockstep).
[[nodiscard]] std::string decode_entities(std::string_view s);

// ── DOM tree (parser.cpp) ────────────────────────────────────────────────────
struct Node {
    enum class Kind : std::uint8_t { Document, Element, Text };
    Kind              kind = Kind::Element;
    std::string       tag;        // lowercased element name (Element)
    std::vector<Attr> attrs;      // Element attributes
    std::string       text;       // Text payload (Text)
    std::vector<Node> children;
};

// Tokenize + build a tolerant DOM (implied closes, void elements, raw-text
// elements). Returns a Document node.
[[nodiscard]] Node parse(std::string_view src);

// ── CSS style parsing (css.cpp) ──────────────────────────────────────────────
// Parse a `style="..."` attribute value (semicolon-separated `prop: value`
// declarations) and overlay the recognised properties onto `base`. Understands
// color / background[-color] / font-weight / font-style / text-decoration.
[[nodiscard]] Style parse_inline_style(std::string_view css, Style base);

// Parse a single CSS color token (named, #rgb, #rrggbb, rgb()/rgba()) into a
// maya Color. Returns nullopt when the token is empty or unrecognised.
[[nodiscard]] std::optional<Color> parse_css_color(std::string_view tok);

// Apply presentational attributes (color=, bgcolor=, face-agnostic) plus any
// inline `style=` onto `base`, in CSS cascade order (presentational first,
// then style overrides). Used by both render paths for <span>/<font>/etc.
[[nodiscard]] Style apply_presentation(const std::vector<Attr>& attrs, Style base);

// ── element classification (tags.cpp) ───────────────────────────────────────
// Void elements never have content/closing tags (br, hr, img, …).
[[nodiscard]] bool is_void_element(std::string_view name);
// Block-level elements break the inline flow and stack vertically.
[[nodiscard]] bool is_block_element(std::string_view name);
// Raw-text elements whose content is taken verbatim until the close tag
// (script, style, textarea). <pre> is NOT raw-text — it keeps markup but
// preserves whitespace, handled in the renderer.
[[nodiscard]] bool is_raw_text_element(std::string_view name);

} // namespace maya::html::detail
