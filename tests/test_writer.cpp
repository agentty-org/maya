// Tests for the Writer buffered I/O: push, flush, write_raw
#include <maya/maya.hpp>
// NDEBUG guard: CMake builds tests in Release (-O3 -DNDEBUG), which strips
// assert(). Undefine it here so this file's runtime asserts actually fire.
#undef NDEBUG
#include "agtest.hpp"
#include <print>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
namespace {
// The Writer takes a NativeHandle -- a HANDLE on windows, an fd on posix --
// and every test below wants the same thing: a sink that accepts bytes and
// is gone by the end of the case. The RAII guard closes whichever open the
// platform got; the POSIX spelling of open/close stays untouched below.
struct null_sink {
    maya::platform::NativeHandle h;
    explicit null_sink() {
#if defined(_WIN32)
        h = ::CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr,
                          OPEN_EXISTING, 0, nullptr);
        assert(h != INVALID_HANDLE_VALUE);
#else
        h = ::open("/dev/null", O_WRONLY);
        assert(h >= 0);
#endif
    }
    ~null_sink() {
#if defined(_WIN32)
        if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
#else
        if (h >= 0) ::close(h);
#endif
    }
    null_sink(const null_sink&)            = delete;
    null_sink& operator=(const null_sink&) = delete;
};
}  // namespace
#else
#include <fcntl.h>
#include <unistd.h>
namespace {
struct null_sink {
    int h;
    explicit null_sink() : h(::open("/dev/null", O_WRONLY)) { assert(h >= 0); }
    ~null_sink() { if (h >= 0) ::close(h); }
    null_sink(const null_sink&)            = delete;
    null_sink& operator=(const null_sink&) = delete;
};
}  // namespace
#endif

using namespace maya;

TEST_CASE("writer flush to devnull") {
    std::println("--- test_writer_flush_to_devnull ---");
    null_sink fd;
    Writer w(fd.h);
    w.push(render_op::CursorHide{});
    w.push(render_op::Write{"hello"});
    w.push(render_op::Write{" world"});
    w.push(render_op::StyleStr{"\x1b[1m"});
    w.push(render_op::CursorShow{});
    auto status = w.flush();
    assert(status.has_value());
    std::println("PASS\n");
}

TEST_CASE("writer write raw") {
    std::println("--- test_writer_write_raw ---");
    null_sink fd;
    Writer w(fd.h);
    auto status = w.write_raw("hello world\n");
    assert(status.has_value());
    std::println("PASS\n");
}

TEST_CASE("writer empty flush") {
    std::println("--- test_writer_empty_flush ---");
    null_sink fd;
    Writer w(fd.h);
    auto status = w.flush(); // nothing pushed, should succeed
    assert(status.has_value());
    std::println("PASS\n");
}

TEST_CASE("writer multiple writes merged") {
    std::println("--- test_writer_multiple_writes_merged ---");
    null_sink fd;
    Writer w(fd.h);
    // Consecutive Write ops should merge (batching optimization)
    w.push(render_op::Write{"aaa"});
    w.push(render_op::Write{"bbb"});
    w.push(render_op::Write{"ccc"});
    auto status = w.flush();
    assert(status.has_value());
    std::println("PASS\n");
}

TEST_CASE("writer cursor ops") {
    std::println("--- test_writer_cursor_ops ---");
    null_sink fd;
    Writer w(fd.h);
    w.push(render_op::CursorHide{});
    w.push(render_op::CursorShow{});
    w.push(render_op::CursorHide{});
    auto status = w.flush();
    assert(status.has_value());
    std::println("PASS\n");
}

TEST_CASE("writer style and write") {
    std::println("--- test_writer_style_and_write ---");
    null_sink fd;
    Writer w(fd.h);
    w.push(render_op::StyleStr{"\x1b[1;31m"});   // bold red
    w.push(render_op::Write{"ERROR"});
    w.push(render_op::StyleStr{"\x1b[0m"});       // reset
    auto status = w.flush();
    assert(status.has_value());
    std::println("PASS\n");
}

TEST_CASE("writer write raw empty string") {
    std::println("--- test_writer_write_raw_empty_string ---");
    null_sink fd;
    Writer w(fd.h);
    auto status = w.write_raw("");
    assert(status.has_value());
    std::println("PASS\n");
}

TEST_CASE("writer large output") {
    std::println("--- test_writer_large_output ---");
    null_sink fd;
    Writer w(fd.h);
    // Simulate a full-screen render by pushing 2000 bytes
    std::string chunk(100, 'X');
    for (int i = 0; i < 20; ++i)
        w.push(render_op::Write{chunk});
    auto status = w.flush();
    assert(status.has_value());
    std::println("PASS\n");
}

// safe_break_len: the residue-safe write boundary helper. The
// property we care about: for every prefix of `data` that the kernel
// might accept (0..safe), the wire is left at a complete-unit
// boundary — no half-CSI, no half-UTF-8, no orphan ESC.
TEST_CASE("safe break len plain ascii") {
    std::println("--- test_safe_break_len_plain_ascii ---");
    assert(detail::safe_break_len("") == 0);
    assert(detail::safe_break_len("hello") == 5);
    std::println("PASS\n");
}

TEST_CASE("safe break len complete csi") {
    std::println("--- test_safe_break_len_complete_csi ---");
    // \x1b[31m = SGR red. Final byte 'm' is in 0x40–0x7E.
    std::string_view s = "\x1b[31m";
    assert(detail::safe_break_len(s) == s.size());
    // Trailing data after a complete CSI stays safe.
    std::string_view s2 = "\x1b[31mhi";
    assert(detail::safe_break_len(s2) == s2.size());
    std::println("PASS\n");
}

TEST_CASE("safe break len incomplete csi") {
    std::println("--- test_safe_break_len_incomplete_csi ---");
    // Half a CSI — parameter bytes only.
    assert(detail::safe_break_len("abc\x1b[3") == 3);
    // Just the introducer.
    assert(detail::safe_break_len("abc\x1b[") == 3);
    // Bare ESC.
    assert(detail::safe_break_len("abc\x1b") == 3);
    std::println("PASS\n");
}

TEST_CASE("safe break len osc bel") {
    std::println("--- test_safe_break_len_osc_bel ---");
    // OSC 0;title BEL — BEL terminates.
    std::string_view s = "\x1b]0;hi\x07" "after";
    // Safe at the trailing "after" plus everything before.
    assert(detail::safe_break_len(s) == s.size());
    // Truncated OSC.
    assert(detail::safe_break_len("a\x1b]0;hi") == 1);
    std::println("PASS\n");
}

TEST_CASE("safe break len osc st") {
    std::println("--- test_safe_break_len_osc_st ---");
    // OSC … ST (\x1b\\).
    std::string_view s = "\x1b]52;c;dGVzdA==\x1b\\";
    assert(detail::safe_break_len(s) == s.size());
    // Truncated at ST's first byte.
    std::string_view t = "\x1b]52;c;dGVzdA==\x1b";
    // The trailing ESC opens a new pending sequence, so safe is at
    // the end of the OSC if we already saw a final — we haven't.
    // The whole thing is in-flight, safe stays at 0.
    assert(detail::safe_break_len(t) == 0);
    std::println("PASS\n");
}

TEST_CASE("safe break len utf8 split") {
    std::println("--- test_safe_break_len_utf8_split ---");
    // "é" is C3 A9 (2-byte UTF-8). Cut mid-codepoint.
    std::string_view s = "ab\xc3";   // missing A9
    assert(detail::safe_break_len(s) == 2);
    // Complete codepoint.
    std::string_view s2 = "ab\xc3\xa9";
    assert(detail::safe_break_len(s2) == s2.size());
    // 3-byte codepoint, complete.
    std::string_view s3 = "\xe2\x9c\x93";  // U+2713 ✓
    assert(detail::safe_break_len(s3) == s3.size());
    // Cut after lead.
    std::string_view s4 = "\xe2\x9c";
    assert(detail::safe_break_len(s4) == 0);
    std::println("PASS\n");
}

TEST_CASE("safe break len can sub clear pending") {
    std::println("--- test_safe_break_len_can_sub_clear_pending ---");
    // CAN (\x18) is a one-byte unit even mid-ESC — the helper
    // treats it like ASCII (a complete unit), which is correct
    // because terminals consume CAN as a CS cancel.
    assert(detail::safe_break_len("abc\x18") == 4);
    assert(detail::safe_break_len("abc\x1a") == 4);
    std::println("PASS\n");
}

