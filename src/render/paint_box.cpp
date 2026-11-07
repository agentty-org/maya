// src/render/paint_box.cpp — a box: hit region, background, border, children, scroll.
#include "render_internal.hpp"

namespace maya {
namespace render_detail {

void paint_box(const PaintCtx& c, const BoxElement& node) {
    Canvas& canvas = c.canvas;
    StylePool& pool = c.pool;
    const auto& layout_nodes = c.layout_nodes;
    const auto& ln = c.ln;
    const int ax = c.ax;
    const int ay = c.ay;
    const int aw = c.aw;
    const int ah = c.ah;
    const Rect& abs_rect = c.abs_rect;
    // 0. Hit-region writeback: a box tagged `| hit(id)` records its
    //    ABSOLUTE painted rect in the per-frame registry so mouse
    //    coordinates resolve to it via maya::hit_test(). Recorded
    //    BEFORE children paint — children registered after are
    //    later in the vector, and hit_test's backward scan gives
    //    them precedence (innermost/topmost target wins).
    if (node.hit_id != 0 && aw > 0 && ah > 0) {
        maya::detail::hit_regions().push_back(
            {node.hit_id, ax, ay, aw, ah});
    }

    // 1. Fill background if the box has a bg color.
    if (node.style.bg.has_value()) {
        uint16_t bg_style_id = pool.intern(
            Style{}.with_bg(*node.style.bg));
        canvas.fill(abs_rect, U' ', bg_style_id);
    }

    // 1b. Establish the ambient background for this box's entire
    //     subtree (children + stack overlays). Descendant text runs
    //     with no explicit bg inherit it at intern time — see the
    //     TextElement visitor — so glyphs can't punch terminal-
    //     default holes in the fill painted above. RAII: restores
    //     the enclosing ambient when this lambda returns.
    AmbientBgScope ambient_scope(node.style.bg);

    // 2. Draw border.
    if (node.has_border()) {
        Style border_style = node.style;
        // The border paints in one style, so take the first side that states
        // a colour. Reading only `top` left a left-only rail (reasoning's ┃)
        // uncoloured.
        const auto& bc = node.border.colors;
        if (const auto& c = bc.top    ? bc.top
                          : bc.left   ? bc.left
                          : bc.right  ? bc.right
                                      : bc.bottom; c.has_value()) {
            border_style = border_style.with_fg(*c);
        }
        uint16_t border_style_id = pool.intern(border_style);
        paint_border(canvas, node.border, abs_rect, border_style_id);
    }

    // 3. Push clip when children must not paint outside this box.
    //    RAII guard ensures the pop happens even if a child's paint
    //    throws — without it, a thrown paint callback would leave a
    //    stale clip on the stack and silently mangle later rendering.
    //
    //    Three cases force a clip:
    //      (a) overflow:hidden / scroll — the explicit opt-in.
    //      (b) the box has a DEFINITE (non-auto) main/cross size.
    //          A box the user sized explicitly (e.g. card(height=N))
    //          is a bounded region; when its content overflows that
    //          size the flex layout can position the overflow rows
    //          at clamped/overlapping coordinates, and with the
    //          default overflow:visible those rows used to paint ON
    //          TOP of the surviving rows — a longer row's tail then
    //          smeared through a shorter one (e.g. a fixed-height
    //          event-log card showing "…replenishedt 2 pods").
    //      (c) the box has a BORDER. A border is a hard visual
    //          boundary the user drew around the content; children
    //          must never paint past it. Without this an auto-height
    //          card that the flex layout squeezed (e.g. grow=1 cards
    //          on a short terminal) would render its overflowing
    //          children BELOW the closed bottom border — a meter row
    //          escaping through "╰─net ███…─╯". Clipping pins the
    //          content inside the frame and the excess is dropped.
    //
    //    An auto-sized, border-less box shrink-wraps to its content
    //    and can't overflow, so it stays unclipped to preserve any
    //    deliberate overflow:visible behaviour.
    //
    //    NB: clipping is suppressed during an auto-height render
    //    (inline mode / the fullscreen grow-and-retry measure pass).
    //    There the canvas starts smaller than the content and the
    //    host grows it based on how far content painted; a clip to
    //    the undersized box would stop content from extending, the
    //    measured height would never grow, and the screen would stay
    //    blank. Real overflow clipping only matters once the size is
    //    settled, which is the non-auto-height paint.
    bool definite_size = node.layout.width.is_fixed() ||
                         node.layout.width.is_percent() ||
                         node.layout.height.is_fixed() ||
                         node.layout.height.is_percent();
    bool clipping = !is_auto_height() &&
                    (node.overflow == Overflow::Hidden ||
                     node.overflow == Overflow::Scroll ||
                     definite_size ||
                     node.has_border());
    // overflow:hidden / scroll are explicit and must clip even in an
    // auto-height pass (a scroll viewport defines its own bounded box).
    if (node.overflow == Overflow::Hidden ||
        node.overflow == Overflow::Scroll)
        clipping = true;

    bool has_b = node.has_border();
    int content_x = ax + (has_b && node.border.sides.left ? 1 : 0) + node.layout.padding.left;
    int content_y = ay + (has_b && node.border.sides.top ? 1 : 0) + node.layout.padding.top;
    int content_w = std::max(0, aw - node.inner_horizontal());
    int content_h = std::max(0, ah - node.inner_vertical());

    std::optional<Canvas::ClipScope> guard;
    if (clipping) {
        guard.emplace(canvas, Rect{
            {Columns{content_x}, Rows{content_y}},
            {Columns{content_w}, Rows{content_h}}
        });
    }

    // 3b. Writeback: record info into the attached ScrollState
    //     based on this box's role in the scroll system.
    //
    //     - Viewport: compute content extent from children, write
    //       max_x/max_y, store the viewport's painted rect.
    //     - VerticalBar / HorizontalBar: store the bar's painted
    //       rect so mouse hover hit-testing in ScrollState::handle
    //       can route wheel events to the right axis.
    //
    //     We always record bounds (even if max didn't change) so
    //     resize and scroll-position changes don't desync the
    //     hover hit-rects.
    if (node.scroll_state != nullptr &&
        node.scroll_role != ScrollRole::None) {
        auto* s = node.scroll_state;
        // First writeback for this state THIS PAINT: clear bar
        // lists so a state that previously had bars but now
        // doesn't (e.g., user removed the scrollbar widget)
        // stops claiming hit regions. Also register with the
        // live-state list for auto-dispatch.
        if (s->paint_gen_seen != detail::paint_generation) {
            s->paint_gen_seen = detail::paint_generation;
            s->bars_h.clear();
            s->bars_v.clear();
            s->bar_h_bounds = {};
            s->bar_v_bounds = {};
            detail::live_scroll_states().push_back(s);
        }

        if (node.scroll_role == ScrollRole::Viewport) {
            int content_extent_w = 0;
            int content_extent_h = 0;
            for (auto child_layout_idx : ln.children) {
                const auto& cn = layout_nodes[child_layout_idx];
                const int right  = cn.computed.pos.x.value + cn.computed.size.width.value;
                const int bottom = cn.computed.pos.y.value + cn.computed.size.height.value;
                if (right  > content_extent_w) content_extent_w = right;
                if (bottom > content_extent_h) content_extent_h = bottom;
            }
            const int inner_origin_x = (has_b && node.border.sides.left ? 1 : 0) + node.layout.padding.left;
            const int inner_origin_y = (has_b && node.border.sides.top  ? 1 : 0) + node.layout.padding.top;
            const int content_w_total = std::max(0, content_extent_w - inner_origin_x);
            const int content_h_total = std::max(0, content_extent_h - inner_origin_y);
            const int new_max_x = std::max(0, content_w_total - content_w);
            const int new_max_y = std::max(0, content_h_total - content_h);
            if (new_max_x != s->max_x || new_max_y != s->max_y) {
                detail::scroll_writeback_dirty = true;
            }
            s->max_x = new_max_x;
            s->max_y = new_max_y;
            s->viewport_bounds = {content_x, content_y, content_w, content_h};
            s->clamp();
        } else if (node.scroll_role == ScrollRole::VerticalBar ||
                   node.scroll_role == ScrollRole::HorizontalBar) {
            // Record the bar's NATURAL extent — the sum of its
            // children's painted sizes — not the (possibly
            // stretched) outer box. A scrollbar widget emits
            // exactly `viewport_w` cells of glyphs; if the
            // container stretches the widget to a larger width
            // (default flexbox align: Stretch), the trailing
            // cells are empty and clicks there must NOT count
            // toward the bar — otherwise the hit-test math
            // diverges from the rendered thumb position.
            int natural_w = 0;
            int natural_h = 0;
            for (auto child_layout_idx : ln.children) {
                const auto& cn = layout_nodes[child_layout_idx];
                const int right  = cn.computed.pos.x.value + cn.computed.size.width.value;
                const int bottom = cn.computed.pos.y.value + cn.computed.size.height.value;
                if (right  > natural_w) natural_w = right;
                if (bottom > natural_h) natural_h = bottom;
            }
            const int inner_origin_x = (has_b && node.border.sides.left ? 1 : 0) + node.layout.padding.left;
            const int inner_origin_y = (has_b && node.border.sides.top  ? 1 : 0) + node.layout.padding.top;
            natural_w = std::max(0, natural_w - inner_origin_x);
            natural_h = std::max(0, natural_h - inner_origin_y);
            // If the bar has no children somehow (shouldn't
            // happen for a real scrollbar widget) fall back to
            // the outer size.
            const int rect_w = natural_w > 0 ? std::min(aw, natural_w) : aw;
            const int rect_h = natural_h > 0 ? std::min(ah, natural_h) : ah;
            const ScrollRect r{ax, ay, rect_w, rect_h};
            if (node.scroll_role == ScrollRole::VerticalBar) {
                s->bars_v.push_back(r);
                s->bar_v_bounds = r;
            } else {
                s->bars_h.push_back(r);
                s->bar_h_bounds = r;
            }
        }
    }

    // 4. Recurse into children. Apply paint-time scroll offset by
    //    shifting the origin handed to descendants — yoga laid them
    //    out at natural positions in this box's coordinate space,
    //    we translate during paint. The clip rect pushed above for
    //    overflow:Hidden/Scroll prevents anything outside the inner
    //    content rect from being painted, so descendants effectively
    //    "scroll" within the viewport. This is the same mechanism
    //    the web uses (overflow:scroll + scrollTop on the element).
    //
    // Prefer the LIVE scroll_state position over the value captured
    // into `node.layout.scroll_y` at element-build time. The
    // writeback above (3b) just ran s->clamp(), so s->y now
    // reflects the freshly-computed max_y. The captured value in
    // node.layout can be stale by an unbounded amount when the
    // app sets y past max_y to express "stick to bottom after
    // pushing new content" (a one-line idiom for chat scrollback).
    // Without this branch the first frame after such a push
    // translates by the stale (huge) value, pushing every child
    // off-screen and producing a visible blank flicker before the
    // writeback-dirty re-render corrects it.
    const int live_scroll_x = (node.scroll_state != nullptr
                               && node.scroll_role == ScrollRole::Viewport)
        ? node.scroll_state->x : node.layout.scroll_x;
    const int live_scroll_y = (node.scroll_state != nullptr
                               && node.scroll_role == ScrollRole::Viewport)
        ? node.scroll_state->y : node.layout.scroll_y;
    const int child_ox = ax - live_scroll_x;
    const int child_oy = ay - live_scroll_y;
    for (const auto& [child, child_layout_idx] :
             std::views::zip(node.children, ln.children)) {
        paint_element(
            child,
            canvas,
            pool,
            layout_nodes,
            child_layout_idx,
            child_ox,
            child_oy);
    }

    // 5. Stack overlays: children beyond index 0 are painted on top
    //    using independent sub-layout trees, clipped to the same region.
    if (node.is_stack && node.children.size() > 1) {
        for (std::size_t oi = 1; oi < node.children.size(); ++oi) {
            const auto& overlay = node.children[oi];

            thread_local std::vector<layout::LayoutNode> overlay_nodes;
            thread_local bool overlay_in_use = false;
            auto was_in_use = overlay_in_use;
            std::vector<layout::LayoutNode> local_overlay;
            auto& sub = was_in_use ? local_overlay : overlay_nodes;
            sub.clear();
            overlay_in_use = true;

            std::size_t sub_root = build_layout_tree(overlay, sub, {});
            sub[sub_root].style.width  = Dimension::fixed(content_w);
            sub[sub_root].style.height = Dimension::fixed(content_h);
            layout::compute(sub, sub_root, content_w, content_h);

            {
                auto _ = canvas.clip_scope(Rect{
                    {Columns{content_x}, Rows{content_y}},
                    {Columns{content_w}, Rows{content_h}}
                });
                paint_element(overlay, canvas, pool, sub, sub_root,
                              content_x, content_y);
            }   // clip auto-popped here, even on throw

            if (!was_in_use) overlay_in_use = false;
        }
    }

    // Outer `guard` (if engaged) auto-pops here as the lambda returns.
}

} // namespace render_detail
} // namespace maya
