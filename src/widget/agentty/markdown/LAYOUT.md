# markdown widget — component layout

The original ~5200-line monolithic `widget/markdown.cpp` is split into three
modules under `src/widget/agentty/markdown/`, each a directory of small
single-responsibility TUs.

This file used to be `src/widget/agentty/markdown.cpp`: a comment-only translation
unit that existed purely to document the layout. Compiling it produced an
object with no symbols, and `ranlib` warned about it on every archive link
(`libmaya.a(markdown.cpp.o) has no symbols`). The documentation was the only
payload, so it lives here as documentation instead — same anchor, no empty
object file.

## parser/ — markdown source → `md::Document` AST

| TU | Responsibility |
|----|----------------|
| `parser_inline.cpp` | inline parsing |
| `parser_block.cpp` | block parsing, ref-def collector |
| `text_transform.cpp` | `Text` post-pass: entities / emoji / URLs / mentions |

## engine/ — spec-faithful CommonMark core

| TU | Responsibility |
|----|----------------|
| `cm_block.cpp` | phase 1: block structure |
| `cm_inline.cpp` | phase 2: inline structure |

## render/ — `md::Document` AST → `maya::Element`

| TU | Responsibility |
|----|----------------|
| `render_block.cpp` | blocks + tables → `Element`; palette accessors |
| `render_inline.cpp` | inlines → `Element` |
| `syntax_lang.cpp` | language detection |
| `syntax_highlight.cpp` | language-aware code-block highlighter |
| `highlight.cpp` | span highlighting |

## streaming/ — incremental / live rendering

| TU | Responsibility |
|----|----------------|
| `markdown_memo.cpp` | `markdown()` LRU memo |
| `boundary.cpp` | safe commit boundaries in a partial stream |
| `commit.cpp` | commit of settled prefix |
| `render_tail.cpp` | the unsettled tail |
| `build.cpp` | `StreamingMarkdown` assembly |
| `reveal_fx.cpp` | reveal animation |
| `folding.cpp` | fold/collapse |
| `async.cpp` | off-thread build |

## Shared headers

- `<maya/widget/agentty/markdown/internal.hpp>` — cross-TU declarations
  (`maya::md_detail`), not installed
- `<maya/widget/agentty/markdown/ast.hpp>` — the AST
- `spec_chars.hpp` — char-class tables

Every public symbol in `<maya/widget/agentty/markdown.hpp>` is defined across the TUs
above.
