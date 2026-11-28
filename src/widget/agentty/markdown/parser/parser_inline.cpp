// parser_inline.cpp — the reference-link TLS slot + the inline-parse bridge.
//
// Inline parsing is done entirely by the engine (engine/cm_inline.cpp). What
// remains here is the thread_local reference-definition slot + RefDefsScope
// (declared in parser_internal.hpp, used by the streaming widget), and the
// cross-TU parse_inlines bridge that delegates to the engine (extensions on).

#include "maya/widget/agentty/markdown.hpp"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "maya/widget/agentty/markdown/parser_internal.hpp"
#include "maya/widget/agentty/markdown/engine/cm_engine.hpp"

namespace maya {

// Reference-link map (thread_local) — declared extern in parser_internal.hpp.
// RefDefsScope stashes the active Document's ref_defs here; kept for the
// streaming widget which constructs it around its parse calls.
namespace md_detail {
thread_local const std::unordered_map<std::string, md::LinkRef>*
    tls_ref_defs = nullptr;

RefDefsScope::RefDefsScope(const std::unordered_map<std::string, md::LinkRef>* p) noexcept
    : prev(tls_ref_defs) { tls_ref_defs = p; }
RefDefsScope::~RefDefsScope() { tls_ref_defs = prev; }
} // namespace md_detail

// ── Cross-TU bridge ──────────────────────────────────────────────────
namespace md_detail {
std::vector<md::Inline> parse_inlines(std::string_view text) {
    // Unified on the engine's inline parser (extensions on). The streaming
    // tail has no resolved ref-map mid-stream; refs resolve on commit when
    // the committed prefix is re-parsed by the engine.
    static const engine::RefMap kNoRefs;
    return engine::parse_inlines(text, kNoRefs, {.extensions = true});
}
} // namespace md_detail

} // namespace maya
