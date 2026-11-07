#pragma once
// maya::style::color - Compile-time type-safe color system
//
// Every color is constexpr-constructible and validated at compile time.
// Supports ANSI 16, ANSI 256, and 24-bit truecolor. Colors are value types -
// small, trivially copyable, and zero-cost to pass around.

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

#include "quantize.hpp"   // chroma-locked perceptual palette matching

namespace maya {

// ============================================================================
// Color - A tagged union of color representations
// ============================================================================
// Like Rust's enum Color { Named(AnsiColor), Indexed(u8), Rgb(u8,u8,u8) }
// but in C++ with constexpr everything.

enum class AnsiColor : uint8_t {
    Black        = 0,
    Red          = 1,
    Green        = 2,
    Yellow       = 3,
    Blue         = 4,
    Magenta      = 5,
    Cyan         = 6,
    White        = 7,
    BrightBlack  = 8,
    BrightRed    = 9,
    BrightGreen  = 10,
    BrightYellow = 11,
    BrightBlue   = 12,
    BrightMagenta= 13,
    BrightCyan   = 14,
    BrightWhite  = 15,
};

namespace detail {

// xterm 256-color 6×6×6 cube levels.
inline constexpr int kCubeLevels[6] = {0x00, 0x5f, 0x87, 0xaf, 0xd7, 0xff};

// The 16 standard ANSI colors as RGB, for nearest-color downgrades.
inline constexpr int kAnsi16[16][3] = {
    {0,0,0},   {128,0,0},  {0,128,0},  {128,128,0},
    {0,0,128}, {128,0,128},{0,128,128},{192,192,192},
    {128,128,128},{255,0,0},{0,255,0}, {255,255,0},
    {0,0,255}, {255,0,255},{0,255,255},{255,255,255},
};

[[nodiscard]] constexpr int color_dist2(int r1,int g1,int b1,
                                        int r2,int g2,int b2) noexcept {
    int dr=r1-r2, dg=g1-g2, db=b1-b2;
    return dr*dr + dg*dg + db*db;
}

// Snap one channel to the nearest of the 6 cube levels (index 0..5).
[[nodiscard]] constexpr int cube_index(int v) noexcept {
    if (v < 48) return 0;
    if (v < 115) return 1;
    return (v - 35) / 40;   // 2..5
}

// RGB → nearest xterm-256 index, choosing between the color cube and the
// 24-step grayscale ramp by Euclidean distance.
[[nodiscard]] constexpr uint8_t rgb_to_xterm256(int r,int g,int b) noexcept {
    int ri=cube_index(r), gi=cube_index(g), bi=cube_index(b);
    int cr=kCubeLevels[ri], cg=kCubeLevels[gi], cb=kCubeLevels[bi];
    int cube = 16 + 36*ri + 6*gi + bi;
    int gray = (r*299 + g*587 + b*114) / 1000;             // Rec.601 luma
    int gidx = gray < 8 ? 0 : gray > 238 ? 23 : (gray - 3) / 10;
    int gv = 8 + 10*gidx;
    if (color_dist2(cr,cg,cb, r,g,b) <= color_dist2(gv,gv,gv, r,g,b))
        return static_cast<uint8_t>(cube);
    return static_cast<uint8_t>(232 + gidx);
}

// RGB → nearest of the 16 ANSI colors.
[[nodiscard]] constexpr uint8_t rgb_to_ansi16(int r,int g,int b) noexcept {
    int best=0, bestd=2147483647;
    for (int i=0;i<16;++i) {
        int d = color_dist2(kAnsi16[i][0],kAnsi16[i][1],kAnsi16[i][2], r,g,b);
        if (d < bestd) { bestd=d; best=i; }
    }
    return static_cast<uint8_t>(best);
}

struct Rgb3 { int r, g, b; };

// xterm-256 index → RGB, so an already-indexed color can be re-quantized to 16.
[[nodiscard]] constexpr Rgb3 xterm256_to_rgb(int i) noexcept {
    if (i < 16) return {kAnsi16[i][0], kAnsi16[i][1], kAnsi16[i][2]};
    if (i < 232) {
        int c = i - 16;
        return {kCubeLevels[(c/36)%6], kCubeLevels[(c/6)%6], kCubeLevels[c%6]};
    }
    int v = 8 + 10*(i - 232);
    return {v, v, v};
}

} // namespace detail

// ── Theme slots, as a colour ────────────────────────────────────────────────
//
// A Color can name a SEMANTIC SLOT instead of a literal hue — "accent",
// "muted", "error" — and be resolved against the theme in force when it is
// painted rather than when it is written.
//
// This is what lets a widget's Config carry a sensible default that still
// follows the user's theme. Before this, a field like
//
//     Color accent_color = Color::magenta();
//
// pinned that widget to magenta for the life of the process: it is a default
// on a struct, evaluated once, with no theme in scope to consult. Multiply by
// the ~540 such defaults across maya's widgets and "pick a theme" repainted
// only the handful of colours a host happened to pass explicitly — which is
// exactly the "some things change, most don't" symptom.
//
// Writing it as
//
//     Color accent_color = Color::slot(ThemeSlot::Accent);
//
// keeps the default meaningful, keeps it constexpr, and makes it a question
// answered at paint time. Hosts that pass an explicit colour still win — an
// override is a literal and literals are left alone.
enum class ThemeSlot : uint8_t {
    Primary, Secondary, Accent,
    Success, Error, Warning, Info,
    Text, InverseText, Muted,
    Surface, Background, Border,
    DiffAdded, DiffRemoved, DiffChanged,
    Highlight, Selection, Cursor, Link, Placeholder, Shadow, Overlay,
};

// ── The resolution index ────────────────────────────────────────────────────
//
// `Slot` is not a colour. It is a FREE VARIABLE standing for one, and the
// theme is the environment that substitutes for it. Keeping both in one
// type made every consumer handle a case it has no answer for, and C++ let
// each pick a different wrong answer silently:
//
//     to_rgb()    on a slot -> white        (the welcome-screen dark slab)
//     put_color() on a slot -> zero bytes   (grid-stream desync)
//     degrade()   on a slot -> passthrough  (a slot index as a colour channel)
//
// So the resolution state is lifted into the TYPE. `Res` indexes the colour
// by whether a slot can still be hiding inside it:
//
//     Color    = BasicColor<Res::Sym>   what widget configs and hosts hold
//     LitColor = BasicColor<Res::Lit>   what the renderer is allowed to eat
//
// Three rules, all enforced by the compiler rather than by review:
//
//   1. slot() exists ONLY on Sym.       A literal cannot name a variable.
//   2. channels exist ONLY on Lit.      A variable has no channels to read.
//   3. Lit converts to Sym implicitly, Sym to Lit NEVER.
//
// Rule 3 is the interesting one: Lit is a SUBTYPE of Sym (every literal is a
// valid symbolic colour), so widening is free and every existing call site
// keeps compiling. The only way back down is Theme::resolve(), which is
// therefore the single introduction rule for a paintable colour — and, being
// the only one, cannot be forgotten.
enum class Res : bool { Sym, Lit };

// The colour's shape. Namespace-scope rather than nested in BasicColor: the
// kind is a property of the COLOUR, not of its resolution state, and nesting
// it would give Color::Kind::Rgb and LitColor::Kind::Rgb as two unrelated
// types that will not compare.
//
// `Unset` is the DEFAULT, and it is what a value-initialized colour is. It
// exists so "nobody filled this in" is a state the type can tell you about,
// instead of a state that looks exactly like a deliberate choice.
//
// Before it, BasicColor() was Named(7) — white. That is a real colour a
// scheme might mean, so a FORGOTTEN Theme field and an intentional white
// were the same bits, and nothing could distinguish them. Add a slot to
// Theme and all 57 designated-initializer schemes silently acquire a
// hardcoded white for it: unreadable on a light scheme, and invisible to
// every test, because the value is well-formed. Unset makes that
// omission nameable, which is what lets Theme::complete() reject it at
// compile time.
enum class ColorKind : uint8_t { Unset, Named, Indexed, Rgb, Default, Slot };

template <Res R> class BasicColor;
using Color    = BasicColor<Res::Sym>;
using LitColor = BasicColor<Res::Lit>;

template <Res R>
class BasicColor {
public:
    using Kind = ColorKind;   // compatibility spelling: Color::Kind::Rgb

private:
    Kind    kind_;
    uint8_t r_, g_, b_;  // For Rgb; r_ doubles as index for Named/Indexed

    constexpr BasicColor(Kind k, uint8_t r, uint8_t g, uint8_t b) noexcept
        : kind_(k), r_(r), g_(g), b_(b) {}

    // The two indices are the same bits; each may read the other's.
    template <Res> friend class BasicColor;

public:
    // Constructors - all constexpr
    //
    // Value-initializes to UNSET, not to white. See ColorKind::Unset: a
    // default-constructed colour is "nobody said", and saying so is what
    // makes a forgotten Theme slot a compile error rather than a plausible
    // wrong colour. Anything that paints treats Unset as the terminal's own
    // ink (the same answer Default gives), so the failure mode is inherit,
    // never a colour maya invented.
    constexpr BasicColor() noexcept : kind_(Kind::Unset), r_(0), g_(0), b_(0) {}

    /// Was this colour ever given a value?
    [[nodiscard]] constexpr bool is_set() const noexcept { return kind_ != Kind::Unset; }

    constexpr explicit BasicColor(AnsiColor c) noexcept
        : kind_(Kind::Named), r_(static_cast<uint8_t>(c)), g_(0), b_(0) {}

    constexpr BasicColor(const BasicColor&) noexcept = default;
    constexpr BasicColor& operator=(const BasicColor&) noexcept = default;

    /// Widening, Lit -> Sym. A template so it never displaces the copy
    /// constructor (as a plain converting ctor would, making `Lit x = lit;`
    /// itself ill-formed — a trap worth naming, since it costs an afternoon).
    template <Res S> requires (S == Res::Lit && R == Res::Sym)
    constexpr BasicColor(const BasicColor<S>& o) noexcept
        : kind_(o.kind_), r_(o.r_), g_(o.g_), b_(o.b_) {}

    // Named color factories.
    //
    // Every one of these is LITERAL by construction, so they return LitColor
    // whichever index they are named through — `Color::red()` still works and
    // still widens for free. That is what keeps ~1000 existing call sites
    // compiling untouched while the renderer gets a type it can trust.
    //
    // `auto`, NOT `LitColor`, and that is load-bearing rather than style.
    // LitColor IS BasicColor<Res::Lit>, so naming it in a signature inside
    // BasicColor's own definition asks for the type to be complete while it
    // is still being defined — which is exactly what it is not, when R is
    // Lit. GCC and Clang accept it; MSVC rejects it, and did:
    //
    //   error C7637: maya::BasicColor<maya::Res::Lit>: you cannot implicitly
    //   instantiate a class template while it is being defined
    //
    // A deduced return type is resolved at INSTANTIATION, by which point the
    // class is complete, so the self-reference never arises. Same type, same
    // constexpr-ness, no forward-declaration dance.
    static constexpr auto black()          noexcept { return LitColor{AnsiColor::Black}; }
    static constexpr auto red()            noexcept { return LitColor{AnsiColor::Red}; }
    static constexpr auto green()          noexcept { return LitColor{AnsiColor::Green}; }
    static constexpr auto yellow()         noexcept { return LitColor{AnsiColor::Yellow}; }
    static constexpr auto blue()           noexcept { return LitColor{AnsiColor::Blue}; }
    static constexpr auto magenta()        noexcept { return LitColor{AnsiColor::Magenta}; }
    static constexpr auto cyan()           noexcept { return LitColor{AnsiColor::Cyan}; }
    static constexpr auto white()          noexcept { return LitColor{AnsiColor::White}; }
    static constexpr auto bright_black()   noexcept { return LitColor{AnsiColor::BrightBlack}; }
    static constexpr auto bright_red()     noexcept { return LitColor{AnsiColor::BrightRed}; }
    static constexpr auto bright_green()   noexcept { return LitColor{AnsiColor::BrightGreen}; }
    static constexpr auto bright_yellow()  noexcept { return LitColor{AnsiColor::BrightYellow}; }
    static constexpr auto bright_blue()    noexcept { return LitColor{AnsiColor::BrightBlue}; }
    static constexpr auto bright_magenta() noexcept { return LitColor{AnsiColor::BrightMagenta}; }
    static constexpr auto bright_cyan()    noexcept { return LitColor{AnsiColor::BrightCyan}; }
    static constexpr auto bright_white()   noexcept { return LitColor{AnsiColor::BrightWhite}; }
    static constexpr auto gray()           noexcept { return bright_black(); }
    static constexpr auto grey()           noexcept { return bright_black(); }

    /// Terminal-default color (SGR 39 fg / 49 bg). Use this when you want a
    /// container to occlude underlying cells in a zstack while still showing
    /// the user's terminal theme background.
    static constexpr auto default_color() noexcept {
        return LitColor{LitColor::Kind::Default, 0, 0, 0};
    }

    // 256-color palette
    static constexpr auto indexed(uint8_t index) noexcept {
        return LitColor{LitColor::Kind::Indexed, index, 0, 0};
    }

    /// A semantic theme slot, resolved at PAINT time against the theme in
    /// force. See ThemeSlot above for why widget Config defaults use this
    /// instead of a literal.
    ///
    /// Sym-only. A LitColor is by definition one no slot can hide inside, so
    /// `LitColor::slot(...)` is a compile error rather than a colour that
    /// silently paints as white somewhere downstream.
    static constexpr auto slot(ThemeSlot s) noexcept requires (R == Res::Sym) {
        return Color{Color::Kind::Slot, static_cast<uint8_t>(s), 0, 0};
    }

    [[nodiscard]] constexpr ThemeSlot theme_slot() const noexcept {
        return static_cast<ThemeSlot>(r_);
    }

    /// Narrowing, Sym -> Lit, CHECKED.
    ///
    /// The only way down the subtype relation, and it is total: a slot has no
    /// literal meaning, so it yields nullopt rather than a plausible wrong
    /// colour. Theme::resolve() is the one caller that matters — it handles
    /// the slot case itself and uses this for the pass-through — but it is
    /// public because a host that has just tested `kind() != Slot` deserves a
    /// way to say so that does not involve a cast.
    ///
    /// Named `try_` because the failure is a real case, not a contract
    /// violation: asking a variable for its value is a fair question with the
    /// honest answer "not yet".
    ///
    /// Unset narrows fine, and deliberately: "nobody stated a colour" IS a
    /// paintable answer (inherit, SGR 39/49), unlike a slot, which is a
    /// question the theme has to answer first.
    ///
    /// `auto` for the same reason as the factories above, and this one is the
    /// harder case: `std::optional<LitColor>` does not merely NAME the
    /// incomplete type, it instantiates a template over it, so MSVC needs the
    /// full definition to compute optional's storage and triviality traits.
    /// That is why the earlier pass over the factories was not enough -- it
    /// removed the mentions that were easy to see and left the two that
    /// actually reach into <optional> and <xsmf_control>.
    static constexpr auto try_literal(Color c) noexcept
        requires (R == Res::Lit)
    {
        if (c.kind_ == ColorKind::Slot) return std::optional<LitColor>{};
        return std::optional<LitColor>{LitColor{c.kind_, c.r_, c.g_, c.b_}};
    }

    // 24-bit truecolor
    static constexpr auto rgb(uint8_t r, uint8_t g, uint8_t b) noexcept {
        return LitColor{LitColor::Kind::Rgb, r, g, b};
    }

    // From hex literal: Color::hex(0xFF00FF)
    //
    // `auto` rather than LitColor: same self-reference as above.
    static consteval auto hex(uint32_t rgb) {
        if (rgb > 0xFFFFFF) throw "Color::hex: value exceeds 0xFFFFFF";
        return BasicColor::rgb(
            static_cast<uint8_t>((rgb >> 16) & 0xFF),
            static_cast<uint8_t>((rgb >> 8) & 0xFF),
            static_cast<uint8_t>(rgb & 0xFF)
        );
    }

    // HSL to RGB conversion (constexpr)
    static constexpr auto hsl(float h, float s, float l) noexcept {
        // Normalize h to [0, 360)
        while (h < 0) h += 360;
        while (h >= 360) h -= 360;
        s = std::clamp(s, 0.0f, 1.0f);
        l = std::clamp(l, 0.0f, 1.0f);

        auto hue2rgb = [](float p, float q, float t) -> float {
            if (t < 0) t += 1;
            if (t > 1) t -= 1;
            if (t < 1.0f/6) return p + (q - p) * 6 * t;
            if (t < 1.0f/2) return q;
            if (t < 2.0f/3) return p + (q - p) * (2.0f/3 - t) * 6;
            return p;
        };

        float r, g, b;
        if (s == 0) {
            r = g = b = l;
        } else {
            float q = l < 0.5f ? l * (1 + s) : l + s - l * s;
            float p = 2 * l - q;
            float hn = h / 360.0f;
            r = hue2rgb(p, q, hn + 1.0f/3);
            g = hue2rgb(p, q, hn);
            b = hue2rgb(p, q, hn - 1.0f/3);
        }
        return BasicColor::rgb(
            static_cast<uint8_t>(r * 255 + 0.5f),
            static_cast<uint8_t>(g * 255 + 0.5f),
            static_cast<uint8_t>(b * 255 + 0.5f)
        );
    }

    // Accessors
    [[nodiscard]] constexpr Kind kind() const noexcept { return kind_; }

    /// Channel / palette-index reads — Lit only.
    ///
    /// On a slot these bytes are the ThemeSlot enum, not a colour: reading
    /// them is how a slot index used to reach a terminal as a red channel.
    /// Resolve first and the question is well-posed; until then it is not a
    /// question this type can answer, so it does not offer to.
    ///
    /// RESOLVING IS NOT ENOUGH. A LitColor is paintable, not necessarily
    /// NUMERIC: on Named/Indexed these bytes are still a palette index, and
    /// on Default/Unset they are nothing at all. `theme::native` states its
    /// slots as Named and Default precisely so the user's own palette shows
    /// through — so under native, every one of these reads is a lie:
    ///
    ///     bright_black -> Named(8) -> r()=8, g()=0, b()=0 -> rgb(8,0,0)
    ///
    /// which is the invisible-reasoning-block bug (agentty #45): a palette
    /// index painted as a near-black truecolor triple. Ask has_channels()
    /// before doing ARITHMETIC on these, or go through to_rgb(), which maps
    /// the palette properly. Reading them to EMIT a palette index is fine
    /// and correct — that is what index() is for; prefer it there so the
    /// two meanings are never spelled the same way.
    [[nodiscard]] constexpr uint8_t r() const noexcept requires (R == Res::Lit) { return r_; }
    [[nodiscard]] constexpr uint8_t g() const noexcept requires (R == Res::Lit) { return g_; }
    [[nodiscard]] constexpr uint8_t b() const noexcept requires (R == Res::Lit) { return b_; }
    [[nodiscard]] constexpr uint8_t index() const noexcept requires (R == Res::Lit) { return r_; }

    /// True when r()/g()/b() are CHANNELS rather than a palette index or
    /// nothing — i.e. when arithmetic on this colour is meaningful.
    ///
    /// This is the guard every blend must apply. darken()/lighten() have
    /// always had it inline (`if (kind_ != Kind::Rgb) return *this`); naming
    /// it is what let lerp(), the springs and the gradients adopt the same
    /// rule instead of each reinventing the hazard.
    [[nodiscard]] constexpr bool has_channels() const noexcept {
        return kind_ == Kind::Rgb;
    }

    /// Raw payload bytes, index-independent.
    ///
    /// NOT channels: for Named/Indexed this is a palette index, and for a
    /// slot it is the ThemeSlot enum. Reading them as colour is the bug this
    /// file's type split exists to prevent, so they are deliberately ugly to
    /// spell and exist for exactly two jobs:
    ///
    ///   - HASHING a style for cache identity, where the question is "are
    ///     these the same authored value", not "what colour is this". A hash
    ///     that resolved first would collide two different slots that happen
    ///     to share a hue under today's theme.
    ///   - SERIALIZING the value itself.
    ///
    /// If you want to paint it, resolve() instead.
    [[nodiscard]] constexpr uint8_t raw_r() const noexcept { return r_; }
    [[nodiscard]] constexpr uint8_t raw_g() const noexcept { return g_; }
    [[nodiscard]] constexpr uint8_t raw_b() const noexcept { return b_; }

    // Downgrade this color to what a terminal of the given capability `level`
    // can actually display:  3 = truecolor (unchanged), 2 = 256-color,
    // 1 = 16-color.  RGB and 256-indexed colors are mapped to the nearest
    // representable color so apps look right on terminals without 24-bit
    // support — most importantly macOS Terminal.app, which is 256-color only
    // and silently drops `38;2` truecolor escapes. Named/Default colors and
    // anything already within the terminal's range pass through untouched.
    // Color is never stripped here; honoring "no color at all" is the
    // caller's job (it simply omits the SGR).
    //
    // Lit only. The switch below has no Slot case and needs none: on this
    // index the kind cannot be Slot, so -Wswitch proves the function total
    // instead of a `return *this` fallthrough quietly shipping a slot index
    // onward as though it were a colour.
    [[nodiscard]] constexpr LitColor degrade(int level) const noexcept
        requires (R == Res::Lit)
    {
        if (level >= 3) return *this;
        switch (kind_) {
            case Kind::Unset:
            case Kind::Default:
            case Kind::Named:
                return *this;
            case Kind::Indexed: {
                if (level >= 2) return *this;
                detail::Rgb3 c = detail::xterm256_to_rgb(r_);
                return LitColor{static_cast<AnsiColor>(
                    color::nearest_16(c.r, c.g, c.b))};
            }
            case Kind::Rgb:
                if (level >= 2)
                    return BasicColor::indexed(color::nearest_256(r_, g_, b_));
                return LitColor{static_cast<AnsiColor>(
                    color::nearest_16(r_, g_, b_))};
            case Kind::Slot:
                break;  // unreachable on Lit; keeps the switch exhaustive
        }
        return *this;
    }

    // Generate foreground SGR codes
    [[nodiscard]] std::string fg_sgr() const requires (R == Res::Lit) {
        switch (kind_) {
            case Kind::Named: {
                int code = r_ < 8 ? 30 + r_ : 90 + (r_ - 8);
                return std::to_string(code);
            }
            case Kind::Indexed:
                return "38;5;" + std::to_string(r_);
            case Kind::Rgb:
                return "38;2;" + std::to_string(r_) + ";" +
                       std::to_string(g_) + ";" + std::to_string(b_);
            // Unset paints as inherit: nobody stated a colour, so the
            // terminal's own ink is the only honest answer.
            case Kind::Unset:
            case Kind::Default:
                return "39";
            case Kind::Slot:
                break;  // unreachable on Lit
        }
        __builtin_unreachable();
    }

    // Generate background SGR codes
    [[nodiscard]] std::string bg_sgr() const requires (R == Res::Lit) {
        switch (kind_) {
            case Kind::Named: {
                int code = r_ < 8 ? 40 + r_ : 100 + (r_ - 8);
                return std::to_string(code);
            }
            case Kind::Indexed:
                return "48;5;" + std::to_string(r_);
            case Kind::Rgb:
                return "48;2;" + std::to_string(r_) + ";" +
                       std::to_string(g_) + ";" + std::to_string(b_);
            case Kind::Unset:
            case Kind::Default:
                return "49";
            case Kind::Slot:
                break;  // unreachable on Lit
        }
        __builtin_unreachable();
    }

    // -------------------------------------------------------------------------
    // Zero-allocation SGR emitters — write directly into an existing string.
    // Avoids any heap allocation on the hot rendering path.
    // -------------------------------------------------------------------------

    void append_fg_sgr(std::string& out) const requires (R == Res::Lit) {
        char buf[16];
        switch (kind_) {
            case Kind::Named: {
                int code = r_ < 8 ? 30 + r_ : 90 + (r_ - 8);
                auto [p, _] = std::to_chars(buf, buf + sizeof(buf), code);
                out.append(buf, p);
                break;
            }
            case Kind::Indexed: {
                out += "38;5;";
                auto [p, _] = std::to_chars(buf, buf + sizeof(buf), static_cast<int>(r_));
                out.append(buf, p);
                break;
            }
            case Kind::Rgb: {
                out += "38;2;";
                auto [p1, _1] = std::to_chars(buf, buf + sizeof(buf), static_cast<int>(r_));
                out.append(buf, p1); out += ';';
                auto [p2, _2] = std::to_chars(buf, buf + sizeof(buf), static_cast<int>(g_));
                out.append(buf, p2); out += ';';
                auto [p3, _3] = std::to_chars(buf, buf + sizeof(buf), static_cast<int>(b_));
                out.append(buf, p3);
                break;
            }
            case Kind::Unset:
            case Kind::Default:
                out += "39";
                break;
        }
    }

    void append_bg_sgr(std::string& out) const requires (R == Res::Lit) {
        char buf[16];
        switch (kind_) {
            case Kind::Named: {
                int code = r_ < 8 ? 40 + r_ : 100 + (r_ - 8);
                auto [p, _] = std::to_chars(buf, buf + sizeof(buf), code);
                out.append(buf, p);
                break;
            }
            case Kind::Indexed: {
                out += "48;5;";
                auto [p, _] = std::to_chars(buf, buf + sizeof(buf), static_cast<int>(r_));
                out.append(buf, p);
                break;
            }
            case Kind::Rgb: {
                out += "48;2;";
                auto [p1, _1] = std::to_chars(buf, buf + sizeof(buf), static_cast<int>(r_));
                out.append(buf, p1); out += ';';
                auto [p2, _2] = std::to_chars(buf, buf + sizeof(buf), static_cast<int>(g_));
                out.append(buf, p2); out += ';';
                auto [p3, _3] = std::to_chars(buf, buf + sizeof(buf), static_cast<int>(b_));
                out.append(buf, p3);
                break;
            }
            case Kind::Unset:
            case Kind::Default:
                out += "49";
                break;
        }
    }

    // Lighten/darken (returns new color)
    [[nodiscard]] constexpr LitColor lighten(float amount) const noexcept
        requires (R == Res::Lit)
    {
        if (kind_ != Kind::Rgb) return *this;
        auto lift = [amount](uint8_t c) -> uint8_t {
            float v = static_cast<float>(c) / 255.0f;
            v += (1.0f - v) * amount;
            return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255);
        };
        return BasicColor::rgb(lift(r_), lift(g_), lift(b_));
    }

    [[nodiscard]] constexpr LitColor darken(float amount) const noexcept
        requires (R == Res::Lit)
    {
        if (kind_ != Kind::Rgb) return *this;
        auto drop = [amount](uint8_t c) -> uint8_t {
            float v = static_cast<float>(c) / 255.0f;
            v *= (1.0f - amount);
            return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255);
        };
        return BasicColor::rgb(drop(r_), drop(g_), drop(b_));
    }

    // Project to concrete Rgb channels. Named goes through the ANSI-16
    // palette, Indexed through the xterm-256 table, Rgb passes through, and
    // Default falls back to white ("assume light ink" — the terminal's real
    // foreground is not knowable from here).
    //
    // Lit only, and that is the whole point: this used to answer `white` for
    // a slot, because a slot's enum lives in the red byte and reading it as
    // an ANSI index returns a plausible, wrong colour. That sentinel shipped
    // as the welcome-screen dark slab. There is nothing to apologise for now
    // — a slot cannot reach this function, so the case is gone rather than
    // guessed at, and the theme-discipline regex that policed it is obsolete.
    [[nodiscard]] constexpr LitColor to_rgb() const noexcept
        requires (R == Res::Lit)
    {
        switch (kind_) {
            case Kind::Rgb:
                return *this;
            case Kind::Named: {
                const auto& c = detail::kAnsi16[r_ & 15];
                return BasicColor::rgb(static_cast<uint8_t>(c[0]),
                                       static_cast<uint8_t>(c[1]),
                                       static_cast<uint8_t>(c[2]));
            }
            case Kind::Indexed: {
                detail::Rgb3 c = detail::xterm256_to_rgb(r_);
                return BasicColor::rgb(static_cast<uint8_t>(c.r),
                                       static_cast<uint8_t>(c.g),
                                       static_cast<uint8_t>(c.b));
            }
            case Kind::Unset:
            case Kind::Default:
                return BasicColor::rgb(255, 255, 255);
            case Kind::Slot:
                break;  // unreachable on Lit
        }
        return *this;
    }

    constexpr auto operator<=>(const BasicColor&) const = default;

    /// Cross-index comparison. A literal and a symbolic colour holding the
    /// same bits ARE the same colour, and refusing to say so would just push
    /// callers back to hand-comparing bytes.
    template <Res S>
    [[nodiscard]] constexpr bool operator==(const BasicColor<S>& o) const noexcept {
        return kind_ == o.kind_ && r_ == o.r_ && g_ == o.g_ && b_ == o.b_;
    }
};

// Compile-time validation
static_assert(Color::hex(0xFF00FF).r() == 255);
static_assert(Color::hex(0xFF00FF).g() == 0);
static_assert(Color::hex(0xFF00FF).b() == 255);
static_assert(Color::red().kind() == Color::Kind::Named);
static_assert(Color::indexed(42).index() == 42);

// ── The resolution index, asserted ──────────────────────────────────────────
//
// The guarantees this file exists to provide, stated as code so they are
// checked on every build rather than believed. The negative half uses
// requires-expressions: each must be FALSE, i.e. the operation must not
// compile. Deleting a `requires (R == Res::Lit)` clause below breaks the
// build here, which is the point.

// Phantom: the index costs nothing at runtime.
static_assert(sizeof(Color) == sizeof(LitColor));
static_assert(sizeof(Color) == 4);
static_assert(std::is_trivially_copyable_v<Color>);
static_assert(std::is_trivially_copyable_v<LitColor>);

// Factories are literal by construction, whichever index names them.
static_assert(std::is_same_v<decltype(Color::rgb(1, 2, 3)), LitColor>);
static_assert(std::is_same_v<decltype(Color::red()), LitColor>);
static_assert(std::is_same_v<decltype(Color::default_color()), LitColor>);

// Lit <: Sym. Widening is implicit; narrowing does not exist.
static_assert(std::is_convertible_v<LitColor, Color>);
static_assert(!std::is_convertible_v<Color, LitColor>);

// A slot is nameable only on the symbolic index...
//
// These probes are written against a dependent `T` on purpose. A
// requires-expression naming a CONSTRAINED STATIC member of a concrete type
// is not a SFINAE context — GCC hard-errors instead of yielding false — so
// the probe has to be a template the compiler substitutes into.
template <class T>
concept CanNameSlot = requires { T::slot(ThemeSlot::Accent); };
template <class T>
concept CanReadChannels = requires (T c) { c.r(); };
template <class T>
concept CanProjectRgb = requires (T c) { c.to_rgb(); };
template <class T>
concept CanDegrade = requires (T c) { c.degrade(3); };
template <class T>
concept CanEmitSgr = requires (T c, std::string& s) { c.append_fg_sgr(s); };

static_assert(CanNameSlot<Color>);
static_assert(!CanNameSlot<LitColor>);

// ...and channels are readable only on the literal one. These are exactly
// the operations that shipped as bugs when handed an unresolved slot.
static_assert(!CanReadChannels<Color>);
static_assert(!CanProjectRgb<Color>);
static_assert(!CanDegrade<Color>);
static_assert(!CanEmitSgr<Color>);
static_assert(CanReadChannels<LitColor>);
static_assert(CanProjectRgb<LitColor>);
static_assert(CanDegrade<LitColor>);
static_assert(CanEmitSgr<LitColor>);

// ============================================================================
// Themed — a colour a WIDGET is allowed to name
// ============================================================================
//
// The Res::Sym/Res::Lit split above makes one bug unrepresentable: painting
// an unresolved slot. This type makes the OTHER one unrepresentable:
// a widget stating its own palette.
//
// ── Why a second type ───────────────────────────────────────────────
//
// These are genuinely different failures and the guards for one are blind
// to the other:
//
//   CHANNEL bug   r()/g()/b() on a Named/Default colour. bright_black is
//                 Named(8), so the index lands in r_ and a blend paints
//                 rgb(8,0,0). Guarded by has_channels().
//
//   PALETTE bug   Color::rgb(12, 80, 38) as a diff band. That is a
//                 perfectly valid colour and a perfectly legal blend —
//                 has_channels() has nothing to say about it. It is wrong
//                 only because the THEME should have chosen it.
//
// agentty #45 was the first; the second then recurred three times in a
// row, in the reveal animation, the diff bands and the status banner,
// each found only after someone looked. Every occurrence is invisible to
// its author because developers run a scheme — under a scheme a literal
// looks fine, and only theme::native (which states Named/Default so the
// user's own palette reaches the screen) exposes it.
//
// ── The rule ───────────────────────────────────────────────────────
//
// A widget names a ROLE. Themed converts implicitly from ThemeSlot, so the
// common case reads as it always did:
//
//     Themed accent = ThemeSlot::Accent;      // fine
//
// and a literal is a COMPILE ERROR rather than a code review someone has
// to remember to do:
//
//     Themed accent = Color::rgb(12, 80, 38); // does not compile
//
// ── The escape hatch ─────────────────────────────────────────────
//
// Some literals are RIGHT. A language brand colour (Rust orange), a named
// syntax deck, a colour PICKER showing raw swatches — these identify
// something that is not a UI role, and resolving them through the theme
// would be the bug. brand() admits them, but it takes the reason as an
// argument, so the exemption is a sentence in the source rather than a
// silent call. It is also trivially greppable, which an allowlist of file
// paths in a test is not.
// Opt-in trait for a HOST's semantic colour token (see Themed's converting
// constructor). Specialise to std::true_type for a type that reads the live
// theme; everything else stays unconvertible, which is the point.
template <class T>
struct is_theme_token : std::false_type {};

// Does `T` convert to a colour during CONSTANT EVALUATION?
//
// This is the decidable half of "is a real theme token". A token reaches
// the live theme through a runtime indirection, so a constant-expression
// conversion of one is not a constant expression and this is false. A
// struct holding a baked-in literal folds to a fixed colour and this is
// true — which is exactly the impostor the token opt-in must not admit.
//
// The probe has to be a template the compiler substitutes into: naming a
// constrained static member of a concrete type is not a SFINAE context
// (GCC hard-errors instead of yielding false), the same reason the
// CanNameSlot family above is written against a dependent `T`.
//
// `static_cast<Color>` rather than a braced conversion so an explicit
// operator counts too — a token that spelled its conversion explicit would
// otherwise slip through as "does not fold" for the wrong reason.
template <class T>
concept ConstantFoldsToColor = requires {
    typename std::bool_constant<(static_cast<void>(static_cast<Color>(T{})), true)>;
};

// The gate is per-VALUE for a bare Color and per-TYPE for a host token, so
// both halves get a probe. A slot-valued Color is not constant-foldable to
// a FIXED colour in the sense that matters here — it folds, but to a slot,
// which resolve() later substitutes — so this concept is deliberately only
// asked about host tokens, never about Color itself.
static_assert(ConstantFoldsToColor<LitColor>,
              "a literal colour must be constant-foldable, or the impostor "
              "check below is vacuous and admits everything.");

class Themed {
    Color c_;

    // Unchecked construction, for the factories below that have already
    // justified their colour. A tag type rather than `explicit`, because
    // an explicit Themed(Color) would collide with the consteval gate.
    struct Unchecked {};
    constexpr Themed(Color c, Unchecked) noexcept : c_(c) {}

public:
    /// The normal path: name a role, get the theme's answer for it.
    constexpr Themed(ThemeSlot s) noexcept : c_(Color::slot(s)) {}

    /// A Color that is ALREADY a slot — `Color::slot(ThemeSlot::X)`, the
    /// spelling 500-odd existing sites use, and what internal helpers pass
    /// positionally into aggregates.
    ///
    /// consteval is what makes this safe: the argument must be a constant
    /// expression, so the kind is known at COMPILE time and a literal is
    /// rejected right here, in the constructor, with the message below.
    /// A runtime Color cannot reach it at all.
    consteval Themed(Color c) : c_(c) {
        if (c.kind() != ColorKind::Slot && c.kind() != ColorKind::Default)
            throw "a widget must not name a literal colour — use "
                  "Color::slot(ThemeSlot::X), or Themed::brand(c, \"why\") "
                  "if it is genuinely not a UI role (a language brand "
                  "colour, a syntax deck, a raw swatch). See agentty #45.";
    }

    /// Same gate for an already-resolved colour. `Color::default_color()`
    /// returns LitColor, and LitColor widens to Color implicitly, so
    /// without this overload the widening happens BEFORE the consteval
    /// context and the terminal default is rejected along with the
    /// literals it is not.
    consteval Themed(LitColor c) : c_(c) {
        if (c.kind() != ColorKind::Default)
            throw "a widget must not name a literal colour — use "
                  "Color::slot(ThemeSlot::X), or Themed::brand(c, \"why\") "
                  "if it is genuinely not a UI role (a language brand "
                  "colour, a syntax deck, a raw swatch). See agentty #45.";
    }

    /// The terminal's own ink/canvas (SGR 39/49). Always legitimate: it is
    /// the absence of a colour, not a choice of one.
    [[nodiscard]] static constexpr Themed terminal_default() noexcept {
        return Themed{Color::default_color(), Unchecked{}};
    }

    /// A literal that is NOT a UI role — a language brand colour, a syntax
    /// deck, a raw swatch in a colour picker. `why` is required and must be
    /// a string literal, so the exemption states itself:
    ///
    ///     Themed::brand(Color::hex(0xCE422B), "Rust brand orange")
    template <std::size_t N>
    [[nodiscard]] static constexpr Themed brand(Color c,
                                                const char (&why)[N]) noexcept {
        static_assert(N > 1, "brand() needs a reason, not an empty string");
        (void)why;
        return Themed{c, Unchecked{}};
    }

    /// A host's own semantic token — a type that READS the live theme
    /// rather than stating a colour (agentty's ui::fg, ui::muted, ...).
    ///
    /// Such a token is theme-correct by construction: it is a named field
    /// of the Theme, resolved on every read, so it tracks a theme switch.
    /// But it hands back an already-resolved LitColor, which is shaped
    /// exactly like a literal — so it cannot convert implicitly without
    /// reopening the hole this type closes.
    ///
    /// Opting in is therefore explicit and per-type: a host specialises
    /// maya::is_theme_token for its token.
    ///
    /// WHAT THE OPT-IN IS WORTH. The specialisation alone is only the
    /// host's word. It names a type, and nothing about naming a type says
    /// its conversion operator reads the live theme; a struct holding a
    /// baked-in literal could be specialised just as easily, and would walk
    /// a hardcoded colour straight through the gate that exists to stop it.
    /// This comment used to call the specialisation "a one-line assertion
    /// that the type reads the theme", which overclaimed: it asserts that
    /// the host SAID so.
    ///
    /// The second requirement below is the part that is actually checked.
    /// No trait can prove a type reads one particular global — but the
    /// property that matters is weaker and IS decidable: a real token
    /// cannot be constant-folded to a colour. It reaches the live theme
    /// through a runtime indirection (a function pointer, in every token
    /// written so far), so converting one in a constant-expression context
    /// does not compile. An impostor holding a literal folds fine, because
    /// a literal is exactly what constant folding is for.
    ///
    /// So `!ConstantFoldsToColor<Token>` rejects the impostor by the same
    /// mechanism the consteval constructors above use on a bare Color, and
    /// leaves genuine tokens untouched. Concretely: without it, an
    /// impostor's token constructor WINS overload resolution (it is an
    /// exact match, and `is_theme_token` said yes), so the literal never
    /// meets the consteval gate at all — verified by A/B'ing the
    /// constraint. With it, that overload is not viable, the conversion
    /// falls to `Themed(Color)`, and the literal throws there with the
    /// usual "a widget must not name a literal colour" diagnostic.
    ///
    /// It does not prove the token reads THE THEME rather than some other
    /// runtime state — that residue is the host's to own, and it is what
    /// test_style's "a theme token tracks a swap" case pins down
    /// behaviourally. Note a `requires`-expression cannot stand in for
    /// either check: it asks whether an expression is WELL-FORMED, and a
    /// consteval throw is well-formed until it is evaluated. The probes
    /// for this gate are therefore compile-fail tests, not static_asserts.
    template <class Token>
        requires is_theme_token<Token>::value
              && (!ConstantFoldsToColor<Token>)
    constexpr Themed(const Token& t) noexcept : c_(static_cast<Color>(t)) {}

    /// The colour, for the paint path. Still symbolic: resolving is the
    /// theme's job and happens at paint time, as before.
    [[nodiscard]] constexpr Color color() const noexcept { return c_; }
    constexpr operator Color() const noexcept { return c_; }

    [[nodiscard]] constexpr bool operator==(const Themed&) const = default;
};

// The contract, checked here rather than in a test because a test can be
// deleted and a header cannot be forgotten.
//
// Note what is and is not asserted. `Color` DOES convert — it must, because
// `Color::slot(ThemeSlot::X)` is the spelling 500-odd sites already use.
// The gate is not the type, it is the consteval constructor: a slot-valued
// Color passes, a literal one throws during constant evaluation, and a
// runtime Color cannot reach it at all. So the interesting property is
// per-VALUE, and the compiler checks it at every call site.
template <class T>
concept ConvertsToThemed = requires { Themed{std::declval<T>()}; };

static_assert(ConvertsToThemed<ThemeSlot>,
              "naming a role must be the path of least resistance");

// A slot-valued Color is accepted...
static_assert(Themed{Color::slot(ThemeSlot::Accent)}.color().kind()
                  == ColorKind::Slot);
static_assert(Themed{Color::default_color()}.color().kind()
                  == ColorKind::Default);

// ...and the escape hatch keeps working for colours that are genuinely not
// a UI role.
static_assert(Themed::brand(Color::hex(0xCE422B), "Rust brand orange")
                  .color().kind() == ColorKind::Rgb);

// A LITERAL is rejected — but per VALUE, not per type, and the difference
// matters. LitColor widens to Color implicitly (every literal IS a valid
// symbolic colour), so `ConvertsToThemed<LitColor>` is true at the type
// level and cannot be the check. The consteval constructor is: it sees the
// KIND at compile time and throws for anything that is not a slot or the
// terminal default.
//
// So the guarantee reads: `Themed x = Color::rgb(12, 80, 38);` does not
// compile — a compile error, not a code review someone has to remember to
// do. That is the palette bug (agentty #45 and its three recurrences) made
// unrepresentable. It cannot be asserted here, because a static_assert
// cannot require that an expression be ill-formed; it is pinned in
// tests/test_theme_discipline.cpp, which compiles the bad spelling in a
// requires-expression and asserts it fails to substitute.
static_assert(!std::is_nothrow_constructible_v<Themed, Color>,
              "the Color constructor must stay consteval-and-throwing — if "
              "it ever becomes noexcept, the literal gate has been removed");

// ── WHERE Themed BELONGS, and where it does not ──────────────────────────
//
// Themed gates CONSTANT colours. Its whole mechanism is a consteval
// constructor, so it can only judge a colour the compiler can already see:
// a Config field's default initialiser, a constexpr palette entry, a
// namespace-scope token. That is a real and useful population — it is where
// a hardcoded colour actually gets written, and agentty #45 was exactly
// such a default.
//
// It CANNOT gate a runtime colour, and the attempt was made and reverted
// (commit 4b78928, "revert the Themed migration in maya's own widgets").
// A blind migration of 533 Config fields broke 22 files in three shapes,
// all the same root cause — the value is not a constant expression:
//
//     void set_color(Color c) { color_ = c; }        // a caller's runtime value
//     cond ? Color::red() : themed_field             // ambiguous, both convert
//     examples/ naming literals directly             // what examples are FOR
//
// So the rule is not "Themed everywhere", it is: Themed where the colour is
// NAMED (a default, a palette, a token), Color where it is PASSED. maya's
// own widget internals are almost entirely the second kind, which is why
// this header has no Themed fields and the three widget files that mention
// it do so in comments explaining why they use Color instead.
//
// maya therefore ships Themed for HOSTS. agentty's palette.hpp is the
// intended shape: semantic tokens at namespace scope, a Themed-typed
// Config, and the gate catching a literal at the one place a literal would
// be written. The type earning its keep outside this repo is the design,
// not an adoption gap to close — and the static_assert below is what keeps
// a future reader from "fixing" that by re-running the migration.
//
// The two properties any re-migration would have to break:
//
//   1. The gate is consteval. A runtime Color must NOT be Themed-
//      constructible, or the gate has been widened into something that
//      accepts what it cannot judge — which is how it would silently start
//      passing everything.
//   2. A slot-valued constant must still pass, or the type has stopped
//      admitting the thing it exists to admit.
//
// If both of these hold, Themed still means what this comment says.
namespace themed_scope {

// A runtime Color is not a constant expression, so it cannot reach the
// consteval gate. `std::is_constructible_v` answers the WELL-FORMEDNESS
// question (true — the constructor exists and would be selected), which is
// deliberately not what we assert: a consteval constructor called on a
// runtime value is ill-formed at the CALL, and the honest instrument for
// that is a compile-fail probe, not a trait. See the probes in
// tests/test_theme_discipline.cpp.
//
// What IS assertable here is the shape the gate depends on.
static_assert(!std::is_nothrow_constructible_v<Themed, Color>,
              "Themed(Color) must stay consteval-and-throwing: it is the "
              "whole literal gate. If this fires, someone widened Themed to "
              "accept runtime colours — see commit 4b78928 for why that "
              "migration was tried and reverted.");

static_assert(ConvertsToThemed<decltype(Color::slot(ThemeSlot::Accent))>,
              "a slot-valued colour must remain Themed-constructible, or the "
              "type has stopped admitting what it exists to admit.");

// And the escape hatch must keep costing a sentence. brand() taking its
// reason as a runtime argument is what makes an exemption greppable prose
// at the site instead of a path in an allowlist somewhere else.
//
// Written against a dependent `C` for the reason the CanNameSlot family
// above is: a requires-expression naming a member of a CONCRETE type is not
// a SFINAE context, so `requires { Themed::brand(lit); }` hard-errors
// instead of yielding false. Substituting into a template makes it a
// deduction failure, which is the answer we want.
template <class C>
concept BrandNeedsNoReason = requires (C c) { Themed::brand(c); };
template <class C>
concept BrandTakesReason   = requires (C c) { Themed::brand(c, "a reason"); };

static_assert(BrandTakesReason<Color>,
              "brand() must keep admitting a literal WITH a reason — it is "
              "the sanctioned way to name a non-role colour.");

static_assert(!BrandNeedsNoReason<Color>,
              "brand() must keep REQUIRING the reason — an exemption that "
              "costs nothing to write is one nobody justifies.");

}  // namespace themed_scope

// ── Ink for a filled band ──────────────────────────────────────────
//
// "What text reads on top of THIS colour?"
//
// ── Why this cannot just measure ────────────────────────────────────
//
// For a real RGB band the answer is arithmetic: take the luminance, pick
// black or white. For a PALETTE band it is unknowable, and pretending
// otherwise is how the diff bands ended up unreadable.
//
// theme::native states its diff slots as ANSI green and red. The standard
// values for those are (0,128,0) and (128,0,0) — luminance 91 and 38, both
// dark, so measuring says "use white ink". But the standard values are not
// what the user sees. A Catppuccin-style palette maps green to #a6d189,
// luminance 194: light. White ink on it is invisible, which is exactly the
// bug this function was added to fix and then reproduced.
//
// We cannot query the palette. The terminal owns those 16 entries and does
// not tell us. So for a palette colour the honest answer is: don't answer.
// Ask the terminal to swap its own foreground and background instead —
// that is SGR 7, reverse video, and it has been in every terminal since
// the seventies precisely because only the terminal knows its own colours.
//
// ── The two answers ─────────────────────────────────────────────────
//
//   Rgb          measure it. We chose the colour, we know its channels.
//   Named/Indexed  reverse video. The terminal knows; we do not.
//   Default      no band at all, so nothing to do.
//
// on_band() returns a Style rather than a Color because the second answer
// is an ATTRIBUTE, not a colour — there is no LitColor that means "swap
// whatever you are about to paint".

// Luminance of a colour on 0..255, or -1 when it carries no channels we
// can trust. Named/Indexed deliberately return -1: the standard table is
// not what a remapped terminal will show.
[[nodiscard]] inline int band_luminance(LitColor c) noexcept {
    if (c.kind() != ColorKind::Rgb) return -1;
    return (2126 * c.r() + 7152 * c.g() + 722 * c.b()) / 10000;
}

// Ink for a band whose channels we KNOW. Only valid on Kind::Rgb — callers
// that may hand a palette colour want on_band() instead.
[[nodiscard]] inline LitColor ink_for(LitColor band) noexcept {
    const int l = band_luminance(band);
    if (l < 0) return LitColor::default_color();
    // Threshold at the midpoint. Note "bright" in ANSI means SATURATED,
    // not light — bright_blue (0,0,255) has luminance 18 — so this has to
    // measure rather than infer from the index.
    return l >= 128 ? LitColor::black() : LitColor::white();
}

// ── The other direction: a band the theme's OWN text reads on ──────────
//
// ink_for() asks "given this band, what text works". This asks the
// question a chip actually has: "I want to keep the theme's normal text
// colour — tint this hue until that text reads on it".
//
// That is the better shape for a label. Ink that changes colour per chip
// is a second thing to look at; ink that stays the prose colour makes the
// chip read as text that happens to sit on a tint, which is what a badge
// should be.
//
// The move is a blend toward the CANVAS rather than toward black or white.
// Blending toward the canvas keeps the hue recognisably itself (a magenta
// chip stays magenta) while pulling it to the side of the scale the text
// is not on — so the contrast comes from distance, not from a colour the
// theme never chose.
//
// Returns the band unchanged when it cannot measure: a palette colour is
// the terminal's to define, and inventing a tint for it is the guess this
// whole area keeps getting punished for.
[[nodiscard]] inline LitColor band_for(LitColor hue, LitColor ink,
                                       LitColor canvas) noexcept {
    // A PALETTE hue stays exactly as the user set it.
    //
    // Projecting it through the standard table and toning THAT was tried
    // and is worse: it throws away the colour the user actually chose (a
    // Catppuccin bright_magenta is #f2a4db, the standard one is #FF00FF)
    // and emits truecolor under a theme whose entire purpose is not to.
    //
    // Under native the right pair is the user's own: their palette entry as
    // the band, their own foreground as the ink. Both sides are theirs and
    // were chosen together, which is a better guarantee than anything we
    // can compute without being able to read the palette.
    if (hue.kind() != ColorKind::Rgb) return hue;

    const int li = band_luminance(ink);
    const int lc = band_luminance(canvas);
    if (li < 0) return hue;   // unmeasurable ink: nothing to tone against

    // Where the canvas sits decides which way to pull. With no canvas to
    // read, the ink tells us: light text implies a dark surface.
    const bool dark_ui = lc >= 0 ? (lc < 128) : (li >= 128);
    const LitColor toward = dark_ui ? LitColor::rgb(0, 0, 0)
                                    : LitColor::rgb(255, 255, 255);

    // Blend until the ink clears a comfortable margin. 96 on a 0..255
    // luminance scale is roughly a 4.5:1 contrast ratio for mid tones —
    // WCAG AA — without the cost of a full gamma-correct solve per frame.
    constexpr int kMargin = 96;
    LitColor out = hue;
    for (int step = 0; step < 8; ++step) {
        const int lo = band_luminance(out);
        if (lo < 0) return hue;
        const int gap = lo > li ? lo - li : li - lo;
        if (gap >= kMargin) break;
        // 25% toward the target per step: enough to converge inside the
        // loop bound, gentle enough that the hue survives.
        out = LitColor::rgb(
            static_cast<uint8_t>(out.r() + (toward.r() - out.r()) / 4),
            static_cast<uint8_t>(out.g() + (toward.g() - out.g()) / 4),
            static_cast<uint8_t>(out.b() + (toward.b() - out.b()) / 4));
    }
    return out;
}

// ── WCAG contrast ────────────────────────────────────────────────────────────────

// Relative luminance of an RGB colour (WCAG 2.1), -1 for palette colours.
[[nodiscard]] inline double rel_luminance(LitColor c) noexcept {
    if (c.kind() != ColorKind::Rgb) return -1;
    auto f = [](int v) {
        const double s = v / 255.0;
        return s <= 0.03928 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * f(c.r()) + 0.7152 * f(c.g()) + 0.0722 * f(c.b());
}

// Contrast ratio between two RGB colours, 0 when either is unmeasurable.
[[nodiscard]] inline double contrast_ratio(LitColor a, LitColor b) noexcept {
    const double la = rel_luminance(a), lb = rel_luminance(b);
    if (la < 0 || lb < 0) return 0;
    return la > lb ? (la + 0.05) / (lb + 0.05) : (lb + 0.05) / (la + 0.05);
}

// `fg` with its lightness moved away from `bg` until the pair reaches
// `target` contrast. Hue and saturation are kept, so a blue stays blue,
// just lighter (or darker on a light background). Unchanged when it already
// reads, or when either side is a palette colour we cannot measure.
[[nodiscard]] inline LitColor readable_on(LitColor fg, LitColor bg,
                                          double target) noexcept {
    const double lb = rel_luminance(bg);
    if (lb < 0 || fg.kind() != ColorKind::Rgb) return fg;
    if (contrast_ratio(fg, bg) >= target) return fg;

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
    auto to_rgb = [&](double L) {
        auto ch = [](double v) { return static_cast<uint8_t>(v * 255 + 0.5); };
        if (s == 0) return LitColor::rgb(ch(L), ch(L), ch(L));
        const double q = L < 0.5 ? L * (1 + s) : L + s - L * s;
        const double p = 2 * L - q;
        return LitColor::rgb(ch(hue(p, q, h + 1.0 / 3)), ch(hue(p, q, h)),
                             ch(hue(p, q, h - 1.0 / 3)));
    };
    const bool dark_bg = lb < 0.18;
    LitColor out = fg;
    for (int step = 0; step < 32; ++step) {
        l = dark_bg ? std::min(0.97, l + 0.02) : std::max(0.03, l - 0.02);
        out = to_rgb(l);
        if (contrast_ratio(out, bg) >= target) break;
        if (l <= 0.03 || l >= 0.97) break;
    }
    return out;
}

} // namespace maya
