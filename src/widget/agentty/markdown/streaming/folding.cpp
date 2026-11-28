// folding.cpp — StreamingMarkdown block introspection + fold state.
//
// block_for_offset (binary search over committed block ranges), and the
// fold map mutators (is_folded / set_fold / toggle_fold / unfold_all /
// auto_fold_long_blocks). Fold state is keyed on BlockMeta::source_offset
// — the Zed-style stable key that survives a reparse.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "maya/widget/agentty/markdown.hpp"

namespace maya {

std::optional<std::size_t>
StreamingMarkdown::block_for_offset(std::size_t off) const noexcept {
    const auto& metas = prefix_->metas;
    if (metas.empty()) return std::nullopt;
    // Binary search for the greatest i with metas[i].source_offset <= off.
    auto it = std::upper_bound(metas.begin(), metas.end(), off,
        [](std::size_t v, const BlockMeta& m) noexcept {
            return v < m.source_offset;
        });
    if (it == metas.begin()) return std::nullopt;
    --it;
    const std::size_t idx = static_cast<std::size_t>(it - metas.begin());
    // off must lie within the block's attributed range. Beyond
    // source_end (e.g. into the tail) returns nullopt.
    if (off >= it->source_end) return std::nullopt;
    return idx;
}

bool StreamingMarkdown::is_folded(std::size_t source_offset) const noexcept {
    auto it = folds_.find(source_offset);
    return it != folds_.end() && it->second;
}

void StreamingMarkdown::set_fold(std::size_t source_offset, bool folded) {
    // Validate against the current block set so we don't accumulate
    // map entries for stale offsets (an offset that doesn't match any
    // known block has no rendering meaning).
    bool known = false;
    for (const auto& m : prefix_->metas) {
        if (m.source_offset == source_offset) { known = true; break; }
    }
    if (!known) return;
    bool& slot = folds_[source_offset];
    if (slot == folded) return;
    slot = folded;
    ++fold_generation_;
    build_dirty_ = true;
}

bool StreamingMarkdown::toggle_fold(std::size_t source_offset) {
    const bool now = !is_folded(source_offset);
    set_fold(source_offset, now);
    // set_fold no-ops on an unknown offset; reflect the actual state.
    return is_folded(source_offset);
}

void StreamingMarkdown::unfold_all() {
    if (folds_.empty()) return;
    folds_.clear();
    ++fold_generation_;
    build_dirty_ = true;
}

void StreamingMarkdown::auto_fold_long_blocks(std::uint16_t threshold_lines,
                                              std::uint32_t kinds_mask) {
    bool any = false;
    for (const auto& m : prefix_->metas) {
        const std::uint32_t bit = 1u << static_cast<unsigned>(m.kind);
        if (!(bit & kinds_mask)) continue;
        if (m.line_count <= threshold_lines) continue;
        // Only fold blocks the user hasn't already explicitly
        // unfolded — if they typed the unfold key, respect it. A
        // missing entry (the dominant case on the first auto-fold
        // pass) gets the auto-fold; an entry set to `false` is the
        // user's explicit "keep open" and stays unfolded.
        if (folds_.contains(m.source_offset)) continue;
        folds_[m.source_offset] = true;
        any = true;
    }
    if (any) {
        ++fold_generation_;
        build_dirty_ = true;
    }
}


} // namespace maya
