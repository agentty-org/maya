// tokenizer.cpp — tolerant HTML tokenizer: byte stream → Start/End/Text
// tokens. Hand-rolled state machine modelled on the HTML5 tokenization
// stages we care about for terminal rendering (tags, attributes, comments,
// declarations, processing instructions, raw-text elements). Never throws;
// anything malformed degrades to literal text.

#include "maya/widget/agentty/html/internal.hpp"

// Reuse the markdown engine's canonical entity table so HTML and markdown
// decode &amp;/&#42;/&copy; identically (single source of truth — the table
// is generated from the WHATWG entity set).
#include "maya/widget/agentty/markdown/engine/cm_util.hpp"

#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace maya::html::detail {

std::string decode_entities(std::string_view s) {
    return ::maya::md_detail::engine::decode_entities(s);
}

namespace {

[[nodiscard]] bool is_name_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
[[nodiscard]] bool is_name_char(char c) {
    return is_name_start(c) || (c >= '0' && c <= '9') || c == '-' || c == ':' ||
           c == '_' || c == '.';
}
[[nodiscard]] bool is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}
[[nodiscard]] char lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// Lowercased ASCII compare of `s` against a literal.
[[nodiscard]] bool iequals(std::string_view s, std::string_view lit) {
    if (s.size() != lit.size()) return false;
    for (std::size_t i = 0; i < s.size(); ++i)
        if (lower(s[i]) != lit[i]) return false;
    return true;
}

class Tokenizer {
public:
    explicit Tokenizer(std::string_view s) : s_(s), n_(s.size()) {}

    std::vector<Token> run() {
        while (i_ < n_) {
            if (s_[i_] == '<' && try_markup()) continue;
            // literal text up to the next '<'
            std::size_t start = i_;
            if (s_[i_] == '<') { pending_ += '<'; ++i_; continue; }
            while (i_ < n_ && s_[i_] != '<') ++i_;
            pending_ += decode_entities(s_.substr(start, i_ - start));
        }
        flush_text();
        return std::move(out_);
    }

private:
    std::string_view s_;
    std::size_t n_;
    std::size_t i_ = 0;
    std::string pending_;
    std::vector<Token> out_;

    void flush_text() {
        if (pending_.empty()) return;
        out_.push_back(Token{.kind = Token::Kind::Text, .text = std::move(pending_)});
        pending_.clear();
    }

    // Attempt to consume a markup construct starting at s_[i_] == '<'.
    // Returns true if it consumed one (tag/comment/decl/PI), false if the
    // '<' should be treated as literal text.
    bool try_markup() {
        std::size_t j = i_ + 1;
        if (j >= n_) return false;

        // comment / declaration / CDATA
        if (s_[j] == '!') {
            if (s_.compare(j, 3, "!--") == 0) {
                std::size_t b = j + 3;
                std::size_t end;
                if (b < n_ && s_[b] == '>') end = b + 1;                 // <!-->
                else if (s_.compare(b, 2, "->") == 0) end = b + 2;        // <!--->
                else {
                    auto e = s_.find("-->", b);
                    end = (e == std::string_view::npos) ? n_ : e + 3;
                }
                i_ = end;
                return true;  // comments are dropped
            }
            // declaration <!DOCTYPE …> / CDATA — skip to '>'
            auto e = s_.find('>', j);
            i_ = (e == std::string_view::npos) ? n_ : e + 1;
            return true;
        }
        // processing instruction <? … >
        if (s_[j] == '?') {
            auto e = s_.find('>', j);
            i_ = (e == std::string_view::npos) ? n_ : e + 1;
            return true;
        }

        bool closing = (s_[j] == '/');
        if (closing) ++j;
        if (j >= n_ || !is_name_start(s_[j])) return false;  // literal '<'

        // element name
        std::size_t ns = j;
        while (j < n_ && is_name_char(s_[j])) ++j;
        std::string name;
        name.reserve(j - ns);
        for (std::size_t k = ns; k < j; ++k) name += lower(s_[k]);

        if (closing) {
            auto e = s_.find('>', j);
            i_ = (e == std::string_view::npos) ? n_ : e + 1;
            flush_text();
            out_.push_back(Token{.kind = Token::Kind::EndTag, .name = std::move(name)});
            return true;
        }

        Token tok{.kind = Token::Kind::StartTag, .name = name};
        parse_attrs(j, tok);  // advances i_ past '>'
        flush_text();
        out_.push_back(std::move(tok));

        // raw-text elements: take content verbatim until the matching close.
        if (!out_.back().self_closing && is_raw_text_element(name))
            consume_raw_text(name);
        return true;
    }

    // Parse attributes from position `j` (first byte after the element name)
    // up to and including the closing '>'. Sets tok.attrs / self_closing and
    // leaves i_ just past '>'.
    void parse_attrs(std::size_t j, Token& tok) {
        while (j < n_) {
            while (j < n_ && is_ws(s_[j])) ++j;
            if (j >= n_) break;
            if (s_[j] == '>') { ++j; break; }
            if (s_[j] == '/') {
                if (j + 1 < n_ && s_[j + 1] == '>') { tok.self_closing = true; j += 2; break; }
                ++j;  // stray slash
                continue;
            }
            // attribute name
            std::size_t ans = j;
            while (j < n_ && !is_ws(s_[j]) && s_[j] != '=' && s_[j] != '>' &&
                   s_[j] != '/')
                ++j;
            if (j == ans) { ++j; continue; }  // defensive: no progress
            Attr a;
            a.name.reserve(j - ans);
            for (std::size_t k = ans; k < j; ++k) a.name += lower(s_[k]);
            // optional value
            std::size_t save = j;
            while (j < n_ && is_ws(s_[j])) ++j;
            if (j < n_ && s_[j] == '=') {
                ++j;
                while (j < n_ && is_ws(s_[j])) ++j;
                if (j < n_ && (s_[j] == '"' || s_[j] == '\'')) {
                    char q = s_[j++];
                    std::size_t vs = j;
                    while (j < n_ && s_[j] != q) ++j;
                    a.value = decode_entities(s_.substr(vs, j - vs));
                    if (j < n_) ++j;  // closing quote
                } else {
                    std::size_t vs = j;
                    while (j < n_ && !is_ws(s_[j]) && s_[j] != '>') ++j;
                    a.value = decode_entities(s_.substr(vs, j - vs));
                }
            } else {
                j = save;  // boolean attribute, no value
            }
            tok.attrs.push_back(std::move(a));
        }
        i_ = j;
    }

    // From i_ (just past a raw-text start tag), emit the verbatim content as
    // a Text token and the matching EndTag, advancing past `</name>`.
    void consume_raw_text(std::string_view name) {
        std::size_t start = i_;
        std::size_t k = i_;
        while (k < n_) {
            if (s_[k] == '<' && k + 1 < n_ && s_[k + 1] == '/' &&
                iequals(s_.substr(k + 2, name.size()), name)) {
                std::size_t after = k + 2 + name.size();
                if (after >= n_ || s_[after] == '>' || is_ws(s_[after]) ||
                    s_[after] == '/')
                    break;
            }
            ++k;
        }
        if (k > start) {
            // <title>/<textarea> content is entity-decoded; <script>/<style>
            // are verbatim (and ignored by the renderer anyway).
            std::string content(s_.substr(start, k - start));
            if (name == "title" || name == "textarea")
                content = decode_entities(content);
            out_.push_back(Token{.kind = Token::Kind::Text, .text = std::move(content)});
        }
        // consume the close tag if present
        if (k < n_) {
            auto e = s_.find('>', k);
            i_ = (e == std::string_view::npos) ? n_ : e + 1;
            out_.push_back(Token{.kind = Token::Kind::EndTag,
                                 .name = std::string(name)});
        } else {
            i_ = n_;
        }
    }
};

} // namespace

std::vector<Token> tokenize(std::string_view src) {
    return Tokenizer(src).run();
}

} // namespace maya::html::detail
