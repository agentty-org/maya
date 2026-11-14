// src/render/renderer.cpp — render_tree: build the layout tree, lay it out,
// paint it. Each element kind's painter lives in its own file (paint_*.cpp).
#include "render_internal.hpp"

namespace maya {

namespace render_detail {

// Diagnostic counter — see renderer.hpp. Relaxed: it's a monotone tally
// read only between frames by tests, never a synchronisation point.
std::atomic<std::uint64_t> g_component_render_calls{0};

// Blit-path telemetry (relaxed; read by the frame profiler).
std::atomic<std::uint64_t> g_blit_rows_epoch_skip{0};   // per-row epoch skip hits
std::atomic<std::uint64_t> g_blit_rows_compared{0};     // rows sent to bulk_eq/memcpy
std::atomic<std::uint64_t> g_blit_entries{0};           // fast-path entries walked
std::uint64_t component_render_calls() noexcept {
    return g_component_render_calls.load(std::memory_order_relaxed);
}
std::uint64_t blit_rows_epoch_skip() noexcept {
    return g_blit_rows_epoch_skip.load(std::memory_order_relaxed);
}
std::uint64_t blit_rows_compared() noexcept {
    return g_blit_rows_compared.load(std::memory_order_relaxed);
}
std::uint64_t blit_entries_walked() noexcept {
    return g_blit_entries.load(std::memory_order_relaxed);
}

// Phase accumulators for the TOP-LEVEL render_tree call (build → layout
// → paint). Nanosecond totals, monotonic; the frame profiler diffs them.
std::atomic<std::uint64_t> g_rt_build_ns{0};
std::atomic<std::uint64_t> g_rt_layout_ns{0};
std::atomic<std::uint64_t> g_rt_paint_ns{0};
std::uint64_t rt_build_ns()  noexcept { return g_rt_build_ns.load(std::memory_order_relaxed); }
std::uint64_t rt_layout_ns() noexcept { return g_rt_layout_ns.load(std::memory_order_relaxed); }
std::uint64_t rt_paint_ns()  noexcept { return g_rt_paint_ns.load(std::memory_order_relaxed); }

void clear_component_cache() noexcept {
    clear_component_cache_impl();
}

// ============================================================================
// Recursive element painter
// ============================================================================

void paint_element(
    const Element& elem,
    Canvas& canvas,
    StylePool& pool,
    const std::vector<layout::LayoutNode>& layout_nodes,
    std::size_t layout_idx,
    int offset_x,
    int offset_y)
{
    const auto& ln = layout_nodes[layout_idx];
    const Rect& computed = ln.computed;

    // Absolute position on canvas.
    int ax = offset_x + computed.pos.x.value;
    int ay = offset_y + computed.pos.y.value;
    int aw = computed.size.width.value;
    int ah = computed.size.height.value;

    Rect abs_rect{{Columns{ax}, Rows{ay}}, {Columns{aw}, Rows{ah}}};

    const PaintCtx c{canvas, pool, layout_nodes, ln, computed, ax, ay, aw, ah, abs_rect};
    visit_element(elem, overload{
        [&](const BoxElement& node)       { paint_box(c, node); },
        [&](const TextElement& node)      { paint_text(c, node); },
        [&](const ElementList& node)      { paint_list(c, node); },
        [&](const ElementListRef& node)   { paint_list_ref(c, node); },
        [&](const ComponentElement& node) { paint_component(c, node); },
    });
}

} // namespace render_detail

Size measure_element(const Element& elem, int max_width, int max_height)
{
    // A real layout pass over the fragment — the exact engine the renderer
    // runs, so the answer can't disagree with the eventual paint. The root
    // keeps its own width/height styles (usually auto): an auto-width
    // container shrink-wraps to its widest line (yoga.cpp §4), an auto-width
    // text leaf reports its natural columns, so the result is the fragment's
    // NATURAL size within the given bounds.
    if (max_width < 0) max_width = 0;
    if (max_height < 0) max_height = 0;
    thread_local std::vector<layout::LayoutNode> nodes;
    thread_local bool in_use = false;   // re-entrant (component measure can nest)
    std::vector<layout::LayoutNode> local;
    const bool was_in_use = in_use;
    auto& buf = was_in_use ? local : nodes;
    buf.clear();
    in_use = true;
    std::size_t root = render_detail::build_layout_tree(elem, buf, /*theme=*/{});
    layout::compute(buf, root, max_width, max_height);
    Size out{Columns{buf[root].computed.size.width.value},
             Rows{buf[root].computed.size.height.value}};
    if (!was_in_use) in_use = false;
    return out;
}

void render_tree(
    const Element& root,
    Canvas& canvas,
    StylePool& pool,
    [[maybe_unused]] const Theme& theme,
    bool auto_height)
{
    // Set the render context so widgets can query available_width() etc.
    // If a parent context exists (e.g. from App::render_frame), this is
    // a no-op override with the same values; if called standalone (tests,
    // one-shot prints), this provides the correct canvas dimensions.
    RenderContext ctx{canvas.width(), canvas.height(), render_generation(), auto_height};
    RenderContextGuard guard(ctx);

    std::vector<layout::LayoutNode> layout_nodes;
    layout_nodes.reserve(128);
    render_tree(root, canvas, pool, theme, layout_nodes, auto_height);
}

int painted_width(const Element& elem, int max_width, int max_height)
{
    if (max_width <= 0) return 0;
    // As tall as the content, not max_height: clearing and diffing a tall
    // scratch canvas was most of the cost.
    const int h = std::clamp(measure_element(elem, max_width, max_height).height.value, 1,
                             std::max(1, max_height));
    // Its own canvas, pool and render context, so a probe from inside a
    // component's render never touches the frame being drawn.
    StylePool pool;
    Canvas scratch(max_width, h, &pool);
    scratch.clear();
    render_tree(elem, scratch, pool, theme::live(), /*auto_height=*/true);
    int w = 0;
    const int rows = std::min(scratch.max_content_row() + 1, scratch.height());
    for (int y = 0; y < rows; ++y)
        w = std::max(w, scratch.last_content_col(y) + 1);
    return std::min(w, max_width);
}

void render_tree(
    const Element& root,
    Canvas& canvas,
    StylePool& pool,
    const Theme& theme,
    std::vector<layout::LayoutNode>& layout_nodes,
    bool auto_height)
{
    // Set the render context if not already set by the parent overload or App.
    RenderContext ctx{canvas.width(), canvas.height(), render_generation(), auto_height};
    // Inherit host-set fields that the dimension-only ctor doesn't carry.
    // inline_min_content is set on the App's persistent render_ctx_ AFTER
    // it computes the composer anti-bounce pad, then App re-invokes
    // render_tree on the SAME tree; without this inheritance the fresh
    // ctx here would zero it and the lazy pad component would read 0.
    if (detail::render_ctx_)
        ctx.inline_min_content = detail::render_ctx_->inline_min_content;
    RenderContextGuard guard(ctx);

    // Cross-frame ComponentElement render cache management.
    //
    // At the OUTERMOST render_tree call: bump the frame counter and
    // evict entries whose last_frame_used is older than the previous
    // frame.  Stable entries (same ComponentElement* still in the
    // tree) get their timestamps refreshed during this frame's
    // measure/paint and survive; ephemeral entries (one-shot
    // components built fresh each frame) age out within a frame of
    // last access.
    //
    // Nested render_tree calls (a component recursively rendering
    // another tree) inherit the existing frame counter — they see the
    // same cache state as their parent.
    auto& depth = render_detail::render_depth();
    bool top_level = (depth == 0);
    ++depth;
    if (top_level) {
        // Bump scroll paint generation and clear the live-states list.
        // Each top-level render walk re-populates the list via the
        // writeback below; any state whose tree was removed since the
        // last paint stops receiving auto-dispatched events.
        ++detail::paint_generation;
        detail::live_scroll_states().clear();
        // Same discipline for the hit-region registry: this frame's
        // paint re-registers every `| hit(id)` box at its new position;
        // targets that left the tree stop hit-testing automatically.
        maya::detail::hit_regions().clear();

        auto& cache = render_detail::component_cache();
        // Evict before bumping current_frame so entries from the
        // previous frame are still tagged "last_frame == previous".
        // The two maps share the same intent (an entry not touched
        // in the immediately preceding frame is gone), but use
        // different retention windows tuned to their identity model.
        const std::uint64_t prev = cache.current_frame;

        // Pointer-keyed entries: strict 1-frame eviction. Cross-frame
        // pointer-keyed REUSE is forbidden anyway (the slow-path lookup
        // gates on `last_frame == current_frame`, see the paint branch
        // below) because the wrapper's lambda may have closed over
        // mutable state, so keeping these around longer would just
        // waste memory on entries we'll never trust again.
        for (auto it = cache.entries.begin(); it != cache.entries.end(); ) {
            if (it->second.last_frame < prev) {
                it = cache.entries.erase(it);
            } else {
                ++it;
            }
        }

        // Hash-keyed entries use an allocation-free LRU. Evict only the
        // accumulated overflow from the tail: each erase is O(1), so the
        // work is amortized over the insertions that created the overflow
        // rather than a full-map scan in a render frame. Running before the
        // frame preserves entries inserted by this frame's measure pass.
        constexpr std::size_t kHashCacheMax = 4096;
        while (cache.entries_by_hash.size() > kHashCacheMax) {
            const CacheId oldest = cache.hash_lru.back();
            auto it = cache.entries_by_hash.find(oldest);
            if (it != cache.entries_by_hash.end())
                erase_hash_cache_entry(cache, it);
            else
                cache.hash_lru.pop_back(); // defensive consistency repair
        }

        // Terminal-resize invalidation. Hash-keyed heights are trusted
        // width-agnostically by the O(1) measure fast path (measure width
        // rarely equals paint width, and height is width-stable for the common
        // card types — so a width check there would defeat the cache every
        // frame). But a GENUINE resize changes the canvas width and can change
        // a component's wrapped height; a stale height then clips/mispositions
        // its content for a frame. Detect the width change ONCE here, at the
        // top level, and drop every cached height+cells so the whole tree
        // re-measures at the new width. A resize is rare, so the full clear is
        // cheap relative to correctness. (Within a single frame the measure and
        // paint widths differ but the canvas width does NOT, so this never
        // fires mid-frame — the O(1) fast path is fully preserved.)
        const int cur_w = canvas.width();
        if (cache.last_canvas_width != -1 && cache.last_canvas_width != cur_w)
            render_detail::clear_component_cache();
        cache.last_canvas_width = cur_w;

        ++cache.current_frame;
    }
    struct Cleanup {
        int* d;
        ~Cleanup() { if (d) --*d; }
    } cleanup{&depth};

    // Phase 1: Build the layout tree (reusing the caller's vector).
    layout_nodes.clear();

    const auto t_build0 = std::chrono::steady_clock::now();
    std::size_t root_idx = render_detail::build_layout_tree(root, layout_nodes, theme);
    const auto t_build1 = std::chrono::steady_clock::now();

    // Phase 2: Constrain root to terminal dimensions.
    layout_nodes[root_idx].style.width  = Dimension::fixed(canvas.width());
    if (!auto_height)
        layout_nodes[root_idx].style.height = Dimension::fixed(canvas.height());

    // Phase 3: Run layout. Positions are parent-relative after this.
    layout::compute(layout_nodes, root_idx, canvas.width(), canvas.height());
    const auto t_layout1 = std::chrono::steady_clock::now();

    // Phase 4: Paint to canvas.
    //
    // A themed canvas is AMBIENT for the entire tree, not just for the
    // subtree of whichever box happens to declare a bg.
    //
    // The terminal cell model does not composite: a style with no bg emits
    // an SGR that resets the cell to the TERMINAL default. So every glyph
    // painted outside a bg-declaring box punched a hole straight through
    // the canvas fill — separator rules, the composer frame, status-bar
    // chrome, anything sitting directly under AppLayout rather than inside
    // a filled container. That is the "holes in the theme" artifact, and
    // chasing it widget-by-widget is endless because the default is wrong
    // rather than any one widget being wrong.
    //
    // Seeding the ambient here makes the canvas colour the DEFAULT that
    // descendants inherit, exactly as the box-level AmbientBgScope already
    // does one level down. A run with an explicit bg still wins, including
    // Color::Kind::Default as the deliberate "I want the terminal's own
    // background" opt-out.
    //
    // Under `native` the theme states no background, so the scope does not
    // engage and nothing changes: the terminal shows through, which is the
    // entire point of native.
    render_detail::AmbientBgScope canvas_ambient(
        theme::owns_canvas(theme) ? std::optional<Color>{theme.background}
                                  : std::nullopt);
    render_detail::paint_element(
        root, canvas, pool, layout_nodes, root_idx,
        /*offset_x=*/0, /*offset_y=*/0);

    // Extend the canvas colour across the rest of each PAINTED row.
    //
    // A text run paints the columns it occupies and no more, so a 20-column
    // label on a 90-column row leaves 70 cells never written — and a
    // terminal renders a never-written cell as ITS background. On a themed
    // canvas that is a ragged hole beside every short row.
    //
    // Doing it HERE rather than with a filled box around the root is what
    // keeps it bounded. A box paints its whole rect, and that rect comes
    // from the host's min_height, which can exceed what the host actually
    // drew — so it colours rows BELOW the content. max_content_row is the
    // real painted extent, captured before this loop so the fill cannot
    // extend it: every cell touched here is inside the frame by
    // construction.
    //
    // Cells already written keep their own style; only the untouched tail
    // of a row is filled. Under `native` the theme states no background, so
    // nothing is filled and the terminal shows through — the entire point
    // of native.
    if (theme::owns_canvas(theme)) {
        const int last = canvas.max_content_row();
        const int w    = canvas.width();
        if (last >= 0 && w > 0) {
            const uint16_t canvas_sid =
                pool.intern(Style{}.with_bg(theme.background));
            for (int y = 0; y <= last; ++y) {
                // Fill every UNSTYLED cell in the row, not just the tail
                // past the last glyph. A row is rarely one contiguous run:
                // centred text, a right-aligned meter, chips separated by
                // gaps all leave untouched cells BETWEEN painted ones, and
                // each of those is a hole the terminal fills with its own
                // background. Walking the row catches them all; cells that
                // already carry a style are left exactly as painted.
                for (int x = 0; x < w; ++x) {
                    const Cell c = canvas.get(x, y);
                    if (c.style_id != 0) continue;          // already styled
                    if (c.character != U' ' && c.character != 0) continue;
                    canvas.set(x, y, c.character == 0 ? U' ' : c.character,
                               canvas_sid);
                }
            }
        }
    }
    const auto t_paint1 = std::chrono::steady_clock::now();
    if (top_level) {
        using namespace render_detail;
        auto ns = [](auto a, auto b) {
            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(b - a)
                    .count());
        };
        g_rt_build_ns.fetch_add(ns(t_build0, t_build1),
                                std::memory_order_relaxed);
        g_rt_layout_ns.fetch_add(ns(t_build1, t_layout1),
                                 std::memory_order_relaxed);
        g_rt_paint_ns.fetch_add(ns(t_layout1, t_paint1),
                                std::memory_order_relaxed);
    }
}

void render_tree_at(
    const Element& root,
    Canvas& canvas,
    StylePool& pool,
    const Theme& theme,
    int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) return;

    // Phase 1: Build the layout tree (reuse thread-local to avoid per-call alloc).
    thread_local std::vector<layout::LayoutNode> layout_nodes;
    layout_nodes.clear();
    std::size_t root_idx = render_detail::build_layout_tree(root, layout_nodes, theme);

    // Phase 2: Constrain root to the sub-region dimensions.
    layout_nodes[root_idx].style.width  = Dimension::fixed(w);
    layout_nodes[root_idx].style.height = Dimension::fixed(h);

    // Phase 3: Run layout within the sub-region bounds.
    layout::compute(layout_nodes, root_idx, w, h);

    // Phase 4: Clip to the sub-region and paint with offset — no clear.
    {
        auto _ = canvas.clip_scope(Rect{
            {Columns{x}, Rows{y}},
            {Columns{w}, Rows{h}}
        });
        render_detail::paint_element(
            root, canvas, pool, layout_nodes, root_idx,
            /*offset_x=*/x, /*offset_y=*/y);
    }
}

} // namespace maya
