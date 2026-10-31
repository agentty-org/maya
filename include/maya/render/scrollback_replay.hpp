#pragma once
// maya::ScrollbackReplay — keep terminal scrollback whole across a width change.
//
// Inline mode prints history into the terminal's own scrollback and forgets
// it. A width change makes every wrapped row wrong, so the device has to
// wipe the screen and scrollback (\x1b[2J\x1b[3J) and repaint. Before this,
// only the live canvas came back; everything older was gone from the
// terminal even though the app still had it.
//
// A host that wants its scrollback back registers a replay source: the
// history ABOVE the live canvas, oldest first, as sealed blocks. On the
// width-change reset the device renders those blocks at the new width and
// writes them between the wipe and the live frame, in the same synchronized
// frame. They scroll off the top into real scrollback; the live frame lands
// exactly where it would have.
//
// Fast because:
//   - each block is laid out and serialized on its own, so blocks render in
//     parallel across cores;
//   - results are cached per (block hash, width, theme); dragging back to a
//     width already seen costs nothing but the write;
//   - rendering stops at a row cap and a byte cap (newest blocks win), sized
//     to what the terminal keeps anyway.
//
// The device calls the source only on a width-change reset, never per frame.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "../element/element.hpp"

namespace maya {

struct Theme;

struct ReplayBlock {
    // Stable identity of this block's content. Same hash, same bytes at a
    // given width and theme; 0 means "don't cache".
    std::uint64_t hash = 0;
    Element       element;
};

struct ScrollbackReplay {
    // History above the live canvas, OLDEST first. Called once per replay,
    // on the UI thread. Blocks may be built lazily; the device drops the
    // vector right after rendering.
    std::function<std::vector<ReplayBlock>()> blocks;

    // Optional, preferred when set: pull history NEWEST first, a chunk at a
    // time. `chunk(cursor)` returns blocks that sit just above `cursor`
    // (oldest first within the chunk) and the cursor to pass next; an empty
    // result means there's nothing older. Start with cursor = SIZE_MAX.
    // The renderer stops pulling once the caps are met, so a long thread
    // only builds the part that will actually be shown.
    struct Chunk { std::vector<ReplayBlock> blocks; std::size_t next = 0; };
    std::function<Chunk(std::size_t cursor)> chunk;

    // Wraps one history block in the same chrome the live canvas puts around
    // its frozen prefix (outer padding etc), so replayed rows sit at the
    // exact columns the live rows did. Identity when unset.
    std::function<Element(Element)> wrap;

    // Caps on what one replay writes. Rows past the terminal's own
    // scrollback limit are wasted; the newest blocks are kept.
    int         max_rows  = 10'000;
    std::size_t max_bytes = 8u << 20;

    // Bumped by the host when the history CHANGES SHAPE in a way hashes
    // can't see (a theme swap is already handled; this is for host-side
    // style knobs). Part of the cache key.
    std::uint64_t epoch = 0;
};

// What the device's last replay did. For host logging and tests.
struct ReplayStats {
    int         rows   = 0;
    int         blocks = 0;
    int         cached = 0;
    bool        capped = false;
    std::size_t bytes  = 0;
    double      ms     = 0;
};

namespace replay {

// What render_history produced. `bytes` is complete VT rows separated by
// \r\n with a trailing \r\n, so the live frame starts on a fresh row.
struct Rendered {
    std::string bytes;
    int         rows    = 0;   // rows in `bytes`
    int         blocks  = 0;   // blocks rendered (after the cap)
    int         cached  = 0;   // of which served from cache
    bool        capped  = false;
    double      build_ms = 0;  // time in the host's blocks() call
};

// Render `src` at `width` with `theme`. Pure apart from the per-block cache;
// safe to call from the UI thread only (the cache is not shared across
// threads, the parallel part works on private copies).
[[nodiscard]] Rendered render_history(const ScrollbackReplay& src, int width,
                                      const Theme& theme);

// Drop every cached rendering (a theme swap, tests).
void clear_cache() noexcept;

}  // namespace replay
}  // namespace maya
