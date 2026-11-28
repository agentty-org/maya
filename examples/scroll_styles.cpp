// scroll_styles.cpp — showcase every built-in scrollbar style preset.
//
// Compact tile layout: each preset gets a small labeled card showing its
// vertical bar + horizontal bar at the current scroll position. All
// tiles share one ScrollState so they move together. Press ↑/↓/←/→ to
// scroll; every tile updates in lock-step.
//
// To add a custom style, define a static factory on ScrollbarStyle in
// widget/agentty/scrollbar.hpp (5 lines) and append it to kPresets below.
//
// To build a fully custom scrollbar widget (gradient thumb, percentage
// label, click-to-jump, etc.), skip ScrollbarStyle entirely:
//
//     Element my_bar(const ScrollState& s, int viewport_h) {
//         int thumb_h = std::max(1, viewport_h * viewport_h /
//                                    (viewport_h + s.max_y));
//         int thumb_y = (viewport_h - thumb_h) * s.y /
//                       std::max(1, s.max_y);
//         // emit any Element from these.
//     }

#include <maya/host/run.hpp>
#include <maya/maya.hpp>
#include <maya/widget/agentty/scrollbar.hpp>

#include <array>
#include <string>
#include <variant>
#include <optional>

using namespace maya;
using namespace maya::dsl;

struct Preset {
    const char*     name;
    ScrollbarStyle  style;
};

static const std::array<Preset, 13> kPresets = {{
    {"line",        ScrollbarStyle::line()},
    {"block",       ScrollbarStyle::block()},
    {"slim",        ScrollbarStyle::slim()},
    {"heavy",       ScrollbarStyle::heavy()},
    {"double_line", ScrollbarStyle::double_line()},
    {"dotted",      ScrollbarStyle::dotted()},
    {"dashed",      ScrollbarStyle::dashed()},
    {"braille",     ScrollbarStyle::braille()},
    {"ascii",       ScrollbarStyle::ascii()},
    {"shadow",      ScrollbarStyle::shadow()},
    {"minimal",     ScrollbarStyle::minimal()},
    {"neon",        ScrollbarStyle::neon()},
    {"retro",       ScrollbarStyle::retro()},
    // danger() and pixel() are also available — drop them in here.
}};

constexpr int kTileH = 6;   // small viewports so each tile stays compact
constexpr int kTileW = 12;

// Prime the maxes so the tiles show meaningful thumb positions from the
// very first frame: nothing here is scrolled content, only bars.
ScrollState make_state() {
    ScrollState s;
    s.max_x = 24;
    s.max_y = 18;
    s.step_x = 2;
    s.step_y = 1;
    return s;
}

struct Model { mutable ScrollState state = make_state(); };

struct Scroll { KeyEvent key; };
struct Quit {};
using Msg = std::variant<Scroll, Quit>;

struct ScrollStyles {
    using Model = ::Model;
    using Msg   = ::Msg;
    using Cmd   = jaal::Cmd<Msg>;

    static Cmd update(Model& m, Scroll s) { (void)m.state.handle(s.key, kTileH, kTileW); return {}; }
    static Cmd update(Model&, Quit)       { return Cmd::quit(0); }

    static Element view(const Model& m) {
        // Each tile is a labeled card showing both axes' bars.
        //   ┌─name──────────┐
        //   │ │ │ │         │  <- vertical bar (kTileH rows tall)
        //   │ ┃ │ │         │
        //   │ ┃ │ │         │
        //   │ │ │ │         │
        //   │ │ │ │         │
        //   │ │ │ │         │
        //   │ ──━━━━──      │  <- horizontal bar (kTileW cols wide)
        //   └───────────────┘
        auto tile = [&m](const Preset& p) {
            return v(
                text(p.name) | Bold | Fg<140, 180, 255>,
                scrollbar_y(m.state, kTileH, p.style),
                scrollbar_x(m.state, kTileW, p.style)
            ) | pad<0, 1> | border_<Single> | bcol<70, 75, 90>;
        };

        // Pack into rows of 4 tiles so the showcase fits in ~70 cols.
        constexpr int kPerRow = 4;
        std::vector<Element> rows;
        for (std::size_t i = 0; i < kPresets.size(); i += kPerRow) {
            std::vector<Element> row_cells;
            for (std::size_t j = i; j < i + kPerRow && j < kPresets.size(); ++j)
                row_cells.push_back(tile(kPresets[j]));
            rows.push_back(h(std::move(row_cells)).build());
        }

        const std::string status =
            "y=" + std::to_string(m.state.y) + "/" + std::to_string(m.state.max_y) +
            "  x=" + std::to_string(m.state.x) + "/" + std::to_string(m.state.max_x);

        return v(
            t<"Scrollbar style showcase"> | Bold | Fg<100, 180, 255>,
            t<"13 presets. All bars share one ScrollState."> | Dim,
            blank_,
            v(std::move(rows)),
            blank_,
            text(status) | Fg<255, 180, 100>,
            t<"↑/↓/←/→ j/k scroll · PgUp/PgDn · Home/End · q quit"> | Dim
        ) | pad<1> | border_<Round> | bcol<50, 55, 70>;
    }

    static std::optional<Msg> on(const Model&, const KeyEvent& k) {
        if (key_is(k, 'q')) return Quit{};
        if (key_is(k, 'j')) return Scroll{KeyEvent{.key = SpecialKey::Down}};
        if (key_is(k, 'k')) return Scroll{KeyEvent{.key = SpecialKey::Up}};
        return Scroll{k};
    }
};

static_assert(Program<ScrollStyles>);

int main() { return run<ScrollStyles>({.title = "scroll_styles", .mouse = true}); }
