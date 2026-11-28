// parser_block.cpp — reference-definition collection + the public
// parse_markdown entry point.
//
// The block/inline parsing is done entirely by the spec-faithful engine
// (engine/cm_block.cpp + cm_inline.cpp). What remains here is the small set
// of helpers the streaming widget still calls cross-TU: collect_ref_defs /
// normalize_ref_label (link-reference-definition extraction), find_eol, and
// the parse_markdown / parse_markdown_impl entry points that delegate to the
// engine (extensions on).

#include "maya/widget/agentty/markdown.hpp"

#include <cctype>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "maya/widget/agentty/markdown/internal.hpp"
#include "maya/widget/agentty/markdown/parser_internal.hpp"
#include "maya/widget/agentty/markdown/engine/cm_engine.hpp"

namespace maya {

// trim / find_eol come from parser_internal.
using namespace ::maya::md_detail::parser_detail;

// Normalize a reference label: lowercase + collapse whitespace runs.
static std::string normalize_ref_label(std::string_view label) {
    std::string out;
    out.reserve(label.size());
    bool prev_space = false;
    for (char c : label) {
        unsigned char uc = static_cast<unsigned char>(c);
        bool sp = std::isspace(uc);
        if (sp) {
            if (!prev_space && !out.empty()) out += ' ';
            prev_space = true;
        } else {
            out += static_cast<char>(std::tolower(uc));
            prev_space = false;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// Extract reference-link definitions `[label]: url "title"` from source.
// Writes them to `defs` and returns a copy of `source` with the matched
// lines removed (replaced by blank lines so line-oriented parsers still see
// block boundaries).  Lines that look like footnote defs (`[^label]: …`)
// are NOT treated as reference defs.
static std::string collect_ref_defs(std::string_view source,
                                    std::unordered_map<std::string, md::LinkRef>& defs) {
    // Early-out: a `[label]: url` line REQUIRES a literal '[' somewhere
    // in the source. The committed prose chunks coming from the streaming
    // markdown widget are most-of-the-time bracket-free paragraphs, so the
    // per-line trim + label parse below is wasted work on those.
    if (source.find('[') == std::string_view::npos) {
        return std::string{source};
    }

    std::string out;
    out.reserve(source.size());

    size_t i = 0;
    while (i < source.size()) {
        size_t nl = source.find('\n', i);
        size_t line_end = (nl == std::string_view::npos) ? source.size() : nl;
        auto line = source.substr(i, line_end - i);
        auto trimmed = trim(line);

        bool consumed = false;
        if (trimmed.size() >= 4 && trimmed[0] == '[' && trimmed[1] != '^') {
            size_t close = trimmed.find("]:");
            if (close != std::string_view::npos && close > 1) {
                auto label_sv = trimmed.substr(1, close - 1);
                auto rest = trim(trimmed.substr(close + 2));
                std::string url;
                size_t p = 0;
                if (!rest.empty() && rest[0] == '<') {
                    size_t gt = rest.find('>', 1);
                    if (gt != std::string_view::npos) {
                        url = std::string{rest.substr(1, gt - 1)};
                        p = gt + 1;
                    }
                } else {
                    size_t e = 0;
                    while (e < rest.size() &&
                           !std::isspace(static_cast<unsigned char>(rest[e]))) ++e;
                    url = std::string{rest.substr(0, e)};
                    p = e;
                }
                if (!url.empty()) {
                    while (p < rest.size() &&
                           std::isspace(static_cast<unsigned char>(rest[p]))) ++p;
                    std::string title;
                    if (p < rest.size() &&
                        (rest[p] == '"' || rest[p] == '\'' || rest[p] == '(')) {
                        char open_q = rest[p];
                        char close_q = (open_q == '(') ? ')' : open_q;
                        ++p;
                        while (p < rest.size() && rest[p] != close_q) {
                            title += rest[p++];
                        }
                    }
                    auto key = normalize_ref_label(label_sv);
                    if (!key.empty()) {
                        defs.emplace(std::move(key),
                                     md::LinkRef{std::move(url), std::move(title)});
                        consumed = true;
                    }
                }
            }
        }

        if (!consumed) {
            out.append(line);
        }
        if (nl != std::string_view::npos) out += '\n';

        if (nl == std::string_view::npos) break;
        i = nl + 1;
    }
    return out;
}

md::Document parse_markdown(std::string_view source) {
    // The spec-faithful engine is the one and only parser. Public entry →
    // extensions ON (GFM strikethrough/task-lists, ==highlight==, ~sub~/^sup^,
    // [^footnote] refs/defs, :emoji:/@mention/bare-URL transforms, alerts).
    // The conformance harness parses the pure core (engine::parse, extensions
    // off) directly, so the spec number stays exact.
    return ::maya::md_detail::engine::parse(source, {.extensions = true});
}

// ── Cross-TU bridge ──────────────────────────────────────────────────
// Still called by the streaming widget; both parse entry points delegate to
// the engine so the tail renders identically to the committed prefix.
namespace md_detail {
std::size_t find_eol(const char* data, std::size_t start, std::size_t end) noexcept {
    return ::maya::md_detail::parser_detail::find_eol(data, start, end);
}
std::string collect_ref_defs(std::string_view source,
                             std::unordered_map<std::string, md::LinkRef>& defs) {
    return ::maya::collect_ref_defs(source, defs);
}
md::Document parse_markdown_impl(std::string_view source, int /*depth*/) {
    return ::maya::md_detail::engine::parse(source, {.extensions = true});
}
} // namespace md_detail

} // namespace maya
