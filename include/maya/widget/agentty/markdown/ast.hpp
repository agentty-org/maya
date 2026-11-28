#pragma once
// ast.hpp — the markdown abstract syntax tree.
//
// Closed sum types for every inline and block construct the engine can
// produce. Pure value types: no I/O, no Element, no parser state — the
// parser builds these, the renderer consumes them, the streaming widget
// snapshots them. Carved out of the public markdown.hpp so the parser /
// renderer / streaming TUs (and the conformance harness) can depend on
// the node shapes without dragging in the StreamingMarkdown machinery.

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace maya {
namespace md {

// ============================================================================
// Inline AST nodes (within paragraphs/headings)
// ============================================================================

struct Text        { std::string content; };
struct Bold        { std::vector<struct Inline> children; };
struct Italic      { std::vector<struct Inline> children; };
struct BoldItalic  { std::vector<struct Inline> children; };
struct Code        { std::string content; };
// Link/Image carry BOTH a flattened string (text/alt — consumed by the
// terminal renderer and the streaming widget, which want a plain label)
// AND the structured inline children (kids — consumed by the CommonMark
// HTML scorer and any future rich renderer). The engine populates both;
// `text`/`alt` is the children flattened to plain text.
struct Link        { std::string text; std::string url; std::string title;
                     std::vector<struct Inline> kids; };
struct Image       { std::string alt;  std::string url; std::string title;
                     std::vector<struct Inline> kids; };
struct Strike      { std::vector<struct Inline> children; };
struct Highlight   { std::vector<struct Inline> children; };    // ==text== or <mark>
struct Sub         { std::vector<struct Inline> children; };    // ~x~ or <sub>
struct Sup         { std::vector<struct Inline> children; };    // ^x^ or <sup>
struct Kbd         { std::vector<struct Inline> children; };    // <kbd>
struct Abbr        { std::string title; std::vector<struct Inline> children; }; // <abbr title="…">
struct Mention     {
    enum class Kind : uint8_t { User, Issue, CrossRepo };
    Kind        kind;
    std::string display;   // "@alice" / "#42" / "foo/bar#7"
    std::string url;       // best-effort GitHub URL
};
struct FootnoteRef { std::string label; };
struct HardBreak   {};
struct SoftBreak   {};                       // raw newline inside a paragraph
struct RawInline   { std::string content; }; // verbatim inline HTML / entity passthrough
// Inline math: `$…$` or `\(…\)`. `latex` is the raw TeX between the
// delimiters, typeset by the terminal renderer (widget/markdown/tex_math).
struct MathInline  { std::string latex; };

struct Inline {
    using Variant = std::variant<Text, Bold, Italic, BoldItalic, Code,
                                 Link, Image, Strike, Highlight, Sub, Sup,
                                 Kbd, Abbr, Mention, FootnoteRef, HardBreak,
                                 SoftBreak, RawInline, MathInline>;
    Variant inner;

    Inline(Text t)        : inner(std::move(t)) {}
    Inline(Bold b)        : inner(std::move(b)) {}
    Inline(Italic i)      : inner(std::move(i)) {}
    Inline(BoldItalic bi) : inner(std::move(bi)) {}
    Inline(Code c)        : inner(std::move(c)) {}
    Inline(Link l)        : inner(std::move(l)) {}
    Inline(Image im)      : inner(std::move(im)) {}
    Inline(Strike s)      : inner(std::move(s)) {}
    Inline(Highlight h)   : inner(std::move(h)) {}
    Inline(Sub sb)        : inner(std::move(sb)) {}
    Inline(Sup sp)        : inner(std::move(sp)) {}
    Inline(Kbd k)         : inner(std::move(k)) {}
    Inline(Abbr a)        : inner(std::move(a)) {}
    Inline(Mention m)     : inner(std::move(m)) {}
    Inline(FootnoteRef f) : inner(std::move(f)) {}
    Inline(HardBreak)     : inner(HardBreak{}) {}
    Inline(SoftBreak)     : inner(SoftBreak{}) {}
    Inline(RawInline r)   : inner(std::move(r)) {}
    Inline(MathInline m)  : inner(std::move(m)) {}
};

// ============================================================================
// Block AST nodes
// ============================================================================

struct Paragraph   { std::vector<Inline> spans; };
struct Heading     { int level; std::vector<Inline> spans; };
struct CodeBlock   { std::string content; std::string lang; };
struct Blockquote  { std::vector<struct Block> children; };

struct ListItem {
    std::vector<Inline> spans;                // first-line inline content
    std::vector<struct Block> children;       // sub-blocks (nested lists, multi-para)
    std::optional<bool> checked;              // nullopt = normal, true/false = task
};
struct List        {
    std::vector<ListItem> items;
    bool ordered;
    int  start_num = 1;
    // CommonMark "loose" list: items are separated by blank lines, or an
    // item holds multiple block-level children. Loose items render their
    // first-line content wrapped in <p>; tight items render it bare.
    bool loose = false;
};

struct HRule       {};
struct TableCell   { std::vector<Inline> spans; };
struct TableRow    { std::vector<TableCell> cells; };
// Per-column alignment from the GFM `|:--|:-:|--:|` delimiter row.
// `aligns.size()` is always equal to `header.cells.size()`. Default
// (no colons on either side) is `Left`.
enum class TableAlign : std::uint8_t { Left, Center, Right };
struct Table       {
    TableRow header;
    std::vector<TableRow> rows;
    std::vector<TableAlign> aligns;
    // Streaming-only per-column width FLOOR. When non-empty the renderer
    // takes each entry as a minimum for that column's ideal width (folded
    // via std::max, never shrinks a column). The live StreamingMarkdown
    // sets this from EVERY already-arrived table row — including rows the
    // reveal cursor hasn't exposed yet — so the visible table renders at
    // its final column widths from the first row onward instead of
    // reflowing horizontally as later / wider cells stream in. Empty for
    // static and committed tables, so their layout is byte-for-byte
    // unchanged (the fold is a no-op on an empty vector).
    std::vector<int> min_col_widths;
    // Streaming-only CHUNK-SEAM flags. The live tail seals a long
    // streaming table's terminated rows into fixed row-range chunks so
    // the renderer blits them from cache instead of repainting the whole
    // table every time a row lands (O(rows)/frame → O(chunk)/frame). A
    // chunk is this same md::Table (header kept for column count /
    // alignment) rendered WITHOUT the parts owned by its neighbours:
    //   omit_top    — skip the top border + header rows + header rule;
    //                 emit a row separator BEFORE the first data row
    //                 instead (the seam to the chunk above).
    //   omit_bottom — skip the bottom border (a later chunk owns it).
    // Chunks are only cell-identical to the whole-table render when
    // min_col_widths pins every column (the streaming floor covers all
    // arrived rows, so each chunk's natural ideal never exceeds it).
    // Defaults false: the engine never sets these; committed and static
    // tables render byte-for-byte as before.
    bool omit_top    = false;
    bool omit_bottom = false;
};
struct FootnoteDef { std::string label; std::vector<struct Block> children; };

// GitHub alert block: `> [!NOTE] ...`
struct Alert {
    enum class Kind : uint8_t { Note, Tip, Important, Warning, Caution };
    Kind kind = Kind::Note;
    std::vector<struct Block> children;
};

// Definition list: term  \n  : def \n  : another def
struct DefItem { std::vector<Inline> term; std::vector<std::vector<struct Block>> defs; };
struct DefList { std::vector<DefItem> items; };

// Collapsible <details>/<summary> section (rendered as a titled blockquote)
struct Details {
    std::vector<Inline>       summary;
    std::vector<struct Block> body;
};

// Arbitrary raw-HTML block — rendered as plain monospaced text.
struct HtmlBlock { std::string content; };

struct Block {
    using Variant = std::variant<Paragraph, Heading, CodeBlock,
                                 Blockquote, List, HRule, Table,
                                 FootnoteDef, Alert, DefList, Details, HtmlBlock>;
    Variant inner;

    Block(Paragraph p)   : inner(std::move(p)) {}
    Block(Heading h)     : inner(std::move(h)) {}
    Block(CodeBlock c)   : inner(std::move(c)) {}
    Block(Blockquote b)  : inner(std::move(b)) {}
    Block(List l)        : inner(std::move(l)) {}
    Block(HRule)         : inner(HRule{}) {}
    Block(Table t)       : inner(std::move(t)) {}
    Block(FootnoteDef f) : inner(std::move(f)) {}
    Block(Alert a)       : inner(std::move(a)) {}
    Block(DefList d)     : inner(std::move(d)) {}
    Block(Details d)     : inner(std::move(d)) {}
    Block(HtmlBlock h)   : inner(std::move(h)) {}
};

// Reference-link definition: `[label]: url "title"`.  Keys are lowercased.
struct LinkRef { std::string url; std::string title; };

struct Document {
    std::vector<Block>                              blocks;
    std::unordered_map<std::string, LinkRef>        ref_defs;
};

} // namespace md
} // namespace maya
