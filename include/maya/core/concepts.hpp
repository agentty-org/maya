#pragma once
// maya::core::concepts - Concept definitions for the maya TUI library
//
// These concepts define the interfaces that maya's generic components
// program against. They serve the same role as Rust traits: they specify
// what a type must be able to do, enabling static dispatch with clear
// compile-time error messages when requirements are not met.

#include <concepts>
#include <optional>
#include <ranges>
#include <string_view>
#include <type_traits>

#include "types.hpp"
#include "../style/color.hpp"

namespace maya {

// ============================================================================
// Forward declarations
// ============================================================================
// These types are defined elsewhere but referenced in concept constraints.
// Forward declarations are sufficient because concepts only check signatures.

class Canvas;
struct KeyEvent;

// ============================================================================
// Measurable - Can report intrinsic size given a width constraint
// ============================================================================
// Layout engines call measure() during the constraint-propagation pass.
// The max_width parameter is the available horizontal space (in columns).
// Returns the desired Size. The widget may report a smaller width, but
// must not exceed max_width.

template <typename T>
concept Measurable = requires(const T& t, int max_width) {
    { t.measure(max_width) } -> std::convertible_to<Size>;
};

// ============================================================================
// Renderable - Can paint itself onto a Canvas within a clip rectangle
// ============================================================================
// Called during the render pass. The clip rect defines the visible region;
// the implementation must not draw outside it.

template <typename T>
concept Renderable = requires(const T& t, Canvas& canvas, Rect clip) {
    { t.render(canvas, clip) } -> std::same_as<void>;
};

// ============================================================================
// Component - Produces an Element tree (analogous to a React component)
// ============================================================================
// The render() method must return something. The actual Element type constraint
// is applied at the call site in app.hpp where Element is fully defined.

template <typename T>
concept Component = requires(T& t) {
    { t.render() };
};

// ============================================================================
// InputHandler - Can handle keyboard events
// ============================================================================
// Returns true if the event was consumed (stopping propagation), false
// if the event should bubble to the parent.

template <typename T>
concept InputHandler = requires(T& t, const KeyEvent& key) {
    { t.on_key(key) } -> std::convertible_to<bool>;
};

// ============================================================================
// Styleable - Exposes foreground, background, and bold properties
// ============================================================================
// A minimal style interface for widgets that support visual customization.

template <typename T>
concept Styleable = requires(const T& t) {
    { t.fg() }   -> std::convertible_to<std::optional<Color>>;
    { t.bg() }   -> std::convertible_to<std::optional<Color>>;
    { t.bold() } -> std::same_as<bool>;
};

// ============================================================================
// ElementLike - Defined in element/element.hpp where Element is complete
// ============================================================================

// ============================================================================
// RangeLike<T> - A range whose value type is T
// ============================================================================
// Used to accept any container, view, or generator that yields T values.
// Enables list/grid components to work with vectors, spans, generators, etc.

template <typename R, typename T>
concept RangeLike = std::ranges::range<R> &&
    std::convertible_to<std::ranges::range_value_t<R>, T>;

// ============================================================================
// StringLike - Anything convertible to std::string_view
// ============================================================================
// Accepts const char*, std::string, std::string_view, and any user type
// with an implicit conversion to string_view.

template <typename T>
concept StringLike = std::convertible_to<T, std::string_view>;

// ============================================================================
// Widget - The full widget interface (Measurable + Renderable)
// ============================================================================
// A convenience composition. Most leaf nodes in the element tree satisfy
// this concept.

template <typename T>
concept Widget = Measurable<T> && Renderable<T>;

// ============================================================================
// InteractiveWidget - Widget that also handles input
// ============================================================================

template <typename T>
concept InteractiveWidget = Widget<T> && InputHandler<T>;

// ============================================================================
// StyledWidget - Widget with style properties
// ============================================================================

template <typename T>
concept StyledWidget = Widget<T> && Styleable<T>;

} // namespace maya
