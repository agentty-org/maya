// markdown_memo.cpp — assemble_markdown + the content-keyed markdown() LRU.
//
// assemble_markdown: md::Document → Element (vstack of block elements).
// markdown(): a bounded thread-local LRU over (source bytes → built Element)
// so a settled assistant message rendered every frame parses once, then hits
// the cache. Both were file-scope in the old monolithic streaming.cpp.

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "maya/element/builder.hpp"
#include "maya/widget/agentty/markdown.hpp"
#include "maya/widget/agentty/markdown/internal.hpp"

#include "maya/widget/agentty/markdown/streaming_internal.hpp"

namespace maya {

using md_detail::streaming::fnv1a64;

Element assemble_markdown(md::Document&& doc) {
    if (doc.blocks.empty()) return Element{TextElement{""}};

    if (doc.blocks.size() == 1)
        return detail::vstack().padding(0, 0, 0, 2)(
            md_block_to_element(doc.blocks[0]));

    std::vector<Element> blocks;
    blocks.reserve(doc.blocks.size());
    for (auto& block : doc.blocks)
        blocks.push_back(md_block_to_element(block));

    return detail::vstack().gap(1).padding(0, 0, 0, 2)(std::move(blocks));
}

namespace {

constexpr std::size_t kMarkdownCacheCap = 64;

[[nodiscard]] inline std::uint64_t hash_markdown_source(
    std::string_view s) noexcept {
    return fnv1a64(s);
}

struct MarkdownCacheEntry {
    std::uint64_t hash;
    std::string   source;       // owned for memcmp tiebreak
    Element       built;        // assembled Element tree; copied on hit
};

[[nodiscard]] inline std::vector<MarkdownCacheEntry>& markdown_cache() {
    thread_local std::vector<MarkdownCacheEntry> cache;
    return cache;
}

} // anonymous

Element markdown(std::string_view source) {
    // Tiny inputs: skip the cache entirely — memo overhead exceeds parse.
    if (source.size() < 32) {
        return assemble_markdown(parse_markdown(source));
    }

    auto& cache = markdown_cache();
    const std::uint64_t h = hash_markdown_source(source);

    // Hit path: linear scan (cache is small, predictable, branch-friendly),
    // MRU-promote by swap-to-back, return a deep copy of the cached Element.
    for (std::size_t i = 0; i < cache.size(); ++i) {
        auto& e = cache[i];
        if (e.hash == h
            && e.source.size() == source.size()
            && std::memcmp(e.source.data(), source.data(), source.size()) == 0)
        {
            if (i + 1 != cache.size()) std::swap(cache[i], cache.back());
            return cache.back().built;   // variant copy, O(tree)
        }
    }

    // Miss: parse, assemble, store, evict oldest if needed.
    Element built = assemble_markdown(parse_markdown(source));
    if (cache.size() >= kMarkdownCacheCap) {
        cache.erase(cache.begin());
    }
    cache.push_back({h, std::string{source}, built});
    return built;
}

// Cross-TU bridge — re-expose assemble_markdown under md_detail for symmetry.
namespace md_detail {
Element assemble_markdown(md::Document&& doc) {
    return ::maya::assemble_markdown(std::move(doc));
}
} // namespace md_detail

} // namespace maya
