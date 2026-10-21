// OSC 8 links: a style with a link opens it on its first cell and closes
// it where the link ends, and nothing is left open at the end of a frame.
#include <maya/maya.hpp>
#undef NDEBUG
#include "agtest.hpp"
#include <print>

using namespace maya;

namespace {
std::size_t count(const std::string& s, std::string_view needle) {
    std::size_t n = 0;
    for (std::size_t i = s.find(needle); i != std::string::npos; i = s.find(needle, i + 1)) ++n;
    return n;
}
}  // namespace

TEST_CASE("link_id: same uri, same id; bad ones get none") {
    std::println("--- test_link_id ---");
    const auto a = link_id("https://example.org/x");
    assert(a != 0);
    assert(link_id("https://example.org/x") == a);
    assert(link_id("https://example.org/y") != a);
    assert(link_uri(a) == "https://example.org/x");
    assert(link_id("") == 0);
    assert(link_id("has space") == 0);
    assert(link_id("evil\x1b]0;x\x07") == 0);   // no escapes in the uri
    assert(link_uri(0).empty());
    std::println("PASS\n");
}

TEST_CASE("diff: a link opens over its cells and closes after them") {
    std::println("--- test_link_diff ---");
    StylePool pool;
    const auto id = link_id("https://example.org/");
    const uint16_t plain = pool.intern(Style{});
    const uint16_t link = pool.intern(Style{}.with_link(id).with_underline());
    Canvas old_c(10, 1, &pool);
    Canvas new_c(10, 1, &pool);
    // "ab" plain, "LINK" linked, "cd" plain
    const char32_t txt[] = U"abLINKcd";
    for (int x = 0; x < 8; ++x) new_c.set(x, 0, txt[x], x >= 2 && x < 6 ? link : plain);
    std::string out;
    diff(old_c, new_c, pool, out);
    const std::string open = "\x1b]8;;https://example.org/\x1b\\";
    const std::string close = "\x1b]8;;\x1b\\";
    assert(count(out, open) == 1);
    const auto at_open = out.find(open);
    const auto at_text = out.find("LINK");
    assert(at_open < at_text);
    // closed after the link text, before "cd"
    const auto at_close = out.find(close, at_text);
    assert(at_close != std::string::npos);
    assert(at_close < out.find("cd"));
    std::println("PASS\n");
}

TEST_CASE("diff: a link on the last cell is closed at the end of the frame") {
    std::println("--- test_link_diff_tail ---");
    StylePool pool;
    const auto id = link_id("https://example.org/tail");
    const uint16_t link = pool.intern(Style{}.with_link(id));
    Canvas old_c(4, 1, &pool);
    Canvas new_c(4, 1, &pool);
    for (int x = 0; x < 4; ++x) new_c.set(x, 0, U'z', link);
    std::string out;
    diff(old_c, new_c, pool, out);
    const std::string close = "\x1b]8;;\x1b\\";
    assert(out.size() >= close.size());
    assert(out.rfind(close) > out.rfind("zzzz"));
    std::println("PASS\n");
}

TEST_CASE("serialize: links in a full frame open and close in pairs") {
    std::println("--- test_link_serialize ---");
    StylePool pool;
    const auto id = link_id("https://example.org/s");
    const uint16_t plain = pool.intern(Style{});
    const uint16_t link = pool.intern(Style{}.with_link(id));
    Canvas c(6, 2, &pool);
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 6; ++x) c.set(x, y, U'q', x < 3 ? link : plain);
    std::string out;
    serialize(c, pool, out);
    const std::string open = "\x1b]8;;https://example.org/s\x1b\\";
    assert(count(out, open) == 2);                       // one per row
    // every open is followed by a close before the next open
    std::size_t pos = 0;
    while ((pos = out.find(open, pos)) != std::string::npos) {
        const auto c2 = out.find("\x1b]8;;\x1b\\", pos + open.size());
        assert(c2 != std::string::npos);
        const auto next = out.find(open, pos + open.size());
        assert(next == std::string::npos || c2 < next);
        pos += open.size();
    }
    std::println("PASS\n");
}
