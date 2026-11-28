// scroll_2d.cpp — two-axis scrolling via the framework primitive.
//
// Demonstrates simultaneous horizontal + vertical scroll over a wide-and-
// tall content surface. Same primitive as scroll_clip.cpp — just one
// ScrollState and one pipe call. The renderer applies scroll_x and
// scroll_y independently; max_x and max_y are written back after layout
// so clamping is automatic on both axes.
//
// Keys:
//   ↑/↓/←/→   move one row/col
//   PgUp/PgDn move one viewport-height vertically
//   Home/End  jump to top / bottom of column
//   Ctrl+Home jump to (0, 0)
//   Ctrl+End  jump to (max_x, max_y)
//   Mouse wheel: vertical · Shift+wheel: horizontal
//   q quit

#include <maya/host/run.hpp>
#include <maya/maya.hpp>
#include <maya/widget/agentty/scrollbar.hpp>

#include <string>
#include <variant>
#include <optional>

using namespace maya;
using namespace maya::dsl;

// Build a wide × tall grid as a vbox of hboxes. Each cell is its own
// text element with a known natural width — important because a single
// wide text() with default TextWrap::Wrap would wrap to the viewport
// width and there'd be nothing to scroll horizontally past. With a
// grid of cells, every cell keeps its size and the scroll mechanism
// shows / hides cells via the renderer's paint-time origin shift.
static std::vector<Element> build_grid(int rows, int cols) {
    std::vector<Element> out;
    out.reserve(static_cast<std::size_t>(rows));
    for (int r = 0; r < rows; ++r) {
        std::vector<Element> cells;
        cells.reserve(static_cast<std::size_t>(cols));
        for (int c = 0; c < cols; ++c) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "[%02d,%02d] ", r, c);
            auto cell = text(std::string{buf});
            if ((r + c) % 2 == 0) cell = cell | Dim;
            cells.push_back(cell);
        }
        out.push_back(h(std::move(cells)).build());
    }
    return out;
}

constexpr int kViewportW = 40;
constexpr int kViewportH = 10;
constexpr int kGridRows  = 40;
constexpr int kGridCols  = 20;

ScrollState make_state() {
    ScrollState s;
    s.step_x = 4;   // horizontal feels nicer in larger steps
    s.step_y = 1;
    return s;
}

struct Model {
    // mutable: the renderer writes the extents back after layout. The
    // Screen hands it wheel and drag events; keys come through update().
    mutable ScrollState state = make_state();
    // The most recent mouse event, so you can see exactly what your
    // terminal sends. If Shift+wheel says "shift=0", the terminal strips
    // the modifier; use a trackpad pan, ←/→, or Kitty/WezTerm/iTerm2.
    std::string mouse_dbg = "(no mouse event yet)";
};

struct Scroll { KeyEvent key; };
struct Mouse  { MouseEvent ev; };
struct Quit {};
using Msg = std::variant<Scroll, Mouse, Quit>;

struct Scroll2D {
    using Model = ::Model;
    using Msg   = ::Msg;
    using Cmd   = jaal::Cmd<Msg>;

    static Cmd update(Model& m, Scroll s) { (void)m.state.handle(s.key, kViewportH, kViewportW); return {}; }
    static Cmd update(Model&, Quit)       { return Cmd::quit(0); }
    static Cmd update(Model& m, Mouse e) {
        const MouseEvent& me = e.ev;
        const char* btn = "?";
        switch (me.button) {
            case MouseButton::Left:        btn = "Left"; break;
            case MouseButton::Right:       btn = "Right"; break;
            case MouseButton::Middle:      btn = "Middle"; break;
            case MouseButton::ScrollUp:    btn = "ScrollUp"; break;
            case MouseButton::ScrollDown:  btn = "ScrollDown"; break;
            case MouseButton::ScrollLeft:  btn = "ScrollLeft"; break;
            case MouseButton::ScrollRight: btn = "ScrollRight"; break;
            case MouseButton::None:        btn = "None"; break;
        }
        const char* kind = me.kind == MouseEventKind::Press ? "Press"
                         : me.kind == MouseEventKind::Release ? "Release" : "Move";
        const int mx = me.x.value - 1, my = me.y.value - 1;
        const bool over_hbar = m.state.h_bar_at(mx, my) != nullptr;
        m.mouse_dbg = std::string{btn} + " " + kind +
            "  at (" + std::to_string(mx) + "," + std::to_string(my) + ")" +
            "  shift=" + std::to_string(int(me.mods.shift)) +
            " alt="    + std::to_string(int(me.mods.alt)) +
            " ctrl="   + std::to_string(int(me.mods.ctrl)) +
            (over_hbar ? "  [OVER H-BAR]" : "");
        return {};
    }

    static Element view(const Model& m) {
        static const auto grid = build_grid(kGridRows, kGridCols);   // constant content
        const std::string status =
            "x=" + std::to_string(m.state.x) + "/" + std::to_string(m.state.max_x) +
            "  y=" + std::to_string(m.state.y) + "/" + std::to_string(m.state.max_y);
        return v(
            t<"2D scroll — both axes via the framework primitive"> | Bold | Fg<100, 180, 255>,
            t<"Grid is 40 rows × ~140 cols; viewport is 40×10."> | Dim,
            blank_,
            h(
                v(grid) | scroll(m.state, kViewportW, kViewportH),
                scrollbar_y(m.state, kViewportH, ScrollbarStyle::block())
            ),
            scrollbar_x(m.state, kViewportW, ScrollbarStyle::block()),
            blank_,
            text(status) | Fg<255, 180, 100>,
            text("last mouse: " + m.mouse_dbg) | Fg<150, 220, 150>,
            t<"arrows · j/k · PgUp/PgDn · Home/End · wheel over h-bar pans horizontally · q quit"> | Dim
        ) | pad<1> | border_<Round> | bcol<50, 55, 70>;
    }

    static std::optional<Msg> on(const Model&, const KeyEvent& k) {
        if (key_is(k, 'q')) return Quit{};
        if (key_is(k, 'j')) return Scroll{KeyEvent{.key = SpecialKey::Down}};
        if (key_is(k, 'k')) return Scroll{KeyEvent{.key = SpecialKey::Up}};
        return Scroll{k};
    }
    static std::optional<Msg> on(const Model&, const MouseEvent& e) { return Mouse{e}; }
};

static_assert(Program<Scroll2D>);

int main() { return run<Scroll2D>({.title = "scroll_2d", .mouse = true}); }
