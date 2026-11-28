#pragma once
// maya — a terminal view layer: elements, layout, style, widgets, and
// static output. The view half of an app; the runtime half is jaal, and
// <maya/host/run.hpp> joins them (maya::run). docs/internals/design.md.
//
//   - DSL          (v, h, text, dyn, when, map, pipes, styles)
//   - Events       (KeyEvent, key_is, ctrl_is, mouse helpers)
//   - Signals      (Signal, Computed, Effect, Batch)
//   - Output       (print, render_to_string)
//
// Internal subsystems (render pipeline, canvas, diff engine, SIMD,
// terminal I/O, layout engine) are in <maya/internal.hpp>.

// ── Core: types, error handling, concepts, reactive signals, focus ───────
#include <maya/core/types.hpp>
#include <maya/core/expected.hpp>
#include <maya/core/concepts.hpp>
#include <maya/core/signal.hpp>
#include <maya/core/animation.hpp>
#include <maya/core/motion.hpp>
#include <maya/core/overload.hpp>
#include <maya/core/scope_exit.hpp>
#include <maya/core/focus.hpp>
#include <maya/core/hit.hpp>
#include <maya/core/render_context.hpp>

// ── Style: colors, text styles, borders, themes ─────────────────────────
#include <maya/style/color.hpp>
#include <maya/style/style.hpp>
#include <maya/style/border.hpp>
#include <maya/style/theme.hpp>

// ── Elements: the element variant, concrete types, builder DSL ──────────
#include <maya/element/text.hpp>
#include <maya/element/box.hpp>
#include <maya/element/element.hpp>
#include <maya/element/builder.hpp>

// ── The terminal device's vocabulary: events, environment, theme ────────
#include <maya/device.hpp>
#include <maya/device/events.hpp>
#include <maya/device/environment.hpp>
#include <maya/print.hpp>

// ── DSL: compile-time UI tree builder ───────────────────────────────────
#include <maya/dsl.hpp>

// ── Widgets ─────────────────────────────────────────────────────────────
// Widgets are NOT included here. Include them individually as needed:
//
//   #include <maya/widget/input.hpp>
//   #include <maya/widget/agentty/markdown.hpp>
//   #include <maya/widget/scrollable.hpp>
//   ...
//
// See include/maya/widget/ for the full list.
