#pragma once
// maya::element::builder — Runtime element construction (internal)
//
// Low-level runtime builders used by maya::dsl to produce Element trees.
// Prefer the compile-time DSL (maya/dsl.hpp) for user-facing code:
//
//   using namespace maya::dsl;
//   constexpr auto ui = v(
//       t<"Hello"> | Bold | Fg<100, 180, 255>,
//       h(t<"A">, t<"B"> | Dim) | border_<Round> | pad<1>
//   );
//   maya::print(ui.build());
//
// These runtime functions (text(), box(), vstack(), hstack()) are retained
// as implementation details and for use inside dyn() escape hatches.

#include "element.hpp"
#include "../layout/columns.hpp"   // kKeepAlways (shared with solve_columns)
#include "../style/gradient.hpp"   // Gradient (shared with gradient()/gradient_rule())
#include "../style/theme.hpp"      // theme::live(), to resolve slots at the gradient boundary

#include <concepts>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// For text() overloads that accept numeric types.
#include <charconv>
#include <array>

namespace maya {

// ============================================================================
// Concepts for operator() child argument deduction
// ============================================================================

// Forward declaration so concepts can reference it.
class BoxBuilder;

/// True for types that are already an Element or implicitly convert to one.
template <typename T>
concept ElementConvertible =
    std::same_as<std::remove_cvref_t<T>, Element>    ||
    std::same_as<std::remove_cvref_t<T>, BoxElement>  ||
    std::same_as<std::remove_cvref_t<T>, TextElement> ||
    std::same_as<std::remove_cvref_t<T>, ElementList> ||
    std::convertible_to<T, Element>;

/// True for BoxBuilder (needs special handling: finalize with no children).
template <typename T>
concept IsBoxBuilder = std::same_as<std::remove_cvref_t<T>, BoxBuilder>;

/// True for string-like types that should be auto-wrapped in text().
template <typename T>
concept TextConvertible =
    !ElementConvertible<T> &&
    !IsBoxBuilder<T> &&
    (std::convertible_to<T, std::string_view> ||
     std::same_as<std::remove_cvref_t<T>, std::string>);

/// True for ranges of Element (e.g., vector<Element>, span<Element>).
template <typename T>
concept ElementRange =
    !ElementConvertible<T> &&
    !IsBoxBuilder<T> &&
    !TextConvertible<T> &&
    std::ranges::range<T> &&
    std::convertible_to<std::ranges::range_value_t<T>, Element>;

/// Anything that operator() can accept as a child.
template <typename T>
concept ChildArg =
    ElementConvertible<T> || IsBoxBuilder<T> || TextConvertible<T> || ElementRange<T>;

// ============================================================================
// BoxBuilder - Fluent builder for BoxElement
// ============================================================================

class BoxBuilder {
    BoxElement element_{};

    // -- Internal: push a single child from a ChildArg ----------------------

    void push_child(Element elem) {
        element_.children.push_back(std::move(elem));
    }

    void push_child(BoxElement elem) {
        element_.children.emplace_back(std::move(elem));
    }

    void push_child(TextElement elem) {
        element_.children.emplace_back(std::move(elem));
    }

    void push_child(ElementList elem) {
        element_.children.emplace_back(std::move(elem));
    }

    // Resolve a single ChildArg into Element(s) and append.
    template <ChildArg T>
    void append_child(T&& child) {
        if constexpr (ElementConvertible<T>) {
            push_child(std::forward<T>(child));
        } else if constexpr (IsBoxBuilder<T>) {
            push_child(static_cast<Element>(std::forward<T>(child)));
        } else if constexpr (TextConvertible<T>) {
            push_child(Element{TextElement{.content = std::string{std::string_view{child}}}});
        } else if constexpr (ElementRange<T>) {
            for (auto&& item : child) {
                push_child(Element{std::forward<decltype(item)>(item)});
            }
        }
    }

public:
    BoxBuilder() = default;

    explicit BoxBuilder(FlexStyle layout) {
        element_.layout = layout;
    }

    // -- Property setters (return *this for chaining) -----------------------

    auto direction(FlexDirection d) -> BoxBuilder&;
    auto wrap(FlexWrap w) -> BoxBuilder&;
    auto padding(int all) -> BoxBuilder&;
    auto padding(int v, int h) -> BoxBuilder&;
    auto padding(int top, int right, int bottom, int left) -> BoxBuilder&;
    auto margin(int all) -> BoxBuilder&;
    auto margin(int v, int h) -> BoxBuilder&;
    auto margin(int top, int right, int bottom, int left) -> BoxBuilder&;
    auto border(BorderStyle bs) -> BoxBuilder&;
    auto border(BorderStyle bs, Color c) -> BoxBuilder&;
    auto border_color(Color c) -> BoxBuilder&;
    auto border_sides(BorderSides sides) -> BoxBuilder&;
    auto border_text(std::string_view content,
                     BorderTextPos pos = BorderTextPos::Top) -> BoxBuilder&;
    auto border_text(std::string_view content,
                     BorderTextPos pos,
                     BorderTextAlign align) -> BoxBuilder&;
    // Second border-text slot (independent position+align). Use for a
    // right-aligned subtitle on the same edge as a left-aligned title:
    //   .border_text(" Title ", Top, Start)
    //   .border_text_end(" 87ms ", Top, End)
    auto border_text_end(std::string_view content,
                         BorderTextPos pos = BorderTextPos::Top,
                         BorderTextAlign align = BorderTextAlign::End) -> BoxBuilder&;
    auto grow(float g = 1.0f) -> BoxBuilder&;
    auto shrink(float s) -> BoxBuilder&;
    auto basis(Dimension d) -> BoxBuilder&;
    auto width(Dimension d) -> BoxBuilder&;
    auto height(Dimension d) -> BoxBuilder&;
    auto min_width(Dimension d) -> BoxBuilder&;
    auto min_height(Dimension d) -> BoxBuilder&;
    auto max_width(Dimension d) -> BoxBuilder&;
    auto max_height(Dimension d) -> BoxBuilder&;
    auto gap(int g) -> BoxBuilder&;
    auto align_items(Align a) -> BoxBuilder&;
    auto align_self(Align a) -> BoxBuilder&;
    auto justify(Justify j) -> BoxBuilder&;
    auto overflow(Overflow o) -> BoxBuilder&;
    auto bg(Color c) -> BoxBuilder&;
    auto fg(Color c) -> BoxBuilder&;
    auto style(Style s) -> BoxBuilder&;

    // -- Finalize with children via operator() ------------------------------

    /// Accept zero or more children and produce the final Element.
    /// Each child can be an Element, BoxBuilder, string, or range<Element>.
    template <ChildArg... Children>
    auto operator()(Children&&... children) -> Element {
        (append_child(std::forward<Children>(children)), ...);
        return Element{std::move(element_)};
    }

    /// Implicit conversion to Element (no-children / self-closing form).
    /// Allows: `auto e = box().grow();` without calling operator().
    operator Element() const& {
        return Element{element_};
    }

    operator Element() && {
        return Element{std::move(element_)};
    }

    // -- Access to the underlying element (escape hatch) --------------------

    [[nodiscard]] const BoxElement& get() const noexcept { return element_; }
    [[nodiscard]] BoxElement& get() noexcept { return element_; }
};

// ============================================================================
// Factory functions
// ============================================================================

namespace detail {

/// Create a new BoxBuilder. Internal — users should use dsl::v() / dsl::h().
[[nodiscard]] auto box() -> BoxBuilder;

/// Create a new BoxBuilder pre-configured with a FlexStyle.
[[nodiscard]] auto box(FlexStyle layout) -> BoxBuilder;

} // namespace detail

// ============================================================================
// operator| — Pipe a Style onto an Element (internal, found via ADL)
// ============================================================================

[[nodiscard]] Element operator|(Element elem, const Style& s);

// ============================================================================
// Layout shortcuts (internal — used by DSL and tests)
// ============================================================================

namespace detail {

[[nodiscard]] auto vstack() -> BoxBuilder;
[[nodiscard]] auto hstack() -> BoxBuilder;
[[nodiscard]] auto center() -> BoxBuilder;

/// Create a z-stack: children layer on top of each other. The first child
/// determines the size; subsequent children paint on top, clipped to that size.
///
/// The stack takes its base layer's FLEX sizing too: a base that grows to
/// fill its slot (a paint()/pixels() canvas, a fill()) makes the stack grow.
/// Without that, `v(zstack({canvas, card}), status)` laid the stack out at
/// the base's minimum (one row): the canvas that filled the screen alone
/// collapsed to a single line the moment a card was layered over it.
[[nodiscard]] inline Element zstack(std::vector<Element> layers) {
    BoxElement box;
    box.layout.direction = FlexDirection::Column;
    box.is_stack = true;
    if (!layers.empty()) {
        const auto grow_of = [](const Element& e) -> float {
            if (const auto* c = std::get_if<ComponentElement>(&e.inner)) return c->layout.grow;
            if (const auto* b = std::get_if<BoxElement>(&e.inner))       return b->layout.grow;
            return 0.f;
        };
        box.layout.grow = grow_of(layers.front());
    }
    box.children = std::move(layers);
    return Element{std::move(box)};
}

} // namespace detail

// ============================================================================
// ComponentBuilder - Fluent builder for ComponentElement
// ============================================================================
// Creates a lazy element that defers rendering until layout allocates a size.
// The render callback receives (width, height) and returns an Element.
//
// Usage:
//   component([](int w, int h) {
//       return LineChart({.series = data, .width = w, .height = h});
//   }).grow(1).border(BorderStyle::Round)
//
// ── COST CONTRACT: read this before wrapping anything big ────────────────
//
// A component is not free deferral — it is a PER-FRAME REBUILD of whatever
// it wraps, unless you give it a stable hash_id.
//
//   1. With no `measure` callback the engine AUTO-MEASURES by literally
//      invoking render() during layout and counting rows. So render() runs
//      during measure AND during paint.
//   2. The cross-frame cache that would absorb that is keyed on hash_id.
//      An EMPTY hash_id falls back to pointer keying, and pointer-keyed
//      cross-frame hits are deliberately REJECTED (the closure may have
//      captured mutated state). Within one frame the measure/paint pair
//      still share a render; across frames they do not.
//
// Net: an un-keyed component re-renders its ENTIRE subtree every frame,
// forever, no matter how static its content is.
//
// That is fine for a leaf that costs nothing to rebuild (a divider rule, a
// one-line status chip — most call sites here). It is a serious bug when
// the wrapped subtree is large or when something on the same frame budget
// is animating.
//
// REAL REGRESSION (maya 8e765ce, reverted): StreamingMarkdown's whole
// document got wrapped in an un-keyed width-aware component so a 2-column
// pad could yield on ultra-narrow surfaces. One cheap decoration dragged
// every committed block plus the live tail into a per-frame rebuild; the
// reveal cursor lost its frame budget and streaming stalls went 6.2x
// (18 -> 112 zero-content frames on reveal_smoothness_probe). Cost showed
// up as +1.00 component render()/frame on reveal_cost_probe.
//
// Non-obvious, and learned by measuring: hoisting the built tree OUT of
// the lambda so the builder body is O(1) does NOT help. The deferral
// itself is the cost, not the child construction.
//
// So, before adding a component() ask:
//   • Is the wrapped subtree cheap to rebuild EVERY frame? — fine.
//   • Does it really need the laid-out width? A value already known at
//     build time needs no component at all.
//   • Otherwise: give it a content-stable .hash_id(), which is what makes
//     both the measure and the paint O(1).
//
// reveal_cost_probe is the regression gate for this class of mistake.

class ComponentBuilder {
    ComponentElement element_{};

public:
    explicit ComponentBuilder(std::function<Element(int, int)> render_fn)
    {
        element_.render = std::move(render_fn);
    }

    auto grow(float g = 1.0f) -> ComponentBuilder& {
        element_.layout.grow = g;
        return *this;
    }

    /// Paint cells directly instead of returning an Element (see paint()).
    auto draw(std::function<void(Canvas&, int x, int y, int w, int h)> fn) -> ComponentBuilder& {
        element_.draw = std::move(fn);
        return *this;
    }

    auto shrink(float s) -> ComponentBuilder& {
        element_.layout.shrink = s;
        return *this;
    }

    auto basis(Dimension d) -> ComponentBuilder& {
        element_.layout.basis = d;
        return *this;
    }

    auto width(Dimension d) -> ComponentBuilder& {
        element_.layout.width = d;
        return *this;
    }

    auto height(Dimension d) -> ComponentBuilder& {
        element_.layout.height = d;
        return *this;
    }

    auto min_width(Dimension d) -> ComponentBuilder& {
        element_.layout.min_width = d;
        return *this;
    }

    auto min_height(Dimension d) -> ComponentBuilder& {
        element_.layout.min_height = d;
        return *this;
    }

    auto max_width(Dimension d) -> ComponentBuilder& {
        element_.layout.max_width = d;
        return *this;
    }

    auto max_height(Dimension d) -> ComponentBuilder& {
        element_.layout.max_height = d;
        return *this;
    }

    auto padding(int all) -> ComponentBuilder& {
        element_.layout.padding = {all, all, all, all};
        return *this;
    }

    auto padding(int v, int h) -> ComponentBuilder& {
        element_.layout.padding = {v, h, v, h};
        return *this;
    }

    auto padding(int top, int right, int bottom, int left) -> ComponentBuilder& {
        element_.layout.padding = {top, right, bottom, left};
        return *this;
    }

    auto margin(int all) -> ComponentBuilder& {
        element_.layout.margin = {all, all, all, all};
        return *this;
    }

    auto align_self(Align a) -> ComponentBuilder& {
        element_.layout.align_self = a;
        return *this;
    }

    /// Supply an explicit measure callback (the component's natural size
    /// for layout). WITHOUT one, the framework auto-measures by RENDERING
    /// the component at an unbounded height (1<<20) and counting the rows
    /// it emits. That is correct for a content-sized render, but CATASTROPHIC
    /// for a HEIGHT-FILLING render that sizes its output to the `h` it is
    /// handed — measured at 1<<20 it would emit ~a million rows and report an
    /// absurd natural height, poisoning its flex-basis so grow can never
    /// expand it. A fill component MUST report a small finite basis here so
    /// flex-grow has something to grow FROM. Prefer the `fill()` factory,
    /// which wires this correctly for you.
    auto measure(std::function<Size(int max_width)> fn) -> ComponentBuilder& {
        element_.measure = std::move(fn);
        return *this;
    }

    // Set the content-stable cache key (Witness Chain).
    //
    // Non-empty CacheId values route this component through the
    // renderer's hash-keyed component_cache, so a fresh
    // ComponentElement value-copied through containers each frame
    // still hits the cells captured on the first paint. Construct
    // the id via `CacheIdBuilder{}.add(...).build()` — the typed
    // builder mixes type tags into the FNV-1a hash, so two ids
    // built from semantically-different inputs can't collide via
    // string-concatenation accidents (e.g. "tool:1:23" vs
    // "tool:12:3").
    //
    // An empty CacheId keeps the pointer-keyed behaviour and pays
    // the full render cost every frame.
    auto hash_id(CacheId id) -> ComponentBuilder& {
        element_.hash_id = id;
        return *this;
    }

    /// Implicit conversion to Element.
    operator Element() const& {
        return Element{element_};
    }

    operator Element() && {
        return Element{std::move(element_)};
    }

    /// Materialize to Element. Having build() makes ComponentBuilder satisfy
    /// the Node concept, so component() / fill() / adapt() / fit_row() /
    /// responsive() / gradient_rule() can be used DIRECTLY as children of the
    /// compile-time v() / h() and accept the runtime | pipes (| grow(1),
    /// | width(n), | hit(id), ...) like any other node — no more routing a
    /// component through a vector<Element> or a runtime builder just to place
    /// it. (The builder's own .grow()/.width(Dimension) methods still work and
    /// avoid the extra wrapper box a pipe introduces.)
    [[nodiscard]] Element build() const& { return Element{element_}; }

    /// Move-out overload for the common case: a TEMPORARY builder.
    ///
    /// `v(rows).build()` and every `... | pipe` chain produce an rvalue
    /// builder that is destroyed on the next line, so copying its element
    /// out is pure waste — and it is not a small waste, because copying a
    /// BoxElement deep-copies the whole child vector and every string in it.
    ///
    /// Measured on a 60-row column (one screen of transcript): 256 allocs
    /// and 135 KB per build, against 61 allocs and 23 KB when the children
    /// are moved. That is ~4x the allocator traffic of an entire frame,
    /// spent duplicating a tree that was about to be thrown away.
    ///
    /// The const& overload above is kept, so a NAMED builder that is reused
    /// after .build() still gets a copy and is unaffected. Overload
    /// resolution picks this one only when the builder is expiring.
    [[nodiscard]] Element build() && { return Element{std::move(element_)}; }
};

// ============================================================================
// component() - Factory for lazy elements
// ============================================================================

namespace detail {

/// Create a lazy element that receives its allocated size before rendering.
/// The callback is called with (width, height) during painting and must
/// return an Element tree that fits within those bounds.
[[nodiscard]] inline auto component(std::function<Element(int, int)> render_fn)
    -> ComponentBuilder
{
    return ComponentBuilder{std::move(render_fn)};
}

// ============================================================================
// fill() - A component that FILLS the size flex allocates to it
// ============================================================================
// The height-responsive (and width-responsive) counterpart to component().
//
// component() sizes to CONTENT: its natural height is whatever its render
// produces, and flex positions it at that size. fill() sizes to its SLOT:
// it grows to consume the space its flex container gives it, and its render
// callback receives the REAL allocated (w, h) at paint time. Size your
// graph / canvas / gauge to `h` and it always fits the box exactly — no
// hand-computed row budget threaded down from the parent, so the estimate
// can never drift from what the layout engine actually allocates.
//
//   // A graph that always fills the space left after the meters:
//   v(
//       fill([&](int w, int h){ return area_chart(data, w, h); }),  // fills
//       meter_row(), meter_row()                                    // natural
//   ) | height(N)     // definite parent → the fill grows into the slack
//
// Mechanics (why this needs a factory, not just component().grow(1)):
//   * A plain grow(1) component with no measure is auto-measured by
//     rendering at an unbounded height and counting rows — a fill render
//     emits ~2^20 rows there and reports a nonsense basis, so grow can
//     never expand it. fill() installs a measure that reports a small
//     fixed minimum (min_w x min_h) instead, giving grow a finite basis.
//   * fill() sets grow(1) so it claims the container's free space.
//
// Requirement: the container must be DEFINITE on the fill axis for grow to
// distribute — an explicit height()/width() on an ancestor, or cross-axis
// stretch inherited from a definite-size parent. Same rule as any grow
// child; an auto-sized container has no free space to hand out. When the
// slot is too small the callback simply receives a small `h` (down to
// min_h) — collapse gracefully there (e.g. return blank() below a floor).
[[nodiscard]] inline auto fill(std::function<Element(int w, int h)> render_fn,
                               int min_w = 0, int min_h = 1)
    -> ComponentBuilder
{
    ComponentBuilder b{std::move(render_fn)};
    b.measure([min_w, min_h](int max_width) -> Size {
        int w = max_width > 0 ? max_width : 0;
        if (w < min_w) w = min_w;
        return {Columns{w}, Rows{min_h < 1 ? 1 : min_h}};
    });
    b.grow(1.0f);
    return b;
}

/// A canvas that fills its slot and paints cells into it directly. The
/// view-layer form of an animation: a fire, a fluid, a ray tracer is a
/// `paint` element beside ordinary widgets, drawn by whatever runtime
/// draws the view, through the same frame diff. No loop of its own.
///
///   v(paint([&](Canvas& c, int x, int y, int w, int h) { fire.draw(c, x, y, w, h); }),
///     status_bar(model))
///
/// Sizing is fill()'s: it grows into the container's free space, so the
/// container needs a definite size on that axis (the screen is one).
[[nodiscard]] inline auto paint(std::function<void(Canvas&, int x, int y, int w, int h)> draw_fn,
                                int min_w = 0, int min_h = 1)
    -> ComponentBuilder
{
    ComponentBuilder b = fill([](int, int) { return Element{}; }, min_w, min_h);
    b.draw(std::move(draw_fn));
    return b;
}

} // namespace detail

// ============================================================================
// measure_element() - Measure an Element tree's natural size
// ============================================================================
// Runs a real layout pass over `elem` (the same engine the renderer uses)
// and returns the size it would occupy given the available bounds. THIS is
// the primitive that makes hand-written width arithmetic obsolete: never
// estimate "2 + host.size()" cells for a styled fragment again — build the
// fragment and ask. Bytes-vs-columns bugs, forgotten gaps, and drifting
// estimates all die here, because the measurement and the eventual paint
// share one source of truth.
//
// Cost: one layout pass over the fragment — trivial for the small pieces
// (header segments, table cells) this is meant for. Fine to call per frame.
// Defined in renderer.cpp.

[[nodiscard]] Size measure_element(const Element& elem,
                                   int max_width,
                                   int max_height = 1 << 20);

// painted_width() - The columns `elem` actually INKS when given `max_width`
// ============================================================================
// measure_element() reports a component as wide as its offer, because a
// component's size is whatever it is handed. This paints into a scratch
// canvas instead and returns the rightmost column with content, so a donut
// or a column of text reports its real width and a chart that stretches
// reports the whole offer. One full paint: use it for layout decisions on
// a handful of cells, not per row. Defined in renderer.cpp.
[[nodiscard]] int painted_width(const Element& elem, int max_width,
                                int max_height = 400);

namespace detail {

// ============================================================================
// adapt() - A component whose BUILD sees its real slot width
// ============================================================================
// The width-responsive counterpart to fill(). fill() is for content that
// SIZES ITSELF to the slot (graphs, canvases); adapt() is for content whose
// STRUCTURE depends on the slot — drop a column below 60 cells, collapse
// labels, switch layouts. The callback receives the real allocated width at
// paint time and returns the tree for exactly that width; natural height is
// auto-measured by the framework from what the callback returns (measure
// literally runs the callback, so measure and paint can never disagree).
//
//   adapt([=](int w) {
//       return w >= 60 ? wide_layout() : narrow_layout();
//   })
//
// Prefer fit_row() for the most common adapt() use case (drop trailing
// items when a row gets tight) — it measures for you.
[[nodiscard]] inline auto adapt(std::function<Element(int w)> render_fn)
    -> ComponentBuilder
{
    return ComponentBuilder{
        [fn = std::move(render_fn)](int w, int) -> Element { return fn(w); }};
}

} // namespace detail

// ============================================================================
// fit_row() - A row that DROPS items when they don't fit
// ============================================================================
// The declarative kill for the "responsive header" bug class. You list the
// row's items once, tag the optional ones with a `keep` rank, and the row
// re-solves itself at every width: items are dropped lowest-`keep` first
// (ties drop the rightmost) until what remains fits. Widths come from
// measure_element() over the REAL styled fragments — no hand-summed cell
// estimates to drift out of sync with the content.

struct FitItem {
    Element el;
    /// Importance: kKeepAlways (default) never drops; lower ranks drop first.
    int keep = kKeepAlways;
};

namespace detail {

// Usage — a status header that sheds detail as the terminal narrows:
//
//   fit_row({
//       {logo},                       // essential (kKeepAlways)
//       {hostname_chip, 5},
//       {kernel_chip,   4},
//       {Element{space}},             // grow spacer: measures 0, always kept
//       {battery_chip,  3},
//       {uptime_chip,   2},
//       {proc_counts,   1},           // first to go
//   })
//
// Notes:
//   * An item is atomic — group an icon+label+value cluster into one
//     Element (h(...)) so it appears/disappears as a unit.
//   * `gap` inserts uniform spacing between KEPT items only; alternatively
//     bake leading spaces/separators into each item so they vanish with it.
//   * Grow spacers (dsl::space) measure 0 wide and still expand at layout,
//     so left/right clusters keep working.
//   * If even the kKeepAlways items overflow, they are all emitted and the
//     row overflows (clip/shrink downstream) — fit_row never drops an
//     essential.
[[nodiscard]] inline auto fit_row(std::vector<FitItem> items, int gap = 0)
    -> ComponentBuilder
{
    return adapt([items = std::move(items), gap](int w) -> Element {
        const std::size_t n = items.size();
        std::vector<char> kept(n, 1);
        std::vector<int> nat(n, 0);
        for (std::size_t i = 0; i < n; ++i)
            nat[i] = measure_element(items[i].el, /*max_width=*/1 << 14)
                         .width.value;

        auto total = [&]() -> long long {
            long long t = 0;
            int c = 0;
            for (std::size_t i = 0; i < n; ++i)
                if (kept[i]) { t += nat[i]; ++c; }
            if (c > 1) t += static_cast<long long>(gap) * (c - 1);
            return t;
        };
        while (total() > w) {
            int best = -1;
            int best_keep = kKeepAlways;
            for (std::size_t i = 0; i < n; ++i) {
                if (!kept[i] || items[i].keep >= kKeepAlways) continue;
                if (items[i].keep <= best_keep) {
                    best = static_cast<int>(i);
                    best_keep = items[i].keep;
                }
            }
            if (best < 0) break;   // only essentials left
            kept[static_cast<std::size_t>(best)] = 0;
        }

        std::vector<Element> out;
        out.reserve(n);
        for (std::size_t i = 0; i < n; ++i)
            if (kept[i]) out.push_back(items[i].el);
        auto b = hstack();
        if (gap > 0) b.gap(gap);
        return b(std::move(out));
    });
}

} // namespace detail

// ============================================================================
// fit_col() - A column that DROPS items when the slot is too SHORT
// ============================================================================
// The vertical counterpart to fit_row(), for the axis everyone forgets:
// HEIGHT. A dashboard that looks right at 40 rows shows garbage at 12 —
// panels crushed, borders sheared. fit_col() lists the column's items once,
// tags the optional ones with a `keep` rank, and sheds lowest-rank first
// until what remains fits the rows it was actually given.
//
//   fit_col({
//       {header},                     // essential (kKeepAlways)
//       {cpu_panel,   5},
//       {mem_panel,   4},
//       {net_panel,   2},
//       {disk_panel,  1},             // first to go on a short terminal
//   })
//
// Heights come from measure_element() over the REAL fragments at the REAL
// slot width (wrapping accounted for) — nothing estimated. Mechanics:
//   * Natural size (measure) is ALL items — in a natural-height context
//     nothing is ever shed; the column just takes its full height.
//   * In a definite-height slot that is SHORTER than natural, flex shrink
//     (on by default) reduces the allocation and the render sees the real
//     budget — that is when shedding kicks in.
//   * If even the kKeepAlways items overflow, they are all emitted and the
//     column overflows (clip downstream) — fit_col never drops an essential.

namespace detail {

[[nodiscard]] inline auto fit_col(std::vector<FitItem> items, int gap = 0)
    -> ComponentBuilder
{
    auto shared = std::make_shared<const std::vector<FitItem>>(std::move(items));

    ComponentBuilder b{[shared, gap](int w, int h) -> Element {
        const auto& its = *shared;
        const std::size_t n = its.size();
        std::vector<char> kept(n, 1);
        std::vector<int> nat(n, 0);
        for (std::size_t i = 0; i < n; ++i)
            nat[i] = measure_element(its[i].el, w > 0 ? w : 1).height.value;

        auto total = [&]() -> long long {
            long long t = 0;
            int c = 0;
            for (std::size_t i = 0; i < n; ++i)
                if (kept[i]) { t += nat[i]; ++c; }
            if (c > 1) t += static_cast<long long>(gap) * (c - 1);
            return t;
        };
        while (total() > h) {
            int best = -1;
            int best_keep = kKeepAlways;
            for (std::size_t i = 0; i < n; ++i) {
                if (!kept[i] || its[i].keep >= kKeepAlways) continue;
                if (its[i].keep <= best_keep) {
                    best = static_cast<int>(i);
                    best_keep = its[i].keep;
                }
            }
            if (best < 0) break;   // only essentials left
            kept[static_cast<std::size_t>(best)] = 0;
        }

        std::vector<Element> out;
        out.reserve(n);
        for (std::size_t i = 0; i < n; ++i)
            if (kept[i]) out.push_back(its[i].el);
        auto vb = vstack();
        if (gap > 0) vb.gap(gap);
        return vb(std::move(out));
    }};

    // Natural size = everything kept — shedding only happens when a
    // definite slot hands us less than this via flex shrink.
    b.measure([shared, gap](int max_width) -> Size {
        int wmax = 0;
        long long hsum = 0;
        int c = 0;
        for (const auto& it : *shared) {
            const Size s = measure_element(it.el, max_width > 0 ? max_width : 1);
            wmax = std::max(wmax, s.width.value);
            hsum += s.height.value;
            ++c;
        }
        if (c > 1) hsum += static_cast<long long>(gap) * (c - 1);
        if (hsum < 1) hsum = 1;
        return {Columns{wmax}, Rows{static_cast<int>(hsum)}};
    });
    return b;
}

} // namespace detail

// ============================================================================
// Pretty text — gradient() / rainbow() (multi-color text via StyledRuns)
// ============================================================================
// A gradient heading is one of the highest-ROI "this looks designed" wins in
// a TUI, and maya already has the machinery: a TextElement carries per-byte
// StyledRuns, each painted with its own color. gradient() splits the string
// into one run per codepoint and colors each by its horizontal position, so
// the color sweeps smoothly across the text. No renderer changes, no per-char
// Element explosion — it is ONE TextElement that wraps, truncates, and
// measures exactly like plain text().
//
//   gradient("MAYA", Color::hex(0xFF5F6D), Color::hex(0xFFC371))
//   gradient("weather", Gradient{{sky, teal, gold}})   // multi-stop
//   rainbow("party mode")                               // full hue sweep
//
// `base` supplies non-color attributes (bold/italic/underline) merged into
// every run — pipe operators can't reach the runs, so pass attributes here:
//   gradient("MAYA", a, b, Style{}.with_bold())

namespace detail {

// Minimal UTF-8 encoder (used by gradient_rule to tile a glyph to width).
inline void encode_utf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// Build one StyledRun per codepoint, colored by a callback mapping the
// codepoint's horizontal cell position (col) + total width to a Style.
template <typename StyleAt>
[[nodiscard]] inline Element paint_per_cp(std::string text, StyleAt&& style_at) {
    std::vector<StyledRun> runs;
    runs.reserve(text.size());
    const int total = string_width(text);
    int col = 0;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t start = pos;
        const char32_t cp = decode_utf8(text, pos);   // advances pos
        const int w = is_wide_char(cp) ? 2 : 1;
        runs.push_back(StyledRun{start, pos - start, style_at(col, total)});
        col += w;
    }
    // Assemble without designated-init: TextElement carries mutable cache
    // fields with defaults, and newer clang warns on every designated-init
    // that omits them (-Wmissing-designated-field-initializers).
    TextElement te;
    te.content = std::move(text);
    te.wrap    = TextWrap::NoWrap;
    te.runs    = std::move(runs);
    return Element{std::move(te)};
}

} // namespace detail

/// Horizontal gradient text: each character colored by its position, from
/// the gradient's first stop on the left to its last on the right.
[[nodiscard]] inline Element gradient(std::string text, Gradient g, Style base = {}) {
    return detail::paint_per_cp(std::move(text),
        [g = std::move(g), base](int col, int total) -> Style {
            const float denom = total > 1 ? static_cast<float>(total - 1) : 1.0f;
            Style s = base;
            s.fg = g.at(static_cast<float>(col) / denom);
            return s;
        });
}

/// Two-color gradient text: from (left) → to (right).
///
/// Takes Color so a caller may pass a theme slot, and resolves here — the
/// boundary between "what the author wrote" and "what gets blended". Gradient
/// itself holds LitColor, because interpolation needs channels.
[[nodiscard]] inline Element gradient(std::string text, Color from, Color to,
                                      Style base = {}) {
    const Theme& th = theme::live();
    return gradient(std::move(text), Gradient::two(th.resolve(from), th.resolve(to)), base);
}

/// Full-spectrum rainbow text (HSL hue sweep across the width).
[[nodiscard]] inline Element rainbow(std::string text, Style base = {},
                                     float saturation = 0.85f, float lightness = 0.62f) {
    return detail::paint_per_cp(std::move(text),
        [base, saturation, lightness](int col, int total) -> Style {
            const float denom = total > 0 ? static_cast<float>(total) : 1.0f;
            Style s = base;
            s.fg = Color::hsl(360.0f * static_cast<float>(col) / denom,
                              saturation, lightness);
            return s;
        });
}

namespace detail {

// ============================================================================
// gradient_rule() - A full-width horizontal divider with a color gradient
// ============================================================================
// Responsive by construction: it measures the width it is allotted and tiles
// a glyph across it, coloring the line from the gradient's first stop to its
// last. Perfect for section dividers and hero underlines that should always
// span the pane, at any terminal size.
//
//   gradient_rule(Color::hex(0x7F5AF0), Color::hex(0x2CB67D))
//   gradient_rule(Gradient{{a, b, c}}, U'━')
[[nodiscard]] inline auto gradient_rule(Gradient g, char32_t glyph = U'─')
    -> ComponentBuilder
{
    return adapt([g = std::move(g), glyph](int w) -> Element {
        if (w <= 0) return Element{ElementList{}};
        std::string line;
        line.reserve(static_cast<std::size_t>(w) * 3);
        for (int i = 0; i < w; ++i) encode_utf8(line, glyph);
        return gradient(std::move(line), g);
    });
}

[[nodiscard]] inline auto gradient_rule(Color from, Color to, char32_t glyph = U'─')
    -> ComponentBuilder
{
    const Theme& th = theme::live();
    return gradient_rule(Gradient::two(th.resolve(from), th.resolve(to)), glyph);
}

} // namespace detail

// ============================================================================
// place() - Position content within the slot it is given
// ============================================================================
// The declarative answer to "center this", "pin it bottom-right", "top-left".
// place() fills the space its flex parent allocates and positions the child
// on both axes per HAlign x VAlign. Because it FILLS (like a grow child), it
// tracks any terminal resize with zero arithmetic — the child just stays put
// at its corner / edge / center of an ever-changing box.
//
//   place(spinner)                                   // dead center (default)
//   place(hint, HAlign::Right, VAlign::Bottom)       // status corner
//   place(logo, HAlign::Center, VAlign::Top)
//
// Needs a definite slot to fill (an explicit size on an ancestor, or the
// cross-stretch every flex container gives its children by default) — the
// same rule as any grow child.

enum class HAlign : std::uint8_t { Left, Center, Right };
enum class VAlign : std::uint8_t { Top, Middle, Bottom };

namespace detail {

[[nodiscard]] inline Element place(Element child, HAlign h = HAlign::Center,
                                  VAlign v = VAlign::Middle) {
    const Justify main = v == VAlign::Top    ? Justify::Start
                       : v == VAlign::Bottom ? Justify::End
                                             : Justify::Center;
    const Align cross = h == HAlign::Left  ? Align::Start
                      : h == HAlign::Right ? Align::End
                                           : Align::Center;
    auto b = vstack();
    b.grow(1.0f);
    b.justify(main);
    b.align_items(cross);
    return b(std::move(child));
}

} // namespace detail

// ============================================================================
// responsive() - Pick a layout by width breakpoint
// ============================================================================
// Named-tier sugar over adapt(): list your breakpoints as { min_width,
// builder }, and the widest tier whose min_width the slot satisfies wins.
// If the slot is narrower than every tier, the smallest tier is used (so
// there is always something to draw). The builder receives the real width.
//
//   responsive({
//       { 0,   [](int){ return compact_view(); } },   // < 80 cols
//       { 80,  [](int){ return two_pane(); } },        // 80..119
//       { 120, [](int w){ return three_pane(w); } },   // >= 120
//   })

struct Bp {
    int min_width = 0;
    std::function<Element(int w)> build;
};

namespace detail {

[[nodiscard]] inline auto responsive(std::vector<Bp> tiers) -> ComponentBuilder {
    return adapt([tiers = std::move(tiers)](int w) -> Element {
        const Bp* chosen = nullptr;
        for (const auto& bp : tiers)
            if (w >= bp.min_width &&
                (chosen == nullptr || bp.min_width >= chosen->min_width))
                chosen = &bp;
        if (chosen == nullptr)          // narrower than every tier → smallest
            for (const auto& bp : tiers)
                if (chosen == nullptr || bp.min_width < chosen->min_width)
                    chosen = &bp;
        if (chosen == nullptr || !chosen->build) return Element{ElementList{}};
        return chosen->build(w);
    });
}

} // namespace detail

// ============================================================================
// pick() - The first alternative that FITS (measured, no breakpoints)
// ============================================================================
// SwiftUI's ViewThatFits, for terminals. Give the alternatives in order of
// preference — richest first — and the slot gets the first one whose REAL
// measured width fits. No breakpoints to pick, no widths to estimate: the
// decision comes from measuring the actual styled fragments, so it stays
// correct when a label, icon, or font changes.
//
//   pick({
//       h(icon, text(" "), text(host), text(" · "), text(kernel)),  // rich
//       h(icon, text(" "), text(host)),                             // medium
//       icon,                                                       // tiny
//   })
//
// The LAST alternative is the fallback — it is used even when it does not
// fit (something must render). Alternatives are measured at their natural
// unwrapped width; multi-line alternatives work (their widest line decides).

namespace detail {

[[nodiscard]] inline auto pick(std::vector<Element> alternatives)
    -> ComponentBuilder
{
    return adapt([alts = std::move(alternatives)](int w) -> Element {
        if (alts.empty()) return Element{ElementList{}};
        for (const auto& alt : alts) {
            if (measure_element(alt, /*max_width=*/1 << 14).width.value <= w)
                return Element{alt};
        }
        return Element{alts.back()};   // fallback: least-detailed alternative
    });
}

// ============================================================================
// clamp() - Cap content width on huge terminals (and center it)
// ============================================================================
// libadwaita's AdwClamp, for terminals. Full-width prose on a 300-column
// ultrawide is unreadable — the eye loses the line on the way back. clamp()
// lets content use the full slot up to `max_width`, then stops growing and
// centers it, like a web page's container column. One number: the widest
// the content still looks good.
//
//   clamp(article, 100)        // never wider than 100 cells, centered
//   clamp(dialog, 60)
//   clamp(toast, 60, HAlign::Right)   // corner-anchored (toasts, popups)
//
// Below max_width it is a transparent wrapper — the child gets the whole
// slot. Composes with everything: clamp(col({...}), 120) gives a dashboard
// a maximum "design width" while row/grid keep re-solving inside it.

[[nodiscard]] inline auto clamp(Element el, int max_width,
                                HAlign align = HAlign::Center)
    -> ComponentBuilder
{
    return adapt([el = std::move(el), max_width, align](int w) -> Element {
        if (max_width <= 0 || w <= max_width) return Element{el};
        auto inner = vstack();
        inner.width(Dimension::fixed(max_width));
        auto outer = hstack();
        outer.justify(align == HAlign::Left    ? Justify::Start
                      : align == HAlign::Right ? Justify::End
                                               : Justify::Center);
        return outer(inner(Element{el}));
    });
}

} // namespace detail

namespace detail {

// ============================================================================
// nothing() - Zero-height placeholder (transparent empty fragment)
// ============================================================================
// Use for view slots that should consume no rows when their content is
// absent (e.g. an in-flight thinking block when the agent isn't thinking).
// Returns an empty ElementList — a flex fragment with no children, which
// the layout engine treats as zero rows / zero columns. Distinct from
// blank() which is a one-row spacer.

[[nodiscard]] inline Element nothing() {
    return Element{ElementList{}};
}

// ============================================================================
// list_ref() - Borrow a vector of Elements without copying
// ============================================================================
// Renders the pointed-to vector as a transparent fragment, identical
// in semantics to wrapping the vector in an ElementList — but without
// the per-frame deep copy. Suitable when the application's Model
// holds a stable vector (e.g. frozen scrollback) and view() is
// called synchronously between updates so the pointer remains valid.

[[nodiscard]] inline Element list_ref(const std::vector<Element>* items) {
    return Element{ElementListRef{items}};
}

[[nodiscard]] inline Element list_ref(const std::vector<Element>& items) {
    return Element{ElementListRef{&items}};
}

// ============================================================================
// ledger_ref() - Borrow a ScrollbackLedger's sealed blocks (measured)
// ============================================================================
// Same zero-copy semantics as list_ref over ledger.elements(), PLUS the
// paint pass records each block's laid-out height back into the ledger
// every frame (Witness Chain — Trim Accounting). Hosts that front-trim
// a sealed prefix MUST render it through this so ledger.harvest() mints
// commit counts from maya's own measurements. Declared here; defined in
// dsl.hpp region after ScrollbackLedger is complete via the include in
// element.hpp consumers — the ledger header is standalone, so include
// it directly.

[[nodiscard]] Element ledger_ref(const ScrollbackLedger& ledger);

} // namespace detail

// ============================================================================
// Public API — promote runtime builders out of detail::
// ============================================================================
// These are the runtime counterpart to the compile-time DSL (dsl::v, dsl::h).
// Use these when you need runtime-configured borders, colors, padding, etc.
//
//   box().border(Round).border_color(status_color)
//       .border_text(title, BorderTextPos::Top)
//       .padding(0, 1, 0, 1)(children)

using detail::box;
using detail::vstack;
using detail::hstack;
using detail::center;

} // namespace maya
