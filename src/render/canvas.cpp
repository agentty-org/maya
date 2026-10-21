#include "maya/render/canvas.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string_view>

#include "maya/device/environment.hpp"
#include "maya/style/theme.hpp"

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace maya {

// ── OSC 8 link registry ─────────────────────────────────────────────────
namespace {
struct LinkRegistry {
    std::mutex mu;
    std::vector<std::string> uris{std::string{}};   // id 0: no link
    std::unordered_map<std::string, uint16_t> ids;
};
LinkRegistry& links() { static LinkRegistry r; return r; }
std::atomic<bool> g_links_used{false};
// what may go between ESC ] 8 ; ; and ST: printable ASCII, no ESC/BEL
bool uri_ok(std::string_view u) {
    if (u.empty() || u.size() > 2048) return false;
    for (unsigned char c : u) if (c < 0x21 || c > 0x7e) return false;
    return true;
}
bool link_registry_used() noexcept { return g_links_used.load(std::memory_order_relaxed); }
}  // namespace

bool links_used() noexcept { return link_registry_used(); }

uint16_t link_id(std::string_view uri) {
    if (!uri_ok(uri)) return 0;
    auto& r = links();
    std::lock_guard lk(r.mu);
    if (auto it = r.ids.find(std::string(uri)); it != r.ids.end()) return it->second;
    if (r.uris.size() >= 0xFFFF) return 0;
    const auto id = static_cast<uint16_t>(r.uris.size());
    r.uris.emplace_back(uri);
    r.ids.emplace(std::string(uri), id);
    g_links_used.store(true, std::memory_order_relaxed);
    return id;
}

const std::string& link_uri(uint16_t id) {
    auto& r = links();
    std::lock_guard lk(r.mu);
    return id < r.uris.size() ? r.uris[id] : r.uris[0];
}

namespace {

// Terminal color capability used to downgrade RGB / 256-color values to what
// the terminal can actually show (3 = truecolor, 2 = 256, 1 = 16, 0 = mono).
// Detected once, on first use.
//
// ONE detector, not two. This used to be its own env-sniffing routine that
// disagreed with theme::detect_tier() — the one the settings UI reports and
// the one that decides whether a named RGB scheme is usable. A UI that says
// "detected: truecolor" while the emit path quantises to 16 colours is worse
// than either answer alone, so both now ask the same function.
//
// MAYA_COLOR=truecolor|256|16|none|auto overrides it (handled inside
// detect_tier) for terminals we cannot sniff — truecolor passthrough inside
// tmux being the usual one — and for deterministic tests.
int detect_color_level() noexcept {
    // Non-TTY keeps truecolor rather than degrading: piped/captured output
    // is someone else's to interpret, and dropping to 16 colours would
    // silently rewrite bytes a test or a recorder is capturing verbatim.
    // (theme::detect_tier answers Mono for a pipe, which is the right
    // answer for "should I paint?" but not for "how precise are the bytes
    // if I do?" — the two questions differ only here.)
    const bool tty = env::is_tty();
    if (!tty) return 3;

    // Pass the REAL answer, not a hardcoded true.
    //
    // This used to say `/*tty=*/true` unconditionally, which was harmless
    // only while the branch above guaranteed it. It is a landmine either
    // way: detect_tier's first act is to decide whether anything should be
    // painted at all, and handing it a constant meant the emit path could
    // reach a different conclusion than the settings UI (which passes the
    // measured value). Two detectors for one fact is exactly what the
    // comment above this function says was already fixed once.
    switch (theme::detect_tier(tty)) {
        case theme::ColorTier::TrueColor: return 3;
        case theme::ColorTier::Ansi256:   return 2;
        case theme::ColorTier::Ansi16:    return 1;
        case theme::ColorTier::Mono:      return 0;
    }
    return 3;
}

int active_color_level() noexcept {
    static const int level = detect_color_level();
    return level;
}

} // namespace

int terminal_color_level() noexcept { return active_color_level(); }

// ============================================================================
// AlignedBuffer
// ============================================================================

AlignedBuffer::AlignedBuffer(std::size_t count, uint64_t fill_value)
    : size_(count)
    , capacity_(count)
{
    if (count > 0) {
        data_ = static_cast<uint64_t*>(
            ::operator new(count * sizeof(uint64_t), std::align_val_t{64}));
        std::fill(data_, data_ + count, fill_value);
    }
}

void AlignedBuffer::resize(std::size_t count, uint64_t fill_value) {
    if (count <= capacity_) {
        if (count > size_) std::fill(data_ + size_, data_ + count, fill_value);
        size_ = count;
        return;
    }
    // Geometric capacity growth. A tall streaming turn grows content_rows
    // by ~1 row EVERY frame, so a compose that resizes prev_cells_ /
    // the canvas to EXACTLY content_rows*width reallocated + full-copied
    // the entire multi-MB buffer every single frame — an O(content_rows)
    // memcpy that scaled linearly with the live turn (the residual `cf`
    // creep). Over-allocate to 1.5x the requested size (never below the
    // request) so a monotone grow reallocates O(log N) times instead of
    // O(N): the amortised per-frame copy drops to O(1). `size_` still
    // tracks the logical length exactly, so every reader (serialize,
    // the diff scan, content_height) is unaffected — only the physical
    // capacity carries slack.
    const std::size_t new_cap = std::max(count, capacity_ + capacity_ / 2);
    auto* new_data = static_cast<uint64_t*>(
        ::operator new(new_cap * sizeof(uint64_t), std::align_val_t{64}));
    if (data_) {
        std::copy(data_, data_ + size_, new_data);
        free();
    }
    std::fill(new_data + size_, new_data + count, fill_value);
    data_ = new_data;
    size_ = count;
    capacity_ = new_cap;
}

void AlignedBuffer::assign(std::size_t count, uint64_t value) {
    if (count > capacity_) {
        free();
        data_ = static_cast<uint64_t*>(
            ::operator new(count * sizeof(uint64_t), std::align_val_t{64}));
        capacity_ = count;
    }
    size_ = count;
    simd::streaming_fill(data_, count, value);
}

void AlignedBuffer::shrink_to_fit_if_oversized(std::size_t count) noexcept {
    // Only shrink when at most half the current capacity is needed.
    // This prevents thrashing when width oscillates within a small
    // band (e.g. a window-manager resize gesture); we only release
    // memory when a meaningful step-down has stabilised.
    if (capacity_ < 2 * count + 64) return;
    if (count == 0) {
        free();
        size_ = capacity_ = 0;
        return;
    }
    auto* new_data = static_cast<uint64_t*>(
        ::operator new(count * sizeof(uint64_t), std::align_val_t{64}));
    const std::size_t copy = std::min(size_, count);
    if (data_ && copy > 0) std::copy(data_, data_ + copy, new_data);
    if (data_) ::operator delete(data_, std::align_val_t{64});
    data_ = new_data;
    size_ = copy;
    capacity_ = count;
}

// ============================================================================
// StylePool
// ============================================================================

// Process-wide monotone counter for StylePool identity. Starts at 1 so
// `pool_id_ == 0` is a recognisable "uninitialised" sentinel for any
// thread-local cache that needs one.
namespace {
std::atomic<uint64_t> g_next_pool_id{1};
}

StylePool::StylePool() {
    pool_id_ = g_next_pool_id.fetch_add(1, std::memory_order_relaxed);
    styles_.reserve(64);
    sgr_cache_.reserve(64);
    looks_.reserve(64);
    styles_.emplace_back();  // ID 0 = default (empty) style
    sgr_cache_.push_back(build_sgr(styles_[0]));
    looks_.push_back(look_of(styles_[0]));
    grow(64);
    // Insert the default style into the map.
    insert_slot(hash_style(styles_[0]), 0);
    size_ = 1;
}

bool StylePool::retheme() {
    // Re-derive every cached SGR string under the theme now in force.
    //
    // build_sgr() bakes the canvas background into any style that does not
    // name one, so the cached bytes are theme-specific. Styles themselves
    // are unchanged — only their rendering is — so ids stay valid and no
    // canvas cell needs rewriting.
    //
    // But "no cell needs rewriting" is true of the CANVAS and false of the
    // WIRE. Both inline emitters diff packed (glyph, style_id) cells against
    // a shadow of the last frame; a retheme leaves every one of those pairs
    // identical while changing what the ids render as. The row compares
    // equal, nothing is emitted, and the terminal keeps showing the old
    // colours — worst on the background, because a bg-less style is exactly
    // the kind whose bytes this function rewrites. Hence the bool: callers
    // that own a wire shadow must drop it when this returns true.
    const Theme& now = theme::live();
    if (sgr_theme_valid_ && sgr_theme_ == now) return false;
    sgr_theme_ = now;
    sgr_theme_valid_ = true;
    for (std::size_t i = 0; i < styles_.size(); ++i) {
        sgr_cache_[i] = build_sgr(styles_[i]);
        looks_[i]     = look_of(styles_[i]);
    }
    // Bump pool_id_ so thread_local intern_const slots re-resolve.
    //
    // intern_const caches a style id per call site, keyed on pool_id_, and
    // skips the pool entirely on a hit. The id is still VALID here — the
    // style did not change — but the caller may have captured more than the
    // id: chrome built through that path kept rendering with the previous
    // theme's bytes. Invalidating is cheaper to reason about than auditing
    // every call site, and it costs one re-intern per site per theme swap.
    pool_id_ = g_next_pool_id.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void StylePool::clear() {
    // Bump pool_id_ so any thread_local intern_const cache that
    // recorded the old id sees a mismatch on next lookup and
    // re-interns. Without this, calling clear() leaves stale
    // intern_const cache slots pointing to ids that were just dropped.
    pool_id_ = g_next_pool_id.fetch_add(1, std::memory_order_relaxed);
    styles_.resize(1);
    sgr_cache_.resize(1);
    looks_.resize(1);
    caret_ids_.clear();
    size_ = 1;
    overflow_ = false;
    std::fill(slots_.begin(), slots_.end(), Slot{});
    insert_slot(hash_style(styles_[0]), 0);
}

char* StylePool::write_uint_sgr(char* p, unsigned n) noexcept {
    if (n < 10) {
        *p++ = static_cast<char>('0' + n);
    } else if (n < 100) {
        *p++ = static_cast<char>('0' + n / 10);
        *p++ = static_cast<char>('0' + n % 10);
    } else {
        *p++ = static_cast<char>('0' + n / 100);
        *p++ = static_cast<char>('0' + (n / 10) % 10);
        *p++ = static_cast<char>('0' + n % 10);
    }
    return p;
}

char* StylePool::append_color_sgr(char* p, const Color& in, bool is_fg) noexcept {
    // Resolve a semantic slot against the theme in force.
    //
    // This is THE point where a widget's `Color::slot(ThemeSlot::Accent)`
    // default becomes a real hue, and it is deliberately the last possible
    // moment: a Config default is written once, at static-init, with no
    // theme in scope, while this runs per span with the live theme
    // available. That is what lets ~540 widget defaults follow the user's
    // theme without any of them being rewritten per swap.
    //
    // Literals pass straight through, so an explicit host override always
    // beats a slot default.
    return append_resolved_sgr(p, theme::live().resolve(in), is_fg);
}

// Emit one already-resolved colour as SGR parameters.
//
// Split from append_color_sgr so a caller that has ALREADY resolved (the
// background diff below) does not resolve twice, and so the emit half states
// in its signature that resolution is somebody else's finished business.
char* StylePool::append_resolved_sgr(char* p, const LitColor& themed, bool is_fg) noexcept {
    const int level = active_color_level();

    // Level 0 is MONOCHROME — NO_COLOR, TERM=dumb, or an explicit
    // MAYA_COLOR=none. Emit the default-colour SGR (39/49) rather than
    // nothing at all: "nothing" would let whatever colour the previous span
    // set bleed into this one, so a monochrome request would still paint
    // colour. 39/49 is the terminal's own ink on its own background, which
    // is exactly what "no colour" has to mean for a user who asked for a
    // black-and-white terminal.
    if (level <= 0) return write_uint_sgr(p, is_fg ? 39u : 49u);

    // Downgrade RGB / 256-color to what the terminal can render before
    // emitting (macOS Terminal.app is 256-only and drops 38;2 truecolor).
    const LitColor c = themed.degrade(level);
    switch (c.kind()) {
        case ColorKind::Named: {
            int base = is_fg ? 30 : 40;
            int code = c.index() < 8 ? base + c.index()
                                     : (base + 60) + (c.index() - 8);
            return write_uint_sgr(p, static_cast<unsigned>(code));
        }
        case ColorKind::Indexed:
            if (is_fg) { *p++='3'; *p++='8'; } else { *p++='4'; *p++='8'; }
            *p++ = ';'; *p++ = '5'; *p++ = ';';
            return write_uint_sgr(p, c.index());
        case ColorKind::Rgb:
            if (is_fg) { *p++='3'; *p++='8'; } else { *p++='4'; *p++='8'; }
            *p++ = ';'; *p++ = '2'; *p++ = ';';
            p = write_uint_sgr(p, c.r()); *p++ = ';';
            p = write_uint_sgr(p, c.g()); *p++ = ';';
            return write_uint_sgr(p, c.b());
        case ColorKind::Unset:
        case ColorKind::Default:
            // Terminal default fg/bg — and the same answer for a colour
            // nobody set, which is the only honest one: inherit rather than
            // invent. Most call sites filter Default out before calling, so
            // this is defensive: emit the ANSI reset (39 = default fg,
            // 49 = default bg) rather than fall through to UB if that
            // invariant ever changes.
            return write_uint_sgr(p, is_fg ? 39u : 49u);
        case ColorKind::Slot:
            // Unreachable: `c` is a LitColor, so the compiler knows no slot
            // can be here. Listed so the switch stays exhaustive under
            // -Wswitch rather than relying on a default: label.
            break;
    }
    __builtin_unreachable();
}

void StylePool::write_transition_sgr(uint16_t prev_id, uint16_t new_id,
                                     std::string& out) const {
    if (prev_id == new_id) return;

    // Clamp out-of-range ids to the default style (id 0). A packed cell
    // carries its style id in bits 32-47; that id can outlive the pool
    // entry it referenced — e.g. clear() shrinks styles_ back to size 1
    // while a prev_cells buffer (inline diff) or a stale canvas cell still
    // holds a higher id from the previous frame. Indexing styles_[id] /
    // sgr_cache_[id] for such an id reads the vector's uninitialized
    // CAPACITY (reserve(64) leaves default-constructed-but-never-assigned
    // Style slots with garbage bool bytes → UBSan: "load of value 190,
    // which is not a valid value for type 'bool'"), or runs past the end
    // entirely. id 0 is always present (the default style) and is the
    // correct visual fallback for a dropped style. UINT16_MAX is the
    // "no SGR emitted yet" sentinel and is handled below, so exclude it
    // from the prev clamp.
    const auto n = static_cast<uint16_t>(styles_.size());
    if (new_id >= n) new_id = 0;
    if (prev_id != UINT16_MAX && prev_id >= n) prev_id = 0;

    // OSC 8: a link opens where its cells start and closes where they end.
    // Unknown state (frame start) closes first, in case one was left open.
    {
        const bool unknown = prev_id == UINT16_MAX;
        const uint16_t from_link = unknown ? 0 : styles_[prev_id].link;
        const uint16_t to_link = styles_[new_id].link;
        if (unknown && link_registry_used()) out.append("\x1b]8;;\x1b\\");
        if (from_link != to_link) {
            if (to_link) { out.append("\x1b]8;;"); out.append(link_uri(to_link)); out.append("\x1b\\"); }
            else out.append("\x1b]8;;\x1b\\");
        }
    }

    // Unknown terminal state (no SGR emitted yet this frame). Fall back
    // to the full reset-and-set form so the terminal lands at a known
    // configuration regardless of any residual SGR from before maya
    // took over the screen.
    if (prev_id == UINT16_MAX) {
        out.append(sgr_cache_[new_id]);
        return;
    }

    const Style& from = styles_[prev_id];
    const Style& to   = styles_[new_id];

    char buf[96];
    char* p = buf;
    *p++ = '\x1b'; *p++ = '[';
    bool first = true;
    auto sep = [&]() noexcept { if (!first) *p++ = ';'; first = false; };

    // Attribute toggles. SGR 22 = bold-off (per spec, also turns off
    // dim; we suppress dim emission entirely so the conflation is
    // moot here — see build_sgr for the SGR-2 suppression rationale).
    if (from.bold != to.bold) {
        sep();
        if (to.bold) *p++ = '1';
        else        { *p++ = '2'; *p++ = '2'; }
    }
    if (from.italic != to.italic) {
        sep();
        if (to.italic) *p++ = '3';
        else          { *p++ = '2'; *p++ = '3'; }
    }
    if (from.underline != to.underline) {
        sep();
        if (to.underline) *p++ = '4';
        else             { *p++ = '2'; *p++ = '4'; }
    }
    if (from.inverse != to.inverse) {
        sep();
        if (to.inverse) *p++ = '7';
        else           { *p++ = '2'; *p++ = '7'; }
    }
    if (from.strikethrough != to.strikethrough) {
        sep();
        if (to.strikethrough) *p++ = '9';
        else                 { *p++ = '2'; *p++ = '9'; }
    }

    // Color::Default is the "occlude but emit no color SGR" sentinel —
    // skip it as if absent. fg / bg are emitted only when the
    // effective-color state actually changes.
    //
    // Compared as the TERMINAL will see them: resolved against the theme and
    // degraded to its colour depth. On a 256-colour link (ssh, tmux) two
    // truecolor inks a few units apart land on the same palette index, and
    // comparing the RGBs emitted a `38;5;N` that repeated the pen exactly.
    // Measured on doom_fire at TERM=tmux-256color: 8% of all bytes on the
    // wire were such no-op SGRs.
    const int level = active_color_level();
    const Theme& th = theme::live();
    auto effective_fg = [&](const std::optional<Color>& c) -> std::optional<LitColor> {
        if (c.has_value() && c->kind() != Color::Kind::Default)
            return th.resolve(*c).degrade(level);
        return std::nullopt;
    };
    const std::optional<LitColor> from_fg = effective_fg(from.fg);
    const std::optional<LitColor> to_fg   = effective_fg(to.fg);
    if (from_fg != to_fg)
    {
        sep();
        if (to_fg) {
            p = append_resolved_sgr(p, *to_fg, /*is_fg=*/true);
        } else {
            // Explicit fg disabled → ANSI 39 (default fg).
            *p++ = '3'; *p++ = '9';
        }
    }

    // Background follows the same rule as build_sgr: a style with no bg
    // means "the canvas", not "reset to the terminal". Resolving it HERE
    // as well is what keeps the differential path honest — it is the other
    // half of the emit funnel, and a rule enforced in only one of the two
    // is a rule that holds until the diff picks the other branch.
    //
    // Kind::Default still means the deliberate terminal-background opt-out
    // and still emits 49, so transparency survives on both paths.
    const bool themed = theme::owns_canvas(th);
    // Resolved AND degraded, because the COMPARISON has to happen on the far
    // side of the theme and the colour depth: two slots that differ as
    // written may paint identically, a slot and a literal that look
    // different may be the same colour, and two RGBs may be one palette
    // index. nullopt means "terminal default" (emit 49).
    auto effective_bg = [&](const std::optional<Color>& c) -> std::optional<LitColor> {
        if (c.has_value()) {
            const LitColor lit = th.resolve(*c);
            if (lit.kind() == ColorKind::Default) return std::nullopt;
            return lit.degrade(level);
        }
        return themed ? std::optional<LitColor>{th.background.degrade(level)} : std::nullopt;
    };
    const std::optional<LitColor> from_bg = effective_bg(from.bg);
    const std::optional<LitColor> to_bg   = effective_bg(to.bg);
    if (from_bg != to_bg) {
        sep();
        if (to_bg) {
            p = append_resolved_sgr(p, *to_bg, /*is_fg=*/false);
        } else {
            *p++ = '4'; *p++ = '9';
        }
    }

    // No SGR-affecting changes (can happen when two distinct style IDs
    // differ only in `dim`, which we don't emit). Skip emission entirely
    // — emitting `\x1b[m` would mean SGR reset, the opposite of "no-op".
    if (first) return;

    *p++ = 'm';
    out.append(buf, static_cast<std::size_t>(p - buf));
}

// What a style paints as on this terminal: exactly the inputs build_sgr
// turns into bytes (so two styles with equal looks emit equivalent SGR),
// with colours resolved against the live theme and degraded to the
// terminal's depth. build_sgr never emits dim or conceal, so they're not
// part of the look either. Kept beside build_sgr so the two can't drift.
StylePool::Look StylePool::look_of(const Style& s) noexcept {
    const int level = active_color_level();
    const Theme& th = theme::live();
    Look l;
    l.attrs = static_cast<std::uint8_t>(
          (s.bold ? 1 : 0) | (s.italic ? 2 : 0) | (s.underline ? 4 : 0)
        | (s.inverse ? 8 : 0) | (s.strikethrough ? 16 : 0));
    if (level <= 0) return l;                 // monochrome: colours never reach the wire
    if (s.fg.has_value() && s.fg->kind() != Color::Kind::Default)
        l.fg = th.resolve(*s.fg).degrade(level);
    if (s.bg.has_value()) {
        if (s.bg->kind() != Color::Kind::Default)
            l.bg = th.resolve(*s.bg).degrade(level);
    } else if (theme::owns_canvas(th)) {
        l.bg = th.background.degrade(level);
    }
    return l;
}

std::string StylePool::build_sgr(const Style& s) {
    char buf[64];
    char* p = buf;
    *p++ = '\x1b'; *p++ = '['; *p++ = '0';

    if (s.bold)          { *p++ = ';'; *p++ = '1'; }
    // SGR 2 (faint) deliberately suppressed. Windows Terminal renders
    // SGR 2 by reducing brightness ~50%, which on already-dark colors
    // (bright_black, dim cyan, etc.) collapses below the readable
    // contrast floor — entire chrome elements vanish on the user's
    // theme. Other terminals (Alacritty, kitty, iTerm2, Ghostty)
    // honor SGR 2 with milder reductions but still measurably hurt
    // contrast on low-contrast themes (Solarized, Gruvbox-light). The
    // hierarchy effect "dim" was meant to provide is already carried
    // by color choice (bright_black for muted text); dropping the SGR
    // attribute keeps the same color and stops the disappearing-text
    // bug. `s.dim` is still honored in hashing/equality so the cache
    // doesn't alias dim and non-dim styles, but it never reaches the
    // wire.
    // if (s.dim)           { *p++ = ';'; *p++ = '2'; }
    if (s.italic)        { *p++ = ';'; *p++ = '3'; }
    if (s.underline)     { *p++ = ';'; *p++ = '4'; }
    if (s.inverse)       { *p++ = ';'; *p++ = '7'; }
    if (s.strikethrough) { *p++ = ';'; *p++ = '9'; }

    // Color::Default is a sentinel meaning "occlude this cell but emit no
    // explicit color SGR" — preserves terminal background transparency
    // (e.g. Ghostty) while still letting the cell overdraw underlying glyphs.
    if (s.fg.has_value() && s.fg->kind() != Color::Kind::Default) {
        *p++ = ';'; p = append_color_sgr(p, *s.fg, true);
    }
    // ── Background: the theme's canvas is the DEFAULT, not an opt-in ────
    //
    // THE structural fix for "some frame lines still show the terminal
    // through". Every styled cell in maya becomes bytes here and nowhere
    // else, so this is the one place the rule can be made unbreakable.
    //
    // A Style with no bg used to emit no background SGR, which a terminal
    // reads as "reset to MY default". On a themed canvas that is a hole.
    // Widgets produce bg-less styles constantly and legitimately — a border
    // glyph, a divider rule, a label — so the holes appeared wherever a
    // widget had no reason to name a colour. Fixing them one widget at a
    // time is endless, because the DEFAULT was wrong rather than any one
    // widget being wrong: the next bg-less style anyone writes brings the
    // bug straight back.
    //
    // Inverting the default ends the whole class. "No opinion" now means
    // "the canvas", which is what every one of those widgets meant. The two
    // ways to say something else both still work and both still win:
    //
    //   * an explicit colour   — selection strips, diff tints, chips
    //   * Color::Kind::Default — the deliberate "I want the terminal's own
    //                            background", which is how transparency
    //                            and background images survive
    //
    // Under `native` the theme states no background (owns_canvas is false),
    // so nothing is emitted and every cell falls through to the terminal —
    // byte-for-byte the old behaviour, which is the entire point of native.
    if (s.bg.has_value()) {
        if (s.bg->kind() != Color::Kind::Default) {
            *p++ = ';'; p = append_color_sgr(p, *s.bg, false);
        }
    } else if (const Theme& th = theme::live(); theme::owns_canvas(th)) {
        *p++ = ';'; p = append_color_sgr(p, th.background, false);
    }

    *p++ = 'm';
    return {buf, static_cast<std::size_t>(p - buf)};
}

std::size_t StylePool::hash_style(const Style& s) noexcept {
    // FNV-1a in 64 bits on every target, folded to size_t at the end: a
    // size_t accumulator truncated the offset basis on 32-bit hosts.
    uint64_t h = 14695981039346656037ULL;
    auto mix = [&](uint64_t v) {
        h ^= v;
        h *= 1099511628211ULL;
    };

    uint8_t flags = 0;
    if (s.bold)          flags |= (1 << 0);
    if (s.dim)           flags |= (1 << 1);
    if (s.italic)        flags |= (1 << 2);
    if (s.underline)     flags |= (1 << 3);
    if (s.strikethrough) flags |= (1 << 4);
    if (s.inverse)       flags |= (1 << 5);
    if (s.conceal)       flags |= (1 << 6);
    if (s.caret_anchor)  flags |= (1 << 7);
    mix(flags);
    if (s.caret_shape) mix(0xCA00u | s.caret_shape);
    if (s.link) mix(0x11C0000u | s.link);

    // Hash the AUTHORED value, not the painted one. A slot and the literal it
    // currently resolves to are different styles that must not share a cache
    // entry — they diverge the moment the theme changes. raw_* is the
    // index-independent spelling for exactly this.
    if (s.fg.has_value()) {
        mix(static_cast<uint64_t>(s.fg->kind()) << 24
          | static_cast<uint64_t>(s.fg->raw_r()) << 16
          | static_cast<uint64_t>(s.fg->raw_g()) << 8
          | static_cast<uint64_t>(s.fg->raw_b()));
    } else {
        mix(0xDEAD);
    }

    if (s.bg.has_value()) {
        mix(static_cast<uint64_t>(s.bg->kind()) << 24
          | static_cast<uint64_t>(s.bg->raw_r()) << 16
          | static_cast<uint64_t>(s.bg->raw_g()) << 8
          | static_cast<uint64_t>(s.bg->raw_b()));
    } else {
        mix(0xBEEF);
    }

    // Map hash 0 → 1 (reserve 0 as empty sentinel).
    std::size_t folded;
    if constexpr (sizeof(std::size_t) < sizeof(uint64_t))
        folded = static_cast<std::size_t>(h ^ (h >> 32));
    else
        folded = static_cast<std::size_t>(h);
    return folded == 0 ? 1 : folded;
}

void StylePool::insert_slot(std::size_t h, uint16_t id) {
    std::size_t idx = h & mask_;
    while (slots_[idx].hash != 0) {
        idx = (idx + 1) & mask_;
    }
    slots_[idx] = {h, id};
}

void StylePool::grow(std::size_t new_cap) {
    capacity_ = new_cap;
    mask_ = new_cap - 1;
    slots_.assign(new_cap, Slot{});
    // Re-insert all existing styles.
    for (uint16_t i = 0; i < static_cast<uint16_t>(styles_.size()); ++i) {
        insert_slot(hash_style(styles_[i]), i);
    }
}

// ============================================================================
// Canvas
// ============================================================================

Canvas::Canvas(int width, int height, StylePool* pool)
    // Clamp at the ONE constructor every canvas goes through. A negative
    // dimension reaching the resize below casts to a huge std::size_t and
    // throws bad_alloc from deep inside a render — a hard crash for what is
    // only ever a degenerate layout input (a terminal reporting 0 columns
    // during a resize race, a nested widget whose chrome exceeds its
    // parent's width, `render_to_string(el, -1)` in a caller's edge path).
    // Clamping to an empty 0-cell grid makes those render NOTHING, which
    // every consumer already handles (content_height() == 0), instead of
    // taking the process down. Rendering must degrade, never abort.
    : width_(width  > 0 ? width  : 0)
    , height_(height > 0 ? height : 0)
    , style_pool_(pool)
    , damage_{{Columns{0}, Rows{0}}, {Columns{width_}, Rows{height_}}}
{
    cells_.resize(static_cast<std::size_t>(width_)
                      * static_cast<std::size_t>(height_),
                  default_cell());
    last_col_.assign(static_cast<std::size_t>(height_), -1);
    row_epoch_.assign(static_cast<std::size_t>(height_), 0);

    // MAYA_STRICT_CLIP=1 — name every write the renderer throws away.
    //
    // The hook below is normally installed by a test, which is fine for
    // layouts a test happens to cover and useless for the one a user is
    // actually looking at. Clipping is silent by nature: the reader sees a
    // frame that looks tidy and is missing information, and cannot tell.
    // So the person who CAN see the broken frame needs a way to make it
    // speak without rebuilding anything.
    //
    //     MAYA_STRICT_CLIP=1 agentty 2>clip.log
    //
    // Every canvas in the process then reports to stderr, which turns
    // "the stats panel looks wrong at this size" into a line naming the
    // text, the column it started at, the width it wanted and the edge
    // that cut it. Installed here because this is the ONE constructor
    // every canvas goes through — including the ones the app rebuilds on
    // every resize, which is exactly when width bugs appear.
    //
    // Read once per process; off by default and zero-cost when off.
    static const bool strict = [] {
        const char* v = std::getenv("MAYA_STRICT_CLIP");
        return v != nullptr && *v != '\0' && *v != '0';
    }();
    if (__builtin_expect(strict, 0)) {
        clip_report_ = [](const ClipOverflow& o) {
            std::fprintf(stderr,
                         "[maya] clipped at (%d,%d): wanted %d cols, edge %d, "
                         "text '%.*s'\n",
                         o.x, o.y, o.wanted, o.edge,
                         static_cast<int>(o.text.size()), o.text.data());
        };
    }
}

std::uint64_t Canvas::next_canvas_uid() noexcept {
    static std::atomic<std::uint64_t> counter{1};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

Cell Canvas::get(int x, int y) const noexcept {
    if (!in_bounds(x, y)) return Cell{};
    return Cell::unpack(cells_[cell_index(x, y)]);
}

void Canvas::write_text(int x, int y, std::string_view text, uint16_t style_id) {
    if (__builtin_expect(!in_bounds(x, y), 0)) return;

    // Clipped row range. y is fixed for this call, so the y-bounds /
    // y-clip checks are hoisted out of the per-character loop.
    if (has_clip_ && (y < clip_y0_ || y >= clip_y1_)) return;

    // Per-row clipped x range. Hoisted, so the per-character branch
    // collapses to a single "are we past the right edge?" check.
    const int x_min = has_clip_ ? std::max(0, clip_x0_) : 0;
    const int x_max = has_clip_ ? std::min(width_, clip_x1_) : width_;

    // ── The overflow tripwire ──────────────────────────────────────────
    //
    // THIS is where a layout bug becomes invisible. The loop below drops
    // every cell past x_max without a word, and in a terminal the dropped
    // end is where values live — a meter's number, a table's count, the
    // right half of a figure. The reader sees a bar with nothing beside it
    // and cannot tell that anything is missing.
    //
    // Every width in this system is produced by subtraction, by a different
    // party, in a different file: terminal → border → padding → gutter →
    // label lane → gap → picture. Each step is locally reasonable and none
    // can be checked where it happens, so a wrong one is only discoverable
    // by looking at pixels and counting. That is how eight separate width
    // bugs shipped through a green suite in a single week; each "fix" was
    // global reasoning validated by eye, which is why fixing one width
    // broke another.
    //
    // Reporting here converts all of them into a message naming the text
    // that was cut and the column it was cut at, at the moment it happens.
    // It is the cheapest possible version of the real fix (widths that
    // cannot be fabricated, slots that divide rather than request) and it
    // subsumes most of the benefit: in a renderer that clips silently, the
    // absence of an error is not evidence of correctness.
    //
    // Off unless MAYA_STRICT_CLIP is set. Clipping is a legitimate, common
    // operation — a scroll viewport clips by design, and a deliberately
    // truncated label has already chosen its ellipsis — so this cannot be
    // an unconditional assert. It is a tool you switch on when a frame
    // looks wrong, and a gate CI can run with.
    if (__builtin_expect(clip_report_ != nullptr, 0)) {
        const int want = static_cast<int>(unicode::str_width(text));
        // BOTH edges. The right is the obvious one — a row that is too long
        // loses its tail. The left is the one that produced a screenshot
        // nobody could explain: content CENTRED in a slot narrower than
        // itself spills equally in both directions, so the first thing lost
        // is the start of the line, and what remains looks like a figure
        // with a bite taken out of its left side. Watching only x_max would
        // have reported that frame as clean.
        if (want > 0 && x < x_min)
            clip_report_(ClipOverflow{.x = x, .y = y,
                                      .wanted = want,
                                      .edge = x_min,
                                      .text = text});
        else if (want > 0 && x + want > x_max)
            clip_report_(ClipOverflow{.x = x, .y = y,
                                      .wanted = want,
                                      .edge = x_max,
                                      .text = text});
    }

    int cx = x;
    std::size_t pos = 0;
    const std::size_t len = text.size();
    const char* data = text.data();

    // Value-gated stamping (matches set()): only stamp the row if some
    // cell actually CHANGED. A label/header re-painting byte-identical
    // text every frame must not disarm the per-row blit skip of cached
    // content sharing this row. The non-ASCII slow path routes through
    // set(), which is itself value-gated.
    bool changed = false;

    // Highest x written this call — used to update last_col_/max_y_
    // once at the end instead of per-character. Starts at -1 so an
    // entirely-clipped or control-only call leaves bookkeeping intact.
    int highest_x = -1;
    bool any_visible = false;

    uint64_t* base = cells_.data();
    const std::size_t row_off = static_cast<std::size_t>(y) * static_cast<std::size_t>(width_);

    // ASCII fast path — batches the run of printable ASCII into a
    // straight write loop. The inner body is bounds-free (we clipped
    // x_max upfront) and goes straight to the cells_ buffer with a
    // pre-packed cell template; we just OR in the codepoint per byte.
    // For an 80-column ASCII run that's ~80 writes with no overhead
    // per cell beyond the codepoint write itself.
    while (pos < len) {
        // Scan run of printable ASCII bytes.
        while (pos < len) {
            auto byte = static_cast<unsigned char>(data[pos]);
            if (byte >= 0x80 || byte < 0x20 || byte == 0x7F) break;
            if (cx >= x_max) {
                // Past the right clip/canvas edge — walk the rest of
                // the ASCII run as no-ops (we still need to advance
                // pos so the outer loop sees the non-ASCII boundary).
                // The decode_utf8 branch below will skip non-ASCII
                // characters that fall past the edge for the same
                // reason.
                ++pos;
                ++cx;
                continue;
            }
            if (cx >= x_min) {
                const uint64_t packed =
                    Cell{static_cast<char32_t>(byte), style_id, 0, 0}.pack();
                uint64_t& slot = base[row_off + static_cast<std::size_t>(cx)];
                if (slot != packed) { slot = packed; changed = true; }
                const bool visible = (byte != ' ' || style_id != 0);
                if (visible) {
                    if (cx > highest_x) highest_x = cx;
                    any_visible = true;
                }
            }
            ++cx;
            ++pos;
        }

        // Handle non-ASCII / control characters one at a time — the
        // slow path falls back to set() so the wide-glyph paired-cell
        // guard runs and updates last_col_/max_y_ for us.
        if (pos < len) {
            auto byte = static_cast<unsigned char>(data[pos]);
            if (byte < 0x20 || byte == 0x7F) { ++pos; continue; } // skip C0/DEL
            char32_t cp = decode_utf8(text, pos);
            // Drop control codepoints (C0, DEL, and the C1 block U+0080..U+009F,
            // incl. UTF-8-encoded C1 which decode_utf8 folds to this range) so
            // untrusted text can't inject a terminal escape sequence.
            if (maya::unicode::is_control(cp)) continue;
            if (is_wide_char(cp)) {
                set(cx, y, cp, style_id, 1);
                set(cx + 1, y, U' ', style_id, 2);
                cx += 2;
            } else {
                set(cx, y, cp, style_id, 0);
                ++cx;
            }
        }
    }

    if (changed) stamp_row(y);
    if (any_visible) {
        if (y > max_y_) max_y_ = y;
        if (highest_x > last_col_[static_cast<std::size_t>(y)])
            last_col_[static_cast<std::size_t>(y)] = highest_x;
        stage_ = CanvasStage::Painted;
    }
}

void Canvas::fill(Rect region, char32_t ch, uint16_t style_id) {
    int x0 = std::max(0, region.left().value);
    int y0 = std::max(0, region.top().value);
    int x1 = std::min(width_, region.right().value);
    int y1 = std::min(height_, region.bottom().value);

    // Apply active clip in one shot — no per-pixel check needed.
    if (has_clip_) {
        x0 = std::max(x0, clip_x0_);
        y0 = std::max(y0, clip_y0_);
        x1 = std::min(x1, clip_x1_);
        y1 = std::min(y1, clip_y1_);
    }

    if (x0 >= x1 || y0 >= y1) return;
    stamp_rows(y0, y1);

    uint64_t packed = Cell{ch, style_id, 0, 0}.pack();
    uint64_t* base  = cells_.data();

    // For full-width fills spanning many rows, use SIMD streaming fill to
    // avoid polluting L1/L2 cache (the data will be read later by diff).
    // Threshold: 4+ full rows ≈ 320+ cells at width=80.
    const bool full_width = (x0 == 0 && x1 == width_);
    if (full_width && (y1 - y0) >= 4) {
        simd::streaming_fill(base + static_cast<std::size_t>(y0) * width_,
                             static_cast<std::size_t>(y1 - y0) * static_cast<std::size_t>(width_),
                             packed);
    } else {
        for (int y = y0; y < y1; ++y) {
            std::fill(base + static_cast<std::size_t>(y) * width_ + x0,
                      base + static_cast<std::size_t>(y) * width_ + x1, packed);
        }
    }

    // Track max painted row + per-row last-content column for non-blank fills.
    if (ch != U' ' || style_id != 0) {
        if (y1 - 1 > max_y_) max_y_ = y1 - 1;
        const int new_last = x1 - 1;
        for (int y = y0; y < y1; ++y) {
            if (new_last > last_col_[static_cast<std::size_t>(y)])
                last_col_[static_cast<std::size_t>(y)] = new_last;
        }
    }
    stage_ = CanvasStage::Painted;
}

void Canvas::clear() {
    uint64_t blank = default_cell();
    // Unconditional clear of the full grid.
    //
    // Previous implementation only zeroed [0, max_y_+1) on the
    // assumption that cells beyond max_y_ stay blank inductively
    // — every paint path bumps max_y_, every clear zeros up to
    // its own max_y_. The assumption is fragile in the presence
    // of:
    //   - clear_row(y) when y == max_y_: rescans last_col_ to find
    //     the new max_y_, but if that rescan misses (off-by-one,
    //     race with a subsequent paint, last_col_ stale from a
    //     historical bug) we end up with cells > max_y_ still
    //     holding non-blank content from a prior frame.
    //   - ComponentElement blit fast paths whose max_y_ bump is
    //     gated on `actual_last >= 0`: correct today, but one
    //     future caller that mutates cells_ directly without going
    //     through blit_packed_row_impl would silently break the
    //     invariant.
    //   - resize() / clear_rows() corner cases.
    //
    // The bounded clear saved ~480KB of streaming_fill per frame
    // on a 120×500 canvas — a sub-millisecond cost on any modern
    // CPU, and well below the per-frame layout+diff budget. The
    // safety win (a single broken invariant produces permanent
    // scrollback corruption once a stale row scrolls past term_h)
    // dominates the perf delta. damage_ stays full_rect; max_y_
    // resets to -1.
    // Widen BEFORE multiplying: `width_ * height_` in int is UB on
    // overflow. Unreachable for a real terminal, but inline auto-grow
    // canvases are as tall as the streamed content — keep the math total.
    const std::size_t total_cells = static_cast<std::size_t>(width_)
                                  * static_cast<std::size_t>(height_);
    if (total_cells > 0) {
        simd::streaming_fill(cells_.data(),
                             total_cells,
                             blank);
        std::fill(last_col_.begin(), last_col_.end(), -1);
        stamp_rows(0, height_);
    }
    damage_ = full_rect();
    max_y_ = -1;
    stage_ = CanvasStage::Drained;
    cursor_hint_.reset();
}

void Canvas::clear_row(int y) noexcept {
    if (static_cast<unsigned>(y) >= static_cast<unsigned>(height_)) return;
    const uint64_t blank = default_cell();
    uint64_t* base = cells_.data();
    const std::size_t row_off = static_cast<std::size_t>(y) * width_;
    std::fill(base + row_off, base + row_off + width_, blank);
    last_col_[static_cast<std::size_t>(y)] = -1;
    stamp_row(y);
    if (y == max_y_) {
        // Rescan downward from y-1 to find the new max_y_.
        int new_max = -1;
        for (int yy = y - 1; yy >= 0; --yy) {
            if (last_col_[static_cast<std::size_t>(yy)] >= 0) { new_max = yy; break; }
        }
        max_y_ = new_max;
    }
    // damage_ stays unchanged: the diff still has to cover the rows
    // this widget will overwrite this frame.
}

void Canvas::clear_rows(int n) {
    if (n <= 0) return;
    int rows = std::min(n, height_);
    auto count = static_cast<std::size_t>(rows) * static_cast<std::size_t>(width_);
    uint64_t blank = default_cell();
    // For small regions (typical inline mode), regular fill keeps L1 warm.
    std::fill(cells_.data(), cells_.data() + count, blank);
    damage_ = Rect{{Columns{0}, Rows{0}}, {Columns{width_}, Rows{rows}}};
    std::fill(last_col_.begin(), last_col_.begin() + rows, -1);
    stamp_rows(0, rows);
    // Partial clear: rows [n, height_) may still hold non-blank content,
    // so max_y_ is NOT canvas-wide -1. Rescan the surviving rows from
    // the bottom up via last_col_ (kept current by set/blit/fill).
    // The previous unconditional `max_y_ = -1` left a corrupt state
    // where last_content_col(y) for y >= n returned a valid column
    // but max_content_row() returned -1, so content_height() said
    // "0 rows" and the renderer would skip emitting the surviving
    // rows entirely — silent corruption (live content invisible to
    // the diff scan, but still on the wire from the prior frame).
    int new_max_y = -1;
    for (int y = height_ - 1; y >= rows; --y) {
        if (last_col_[static_cast<std::size_t>(y)] >= 0) {
            new_max_y = y;
            break;
        }
    }
    max_y_ = new_max_y;
    stage_ = CanvasStage::Drained;
    cursor_hint_.reset();
}

void Canvas::clear_below(int keep_top, int clear_bottom) {
    // Degenerate cases: keep nothing → full clear; keep everything →
    // no-op on cells (but still ensure stage/state coherence).
    if (keep_top <= 0 && clear_bottom >= height_) { clear(); return; }
    if (keep_top >= height_) {
        // Nothing to clear; preserved region already correct. Leave
        // cells, last_col_, max_y_ untouched — the frame's paint will
        // only ADD content below (there is none here) and the diff
        // reads the surviving rows as-is.
        return;
    }
    if (keep_top < 0) keep_top = 0;
    // Cap the cleared tail at clear_bottom. Rows [bottom, height_) are
    // never painted this frame (they are headroom slack an oversized
    // resize left above the content), so re-blanking them every frame
    // is an O(rows)/frame fill that scales with the turn length. They
    // stay blank from the last resize's assign(); leaving them dirty is
    // sound because nothing reads them (last_col_ there is -1, so
    // max_y_ can't pick them up, and the diff only walks the painted
    // extent).
    const int bottom = std::min(height_, clear_bottom);
    if (bottom <= keep_top) {
        // Preserved head only; the (would-be) cleared band is entirely
        // above the bottom cap. Still refresh bookkeeping/stage below.
        // Fall through with an empty fill span.
    }
    const uint64_t blank = default_cell();
    uint64_t* base = cells_.data();
    if (bottom > keep_top) {
        const std::size_t off = static_cast<std::size_t>(keep_top) * width_;
        const std::size_t count =
            static_cast<std::size_t>(bottom - keep_top) * width_;
        // Regular fill for the cleared tail; the preserved head keeps its
        // prior-frame bytes so blit_packed_row_cached can skip re-writing
        // an unchanged frozen prefix.
        std::fill(base + off, base + off + count, blank);
        std::fill(last_col_.begin() + keep_top,
                  last_col_.begin() + bottom, -1);
        // Stamp ONLY the cleared band. The preserved head [0, keep_top)
        // keeps its epochs untouched — that is exactly what lets the
        // renderer's whole-entry blit skip prove the frozen prefix is
        // still byte-identical without re-comparing it.
        stamp_rows(keep_top, bottom);
    }
    // Re-derive max_y_ across the WHOLE canvas: the preserved head may
    // hold the tallest content (a tall frozen prefix with an empty
    // live tail) or the cleared tail may (steady streaming). Scan the
    // preserved head from the bottom up via last_col_ (kept current by
    // set/blit/fill); the cleared tail is all blank so it can't win.
    // The un-cleared slack [bottom, height_) has last_col_ == -1 (from
    // the last resize; nothing paints there), so it never contributes.
    int new_max_y = -1;
    for (int y = keep_top - 1; y >= 0; --y) {
        if (last_col_[static_cast<std::size_t>(y)] >= 0) { new_max_y = y; break; }
    }
    max_y_ = new_max_y;
    // Damage must cover the region the diff may need to re-emit; the
    // preserved head is unchanged so damage starts at keep_top.
    damage_ = Rect{{Columns{0}, Rows{keep_top}},
                   {Columns{width_}, Rows{std::max(0, bottom - keep_top)}}};
    stage_ = CanvasStage::Drained;
    cursor_hint_.reset();
}

void Canvas::push_clip(Rect clip) {
    if (clip_stack_.empty()) {
        clip_stack_.push_back(clip);
    } else {
        // Intersect with the current effective clip.
        clip_stack_.push_back(clip_stack_.back().intersect(clip));
    }
    update_clip_cache();
}

void Canvas::pop_clip() {
    if (!clip_stack_.empty()) {
        clip_stack_.pop_back();
    }
    update_clip_cache();
}

void Canvas::reset_clips() noexcept {
    clip_stack_.clear();
    update_clip_cache();
}

bool Canvas::is_clipped(int x, int y) const noexcept {
    if (!has_clip_) return false;
    return x < clip_x0_ || x >= clip_x1_ || y < clip_y0_ || y >= clip_y1_;
}

void Canvas::resize(int w, int h) {
    width_ = w;
    height_ = h;
    max_y_ = -1;
    cells_.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h),
                  default_cell());
    last_col_.assign(static_cast<std::size_t>(h), -1);
    // Fresh allocation — nothing previously recorded against this canvas
    // may skip. epoch_counter_ survives the resize (monotonic for the
    // canvas lifetime), so stamping every row with a NEW epoch strictly
    // invalidates every pre-resize record; a plain zero-assign would
    // RE-ARM old records (0 <= recorded epoch) against blank memory.
    row_epoch_.assign(static_cast<std::size_t>(h), 0);
    stamp_rows(0, h);
    damage_ = full_rect();
    clip_stack_.clear();
    update_clip_cache();
    stage_ = CanvasStage::Drained;
    cursor_hint_.reset();
}

void Canvas::mark_damage(Rect region) {
    damage_ = damage_.unite(region);
}

void Canvas::mark_all_damaged() {
    damage_ = full_rect();
}

} // namespace maya
