#pragma once
// maya::widget::spinner — Animated spinner indicators
//
// Frame-based spinners with multiple built-in styles, driven by the shared
// animation clock (core/motion.hpp) — no dt plumbing, no threads, no state.
// The current frame is a pure function of anim_now_ms(), so every spinner
// in the process steps in lockstep and a test clock makes it deterministic.
//
// Usage:
//   Spinner spin;                       // default dots style
//   auto ui = h(spin, text(" Loading..."));   // build() self-paces
//
//   Spinner<SpinnerStyle::Braille> s2;  // braille style
//
// build() requests the next frame (one wake per step — build it only while
// it should be visible). current_frame()/frame_index() are PURE reads for
// hosts that own their render cadence (status-bar hash gates, Tick loops).

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "../../element/builder.hpp"
#include "../../core/motion.hpp"   // anim::frame_index — shared-clock stepping
#include "../../style/style.hpp"

namespace maya {

// ============================================================================
// SpinnerStyle — built-in animation patterns
// ============================================================================

enum class SpinnerStyle : uint8_t {
    Dots,       // ⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏
    Line,       // -\|/
    Arc,        // ◜◠◝◞◡◟
    Arrow,      // ←↖↑↗→↘↓↙
    Bounce,     // ⠁⠂⠄⠂
    Bar,        // ▉▊▋▌▍▎▏▎▍▌▋▊▉
    Clock,      // 🕐🕑🕒🕓🕔🕕🕖🕗🕘🕙🕚🕛
    Star,       // ✶✸✹✺✹✷
    Pulse,      // ░▒▓█▓▒
};

namespace detail {

struct SpinnerFrames {
    const char* const* frames;
    int count;
    float interval;  // seconds per frame
};

// Dots
inline constexpr const char* dots_frames[] = {
    "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"
};

// Line
inline constexpr const char* line_frames[] = {
    "-", "\\", "|", "/"
};

// Arc
inline constexpr const char* arc_frames[] = {
    "◜", "◠", "◝", "◞", "◡", "◟"
};

// Arrow
inline constexpr const char* arrow_frames[] = {
    "←", "↖", "↑", "↗", "→", "↘", "↓", "↙"
};

// Bounce
inline constexpr const char* bounce_frames[] = {
    "⠁", "⠂", "⠄", "⠂"
};

// Bar
inline constexpr const char* bar_frames[] = {
    "▉", "▊", "▋", "▌", "▍", "▎", "▏", "▎", "▍", "▌", "▋", "▊", "▉"
};

// Clock
inline constexpr const char* clock_frames[] = {
    "🕐", "🕑", "🕒", "🕓", "🕔", "🕕", "🕖", "🕗", "🕘", "🕙", "🕚", "🕛"
};

// Star
inline constexpr const char* star_frames[] = {
    "✶", "✸", "✹", "✺", "✹", "✷"
};

// Pulse
inline constexpr const char* pulse_frames[] = {
    "░", "▒", "▓", "█", "▓", "▒"
};

template <SpinnerStyle S>
consteval SpinnerFrames get_spinner_frames() {
    if constexpr (S == SpinnerStyle::Dots)    return {dots_frames,    10, 0.08f};
    if constexpr (S == SpinnerStyle::Line)    return {line_frames,     4, 0.10f};
    if constexpr (S == SpinnerStyle::Arc)     return {arc_frames,      6, 0.10f};
    if constexpr (S == SpinnerStyle::Arrow)   return {arrow_frames,    8, 0.10f};
    if constexpr (S == SpinnerStyle::Bounce)  return {bounce_frames,   4, 0.12f};
    if constexpr (S == SpinnerStyle::Bar)     return {bar_frames,     13, 0.08f};
    if constexpr (S == SpinnerStyle::Clock)   return {clock_frames,   12, 0.10f};
    if constexpr (S == SpinnerStyle::Star)    return {star_frames,     6, 0.08f};
    if constexpr (S == SpinnerStyle::Pulse)   return {pulse_frames,    6, 0.12f};
}

} // namespace detail

// ============================================================================
// Spinner — animated spinner widget
// ============================================================================

template <SpinnerStyle S = SpinnerStyle::Dots>
class Spinner {
    static constexpr auto frames_ = detail::get_spinner_frames<S>();
    Style style_{};

    static constexpr double interval_ms_() noexcept {
        return static_cast<double>(frames_.interval) * 1000.0;
    }
    [[nodiscard]] static std::size_t frame_at_(bool request) noexcept {
        return anim::frame_index(static_cast<std::size_t>(frames_.count),
                                 interval_ms_(), request);
    }

public:
    Spinner() = default;

    explicit Spinner(Style s) : style_(s) {}

    void set_style(Style s) { style_ = s; }

    operator Element() const { return build(); }

    // Self-pacing render: derives the frame from the shared animation clock
    // AND schedules one wake at the next step boundary. Build it only while
    // the spinner should be visible — a built spinner keeps the loop alive.
    [[nodiscard]] Element build() const {
        return Element{TextElement{
            .content = std::string{frames_.frames[frame_at_(true)]},
            .style = style_,
        }};
    }

    // Current frame glyph — PURE read (no frame request); for callers that
    // wrap the spinner into a mixed-style text run (status-bar pills) and
    // own their render cadence.
    [[nodiscard]] std::string_view current_frame() const noexcept {
        return frames_.frames[frame_at_(false)];
    }

    // Current frame index — PURE read; for hosts that hash the frame into a
    // repaint gate or hand tick state to another widget so multiple animated
    // surfaces step in lockstep (they all read the same clock anyway).
    [[nodiscard]] int frame_index() const noexcept {
        return static_cast<int>(frame_at_(false));
    }
};

} // namespace maya
