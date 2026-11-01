// test_scrollback_replay — a width change keeps the whole history scrollable.
//
// Inline mode prints history into the terminal's scrollback and forgets it.
// A width change wipes that scrollback (\x1b[3J) because every wrapped row
// is wrong at the new width. Before scrollback replay only the live canvas
// came back; this pins that a host's replay source brings all of it back,
// rewrapped, in the same frame:
//
//   1. every history block lands once, oldest first, above the live frame
//   2. long lines rewrap to the NEW width
//   3. the live frame is intact below the history, viewport unchanged
//   4. the row cap keeps the NEWEST blocks
//   5. a second replay at a width already seen is served from cache, and
//      produces the same bytes
//   6. no source → plain wipe (old behaviour)

#include "maya/dsl.hpp"
#include "maya/render/canvas.hpp"
#include "maya/render/inline_frame.hpp"
#include "maya/render/scrollback_replay.hpp"
#include "maya/style/theme.hpp"
#include "maya/terminal/writer.hpp"
#include "check.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <print>
#include <string>
#include <vector>

using namespace maya;
using namespace maya::inline_frame;

namespace {

std::pair<Writer, int> make_pipe_writer() {
    int fds[2];
    (void)pipe(fds);
    fcntl(fds[1], F_SETFL, fcntl(fds[1], F_GETFL, 0) | O_NONBLOCK);
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK);
    // The pipe must hold a whole replay frame without blocking.
    (void)fcntl(fds[1], F_SETPIPE_SZ, 1 << 20);
    return {Writer{static_cast<platform::NativeHandle>(fds[1])}, fds[0]};
}

std::string read_fd(int rfd) {
    std::string s;
    char buf[65536];
    for (;;) {
        ssize_t n = read(rfd, buf, sizeof(buf));
        if (n > 0) { s.append(buf, static_cast<std::size_t>(n)); continue; }
        break;
    }
    return s;
}

// Minimal VT: cursor moves, CR/LF scroll into scrollback, EL/ED, 3J wipes
// scrollback, DECAWM, SGR ignored. UTF-8 bytes pass through as-is.
struct TermEmu {
    int cols, rows, cx = 0, cy = 0;
    bool decawm = true;
    std::vector<std::string> screen, scrollback;
    TermEmu(int c, int r) : cols(c), rows(r) { screen.assign(rows, std::string(c, ' ')); }

    void newline() {
        if (cy < rows - 1) { ++cy; return; }
        std::string top = screen.front();
        while (!top.empty() && top.back() == ' ') top.pop_back();
        scrollback.push_back(std::move(top));
        screen.erase(screen.begin());
        screen.push_back(std::string(cols, ' '));
    }
    void put(char ch) {
        if (cx >= cols) { if (decawm) { cx = 0; newline(); } else cx = cols - 1; }
        screen[cy][cx++] = ch;
    }
    void feed(const std::string& b) {
        for (std::size_t i = 0; i < b.size();) {
            const unsigned char ch = static_cast<unsigned char>(b[i]);
            if (ch == '\r') { cx = 0; ++i; continue; }
            if (ch == '\n') { newline(); ++i; continue; }
            if (ch == 0x1b) { i = esc(b, i); continue; }
            if (ch >= 0x20) put(static_cast<char>(ch));
            ++i;
        }
    }
    std::size_t esc(const std::string& b, std::size_t i) {
        if (i + 1 >= b.size()) return b.size();
        if (b[i + 1] == ']') {                       // OSC: skip to ST/BEL
            for (std::size_t j = i + 2; j < b.size(); ++j) {
                if (b[j] == '\a') return j + 1;
                if (b[j] == 0x1b && j + 1 < b.size() && b[j + 1] == '\\') return j + 2;
            }
            return b.size();
        }
        if (b[i + 1] != '[') return i + 2;
        std::size_t j = i + 2;
        bool priv = false;
        if (j < b.size() && (b[j] == '?' || b[j] == '>' || b[j] == '=')) { priv = b[j] == '?'; ++j; }
        int p = 0; bool have = false;
        while (j < b.size() && ((b[j] >= '0' && b[j] <= '9') || b[j] == ';')) {
            if (b[j] == ';') { ++j; continue; }
            p = p * 10 + (b[j] - '0'); have = true; ++j;
        }
        if (j >= b.size()) return b.size();
        const char f = b[j];
        const int n = have ? p : 0;
        switch (f) {
            case 'A': cy = std::max(0, cy - (n ? n : 1)); break;
            case 'B': for (int k = 0; k < (n ? n : 1); ++k) newline(); break;
            case 'C': cx = std::min(cols, cx + (n ? n : 1)); break;
            case 'G': cx = std::max(0, (n ? n : 1) - 1); break;
            case 'H': cx = 0; cy = 0; break;
            case 'K': for (int x = cx; x < cols; ++x) screen[cy][x] = ' '; break;
            case 'J':
                if (n == 3) scrollback.clear();
                else if (n == 2) for (auto& l : screen) l.assign(cols, ' ');
                else { for (int x = cx; x < cols; ++x) screen[cy][x] = ' ';
                       for (int y = cy + 1; y < rows; ++y) screen[y].assign(cols, ' '); }
                break;
            case 'h': if (priv && n == 7) decawm = true; break;
            case 'l': if (priv && n == 7) decawm = false; break;
            default: break;
        }
        return j + 1;
    }
    std::vector<std::string> all() const {
        std::vector<std::string> t = scrollback;
        for (auto s : screen) { while (!s.empty() && s.back() == ' ') s.pop_back(); t.push_back(s); }
        return t;
    }
};

int failures = 0;
void expect(bool ok, const std::string& msg) {
    if (!ok) { ++failures; std::println(stderr, "  FAIL: {}", msg); }
}

Element v_stack(Element a, Element b) {
    using namespace dsl;
    return v(std::move(a), std::move(b)).build();
}

// History: N blocks, each a header line plus one long line that wraps
// differently at different widths.
ScrollbackReplay history(int n) {
    ScrollbackReplay r;
    r.blocks = [n] {
        std::vector<ReplayBlock> v;
        for (int i = 0; i < n; ++i) {
            using namespace dsl;
            std::string longline = "BODY-" + std::to_string(i) + ":";
            for (int w = 0; w < 30; ++w) longline += " word" + std::to_string(w);
            longline += " END-" + std::to_string(i);
            v.push_back(make_replay_block(0x1000u + static_cast<std::uint64_t>(i),
                         v_stack(text("BLOCK-" + std::to_string(i)), text(longline))));
        }
        return v;
    };
    return r;
}

Canvas live_canvas(int width, int rows, StylePool& pool) {
    Canvas c(width, rows + 4, &pool);
    const auto sid = pool.intern(Style{});
    for (int y = 0; y < rows; ++y) c.write_text(0, y, "LIVE-" + std::to_string(y), sid);
    return c;
}

// Drive a real HardReset render with `history` bytes; feed the emu.
void hard_reset(TermEmu& emu, StylePool& pool, Writer& w, int rfd,
                const Canvas& c, int term_h, std::string_view hist) {
    auto o = InlineFrame<HardReset>{}.render(c, content_rows(c),
                                             term_rows_for_test(term_h), pool, w,
                                             /*sync=*/false, hist);
    (void)o;
    emu.feed(read_fd(rfd));
}

int index_of(const std::vector<std::string>& t, const std::string& needle) {
    for (int i = 0; i < static_cast<int>(t.size()); ++i)
        if (t[i] == needle || t[i].rfind(needle + ":", 0) == 0 || t[i].rfind(needle + " ", 0) == 0)
            return i;
    return -1;
}
int find_sub(const std::vector<std::string>& t, const std::string& needle) {
    for (int i = 0; i < static_cast<int>(t.size()); ++i)
        if (t[i].find(needle) != std::string::npos) return i;
    return -1;
}
int count_of(const std::vector<std::string>& t, const std::string& needle) {
    int n = 0;
    for (const auto& l : t) if (l == needle) ++n;
    return n;
}

void test_replay_restores_everything() {
    std::println("--- replay restores every block, rewrapped, above the live frame ---");
    const int TERM_H = 24, BLOCKS = 40, LIVE = 10;
    for (int width : {100, 60}) {
        replay::clear_cache();
        StylePool pool;
        auto [writer, rfd] = make_pipe_writer();
        TermEmu emu(width, TERM_H);
        // Pre-existing scrollback that the wipe must remove (no dupes).
        emu.feed("OLD-1\r\nOLD-2\r\n");

        auto src = history(BLOCKS);
        auto got = replay::render_history(src, width, theme::native);
        expect(got.blocks == BLOCKS, "all blocks rendered at width " + std::to_string(width));
        expect(!got.capped, "under the cap");

        Canvas c = live_canvas(width, LIVE, pool);
        hard_reset(emu, pool, writer, rfd, c, TERM_H, got.bytes);
        const auto t = emu.all();

        expect(find_sub(t, "OLD-1") < 0, "old scrollback wiped");
        int prev = -1;
        bool order = true, once = true;
        for (int i = 0; i < BLOCKS; ++i) {
            const std::string tag = "BLOCK-" + std::to_string(i);
            const int at = index_of(t, tag);
            if (at <= prev) order = false;
            prev = at;
            if (count_of(t, tag) != 1) once = false;
        }
        expect(order, "blocks in order at width " + std::to_string(width));
        expect(once, "each block exactly once at width " + std::to_string(width));

        // Rewrapped: no row wider than the terminal, and the long body's
        // tail lands below its own header (it wrapped onto later rows).
        bool fits = true;
        for (const auto& l : t) if (static_cast<int>(l.size()) > width) fits = false;
        expect(fits, "no row wider than " + std::to_string(width));
        const int end39 = find_sub(t, "END-39");
        expect(end39 > index_of(t, "BLOCK-39") + 1, "last body wrapped below its header");

        // Live frame intact, below all history, filling the bottom of the
        // viewport exactly as a fresh paint would.
        const int live0 = index_of(t, "LIVE-0");
        expect(live0 > end39, "live frame below the history");
        bool live_ok = true;
        for (int y = 0; y < LIVE; ++y)
            if (index_of(t, "LIVE-" + std::to_string(y)) != live0 + y) live_ok = false;
        expect(live_ok, "live rows contiguous and in order");
        expect(static_cast<int>(emu.scrollback.size()) > 0, "history is in real scrollback");
        close(rfd);
    }
}

void test_cap_keeps_newest() {
    std::println("--- the row cap keeps the newest blocks ---");
    replay::clear_cache();
    auto src = history(200);
    src.max_rows = 60;
    auto got = replay::render_history(src, 80, theme::native);
    expect(got.capped, "capped");
    expect(got.rows <= 60, "rows within cap (" + std::to_string(got.rows) + ")");
    expect(got.bytes.find("BLOCK-199") != std::string::npos, "newest block kept");
    expect(got.bytes.find("BLOCK-0\x1b") == std::string::npos
           && got.bytes.find("BLOCK-0\r") == std::string::npos, "oldest block dropped");
    // Contiguous suffix: if BLOCK-k is in, BLOCK-k+1 is in.
    int first = -1;
    for (int i = 0; i < 200; ++i)
        if (got.bytes.find("BLOCK-" + std::to_string(i) + "\x1b") != std::string::npos
            || got.bytes.find("BLOCK-" + std::to_string(i) + "\r") != std::string::npos) { first = i; break; }
    bool contiguous = first >= 0;
    for (int i = first; i >= 0 && i < 200; ++i)
        if (got.bytes.find("BLOCK-" + std::to_string(i)) == std::string::npos) contiguous = false;
    expect(contiguous, "kept blocks are a contiguous newest suffix");
}

void test_cache_hits_and_same_bytes() {
    std::println("--- replay at a width already seen is cached and identical ---");
    replay::clear_cache();
    auto src = history(50);
    auto a = replay::render_history(src, 90, theme::native);
    auto b = replay::render_history(src, 70, theme::native);
    auto c = replay::render_history(src, 90, theme::native);
    expect(a.cached == 0, "first render is all misses");
    expect(c.cached == 50, "second render at 90 is all hits (" + std::to_string(c.cached) + ")");
    expect(a.bytes == c.bytes, "cached bytes identical");
    expect(a.bytes != b.bytes, "different width, different wrap");
}

void test_no_source_is_plain_wipe() {
    std::println("--- no history: plain wipe, live frame only ---");
    StylePool pool;
    auto [writer, rfd] = make_pipe_writer();
    TermEmu emu(80, 24);
    emu.feed("OLD-1\r\n");
    Canvas c = live_canvas(80, 5, pool);
    hard_reset(emu, pool, writer, rfd, c, 24, {});
    const auto t = emu.all();
    expect(find_sub(t, "OLD-1") < 0, "wiped");
    expect(index_of(t, "LIVE-0") == 0, "live frame at the top");
    close(rfd);
}

}  // namespace

int main() {
    test_replay_restores_everything();
    test_cap_keeps_newest();
    test_cache_hits_and_same_bytes();
    test_no_source_is_plain_wipe();
    if (failures) {
        std::println("SCROLLBACK REPLAY TESTS FAILED ({} failures)", failures);
        return 1;
    }
    std::println("ALL SCROLLBACK REPLAY TESTS PASSED");
    return 0;
}
