# API Reference

Complete reference for all public types, functions, and constants in maya.

## Table of Contents

- [App Framework](#app-framework) (run, Options, Program, effects, input, keymap, print)
- [DSL Nodes](#dsl-nodes)
- [DSL Style Tags](#dsl-style-tags)
- [DSL Layout Tags](#dsl-layout-tags)
- [DSL Runtime Pipes](#dsl-runtime-pipes)
- [DSL Factory Functions](#dsl-factory-functions)
- [DSL Constants](#dsl-constants)
- [Element Types](#element-types)
- [Style System](#style-system)
- [Color](#color)
- [Theme](#theme)
- [Border](#border)
- [Layout Types](#layout-types)
- [Event System](#event-system)
- [Event Predicates](#event-predicates)
- [Signals](#signals)
- [Canvas](#canvas)
- [Widgets](#widgets)
- [Core Types](#core-types)

---

## App Framework

`#include <maya/host/run.hpp>` (it includes `<maya/maya.hpp>`), link `maya::app`.
An app is a [jaal](../third_party/jaal/README.md) program whose `view()`
returns an `Element`; maya is to jaal what Ink is to React. The rules are in
[internals/design.md](internals/design.md).

### run\<P\>()

```cpp
template <Program P>
int run(Options cfg = {}, jaal::run_options opt = {});
```

Takes the terminal (raw mode, alt screen or an inline region), runs `P` on
jaal until it quits, gives the terminal back on every path, and returns the
program's exit code (70 if the terminal couldn't be taken).

### Options

```cpp
struct Options {
    std::string_view title        = "";               // window title (OSC 0)
    int              fps          = 0;                // >0: redraw continuously; 0: only on change
    bool             mouse        = false;            // mouse reporting
    bool             hover_motion = false;            // also bare motion (mode 1003)
    Mode             mode         = Mode::Fullscreen; // or Mode::Inline (renders into scrollback)
    RenderBackend    backend      = RenderBackend::Ansi;
    Theme            theme        = theme::native;
    bool             enhanced_keyboard = true;        // kitty keyboard protocol when available
};
```

Prefer `Sub::every` in `subscribe()` over `fps`: a timer you can drop when
nothing moves costs nothing while idle.

### Program concept

```cpp
template <class P>
concept Program = jaal::Program<P> && jaal::Viewable<P, Element>;
```

The shape:

```cpp
struct Counter {
    struct Model { int count = 0; };                   // default-constructible, all state
    struct Inc {}; struct Quit {};
    using Msg = std::variant<Inc, Quit>;               // may nest variants (domain groups)

    using Cmd = jaal::Cmd<Msg>;                        // + the terminal effects you use
    using Sub = jaal::Sub<Msg>;                        // timers/streams only; optional

    static Cmd init(Model&);                           // optional
    static Cmd update(Model& m, Inc);                  // exactly one per alternative
    static Cmd update(Model& m, Quit);
    static Element view(const Model& m);               // pure
    static std::optional<Msg> on(const Model&, const KeyEvent&);  // optional; one per input kind
    static Sub subscribe(const Model& m);              // optional: timers/streams; diffed every step
    static auto subs_key(const Model& m);              // optional: what subscribe() reads
};
static_assert(Program<Counter>);
```

A message without a handler or an effect not in the `Cmd` row is a compile
error. Optional hooks read by the
host: `static std::uint64_t visual_hash(const Model&)` (skip the view when
unchanged), `static bool needs_warmup(const Model&)`.

### Effects: Cmd

The core effects are jaal's: `Cmd::quit(code)`, `Cmd::after(dur, msg)`,
`Cmd::task(fn, args..., mapper)` (a worker thread; the mapper turns its
result into a message), `Cmd::send(msg)`, `Cmd::batch(...)`, `map`/`map_with`
for composing child programs. `Cmd::perform<Found>(body, args...)` /
`Cmd::attempt<Ok, Err>(...)` deliver a body's result as a message. Keyed
effects replace the running one with the same key: `Cmd::task("search", body, args...)`,
`Cmd::after("save", 1s, Msg{})`, `Cmd::cancel("search")`. See jaal's README.

maya adds the terminal's own effects. List the ones a program uses in its
`Cmd` row, e.g. `jaal::Cmd<Msg, set_title, write_clipboard>`, and return the
payload as a `Cmd`: `return Cmd(SetTitle{"build: ok"});`

| Effect | Payload | Does |
|---|---|---|
| `set_title` | `SetTitle{std::string}` | window title |
| `write_clipboard` | `WriteClipboard{std::string}` | OSC 52 copy |
| `query_clipboard` | `QueryClipboard{}` | OSC 52 paste request; the answer arrives as a `PasteEvent` |
| `emit_host_sequence` | `EmitHostSequence{std::string}` (see `osc(code, payload)`) | raw sequence to the terminal, out of band |
| `commit_scrollback` | `CommitScrollback{ScrollbackDebt}` — use `commit_from<Cmd>(ledger.harvest())` | inline: move finished rows into scrollback |
| `commit_overflow` | `CommitOverflow{}` | inline: commit rows that overflowed the viewport |
| `force_redraw` | `ForceRedraw{}` | repaint everything |
| `reset_inline` | `ResetInline{}` | inline: start a fresh frame below |
| `set_mouse` | `SetMouse{bool}` | mouse capture on/off at runtime |
| `suspend` | `Suspend<Msg>{std::function<Msg()>}` | hand the tty to a child ($EDITOR, a pager); its return is the next message |

`terminal_cmd<Msg>` is a `Cmd` with all of them.

### Input: on()

Input is one static `on(const Model&, const E&)` overload per kind the
program handles. It reads the live model when the event arrives and returns
at most one message; a kind with no overload is dropped.

| Event | Carries |
|---|---|
| `KeyEvent` | `key`, `mods`, `raw_sequence` |
| `MouseEvent` | `{button, kind (Press/Release/Move), x, y (1-based), mods}` — needs `.mouse = true` |
| `PasteEvent` | `{content}` |
| `FocusEvent` | `{focused}` |
| `ResizeEvent` | `{width, height}` (cells); the first arrives at startup |

`maya::terminal_events` is the variant of all five (same as `maya::Event`).
A program may declare `using Event = ...;` — `jaal::headless<App>` /
`jaal::sim<App>` pick it up, and a listed kind with no `on()` is a compile
error. Signals arrive the same way: `on(const Model&, jaal::sig)`.

```cpp
static std::optional<Msg> on(const Model&, const KeyEvent& e) {
    if (ctrl_is(e, 'c')) return Quit{};
    return Key{e};                                     // let update() decide
}
static Sub subscribe(const Model& m) {
    if (m.paused) return Sub::none();                  // no clock while paused
    return Sub::every(16ms, Tick{});
}
```

### Subscriptions: Sub

`Sub` is sources only: `Sub::every(period, msg)`, `Sub::stream(...)`,
`Sub::batch(...)`, `Sub::none()`. A program whose only input is events has no
`subscribe()` at all.

### keymap\<Msg\> and key predicates

```cpp
static std::optional<Msg> on(const Model&, const KeyEvent& k) {
    static const keymap<Msg> keys{{'q', Quit{}}, {SpecialKey::Up, Up{}}, {' ', Toggle{}}};
    return keys(k);
}
```

Keys are a `char` or `SpecialKey`, matched with no modifiers ('q' doesn't
fire on Ctrl+Q); first entry wins; lookup is O(1).
Predicates for richer `on()` handlers: `key_is(k, 'c')`, `key_is(k, U'é')`,
`key_is(k, SpecialKey::Up)`, `ctrl_is(k, 'c')`, `alt_is(k, 'x')`.

### Scroll views

Keep a `mutable ScrollState` in the model and pipe content through
`| scroll(state, h)`. The terminal forwards mouse wheel and scrollbar drags
to scroll views painted last frame; keys are the program's own — route them
with a message and `state.handle(key, viewport_h)` in `update`.

### print()

```cpp
#include <maya/print.hpp>                      // included by maya.hpp
void print(const Element& root);               // at the terminal's width
void print(const Element& root, int width);
std::string render_to_string(const Element& root, int width = 80);
std::string render_to_string_ansi(const Element& root, int width = 80);
```

Static output with no runtime and no terminal mode changes: reports,
tables, tests.

### Screen and terminal_host

`maya::Screen` (`<maya/screen.hpp>`) is the terminal device: `open(Options)`,
`read()` (parsed events), `present(element)`, `set_title`, `set_mouse`,
`commit_scrollback`, `suspend(fn)`, frame flow control (`ready()`,
`round_trip()`). `maya::terminal_host<P>` adapts a `Screen` to jaal's host
protocol; `run<P>()` is `Screen::open` + `terminal_host` + `jaal::run`. Use
them directly only for a custom driver or a test.

## DSL Nodes

All nodes satisfy the `Node` concept:

```cpp
template <typename T>
concept Node = requires(const T& n) {
    { n.build() } -> std::convertible_to<Element>;
};

template <typename T>
concept DslChild = Node<T> || ElementRange<T>;
```

`DslChild` is what `v()` and `h()` accept — either a `Node` (compile-time or
runtime) or an `ElementRange` (e.g. `std::vector<Element>`).

### TextNode

```cpp
template <Str S, CTStyle Sty = CTStyle{}>
struct TextNode {
    Element build() const;
};
```

Created via `t<"...">`.

### RuntimeTextNode

```cpp
template <typename S>
struct RuntimeTextNode {
    S content;
    Style style{};
    TextWrap wrap{TextWrap::Wrap};

    operator Element() const;
    Element build() const;
};
```

Created via `text()`. Supports `| Bold`, `| Fg<R,G,B>`, etc.

### BoxNode

```cpp
template <FlexDirection Dir, BoxCfg Cfg, typename... Children>
struct BoxNode {
    std::tuple<Children...> children;
    Element build() const;
};
```

Created via `v()` and `h()`. Supports layout pipes.

### DynNode

```cpp
template <typename F>
struct DynNode {
    F fn;
    Element build() const;  // Calls fn()
};
```

Created via `dyn()`.

### MapNode

```cpp
template <typename R, typename Proj>
struct MapNode {
    R range;
    Proj proj;
    Element build() const;  // Iterates range, applies proj
};
```

Created via `map()`.

### SpacerNode

```cpp
struct SpacerNode {
    operator Element() const;
    Element build() const;  // BoxElement with grow=1
};
```

### SepNode

```cpp
struct SepNode {
    operator Element() const;
    Element build() const;  // Horizontal separator line
};
```

### VSepNode

```cpp
struct VSepNode {
    operator Element() const;
    Element build() const;  // Vertical separator line
};
```

### BlankNode

```cpp
struct BlankNode {
    operator Element() const;
    Element build() const;  // Empty TextElement
};
```

---

## DSL Style Tags

```cpp
inline constexpr StyTag<...> Bold;
inline constexpr StyTag<...> Dim;
inline constexpr StyTag<...> Italic;
inline constexpr StyTag<...> Underline;
inline constexpr StyTag<...> Strike;
inline constexpr StyTag<...> Inverse;

template <uint8_t R, uint8_t G, uint8_t B>
inline constexpr StyTag<...> Fg;

template <uint8_t R, uint8_t G, uint8_t B>
inline constexpr StyTag<...> Bg;
```

---

## DSL Layout Tags

```cpp
template <int T, int R = T, int B = T, int L = R>
inline constexpr PadTag<T,R,B,L> pad;

template <int G>
inline constexpr GapTag<G> gap_;

template <BorderStyle BS>
inline constexpr BorderTag<BS> border_;

template <uint8_t R, uint8_t G, uint8_t B>
inline constexpr BColTag<R,G,B> bcol;    // Requires border first

template <int G = 1>
inline constexpr GrowTag<G> grow_;

template <int W>
inline constexpr WidthTag<W> w_;        // fixed width; also wraps text nodes

template <int H>
inline constexpr HeightTag<H> h_;       // fixed height
```

---

## DSL Runtime Pipes

Runtime pipe tags for dynamic values. Same `|` syntax as compile-time pipes.

### Layout Pipes

| Function | Tag Type | Description |
|----------|----------|-------------|
| `padding(int)` / `padding(int,int)` / `padding(int,int,int,int)` | `RPad` | Runtime padding |
| `gap(int)` | `RGap` | Gap between children |
| `margin(int)` / `margin(int,int)` / `margin(int,int,int,int)` | `RMargin` | Outer margin |
| `grow(float g = 1.0f)` | `RGrow` | Flex grow factor |
| `width(int)` | `RWidth` | Fixed width |
| `height(int)` | `RHeight` | Fixed height |

### Border Pipes

| Function | Tag Type | Description |
|----------|----------|-------------|
| `border(BorderStyle)` | `RBorder` | Border style |
| `bcolor(Color)` | `RBCol` | Border color |
| `btext(string, pos = Top, align = Start)` | `RBText` | Border text label (pos/align default) |

### Style Pipes

| Function | Tag Type | Description |
|----------|----------|-------------|
| `fgc(Color)` | `RFg` | Foreground color |
| `bgc(Color)` | `RBg` | Background color |

### Alignment Pipes

| Function | Tag Type | Description |
|----------|----------|-------------|
| `align(Align)` | `RAlign` | Cross-axis alignment |
| `justify(Justify)` | `RJust` | Main-axis distribution |
| `overflow(Overflow)` | `ROvf` | Overflow behavior |

### Scroll Pipes

Wrap content in a scroll viewport backed by a caller-owned `ScrollState`. The
renderer applies the scroll offset and writes `max_x`/`max_y` back after layout
so clamping is automatic.

| Function | Description |
|----------|-------------|
| `scroll(ScrollState&)` | Scroll on both axes, viewport = allocated size |
| `scroll(ScrollState&, int viewport_h)` | Vertical scroll, fixed viewport height |
| `scroll(ScrollState&, int w, int h)` | Both axes, fixed viewport w×h |
| `scrolly(ScrollState&, int h)` | Vertical-only scroll, fixed height |
| `scrollx(ScrollState&, int w)` | Horizontal-only scroll, fixed width |

### Text-Wrap Tags

| Tag | Effect on a `text(...)` node |
|-----|------------------------------|
| `clip` | `TextWrap::TruncateEnd` — hard-truncate at the box edge |
| `nowrap` | `TextWrap::NoWrap` — overflow past the edge, never wrap |

### WrappedNode

```cpp
template <Node Inner>
struct WrappedNode;
```

Created automatically when a runtime pipe is applied to any Node. Satisfies `Node`.
Multiple runtime pipes chain onto the same WrappedNode without extra nesting.

---

## DSL Factory Functions

```cpp
// Compile-time text
template <Str S>
inline constexpr TextNode<S> t;

// Vertical stack — accepts Nodes and ElementRanges (e.g. vector<Element>)
template <DslChild... Cs>
constexpr auto v(Cs... cs) -> BoxNode<Column, BoxCfg{}, Cs...>;

// Horizontal stack — accepts Nodes and ElementRanges (e.g. vector<Element>)
template <DslChild... Cs>
constexpr auto h(Cs... cs) -> BoxNode<Row, BoxCfg{}, Cs...>;

// Runtime builders (promoted from detail namespace)
auto box() -> BoxBuilder;      // Base builder
auto vstack() -> BoxBuilder;   // Column direction
auto hstack() -> BoxBuilder;   // Row direction
auto center() -> BoxBuilder;   // Centered, grow=1

// Z-stack — layer children on top of one another
Element zstack(std::vector<Element> layers);

// Measure-aware component: receives the (width, height) it was allotted
Element component(std::function<Element(int w, int h)> fn);

// Responsive layout toolkit (see "Responsive layout" below)
Size measure_element(const Element& el, int max_w, int max_h = 1 << 20);
auto fill(std::function<Element(int w, int h)> fn, int min_w = 0, int min_h = 1);
auto adapt(std::function<Element(int w)> fn);
auto fit_row(std::vector<FitItem> items, int gap = 0);
auto fit_col(std::vector<FitItem> items, int gap = 0);
auto pick(std::vector<Element> alternatives);
auto clamp(Element el, int max_width, HAlign align = HAlign::Center);
auto responsive(std::vector<Bp> tiers);
Element place(Element child, HAlign h = HAlign::Center, VAlign v = VAlign::Middle);

// The grid — responsive layout with one number (see "Responsive layout" below)
auto row(std::vector<Element> cells, int min_width = 24) -> ComponentBuilder;
Element col(std::vector<Element> cells, int gap = 0);
auto grid(std::vector<Element> cells, int min_width) -> ComponentBuilder;
auto sidebar(Element rail, Element main, int width)  -> ComponentBuilder;

// Pretty text (see "Gradients" below)
Element gradient(std::string text, Color from, Color to, Style base = {});
Element gradient(std::string text, Gradient g, Style base = {});
Element rainbow(std::string text, Style base = {},
                float saturation = 0.85f, float lightness = 0.62f);
auto    gradient_rule(Color from, Color to, char32_t glyph = U'─');
auto    gradient_rule(Gradient g, char32_t glyph = U'─');

// Empty element (renders nothing, satisfies Node)
Element nothing();

// Zero-copy references (no element copy): render an externally-owned
// element vector / sealed ScrollbackLedger in place
Element list_ref(const std::vector<Element>* items);
Element ledger_ref(const ScrollbackLedger& ledger);

// Runtime text
template <typename S>
auto text(S&& content, Style s = {}) -> RuntimeTextNode<decay_t<S>>;

template <typename S>
auto text(S&& content, Style s, TextWrap w) -> RuntimeTextNode<decay_t<S>>;

// Dynamic node
template <typename F>
auto dyn(F&& fn) -> DynNode<decay_t<F>>;

// Map range
template <std::ranges::range R, typename Proj>
auto map(R&& range, Proj&& proj) -> MapNode<decay_t<R>, decay_t<Proj>>;

// each() — alias for map(), for discoverability
template <std::ranges::range R, typename Proj>
auto each(R&& range, Proj&& proj);

// Spacer/separator/blank (function forms)
constexpr auto spacer()    -> SpacerNode;
constexpr auto separator() -> SepNode;
constexpr auto blank()     -> BlankNode;
```

---

## DSL Constants

```cpp
inline constexpr SpacerNode space;
inline constexpr SepNode    sep;
inline constexpr VSepNode   vsep;
inline constexpr BlankNode  blank_;

inline constexpr BorderStyle Round  = BorderStyle::Round;
inline constexpr BorderStyle Single = BorderStyle::Single;
inline constexpr BorderStyle Thick  = BorderStyle::Bold;
inline constexpr BorderStyle Double = BorderStyle::Double;
```

---

## Element Types

### Element

```cpp
struct Element {
    std::variant<BoxElement, TextElement, ElementList> inner;

    // Implicit constructors from each variant type
    Element(BoxElement);
    Element(TextElement);
    Element(ElementList);

    Element build() const;  // Returns *this (satisfies Node concept)
};
```

### TextElement

```cpp
struct TextElement {
    std::string content;
    Style       style{};
    TextWrap    wrap{TextWrap::Wrap};

    Size measure(int max_width) const;
    std::vector<std::string> format(int max_width) const;
};
```

### BoxElement

```cpp
struct BoxElement {
    FlexStyle             layout{};
    Style                 style{};
    BorderConfig          border{};
    Overflow              overflow{Overflow::Visible};
    std::vector<Element>  children;

    bool has_border() const;
    int  inner_horizontal() const;  // padding + border horizontal
    int  inner_vertical() const;    // padding + border vertical
};
```

### ElementList

```cpp
struct ElementList {
    std::vector<Element> items;
};
```

### TextWrap

```cpp
enum class TextWrap {
    Wrap,              // Word wrap at container width
    TruncateEnd,       // "Long te..."
    TruncateMiddle,    // "Lon...xt"
    TruncateStart,     // "...g text"
    NoWrap             // Overflow
};
```

---

## Style System

### Style

```cpp
struct Style {
    std::optional<Color> fg, bg;
    bool bold = false, dim = false, italic = false;
    bool underline = false, strikethrough = false, inverse = false;

    Style with_fg(Color c) const;
    Style with_bg(Color c) const;
    Style with_bold(bool v = true) const;
    Style with_dim(bool v = true) const;
    Style with_italic(bool v = true) const;
    Style with_underline(bool v = true) const;
    Style with_strikethrough(bool v = true) const;
    Style with_inverse(bool v = true) const;

    Style merge(const Style& other) const;
    bool  empty() const;
    std::string to_sgr() const;              // resolves via the live theme
    template <class R> std::string to_sgr(R&& resolve) const;  // explicit theme
};

Style operator|(const Style& lhs, const Style& rhs);  // Merge
```

---

## Color

```cpp
class Color {
public:
    enum class Kind { Named, Indexed, Rgb };

    // Named colors (constexpr)
    static constexpr Color black();
    static constexpr Color red();
    static constexpr Color green();
    static constexpr Color yellow();
    static constexpr Color blue();
    static constexpr Color magenta();
    static constexpr Color cyan();
    static constexpr Color white();
    static constexpr Color bright_black();   // gray
    static constexpr Color bright_red();
    static constexpr Color bright_green();
    static constexpr Color bright_yellow();
    static constexpr Color bright_blue();
    static constexpr Color bright_magenta();
    static constexpr Color bright_cyan();
    static constexpr Color bright_white();
    static constexpr Color gray();

    // RGB (constexpr)
    static constexpr Color rgb(uint8_t r, uint8_t g, uint8_t b);

    // Hex (consteval — compile-time only)
    static consteval Color hex(uint32_t rgb);

    // HSL (constexpr)
    static constexpr Color hsl(float h, float s, float l);

    // Indexed (256-color)
    static Color indexed(uint8_t index);

    // Accessors
    Kind kind() const;
    uint8_t r() const, g() const, b() const;
    uint8_t index() const;

    // Adjustment (constexpr)
    Color lighten(float amount) const;
    Color darken(float amount) const;
    Color to_rgb() const;   // resolve Named/Indexed → true RGB channels

    // SGR sequences
    std::string fg_sgr() const;
    std::string bg_sgr() const;
};
```

---

## Theme

```cpp
struct Theme {
    Color primary, secondary, accent;
    Color success, error, warning, info;
    Color text, inverse_text, muted;
    Color surface, background, border;
    Color diff_added, diff_removed, diff_changed;
    Color highlight, selection, cursor, link;
    Color placeholder, shadow, overlay;

    static constexpr Theme derive(Theme base, auto&& patch);
};

namespace theme {
    inline constexpr Theme dark;
    inline constexpr Theme light;
    inline constexpr Theme dark_ansi;
    inline constexpr Theme light_ansi;
}
```

---

## Border

### BorderStyle

```cpp
enum class BorderStyle {
    None, Single, Double, Round, Bold,
    SingleDouble, DoubleSingle, Classic, Arrow
};
```

### BorderSides

```cpp
struct BorderSides {
    bool top = false, right = false, bottom = false, left = false;

    static BorderSides all();
    static BorderSides none();
    static BorderSides horizontal();  // top + bottom
    static BorderSides vertical();    // left + right
};
```

### BorderText

```cpp
struct BorderText {
    std::string    content;
    BorderTextPos  position = BorderTextPos::Top;
    BorderTextAlign align   = BorderTextAlign::Start;
    int            offset   = 0;
};
```

### BorderColors

```cpp
struct BorderColors {
    std::optional<Color> top, right, bottom, left;
    static BorderColors uniform(Color c);
};
```

### BorderConfig

```cpp
struct BorderConfig {
    BorderStyle  style = BorderStyle::None;
    BorderSides  sides{};
    BorderColors colors{};
    std::optional<BorderText> text;

    bool empty() const;
};
```

---

## Layout Types

### FlexDirection

```cpp
enum class FlexDirection { Row, Column, RowReverse, ColumnReverse };
```

### FlexWrap

```cpp
enum class FlexWrap { NoWrap, Wrap, WrapReverse };
```

### Align

```cpp
enum class Align { Start, Center, End, Stretch, Baseline, Auto };
```

### Justify

```cpp
enum class Justify { Start, Center, End, SpaceBetween, SpaceAround, SpaceEvenly };
```

### Overflow

```cpp
enum class Overflow { Visible, Hidden, Scroll };
```

### Dimension

```cpp
struct Dimension {
    enum class Kind { Auto, Fixed, Percent };
    Kind  kind;
    float value;

    static Dimension auto_();
    static Dimension fixed(int v);
    static Dimension percent(float v);

    bool is_auto() const;
    bool is_fixed() const;
    bool is_percent() const;
    int  resolve(int parent) const;
};

Dimension operator""_pct(unsigned long long v);  // 50_pct
```

### FlexStyle

```cpp
struct FlexStyle {
    FlexDirection direction = FlexDirection::Row;
    FlexWrap      wrap      = FlexWrap::NoWrap;
    Align         align_items = Align::Stretch;
    Align         align_self  = Align::Auto;
    Justify       justify     = Justify::Start;
    float         grow   = 0;
    float         shrink = 1;
    Dimension     basis  = Dimension::auto_();
    Dimension     width  = Dimension::auto_();
    Dimension     height = Dimension::auto_();
    Dimension     min_width  = Dimension::auto_();
    Dimension     min_height = Dimension::auto_();
    Dimension     max_width  = Dimension::auto_();
    Dimension     max_height = Dimension::auto_();
    int           gap = 0;
    Edges<int>    padding{};
    Edges<int>    margin{};
};
```

### Edges\<T\>

```cpp
template <typename T>
struct Edges {
    T top{}, right{}, bottom{}, left{};

    Edges() = default;
    Edges(T all);            // uniform
    Edges(T v, T h);         // vertical, horizontal
    Edges(T t, T r, T b, T l);

    T horizontal() const;    // left + right
    T vertical() const;      // top + bottom
};
```

---

## Responsive Layout

Measure-driven primitives for layouts that restructure with the terminal size.
Full treatment in [Responsive Layouts](15-responsive.md). All are in `maya::dsl`
(and `maya::`); `solve_columns` and its types live in
`<maya/layout/columns.hpp>`.

### row() / col() / grid() / GridOpts

```cpp
// Cells side by side, sharing the width equally and EXACTLY (largest-
// remainder split — no ragged right edge) — wrapping, then stacking, by
// itself as the slot narrows: 4-across → 2×2 → one column. `min_width` is
// the one number: how wide one cell needs to be to look right.
auto row(std::vector<Element> cells, int min_width = 24) -> ComponentBuilder;

// Cells stacked top to bottom, each stretched to the full width (flex
// cross-stretch — the GTK "fill"). Pipe `| grow(1)` onto the child that
// should take the leftover height. Compose: col({ row({a, b}), table }).
Element col(std::vector<Element> cells, int gap = 0);

struct GridOpts {
    int  min       = 24;     // a cell's comfortable minimum width (columns)
    int  max_cols  = 0;      // cap cells-per-row; 0 = as many as fit
    int  gap_x     = 1;      // blank columns between cells
    int  gap_y     = 0;      // blank rows between rows
    bool grow_rows = false;  // rows share surplus height (definite slot)
};

// row's engine with the knobs exposed. Re-solved from the REAL slot width
// per frame (adapt() underneath), so nested grids collapse independently.
// A short last row keeps the same cell width so columns line up.
auto grid(std::vector<Element> cells, GridOpts opts = {}) -> ComponentBuilder;
auto grid(std::vector<Element> cells, int min_width)      -> ComponentBuilder;

//   row({cpu, mem, net, disk})        // side by side; stacks by itself
//   grid(cells, {.min = 24, .max_cols = 2})   // capped 2-across flow
```

### sidebar() / SidebarOpts

```cpp
struct SidebarOpts {
    int  width       = 32;    // the rail's fixed width (columns)
    int  stack_below = 0;     // stack when slot < this; 0 = auto (2×width)
    int  gap         = 1;     // blank columns between rail and main
    bool right       = false; // rail on the right instead of the left
};

// Fixed-width rail beside a main pane that takes the rest; the pair stacks
// vertically (reading order preserved) when the slot is too narrow. The
// default threshold (2×width) keeps them side-by-side only while the main
// pane gets at least as much as the rail.
auto sidebar(Element rail, Element main, SidebarOpts opts = {}) -> ComponentBuilder;
auto sidebar(Element rail, Element main, int width)             -> ComponentBuilder;

//   sidebar(row({cpu, mem, net, disk}), proc_table, 42)
//   // wide: 42-cell rail + table · medium: stats flow over the table
//   // narrow: one column — a whole dashboard in two lines.
```

### measure_element()

```cpp
// Runs a real layout pass over `el` and returns its natural size within the
// given bounds. The measurement uses the SAME engine the renderer uses, so it
// can never disagree with the eventual paint. Cheap enough to call per frame.
Size measure_element(const Element& el, int max_width, int max_height = 1 << 20);
```

Pass a large `max_width` (e.g. `1 << 14`) for the natural one-line width; pass a
real width to learn how many rows the fragment wraps to.

### fill()

```cpp
// A component that GROWS to fill the (w, h) its flex container allocates, then
// calls `fn` with the real allocated size at paint time. `min_w`/`min_h` set
// the measured minimum so grow has a finite basis. Sets grow(1) internally.
// The container must be DEFINITE on the fill axis for it to expand.
auto fill(std::function<Element(int w, int h)> fn, int min_w = 0, int min_h = 1)
    -> ComponentBuilder;
```

### adapt()

```cpp
// A component that BUILDS a different tree depending on the width it is given.
// `fn` receives the real allocated width at paint time; natural height is
// auto-measured from what `fn` returns (measure runs the callback).
auto adapt(std::function<Element(int w)> fn) -> ComponentBuilder;
```

### fit_row() / FitItem

```cpp
struct FitItem {
    Element el;
    int     keep = kKeepAlways;   // importance; lower ranks drop first
};

// A row that DROPS items when they don't fit: lowest-`keep` first (ties drop
// the rightmost) until the remainder fits. Widths come from measure_element
// over the real fragments. `kKeepAlways` items never drop. `gap` spaces the
// KEPT items. Grow spacers (dsl::space) measure 0 and always survive.
auto fit_row(std::vector<FitItem> items, int gap = 0) -> ComponentBuilder;

// The vertical fit_row: DROPS items when the slot is too SHORT, lowest-`keep`
// first, until what remains fits the rows actually given. Heights come from
// measure_element at the real slot width (wrapping accounted for). Natural
// size is ALL items — shedding only happens when a definite-height slot
// hands it fewer rows (flex shrink, on by default, delivers the budget).
auto fit_col(std::vector<FitItem> items, int gap = 0) -> ComponentBuilder;
```

### pick()

```cpp
// SwiftUI's ViewThatFits: the FIRST alternative whose real measured width
// fits the slot renders; richest first, the LAST is the always-rendered
// fallback. No breakpoints — the decision measures the actual fragments.
auto pick(std::vector<Element> alternatives) -> ComponentBuilder;
```

### clamp()

```cpp
// libadwaita's AdwClamp: content uses the full slot up to max_width, then
// stops growing and aligns (Center by default; Left/Right for corner-
// anchored content like toasts) — a web page's container column.
// Transparent below max_width. The "too wide" half of responsive design.
auto clamp(Element el, int max_width,
           HAlign align = HAlign::Center) -> ComponentBuilder;
```

### responsive() / Bp

```cpp
struct Bp {
    int min_width = 0;
    std::function<Element(int w)> build;
};

// Named width breakpoints over adapt(): the widest tier whose min_width the
// slot satisfies builds the view (the builder receives the real width). If
// the slot is narrower than every tier, the smallest tier is used.
auto responsive(std::vector<Bp> tiers) -> ComponentBuilder;
```

### place() / HAlign / VAlign

```cpp
enum class HAlign { Left, Center, Right };
enum class VAlign { Top, Middle, Bottom };

// Fill the slot the flex parent allocates and pin `child` at the given
// corner / edge / center. Tracks every resize with zero arithmetic.
Element place(Element child, HAlign h = HAlign::Center,
              VAlign v = VAlign::Middle);
```

!!! note
    `ComponentBuilder` (returned by `component` / `fill` / `adapt` / `fit_row`
    / `responsive` / `gradient_rule`) satisfies the `Node` concept — components
    go directly in `v()` / `h()` and accept runtime pipes (`| grow(1)`,
    `| width(40)`, `| hit(id)`).

### solve_columns() / ColSpec / ColPlan

```cpp
#include <maya/layout/columns.hpp>

inline constexpr int kKeepAlways = std::numeric_limits<int>::max();

struct ColSpec {
    int   min    = 1;             // minimum content width (cells)
    int   max    = 0;             // growth cap; 0 = unbounded (weight > 0 only)
    float weight = 0.0f;          // share of surplus space; 0 = fixed at min
    int   keep   = kKeepAlways;    // drop order: LOWER dropped first
};

struct ColPlan {
    std::vector<int> width;       // solved widths; 0 = dropped
    int gap = 0;

    bool has(std::size_t i) const;   // is column i visible?
    int  at(std::size_t i) const;    // solved width (0 when dropped/out of range)
    int  used() const;               // total cells: visible widths + inter-column gaps
};

// Solve one shared width plan for a table, so the header row and every body row
// read the same column widths. Drop phase (lowest keep first) then weighted
// surplus waterfill clamped at max. With an unbounded weighted column the plan
// fills `avail` exactly. Pure arithmetic — no Element types, no layout engine.
ColPlan solve_columns(std::span<const ColSpec> cols, int avail, int gap = 1);
```

---

## Gradients

Multi-color "pretty" primitives. Full treatment in
[Styling › Gradients](03-styling.md#gradients).

### Gradient

```cpp
#include <maya/maya.hpp>          // or <maya/style/gradient.hpp> standalone

struct Gradient {
    std::vector<LitColor> stops;

    Gradient(std::initializer_list<LitColor> stops);
    static Gradient two(LitColor from, LitColor to);

    // Sample at t ∈ [0,1] (clamped): linear RGB blend across the two nearest
    // stops. Named/Indexed stops project via to_rgb() first.
    Color at(float t) const;
};
```

### gradient() / rainbow() / gradient_rule()

```cpp
// Horizontal gradient text — ONE TextElement with per-codepoint StyledRuns,
// so it wraps/truncates/measures exactly like plain text(). `base` carries
// non-color attributes (bold/italic) into every run.
Element gradient(std::string text, Color from, Color to, Style base = {});
Element gradient(std::string text, Gradient g, Style base = {});

// Full HSL hue sweep across the text width.
Element rainbow(std::string text, Style base = {},
                float saturation = 0.85f, float lightness = 0.62f);

// Full-width divider that re-tiles `glyph` to its real allocated width and
// sweeps the gradient across it. Responsive by construction.
auto gradient_rule(Color from, Color to, char32_t glyph = U'─') -> ComponentBuilder;
auto gradient_rule(Gradient g,           char32_t glyph = U'─') -> ComponentBuilder;
```

---

## Event System

### Event

```cpp
using Event = std::variant<KeyEvent, MouseEvent, PasteEvent, FocusEvent, ResizeEvent>;
```

### KeyEvent

```cpp
using Key = std::variant<CharKey, SpecialKey>;

struct CharKey { char32_t codepoint; };

enum class SpecialKey {
    Up, Down, Left, Right, Home, End,
    PageUp, PageDown, Tab, BackTab,
    Backspace, Delete, Insert, Enter, Escape,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12
};

struct Modifiers {
    bool ctrl = false, alt = false, shift = false, super_ = false;
    bool none() const;
};

struct KeyEvent {
    Key         key;
    Modifiers   mods;
    std::string raw_sequence;
};
```

### MouseEvent

```cpp
enum class MouseButton { Left, Right, Middle, ScrollUp, ScrollDown, None };
enum class MouseEventKind { Press, Release, Move };

struct MouseEvent {
    MouseButton    button;
    MouseEventKind kind;
    Columns        x;
    Rows           y;
    Modifiers      mods;
};
```

### Other Events

```cpp
struct PasteEvent  { std::string content; };
struct FocusEvent  { bool focused; };
struct ResizeEvent { Columns width; Rows height; };
```

---

## Event Predicates

```cpp
// Keyboard
bool key(const Event&, char c);
bool key(const Event&, char32_t cp);
bool key(const Event&, SpecialKey sk);
bool ctrl(const Event&, char c);
bool alt(const Event&, char c);
bool shift(const Event&, SpecialKey sk);
bool any_key(const Event&);
const KeyEvent* as_key(const Event&);

// Mouse
struct MousePos { int col, row; };
bool mouse_clicked(const Event&, MouseButton = MouseButton::Left);
bool mouse_released(const Event&, MouseButton = MouseButton::Left);
bool mouse_moved(const Event&);
bool scrolled_up(const Event&);
bool scrolled_down(const Event&);
std::optional<MousePos> mouse_pos(const Event&);
const MouseEvent* as_mouse(const Event&);

// Other
bool resized(const Event&, int* w = nullptr, int* h = nullptr);
bool pasted(const Event&, std::string* out = nullptr);
bool focused(const Event&);
bool unfocused(const Event&);

// Fire-and-forget
template <typename F> bool on(const Event&, char c, F&& fn);
template <typename F> bool on(const Event&, char c1, char c2, F&& fn);
template <typename F> bool on(const Event&, SpecialKey sk, F&& fn);
```

---

## Signals

### Signal\<T\>

```cpp
template <typename T>
class Signal {
    Signal();                        // Default-construct T
    explicit Signal(U&& initial);    // Construct from value

    const T& get() const;           // Read (auto-tracks dependencies)
    const T& operator()() const;    // Shorthand for get()
    void set(const T& v);           // Write (notifies if changed)
    void set(T&& v);                // Write (move)
    void update(F&& fn);            // Mutate in-place (always notifies)
    auto map(F&& fn) const -> Computed<R>;  // Derive computed value
    uint64_t version() const;       // Change counter
};
```

### Computed\<T\>

```cpp
template <typename T>
class Computed {
    const T& get() const;           // Read (recomputes if dirty)
    const T& operator()() const;
};

template <std::invocable F>
auto computed(F&& fn) -> Computed<invoke_result_t<F>>;
```

### Effect

```cpp
class Effect {
    Effect();                        // Default (inactive)
    explicit Effect(F&& fn);        // Create and run immediately
    void dispose();                  // Unsubscribe from all deps
    bool active() const;
};

template <std::invocable F>
Effect effect(F&& fn);
```

### Batch

```cpp
class Batch {
    Batch();      // Begin batch
    ~Batch();     // End batch, flush notifications
};

template <std::invocable F>
decltype(auto) batch(F&& fn);  // Run fn inside a batch scope
```

---

## Canvas

### Canvas

```cpp
class Canvas {
    Canvas(int width, int height, StylePool* pool);

    void set(int x, int y, char32_t ch, uint16_t style_id, uint8_t width = 0);
    void write_text(int x, int y, std::string_view text, uint16_t style_id);
    void fill(Rect region, char32_t ch, uint16_t style_id);
    void clear();
    void resize(int w, int h);

    Cell get(int x, int y) const;
    int  width() const;
    int  height() const;

    void push_clip(Rect clip);
    void pop_clip();
};
```

### Cell

```cpp
struct Cell {
    char32_t character;
    uint16_t style_id;
    uint16_t hyperlink_id;
    uint8_t  width;

    uint64_t pack() const;
    static Cell unpack(uint64_t v);
};
```

### StylePool

```cpp
class StylePool {
    uint16_t intern(const Style& s);
    const Style& get(uint16_t id) const;
    void clear();
    std::size_t size() const;
};
```

### render_tree()

```cpp
void render_tree(const Element& root, Canvas& canvas,
                 StylePool& pool, const Theme& theme);
```

---

## Widgets

All widgets live in `maya::widget`. Full documentation in [13-widgets.md](13-widgets.md).

### Input

| Widget | Header | Description |
|--------|--------|-------------|
| `Input` | `widget/input.hpp` | Single-line text input with cursor and history |
| `TextArea` | `widget/textarea.hpp` | Multi-line text editor |
| `Checkbox` | `widget/checkbox.hpp` | Toggle checkbox |
| `ToggleSwitch` | `widget/checkbox.hpp` | iOS-style toggle switch |
| `Radio` | `widget/radio.hpp` | Radio button group |
| `Select` | `widget/select.hpp` | Dropdown select menu |
| `Slider` | `widget/slider.hpp` | Numeric slider |
| `Button` | `widget/button.hpp` | Clickable button with variants |
| `CommandPalette` | `widget/command_palette.hpp` | Fuzzy-search command launcher; clamped, rows shed detail when narrow |

### Data Display

| Widget | Header | Description |
|--------|--------|-------------|
| `Table` | `widget/table.hpp` | Data table: rich cells (spans + width-adaptive builders), selection, height-aware windowing + scrollbar, host-owned scroll, sort indicators, flexible + shedding columns, hit rects, `flow_rows()` per-row Elements for host-owned flows |
| `Tree` | `widget/tree.hpp` | Expandable tree view |
| `List` | `widget/list.hpp` | Selectable item list |
| `KeyHelp` | `widget/key_help.hpp` | Keyboard shortcut legend; 2-col ↔ 1-col by real measurement (pick) |
| `Badge` | `widget/badge.hpp` | Inline status badge |
| `Callout` | `widget/callout.hpp` | Info/success/warning/error callout box |
| `Link` | `widget/link.hpp` | Hyperlink element |
| `ShortcutRow` | `widget/shortcut_row.hpp` | Width-adaptive keyboard hint row (Helix/k9s style) |
| `ModelBadge` | `widget/model_badge.hpp` | Color-coded active-model indicator |

### Navigation

| Widget | Header | Description |
|--------|--------|-------------|
| `Tabs` | `widget/tabs.hpp` | Tab bar navigation; falls back to ‹ active i/n › when narrow (pick) |
| `Breadcrumb` | `widget/breadcrumb.hpp` | Breadcrumb path navigation; collapses to first › … › last (pick) |
| `Menu` | `widget/menu.hpp` | Menu with selectable items; shortcuts shed when narrow |
| `ActivityBar` | `widget/activity_bar.hpp` | Vertical icon sidebar |
| `Scrollable` | `widget/scrollable.hpp` | Scrollable content region |
| `Scrollbar` | `widget/scrollbar.hpp` | Visual indicator for a `ScrollState` |
| `Picker` | `widget/picker.hpp` | Bordered modal picker with scrollable results |
| `CommandPalette` | `widget/command_palette.hpp` | Fuzzy-search command launcher; clamped, rows shed detail when narrow |

### Display

| Widget | Header | Description |
|--------|--------|-------------|
| `StreamingMarkdown` | `widget/markdown.hpp` | Streaming markdown renderer |
| `Spinner` | `widget/spinner.hpp` | Animated loading spinner |
| `ProgressBar` | `widget/progress.hpp` | Progress bar with percentage |
| `Gauge` | `widget/gauge.hpp` | Gauge / meter display |
| `Divider` | `widget/divider.hpp` | Horizontal or vertical divider |
| `Image` | `widget/image.hpp` | Terminal image display |
| `gradient()` | `widget/gradient.hpp` | Color gradient text builder |
| `Disclosure` | `widget/disclosure.hpp` | Collapsible disclosure section |
| `Html` | `widget/agentty/html.hpp` | Render a safe subset of HTML as styled Elements |
| `Overlay` | `widget/overlay.hpp` | Base layer + one anchored floating element |

### Overlay

| Widget | Header | Description |
|--------|--------|-------------|
| `Modal` | `widget/modal.hpp` | Modal dialog with buttons; clamped to dialog width |
| `Popup` | `widget/popup.hpp` | Floating popup |
| `ToastManager` | `widget/toast.hpp` | Toast notification manager; clamped right-anchored cards |

### Visualization

| Widget | Header | Description |
|--------|--------|-------------|
| `BarChart` | `widget/bar_chart.hpp` | Horizontal/vertical bar chart |
| `LineChart` | `widget/line_chart.hpp` | Line chart with series |
| `Sparkline` | `widget/sparkline.hpp` | Inline sparkline graph |
| `Heatmap` | `widget/heatmap.hpp` | Grid heatmap |
| `Calendar` | `widget/calendar.hpp` | Calendar date display |
| `PixelCanvas` | `widget/canvas.hpp` | Pixel-level drawing canvas |
| `FlameChart` | `widget/flame_chart.hpp` | Flame graph for nested execution spans |
| `Waterfall` | `widget/waterfall.hpp` | Timing waterfall chart (devtools style) |
| `Timeline` | `widget/timeline.hpp` | Vertical CI/pipeline event timeline |
| `GitGraph` | `widget/git_graph.hpp` | Commit graph with colored branch lines |

### Agent UI

Composable pieces of a Claude-Code / Zed-style agent conversation. `Thread`
is the top-level viewport; the rest are the parts it (or a host) assembles.

| Widget | Header | Description |
|--------|--------|-------------|
| `Thread` | `widget/thread.hpp` | Top-level viewport: empty → `WelcomeScreen`, else `Conversation` |
| `WelcomeScreen` | `widget/welcome_screen.hpp` | Empty-thread brand splash + starters/hints |
| `Conversation` | `widget/conversation.hpp` | Vertical list of turns with dividers + in-flight indicator |
| `Turn` | `widget/turn.hpp` | One speaker turn (rail, role, checkpoint) |
| `TurnDivider` | `widget/turn_divider.hpp` | Styled rule between conversation turns |
| `CheckpointDivider` | `widget/checkpoint_divider.hpp` | Full-width "↺ Restore checkpoint" rule above a turn |
| `Composer` | `widget/composer.hpp` | State-driven bordered input box (idle/streaming/permission) |
| `AgentTimeline` | `widget/agent_timeline.hpp` | Bordered Actions panel logging tool events for a turn |
| `ToolBodyPreview` | `widget/tool_body_preview.hpp` | Per-`ToolKind` body detail under a timeline event |
| `AppLayout` | `widget/app_layout.hpp` | Top-level chat-app frame (header/body/composer/overlay) |
| `UserMessage` | `widget/message.hpp` | Chat user message bubble |
| `AssistantMessage` | `widget/message.hpp` | Chat assistant message bubble |
| `ThinkingBlock` | `widget/thinking.hpp` | Collapsible AI thinking block |
| `StreamingCursor` | `widget/streaming_cursor.hpp` | Pulsing "assistant is streaming" indicator |
| `SystemBanner` | `widget/system_banner.hpp` | Severity-coloured system alert (ctx warning, rate limit) |
| `TodoList` | `widget/todo_list.hpp` | Session todo list card |
| `PlanView` | `widget/plan_view.hpp` | Task plan with status tracking |
| `Permission` | `widget/permission.hpp` | Permission approval prompt |

### Session / Diagnostics

| Widget | Header | Description |
|--------|--------|-------------|
| `DiffView` | `widget/diff_view.hpp` | Side-by-side or unified diff |
| `InlineDiff` | `widget/inline_diff.hpp` | Two-line word-level LCS diff |
| `FileChanges` | `widget/file_changes.hpp` | Session file-change summary (created/modified/deleted + counts) |
| `ChangesStrip` | `widget/changes_strip.hpp` | Bordered "session has pending changes" banner |
| `FileRef` | `widget/file_ref.hpp` | File reference with icon; dir collapses to …/ when narrow |
| `LogViewer` | `widget/log_viewer.hpp` | Filterable log viewer |
| `SearchResult` | `widget/search_result.hpp` | Grouped search results; paths keep their filename when narrow |
| `ErrorBlock` | `widget/error_block.hpp` | Structured error/exception card |
| `GitStatus` | `widget/git_status.hpp` | Branch + working-tree status |
| `ContextWindow` | `widget/context_window.hpp` | Segmented context-window usage meter |
| `TokenStream` | `widget/token_stream.hpp` | Live token-rate sparkline + stats (compact/full) |
| `CostTracker` | `widget/cost_tracker.hpp` | Per-turn + cumulative token/cost breakdown |
| `ApiUsage` | `widget/api_usage.hpp` | API rate-limit / request-count / latency display |
| `ActivityIndicator` | `widget/activity_indicator.hpp` | Single-row hex-dump activity tape — decorative noise while waiting, a real hexdump of `Config::stream` (the host's live byte tail + true total) once bytes flow |

### Status Bar

| Widget | Header | Description |
|--------|--------|-------------|
| `StatusBar` | `widget/status_bar.hpp` | Single-line streaming status bar (composes the pieces below) |
| `TokenStreamSparkline` | `widget/token_stream_sparkline.hpp` | Live tok/s rate + sparkline chip (adaptive width) |
| `PhaseChip` | `widget/phase_chip.hpp` | Current-phase verb + elapsed, breathing |
| `TitleChip` | `widget/title_chip.hpp` | Breadcrumb / title chip |
| `ContextGauge` | `widget/context_gauge.hpp` | Context-window usage gauge |
| `PhaseAccent` | `widget/phase_accent.hpp` | Top/bottom accent rail strip |
| `StatusBanner` | `widget/status_banner.hpp` | Full-width toast that takes over the activity row |

### Tool Widgets

| Widget | Header | Description |
|--------|--------|-------------|
| `ToolCall` | `widget/tool_call.hpp` | Generic tool invocation display |
| `BashTool` | `widget/bash_tool.hpp` | Shell command tool call |
| `ReadTool` | `widget/read_tool.hpp` | File read tool call |
| `EditTool` | `widget/edit_tool.hpp` | File edit tool call |
| `WriteTool` | `widget/write_tool.hpp` | File write tool call |
| `FetchTool` | `widget/fetch_tool.hpp` | HTTP fetch tool call |
| `AgentTool` | `widget/agent_tool.hpp` | Sub-agent tool call |
| `GitCommitTool` | `widget/git_commit_tool.hpp` | Git commit operation card |

---

## Core Types

### Strong\<Tag, T\>

```cpp
template <typename Tag, typename T>
struct Strong {
    T value{};
    // Arithmetic: Strong + Strong, Strong - Strong, etc.
    // Comparison: ==, !=, <, >, <=, >=
};
```

### Size / Position / Rect

```cpp
using Columns = Strong<ColumnTag, int>;
using Rows    = Strong<RowTag, int>;

struct Size     { Columns width; Rows height; };
struct Position { Columns x; Rows y; };
struct Rect     { Position pos; Size size; };
```

### Result / Status / Error

```cpp
enum class ErrorKind {
    TerminalInit, Io, LayoutOverflow, InvalidStyle,
    InvalidUtf8, Unsupported, Signal, WouldBlock
};

struct Error {
    ErrorKind kind;
    std::string message;
    std::source_location location;

    static Error terminal(std::string msg);
    static Error io(std::string msg);
    static Error from_errno(std::string ctx);
};

template <typename T>
using Result = std::expected<T, Error>;
using Status = Result<void>;
```
