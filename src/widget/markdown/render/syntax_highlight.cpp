// syntax_highlight.cpp — the code-block tokeniser + memoised entry point.
//
// highlight_diff (per-line +/-/@@ coloring), highlight_code_impl (the
// language-aware byte scanner emitting StyledRuns + a line-number gutter for
// blocks ≥ 5 lines), and highlight_code (a thread-local FNV-keyed cache over
// (lang_tag, code)). Pulls its language tables from syntax_lang.cpp via
// syntax_internal.hpp; calls find_eol from the parser module.

#include <atomic>
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "maya/element/builder.hpp"
#include "maya/style/style.hpp"
#include "maya/widget/markdown.hpp"
#include "maya/widget/markdown/internal.hpp"

#include <atomic>
#include "maya/widget/markdown/spec_chars.hpp"
#include "maya/widget/markdown/syntax_internal.hpp"

namespace maya {

// find_eol lives in the parser module; the scanner body below calls it
// unqualified.
using ::maya::md_detail::find_eol;

// Language tables (syntax_lang.cpp) — bring into scope so highlight_code_impl
// calls them unqualified.
using ::maya::syntax_detail::LangId;
using ::maya::syntax_detail::detect_lang;
using ::maya::syntax_detail::classify_word;
using ::maya::syntax_detail::comment_style_for;
using ::maya::syntax_detail::features_for;

namespace {

// Char-class tables shared with the parser — one source of truth in
// markdown/spec_chars.hpp.
constexpr auto kPunctChar = md_detail::chars::kPunct;
constexpr auto kOpChar    = md_detail::chars::kOp;

} // anonymous

static Element highlight_diff(const std::string& code) {
    std::string out;
    out.reserve(code.size());
    std::vector<StyledRun> runs;

    size_t i = 0;
    size_t n = code.size();

    while (i < n) {
        size_t line_start = i;
        // Find end of line (memchr is SIMD-accelerated in glibc)
        size_t eol = find_eol(code.data(), i, n);
        bool has_nl = (eol < n);
        size_t line_end = has_nl ? eol + 1 : eol;

        std::string_view line{code.data() + line_start, eol - line_start};
        size_t out_start = out.size();
        out.append(code, line_start, line_end - line_start);

        Style sty = syntax::plain();
        if (!line.empty()) {
            if (line[0] == '+')                                    sty = syntax::diff_add();
            else if (line[0] == '-')                               sty = syntax::diff_del();
            else if (line.starts_with("@@"))                       sty = syntax::diff_hunk();
            else if (line.starts_with("diff ") ||
                     line.starts_with("index ") ||
                     line.starts_with("--- ") ||
                     line.starts_with("+++ "))                     sty = syntax::diff_meta();
        }
        runs.push_back({out_start, line_end - line_start, sty});
        i = line_end;
    }

    return Element{TextElement{
        .content = std::move(out),
        .style = syntax::plain(),
        // Same rationale as highlight_code_impl’s return: diff
        // lines are atomic and must not soft-wrap into the gutter.
        .wrap = TextWrap::NoWrap,
        .runs = std::move(runs),
    }};
}

static inline bool is_punct_char(char c) {
    return kPunctChar[static_cast<unsigned char>(c)];
}

static inline bool is_op_char(char c) {
    return kOpChar[static_cast<unsigned char>(c)];
}

// Incremental resume state for a streaming code fence. Caches the tokeniser
// output for the largest "safe" prefix of the fence — the bytes up to the
// last newline at which the lexer held no cross-line state (no open block
// comment or triple-quoted string). See highlight_code_from() for why this
// prefix is a byte-for-byte-stable frozen cache during streaming.
struct ResumeState {
    std::string            lang_tag;
    std::size_t            ground_off = 0;  // safe resume offset into `code`
    std::string            pre_out;         // tokeniser out for [0, ground_off)
    std::vector<StyledRun> pre_runs;        // runs for [0, ground_off)
};

static Element highlight_code_from(const std::string& code,
                                   const std::string& lang_tag,
                                   ResumeState* resume,
                                   bool with_gutter = true);

// highlight_code_from — tokeniser + gutter pass, with an optional incremental
// resume path (see ResumeState). When `resume` is non-null and holds a valid
// cached prefix of `code`, the tokeniser seeds from that prefix and re-scans
// only the tail; otherwise it tokenises the whole `code`. Either way it
// refreshes `resume` with the new safe prefix. The rendered Element is
// identical to a full re-tokenise — load-bearing, because the reveal
// animation clips by byte offset.
static Element highlight_code_from(const std::string& code,
                                   const std::string& lang_tag,
                                   ResumeState* resume,
                                   bool with_gutter) {
    LangId lang = detect_lang(lang_tag);

    // Special case: diff gets its own highlighter
    if (lang == LangId::Diff) return highlight_diff(code);

    auto cs = comment_style_for(lang);
    auto feat = features_for(lang);

    std::string out;
    std::vector<StyledRun> runs;

    // ── Incremental resume ──────────────────────────────────────────
    // If the cache's frozen prefix is still a genuine prefix of `code`,
    // seed out/runs from it and begin scanning at ground_off. The prefix
    // is validated byte-for-byte so a fence that was edited/replaced (not
    // purely appended) safely falls back to a full re-tokenise.
    size_t start_i = 0;
    if (resume && resume->lang_tag == lang_tag && resume->ground_off > 0 &&
        resume->ground_off <= code.size() &&
        resume->pre_out.size() >= resume->ground_off &&
        // The cached prefix bytes must equal the current code's prefix
        // (guards against a fence being edited/replaced rather than appended).
        std::equal(code.begin(),
                   code.begin() + static_cast<std::ptrdiff_t>(resume->ground_off),
                   resume->pre_out.begin())) {
        out = resume->pre_out;
        runs = resume->pre_runs;
        start_i = resume->ground_off;
        out.reserve(code.size());
    } else {
        out.reserve(code.size());
    }

    size_t i = start_i;
    size_t n = code.size();
    // ground_off: byte offset of the last newline seen while the lexer holds
    // no cross-line state — the safe point to resume from next token. Seeded
    // from the resume cache (start_i) since everything before it is ground.
    std::size_t ground_off = start_i;

    auto emit = [&](size_t start, size_t byte_len, Style sty) {
        if (byte_len == 0) return;
        runs.push_back({start, byte_len, sty});
    };

    while (i < n) {
        char ch = code[i];

        // ── Newline ──────────────────────────────────────────────────
        if (ch == '\n') {
            size_t s = out.size();
            out += '\n';
            emit(s, 1, syntax::plain());
            ++i;
            // Reaching a newline in the main loop means the lexer is in
            // ground state (every multi-line construct is consumed to its
            // close within its own branch below, so control only returns
            // here between constructs). Record this as a safe resume point.
            ground_off = i;
            continue;
        }

        // ── Whitespace ───────────────────────────────────────────────
        if (ch == ' ' || ch == '\t') {
            size_t s = out.size();
            size_t j = i;
            while (j < n && (code[j] == ' ' || code[j] == '\t')) ++j;
            out.append(code, i, j - i);
            emit(s, j - i, syntax::plain());
            i = j;
            continue;
        }

        // ── Preprocessor: #include, #define, etc. ────────────────────
        if (feat.preprocessor && ch == '#') {
            // Check if at start of line (or start of code)
            bool at_line_start = (i == 0 || code[i - 1] == '\n');
            if (at_line_start) {
                size_t s = out.size();
                size_t j = find_eol(code.data(), i, n);
                out.append(code, i, j - i);
                emit(s, j - i, syntax::preproc());
                i = j;
                continue;
            }
        }

        // ── Line comment: // or # or -- ──────────────────────────────
        if (cs.line && !std::string_view(cs.line).empty()) {
            std::string_view lc{cs.line};
            if (code.compare(i, lc.size(), lc) == 0) {
                size_t s = out.size();
                size_t j = find_eol(code.data(), i, n);
                out.append(code, i, j - i);
                emit(s, j - i, syntax::comment());
                i = j;
                continue;
            }
        }
        if (cs.hash_comment && ch == '#') {
            size_t s = out.size();
            size_t j = find_eol(code.data(), i, n);
            out.append(code, i, j - i);
            emit(s, j - i, syntax::comment());
            i = j;
            continue;
        }

        // ── Block comment: /* ... */, <!-- ... -->, {- ... -} ────────
        if (cs.block_open) {
            std::string_view bo{cs.block_open};
            std::string_view bc{cs.block_close};
            if (code.compare(i, bo.size(), bo) == 0) {
                size_t s = out.size();
                size_t j = i + bo.size();
                while (j + bc.size() <= n &&
                       code.compare(j, bc.size(), bc) != 0)
                    ++j;
                if (j + bc.size() <= n) j += bc.size();
                out.append(code, i, j - i);
                emit(s, j - i, syntax::comment());
                i = j;
                continue;
            }
        }

        // ── Decorators/attributes: @decorator, #[attr] ──────────────
        if (feat.decorators && ch == '@') {
            size_t s = out.size();
            size_t j = i + 1;
            while (j < n && (std::isalnum(static_cast<unsigned char>(code[j])) ||
                              code[j] == '_' || code[j] == '.'))
                ++j;
            out.append(code, i, j - i);
            emit(s, j - i, syntax::attr());
            i = j;
            continue;
        }

        // ── Rust lifetime: 'a, 'static ──────────────────────────────
        if (feat.lifetime && ch == '\'' &&
            i + 1 < n && std::isalpha(static_cast<unsigned char>(code[i + 1]))) {
            size_t s = out.size();
            size_t j = i + 1;
            while (j < n && (std::isalnum(static_cast<unsigned char>(code[j])) ||
                              code[j] == '_'))
                ++j;
            out.append(code, i, j - i);
            emit(s, j - i, syntax::type());
            i = j;
            continue;
        }

        // ── Shell variables: $VAR, ${VAR}, $(...) ────────────────────
        if (feat.shell_vars && ch == '$' && i + 1 < n) {
            size_t s = out.size();
            size_t j = i + 1;
            if (code[j] == '{') {
                // ${VAR}
                ++j;
                while (j < n && code[j] != '}') ++j;
                if (j < n) ++j;
            } else if (code[j] == '(') {
                // $(...) — just highlight the $( and )
                out.append(code, i, 2);
                emit(s, 2, syntax::shellvar());
                i += 2;
                continue;
            } else if (std::isalpha(static_cast<unsigned char>(code[j])) ||
                       code[j] == '_') {
                while (j < n && (std::isalnum(static_cast<unsigned char>(code[j])) ||
                                  code[j] == '_'))
                    ++j;
            } else {
                // $? $# $@ etc.
                ++j;
            }
            out.append(code, i, j - i);
            emit(s, j - i, syntax::shellvar());
            i = j;
            continue;
        }

        // ── Triple-quoted strings: """...""" / '''...''' ─────────────
        if (feat.triple_quote_strings &&
            (ch == '"' || ch == '\'') &&
            i + 2 < n && code[i + 1] == ch && code[i + 2] == ch) {
            char q = ch;
            size_t s = out.size();
            size_t j = i + 3;
            while (j + 2 < n) {
                if (code[j] == '\\') { j += 2; continue; }
                if (code[j] == q && code[j + 1] == q && code[j + 2] == q) {
                    j += 3;
                    break;
                }
                ++j;
            }
            if (j + 2 >= n && !(j >= 3 && code[j-1] == q && code[j-2] == q && code[j-3] == q))
                j = n;
            out.append(code, i, j - i);
            emit(s, j - i, syntax::str());
            i = j;
            continue;
        }

        // ── Backtick template literals: `...${...}...` ──────────────
        if (feat.backtick_strings && ch == '`') {
            size_t s = out.size();
            size_t j = i + 1;
            while (j < n && code[j] != '`') {
                if (code[j] == '\\' && j + 1 < n) { j += 2; continue; }
                ++j;
            }
            if (j < n) ++j; // consume closing `
            out.append(code, i, j - i);
            emit(s, j - i, syntax::str());
            i = j;
            continue;
        }

        // ── String literals: "..." ───────────────────────────────────
        if (ch == '"') {
            size_t s = out.size();
            size_t j = i + 1;
            while (j < n && code[j] != '\n') {
                if (code[j] == '\\' && j + 1 < n) { j += 2; continue; }
                if (code[j] == '"') { ++j; break; }
                ++j;
            }
            out.append(code, i, j - i);
            emit(s, j - i, syntax::str());
            i = j;
            continue;
        }

        // ── Char literal or string: '...' ────────────────────────────
        if (ch == '\'') {
            if (feat.char_literals) {
                // C-style char: 'x' or '\n' — short
                size_t s = out.size();
                size_t j = i + 1;
                if (j < n && code[j] == '\\') j += 2;
                else if (j < n) ++j;
                if (j < n && code[j] == '\'') ++j;
                out.append(code, i, j - i);
                emit(s, j - i, syntax::str());
                i = j;
                continue;
            }
            // Treat as string in Ruby, Python (single-quoted), etc.
            size_t s = out.size();
            size_t j = i + 1;
            while (j < n && code[j] != '\n') {
                if (code[j] == '\\' && j + 1 < n) { j += 2; continue; }
                if (code[j] == '\'') { ++j; break; }
                ++j;
            }
            out.append(code, i, j - i);
            emit(s, j - i, syntax::str());
            i = j;
            continue;
        }

        // ── Ruby/Elixir atom: :symbol ────────────────────────────────
        if (feat.colon_atom && ch == ':' &&
            i + 1 < n && std::isalpha(static_cast<unsigned char>(code[i + 1]))) {
            size_t s = out.size();
            size_t j = i + 1;
            while (j < n && (std::isalnum(static_cast<unsigned char>(code[j])) ||
                              code[j] == '_' || code[j] == '?' || code[j] == '!'))
                ++j;
            out.append(code, i, j - i);
            emit(s, j - i, syntax::constant());
            i = j;
            continue;
        }

        // ── Number: 0x..., 0b..., 0o..., digits[.digits][e...] ──────
        if (std::isdigit(static_cast<unsigned char>(ch)) ||
            (ch == '.' && i + 1 < n &&
             std::isdigit(static_cast<unsigned char>(code[i + 1])))) {
            size_t s = out.size();
            size_t j = i;
            if (ch == '0' && j + 1 < n) {
                char next = code[j + 1];
                if (next == 'x' || next == 'X') {
                    j += 2;
                    while (j < n && (std::isxdigit(static_cast<unsigned char>(code[j])) ||
                                      code[j] == '_'))
                        ++j;
                } else if (next == 'b' || next == 'B') {
                    j += 2;
                    while (j < n && (code[j] == '0' || code[j] == '1' || code[j] == '_'))
                        ++j;
                } else if (next == 'o' || next == 'O') {
                    j += 2;
                    while (j < n && ((code[j] >= '0' && code[j] <= '7') || code[j] == '_'))
                        ++j;
                } else goto decimal;
            } else {
            decimal:
                while (j < n && (std::isdigit(static_cast<unsigned char>(code[j])) ||
                                  code[j] == '_'))
                    ++j;
                if (j < n && code[j] == '.' && j + 1 < n &&
                    std::isdigit(static_cast<unsigned char>(code[j + 1]))) {
                    ++j;
                    while (j < n && (std::isdigit(static_cast<unsigned char>(code[j])) ||
                                      code[j] == '_'))
                        ++j;
                }
                // Exponent
                if (j < n && (code[j] == 'e' || code[j] == 'E')) {
                    ++j;
                    if (j < n && (code[j] == '+' || code[j] == '-')) ++j;
                    while (j < n && std::isdigit(static_cast<unsigned char>(code[j]))) ++j;
                }
            }
            // Number suffix: f, u, l, i32, etc.
            while (j < n && (std::isalpha(static_cast<unsigned char>(code[j])) ||
                              code[j] == '_'))
                ++j;
            out.append(code, i, j - i);
            emit(s, j - i, syntax::num());
            i = j;
            continue;
        }

        // ── Identifier / keyword / type / function ───────────────────
        if (std::isalpha(static_cast<unsigned char>(ch)) || ch == '_') {
            size_t j = i;
            while (j < n && (std::isalnum(static_cast<unsigned char>(code[j])) ||
                              code[j] == '_'))
                ++j;
            // Rust macros: name!
            if (lang == LangId::Rust && j < n && code[j] == '!')
                ++j;

            std::string_view word{code.data() + i, j - i};
            bool is_fn_call = (j < n && code[j] == '(');

            size_t s = out.size();
            out.append(code, i, j - i);

            auto wc = classify_word(word, lang);
            if (wc.constant)      emit(s, j - i, syntax::constant());
            else if (wc.keyword)  emit(s, j - i, syntax::kw());
            else if (wc.type)     emit(s, j - i, syntax::type());
            else if (is_fn_call)  emit(s, j - i, syntax::fn());
            else if (!word.empty() &&
                     std::isupper(static_cast<unsigned char>(word[0])) &&
                     word.size() > 1)
                                  emit(s, j - i, syntax::type());
            else                  emit(s, j - i, syntax::plain());

            i = j;
            continue;
        }

        // ── Multi-char operators: =>, ->, ::, |>, <-, ==, != etc. ───
        if (is_op_char(ch)) {
            size_t s = out.size();
            size_t j = i;
            // Consume runs of operator chars (max 3 for things like >>=)
            while (j < n && is_op_char(code[j]) && (j - i) < 3) ++j;
            out.append(code, i, j - i);
            emit(s, j - i, syntax::op());
            i = j;
            continue;
        }

        // ── Punctuation ──────────────────────────────────────────────
        if (is_punct_char(ch)) {
            size_t s = out.size();
            out += ch;
            emit(s, 1, syntax::punct());
            ++i;
            continue;
        }

        // ── Anything else — plain ────────────────────────────────────
        {
            size_t s = out.size();
            // Consume UTF-8 continuation bytes together
            size_t j = i + 1;
            if (static_cast<unsigned char>(ch) >= 0x80) {
                while (j < n && (static_cast<unsigned char>(code[j]) & 0xC0) == 0x80) ++j;
            }
            out.append(code, i, j - i);
            emit(s, j - i, syntax::plain());
            i = j;
        }
    }

    // ── Refresh the incremental resume cache ─────────────────────────
    // At this point `out` still holds the code bytes verbatim (the gutter
    // pass below has not run yet), so out[0..ground_off) == code[0..ground_off).
    // Snapshot the ground-state prefix + its runs so the next (longer) token
    // can resume from here instead of re-tokenising byte 0. Runs are copied
    // up to the last one fully inside [0, ground_off); a run straddling the
    // boundary (only possible for a construct still open at end, which by
    // definition sits AFTER ground_off) is excluded.
    if (resume) {
        resume->lang_tag = lang_tag;
        resume->ground_off = ground_off;
        resume->pre_out.assign(out.data(), ground_off);
        resume->pre_runs.clear();
        resume->pre_runs.reserve(runs.size());
        for (const auto& r : runs) {
            if (r.byte_offset + r.byte_length <= ground_off)
                resume->pre_runs.push_back(r);
            else
                break;  // runs are emitted in increasing offset order
        }
    }

    // ── Gutter pass: prepend a right-aligned line-number column to each
    //    line. Height of the rendered block is unchanged — exactly one
    //    line out per line in, so monotonicity is preserved (a code
    //    fence that committed at K rows still commits at K rows).
    //
    //    Skip on small blocks (< 5 lines): the gutter is visual noise
    //    on a 2-line snippet, where the line numbers are obvious from
    //    position alone. The threshold matches what users intuitively
    //    expect — a "code listing" rather than a "snippet".
    {
        auto count_lines = [](std::string_view s) -> int {
            int n = 0;
            for (char c : s) if (c == '\n') ++n;
            if (!s.empty() && s.back() != '\n') ++n;
            return n;
        };
        const int line_count = count_lines(out);
        // Gutter presence MUST be stable across a code block's streamed
        // lifetime. A min-lines threshold (e.g. "only gutter blocks of
        // >=5 lines") flips the gutter ON the moment a streaming block
        // crosses the threshold, re-indenting every line including rows
        // that already scrolled into native scrollback (an immutable
        // committed-row rewrite — same corruption class as the gutter
        // WIDTH growth handled below). Emit the gutter unconditionally so
        // a block that starts at 1 line and streams to N never gains or
        // loses its line-number column mid-stream. (reveal_scrollback_test
        // cb-fold scenario caught the threshold flip at the 4→5 boundary.)
        constexpr int kGutterMinLines = 1;
        if (with_gutter && line_count >= kGutterMinLines) {
            // Width of the line-number column. log10-style.
            //
            // STABILITY CONTRACT: a code block streamed incrementally
            // grows line-by-line. If w_digits were a tight fit of the
            // CURRENT line_count, crossing a power-of-ten boundary
            // (9→10, 99→100) would widen the gutter by one column and
            // re-indent EVERY line of the block — including rows that
            // have already scrolled into the terminal's native
            // scrollback, which the inline renderer can no longer
            // rewrite. The result is a committed-row rewrite: maya's
            // diff tries to shift a scrolled-off row right by one and
            // strands a corrupted / duplicated copy (reproduced by
            // agentty's reveal_scrollback_test cb-fold scenario at the
            // 9→10 boundary). Reserve a fixed FLOOR (3 digits ⇒ stable
            // up to 999 lines) so the gutter width is constant across
            // the entire streamed lifetime of any realistic block;
            // blocks longer than that are auto-folded to one row by the
            // host before they could cross the next boundary. Height
            // monotonicity is unaffected (still one line out per line
            // in); this only fixes the HORIZONTAL stability the
            // committed-row contract requires.
            constexpr int kGutterMinDigits = 3;
            int w_digits = kGutterMinDigits;
            {
                int need = 1;
                for (int v = line_count; v >= 10; v /= 10) ++need;
                if (need > w_digits) w_digits = need;
            }
            constexpr std::string_view kSep =
                " \xe2\x94\x82 ";  // " │ " (U+2502 = ~3 cells wide visually with pad)
            const std::size_t kSepBytes = kSep.size();
            const Style& gstyle = syntax::gutter();

            // Pre-split runs at newline boundaries so no run spans
            // a line break — block comments in C-style languages emit
            // a single run for the whole comment, including embedded
            // \n. Splitting up-front lets the line-shift remap below
            // be a simple per-line offset add rather than a byte-by-
            // byte run split-walk.
            std::vector<StyledRun> runs_split;
            runs_split.reserve(runs.size() * 2);
            for (const auto& r : runs) {
                std::size_t cur = r.byte_offset;
                const std::size_t end = cur + r.byte_length;
                while (cur < end) {
                    std::size_t nl = out.find('\n', cur);
                    std::size_t seg_end = (nl == std::string::npos || nl >= end)
                                        ? end : nl;
                    if (seg_end > cur) {
                        runs_split.push_back({cur, seg_end - cur, r.style});
                    }
                    if (nl != std::string::npos && nl < end) {
                        runs_split.push_back({nl, 1, r.style});
                        cur = nl + 1;
                    } else {
                        break;
                    }
                }
            }

            // Build line-start offset table for the original `out`.
            std::vector<std::size_t> line_starts;
            line_starts.reserve(static_cast<std::size_t>(line_count) + 1);
            line_starts.push_back(0);
            for (std::size_t k = 0; k < out.size(); ++k) {
                if (out[k] == '\n') line_starts.push_back(k + 1);
            }
            // Sentinel for the binary-search remap below: any run
            // offset is < out.size() < line_starts.back() + ε.

            // Emit out2 with per-line gutter prefixes.
            std::string out2;
            out2.reserve(out.size() + line_starts.size()
                         * (static_cast<std::size_t>(w_digits) + kSepBytes));
            std::vector<StyledRun> runs2;
            runs2.reserve(runs_split.size() + line_starts.size() * 2);

            for (std::size_t li = 0; li < line_starts.size(); ++li) {
                // Gutter for line li+1.
                char buf[24];
                int ndig = std::snprintf(buf, sizeof(buf), "%*zu",
                                      w_digits, li + 1);
                runs2.push_back({out2.size(), static_cast<std::size_t>(ndig), gstyle});
                out2.append(buf, static_cast<std::size_t>(ndig));
                runs2.push_back({out2.size(), kSepBytes, gstyle});
                out2.append(kSep);

                // Line content.
                std::size_t s = line_starts[li];
                std::size_t e = (li + 1 < line_starts.size())
                              ? line_starts[li + 1]
                              : out.size();
                out2.append(out.data() + s, e - s);
            }

            // Remap each split run.
            const std::size_t per_line_shift =
                static_cast<std::size_t>(w_digits) + kSepBytes;
            for (const auto& r : runs_split) {
                // Find which line this run sits on. line_starts is
                // sorted; r.byte_offset is in [line_starts[i],
                // line_starts[i+1]) for the line index i.
                auto it = std::upper_bound(
                    line_starts.begin(), line_starts.end(), r.byte_offset);
                std::size_t line_idx = static_cast<std::size_t>(
                    (it - line_starts.begin())) - 1;
                std::size_t shift = (line_idx + 1) * per_line_shift;
                runs2.push_back({r.byte_offset + shift,
                                 r.byte_length, r.style});
            }

            // Sort by offset for traversal-friendly downstream use.
            std::sort(runs2.begin(), runs2.end(),
                [](const StyledRun& a, const StyledRun& b) {
                    return a.byte_offset < b.byte_offset;
                });

            out = std::move(out2);
            runs = std::move(runs2);
        }
    }

    return Element{TextElement{
        .content = std::move(out),
        .style = syntax::plain(),
        // Code blocks must not soft-wrap: a long identifier or
        // command line is one logical unit, and breaking it at a
        // column boundary spills continuation bytes back to column
        // 0 — directly under (or through) the line-number gutter.
        // Visible symptom: the next line’s gutter digit lands
        // *inside* the wrapped tail of the previous line. Hand the
        // overflow to the parent box’s clip rect instead; the
        // CodeBlock builder in render.cpp opts into
        // Overflow::Hidden so the tail simply truncates at the
        // right border.
        .wrap = TextWrap::NoWrap,
        .runs = std::move(runs),
    }};
}

// ============================================================================
// highlight_code — memoising wrapper
// ============================================================================
// The highlighter is pure: same `(lang_tag, code)` produces the same
// Element every call. For long agent transcripts the same code block is
// re-rendered tens or hundreds of times — once per inline frame — so a
// content-keyed cache turns the recurring cost into a hash lookup.
//
// Cache is thread_local, and that is LOAD-BEARING rather than defensive.
// This comment used to read "renderer is single-threaded per app instance in
// practice; this avoids contention if the host ever spawns a side renderer",
// which is wrong in a way worth spelling out: maya already renders on a
// second thread, and has for as long as streaming markdown has existed.
// StreamingMarkdown::spawn_async_worker_ (src/widget/markdown/streaming/
// async.cpp) hands a full re-parse to maya::exec (the runtime pool), and that job
// calls md_block_to_element on every block — which lands here.
//
// So this cache, the resume state below, and the layout/paint scratch pools
// the block render touches are all genuinely per-thread: the loop thread and
// the async worker each get their own, which is the CORRECT semantics and not
// a contention hedge. In particular they must NOT become jaal's
// loop_bound<T>: that would abort the worker on its first code fence.
//
// FIFO-evicts in halves when capacity is exceeded so we never grow unbounded
// across long sessions. Keying combines a 64-bit FNV-1a hash of `code` with a
// hash of `lang_tag`; the cached Element is returned by copy, which for the
// typical TextElement case is a string + runs vector — cheap relative to
// running the highlighter.
namespace {
// The host's on/off switch. Read on the render path, written from the UI
// thread — same ownership model as the markdown palette.
std::atomic<bool>& syntax_on_slot() noexcept {
    static std::atomic<bool> on{true};
    return on;
}
}  // namespace

void set_syntax_highlighting(bool on) noexcept {
    syntax_on_slot().store(on, std::memory_order_relaxed);
}

bool syntax_highlighting() noexcept {
    return syntax_on_slot().load(std::memory_order_relaxed);
}

static Element highlight_code(const std::string& code, const std::string& lang_tag) {
    // Off: one flat colour for the whole body. The fence, the language tag
    // and the layout are untouched — only the tokenised colour inside the
    // block goes away, which is exactly what the setting says.
    //
    // Checked before the cache, not inside it, because the cache is keyed on
    // (code, lang) and would otherwise serve a highlighted body after the
    // switch flipped.
    if (!syntax_highlighting()) {
        return Element{TextElement{.content = code,
                                   .style   = Style{}.with_fg(colors::code_fg())}};
    }
    struct CacheEntry {
        uint64_t key;
        Element  elem;
    };
    static constexpr std::size_t kMaxEntries = 256;
    thread_local std::vector<CacheEntry> cache;

    auto fnv1a = [](std::string_view sv, uint64_t seed) noexcept -> uint64_t {
        uint64_t h = seed;
        for (unsigned char c : sv) {
            h ^= c;
            h *= 1099511628211ULL;
        }
        return h;
    };
    const uint64_t key = fnv1a(code, fnv1a(lang_tag, 14695981039346656037ULL));

    for (auto& e : cache) {
        if (e.key == key) return e.elem;
    }

    // ── Streaming resume ─────────────────────────────────────
    // On a content-key miss (which happens every token while a fence grows,
    // because the key hashes the whole body), hand highlight_code_from a
    // per-language resume cache. It seeds the tokeniser from the last
    // ground-state prefix and re-scans only the newly-arrived tail — turning
    // the per-token cost from O(fence) into O(last block of lines). The
    // cache is keyed per language and validated byte-for-byte inside
    // highlight_code_from, so a switch to a different fence (or an edit that
    // isn't a pure append) transparently falls back to a full tokenise.
    thread_local ResumeState resume;
    Element elem = highlight_code_from(code, lang_tag, &resume);

    if (cache.size() >= kMaxEntries) {
        cache.erase(cache.begin(), cache.begin() + kMaxEntries / 2);
    }
    cache.push_back({key, elem});
    return elem;
}

// ── Token spans per line, for tool bodies ───────────────────────────────
// Same tokeniser as markdown fences, without the gutter. Offsets are relative
// to each line. Empty when the language is unknown or highlighting is off.
std::vector<std::vector<StyledRun>> syntax_line_runs(std::string_view code,
                                                     std::string_view lang) {
    std::vector<std::vector<StyledRun>> lines;
    if (!syntax_highlighting() || code.empty()) return lines;
    const LangId id = detect_lang(lang);
    if (id == LangId::Unknown || id == LangId::Diff) return lines;

    struct Entry { uint64_t key; std::vector<std::vector<StyledRun>> v; };
    thread_local std::vector<Entry> cache;
    uint64_t key = 14695981039346656037ULL;
    for (unsigned char c : lang) { key ^= c; key *= 1099511628211ULL; }
    key ^= 0xffu; key *= 1099511628211ULL;
    for (unsigned char c : code) { key ^= c; key *= 1099511628211ULL; }
    for (const auto& e : cache) if (e.key == key) return e.v;

    Element el = highlight_code_from(std::string{code}, std::string{lang},
                                     nullptr, /*with_gutter=*/false);
    const auto* t = std::get_if<TextElement>(&el.inner);
    if (!t) return lines;
    const std::string& out = t->content;

    std::size_t pos = 0;
    for (;;) {
        const auto nl  = out.find('\n', pos);
        const auto end = nl == std::string::npos ? out.size() : nl;
        std::vector<StyledRun> line;
        for (const auto& r : t->runs) {
            const std::size_t rs = r.byte_offset, re = rs + r.byte_length;
            if (re <= pos || rs >= end) continue;
            const std::size_t a = std::max(rs, pos), b = std::min(re, end);
            if (b > a) line.push_back({a - pos, b - a, r.style});
        }
        std::sort(line.begin(), line.end(),
                  [](const StyledRun& x, const StyledRun& y) {
                      return x.byte_offset < y.byte_offset;
                  });
        lines.push_back(std::move(line));
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }

    if (cache.size() >= 64) cache.erase(cache.begin(), cache.begin() + 32);
    cache.push_back({key, lines});
    return lines;
}

// ── Cross-TU bridge ──────────────────────────────────────────────────
namespace md_detail {
Element highlight_code(const std::string& code, const std::string& lang_tag) {
    return ::maya::highlight_code(code, lang_tag);
}
} // namespace md_detail

} // namespace maya
