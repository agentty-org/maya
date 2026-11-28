// cm_block.cpp — CommonMark block-structure phase (spec §4 + GFM tables).
//
// Implements the spec's line-driven block parser: maintain a stack of open
// blocks; for each input line walk the stack matching continuation rules,
// then try to open new blocks, then add the remaining text to the deepest
// open leaf. After all lines are consumed, finalize the tree, run the
// inline phase over leaf text, and lower to the public md::Document AST.
//
// Reference: https://spec.commonmark.org/0.31.2/#appendix-a-parsing-strategy

#include "maya/widget/agentty/markdown/engine/cm_engine.hpp"
#include "maya/widget/agentty/markdown/engine/cm_util.hpp"
#include "maya/widget/agentty/html.hpp"

#include <algorithm>
#include <cctype>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace maya::md_detail::engine {
namespace {

// ── line model ──────────────────────────────────────────────────────────────
// A Line is a view onto the current source line plus a cursor that tracks
// how far we've consumed (in bytes) and the corresponding column (tabs
// expand to width 4). The block matchers advance the cursor.
struct Line {
    std::string_view s;     // full line content (no trailing '\n')
    std::size_t pos = 0;    // byte cursor
    int col = 0;            // column cursor (tab-aware)

    [[nodiscard]] bool eol() const { return pos >= s.size(); }
    [[nodiscard]] char peek() const { return pos < s.size() ? s[pos] : '\0'; }

    // advance one byte, updating col (tab → next multiple of 4)
    void advance() {
        if (pos >= s.size()) return;
        if (s[pos] == '\t') col += 4 - (col % 4);
        else ++col;
        ++pos;
    }

    // Consume up to n columns of whitespace; returns columns consumed. A tab
    // counts as the columns to the next tab stop (width 4) but is consumed
    // only partially when n falls inside it: the byte cursor stays on the tab
    // and `col` advances, so a later indent()/consume_spaces() recomputes the
    // residual columns from `col % 4` (CommonMark tab handling, §2.2). This is
    // what lets `\t` act as indentation shared across nested list items.
    int consume_spaces(int n) {
        int start = col;
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t') &&
               col - start < n) {
            if (s[pos] == '\t') {
                int to_tab = 4 - (col % 4);
                if (to_tab > n - (col - start)) {
                    col += n - (col - start);  // partial tab: leave pos on it
                    break;
                }
                col += to_tab;
                ++pos;
            } else {
                ++col;
                ++pos;
            }
        }
        return col - start;
    }

    // count leading whitespace columns without consuming
    [[nodiscard]] int indent() const {
        int c = col;
        std::size_t i = pos;
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) {
            if (s[i] == '\t') c += 4 - (c % 4);
            else ++c;
            ++i;
        }
        return c - col;
    }

    // remaining text from cursor
    [[nodiscard]] std::string_view rest() const { return s.substr(pos); }

    // skip all leading whitespace
    void skip_ws() {
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) advance();
    }
};

// ── block-start detectors (spec §4) ─────────────────────────────────────────

[[nodiscard]] bool is_thematic_break(std::string_view line) {
    auto t = strip(line);
    if (t.empty()) return false;
    char c = t[0];
    if (c != '-' && c != '_' && c != '*') return false;
    int count = 0;
    for (char ch : t) {
        if (ch == c) ++count;
        else if (ch == ' ' || ch == '\t') continue;
        else return false;
    }
    return count >= 3;
}

// ATX heading: 1-6 #, then space/eol. Returns level or 0.
[[nodiscard]] int atx_heading_level(std::string_view line, std::size_t& content_start) {
    std::size_t i = 0;
    while (i < line.size() && line[i] == '#') ++i;
    int level = static_cast<int>(i);
    if (level < 1 || level > 6) return 0;
    if (i < line.size() && line[i] != ' ' && line[i] != '\t') return 0;
    content_start = i;
    return level;
}

// Strip ATX trailing #'s (§4.2): a run of # preceded by space, optionally
// followed by spaces.
[[nodiscard]] std::string_view atx_strip(std::string_view content) {
    content = strip(content);
    // remove trailing run of # that's space-separated
    std::size_t end = content.size();
    std::size_t h = end;
    while (h > 0 && content[h - 1] == '#') --h;
    if (h < end && (h == 0 || content[h - 1] == ' ' || content[h - 1] == '\t')) {
        content = rstrip(content.substr(0, h));
    }
    return content;
}

// Setext underline: line of = or - (after a paragraph). Returns 1, 2, or 0.
[[nodiscard]] int setext_level(std::string_view line) {
    auto t = strip(line);
    if (t.empty()) return 0;
    char c = t[0];
    if (c != '=' && c != '-') return 0;
    for (char ch : t)
        if (ch != c) return 0;
    return c == '=' ? 1 : 2;
}

// Fenced code start: ``` or ~~~ (>=3), info string. Returns fence length or 0.
// Extension: a `$$` line opens a DISPLAY-MATH fence — the de-facto convention
// every chat model uses for block equations. It lowers to a CodeBlock whose
// language is "math", which render_block.cpp typesets via the tex engine.
// The fence char is '$' with length 2, so append_to_leaf's generic
// same-char/≥-len close logic matches a bare `$$` line unchanged.
[[nodiscard]] int code_fence(std::string_view line, char& fc, std::string& info,
                             int& indent_out) {
    std::size_t bytes;
    int ind = leading_indent(line, bytes);
    if (ind > 3) return 0;
    auto t = line.substr(bytes);
    if (t.empty()) return 0;
    char c = t[0];
    if (c == '$') {
        // Require the line to be EXACTLY `$$` (optionally trailing ws) so we
        // never swallow inline `$$a+b$$` or a `$5` currency line.
        if (t.size() < 2 || t[1] != '$') return 0;
        std::size_t n = 0;
        while (n < t.size() && t[n] == '$') ++n;
        if (n != 2) return 0;               // `$$$` etc. → not a math fence
        if (!strip(t.substr(n)).empty()) return 0;  // `$$x` is inline, not a fence
        fc = '$';
        info = "math";
        indent_out = ind;
        return 2;
    }
    if (c != '`' && c != '~') return 0;
    std::size_t n = 0;
    while (n < t.size() && t[n] == c) ++n;
    if (n < 3) return 0;
    auto rest = std::string(strip(t.substr(n)));
    // backtick fences can't contain backticks in the info string
    if (c == '`' && rest.find('`') != std::string::npos) return 0;
    fc = c;
    info = rest;
    indent_out = ind;
    return static_cast<int>(n);
}

// Single-line display math: a whole line of the form `$$ … $$` where the
// closing `$$` is on the SAME line and there is non-empty content between.
// Chat models emit this form constantly (`$$x = \frac{-b}{2a}$$` on one
// line) alongside the canonical `$$`-alone-on-its-own-line fence, so we
// accept it as a self-contained display-math block instead of leaking raw
// LaTeX into a paragraph. Returns true and fills `content` (the inner TeX,
// stripped) on a match. Rejected: `$$$…`, `$$` with an empty body, and any
// leftover `$` in the body (`$$a$$b$$` is ambiguous → leave to inline).
[[nodiscard]] bool one_line_math_block(std::string_view line,
                                       std::string& content) {
    std::size_t bytes;
    int ind = leading_indent(line, bytes);
    if (ind > 3) return false;
    auto t = strip(line.substr(bytes));
    // exactly `$$` … `$$`, at least one char between the delimiters
    if (t.size() < 5) return false;                    // `$$` + x + `$$` = 5
    if (t[0] != '$' || t[1] != '$') return false;
    if (t[2] == '$') return false;                     // `$$$…` → not this
    if (t[t.size() - 1] != '$' || t[t.size() - 2] != '$') return false;
    auto body = t.substr(2, t.size() - 4);
    auto inner = strip(body);
    if (inner.empty()) return false;
    // A stray `$` inside means the `$$…$$` pairing is ambiguous (e.g.
    // `$$a$$ text $$b$$`): defer to the inline parser rather than guess.
    if (inner.find('$') != std::string_view::npos) return false;
    content = std::string(inner);
    return true;
}

// Bullet list marker: -, +, * followed by space/eol. Returns marker width
// (1) and sets delim. Ordered: digits(1-9) then . or ) — returns digit count.
struct ListMarker {
    bool ok = false;
    bool ordered = false;
    char delim = '-';
    int  start = 1;
    std::size_t marker_bytes = 0;  // bytes of the marker itself (excl. indent)
};

[[nodiscard]] ListMarker parse_list_marker(std::string_view after_indent) {
    ListMarker m;
    if (after_indent.empty()) return m;
    char c = after_indent[0];
    if (c == '-' || c == '+' || c == '*') {
        // followed by space/tab or end-of-line
        if (after_indent.size() == 1 ||
            after_indent[1] == ' ' || after_indent[1] == '\t') {
            m.ok = true;
            m.ordered = false;
            m.delim = c;
            m.marker_bytes = 1;
        }
        return m;
    }
    if (is_ascii_digit(c)) {
        std::size_t d = 0;
        int num = 0;
        while (d < after_indent.size() && is_ascii_digit(after_indent[d]) && d < 9) {
            num = num * 10 + (after_indent[d] - '0');
            ++d;
        }
        if (d == 0 || d >= after_indent.size()) return m;
        char delim = after_indent[d];
        if (delim != '.' && delim != ')') return m;
        if (d + 1 < after_indent.size() &&
            after_indent[d + 1] != ' ' && after_indent[d + 1] != '\t')
            return m;
        m.ok = true;
        m.ordered = true;
        m.delim = delim;
        m.start = num;
        m.marker_bytes = d + 1;
    }
    return m;
}

// ── HTML block start conditions (§4.6) ───────────────────────────────────

// Is `st` exactly one complete HTML open or closing tag (no trailing text)?
// Used by HTML-block kind 7. Mirrors the inline tag grammar (§6.6).
[[nodiscard]] bool is_complete_tag_line(std::string_view st) {
    std::size_t n = st.size();
    if (n < 3 || st[0] != '<' || st[n - 1] != '>') return false;
    std::size_t j = 1;
    bool closing = (j < n && st[j] == '/');
    if (closing) ++j;
    if (j >= n || !is_ascii_alpha(st[j])) return false;
    ++j;
    while (j < n && (is_ascii_alnum(st[j]) || st[j] == '-')) ++j;
    if (closing) {
        while (j < n && is_space_or_tab(st[j])) ++j;
        return j == n - 1 && st[j] == '>';
    }
    // attributes
    for (;;) {
        std::size_t ws = j;
        while (j < n && is_space_or_tab(st[j])) ++j;
        if (j < n && (st[j] == '>' || st[j] == '/')) break;
        if (j == ws) return false;
        if (j >= n || !(is_ascii_alpha(st[j]) || st[j] == '_' || st[j] == ':'))
            return false;
        ++j;
        while (j < n && (is_ascii_alnum(st[j]) || st[j] == '_' || st[j] == ':' ||
                         st[j] == '.' || st[j] == '-')) ++j;
        std::size_t save = j;
        while (j < n && is_space_or_tab(st[j])) ++j;
        if (j < n && st[j] == '=') {
            ++j;
            while (j < n && is_space_or_tab(st[j])) ++j;
            if (j < n && (st[j] == '"' || st[j] == '\'')) {
                char q = st[j++];
                while (j < n && st[j] != q) ++j;
                if (j >= n) return false;
                ++j;
            } else {
                std::size_t vs = j;
                while (j < n && st[j] != ' ' && st[j] != '\t' && st[j] != '"' &&
                       st[j] != '\'' && st[j] != '=' && st[j] != '<' &&
                       st[j] != '>' && st[j] != '`') ++j;
                if (j == vs) return false;
            }
        } else {
            j = save;
        }
    }
    while (j < n && is_space_or_tab(st[j])) ++j;
    if (j < n && st[j] == '/') ++j;
    return j == n - 1 && st[j] == '>';
}

// Returns the block kind 1..7, or 0 if no HTML block starts here.
[[nodiscard]] int html_block_start(std::string_view t, bool in_paragraph) {
    // t is the line content after up to 3 spaces of indent removed.
    if (t.empty() || t[0] != '<') return 0;
    auto lc = ascii_lower(t);
    auto starts = [&](std::string_view p) { return lc.rfind(p, 0) == 0; };

    // kind 1: <script>, <pre>, <style>, <textarea>
    for (auto tag : {"<script", "<pre", "<style", "<textarea"}) {
        if (starts(tag)) {
            char after = lc.size() > std::string_view(tag).size()
                             ? lc[std::string_view(tag).size()] : '\0';
            if (after == ' ' || after == '\t' || after == '>' || after == '\0')
                return 1;
        }
    }
    // kind 2: <!--
    if (starts("<!--")) return 2;
    // kind 3: <?
    if (starts("<?")) return 3;
    // kind 4: <!LETTER
    if (t.size() >= 2 && t[1] == '!' && t.size() >= 3 && is_ascii_alpha(t[2]))
        return 4;
    // kind 5: <![CDATA[
    if (starts("<![cdata[")) return 5;
    // kind 6: block-level tag name
    static const char* kBlockTags[] = {
        "address","article","aside","base","basefont","blockquote","body",
        "caption","center","col","colgroup","dd","details","dialog","dir",
        "div","dl","dt","fieldset","figcaption","figure","footer","form",
        "frame","frameset","h1","h2","h3","h4","h5","h6","head","header",
        "hr","html","iframe","legend","li","link","main","menu","menuitem",
        "nav","noframes","ol","optgroup","option","p","param","search",
        "section","summary","table","tbody","td","tfoot","th","thead",
        "title","tr","track","ul"};
    {
        std::size_t i = 1;
        bool closing = (i < t.size() && t[i] == '/');
        if (closing) ++i;
        std::size_t ns = i;
        while (i < lc.size() && (is_ascii_alnum(lc[i]) || lc[i] == '-')) ++i;
        std::string name = lc.substr(ns, i - ns);
        char after = i < t.size() ? t[i] : '\0';
        bool boundary = (after == ' ' || after == '\t' || after == '\0' ||
                         after == '>' ||
                         (after == '/' && i + 1 < t.size() && t[i + 1] == '>'));
        if (!name.empty() && boundary) {
            for (auto* bt : kBlockTags)
                if (name == bt) return 6;
        }
    }
    // kind 7: complete open or closing tag on its own line, not pre/script/
    // style/textarea — only when NOT interrupting a paragraph.
    //
    // DIVERGENCE FROM STRICT COMMONMARK (deliberate): the spec's kind-7 start
    // condition accepts ANY tag name. We additionally require the name to be
    // a tag the HTML renderer actually knows (html::is_known_tag). Chat models
    // constantly emit angle-bracket PSEUDO-TAGS — <shell>, <thinking>,
    // <tool_call>, <system-reminder> — that are not HTML. Under strict kind 7
    // a lone `<shell>` opens a raw HTML block that swallows every following
    // line up to the next blank line; the html widget then flow-collapses that
    // content, destroying any markdown list inside AND shrinking the block on
    // commit (the "bullets appear while streaming, then vanish and snap up"
    // bug). Treating unknown tags as ordinary text lets them — and the
    // markdown beneath them — render normally. Known tags keep full HTML-block
    // behaviour, so real inline/other tags on their own line still work.
    if (!in_paragraph) {
        auto st = strip(t);
        if (is_complete_tag_line(st)) {
            auto lcs = ascii_lower(st);
            for (auto raw : {"<script","<pre","<style","<textarea"})
                if (lcs.rfind(raw, 0) == 0) return 0;
            // Extract the tag name: skip '<' and an optional '/', then read
            // the run of name characters. Reject unknown (pseudo) tags.
            std::size_t p = 1;                       // past '<'
            if (p < lcs.size() && lcs[p] == '/') ++p; // closing tag
            std::size_t ns = p;
            while (p < lcs.size()
                   && ((lcs[p] >= 'a' && lcs[p] <= 'z')
                       || (lcs[p] >= '0' && lcs[p] <= '9')
                       || lcs[p] == '-'))
                ++p;
            // substr() on the std::string would return a TEMPORARY whose
            // buffer dies at the end of this full-expression, leaving `name`
            // dangling before is_known_tag() ever reads it (-Wdangling-gsl).
            // View `lcs` first so substr() is the string_view overload: a
            // borrow of the live local, no allocation, no temporary.
            std::string_view name = std::string_view{lcs}.substr(ns, p - ns);
            if (!html::is_known_tag(name)) return 0;
            return 7;
        }
    }
    return 0;
}

// Does the HTML block of `kind` end on this line (or contain its end)?
[[nodiscard]] bool html_block_ends(int kind, std::string_view line) {
    auto lc = ascii_lower(line);
    switch (kind) {
        case 1:
            return lc.find("</script>") != std::string::npos ||
                   lc.find("</pre>") != std::string::npos ||
                   lc.find("</style>") != std::string::npos ||
                   lc.find("</textarea>") != std::string::npos;
        case 2: return line.find("-->") != std::string::npos;
        case 3: return line.find("?>") != std::string::npos;
        case 4: return line.find('>') != std::string::npos;
        case 5: return line.find("]]>") != std::string::npos;
        case 6:
        case 7: return is_blank(line);  // ended by blank line
    }
    return false;
}

// ── GFM table detection ──────────────────────────────────────────────────
[[nodiscard]] bool looks_like_table_delim(std::string_view line) {
    auto t = strip(line);
    if (t.empty()) return false;
    bool seen_dash = false;
    for (char c : t) {
        if (c == '-') seen_dash = true;
        else if (c != '|' && c != ':' && c != ' ' && c != '\t') return false;
    }
    return seen_dash;
}

std::vector<std::string_view> split_pipe_cells(std::string_view row) {
    std::vector<std::string_view> cells;
    auto t = strip(row);
    if (!t.empty() && t.front() == '|') t.remove_prefix(1);
    if (!t.empty() && t.back() == '|') t.remove_suffix(1);
    std::size_t start = 0;
    bool esc = false;
    for (std::size_t i = 0; i < t.size(); ++i) {
        if (esc) { esc = false; continue; }
        if (t[i] == '\\') { esc = true; continue; }
        if (t[i] == '|') {
            cells.push_back(strip(t.substr(start, i - start)));
            start = i + 1;
        }
    }
    cells.push_back(strip(t.substr(start)));
    return cells;
}

std::vector<md::TableAlign> parse_aligns(std::string_view delim) {
    std::vector<md::TableAlign> a;
    for (auto cell : split_pipe_cells(delim)) {
        auto c = strip(cell);
        bool left = !c.empty() && c.front() == ':';
        bool right = !c.empty() && c.back() == ':';
        if (left && right) a.push_back(md::TableAlign::Center);
        else if (right) a.push_back(md::TableAlign::Right);
        else if (left) a.push_back(md::TableAlign::Left);
        else a.push_back(md::TableAlign::Left);
    }
    return a;
}

// ════════════════════════════════════════════════════════════════════════
// The parser object
// ════════════════════════════════════════════════════════════════════════
class BlockParser {
public:
    md::Document run(std::string_view source, bool ext) {
        ext_ = ext;
        doc_ = std::make_unique<CMBlock>(BlockType::Document);

        // Normalize: split into lines. Strip a single trailing newline; keep
        // internal structure. Tabs are handled by the column model.
        std::size_t i = 0;
        std::string src;
        // Replace NUL with U+FFFD per spec §2.3 (insecure characters). Must
        // emit the FULL 3-byte replacement (EF BF BD); writing a lone 0xEF
        // is a broken UTF-8 lead byte that swallows the next two bytes.
        if (source.find('\0') == std::string_view::npos) {
            src.assign(source);
        } else {
            src.reserve(source.size());
            for (char c : source) {
                if (c == '\0') src.append("\xEF\xBF\xBD");
                else            src.push_back(c);
            }
        }
        while (i <= src.size()) {
            std::size_t nl = src.find('\n', i);
            std::string_view line;
            if (nl == std::string::npos) {
                // Final unterminated line. When the source ends with '\n',
                // i == src.size() and the slice is EMPTY — that is NOT a
                // line: a trailing newline TERMINATES the last line (cmark
                // §2.1), it doesn't open an empty one. Processing it fed a
                // phantom blank content line into any still-open leaf — an
                // UNCLOSED code fence gained a trailing "\n\n" that the
                // CodeFence emit (which pops exactly one '\n') couldn't
                // fully strip, rendering a phantom blank row inside the
                // border. During the streaming reveal (line-granular clip —
                // every intermediate extent ends in '\n') that blank row
                // appeared and vanished at every line step: the 1-row
                // height oscillation st_reveal_fx_height_monotonic catches.
                if (i < src.size()) {
                    line = std::string_view(src).substr(i);
                    process_line(line);
                }
                break;
            } else {
                line = std::string_view(src).substr(i, nl - i);
                // strip trailing \r
                if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
                process_line(line);
                i = nl + 1;
            }
        }
        finalize(doc_.get());
        // phase 2: inlines
        parse_inlines_tree(doc_.get());
        // lower
        md::Document out;
        out.ref_defs = refs_;
        for (auto& ch : doc_->children) lower_block(*ch, out.blocks);
        return out;
    }

private:
    std::unique_ptr<CMBlock> doc_;
    RefMap refs_;
    std::vector<CMBlock*> open_;  // path of currently-open blocks (doc first)
    int line_number_ = 0;         // 1-based current line
    CMBlock* cur_container_ = nullptr;  // block this line's content lands in
    bool ext_ = false;            // GFM/maya extensions enabled

    // ── line processing ──────────────────────────────────────────────────
    void process_line(std::string_view raw) {
        ++line_number_;
        Line line{raw};
        rebuild_open_path();

        // Walk the open CONTAINER blocks (document/blockquote/list/item),
        // matching their continuation markers. Stop at the first that fails
        // or at the leaf. open_ holds [doc, ...containers..., maybe-leaf].
        std::size_t idx = 1;
        CMBlock* container = doc_.get();          // deepest matched container
        CMBlock* open_block_parent = doc_.get();  // parent to attach new blocks
        bool all_matched = true;
        for (; idx < open_.size(); ++idx) {
            CMBlock* blk = open_[idx];
            if (is_leaf(blk->type)) break;  // leaf handled below
            Line probe = line;
            if (matches_continuation(blk, probe)) {
                line = probe;
                container = blk;
                // New blocks attach to the deepest matched ITEM (or quote/
                // doc); a List is a passthrough — its items are the real
                // containers, so new sibling markers attach to the List's
                // PARENT and the marker code re-finds the list.
                if (blk->type != BlockType::List) open_block_parent = blk;
            } else {
                all_matched = false;
                break;
            }
        }

        // "Blank" is measured after consuming the matched container prefixes
        // (a `>`-only line inside a quote is blank for tight/loose purposes).
        bool blank = is_blank(line.rest());

        // The deepest open block (could be a leaf).
        CMBlock* leaf = open_.back();
        bool leaf_is_open_leaf = is_leaf(leaf->type) && leaf->open;

        // cur_container_ mirrors cmark's `container`: the block this line's
        // content lands in. It defaults to the deepest matched container and
        // is advanced to any leaf/container opened below (open_child updates
        // it). apply_last_line_blank() consults it once per line.
        cur_container_ = container;

        // A still-matched open code/html leaf swallows the line verbatim.
        if (leaf_is_open_leaf && all_matched &&
            (leaf->type == BlockType::CodeFence ||
             leaf->type == BlockType::CodeIndented ||
             leaf->type == BlockType::HtmlBlock)) {
            append_to_leaf(leaf, line);
            cur_container_ = leaf;
            apply_last_line_blank(cur_container_, blank);
            return;
        }

        // Lazy continuation (§5.1/§4.8): a paragraph continuation line can
        // continue an open paragraph even if some intervening container's
        // continuation marker is missing — as long as the line is not blank
        // and does not itself start a new block.
        if (!all_matched && leaf_is_open_leaf &&
            leaf->type == BlockType::Paragraph &&
            !blank &&
            !line_starts_new_block(line.rest()) &&
            !continues_open_list(line.rest())) {
            // Append the raw line rest (not strip()'d) so a trailing
            // two-space hard-break marker survives — matching the normal
            // paragraph-continuation path in open_leaf (which appends the
            // unstripped content). Phase-2 inline parsing strips the whole
            // paragraph's outer whitespace, so leading indent is handled
            // there; stripping here silently dropped hard breaks.
            leaf->text += std::string(line.rest());
            leaf->text += '\n';
            apply_last_line_blank(cur_container_, blank);
            return;
        }

        try_open_new_blocks(open_block_parent, line);
        apply_last_line_blank(cur_container_, blank);
    }

    // cmark add_text_to_container (§5.3 loose/tight bookkeeping): record where
    // a blank line falls so list finalization can tell tight from loose.
    //   * a blank line marks the deepest matched container's last child;
    //   * the container itself is marked blank unless it is a type that does
    //     not count blanks (block quote, heading, thematic break, fenced
    //     code) or a just-opened empty list item;
    //   * every ancestor is cleared — a blank only "belongs" to the deepest
    //     block, so nested blanks never leak up to an outer list.
    void apply_last_line_blank(CMBlock* container, bool blank) {
        if (!container) return;
        if (blank && !container->children.empty())
            container->children.back()->last_line_blank = true;

        bool llb = blank &&
            container->type != BlockType::BlockQuote &&
            container->type != BlockType::Heading &&
            container->type != BlockType::ThematicBreak &&
            container->type != BlockType::CodeFence &&
            !(container->type == BlockType::Item &&
              container->children.empty() &&
              container->start_line == line_number_);
        container->last_line_blank = llb;

        for (CMBlock* a = container->parent; a; a = a->parent)
            a->last_line_blank = false;
    }

    [[nodiscard]] static bool is_leaf(BlockType t) {
        switch (t) {
            case BlockType::Document:
            case BlockType::BlockQuote:
            case BlockType::List:
            case BlockType::Item:
                return false;
            default:
                return true;
        }
    }

    // Does `content` start a list marker while a list is already open in the
    // current path? Such a marker relates to that list — it continues it (same
    // kind) or terminates it and starts a new one (a change of bullet char or
    // ordered delimiter, §5.3) — so it opens block structure rather than
    // lazily continuing the deepest paragraph. Either way it is NOT paragraph
    // continuation text. (A marker with no open list is governed by the
    // paragraph-interruption rule in line_starts_new_block instead.)
    [[nodiscard]] bool continues_open_list(std::string_view content) {
        std::size_t bytes;
        int ind = leading_indent(content, bytes);
        if (ind > 3) return false;
        auto m = parse_list_marker(content.substr(bytes));
        if (!m.ok) return false;
        for (CMBlock* b : open_) {
            if (b->type == BlockType::List && b->open) return true;
        }
        return false;
    }

    // Would `content` begin a new block (so it can't lazily continue a
    // paragraph)? Conservative per §5.1: headings, fences, thematic breaks,
    // blockquotes, and HTML blocks interrupt; bullet markers that aren't
    // empty interrupt; ordered 1. interrupts.
    [[nodiscard]] static bool line_starts_new_block(std::string_view content) {
        std::size_t bytes;
        int ind = leading_indent(content, bytes);
        if (ind > 3) return false;  // indented code can't interrupt a para
        std::string_view t = content.substr(bytes);
        if (t.empty()) return false;
        std::size_t cs;
        if (atx_heading_level(t, cs)) return true;
        if (is_thematic_break(content)) return true;
        char fc; std::string info; int fi;
        if (code_fence(content, fc, info, fi)) return true;
        if (t[0] == '>') return true;
        if (html_block_start(t, /*in_paragraph=*/true)) return true;
        // list marker that interrupts a paragraph: non-empty bullet, or `1.`
        auto m = parse_list_marker(t);
        if (m.ok) {
            auto after = strip(t.substr(m.marker_bytes));
            if (!after.empty()) {
                if (!m.ordered) return true;
                if (m.ordered && m.start == 1) return true;
            }
        }
        return false;
    }

    // Reconstruct open_ from the tree (path of rightmost open descendants).
    void rebuild_open_path() {
        open_.clear();
        CMBlock* cur = doc_.get();
        open_.push_back(cur);
        while (true) {
            if (cur->children.empty()) break;
            CMBlock* last = cur->children.back().get();
            if (!last->open) break;
            open_.push_back(last);
            if (is_leaf(last->type)) break;
            cur = last;
        }
    }

    [[nodiscard]] CMBlock* deepest_open_leaf() {
        return open_.empty() ? nullptr : open_.back();
    }

    // Can `blk` continue with this line? Advances the line cursor on success.
    [[nodiscard]] bool matches_continuation(CMBlock* blk, Line& line) {
        switch (blk->type) {
            case BlockType::BlockQuote: {
                int ind = line.indent();
                if (ind > 3) return false;
                line.consume_spaces(3);
                if (line.peek() != '>') return false;
                line.advance();  // '>'
                if (line.peek() == ' ' || line.peek() == '\t') line.advance();
                return true;
            }
            case BlockType::Item: {
                if (line.eol() || is_blank(line.rest())) {
                    // A blank line continues the item only if the item already
                    // has content. An empty item (marker followed by nothing)
                    // is closed by a blank line — its content cannot begin
                    // after the opening blank (§5.2, cmark parse_node_item_
                    // prefix). `-\n\n  foo` ⇒ empty <li> then a separate <p>.
                    if (blk->children.empty()) { blk->open = false; return false; }
                    return true;
                }
                int ind = line.indent();
                if (ind >= blk->item_indent) {
                    line.consume_spaces(blk->item_indent);
                    return true;
                }
                return false;
            }
            case BlockType::List:
                return true;  // lists match; items handle indentation
            case BlockType::Document:
                return true;
            case BlockType::Paragraph:
            case BlockType::Heading:
            case BlockType::CodeFence:
            case BlockType::CodeIndented:
            case BlockType::HtmlBlock:
            case BlockType::ThematicBreak:
            case BlockType::Table:
                return true;  // leaves: continuation handled by phase 2
        }
        return false;
    }

    void try_open_new_blocks(CMBlock* container, Line& line) {
        // Current nesting depth = open-container prefix already walked plus
        // any containers we open in this loop. Bounding it keeps pathological
        // deep nesting (`>>>>...` x1000) from blowing the renderer's
        // recursive layout; CommonMark inputs never need more than a few.
        int depth = container_depth(container);
        for (;;) {
            if (depth >= kMaxNestDepth) break;
            int ind = line.indent();

            // indented code block (only if not in a paragraph continuation)
            if (ind >= 4 && !para_can_lazily_continue(container) &&
                !is_blank(line.rest())) {
                // not when it would interrupt list marker context handled above
                line.consume_spaces(4);
                CMBlock* code = open_child(container, BlockType::CodeIndented);
                code->text = std::string(line.rest());
                code->text += '\n';
                return;
            }

            // blockquote
            if (ind <= 3) {
                Line probe = line;
                probe.consume_spaces(3);
                if (probe.peek() == '>') {
                    probe.advance();
                    if (probe.peek() == ' ' || probe.peek() == '\t') probe.advance();
                    line = probe;
                    container = open_child(container, BlockType::BlockQuote);
                    ++depth;
                    continue;
                }
            }

            // list item
            if (ind <= 3) {
                // Thematic break takes precedence over a list-item marker
                // (`- - -` is an <hr>, not three nested lists). A '-' line
                // that's a valid thematic break is handled by open_leaf.
                if (is_thematic_break(line.rest()) &&
                    !(para_can_lazily_continue(container) &&
                      setext_level(line.rest()) == 2)) {
                    break;
                }
                Line probe = line;
                int entry_col = line.col;  // cursor col after parent prefixes
                int marker_col = probe.col + probe.indent();
                probe.consume_spaces(3);
                std::size_t before = probe.pos;
                auto m = parse_list_marker(probe.rest());
                if (m.ok) {
                    bool interrupting = para_can_lazily_continue(container);
                    // advance past marker
                    for (std::size_t k = 0; k < m.marker_bytes; ++k) probe.advance();
                    int after_marker_col = probe.col;
                    int spaces = probe.indent();
                    bool blank_item = strip(probe.rest()).empty();

                    bool reject = false;
                    if (interrupting) {
                        // ordered lists interrupting a paragraph must start at 1
                        if (m.ordered && m.start != 1) reject = true;
                        // an empty list item can't interrupt a paragraph
                        if (blank_item) reject = true;
                    }

                    if (!reject) {
                        // Content indent is RELATIVE to the cursor at entry
                        // (parent container prefixes already consumed), so
                        // matches_continuation — which measures line.indent()
                        // from the same cursor after re-consuming parent
                        // prefixes — lines up for nested items.
                        int marker_width = after_marker_col - entry_col;
                        int content_indent;
                        if (blank_item || spaces == 0 || spaces > 4) {
                            content_indent = marker_width + 1;
                        } else {
                            content_indent = marker_width + spaces;
                        }

                        CMBlock* list = nullptr;
                        if (!container->children.empty() &&
                            container->children.back()->open &&
                            container->children.back()->type == BlockType::List &&
                            list_compatible(*container->children.back(), m)) {
                            list = container->children.back().get();
                        } else {
                            list = open_child(container, BlockType::List);
                            list->ordered = m.ordered;
                            list->list_delim = m.delim;
                            list->start_num = m.start;
                        }
                        CMBlock* item = open_child(list, BlockType::Item);
                        item->item_indent = content_indent;
                        item->item_marker_offset = marker_col;
                        item->ordered = m.ordered;
                        if (!blank_item) {
                            if (spaces > 4) probe.consume_spaces(1);
                            else probe.consume_spaces(spaces);
                        }
                        line = probe;
                        container = item;
                        ++depth;
                        (void)before;
                        continue;
                    }
                    (void)before;
                }
            }
            break;
        }

        open_leaf(container, line);
    }

    [[nodiscard]] bool list_compatible(const CMBlock& list, const ListMarker& m) {
        return list.ordered == m.ordered && list.list_delim == m.delim;
    }

    // Cap container nesting depth. The terminal renderer's recursive block
    // layout (md_block_to_element) is the binding constraint: it was tuned
    // for the legacy parser's depth-8 cap, and far-deeper nesting (e.g.
    // `>>>>...` x1000) makes its per-level layout pathological. Real
    // content never approaches this; past the cap, surplus markers fall
    // into the leaf as literal text.
    static constexpr int kMaxNestDepth = 8;

    // Depth of `blk` from the document root (number of ancestors), computed
    // from the live open path rebuilt at the start of each line.
    [[nodiscard]] int container_depth(CMBlock* blk) const {
        int d = 0;
        for (CMBlock* b : open_) { ++d; if (b == blk) break; }
        return d;
    }

    // Whether `container` currently ends in an open paragraph that a list
    // marker / etc. would be interrupting.
    [[nodiscard]] bool para_can_lazily_continue(CMBlock* container) {
        if (container->children.empty()) return false;
        CMBlock* last = container->children.back().get();
        return last->open && last->type == BlockType::Paragraph;
    }

    // Open a leaf block for the remaining line content.
    void open_leaf(CMBlock* container, Line& line) {
        std::string_view rest_full = line.s.substr(line.pos);
        // strip the leading whitespace we logically consumed but keep for code
        std::string_view content = line.rest();

        if (is_blank(content)) {
            // blank line: close paragraph/table and any unmatched quote.
            // (last_line_blank bookkeeping is done once per line in
            // apply_last_line_blank, using the deepest matched container.)
            handle_blank(container);
            return;
        }

        std::size_t bytes;
        int ind = leading_indent(content, bytes);
        std::string_view t = content.substr(bytes);

        // thematic break (before list interpretation already done; but '-'
        // could be setext — handled in paragraph branch).
        if (ind <= 3 && is_thematic_break(content)) {
            // a setext-2 underline only if there's an open paragraph and the
            // line is all '-'. That's handled in the paragraph path below.
            if (!(para_open(container) && setext_level(content) == 2)) {
                open_child(container, BlockType::ThematicBreak);
                return;
            }
        }

        // ATX heading
        if (ind <= 3) {
            std::size_t cs;
            int lvl = atx_heading_level(t, cs);
            if (lvl) {
                CMBlock* h = open_child(container, BlockType::Heading);
                h->level = lvl;
                auto body = atx_strip(t.substr(cs));
                h->text = std::string(body);
                h->open = false;
                return;
            }
        }

        // fenced code
        {
            char fc;
            std::string info;
            int find;
            int flen = code_fence(content, fc, info, find);
            // The `$$` display-math fence is an extension: ignore it (fall
            // through to paragraph) when extensions are off so plain
            // CommonMark keeps `$$` as literal text.
            if (flen && fc == '$' && !ext_) flen = 0;
            if (flen) {
                CMBlock* code = open_child(container, BlockType::CodeFence);
                code->fence_char = fc;
                code->fence_len = flen;
                code->fence_indent = find;
                code->info = info;
                return;
            }
            // Single-line `$$ … $$` display math (ext only): emit a
            // self-closing math CodeFence carrying the inner TeX. This is
            // the form chat models emit inline-on-one-line; without this it
            // would fall through to a paragraph and render as raw LaTeX.
            std::string math_body;
            if (ext_ && one_line_math_block(content, math_body)) {
                CMBlock* code = open_child(container, BlockType::CodeFence);
                code->fence_char = '$';
                code->fence_len = 2;
                code->fence_indent = 0;
                code->info = "math";
                code->text = std::move(math_body);
                code->text += '\n';   // lower_block trims the trailing NL
                code->open = false;   // complete block, no closing line
                return;
            }
        }

        // HTML block
        if (ind <= 3) {
            int kind = html_block_start(t, para_open(container));
            if (kind) {
                CMBlock* h = open_child(container, BlockType::HtmlBlock);
                h->html_block_kind = kind;
                h->text = std::string(content);
                h->text += '\n';
                if (html_block_ends(kind, content)) h->open = false;
                return;
            }
        }

        // setext heading: an open paragraph followed by = or - underline
        if (ind <= 3 && para_open(container)) {
            int sl = setext_level(content);
            if (sl) {
                CMBlock* para = container->children.back().get();
                // Leading reference definitions are NOT part of the setext
                // heading text — peel them off first (spec §4.3 example 215).
                extract_ref_defs(para);
                if (!strip(para->text).empty()) {
                    para->type = BlockType::Heading;
                    para->level = sl;
                    para->open = false;
                    return;
                }
                // paragraph was entirely ref-defs: the underline is not a
                // heading. A '-' line falls through to thematic break; a '='
                // line becomes its own paragraph text.
                para->open = false;
                if (sl == 2 && is_thematic_break(content)) {
                    open_child(container, BlockType::ThematicBreak);
                    return;
                }
                CMBlock* p2 = open_child(container, BlockType::Paragraph);
                p2->text = std::string(strip(content));
                p2->text += '\n';
                return;
            }
        }

        // GFM table: an open paragraph whose single accumulated line is a
        // pipe row, and THIS line is a delimiter row.
        if (para_open(container) && looks_like_table_delim(content)) {
            CMBlock* para = container->children.back().get();
            auto header_line = strip(std::string_view(para->text));
            // single line paragraph
            if (header_line.find('\n') == std::string_view::npos &&
                header_line.find('|') != std::string_view::npos) {
                auto haligns = parse_aligns(content);
                auto hcells = split_pipe_cells(header_line);
                if (haligns.size() == hcells.size()) {
                    // convert paragraph into table
                    para->type = BlockType::Table;
                    para->aligns = haligns;
                    para->info = std::string(header_line);  // stash header row
                    para->text.clear();
                    para->open = true;
                    return;
                }
            }
        }

        // append to / continue an open table
        if (table_open(container)) {
            CMBlock* tbl = container->children.back().get();
            if (strip(content).find('|') != std::string_view::npos ||
                !strip(content).empty()) {
                // a row line
                if (is_blank(content)) { tbl->open = false; }
                else { tbl->text += std::string(content); tbl->text += '\n'; }
                return;
            }
        }

        // lazy paragraph continuation
        if (para_open(container)) {
            CMBlock* para = container->children.back().get();
            para->text += std::string(content);
            para->text += '\n';
            return;
        }

        // new paragraph
        CMBlock* para = open_child(container, BlockType::Paragraph);
        para->text = std::string(content);
        para->text += '\n';
    }

    void append_to_leaf(CMBlock* leaf, Line& line) {
        // Work relative to the line cursor (container prefixes such as list
        // indent / blockquote markers have already been consumed).
        std::string_view rest = line.rest();
        if (leaf->type == BlockType::CodeFence) {
            // closing fence?
            std::size_t bytes;
            int ind = leading_indent(rest, bytes);
            auto t = rest.substr(bytes);
            if (ind <= 3 && !t.empty() && t[0] == leaf->fence_char) {
                std::size_t n = 0;
                while (n < t.size() && t[n] == leaf->fence_char) ++n;
                if (static_cast<int>(n) >= leaf->fence_len &&
                    strip(t.substr(n)).empty()) {
                    leaf->open = false;
                    return;
                }
            }
            // content line: strip up to fence_indent leading columns.
            // Tabs advance to the next 4-column tab stop (spec §2.2), not a
            // flat 4 — a tab at column 1 spans 3 columns, at column 0 it
            // spans 4. Counting it flat over-consumed indentation when the
            // fence opener sat at a non-zero column.
            int strip_n = leaf->fence_indent;
            std::size_t k = 0;
            int removed = 0;
            while (k < rest.size() && removed < strip_n &&
                   (rest[k] == ' ' || rest[k] == '\t')) {
                removed += (rest[k] == '\t') ? (4 - (removed % 4)) : 1;
                ++k;
            }
            leaf->text += std::string(rest.substr(k));
            leaf->text += '\n';
            return;
        }
        if (leaf->type == BlockType::CodeIndented) {
            if (is_blank(rest)) {
                leaf->text += '\n';
                return;
            }
            std::size_t bytes;
            int ind = leading_indent(rest, bytes);
            if (ind < 4) {
                // dedented non-blank line: close the code block and
                // reprocess this line from the enclosing container.
                leaf->open = false;
                process_after_close(line);
                return;
            }
            // remove 4 cols
            Line l = line;
            l.consume_spaces(4);
            leaf->text += std::string(l.rest());
            leaf->text += '\n';
            return;
        }
        if (leaf->type == BlockType::HtmlBlock) {
            leaf->text += std::string(rest);
            leaf->text += '\n';
            if (html_block_ends(leaf->html_block_kind, rest))
                leaf->open = false;
            return;
        }
    }

    // When an indented code block ends because indentation dropped, the line
    // needs to be reprocessed from the enclosing container. `line` still
    // carries its consumed container prefix in the cursor.
    void process_after_close(Line& line) {
        // Re-open the new-block machinery starting at the current cursor;
        // open_block_parent here is the deepest open container (the item /
        // blockquote / doc) whose prefix we already consumed.
        rebuild_open_path();
        CMBlock* parent = doc_.get();
        for (std::size_t idx = 1; idx < open_.size(); ++idx) {
            if (is_leaf(open_[idx]->type)) break;
            if (open_[idx]->type != BlockType::List) parent = open_[idx];
        }
        try_open_new_blocks(parent, line);
    }

    void handle_blank(CMBlock* container) {
        // Close an open paragraph.
        if (!container->children.empty()) {
            CMBlock* last = container->children.back().get();
            if (last->type == BlockType::Paragraph) last->open = false;
            if (last->type == BlockType::Table) last->open = false;
        }
        // A blank line ends an open block quote ONLY when the blank line
        // did not itself carry the quote's `>` marker (i.e. the quote did
        // not match this line). `container` is the deepest block that DID
        // match; any open block quote deeper than it is unmatched, so a
        // following `>` opens a fresh quote (spec §5.1). A `>`-marked blank
        // line keeps its quote open (one quote, multiple paragraphs).
        bool past_container = false;
        for (CMBlock* b : open_) {
            if (b == container) { past_container = true; continue; }
            if (past_container && b->type == BlockType::BlockQuote)
                b->open = false;
        }
    }

    // ── helpers ──────────────────────────────────────────────────────────
    [[nodiscard]] bool para_open(CMBlock* container) {
        return !container->children.empty() &&
               container->children.back()->open &&
               container->children.back()->type == BlockType::Paragraph;
    }
    [[nodiscard]] bool table_open(CMBlock* container) {
        return !container->children.empty() &&
               container->children.back()->open &&
               container->children.back()->type == BlockType::Table;
    }

    CMBlock* open_child(CMBlock* parent, BlockType t) {
        // close any open paragraph sibling first
        if (!parent->children.empty()) {
            CMBlock* last = parent->children.back().get();
            if (last->open && (last->type == BlockType::Paragraph ||
                               last->type == BlockType::Heading))
                last->open = false;
        }
        auto blk = std::make_unique<CMBlock>(t);
        CMBlock* raw = blk.get();
        raw->parent = parent;
        raw->start_line = line_number_;
        parent->children.push_back(std::move(blk));
        cur_container_ = raw;  // this line's content now lands here
        return raw;
    }

    // ── finalize: close blocks, collect ref defs, compute tight/loose ────
    void finalize(CMBlock* blk) {
        blk->open = false;
        for (auto& ch : blk->children) finalize(ch.get());

        if (blk->type == BlockType::Paragraph) {
            // extract leading reference definitions
            extract_ref_defs(blk);
        }
        if (blk->type == BlockType::List) compute_list_tightness(blk);
    }

    // cmark S_ends_with_blank_line: descend the last-child chain through
    // lists and items (memoized) and report the blank flag of the block we
    // bottom out at. A blank that ends a nested last child propagates to its
    // containing item, but a blank that ends a non-last child does not.
    [[nodiscard]] static bool ends_with_blank_line(CMBlock* node) {
        while (!node->last_line_checked) {
            node->last_line_checked = true;
            if (node->type != BlockType::List && node->type != BlockType::Item)
                break;
            if (node->children.empty()) break;
            node = node->children.back().get();
        }
        return node->last_line_blank;
    }

    // cmark list-finalization tightness (§5.3): a list is loose iff some item
    // is followed by a blank line, or some item directly contains two blocks
    // separated by a blank line.
    void compute_list_tightness(CMBlock* list) {
        list->tight = true;
        for (std::size_t i = 0; i < list->children.size(); ++i) {
            CMBlock* item = list->children[i].get();
            bool has_next = i + 1 < list->children.size();
            // non-final item ending with a blank line → loose
            if (item->last_line_blank && has_next) {
                list->tight = false;
                break;
            }
            // blank line between two of the item's own block children → loose
            auto& kids = item->children;
            for (std::size_t j = 0; j < kids.size(); ++j) {
                bool sub_has_next = j + 1 < kids.size();
                if ((has_next || sub_has_next) &&
                    ends_with_blank_line(kids[j].get())) {
                    list->tight = false;
                    break;
                }
            }
            if (!list->tight) break;
        }
    }

    void extract_ref_defs(CMBlock* para) {
        // Repeatedly peel `[label]: dest "title"` from the front.
        std::string& text = para->text;
        std::size_t consumed = 0;
        for (;;) {
            std::size_t used = try_parse_ref_def(std::string_view(text).substr(consumed));
            if (used == 0) break;
            consumed += used;
        }
        if (consumed > 0) text.erase(0, consumed);
    }

    // Parse one ref def at the start of `s`. Returns bytes consumed (0 = none).
    [[nodiscard]] std::size_t try_parse_ref_def(std::string_view s) {
        std::size_t i = 0;
        // optional up to 3 spaces
        while (i < s.size() && s[i] == ' ' && i < 3) ++i;
        if (i >= s.size() || s[i] != '[') return 0;
        ++i;
        // label up to unescaped ]
        std::string label;
        bool esc = false;
        std::size_t start = i;
        while (i < s.size()) {
            char c = s[i];
            if (esc) { esc = false; ++i; continue; }
            if (c == '\\') { esc = true; ++i; continue; }
            if (c == '[') return 0;  // labels can't contain unescaped [
            if (c == ']') break;
            ++i;
        }
        if (i >= s.size() || s[i] != ']') return 0;
        label = std::string(s.substr(start, i - start));
        ++i;  // ]
        if (i >= s.size() || s[i] != ':') return 0;
        ++i;  // :
        // skip ws incl up to one newline
        std::size_t nl = 0;
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n')) {
            if (s[i] == '\n') { if (++nl > 1) return 0; }
            ++i;
        }
        // destination
        std::string dest;
        if (i < s.size() && s[i] == '<') {
            ++i;
            while (i < s.size() && s[i] != '>' && s[i] != '\n') {
                if (s[i] == '\\' && i + 1 < s.size() && is_ascii_punct(s[i + 1])) {
                    dest += s[i + 1]; i += 2; continue;
                }
                dest += s[i++];
            }
            if (i >= s.size() || s[i] != '>') return 0;
            ++i;
        } else {
            int depth = 0;
            std::string acc;
            while (i < s.size() && !is_unicode_ws_byte(s[i])) {
                if (s[i] == '\\' && i + 1 < s.size() && is_ascii_punct(s[i + 1])) {
                    acc += s[i + 1]; i += 2; continue;
                }
                if (s[i] == '(') ++depth;
                else if (s[i] == ')') { if (depth == 0) break; --depth; }
                acc += s[i];
                ++i;
            }
            dest = std::move(acc);
            if (dest.empty()) return 0;
        }
        // optional title, preceded by ws (maybe newline)
        std::size_t save = i;
        std::string title;
        int nl2 = 0;
        std::size_t j = i;
        bool had_ws = false;
        while (j < s.size() && (s[j] == ' ' || s[j] == '\t' || s[j] == '\n')) {
            if (s[j] == '\n') ++nl2;
            had_ws = true;
            ++j;
        }
        bool got_title = false;
        if (had_ws && j < s.size() &&
            (s[j] == '"' || s[j] == '\'' || s[j] == '(')) {
            char open = s[j];
            char close = (open == '(') ? ')' : open;
            std::size_t k = j + 1;
            std::string t;
            bool ok = false;
            while (k < s.size()) {
                if (s[k] == '\\' && k + 1 < s.size() && is_ascii_punct(s[k + 1])) {
                    t += s[k + 1]; k += 2; continue;
                }
                if (s[k] == close) { ok = true; ++k; break; }
                t += s[k++];
            }
            if (ok) {
                // rest of line must be blank
                std::size_t m = k;
                while (m < s.size() && (s[m] == ' ' || s[m] == '\t')) ++m;
                if (m >= s.size() || s[m] == '\n') {
                    title = t;
                    got_title = true;
                    i = (m < s.size()) ? m + 1 : m;
                }
            }
        }
        if (!got_title) {
            // dest line must end (rest blank)
            std::size_t m = save;
            while (m < s.size() && (s[m] == ' ' || s[m] == '\t')) ++m;
            if (m < s.size() && s[m] != '\n') return 0;
            i = (m < s.size()) ? m + 1 : m;
        }
        (void)nl2;

        std::string key = normalize_label(label);
        if (key.empty()) return 0;
        if (refs_.find(key) == refs_.end())
            refs_[key] = md::LinkRef{decode_entities(dest), decode_entities(title)};
        return i;
    }

    // ── phase 2: inline parsing over leaf text ───────────────────────────
    void parse_inlines_tree(CMBlock* blk) {
        if (blk->type == BlockType::Paragraph || blk->type == BlockType::Heading) {
            auto t = strip(std::string_view(blk->text));
            blk->inlines = engine::parse_inlines(t, refs_, {ext_});
        }
        for (auto& ch : blk->children) parse_inlines_tree(ch.get());
    }

    // ── lowering to public AST ───────────────────────────────────────────
    // ── extension lowering helpers (gated on ext_) ──────────────────────
    // GFM task list: strip a leading "[ ] " / "[x] " / "[X] " from an item's
    // first-line spans and return its checked state (nullopt = not a task).
    [[nodiscard]] static std::optional<bool>
    take_task_marker(std::vector<md::Inline>& spans) {
        if (spans.empty()) return std::nullopt;
        auto* t = std::get_if<md::Text>(&spans.front().inner);
        if (!t || t->content.size() < 3) return std::nullopt;
        const std::string& c = t->content;
        if (c[0] != '[' || c[2] != ']') return std::nullopt;
        bool checked;
        if (c[1] == ' ') checked = false;
        else if (c[1] == 'x' || c[1] == 'X') checked = true;
        else return std::nullopt;
        std::size_t strip = 3;
        if (c.size() > 3) {
            if (c[3] != ' ') return std::nullopt;  // must be "[ ] " etc.
            strip = 4;
        }
        t->content.erase(0, strip);
        if (t->content.empty()) spans.erase(spans.begin());
        return checked;
    }

    // GitHub alert: if a blockquote's first line is exactly [!NOTE] (or TIP/
    // IMPORTANT/WARNING/CAUTION), strip it and return an Alert wrapping the
    // rest. `children` is consumed on success.
    [[nodiscard]] static std::optional<md::Alert>
    try_alert(std::vector<md::Block>& children) {
        if (children.empty()) return std::nullopt;
        auto* p = std::get_if<md::Paragraph>(&children.front().inner);
        if (!p || p->spans.empty()) return std::nullopt;
        auto* t = std::get_if<md::Text>(&p->spans.front().inner);
        if (!t) return std::nullopt;
        std::string s = t->content;
        if (s.size() < 4 || s.front() != '[' || s[1] != '!' || s.back() != ']')
            return std::nullopt;
        std::string type = ascii_lower(s.substr(2, s.size() - 3));
        md::Alert::Kind kind;
        if (type == "note") kind = md::Alert::Kind::Note;
        else if (type == "tip") kind = md::Alert::Kind::Tip;
        else if (type == "important") kind = md::Alert::Kind::Important;
        else if (type == "warning") kind = md::Alert::Kind::Warning;
        else if (type == "caution") kind = md::Alert::Kind::Caution;
        else return std::nullopt;
        // drop the [!TYPE] span and a following soft break, then the
        // paragraph itself if nothing else remains on that line.
        p->spans.erase(p->spans.begin());
        if (!p->spans.empty() &&
            std::holds_alternative<md::SoftBreak>(p->spans.front().inner))
            p->spans.erase(p->spans.begin());
        if (p->spans.empty()) children.erase(children.begin());
        md::Alert a;
        a.kind = kind;
        a.children = std::move(children);
        return a;
    }

    // Footnote definition: spans beginning with FootnoteRef + Text(":…").
    // Single-paragraph form (the common case); consumes `spans` on success.
    [[nodiscard]] static std::optional<md::FootnoteDef>
    try_footnote_def(std::vector<md::Inline>& spans) {
        if (spans.size() < 2) return std::nullopt;
        auto* fr = std::get_if<md::FootnoteRef>(&spans[0].inner);
        auto* tx = std::get_if<md::Text>(&spans[1].inner);
        if (!fr || !tx || tx->content.empty() || tx->content[0] != ':')
            return std::nullopt;
        md::FootnoteDef fd;
        fd.label = fr->label;
        std::vector<md::Inline> rest(std::make_move_iterator(spans.begin() + 1),
                                     std::make_move_iterator(spans.end()));
        auto& t0 = std::get<md::Text>(rest[0].inner);
        std::size_t strip = (t0.content.size() > 1 && t0.content[1] == ' ') ? 2 : 1;
        t0.content.erase(0, strip);
        if (t0.content.empty()) rest.erase(rest.begin());
        if (!rest.empty())
            fd.children.push_back(md::Block{md::Paragraph{std::move(rest)}});
        return fd;
    }

    void lower_children(CMBlock& blk, std::vector<md::Block>& out) {
        for (auto& ch : blk.children) lower_block(*ch, out);
    }

    void lower_block(CMBlock& blk, std::vector<md::Block>& out) {
        switch (blk.type) {
            case BlockType::Paragraph: {
                if (blk.inlines.empty() && strip(std::string_view(blk.text)).empty())
                    return;  // ref-def-only paragraph
                // ext: footnote definition — a paragraph that begins with a
                // footnote ref followed by ":" (i.e. "[^label]: content").
                if (ext_) {
                    if (auto fd = try_footnote_def(blk.inlines)) {
                        out.push_back(md::Block{std::move(*fd)});
                        break;
                    }
                }
                out.push_back(md::Block{md::Paragraph{std::move(blk.inlines)}});
                break;
            }
            case BlockType::Heading: {
                out.push_back(md::Block{md::Heading{blk.level, std::move(blk.inlines)}});
                break;
            }
            case BlockType::CodeFence: {
                // info string: backslash-unescape, then entities; the first
                // word is the language class.
                std::string info_unesc;
                for (std::size_t k = 0; k < blk.info.size(); ++k) {
                    if (blk.info[k] == '\\' && k + 1 < blk.info.size() &&
                        is_ascii_punct(blk.info[k + 1])) {
                        info_unesc += blk.info[++k];
                    } else {
                        info_unesc += blk.info[k];
                    }
                }
                std::string lang = decode_entities(info_unesc);
                auto sp = lang.find_first_of(" \t");
                if (sp != std::string::npos) lang = lang.substr(0, sp);
                // append_to_leaf adds a '\n' after every content line, so a
                // closed fence always ends with one. Highlighter renders
                // that trailing newline as a blank visual row inside the
                // bordered box — visible as an empty line between the
                // last line of code and the bottom border. CodeIndented
                // and HtmlBlock already strip; do the same here.
                std::string& text = blk.text;
                if (!text.empty() && text.back() == '\n') text.pop_back();
                out.push_back(md::Block{md::CodeBlock{std::move(text), std::move(lang)}});
                break;
            }
            case BlockType::CodeIndented: {
                // trim trailing blank lines
                std::string& c = blk.text;
                while (c.size() >= 2 && c[c.size()-1] == '\n' && c[c.size()-2] == '\n')
                    c.pop_back();
                out.push_back(md::Block{md::CodeBlock{std::move(c), ""}});
                break;
            }
            case BlockType::HtmlBlock: {
                std::string h = blk.text;
                // strip trailing newline run to one
                while (!h.empty() && (h.back() == '\n')) h.pop_back();
                // A lone/empty HTML tag (e.g. `<shell>` on its own line, a
                // kind-7 block closed by the next blank line per CommonMark
                // §4.6, or `<div></div>`) renders to nothing in a terminal.
                // Emitting it as a block would still occupy a row AND earn
                // the surrounding gap(1), punching a blank hole into the
                // output. Drop it here so BOTH the streaming and one-shot
                // paths see a dense block list with identical heights.
                if (html::renders_empty(h)) break;
                out.push_back(md::Block{md::HtmlBlock{std::move(h)}});
                break;
            }
            case BlockType::ThematicBreak:
                out.push_back(md::Block{md::HRule{}});
                break;
            case BlockType::BlockQuote: {
                md::Blockquote bq;
                lower_children(blk, bq.children);
                // ext: GitHub alert — a blockquote whose first line is
                // [!NOTE]/[!TIP]/[!IMPORTANT]/[!WARNING]/[!CAUTION].
                if (ext_)
                    if (auto a = try_alert(bq.children)) {
                        out.push_back(md::Block{std::move(*a)});
                        break;
                    }
                out.push_back(md::Block{std::move(bq)});
                break;
            }
            case BlockType::List: {
                md::List list;
                list.ordered = blk.ordered;
                list.start_num = blk.start_num;
                list.loose = !blk.tight;
                for (auto& itptr : blk.children) {
                    CMBlock& it = *itptr;
                    md::ListItem item;
                    // gather child blocks
                    std::vector<md::Block> kids;
                    lower_children(it, kids);
                    // If the first child is a paragraph, hoist its spans to the
                    // item's first-line content (matches the AST shape the
                    // renderer + scorer expect).
                    if (!kids.empty() &&
                        std::holds_alternative<md::Paragraph>(kids.front().inner)) {
                        item.spans = std::move(
                            std::get<md::Paragraph>(kids.front().inner).spans);
                        kids.erase(kids.begin());
                    }
                    // ext: GFM task-list marker "[ ] " / "[x] " at item start.
                    if (ext_)
                        if (auto chk = take_task_marker(item.spans)) item.checked = chk;
                    item.children = std::move(kids);
                    list.items.push_back(std::move(item));
                }
                out.push_back(md::Block{std::move(list)});
                break;
            }
            case BlockType::Item: {
                // items only appear under lists; handled there
                lower_children(blk, out);
                break;
            }
            case BlockType::Table: {
                md::Table tbl;
                tbl.aligns = blk.aligns;
                // header from info, rows from text
                for (auto cell : split_pipe_cells(blk.info)) {
                    md::TableCell c;
                    c.spans = engine::parse_inlines(cell, refs_, {ext_});
                    tbl.header.cells.push_back(std::move(c));
                }
                std::string_view body = blk.text;
                std::size_t i = 0;
                while (i < body.size()) {
                    std::size_t nl = body.find('\n', i);
                    auto row = body.substr(i, (nl == std::string_view::npos ? body.size() : nl) - i);
                    i = (nl == std::string_view::npos) ? body.size() : nl + 1;
                    if (strip(row).empty()) continue;
                    md::TableRow r;
                    auto cells = split_pipe_cells(row);
                    for (std::size_t ci = 0; ci < tbl.header.cells.size(); ++ci) {
                        md::TableCell c;
                        if (ci < cells.size())
                            c.spans = engine::parse_inlines(cells[ci], refs_, {ext_});
                        r.cells.push_back(std::move(c));
                    }
                    tbl.rows.push_back(std::move(r));
                }
                out.push_back(md::Block{std::move(tbl)});
                break;
            }
            case BlockType::Document:
                lower_children(blk, out);
                break;
        }
    }
};

} // namespace

md::Document parse(std::string_view source, Options opts) {
    BlockParser p;
    return p.run(source, opts.extensions);
}

} // namespace maya::md_detail::engine
