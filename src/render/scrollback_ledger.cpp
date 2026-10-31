// scrollback_ledger.cpp — seal_measured: the maya-measured seal.
//
// Witness Chain, Layer 6 (Trim Accounting) — the freeze-instant half.
// The ledger already owns the ACCOUNTING (paint-recorded heights →
// harvest() → ScrollbackDebt). This file moves the last host-side
// measurement — the seal-time layout that warms the hash-keyed measure
// cache so the freeze frame is byte-stable — into maya itself.
//
// Before: hosts ran the block through build_layout_tree + compute at a
// width THEY reconstructed ("terminal columns minus the chrome
// paddings I believe wrap my fragment"). That reconstruction was a
// fossil of the drift class the ledger exists to kill: agentty's
// famous -2 vs -4 comment stack (AppLayout pads 2, Conversation pads
// 2, forget one and every wrapped block under-measures on narrow
// terminals — the phone-over-SSH duplication ghost).
//
// After: the paint pass records the ACTUAL width constraint compute()
// handed the ledger's fragment (record_paint_width, renderer.cpp
// ElementListRef arm), and seal_measured lays the new block out at
// exactly that width. There is nothing left for the host to
// reconstruct; the chrome can change shape without any host code
// knowing.

#include "maya/render/scrollback_ledger.hpp"

#include <vector>

#include "maya/core/render_context.hpp"
#include "maya/layout/yoga.hpp"
#include "maya/render/renderer.hpp"

namespace maya {

std::size_t ScrollbackLedger::seal_measured(Element e,
                                            std::size_t fallback_est_rows,
                                            bool separator,
                                            std::uint64_t source) {
    std::size_t rows = fallback_est_rows;

    if (paint_width_ > 0) {
        // Real layout pass at the paint-recorded width. Running this
        // does DOUBLE duty: besides producing the policy height, the
        // ComponentElement auto-measure path inside build_layout_tree
        // renders each hash-keyed component and stores its natural
        // height into the renderer's component cache — refreshing any
        // stale live-phase entry so the freeze frame lays out at the
        // true height (byte-stable seam; no gate recovery).
        //
        // Scoped RenderContext: seal_measured runs from the host's
        // update(), OUTSIDE a render pass, where available_width()
        // falls back to its 80-col default. The component auto-measure
        // path clamps its width against available_width() (the
        // measure/paint hash-slot agreement — see renderer.cpp), so
        // without this guard a terminal wider than ~80+chrome would
        // warm every hash entry at the WRONG width — and
        // store_component_cache's width-keyed replace would evict the
        // paint pass's captured cells, forcing the freeze frame to
        // re-render (the host-side seal-time measure had this same
        // latent flaw). Pinning the context to the fragment's real
        // paint width makes the warm exact at any terminal size.
        RenderContext ctx;
        ctx.width       = paint_width_;
        ctx.auto_height = true;   // sealed prefix lives on an inline surface
        RenderContextGuard guard(ctx);

        thread_local std::vector<layout::LayoutNode> nodes;
        nodes.clear();
        const std::size_t root =
            render_detail::build_layout_tree(e, nodes, /*theme=*/{});
        if (root < nodes.size()) {
            layout::compute(nodes, root, paint_width_);
            const int h = nodes[root].computed.size.height.raw();
            if (h > 0) rows = static_cast<std::size_t>(h);
        }
    }

    seal(std::move(e), rows, separator, source);
    return rows < 1 ? 1 : rows;
}

} // namespace maya
