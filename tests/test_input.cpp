// Tests for the input parser FSM: key events, mouse, UTF-8, modifiers
#include <maya/maya.hpp>
// NDEBUG guard: CMake builds tests in Release (-O3 -DNDEBUG), which strips
// assert(). Undefine it here so this file's runtime asserts actually fire.
#undef NDEBUG
#include "agtest.hpp"
#include <chrono>
#include <print>
#include <thread>

using namespace maya;

// Convenience: extract KeyEvent or assert failure
static const KeyEvent& get_key(const Event& ev) {
    const auto* ke = std::get_if<KeyEvent>(&ev);
    assert(ke != nullptr);
    return *ke;
}

static const MouseEvent& get_mouse(const Event& ev) {
    const auto* me = std::get_if<MouseEvent>(&ev);
    assert(me != nullptr);
    return *me;
}

TEST_CASE("input ascii char") {
    std::println("--- test_input_ascii_char ---");
    InputParser p;
    auto events = p.feed("a");
    assert(events.size() == 1);
    const KeyEvent& ke = get_key(events[0]);
    const auto* ck = std::get_if<CharKey>(&ke.key);
    assert(ck != nullptr && ck->codepoint == U'a');
    assert(ke.mods.none());
    std::println("PASS\n");
}

TEST_CASE("input multiple ascii chars") {
    std::println("--- test_input_multiple_ascii_chars ---");
    InputParser p;
    auto events = p.feed("xyz");
    assert(events.size() == 3);
    assert(std::get<CharKey>(get_key(events[0]).key).codepoint == U'x');
    assert(std::get<CharKey>(get_key(events[1]).key).codepoint == U'y');
    assert(std::get<CharKey>(get_key(events[2]).key).codepoint == U'z');
    std::println("PASS\n");
}

TEST_CASE("input space char") {
    std::println("--- test_input_space_char ---");
    InputParser p;
    auto events = p.feed(" ");
    assert(!events.empty());
    const KeyEvent& ke = get_key(events[0]);
    const auto* ck = std::get_if<CharKey>(&ke.key);
    assert(ck != nullptr && ck->codepoint == U' ');
    std::println("PASS\n");
}

TEST_CASE("input up arrow") {
    std::println("--- test_input_up_arrow ---");
    InputParser p;
    auto events = p.feed("\x1b[A");
    assert(!events.empty());
    const KeyEvent& ke = get_key(events[0]);
    const auto* sk = std::get_if<SpecialKey>(&ke.key);
    assert(sk != nullptr && *sk == SpecialKey::Up);
    std::println("PASS\n");
}

TEST_CASE("input down arrow") {
    std::println("--- test_input_down_arrow ---");
    InputParser p;
    auto events = p.feed("\x1b[B");
    assert(!events.empty());
    const KeyEvent& ke = get_key(events[0]);
    const auto* sk = std::get_if<SpecialKey>(&ke.key);
    assert(sk != nullptr && *sk == SpecialKey::Down);
    std::println("PASS\n");
}

TEST_CASE("input right arrow") {
    std::println("--- test_input_right_arrow ---");
    InputParser p;
    auto events = p.feed("\x1b[C");
    assert(!events.empty());
    const auto* sk = std::get_if<SpecialKey>(&get_key(events[0]).key);
    assert(sk != nullptr && *sk == SpecialKey::Right);
    std::println("PASS\n");
}

TEST_CASE("input left arrow") {
    std::println("--- test_input_left_arrow ---");
    InputParser p;
    auto events = p.feed("\x1b[D");
    assert(!events.empty());
    const auto* sk = std::get_if<SpecialKey>(&get_key(events[0]).key);
    assert(sk != nullptr && *sk == SpecialKey::Left);
    std::println("PASS\n");
}

TEST_CASE("input home key") {
    std::println("--- test_input_home_key ---");
    InputParser p;
    auto events = p.feed("\x1b[H");
    assert(!events.empty());
    const auto* sk = std::get_if<SpecialKey>(&get_key(events[0]).key);
    assert(sk != nullptr && *sk == SpecialKey::Home);
    std::println("PASS\n");
}

TEST_CASE("input end key") {
    std::println("--- test_input_end_key ---");
    InputParser p;
    auto events = p.feed("\x1b[F");
    assert(!events.empty());
    const auto* sk = std::get_if<SpecialKey>(&get_key(events[0]).key);
    assert(sk != nullptr && *sk == SpecialKey::End);
    std::println("PASS\n");
}

TEST_CASE("input enter key") {
    std::println("--- test_input_enter_key ---");
    InputParser p;
    auto events = p.feed("\r");
    assert(!events.empty());
    const auto* sk = std::get_if<SpecialKey>(&get_key(events[0]).key);
    assert(sk != nullptr && *sk == SpecialKey::Enter);
    std::println("PASS\n");
}

TEST_CASE("input backspace") {
    std::println("--- test_input_backspace ---");
    InputParser p;
    auto events = p.feed("\x7f");
    assert(!events.empty());
    const auto* sk = std::get_if<SpecialKey>(&get_key(events[0]).key);
    assert(sk != nullptr && *sk == SpecialKey::Backspace);
    std::println("PASS\n");
}

TEST_CASE("input tab") {
    std::println("--- test_input_tab ---");
    InputParser p;
    auto events = p.feed("\t");
    assert(!events.empty());
    const auto* sk = std::get_if<SpecialKey>(&get_key(events[0]).key);
    assert(sk != nullptr && *sk == SpecialKey::Tab);
    std::println("PASS\n");
}

TEST_CASE("input ctrl c") {
    std::println("--- test_input_ctrl_c ---");
    InputParser p;
    auto events = p.feed("\x03"); // ETX = Ctrl+C
    assert(!events.empty());
    const KeyEvent& ke = get_key(events[0]);
    const auto* ck = std::get_if<CharKey>(&ke.key);
    assert(ck != nullptr && ck->codepoint == 'c');
    assert(ke.mods.ctrl == true);
    std::println("PASS\n");
}

TEST_CASE("input ctrl a") {
    std::println("--- test_input_ctrl_a ---");
    InputParser p;
    auto events = p.feed("\x01"); // SOH = Ctrl+A
    assert(!events.empty());
    const KeyEvent& ke = get_key(events[0]);
    const auto* ck = std::get_if<CharKey>(&ke.key);
    assert(ck != nullptr && ck->codepoint == 'a');
    assert(ke.mods.ctrl == true);
    std::println("PASS\n");
}

// ── Kitty keyboard protocol (CSI-u) — the disambiguated encoding the app
//    enables so Ctrl+/ and Ctrl+Tab become expressible. ────────────────────
TEST_CASE("input kitty ctrl slash") {
    std::println("--- test_input_kitty_ctrl_slash ---");
    InputParser p;
    auto events = p.feed("\x1b[47;5u");   // '/'=47, mod 5 = ctrl
    assert(!events.empty());
    const KeyEvent& ke = get_key(events[0]);
    const auto* ck = std::get_if<CharKey>(&ke.key);
    assert(ck != nullptr && ck->codepoint == '/');
    assert(ke.mods.ctrl == true);
    std::println("PASS\n");
}

TEST_CASE("input kitty ctrl tab") {
    std::println("--- test_input_kitty_ctrl_tab ---");
    InputParser p;
    auto events = p.feed("\x1b[9;5u");    // Tab=9, mod 5 = ctrl
    assert(!events.empty());
    const KeyEvent& ke = get_key(events[0]);
    const auto* sk = std::get_if<SpecialKey>(&ke.key);
    assert(sk != nullptr && *sk == SpecialKey::Tab);
    assert(ke.mods.ctrl == true);
    std::println("PASS\n");
}

TEST_CASE("input kitty shift tab is backtab") {
    std::println("--- test_input_kitty_shift_tab ---");
    InputParser p;
    auto events = p.feed("\x1b[9;2u");    // Tab=9, mod 2 = shift
    assert(!events.empty());
    const KeyEvent& ke = get_key(events[0]);
    const auto* sk = std::get_if<SpecialKey>(&ke.key);
    assert(sk != nullptr && *sk == SpecialKey::BackTab);
    std::println("PASS\n");
}

TEST_CASE("input utf8 two byte") {
    std::println("--- test_input_utf8_two_byte ---");
    InputParser p;
    // U+00E9 'é' = C3 A9
    auto events = p.feed("\xc3\xa9");
    assert(!events.empty());
    const auto* ck = std::get_if<CharKey>(&get_key(events[0]).key);
    assert(ck != nullptr && ck->codepoint == 0x00E9);
    std::println("PASS\n");
}

TEST_CASE("input utf8 three byte") {
    std::println("--- test_input_utf8_three_byte ---");
    InputParser p;
    // U+2603 snowman ☃ = E2 98 83
    auto events = p.feed("\xe2\x98\x83");
    assert(!events.empty());
    const auto* ck = std::get_if<CharKey>(&get_key(events[0]).key);
    assert(ck != nullptr && ck->codepoint == 0x2603);
    std::println("PASS\n");
}

TEST_CASE("input utf8 four byte") {
    std::println("--- test_input_utf8_four_byte ---");
    InputParser p;
    // U+1F600 grinning face = F0 9F 98 80
    auto events = p.feed("\xf0\x9f\x98\x80");
    assert(!events.empty());
    const auto* ck = std::get_if<CharKey>(&get_key(events[0]).key);
    assert(ck != nullptr && ck->codepoint == 0x1F600);
    std::println("PASS\n");
}

TEST_CASE("input mouse left press") {
    std::println("--- test_input_mouse_left_press ---");
    InputParser p;
    // SGR mouse: ESC[<0;5;3M = left button press at col=5, row=3
    auto events = p.feed("\x1b[<0;5;3M");
    assert(!events.empty());
    const MouseEvent& me = get_mouse(events[0]);
    assert(me.button == MouseButton::Left);
    assert(me.kind   == MouseEventKind::Press);
    assert(me.x.value == 5);
    assert(me.y.value == 3);
    std::println("PASS\n");
}

TEST_CASE("input mouse left release") {
    std::println("--- test_input_mouse_left_release ---");
    InputParser p;
    // SGR mouse release: trailing 'm' instead of 'M'
    auto events = p.feed("\x1b[<0;5;3m");
    assert(!events.empty());
    const MouseEvent& me = get_mouse(events[0]);
    assert(me.kind == MouseEventKind::Release);
    std::println("PASS\n");
}

TEST_CASE("input mouse right button") {
    std::println("--- test_input_mouse_right_button ---");
    InputParser p;
    // Button 2 = right mouse button
    auto events = p.feed("\x1b[<2;1;1M");
    assert(!events.empty());
    const MouseEvent& me = get_mouse(events[0]);
    assert(me.button == MouseButton::Right);
    std::println("PASS\n");
}

TEST_CASE("input mouse scroll up") {
    std::println("--- test_input_mouse_scroll_up ---");
    InputParser p;
    // Button 64 = scroll up
    auto events = p.feed("\x1b[<64;1;1M");
    assert(!events.empty());
    const MouseEvent& me = get_mouse(events[0]);
    assert(me.button == MouseButton::ScrollUp);
    std::println("PASS\n");
}

TEST_CASE("input focus gained") {
    std::println("--- test_input_focus_gained ---");
    InputParser p;
    // Focus gained: ESC[I
    auto events = p.feed("\x1b[I");
    assert(!events.empty());
    const auto* fe = std::get_if<FocusEvent>(&events[0]);
    assert(fe != nullptr && fe->focused == true);
    std::println("PASS\n");
}

TEST_CASE("input focus lost") {
    std::println("--- test_input_focus_lost ---");
    InputParser p;
    // Focus lost: ESC[O
    auto events = p.feed("\x1b[O");
    assert(!events.empty());
    const auto* fe = std::get_if<FocusEvent>(&events[0]);
    assert(fe != nullptr && fe->focused == false);
    std::println("PASS\n");
}

TEST_CASE("input bracketed paste") {
    std::println("--- test_input_bracketed_paste ---");
    InputParser p;
    // Bracketed paste: ESC[200~ content ESC[201~
    auto events = p.feed("\x1b[200~hello paste\x1b[201~");
    assert(!events.empty());
    const auto* pe = std::get_if<PasteEvent>(&events[0]);
    assert(pe != nullptr);
    assert(pe->content == "hello paste");
    assert(!pe->unbracketed);
    std::println("PASS\n");
}

TEST_CASE("input unbracketed paste becomes one paste, typing stays keys") {
    std::println("--- test_input_unbracketed_paste ---");
    // A terminal without bracketed paste sends the text as plain bytes,
    // read by read. Each big read is a PasteEvent; the later ones say they
    // continue the first, so the app joins them into one paste.
    InputParser p;
    const std::string a(4095, 'x'), b(4095, 'y');
    auto e1 = p.feed(a);
    assert(e1.size() == 1);
    const auto* p1 = std::get_if<PasteEvent>(&e1[0]);
    assert(p1 && p1->unbracketed && !p1->continues && p1->content == a);
    auto e2 = p.feed("z");   // a 4096-byte write split 4095 + 1
    const auto* p2 = std::get_if<PasteEvent>(&e2[0]);
    assert(p2 && p2->continues && p2->content == "z");
    auto e3 = p.feed(b);
    const auto* p3 = std::get_if<PasteEvent>(&e3[0]);
    assert(p3 && p3->continues);

    // Typing: short reads, no burst before them, stay keystrokes.
    InputParser q;
    auto k = q.feed("hi");
    assert(k.size() == 2 && std::holds_alternative<KeyEvent>(k[0]));
    // A long read with a control key in it is not a paste either.
    std::string ctl(100, 'a');
    ctl[50] = '\x01';
    auto c = q.feed(ctl);
    assert(std::holds_alternative<KeyEvent>(c[0]));
    std::println("PASS\n");
}

TEST_CASE("input frame acks inside a paste or clipboard reply are not data") {
    std::println("--- test_input_acks_inside_paste ---");
    // Every frame asks for a DSR; while a long paste or an OSC 5522 image
    // reply streams in, the answer (ESC[0n) lands in the middle of it.
    InputParser p;
    const std::string body(6000, 'A');
    auto ev = p.feed("\x1b[200~" + body.substr(0, 3000) + "\x1b[0n" + body.substr(3000) + "\x1b[201~");
    const auto* pe = std::get_if<PasteEvent>(&ev[0]);
    assert(pe && pe->content == body);
    assert(p.take_acks() == 1);

    InputParser q;
    std::string r = "\x1b]5522;type=read:status=OK\x1b\\";
    r += "\x1b]5522;type=read:status=DATA:mime=aW1hZ2UvcG5n;QU\x1b[0nJD\x1b\\";
    r += "\x1b]5522;type=read:status=DONE\x1b\\";
    auto e2 = q.feed(r);
    std::string got;
    for (auto& e : e2)
        if (auto* x = std::get_if<PasteEvent>(&e)) got += x->content;
    assert(got == "ABC");
    std::println("PASS\n");
}

TEST_CASE("input osc52 clipboard text st") {
    std::println("--- test_input_osc52_clipboard_text_st ---");
    InputParser p;
    // OSC 52 ; c ; base64("hi") ST  -> "hi" = aGk=
    auto events = p.feed("\x1b]52;c;aGk=\x1b\\");
    assert(events.size() == 1);
    const auto* pe = std::get_if<PasteEvent>(&events[0]);
    assert(pe != nullptr);
    assert(pe->content == "hi");
    std::println("PASS\n");
}

TEST_CASE("input osc52 clipboard text bel") {
    std::println("--- test_input_osc52_clipboard_text_bel ---");
    InputParser p;
    // BEL-terminated form. base64("maya") = bWF5YQ==
    auto events = p.feed("\x1b]52;c;bWF5YQ==\x07");
    assert(events.size() == 1);
    const auto* pe = std::get_if<PasteEvent>(&events[0]);
    assert(pe != nullptr);
    assert(pe->content == "maya");
    std::println("PASS\n");
}

TEST_CASE("input osc52 clipboard binary") {
    std::println("--- test_input_osc52_clipboard_binary ---");
    InputParser p;
    // The PNG magic prefix 89 50 4E 47 0D 0A 1A 0A base64s to
    // iVBORw0KGgo= — the exact bytes agentty's image sniff needs.
    auto events = p.feed("\x1b]52;c;iVBORw0KGgo=\x1b\\");
    assert(events.size() == 1);
    const auto* pe = std::get_if<PasteEvent>(&events[0]);
    assert(pe != nullptr);
    const std::string& b = pe->content;
    assert(b.size() == 8);
    assert(static_cast<unsigned char>(b[0]) == 0x89);
    assert(static_cast<unsigned char>(b[1]) == 0x50);
    assert(static_cast<unsigned char>(b[2]) == 0x4E);
    assert(static_cast<unsigned char>(b[3]) == 0x47);
    std::println("PASS\n");
}

TEST_CASE("input osc52 empty payload ignored") {
    std::println("--- test_input_osc52_empty_payload_ignored ---");
    InputParser p;
    // Empty payload (clipboard empty) and the "?" refusal both yield
    // NO event — never an empty paste that could loop the host.
    auto e1 = p.feed("\x1b]52;c;\x1b\\");
    assert(e1.empty());
    auto e2 = p.feed("\x1b]52;c;?\x1b\\");
    assert(e2.empty());
    std::println("PASS\n");
}

TEST_CASE("input osc non52 discarded") {
    std::println("--- test_input_osc_non52_discarded ---");
    InputParser p;
    // OSC 0 (set title) and OSC 11 (bg color report) must stay
    // unhandled — no spurious paste.
    auto e1 = p.feed("\x1b]0;my title\x07");
    assert(e1.empty());
    auto e2 = p.feed("\x1b]11;rgb:0000/0000/0000\x1b\\");
    assert(e2.empty());
    std::println("PASS\n");
}

TEST_CASE("input osc52 split feed") {
    std::println("--- test_input_osc52_split_feed ---");
    InputParser p;
    // The reply can arrive across multiple read() chunks (SSH / slow
    // pty). The FSM must accumulate across feed() calls. base64("ok")
    // = b2s=.
    auto e0 = p.feed("\x1b]52;c;b2");
    assert(e0.empty());
    auto e1 = p.feed("s=\x1b\\");
    assert(e1.size() == 1);
    const auto* pe = std::get_if<PasteEvent>(&e1[0]);
    assert(pe != nullptr && pe->content == "ok");
    std::println("PASS\n");
}

// ── OSC 5522 (kitty clipboard protocol) read replies ──────────────────

TEST_CASE("input osc5522 image reassembly") {
    std::println("--- test_input_osc5522_image_reassembly ---");
    InputParser p;
    // OK → two PNG chunks → trailing text/plain (must be skipped, the
    // image outranks it) → DONE. mime b64: image/png = aW1hZ2UvcG5n,
    // text/plain = dGV4dC9wbGFpbg==. PNG magic prefix iVBORw0KGgo=
    // split as iVBORw0K (89 50 4E 47 0D 0A) + Ggo= (1A 0A).
    std::string s;
    s += "\x1b]5522;type=read:status=OK\x1b\\";
    s += "\x1b]5522;type=read:status=DATA:mime=aW1hZ2UvcG5n;iVBORw0K\x1b\\";
    s += "\x1b]5522;type=read:status=DATA:mime=aW1hZ2UvcG5n;Ggo=\x1b\\";
    s += "\x1b]5522;type=read:status=DATA:mime=dGV4dC9wbGFpbg==;aGVsbG8=\x1b\\";
    s += "\x1b]5522;type=read:status=DONE\x1b\\";
    auto events = p.feed(s);
    assert(events.size() == 1);
    const auto* pe = std::get_if<PasteEvent>(&events[0]);
    assert(pe != nullptr);
    const std::string& b = pe->content;
    assert(b.size() == 8);
    assert(static_cast<unsigned char>(b[0]) == 0x89);
    assert(static_cast<unsigned char>(b[1]) == 0x50);
    assert(static_cast<unsigned char>(b[6]) == 0x1A);
    assert(static_cast<unsigned char>(b[7]) == 0x0A);
    std::println("PASS\n");
}

TEST_CASE("input osc5522 unaligned chunk seams") {
    std::println("--- test_input_osc5522_unaligned_chunk_seams ---");
    // THE REGRESSION. Base64 packs 3 bytes into 4 characters, so a DATA
    // packet whose length is not a multiple of 4 ends mid-group. When each
    // packet was decoded on its own, the leftover bits were dropped and the
    // next packet restarted the accumulator: every unaligned seam silently
    // lost 1-2 bytes and shifted the remainder.
    //
    // It hid because the damage is invisible where anyone looks. A PNG's
    // magic + IHDR live in the first packet and survive, so the media type
    // and the reported dimensions stay right; only the IDAT stream is
    // shredded, which decoders render as a uniformly BLACK image of exactly
    // the expected size. And it is alignment-dependent, so a terminal whose
    // packets happen to be 4-aligned works perfectly — until it changes.
    //
    // Every earlier case here used 4-aligned chunks ("iVBORw0K" is 8), so
    // none of them could catch it. Sweep the seam across all four phases.
    const std::string payload = [] {
        std::string s;
        for (int i = 0; i < 300; ++i) s.push_back(static_cast<char>(i & 0xFF));
        return s;
    }();

    static const char kT[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string b64;
    {
        int acc = 0, bits = 0;
        for (unsigned char c : payload) {
            acc = (acc << 8) | c; bits += 8;
            while (bits >= 6) { bits -= 6; b64.push_back(kT[(acc >> bits) & 63]); }
        }
        if (bits) { acc <<= (6 - bits); b64.push_back(kT[acc & 63]); }
        while (b64.size() % 4) b64.push_back('=');
    }

    for (std::size_t chunk : {1u, 2u, 3u, 5u, 7u, 13u, 31u, 63u, 101u}) {
        InputParser p;
        std::string s = "\x1b]5522;type=read:status=OK\x1b\\";
        for (std::size_t off = 0; off < b64.size(); off += chunk) {
            s += "\x1b]5522;type=read:status=DATA:mime=aW1hZ2UvcG5n;";
            s += b64.substr(off, chunk);
            s += "\x1b\\";
        }
        s += "\x1b]5522;type=read:status=DONE\x1b\\";

        auto events = p.feed(s);
        assert(events.size() == 1);
        const auto* pe = std::get_if<PasteEvent>(&events[0]);
        assert(pe != nullptr);
        // Byte-exact: a seam must not cost even one byte.
        assert(pe->content.size() == payload.size());
        assert(pe->content == payload);
    }

    // Terminals wrap long replies with ASCII whitespace. It must be
    // skipped WITHOUT breaking group alignment — the reason stripping
    // happens as bytes are accumulated rather than at decode time.
    {
        InputParser p;
        std::string s = "\x1b]5522;type=read:status=OK\x1b\\";
        for (std::size_t off = 0; off < b64.size(); off += 7) {
            s += "\x1b]5522;type=read:status=DATA:mime=aW1hZ2UvcG5n;";
            s += b64.substr(off, 7);
            s += "\r\n";
            s += "\x1b\\";
        }
        s += "\x1b]5522;type=read:status=DONE\x1b\\";
        auto events = p.feed(s);
        assert(events.size() == 1);
        const auto* pe = std::get_if<PasteEvent>(&events[0]);
        assert(pe != nullptr && pe->content == payload);
    }
    // Style (a): kitty pads EVERY packet independently (≤4096 raw bytes
    // each, its own trailing '='). A reassembler that concatenates the
    // base64 and decodes once stops at the FIRST packet's padding — a
    // 100 KB image becomes exactly 4096 bytes. This and the unaligned
    // sweep above pull in opposite directions; both must pass.
    {
        std::string big(100u * 1024u, '\x01');
        big[0] = '\x89'; big[1] = 'P'; big[2] = 'N'; big[3] = 'G';
        auto enc = [](const std::string& raw) {
            static const char* T =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
                "0123456789+/";
            std::string o; int acc = 0, bits = 0;
            for (unsigned char c : raw) {
                acc = (acc << 8) | c; bits += 8;
                while (bits >= 6) { bits -= 6; o.push_back(T[(acc >> bits) & 63]); }
            }
            if (bits) { acc <<= (6 - bits); o.push_back(T[acc & 63]); }
            while (o.size() % 4) o.push_back('=');
            return o;
        };
        InputParser p;
        std::string s = "\x1b]5522;type=read:status=OK\x1b\\";
        for (std::size_t off = 0; off < big.size(); off += 4096) {
            s += "\x1b]5522;type=read:status=DATA:mime=aW1hZ2UvcG5n;";
            s += enc(big.substr(off, 4096));   // padded per packet
            s += "\x1b\\";
        }
        s += "\x1b]5522;type=read:status=DONE\x1b\\";
        std::string got;
        for (std::size_t off = 0; off < s.size(); off += 4096)
            for (auto& ev : p.feed(std::string_view(s).substr(off, 4096)))
                if (auto* pe = std::get_if<PasteEvent>(&ev)) got += pe->content;
        assert(got == big);
    }
    std::println("PASS\n");
}

TEST_CASE("input paste routes deliver fully and report progress") {
    std::println("--- test_input_paste_routes_complete ---");
    // Two invariants, swept over EVERY route a paste can take. Both were
    // violated in ways that silently destroyed user data:
    //
    //  1. COMPLETENESS — all bytes arrive. (OSC 5522 decoded each DATA
    //     packet separately and lost 1-2 bytes per unaligned seam.)
    //
    //  2. PROGRESS — the transfer reports movement. The host arms a
    //     no-reply timer and re-arms it whenever clipboard_rx_bytes()
    //     MOVES; a route that never bumps it is indistinguishable from a
    //     terminal that stayed silent, so a big paste over a slow link
    //     gets diagnosed as "your terminal didn't answer" while its bytes
    //     are still in flight. Only the OSC 5522 path reported.
    //
    // 100 KB is past the streaming threshold, so the bracketed case also
    // covers the multi-chunk flush.
    std::string payload(100u * 1024u, '\x01');
    payload[0] = '\x89'; payload[1] = 'P'; payload[2] = 'N'; payload[3] = 'G';

    static const char kT[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    auto b64 = [](const std::string& raw) {
        std::string o; int acc = 0, bits = 0;
        for (unsigned char c : raw) {
            acc = (acc << 8) | c; bits += 8;
            while (bits >= 6) { bits -= 6; o.push_back(kT[(acc >> bits) & 63]); }
        }
        if (bits) { acc <<= (6 - bits); o.push_back(kT[acc & 63]); }
        while (o.size() % 4) o.push_back('=');
        return o;
    };

    auto check = [&](const std::string& wire) {
        InputParser p;
        const auto before = clipboard_rx_bytes().load();
        std::string got;
        // 4 KB reads: what a real tty hands us, and small enough that a
        // 100 KB paste spans many of them.
        for (std::size_t off = 0; off < wire.size(); off += 4096)
            for (auto& ev : p.feed(std::string_view(wire).substr(off, 4096)))
                if (auto* pe = std::get_if<PasteEvent>(&ev)) got += pe->content;
        assert(got == payload);                               // completeness
        assert(clipboard_rx_bytes().load() != before);        // progress
    };

    check("\x1b[200~" + payload + "\x1b[201~");               // bracketed
    check("\x1b]52;c;" + b64(payload) + "\x1b\\");            // OSC 52
    {                                                          // OSC 5522
        const std::string enc = b64(payload);
        std::string s = "\x1b]5522;type=read:status=OK\x1b\\";
        // 997 is deliberately NOT a multiple of 4 — every seam is unaligned.
        for (std::size_t off = 0; off < enc.size(); off += 997) {
            s += "\x1b]5522;type=read:status=DATA:mime=aW1hZ2UvcG5n;";
            s += enc.substr(off, 997);
            s += "\x1b\\";
        }
        s += "\x1b]5522;type=read:status=DONE\x1b\\";
        check(s);
    }
    std::println("PASS\n");
}

TEST_CASE("input unbracketed paste with a frame ack inside is still paste") {
    // A frame's DSR reply lands in the middle of a big pasted read: it must be
    // counted as an ack and taken out, not turn the whole read into keys.
    InputParser p;
    std::string a(2000, 'A'), b(2000, 'B');
    auto events = p.feed(a + "\x1b[0n" + b);
    REQUIRE(events.size() == 1);
    const auto* pe = std::get_if<PasteEvent>(&events[0]);
    REQUIRE(pe != nullptr);
    CHECK(pe->content == a + b);
    CHECK(pe->unbracketed);
    CHECK(p.take_acks() == 1);
    // An ack on its own in the middle of the burst is an ack, the paste goes on.
    auto ev2 = p.feed("\x1b[0n");
    CHECK(ev2.empty());
    CHECK(p.take_acks() == 1);
    auto ev3 = p.feed(std::string(100, 'C'));
    REQUIRE(ev3.size() == 1);
    const auto* pe3 = std::get_if<PasteEvent>(&ev3[0]);
    REQUIRE(pe3 != nullptr);
    CHECK(pe3->continues);
}

TEST_CASE("input osc5522 refusal without OK is counted") {
    // kitty refuses a read with a lone status=EPERM; no OK comes first.
    InputParser p;
    const auto before = clipboard_read_refused().load();
    auto events = p.feed("\x1b]5522;type=read:status=EPERM\x1b\\");
    assert(events.empty());
    assert(clipboard_read_refused().load() == before + 1);
    std::println("PASS\n");
}

TEST_CASE("input osc5522 text only") {
    std::println("--- test_input_osc5522_text_only ---");
    InputParser p;
    std::string s;
    s += "\x1b]5522;type=read:status=OK\x1b\\";
    s += "\x1b]5522;type=read:status=DATA:mime=dGV4dC9wbGFpbg==;aGVsbG8=\x1b\\";
    s += "\x1b]5522;type=read:status=DONE\x1b\\";
    auto events = p.feed(s);
    assert(events.size() == 1);
    const auto* pe = std::get_if<PasteEvent>(&events[0]);
    assert(pe != nullptr && pe->content == "hello");
    std::println("PASS\n");
}

TEST_CASE("input osc5522 image outranks earlier text") {
    std::println("--- test_input_osc5522_image_outranks_earlier_text ---");
    InputParser p;
    // text arrives FIRST — a terminal that ignores our request order.
    // The later image/png must replace the accumulated text.
    std::string s;
    s += "\x1b]5522;type=read:status=OK\x1b\\";
    s += "\x1b]5522;type=read:status=DATA:mime=dGV4dC9wbGFpbg==;aGVsbG8=\x1b\\";
    s += "\x1b]5522;type=read:status=DATA:mime=aW1hZ2UvcG5n;iVBORw0KGgo=\x1b\\";
    s += "\x1b]5522;type=read:status=DONE\x1b\\";
    auto events = p.feed(s);
    assert(events.size() == 1);
    const auto* pe = std::get_if<PasteEvent>(&events[0]);
    assert(pe != nullptr && pe->content.size() == 8);
    assert(static_cast<unsigned char>(pe->content[0]) == 0x89);
    std::println("PASS\n");
}

TEST_CASE("input osc5522 error aborts then recovers") {
    std::println("--- test_input_osc5522_error_aborts_then_recovers ---");
    InputParser p;
    // EPERM mid-transfer → no event, and a following clean transfer
    // must succeed (state fully reset).
    std::string s;
    s += "\x1b]5522;type=read:status=OK\x1b\\";
    s += "\x1b]5522;type=read:status=DATA:mime=aW1hZ2UvcG5n;iVBORw0K\x1b\\";
    s += "\x1b]5522;type=read:status=EPERM\x1b\\";
    auto e1 = p.feed(s);
    assert(e1.empty());
    std::string s2;
    s2 += "\x1b]5522;type=read:status=OK\x1b\\";
    s2 += "\x1b]5522;type=read:status=DATA:mime=dGV4dC9wbGFpbg==;b2s=\x1b\\";
    s2 += "\x1b]5522;type=read:status=DONE\x1b\\";
    auto e2 = p.feed(s2);
    assert(e2.size() == 1);
    const auto* pe = std::get_if<PasteEvent>(&e2[0]);
    assert(pe != nullptr && pe->content == "ok");
    std::println("PASS\n");
}

TEST_CASE("input osc5522 stray packets dropped") {
    std::println("--- test_input_osc5522_stray_packets_dropped ---");
    InputParser p;
    // DATA/DONE with no preceding OK (another client's leak through a
    // multiplexer) must yield nothing.
    auto e1 = p.feed("\x1b]5522;type=read:status=DATA:mime=dGV4dC9wbGFpbg==;b2s=\x1b\\");
    assert(e1.empty());
    auto e2 = p.feed("\x1b]5522;type=read:status=DONE\x1b\\");
    assert(e2.empty());
    // Write acks are not read replies — ignored even mid-ground.
    auto e3 = p.feed("\x1b]5522;type=write:status=DONE\x1b\\");
    assert(e3.empty());
    std::println("PASS\n");
}

TEST_CASE("input page up") {
    std::println("--- test_input_page_up ---");
    InputParser p;
    auto events = p.feed("\x1b[5~");
    assert(!events.empty());
    const auto* sk = std::get_if<SpecialKey>(&get_key(events[0]).key);
    assert(sk != nullptr && *sk == SpecialKey::PageUp);
    std::println("PASS\n");
}

TEST_CASE("input page down") {
    std::println("--- test_input_page_down ---");
    InputParser p;
    auto events = p.feed("\x1b[6~");
    assert(!events.empty());
    const auto* sk = std::get_if<SpecialKey>(&get_key(events[0]).key);
    assert(sk != nullptr && *sk == SpecialKey::PageDown);
    std::println("PASS\n");
}

TEST_CASE("input delete key") {
    std::println("--- test_input_delete_key ---");
    InputParser p;
    auto events = p.feed("\x1b[3~");
    assert(!events.empty());
    const auto* sk = std::get_if<SpecialKey>(&get_key(events[0]).key);
    assert(sk != nullptr && *sk == SpecialKey::Delete);
    std::println("PASS\n");
}

TEST_CASE("input f1 key") {
    std::println("--- test_input_f1_key ---");
    InputParser p;
    // F1 via SS3: ESC O P
    auto events = p.feed("\x1bOP");
    assert(!events.empty());
    const auto* sk = std::get_if<SpecialKey>(&get_key(events[0]).key);
    assert(sk != nullptr && *sk == SpecialKey::F1);
    std::println("PASS\n");
}

TEST_CASE("input mixed sequence") {
    std::println("--- test_input_mixed_sequence ---");
    InputParser p;
    // ASCII char + arrow key + ASCII char
    auto events = p.feed("a\x1b[Cb");
    assert(events.size() == 3);
    assert(std::get<CharKey>(get_key(events[0]).key).codepoint == U'a');
    assert(*std::get_if<SpecialKey>(&get_key(events[1]).key) == SpecialKey::Right);
    assert(std::get<CharKey>(get_key(events[2]).key).codepoint == U'b');
    std::println("PASS\n");
}

TEST_CASE("input has pending false after complete") {
    std::println("--- test_input_has_pending_false_after_complete ---");
    InputParser p;
    (void)p.feed("a");
    assert(!p.has_pending()); // complete sequence consumed
    std::println("PASS\n");
}

TEST_CASE("input reset clears state") {
    std::println("--- test_input_reset_clears_state ---");
    InputParser p;
    p.reset();
    assert(!p.has_pending());
    auto events = p.feed("z");
    assert(!events.empty());
    std::println("PASS\n");
}

// ── Regression tests for the escape/UTF-8 hardening pass ──────────────

TEST_CASE("input double esc is alt escape") {
    std::println("--- test_input_double_esc_is_alt_escape ---");
    // ESC ESC with nothing after = Alt+Escape (altSendsEscape). Before
    // the alt latch, this decoded as Ctrl+Alt+'{' — a garbage event.
    InputParser p;
    auto events = p.feed("\x1b\x1b");
    assert(events.empty());          // still ambiguous — could prefix a CSI
    assert(p.has_pending());
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    events = p.flush_timeout();
    assert(events.size() == 1);
    const KeyEvent& ke = get_key(events[0]);
    const auto* sk = std::get_if<SpecialKey>(&ke.key);
    assert(sk != nullptr && *sk == SpecialKey::Escape);
    assert(ke.mods.alt);
    assert(!p.has_pending());
    std::println("PASS\n");
}

TEST_CASE("input double esc csi is alt arrow") {
    std::println("--- test_input_double_esc_csi_is_alt_arrow ---");
    // ESC ESC [ A = Alt+Up on altSendsEscape terminals. Before the
    // latch: garbage Ctrl+Alt+'{' + the tail re-parsed as a bare CSI.
    InputParser p;
    auto events = p.feed("\x1b\x1b[A");
    assert(events.size() == 1);
    const KeyEvent& ke = get_key(events[0]);
    const auto* sk = std::get_if<SpecialKey>(&ke.key);
    assert(sk != nullptr && *sk == SpecialKey::Up);
    assert(ke.mods.alt);
    std::println("PASS\n");
}

TEST_CASE("input alt utf8 key") {
    std::println("--- test_input_alt_utf8_key ---");
    // Alt+é arrives as ESC 0xC3 0xA9. Before the fix: Alt+CharKey{0xC3}
    // plus a stray invalid-byte event.
    InputParser p;
    auto events = p.feed("\x1b\xc3\xa9");
    assert(events.size() == 1);
    const KeyEvent& ke = get_key(events[0]);
    const auto* ck = std::get_if<CharKey>(&ke.key);
    assert(ck != nullptr && ck->codepoint == U'\u00e9');
    assert(ke.mods.alt);
    std::println("PASS\n");
}

TEST_CASE("input overlong utf8 rejected") {
    std::println("--- test_input_overlong_utf8_rejected ---");
    // 0xC0 0xAF is the classic overlong '/' smuggle. Strict decoder
    // rejects the 0xC0 lead outright (U+FFFD), never emits '/'.
    InputParser p;
    auto events = p.feed("\xc0\xaf");
    assert(!events.empty());
    for (const auto& ev : events) {
        const KeyEvent& ke = get_key(ev);
        const auto* ck = std::get_if<CharKey>(&ke.key);
        assert(ck != nullptr);
        assert(ck->codepoint != U'/');
        assert(ck->codepoint == char32_t{0xFFFD});
    }
    // Overlong 3-byte: 0xE0 0x80 0xAF would decode to U+002F as well.
    events = p.feed("\xe0\x80\xaf");
    for (const auto& ev : events) {
        const auto* ck = std::get_if<CharKey>(&get_key(ev).key);
        assert(ck == nullptr || ck->codepoint != U'/');
    }
    std::println("PASS\n");
}

TEST_CASE("input surrogate utf8 rejected") {
    std::println("--- test_input_surrogate_utf8_rejected ---");
    // 0xED 0xA0 0x80 decodes numerically to U+D800 — a surrogate half,
    // not a scalar value. Must surface as U+FFFD, not smuggle through.
    InputParser p;
    auto events = p.feed("\xed\xa0\x80");
    assert(!events.empty());
    bool saw_fffd = false;
    for (const auto& ev : events) {
        const auto* ck = std::get_if<CharKey>(&get_key(ev).key);
        assert(ck != nullptr);
        assert(!(ck->codepoint >= 0xD800 && ck->codepoint <= 0xDFFF));
        if (ck->codepoint == char32_t{0xFFFD}) saw_fffd = true;
    }
    assert(saw_fffd);
    std::println("PASS\n");
}

TEST_CASE("input stray continuation rejected") {
    std::println("--- test_input_stray_continuation_rejected ---");
    // A lone continuation byte (no lead) is ill-formed. Old fallback
    // emitted CharKey{0x85} (a C1 control); strict decoder says U+FFFD.
    InputParser p;
    auto events = p.feed("\x85");
    assert(events.size() == 1);
    const auto* ck = std::get_if<CharKey>(&get_key(events[0]).key);
    assert(ck != nullptr && ck->codepoint == char32_t{0xFFFD});
    std::println("PASS\n");
}

TEST_CASE("input invalid continuation resyncs") {
    std::println("--- test_input_invalid_continuation_resyncs ---");
    // Lead byte followed by ASCII instead of a continuation: the parser
    // must surface the malformed prefix as U+FFFD AND still deliver the
    // ASCII byte (old code silently dropped the partial sequence).
    InputParser p;
    auto events = p.feed("\xc3" "a");
    assert(events.size() == 2);
    const auto* bad = std::get_if<CharKey>(&get_key(events[0]).key);
    assert(bad != nullptr && bad->codepoint == char32_t{0xFFFD});
    const auto* ok = std::get_if<CharKey>(&get_key(events[1]).key);
    assert(ok != nullptr && ok->codepoint == U'a');
    std::println("PASS\n");
}

TEST_CASE("input stale csi rescued by timeout") {
    std::println("--- test_input_stale_csi_rescued_by_timeout ---");
    // A truncated CSI ("ESC [ 1" then silence — dropped bytes on a flaky
    // pty) used to wedge the FSM forever: has_pending() stayed true and
    // the next keystrokes were swallowed as parameter bytes. The timeout
    // must abandon it and hand the parser back to Ground.
    InputParser p;
    auto events = p.feed("\x1b[1");
    assert(events.empty());
    assert(p.has_pending());
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    events = p.flush_timeout();
    assert(events.empty());          // nothing fabricated from the fragment
    assert(!p.has_pending());        // FSM back at Ground
    events = p.feed("a");            // next keystroke arrives intact
    assert(events.size() == 1);
    const auto* ck = std::get_if<CharKey>(&get_key(events[0]).key);
    assert(ck != nullptr && ck->codepoint == U'a');
    std::println("PASS\n");
}


TEST_CASE("input terminal reports are consumed, never typed") {
    std::println("--- test_input_terminal_reports_consumed ---");
    // A private-mode CSI (`CSI ? … final`) is the TERMINAL answering us:
    // DA1, DECRPM, DECXCPR. Those replies are solicited by the startup
    // probes, but one can arrive after its probe window closes — routinely
    // over ssh, where a reply fragments and the tail lands late.
    //
    // Such a late reply used to hit the "unknown sequence" fallback, which
    // emits a literal '?' CharKey. The user saw a composer that began with
    // a `?` they never typed, plus a burst of junk key events that made the
    // caret jitter. Both symptoms, one cause: input was being invented from
    // the terminal's own answer.
    //
    // The contract is silence. Never fabricate a keystroke from a report.
    const char* reports[] = {
        "\x1b[?1;2c",          // DA1 — primary device attributes
        "\x1b[?62;1;6c",       // DA1, another shape
        "\x1b[?5522;1$y",      // DECRPM — kitty clipboard mode
        "\x1b[?2026;2$y",      // DECRPM — synchronized output
        "\x1b[?1;1R",          // DECXCPR — extended cursor position
    };
    for (const char* r : reports) {
        InputParser p;
        auto events = p.feed(r);
        assert(events.empty() && "terminal report must produce no events");
        assert(!p.has_pending() && "report must leave the FSM at Ground");
        // The parser is still usable for real input right after.
        events = p.feed("a");
        assert(events.size() == 1);
        const auto* ck = std::get_if<CharKey>(&get_key(events[0]).key);
        assert(ck != nullptr && ck->codepoint == U'a');
    }

    // A report SPLIT across reads must not leak its head either.
    {
        InputParser p;
        auto events = p.feed("\x1b[?1;");
        assert(events.empty());
        events = p.feed("2c");
        assert(events.empty() && "a fragmented report is still a report");
    }

    // Guard the neighbours: SGR mouse uses '<', not '?', and must survive.
    {
        InputParser p;
        auto events = p.feed("\x1b[<0;10;5M");
        assert(events.size() == 1 && "SGR mouse must still parse");
        assert(std::get_if<MouseEvent>(&events[0]) != nullptr);
    }
    std::println("PASS\n");
}

TEST_CASE("input escape key racing a frame ack") {
    std::println("--- test_input_escape_then_ack ---");
    // Every frame sends a DSR (`CSI 5 n`) and the terminal answers `CSI 0 n`.
    // Press Escape while an answer is in flight and the parser sees ESC,
    // then ESC [ 0 n: the second ESC used to latch the first as an Alt
    // prefix, the report produced no key, and the Escape vanished. Escape
    // almost never reached an animating program (one frame, one DSR).
    {
        InputParser p;
        auto events = p.feed("\x1b");
        assert(events.empty() && p.has_pending());
        events = p.feed("\x1b[0n");
        assert(events.size() == 1 && "the Escape key must come out");
        const auto& k = get_key(events[0]);
        assert(std::get_if<SpecialKey>(&k.key) && *std::get_if<SpecialKey>(&k.key) == SpecialKey::Escape);
        assert(!k.mods.alt && "a report is not an Alt prefix");
        assert(p.take_acks() == 1 && "and the ack still counts");
        assert(!p.has_pending());
    }
    // In one read, too.
    {
        InputParser p;
        auto events = p.feed("\x1b\x1b[0n");
        assert(events.size() == 1);
        assert(*std::get_if<SpecialKey>(&get_key(events[0]).key) == SpecialKey::Escape);
    }
    // The neighbour that must not change: ESC ESC [ A is still Alt+Up.
    {
        InputParser p;
        auto events = p.feed("\x1b\x1b[A");
        assert(events.size() == 1);
        const auto& k = get_key(events[0]);
        assert(*std::get_if<SpecialKey>(&k.key) == SpecialKey::Up && k.mods.alt);
    }
    std::println("PASS\n");
}
