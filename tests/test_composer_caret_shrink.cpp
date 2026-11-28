// test_composer_caret_shrink.cpp — REPRO for the "ghost caret on line 2"
// bug report (diagnosis credit: davidwed, PR #7): fill the composer's
// line to the wrap edge, type one more char (composer wraps to 2 visual
// rows), then backspace (composer shrinks back to 1 row). The user sees
// a caret block still sitting at the START of the (now erased) second
// row.
//
// Two distinct mechanisms are guarded here:
//   1. PAINTED caret: the canvas diff/shrink path leaving stale █ cells
//      on the erased row (wire-content bug — never actually broken, but
//      cheap to pin).
//   2. HARDWARE cursor: inline mode never positions the real cursor —
//      the frame's last byte leaves it at an arbitrary column INSIDE
//      the box. If the terminal loses DECTCEM (?25l) — e.g. a timed-out
//      ?2026 sync block is rolled back INCLUDING the hide inside it —
//      the cursor surfaces there as a caret-lookalike block. The fix
//      parks it at column 0 of the frame's last wire row (row-relative
//      \r only — never absolute CUP) and re-asserts ?25l AFTER sync_end.
//      Assertions: hardware cursor at (last wire row, col 0) after every
//      frame, and the byte stream's final ?25l ordered after the final
//      ?2026l. These FAIL on the pre-fix serializer.
//
// Harness: render the composer at frame A (wrapped) and frame B
// (shrunk) into real Canvases, push both through the REAL inline-frame
// chain (Fresh render → Synced diff render) into a byte pipe, and feed
// those bytes through a compact VT emulator.
#include <maya/maya.hpp>
#include <maya/widget/agentty/composer.hpp>
#include <maya/render/inline_frame.hpp>
#include <maya/render/renderer.hpp>
#include <maya/render/serialize.hpp>

#include <fcntl.h>
#include <unistd.h>

#include <print>
#include <string>
#include <utility>
#include <vector>

using namespace maya;

// ── Writer over a pipe (same shape as test_inline_frame.cpp) ───────────
static std::pair<Writer, int> make_pipe_writer() {
    int fds[2];
    int rc = pipe(fds);
    (void)rc;
    int flags = fcntl(fds[1], F_GETFL, 0);
    fcntl(fds[1], F_SETFL, flags | O_NONBLOCK);
    int rflags = fcntl(fds[0], F_GETFL, 0);
    fcntl(fds[0], F_SETFL, rflags | O_NONBLOCK);
    Writer w{static_cast<platform::NativeHandle>(fds[1])};
    return {std::move(w), fds[0]};
}

// ── Compact VT emulator: just enough for maya's inline wire ────────────
// Handles: CUP (row;colH), CUU/CUD (A/B), CR, LF, EL (K), ED (J: 0,1,2),
// DECAWM off/on, SGR (ignored, but parsed), printable chars, DECSCUSR
// (ignored). We only care about GEOMETRY + text cells.
struct VtEmu {
    int w = 80, h = 24;
    std::vector<std::string> screen;   // h rows of w cols
    int cx = 0, cy = 0;
    bool autowrap = true;              // DECAWM
    bool cursor_hidden = false;        // DECTCEM (?25l/?25h)
    int  cursor_shape  = 0;            // DECSCUSR (0 = terminal default)

    explicit VtEmu(int width, int height)
        : w(width), h(height), screen(height, std::string(width, ' ')) {}

    void scroll_up_once() {
        screen.erase(screen.begin());
        screen.push_back(std::string(w, ' '));
    }

    void putc_plain(char c) {
        if (cy >= h) { scroll_up_once(); cy = h - 1; }
        if (cx >= w) {
            if (autowrap) { cx = 0; ++cy; if (cy >= h) { scroll_up_once(); cy = h - 1; } }
            else cx = w - 1;
        }
        screen[cy][cx] = c;
        ++cx;
    }

    void feed(const std::string& s) {
        std::size_t i = 0, n = s.size();
        while (i < n) {
            char c = s[i];
            if (c == '\x1b') {
                if (i + 1 >= n) break;
                if (s[i + 1] == '[') {
                    // CSI: parse params + final.
                    std::size_t j = i + 2;
                    std::string params;
                    while (j < n && ((s[j] >= '0' && s[j] <= '9')
                                  || s[j] == ';' || s[j] == '?'
                                  || s[j] == ':' || s[j] == '<')) {
                        params += s[j]; ++j;
                    }
                    // Intermediate bytes (0x20-0x2F): the SP in DECSCUSR
                    // `CSI n SP q`. Note their presence so `q` can be
                    // disambiguated from other finals if needed.
                    bool had_intermediate = false;
                    while (j < n && s[j] >= 0x20 && s[j] <= 0x2F) {
                        had_intermediate = true; ++j;
                    }
                    if (j >= n) break;
                    char fin = s[j];
                    i = j + 1;
                    auto num = [&](int def) {
                        if (params.empty()) return def;
                        std::string first;
                        std::size_t k = 0;
                        if (params[0] == '?') k = 1;
                        while (k < params.size() && params[k] != ';')
                            first += params[k++];
                        if (first.empty()) return def;
                        return std::atoi(first.c_str());
                    };
                    auto num_at = [&](int idx, int def) {
                        // idx-th ;-separated field (0-based)
                        int cur = 0; std::string field;
                        for (char p : params) {
                            if (p == ';') { if (cur == idx) break; ++cur; field.clear(); }
                            else field += p;
                        }
                        if (field.empty()) return def;
                        return std::atoi(field.c_str());
                    };
                    switch (fin) {
                        case 'A': { int k = num(1); cy -= k; if (cy < 0) cy = 0; break; }
                        case 'B': { int k = num(1); cy += k; if (cy > h - 1) cy = h - 1; break; }
                        case 'C': { int k = num(1); cx += k; if (cx > w - 1) cx = w - 1; break; }
                        case 'D': { int k = num(1); cx -= k; if (cx < 0) cx = 0; break; }
                        case 'H': case 'f': {
                            int r = num_at(0, 1), col = num_at(1, 1);
                            cy = r - 1; cx = col - 1;
                            if (cy < 0) cy = 0; if (cy > h - 1) cy = h - 1;
                            if (cx < 0) cx = 0; if (cx > w - 1) cx = w - 1;
                            break;
                        }
                        case 'K': {
                            int m = num(0);
                            if (m == 0)      for (int x = cx; x < w; ++x) screen[cy][x] = ' ';
                            else if (m == 1) for (int x = 0; x <= cx && x < w; ++x) screen[cy][x] = ' ';
                            else             screen[cy].assign(w, ' ');
                            break;
                        }
                        case 'J': {
                            int m = num(0);
                            if (m == 0) {
                                for (int x = cx; x < w; ++x) screen[cy][x] = ' ';
                                for (int y = cy + 1; y < h; ++y) screen[y].assign(w, ' ');
                            } else if (m == 1) {
                                for (int y = 0; y < cy; ++y) screen[y].assign(w, ' ');
                                for (int x = 0; x <= cx; ++x) screen[cy][x] = ' ';
                            } else {
                                for (auto& row : screen) row.assign(w, ' ');
                            }
                            break;
                        }
                        case 'h': {
                            if (params.find("?7") != std::string::npos) autowrap = true;
                            if (params.find("?25") != std::string::npos) cursor_hidden = false;
                            break;
                        }
                        case 'l': {
                            if (params.find("?7") != std::string::npos) autowrap = false;
                            if (params.find("?25") != std::string::npos) cursor_hidden = true;
                            break;
                        }
                        case 'm': break;   // SGR — cosmetic only
                        case 'q':
                            // DECSCUSR (CSI n SP q) — cursor shape.
                            if (had_intermediate) cursor_shape = num(0);
                            break;
                        case 'r': break;   // DECSTBM — not used by inline path
                        default: break;    // sync, DECTCEM etc. — no geometry
                    }
                    continue;
                }
                // OSC / other escapes: skip till BEL or ST (rare on this wire)
                if (s[i + 1] == ']' || s[i + 1] == 'P') {
                    std::size_t j = i + 2;
                    while (j < n && s[j] != '\x07'
                           && !(s[j] == '\x1b' && j + 1 < n && s[j + 1] == '\\')) ++j;
                    i = (j < n && s[j] == '\x07') ? j + 1 : j + 2;
                    continue;
                }
                i += 2;  // unknown 2-byte escape
                continue;
            }
            if (c == '\r') { cx = 0; ++i; continue; }
            if (c == '\n') { ++cy; if (cy >= h) { scroll_up_once(); cy = h - 1; } ++i; continue; }
            if (c == '\x08') { if (cx > 0) --cx; ++i; continue; }
            putc_plain(c);
            ++i;
        }
    }

    std::string row(int y) const {
        std::string r = screen[y];
        while (!r.empty() && r.back() == ' ') r.pop_back();
        return r;
    }
};

// UTF-8 decode of a full block '█' for emulator cell checks.
static constexpr const char* kBlock = "\xe2\x96\x88";

// ── Render helper: composer config → painted canvas ────────────────────
static Canvas paint_composer(StylePool& pool, const std::string& text,
                             int cursor, int width,
                             bool hardware_caret = false,
                             Composer::State state = Composer::State::Idle) {
    Composer::Config cfg;
    cfg.text   = text;
    cfg.cursor = cursor;
    cfg.hardware_caret = hardware_caret;
    cfg.state  = state;
    Canvas canvas(width, 64, &pool);
    render_tree(Composer{cfg}.build(), canvas, pool, theme::native,
                /*auto_height=*/true);
    return canvas;
}

static void dump_screen(const VtEmu& emu, const char* why) {
    std::println("--- emulator screen after {} ---", why);
    for (int y = 0; y < 8; ++y) {
        std::string r = emu.row(y);
        std::println("  {:2}|{}|", y, r);
    }
}

int main() {
    // Composer box: border(2) + padding(1 each side) ⇒ body = W-4.
    // At W=20 (body=16) the wordy frame A wraps to an extra visual row;
    // frame B (backspace removed " FFFF") fits on one row fewer.
    const int W = 20;

    std::string text_a = "AAAA BBBB CCCC DDDD EEEE FFFF";
    const int cur_a = static_cast<int>(text_a.size());

    // Frame B: backspace removed the last word+space → one visual row
    // fewer; caret stays at end-of-buffer.
    std::string text_b = "AAAA BBBB CCCC DDDD EEEE";
    const int cur_b = static_cast<int>(text_b.size());

    StylePool pool;

    // Sanity: A wraps to ≥2 body rows, B fits in 1.
    {
        Canvas ca = paint_composer(pool, text_a, cur_a, W);
        Canvas cb = paint_composer(pool, text_b, cur_b, W);
        int ha = content_height(ca);
        int hb = content_height(cb);
        std::println("frame A content_rows={} frame B content_rows={}",
                     ha, hb);
        if (!(ha == hb + 1)) {
            std::println("SKIP: chosen widths do not produce the wrap "
                         "delta (A={} B={}); tune W/text and rerun.",
                         ha, hb);
            return 77;
        }
    }

    auto [writer, rfd] = make_pipe_writer();

    // Byte-level guard: the LAST ?25l of a frame must come AFTER the
    // LAST ?2026l — i.e. the hide is re-asserted outside the sync
    // wrapper, where a terminal that discards a timed-out sync block
    // cannot lose it. This is the rollback-proofing the fix adds; on
    // the pre-fix serializer the only hide sits INSIDE the block.
    auto hide_outside_sync = [](const std::string& bytes) {
        const std::size_t last_hide = bytes.rfind("\x1b[?25l");
        const std::size_t last_sync_end = bytes.rfind("\x1b[?2026l");
        if (last_hide == std::string::npos) return false;
        if (last_sync_end == std::string::npos) return true; // sync off
        return last_hide > last_sync_end;
    };

    // Frame A through the real inline chain (Fresh render), WITH the
    // ?2026 sync wrapper — the rollback-window mechanism under test
    // only exists when the frame body ships inside a sync block.
    Canvas ca = paint_composer(pool, text_a, cur_a, W);
    const int rows_a = content_height(ca);
    maya::inline_frame::InlineFrame<maya::inline_frame::Empty> f0;
    auto outcome_a = std::move(f0).seed().render(
        ca, content_rows(ca), term_rows_for_test(24), pool, writer,
        /*sync=*/true);
    if (!std::holds_alternative<
            maya::inline_frame::InlineFrame<maya::inline_frame::Synced>>(
            outcome_a)) {
        std::println("FAIL: frame A did not land in Synced");
        return 1;
    }
    auto synced =
        std::get<maya::inline_frame::InlineFrame<maya::inline_frame::Synced>>(
            std::move(outcome_a));

    // Drain the pipe into the emulator.
    VtEmu emu(W, 24);
    std::string bytes_a;
    {
        char buf[4096];
        ssize_t k;
        while ((k = ::read(rfd, buf, sizeof(buf))) > 0) bytes_a.append(buf, k);
        emu.feed(bytes_a);
    }
    dump_screen(emu, "frame A (wrapped, caret should sit at end of row 1)");

    // Hardware-cursor contract after a Fresh frame: parked at column 0
    // of the frame's last wire row (rows_a - 1), at the RIGHT margin
    // (col W-1 — blank padding under a tmux copy-mode cursor), hidden,
    // and the hide re-asserted outside the sync block. Pre-fix, the
    // cursor rests at an arbitrary column inside the box and the only
    // hide is inside the sync wrapper — all three checks fail.
    if (emu.cy != rows_a - 1 || emu.cx != W - 1) {
        std::println("\nBUG: hardware cursor not parked after frame A — "
                     "at ({}, {}), want ({}, {}). A DECTCEM loss would "
                     "surface it as a ghost caret mid-box.",
                     emu.cy, emu.cx, rows_a - 1, W - 1);
        return 44;
    }
    if (!emu.cursor_hidden) {
        std::println("\nBUG: hardware cursor not hidden after frame A.");
        return 45;
    }
    if (!hide_outside_sync(bytes_a)) {
        std::println("\nBUG: frame A's final ?25l sits INSIDE the ?2026 "
                     "sync block — a rolled-back sync discards the hide "
                     "and the cursor surfaces wherever it was left.");
        return 46;
    }

    // Find the caret block on the emulator screen. Returns count.
    auto count_carets = [&]() {
        int n = 0;
        for (int y = 0; y < 24; ++y) {
            std::size_t p = emu.screen[y].find(kBlock);
            while (p != std::string::npos) {
                ++n;
                p = emu.screen[y].find(kBlock, p + 1);
            }
        }
        return n;
    };
    auto find_caret = [&](int& row, int& col) {
        for (int y = 0; y < 24; ++y) {
            std::size_t p = emu.screen[y].find(kBlock);
            if (p != std::string::npos) { row = y; col = static_cast<int>(p); return true; }
        }
        return false;
    };
    int cr = -1, cc = -1;
    if (!find_caret(cr, cc)) {
        std::println("FAIL: caret block not found after frame A");
        return 1;
    }
    std::println("frame A: caret at row {} col {} ({} block cells on screen)",
                 cr, cc, count_carets());
    if (count_carets() != 1) {
        std::println("FAIL: frame A shows more than one caret block");
        return 1;
    }
    const int caret_row_a = cr;

    // ── Frame B: the backspace. Synced diff render. ────────────────────
    Canvas cb = paint_composer(pool, text_b, cur_b, W);
    const int rows_b = content_height(cb);
    auto wit = synced.verify();
    if (!wit) { std::println("FAIL: verify() failed"); return 1; }
    auto proof = synced.check_scrollback(cb, 24);
    if (!proof) { std::println("FAIL: check_scrollback failed"); return 1; }
    auto outcome_b = std::move(synced).render(
        cb, content_rows(cb), term_rows_for_test(24), pool, writer,
        std::move(*wit), std::move(*proof), /*sync=*/true);
    if (!std::holds_alternative<
            maya::inline_frame::InlineFrame<maya::inline_frame::Synced>>(
            outcome_b)) {
        std::println("FAIL: frame B did not land in Synced");
        return 1;
    }

    {
        std::string bytes;
        char buf[4096];
        ssize_t k;
        while ((k = ::read(rfd, buf, sizeof(buf))) > 0) bytes.append(buf, k);
        emu.feed(bytes);
        if (!hide_outside_sync(bytes)) {
            std::println("\nBUG: frame B's final ?25l sits INSIDE the "
                         "?2026 sync block.");
            return 46;
        }
    }
    dump_screen(emu, "frame B (shrunk, caret must be back on row 0)");

    // Park contract after a diff/shrink frame: last wire row of the NEW
    // (shorter) frame, column 0, hidden. This is the exact reported
    // geometry — pre-fix the cursor rests where the shrink erase left
    // it, ON the erased row, so a DECTCEM loss paints the ghost caret
    // at the start of the vanished second row.
    if (emu.cy != rows_b - 1 || emu.cx != W - 1) {
        std::println("\nBUG: hardware cursor not parked after the shrink — "
                     "at ({}, {}), want ({}, {}).", emu.cy, emu.cx,
                     rows_b - 1, W - 1);
        return 44;
    }
    if (!emu.cursor_hidden) {
        std::println("\nBUG: hardware cursor not hidden after the shrink.");
        return 45;
    }

    int br = -1, bc = -1;
    if (!find_caret(br, bc)) {
        std::println("FAIL: caret block gone entirely after shrink");
        return 1;
    }
    std::println("frame B: caret at row {} col {} ({} block cells on screen)",
                 br, bc, count_carets());

    if (br != caret_row_a - 1) {
        std::println("\nBUG REPRODUCED: after the shrink the painted caret "
                     "sits on row {} but the composer lost exactly one row "
                     "(frame-A caret row was {}).", br, caret_row_a);
        std::println("row 0: |{}|", emu.row(0));
        std::println("row 1: |{}|", emu.row(1));
        std::println("row 2: |{}|", emu.row(2));
        return 42;
    }

    // The row that used to hold the caret must now be erased or hold
    // legitimate content — but there must be exactly ONE caret block.
    if (count_carets() != 1) {
        std::println("\nBUG (variant): stale caret block(s) survive the "
                     "shrink ({} block cells on screen).", count_carets());
        return 43;
    }

    // ── Frame C: idle no-op (same canvas again). The unchanged-path
    // early-return must also park + re-hide: an idle composer is the
    // COMMON state, and the mobile-terminal / subprocess ?25h cases
    // strike between frames — the next no-op tick must both re-hide
    // and leave the cursor at the benign anchor. ───────────────────
    auto synced_b =
        std::get<maya::inline_frame::InlineFrame<maya::inline_frame::Synced>>(
            std::move(outcome_b));
    Canvas canvas_c = paint_composer(pool, text_b, cur_b, W);
    auto wit_c = synced_b.verify();
    if (!wit_c) { std::println("FAIL: verify() failed (frame C)"); return 1; }
    auto proof_c = synced_b.check_scrollback(canvas_c, 24);
    if (!proof_c) { std::println("FAIL: check_scrollback failed (frame C)"); return 1; }
    auto outcome_c = std::move(synced_b).render(
        canvas_c, content_rows(canvas_c), term_rows_for_test(24), pool, writer,
        std::move(*wit_c), std::move(*proof_c), /*sync=*/true);
    if (!std::holds_alternative<
            maya::inline_frame::InlineFrame<maya::inline_frame::Synced>>(
            outcome_c)) {
        std::println("FAIL: frame C did not land in Synced");
        return 1;
    }
    {
        // Simulate the external re-show davidwed's report implicates
        // (mobile soft-keyboard / sandboxed subprocess): DECTCEM comes
        // back on between frames…
        emu.cursor_hidden = false;
        std::string bytes;
        char buf[4096];
        ssize_t k;
        while ((k = ::read(rfd, buf, sizeof(buf))) > 0) bytes.append(buf, k);
        emu.feed(bytes);
        // …and the idle no-op frame must re-assert the hide (outside
        // any sync block — the no-op path ships none) and hold the park.
        if (!hide_outside_sync(bytes)) {
            std::println("\nBUG: idle no-op frame does not re-hide the "
                         "cursor outside a sync block.");
            return 46;
        }
    }
    if (!emu.cursor_hidden) {
        std::println("\nBUG: externally re-shown cursor not re-hidden by "
                     "the idle no-op frame.");
        return 45;
    }
    if (emu.cy != rows_b - 1 || emu.cx != W - 1) {
        std::println("\nBUG: idle no-op frame moved the parked cursor — "
                     "at ({}, {}), want ({}, {}).", emu.cy, emu.cx,
                     rows_b - 1, W - 1);
        return 44;
    }

    std::println("PASS: shrink erased the wrapped row, the caret returned "
                 "to row 0, and the hardware cursor stayed parked+hidden "
                 "(rollback-proof) across fresh/diff/no-op frames.");

    // ── Hardware-caret mode ─────────────────────────────────────
    // Config::hardware_caret: the caret cell is conceal+caret_anchor
    // (paints NOTHING — no █ on screen) and the serializer's epilogue
    // must move the REAL cursor onto that cell and SHOW it. Checks:
    //   H1  fresh frame: cursor SHOWN at the caret cell (not parked);
    //   H2  no █ glyph painted anywhere (terminal owns the caret);
    //   H3  idle no-op frame: ZERO bytes (keep-still — any motion
    //       would reset the terminal's blink phase);
    //   H4  wrap→shrink: cursor tracks the caret across a geometry
    //       change and ends shown at the new caret cell;
    //   H5  finalize returns the cursor to the resting row (host
    //       shell resumes BELOW the frame, not mid-box).
    {
        auto [writer2, rfd2] = make_pipe_writer();
        VtEmu hemu(W, 24);
        StylePool pool2;

        auto drain = [&]() {
            std::string bytes;
            char buf[4096];
            ssize_t k;
            while ((k = ::read(rfd2, buf, sizeof(buf))) > 0)
                bytes.append(buf, static_cast<std::size_t>(k));
            hemu.feed(bytes);
            return bytes;
        };
        auto count_blocks = [&]() {
            int n = 0;
            for (int y = 0; y < 24; ++y) {
                std::size_t p = hemu.screen[y].find(kBlock);
                while (p != std::string::npos) {
                    ++n;
                    p = hemu.screen[y].find(kBlock, p + 1);
                }
            }
            return n;
        };
        // A shown caret must sit INSIDE the sync block: Konsole repaints from
        // a snapshot taken at ?2026l and never again until the next write, so
        // a ?25h after the block leaves the cursor invisible (#82).
        auto show_inside_sync = [](const std::string& bytes) {
            const std::size_t show = bytes.rfind("\x1b[?25h");
            const std::size_t end  = bytes.rfind("\x1b[?2026l");
            if (show == std::string::npos) return false;
            if (end == std::string::npos) return true;   // sync off
            return show < end;
        };

        // H1: fresh render, caret mid-text (end of "AAAA").
        std::string htext = "AAAA BBBB";
        Canvas h1 = paint_composer(pool2, htext, 4, W, /*hardware_caret=*/true);
        const int hrows = content_height(h1);
        maya::inline_frame::InlineFrame<maya::inline_frame::Empty> hf0;
        auto houtcome = std::move(hf0).seed().render(
            h1, content_rows(h1), term_rows_for_test(24), pool2, writer2,
            /*sync=*/true);
        if (!std::holds_alternative<
                maya::inline_frame::InlineFrame<maya::inline_frame::Synced>>(
                houtcome)) {
            std::println("FAIL: hw frame 1 did not land in Synced");
            return 1;
        }
        auto hsynced =
            std::get<maya::inline_frame::InlineFrame<maya::inline_frame::Synced>>(
                std::move(houtcome));
        if (!show_inside_sync(drain())) {
            std::println("\nBUG(hw): fresh frame shows the caret after "
                         "?2026l; Konsole never paints it.");
            return 57;
        }
        dump_screen(hemu, "hw frame 1 (fresh, cursor must be SHOWN at caret)");
        if (hemu.cursor_hidden) {
            std::println("\nBUG(hw): cursor hidden after a hardware-caret "
                         "frame — epilogue did not show it.");
            return 50;
        }
        if (hemu.cy >= hrows - 1 || hemu.cy < 0) {
            // Caret sits in the composer BODY (above the bottom border) —
            // a parked cursor would rest on the border row hrows-1.
            std::println("\nBUG(hw): cursor at row {} (frame rows {}) — "
                         "looks parked, not at the caret.", hemu.cy, hrows);
            return 51;
        }
        const int hw_row1 = hemu.cy, hw_col1 = hemu.cx;
        std::println("hw frame 1: cursor shown at ({}, {})", hw_row1, hw_col1);
        // H2b: DECSCUSR discipline — a text input's caret is a BLOCK,
        // always. Idle = 1 (BLINKING block: the terminal owns the blink,
        // maya schedules no frames for it); busy/awaiting = 2 (steady).
        if (hemu.cursor_shape != 1) {
            std::println("\nBUG(hw): idle frame shape {} — want 1 "
                         "(blinking block).", hemu.cursor_shape);
            return 56;
        }
        // H2: concealed caret — no block glyph painted anywhere.
        if (count_blocks() != 0) {
            std::println("\nBUG(hw): {} painted █ cell(s) — hardware mode "
                         "must paint no caret glyph.", count_blocks());
            return 52;
        }

        // H3: idle no-op frame — keep-still (zero bytes).
        Canvas h2 = paint_composer(pool2, htext, 4, W, true);
        auto wit2 = hsynced.verify();
        auto proof2 = hsynced.check_scrollback(h2, 24);
        if (!wit2 || !proof2) { std::println("FAIL: hw verify/proof"); return 1; }
        auto houtcome2 = std::move(hsynced).render(
            h2, content_rows(h2), term_rows_for_test(24), pool2, writer2,
            std::move(*wit2), std::move(*proof2), /*sync=*/true);
        auto hsynced2 =
            std::get<maya::inline_frame::InlineFrame<maya::inline_frame::Synced>>(
                std::move(houtcome2));
        const std::string idle_bytes = drain();
        if (!idle_bytes.empty()) {
            std::println("\nBUG(hw): idle no-op frame emitted {} byte(s) — "
                         "cursor motion resets the terminal blink phase; "
                         "keep-still must emit NOTHING.", idle_bytes.size());
            return 53;
        }

        // H4: type to wrap, then backspace to shrink — cursor tracks.
        // The wrap frame is AWAITING-PERMISSION: exercises the DECSCUSR
        // state channel (shape 2 = steady block while a decision is
        // pending) — idle frames leave the shape untouched, so this is
        // the only place the shape machinery can be observed.
        std::string wtext = "AAAA BBBB CCCC DDDD EEEE FFFF";
        Canvas h3 = paint_composer(pool2, wtext,
                                   static_cast<int>(wtext.size()), W, true,
                                   Composer::State::AwaitingPermission);
        auto wit3 = hsynced2.verify();
        auto proof3 = hsynced2.check_scrollback(h3, 24);
        if (!wit3 || !proof3) { std::println("FAIL: hw verify/proof 3"); return 1; }
        auto houtcome3 = std::move(hsynced2).render(
            h3, content_rows(h3), term_rows_for_test(24), pool2, writer2,
            std::move(*wit3), std::move(*proof3), /*sync=*/true);
        auto hsynced3 =
            std::get<maya::inline_frame::InlineFrame<maya::inline_frame::Synced>>(
                std::move(houtcome3));
        if (!show_inside_sync(drain())) {
            std::println("\nBUG(hw): diff frame shows the caret after "
                         "?2026l; Konsole never paints it.");
            return 57;
        }
        const int wrap_row = hemu.cy, wrap_col = hemu.cx;
        if (hemu.cursor_hidden) {
            std::println("\nBUG(hw): cursor hidden after wrap frame.");
            return 50;
        }
        // Awaiting-permission frame must carry DECSCUSR 2 (steady block).
        if (hemu.cursor_shape != 2) {
            std::println("\nBUG(hw): awaiting frame shape {} — want 2 "
                         "(steady block).", hemu.cursor_shape);
            return 56;
        }

        Canvas h4 = paint_composer(pool2, text_b, cur_b, W, true);
        auto wit4 = hsynced3.verify();
        auto proof4 = hsynced3.check_scrollback(h4, 24);
        if (!wit4 || !proof4) { std::println("FAIL: hw verify/proof 4"); return 1; }
        auto houtcome4 = std::move(hsynced3).render(
            h4, content_rows(h4), term_rows_for_test(24), pool2, writer2,
            std::move(*wit4), std::move(*proof4), /*sync=*/true);
        auto hsynced4 =
            std::get<maya::inline_frame::InlineFrame<maya::inline_frame::Synced>>(
                std::move(houtcome4));
        drain();
        dump_screen(hemu, "hw frame 4 (shrunk — cursor tracks the caret)");
        if (hemu.cursor_hidden) {
            std::println("\nBUG(hw): cursor hidden after shrink frame.");
            return 50;
        }
        if (hemu.cy >= wrap_row && hemu.cx == wrap_col) {
            std::println("\nBUG(hw): cursor did not move off the wrapped "
                         "caret cell ({}, {}) after the shrink.",
                         wrap_row, wrap_col);
            return 54;
        }
        if (count_blocks() != 0) {
            std::println("\nBUG(hw): painted █ appeared after shrink.");
            return 52;
        }
        // Back to idle — the shape returns to 1 (blinking block): the
        // cosmetics helper emits `1 q` on the 2→1 transition, so the
        // caret resumes blinking the moment the agent goes quiet.
        if (hemu.cursor_shape != 1) {
            std::println("\nBUG(hw): idle frame after awaiting kept shape "
                         "{} — must return to 1 (blinking block).",
                         hemu.cursor_shape);
            return 56;
        }

        // H5: finalize — cursor returns to the resting row (below the
        // caret), ready for the host shell.
        const int rows4 = content_height(h4);
        std::string fin;
        auto sealed = std::move(hsynced4).finalize(fin);
        (void)sealed;
        hemu.feed(fin);
        if (hemu.cy != rows4 - 1) {
            std::println("\nBUG(hw): finalize left the cursor at row {} — "
                         "must return it to the resting row {} so the host "
                         "shell resumes below the frame.", hemu.cy, rows4 - 1);
            return 55;
        }
        // H5b: finalize must restore the terminal-default shape (`0 q`)
        // — DECSCUSR is global terminal state; leaking shape 5 would
        // change the SHELL's cursor after exit.
        if (hemu.cursor_shape != 0) {
            std::println("\nBUG(hw): finalize left DECSCUSR shape {} — "
                         "must restore 0 (terminal default).",
                         hemu.cursor_shape);
            return 57;
        }
        std::println("PASS(hw): hardware caret shown at the true cell, "
                     "keep-still idle frames, tracks wrap/shrink, finalize "
                     "parks below the frame.");
        ::close(rfd2);
    }

    ::close(rfd);
    return 0;
}
