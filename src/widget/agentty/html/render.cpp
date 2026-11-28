// render.cpp — HTML DOM → maya Element. Implements a small CSS-like block /
// inline formatting model:
//   * phrasing content (text + inline elements) collapses HTML whitespace and
//     accumulates into one word-wrapped TextElement with per-run styles;
//   * block elements flush the current inline flow and stack vertically.
// Layout is built with the maya DSL (vstack/hstack); colour comes from the
// caller's Theme. Public entry: maya::html::render().

#include "maya/widget/agentty/html.hpp"
#include "maya/widget/agentty/html/internal.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "maya/dsl.hpp"
#include "maya/element/builder.hpp"
#include "maya/element/text.hpp"
#include "maya/style/style.hpp"

namespace maya::html {
namespace {

using detail::Node;

// ── inline flow ───────────────────────────────────────────────────────────
// A segment of phrasing content awaiting whitespace collapse. `is_break` is a
// hard <br> (preserved); ordinary source whitespace inside `text` collapses.
struct Seg {
    std::string text;
    Style       style;
    bool        is_break = false;
};

class Renderer {
public:
    explicit Renderer(const Theme& th) : th_(th) {}

    Element render_document(const Node& doc) {
        auto blocks = render_children(doc, 0);
        if (blocks.empty()) return Element{TextElement{}};
        if (blocks.size() == 1) return std::move(blocks.front());
        return dsl::vstack()(std::move(blocks));
    }

private:
    const Theme& th_;

    [[nodiscard]] Style base_text() const { return Style{}.with_fg(th_.text); }

    // Map a phrasing role onto `cur`, themed.
    [[nodiscard]] Style apply_role(Role r, Style cur, const Node& el) const {
        switch (r) {
            case Role::Bold:      return cur.with_bold();
            case Role::Italic:    return cur.with_italic();
            case Role::Underline: return cur.with_underline();
            case Role::Strike:    return cur.with_strikethrough().with_fg(th_.muted);
            case Role::Code:      return cur.with_fg(th_.info).with_bg(th_.surface);
            case Role::KeyCap:    return cur.with_bold().with_fg(th_.text)
                                            .with_bg(th_.surface);
            case Role::Mark:      return cur.with_fg(th_.inverse_text)
                                            .with_bg(th_.highlight);
            case Role::Small:     return cur.with_dim();
            case Role::Sub:
            case Role::Sup:       return cur.with_fg(th_.muted);
            case Role::Link:      return cur.with_fg(th_.link).with_underline();
            case Role::None:
            case Role::Break:     break;
        }
        (void)el;
        return cur;
    }

    // Append a phrasing subtree to `segs`.
    void emit_inline(const Node& n, Style cur, std::vector<Seg>& segs) const {
        if (n.kind == Node::Kind::Text) {
            if (!n.text.empty()) segs.push_back({n.text, cur, false});
            return;
        }
        if (n.kind != Node::Kind::Element) return;
        Role r = inline_role(n.tag);
        if (r == Role::Break) { segs.push_back({"", cur, true}); return; }
        // Role styling first, then per-element CSS (style=/color=/bgcolor=)
        // overlays so `<span style=...>` and `<font color=...>` win over the
        // inherited/role style, exactly like the CSS cascade.
        Style st = apply_role(r, cur, n);
        st = detail::apply_presentation(n.attrs, st);
        for (const auto& c : n.children) emit_inline(c, st, segs);
    }

    // Collapse HTML whitespace across the flow and build one TextElement.
    // Source spaces/tabs/newlines collapse to single spaces and trim at the
    // ends; <br> segments force a literal newline.
    [[nodiscard]] Element flow_element(const std::vector<Seg>& segs) const {
        std::string content;
        std::vector<StyledRun> runs;
        bool prev_space = true;  // suppress leading whitespace
        for (const auto& seg : segs) {
            std::size_t start = content.size();
            if (seg.is_break) {
                content += '\n';
                runs.push_back({start, 1, seg.style});
                prev_space = true;
                continue;
            }
            for (char c : seg.text) {
                bool ws = (c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
                           c == '\f');
                if (ws) {
                    if (!prev_space) content += ' ';
                    prev_space = true;
                } else {
                    content += c;
                    prev_space = false;
                }
            }
            if (content.size() > start)
                runs.push_back({start, content.size() - start, seg.style});
        }
        // trim a single trailing collapsed space
        if (!content.empty() && content.back() == ' ') {
            content.pop_back();
            if (!runs.empty()) {
                auto& last = runs.back();
                if (last.byte_offset + last.byte_length > content.size())
                    last.byte_length = content.size() - last.byte_offset;
                if (last.byte_length == 0) runs.pop_back();
            }
        }
        return make_text(std::move(content), std::move(runs));
    }

    [[nodiscard]] Element make_text(std::string content,
                                    std::vector<StyledRun> runs) const {
        if (runs.size() == 1)
            return Element{TextElement{.content = std::move(content),
                                       .style = runs[0].style}};
        return Element{TextElement{.content = std::move(content),
                                   .style = base_text(),
                                   .runs = std::move(runs)}};
    }

    // Render a node's children: phrasing groups into TextElements, block
    // elements recurse. Returns the interleaved block-level Elements.
    std::vector<Element> render_children(const Node& n, int depth) {
        std::vector<Element> out;
        std::vector<Seg> flow;
        auto flush = [&] {
            if (flow.empty()) return;
            Element e = flow_element(flow);
            flow.clear();
            if (auto* t = std::get_if<TextElement>(&e.inner);
                t && t->content.empty() && t->runs.empty())
                return;  // nothing but collapsed whitespace
            out.push_back(std::move(e));
        };
        for (const auto& c : n.children) {
            bool block = c.kind == Node::Kind::Element &&
                         detail::is_block_element(c.tag);
            if (block) {
                flush();
                out.push_back(render_block(c, depth));
            } else {
                emit_inline(c, base_text(), flow);
            }
        }
        flush();
        return out;
    }

    [[nodiscard]] Element stack_children(const Node& n, int depth) {
        auto kids = render_children(n, depth);
        if (kids.empty()) return Element{TextElement{}};
        if (kids.size() == 1) return std::move(kids.front());
        return dsl::vstack()(std::move(kids));
    }

    // Gather all descendant text verbatim (for <pre>).
    static void gather_text(const Node& n, std::string& out) {
        if (n.kind == Node::Kind::Text) { out += n.text; return; }
        for (const auto& c : n.children) gather_text(c, out);
    }

    // Hard cap on block-nesting recursion. HTML parses into an arbitrarily
    // deep tree (the parser uses an explicit stack, so PARSING deep input is
    // safe), but rendering recurses that tree on the native call stack:
    // render_block → stack_children → render_children → render_block. Adversarial
    // or LLM-generated markup like `<div><div>…×20000` would overflow the stack
    // and crash the process. Past this depth we stop descending and render the
    // remaining subtree as its flattened text — legible, bounded, no crash.
    //
    // NOTE: this is a SEPARATE counter from the `depth` parameter (which tracks
    // LIST/nesting level for bullet styling and increments only in list/
    // details/deflist). recursion_ counts actual call-stack frames so the guard
    // is exact regardless of which element types nest. 64 comfortably exceeds
    // any real document while keeping worst-case frames well under an 8 MiB
    // stack.
    static constexpr int kMaxDepth = 64;
    int recursion_ = 0;

    struct DepthScope {
        int& d;
        explicit DepthScope(int& r) noexcept : d(r) { ++d; }
        ~DepthScope() { --d; }
    };

    // One block element → one Element.
    Element render_block(const Node& n, int depth) {
        DepthScope guard(recursion_);
        const std::string& tag = n.tag;

        if (recursion_ > kMaxDepth) {
            // Bail out of recursion: gather the whole subtree's text and
            // render it flat. Keeps deeply-pathological input safe.
            std::string text;
            gather_text(n, text);
            while (!text.empty() && (text.back() == '\n' || text.back() == ' '))
                text.pop_back();
            if (text.empty()) return Element{TextElement{}};
            return Element{TextElement{.content = std::move(text),
                                       .style = base_text()}};
        }

        if (tag == "h1" || tag == "h2" || tag == "h3" || tag == "h4" ||
            tag == "h5" || tag == "h6")
            return heading(n, tag[1] - '0');
        if (tag == "ul" || tag == "ol") return list(n, depth);
        if (tag == "li") return stack_children(n, depth);  // bare <li>
        if (tag == "blockquote") return blockquote(n, depth);
        if (tag == "pre") return preformatted(n);
        if (tag == "hr") return rule();
        if (tag == "table") return table(n);
        if (tag == "dl") return deflist(n, depth);
        if (tag == "details") return details(n, depth);
        // table sub-parts reached out of context, and generic containers
        // (div/p/section/article/header/footer/main/aside/figure/…): just
        // stack their children.
        return stack_children(n, depth);
    }

    Element heading(const Node& n, int level) {
        std::vector<Seg> segs;
        Style base = Style{}.with_bold();
        switch (level) {
            case 1: base = base.with_fg(th_.primary); break;
            case 2: base = base.with_fg(th_.accent); break;
            case 3: base = base.with_fg(th_.info); break;
            default: base = base.with_fg(th_.muted); break;
        }
        for (const auto& c : n.children) emit_inline(c, base, segs);
        return flow_element(segs);
    }

    Element list(const Node& n, int depth) {
        bool ordered = (n.tag == "ol");
        int num = 1;
        if (ordered) {
            auto s = detail::attr_of(n.attrs, "start");
            if (!s.empty()) {
                int v = 0; bool ok = !s.empty();
                for (char c : s) { if (c < '0' || c > '9') { ok = false; break; }
                                   v = v * 10 + (c - '0'); }
                if (ok) num = v;
            }
        }
        std::vector<Element> items;
        Style marker_style = Style{}.with_fg(th_.muted);
        for (const auto& c : n.children) {
            if (c.kind != Node::Kind::Element || c.tag != "li") continue;
            std::string prefix = ordered
                ? "  " + std::to_string(num++) + ". "
                : (depth == 0 ? "  \xe2\x80\xa2 "    // •
                              : "    \xe2\x97\xa6 "); // ◦
            Element body = stack_children(c, depth + 1);
            items.push_back(dsl::hstack()(
                Element{TextElement{.content = std::move(prefix),
                                    .style = marker_style}},
                std::move(body)));
        }
        if (items.empty()) return Element{TextElement{}};
        return dsl::vstack()(std::move(items));
    }

    Element blockquote(const Node& n, int depth) {
        Style bar = Style{}.with_fg(th_.muted);
        Style txt = Style{}.with_italic().with_fg(th_.muted);
        std::vector<Element> rows;
        for (auto& child : render_children(n, depth)) {
            rows.push_back(dsl::hstack()(
                Element{TextElement{.content = "\xe2\x94\x82 ", .style = bar}},  // │
                std::move(child) | txt));
        }
        if (rows.empty()) return Element{TextElement{}};
        return dsl::vstack()(std::move(rows));
    }

    Element preformatted(const Node& n) {
        std::string text;
        gather_text(n, text);
        // strip one leading newline (HTML drops the first newline after <pre>)
        if (!text.empty() && text.front() == '\n') text.erase(0, 1);
        while (!text.empty() && (text.back() == '\n' || text.back() == ' '))
            text.pop_back();
        Element body{TextElement{.content = std::move(text),
                                 .style = Style{}.with_fg(th_.info),
                                 .wrap = TextWrap::NoWrap}};
        return dsl::vstack()
            .align_self(Align::Stretch)
            .border(BorderStyle::Round)
            .border_color(th_.border)
            .padding(0, 1, 0, 1)
            .overflow(Overflow::Hidden)(std::move(body));
    }

    Element rule() {
        Style s = Style{}.with_fg(th_.border);
        return Element{ComponentElement{
            .render = [s](int w, int) -> Element {
                std::string line;
                line.reserve(static_cast<std::size_t>(w > 0 ? w : 0) * 3);
                for (int i = 0; i < w; ++i) line += "\xe2\x94\x80";  // ─
                return Element{TextElement{.content = std::move(line), .style = s}};
            },
        }};
    }

    Element deflist(const Node& n, int depth) {
        std::vector<Element> rows;
        for (const auto& c : n.children) {
            if (c.kind != Node::Kind::Element) continue;
            if (c.tag == "dt") {
                std::vector<Seg> segs;
                Style base = Style{}.with_bold().with_fg(th_.text);
                for (const auto& g : c.children) emit_inline(g, base, segs);
                rows.push_back(flow_element(segs));
            } else if (c.tag == "dd") {
                rows.push_back(dsl::hstack()(
                    Element{TextElement{.content = "    "}},
                    stack_children(c, depth + 1)));
            }
        }
        if (rows.empty()) return Element{TextElement{}};
        return dsl::vstack()(std::move(rows));
    }

    Element details(const Node& n, int depth) {
        std::vector<Element> rows;
        std::vector<Element> body;
        for (const auto& c : n.children) {
            if (c.kind == Node::Kind::Element && c.tag == "summary") {
                std::vector<Seg> segs;
                Style base = Style{}.with_bold().with_fg(th_.text);
                for (const auto& g : c.children) emit_inline(g, base, segs);
                std::string head = "\xe2\x96\xb8 ";  // ▸
                Element s = flow_element(segs);
                rows.insert(rows.begin(), dsl::hstack()(
                    Element{TextElement{.content = std::move(head),
                                        .style = Style{}.with_fg(th_.muted)}},
                    std::move(s)));
            } else {
                bool block = c.kind == Node::Kind::Element &&
                             detail::is_block_element(c.tag);
                if (block) body.push_back(render_block(c, depth + 1));
            }
        }
        if (rows.empty() && body.empty()) return Element{TextElement{}};
        std::vector<Element> out;
        if (!rows.empty()) out.push_back(std::move(rows.front()));
        if (!body.empty())
            out.push_back(dsl::hstack()(
                Element{TextElement{.content = "  "}},
                dsl::vstack()(std::move(body))));
        return dsl::vstack()(std::move(out));
    }

    // ── tables ──────────────────────────────────────────────────────────────
    struct Cell { std::string text; std::vector<StyledRun> runs; bool header = false; };
    using Row = std::vector<Cell>;

    void collect_rows(const Node& n, std::vector<Row>& rows) {
        for (const auto& c : n.children) {
            if (c.kind != Node::Kind::Element) continue;
            if (c.tag == "tr") {
                Row row;
                for (const auto& cell : c.children) {
                    if (cell.kind != Node::Kind::Element) continue;
                    if (cell.tag != "td" && cell.tag != "th") continue;
                    std::vector<Seg> segs;
                    Style base = (cell.tag == "th")
                        ? Style{}.with_bold().with_fg(th_.text)
                        : Style{}.with_fg(th_.text);
                    for (const auto& g : cell.children) emit_inline(g, base, segs);
                    // flatten flow to one line for the grid
                    Element e = flow_element(segs);
                    Cell out;
                    out.header = (cell.tag == "th");
                    if (auto* t = std::get_if<TextElement>(&e.inner)) {
                        out.text = t->content;
                        out.runs = t->runs.empty()
                            ? std::vector<StyledRun>{{0, t->content.size(), t->style}}
                            : t->runs;
                    }
                    // collapse hard breaks to spaces in a grid cell
                    for (char& ch : out.text) if (ch == '\n') ch = ' ';
                    row.push_back(std::move(out));
                }
                if (!row.empty()) rows.push_back(std::move(row));
            } else if (c.tag == "thead" || c.tag == "tbody" || c.tag == "tfoot") {
                collect_rows(c, rows);
            }
        }
    }

    Element table(const Node& n) {
        std::vector<Row> rows;
        collect_rows(n, rows);
        if (rows.empty()) return Element{TextElement{}};

        std::size_t ncols = 0;
        for (auto& r : rows) ncols = std::max(ncols, r.size());
        std::vector<int> width(ncols, 1);
        for (auto& r : rows)
            for (std::size_t c = 0; c < r.size(); ++c)
                width[c] = std::max(width[c], string_width(r[c].text));

        const std::string H = "\xe2\x94\x80";  // ─
        Style border = Style{}.with_fg(th_.border);
        auto border_line = [&](const char* l, const char* m, const char* r) {
            std::string s = l;
            for (std::size_t c = 0; c < ncols; ++c) {
                if (c) s += m;
                for (int k = 0; k < width[c] + 2; ++k) s += H;
            }
            s += r;
            return Element{TextElement{.content = std::move(s), .style = border}};
        };
        auto row_element = [&](const Row& row) {
            std::string line;
            std::vector<StyledRun> runs;
            const std::string sep = "\xe2\x94\x82";  // │
            auto add_sep = [&] {
                runs.push_back({line.size(), sep.size(), border});
                line += sep;
            };
            add_sep();
            for (std::size_t c = 0; c < ncols; ++c) {
                const Cell* cell = c < row.size() ? &row[c] : nullptr;
                int cw = cell ? string_width(cell->text) : 0;
                int pad = width[c] - cw;
                line += ' ';
                std::size_t coff = line.size();
                if (cell) {
                    line += cell->text;
                    for (auto& rn : cell->runs)
                        runs.push_back({coff + rn.byte_offset, rn.byte_length, rn.style});
                }
                line.append(static_cast<std::size_t>(pad < 0 ? 0 : pad), ' ');
                line += ' ';
                add_sep();
            }
            return Element{TextElement{.content = std::move(line),
                                       .style = base_text(),
                                       .runs = std::move(runs)}};
        };

        std::vector<Element> out;
        out.push_back(border_line("\xe2\x94\x8c", "\xe2\x94\xac", "\xe2\x94\x90")); // ┌┬┐
        bool first = true;
        for (std::size_t i = 0; i < rows.size(); ++i) {
            out.push_back(row_element(rows[i]));
            bool header_sep = first && (i == 0) &&
                              !rows[i].empty() && rows[i][0].header;
            if (i == 0 && (header_sep || rows.size() > 1))
                out.push_back(border_line("\xe2\x94\x9c", "\xe2\x94\xbc",
                                          "\xe2\x94\xa4"));  // ├┼┤
            first = false;
        }
        out.push_back(border_line("\xe2\x94\x94", "\xe2\x94\xb4", "\xe2\x94\x98")); // └┴┘
        return dsl::vstack()(std::move(out));
    }
};

} // namespace

Element render(std::string_view source, const Theme& theme) {
    detail::Node doc = detail::parse(source);
    return Renderer(theme).render_document(doc);
}

namespace {
// Would this rendered subtree show a reader nothing at all? True only when
// there are no glyphs, no styled runs, and no visible structural children
// (tables/rules carry glyphs via their content strings, so they count as
// non-empty). ComponentElement/ElementListRef render lazily and are never
// assumed empty.
[[nodiscard]] bool visually_empty(const Element& e) {
    return std::visit([](const auto& n) -> bool {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, TextElement>) {
            return n.content.empty() && n.runs.empty();
        } else if constexpr (std::is_same_v<T, BoxElement>) {
            for (const auto& c : n.children)
                if (!visually_empty(c)) return false;
            return true;
        } else if constexpr (std::is_same_v<T, ElementList>) {
            for (const auto& c : n.items)
                if (!visually_empty(c)) return false;
            return true;
        } else {
            return false;
        }
    }, e.inner);
}
} // namespace

bool renders_empty(std::string_view source, const Theme& theme) {
    detail::Node doc = detail::parse(source);
    return visually_empty(Renderer(theme).render_document(doc));
}

} // namespace maya::html
