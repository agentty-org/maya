#pragma once
// cm_html.hpp — TESTS ONLY. Serialize a parsed md::Document to
// CommonMark-style HTML so the parser can be scored against the
// official spec.json example set. This is a *parser* conformance
// probe — it walks the AST (md::Block / md::Inline), independent of
// the terminal Element renderer.
//
// It is intentionally a faithful-as-the-AST-allows mapping, not a
// byte-exact CommonMark emitter: the maya AST is lossy in a few places
// the spec cares about (paragraph soft-break whitespace is collapsed,
// link text is pre-flattened to a string, tight/loose is not tracked).
// Those show up as scored failures — which is the point: the number is
// a ratchet to drive the engine rebuild, and every real parser fix
// moves it up.

#include <string>
#include <string_view>

#include "maya/widget/agentty/markdown.hpp"

namespace cm {
using namespace maya;

// ── HTML escaping ──────────────────────────────────────────────────
inline void esc_text(std::string& out, std::string_view s) {
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;";  break;
            case '<': out += "&lt;";   break;
            case '>': out += "&gt;";   break;
            case '"': out += "&quot;"; break;
            default:  out += c;        break;
        }
    }
}

inline void esc_attr(std::string& out, std::string_view s) {
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;";  break;
            case '<': out += "&lt;";   break;
            case '>': out += "&gt;";   break;
            case '"': out += "&quot;"; break;
            default:  out += c;        break;
        }
    }
}

// CommonMark percent-encodes a specific set in URLs while leaving most
// reserved/unreserved chars intact, then HTML-escapes & and ". This is
// the common subset; exotic URL normalization cases will still fail.
inline void esc_url(std::string& out, std::string_view s) {
    auto hex = [](unsigned v) -> char {
        return static_cast<char>(v < 10 ? '0' + v : 'A' + (v - 10));
    };
    auto pct = [&](unsigned char c) {
        out += '%'; out += hex(c >> 4); out += hex(c & 0xF);
    };
    for (unsigned char c : s) {
        if (c == '&') { out += "&amp;"; continue; }
        // CommonMark percent-encodes these unsafe ASCII chars in hrefs,
        // leaves the rest (incl. most reserved chars) intact.
        switch (c) {
            case '"': pct('"'); continue;
            case ' ': pct(' '); continue;
            case '`': pct('`'); continue;
            case '<': pct('<'); continue;
            case '>': pct('>'); continue;
            case '[': pct('['); continue;
            case ']': pct(']'); continue;
            case '\\': pct('\\'); continue;
            case '^': pct('^'); continue;
            case '{': pct('{'); continue;
            case '|': pct('|'); continue;
            case '}': pct('}'); continue;
            default: break;
        }
        if (c < 0x20 || c >= 0x7F) { pct(c); continue; }
        out += static_cast<char>(c);
    }
}

// ── Inline serialization ───────────────────────────────────────────
inline void inline_to_html(std::string& out, const md::Inline& span);

inline void inlines_to_html(std::string& out,
                            const std::vector<md::Inline>& spans) {
    for (const auto& s : spans) inline_to_html(out, s);
}

inline void inline_to_html(std::string& out, const md::Inline& span) {
    std::visit([&](const auto& n) {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, md::Text>) {
            esc_text(out, n.content);
        } else if constexpr (std::is_same_v<T, md::Bold>) {
            out += "<strong>"; inlines_to_html(out, n.children); out += "</strong>";
        } else if constexpr (std::is_same_v<T, md::Italic>) {
            out += "<em>"; inlines_to_html(out, n.children); out += "</em>";
        } else if constexpr (std::is_same_v<T, md::BoldItalic>) {
            out += "<em><strong>"; inlines_to_html(out, n.children);
            out += "</strong></em>";
        } else if constexpr (std::is_same_v<T, md::Code>) {
            out += "<code>"; esc_text(out, n.content); out += "</code>";
        } else if constexpr (std::is_same_v<T, md::Link>) {
            out += "<a href=\""; esc_url(out, n.url); out += "\"";
            if (!n.title.empty()) { out += " title=\""; esc_attr(out, n.title); out += "\""; }
            out += ">";
            if (!n.kids.empty()) inlines_to_html(out, n.kids);
            else esc_text(out, n.text);
            out += "</a>";
        } else if constexpr (std::is_same_v<T, md::Image>) {
            out += "<img src=\""; esc_url(out, n.url); out += "\" alt=\"";
            esc_attr(out, n.alt); out += "\"";
            if (!n.title.empty()) { out += " title=\""; esc_attr(out, n.title); out += "\""; }
            out += " />";
        } else if constexpr (std::is_same_v<T, md::Strike>) {
            out += "<del>"; inlines_to_html(out, n.children); out += "</del>";
        } else if constexpr (std::is_same_v<T, md::Highlight>) {
            out += "<mark>"; inlines_to_html(out, n.children); out += "</mark>";
        } else if constexpr (std::is_same_v<T, md::Sub>) {
            out += "<sub>"; inlines_to_html(out, n.children); out += "</sub>";
        } else if constexpr (std::is_same_v<T, md::Sup>) {
            out += "<sup>"; inlines_to_html(out, n.children); out += "</sup>";
        } else if constexpr (std::is_same_v<T, md::Kbd>) {
            out += "<kbd>"; inlines_to_html(out, n.children); out += "</kbd>";
        } else if constexpr (std::is_same_v<T, md::Abbr>) {
            out += "<abbr title=\""; esc_attr(out, n.title); out += "\">";
            inlines_to_html(out, n.children); out += "</abbr>";
        } else if constexpr (std::is_same_v<T, md::Mention>) {
            esc_text(out, n.display);
        } else if constexpr (std::is_same_v<T, md::FootnoteRef>) {
            out += "[^"; esc_text(out, n.label); out += "]";
        } else if constexpr (std::is_same_v<T, md::HardBreak>) {
            out += "<br />\n";
        } else if constexpr (std::is_same_v<T, md::SoftBreak>) {
            out += "\n";
        } else if constexpr (std::is_same_v<T, md::RawInline>) {
            out += n.content;  // verbatim — raw HTML / passthrough
        } else if constexpr (std::is_same_v<T, md::MathInline>) {
            // KaTeX/MathJax convention: inline math as \(…\) inside a
            // span the client-side typesetter picks up.
            out += "<span class=\"math inline\">\\(";
            esc_text(out, n.latex);
            out += "\\)</span>";
        }
    }, span.inner);
}

// ── Block serialization ────────────────────────────────────────────
inline void block_to_html(std::string& out, const md::Block& block);

inline void blocks_to_html(std::string& out,
                           const std::vector<md::Block>& blocks) {
    for (const auto& b : blocks) block_to_html(out, b);
}

inline void block_to_html(std::string& out, const md::Block& block) {
    std::visit([&](const auto& n) {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, md::Paragraph>) {
            out += "<p>"; inlines_to_html(out, n.spans); out += "</p>\n";
        } else if constexpr (std::is_same_v<T, md::Heading>) {
            std::string h = "h" + std::to_string(n.level);
            out += "<" + h + ">"; inlines_to_html(out, n.spans);
            out += "</" + h + ">\n";
        } else if constexpr (std::is_same_v<T, md::CodeBlock>) {
            out += "<pre><code";
            if (!n.lang.empty()) {
                // info string's first word is the language class
                std::string_view lang{n.lang};
                auto sp = lang.find_first_of(" \t");
                if (sp != std::string_view::npos) lang = lang.substr(0, sp);
                out += " class=\"language-"; esc_attr(out, lang); out += "\"";
            }
            out += ">"; esc_text(out, n.content);
            if (n.content.empty() || n.content.back() != '\n') out += '\n';
            out += "</code></pre>\n";
        } else if constexpr (std::is_same_v<T, md::Blockquote>) {
            out += "<blockquote>\n"; blocks_to_html(out, n.children);
            out += "</blockquote>\n";
        } else if constexpr (std::is_same_v<T, md::List>) {
            if (n.ordered) {
                out += "<ol";
                if (n.start_num != 1) out += " start=\"" + std::to_string(n.start_num) + "\"";
                out += ">\n";
            } else {
                out += "<ul>\n";
            }
            for (const auto& it : n.items) {
                out += "<li>";
                if (it.checked.has_value()) {
                    out += "<input ";
                    if (*it.checked) out += "checked=\"\" ";
                    out += "disabled=\"\" type=\"checkbox\" /> ";
                }
                // Loose lists wrap the item's first-line content in <p>;
                // tight lists render it bare. Child blocks emit their own
                // wrapping either way.
                if (n.loose) {
                    if (!it.spans.empty()) {
                        out += "<p>"; inlines_to_html(out, it.spans); out += "</p>\n";
                    }
                } else {
                    inlines_to_html(out, it.spans);
                }
                if (!it.children.empty()) {
                    out += "\n";
                    if (n.loose) {
                        blocks_to_html(out, it.children);
                    } else {
                        // Tight list: paragraph children render their inline
                        // content bare (no <p>); other blocks render normally.
                        for (const auto& cb : it.children) {
                            if (std::holds_alternative<md::Paragraph>(cb.inner)) {
                                inlines_to_html(
                                    out, std::get<md::Paragraph>(cb.inner).spans);
                                out += "\n";
                            } else {
                                block_to_html(out, cb);
                            }
                        }
                    }
                }
                out += "</li>\n";
            }
            out += n.ordered ? "</ol>\n" : "</ul>\n";
        } else if constexpr (std::is_same_v<T, md::HRule>) {
            out += "<hr />\n";
        } else if constexpr (std::is_same_v<T, md::Table>) {
            out += "<table>\n<thead>\n<tr>\n";
            for (const auto& c : n.header.cells) {
                out += "<th>"; inlines_to_html(out, c.spans); out += "</th>\n";
            }
            out += "</tr>\n</thead>\n";
            if (!n.rows.empty()) {
                out += "<tbody>\n";
                for (const auto& r : n.rows) {
                    out += "<tr>\n";
                    for (const auto& c : r.cells) {
                        out += "<td>"; inlines_to_html(out, c.spans); out += "</td>\n";
                    }
                    out += "</tr>\n";
                }
                out += "</tbody>\n";
            }
            out += "</table>\n";
        } else if constexpr (std::is_same_v<T, md::HtmlBlock>) {
            out += n.content;
            if (n.content.empty() || n.content.back() != '\n') out += '\n';
        } else if constexpr (std::is_same_v<T, md::FootnoteDef>) {
            // extension; emit a stable shape
            out += "<section class=\"footnote\" id=\"fn-";
            esc_attr(out, n.label); out += "\">\n";
            blocks_to_html(out, n.children); out += "</section>\n";
        } else if constexpr (std::is_same_v<T, md::Alert>) {
            out += "<blockquote class=\"alert\">\n";
            blocks_to_html(out, n.children); out += "</blockquote>\n";
        } else if constexpr (std::is_same_v<T, md::DefList>) {
            out += "<dl>\n";
            for (const auto& it : n.items) {
                out += "<dt>"; inlines_to_html(out, it.term); out += "</dt>\n";
                for (const auto& def : it.defs) {
                    out += "<dd>"; blocks_to_html(out, def); out += "</dd>\n";
                }
            }
            out += "</dl>\n";
        } else if constexpr (std::is_same_v<T, md::Details>) {
            out += "<details>\n<summary>";
            inlines_to_html(out, n.summary); out += "</summary>\n";
            blocks_to_html(out, n.body); out += "</details>\n";
        }
    }, block.inner);
}

// Top-level: parse + serialize.
inline std::string to_html(const md::Document& doc) {
    std::string out;
    blocks_to_html(out, doc.blocks);
    return out;
}

} // namespace cm
