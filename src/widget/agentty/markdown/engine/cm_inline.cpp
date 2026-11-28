// cm_inline.cpp — CommonMark inline-structure phase (spec §6).
//
// The delimiter-stack algorithm: scan the text left-to-right producing a
// flat list of inline nodes, pushing emphasis delimiters (* _) and link/
// image openers ([ ![) onto a stack; when we hit a ] or end of input we
// resolve links/images, then process emphasis by walking the delimiter
// stack pairing openers with closers under the flanking + multiple-of-3
// rules. Code spans, autolinks, raw HTML, entities, backslash escapes, and
// hard/soft line breaks are handled inline during the scan.
//
// Reference: https://spec.commonmark.org/0.31.2/#inlines

#include "maya/widget/agentty/markdown/engine/cm_engine.hpp"
#include "maya/widget/agentty/markdown/engine/cm_util.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace maya::md_detail {
// AST-level text-transform pass (text_transform.cpp): :emoji:, @mentions,
// bare-URL links, maya entity set. Reused by the engine's extension layer.
void post_process_text_nodes(std::vector<md::Inline>& nodes);
}

namespace maya::md_detail::engine {
namespace {

// A node under construction. We keep a flat vector; emphasis resolution
// reaches back into it by index. Text nodes are mutable so emphasis can
// split them at delimiter runs.
struct Node {
    md::Inline span;
    // delimiter bookkeeping (only meaningful for delimiter-run text nodes)
    bool is_delim = false;
    char delim_char = 0;     // '*', '_', '[', '!'
    int  delim_count = 0;    // remaining usable delimiters
    bool can_open = false;
    bool can_close = false;
    bool active = true;      // for link openers
    bool is_image = false;   // for openers
    bool removed = false;     // tombstone: consumed by emphasis, skip everywhere
    std::size_t text_pos = 0; // for [ / ![ openers: byte offset of label content
    std::size_t orig_count = 0;
    int emph_depth = 0;       // emphasis nesting depth of this node

    Node() : span(md::Inline{md::Text{}}) {}
    explicit Node(md::Inline s) : span(std::move(s)) {}
};

class InlineParser {
public:
    explicit InlineParser(const RefMap& refs, bool ext = false)
        : refs_(refs), ext_(ext) {}

    std::vector<md::Inline> parse(std::string_view text) {
        text_ = text;
        scan();
        process_emphasis(0);
        // collect output, skipping fully-consumed delimiters
        std::vector<md::Inline> out;
        for (auto& n : nodes_) {
            if (n.removed) continue;
            if (n.is_delim && n.delim_count > 0) {
                // leftover delimiter run → literal text
                std::string lit(static_cast<std::size_t>(n.delim_count), n.delim_char);
                out.push_back(md::Text{std::move(lit)});
            } else if (n.is_delim) {
                // openers ([ !) that were never matched → literal
                if (n.delim_char == '[') out.push_back(md::Text{"["});
                else if (n.delim_char == '!') out.push_back(md::Text{"!["});
                // consumed emphasis delimiters contribute nothing
            } else {
                out.push_back(std::move(n.span));
            }
        }
        return merge_text(std::move(out));
    }

private:
    std::string_view text_;
    const RefMap& refs_;
    bool ext_ = false;     // GFM/maya extensions enabled
    std::vector<Node> nodes_;

    void push_text(std::string s) {
        if (s.empty()) return;
        nodes_.push_back(Node{md::Inline{md::Text{std::move(s)}}});
    }
    void push_node(md::Inline n) {
        nodes_.push_back(Node{std::move(n)});
    }

    // ── main scan ────────────────────────────────────────────────────────
    void scan() {
        std::size_t i = 0;
        std::string pending;
        auto flush = [&] { if (!pending.empty()) { push_text(std::move(pending)); pending.clear(); } };

        while (i < text_.size()) {
            char c = text_[i];
            switch (c) {
                case '\\': {
                    if (i + 1 < text_.size()) {
                        char n = text_[i + 1];
                        // ext: \(...\) inline math (LaTeX delimiters used by
                        // most chat models). Content is verbatim TeX — typeset
                        // by the terminal renderer, NOT re-parsed as markdown.
                        if (ext_ && n == '(') {
                            std::size_t adv;
                            if (try_math_paren(i, pending, adv)) { i = adv; continue; }
                        }
                        if (n == '\n') {
                            // hard break
                            flush();
                            push_node(md::HardBreak{});
                            i += 2;
                            // skip leading spaces of next line
                            while (i < text_.size() && text_[i] == ' ') ++i;
                            continue;
                        }
                        if (is_ascii_punct(n)) {
                            pending += n;
                            i += 2;
                            continue;
                        }
                    }
                    pending += '\\';
                    ++i;
                    continue;
                }
                case '`': {
                    std::size_t adv;
                    if (try_code_span(i, pending, adv)) { i = adv; continue; }
                    // No matching closing run: the ENTIRE opening run is
                    // literal (so ```foo`` doesn't spuriously match the
                    // trailing `` as a shorter span).
                    std::size_t run = 0;
                    while (i + run < text_.size() && text_[i + run] == '`') ++run;
                    pending.append(run, '`');
                    i += run;
                    continue;
                }
                case '<': {
                    std::size_t adv;
                    if (try_autolink_or_html(i, pending, adv)) { i = adv; continue; }
                    pending += '<';
                    ++i;
                    continue;
                }
                case '&': {
                    std::string dec;
                    std::size_t used = decode_entity(text_, i, dec);
                    if (used) { pending += dec; i += used; continue; }
                    pending += '&';
                    ++i;
                    continue;
                }
                case '\n': {
                    // soft or hard break: trailing >=2 spaces → hard
                    flush();
                    std::size_t sp = pending.size();
                    (void)sp;
                    // count trailing spaces already in pending? we flushed; so
                    // look back at text_ before the newline
                    std::size_t back = i;
                    int spaces = 0;
                    while (back > 0 && text_[back - 1] == ' ') { ++spaces; --back; }
                    if (spaces >= 2) {
                        // remove the trailing spaces from the last text node
                        trim_trailing_spaces();
                        push_node(md::HardBreak{});
                    } else {
                        trim_trailing_spaces();
                        push_node(md::SoftBreak{});
                    }
                    ++i;
                    // skip leading spaces of the continuation line
                    while (i < text_.size() && (text_[i] == ' ' || text_[i] == '\t')) ++i;
                    continue;
                }
                case '*':
                case '_': {
                    flush();
                    scan_delim_run(i, c);
                    continue;
                }
                case '[': {
                    // ext: [^label] footnote reference (not [^label](...) link)
                    if (ext_ && i + 1 < text_.size() && text_[i + 1] == '^') {
                        std::size_t adv;
                        if (try_footnote_ref(i, pending, adv)) { i = adv; continue; }
                    }
                    flush();
                    Node n{md::Inline{md::Text{"["}}};
                    n.is_delim = true;
                    n.delim_char = '[';
                    n.text_pos = i + 1;
                    nodes_.push_back(std::move(n));
                    ++i;
                    continue;
                }
                case '~':
                case '=':
                case '^': {
                    std::size_t adv;
                    if (ext_ && try_ext_span(i, c, pending, adv)) { i = adv; continue; }
                    pending += c;
                    ++i;
                    continue;
                }
                case '!': {
                    if (i + 1 < text_.size() && text_[i + 1] == '[') {
                        flush();
                        Node n{md::Inline{md::Text{"!["}}};
                        n.is_delim = true;
                        n.delim_char = '!';
                        n.is_image = true;
                        n.text_pos = i + 2;
                        nodes_.push_back(std::move(n));
                        i += 2;
                        continue;
                    }
                    pending += '!';
                    ++i;
                    continue;
                }
                case ']': {
                    flush();
                    if (!try_close_link(i)) { pending += ']'; ++i; }
                    continue;
                }
                case '$': {
                    // ext: $...$ inline math (TeX / dollar delimiters). A $$ run
                    // here is display math better handled at block level, but a
                    // paragraph-embedded $$a$$ still typesets inline as a
                    // fallback. Escaped \$ was already consumed as a literal in
                    // the backslash case, so any $ reaching here is a delimiter
                    // candidate.
                    std::size_t adv;
                    if (ext_ && try_math_dollar(i, pending, adv)) { i = adv; continue; }
                    pending += '$';
                    ++i;
                    continue;
                }
                default:
                    pending += c;
                    ++i;
                    continue;
            }
        }
        flush();
    }

    void trim_trailing_spaces() {
        if (nodes_.empty()) return;
        Node& n = nodes_.back();
        if (n.is_delim) return;
        if (auto* t = std::get_if<md::Text>(&n.span.inner)) {
            while (!t->content.empty() && t->content.back() == ' ')
                t->content.pop_back();
            if (t->content.empty()) nodes_.pop_back();
        }
    }

    // ── extensions (gated on ext_) ───────────────────────────────────────
    void flush_pending(std::string& pending) {
        if (!pending.empty()) { push_text(std::move(pending)); pending.clear(); }
    }

    // A paired-delimiter span: open == close (e.g. "~~", "=="). Inner content
    // is recursively inline-parsed so it can hold emphasis/links/etc.
    template <typename Make>
    bool try_paired(std::size_t i, std::string_view delim, std::string& pending,
                    std::size_t& adv, Make make) {
        std::size_t open_end = i + delim.size();
        std::size_t j = text_.find(delim, open_end);
        if (j == std::string_view::npos || j - open_end > 2000) return false;
        if (j == open_end) return false;  // empty
        InlineParser sub(refs_, ext_);
        auto kids = sub.parse(text_.substr(open_end, j - open_end));
        flush_pending(pending);
        push_node(make(std::move(kids)));
        adv = j + delim.size();
        return true;
    }

    // A single-delimiter span with no internal whitespace, bounded length
    // (~sub~, ^sup^). Spaces or newlines abort the match.
    template <typename Make>
    bool try_bounded(std::size_t i, char c, std::string& pending,
                     std::size_t& adv, Make make) {
        std::size_t open = i + 1;
        if (open >= text_.size()) return false;
        char first = text_[open];
        if (first == c || first == ' ' || first == '\t' || first == '\n')
            return false;
        std::size_t limit = std::min(text_.size(), open + 200);
        std::size_t j = open;
        while (j < limit && text_[j] != c) {
            char ch = text_[j];
            if (ch == ' ' || ch == '\t' || ch == '\n') return false;
            ++j;
        }
        if (j >= limit || text_[j] != c) return false;
        InlineParser sub(refs_, ext_);
        auto kids = sub.parse(text_.substr(open, j - open));
        flush_pending(pending);
        push_node(make(std::move(kids)));
        adv = j + 1;
        return true;
    }

    // ── math spans ($…$, \(…\)) ─────────────────────────────────────────
    // The body is captured VERBATIM (no markdown re-parse, no entity decode,
    // no backslash-escape collapse) because it is LaTeX destined for the math
    // typesetter. Delimiters follow the de-facto chat-model conventions:
    //   • `$x$`   — inline; a `$` with no matching close, or a `$ `/`100$`
    //               currency pattern, stays literal.
    //   • `\(x\)` — the LaTeX inline form (Claude/OpenAI emit this a lot).
    bool make_math(std::size_t body_start, std::size_t body_end,
                   std::string& pending, std::size_t adv, std::size_t& out_adv) {
        std::string_view body = text_.substr(body_start, body_end - body_start);
        if (body.find_first_not_of(" \t\n") == std::string_view::npos) return false;
        flush_pending(pending);
        push_node(md::MathInline{std::string(body)});
        out_adv = adv;
        return true;
    }

    bool try_math_dollar(std::size_t i, std::string& pending, std::size_t& adv) {
        std::size_t open = i + 1;
        bool dbl = (open < text_.size() && text_[open] == '$');
        std::string_view close_delim = dbl ? "$$" : "$";
        std::size_t body_start = i + close_delim.size();
        if (body_start >= text_.size()) return false;
        if (!dbl) {
            char nx = text_[body_start];
            if (nx == ' ' || nx == '\t' || nx == '\n') return false;  // "$ " literal
        }
        std::size_t limit = std::min(text_.size(), body_start + 4000);
        std::size_t j = body_start;
        while (j < limit) {
            if (text_[j] == '\\') { j += 2; continue; }  // \$ etc. inside math
            if (dbl) {
                if (j + 1 < text_.size() && text_[j] == '$' && text_[j + 1] == '$') break;
            } else if (text_[j] == '$') {
                break;
            }
            if (text_[j] == '\n' && j + 1 < text_.size() && text_[j + 1] == '\n')
                return false;  // blank line inside inline math: not a span
            ++j;
        }
        if (j >= limit) return false;
        if (!dbl && j + 1 < text_.size() &&
            std::isdigit(static_cast<unsigned char>(text_[j + 1]))) {
            return false;  // "...$5" — glued to a digit → currency, bail
        }
        return make_math(body_start, j, pending, j + close_delim.size(), adv);
    }

    bool try_math_paren(std::size_t i, std::string& pending, std::size_t& adv) {
        // i points at '\', text_[i+1] == '('. Close is the literal "\)".
        std::size_t body_start = i + 2;
        std::size_t limit = std::min(text_.size(), body_start + 4000);
        std::size_t j = body_start;
        while (j + 1 < limit) {
            if (text_[j] == '\\' && text_[j + 1] == ')') break;
            ++j;
        }
        if (j + 1 >= limit) return false;
        return make_math(body_start, j, pending, j + 2, adv);
    }

    // ~~strike~~ / ==highlight== / ~sub~ / ^sup^
    bool try_ext_span(std::size_t i, char c, std::string& pending,
                      std::size_t& adv) {
        bool dbl = (i + 1 < text_.size() && text_[i + 1] == c);
        if (c == '~' && dbl)
            return try_paired(i, "~~", pending, adv, [](std::vector<md::Inline> k) {
                return md::Inline{md::Strike{std::move(k)}}; });
        if (c == '=' && dbl)
            return try_paired(i, "==", pending, adv, [](std::vector<md::Inline> k) {
                return md::Inline{md::Highlight{std::move(k)}}; });
        if (c == '~')
            return try_bounded(i, '~', pending, adv, [](std::vector<md::Inline> k) {
                return md::Inline{md::Sub{std::move(k)}}; });
        if (c == '^')
            return try_bounded(i, '^', pending, adv, [](std::vector<md::Inline> k) {
                return md::Inline{md::Sup{std::move(k)}}; });
        return false;  // single '=' is literal
    }

    // [^label] footnote reference (not the inline-link form [^label](...)).
    bool try_footnote_ref(std::size_t i, std::string& pending, std::size_t& adv) {
        std::size_t s = i + 2;  // past "[^"
        std::size_t limit = std::min(text_.size(), s + 200);
        std::size_t j = s;
        while (j < limit && text_[j] != ']' && text_[j] != '[' && text_[j] != '\n')
            ++j;
        if (j >= limit || text_[j] != ']' || j == s) return false;
        if (j + 1 < text_.size() && text_[j + 1] == '(') return false;
        flush_pending(pending);
        push_node(md::FootnoteRef{std::string(text_.substr(s, j - s))});
        adv = j + 1;
        return true;
    }

    // ── code spans (§6.1) ────────────────────────────────────────────────
    bool try_code_span(std::size_t i, std::string& pending, std::size_t& adv) {
        std::size_t n = 0;
        while (i + n < text_.size() && text_[i + n] == '`') ++n;
        // find closing run of exactly n backticks
        std::size_t j = i + n;
        while (j < text_.size()) {
            if (text_[j] == '`') {
                std::size_t m = 0;
                while (j + m < text_.size() && text_[j + m] == '`') ++m;
                if (m == n) {
                    // content is text_[i+n .. j)
                    std::string content(text_.substr(i + n, j - (i + n)));
                    // line endings → spaces; strip single leading+trailing
                    for (char& ch : content) if (ch == '\n') ch = ' ';
                    if (content.size() >= 2 && content.front() == ' ' &&
                        content.back() == ' ' &&
                        content.find_first_not_of(' ') != std::string::npos) {
                        content = content.substr(1, content.size() - 2);
                    }
                    if (!pending.empty()) { push_text(std::move(pending)); pending.clear(); }
                    push_node(md::Code{std::move(content)});
                    adv = j + m;
                    return true;
                }
                j += m;
            } else {
                ++j;
            }
        }
        return false;
    }

    // ── autolinks + raw HTML (§6.5, §6.6) ────────────────────────────────
    bool try_autolink_or_html(std::size_t i, std::string& pending, std::size_t& adv) {
        // absolute URI autolink: <scheme:...>
        // Bound the closing-'>' search: a valid autolink/email has no spaces
        // or '<' inside and is short in practice, so a runaway '<' with no
        // nearby '>' should fail fast rather than scan to end-of-input on
        // every '<' (the O(n²) adversarial `<<<<...` path). 4000 matches the
        // raw-HTML closer bound (find_html_closer max_dist).
        constexpr std::size_t kMaxAutolink = 4000;
        std::size_t search_end = std::min(text_.size(), i + 1 + kMaxAutolink);
        std::size_t close = text_.substr(0, search_end).find('>', i + 1);
        if (close == std::string_view::npos) return false;
        std::string_view inner = text_.substr(i + 1, close - (i + 1));
        if (inner.find(' ') == std::string_view::npos &&
            inner.find('<') == std::string_view::npos && !inner.empty()) {
            // scheme: letter, then 1-31 of (letter,digit,+,-,.) then ':'
            std::size_t colon = inner.find(':');
            if (colon != std::string_view::npos && colon >= 2 && colon <= 32 &&
                is_ascii_alpha(inner[0])) {
                bool good = true;
                for (std::size_t k = 1; k < colon; ++k) {
                    char ch = inner[k];
                    if (!is_ascii_alnum(ch) && ch != '+' && ch != '-' && ch != '.') {
                        good = false; break;
                    }
                }
                if (good) {
                    if (!pending.empty()) { push_text(std::move(pending)); pending.clear(); }
                    md::Link l;
                    l.url = std::string(inner);
                    l.text = std::string(inner);
                    l.kids.push_back(md::Text{std::string(inner)});
                    push_node(std::move(l));
                    adv = close + 1;
                    return true;
                }
            }
            // email autolink
            if (is_email(inner)) {
                if (!pending.empty()) { push_text(std::move(pending)); pending.clear(); }
                md::Link l;
                l.url = "mailto:" + std::string(inner);
                l.text = std::string(inner);
                l.kids.push_back(md::Text{std::string(inner)});
                push_node(std::move(l));
                adv = close + 1;
                return true;
            }
        }
        // raw inline HTML tag
        std::size_t htlen = scan_html_tag(i);
        if (htlen) {
            if (!pending.empty()) { push_text(std::move(pending)); pending.clear(); }
            push_node(md::RawInline{std::string(text_.substr(i, htlen))});
            adv = i + htlen;
            return true;
        }
        return false;
    }

    [[nodiscard]] static bool is_email(std::string_view s) {
        auto at = s.find('@');
        if (at == std::string_view::npos || at == 0 || at + 1 >= s.size())
            return false;
        for (std::size_t k = 0; k < at; ++k) {
            char c = s[k];
            if (!is_ascii_alnum(c) &&
                std::string_view(".!#$%&'*+/=?^_`{|}~-").find(c) == std::string_view::npos)
                return false;
        }
        // domain: labels of alnum/-, dot separated
        std::string_view dom = s.substr(at + 1);
        std::size_t start = 0;
        for (std::size_t k = 0; k <= dom.size(); ++k) {
            if (k == dom.size() || dom[k] == '.') {
                if (k == start) return false;
                start = k + 1;
            } else {
                char c = dom[k];
                if (!is_ascii_alnum(c) && c != '-') return false;
            }
        }
        return true;
    }

    // Scan a raw HTML tag starting at text_[i]=='<'. Returns length or 0.
    [[nodiscard]] std::size_t scan_html_tag(std::size_t i) const {
        std::string_view s = text_;
        std::size_t n = s.size();
        if (i >= n || s[i] != '<') return 0;
        std::size_t j = i + 1;
        // comment (§6.6, 0.31.2): `<!-->`, `<!--->`, or `<!--` text `-->`
        // where text does not contain `-->`. The two short forms are
        // explicit empty comments.
        if (s.compare(j, 3, "!--") == 0) {
            std::size_t b = j + 3;            // first char after `<!--`
            if (b < n && s[b] == '>') return (b + 1) - i;          // <!-->
            if (s.compare(b, 2, "->") == 0) return (b + 2) - i;     // <!--->
            std::size_t e = s.find("-->", b);
            return e == std::string_view::npos ? 0 : (e + 3) - i;
        }
        // CDATA <![CDATA[ ... ]]>
        if (s.compare(j, 8, "![CDATA[") == 0) {
            std::size_t e = s.find("]]>", j + 8);
            return e == std::string_view::npos ? 0 : (e + 3) - i;
        }
        // processing instruction
        if (j < n && s[j] == '?') {
            std::size_t e = s.find("?>", j + 1);
            return e == std::string_view::npos ? 0 : (e + 2) - i;
        }
        // declaration <!NAME ...>
        if (j < n && s[j] == '!') {
            std::size_t e = s.find('>', j);
            return e == std::string_view::npos ? 0 : (e + 1) - i;
        }
        bool closing = (j < n && s[j] == '/');
        if (closing) ++j;
        if (j >= n || !is_ascii_alpha(s[j])) return 0;
        ++j;
        while (j < n && (is_ascii_alnum(s[j]) || s[j] == '-')) ++j;
        // attributes (only for open tags)
        if (!closing) {
            for (;;) {
                std::size_t ws = j;
                while (j < n && (s[j]==' '||s[j]=='\t'||s[j]=='\n'||s[j]=='\r')) ++j;
                if (j < n && (s[j] == '>' || s[j] == '/')) break;
                if (j == ws) break;  // need whitespace before attr
                if (j >= n || !(is_ascii_alpha(s[j]) || s[j]=='_' || s[j]==':')) break;
                ++j;
                while (j < n && (is_ascii_alnum(s[j]) || s[j]=='_' || s[j]==':' ||
                                 s[j]=='.' || s[j]=='-')) ++j;
                // optional value
                std::size_t save = j;
                while (j < n && (s[j]==' '||s[j]=='\t'||s[j]=='\n'||s[j]=='\r')) ++j;
                if (j < n && s[j] == '=') {
                    ++j;
                    while (j < n && (s[j]==' '||s[j]=='\t'||s[j]=='\n'||s[j]=='\r')) ++j;
                    if (j < n && (s[j] == '"' || s[j] == '\'')) {
                        char q = s[j++];
                        while (j < n && s[j] != q) ++j;
                        if (j >= n) return 0;
                        ++j;
                    } else {
                        std::size_t vs = j;
                        while (j < n && s[j]!=' ' && s[j]!='\t' && s[j]!='\n' &&
                               s[j]!='\r' && s[j]!='"' && s[j]!='\'' && s[j]!='=' &&
                               s[j]!='<' && s[j]!='>' && s[j]!='`') ++j;
                        if (j == vs) return 0;
                    }
                } else {
                    j = save;  // no value
                }
            }
        } else {
            while (j < n && (s[j]==' '||s[j]=='\t'||s[j]=='\n'||s[j]=='\r')) ++j;
        }
        if (j < n && s[j] == '/' && !closing) ++j;
        if (j >= n || s[j] != '>') return 0;
        return (j + 1) - i;
    }

    // ── emphasis delimiter runs (§6.2) ───────────────────────────────────
    void scan_delim_run(std::size_t& i, char c) {
        std::size_t start = i;
        while (i < text_.size() && text_[i] == c) ++i;
        int count = static_cast<int>(i - start);

        // Flanking is defined over the adjacent Unicode code points (§6.2):
        // a run is left-/right-flanking by the whitespace / punctuation class
        // of the char before the run start and after the run end. Start/end
        // of input count as whitespace.
        std::size_t next_len;
        char32_t prev = start > 0 ? decode_cp_before(text_, start) : ' ';
        char32_t next = i < text_.size() ? decode_cp_at(text_, i, next_len) : ' ';

        bool prev_ws = is_unicode_whitespace_cp(prev);
        bool next_ws = is_unicode_whitespace_cp(next);
        bool prev_punct = is_unicode_punct_cp(prev);
        bool next_punct = is_unicode_punct_cp(next);

        bool left_flanking = !next_ws &&
            (!next_punct || prev_ws || prev_punct);
        bool right_flanking = !prev_ws &&
            (!prev_punct || next_ws || next_punct);

        bool can_open, can_close;
        if (c == '_') {
            can_open = left_flanking && (!right_flanking || prev_punct);
            can_close = right_flanking && (!left_flanking || next_punct);
        } else {
            can_open = left_flanking;
            can_close = right_flanking;
        }

        Node n{md::Inline{md::Text{std::string(static_cast<std::size_t>(count), c)}}};
        n.is_delim = true;
        n.delim_char = c;
        n.delim_count = count;
        n.orig_count = static_cast<std::size_t>(count);
        n.can_open = can_open;
        n.can_close = can_close;
        nodes_.push_back(std::move(n));
    }

    // ── link / image resolution (§6.3, §6.4) ─────────────────────────────
    // Called when we hit ']'. Returns true if a link/image was produced
    // (advances i appropriately via member i_after_).
    std::size_t i_after_ = 0;
    bool try_close_link(std::size_t& i) {
        // find the most recent unmatched, active '[' or '!['
        int opener_idx = -1;
        for (int k = static_cast<int>(nodes_.size()) - 1; k >= 0; --k) {
            Node& n = nodes_[k];
            if (n.is_delim && (n.delim_char == '[' || n.delim_char == '!')) {
                opener_idx = k;
                break;
            }
        }
        if (opener_idx < 0) return false;
        Node& opener = nodes_[opener_idx];
        if (!opener.active) {
            opener.is_delim = false;  // deactivate → literal handled later
            opener.delim_char = 0;
            // turn into literal text
            opener.span = md::Inline{md::Text{opener.is_image ? "![" : "["}};
            return false;
        }
        bool is_image = opener.is_image;

        // Now parse what follows ']': inline (url), reference, collapsed,
        // or shortcut.
        std::size_t after = i + 1;
        std::string url, title;
        bool matched = false;
        std::vector<md::Inline> ref_kids_unused;

        // inline link/image: ( ... )
        if (after < text_.size() && text_[after] == '(') {
            std::size_t pos = after + 1;
            if (parse_inline_dest(pos, url, title)) {
                matched = true;
                after = pos;  // pos points one past ')'
            }
        }

        // reference forms. Labels match on the RAW source text between the
        // brackets (backslashes preserved), normalized per §6.3 — NOT the
        // escape-resolved inline text. `[Foo*bar\]]` matches a def whose
        // label is also `Foo*bar\]`; `[foo\!]` does NOT match a def `[foo!]`.
        if (!matched) {
            std::string_view src_label =
                text_.substr(opener.text_pos, i - opener.text_pos);
            // [text][label]
            std::size_t pos = after;
            std::string label;
            if (pos < text_.size() && text_[pos] == '[') {
                std::size_t lp = pos + 1;
                std::string lab;
                bool ok = read_label(lp, lab);
                if (ok) {
                    if (strip(lab).empty()) {
                        // collapsed [text][] → use the (raw) text as label
                        label = std::string(src_label);
                    } else {
                        label = lab;
                    }
                    if (auto* r = lookup(label)) {
                        url = r->url; title = r->title; matched = true;
                        after = lp;
                    }
                }
            } else {
                // shortcut [text]
                label = std::string(src_label);
                if (auto* r = lookup(label)) {
                    url = r->url; title = r->title; matched = true;
                    after = i + 1;
                }
            }
        }

        if (!matched) {
            // failed: opener becomes literal
            opener.is_delim = false;
            opener.delim_char = 0;
            opener.span = md::Inline{md::Text{is_image ? "![" : "["}};
            return false;
        }

        // Build the link/image. Inner nodes are everything after opener.
        // First process emphasis within that range.
        process_emphasis(opener_idx + 1);
        std::vector<md::Inline> kids = collect_inner(opener_idx + 1);
        std::string plain = inline_plain_text(kids);

        // remove inner nodes + opener
        nodes_.resize(static_cast<std::size_t>(opener_idx));

        if (is_image) {
            md::Image im;
            im.url = std::move(url);
            im.title = std::move(title);
            im.alt = plain;
            im.kids = std::move(kids);
            push_node(std::move(im));
        } else {
            md::Link l;
            l.url = std::move(url);
            l.title = std::move(title);
            l.text = plain;
            l.kids = std::move(kids);
            push_node(std::move(l));
            // deactivate earlier '[' openers (no links in links)
            for (auto& nn : nodes_)
                if (nn.is_delim && nn.delim_char == '[') nn.active = false;
        }

        i = after;
        return true;
    }

    // Parse inline destination starting after '(' (pos at first content).
    // On success sets url/title and advances pos past ')'.
    bool parse_inline_dest(std::size_t& pos, std::string& url, std::string& title) {
        std::string_view s = text_;
        // skip ws (incl newlines)
        auto skip = [&] {
            while (pos < s.size() && (s[pos]==' '||s[pos]=='\t'||s[pos]=='\n')) ++pos;
        };
        skip();
        url.clear(); title.clear();
        if (pos < s.size() && s[pos] == ')') { ++pos; return true; }  // empty ()
        // destination
        if (pos < s.size() && s[pos] == '<') {
            ++pos;
            std::string u;
            while (pos < s.size() && s[pos] != '>' && s[pos] != '\n') {
                if (s[pos] == '\\' && pos + 1 < s.size() && is_ascii_punct(s[pos+1])) {
                    u += s[pos+1]; pos += 2; continue;
                }
                u += s[pos++];
            }
            if (pos >= s.size() || s[pos] != '>') return false;
            ++pos;
            url = std::move(u);
        } else {
            int depth = 0;
            std::string u;
            while (pos < s.size()) {
                char c = s[pos];
                if (c == '\\' && pos + 1 < s.size() && is_ascii_punct(s[pos+1])) {
                    u += s[pos+1]; pos += 2; continue;
                }
                if (is_unicode_ws_byte(static_cast<unsigned char>(c))) break;
                if (c == '(') { ++depth; }
                else if (c == ')') { if (depth == 0) break; --depth; }
                u += c;
                ++pos;
            }
            url = std::move(u);
        }
        skip();
        // optional title
        if (pos < s.size() && (s[pos] == '"' || s[pos] == '\'' || s[pos] == '(')) {
            char open = s[pos];
            char close = open == '(' ? ')' : open;
            ++pos;
            std::string t;
            bool ok = false;
            while (pos < s.size()) {
                char c = s[pos];
                if (c == '\\' && pos + 1 < s.size() && is_ascii_punct(s[pos+1])) {
                    t += s[pos+1]; pos += 2; continue;
                }
                if (c == close) { ok = true; ++pos; break; }
                t += c; ++pos;
            }
            if (!ok) return false;
            // decode entities in title
            title = decode_text(t);
            skip();
        }
        if (pos >= s.size() || s[pos] != ')') return false;
        ++pos;
        url = decode_text(url);
        return true;
    }

    bool read_label(std::size_t& pos, std::string& out) {
        std::string_view s = text_;
        std::string lab;
        while (pos < s.size() && s[pos] != ']') {
            if (s[pos] == '[') return false;
            if (s[pos] == '\\' && pos + 1 < s.size()) { lab += s[pos]; lab += s[pos+1]; pos += 2; continue; }
            lab += s[pos++];
        }
        if (pos >= s.size() || s[pos] != ']') return false;
        ++pos;
        out = std::move(lab);
        return true;
    }

    [[nodiscard]] const md::LinkRef* lookup(std::string_view label) const {
        auto key = normalize_label(label);
        auto it = refs_.find(key);
        return it == refs_.end() ? nullptr : &it->second;
    }

    std::vector<md::Inline> collect_inner(int from) {
        std::vector<md::Inline> out;
        for (std::size_t k = static_cast<std::size_t>(from); k < nodes_.size(); ++k) {
            Node& n = nodes_[k];
            if (n.removed) continue;
            if (n.is_delim && n.delim_count > 0) {
                out.push_back(md::Text{std::string(static_cast<std::size_t>(n.delim_count), n.delim_char)});
            } else if (n.is_delim && (n.delim_char == '[' || n.delim_char == '!')) {
                out.push_back(md::Text{n.delim_char == '!' ? "![" : "["});
            } else if (!n.is_delim) {
                out.push_back(std::move(n.span));
            }
        }
        return merge_text(std::move(out));
    }

    static std::string decode_text(std::string_view s) {
        std::string out;
        for (std::size_t i = 0; i < s.size();) {
            if (s[i] == '&') {
                std::string dec;
                std::size_t used = decode_entity(s, i, dec);
                if (used) { out += dec; i += used; continue; }
            }
            out += s[i++];
        }
        return out;
    }

    // ── emphasis processing (§6.2) ───────────────────────────────────────
    void process_emphasis(std::size_t stack_bottom) {
        // walk delimiters from stack_bottom forward looking for closers
        std::size_t i = stack_bottom;
        // openers_bottom (CommonMark §6.2): for each (delim_char, can_open,
        // length%3) class, remember the position below which no opener was
        // found for a previous closer of the same class. A failed closer
        // search never re-scans that exhausted prefix again, turning the
        // pathological O(n²) walk on inputs like `*a*a*a…` into O(n).
        // Index: char (* / _) × can_open(0/1) × len%3 (0/1/2) = 12 slots.
        std::size_t openers_bottom[2][2][3];
        for (auto& a : openers_bottom)
            for (auto& b : a)
                for (auto& v : b) v = stack_bottom;
        auto ob_slot = [&](const Node& n) -> std::size_t& {
            std::size_t ci = (n.delim_char == '_') ? 1 : 0;
            std::size_t co = n.can_open ? 1 : 0;
            return openers_bottom[ci][co][n.orig_count % 3];
        };
        while (i < nodes_.size()) {
            Node& closer = nodes_[i];
            if (!closer.is_delim || !closer.can_close ||
                (closer.delim_char != '*' && closer.delim_char != '_') ||
                closer.delim_count == 0) {
                ++i;
                continue;
            }
            // find matching opener walking back, but not below the recorded
            // bottom for this closer's class.
            std::size_t floor = ob_slot(closer);
            if (floor < stack_bottom) floor = stack_bottom;
            int j = static_cast<int>(i) - 1;
            int found = -1;
            while (j >= static_cast<int>(floor)) {
                Node& op = nodes_[static_cast<std::size_t>(j)];
                if (!op.removed && op.is_delim && op.can_open &&
                    op.delim_char == closer.delim_char && op.delim_count > 0) {
                    // rule of 3
                    bool ok = true;
                    if ((closer.can_open || op.can_close)) {
                        if ((op.orig_count + closer.orig_count) % 3 == 0 &&
                            !(op.orig_count % 3 == 0 && closer.orig_count % 3 == 0))
                            ok = false;
                    }
                    if (ok) { found = j; break; }
                }
                --j;
            }
            if (found < 0) {
                // No opener for this closer's class above the floor: record the
                // floor so future closers of this class skip the dead prefix.
                // A closer that cannot also open can never become an opener,
                // so nothing below it will ever match — advance the bottom to i.
                ob_slot(closer) = i;
                ++i;
                continue;
            }

            Node& op = nodes_[static_cast<std::size_t>(found)];
            int use = (op.delim_count >= 2 && closer.delim_count >= 2) ? 2 : 1;

            // Bound emphasis nesting depth. Real content never nests beyond
            // a few levels; pathological input (`**`×2000…) would build a
            // thousand-deep AST that blows the renderer's recursive layout.
            // Matches the block-nesting cap. Past the cap, delimiters stay
            // literal.
            constexpr int kMaxEmphasisDepth = 16;
            int inner_depth = 0;
            for (std::size_t k = static_cast<std::size_t>(found) + 1; k < i; ++k)
                if (!nodes_[k].removed)
                    inner_depth = std::max(inner_depth, nodes_[k].emph_depth);
            if (inner_depth >= kMaxEmphasisDepth) { ++i; continue; }

            // collect inner nodes between op and closer (skipping tombstones),
            // tombstoning each as we go.
            std::vector<md::Inline> inner;
            for (std::size_t k = static_cast<std::size_t>(found) + 1; k < i; ++k) {
                Node& n = nodes_[k];
                if (n.removed) continue;
                if (n.is_delim && n.delim_count > 0) {
                    inner.push_back(md::Text{std::string(static_cast<std::size_t>(n.delim_count), n.delim_char)});
                } else if (n.is_delim) {
                    if (n.delim_char == '[') inner.push_back(md::Text{"["});
                    else if (n.delim_char == '!') inner.push_back(md::Text{"!["});
                } else {
                    inner.push_back(std::move(n.span));
                }
                n.removed = true;
            }
            inner = merge_text(std::move(inner));

            md::Inline node = (use == 2)
                ? md::Inline{md::Bold{std::move(inner)}}
                : md::Inline{md::Italic{std::move(inner)}};

            // consume delimiters
            op.delim_count -= use;
            closer.delim_count -= use;
            int closer_left = closer.delim_count;  // capture before any insert
                                                   // invalidates the reference

            // Replace the inner range with the emphasis node WITHOUT shifting
            // the vector tail (which made the whole pass O(n²)). We reuse the
            // first inner slot (found+1) for the emphasis node when one exists;
            // it was just tombstoned above. If the inner range is empty
            // (adjacent delimiters, e.g. `****`), fall back to a single insert.
            Node en{std::move(node)};
            en.emph_depth = inner_depth + 1;
            if (i > static_cast<std::size_t>(found) + 1) {
                nodes_[static_cast<std::size_t>(found) + 1] = std::move(en);
            } else {
                nodes_.insert(nodes_.begin() + found + 1, std::move(en));
                ++i;  // everything from the closer onward shifted up by one
            }
            // Re-process the closer in place (it may still have delimiters to
            // pair with an earlier opener). Do NOT advance past it.
            if (closer_left == 0) ++i;
        }
    }

    // merge adjacent Text nodes
    static std::vector<md::Inline> merge_text(std::vector<md::Inline> in) {
        std::vector<md::Inline> out;
        for (auto& n : in) {
            if (auto* t = std::get_if<md::Text>(&n.inner)) {
                if (!out.empty()) {
                    if (auto* p = std::get_if<md::Text>(&out.back().inner)) {
                        p->content += t->content;
                        continue;
                    }
                }
            }
            out.push_back(std::move(n));
        }
        return out;
    }
};

} // namespace

std::vector<md::Inline> parse_inlines(std::string_view text, const RefMap& refs,
                                      Options opts) {
    InlineParser p(refs, opts.extensions);
    auto spans = p.parse(text);
    if (opts.extensions) apply_text_extensions(spans);
    return spans;
}

void apply_text_extensions(std::vector<md::Inline>& spans) {
    // AST-level text transforms (:emoji:, @mentions, bare URLs, entity set).
    // Implemented in text_transform.cpp; recurses into children itself.
    ::maya::md_detail::post_process_text_nodes(spans);
}

std::string inline_plain_text(const std::vector<md::Inline>& spans) {
    std::string out;
    for (const auto& s : spans) {
        std::visit([&](const auto& n) {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, md::Text>) out += n.content;
            else if constexpr (std::is_same_v<T, md::Code>) out += n.content;
            else if constexpr (std::is_same_v<T, md::RawInline>) out += n.content;
            else if constexpr (std::is_same_v<T, md::MathInline>) out += n.latex;
            else if constexpr (std::is_same_v<T, md::HardBreak>) out += '\n';
            else if constexpr (std::is_same_v<T, md::SoftBreak>) out += ' ';
            else if constexpr (requires { n.children; })
                out += inline_plain_text(n.children);
            else if constexpr (requires { n.kids; })
                out += inline_plain_text(n.kids);
        }, s.inner);
    }
    return out;
}

} // namespace maya::md_detail::engine
