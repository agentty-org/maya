#pragma once
// maya::widget::scrollbar — visual indicator for a ScrollState.
//
// The scrollbar is just a widget. It reads from a ScrollState (which the
// framework owns) and emits an Element styled to show where the viewport
// sits inside its content. There is no inheritance, no registration, no
// magic — to make your own scrollbar, build any Element using the same
// public state fields:
//
//     int thumb_h = std::max(1, viewport_h * viewport_h / (viewport_h + s.max_y));
//     int thumb_y = (viewport_h - thumb_h) * s.y / std::max(1, s.max_y);
//     // …emit whatever you want using those.
//
// The framework ships three presets — Line, Block, Slim — and exposes
// every glyph and color via ScrollbarStyle so 90% of customization is
// changing four fields, not rewriting the widget.
//
// Composition:
//
//     auto ui = h(
//         my_content | scroll(state, /*h=*/8) | grow,
//         scrollbar_y(state, /*viewport=*/8)
//     );
//
//     // Or with a style preset:
//     scrollbar_y(state, 8, ScrollbarStyle::block())
//
// 2D combo:
//
//     auto ui = v(
//         h(content | scroll(state, w, h) | grow,
//           scrollbar_y(state, h)),
//         scrollbar_x(state, w)
//     );

#include "../core/scroll_state.hpp"
#include "../dsl.hpp"
#include "../element/element.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace maya {

// ============================================================================
// ScrollbarStyle — glyphs + colors. Customize directly, or use a preset.
// ============================================================================

struct ScrollbarStyle {
    /// Glyph painted for cells the thumb does NOT occupy.
    std::string_view track_glyph = "\xe2\x94\x82";   // │  U+2502 LIGHT VERTICAL
    /// Glyph painted for cells the thumb occupies.
    std::string_view thumb_glyph = "\xe2\x94\x83";   // ┃  U+2503 HEAVY VERTICAL

    /// Same glyphs but for the horizontal scrollbar variant.
    std::string_view track_glyph_h = "\xe2\x94\x80"; // ─  U+2500 LIGHT HORIZONTAL
    std::string_view thumb_glyph_h = "\xe2\x94\x81"; // ━  U+2501 HEAVY HORIZONTAL

    Color track_color = Color::slot(ThemeSlot::Muted);
    Color thumb_color = Color::slot(ThemeSlot::Muted);

    /// Half-cell thumb ends for scrollbar_y. When set, the thumb moves in
    /// half-cell steps: a cell whose lower half is thumb draws
    /// `thumb_lower_half`, one whose upper half is thumb draws
    /// `thumb_upper_half`. Empty = whole cells only.
    std::string_view thumb_lower_half;
    std::string_view thumb_upper_half;

    /// A Panel paints the thumb in its own accent instead of thumb_color.
    bool thumb_follows_accent = false;

    // -- Presets ------------------------------------------------------------

    /// For overlay panels: no track line (beside a border it reads as a
    /// second border), and a right-half block thumb in the panel's accent
    /// that moves in half cells. A block can't be mistaken for a content
    /// rail (┃) the way a heavy line can.
    [[nodiscard]] static ScrollbarStyle overlay() {
        return {
            .track_glyph = " ",
            .thumb_glyph = "\xe2\x96\x90",                 // ▐ right half block
            .track_glyph_h = " ",
            .thumb_glyph_h = "\xe2\x96\x84",               // ▄
            .track_color = Color::slot(ThemeSlot::Muted),
            .thumb_color = Color::slot(ThemeSlot::Primary),
            .thumb_lower_half = "\xe2\x96\x97",            // ▗ lower right quadrant
            .thumb_upper_half = "\xe2\x96\x9d",            // ▝ upper right quadrant
            .thumb_follows_accent = true,
        };
    }

    /// Default — thin line track, heavy line thumb. Dim throughout.
    [[nodiscard]] static ScrollbarStyle line() { return {}; }

    /// Block style — invisible track, solid block thumb. High contrast.
    [[nodiscard]] static ScrollbarStyle block() {
        return {
            .track_glyph = " ",
            .thumb_glyph = "\xe2\x96\x88",                 // █  U+2588 FULL BLOCK
            .track_glyph_h = " ",
            .thumb_glyph_h = "\xe2\x96\x88",
            .track_color = Color::slot(ThemeSlot::Muted),
            .thumb_color = Color::slot(ThemeSlot::Text),
        };
    }

    /// Slim style — left/top-edge ticks. Very thin visual footprint.
    [[nodiscard]] static ScrollbarStyle slim() {
        return {
            .track_glyph = "\xe2\x96\x8f",                 // ▏  U+258F LEFT ONE EIGHTH BLOCK
            .thumb_glyph = "\xe2\x96\x8c",                 // ▌  U+258C LEFT HALF BLOCK
            .track_glyph_h = "\xe2\x96\x81",               // ▁  U+2581 LOWER ONE EIGHTH BLOCK
            .thumb_glyph_h = "\xe2\x96\x84",               // ▄  U+2584 LOWER HALF BLOCK
            .track_color = Color::slot(ThemeSlot::Muted),
            .thumb_color = Color::slot(ThemeSlot::Muted),
        };
    }

    /// Heavy line track + solid block thumb — bold both.
    [[nodiscard]] static ScrollbarStyle heavy() {
        return {
            .track_glyph = "\xe2\x94\x83",                 // ┃  U+2503 HEAVY VERTICAL
            .thumb_glyph = "\xe2\x96\x88",                 // █  U+2588 FULL BLOCK
            .track_glyph_h = "\xe2\x94\x81",               // ━  U+2501 HEAVY HORIZONTAL
            .thumb_glyph_h = "\xe2\x96\x88",               // █
            .track_color = Color::slot(ThemeSlot::Muted),
            .thumb_color = Color::slot(ThemeSlot::Text),
        };
    }

    /// Double-line track + heavy thumb.
    [[nodiscard]] static ScrollbarStyle double_line() {
        return {
            .track_glyph = "\xe2\x95\x91",                 // ║  U+2551 DOUBLE VERTICAL
            .thumb_glyph = "\xe2\x96\x88",                 // █
            .track_glyph_h = "\xe2\x95\x90",               // ═  U+2550 DOUBLE HORIZONTAL
            .thumb_glyph_h = "\xe2\x96\x88",               // █
            .track_color = Color::slot(ThemeSlot::Muted),
            .thumb_color = Color::slot(ThemeSlot::Text),
        };
    }

    /// Dotted track + heavy thumb.
    [[nodiscard]] static ScrollbarStyle dotted() {
        return {
            .track_glyph = "\xe2\x94\x8a",                 // ┊  U+250A LIGHT QUADRUPLE DASH VERTICAL
            .thumb_glyph = "\xe2\x94\x83",                 // ┃
            .track_glyph_h = "\xe2\x94\x88",               // ┈  U+2508 LIGHT QUADRUPLE DASH HORIZONTAL
            .thumb_glyph_h = "\xe2\x94\x81",               // ━
            .track_color = Color::slot(ThemeSlot::Muted),
            .thumb_color = Color::slot(ThemeSlot::Text),
        };
    }

    /// Dashed track + heavy thumb.
    [[nodiscard]] static ScrollbarStyle dashed() {
        return {
            .track_glyph = "\xe2\x95\x8e",                 // ╎  U+254E LIGHT DOUBLE DASH VERTICAL
            .thumb_glyph = "\xe2\x94\x83",                 // ┃
            .track_glyph_h = "\xe2\x95\x8c",               // ╌  U+254C LIGHT DOUBLE DASH HORIZONTAL
            .thumb_glyph_h = "\xe2\x94\x81",               // ━
            .track_color = Color::slot(ThemeSlot::Muted),
            .thumb_color = Color::slot(ThemeSlot::Text),
        };
    }

    /// Braille dots — works well on small monospaced fonts.
    [[nodiscard]] static ScrollbarStyle braille() {
        return {
            .track_glyph = "\xe2\xa0\x87",                 // ⠇  U+2807 LEFT-COLUMN BRAILLE DOTS
            .thumb_glyph = "\xe2\xa1\x87",                 // ⡇  U+2847 LEFT 4 DOTS BRAILLE
            .track_glyph_h = "\xe2\xa3\x80",               // ⣀  U+28C0 BOTTOM BRAILLE DOTS
            .thumb_glyph_h = "\xe2\xa3\xbf",               // ⣿  U+28FF FULL BRAILLE BLOCK
            .track_color = Color::slot(ThemeSlot::Muted),
            .thumb_color = Color::slot(ThemeSlot::Text),
        };
    }

    /// ASCII-only — for terminals that don't render Unicode box-drawing.
    [[nodiscard]] static ScrollbarStyle ascii() {
        return {
            .track_glyph = "|",
            .thumb_glyph = "#",
            .track_glyph_h = "-",
            .thumb_glyph_h = "=",
            .track_color = Color::slot(ThemeSlot::Muted),
            .thumb_color = Color::slot(ThemeSlot::Text),
        };
    }

    /// Shadow gradient — light track with medium-shade thumb.
    [[nodiscard]] static ScrollbarStyle shadow() {
        return {
            .track_glyph = "\xe2\x96\x91",                 // ░  U+2591 LIGHT SHADE
            .thumb_glyph = "\xe2\x96\x93",                 // ▓  U+2593 DARK SHADE
            .track_glyph_h = "\xe2\x96\x91",               // ░
            .thumb_glyph_h = "\xe2\x96\x93",               // ▓
            .track_color = Color::slot(ThemeSlot::Muted),
            .thumb_color = Color::slot(ThemeSlot::Text),
        };
    }

    /// Minimal — invisible track, small one-quarter block thumb.
    [[nodiscard]] static ScrollbarStyle minimal() {
        return {
            .track_glyph = " ",
            .thumb_glyph = "\xe2\x96\x8e",                 // ▎  U+258E LEFT ONE QUARTER BLOCK
            .track_glyph_h = " ",
            .thumb_glyph_h = "\xe2\x96\x82",               // ▂  U+2582 LOWER ONE QUARTER BLOCK
            .track_color = Color::slot(ThemeSlot::Muted),
            .thumb_color = Color::slot(ThemeSlot::Muted),
        };
    }

    /// Neon — line track with bright cyan thumb. Pops against a dark bg.
    [[nodiscard]] static ScrollbarStyle neon() {
        return {
            .track_glyph = "\xe2\x94\x82",                 // │
            .thumb_glyph = "\xe2\x94\x83",                 // ┃
            .track_glyph_h = "\xe2\x94\x80",               // ─
            .thumb_glyph_h = "\xe2\x94\x81",               // ━
            .track_color = Color::slot(ThemeSlot::Muted),
            .thumb_color = Color::slot(ThemeSlot::Link),
        };
    }

    /// Retro — green-on-black solid blocks. CRT vibes.
    [[nodiscard]] static ScrollbarStyle retro() {
        return {
            .track_glyph = " ",
            .thumb_glyph = "\xe2\x96\x88",                 // █
            .track_glyph_h = " ",
            .thumb_glyph_h = "\xe2\x96\x88",               // █
            .track_color = Color::slot(ThemeSlot::Muted),
            .thumb_color = Color::slot(ThemeSlot::Success),
        };
    }

    /// Danger — red thumb on dim track. Use for "scroll past warning"
    /// UIs (terms of service, etc.).
    [[nodiscard]] static ScrollbarStyle danger() {
        return {
            .track_glyph = "\xe2\x94\x82",                 // │
            .thumb_glyph = "\xe2\x96\x88",                 // █
            .track_glyph_h = "\xe2\x94\x80",               // ─
            .thumb_glyph_h = "\xe2\x96\x88",               // █
            .track_color = Color::slot(ThemeSlot::Muted),
            .thumb_color = Color::slot(ThemeSlot::Error),
        };
    }

    /// Pixel — solid horizontal halves stacked. Looks chunky and retro.
    [[nodiscard]] static ScrollbarStyle pixel() {
        return {
            .track_glyph = "\xe2\x96\x80",                 // ▀  U+2580 UPPER HALF BLOCK
            .thumb_glyph = "\xe2\x96\x88",                 // █
            .track_glyph_h = "\xe2\x96\x80",               // ▀
            .thumb_glyph_h = "\xe2\x96\x88",               // █
            .track_color = Color::slot(ThemeSlot::Muted),
            .thumb_color = Color::slot(ThemeSlot::Accent),
        };
    }
};

// ============================================================================
// scrollbar_y — vertical scrollbar driven by ScrollState
// ============================================================================
//
// Renders a column of `viewport_h` cells. The thumb size is proportional
// to viewport / content, position is proportional to current offset.
// When state.max_y == 0 the content fits and a full-height thumb is drawn
// (consistent layout, no jump when content grows past the viewport).
//
// `s` is BORROWED: the returned Element stores &s so the renderer can write
// the painted bar's geometry back (hit-rects for wheel/drag). It must
// outlive the frame, so it belongs in the Model, never in view(). Passing a
// temporary or a view()-local is a dangling write during paint — so the
// rvalue overload is deleted and that mistake won't compile.

[[nodiscard]] inline Element scrollbar_y(const ScrollState& s,
                                         int viewport_h,
                                         ScrollbarStyle style = {}) {
    if (viewport_h <= 0) return Element{TextElement{}};

    const int content_h = viewport_h + s.max_y;
    const int thumb_h   = std::max(1, viewport_h * viewport_h / std::max(1, content_h));
    // Clamp thumb_y to the track. If s.y briefly exceeds s.max_y (e.g.,
    // content size just shrunk before the next clamp() ran), the raw
    // formula would place the thumb past the bottom of the track.
    const int raw_y     = (s.max_y > 0)
        ? (viewport_h - thumb_h) * s.y / s.max_y
        : 0;
    const int thumb_y   = std::clamp(raw_y, 0, std::max(0, viewport_h - thumb_h));

    auto vrepeat = [](std::string_view g, int n) -> std::string {
        std::string out;
        // n glyphs + (n-1) newlines.
        out.reserve(g.size() * static_cast<std::size_t>(n) +
                    static_cast<std::size_t>(std::max(0, n - 1)));
        for (int i = 0; i < n; ++i) {
            out.append(g);
            if (i < n - 1) out += '\n';
        }
        return out;
    };
    auto seg = [&](std::string_view g, Color c, int n) -> Element {
        return Element{TextElement{
            .content = vrepeat(g, n),
            .style   = Style{}.with_fg(c),
        }};
    };

    std::vector<Element> parts;
    if (!style.thumb_lower_half.empty() && !style.thumb_upper_half.empty()) {
        // Half-cell placement. The size stays whole cells (thumb_h, the
        // same number drag uses); only the position gets half steps.
        const int halves   = viewport_h * 2;
        // At least a cell and a half, so a long list still shows a thumb
        // you can find at a glance.
        const int t_len    = std::min(halves, std::max(3, thumb_h * 2));
        const int t_pos    = s.max_y > 0
            ? std::clamp((halves - t_len) * s.y / s.max_y, 0, halves - t_len) : 0;
        auto in = [&](int h) { return h >= t_pos && h < t_pos + t_len; };
        // Runs of the same glyph become one segment.
        std::string_view run_g;
        bool run_thumb = false;
        int  run_n = 0;
        auto flush = [&] {
            if (run_n > 0)
                parts.push_back(seg(run_g, run_thumb ? style.thumb_color
                                                     : style.track_color, run_n));
            run_n = 0;
        };
        for (int i = 0; i < viewport_h; ++i) {
            const bool up = in(2 * i), down = in(2 * i + 1);
            const std::string_view g = up && down ? style.thumb_glyph
                                     : down       ? style.thumb_lower_half
                                     : up         ? style.thumb_upper_half
                                                  : style.track_glyph;
            const bool thumb = up || down;
            if (run_n > 0 && (g != run_g || thumb != run_thumb)) flush();
            run_g = g; run_thumb = thumb; ++run_n;
        }
        flush();
    } else {
        // Stack three segments: track-before-thumb, thumb, track-after-thumb.
        // Filter empties so dsl::v doesn't reserve a row for an empty text node.
        parts.reserve(3);
        if (thumb_y > 0)
            parts.push_back(seg(style.track_glyph, style.track_color, thumb_y));
        if (thumb_h > 0)
            parts.push_back(seg(style.thumb_glyph, style.thumb_color, thumb_h));
        const int after = viewport_h - thumb_y - thumb_h;
        if (after > 0)
            parts.push_back(seg(style.track_glyph, style.track_color, after));
    }

    // Tag with VerticalBar role + state ptr so the renderer's writeback
    // records bar_v_bounds (for future drag-to-scroll / hover features).
    Element bar = dsl::v(std::move(parts)).build();
    if (auto* bx = as_box(bar)) {
        bx->scroll_state = const_cast<ScrollState*>(&s);
        bx->scroll_role  = ScrollRole::VerticalBar;
        // Pin to the natural viewport-height extent: a parent whose
        // align_items is Stretch (the CSS default) must not cross-stretch
        // the bar, or bar_v_bounds diverges from the rendered cells.
        bx->layout.align_self = Align::Start;
    }
    return bar;
}

/// A scrollbar borrows its state for the life of the frame; a temporary or a
/// view()-local would leave the renderer writing into freed stack during
/// paint. Keep the ScrollState in your Model and pass it by reference.
Element scrollbar_y(ScrollState&&, int, ScrollbarStyle = {}) = delete;

// ============================================================================
// scrollbar_x — horizontal scrollbar driven by ScrollState
// ============================================================================
//
// Renders a row of `viewport_w` cells. Same proportions as scrollbar_y.

[[nodiscard]] inline Element scrollbar_x(const ScrollState& s,
                                         int viewport_w,
                                         ScrollbarStyle style = {}) {
    if (viewport_w <= 0) return Element{TextElement{}};

    const int content_w = viewport_w + s.max_x;
    const int thumb_w   = std::max(1, viewport_w * viewport_w / std::max(1, content_w));
    // Clamp — see note in scrollbar_y above.
    const int raw_x     = (s.max_x > 0)
        ? (viewport_w - thumb_w) * s.x / s.max_x
        : 0;
    const int thumb_x   = std::clamp(raw_x, 0, std::max(0, viewport_w - thumb_w));

    auto hrepeat = [](std::string_view g, int n) -> std::string {
        std::string out;
        out.reserve(g.size() * static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) out.append(g);
        return out;
    };
    auto seg = [&](std::string_view g, Color c, int n) -> Element {
        return Element{TextElement{
            .content = hrepeat(g, n),
            .style   = Style{}.with_fg(c),
        }};
    };

    std::vector<Element> parts;
    parts.reserve(3);
    if (thumb_x > 0)
        parts.push_back(seg(style.track_glyph_h, style.track_color, thumb_x));
    if (thumb_w > 0)
        parts.push_back(seg(style.thumb_glyph_h, style.thumb_color, thumb_w));
    const int after = viewport_w - thumb_x - thumb_w;
    if (after > 0)
        parts.push_back(seg(style.track_glyph_h, style.track_color, after));

    // Tag with HorizontalBar role so the renderer's writeback records
    // bar_h_bounds — used by ScrollState::handle to switch wheel events
    // to horizontal pan when the cursor is over this bar.
    Element bar = dsl::h(std::move(parts)).build();
    if (auto* bx = as_box(bar)) {
        bx->scroll_state = const_cast<ScrollState*>(&s);
        bx->scroll_role  = ScrollRole::HorizontalBar;
        // Pin to the natural `viewport_w` extent: a parent whose align_items
        // is Stretch (the CSS default) must not cross-stretch the bar past
        // the width it paints, or the hit-test geometry (bar_h_bounds)
        // diverges from the rendered cells.
        bx->layout.align_self = Align::Start;
    }
    return bar;
}

/// See scrollbar_y: the state is borrowed for the frame, so it can't be a
/// temporary.
Element scrollbar_x(ScrollState&&, int, ScrollbarStyle = {}) = delete;

} // namespace maya
