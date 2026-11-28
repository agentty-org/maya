// scroll_clip.cpp — first-class framework scrolling, one-axis (vertical).
//
// Uses the maya scroll primitive:
//   - ScrollState — plain data, holds {x, y, max_x, max_y, step_x, step_y}
//                   and key/mouse handlers.
//   - dsl::scroll(state, viewport) — DSL pipe that wraps any element into
//                                    an overflow:Hidden viewport.
//   - The renderer translates descendants by -scroll_y at paint time and
//     writes max_y back after layout so clamping is automatic.
//
// Compare with scroll_slice.cpp (which emits only visible rows manually).
// Use the slice pattern for large indexable data sets (logs, lists with
// millions of rows). Use the clip pattern — this file — for any other
// content: heterogeneous, markdown, computed, dynamic.
//
// Keys: ↑/↓ j/k row · PgUp/PgDn page · Home/End jump · q quit.

#include <maya/host/run.hpp>
#include <maya/maya.hpp>
#include <maya/widget/agentty/scrollbar.hpp>

#include <array>
#include <optional>
#include <string>
#include <variant>

using namespace maya;
using namespace maya::dsl;

namespace {

static constexpr std::array<const char*, 30> kLines = {
    "01  the quick brown fox jumps over the lazy dog",
    "02  pack my box with five dozen liquor jugs",
    "03  sphinx of black quartz, judge my vow",
    "04  how vexingly quick daft zebras jump",
    "05  bright vixens jump; dozy fowl quack",
    "06  jackdaws love my big sphinx of quartz",
    "07  the five boxing wizards jump quickly",
    "08  amazingly few discotheques provide jukeboxes",
    "09  heavy boxes perform quick waltzes and jigs",
    "10  jaded zombies acted quaintly but kept driving",
    "11  a quick movement of the enemy will jeopardize six gunboats",
    "12  all questions asked by five watched experts amaze the judge",
    "13  back in june we delivered oxygen equipment of the same size",
    "14  crazy frederick bought many very exquisite opal jewels",
    "15  fix problem quickly with galvanized jets",
    "16  glib jocks quiz nymph to vex dwarf",
    "17  jinxed wizards pluck ivy from the big quilt",
    "18  my girl wove six dozen plaid jackets before she quit",
    "19  six big juicy steaks sizzled in a pan as five workmen left",
    "20  the wizard quickly jinxed the gnomes before they vaporized",
    "21  we promptly judged antique ivory buckles for the next prize",
    "22  waltz, bad nymph, for quick jigs vex",
    "23  watch jeopardy! alex trebek's fun tv quiz game",
    "24  when zombies arrive, quickly fax judge pat",
    "25  woven silk pyjamas exchanged for blue quartz",
    "26  big july earthquakes confound zany experimental vow",
    "27  five quacking zephyrs jolt my wax bed",
    "28  fix problem quickly with galvanized jets",
    "29  jumpy halfling dwarves pick quartz box",
    "30  ── end ──",
};

// The rows, built once: the content is constant, only the scroll moves.
Element content() {
    std::vector<Element> out;
    out.reserve(kLines.size());
    for (std::size_t i = 0; i < kLines.size(); ++i) {
        auto row = text(kLines[i]);
        if (i % 2 == 0) row = row | Dim;
        out.push_back(row);
    }
    return v(std::move(out));
}

constexpr int kViewportH = 8;

struct Model {
    // mutable: the renderer writes max_y back into it after layout, which is
    // how scroll() clamps with no code here. The wheel is dispatched to it
    // by the Screen; keys come through update().
    mutable ScrollState state;
};

struct Scroll { KeyEvent key; };
struct Quit {};
using Msg = std::variant<Scroll, Quit>;

struct ScrollClip {
    using Model = ::Model;
    using Msg   = ::Msg;
    using Cmd   = jaal::Cmd<Msg>;

    static Cmd update(Model& m, Scroll s) { (void)m.state.handle(s.key, kViewportH); return {}; }
    static Cmd update(Model&, Quit)       { return Cmd::quit(0); }

    static Element view(const Model& m) {
        const std::string status = "y=" + std::to_string(m.state.y) + "/" + std::to_string(m.state.max_y);
        return v(
            t<"Scrollable viewport — framework primitive"> | Bold | Fg<100, 180, 255>,
            t<"overflow:Hidden + scroll_y; renderer translates at paint time."> | Dim,
            blank_,
            h(
                content() | scroll(m.state, kViewportH) | grow_<1>,
                scrollbar_y(m.state, kViewportH)
            ),
            blank_,
            text(status) | Fg<255, 180, 100>,
            t<"↑/↓ j/k row · PgUp/PgDn page · Home/End · q quit"> | Dim
        ) | pad<1> | border_<Round> | bcol<50, 55, 70>;
    }

    static std::optional<Msg> on(const Model&, const KeyEvent& k) {
        if (key_is(k, 'q')) return Quit{};
        if (key_is(k, 'j')) return Scroll{KeyEvent{.key = SpecialKey::Down}};
        if (key_is(k, 'k')) return Scroll{KeyEvent{.key = SpecialKey::Up}};
        return Scroll{k};
    }
};

static_assert(Program<ScrollClip>);

}  // namespace

int main() { return run<ScrollClip>({.title = "scroll_clip", .mouse = true}); }
