// src/render/scrollback_replay.cpp — see include/maya/render/scrollback_replay.hpp.
//
// Each block is rendered alone, on its own canvas and StylePool, and
// serialized to VT bytes with styles re-stated per row. That makes blocks
// independent (render them in parallel, cache them, concatenate in any
// grouping) at the cost of a few redundant SGRs per row.
//
// Thread safety: workers share only const Elements. render_tree's
// component cache is thread_local, StylePool and Canvas are per worker, and
// component render callbacks capture immutable data (sealed blocks). The
// theme and colour level are process-wide reads.

#include "maya/render/scrollback_replay.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <list>
#include <unordered_map>
#include <utility>

#include "maya/core/executor.hpp"
#include "maya/render/canvas.hpp"
#include "maya/render/renderer.hpp"
#include "maya/render/serialize.hpp"
#include "maya/style/theme.hpp"
#include "render_internal.hpp"

namespace maya::replay {

namespace {

struct Rows {
    std::string bytes;   // rows joined by \r\n, NO trailing \r\n
    int         rows = 0;
};

// Per-worker scratch (canvas, layout nodes, style pool). Lives for one
// replay and is freed when it ends. It used to be thread_local, so a worker
// that once painted a 1000-row block held that canvas for the session.
struct Scratch {
    StylePool                       pool;
    std::vector<layout::LayoutNode> nodes;
    Canvas                          canvas{1, 1, nullptr};
    int                             tallest = 256;
    int                             dirty   = 0;   // rows the last block painted
};

// One block at `width`: lay out with auto height, paint, serialize. The
// root states the full width, the same as the live frame's theme wrapper
// (theme_canvas.hpp), so a themed background reaches the right edge.
//
// One paint per block. The canvas starts tall enough for most blocks and
// grows to fit; a block only gets a second pass if it outgrew a canvas
// that already held the tallest block this scratch has seen. Scratch is
// borrowed from the replay's list, so it's reused across blocks.
// `content` is the block, already wrapped in host chrome, built over an
// ElementListRef into the block's shared storage: no tree is copied. A
// block is a whole Turn tree, and deep-copying it per render was the
// largest single cost of a replay.
Rows render_block(const Element& content, int width, const Theme& theme, Scratch& s) {
    BoxElement box;
    box.layout.width = Dimension::fixed(width);
    box.children.push_back(content);   // a ref + chrome: shallow
    const Element e{std::move(box)};

    // A fresh pool per block: serialize() emits styles by pool id, and a
    // block's bytes must not depend on which blocks rendered before it on
    // this scratch (the bytes are cached and reused at any position).
    s.pool.clear();
    s.canvas.set_style_pool(&s.pool);
    if (s.canvas.width() != width || s.canvas.height() < s.tallest) {
        s.canvas.resize(width, s.tallest);
    } else {
        // Only the rows the previous block touched hold anything; clearing
        // the whole (tall) canvas per block was most of the cost.
        s.canvas.reset_clips();
        s.canvas.clear_rows(s.dirty + 1);
    }

    render_tree(e, s.canvas, s.pool, theme, s.nodes, /*auto_height=*/true);
    int h = s.nodes.empty() ? 0 : s.nodes[0].computed.size.height.raw();
    if (h > s.canvas.height()) {
        // Taller than anything before: grow (with room) and paint again.
        s.tallest = h + h / 2;
        s.canvas.resize(width, s.tallest);
        render_tree(e, s.canvas, s.pool, theme, s.nodes, /*auto_height=*/true);
        h = s.nodes[0].computed.size.height.raw();
    }
    // render_tree cached every component's cells in this thread's component
    // cache. Their style ids belong to s.pool, which the next block clears,
    // so they can never be reused, and across a long history they add up to
    // hundreds of MB. Drop them. render_history() swaps the caller's own
    // (live frame) cache out first, so this never touches it.
    render_detail::clear_component_cache();
    // Paint stays inside the laid-out height (the root box clips to it), so
    // [0, h) is everything this block can have written.
    s.dirty = std::min(s.canvas.height(), std::max(h, s.canvas.max_content_row() + 1));
    if (h <= 0) return {};

    // serialize() is the same row writer the live frame uses, so replayed
    // rows are byte-identical to what the live canvas would emit. It
    // brackets with DECAWM off/on and ends with an SGR reset; that's
    // harmless between blocks.
    Rows out;
    out.rows = h;
    out.bytes.reserve(static_cast<std::size_t>(h) * static_cast<std::size_t>(width + 16));
    serialize(s.canvas, s.pool, out.bytes, h, 0);
    // The reserve is a guess; the rows are cached, so don't keep the slack.
    out.bytes.shrink_to_fit();
    return out;
}

// Per (hash, width, theme, epoch) rendering. LRU by byte size: enough to
// hold a full replay at two or three widths, so dragging back and forth
// is a pure write.
struct Key {
    std::uint64_t hash, theme, epoch;
    int           width;
    bool operator==(const Key&) const = default;
};
struct KeyHash {
    std::size_t operator()(const Key& k) const noexcept {
        std::uint64_t h = k.hash * 0x9E3779B97F4A7C15ull;
        h ^= k.theme + 0x632BE59BD9B4E019ull + (h << 6) + (h >> 2);
        h ^= k.epoch + 0x94D049BB133111EBull + (h << 6) + (h >> 2);
        h ^= static_cast<std::uint64_t>(k.width) + (h << 6) + (h >> 2);
        return static_cast<std::size_t>(h);
    }
};

class Cache {
public:
    static constexpr std::size_t kMaxBytes = 48u << 20;

    const Rows* find(const Key& k) {
        auto it = map_.find(k);
        if (it == map_.end()) return nullptr;
        lru_.splice(lru_.begin(), lru_, it->second);
        return &it->second->second;
    }
    void put(const Key& k, Rows r) {
        if (map_.contains(k) || r.bytes.size() > kMaxBytes / 4) return;
        bytes_ += r.bytes.size();
        lru_.emplace_front(k, std::move(r));
        map_.emplace(k, lru_.begin());
        while (bytes_ > kMaxBytes && !lru_.empty()) {
            bytes_ -= lru_.back().second.bytes.size();
            map_.erase(lru_.back().first);
            lru_.pop_back();
        }
    }
    void clear() noexcept { map_.clear(); lru_.clear(); bytes_ = 0; }
    std::size_t bytes() const noexcept { return bytes_; }

private:
    using Entry = std::pair<Key, Rows>;
    std::list<Entry> lru_;
    std::unordered_map<Key, std::list<Entry>::iterator, KeyHash> map_;
    std::size_t bytes_ = 0;
};

Cache& cache() {
    static Cache c;   // UI thread only (render_history's contract)
    return c;
}

std::uint64_t theme_key(const Theme& t) noexcept {
    // The live theme is assigned through one slot, so its address never
    // changes; the epoch the runtime bumps on every swap is the identity.
    (void)t;
    return theme::live_epoch();
}

}  // namespace

void clear_cache() noexcept { cache().clear(); }
std::size_t cache_bytes() noexcept { return cache().bytes(); }

Rendered render_history(const ScrollbackReplay& src, int width, const Theme& theme) {
    Rendered out;
    // The host's live limits win over the static fields.
    const auto [max_rows, max_bytes] =
        src.limits ? src.limits() : std::pair{src.max_rows, src.max_bytes};
    if ((!src.blocks && !src.chunk) || width <= 0 || max_rows <= 0) return out;
    // Blocks may render inline on this thread, which also owns the live
    // frame's component cache. render_block clears the cache per block, so
    // park the live one and put it back when the replay is done.
    struct KeepLiveCache {
        render_detail::ComponentCache saved = std::move(render_detail::component_cache());
        ~KeepLiveCache() { render_detail::component_cache() = std::move(saved); }
    } keep_live;
    // Canvases and layout buffers for this replay only (see Scratch).
    std::vector<Scratch> scratch;
    using clock = std::chrono::steady_clock;
    auto ms = [](clock::time_point t0) {
        return std::chrono::duration<double, std::milli>(clock::now() - t0).count();
    };

    const std::uint64_t tk = theme_key(theme);

    // Newest-first windows. Each window: get the blocks (pull a chunk from
    // the host, or slice the whole list), serve cache hits, render the
    // misses in parallel, then keep blocks while they fit the caps. Heights
    // are only known after rendering, so a window is the unit of
    // speculation: at most one window of work past the cap is wasted, and
    // with `chunk` the host never builds history past it either.
    std::vector<ReplayBlock> whole;           // `blocks` mode only
    std::size_t whole_hi = 0;
    if (!src.chunk) {
        const auto t0 = clock::now();
        whole = src.blocks();
        out.build_ms += ms(t0);
        whole_hi = whole.size();
    }
    constexpr std::size_t kWindow = 64;
    std::size_t cursor = SIZE_MAX;            // `chunk` mode

    std::vector<Rows> kept;                   // newest first
    long long   rows  = 0;
    std::size_t bytes = 0;
    for (bool full = false; !full;) {
        std::vector<ReplayBlock> win;
        if (src.chunk) {
            const auto t0 = clock::now();
            auto c = src.chunk(cursor);
            out.build_ms += ms(t0);
            if (c.blocks.empty()) break;
            win = std::move(c.blocks);
            cursor = c.next;
        } else {
            if (whole_hi == 0) break;
            const std::size_t from = whole_hi > kWindow ? whole_hi - kWindow : 0;
            win.assign(std::make_move_iterator(whole.begin() + static_cast<std::ptrdiff_t>(from)),
                       std::make_move_iterator(whole.begin() + static_cast<std::ptrdiff_t>(whole_hi)));
            whole_hi = from;
        }

        const std::size_t wn = win.size();
        std::vector<Rows> rendered(wn);
        std::vector<std::size_t> miss;
        for (std::size_t k = 0; k < wn; ++k) {
            const auto& b = win[k];
            const Rows* hit = b.hash != 0
                ? cache().find(Key{b.hash, tk, src.epoch, width}) : nullptr;
            if (hit) { rendered[k] = *hit; ++out.cached; }
            else     miss.push_back(k);
        }
        if (!miss.empty()) {
            // Wrap on this thread (host chrome may read host state), then
            // lay out + serialize in parallel. Nothing is deep-copied: each
            // block's shared tree is borrowed through an ElementListRef, and
            // the wrapper only adds chrome around that reference. Workers
            // read these and write their own slot.
            std::vector<Element> wrapped(miss.size());
            for (std::size_t j = 0; j < miss.size(); ++j) {
                const auto& sp = win[miss[j]].element;
                if (!sp) continue;
                Element ref{ElementListRef{sp.get()}};
                wrapped[j] = src.wrap ? src.wrap(std::move(ref)) : std::move(ref);
            }
            std::vector<Rows> got(miss.size());
            const auto tr0 = clock::now();
            // One contiguous slice per scratch, so each slice reuses its own
            // canvas and nothing is shared between workers.
            constexpr std::size_t kSlices = 32;
            const std::size_t slices = std::min(kSlices, miss.size());
            if (scratch.size() < slices) scratch.resize(slices);
            exec::parallel_for(slices, [&](std::size_t s) {
                const std::size_t lo = miss.size() * s / slices;
                const std::size_t hi = miss.size() * (s + 1) / slices;
                for (std::size_t j = lo; j < hi; ++j)
                    got[j] = render_block(wrapped[j], width, theme, scratch[s]);
            });
            out.render_ms += ms(tr0);
            for (std::size_t j = 0; j < miss.size(); ++j) {
                const std::size_t k = miss[j];
                if (win[k].hash != 0)
                    cache().put(Key{win[k].hash, tk, src.epoch, width}, got[j]);
                rendered[k] = std::move(got[j]);
            }
        }

        for (std::size_t k = wn; k-- > 0;) {
            const long long   r = rendered[k].rows;
            const std::size_t b = rendered[k].bytes.size() + 2;
            // Always keep the newest block, even if it alone is over.
            if (!kept.empty() && (rows + r > max_rows || bytes + b > max_bytes)) {
                full = true;
                out.capped = true;
                break;
            }
            rows += r; bytes += b;
            kept.push_back(std::move(rendered[k]));
        }
    }

    out.bytes.reserve(bytes + 16);
    for (auto it = kept.rbegin(); it != kept.rend(); ++it) {   // oldest first
        if (it->rows == 0) continue;
        out.bytes += it->bytes;
        out.bytes += "\r\n";
        out.rows  += it->rows;
        ++out.blocks;
    }
    return out;
}

}  // namespace maya::replay
