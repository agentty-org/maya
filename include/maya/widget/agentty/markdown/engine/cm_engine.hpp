#pragma once
// cm_engine.hpp — the CommonMark core's internal block tree + entry point.
//
// The spec parses in two phases:
//   1. Block structure — consume the input line by line, maintaining a
//      stack of open blocks; each line either continues open blocks,
//      closes them, or opens new ones. Produces a tree of CMBlock.
//   2. Inline structure — for every leaf block that holds raw text
//      (paragraphs, headings), run the delimiter-stack inline parser to
//      produce md::Inline spans.
//
// CMBlock is the mutable parse-time node (it accumulates raw lines and
// tracks open/closed + tight/loose). After block parsing finishes we walk
// the tree, run inlines, and lower to the public md::Document AST that the
// renderer / streaming widget / conformance harness consume.
//
// Private to the markdown implementation; NOT installed.

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "maya/widget/agentty/markdown/ast.hpp"

namespace maya::md_detail::engine {

enum class BlockType : std::uint8_t {
    Document,
    BlockQuote,
    List,
    Item,
    Paragraph,
    Heading,      // ATX or setext (level set)
    CodeFence,    // ``` / ~~~
    CodeIndented,
    HtmlBlock,
    ThematicBreak,
    Table,        // GFM
};

struct CMBlock {
    BlockType type;
    CMBlock* parent = nullptr;
    std::vector<std::unique_ptr<CMBlock>> children;

    // open/closed bookkeeping (block phase)
    bool open = true;
    bool last_line_blank = false;
    bool last_line_checked = false;  // memo for ends_with_blank_line()
    int  start_line = 0;             // 1-based line where this block opened

    // raw text accumulator for leaf blocks (paragraph/heading/code/html)
    std::string text;

    // ── heading ──
    int level = 0;

    // ── code fence ──
    char fence_char = '`';
    int  fence_len = 0;
    int  fence_indent = 0;
    std::string info;            // info string (fenced) / lang
    int  html_block_kind = 0;    // 1..7, for HTML block end conditions

    // ── list / item ──
    bool ordered = false;
    char list_delim = '-';       // '-', '+', '*', '.', ')'
    int  start_num = 1;
    bool tight = true;
    int  item_indent = 0;        // content column for an item
    int  item_marker_offset = 0; // column where the marker begins
    bool has_task = false;
    bool task_checked = false;

    // ── table (GFM) ──
    std::vector<md::TableAlign> aligns;

    // parsed inline spans (filled in phase 2 for leaf text blocks)
    std::vector<md::Inline> inlines;

    explicit CMBlock(BlockType t) : type(t) {}
};

// Reference-link definitions collected during block parsing (§4.7).
using RefMap = std::unordered_map<std::string, md::LinkRef>;

// ── extensions ─────────────────────────────────────────────────────────────
// The engine core is pure CommonMark 0.31.2 (+ GFM tables). `extensions`
// turns on the non-spec layer the public parse_markdown() ships: GFM
// strikethrough/task-lists, ==highlight==, ~sub~/^sup^, [^footnote] refs,
// :emoji:/@mention/bare-URL text transforms, GitHub alerts, definition
// lists, footnote definitions, <details>. The conformance harness parses
// with extensions OFF so the spec number stays exact.
struct Options {
    bool extensions = false;
};

// ── public engine entry ──────────────────────────────────────────────────
// Parse `source` and lower to the public AST. This is what the new
// parse_markdown() delegates to.
[[nodiscard]] md::Document parse(std::string_view source, Options opts = {});

// ── phase-2 inline parser (cm_inline.cpp) ────────────────────────────────
// Parse a raw text run into inline spans, resolving references via `refs`.
[[nodiscard]] std::vector<md::Inline> parse_inlines(std::string_view text,
                                                    const RefMap& refs,
                                                    Options opts = {});

// AST-level extension post-pass (cm_inline.cpp / text_transform): rewrites
// Text nodes into :emoji:, @mentions, bare-URL links, and decodes the maya
// entity set. Runs over the engine's inline output when extensions are on.
void apply_text_extensions(std::vector<md::Inline>& spans);

// Flatten inline spans to their plain-text form (used to fill Link.text /
// Image.alt and to render heading anchors). Mirrors the spec's notion of
// the literal text content.
[[nodiscard]] std::string inline_plain_text(const std::vector<md::Inline>& spans);

} // namespace maya::md_detail::engine
