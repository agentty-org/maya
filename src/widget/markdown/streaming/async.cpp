// async.cpp — StreamingMarkdown off-thread parse path.
//
// set_content_async hands large divergent parses to a detached worker
// (single-flight coalesced via async_latest_source_), keeping the previous
// frame's element tree visible until the result lands. spawn_async_worker_
// runs the full top-level parse + per-block segmentation on the worker;
// maybe_apply_async_ (foreground-only, called from build()) adopts or
// re-queues the landed result.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "maya/element/builder.hpp"
#include "maya/widget/markdown.hpp"
#include "maya/widget/markdown/internal.hpp"
#include "maya/widget/markdown/streaming_internal.hpp"
#include "maya/core/executor.hpp"

namespace maya {

using ::maya::md_detail::parse_markdown_impl;
using ::maya::md_detail::collect_ref_defs;

bool StreamingMarkdown::is_parsing() const noexcept {
    std::lock_guard<std::mutex> lk(async_mu_());
    return static_cast<bool>(async_slot_);
}

void StreamingMarkdown::set_content_async(std::string_view content) {
    // O(1) append-detection mirroring set_content: a bounded edge-window
    // compare instead of a full O(source_) memcmp every frame. See the
    // detailed rationale in commit.cpp::set_content — during streaming this
    // instance only ever sees pure-append growth, and a genuine divergence
    // lands on a fresh instance, so the bounded window is exact in practice
    // while removing the per-frame O(N^2) re-verify at the ingest seam.
    constexpr std::size_t kVerifyEdge = 64;
    auto prefix_matches = [&](std::size_t n) -> bool {
        if (n == 0) return true;
        if (n <= 2 * kVerifyEdge)
            return std::memcmp(content.data(), source_.data(), n) == 0;
        return std::memcmp(content.data(), source_.data(), kVerifyEdge) == 0
            && std::memcmp(content.data() + n - kVerifyEdge,
                           source_.data() + n - kVerifyEdge,
                           kVerifyEdge) == 0;
    };
    // Same fast-path as set_content: no-op on unchanged.
    if (content.size() == source_.size() && prefix_matches(content.size())) {
        return;
    }
    // Pure-append growth → cheap incremental sync path is strictly
    // better than handing the delta off to a worker (the steady-state
    // streaming case). The expensive case async exists for is the
    // "divergent prefix" / large initial set path below.
    if (content.size() > source_.size() && prefix_matches(source_.size())) {
        set_content(content);
        return;
    }

    // Below a threshold the synchronous reset-and-parse path is faster
    // than thread handoff; pay the cost inline. 16 KB is a generous
    // ceiling — pasting a long markdown blob comfortably exceeds it
    // while typical short composer edits stay below.
    constexpr std::size_t kAsyncThreshold = 16 * 1024;
    if (content.size() < kAsyncThreshold) {
        set_content(content);
        return;
    }

    // Divergent and large → schedule a background parse. Keep the
    // current cached_build_ visible until the worker lands so the UI
    // doesn't blank.
    auto requested = std::make_shared<std::string>(content);
    {
        std::lock_guard<std::mutex> lk(async_mu_());
        // Mark obsolete work before replacing the coalesced request. The
        // detached worker owns its slot, so this cannot shorten its lifetime.
        if (async_slot_)
            async_slot_->cancelled.store(true, std::memory_order_release);
        async_latest_source_ = requested;
        // If a worker is already in flight, it will stop at its next safe
        // phase boundary; maybe_apply_async_ then starts this newest request.
        if (async_slot_) return;
    }
    spawn_async_worker_(std::move(requested));
}

void StreamingMarkdown::spawn_async_worker_(std::shared_ptr<std::string> source) const {
    auto slot = std::make_shared<AsyncResult>();
    slot->source = std::move(source);
    {
        std::lock_guard<std::mutex> lk(async_mu_());
        async_slot_ = slot;
    }

    // On the runtime's pool via maya::exec, joined before maya::run returns
    // (inline with no runtime). The slot is co-owned by the job, so a
    // StreamingMarkdown destroyed mid-parse just drops its copy. The body is
    // a plain function: it reaches the slot and nothing else.
    exec::post<AsyncResult>(+[](AsyncResult& slot_ref) {
        AsyncResult* slot = &slot_ref;
        // The slot retains the source for the entire task; using a
        // reference avoids a second full-buffer copy in the thread closure.
        const std::string& src = *slot->source;
        auto publish_cancelled = [&] {
            slot->ready.store(true, std::memory_order_release);
        };
        if (slot->cancelled.load(std::memory_order_acquire)) {
            publish_cancelled();
            return;
        }
        // Re-parse from scratch. We can't reuse the host's incremental
        // state because that lives on the foreground thread; instead
        // we run the full top-level parse on the worker. Output is
        // the rendered Element list + per-block metadata + the
        // ref-defs map a fresh parse produces.
        std::unordered_map<std::string, md::LinkRef> defs;
        std::string cleaned = collect_ref_defs(std::string_view{src}, defs);
        ::maya::md_detail::RefDefsScope guard(&defs);
        auto parsed = parse_markdown_impl(cleaned, 0);
        if (slot->cancelled.load(std::memory_order_acquire)) {
            publish_cancelled();
            return;
        }

        // Compute segment ranges over the full src — same algorithm
        // as the foreground commit_range above, just bounded by the
        // whole buffer.
        std::vector<std::pair<std::size_t, std::size_t>> seg_ranges;
        seg_ranges.reserve(parsed.blocks.size() + 1);
        {
            bool seg_in_fence = false;
            char seg_fence_ch = '\0';
            std::size_t seg_fence_len = 0;
            std::size_t k = 0;
            while (k < src.size() && src[k] == '\n') ++k;
            std::size_t seg_start = k;
            while (k < src.size()) {
                if ((k & 0xFFFu) == 0
                    && slot->cancelled.load(std::memory_order_relaxed)) {
                    publish_cancelled();
                    return;
                }
                bool at_ls = (k == 0 || src[k - 1] == '\n');
                std::size_t eol0 = src.find('\n', k);
                std::size_t le = (eol0 == std::string::npos) ? src.size() : eol0;
                bool is_code_open = false;
                if (at_ls) {
                    md_detail::streaming::FenceState fs{
                        seg_in_fence, seg_fence_ch, seg_fence_len};
                    is_code_open = md_detail::streaming::fence_scan_line(
                        fs, std::string_view{src}, k, le);
                    if (is_code_open) {
                        seg_in_fence  = fs.in_fence;
                        seg_fence_ch  = fs.open_ch;
                        seg_fence_len = fs.open_len;
                    }
                }
                if (is_code_open) {
                    std::size_t eol = src.find('\n', k);
                    if (eol == std::string::npos) { k = src.size(); break; }
                    k = eol + 1;
                    if (!seg_in_fence) {
                        if (k > seg_start) seg_ranges.emplace_back(seg_start, k);
                        while (k < src.size() && src[k] == '\n') ++k;
                        seg_start = k;
                    }
                    continue;
                }
                if (!seg_in_fence && at_ls && src[k] == '\n') {
                    if (k > seg_start) seg_ranges.emplace_back(seg_start, k);
                    while (k < src.size() && src[k] == '\n') ++k;
                    seg_start = k;
                    continue;
                }
                ++k;
            }
            if (seg_start < src.size())
                seg_ranges.emplace_back(seg_start, src.size());
        }
        const bool seg_match = seg_ranges.size() == parsed.blocks.size();

        slot->blocks.reserve(parsed.blocks.size());
        slot->metas.reserve (parsed.blocks.size());
        std::size_t synth = 0;
        for (std::size_t bi = 0; bi < parsed.blocks.size(); ++bi) {
            if (slot->cancelled.load(std::memory_order_relaxed)) {
                publish_cancelled();
                return;
            }
            auto& block = parsed.blocks[bi];
            BlockMeta meta;
            if (seg_match) {
                meta.source_offset = seg_ranges[bi].first;
                meta.source_end    = seg_ranges[bi].second;
            } else {
                meta.source_offset = synth;
                meta.source_end = (bi + 1 < parsed.blocks.size())
                    ? (synth + 1) : src.size();
                synth += 1;
            }
            meta.kind = std::visit([](const auto& x) noexcept -> BlockKind {
                using T = std::decay_t<decltype(x)>;
                if constexpr (std::is_same_v<T, md::Paragraph>)       return BlockKind::Paragraph;
                else if constexpr (std::is_same_v<T, md::Heading>)    return BlockKind::Heading;
                else if constexpr (std::is_same_v<T, md::CodeBlock>)  return BlockKind::CodeBlock;
                else if constexpr (std::is_same_v<T, md::Blockquote>) return BlockKind::Blockquote;
                else if constexpr (std::is_same_v<T, md::List>)       return BlockKind::List;
                else if constexpr (std::is_same_v<T, md::HRule>)      return BlockKind::HRule;
                else if constexpr (std::is_same_v<T, md::Table>)      return BlockKind::Table;
                else if constexpr (std::is_same_v<T, md::FootnoteDef>) return BlockKind::FootnoteDef;
                else if constexpr (std::is_same_v<T, md::Alert>)      return BlockKind::Alert;
                else if constexpr (std::is_same_v<T, md::DefList>)    return BlockKind::DefList;
                else if constexpr (std::is_same_v<T, md::Details>)    return BlockKind::Details;
                else if constexpr (std::is_same_v<T, md::HtmlBlock>)  return BlockKind::HtmlBlock;
                else return BlockKind::Other;
            }, block.inner);
            if (auto* c = std::get_if<md::CodeBlock>(&block.inner)) meta.lang = c->lang;
            std::size_t lc = 0;
            for (std::size_t q = meta.source_offset; q < meta.source_end && q < src.size(); ++q)
                if (src[q] == '\n') ++lc;
            meta.line_count = static_cast<std::uint16_t>(std::min<std::size_t>(lc, 0xFFFFu));
            slot->blocks.push_back(
                std::make_shared<const Element>(md_block_to_element(block)));
            slot->metas.push_back(std::move(meta));
        }
        slot->ref_defs = std::move(defs);
        // in_code_fence at end-of-source — spec-faithful ``` / ~~~ parity
        // (≤3 indent, char + run-length matched close) via the shared
        // classifier, so the async result agrees with the incremental path.
        md_detail::streaming::FenceState fst;
        for (std::size_t j = 0; j < src.size(); ++j) {
            bool at_ls = (j == 0 || src[j - 1] == '\n');
            if (!at_ls) continue;
            std::size_t eol = src.find('\n', j);
            std::size_t le = (eol == std::string::npos) ? src.size() : eol;
            (void)md_detail::streaming::fence_scan_line(
                fst, std::string_view{src}, j, le);
        }
        slot->in_code_fence  = fst.in_fence;
        slot->fence_open_ch  = fst.open_ch;
        slot->fence_open_len = fst.open_len;
        // Publish: release-store on `ready` so the foreground's
        // acquire-load sees the populated vectors above.
        slot->ready.store(true, std::memory_order_release);
    }, slot);
}

void StreamingMarkdown::maybe_apply_async_() const {
    std::shared_ptr<AsyncResult> slot;
    std::shared_ptr<std::string>      latest_after;
    {
        std::lock_guard<std::mutex> lk(async_mu_());
        if (!async_slot_) return;
        if (!async_slot_->ready.load(std::memory_order_acquire)) return;
        slot = std::move(async_slot_);  // detach from member
        latest_after = async_latest_source_;
    }

    // Decide what to do with the landed result:
    //   A. The caller's latest request matches what this worker
    //      parsed → adopt the result; clear latest_source.
    //   B. The caller has since asked for a different source →
    //      discard this result, spawn a follow-up on the new
    //      request.
    const bool current = latest_after && latest_after == slot->source;
    if (current && !slot->cancelled.load(std::memory_order_acquire)) {
        // Adopt. This is the foreground thread (build() calls us),
        // so mutating self's state is safe with no extra locks.
        auto self = const_cast<StreamingMarkdown*>(this);
        self->source_ = std::move(*slot->source);
        self->committed_ = self->source_.size();
        self->in_code_fence_ = slot->in_code_fence;
        self->fence_open_ch_ = slot->fence_open_ch;
        self->fence_open_len_ = slot->fence_open_len;
        self->ref_defs_ = std::move(slot->ref_defs);
        self->sink_.reset();
        // The source_ buffer was replaced wholesale (async worker
        // reparsed a possibly-different byte sequence), so the cp
        // caches' "bytes [0,cached_at) unchanged" invariant no longer
        // holds — invalidate them exactly as clear() does.
        self->cached_total_cp_        = 0;
        self->cached_total_cp_at_     = 0;
        self->cached_committed_cp_    = 0;
        self->cached_committed_cp_at_ = 0;
        self->cp_to_byte_cache_cp_    = 0;
        self->cp_to_byte_cache_byte_  = 0;
        self->cp_to_byte_cache_at_    = 0;
        auto fresh = std::make_shared<CommittedPrefix>();
        fresh->blocks = std::move(slot->blocks);
        fresh->metas  = std::move(slot->metas);
        // Generation must STRICTLY exceed the prior gen so the
        // prefix ComponentElement's hash_id changes and the
        // renderer's cache misses cleanly.
        fresh->generation = self->prefix_->generation + 1;
        self->prefix_ = std::move(fresh);
        self->build_dirty_ = true;
        ++self->source_version_;
        // Reset scanner / sink state to the new committed end.
        self->scan_cursor_ = self->committed_;
        self->scan_in_fence_ = self->in_code_fence_;
        self->scan_fence_open_ch_  = self->fence_open_ch_;
        self->scan_fence_open_len_ = self->fence_open_len_;
        self->scan_last_boundary_ = self->committed_;
        // The whole buffer is committed — every pending boundary is consumed.
        self->scan_boundaries_.clear();
        // Block offsets are the fold-map keys. Build a lookup set once,
        // then prune in one pass instead of scanning every block per fold.
        std::unordered_set<std::size_t> offsets;
        offsets.reserve(self->prefix_->metas.size());
        for (const auto& m : self->prefix_->metas)
            offsets.insert(m.source_offset);
        for (auto it = self->folds_.begin(); it != self->folds_.end();) {
            if (offsets.find(it->first) == offsets.end())
                it = self->folds_.erase(it);
            else
                ++it;
        }
        {
            std::lock_guard<std::mutex> lk(self->async_mu_());
            self->async_latest_source_.reset();
        }
    } else if (latest_after) {
        // Stale (or cancelled): spawn a follow-up for the newest request.
        spawn_async_worker_(std::move(latest_after));
    }
}

} // namespace maya
