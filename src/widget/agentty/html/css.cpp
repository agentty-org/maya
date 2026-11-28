// css.cpp — a small, tolerant CSS parser for inline `style="..."` attributes
// and presentational color attributes (color= / bgcolor=). Turns CSS color +
// text-decoration + font declarations into maya Style overlays.
//
// This is what makes `<span style="color:#ff0080; font-weight:bold">…</span>`
// render as an actual truecolor bold run instead of literal dim tag text. It
// is shared by both render paths (the standalone html::render block renderer
// and the markdown widget's inline style stack), so styled HTML looks
// identical wherever it appears.
//
// Scope (deliberately a safe, useful subset — never throws):
//   color / background-color / background   → fg / bg
//   font-weight (bold|bolder|700..900)      → bold
//   font-style  (italic|oblique)            → italic
//   text-decoration[-line] (underline /
//       line-through / overline / none)     → underline / strikethrough
//   opacity < 0.6 / visibility:hidden       → dim
//   Color tokens: 148 CSS named colors, #rgb, #rgba, #rrggbb, #rrggbbaa,
//                 rgb()/rgba(), hsl()/hsla(), transparent, currentColor.

#include "maya/widget/agentty/html/internal.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <optional>
#include <string_view>

namespace maya::html::detail {
namespace {

[[nodiscard]] char lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}
[[nodiscard]] bool is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

[[nodiscard]] std::string_view trim(std::string_view s) {
    std::size_t b = 0, e = s.size();
    while (b < e && is_ws(s[b])) ++b;
    while (e > b && is_ws(s[e - 1])) --e;
    return s.substr(b, e - b);
}

// Case-insensitive equality against an already-lowercase literal.
[[nodiscard]] bool ieq(std::string_view s, std::string_view lit) {
    if (s.size() != lit.size()) return false;
    for (std::size_t i = 0; i < s.size(); ++i)
        if (lower(s[i]) != lit[i]) return false;
    return true;
}

[[nodiscard]] int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    char lc = lower(c);
    if (lc >= 'a' && lc <= 'f') return 10 + (lc - 'a');
    return -1;
}

// ── CSS named colors (CSS Color Module Level 4 extended set) ────────────────
// Sorted by name for binary search. Value is packed 0xRRGGBB.
struct NamedColor { std::string_view name; std::uint32_t rgb; };

constexpr std::array<NamedColor, 148> kNamedColors = {{
    {"aliceblue", 0xF0F8FF}, {"antiquewhite", 0xFAEBD7}, {"aqua", 0x00FFFF},
    {"aquamarine", 0x7FFFD4}, {"azure", 0xF0FFFF}, {"beige", 0xF5F5DC},
    {"bisque", 0xFFE4C4}, {"black", 0x000000}, {"blanchedalmond", 0xFFEBCD},
    {"blue", 0x0000FF}, {"blueviolet", 0x8A2BE2}, {"brown", 0xA52A2A},
    {"burlywood", 0xDEB887}, {"cadetblue", 0x5F9EA0}, {"chartreuse", 0x7FFF00},
    {"chocolate", 0xD2691E}, {"coral", 0xFF7F50}, {"cornflowerblue", 0x6495ED},
    {"cornsilk", 0xFFF8DC}, {"crimson", 0xDC143C}, {"cyan", 0x00FFFF},
    {"darkblue", 0x00008B}, {"darkcyan", 0x008B8B}, {"darkgoldenrod", 0xB8860B},
    {"darkgray", 0xA9A9A9}, {"darkgreen", 0x006400}, {"darkgrey", 0xA9A9A9},
    {"darkkhaki", 0xBDB76B}, {"darkmagenta", 0x8B008B},
    {"darkolivegreen", 0x556B2F}, {"darkorange", 0xFF8C00},
    {"darkorchid", 0x9932CC}, {"darkred", 0x8B0000}, {"darksalmon", 0xE9967A},
    {"darkseagreen", 0x8FBC8F}, {"darkslateblue", 0x483D8B},
    {"darkslategray", 0x2F4F4F}, {"darkslategrey", 0x2F4F4F},
    {"darkturquoise", 0x00CED1}, {"darkviolet", 0x9400D3},
    {"deeppink", 0xFF1493}, {"deepskyblue", 0x00BFFF}, {"dimgray", 0x696969},
    {"dimgrey", 0x696969}, {"dodgerblue", 0x1E90FF}, {"firebrick", 0xB22222},
    {"floralwhite", 0xFFFAF0}, {"forestgreen", 0x228B22}, {"fuchsia", 0xFF00FF},
    {"gainsboro", 0xDCDCDC}, {"ghostwhite", 0xF8F8FF}, {"gold", 0xFFD700},
    {"goldenrod", 0xDAA520}, {"gray", 0x808080}, {"green", 0x008000},
    {"greenyellow", 0xADFF2F}, {"grey", 0x808080}, {"honeydew", 0xF0FFF0},
    {"hotpink", 0xFF69B4}, {"indianred", 0xCD5C5C}, {"indigo", 0x4B0082},
    {"ivory", 0xFFFFF0}, {"khaki", 0xF0E68C}, {"lavender", 0xE6E6FA},
    {"lavenderblush", 0xFFF0F5}, {"lawngreen", 0x7CFC00},
    {"lemonchiffon", 0xFFFACD}, {"lightblue", 0xADD8E6}, {"lightcoral", 0xF08080},
    {"lightcyan", 0xE0FFFF}, {"lightgoldenrodyellow", 0xFAFAD2},
    {"lightgray", 0xD3D3D3}, {"lightgreen", 0x90EE90}, {"lightgrey", 0xD3D3D3},
    {"lightpink", 0xFFB6C1}, {"lightsalmon", 0xFFA07A},
    {"lightseagreen", 0x20B2AA}, {"lightskyblue", 0x87CEFA},
    {"lightslategray", 0x778899}, {"lightslategrey", 0x778899},
    {"lightsteelblue", 0xB0C4DE}, {"lightyellow", 0xFFFFE0}, {"lime", 0x00FF00},
    {"limegreen", 0x32CD32}, {"linen", 0xFAF0E6}, {"magenta", 0xFF00FF},
    {"maroon", 0x800000}, {"mediumaquamarine", 0x66CDAA},
    {"mediumblue", 0x0000CD}, {"mediumorchid", 0xBA55D3},
    {"mediumpurple", 0x9370DB}, {"mediumseagreen", 0x3CB371},
    {"mediumslateblue", 0x7B68EE}, {"mediumspringgreen", 0x00FA9A},
    {"mediumturquoise", 0x48D1CC}, {"mediumvioletred", 0xC71585},
    {"midnightblue", 0x191970}, {"mintcream", 0xF5FFFA}, {"mistyrose", 0xFFE4E1},
    {"moccasin", 0xFFE4B5}, {"navajowhite", 0xFFDEAD}, {"navy", 0x000080},
    {"oldlace", 0xFDF5E6}, {"olive", 0x808000}, {"olivedrab", 0x6B8E23},
    {"orange", 0xFFA500}, {"orangered", 0xFF4500}, {"orchid", 0xDA70D6},
    {"palegoldenrod", 0xEEE8AA}, {"palegreen", 0x98FB98},
    {"paleturquoise", 0xAFEEEE}, {"palevioletred", 0xDB7093},
    {"papayawhip", 0xFFEFD5}, {"peachpuff", 0xFFDAB9}, {"peru", 0xCD853F},
    {"pink", 0xFFC0CB}, {"plum", 0xDDA0DD}, {"powderblue", 0xB0E0E6},
    {"purple", 0x800080}, {"rebeccapurple", 0x663399}, {"red", 0xFF0000},
    {"rosybrown", 0xBC8F8F}, {"royalblue", 0x4169E1}, {"saddlebrown", 0x8B4513},
    {"salmon", 0xFA8072}, {"sandybrown", 0xF4A460}, {"seagreen", 0x2E8B57},
    {"seashell", 0xFFF5EE}, {"sienna", 0xA0522D}, {"silver", 0xC0C0C0},
    {"skyblue", 0x87CEEB}, {"slateblue", 0x6A5ACD}, {"slategray", 0x708090},
    {"slategrey", 0x708090}, {"snow", 0xFFFAFA}, {"springgreen", 0x00FF7F},
    {"steelblue", 0x4682B4}, {"tan", 0xD2B48C}, {"teal", 0x008080},
    {"thistle", 0xD8BFD8}, {"tomato", 0xFF6347}, {"turquoise", 0x40E0D0},
    {"violet", 0xEE82EE}, {"wheat", 0xF5DEB3}, {"white", 0xFFFFFF},
    {"whitesmoke", 0xF5F5F5}, {"yellow", 0xFFFF00}, {"yellowgreen", 0x9ACD32},
}};

[[nodiscard]] std::optional<std::uint32_t> named_rgb(std::string_view name) {
    // binary search over the sorted table (lowercase compare)
    std::size_t lo = 0, hi = kNamedColors.size();
    while (lo < hi) {
        std::size_t mid = (lo + hi) / 2;
        std::string_view cand = kNamedColors[mid].name;
        // three-way lowercase compare
        int cmp = 0;
        std::size_t n = std::min(name.size(), cand.size());
        for (std::size_t i = 0; i < n && cmp == 0; ++i)
            cmp = lower(name[i]) - cand[i];
        if (cmp == 0) cmp = static_cast<int>(name.size()) - static_cast<int>(cand.size());
        if (cmp == 0) return kNamedColors[mid].rgb;
        if (cmp < 0) hi = mid; else lo = mid + 1;
    }
    return std::nullopt;
}

// Parse an integer or percentage channel [0,255].
[[nodiscard]] std::optional<int> parse_channel(std::string_view t) {
    t = trim(t);
    if (t.empty()) return std::nullopt;
    bool pct = t.back() == '%';
    if (pct) t = trim(t.substr(0, t.size() - 1));
    // float parse (handles "128", "50%", "127.5")
    double v = 0; bool any = false, dot = false; double frac = 0.1;
    std::size_t i = 0;
    for (; i < t.size(); ++i) {
        char c = t[i];
        if (c >= '0' && c <= '9') {
            if (!dot) { v = v * 10 + (c - '0'); }
            else { v += (c - '0') * frac; frac *= 0.1; }
            any = true;
        } else if (c == '.' && !dot) { dot = true; }
        else break;
    }
    if (!any) return std::nullopt;
    if (pct) v = v * 255.0 / 100.0;
    int iv = static_cast<int>(v + 0.5);
    return std::clamp(iv, 0, 255);
}

[[nodiscard]] std::optional<Color> from_rgb24(std::uint32_t rgb) {
    return Color::rgb(static_cast<std::uint8_t>((rgb >> 16) & 0xFF),
                      static_cast<std::uint8_t>((rgb >> 8) & 0xFF),
                      static_cast<std::uint8_t>(rgb & 0xFF));
}

} // namespace

std::optional<Color> parse_css_color(std::string_view tok) {
    tok = trim(tok);
    if (tok.empty()) return std::nullopt;
    if (ieq(tok, "transparent") || ieq(tok, "inherit") ||
        ieq(tok, "currentcolor") || ieq(tok, "initial") || ieq(tok, "unset"))
        return std::nullopt;  // no representable override

    // #hex forms
    if (tok[0] == '#') {
        std::string_view h = tok.substr(1);
        auto all_hex = [&] {
            for (char c : h) if (hex_val(c) < 0) return false;
            return !h.empty();
        };
        if (!all_hex()) return std::nullopt;
        if (h.size() == 3 || h.size() == 4) {  // #rgb / #rgba (drop alpha)
            int r = hex_val(h[0]), g = hex_val(h[1]), b = hex_val(h[2]);
            return Color::rgb(static_cast<std::uint8_t>(r * 17),
                              static_cast<std::uint8_t>(g * 17),
                              static_cast<std::uint8_t>(b * 17));
        }
        if (h.size() == 6 || h.size() == 8) {  // #rrggbb / #rrggbbaa
            int r = hex_val(h[0]) * 16 + hex_val(h[1]);
            int g = hex_val(h[2]) * 16 + hex_val(h[3]);
            int b = hex_val(h[4]) * 16 + hex_val(h[5]);
            return Color::rgb(static_cast<std::uint8_t>(r),
                              static_cast<std::uint8_t>(g),
                              static_cast<std::uint8_t>(b));
        }
        return std::nullopt;
    }

    // rgb()/rgba()/hsl()/hsla() functional forms
    auto paren = tok.find('(');
    if (paren != std::string_view::npos && tok.back() == ')') {
        std::string_view fn = trim(tok.substr(0, paren));
        std::string_view args = tok.substr(paren + 1, tok.size() - paren - 2);
        // split on ',' or whitespace/slash (CSS4 allows space-separated)
        std::array<std::string_view, 4> parts{};
        std::size_t np = 0, start = 0;
        for (std::size_t i = 0; i <= args.size() && np < 4; ++i) {
            bool sep = (i == args.size()) || args[i] == ',' || args[i] == '/' ||
                       is_ws(args[i]);
            if (sep) {
                if (i > start) { auto p = trim(args.substr(start, i - start));
                                 if (!p.empty()) parts[np++] = p; }
                start = i + 1;
            }
        }
        if (ieq(fn, "rgb") || ieq(fn, "rgba")) {
            if (np < 3) return std::nullopt;
            auto r = parse_channel(parts[0]);
            auto g = parse_channel(parts[1]);
            auto b = parse_channel(parts[2]);
            if (!r || !g || !b) return std::nullopt;
            return Color::rgb(static_cast<std::uint8_t>(*r),
                              static_cast<std::uint8_t>(*g),
                              static_cast<std::uint8_t>(*b));
        }
        if (ieq(fn, "hsl") || ieq(fn, "hsla")) {
            if (np < 3) return std::nullopt;
            auto num = [](std::string_view s) -> double {
                s = trim(s);
                bool pct = !s.empty() && s.back() == '%';
                if (pct) s = s.substr(0, s.size() - 1);
                double v = 0; std::from_chars(s.data(), s.data() + s.size(), v);
                return v;
            };
            double h = num(parts[0]);
            double sp = num(parts[1]) / 100.0;
            double lp = num(parts[2]) / 100.0;
            return Color::hsl(static_cast<float>(h), static_cast<float>(sp),
                              static_cast<float>(lp));
        }
        return std::nullopt;
    }

    // named color
    if (auto rgb = named_rgb(tok)) return from_rgb24(*rgb);
    return std::nullopt;
}

Style parse_inline_style(std::string_view css, Style base) {
    Style out = base;
    std::size_t i = 0;
    while (i < css.size()) {
        std::size_t semi = css.find(';', i);
        std::string_view decl = css.substr(i, semi == std::string_view::npos
                                                  ? std::string_view::npos
                                                  : semi - i);
        i = (semi == std::string_view::npos) ? css.size() : semi + 1;

        auto colon = decl.find(':');
        if (colon == std::string_view::npos) continue;
        std::string_view prop = trim(decl.substr(0, colon));
        std::string_view val  = trim(decl.substr(colon + 1));
        if (prop.empty() || val.empty()) continue;

        if (ieq(prop, "color")) {
            if (auto c = parse_css_color(val)) out = out.with_fg(*c);
        } else if (ieq(prop, "background-color") || ieq(prop, "background")) {
            // For shorthand `background`, use the first color-looking token.
            if (auto c = parse_css_color(val)) {
                out = out.with_bg(*c);
            } else {
                std::size_t s = 0;
                while (s < val.size()) {
                    std::size_t e = s;
                    while (e < val.size() && !is_ws(val[e])) ++e;
                    if (auto bc = parse_css_color(val.substr(s, e - s))) {
                        out = out.with_bg(*bc); break;
                    }
                    while (e < val.size() && is_ws(val[e])) ++e;
                    s = e;
                }
            }
        } else if (ieq(prop, "font-weight")) {
            bool b = ieq(val, "bold") || ieq(val, "bolder") ||
                     ieq(val, "700") || ieq(val, "800") || ieq(val, "900");
            if (b) out = out.with_bold();
            else if (ieq(val, "normal") || ieq(val, "400") || ieq(val, "300") ||
                     ieq(val, "lighter"))
                out.bold = false;
        } else if (ieq(prop, "font-style")) {
            if (ieq(val, "italic") || ieq(val, "oblique")) out = out.with_italic();
            else if (ieq(val, "normal")) out.italic = false;
        } else if (ieq(prop, "text-decoration") ||
                   ieq(prop, "text-decoration-line")) {
            // May carry multiple space-separated keywords.
            std::size_t s = 0;
            bool saw_none = false;
            while (s < val.size()) {
                std::size_t e = s;
                while (e < val.size() && !is_ws(val[e])) ++e;
                std::string_view kw = val.substr(s, e - s);
                if (ieq(kw, "underline")) out = out.with_underline();
                else if (ieq(kw, "line-through")) out = out.with_strikethrough();
                else if (ieq(kw, "none")) saw_none = true;
                s = e;
                while (s < val.size() && is_ws(val[s])) ++s;
            }
            if (saw_none) { out.underline = false; out.strikethrough = false; }
        } else if (ieq(prop, "opacity")) {
            double v = 1.0;
            std::from_chars(val.data(), val.data() + val.size(), v);
            if (v < 0.6) out = out.with_dim();
        } else if (ieq(prop, "visibility")) {
            if (ieq(val, "hidden")) out = out.with_dim();
        }
    }
    return out;
}

Style apply_presentation(const std::vector<Attr>& attrs, Style base) {
    Style out = base;
    // Presentational attributes first (lowest priority).
    if (auto c = attr_of(attrs, "color"); !c.empty())
        if (auto col = parse_css_color(c)) out = out.with_fg(*col);
    if (auto c = attr_of(attrs, "bgcolor"); !c.empty())
        if (auto col = parse_css_color(c)) out = out.with_bg(*col);
    // Inline style overrides.
    if (auto st = attr_of(attrs, "style"); !st.empty())
        out = parse_inline_style(st, out);
    return out;
}

} // namespace maya::html::detail
