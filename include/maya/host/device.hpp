#pragma once
// maya/host/device.hpp — what terminal_host needs from a screen.
//
// maya::Screen is the production device. This concept is the shape of it
// that the host actually uses, so the host can be written against ANY device
// with that shape — the real one, or a fake in a test.
//
// That matters because the host holds the only logic in maya that nobody
// could unit-test: frame debt, the animation deadline, the ack window, the
// input held back across a navigation key. Those were reachable only by
// driving a real pty and squinting at bytes. With the host templated, a test
// hands it a scripted device and asserts on the calls it makes.
//
// The concept is deliberately the WHOLE surface the host touches (26 calls,
// checked below). If a host change needs a 27th, it gets added here and
// every fake stops compiling until it is implemented — which is the point:
// the fake can't silently drift from the device it stands in for.

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../device/options.hpp"
#include "../element/element.hpp"
#include "../render/scrollback_ledger.hpp"
#include "../screen.hpp"
#include "../terminal/input.hpp"

namespace maya {

/// The device surface terminal_host drives. maya::Screen models it; so does
/// any test double that wants to stand in for a terminal.
template <class D>
concept Device = requires(D d,
                          const D cd,
                          const Element& root,
                          std::string_view sv,
                          ScrollbackDebt debt,
                          bool on,
                          Element (*build)()) {
    // ── what it is ────────────────────────────────────────────────────────
    { cd.size() }          -> std::same_as<Size>;
    { cd.input_handle() }  -> std::same_as<platform::NativeHandle>;
    { cd.output_handle() } -> std::same_as<platform::NativeHandle>;

    // ── input: bytes in, events out ───────────────────────────────────────
    { d.read() }                   -> std::same_as<Result<std::vector<Event>>>;
    { d.has_pending_input() }      -> std::same_as<bool>;
    { d.resolve_pending_input() }  -> std::same_as<std::vector<Event>>;
    { d.on_resize() };
    // How long a lone ESC waits before it resolves to the Escape key. The
    // host reads it off the DEVICE, not off Screen: a fake that stands in
    // for a terminal sets its own, so escape-resolution is testable at a
    // timeout a test can actually wait out.
    { D::kEscapeTimeout } -> std::convertible_to<std::chrono::milliseconds>;

    // ── drawing: one frame, and what the scheduler must do next ───────────
    // present() takes a BUILDER, not a finished tree: widgets register their
    // animation requests while being built, so the device clears the request
    // slots, runs build(), then reads them. present_if() adds a visual-hash
    // skip. The raw-Element overload exists too but the host doesn't use it.
    { d.present(root) }    -> std::same_as<Presented>;
    { d.present(build) }   -> std::same_as<Presented>;
    { d.present_if(std::uint64_t{}, build, on) } -> std::same_as<Presented>;
    { d.warm(root) };

    // ── flow control: the tty can refuse bytes ────────────────────────────
    { cd.backpressured() }   -> std::same_as<bool>;
    { cd.pending_output() }  -> std::same_as<bool>;
    // flush() reports whether the residue is GONE; while false the host
    // watches output_handle() for writability and calls again.
    { d.flush() }            -> std::same_as<bool>;
    // ready() is non-const: it may time out a missing ack as it answers.
    { d.ready() }            -> std::same_as<bool>;
    { cd.ready_deadline() }  -> std::same_as<std::optional<Presented::clock::time_point>>;

    // ── effects only a terminal can do ────────────────────────────────────
    { d.set_title(sv) };
    { d.write_clipboard(sv) };
    { d.query_clipboard(ClipboardDialect::automatic) };
    { d.emit_host_sequence(sv) };
    { d.commit_scrollback(debt) };
    { d.commit_overflow() };
    { d.force_redraw() };
    { d.reset_inline() };
    { d.set_mouse(on) };
};

// The production device satisfies it. If this fires, Screen and the host
// have drifted apart — fix the concept, then every fake follows.
static_assert(Device<Screen>,
              "maya::Screen must model Device: terminal_host is written against it");

}  // namespace maya
