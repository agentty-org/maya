// text_transform.cpp — Text-node post-pass: HTML entities, emoji shortcodes,
// bare-URL linkification, and GitHub @user / #N / org/repo#N mentions.
//
// Carved out of parser.cpp. This is a self-contained pass over already-parsed
// inline Text nodes: it decodes &name; / &#N; / &#xH; entities, replaces
// `:emoji:` shortcodes, turns bare URLs into Links, and recognizes GitHub
// mentions — none of which interact with the structural inline parser.
// parser.cpp's parse_inlines() wrapper calls post_process_text_nodes() once
// over the whole tree.

#include <array>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "maya/widget/agentty/markdown/ast.hpp"
#include "maya/widget/agentty/markdown/internal.hpp"

namespace maya {
namespace md_detail {

namespace {

// ── UTF-8 encode a single code point ──────────────────────────────────────
bool append_utf8(std::string& out, std::uint32_t cp) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
    if (cp < 0x80) { out += static_cast<char>(cp); return true; }
    if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
        return true;
    }
    if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
        return true;
    }
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
    return true;
}

// ── HTML entity decoding ──────────────────────────────────────────────────
// Recognises the named entities that appear in LLM output plus numeric
// (&#123; / &#xNN;) forms.
struct NamedEntity { std::string_view name; std::string_view utf8; };
constexpr std::array<NamedEntity, 40> kNamedEntities{{
    {"amp",    "&"},  {"lt",     "<"},  {"gt",    ">"},
    {"quot",   "\""}, {"apos",   "'"},  {"nbsp",  "\xC2\xA0"},
    {"copy",   "\xC2\xA9"},           {"reg",   "\xC2\xAE"},
    {"trade",  "\xE2\x84\xA2"},       {"sect",  "\xC2\xA7"},
    {"para",   "\xC2\xB6"},           {"deg",   "\xC2\xB0"},
    {"plusmn", "\xC2\xB1"},           {"times", "\xC3\x97"},
    {"divide", "\xC3\xB7"},           {"micro", "\xC2\xB5"},
    {"hellip", "\xE2\x80\xA6"},       {"mdash", "\xE2\x80\x94"},
    {"ndash",  "\xE2\x80\x93"},       {"lsquo", "\xE2\x80\x98"},
    {"rsquo",  "\xE2\x80\x99"},       {"ldquo", "\xE2\x80\x9C"},
    {"rdquo",  "\xE2\x80\x9D"},       {"laquo", "\xC2\xAB"},
    {"raquo",  "\xC2\xBB"},           {"bull",  "\xE2\x80\xA2"},
    {"dagger", "\xE2\x80\xA0"},       {"middot","\xC2\xB7"},
    {"larr",   "\xE2\x86\x90"},       {"uarr",  "\xE2\x86\x91"},
    {"rarr",   "\xE2\x86\x92"},       {"darr",  "\xE2\x86\x93"},
    {"harr",   "\xE2\x86\x94"},       {"infin", "\xE2\x88\x9E"},
    {"ne",     "\xE2\x89\xA0"},       {"le",    "\xE2\x89\xA4"},
    {"ge",     "\xE2\x89\xA5"},       {"pi",    "\xCF\x80"},
    {"check",  "\xE2\x9C\x93"},       {"cross", "\xE2\x9C\x97"},
}};

// Try to decode an HTML entity starting at text[start]='&'. On success writes
// decoded bytes into `out` and sets *consumed to the byte count incl & and ;.
bool try_decode_entity(std::string_view text, size_t start,
                       std::string& out, size_t* consumed) {
    if (start >= text.size() || text[start] != '&') return false;
    size_t limit = std::min(text.size(), start + 10);
    size_t semi = std::string_view::npos;
    for (size_t k = start + 1; k < limit; ++k) {
        if (text[k] == ';') { semi = k; break; }
        if (text[k] == '&' || text[k] == ' ') break;
    }
    if (semi == std::string_view::npos) return false;
    auto body = text.substr(start + 1, semi - start - 1);
    if (body.empty()) return false;

    if (body[0] == '#') {
        if (body.size() < 2) return false;
        std::uint32_t cp = 0;
        if (body[1] == 'x' || body[1] == 'X') {
            if (body.size() < 3 || body.size() > 8) return false;
            for (size_t k = 2; k < body.size(); ++k) {
                char c = body[k];
                cp <<= 4;
                if (c >= '0' && c <= '9') cp |= static_cast<std::uint32_t>(c - '0');
                else if (c >= 'a' && c <= 'f') cp |= static_cast<std::uint32_t>(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') cp |= static_cast<std::uint32_t>(c - 'A' + 10);
                else return false;
            }
        } else {
            if (body.size() > 8) return false;
            for (size_t k = 1; k < body.size(); ++k) {
                char c = body[k];
                if (c < '0' || c > '9') return false;
                cp = cp * 10 + static_cast<std::uint32_t>(c - '0');
            }
        }
        if (!append_utf8(out, cp)) return false;
        *consumed = semi - start + 1;
        return true;
    }

    for (auto& e : kNamedEntities) {
        if (body == e.name) {
            out.append(e.utf8);
            *consumed = semi - start + 1;
            return true;
        }
    }
    return false;
}

// ── Emoji shortcodes ──────────────────────────────────────────────────────
struct EmojiCode { std::string_view name; std::string_view utf8; };
constexpr std::array<EmojiCode, 52> kEmojis{{
    {"smile",             "\xF0\x9F\x98\x84"},
    {"grin",              "\xF0\x9F\x98\x81"},
    {"laughing",          "\xF0\x9F\x98\x86"},
    {"joy",               "\xF0\x9F\x98\x82"},
    {"heart",             "\xE2\x9D\xA4"},
    {"broken_heart",      "\xF0\x9F\x92\x94"},
    {"thumbsup",          "\xF0\x9F\x91\x8D"},
    {"thumbsdown",        "\xF0\x9F\x91\x8E"},
    {"+1",                "\xF0\x9F\x91\x8D"},
    {"-1",                "\xF0\x9F\x91\x8E"},
    {"eyes",              "\xF0\x9F\x91\x80"},
    {"tada",              "\xF0\x9F\x8E\x89"},
    {"rocket",            "\xF0\x9F\x9A\x80"},
    {"fire",              "\xF0\x9F\x94\xA5"},
    {"bug",               "\xF0\x9F\x90\x9B"},
    {"sparkles",          "\xE2\x9C\xA8"},
    {"star",              "\xE2\xAD\x90"},
    {"warning",           "\xE2\x9A\xA0\xEF\xB8\x8F"},
    {"check",             "\xE2\x9C\x85"},
    {"white_check_mark",  "\xE2\x9C\x85"},
    {"x",                 "\xE2\x9D\x8C"},
    {"lock",              "\xF0\x9F\x94\x92"},
    {"unlock",            "\xF0\x9F\x94\x93"},
    {"key",               "\xF0\x9F\x94\x91"},
    {"bulb",              "\xF0\x9F\x92\xA1"},
    {"book",              "\xF0\x9F\x93\x96"},
    {"books",             "\xF0\x9F\x93\x9A"},
    {"memo",              "\xF0\x9F\x93\x9D"},
    {"pencil",            "\xE2\x9C\x8F\xEF\xB8\x8F"},
    {"wrench",            "\xF0\x9F\x94\xA7"},
    {"hammer",            "\xF0\x9F\x94\xA8"},
    {"zap",               "\xE2\x9A\xA1"},
    {"boom",              "\xF0\x9F\x92\xA5"},
    {"rotating_light",    "\xF0\x9F\x9A\xA8"},
    {"construction",      "\xF0\x9F\x9A\xA7"},
    {"package",           "\xF0\x9F\x93\xA6"},
    {"mag",               "\xF0\x9F\x94\x8D"},
    {"chart",             "\xF0\x9F\x93\x88"},
    {"calendar",          "\xF0\x9F\x93\x85"},
    {"clock",             "\xF0\x9F\x95\x92"},
    {"hourglass",         "\xE2\x8C\x9B"},
    {"coffee",            "\xE2\x98\x95"},
    {"thinking",          "\xF0\x9F\xA4\x94"},
    {"wave",              "\xF0\x9F\x91\x8B"},
    {"muscle",            "\xF0\x9F\x92\xAA"},
    {"ok_hand",           "\xF0\x9F\x91\x8C"},
    {"clap",              "\xF0\x9F\x91\x8F"},
    {"sunny",             "\xE2\x98\x80\xEF\xB8\x8F"},
    {"cloud",             "\xE2\x98\x81\xEF\xB8\x8F"},
    {"snowflake",         "\xE2\x9D\x84\xEF\xB8\x8F"},
    {"moon",              "\xF0\x9F\x8C\x99"},
    {"earth",             "\xF0\x9F\x8C\x8D"},
}};

std::string_view lookup_emoji(std::string_view name) {
    for (auto& e : kEmojis)
        if (e.name == name) return e.utf8;
    return {};
}

// ── URL / mention sniffing helpers ────────────────────────────────────────
bool is_url_char(unsigned char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           c == '-' || c == '_' || c == '.' || c == '~' ||
           c == '/' || c == '?' || c == '#' || c == '=' || c == '&' ||
           c == '%' || c == '+' || c == ':' || c == '@' || c == ',' ||
           c == ';' || c == '!' || c == '(' || c == ')';
}

bool is_word_char(unsigned char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') || c == '_';
}

// Find the end of a bare URL starting at `start`. Trailing punctuation is
// stripped so "See https://x.com/a." doesn't swallow the period.
size_t scan_bare_url(std::string_view text, size_t start) {
    size_t e = start;
    while (e < text.size() && is_url_char(static_cast<unsigned char>(text[e])))
        ++e;
    while (e > start) {
        char c = text[e - 1];
        if (c == '.' || c == ',' || c == ';' || c == ':' ||
            c == '!' || c == '?' || c == ')') --e;
        else break;
    }
    return e;
}

// Expand a single Text run into a sequence of Inline nodes, decoding HTML
// entities, replacing `:emoji:` shortcodes, linkifying bare URLs, and turning
// `@user` / `#N` / `org/repo#N` into Mention nodes. Single left-to-right pass.
std::vector<md::Inline> split_text_transform(std::string_view text) {
    std::vector<md::Inline> out;
    std::string buf;

    auto flush = [&] {
        if (!buf.empty()) {
            out.push_back(md::Text{std::move(buf)});
            buf.clear();
        }
    };

    auto word_boundary_before = [&](size_t i) -> bool {
        if (i == 0 && buf.empty()) return true;
        char prev = (i > 0) ? text[i - 1] : (buf.empty() ? '\0' : buf.back());
        return !is_word_char(static_cast<unsigned char>(prev));
    };

    size_t i = 0;
    while (i < text.size()) {
        char c = text[i];

        // HTML entity: &name; / &#N; / &#xH;
        if (c == '&') {
            std::string decoded;
            size_t consumed = 0;
            if (try_decode_entity(text, i, decoded, &consumed)) {
                buf += decoded;
                i += consumed;
                continue;
            }
        }

        // Emoji shortcode :name:
        if (c == ':' && i + 1 < text.size()) {
            char n0 = text[i + 1];
            if (std::isalpha(static_cast<unsigned char>(n0)) ||
                n0 == '+' || n0 == '-' || n0 == '_') {
                size_t limit = std::min(text.size(), i + 40);
                size_t end = std::string_view::npos;
                for (size_t s = i + 1; s < limit; ++s) {
                    char x = text[s];
                    if (x == ':') { end = s; break; }
                    if (!std::isalnum(static_cast<unsigned char>(x)) &&
                        x != '_' && x != '-' && x != '+') { end = std::string_view::npos; break; }
                }
                if (end != std::string_view::npos && end > i + 1) {
                    auto name = text.substr(i + 1, end - i - 1);
                    auto utf8 = lookup_emoji(name);
                    if (!utf8.empty()) {
                        buf.append(utf8);
                        i = end + 1;
                        continue;
                    }
                }
            }
        }

        // Bare URL: http:// https:// www.  (word-boundary preceding)
        if ((c == 'h' || c == 'w' || c == 'H' || c == 'W') &&
            word_boundary_before(i)) {
            size_t prefix = 0;
            bool need_scheme = false;
            auto tail = text.substr(i);
            if (tail.size() >= 7 && (tail.substr(0, 7) == "http://" ||
                                     tail.substr(0, 7) == "HTTP://")) prefix = 7;
            else if (tail.size() >= 8 && (tail.substr(0, 8) == "https://" ||
                                          tail.substr(0, 8) == "HTTPS://")) prefix = 8;
            else if (tail.size() >= 5 && (tail.substr(0, 4) == "www." ||
                                          tail.substr(0, 4) == "WWW.") &&
                     std::isalnum(static_cast<unsigned char>(tail[4]))) {
                prefix = 4;
                need_scheme = true;
            }
            if (prefix > 0) {
                size_t end = scan_bare_url(text, i + prefix);
                if (end > i + prefix + 1) {
                    auto span = text.substr(i, end - i);
                    std::string href = need_scheme
                        ? "http://" + std::string{span}
                        : std::string{span};
                    flush();
                    out.push_back(md::Link{
                        std::string{span}, std::move(href), ""});
                    i = end;
                    continue;
                }
            }
        }

        // GitHub cross-repo ref: owner/repo#NNN
        if ((std::isalnum(static_cast<unsigned char>(c)) || c == '_') &&
            word_boundary_before(i)) {
            size_t p = i;
            while (p < text.size() && (std::isalnum(static_cast<unsigned char>(text[p])) ||
                                       text[p] == '-' || text[p] == '_' ||
                                       text[p] == '.')) ++p;
            if (p > i && p < text.size() && text[p] == '/') {
                size_t q = p + 1;
                while (q < text.size() && (std::isalnum(static_cast<unsigned char>(text[q])) ||
                                           text[q] == '-' || text[q] == '_' ||
                                           text[q] == '.')) ++q;
                if (q > p + 1 && q < text.size() && text[q] == '#') {
                    size_t r = q + 1;
                    while (r < text.size() &&
                           std::isdigit(static_cast<unsigned char>(text[r]))) ++r;
                    if (r > q + 1 && r - q - 1 <= 10) {
                        auto owner = text.substr(i,     p - i);
                        auto repo  = text.substr(p + 1, q - p - 1);
                        auto num   = text.substr(q + 1, r - q - 1);
                        std::string display{text.substr(i, r - i)};
                        std::string url = "https://github.com/";
                        url.append(owner); url += '/';
                        url.append(repo);  url += "/issues/";
                        url.append(num);
                        flush();
                        out.push_back(md::Mention{
                            md::Mention::Kind::CrossRepo, std::move(display), std::move(url)});
                        i = r;
                        continue;
                    }
                }
            }
        }

        // @user mention
        if (c == '@' && word_boundary_before(i) && i + 1 < text.size() &&
            (std::isalnum(static_cast<unsigned char>(text[i + 1])) || text[i + 1] == '_')) {
            size_t e = i + 1;
            while (e < text.size() && (std::isalnum(static_cast<unsigned char>(text[e])) ||
                                       text[e] == '-' || text[e] == '_')) ++e;
            if (e - i >= 2 && e - i <= 40) {
                std::string display{text.substr(i, e - i)};
                std::string url = "https://github.com/";
                url.append(text.substr(i + 1, e - i - 1));
                flush();
                out.push_back(md::Mention{
                    md::Mention::Kind::User, std::move(display), std::move(url)});
                i = e;
                continue;
            }
        }

        // #NNN issue reference
        if (c == '#' && word_boundary_before(i) && i + 1 < text.size() &&
            std::isdigit(static_cast<unsigned char>(text[i + 1]))) {
            size_t e = i + 1;
            while (e < text.size() &&
                   std::isdigit(static_cast<unsigned char>(text[e]))) ++e;
            if (e - i >= 2 && e - i <= 10) {
                std::string display{text.substr(i, e - i)};
                flush();
                out.push_back(md::Mention{
                    md::Mention::Kind::Issue, std::move(display), ""});
                i = e;
                continue;
            }
        }

        buf += c;
        ++i;
    }
    flush();
    return out;
}

} // anonymous namespace

// Apply split_text_transform to every Text node in `nodes`, recursing into
// the children of container inlines (Bold/Italic/…). The delimiter-stack
// inline parser builds emphasis children directly into the node list rather
// than via a recursive parse_inlines call, so the entity / emoji / bare-URL /
// mention post-pass must descend into them here. Adjacent Text runs coalesce.
void post_process_text_nodes(std::vector<md::Inline>& nodes) {
    auto recurse_children = [](md::Inline& span) {
        std::visit([](auto& n) {
            if constexpr (requires { n.children; }) {
                post_process_text_nodes(n.children);
            }
        }, span.inner);
    };

    std::vector<md::Inline> out;
    out.reserve(nodes.size());
    for (auto& span : nodes) {
        auto* t = std::get_if<md::Text>(&span.inner);
        if (!t) { recurse_children(span); out.push_back(std::move(span)); continue; }
        if (t->content.empty()) continue;
        // Skip the transform if no trigger chars present.
        bool has_trigger = false;
        for (char c : t->content) {
            if (c == '&' || c == ':' || c == '@' || c == '#' ||
                c == 'h' || c == 'w' || c == 'H' || c == 'W' || c == '/') {
                has_trigger = true; break;
            }
        }
        if (!has_trigger) { out.push_back(std::move(span)); continue; }
        auto expanded = split_text_transform(t->content);
        for (auto& e : expanded) {
            auto* et = std::get_if<md::Text>(&e.inner);
            if (et && !out.empty()) {
                if (auto* ot = std::get_if<md::Text>(&out.back().inner)) {
                    ot->content += et->content;
                    continue;
                }
            }
            out.push_back(std::move(e));
        }
    }
    nodes = std::move(out);
}

} // namespace md_detail
} // namespace maya
