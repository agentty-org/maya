#pragma once
// maya::theme — semantic color slots, and the terminal they land on.
//
// ── The default is NATIVE ────────────────────────────────────────────────
//
// theme::native states almost no colors of its own. Its slots resolve to
// Color::default_color() — SGR 39/49, "whatever this terminal uses" — and
// to the sixteen NAMED ansi slots, which are indices into the palette the
// user configured, not colors maya picked.
//
// That is the only theme that is correct everywhere, and the reason is
// polarity. A palette of literal RGB is authored against one background:
// pick #565F89 for muted text and it reads on black and vanishes on light
// grey. maya cannot know the background — no terminal reliably reports it
// — so the only safe move is not to state one. Emit `39` and the user's
// foreground arrives, whatever they chose; emit `31` and their red
// arrives, already contrast-checked against their own background by them.
//
// The cost is that native cannot express a shade the palette lacks. Where
// a theme would use a dim blue-grey, native uses BOLD and DIM attributes
// for hierarchy instead of a third colour — which is what a 16-colour
// terminal has always done, and what degrades cleanly to monochrome.
//
// ── The named schemes are opt-in ─────────────────────────────────────────
//
// style/schemes.hpp carries Dracula, Nord, Gruvbox, Solarized, Catppuccin,
// TokyoNight and the rest, generated from mbadolato/iTerm2-Color-Schemes.
// Those DO state literal colors, including a background, which is the
// point of choosing one: the user has said "paint it like this" and maya
// then owns the whole surface. They are never a default.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <concepts>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

// LSan's interface, when we are built under it. Used by immortal_snapshot()
// below to mark the deliberately-never-freed projection snapshots as reachable
// -- see the comment there for why they cannot be freed.
#if defined(__SANITIZE_ADDRESS__)
#  define MAYA_HAS_LSAN 1
#elif defined(__has_feature)
// Nested #if, not `defined(__has_feature) && __has_feature(...)`: MSVC has no
// __has_feature and errors on the call syntax even when the defined() guard is
// false, because it does not short-circuit the call form.
#  if __has_feature(address_sanitizer)
#    define MAYA_HAS_LSAN 1
#  endif
#endif
#if defined(MAYA_HAS_LSAN) && __has_include(<sanitizer/lsan_interface.h>)
#  include <sanitizer/lsan_interface.h>
#else
#  undef MAYA_HAS_LSAN
#endif

#include "color.hpp"
#include "style.hpp"   // Style + live_color_resolver(), installed at the bottom

namespace maya {

// ============================================================================
// Theme - named color slots for semantic UI coloring
// ============================================================================
// 23 named slots covering general UI chrome, status colors, and diff
// highlighting.
//
// Every field is a LitColor, and that is load-bearing rather than cosmetic.
// A slot resolving to another slot made resolve() a PARTIAL function that
// advertised itself as total: `t.primary = Color::slot(Accent)` used to make
// resolve() hand back Kind::Slot, and the slot's enum then travelled onward
// as a red channel. LitColor cannot hold a slot, so the cycle is
// unrepresentable and resolve() is total because its codomain says so.
//
// ── One list ─────────────────────────────────────────────────────────
//
// MAYA_THEME_SLOTS below is the single authored statement of what a slot
// IS. The ThemeSlot enum, the struct's fields, resolve()'s switch, the
// field-name table and the completeness check are all generated from it.
//
// A slot used to be four separate edits — enum, field, switch arm, and every
// scheme — and missing any one of them failed differently: a missing switch
// arm is a -Wswitch warning (only fatal under MAYA_WERROR, which is CI-only),
// while a missing FIELD in the 57 designated-initializer schemes was silent
// and became ColorKind::Unset. Now the first three cannot disagree because
// they are one statement, and the fourth is caught by complete() below.
//
// The struct stays a plain aggregate: `Theme{.primary = ..., ...}` is exactly
// as before, which is what keeps all 57 schemes and every derive() call site
// untouched.

#define MAYA_THEME_SLOTS(X)                                                   \
    /* --- Primary palette --- */                                             \
    X(primary,      Primary)                                                  \
    X(secondary,    Secondary)                                                \
    X(accent,       Accent)                                                   \
    /* --- Status --- */                                                      \
    X(success,      Success)                                                  \
    X(error,        Error)                                                    \
    X(warning,      Warning)                                                  \
    X(info,         Info)                                                     \
    /* --- Text --- */                                                        \
    X(text,         Text)                                                     \
    X(inverse_text, InverseText)                                              \
    X(muted,        Muted)                                                    \
    /* --- Surfaces --- */                                                    \
    X(surface,      Surface)                                                  \
    X(background,   Background)                                               \
    X(border,       Border)                                                   \
    /* --- Diff --- */                                                        \
    X(diff_added,   DiffAdded)                                                \
    X(diff_removed, DiffRemoved)                                              \
    X(diff_changed, DiffChanged)                                              \
    /* --- Extras --- */                                                      \
    X(highlight,    Highlight)                                                \
    X(selection,    Selection)                                                \
    X(cursor,       Cursor)                                                   \
    X(link,         Link)                                                     \
    X(placeholder,  Placeholder)                                              \
    X(shadow,       Shadow)                                                   \
    X(overlay,      Overlay)

// The slot count, from the list rather than a hand-kept number.
inline constexpr std::size_t kThemeSlotCount = [] {
    std::size_t n = 0;
#define X(f, E) ++n;
    MAYA_THEME_SLOTS(X)
#undef X
    return n;
}();

static_assert(kThemeSlotCount == static_cast<std::size_t>(ThemeSlot::Overlay) + 1,
              "MAYA_THEME_SLOTS and the ThemeSlot enum disagree: color.hpp "
              "declares the enum, this list must name every enumerator.");

/// The struct-field name of a slot, for diagnostics and settings UI.
[[nodiscard]] constexpr std::string_view slot_field_name(ThemeSlot s) noexcept {
    switch (s) {
#define X(f, E) case ThemeSlot::E: return #f;
        MAYA_THEME_SLOTS(X)
#undef X
    }
    return "?";
}

struct Theme {
#define X(f, E) LitColor f;
    MAYA_THEME_SLOTS(X)
#undef X

    // ========================================================================
    // derive - create a new theme from a base with compile-time overrides
    // ========================================================================
    // Usage:
    //   constexpr auto my_theme = Theme::derive(theme::native,
    //       [](Theme& t) { t.primary = Color::hex(0x00FF00); });
    //
    // The callable receives a mutable Theme& and patches it in place.
    // Multiple overrides compose left-to-right.

    template <typename... Fns>
        requires (std::invocable<Fns, Theme&> && ...)
    [[nodiscard]] static constexpr Theme derive(const Theme& base, Fns&&... fns) {
        Theme t = base;
        (std::forward<Fns>(fns)(t), ...);
        return t;
    }

    constexpr bool operator==(const Theme&) const = default;

    // ========================================================================
    // Completeness — every slot was actually stated
    // ========================================================================
    // A Theme is an aggregate, so a designated initializer that omits a field
    // is legal and value-initializes it. That is the trap this guards: add a
    // slot to MAYA_THEME_SLOTS and all 57 schemes below silently acquire an
    // unstated colour for it, well-formed and invisible to every test.
    //
    // Unset (ColorKind's default) is what makes the omission detectable — it
    // is a state no deliberate choice can produce, unlike the white a
    // default-constructed Color used to be.

    /// The first slot nobody stated, or nullopt when the theme is total.
    /// Returns the SLOT rather than a bool so a failing static_assert can be
    /// chased with slot_field_name(*t.first_unset()).
    [[nodiscard]] constexpr std::optional<ThemeSlot> first_unset() const noexcept {
#define X(f, E) if (!f.is_set()) return ThemeSlot::E;
        MAYA_THEME_SLOTS(X)
#undef X
        return std::nullopt;
    }

    /// Did this theme state every slot?
    [[nodiscard]] constexpr bool complete() const noexcept {
        return !first_unset().has_value();
    }

    /// Substitute for a slot. Literals pass through untouched, so a host
    /// override always wins over a widget default.
    ///
    /// TOTAL: Sym in, Lit out, no case unhandled and no fallthrough that can
    /// silently ship a slot onward. The `return text` below is unreachable
    /// (every enumerator is covered) and exists only to satisfy the compiler
    /// on a value cast in from outside the enum's range.
    [[nodiscard]] constexpr LitColor resolve(Color c) const noexcept {
        if (auto lit = LitColor::try_literal(c)) return *lit;
        switch (c.theme_slot()) {
#define X(f, E) case ThemeSlot::E: return f;
            MAYA_THEME_SLOTS(X)
#undef X
        }
        return text;
    }
};

namespace theme {

// The theme in force, readable from anywhere that paints.
//
// Slot-kind colours are resolved against this at SGR-emit time, which is the
// only moment a widget Config default (written at static-init, with no theme
// in scope) can become a real hue. It is a POINTER into a table of themes
// with static storage, so a read is one relaxed atomic load rather than a
// copy of 24 Colors per span.
//
// The runtime re-seats it on every theme swap; before that it is `native`,
// which resolves every slot to the terminal's own palette — the safe answer
// for a unit test or a one-shot print that never starts a runtime.
namespace detail {
inline std::atomic<const Theme*>& live_slot() noexcept;
}

[[nodiscard]] inline const Theme& live() noexcept;
inline void set_live(const Theme& t) noexcept;
[[nodiscard]] inline bool try_set_live(const Theme& t) noexcept;

// ============================================================================
// native — the default. The terminal's own colors, and nothing else.
// ============================================================================
//
// Every slot here is Default (SGR 39/49) or Named (SGR 30-37/90-97). Not one
// is an RGB literal, which is what makes it correct on a light background, a
// dark one, a 16-color tty and a monochrome one alike.
//
// Reading the slots:
//
//   text/background  Default. The user's own pair, already legible together
//                    because they chose it. Stating either is how a TUI ends
//                    up as grey-on-grey in someone's light terminal.
//
//   error/success/   The palette's red/green/yellow/cyan. Every program
//   warning/info     emitting ANSI has meant these for forty years, so the
//                    user's palette has already contrast-checked them.
//
//   muted            bright_black — the dim tier every palette carries. This
//                    is the one slot with real tension: on some light schemes
//                    bright_black is close to the background. Prefer the DIM
//                    attribute over this slot where the hierarchy matters
//                    more than the hue.
//
//   surface/overlay  Default, NOT a shade. maya cannot darken a background it
//                    has not been told, and a guessed surface is the exact
//                    bug this theme exists to avoid. A panel separates itself
//                    with a border, not a fill.
inline constexpr Theme native {
    .primary       = Color::bright_blue(),
    .secondary     = Color::bright_green(),
    .accent        = Color::bright_magenta(),

    .success       = Color::green(),
    .error         = Color::red(),
    .warning       = Color::yellow(),
    .info          = Color::cyan(),

    .text          = Color::default_color(),
    .inverse_text  = Color::default_color(),
    .muted         = Color::bright_black(),

    .surface       = Color::default_color(),
    .background    = Color::default_color(),
    .border        = Color::bright_black(),

    // A diff tint is a BACKGROUND wash, and native has no background to
    // wash. The named green/red carry it as foreground instead, which is
    // how diff has always read on a 16-color terminal.
    .diff_added    = Color::green(),
    .diff_removed  = Color::red(),
    .diff_changed  = Color::yellow(),

    .highlight     = Color::bright_blue(),
    .selection     = Color::blue(),
    .cursor        = Color::default_color(),
    .link          = Color::bright_cyan(),
    .placeholder   = Color::bright_black(),
    .shadow        = Color::default_color(),
    .overlay       = Color::default_color(),
};

// ============================================================================
// Does this theme own the canvas?
// ============================================================================
//
// The one question that separates `native` from every RGB scheme, and it is
// answered by DATA rather than by a name or a table index: a theme owns the
// canvas exactly when it states a real background.
//
// `native` sets `.background = default_color()`, meaning "whatever the user's
// terminal already is". There is nothing to paint, and painting anything
// would be a guess — the bug native exists to avoid. So the app fills
// nothing and the terminal shows through, including its own transparency
// and background image.
//
// A scheme like Dracula states `#282A36`. Half-applying it — Dracula's
// foregrounds over the user's own background — is what makes a "theme"
// feel broken: the hues were contrast-checked against THAT canvas, not
// against whatever is behind them. So a theme that names a background
// means it, and the host fills the frame with it.
//
// Hosts use this to decide whether to wrap their root element in a bgc():
//
//     Element root = build();
//     if (theme::owns_canvas(t)) root = std::move(root) | bgc(t.background);
//
[[nodiscard]] constexpr bool owns_canvas(const Theme& t) noexcept {
    return t.background.kind() != Color::Kind::Default;
}

// ── Categorical series colour ───────────────────────────────────────
//
// The hue for the i-th slice of a chart whose slices are NAMED THINGS
// rather than states — tools by call count, providers by spend, files by
// churn.
//
// This is a genuinely different question from "what colour is an error",
// and conflating the two is why hosts kept growing private RGB ramps: a
// status palette says green=good / red=bad, which is a lie when the slices
// are "read" and "write". But a private ramp is a palette the theme cannot
// see, so it stops following the user the moment they pick a scheme.
//
// Both concerns are satisfied by deriving the ramp FROM the theme's own
// role slots, in an order that alternates across the spectrum so adjacent
// slices contrast instead of walking through two neighbouring blues. The
// slots reused here (link/primary/warning/success/accent/info/error/
// secondary) are the eight the schemes keep maximally distinct, because
// they are the ones that must stay distinguishable as UI roles.
//
// `i` wraps, so a caller never has to bound it.
[[nodiscard]] constexpr Color series(const Theme& t, std::size_t i) noexcept {
    const Color ramp[] = {
        t.link, t.primary, t.warning, t.success,
        t.accent, t.info, t.error, t.secondary,
    };
    return ramp[i % (sizeof(ramp) / sizeof(ramp[0]))];
}

// The "none of the above" slice. A catch-all is the ABSENCE of a category,
// so giving it a category's colour makes it read as one more of them.
[[nodiscard]] constexpr Color series_other(const Theme& t) noexcept {
    return t.muted;
}

// ── live theme slot (declared above; defined here, now that native exists)
namespace detail {
inline std::atomic<const Theme*>& live_slot() noexcept {
    static std::atomic<const Theme*> t{&native};
    return t;
}
// Bumped by every set_live(). See live_epoch() / projected<P>().
inline std::atomic<unsigned>& epoch_slot() noexcept {
    static std::atomic<unsigned> e{0};
    return e;
}
}  // namespace detail

[[nodiscard]] inline const Theme& live() noexcept {
    return *detail::live_slot().load(std::memory_order_acquire);
}

/// How many times the live theme has been replaced.
///
/// The version a PROJECTED palette compares against to notice it is stale.
/// A counter rather than a pointer because the theme slot is assigned
/// THROUGH (`*slot = t`), so its address never moves and identity cannot
/// detect a swap — the bug that made StylePool's cache a permanent no-op.
[[nodiscard]] inline unsigned live_epoch() noexcept {
    return detail::epoch_slot().load(std::memory_order_acquire);
}

// ── Interning: the slot OWNS what it points at ──────────────────────────
//
// set_live used to store `&t` and rely on the caller to keep `t` alive
// forever. That contract was real — app_set_theme() routes through the
// Runtime's slot or a function-local static precisely to satisfy it — but
// it was unstated at the signature, so the obvious call
//
//     theme::set_live(Theme::derive(base, accent));   // temporary
//
// compiled, and left live() dereferencing a dead object on the very next
// paint. A borrow whose lifetime obligation is documented in the CALLER is
// not a contract, it is a convention; this one had exactly one honest
// implementation and any number of wrong ones.
//
// So the slot interns. A theme handed in is copied into storage that is
// never freed, and the pointer published to readers is into THAT copy. The
// caller's object is no longer load-bearing and may be a temporary, a
// stack local, or a member that outlives nothing.
//
// Never-freed is the same reasoning immortal_snapshot() states below: a
// concurrent reader may hold the previous pointer, and anything that frees
// hands the memory back to the allocator whose next write races that read.
// Dedup keeps the count at "distinct theme VALUES ever set" rather than
// "calls", which is what bounds it under a flapping auto-detect that
// oscillates between two themes forever (see app_set_theme's note on
// background detection flipping across an ssh hop).
namespace detail {

// Allocate an object that is meant to outlive the program.
//
// Two clients: the live-theme intern table just below, and the projection
// snapshots further down. The leak is the DESIGN, stated once here so
// neither has to re-argue it: a reader holding the previous pointer must
// keep reading a valid, unwritten object, and anything that frees hands the
// memory back to the allocator, whose next write into it races that reader.
// Measured -- a deque-backed version was still flagged by TSan for exactly
// this.
//
// But LSan cannot tell a deliberate immortal from a bug, and it was right to
// complain: every theme change left a snapshot unreachable at exit, so maya's
// sanitizer job reported 615 leaked allocations and had been red since the
// projection cache landed. A red CI that is red on purpose stops being read,
// which is the real cost -- it hid the MSVC break for six commits.
//
// __lsan_ignore_object() says "this one is intentional" at the allocation
// site, so a snapshot is exempt while an actual leak anywhere else still
// fails the build. Compiled out entirely when not under ASan.
template <class T, class... Args>
[[nodiscard]] T* immortal_snapshot(Args&&... args) {
    T* p = new T{std::forward<Args>(args)...};
#if defined(MAYA_HAS_LSAN)
    __lsan_ignore_object(p);
#endif
    return p;
}

inline std::mutex& intern_mu() noexcept { static std::mutex m; return m; }

// Append-only, and only ever appended to under intern_mu(). Readers never
// touch this vector — they read the atomic slot, which points into a
// stable heap object, not into the vector's buffer. That indirection is
// what lets the table grow (and reallocate) without disturbing a reader.
inline std::vector<const Theme*>& intern_table() {
    static std::vector<const Theme*> v;
    return v;
}

// The stable address for `t`'s VALUE. Returns the existing entry when this
// theme has been seen before, so N swaps between two themes allocate twice.
// Caller holds intern_mu().
[[nodiscard]] inline const Theme* intern_locked(const Theme& t) {
    auto& tab = intern_table();
    for (const Theme* p : tab)
        if (*p == t) return p;
    // native is the initial slot value and is never interned by the loop
    // above on first call; comparing against it here keeps the common
    // "reset to native" path from minting a duplicate of a static.
    if (t == native) return &native;
    const Theme* p = immortal_snapshot<Theme>(t);
    tab.push_back(p);
    return p;
}

}  // namespace detail

/// Install `t` as the live theme. Returns false — changing nothing — when
/// `t` leaves a slot unstated.
///
/// Totality is checked HERE because this is the one place a Theme that was
/// never a constant expression can enter the paint path. complete() is a
/// static_assert for the 57 built-in schemes and for anything constexpr,
/// but a theme parsed from a user config, or built field-by-field, or
/// derive()d at runtime reaches the slot without ever meeting that assert.
/// An Unset slot paints as "inherit" (ColorKind::Unset, color.hpp:492), so
/// the failure is a widget silently adopting the terminal's foreground
/// instead of its role colour — legible often enough to ship, wrong on
/// exactly the light-background terminal nobody tested.
///
/// Rejecting is better than clamping: a theme missing a slot is a caller
/// bug, and substituting native's answer for that one slot would produce a
/// half-themed surface that looks deliberate.
[[nodiscard]] inline bool try_set_live(const Theme& t) noexcept {
    if (!t.complete()) return false;
    // Theme first, THEN the epoch, with release ordering: a reader that sees
    // epoch N+1 sees the theme that produced it. That only holds if the two
    // stores of one writer can't interleave with another's (A stores its
    // theme, B stores its theme and bumps, A bumps: a reader of A's epoch may
    // read A's theme while B's is current, and cache a stale projection for
    // good). So writers take intern_mu() across the pair. Readers never lock.
    std::lock_guard lk(detail::intern_mu());
    const Theme* p = detail::intern_locked(t);
    detail::live_slot().store(p, std::memory_order_release);
    detail::epoch_slot().fetch_add(1, std::memory_order_release);
    return true;
}

/// Install `t` as the live theme, asserting totality.
///
/// The ergonomic form, for the callers that build a theme from a built-in
/// scheme or a derive() of one — where completeness is already a
/// static_assert and a runtime failure would mean the static_assert lied.
/// Hosts taking a theme from outside the program (a config file, a wire
/// message) should call try_set_live() and handle false.
inline void set_live(const Theme& t) noexcept {
    const bool ok = try_set_live(t);
    assert(ok && "theme::set_live: theme leaves a slot unstated — find it "
                 "with slot_field_name(*t.first_unset())");
    (void)ok;
}

/// Binding a temporary used to be the quiet way to get a dangling live
/// theme. Interning made it safe, so this no longer needs to be deleted —
/// `set_live(Theme::derive(base, fn))` is now correct, and the overload
/// exists only to say so where a reader would otherwise wonder.
inline void set_live(Theme&& t) noexcept { set_live(static_cast<const Theme&>(t)); }
[[nodiscard]] inline bool try_set_live(Theme&& t) noexcept {
    return try_set_live(static_cast<const Theme&>(t));
}

// ============================================================================
// Terminal colours — what the terminal itself reported at startup
// ============================================================================
// theme::native states no RGB: text and background are "the terminal's
// default" and hues are ANSI palette entries, so nothing can be mixed from
// them (tinted bands, panels, match highlights). At startup maya asks the
// terminal for its foreground, background and 16-colour palette (OSC 10,
// 11, 4) in the same round-trip it already makes. When the terminal
// answers, measure() turns those into real RGB; when it doesn't, measure()
// stays honest and returns the colour unmeasured.
struct TerminalColors {
    bool     have_fgbg = false;
    LitColor fg = LitColor::rgb(0, 0, 0), bg = LitColor::rgb(0, 0, 0);
    std::uint16_t have_palette = 0;           // bit i = palette[i] known
    LitColor palette[16]{};
};
namespace detail {
inline TerminalColors& terminal_colors_slot() noexcept {
    static TerminalColors tc;
    return tc;
}
}  // namespace detail

// Set once at startup, from the device's probe, before the first frame.
inline void set_terminal_colors(LitColor fg, LitColor bg) noexcept {
    auto& tc = detail::terminal_colors_slot();
    tc.fg = fg; tc.bg = bg; tc.have_fgbg = true;
}
inline void set_terminal_palette(int index, LitColor rgb) noexcept {
    if (index < 0 || index > 15) return;
    auto& tc = detail::terminal_colors_slot();
    tc.palette[index] = rgb;
    tc.have_palette = static_cast<std::uint16_t>(tc.have_palette | (1u << index));
}
[[nodiscard]] inline const TerminalColors& terminal_colors() noexcept {
    return detail::terminal_colors_slot();
}

// `c` as real RGB, or `c` unchanged when that isn't known. `as_bg` picks
// which default a Default colour means (the canvas or the ink).
[[nodiscard]] inline LitColor measure(LitColor c, bool as_bg = false) noexcept {
    if (c.has_channels()) return c;
    const auto& tc = terminal_colors();
    if (c.kind() == ColorKind::Default)
        return tc.have_fgbg ? (as_bg ? tc.bg : tc.fg) : c;
    if (c.kind() == ColorKind::Named && c.index() < 16
        && (tc.have_palette & (1u << c.index())))
        return tc.palette[c.index()];
    return c;
}

// ============================================================================
// with_contrast_floor — lift a scheme's ink slots until they read
// ============================================================================
// Measured across the 615 bundled schemes: `primary`, `accent` and `error`
// fall under 4.5:1 against their own background in over a third of them,
// and `muted` under 3:1 in many. Everything coloured by those slots (links,
// headings, bullets, hints) was hard to read there, and fixing it one
// widget at a time never ends.
//
// This returns `t` with each ink slot moved in LIGHTNESS only (hue and
// saturation kept) until it reaches the floor against `background`: 4.5:1
// for the hue slots, 3.5:1 for the deliberately quiet ones (3:1 plus room
// for the tinted tool panel, which sits a shade off the background). A slot that
// already reads is untouched, and so is any slot or background that isn't
// an RGB colour (theme::native states none, so it comes back unchanged).
// Backgrounds, bands and the chip ink are not ink-on-canvas, so they are
// left alone.
namespace detail {
[[nodiscard]] inline double rel_lum(const LitColor& c) noexcept {
    if (!c.has_channels()) return 0.0;   // a slot or named colour has no RGB to weigh
    auto f = [](int v) {
        const double s = v / 255.0;
        return s <= 0.03928 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * f(c.r()) + 0.7152 * f(c.g()) + 0.0722 * f(c.b());
}
[[nodiscard]] inline double contrast(const LitColor& a, const LitColor& b) noexcept {
    const double x = rel_lum(a), y = rel_lum(b);
    return (std::max(x, y) + 0.05) / (std::min(x, y) + 0.05);
}
// `fg` with its HSL lightness stepped away from `bg` until it reaches
// `target`, or as far as it can go.
[[nodiscard]] inline LitColor lift_to(LitColor fg, const LitColor& bg,
                                      double target) noexcept {
    if (!fg.has_channels() || !bg.has_channels()) return fg;
    if (contrast(fg, bg) >= target) return fg;
    const double r = fg.r() / 255.0, g = fg.g() / 255.0, b = fg.b() / 255.0;
    const double mx = std::max({r, g, b}), mn = std::min({r, g, b});
    double h = 0, s = 0, l = (mx + mn) / 2;
    if (mx != mn) {
        const double d = mx - mn;
        s = l > 0.5 ? d / (2 - mx - mn) : d / (mx + mn);
        if (mx == r)      h = (g - b) / d + (g < b ? 6 : 0);
        else if (mx == g) h = (b - r) / d + 2;
        else              h = (r - g) / d + 4;
        h /= 6;
    }
    auto hue = [](double p, double q, double t) {
        if (t < 0) t += 1;
        if (t > 1) t -= 1;
        if (t < 1.0 / 6) return p + (q - p) * 6 * t;
        if (t < 0.5)     return q;
        if (t < 2.0 / 3) return p + (q - p) * (2.0 / 3 - t) * 6;
        return p;
    };
    auto at = [&](double L) {
        auto byte = [](double v) {
            return static_cast<uint8_t>(std::clamp(v, 0.0, 1.0) * 255 + 0.5);
        };
        if (s == 0) return LitColor::rgb(byte(L), byte(L), byte(L));
        const double q = L < 0.5 ? L * (1 + s) : L + s - L * s;
        const double p = 2 * L - q;
        return LitColor::rgb(byte(hue(p, q, h + 1.0 / 3)), byte(hue(p, q, h)),
                             byte(hue(p, q, h - 1.0 / 3)));
    };
    // Walk both ways and keep whichever reaches the target first (or gets
    // closest). Usually that is away from the canvas, but on a mid-grey
    // canvas the far side can be the only one with room.
    auto walk = [&](bool up) {
        double L = l;
        LitColor best = fg;
        for (int i = 0; i < 50; ++i) {
            L = up ? std::min(1.0, L + 0.02) : std::max(0.0, L - 0.02);
            const LitColor c = at(L);
            if (contrast(c, bg) > contrast(best, bg)) best = c;
            if (contrast(c, bg) >= target || L <= 0.0 || L >= 1.0) break;
        }
        return best;
    };
    const LitColor away = walk(rel_lum(bg) < 0.18);
    if (contrast(away, bg) >= target) return away;
    const LitColor toward = walk(rel_lum(bg) >= 0.18);
    const LitColor best = contrast(toward, bg) > contrast(away, bg) ? toward : away;
    return contrast(best, bg) > contrast(fg, bg) ? best : fg;
}
}  // namespace detail

[[nodiscard]] inline Theme with_contrast_floor(const Theme& t,
                                               double ink   = 4.5,
                                               double quiet = 3.5) noexcept {
    Theme out = t;
    const LitColor bg = t.background;
    if (!bg.has_channels()) return out;
    for (LitColor* c : {&out.primary, &out.secondary, &out.accent, &out.success,
                        &out.warning, &out.error, &out.info, &out.link, &out.text})
        *c = detail::lift_to(*c, bg, ink);
    for (LitColor* c : {&out.muted, &out.placeholder})
        *c = detail::lift_to(*c, bg, quiet);
    return out;
}

// ============================================================================
// projected<P> — a palette DERIVED from the theme, never pushed to
// ============================================================================
// Some subsystems cannot read the Theme per-use: markdown keeps a flat
// palette because its render path is hot and its parse worker runs
// off-thread with no way to reach a Theme. Those projections have to be
// re-derived when the theme changes.
//
// The old answer was a push: on_theme_changed(fn), with each subsystem
// registering once. That works only if everyone remembers — and the one
// that forgot (markdown) was invisible until a user reported half-themed
// output, because a projection that never re-derives looks exactly like a
// projection whose theme never changed.
//
// So the dependency is inverted: deriving IS the read path. A projection
// states how to compute itself, and asking for it checks the epoch first.
// A subsystem that "forgets to subscribe" is not expressible, because there
// is no subscription — the only way to read is the way that refreshes.
//
//     struct MyPalette {
//         using type = MyColors;
//         static type project(const Theme& t) { return {...}; }
//     };
//     const MyColors& c = theme::projected<MyPalette>();
//
// Cost in the steady state is one acquire load and one integer compare.
template <class P>
concept Projection = requires (const Theme& t) {
    typename P::type;
    { P::project(t) } -> std::same_as<typename P::type>;
    requires std::equality_comparable<typename P::type>;
};

namespace detail {

// Per-projection state. Snapshots are append-only and NEVER freed: a reader
// holding the previous pointer must keep reading a valid, UNWRITTEN object,
// and any container that frees hands the memory back to the allocator, whose
// write into it races that reader. (Measured — a deque-backed version was
// still flagged by TSan for exactly this.) The cost is bounded by how many
// times a human changes theme in one session.
template <Projection P>
struct ProjectionState {
    static std::mutex& mu() { static std::mutex m; return m; }
    static std::atomic<const typename P::type*>& slot() {
        static std::atomic<const typename P::type*> s{
            immortal_snapshot<typename P::type>(P::project(live()))};
        return s;
    }
    static std::atomic<unsigned>& seen() {
        // No epoch is ever ~0u, so the first projected() re-derives. Starting
        // at live_epoch() let a theme change between this and slot()'s init
        // cache the old projection under the new epoch, for good.
        static std::atomic<unsigned> e{~0u};
        return e;
    }
};

}  // namespace detail

/// The projection of the live theme. Re-derives itself if the theme moved.
template <Projection P>
[[nodiscard]] const typename P::type& projected() {
    using S = detail::ProjectionState<P>;
    auto& slot = S::slot();          // forces init before the epoch compare
    const unsigned now = live_epoch();
    if (S::seen().load(std::memory_order_acquire) != now) {
        // Serialises re-derivers against each other. Readers in the common
        // case never reach here.
        std::lock_guard lk(S::mu());
        if (S::seen().load(std::memory_order_relaxed) != now) {
            auto next = P::project(live());
            // Only publish a new snapshot when the VALUE actually moved.
            // Hosts re-publish the same theme every frame (that is how
            // `auto` follows a tmux detach), and two different themes can
            // project to the same palette; neither should leak a snapshot.
            if (!(*slot.load(std::memory_order_relaxed) == next))
                slot.store(detail::immortal_snapshot<typename P::type>(std::move(next)),
                           std::memory_order_release);
            S::seen().store(now, std::memory_order_release);
        }
    }
    return *slot.load(std::memory_order_acquire);
}

/// Force a projection to a value the theme did not produce.
///
/// For a host that wants an explicit palette rather than a derived one. It
/// stands until the next theme change, which re-derives — the theme is the
/// source of truth, and an override is a deliberate exception to it.
template <Projection P>
void override_projection(const typename P::type& v) {
    using S = detail::ProjectionState<P>;
    auto& slot = S::slot();
    std::lock_guard lk(S::mu());
    if (!(*slot.load(std::memory_order_relaxed) == v))
        slot.store(detail::immortal_snapshot<typename P::type>(v),
                   std::memory_order_release);
    S::seen().store(live_epoch(), std::memory_order_release);
}

// ── Is this colour already the muted ink? ───────────────────────────────
//
// Several widgets dim a colour to recede it, but must NOT dim one that is
// already the muted tone — double-dimming sinks it into the background.
// Six of them had grown the same private check against literal
// bright_black, which stopped being true the moment configs carried the
// Muted slot instead. One definition, asked on the far side of the theme.
//
// The kind-by-kind comparison this used to do is gone: resolve() maps both
// sides into LitColor, where equality is just equality. That is the shape of
// the welcome-screen bug too (comparing before resolving), and it cannot be
// written here any more — Color has no channels to compare.
[[nodiscard]] inline bool is_muted(const Color& c) noexcept {
    const Theme& t = live();
    return t.resolve(c) == t.muted;
}

// ============================================================================
// Capability detection
// ============================================================================
//
// What a terminal can RENDER, decided before the first colored byte. The
// order is the one every mature TUI converges on, and each step is a signal
// the user or their terminal actually set — never a guess from TERM alone.

enum class ColorTier : std::uint8_t {
    Mono,        // no color at all
    Ansi16,      // 30-37 / 90-97
    Ansi256,     // 38;5;n
    TrueColor,   // 38;2;r;g;b
};

namespace detail {

[[nodiscard]] inline bool env_set(const char* k) {
    const char* v = std::getenv(k);
    return v != nullptr && *v != '\0';
}

[[nodiscard]] inline std::string_view env_or(const char* k, std::string_view d = {}) {
    const char* v = std::getenv(k);
    return (v && *v) ? std::string_view{v} : d;
}

[[nodiscard]] inline bool contains(std::string_view h, std::string_view n) {
    return h.find(n) != std::string_view::npos;
}

[[nodiscard]] inline bool starts_with(std::string_view h, std::string_view n) {
    return h.size() >= n.size() && h.substr(0, n.size()) == n;
}

// Terminals that ARE truecolor but only say so via COLORTERM — which ssh
// does not forward (it is not in most sshd AcceptEnv lists) while TERM is.
// Over ssh the escape bytes are interpreted by the LOCAL terminal, so when
// TERM names one of these, emitting 38;2 is always right. Without this a
// remote session silently drops to 16 colours and vivid RGB row bands
// quantise into garish solid blocks.
inline constexpr std::string_view kTrueColorTerms[] = {
    "kitty", "ghostty", "wezterm", "alacritty", "foot",
    "iterm",  "konsole", "contour", "rio",
};

// Host applications that set their own marker instead of COLORTERM.
// Checked only as a LAST resort before the TERM heuristics, because a
// program name is weaker evidence than a capability claim.
[[nodiscard]] inline bool truecolor_host() {
    // Windows Terminal and ConEmu both do 24-bit and neither reliably
    // sets COLORTERM.
    if (env_set("WT_SESSION")) return true;
    if (env_or("ConEmuANSI") == "ON") return true;
    const std::string_view tp = env_or("TERM_PROGRAM");
    // Apple Terminal is deliberately ABSENT: it is 256-colour only, and
    // claiming truecolor there produces visibly wrong hues.
    return tp == "iTerm.app" || tp == "WezTerm" || tp == "ghostty"
        || tp == "vscode"    || tp == "Hyper"   || tp == "rio";
}

}  // namespace detail

// Detect what the terminal can render.
//
//   `tty` is whether stdout is a terminal; a redirect gets Mono so a piped
//   log is plain text rather than escape soup. The caller supplies it
//   because maya's platform layer owns that question.
// ── Is this terminal incapable of ANSI at all? ─────────────────────────
//
// `TERM=dumb` is the one value with a precise, non-negotiable meaning: no
// escape sequences. Not "no colour" — no cursor addressing, no DEC private
// modes, nothing. It is what a user sets when they want plain text, and
// what a CI log or a `TERM=dumb make` invocation reports.
//
// Checked in ONE place, because it used to be checked in exactly one place
// and that place was the COLOUR tier — so agentty honoured it for colour
// and then emitted cursor hides and mode switches anyway (issue #37: "I
// found agentty doesn't respect TERM"). A capability question needs one
// answer every layer can ask, not a per-layer opinion.
[[nodiscard]] inline bool terminal_is_dumb() {
    return detail::env_or("TERM") == "dumb";
}

[[nodiscard]] inline ColorTier detect_tier(bool tty) {
    using namespace detail;

    // ── 0. Explicit override ────────────────────────────────────────
    // Above even NO_COLOR, because it is the escape hatch for the case
    // detection cannot win: truecolor passthrough inside tmux, a terminal
    // nobody has heard of, or a test that needs a fixed answer.
    const std::string_view forced_tier = env_or("MAYA_COLOR");
    if (forced_tier == "truecolor" || forced_tier == "24bit"
        || forced_tier == "3")                    return ColorTier::TrueColor;
    if (forced_tier == "256" || forced_tier == "2") return ColorTier::Ansi256;
    if (forced_tier == "16" || forced_tier == "basic"
        || forced_tier == "1")                    return ColorTier::Ansi16;
    if (forced_tier == "none" || forced_tier == "mono"
        || forced_tier == "0")                    return ColorTier::Mono;
    // "auto" or anything unrecognised falls through to detection.

    // ── 1. NO_COLOR ─────────────────────────────────────────────
    // Any non-empty value disables colour (no-color.org). An EMPTY value is
    // treated as unset, which the standard is explicit about — `NO_COLOR= cmd`
    // must still be colourful. env_set() already encodes that.
    if (env_set("NO_COLOR")) return ColorTier::Mono;

    // ── 2. CLICOLOR_FORCE / the tty test ──────────────────────────────
    // A redirect gets Mono so a piped log is text, not escape soup;
    // CLICOLOR_FORCE exists precisely to override that for CI and pagers.
    const bool forced = env_set("CLICOLOR_FORCE")
                        && env_or("CLICOLOR_FORCE") != "0";

    const std::string_view term = env_or("TERM");
    if (term == "dumb") return ColorTier::Mono;   // see terminal_is_dumb()

    // A host that NAMES ITSELF outranks the handle probe.
    //
    // `tty` is a property of a file descriptor; "I am Windows Terminal" is a
    // property of the thing drawing the pixels, and the second is what the
    // colour question is actually about. They disagree exactly where the
    // handle is a VT pipe rather than a console object -- mintty, Git Bash,
    // Cygwin, ConPTY -- and when they do, the descriptor is the one that is
    // wrong. Ordering the probe first is what made a self-identified
    // truecolor terminal answer Mono, collapsing every theme to native and
    // dropping diff bands to plain text.
    //
    // is_tty() now understands VT pipes too, so this is belt AND braces: the
    // tier survives even where the handle cannot be classified at all.
    const bool self_identified = truecolor_host();
    if (!tty && !forced && !self_identified) return ColorTier::Mono;

    // ── 3. COLORTERM ── the terminal's own capability claim ───────────────
    const std::string_view ct = env_or("COLORTERM");
    if (ct == "truecolor" || ct == "24bit") return ColorTier::TrueColor;

    // ── 4. TERM conventions, most specific first ───────────────────────
    if (contains(term, "direct")) return ColorTier::TrueColor;

    // Named truecolor terminals. This is the ssh case: COLORTERM does not
    // survive the hop, TERM does, and the bytes are drawn by the local
    // terminal either way.
    for (const std::string_view name : kTrueColorTerms)
        if (contains(term, name)) return ColorTier::TrueColor;

    // ── 5. Host-application markers ─────────────────────────────────
    // Weaker evidence than a capability CLAIM (COLORTERM/TERM name it
    // outright), so it sits below both -- but computed up at step 2,
    // because it also decides whether a non-console handle is a terminal
    // at all.
    if (self_identified) return ColorTier::TrueColor;

    if (contains(term, "256color")) return ColorTier::Ansi256;
    if (term.empty()) {
#if defined(_WIN32)
        // A native Windows console (cmd.exe / PowerShell under conhost) leaves
        // TERM unset, yet modern conhost — Win10 1607+, which maya's Win32
        // backend already requires and switches into ENABLE_VIRTUAL_TERMINAL_
        // PROCESSING — renders 24-bit colour. Reaching here means a real
        // console: a redirect with no host marker already returned Mono at the
        // handle-probe gate above, and every self-identifying host (WT_SESSION,
        // ConEmu, TERM_PROGRAM) returned TrueColor earlier. So the correct
        // answer is TrueColor, not the POSIX "empty TERM = not a terminal"
        // Mono that left agentty monochrome in plain cmd/PowerShell.
        return ColorTier::TrueColor;
#else
        return ColorTier::Mono;
#endif
    }

    // ── 6. The xterm/screen/tmux family ───────────────────────────────
    // Real xterm has done 256 colours for two decades, and screen and tmux
    // both pass 38;5 through. Calling these Ansi16 was the harmful default:
    // indexed SGR is universally safe here, and quantising to sixteen makes
    // tuned greys and dark tints collapse into primaries.
    if (starts_with(term, "xterm") || starts_with(term, "screen")
        || starts_with(term, "tmux")  || starts_with(term, "rxvt")
        || starts_with(term, "vte")   || starts_with(term, "linux"))
        return ColorTier::Ansi256;

    // 7. Anything else that looks like a terminal gets the sixteen colours
    //    every terminal has had since the VT100's descendants.
    return ColorTier::Ansi16;
}

// ============================================================================
// Background polarity
// ============================================================================
//
// Whether the terminal is light or dark. This is ONLY needed to pick a
// default named scheme — theme::native does not care, which is the point of
// it. Detection is deliberately conservative: it reports Unknown rather than
// guess, because a wrong guess here is the bug that makes a TUI unreadable.

enum class Polarity : std::uint8_t { Unknown, Dark, Light };

// COLORFGBG is set by rxvt, urxvt, konsole and a few others: "fg;bg" or
// "fg;default;bg", where the background field is an ANSI index. 0-6 and 8
// are dark; 7 and 15 are the light greys.
//
// This is the cheap signal and it never blocks. The authoritative one is an
// OSC 11 query, which costs a round trip and raw mode — maya does not do it
// here because a library must not steal the terminal mid-render; a host that
// wants it should query at startup and pass the answer to set_polarity().
[[nodiscard]] inline Polarity detect_polarity() {
    const std::string_view v = detail::env_or("COLORFGBG");
    if (v.empty()) return Polarity::Unknown;
    const auto last = v.find_last_of(';');
    if (last == std::string_view::npos) return Polarity::Unknown;
    const std::string_view bg = v.substr(last + 1);
    if (bg.empty()) return Polarity::Unknown;
    // A single index. Anything non-numeric ("default") tells us nothing.
    int n = 0;
    for (const char c : bg) {
        if (c < '0' || c > '9') return Polarity::Unknown;
        n = n * 10 + (c - '0');
    }
    return (n == 7 || n == 15) ? Polarity::Light : Polarity::Dark;
}

}  // namespace theme

// ============================================================================
// Install the live resolver into the style layer
// ============================================================================
// style.hpp declares live_color_resolver() and cannot fill it: it sits BELOW
// this header and has never heard of a Theme. Here the live theme is
// reachable, so the hook is installed at static-init — the dependency is
// inverted rather than the layering broken, and Style::to_sgr() keeps its
// convenient zero-argument spelling without style.hpp learning about themes.
namespace detail {
inline const bool kResolverInstalled = [] {
    live_color_resolver() = [](Color c) noexcept -> LitColor {
        return theme::live().resolve(c);
    };
    return true;
}();
}  // namespace detail

// ── The diff palette ───────────────────────────────────────────────
//
// The ONE place in the program that states colour literals on purpose,
// and the reasoning is worth keeping because it is the exact opposite of
// the rule everywhere else (agentty #45: a widget must name a role, not a
// colour).
//
// A diff is not themed chrome. Green-means-added and red-means-removed is
// forty years of muscle memory, the same in `git diff`, GitHub, every
// review tool anyone has used. It is CONTENT, not decoration — closer to
// syntax highlighting (which is also exempt) than to a button.
//
// And unlike chrome, both sides of the pair have to be right TOGETHER.
// Taking the band from a theme slot and the ink from anywhere else is
// what broke: under theme::native the diff slots resolve to plain ANSI
// green/red, whose real values only the terminal knows, so every attempt
// to compute readable ink for them was guessing at a palette we cannot
// read. Stating both sides is the only way to guarantee the pair.
//
// Contrast measured (WCAG 2.1, AAA needs 7:1 for body text):
//
//   add  band  10.29:1     rail  7.06:1
//   rem  band  11.42:1     rail  8.92:1
//   hunk band  10.40:1
//
// Dark, saturated bands with pale same-hue ink. Dark because the band has
// to sit UNDER text without competing with it, saturated because on a
// low-gamma screen it is channel separation rather than lightness that
// carries hue — a greyish dark green reads as grey.
//
// Truecolor values, only used when the terminal has 256+ colours (see
// ToolBodyPreview::diff_bands_ok). Below that the bands are dropped for
// fg-only diff: there is no 16-colour background that reads as a band.
namespace diff_palette {
inline constexpr Color add_bg    = Color::hex(0x0A3D1C);
inline constexpr Color add_rail  = Color::hex(0x11602A);
inline constexpr Color add_fg    = Color::hex(0xC8F5D4);
inline constexpr Color add_fg_hi = Color::hex(0xE4FBEA);

inline constexpr Color rem_bg    = Color::hex(0x4A0E16);
inline constexpr Color rem_rail  = Color::hex(0x7A1C24);
inline constexpr Color rem_fg    = Color::hex(0xFCD4DC);
inline constexpr Color rem_fg_hi = Color::hex(0xFFE8EC);

inline constexpr Color hunk_bg   = Color::hex(0x1E2555);
inline constexpr Color hunk_fg   = Color::hex(0xCEDAFF);
// A search hit on either band: pure white, brighter than both *_fg_hi.
inline constexpr Color hit_fg    = Color::hex(0xFFFFFF);
}  // namespace diff_palette

}  // namespace maya
