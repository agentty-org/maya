// maya::tmux — impl. See the header for the capability model and the
// measured tmux behaviours that motivate it.

#include "maya/terminal/tmux.hpp"
#include "maya/terminal/ansi.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace maya::tmux {

namespace {

// ── One-shot probe of the live tmux server ───────────────────────────
// A `tmux display-message -p` round-trip costs ~8 ms; we pay it ONCE per
// process and cache. Every accessor funnels through probe().
struct Probe {
    bool        in_tmux      = false;
    bool        passthrough  = false;
    bool        clip_relay   = false;   // get-clipboard is request|both
    std::string features;      // raw comma list from #{client_termfeatures}
    // WHICH client the answers above describe. tmux capabilities are
    // per-CLIENT, not per-server: the same session attached from kitty on a
    // desktop and from a phone over mosh reports different termfeatures and
    // a different clipboard story. Caching for process lifetime therefore
    // freezes whichever client happened to be attached at startup, and every
    // later detach/reattach silently answers from a stale verdict — the
    // "tmux reports your outer terminal has no clipboard support" that
    // survives fixing the config, because the process never looks again.
    // tty+termname is the cheap identity: one display-message round-trip.
    std::string client_id;
    // history-limit: rows of scrollback tmux keeps per pane. 0 = unknown.
    int         history_limit = 0;
};

std::string env_str(const char* k) {
    const char* v = std::getenv(k);
    return v ? std::string{v} : std::string{};
}

// Presence: $TMUX (tmux on THIS host) or a tmux-*/screen-* $TERM that
// survived an ssh hop (tmux on the LOCAL side). Kept identical to the
// long-standing ansi::tmux_in_path() so behaviour can't drift between
// the two — that function now delegates here.
bool detect_presence() {
    if (!env_str("TMUX").empty()) return true;
    const std::string term = env_str("TERM");
    return term.rfind("tmux", 0) == 0 || term.rfind("screen", 0) == 0;
}

// Ask the running server. Returns "" on any failure (no server, tmux not
// on PATH, permission) — callers treat that as "unknown", never as a
// hard error: a capability probe must not be able to break rendering.
//
// WINDOWS: compiled out entirely rather than aliased to _popen. tmux is a
// POSIX terminal multiplexer with no Windows build; the probe would spawn
// a shell on every start only to fail, and detect_presence() already
// returns false there (no $TMUX, no tmux-*/screen-* $TERM), so this is
// unreachable in practice. Returning "unknown" is exactly the documented
// failure contract — no behaviour changes, one less process spawn.
#if defined(_WIN32)
std::string ask_tmux(const char* /*format*/) { return {}; }
#else
std::string ask_tmux(const char* format) {
    std::string cmd = "tmux display-message -p '";
    cmd += format;
    cmd += "' 2>/dev/null";
    // NOLINTNEXTLINE(cert-env33-c) — fixed, argument-free command string;
    // `format` is a compile-time literal from THIS file, never user input.
    std::FILE* p = ::popen(cmd.c_str(), "r");
    if (!p) return {};
    std::string out;
    std::array<char, 256> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), p))
        out += buf.data();
    ::pclose(p);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();
    return out;
}
#endif

// The cache. Read from the UI thread (frame sync markers) and from agentty's
// clipboard worker (refresh_if_client_changed), so every access to it holds
// probe_mu(). A probe spawns tmux (~8 ms); only the first caller pays it.
std::mutex& probe_mu() { static std::mutex m; return m; }
bool& probe_done()  { static bool d = false; return d; }

// Callers hold probe_mu().
Probe& probe_locked() {
    static Probe p;
    if (!probe_done()) {
        probe_done() = true;
        p = Probe{};
        p.in_tmux = detect_presence();
        if (p.in_tmux) {
            // Test/CI seam: a fabricated tmux environment ($TMUX set by a
            // test) must NOT reach out to the developer's REAL tmux
            // server — that would make results depend on the machine and
            // on whoever's config is loaded. When MAYA_TMUX_FAKE is set,
            // the probe answers purely from env, never spawning tmux.
            if (const char* fake = std::getenv("MAYA_TMUX_FAKE"); fake && *fake) {
                p.features    = env_str("MAYA_TMUX_FEATURES");
                p.passthrough = env_str("MAYA_TMUX_PASSTHROUGH") == "1";
                p.clip_relay  = env_str("MAYA_TMUX_GET_CLIPBOARD") == "1";
                // Mirror the real branch: record WHICH client these
                // fabricated answers describe, so refresh_if_client_changed
                // can distinguish "same client" from "reattached" under the
                // seam exactly as it does live. Without this the fake probe
                // leaves client_id empty and every refresh reads as a
                // change, re-probing on every call.
                p.client_id   = env_str("MAYA_TMUX_CLIENT");
                p.history_limit = std::atoi(env_str("MAYA_TMUX_HISTORY_LIMIT").c_str());
                return p;
            }
            // Only meaningful when a server is actually reachable: the
            // ssh-local topology ($TERM says tmux, but the server lives
            // on the OTHER host) yields empty answers, which correctly
            // degrade to "no features, no passthrough".
            p.features = ask_tmux("#{client_termfeatures}");
            const std::string pt =
                ask_tmux("#{?#{==:#{allow-passthrough},on},1,"
                         "#{?#{==:#{allow-passthrough},all},1,0}}");
            p.passthrough = (pt == "1");
            // get-clipboard: only request|both actually ask the terminal
            // and relay its answer. The default `buffer` serves tmux's
            // own paste buffer (text) and never consults the terminal,
            // so an IMAGE read can never succeed under it.
            const std::string gc =
                ask_tmux("#{?#{==:#{get-clipboard},request},1,"
                         "#{?#{==:#{get-clipboard},both},1,0}}");
            p.clip_relay = (gc == "1");
            // Identity of the client these answers describe. Cheap enough to
            // re-ask later (one round-trip) to decide whether the cached
            // verdict is still about the terminal the user is looking at.
            // history_limit rides the same round-trip.
            const std::string id_hl =
                ask_tmux("#{client_tty}/#{client_termname} #{history_limit}");
            const auto sp = id_hl.rfind(' ');
            p.client_id = sp == std::string::npos ? id_hl : id_hl.substr(0, sp);
            if (sp != std::string::npos)
                p.history_limit = std::max(0, std::atoi(id_hl.c_str() + sp + 1));
        }
    }
    return p;
}

Probe probe() {
    std::lock_guard lk(probe_mu());
    return probe_locked();
}

// Current client identity, without disturbing the cache. Empty when there
// is no reachable server (ssh-local topology) — which we treat as "cannot
// tell", never as "changed", so a degraded environment doesn't re-probe on
// every call.
[[nodiscard]] std::string current_client_id() {
    if (const char* fake = std::getenv("MAYA_TMUX_FAKE"); fake && *fake)
        return env_str("MAYA_TMUX_CLIENT");
    return ask_tmux("#{client_tty}/#{client_termname}");
}

[[nodiscard]] std::string_view feature_name(Feature f) noexcept {
    switch (f) {
        case Feature::Sync:         return "sync";
        case Feature::CursorStyle:  return "cstyle";
        case Feature::CursorColour: return "ccolour";
        case Feature::Focus:        return "focus";
        case Feature::Clipboard:    return "clipboard";
        case Feature::ExtendedKeys: return "extkeys";
        case Feature::Rgb:          return "RGB";
        case Feature::Hyperlinks:   return "hyperlinks";
        case Feature::Title:        return "title";
    }
    return {};
}

// Exact, comma-delimited membership test (so "sync" never matches a
// hypothetical "nosync", and "RGB" is case-sensitive as tmux spells it).
bool list_contains(std::string_view list, std::string_view want) {
    if (want.empty()) return false;
    std::size_t i = 0;
    while (i <= list.size()) {
        const std::size_t j = list.find(',', i);
        const std::size_t end = (j == std::string_view::npos) ? list.size() : j;
        if (list.substr(i, end - i) == want) return true;
        if (j == std::string_view::npos) break;
        i = j + 1;
    }
    return false;
}

// Resolved sync markers. Recomputed per call — they depend on presence
// (live) and passthrough (memoised), and string-building three short
// literals is nothing next to a frame. Caching them across an env change
// was a real hazard: a test (or a re-exec into/out of tmux) would keep
// emitting markers for the WRONG environment.
struct SyncPair { std::string begin, end; };

SyncPair make_sync_pair() {
    SyncPair sp;
    const std::string raw_begin{ansi::sync_start};
    const std::string raw_end{ansi::sync_end};
    if (!active()) {                       // no tmux: plain markers
        sp.begin = raw_begin;
        sp.end   = raw_end;
    } else if (passthrough_allowed()) {    // tmux: must be wrapped
        sp.begin = wrap(raw_begin);
        sp.end   = wrap(raw_end);
    }
    // else — tmux without passthrough: a raw marker is swallowed by tmux
    // (measured) and a wrapped one is dropped. Emit nothing.
    return sp;
}

// Stable storage so the string_view accessors below stay valid for the
// caller's use (recomputed on each call; the previous value lives until
// the next one, which is exactly the lifetime a frame needs).
const SyncPair& sync_pair() {
    static thread_local SyncPair sp;
    sp = make_sync_pair();
    return sp;
}

} // namespace

// PRESENCE IS NOT CACHED. It is two getenv() calls, and callers
// (including tests that fabricate a tmux environment mid-run) expect it
// to track $TMUX/$TERM live — the historical ansi::tmux_in_path()
// behaviour. Only the EXPENSIVE server probes below are memoised.
bool active() noexcept { return detect_presence(); }

bool passthrough_allowed() noexcept {
    return active() && probe().passthrough;
}

bool clipboard_reads_relayed() noexcept {
    return active() && probe().clip_relay;
}

int history_limit() noexcept {
    return active() ? probe().history_limit : 0;
}

bool has_feature(Feature f) noexcept {
    if (!active()) return false;
    return list_contains(probe().features, feature_name(f));
}

std::string wrap(std::string_view seq) {
    if (!active()) return std::string{seq};
    std::string out;
    out.reserve(seq.size() * 2 + 16);
    out += "\x1bPtmux;";
    for (char c : seq) {
        out.push_back(c);
        if (c == '\x1b') out.push_back('\x1b');   // ESC must be DOUBLED
    }
    out += "\x1b\\";
    return out;
}

std::string_view sync_begin() noexcept { return sync_pair().begin; }
std::string_view sync_end()   noexcept { return sync_pair().end; }
bool sync_available() noexcept { return !sync_pair().begin.empty(); }

void reset_cache_for_test() noexcept {
    std::lock_guard lk(probe_mu());
    probe_done() = false;
}

bool refresh_if_client_changed() noexcept {
    if (!active()) return false;
    // Force the cached probe to exist so client_id is populated, then
    // compare against the client attached RIGHT NOW.
    const std::string cached = probe().client_id;
    std::string now;
    try { now = current_client_id(); } catch (...) { return false; }
    // Empty = no reachable server / degraded env. "Cannot tell" must not
    // count as a change, or we'd re-probe on every keystroke.
    if (now.empty() || now == cached) return false;
    std::lock_guard lk(probe_mu());
    probe_done() = false;
    (void)probe_locked();   // re-probe under the new client
    return true;
}

int client_pid() noexcept {
    if (!active()) return -1;
    // Not cached: the attached client changes across detach/reattach, and
    // callers ask this exactly when they need the CURRENT one (diagnosing
    // a transport, which is per-client by nature).
    std::string s;
    try { s = ask_tmux("#{client_pid}"); } catch (...) { return -1; }
    if (s.empty()) return -1;
    errno = 0;
    char* end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 10);
    if (errno != 0 || end == s.c_str() || v <= 0 || v > 0x7fffffff) return -1;
    return static_cast<int>(v);
}

} // namespace maya::tmux
