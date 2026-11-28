#pragma once
// maya::widget::ActivityIndicator — single-row hex-dump tape.
//
// One row that looks like a live memory window scrolling past the user.
// An offset pointer on the left ticks downward each second (as if a
// debugger were walking the stack backward); the middle is a row of
// hex bytes tumbling at ~100 ms; on the right a classic xxd-style
// ASCII gutter (|...t.h.i.n.k...|) reveals printable bytes as glyphs
// and non-printable as dots.
//
// The bytes ARE random-looking but they're not random. Each is a
// deterministic function of (now_ms, stream_position): noise bytes
// are a splitmix64 hash with a bit-drift overlay so values mutate
// continuously instead of flipping randomly every frame. If the host
// supplies Config::words, the widget places one word per tape
// "cycle" at a fixed stream position so the word physically scrolls
// left across the byte window, enters from the right, transits, and
// exits. As each new word's letters appear on the right edge they
// "lock in" one column at a time — hash-cracker cadence — with the
// accent color so the eye can spot the hidden word forming inside
// what otherwise looks like raw memory. A scanline highlight sweeps
// across the row independently of the tape, like a debugger read-
// head inspecting whichever byte it's over.
//
//   0x7ffd  74 ef 91 b3 4f 70 75 73 04 1c 22 5f  |..O.p.u.s.".|
//                       ^^ ^^ ^^ ^^                <- locked letters
//                                          ↑       <- sweep column
//
// Content-agnostic: the word list belongs to the host. With an empty
// Config::words the widget is pure scrolling noise.

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "../../device.hpp"        // Element plumbing (dsl)
#include "../../core/motion.hpp"     // anim::default_clock / keep_animating_after
#include "../../dsl.hpp"
#include "../../element/element.hpp"
#include "../../element/text.hpp"   // StyledRun, TextElement
#include "../../style/color.hpp"

namespace maya {

class ActivityIndicator {
public:
    struct Config {
        Color       edge_color = Color::slot(ThemeSlot::Info);
        std::string spinner_glyph;   // unused; kept for ABI compat with hosts
        std::string label;           // unused; widget rotates its own word pool
        std::string detail;          // optional trailing token ("3.4s")
        // Host-supplied rotating word pool. The widget rotates
        // through them one per tape cycle, scrolling each across the
        // visible window. Empty → pure scrolling noise, no embedded
        // word. Entries are viewed, not owned; the host must keep
        // them alive for the lifetime of this Config (static storage
        // is the natural fit).
        std::vector<std::string_view> words;
        // Legacy telemetry fields — retained so existing hosts compile.
        std::size_t      stream_bytes = 0;
        float            stream_rate  = 0.f;
        std::string_view entropy_window;
    };

    explicit ActivityIndicator(Config c) : cfg_(std::move(c)) {}

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;

        // Absolute shared animation clock (clamped at 0): the tape is a
        // pure function of it, every instance in the process scrolls in
        // lockstep, and a frozen test clock pins the frame exactly.
        const std::int64_t now_ms =
            std::max<std::int64_t>(0, ::maya::anim_now_ms());

        const Color muted     = Color::slot(ThemeSlot::Muted);
        const Color highlight = cfg_.edge_color;
        const Color sweep_fg  = Color::slot(ThemeSlot::Text);

        // ── Timing knobs.
        //   kScrollMs  ms per 1-column leftward scroll of the tape.
        //   kLockMs    ms per letter "locking in" once a word enters.
        //   kDriftMs   ms per bit-flip on noise bytes (continuous mutation).
        //   kSweepMs   ms per 1-column sweep of the scanline highlight.
        constexpr int kScrollMs = 140;
        constexpr int kLockMs   = 110;
        constexpr int kDriftMs  =  60;
        constexpr int kSweepMs  =  90;

        // Every visible change steps on one of the four cadences above.
        // Wake the loop exactly at the NEAREST upcoming step boundary
        // instead of unconditionally at ~60 fps — the tape renders the
        // same frames it always did, the loop just sleeps between them.
        {
            std::int64_t until_next = kDriftMs;   // finest cadence bound
            for (int step : {kScrollMs, kLockMs, kDriftMs, kSweepMs}) {
                const std::int64_t u = step - (now_ms % step);
                if (u < until_next) until_next = u;
            }
            anim::keep_animating_after(until_next > 0 ? until_next : 1);
        }

        // Stream position of the leftmost visible column.
        const std::int64_t scroll = now_ms / kScrollMs;

        // splitmix64-ish base hash of a stream position.
        auto base_byte = [](std::int64_t pos) -> std::uint8_t {
            std::uint64_t x = static_cast<std::uint64_t>(pos + 1)
                              * 0x9E3779B97F4A7C15ull;
            x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ull;
            x ^= x >> 27; x *= 0x94D049BB133111EBull;
            x ^= x >> 31;
            return static_cast<std::uint8_t>(x & 0xff);
        };
        // Bit-drift overlay: 0–2 flipped bits per (drift_tick, pos).
        // Bytes mutate continuously instead of fully re-randomizing.
        const std::int64_t drift_tick = now_ms / kDriftMs;
        auto drift_byte = [drift_tick](std::int64_t pos) -> std::uint8_t {
            std::uint64_t a = static_cast<std::uint64_t>(drift_tick)
                              * 0xD1B54A32D192ED03ull
                            ^ static_cast<std::uint64_t>(pos + 17)
                              * 0xA13FC965B91E1709ull;
            a ^= a >> 33; a *= 0xff51afd7ed558ccdull; a ^= a >> 33;
            const std::uint8_t bit1 = static_cast<std::uint8_t>(1u << ((a >> 0) & 7));
            const std::uint8_t bit2 = static_cast<std::uint8_t>(1u << ((a >> 8) & 7));
            const bool hold = ((a >> 16) & 0x3) == 0;   // ~25% of slots hold a beat
            return hold ? 0u : static_cast<std::uint8_t>(bit1 ^ bit2);
        };

        // Offset pointer ticks 1 byte per visible-column scroll, so
        // its rate is physically coupled to the tape motion.
        constexpr int kOffsetStart = 0x7ffd;
        const int     offset = kOffsetStart - static_cast<int>(scroll & 0xfff);
        char offbuf[12];
        std::snprintf(offbuf, sizeof(offbuf), "0x%04x", offset);
        const std::string off_str = offbuf;

        const std::string detail = cfg_.detail;

        // Snapshot word list into the lambda. cfg_.words is
        // host-owned static, but copying string_views is trivial and
        // makes the capture self-contained.
        std::vector<std::string_view> words = cfg_.words;

        return component([=, words = std::move(words),
                          off_str = off_str](int avail_w, int /*h*/) -> Element {
            using namespace dsl;

            // ── Longest word in the pool. The visible window must fit
            // it whole; that's the one promise we never break.
            int max_word_len = 0;
            for (auto sv : words)
                max_word_len = std::max(max_word_len, static_cast<int>(sv.size()));

            // ── Progressive degradation. We pick the richest variant
            // that still fits in avail_w, then size the byte window
            // to consume the remaining budget. Variants from richest
            // to leanest:
            //
            //   FULL    indent + offset + hex + gutter + detail
            //   NODET   indent + offset + hex + gutter
            //   NOHEX   indent + offset + gutter
            //   NOOFF   indent + gutter
            //
            // The word lives in the gutter, so we drop hex BEFORE
            // gutter — the active word is the row's whole point and
            // must stay visible at every width the row can render.
            enum Variant { FULL, NODET, NOHEX, NOOFF };

            constexpr int kPerByteHex   = 4;   // "xx " + last has no space, +1 added back
            constexpr int kPerByteAscii = 1;
            const int indent_cost = 2;
            // MEASURED, not byte-counted: off_str is ASCII but `detail`
            // is host text — "⚙ résolution" is 14 bytes, 12 cells; a
            // byte count here over-charges the chrome and starves the
            // byte window.
            const int off_cost    = string_width(off_str);
            const int det_cost    = detail.empty() ? 0
                : string_width(detail) + 5;   // "  ·  " + detail
            // Chrome cost = everything that's NOT the per-byte budget.
            // Every variant includes the two outer `|` pipes of the
            // gutter (2 cols) since we never drop the gutter.
            auto fixed_chrome = [&](Variant v) -> int {
                switch (v) {
                    // FULL: indent + off + 2sp + hex + 2sp + gutter(2) + detail
                    case FULL:  return indent_cost + off_cost + 2 + 2 + 2 + det_cost;
                    // NODET: indent + off + 2sp + hex + 2sp + gutter(2)
                    case NODET: return indent_cost + off_cost + 2 + 2 + 2;
                    // NOHEX: indent + off + 2sp + gutter(2)
                    case NOHEX: return indent_cost + off_cost + 2 + 2;
                    // NOOFF: indent + gutter(2)
                    case NOOFF: return indent_cost + 2;
                }
                return 0;
            };
            // Per-column cost in the byte window. FULL/NODET have
            // both hex (3 cols/byte: "xx ") AND ascii (1 col/byte).
            // NOHEX/NOOFF have just ascii.
            auto per_byte = [&](Variant v) -> int {
                if (v == FULL || v == NODET) return kPerByteHex;  // 3 hex + 1 ascii = 4
                return kPerByteAscii;                              // 1
            };
            // Need enough bytes to fit the word; floor at 4 so a row
            // never collapses to nothing.
            const int floor_bytes = std::max(4, max_word_len);

            auto cols_for = [&](Variant v) -> int {
                const int b = avail_w - fixed_chrome(v);
                return b / per_byte(v);
            };
            Variant variant = FULL;
            for (Variant cand : {FULL, NODET, NOHEX, NOOFF}) {
                if (cols_for(cand) >= floor_bytes) { variant = cand; break; }
                variant = cand;   // keep the leanest even if still tight
            }
            int cols = cols_for(variant);
            if (cols < 1) cols = 1;
            if (cols > 32) cols = 32;
            // Even the leanest variant couldn't reach floor_bytes —
            // we're on a truly tiny terminal. Show what we can; the
            // word will be partially clipped but no row will wrap.

            // ── Word placement on the infinite tape.
            //
            // Cycle length = cols + max_word_len + kGap. Each cycle
            // contains exactly one word, placed `kLeadIn` positions
            // into the cycle. As `scroll` grows, the visible window
            // [scroll, scroll+cols) slides right along the tape so
            // the word drifts left across the screen, then a fresh
            // word enters from the right.
            constexpr int kLeadIn = 2;
            constexpr int kGap    = 6;
            const int kCycleCols = cols + max_word_len + kGap;
            const std::int64_t scroll_pos = scroll;

            auto floor_div = [&](std::int64_t a, int b) -> std::int64_t {
                std::int64_t q = a / b;
                if ((a % b) != 0 && ((a < 0) != (b < 0))) --q;
                return q;
            };

            auto pick_word = [&](std::int64_t cyc) -> std::string_view {
                if (words.empty()) return {};
                std::uint64_t x = static_cast<std::uint64_t>(cyc + 1)
                                  * 0x9E3779B97F4A7C15ull;
                x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ull; x ^= x >> 27;
                return words[static_cast<std::size_t>(x % words.size())];
            };

            struct Placement {
                std::int64_t     start_pos;
                int              len;
                std::string_view word;
            };
            auto placement = [&](std::int64_t cyc) -> Placement {
                auto w = pick_word(cyc);
                return Placement{
                    static_cast<std::int64_t>(cyc) * kCycleCols + kLeadIn,
                    static_cast<int>(w.size()),
                    w,
                };
            };

            // At most two cycles overlap the visible window.
            const std::int64_t cyc_l = floor_div(scroll_pos, kCycleCols);
            const std::int64_t cyc_r =
                floor_div(scroll_pos + cols - 1, kCycleCols);
            const Placement plc_a = placement(cyc_l);
            const Placement plc_b = (cyc_r == cyc_l) ? plc_a
                                                     : placement(cyc_r);

            // Returns (letter, letter_index, start_pos) if pos is
            // inside some placed word; letter < 0 means "noise".
            auto letter_at = [&](std::int64_t pos)
                -> std::tuple<int, int, std::int64_t> {
                auto check = [&](const Placement& p)
                    -> std::tuple<int, int, std::int64_t> {
                    if (p.len == 0) return {-1, 0, 0};
                    if (pos < p.start_pos || pos >= p.start_pos + p.len)
                        return {-1, 0, 0};
                    const int li = static_cast<int>(pos - p.start_pos);
                    return {
                        static_cast<unsigned char>(
                            p.word[static_cast<std::size_t>(li)]),
                        li,
                        p.start_pos,
                    };
                };
                auto a = check(plc_a);
                if (std::get<0>(a) >= 0) return a;
                return check(plc_b);
            };

            // Sweeping highlight column — independent of scroll, so
            // it visually crosses the tape rather than sliding with
            // it. Reads like a debugger read-head.
            const int sweep_col =
                static_cast<int>((now_ms / kSweepMs) % cols);

            // role: 0=plain noise, 1=locked letter, 2=tumbling letter slot
            auto resolve = [&](int c) -> std::pair<std::uint8_t, int> {
                const std::int64_t pos = scroll_pos + c;
                auto [letter, li, start_pos] = letter_at(pos);
                if (letter < 0) {
                    return {static_cast<std::uint8_t>(
                                base_byte(pos) ^ drift_byte(pos)), 0};
                }
                // Letter slot. The word's first column enters the
                // visible window when scroll == start_pos - (cols-1).
                // Lock-in: letter i settles kLockMs after the word's
                // first letter has entered.
                const std::int64_t entry_scroll = start_pos - (cols - 1);
                const std::int64_t entry_ms = entry_scroll * kScrollMs;
                const std::int64_t lock_ms  = entry_ms
                    + static_cast<std::int64_t>(li) * kLockMs;
                if (now_ms >= lock_ms) {
                    return {static_cast<std::uint8_t>(letter), 1};
                }
                // Pre-lock: fast tumble on this slot so the letter
                // visibly churns before settling.
                std::uint64_t x = static_cast<std::uint64_t>(now_ms)
                                    * 0x9E3779B97F4A7C15ull
                                ^ static_cast<std::uint64_t>(pos)
                                    * 0xD1B54A32D192ED03ull;
                x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ull; x ^= x >> 27;
                return {static_cast<std::uint8_t>(x & 0xff), 2};
            };

            // Pre-resolve so hex + ascii agree byte-for-byte.
            std::vector<std::pair<std::uint8_t, int>> resolved;
            resolved.reserve(static_cast<std::size_t>(cols));
            for (int c = 0; c < cols; ++c) resolved.push_back(resolve(c));

            auto style_for = [&](int c, int role) -> Style {
                if (role == 1) return Style{}.with_fg(highlight).with_bold();
                if (role == 2) return Style{}.with_fg(highlight).with_dim();
                if (c == sweep_col) return Style{}.with_fg(sweep_fg);
                return Style{}.with_fg(muted).with_dim();
            };

            // ── Hex column.
            std::string hex;
            hex.reserve(static_cast<std::size_t>(cols) * 3);
            std::vector<StyledRun> hex_runs;
            hex_runs.reserve(static_cast<std::size_t>(cols) * 2);
            for (int c = 0; c < cols; ++c) {
                const auto [b, role] = resolved[static_cast<std::size_t>(c)];
                char buf[4];
                std::snprintf(buf, sizeof(buf), "%02x", b);
                const std::size_t before = hex.size();
                hex.append(buf, 2);
                hex_runs.push_back(StyledRun{before, 2, style_for(c, role)});
                if (c + 1 < cols) {
                    const std::size_t sp = hex.size();
                    hex.push_back(' ');
                    hex_runs.push_back(StyledRun{
                        sp, 1, Style{}.with_fg(muted).with_dim()});
                }
            }

            // ── ASCII gutter.
            std::string ascii;
            ascii.reserve(static_cast<std::size_t>(cols) + 2);
            std::vector<StyledRun> ascii_runs;
            ascii_runs.reserve(static_cast<std::size_t>(cols) + 2);
            ascii.push_back('|');
            ascii_runs.push_back(StyledRun{0, 1, Style{}.with_fg(muted).with_dim()});
            for (int c = 0; c < cols; ++c) {
                const auto [b, role] = resolved[static_cast<std::size_t>(c)];
                const std::size_t before = ascii.size();
                if (b >= 0x20 && b < 0x7f) ascii.push_back(static_cast<char>(b));
                else                       ascii.push_back('.');
                ascii_runs.push_back(StyledRun{before, 1, style_for(c, role)});
            }
            const std::size_t pipe = ascii.size();
            ascii.push_back('|');
            ascii_runs.push_back(StyledRun{pipe, 1, Style{}.with_fg(muted).with_dim()});

            Element off_e = text(off_str) | fgc(muted) | Italic;
            Element hex_e = Element{TextElement{
                .content = std::move(hex),
                .style   = Style{}.with_fg(muted),
                .runs    = std::move(hex_runs),
            }};
            Element ascii_e = Element{TextElement{
                .content = std::move(ascii),
                .style   = Style{}.with_fg(muted),
                .runs    = std::move(ascii_runs),
            }};

            std::vector<Element> parts;
            parts.reserve(8);
            parts.push_back(text("  "));
            if (variant != NOOFF) {
                parts.push_back(std::move(off_e));
                parts.push_back(text("  "));
            }
            if (variant == FULL || variant == NODET) {
                parts.push_back(std::move(hex_e));
                parts.push_back(text("  "));
            }
            // Gutter is always shown — it carries the word.
            parts.push_back(std::move(ascii_e));
            if (variant == FULL && !detail.empty()) {
                parts.push_back(text("  \xc2\xb7  ") | fgc(muted) | Italic);
                parts.push_back(text(detail) | fgc(muted) | Italic);
            }
            // Exactly one row — no internal leading blank. Hosts that
            // need separation from the element above supply it (the
            // in-Turn placeholder gets it from Turn's under-header
            // blank). Staying single-row makes the placeholder height
            // match the first streamed content slot, so the
            // indicator→content flip is height-seamless.
            return h(std::move(parts)).build();
        });
    }

private:
    Config cfg_;
};

} // namespace maya
