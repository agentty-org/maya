// markup.cpp — the markdown engine + the HTML widget, two ways.
//
//   ./maya_markup           interactive, scrollable viewer (default)
//   ./maya_markup --dump    one-shot colored dump to stdout (pipe / less -R)
//
// The document (four panels) is rendered ONCE per width into a canvas, then
// every canvas row is extracted as a plain styled TextElement. The viewer
// then slice-scrolls that flat line list (the scroll_slice.cpp pattern):
// only the visible rows are ever put in the Element tree, and they are plain
// text — no ComponentElements whose height a scroll viewport can't measure.
// That keeps scrolling correct and O(viewport), and re-flattening on resize
// keeps it responsive. A full flatten is ~1 ms.
//
//   Model      the flattened lines, the width they were flattened at, the
//              scroll offset and the viewport height.
//   update()   Resize re-flattens (only when the width changes); scroll
//              messages move and clamp the offset.
//   view()     the visible slice of lines, a scrollbar and a status line.
//
// Four panels, each full width:
//   1. CommonMark + GFM: headings, emphasis, nested loose/tight lists, a
//      table, a blockquote, a fenced code block, links/autolinks.
//   2. Inline HTML in markdown: <b>/<i>/<kbd>/<mark>/<sub>/<sup>/<code>/<a>/
//      <br> become styled runs (markdown delegates tag semantics to maya::html).
//   3. A raw HTML *block* in markdown — parsed and rendered, not literal tags.
//   4. The standalone maya::html widget rendering a full HTML fragment.
//
// Keys: ↑/↓ (j/k) scroll · PgUp/PgDn (space) page · Home/End jump · q/Esc quit.

#include <maya/core/target.hpp>
#include <maya/host/run.hpp>
#include <maya/maya.hpp>
#include <maya/widget/html.hpp>
#include <maya/widget/markdown.hpp>
#include <maya/widget/scrollbar.hpp>
#include <maya/render/diff.hpp>  // detail::encode_utf8

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <optional>
#include <string_view>
#include <variant>
#include <vector>

#if !MAYA_OS_WINDOWS
#include <sys/ioctl.h>
#include <unistd.h>
#endif

using namespace maya;
using namespace maya::dsl;

namespace {

// ── content ────────────────────────────────────────────────────────────────

// A titled panel that stretches to the full available width.
Element panel(std::string_view title, Element body) {
    auto head = Element{TextElement{
        .content = std::string(title),
        .style = Style{}.with_bold().with_fg(Color::hex(0x7DCFFF)),
    }};
    return vstack()
        .align_self(Align::Stretch)
        .border(BorderStyle::Round)
        .border_color(Color::hex(0x3B4261))
        .padding(0, 1, 0, 1)
        .gap(1)(std::move(head), std::move(body));
}

constexpr std::string_view kMarkdown = R"(# maya markdown

Supports **bold**, *italic*, ***both***, `inline code`,
[links](https://example.com) and autolinks <https://maya.dev>.

Tight list:
- alpha
- beta
  - nested gamma
  - nested delta

Loose list (blank lines between items):

1. first paragraph

2. second paragraph

> Block quotes nest and stay tight,
> across soft-wrapped lines.

| Feature   | State |
|-----------|:-----:|
| CommonMark| 100%  |
| GFM tables| yes   |

```cpp
auto ui = maya::markdown(source);
```
)";

constexpr std::string_view kInlineHtml =
    "Inline HTML is interpreted: <b>bold</b>, <i>italic</i>, "
    "<u>underline</u>, <mark>highlight</mark>, <code>code()</code>, "
    "press <kbd>Ctrl</kbd>+<kbd>C</kbd>, H<sub>2</sub>O, e=mc<sup>2</sup>, "
    "a <a href=\"https://maya.dev\">styled link</a>.<br>"
    "A &lt;br&gt; above forced this new line &mdash; entities decode too.";

constexpr std::string_view kBlockHtml = R"(A raw HTML block in markdown:

<table>
  <tr><th>Lang</th><th>Speed</th></tr>
  <tr><td>C++</td><td>fast</td></tr>
  <tr><td>maya</td><td>faster</td></tr>
</table>

<details>
  <summary>Click to expand</summary>
  <p>Hidden content rendered inline, with a <b>bold</b> word.</p>
</details>
)";

constexpr std::string_view kHtmlDoc = R"(
<h1>maya::html</h1>
<p>A standalone widget: tokenizer &rarr; DOM &rarr; Element, built on the
   maya DSL. Whitespace
   collapses    like a browser.</p>
<h2>Phrasing</h2>
<p><b>bold</b>, <em>em</em>, <s>strike</s>, <code>code</code>,
   <kbd>Esc</kbd>, <a href="/x">link</a>.</p>
<ul>
  <li>unordered one</li>
  <li>unordered two
    <ol start="5">
      <li>nested five</li>
      <li>nested six</li>
    </ol>
  </li>
</ul>
<blockquote>Block quotes get a gutter bar.</blockquote>
<pre>preformatted
  whitespace   preserved</pre>
<hr>
<table>
  <thead><tr><th>Col A</th><th>Col B</th></tr></thead>
  <tbody>
    <tr><td>1</td><td>two</td></tr>
    <tr><td>three</td><td>4</td></tr>
  </tbody>
</table>
)";

// The html widget is fully themable; tweak a couple of slots for panel 4.
constexpr Theme accent = Theme::derive(theme::native, [](Theme& t) {
    t.primary = Color::hex(0x9ECE6A);  // green h1
    t.link    = Color::hex(0xE0AF68);  // amber links
});

Element build_doc() {
    std::vector<Element> sections;
    sections.push_back(panel("1 · CommonMark + GFM", markdown(kMarkdown)));
    sections.push_back(panel("2 · Inline HTML in markdown", markdown(kInlineHtml)));
    sections.push_back(panel("3 · HTML block in markdown", markdown(kBlockHtml)));
    sections.push_back(panel("4 · Standalone html::render (custom theme)",
                             html::render(kHtmlDoc, accent)));
    return vstack().gap(1)(std::move(sections));
}

// ── render the document to a flat list of styled lines ──────────────────────
// Paint the tree into a canvas at `width`, then turn each row into a plain
// TextElement carrying the row's styled runs. The result has no
// ComponentElements, so it slices and scrolls perfectly.
std::vector<Element> flatten_to_lines(const Element& root, int width) {
    std::vector<Element> lines;
    if (width < 1) return lines;
    StylePool pool;
    Canvas canvas{width, 16000, &pool};
    render_tree(root, canvas, pool, theme::native, /*auto_height=*/true);
    int rows = content_height(canvas);
    lines.reserve(static_cast<std::size_t>(rows) + 1);
    for (int y = 0; y <= rows; ++y) {
        int last = -1;  // rightmost non-blank column (trim trailing blanks)
        for (int x = 0; x < width; ++x) {
            char32_t ch = canvas.get(x, y).character;
            if (ch != U' ' && ch != 0) last = x;
        }
        std::string content;
        std::vector<StyledRun> runs;
        std::uint16_t cur = 0;
        std::size_t run_start = 0;
        bool open = false;
        for (int x = 0; x <= last; ++x) {
            Cell cell = canvas.get(x, y);
            if (cell.character == 0) continue;  // wide-char continuation cell
            if (!open || cell.style_id != cur) {
                if (open && content.size() > run_start)
                    runs.push_back({run_start, content.size() - run_start,
                                    pool.get(cur)});
                cur = cell.style_id;
                run_start = content.size();
                open = true;
            }
            detail::encode_utf8(cell.character, content);
        }
        if (open && content.size() > run_start)
            runs.push_back({run_start, content.size() - run_start, pool.get(cur)});
        lines.push_back(Element{TextElement{
            .content = std::move(content),
            .style = Style{},
            .wrap = TextWrap::NoWrap,  // already laid out — never re-wrap
            .runs = std::move(runs),
        }});
    }
    return lines;
}

// Terminal width for the one-shot dump (full columns, or 80 when not a TTY).
int dump_width() {
#if !MAYA_OS_WINDOWS
    struct winsize ws {};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return std::max<int>(ws.ws_col, 40);
#endif
    return 80;
}

void dump() {
    int width = dump_width();
    StylePool pool;
    Canvas canvas{width, 16000, &pool};
    render_tree(build_doc(), canvas, pool, theme::native, /*auto_height=*/true);
    int rows = content_height(canvas);
    if (rows < 0) return;
    std::string out;
    serialize(canvas, pool, out, rows + 1, 0);
    out += "\x1b[?7h";  // serialize disables autowrap; restore it
    out += '\n';
    std::fwrite(out.data(), 1, out.size(), stdout);
    std::fflush(stdout);
}

// ── the viewer ─────────────────────────────────────────────────────────────

struct Model {
    std::vector<Element> lines;   // the doc flattened at `flat_w`
    int flat_w = -1;              // width `lines` was flattened at
    int offset = 0;               // top visible line
    int viewport_h = 1;           // rows available for the document

    // The scrollbar BORROWS this for the whole frame: the renderer writes the
    // painted bar's hit-rects back into it after view() has returned. So it
    // lives here, not in view(). `mutable` because view() is const and only
    // mirrors the offset into it — the scroll position of record is `offset`.
    mutable ScrollState bar;

    int max_offset() const { return std::max(0, static_cast<int>(lines.size()) - viewport_h); }
};

struct Resize { int cols, rows; };
struct Scroll { int by; };        // lines; ±page is computed in update
struct Page   { int dir; };
struct Home {};
struct End {};
struct Quit {};
using Msg = std::variant<Resize, Scroll, Page, Home, End, Quit>;

struct Markup {
    using Model = ::Model;
    using Msg   = ::Msg;
    using Cmd   = jaal::Cmd<Msg>;

    static void clamp(Model& m) { m.offset = std::clamp(m.offset, 0, m.max_offset()); }

    static Cmd update(Model& m, Resize r) {
        // Re-flatten only when the width changes; 1 column for the scrollbar.
        const int content_w = std::max(1, r.cols - 1);
        if (content_w != m.flat_w) {
            m.lines = flatten_to_lines(build_doc(), content_w);
            m.flat_w = content_w;
        }
        m.viewport_h = std::max(1, r.rows - 3);   // header + help + status rows
        clamp(m);
        return {};
    }
    static Cmd update(Model& m, Scroll s) { m.offset += s.by; clamp(m); return {}; }
    static Cmd update(Model& m, Page p) {
        m.offset += p.dir * std::max(1, m.viewport_h - 1);
        clamp(m);
        return {};
    }
    static Cmd update(Model& m, Home) { m.offset = 0; return {}; }
    static Cmd update(Model& m, End)  { m.offset = m.max_offset(); return {}; }
    static Cmd update(Model&, Quit)   { return Cmd::quit(0); }

    static Element view(const Model& m) {
        const int total = static_cast<int>(m.lines.size());
        const int start = std::clamp(m.offset, 0, m.max_offset());
        const int count = std::max(0, std::min(m.viewport_h, total - start));
        std::vector<Element> visible(m.lines.begin() + start,
                                     m.lines.begin() + start + count);

        // Mirror the offset into the Model's ScrollState so scrollbar_y can
        // draw the thumb. auto_dispatch = false: update() drives scrolling.
        m.bar.y = start;
        m.bar.max_y = m.max_offset();
        m.bar.auto_dispatch = false;

        std::string status = "lines " + std::to_string(start + 1) + "–" +
                             std::to_string(start + count) + " / " +
                             std::to_string(total);

        return v(
            t<"maya markup — CommonMark + GFM + HTML widget">
                | Bold | Fg<125, 207, 255>,
            t<"↑/↓ PgUp/PgDn Home/End scroll · q quit"> | Dim,
            h(
                v(std::move(visible)) | grow_<1>,
                scrollbar_y(m.bar, m.viewport_h)
            ),
            text(status) | Fg<224, 175, 104>
        );
    }

    static std::optional<Msg> on(const Model&, const ResizeEvent& r) {
        return Resize{r.width.value, r.height.value};
    }
    static std::optional<Msg> on(const Model&, const MouseEvent& e) {
        if (e.kind != MouseEventKind::Press) return std::nullopt;
        if (e.button == MouseButton::ScrollUp)   return Scroll{-3};
        if (e.button == MouseButton::ScrollDown) return Scroll{+3};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model&, const KeyEvent& k) {
        static const keymap<Msg> keys{
            {SpecialKey::Up, Scroll{-1}}, {SpecialKey::Down, Scroll{+1}},
            {'k', Scroll{-1}}, {'j', Scroll{+1}}, {' ', Page{+1}},
            {SpecialKey::PageUp, Page{-1}}, {SpecialKey::PageDown, Page{+1}},
            {SpecialKey::Home, Home{}}, {SpecialKey::End, End{}},
            {'q', Quit{}}, {SpecialKey::Escape, Quit{}},
        };
        return keys(k);
    }
};

static_assert(Program<Markup>);

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--dump") == 0) {
        dump();
        return 0;
    }
    return run<Markup>({.title = "markup", .mouse = true});
}
