#pragma once
// maya::tmux — first-class tmux support: ONE place that answers "are we
// inside tmux, what will it forward, and how do I reach the terminal
// underneath it".
//
// ─────────────────────────────────────────────────────────────────────────
// Why a capability layer instead of scattered `if (in_tmux)` branches
// ─────────────────────────────────────────────────────────────────────────
// tmux is not a terminal — it is a terminal MULTIPLEXER that re-renders
// its own screen and forwards only what it understands. Three distinct
// facts decide what an app may do, and they are independent:
//
//   1. PRESENCE   — is tmux in the path at all? ($TMUX here, or a
//                   tmux-*/screen-* $TERM that survived an ssh hop, so a
//                   locally-multiplexed remote session counts too.)
//   2. FEATURES   — what does tmux believe the OUTER terminal supports?
//                   tmux publishes this as #{client_termfeatures}: a
//                   comma list from its own vocabulary (cstyle, focus,
//                   clipboard, sync, RGB, extkeys, …). This is
//                   AUTHORITATIVE where env-sniffing is a guess: inside
//                   tmux the outer terminal's fingerprints
//                   (KITTY_WINDOW_ID, TERM_PROGRAM, …) are stripped and
//                   $TERM is rewritten, so every heuristic that reads
//                   them silently answers "unknown" and the app degrades
//                   for no reason.
//   3. PASSTHROUGH— will tmux hand raw bytes to the outer terminal when
//                   wrapped in DCS `\ePtmux;…\e\\`? Governed by the
//                   `allow-passthrough` option, which DEFAULTS TO OFF.
//                   Never assume it: an unwrapped-but-needed sequence is
//                   swallowed, and a wrapped-but-disallowed one is
//                   swallowed too. Only a probe knows.
//
// Measured behaviour that motivates this module (nested-tmux probes, tmux
// 3.7 under kitty):
//   • A RAW `CSI ?2026h … l` (synchronized output) is SWALLOWED by tmux —
//     the body still renders, un-synced, so every frame can tear. The
//     same sequence WRAPPED in passthrough reaches the outer terminal.
//   • tmux reported features `bpaste,ccolour,clipboard,cstyle,focus,RGB,
//     title` — note `sync` ABSENT even though the outer terminal (kitty)
//     supports it, because kitty advertises the DCS form `\eP=1s\e\\`
//     rather than the `?2026` private mode tmux detects. So "tmux says
//     no sync" must NOT be read as "the outer terminal cannot sync".
//
// Everything here is cached after one query (a `tmux display-message`
// costs ~8 ms; we pay it once per process, never per frame) and is safe
// to call when tmux is absent — every accessor degrades to "not in
// tmux", which is the pre-existing behaviour.

#include <string>
#include <string_view>

namespace maya::tmux {

// ── Presence ─────────────────────────────────────────────────────────
// True when tmux (or GNU screen) is anywhere in the path: $TMUX set on
// THIS host, or a tmux-*/screen-* $TERM inherited across ssh (tmux on
// the LOCAL side). Both topologies matter: in the second, $TMUX is gone
// but every byte we write still transits tmux's parser.
[[nodiscard]] bool active() noexcept;

// ── Features (what tmux says the OUTER terminal supports) ────────────
// One entry of tmux's `terminal-features` vocabulary. Only the ones an
// app actually gates behaviour on are modelled; unknown names are simply
// not queryable, which keeps this honest.
enum class Feature {
    Sync,        // "sync"       synchronized updates (DEC 2026)
    CursorStyle, // "cstyle"     DECSCUSR cursor shape
    CursorColour,// "ccolour"    OSC 12 cursor colour
    Focus,       // "focus"      ?1004 focus reporting
    Clipboard,   // "clipboard"  OSC 52
    ExtendedKeys,// "extkeys"    CSI-u / kitty keyboard
    Rgb,         // "RGB"        24-bit colour
    Hyperlinks,  // "hyperlinks" OSC 8
    Title,       // "title"      window title
};

// Does tmux report `f` for the attached client's terminal? False when
// tmux is absent (callers should consult their own env heuristics then)
// or when tmux simply doesn't list it.
//
// IMPORTANT ASYMMETRY: a `true` is authoritative — tmux will forward it.
// A `false` is NOT proof the outer terminal lacks the feature; it may be
// a capability tmux failed to detect (see the kitty/sync case above). So
// gate ENABLING on true, but never treat false as "impossible" if a
// passthrough path exists.
[[nodiscard]] bool has_feature(Feature f) noexcept;

// ── Passthrough ──────────────────────────────────────────────────────
// True when tmux will forward DCS-wrapped bytes to the outer terminal,
// i.e. `allow-passthrough` is on|all. DEFAULTS TO OFF in tmux, so this
// is a real question, not a formality.
[[nodiscard]] bool passthrough_allowed() noexcept;

// ── Clipboard READS ──────────────────────────────────────────────────
// True when tmux will ASK THE TERMINAL for the clipboard and relay the
// reply back to us, i.e. `get-clipboard` is request|both.
//
// This is THE gate on clipboard reads under tmux, and it is the one
// most likely to be closed: `get-clipboard` DEFAULTS TO `buffer`, which
// means tmux answers a clipboard request from its OWN paste buffer and
// never consults the real terminal. A paste buffer holds TEXT, so an
// image request can't be satisfied no matter what the outer terminal
// supports — the request is intercepted before it ever leaves tmux.
// (`off` ignores the request entirely.)
//
// Independent of passthrough_allowed(): passthrough governs bytes we
// send OUT; this governs whether an answer comes BACK.
[[nodiscard]] bool clipboard_reads_relayed() noexcept;

// Rows of scrollback tmux keeps per pane (history-limit), or 0 when unknown
// / not in tmux. Read from the same cached probe, no extra round-trip.
[[nodiscard]] int history_limit() noexcept;

// Wrap `seq` for tmux passthrough: `\ePtmux;` + seq with every ESC
// DOUBLED + `\e\\`. Returns `seq` UNCHANGED when tmux is not in the
// path, so call sites stay branch-free.
//
// Note this does not consult passthrough_allowed(): wrapping is
// harmless when passthrough is off (tmux drops the DCS), whereas NOT
// wrapping inside tmux means the sequence is interpreted by tmux itself.
// Callers that need the byte to actually ARRIVE should check
// passthrough_allowed() and pick a fallback.
[[nodiscard]] std::string wrap(std::string_view seq);

// ── Synchronized output ──────────────────────────────────────────────
// The correct sync markers for the current environment, resolved once:
//   • no tmux            → plain CSI ?2026h / ?2026l
//   • tmux + passthrough → the same, DCS-wrapped so they REACH the outer
//                          terminal (raw ones are swallowed — measured)
//   • tmux, no passthrough → EMPTY, because an unwrapped marker is eaten
//                          by tmux and a wrapped one is dropped: there is
//                          no way to sync, and emitting bytes that can
//                          only be misparsed is worse than not trying.
// Empty strings mean "this environment cannot do synchronized output";
// the renderer should skip the wrapper entirely rather than emit half.
[[nodiscard]] std::string_view sync_begin() noexcept;
[[nodiscard]] std::string_view sync_end() noexcept;

// True when sync_begin()/sync_end() are non-empty AND worth using.
[[nodiscard]] bool sync_available() noexcept;

// Re-probe if a DIFFERENT client is attached than the one the cached
// answers describe. tmux capabilities are per-client, so the same session
// reports a different clipboard story from a desktop kitty than from a
// phone over mosh — and the probe is otherwise memoised for the whole
// process, which freezes whichever client was attached at startup. Call
// this before acting on a capability that a reattach can invalidate (the
// clipboard read is the motivating case). Costs one display-message
// round-trip (~8 ms) and only re-probes when the client actually changed;
// returns true if the cache was refreshed. Safe when tmux is absent.
bool refresh_if_client_changed() noexcept;

// PID of the tmux client currently attached, or -1 when unknown / no tmux.
// Useful for identifying the TRANSPORT behind a session: with a persistent
// (systemd-launched) tmux server the ssh/mosh process that carries the
// session is an ancestor of the CLIENT, never of the program running in a
// pane — so walking your own parents cannot see it. Not cached: the answer
// changes on every detach/reattach.
[[nodiscard]] int client_pid() noexcept;

// ── Testing seam ──────────────────────────────────────────────────
// Reset the memoised probe results so a test can re-evaluate after
// changing the environment. Not for production use.
void reset_cache_for_test() noexcept;

} // namespace maya::tmux
