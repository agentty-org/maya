# Roadmap: Production-Quality TUI Framework

Gap analysis comparing maya to Ink (React for CLI), which powers Claude Code's terminal UI.
All designs below use C++26 idioms that match maya's existing patterns: compile-time DSL with structural NTTPs, type-state safety via `requires`, signal-based reactivity, and zero-cost abstractions.

> **Status:** every feature in the original gap analysis is now implemented —
> see Tiers 1–4 below. The **[Tier 4 API](#tier-4-api)** section documents the
> shipped headers for the last four items (animation, overlays, partial updates,
> syntax highlighting). Remaining work is tracked in the
> [render roadmap](internals/render-roadmap.md) (renderer internals), not here.

## What's been built

The following features from the original roadmap have been fully implemented:

### Tier 1 — Core Infrastructure ✅

- **Scrollable Container** — `widget/scrollable.hpp`: `ScrollableView` with viewport clipping, scrollbar rendering, overflow hidden, and margin offset
- **Text Input** — `widget/input.hpp`: `Input<Cfg>` with cursor, placeholder, password masking, and validation; `widget/textarea.hpp`: `TextArea` for multi-line editing
- **Focus Management** — `FocusNode` integrated into the framework for keyboard navigation across interactive widgets
- **Streaming Text** — Markdown widget handles streaming content; `ThinkingIndicator` provides animated progress during generation

### Tier 2 — Widget Library ✅

- **Select / Menu** — `widget/select.hpp`: `Select` with options and filtering; `widget/menu.hpp`: `Menu` with keyboard navigation
- **Spinner / Progress** — `widget/progress.hpp`: `ProgressBar` with dynamic width
- **50+ additional widgets** across 9 categories:
  - **Input**: Button, Checkbox, Radio, Slider, Input, TextArea, Select, Menu, CommandPalette
  - **Data Display**: Table, List, Tree, Badge, Callout
  - **Navigation**: Tabs, Breadcrumb, ActivityBar
  - **Display**: Divider, Disclosure, Sparkline, KeyHelp
  - **Overlay**: Modal, Popup, Toast
  - **Visualization**: BarChart, LineChart, Gauge, Heatmap, PixelCanvas
  - **Specialized**: Calendar, LogViewer, DiffView, PlanView
  - **Tool Widgets**: FileRef, Message

### Tier 3 — Rich Content ✅

- **Markdown Rendering** — `src/widget/agentty/markdown/`: full renderer supporting headers, code blocks (fenced), ordered/unordered lists, links (OSC 8 hyperlinks), bold/italic emphasis, and tables

### Tier 4 — Motion, Layering, Partial Updates & Highlighting ✅

The four features that were the last remaining roadmap items are now shipped.
They are header-first, allocation-light, and fully tested
(`tests/test_animation.cpp`, `test_overlay_layer.cpp`, `test_static_split.cpp`,
`test_highlight.cpp`).

- **Animation system** — two layers, fully documented on the dedicated
  **[Animation](14-animation.md)** page.
    - *The math* (`core/animation.hpp`): `constexpr` easing curves
      (`ease::linear` → `smootherstep`, plus `in/out_back`), `Tween<T>`
      (fixed-duration eased interpolation with continuity-preserving
      `retarget`), `Spring<T>` (semi-implicit Euler integrator,
      momentum-preserving `set_target`, sub-stepped + clamped against dropped
      frames, `consteval`-validated `SpringParams` + presets), the
      `Animated<T>` wrapper, and `RateCursor` (the constant-glide
      streaming-typewriter integrator — buffer-absorbed pacing à la the Vercel
      AI SDK / ChatGPT, with a bounded catch-up and a finalize deadline ramp).
      Works on arithmetic `T` and `maya::Color`.
    - *The framework* (`core/motion.hpp`): `Clock` (one frame-time source,
      cached `dt`, dropped-frame clamp, remount detection), `Motion<T>` (the
      headline self-driving value — `to()` to aim, `get()` to read, no clock /
      no `dt` / no RAF in widget code), `pulse()`/`loop_phase()` (perpetual
      phase), `Timeline` (keyframe choreography), `Stagger` (index-phased
      fan-out), and `Mount` (ms-since-appeared). The library owns time,
      ticking, frame-request cadence, and lifecycle.
    - *The reveal decorator* (`anim/text_reveal.hpp`): the streaming typewriter
      (scramble tip, hot→cool gradient, ghosted body, sweep cursor, pulsing
      caret) lifted into a height-stable decorator any widget can call on any
      text leaf. Tested in `tests/test_animation.cpp` + `test_motion.cpp`.
      See **[examples/motion_showcase.cpp](14-animation.md#putting-it-together)**.
- **Absolute positioning / z-index** — `app/overlay_layer.hpp`: `OverlayCfg`
  structural NTTP (anchor + z + offset + clamp), the `elem | overlay_<Cfg>`
  pipe and runtime `overlay_at()`, and `OverlayStack` which composites floats
  *after* the flow tree (z-sorted, stable) via `render_tree_at` so a float
  never perturbs the layout beneath it.
- **Static component / partial updates** — `app/static_region.hpp` +
  DECSTBM helpers in `terminal/ansi.hpp`: `StaticSplit` partitions the screen
  into a frozen write-once band and an active redrawn band, enforced by the
  terminal's scrolling region (`\e[top;bottom r`) so the terminal *physically*
  cannot scroll the frozen rows.
- **Themeable syntax highlighting** — `widget/markdown/highlight.hpp` +
  `render/highlight.cpp`: a tree-sitter-compatible `Capture` enum, a
  `constexpr HighlightTheme` (`Capture → Style`) with `terminal` / `monokai` /
  `github_dark` presets and a `constexpr .with()` builder, and a pluggable
  tokenizer — a fast O(n) dependency-free built-in lexer by default, with a
  tree-sitter backend opt-in behind `MAYA_TREE_SITTER` emitting the same
  `Span` stream. See **[Tier 4 API](#tier-4-api)** below for usage.

---

## What maya has today

- **Elm architecture, via jaal**: a program is a jaal program (Model/Msg/init/update/view/subscribe) whose `view()` returns an `Element`
- **Effects as data**: `jaal::Cmd<Msg, ...>` (quit, batch, after, plus the terminal effects a program lists) and `jaal::Sub<Msg>` (every, stream), with input as `on(const Model&, KeyEvent|MouseEvent|PasteEvent|FocusEvent|ResizeEvent)` overloads
- **`run<P>(Options)`**: the single entry point for all interactive apps (fullscreen and inline)
- 90+ widgets across a dozen categories (Input, Data Display, Navigation, Display, Overlay, Visualization, Agent UI, Session/Diagnostics, Status Bar, Tool Widgets)
- Compile-time DSL with both compile-time and runtime pipes
- Runtime pipe system for dynamic values
- Flexbox layout engine
- `pixels()` / `glyphs()` elements over a double-buffered SIMD frame diff at 60fps
- Signal/slot reactivity system (SolidJS-inspired)
- Interactive widgets with FocusNode and keyboard handling
- Full markdown renderer with streaming support
- Mode::Inline for inline terminal rendering
- Braille rendering for high-res graphics
- StylePool with interned styles for fast comparison
- Type-state render pipeline (linear types with `&&`-qualified methods)

---

## Tier 4 API {#tier-4-api}

The shipped APIs for the Tier 4 features. These are the real headers — the
speculative sketches that used to live here have been replaced by what was
actually built.

### Themeable syntax highlighting

`#include <maya/widget/agentty/markdown/highlight.hpp>` — namespace `maya::syntax`.

Tokenizing returns a flat, sorted, non-overlapping span stream over the source
(gaps are implicitly `Capture::None`). A `HighlightTheme` maps each `Capture`
to a `Style`; swap themes by passing a different one.

```cpp
using namespace maya::syntax;

Lang lang = lang_from_tag("cpp");          // "py", "rust", "go", "ts", …
std::vector<Span> spans = highlight(source, lang);

for (const Span& s : spans) {
    Style style = themes::monokai.style_for(s.cap);
    paint(source.substr(s.start, s.len), style);
}
```

The `Capture` enum follows tree-sitter capture names (`Keyword`, `KeywordCtrl`,
`Type`, `Function`, `String`, `Number`, `Comment`, `Constant`, `Operator`,
`Punctuation`, `Preproc`, `Attribute`, `Variable`, `Property`, plus `None`).

`HighlightTheme` is `constexpr` — the palette is baked into the binary. Derive a
variant at compile time:

```cpp
constexpr HighlightTheme my_theme =
    themes::github_dark.with(Capture::Comment, Style{}.with_fg(Color::hex(0x6A737D)));
```

Presets: `themes::terminal` (16-colour), `themes::monokai`, `themes::github_dark`.

**Pluggable backend.** The default tokenizer is a fast, dependency-free O(n)
lexer covering C/C++/Python/Rust/Go/JS/TS/Shell/JSON. To use a real tree-sitter
parser, compile with `-DMAYA_TREE_SITTER` and define:

```cpp
bool maya::syntax::ts_highlight(std::string_view src, Lang lang,
                                std::vector<Span>& out);
```

Return `true` after filling `out` (same `Span` contract) to use the grammar,
or `false` to fall through to the built-in lexer. The theme/render half is
identical — only the tokenizer swaps.

### Animation system

`#include <maya/core/animation.hpp>` — namespace `maya::anim`.

```cpp
using namespace maya::anim;

// Tween: eased A→B over a fixed duration.
auto width = Animated<double>::tween(30.0, 50.0, 0.2, ease::out_cubic);

// Spring: physically-modelled motion; retarget mid-flight keeps momentum.
auto x = Animated<double>::spring(0.0, spring_presets::snappy);
x.set_target(1.0);

// Drive from the host frame loop — there is no hidden clock.
double v = x.tick(dt);          // advance by dt seconds, returns current value
if (!x.done()) request_animation_frame();
```

Easing curves (all `constexpr`): `linear`, `in/out/in_out_quad`,
`in/out/in_out_cubic`, `in_out_quint`, `smoothstep`, `smootherstep`.

`SpringParams` are validated with a `consteval` factory — a degenerate config is
a compile error:

```cpp
constexpr SpringParams p = spring_presets::make(/*stiffness=*/200, /*zeta=*/0.6);
```

Presets: `gentle`, `snappy`, `wobbly`, `stiff`, `molasses`. `Animated<T>`,
`Tween<T>` and `Spring<T>` work on any arithmetic `T` and on `maya::Color`
(componentwise lerp). All are trivially-copyable value types — no heap.

> `std::sqrt` / `std::abs(double)` are not standard-`constexpr` (libstdc++ allows
> them as an extension; libc++ and MSVC do not), so the spring presets use
> maya's own `constexpr` `cmath::c_sqrt` / `c_abs` for the compile-time path.

The block above is the **math** layer (you tick it by hand). For widget code,
reach for the **framework** layer (`#include <maya/core/motion.hpp>`) instead —
it owns the clock, the `dt`, and the frame request so there is *no clock, no
`dt`, and no `request_animation_frame()` in your widget*:

```cpp
using namespace maya::anim;

struct Toggle {
    Motion<double> x{0.0};                       // self-driving value
    void set(bool on) { x.to(on ? 1.0 : 0.0, 0.18); }   // aim
    maya::Element build() const {
        int fill = int(x.get() * width);         // read — ticks + requests frames
        return bar(fill);                        // settled get() == free
    }
};

double breath = pulse(1400.0);                   // perpetual breathing phase
```

The framework adds `Clock`, `Motion<T>`, `pulse()`/`loop_phase()`, `Timeline`
(keyframe choreography), `Stagger` (index-phased fan-out), `Mount`
(ms-since-appeared), and `RateCursor` (the constant-glide streaming-typewriter
integrator). The streaming reveal effect itself is a reusable height-stable
decorator in `#include <maya/anim/text_reveal.hpp>`. **See the dedicated
[Animation](14-animation.md) page for the full reference and
`examples/motion_showcase.cpp` for a one-screen tour.**

### Absolute positioning / z-index overlays

`#include <maya/app/overlay_layer.hpp>` — namespace `maya`.

`OverlayStack` paints floating layers on top of an already-rendered flow canvas,
z-sorted (stable for equal z), each anchored + nudged + clamped on-screen. It
uses `render_tree_at` (clip + offset, no clear) so floats never disturb flow
layout.

```cpp
OverlayStack stack;

// Compile-time config via the pipe — anchor + z + offset baked at the call site.
stack += (tooltip | overlay_<overlay_cfg(Anchor::TopRight, /*z=*/10)>);

// Runtime position (e.g. at a mouse cursor / hit-test result).
stack += overlay_at(menu, /*x=*/cursor_x, /*y=*/cursor_y, /*z=*/20);

// After painting the flow tree into `canvas`:
stack.composite(canvas, pool, theme::dark);
```

`OverlayCfg` is a structural NTTP (`anchor`, `z`, `dx`, `dy`, `max_w`, `clamp`)
built by the `consteval overlay_cfg(…)` validator. Anchors: the nine
edge/corner positions (`TopLeft` … `BottomRight`, `Center`).

### Static component (partial terminal updates)

`#include <maya/app/static_region.hpp>` — namespace `maya`.

`StaticSplit` partitions the terminal into a **frozen** write-once band and an
**active** redrawn band, enforced by DECSTBM (the terminal's vertical scrolling
region). Because the scroll region excludes the frozen rows, the terminal
physically cannot move them — a render bug below the margin cannot corrupt the
completed content above it.

```cpp
StaticSplit split;
std::string out;

split.begin(screen_h, out);                 // arm DECSTBM for the whole screen
split.freeze(completed_msg, width, pool, out);  // promote rows into the frozen band
split.draw_active(live_response, width, pool, out);  // repaint only the active band
// … each frame: split.draw_active(…) …
split.end(out);                             // restore the full-screen region
```

Invariant: at least one active row always survives (`frozen_rows()` never
reaches `screen_h`). The underlying ANSI helpers — `ansi::write_scroll_region`
(DECSTBM), `write_scroll_region_reset`, `write_scroll_up`/`down` (SU/SD) — live
in `terminal/ansi.hpp` for hosts that want to drive the partition directly.

The simpler `StaticRegion` (freeze an element's ANSI once, flush above the
active area) remains for the append-to-scrollback case that doesn't need a
scroll-region partition.

---

## Design Principles

These designs follow maya's existing C++26 patterns:

| Pattern | Where used | C++26 feature |
|---------|-----------|---------------|
| **Structural NTTPs** | `ScrollCfg`, `InputCfg`, `OverlayCfg`, `HighlightTheme` | Structural types as template params |
| **Type-state via `requires`** | Focus modifiers only after `\| focusable`, border color after border | `requires` clauses |
| **Pipe composition** | `\| scrollable`, `\| focusable`, `\| overlay_<>` | `operator\|` overloads |
| **RAII scopes** | `FocusScope` mirrors `ReactiveScope` | Thread-local, deterministic destruction |
| **Signal reactivity** | `ScrollState`, `Input::value_`, `Animated<T>` | `Signal<T>`, `Computed<T>`, `Effect` |
| **`consteval` validation** | `Spring::validate()`, `SpinnerStyle` frame arrays | Compile-time error on invalid config |
| **`constexpr` computation** | Easing functions, braille scrollbar, block progress | Zero-cost runtime |
| **Concepts** | `Node`, event handler constraints, `std::ranges::range` | Concept-based polymorphism |
| **`std::variant` + `visit`** | Markdown AST, Element dispatch | Closed sum types |
| **Linear types (`&&`)** | Extended render pipeline stages | Move-only state transitions |
| **Zero-copy rendering** | `StreamSignal::delta()`, `StaticRegion` | `std::span`, direct string append |
