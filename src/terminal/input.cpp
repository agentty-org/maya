#include "maya/terminal/input.hpp"

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace maya {

std::atomic<std::uint64_t>& clipboard_rx_bytes() noexcept {
    static std::atomic<std::uint64_t> n{0};
    return n;
}

std::atomic<std::uint64_t>& clipboard_read_refused() noexcept {
    static std::atomic<std::uint64_t> n{0};
    return n;
}

// ============================================================================
// InputParser public methods
// ============================================================================

auto InputParser::feed(std::string_view bytes) -> std::vector<Event> {
    std::vector<Event> events;

    // The rest of a bracketed paste whose stream stalled long enough that
    // flush_timeout() already handed over the first part. Everything up to
    // its ESC[201~ continues that paste; what follows is ordinary input.
    if (paste_tail_ && state_ == State::Ground && !bytes.empty()) {
        paste_tail_ = false;
        const auto end = bytes.find("\x1b[201~");
        const auto body = bytes.substr(0, end);
        if (!body.empty()) {
            clipboard_rx_bytes().fetch_add(body.size(), std::memory_order_relaxed);
            events.emplace_back(PasteEvent{std::string(body), /*unbracketed=*/false, /*continues=*/true});
        }
        if (end == std::string_view::npos) {
            paste_tail_ = true;   // still no terminator: keep going
            return events;
        }
        bytes.remove_prefix(end + 6);
        in_burst_ = false;
        if (bytes.empty()) return events;
        auto rest = feed(bytes);
        for (auto& e : rest) events.push_back(std::move(e));
        return events;
    }

    // A terminal without bracketed paste (or one that drops the markers,
    // like some over ssh) delivers a paste as plain bytes. One read holding
    // a long run of them is not typing; nobody types that fast. Hand it over
    // as one PasteEvent so the app shows it as a paste, not thousands of
    // keystrokes dumped into the input. A big paste comes in several reads;
    // each is its own event, marked unbracketed, and the app joins them. A
    // short read right after such a burst (its tail) is part of it too.
    const auto now = clock::now();
    // The remains of a clipboard reply we gave up on (it stalled past
    // kClipboardStall): drop everything up to its DONE packet. Typing it in
    // dumped megabytes of base64 into the input box.
    if (draining_5522_ && state_ == State::Ground) {
        constexpr std::string_view kDone = "type=read:status=DONE";
        const auto at = bytes.find(kDone);
        if (at == std::string_view::npos && now - drain_since_ < kClipboardStall) return events;
        draining_5522_ = false;
        if (at != std::string_view::npos) {
            auto st = bytes.find("\x1b\\", at);
            if (st == std::string_view::npos) st = bytes.find('\x07', at);
            bytes.remove_prefix(st == std::string_view::npos ? bytes.size() : st + (bytes[st] == 0x07 ? 1 : 2));
            if (bytes.empty()) return events;
        }
    }
    // A big read right after a burst is more of it however long the reader
    // took (a busy app reads a large paste slowly). A small one only counts
    // if it came quickly, so a key typed after a paste stays a key.
    if (in_burst_ && bytes.size() < kUnbracketedPasteMin && now - last_burst_ > kBurstGap)
        in_burst_ = false;
    if (in_burst_ && now - last_burst_ > kBurstIdle && !paste_tail_) in_burst_ = false;
    // A clipboard reply whose ESC bytes were stripped on the way (seen with
    // some tmux/ssh setups) is still a reply, not typing: hand it over as
    // paste input so the app can recognise it, however short the read.
    const bool stripped_reply = bytes.find("5522;type=read:status=") != std::string_view::npos;
    if (state_ == State::Ground && !bytes.empty()
        && (bytes.size() >= kUnbracketedPasteMin || in_burst_ || stripped_reply)) {
        bool plain = true;
        for (unsigned char c : bytes)
            if (c == 0x1b || (c < 0x20 && c != '\n' && c != '\r' && c != '\t')) { plain = false; break; }
        if (plain) {
            const bool continues = in_burst_;
            // Reads come in whatever sizes the pty hands over (a 4 KB write
            // can arrive as 4095 + 1), so a short read doesn't end the burst;
            // only a quiet gap of kBurstGap does.
            in_burst_ = true;
            last_burst_ = now;
            events.emplace_back(PasteEvent{std::string(bytes), /*unbracketed=*/true, continues});
            return events;
        }
    }
    in_burst_ = false;

    for (size_t i = 0; i < bytes.size(); ++i) {
        auto ch = static_cast<uint8_t>(bytes[i]);

        switch (state_) {
        case State::Ground:
            if (ch == 0x1b) {
                // Start of escape sequence
                buf_.clear();
                buf_ += static_cast<char>(ch);
                state_ = State::Escape;
                escape_start_ = clock::now();
            } else {
                ground_byte(ch, events);
            }
            break;

        case State::Escape:
            if (ch == 0x1b) {
                // Double-ESC. Legacy altSendsEscape terminals prefix an
                // ESC to mean Alt: ESC ESC alone is Alt+Escape, and
                // ESC ESC [ A is Alt+Up. Latch alt and keep waiting for
                // the rest — do NOT append the second ESC to buf_, so
                // downstream parsers (parse_csi strips exactly "\x1b[")
                // see the canonical single-ESC form.
                //
                // The old code fell into emit_alt_key(0x1b), whose
                // ch < 0x20 remap produced Ctrl+Alt+'{' — a garbage
                // event — and then re-parsed the sequence tail as bare
                // keystrokes.
                if (pending_alt_) {
                    // Triple-ESC: resolve the pending pair as Alt+Escape,
                    // restart a fresh (single) escape with this byte.
                    events.emplace_back(KeyEvent{
                        .key = SpecialKey::Escape,
                        .mods = {.alt = true},
                        .raw_sequence = "\x1b\x1b",
                    });
                    pending_alt_ = false;
                } else {
                    pending_alt_ = true;
                }
                escape_start_ = clock::now();
                break;
            }
            if (ch >= 0x80) {
                // Alt + non-ASCII: terminals send ESC followed by the
                // key's UTF-8 bytes (Alt+é = ESC 0xC3 0xA9). Decode the
                // codepoint through the shared UTF-8 machinery with the
                // alt latch set; the completed KeyEvent carries alt=true.
                // Previously this path emitted Alt+CharKey{lead-byte}
                // plus stray invalid-byte events for the continuations.
                pending_alt_ = true;
                std::string prefix = std::move(buf_);
                buf_.clear();
                if (!begin_utf8(ch, std::move(prefix))) {
                    // Invalid lead after ESC — emit the replacement
                    // character with alt, resync to Ground.
                    events.emplace_back(KeyEvent{
                        .key = CharKey{char32_t{0xFFFD}},
                        .mods = {.alt = true},
                        .raw_sequence = std::string(1, static_cast<char>(ch)),
                    });
                    state_ = State::Ground;
                    pending_alt_ = false;
                }
                break;
            }
            buf_ += static_cast<char>(ch);
            if (ch == '[') {
                state_ = State::Csi;
            } else if (ch == 'O') {
                state_ = State::Ss3;
            } else if (ch == ']') {
                state_ = State::Osc;
                pending_alt_ = false;   // OSC replies aren't keyboard input
            } else {
                // Alt + key (emit_alt_key already sets alt; a latched
                // double-ESC collapses into the same event)
                emit_alt_key(ch, events);
                state_ = State::Ground;
                pending_alt_ = false;
            }
            break;

        case State::Csi:
            // Bound the CSI buffer. Every real CSI is a few dozen bytes at
            // most (params + intermediates + one final byte); a stream
            // that keeps feeding parameter bytes with no final (hostile
            // peer, corrupted pty) must not grow buf_ forever. On
            // overflow, abandon the sequence and return to Ground — the
            // bytes are dropped, never emitted as a corrupt event.
            if (buf_.size() >= kMaxCsiLen) {
                state_ = State::Ground;
                buf_.clear();
                pending_alt_ = false;
                break;
            }
            buf_ += static_cast<char>(ch);
            if (is_csi_final(ch)) {
                // Apply a latched double-ESC alt prefix to whatever key
                // events this CSI resolves to (ESC ESC [ A = Alt+Up).
                // Mouse / focus / paste events are left untouched — the
                // latch is a keyboard-modifier convention only.
                const std::size_t before = events.size();
                const int acks_before = acks_;
                parse_csi(events);
                // ...unless the CSI was a REPORT (the per-frame ack,
                // `CSI 0 n`). A report is the terminal talking, not a key,
                // so the ESC before it was never a prefix: it was the
                // Escape key, pressed while an ack was on its way. Every
                // frame sends a DSR, so this race lost nearly every Escape.
                if (pending_alt_ && acks_ != acks_before && events.size() == before) {
                    events.emplace_back(KeyEvent{.key = SpecialKey::Escape, .raw_sequence = "\x1b"});
                } else if (pending_alt_) {
                    for (std::size_t k = before; k < events.size(); ++k)
                        if (auto* ke = std::get_if<KeyEvent>(&events[k]))
                            ke->mods.alt = true;
                }
                if (state_ == State::Csi)
                    state_ = State::Ground;
                pending_alt_ = false;
            }
            // Intermediate and parameter bytes: keep accumulating
            break;

        case State::Ss3: {
            buf_ += static_cast<char>(ch);
            const std::size_t before = events.size();
            parse_ss3(ch, events);
            if (pending_alt_) {
                for (std::size_t k = before; k < events.size(); ++k)
                    if (auto* ke = std::get_if<KeyEvent>(&events[k]))
                        ke->mods.alt = true;
            }
            state_ = State::Ground;
            pending_alt_ = false;
            break;
        }

        case State::Osc:
            // Bound the OSC buffer. A well-behaved terminal terminates an
            // OSC promptly; an unterminated / runaway one (or a hostile
            // peer streaming megabytes with no ST) must not grow buf_
            // without limit. On overflow, abandon the sequence and
            // return to Ground — the partial OSC is dropped, never
            // emitted as a (truncated, corrupt) clipboard event.
            if (buf_.size() >= kMaxOscLen) {
                state_ = State::Ground;
                buf_.clear();
                break;
            }
            buf_ += static_cast<char>(ch);
            // A frame's DSR reply (ESC[0n) can land inside a long OSC reply
            // (a clipboard image streaming in): it's an ack, not OSC data.
            // Left in, its ESC breaks the reply and the rest of the image
            // arrives as typed text.
            if (ch == 'n' && buf_.size() >= 4) {
                const auto tail = std::string_view(buf_).substr(buf_.size() - 4);
                if (tail == "\x1b[0n" || tail == "\x1b[3n") {
                    buf_.resize(buf_.size() - 4);
                    ++acks_;
                    break;
                }
            }
            // OSC is terminated by BEL (0x07) or ST (ESC \)
            if (ch == 0x07) {
                parse_osc(events);
                state_ = State::Ground;
            } else if (buf_.size() >= 2
                       && buf_[buf_.size() - 2] == '\x1b'
                       && ch == '\\') {
                parse_osc(events);
                state_ = State::Ground;
            }
            break;

        case State::Utf8:
            if ((ch & 0xC0) == 0x80) {
                // Valid continuation byte
                utf8_cp_ = (utf8_cp_ << 6) | (ch & 0x3F);
                utf8_raw_ += static_cast<char>(ch);
                if (--utf8_remaining_ == 0) {
                    // Structural validation before emitting. A malformed
                    // encoder (or hostile peer) can produce sequences
                    // that decode "successfully" to codepoints UTF-8
                    // forbids; launder them all to U+FFFD:
                    //   - overlong: cp below the lead byte's minimum
                    //     (classic smuggling vector — 0xC0 0xAF → '/')
                    //   - surrogate halves D800–DFFF (not scalar values)
                    //   - above U+10FFFF
                    const bool valid =
                        utf8_cp_ >= utf8_min_
                        && utf8_cp_ <= 0x10FFFF
                        && !(utf8_cp_ >= 0xD800 && utf8_cp_ <= 0xDFFF);
                    events.emplace_back(KeyEvent{
                        .key = CharKey{valid ? utf8_cp_
                                             : char32_t{0xFFFD}},
                        .mods = {.alt = pending_alt_},
                        .raw_sequence = std::move(utf8_raw_),
                    });
                    state_ = State::Ground;
                    pending_alt_ = false;
                }
            } else {
                // Invalid continuation — the partial sequence is
                // malformed. Emit U+FFFD for it (the old code dropped
                // the accumulated bytes silently) and resync on this
                // byte, which may be the start of something valid.
                events.emplace_back(KeyEvent{
                    .key = CharKey{char32_t{0xFFFD}},
                    .mods = {.alt = pending_alt_},
                    .raw_sequence = std::move(utf8_raw_),
                });
                state_ = State::Ground;
                pending_alt_ = false;
                --i; // reprocess this byte
            }
            break;

        case State::BracketedPaste:
            buf_ += static_cast<char>(ch);
            // The screen asks for a DSR after every frame (flow control), and
            // a frame drawn while a long paste streams in puts the terminal's
            // reply (ESC[0n) in the middle of the paste bytes. It is an ack,
            // not paste: take it out, or it corrupts the paste (an image sent
            // as text no longer decodes).
            if (ch == 'n' && buf_.size() >= 4) {
                const auto tail = std::string_view(buf_).substr(buf_.size() - 4);
                if (tail == "\x1b[0n" || tail == "\x1b[3n") {
                    buf_.resize(buf_.size() - 4);
                    ++acks_;
                    break;
                }
            }
            // Look for the terminator: ESC [201~
            if (buf_.size() >= 6) {
                auto tail = std::string_view(buf_).substr(buf_.size() - 6);
                if (tail == "\x1b[201~") {
                    // Emit paste content (strip the terminator)
                    auto content = buf_.substr(0, buf_.size() - 6);
                    // Progress: see note at the OSC 52 site. The host
                    // re-arms its no-reply timer whenever this MOVES, so a
                    // route that never bumps it reads as terminal silence.
                    clipboard_rx_bytes().fetch_add(content.size() + 1,
                                                   std::memory_order_relaxed);
                    events.emplace_back(PasteEvent{std::move(content)});
                    buf_.clear();
                    state_ = State::Ground;
                    break;
                }
            }
            // Bound the paste accumulation: a paste whose ESC[201~
            // terminator never arrives (dropped bytes, a hostile peer)
            // must not grow buf_ forever. Past the cap, ship what
            // arrived as a PasteEvent CHUNK and stay in BracketedPaste
            // — an oversized paste streams out as multiple events
            // rather than one truncated event plus garbage keystrokes.
            //
            // Crucially, withhold any buffer tail that is a proper
            // prefix of the terminator: if the cap lands mid-"\x1b[201~",
            // a naive flush would ship half the terminator inside the
            // paste content and re-parse the other half as keystrokes
            // (paste injection). The ≤5 withheld bytes complete (or
            // fail) the match against subsequent input as usual.
            if (buf_.size() >= kMaxOscLen) {
                static constexpr std::string_view kTerm = "\x1b[201~";
                std::size_t keep = 0;
                for (std::size_t k = kTerm.size() - 1; k > 0; --k) {
                    if (buf_.size() >= k
                        && std::string_view(buf_).substr(buf_.size() - k)
                               == kTerm.substr(0, k)) {
                        keep = k;
                        break;
                    }
                }
                std::string chunk = buf_.substr(0, buf_.size() - keep);
                buf_.erase(0, buf_.size() - keep);
                // Progress. THIS is the streaming site — a paste larger than
                // the buffer arrives as many chunks over many reads, which
                // is exactly the transfer the host must not mistake for
                // silence while it is still running.
                clipboard_rx_bytes().fetch_add(chunk.size() + 1,
                                               std::memory_order_relaxed);
                events.emplace_back(PasteEvent{std::move(chunk)});
            }
            break;
        }
    }

    // Inactivity clock for flush_timeout(): any partial sequence still
    // pending after this feed measures its timeout from the LAST byte
    // that arrived, so a slowly-streaming (but live) sequence is never
    // chopped mid-flight — only a genuinely stalled one.
    if (state_ != State::Ground && !bytes.empty())
        escape_start_ = clock::now();

    return events;
}

auto InputParser::flush_timeout() -> std::vector<Event> {
    std::vector<Event> events;
    if (state_ == State::Ground) return events;

    const auto elapsed = clock::now() - escape_start_;

    // Return-to-ground hygiene shared by every branch below.
    auto to_ground = [&] {
        state_ = State::Ground;
        buf_.clear();
        utf8_raw_.clear();
        utf8_remaining_ = 0;
        pending_alt_ = false;
    };

    switch (state_) {
    case State::Escape:
        // The classic lone-ESC disambiguation: no second byte arrived, so
        // it really was the Escape key (Alt+Escape after a double-ESC).
        //
        // Except mid clipboard reply. An OSC 5522 image reply is a train of
        // packets, and over ssh a read can end right after the ESC that
        // starts the next one, with the rest landing well after 50 ms.
        // Taking that ESC as the Escape key throws the parser out of the
        // reply and the rest of the image arrives as thousands of typed
        // characters. While a reply is open, wait as long as for any OSC.
        if (elapsed >= (osc5522_active_ ? kClipboardStall : escape_timeout_)) {
            if (osc5522_active_) {
                // The image reply died mid packet: no Escape key, just drop
                // what's left of it.
                draining_5522_ = true;
                drain_since_ = clock::now();
                to_ground();
                break;
            }
            events.emplace_back(KeyEvent{
                .key = SpecialKey::Escape,
                .mods = {.alt = pending_alt_},
                .raw_sequence = std::move(buf_),
            });
            to_ground();
        }
        break;

    case State::Csi:
    case State::Ss3:
        // Truncated control sequence (dropped bytes on a flaky pty /
        // SSH / tmux). Abandon it — emitting a guess would be worse.
        // Without this rescue the FSM wedged here FOREVER: has_pending()
        // kept the event loop waking every 16ms, and the user's next
        // real keystrokes were swallowed as CSI parameter bytes until
        // one happened to land in the final-byte range 0x40–0x7E.
        if (elapsed >= (osc5522_active_ ? kClipboardStall : escape_timeout_)) to_ground();
        break;

    case State::Utf8:
        // Truncated codepoint. Surface U+FFFD so the byte loss is
        // visible (and any latched Alt isn't silently dropped), resync.
        if (elapsed >= escape_timeout_) {
            events.emplace_back(KeyEvent{
                .key = CharKey{char32_t{0xFFFD}},
                .mods = {.alt = pending_alt_},
                .raw_sequence = std::move(utf8_raw_),
            });
            to_ground();
        }
        break;

    case State::Osc:
        // OSC replies (clipboard reads) can be megabytes streaming over
        // a slow link, so the deadline is generous — and it's an
        // INACTIVITY deadline (feed() refreshes escape_start_ on every
        // chunk), so only a truly dead transfer is abandoned. Mid image
        // reply over ssh a packet can stall for seconds; give up then and
        // its tail would arrive as typed text.
        if (elapsed >= (osc5522_active_ ? kClipboardStall : kOscTimeout)) {
            if (osc5522_active_) { draining_5522_ = true; drain_since_ = clock::now(); }
            to_ground();
        }
        break;

    case State::BracketedPaste:
        // Lost terminator. Flush what arrived as a best-effort paste —
        // for a real (huge but healthy) paste the inactivity clock keeps
        // resetting, so this only fires when the stream truly died.
        if (elapsed >= kPasteTimeout) {
            if (!buf_.empty()) {
                clipboard_rx_bytes().fetch_add(buf_.size() + 1,
                                               std::memory_order_relaxed);
                events.emplace_back(PasteEvent{std::move(buf_)});
            }
            to_ground();
            // The stream may only have stalled (a slow ssh link): if more
            // arrives, it is the rest of this paste, not typing, and its
            // ESC[201~ must not leak. paste_tail_ makes feed() hand it on
            // as a continuation.
            paste_tail_ = true;
            in_burst_ = true;
            last_burst_ = clock::now();
        }
        break;

    case State::Ground:
        break;
    }

    return events;
}

bool InputParser::has_pending() const noexcept {
    return state_ != State::Ground;
}

void InputParser::reset() noexcept {
    state_ = State::Ground;
    buf_.clear();
    pending_alt_ = false;
    in_burst_ = false;
    paste_tail_ = false;
    draining_5522_ = false;
    utf8_remaining_ = 0;
    utf8_raw_.clear();
    osc5522_active_ = false;
    osc5522_locked_ = false;
    osc5522_mime_.clear();
    osc5522_data_.clear();
    osc5522_pending_.clear();
}

// ============================================================================
// InputParser private methods
// ============================================================================

bool InputParser::begin_utf8(uint8_t ch, std::string prefix) {
    // Classify the lead byte, rejecting values that can only produce
    // ill-formed UTF-8 (RFC 3629 table):
    //   0x80–0xBF  stray continuation (no lead)
    //   0xC0/0xC1  2-byte lead whose every decoding is overlong
    //   0xF5–0xFF  4-byte lead beyond U+10FFFF (or 5/6-byte legacy)
    // utf8_min_ records the smallest codepoint the accepted lead may
    // encode; the completion path rejects anything below it (overlong)
    // plus surrogates — so both halves of validation are structural.
    int      len;
    char32_t min;
    if ((ch & 0xE0) == 0xC0)      { len = 2; min = 0x80; }
    else if ((ch & 0xF0) == 0xE0) { len = 3; min = 0x800; }
    else if ((ch & 0xF8) == 0xF0) { len = 4; min = 0x10000; }
    else return false;              // stray continuation or 0xF8+
    if (ch < 0xC2) return false;    // 0xC0/0xC1: overlong by construction
    if (ch > 0xF4) return false;    // beyond U+10FFFF by construction

    utf8_cp_        = ch & (0xFF >> (len + 1));
    utf8_remaining_ = static_cast<uint8_t>(len - 1);
    utf8_min_       = min;
    utf8_raw_       = std::move(prefix);
    utf8_raw_      += static_cast<char>(ch);
    state_          = State::Utf8;
    return true;
}

void InputParser::ground_byte(uint8_t ch, std::vector<Event>& events) {
    if (ch == '\r' || ch == '\n') {
        events.emplace_back(KeyEvent{
            .key = SpecialKey::Enter,
            .mods = {},
            .raw_sequence = std::string(1, static_cast<char>(ch)),
        });
    } else if (ch == '\t') {
        events.emplace_back(KeyEvent{
            .key = SpecialKey::Tab,
            .mods = {},
            .raw_sequence = "\t",
        });
    } else if (ch == 0x7f) {
        events.emplace_back(KeyEvent{
            .key = SpecialKey::Backspace,
            .mods = {},
            .raw_sequence = std::string(1, static_cast<char>(ch)),
        });
    } else if (ch == 0x08) {
        events.emplace_back(KeyEvent{
            .key = SpecialKey::Backspace,
            .mods = {.ctrl = true},
            .raw_sequence = std::string(1, static_cast<char>(ch)),
        });
    } else if (ch == 0x1f) {
        events.emplace_back(KeyEvent{
            .key = CharKey{U'/'},
            .mods = {.ctrl = true},
            .raw_sequence = std::string(1, static_cast<char>(ch)),
        });
    } else if (ch < 0x20) {
        // Ctrl + letter (Ctrl-A = 0x01, Ctrl-Z = 0x1a)
        char32_t letter = static_cast<char32_t>(ch + 'a' - 1);
        events.emplace_back(KeyEvent{
            .key = CharKey{letter},
            .mods = {.ctrl = true},
            .raw_sequence = std::string(1, static_cast<char>(ch)),
        });
    } else if (ch < 0x80) {
        // Printable ASCII
        events.emplace_back(KeyEvent{
            .key = CharKey{static_cast<char32_t>(ch)},
            .mods = {},
            .raw_sequence = std::string(1, static_cast<char>(ch)),
        });
    } else if (!begin_utf8(ch, {})) {
        // Invalid UTF-8 lead (stray continuation, overlong 0xC0/0xC1,
        // 0xF5+). Strict decoder: emit U+FFFD rather than laundering
        // the raw byte into a bogus codepoint — the old fallback
        // emitted CharKey{ch}, which smuggled non-characters into
        // hosts' text handling.
        events.emplace_back(KeyEvent{
            .key = CharKey{char32_t{0xFFFD}},
            .mods = {},
            .raw_sequence = std::string(1, static_cast<char>(ch)),
        });
    }
}

void InputParser::emit_alt_key(uint8_t ch, std::vector<Event>& events) {
    KeyEvent ev{
        .key = CharKey{static_cast<char32_t>(ch)},
        .mods = {.alt = true},
        .raw_sequence = std::move(buf_),
    };

    // Remap specific keys
    if (ch == '\r' || ch == '\n') {
        ev.key = SpecialKey::Enter;
    } else if (ch == 0x7f) {
        ev.key = SpecialKey::Backspace;
    } else if (ch < 0x20) {
        ev.key  = CharKey{static_cast<char32_t>(ch + 'a' - 1)};
        ev.mods.ctrl = true;
    }

    events.emplace_back(std::move(ev));
}

void InputParser::parse_csi(std::vector<Event>& events) {
    // Strip ESC [ prefix
    std::string_view seq(buf_);
    seq.remove_prefix(2); // skip \x1b[

    if (seq.empty()) return;

    uint8_t final_byte = static_cast<uint8_t>(seq.back());
    std::string_view params_str = seq.substr(0, seq.size() - 1);

    // ── Terminal REPORTS are answers, not keystrokes ──────────────────
    //
    // A private-mode CSI (`CSI ? … <final>`) is something the terminal said
    // back to us: DA1 (`CSI ? 1;2 c`), DECRPM (`CSI ? 5522;1 $ y`), DECXCPR,
    // and friends. They are solicited during startup probing, but a reply
    // can arrive AFTER its probe window closes — trivially so over ssh,
    // where a reply fragments and the tail lands milliseconds late, or when
    // the terminal is slow enough that the deadline expires first.
    //
    // Such a late reply used to fall through to the "unknown sequence"
    // fallback at the bottom of this function, which emits a literal '?'
    // CharKey. The user sees a composer that starts with a stray `?` they
    // never typed, and — because the reply also carries the parameter bytes
    // as further unknown input — a burst of spurious key events that make
    // the caret jitter. Both symptoms, one cause.
    //
    // Reports are unconditionally CONSUMED: this parser has no consumer for
    // them (the startup probe reads them off the fd directly, before the
    // parser is in the loop), so the only correct thing to do with a late
    // one is drop it. Silence is the contract — never invent input the user
    // did not type.
    if (!params_str.empty() && params_str.front() == '?') {
        buf_.clear();
        return;
    }

    // `CSI 0 n`: the terminal's answer to our per-frame DSR (`CSI 5 n`),
    // i.e. a frame acknowledgement. Counted for the screen's flow control,
    // and consumed: it's a report, not a key. (`CSI 3 n` would mean the
    // terminal is malfunctioning; it still proves the frame was parsed.)
    if (final_byte == 'n' && (params_str == "0" || params_str == "3")) {
        ++acks_;
        buf_.clear();
        return;
    }

    // Check for bracketed paste start
    if (final_byte == '~') {
        auto params = parse_params(params_str);
        if (!params.empty()) {
            int code = params[0];
            if (code == 200) {
                // Bracketed paste start
                state_ = State::BracketedPaste;
                buf_.clear();
                return;
            }
            // xterm modifyOtherKeys=2: CSI 27 ; <mods> ; <keycode> ~
            // The sentinel `27` (ESC code) marks this as a modified-key
            // report; the real codepoint is in params[2]. Used for keys
            // whose plain encoding lacks modifier info — Shift+Enter,
            // Ctrl+Enter, etc — on terminals that don't speak KKP.
            if (code == 27 && params.size() >= 3) {
                int mod_param = params[1];
                int keycode   = params[2];
                Modifiers mods;
                if (mod_param > 1) mods = Modifiers::from_param(mod_param);
                std::optional<Key> k;
                switch (keycode) {
                    case 9:   k = SpecialKey::Tab;       break;
                    case 13:  k = SpecialKey::Enter;     break;
                    case 27:  k = SpecialKey::Escape;    break;
                    case 127: k = SpecialKey::Backspace; break;
                    default:
                        if (keycode >= 32 && keycode < 0x110000)
                            k = CharKey{static_cast<char32_t>(keycode)};
                        break;
                }
                if (k) {
                    events.emplace_back(KeyEvent{
                        .key = *k,
                        .mods = mods,
                        .raw_sequence = std::move(buf_),
                    });
                }
                return;
            }
            // Tilde-terminated special keys
            handle_tilde_key(code, params.size() > 1 ? params[1] : 1, events);
            return;
        }
    }

    // Focus events: CSI I (focus in) and CSI O (focus out)
    if (final_byte == 'I') {
        events.emplace_back(FocusEvent{.focused = true});
        return;
    }
    if (final_byte == 'O') {
        events.emplace_back(FocusEvent{.focused = false});
        return;
    }

    // SGR mouse: CSI < Pb ; Px ; Py M/m
    if (!params_str.empty() && params_str[0] == '<'
        && (final_byte == 'M' || final_byte == 'm'))
    {
        parse_sgr_mouse(params_str.substr(1), final_byte, events);
        return;
    }

    // Arrow keys, Home, End with optional modifiers
    // CSI [modifier] A/B/C/D/H/F
    if ((final_byte >= 'A' && final_byte <= 'F') || final_byte == 'H') {
        auto params = parse_params(params_str);
        Modifiers mods;
        if (params.size() >= 2 && params[1] > 1) {
            mods = Modifiers::from_param(params[1]);
        }

        std::optional<SpecialKey> sk;
        switch (final_byte) {
            case 'A': sk = SpecialKey::Up;    break;
            case 'B': sk = SpecialKey::Down;  break;
            case 'C': sk = SpecialKey::Right; break;
            case 'D': sk = SpecialKey::Left;  break;
            case 'H': sk = SpecialKey::Home;  break;
            case 'F': sk = SpecialKey::End;   break;
        }

        if (sk) {
            events.emplace_back(KeyEvent{
                .key = *sk,
                .mods = mods,
                .raw_sequence = std::move(buf_),
            });
        }
        return;
    }

    // Shift-Tab: CSI Z
    if (final_byte == 'Z') {
        events.emplace_back(KeyEvent{
            .key = SpecialKey::BackTab,
            .mods = {.shift = true},
            .raw_sequence = std::move(buf_),
        });
        return;
    }

    // CSI u -- Kitty / Unicode keyboard protocol: ESC [ <codepoint> ; <mods> u
    // Encodes any Unicode codepoint with unambiguous modifier flags.
    if (final_byte == 'u') {
        auto params = parse_params(params_str);
        if (!params.empty()) {
            int codepoint = params[0];
            Modifiers mods;
            if (params.size() >= 2 && params[1] > 1) {
                mods = Modifiers::from_param(params[1]);
            }

            std::optional<Key> key;
            switch (codepoint) {
                // Shift+Tab must surface as BackTab (as the legacy `CSI Z`
                // path does) so callers that special-case BackTab keep
                // working when the kitty protocol is active.
                case 9:   key = mods.shift ? SpecialKey::BackTab
                                           : SpecialKey::Tab;       break;
                case 13:  key = SpecialKey::Enter;     break;
                case 27:  key = SpecialKey::Escape;    break;
                case 127: key = SpecialKey::Backspace; break;
                // Function keys encoded as codepoints >= 57344 (Kitty extension)
                case 57344: key = SpecialKey::F1;  break;
                case 57345: key = SpecialKey::F2;  break;
                case 57346: key = SpecialKey::F3;  break;
                case 57347: key = SpecialKey::F4;  break;
                case 57348: key = SpecialKey::F5;  break;
                case 57349: key = SpecialKey::F6;  break;
                case 57350: key = SpecialKey::F7;  break;
                case 57351: key = SpecialKey::F8;  break;
                case 57352: key = SpecialKey::F9;  break;
                case 57353: key = SpecialKey::F10; break;
                case 57354: key = SpecialKey::F11; break;
                case 57355: key = SpecialKey::F12; break;
                // Arrow keys / navigation
                case 57356: key = SpecialKey::Up;       break;
                case 57357: key = SpecialKey::Down;     break;
                case 57358: key = SpecialKey::Left;     break;
                case 57359: key = SpecialKey::Right;    break;
                case 57360: key = SpecialKey::Home;     break;
                case 57361: key = SpecialKey::End;      break;
                case 57362: key = SpecialKey::PageUp;   break;
                case 57363: key = SpecialKey::PageDown; break;
                case 57364: key = SpecialKey::Insert;   break;
                case 57365: key = SpecialKey::Delete;   break;
                default:
                    if (codepoint >= 32 && codepoint < 0x110000) {
                        key = CharKey{static_cast<char32_t>(codepoint)};
                    }
                    break;
            }

            if (key) {
                events.emplace_back(KeyEvent{
                    .key = *key,
                    .mods = mods,
                    .raw_sequence = std::move(buf_),
                });
            }
            return;
        }
    }

    // Fallback: emit as unknown key event with raw sequence
    events.emplace_back(KeyEvent{
        .key = CharKey{U'?'},
        .mods = {},
        .raw_sequence = std::move(buf_),
    });
}

void InputParser::handle_tilde_key(int code, int modifier_param, std::vector<Event>& events) {
    Modifiers mods;
    if (modifier_param > 1) {
        mods = Modifiers::from_param(modifier_param);
    }

    std::optional<SpecialKey> sk;
    switch (code) {
        case 1:  sk = SpecialKey::Home;   break;
        case 2:  sk = SpecialKey::Insert;  break;
        case 3:  sk = SpecialKey::Delete;  break;
        case 4:  sk = SpecialKey::End;     break;
        case 5:  sk = SpecialKey::PageUp;  break;
        case 6:  sk = SpecialKey::PageDown; break;
        case 11: sk = SpecialKey::F1;      break;
        case 12: sk = SpecialKey::F2;      break;
        case 13: sk = SpecialKey::F3;      break;
        case 14: sk = SpecialKey::F4;      break;
        case 15: sk = SpecialKey::F5;      break;
        case 17: sk = SpecialKey::F6;      break;
        case 18: sk = SpecialKey::F7;      break;
        case 19: sk = SpecialKey::F8;      break;
        case 20: sk = SpecialKey::F9;      break;
        case 21: sk = SpecialKey::F10;     break;
        case 23: sk = SpecialKey::F11;     break;
        case 24: sk = SpecialKey::F12;     break;
        case 201: return; // Bracketed paste end (handled elsewhere)
        default: return;
    }

    if (sk) {
        events.emplace_back(KeyEvent{
            .key = *sk,
            .mods = mods,
            .raw_sequence = std::move(buf_),
        });
    }
}

void InputParser::parse_sgr_mouse(std::string_view params_str, uint8_t final_byte,
                                   std::vector<Event>& events)
{
    auto params = parse_params(params_str);
    if (params.size() < 3) return;

    int cb = params[0];
    int px = params[1];
    int py = params[2];

    // SGR mouse coordinates are 1-BASED (column/row 1 is the top-left cell).
    // A malformed report like `\x1b[<0;0;0m` yields 0, which downstream code
    // that converts to 0-based (px-1) would turn into -1 and use to index a
    // cell grid. Clamp to the valid floor at the trust boundary so no consumer
    // ever sees a sub-1 coordinate. (No upper clamp: the terminal size isn't
    // known here, and consumers already bound against the live viewport.)
    if (px < 1) px = 1;
    if (py < 1) py = 1;

    // Decode button and modifiers from cb
    Modifiers mods{
        .ctrl  = (cb & 16) != 0,
        .alt   = (cb & 8)  != 0,
        .shift = (cb & 4)  != 0,
    };

    int button_bits = cb & 0x43; // bits 0-1 and 6
    bool is_motion  = (cb & 32) != 0;

    MouseButton button   = MouseButton::None;
    MouseEventKind kind  = MouseEventKind::Press;

    if (final_byte == 'm') {
        kind = MouseEventKind::Release;
    } else if (is_motion) {
        kind = MouseEventKind::Move;
    }

    // Decode button. In SGR, a DRAG report is the held button's code with the
    // motion bit (32) set, so cb=32 (button_bits==0 + motion) is a genuine
    // LEFT-button drag and must decode as Left. A BUTTONLESS move is reported
    // as button code 3 (button_bits==3), which maps to None below — so no
    // special-casing is needed here; the plain table is correct.
    if (button_bits == 0)       button = MouseButton::Left;
    else if (button_bits == 1)  button = MouseButton::Middle;
    else if (button_bits == 2)  button = MouseButton::Right;
    else if (button_bits == 3)  button = MouseButton::None; // release / buttonless move
    else if (button_bits == 64) button = MouseButton::ScrollUp;
    else if (button_bits == 65) button = MouseButton::ScrollDown;
    else if (button_bits == 66) button = MouseButton::ScrollLeft;   // SGR horizontal wheel
    else if (button_bits == 67) button = MouseButton::ScrollRight;

    events.emplace_back(MouseEvent{
        .button = button,
        .kind   = kind,
        .x      = Columns{px},
        .y      = Rows{py},
        .mods   = mods,
    });
}

void InputParser::parse_ss3(uint8_t ch, std::vector<Event>& events) {
    std::optional<SpecialKey> sk;
    switch (ch) {
        case 'P': sk = SpecialKey::F1;   break;
        case 'Q': sk = SpecialKey::F2;   break;
        case 'R': sk = SpecialKey::F3;   break;
        case 'S': sk = SpecialKey::F4;   break;
        case 'A': sk = SpecialKey::Up;   break;
        case 'B': sk = SpecialKey::Down;  break;
        case 'C': sk = SpecialKey::Right; break;
        case 'D': sk = SpecialKey::Left;  break;
        case 'H': sk = SpecialKey::Home;  break;
        case 'F': sk = SpecialKey::End;   break;
        default:  break;
    }

    if (sk) {
        events.emplace_back(KeyEvent{
            .key = *sk,
            .mods = {},
            .raw_sequence = std::move(buf_),
        });
    }
}

void InputParser::parse_osc([[maybe_unused]] std::vector<Event>& events) {
    // buf_ holds the whole sequence including the leading "\x1b]" and the
    // trailing terminator (BEL, or ESC \). Strip both ends to the OSC
    // body "<Ps>;<Pt>".
    std::string_view body{buf_};
    if (body.size() >= 2 && body[0] == '\x1b' && body[1] == ']')
        body.remove_prefix(2);
    if (!body.empty() && body.back() == '\x07') {
        body.remove_suffix(1);                      // BEL
    } else if (body.size() >= 2
               && body[body.size() - 2] == '\x1b'
               && body.back() == '\\') {
        body.remove_suffix(2);                      // ST (ESC \)
    }

    // OSC 5522 — kitty's multi-format clipboard protocol. The read
    // reply is a PACKET SEQUENCE (OK / DATA… / DONE), reassembled by
    // parse_osc5522 into a single PasteEvent. This is the only escape
    // path that carries IMAGE bytes across an SSH pty.
    if (body.size() >= 5 && body.substr(0, 5) == "5522;") {
        parse_osc5522(body.substr(5), events);
        return;
    }

    // OSC 52 — clipboard. A READ response looks like
    //   52 ; <selection> ; <base64-of-clipboard-bytes>
    // The terminal emulator (running on the user's local machine, even
    // when the app is on the far end of an SSH pty) reads its OWN system
    // clipboard and ships the bytes back inline. This is the ONLY
    // clipboard path that crosses an SSH link with no remote tool and no
    // env var — the whole point of decoding it here. We surface the
    // decoded bytes as a PasteEvent so hosts handle a clipboard reply
    // through the exact same path as a bracketed paste (text OR, after
    // a magic-byte sniff at the host, an image).
    //
    // All other OSC replies (title query, color report, cursor shape,
    // …) remain unhandled — silently dropped as before.
    if (body.size() < 3 || body[0] != '5' || body[1] != '2' || body[2] != ';')
        return;
    body.remove_prefix(3);                          // past "52;"

    // Skip the selection field (c / p / s / q / …) up to the next ';'.
    auto semi = body.find(';');
    if (semi == std::string_view::npos) return;
    std::string_view b64 = body.substr(semi + 1);

    // A terminal with no clipboard data (or that refuses the read)
    // answers with an empty or "?" payload. Nothing to deliver.
    if (b64.empty() || b64 == "?") return;

    if (auto decoded = decode_base64(b64)) {
        // Report progress like the OSC 5522 path does. The host arms a
        // no-reply timer and re-arms it whenever this counter MOVES; a
        // route that never bumps it is indistinguishable from a terminal
        // that stayed silent, so a large OSC 52 reply arriving over a slow
        // link gets diagnosed as "your terminal didn't answer" while its
        // bytes are still landing. One bump per delivered reply is enough:
        // the host only tests the counter for movement, never for a size.
        clipboard_rx_bytes().fetch_add(decoded->size() + 1,
                                       std::memory_order_relaxed);
        events.emplace_back(PasteEvent{std::move(*decoded)});
    }
    // Malformed base64 — drop silently; a corrupt half-paste is worse
    // than no paste.
}

void InputParser::parse_osc5522(std::string_view body,
                                std::vector<Event>& events) {
    // `body` is the OSC payload past "5522;":
    //   type=read:status=OK
    //   type=read:status=DATA:mime=<b64-mime>;<b64-chunk>
    //   type=read:status=DONE
    // metadata is colon-separated key=value pairs; the data payload (if
    // any) follows the first ';'. Anything that isn't a read reply
    // (write acks, paste events, unknown types) is ignored — we only
    // ever ISSUE read requests, so nothing else should arrive, but a
    // multiplexer could leak another client's packets.
    auto abort_transfer = [&] {
        osc5522_active_ = false;
        osc5522_locked_ = false;
        osc5522_mime_.clear();
        osc5522_data_.clear();
        osc5522_data_.shrink_to_fit();
        osc5522_pending_.clear();
    };

    std::string_view metadata = body;
    std::string_view payload;
    if (auto semi = body.find(';'); semi != std::string_view::npos) {
        metadata = body.substr(0, semi);
        payload  = body.substr(semi + 1);
    }

    // Walk the colon-separated key=value metadata.
    std::string_view type, status, mime_b64;
    for (std::string_view rest = metadata; !rest.empty();) {
        auto colon = rest.find(':');
        std::string_view kv = rest.substr(0, colon);
        rest = (colon == std::string_view::npos)
                   ? std::string_view{} : rest.substr(colon + 1);
        auto eq = kv.find('=');
        if (eq == std::string_view::npos) continue;
        auto key = kv.substr(0, eq);
        auto val = kv.substr(eq + 1);
        if      (key == "type")   type     = val;
        else if (key == "status") status   = val;
        else if (key == "mime")   mime_b64 = val;
    }

    if (type != "read") return;

    if (status == "OK") {
        // Fresh transfer. A dangling previous transfer (terminal died
        // mid-stream) is superseded.
        abort_transfer();
        osc5522_active_ = true;
        return;
    }

    if (!osc5522_active_) return;   // stray packet outside a transfer

    if (status == "DATA") {
        auto mime = decode_base64(mime_b64);
        if (!mime) { abort_transfer(); return; }

        // Preference: the request lists images before text (see
        // ansi::request_clipboard_image), and kitty replies in request
        // order — but don't rely on it. Rank explicitly and keep the
        // best type seen; all chunks of one type arrive sequentially,
        // so switching types can discard the old accumulation whole.
        auto rank = [](std::string_view m) -> int {
            if (m == "image/png")  return 5;
            if (m == "image/jpeg") return 4;
            if (m == "image/webp") return 3;
            if (m == "image/gif")  return 2;
            if (m.substr(0, 6) == "image/") return 1;
            return 0;   // text/plain and anything else
        };
        if (*mime != osc5522_mime_) {
            if (osc5522_locked_ && rank(*mime) <= rank(osc5522_mime_))
                return;             // keeping the better earlier type
            osc5522_mime_ = std::move(*mime);
            osc5522_data_.clear();
            osc5522_locked_ = true;
        }

        // CHUNK REASSEMBLY. Two terminal behaviours must both work, and
        // they pull in opposite directions:
        //
        //  (a) kitty pads EVERY DATA packet independently (≤4096 raw bytes
        //      each, its own trailing '='). Concatenating the base64 text
        //      and decoding once then stops at the FIRST packet's padding
        //      — a 100 KB image decodes to exactly 4096 bytes.
        //  (b) a terminal may split the stream mid-group, so a packet ends
        //      on a non-multiple of 4. Decoding that packet alone drops the
        //      leftover bits, losing 1-2 bytes at every seam — which the
        //      PNG header survives, so the image keeps its type, size and
        //      dimensions and merely renders solid black.
        //
        // Handling only (b) breaks (a) and vice versa. The rule that
        // satisfies both: a packet ENDING IN PADDING is self-contained —
        // decode it and flush. Otherwise keep only the incomplete tail
        // group (0-3 chars) and prepend it to the next packet, so a seam
        // inside a group is repaired without ever crossing a '='.
        //
        // Bound the pending buffer in base64 units: kMaxOscLen bounds the
        // DECODED payload, 4 chars carry 3 bytes, +4 for a final group.
        constexpr std::size_t kMaxB64 = kMaxOscLen / 3 * 4 + 4;
        // Strip the ASCII whitespace terminals insert to wrap long replies
        // HERE, not at decode time: it must not consume budget, and
        // removing it keeps the pending text a pure base64 stream whose
        // length is meaningful for the alignment test below.
        std::string text;
        text.reserve(payload.size());
        for (char ch : payload) {
            if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') continue;
            text.push_back(ch);
        }
        if (text.empty()) return;

        if (osc5522_pending_.size() + text.size() > kMaxB64) {
            abort_transfer();
            return;
        }
        osc5522_pending_ += text;

        // Decode as much as is unambiguously complete. Padding closes the
        // stream for this packet; otherwise round down to whole groups.
        const bool padded = osc5522_pending_.back() == '=';
        const std::size_t take =
            padded ? osc5522_pending_.size()
                   : osc5522_pending_.size() - (osc5522_pending_.size() % 4);
        if (take > 0) {
            auto decoded =
                decode_base64(std::string_view(osc5522_pending_).substr(0, take));
            if (!decoded) { abort_transfer(); return; }
            if (osc5522_data_.size() + decoded->size() > kMaxOscLen) {
                abort_transfer();
                return;
            }
            osc5522_data_ += *decoded;
            osc5522_pending_.erase(0, take);
        }

        // Publish progress. A clipboard IMAGE is base64 PNG — hundreds of KB
        // to megabytes — and over ssh+tmux it arrives in many chunks over
        // well past a second. The host arms a short timer to diagnose "the
        // terminal never answered", and without this it cannot tell silence
        // from a transfer still streaming: it declares failure at the
        // deadline while the bytes are mid-flight, and a big screenshot can
        // never be pasted. Counting delivered bytes lets the host see
        // progress and keep waiting.
        clipboard_rx_bytes().fetch_add(text.size(), std::memory_order_relaxed);
        return;
    }

    if (status == "DONE") {
        // Flush any tail the terminal left unpadded and unaligned.
        if (!osc5522_pending_.empty())
            if (auto tail = decode_base64(osc5522_pending_))
                osc5522_data_ += *tail;
        if (!osc5522_data_.empty())
            events.emplace_back(PasteEvent{std::move(osc5522_data_)});
        abort_transfer();
        return;
    }

    // ENOSYS / EPERM / EBUSY / anything unknown — abandon the transfer, but
    // RECORD it: a refusal and silence need different advice, and the user
    // cannot tell them apart from the symptom (both end as "text pasted").
    //
    // EPERM is the COMMON one, not an edge case: kitty's `clipboard_control`
    // defaults to write-only (`write-clipboard write-primary`), so a read
    // request from a fresh install is refused even though kitty fully
    // implements the protocol. The user sees an image paste silently become
    // text, because kitty still serves text/plain over OSC 52.
    //
    // The counter is how the host learns which message to show. This used to
    // say "hosts that care should detect kitty independently" — they cannot:
    // across ssh+tmux there is no env fingerprint for the OUTER terminal, so
    // the only evidence that kitty is there AND refusing is this packet.
    // Dropping it sent users to fix the wrong thing.
    if (!status.empty() && status != "OK" && status != "DATA"
        && status != "DONE")
        clipboard_read_refused().fetch_add(1, std::memory_order_relaxed);
    abort_transfer();
}

std::optional<std::string> InputParser::decode_base64(std::string_view in) {
    // Standard RFC 4648 alphabet. Returns nullopt on any illegal byte
    // (other than ASCII whitespace, which terminals sometimes insert to
    // wrap long replies and which we skip). Output is raw bytes — may
    // be binary (a PNG), so we build a std::string of bytes, not text.
    auto val = [](unsigned char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string out;
    out.reserve(in.size() / 4 * 3 + 3);
    int acc = 0, bits = 0;
    for (char ch : in) {
        auto c = static_cast<unsigned char>(ch);
        if (c == '=') break;                        // padding — end of data
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        int v = val(c);
        if (v < 0) return std::nullopt;             // illegal symbol
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((acc >> bits) & 0xFF));
        }
    }
    return out;
}

auto InputParser::parse_params(std::string_view s) -> std::vector<int> {
    std::vector<int> result;
    if (s.empty()) return result;

    int current = 0;
    bool has_digit = false;

    for (char c : s) {
        if (c >= '0' && c <= '9') {
            // Clamp instead of overflowing: a hostile "CSI 99999999999999u"
            // would run current*10 past INT_MAX — signed overflow, UB. No
            // legitimate CSI parameter is anywhere near 1e8; saturate there
            // and keep consuming digits so the parse stays in sync.
            if (current < 100000000)
                current = current * 10 + (c - '0');
            has_digit = true;
        } else if (c == ';') {
            result.push_back(has_digit ? current : 0);
            current = 0;
            has_digit = false;
        }
        // Skip non-digit, non-semicolon characters (e.g., '?' private mode indicator)
    }

    result.push_back(has_digit ? current : 0);
    return result;
}

} // namespace maya
