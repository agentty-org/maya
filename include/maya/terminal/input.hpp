#pragma once
// maya::input - Input parser FSM
//
// Converts raw terminal bytes into structured Event values. The parser is
// a state machine that handles partial escape sequences, CSI parameters,
// SS3 function keys, SGR mouse reports, bracketed paste, and focus events.
//
// The ambiguous-escape problem: a bare ESC byte could be the start of a
// CSI sequence or the user pressing Escape. We resolve this with a 50ms
// timeout - if no follow-up byte arrives within that window, we emit
// an Escape key event.

#include <chrono>
#include <cstdint>
#include <atomic>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "../core/types.hpp"

namespace maya {

// Total clipboard-reply bytes decoded since process start (OSC 5522 /
// OSC 52 reads). Monotonic; the VALUE is meaningless, only its movement.
//
// A clipboard image is base64 PNG — hundreds of KB to megabytes — and over
// ssh+tmux it streams in over well more than a second. A host that arms a
// short timer to report "your terminal never answered" otherwise cannot
// distinguish real silence from a transfer still in flight, and kills a
// large paste at the deadline. Sample this before and after waiting: if it
// moved, the terminal IS answering and the deadline should be extended.
[[nodiscard]] std::atomic<std::uint64_t>& clipboard_rx_bytes() noexcept;

// Bumped when the terminal REFUSED a clipboard read — an OSC 5522 reply whose
// status is an error (EPERM/ENOSYS/EBUSY) rather than OK/DATA/DONE.
//
// Separate from silence, because the fix is different and the user cannot
// guess which they hit. kitty's `clipboard_control` defaults to write-only,
// so a fresh install ANSWERS a read request with EPERM: the protocol is
// implemented, the permission is not granted. Dropping that on the floor
// makes an image paste silently become text (kitty still serves text/plain
// via OSC 52), and the host then tells the user their terminal lacks OSC
// 5522 support — sending them to fix the wrong thing.
//
// Monotonic; only movement is meaningful. Sample before arming a read and
// compare after: if it moved, say "your terminal refused the read" and name
// the setting, not "your terminal never answered".
[[nodiscard]] std::atomic<std::uint64_t>& clipboard_read_refused() noexcept;

// ============================================================================
// Key - all recognized key values
// ============================================================================

struct CharKey { char32_t codepoint; };

enum class SpecialKey : uint8_t {
    Up, Down, Left, Right,
    Home, End,
    PageUp, PageDown,
    Tab, BackTab,
    Backspace, Delete, Insert,
    Enter, Escape,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
};

using Key = std::variant<CharKey, SpecialKey>;

// ============================================================================
// Modifiers
// ============================================================================

struct Modifiers {
    bool ctrl  = false;
    bool alt   = false;
    bool shift = false;
    bool super_ = false;

    constexpr bool none() const noexcept {
        return !ctrl && !alt && !shift && !super_;
    }

    constexpr bool operator==(const Modifiers&) const = default;

    // Parse the ANSI modifier parameter (1-based bitmask, value = 1 + bitmask)
    [[nodiscard]] static constexpr Modifiers from_param(int param) noexcept {
        int bits = param - 1;
        return {
            .ctrl  = (bits & 0x04) != 0,
            .alt   = (bits & 0x02) != 0,
            .shift = (bits & 0x01) != 0,
            .super_ = (bits & 0x08) != 0,
        };
    }
};

// ============================================================================
// KeyEvent
// ============================================================================

struct KeyEvent {
    Key       key;
    Modifiers mods;
    std::string raw_sequence; // original bytes that produced this event
};

// ============================================================================
// Mouse types
// ============================================================================

enum class MouseButton : uint8_t {
    Left, Right, Middle,
    ScrollUp, ScrollDown,
    ScrollLeft, ScrollRight,   // SGR mouse codes 66 / 67 (horizontal wheel)
    None,
};

enum class MouseEventKind : uint8_t {
    Press,
    Release,
    Move,
};

struct MouseEvent {
    MouseButton   button;
    MouseEventKind kind;
    Columns       x;
    Rows          y;
    Modifiers     mods;
};

// ============================================================================
// Other event types
// ============================================================================

struct PasteEvent {
    std::string content;
    // Not from a bracketed paste: a burst of plain bytes taken as one.
    bool unbracketed = false;
    // The next read of the same unbracketed burst (it arrived right after
    // the previous one). The app should append it to that paste.
    bool continues = false;
};

struct FocusEvent {
    bool focused;
};

struct ResizeEvent {
    Columns width;
    Rows    height;
};

// ============================================================================
// Event - the unified event variant
// ============================================================================

using Event = std::variant<KeyEvent, MouseEvent, PasteEvent, FocusEvent, ResizeEvent>;

// ============================================================================
// InputParser - stateful FSM for converting raw bytes to events
// ============================================================================
// Feed raw bytes via feed(). The parser may buffer partial sequences
// internally. Call flush_timeout() periodically (e.g. after a poll timeout)
// to resolve ambiguous escapes.

class InputParser {
public:
    InputParser() = default;

    /// Feed raw bytes and extract all complete events.
    [[nodiscard]] auto feed(std::string_view bytes) -> std::vector<Event>;

    /// Call after a poll/read timeout to resolve a stalled partial
    /// sequence. Per-state deadlines (measured from the LAST byte fed
    /// while a sequence was pending, so a slowly-streaming sequence is
    /// never chopped mid-flight):
    ///   Escape          → emit the bare Escape key (Alt+Escape after
    ///                     a double-ESC) after 50ms.
    ///   Csi / Ss3       → abandon the partial sequence after 50ms so a
    ///                     truncated CSI (dropped bytes on a flaky pty)
    ///                     can't wedge the FSM and swallow subsequent
    ///                     keystrokes as parameter bytes forever.
    ///   Utf8            → emit U+FFFD for the truncated codepoint after
    ///                     50ms and resync.
    ///   Osc             → abandon after 1s (OSC replies can be large and
    ///                     stream slowly over SSH; only true inactivity
    ///                     aborts).
    ///   BracketedPaste  → flush the accumulated bytes as a PasteEvent
    ///                     after 2s of silence (lost \x1b[201~ terminator;
    ///                     a live paste keeps resetting the clock).
    [[nodiscard]] auto flush_timeout() -> std::vector<Event>;

    /// Check if the parser has a partial sequence buffered.
    [[nodiscard]] bool has_pending() const noexcept;

    /// Reset the parser to ground state, discarding any partial sequence.
    void reset() noexcept;

    /// Frame acknowledgements received: each `CSI 0 n` (the terminal's reply
    /// to a Device Status Report query, `CSI 5 n`) seen since the last call.
    /// The screen appends that query after every frame; the terminal answers
    /// only once it has PARSED everything before it, so a reply means "that
    /// frame is on the glass". Consumed here, never turned into input.
    [[nodiscard]] int take_acks() noexcept { return std::exchange(acks_, 0); }
    [[nodiscard]] int peek_acks() const noexcept { return acks_; }

private:
    using clock = std::chrono::steady_clock;
    int acks_ = 0;
    // Inactivity deadlines per pending state — see flush_timeout() docs.
    static constexpr auto escape_timeout_ = std::chrono::milliseconds(50);
    static constexpr auto kOscTimeout     = std::chrono::milliseconds(1000);
    static constexpr auto kPasteTimeout   = std::chrono::milliseconds(2000);
    // A single read of at least this many plain bytes (no escapes, no
    // control keys) is taken as an unbracketed paste. Typing, even key
    // repeat, never puts this much into one read.
    static constexpr std::size_t kUnbracketedPasteMin = 64;
    // The last read was part of an unbracketed paste, so more may follow if
    // it comes within kBurstGap. Short enough that a key you type after a
    // paste is a key, not more paste.
    bool in_burst_ = false;
    // A bracketed paste was flushed by its timeout before ESC[201~ came:
    // bytes up to that terminator are still paste.
    bool paste_tail_ = false;
    clock::time_point last_burst_{};
    static constexpr auto kBurstGap = std::chrono::milliseconds(40);
    // After this long with nothing, a burst is over whatever comes next.
    static constexpr auto kBurstIdle = std::chrono::milliseconds(1000);

    // Upper bound on a single OSC string's accumulated length. OSC 52
    // clipboard responses can carry a base64 image (the terminal reads
    // its OWN clipboard on the user's machine and ships the bytes back
    // over the pty — the SSH image-paste path), so the cap is generous:
    // 16 MiB of base64 decodes to ~12 MiB of image, comfortably above
    // Anthropic's 5 MB per-image limit. A reply longer than this is
    // abandoned (FSM resets to Ground) rather than growing buf_
    // unbounded on hostile / runaway input.
    static constexpr std::size_t kMaxOscLen = 16u * 1024u * 1024u;
    // CSI sequences are tiny (params + intermediates + final). 256 bytes is
    // far beyond any real terminal's emission; past it the sequence is
    // garbage and the parser abandons it back to Ground.
    static constexpr std::size_t kMaxCsiLen = 256;

    enum class State : uint8_t {
        Ground,
        Escape,         // saw ESC, waiting for second byte
        Csi,            // inside CSI sequence (ESC [)
        Ss3,            // inside SS3 sequence (ESC O)
        Osc,            // inside OSC sequence (ESC ])
        BracketedPaste, // inside bracketed paste content
        Utf8,           // buffering UTF-8 continuation bytes
    };

    State       state_ = State::Ground;
    std::string buf_;
    // Time of the LAST byte fed while a partial sequence was pending.
    // flush_timeout() measures inactivity from here, so any state can be
    // rescued from a truncated sequence — not just a bare Escape.
    clock::time_point escape_start_;
    char32_t    utf8_cp_ = 0;      // accumulated codepoint
    uint8_t     utf8_remaining_ = 0; // continuation bytes left
    char32_t    utf8_min_ = 0;     // smallest codepoint the lead byte may encode
    std::string utf8_raw_;         // raw bytes for the sequence
    // Alt latch. Set by a double-ESC prefix (legacy altSendsEscape:
    // ESC ESC <seq> means Alt+<seq>) and by an ESC-prefixed UTF-8 lead
    // (Alt+é arrives as ESC 0xC3 0xA9). Applied to the KeyEvent(s) the
    // pending sequence resolves to, then cleared on return to Ground.
    bool        pending_alt_ = false;

    // Character classification
    static constexpr bool is_csi_final(uint8_t ch) noexcept {
        return ch >= 0x40 && ch <= 0x7e;
    }

    // Ground state: plain bytes
    void ground_byte(uint8_t ch, std::vector<Event>& events);

    // Start a UTF-8 multi-byte sequence from its lead byte. Shared by
    // the Ground path and the Escape path (Alt + non-ASCII). `prefix`
    // carries any already-consumed raw bytes (the ESC of an alt latch)
    // so raw_sequence stays faithful. Returns false if `ch` is not a
    // valid lead (overlong 0xC0/0xC1, 0xF5+, stray continuation).
    [[nodiscard]] bool begin_utf8(uint8_t ch, std::string prefix);

    // Alt + key
    void emit_alt_key(uint8_t ch, std::vector<Event>& events);

    // CSI sequence parser
    void parse_csi(std::vector<Event>& events);

    // Tilde-terminated special keys (CSI number ~)
    void handle_tilde_key(int code, int modifier_param, std::vector<Event>& events);

    // SGR mouse report: CSI < Pb ; Px ; Py M/m
    void parse_sgr_mouse(std::string_view params_str, uint8_t final_byte,
                         std::vector<Event>& events);

    // SS3 function keys (ESC O P/Q/R/S)
    void parse_ss3(uint8_t ch, std::vector<Event>& events);

    // OSC parser. Recognises the OSC 52 clipboard READ response
    // (52;<sel>;<base64>) and emits its decoded bytes as a PasteEvent so
    // image/text paste works over SSH with no remote clipboard tool;
    // OSC 5522 packets (kitty's multi-format clipboard read — the only
    // escape path that carries IMAGE bytes) are routed to parse_osc5522;
    // all other OSC replies are silently discarded.
    void parse_osc(std::vector<Event>& events);

    // OSC 5522 (kitty clipboard protocol) read-reply reassembly. The
    // terminal answers a type=read request with a packet SEQUENCE:
    //   5522;type=read:status=OK
    //   5522;type=read:status=DATA:mime=<b64-mime>;<b64-chunk>   (×N)
    //   5522;type=read:status=DONE
    // Chunks are individually base64-padded, ≤4096 raw bytes each, and
    // all chunks of one MIME type arrive before the next type starts.
    // Types arrive in the REQUEST's preference order (images first, see
    // ansi::request_clipboard_image), so we keep only the FIRST type's
    // bytes and emit them as one PasteEvent on DONE — image when the
    // clipboard has one, text otherwise. Any error status (EPERM /
    // EBUSY / ENOSYS) or oversize payload abandons the transfer
    // silently: to the host it looks like a terminal that never
    // replied, which is already a handled case.
    void parse_osc5522(std::string_view body, std::vector<Event>& events);

    // Decode an RFC 4648 base64 payload to raw bytes (whitespace
    // tolerated, '=' padding ends the data). nullopt on an illegal
    // symbol. Used by parse_osc for the OSC 52 clipboard reply, which
    // may carry binary image bytes.
    [[nodiscard]] static std::optional<std::string>
        decode_base64(std::string_view in);

    // Parameter parsing utility
    [[nodiscard]] static auto parse_params(std::string_view s) -> std::vector<int>;

    // ---- OSC 5522 read-reply accumulation (see parse_osc5522) ----------
    // Alive between the status=OK packet and the DONE / error packet of
    // one clipboard read. reset() clears it like any other partial state.
    bool        osc5522_active_ = false;  // saw status=OK, DONE pending
    bool        osc5522_locked_ = false;  // first mime chosen; skip others
    std::string osc5522_mime_;            // decoded mime of the kept type
    std::string osc5522_data_;            // decoded bytes of the kept type
    // Incomplete trailing base64 group (0-3 chars) carried to the next
    // DATA packet when a terminal splits the stream mid-group. Empty
    // whenever the packet ended on padding or a 4-char boundary.
    std::string osc5522_pending_;
};

} // namespace maya
