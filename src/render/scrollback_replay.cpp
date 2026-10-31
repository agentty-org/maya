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

namespace maya::replay {

namespace {

struct Rows {
    std::string bytes;   // rows joined by \r\n, NO trailing \r\n
    int         rows = 0;
};

// One block at `width`: lay out with auto height, paint, serialize. The
// root states the full width, the same as the live frame's theme wrapper
// (theme_canvas.hpp), so a themed background reaches the right edge.
Rows render_block(const Element& block, int width, const Theme& theme) {
    BoxElement box;
    box.layout.width = Dimension::fixed(width);
    box.children.push_back(block);
    const Element e{std::move(box)};
    StylePool pool;
    std::vector<layout::LayoutNode> nodes;
    Canvas canvas{width, 64, &pool};
    render_tree(e, canvas, pool, theme, nodes, /*auto_height=*/true);
    int h = nodes.empty() ? 0 : nodes[0].computed.size.height.raw();
    if (h > canvas.height()) {
        canvas.resize(width, h);
        canvas.clear();
        render_tree(e, canvas, pool, theme, nodes, /*auto_height=*/true);
        h = nodes[0].computed.size.height.raw();
    }
    if (h <= 0) return {};

    // serialize() is the same row writer the live frame uses, so replayed
    // rows are byte-identical to what the live canvas would emit. It
    // brackets with DECAWM off/on and ends with an SGR reset; that's
    // harmless between blocks.
    Rows out;
    out.rows = h;
    out.bytes.reserve(static_cast<std::size_t>(h) * static_cast<std::size_t>(width + 16));
    serialize(canvas, pool, out.bytes, h, 0);
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
    // The address is stable for a given theme value only while it lives,
    // so key on the epoch the runtime bumps on every swap instead.
    (void)t;
    return theme::live_epoch();
}

}  // namespace

void clear_cache() noexcept { cache().clear(); }

Rendered render_history(const ScrollbackReplay& src, int width, const Theme& theme) {
    Rendered out;
    if ((!src.blocks && !src.chunk) || width <= 0 || src.max_rows <= 0) return out;
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
            // lay out + serialize in parallel. Workers only read the
            // wrapped Elements and write their own slot.
            std::vector<Element> wrapped(miss.size());
            for (std::size_t j = 0; j < miss.size(); ++j)
                wrapped[j] = src.wrap ? src.wrap(win[miss[j]].element)
                                      : win[miss[j]].element;
            std::vector<Rows> got(miss.size());
            const auto tr0 = clock::now();
            exec::parallel_for(miss.size(), [&](std::size_t j) {
                got[j] = render_block(wrapped[j], width, theme);
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
            if (!kept.empty() && (rows + r > src.max_rows || bytes + b > src.max_bytes)) {
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
