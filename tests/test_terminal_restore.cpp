// Verifies maya never strands the terminal: a child enters raw + alt-screen
// (+ mouse/paste/focus/kkp) via maya::Terminal, then exits or crashes by every
// route that bypasses normal stack unwinding — graceful, exit(atexit), throw
// (std::terminate), and the fatal signals SIGINT/SIGTERM/SIGHUP/SEGV/ABRT.
//
// The child runs under a forked PTY so we observe exactly the bytes the
// terminal would have received. For each route we assert (a) the alt-screen
// leave + cursor-show + mouse/paste/focus/kkp disable escapes were emitted, and
// (b) the pty is left in cooked termios (ECHO|ICANON). SIGINT is the regression
// guard for the chained emergency handler — a host SIGINT must still leave the
// tty restored.
//
// POSIX-only (PTY + termios). On other platforms the test is a no-op pass.

// Self-contained hard assertion that survives -DNDEBUG (CMake may strip
// assert() in Release).
#include <maya/core/target.hpp>
#include <cstdio>
#include <cstdlib>
#define CHECK(cond) do {                                                  \
    if (!(cond)) {                                                        \
        std::fprintf(stderr, "\n  HARD FAIL at %s:%d: %s\n",            \
                     __FILE__, __LINE__, #cond);                         \
        std::abort();                                                    \
    }                                                                    \
} while (0)

#if MAYA_OS_POSIX

#include <maya/terminal/terminal.hpp>

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#if MAYA_OS_APPLE
#  include <util.h>   // forkpty() lives in <util.h> on macOS/BSD
#else
#  include <pty.h>    // forkpty() lives in <pty.h> on Linux (glibc)
#endif
#include <sys/select.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

namespace {

// Restore escapes maya must emit on teardown / emergency. We check the subset
// that is unconditional across every exit route.
struct Marker { const char* name; const char* seq; };
constexpr Marker kMarkers[] = {
    {"alt_screen_leave", "\x1b[?1049l"},
    {"show_cursor",      "\x1b[?25h"},
    {"disable_mouse",    "\x1b[?1000l"},
    {"disable_paste",    "\x1b[?2004l"},
    {"disable_focus",    "\x1b[?1004l"},
};

// The child: enter the full TUI state, then die per `how`.
[[noreturn]] void child_body(const std::string& how) {
    {
        auto cooked = maya::Terminal<>::create();
        if (!cooked) std::_Exit(99);
        auto raw = std::move(*cooked).enable_raw_mode();
        if (!raw) std::_Exit(98);
        auto alt = std::move(*raw).enter_alt_screen();
        if (!alt) std::_Exit(97);

        if (how == "graceful") {
            // fall out of this scope: RAII destructors restore the tty.
        } else if (how == "exit") {
            std::exit(7);            // atexit path
        } else if (how == "throw") {
            throw std::runtime_error("boom");   // std::terminate path
        } else if (how == "int") {
            ::raise(SIGINT);
        } else if (how == "term") {
            ::raise(SIGTERM);
        } else if (how == "hup") {
            ::raise(SIGHUP);
        } else if (how == "segv") {
            volatile int* p = nullptr; (void)*p;
        } else if (how == "abort") {
            std::abort();
        }
        // graceful only reaches here, after the guards above are destroyed.
    }
    std::_Exit(0);
}

struct Result {
    std::string out;
    bool cooked = false;   // pty left in ECHO|ICANON
    bool readable = false; // we could read termios at all
};

Result run(const std::string& how) {
    int master = -1;
    pid_t pid = ::forkpty(&master, nullptr, nullptr, nullptr);
    CHECK(pid >= 0);
    if (pid == 0) {
        child_body(how);   // never returns
    }

    // Parent: drain pty output until the child exits, then keep reading until
    // EOF so we capture teardown bytes written just before exit. The PTY master
    // returns EOF (read == 0) only once the slave side is fully closed.
    std::string buf;
    bool child_done = false;
    char chunk[4096];
    for (int spins = 0; spins < 400; ++spins) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(master, &rfds);
        timeval tv{0, 50 * 1000};  // 50ms
        int r = ::select(master + 1, &rfds, nullptr, nullptr, &tv);
        if (r > 0 && FD_ISSET(master, &rfds)) {
            ssize_t n = ::read(master, chunk, sizeof chunk);
            if (n > 0) { buf.append(chunk, static_cast<size_t>(n)); continue; }
            if (n == 0) break;            // EOF: slave fully closed, all bytes read
            if (n < 0) { if (child_done) break; }
        }
        if (!child_done) {
            int st = 0;
            if (::waitpid(pid, &st, WNOHANG) == pid) child_done = true;
        } else if (r == 0) {
            // child gone and no data for a full select interval after EOF would
            // have arrived: give one more grace spin, then stop.
            timeval g{0, 50 * 1000};
            fd_set gf; FD_ZERO(&gf); FD_SET(master, &gf);
            if (::select(master + 1, &gf, nullptr, nullptr, &g) <= 0) break;
        }
    }

    Result res;
    res.out = std::move(buf);
    termios attrs{};
    if (::tcgetattr(master, &attrs) == 0) {
        res.readable = true;
        res.cooked = (attrs.c_lflag & ECHO) && (attrs.c_lflag & ICANON);
    }
    // Reap if still running (shouldn't be).
    int st = 0;
    ::waitpid(pid, &st, WNOHANG);
    ::close(master);
    return res;
}

bool has(const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
}

} // namespace

int main() {
    const char* routes[] = {"graceful", "exit", "throw", "int",
                            "term", "hup", "segv", "abort"};
    for (const char* how : routes) {
        Result r = run(how);
        for (const Marker& m : kMarkers) {
            if (!has(r.out, m.seq)) {
                std::fprintf(stderr,
                    "route '%s': missing restore escape '%s'\n", how, m.name);
                std::fprintf(stderr, "  captured %zu bytes: ", r.out.size());
                for (unsigned char c : r.out) {
                    if (c == 0x1b) std::fprintf(stderr, "\\e");
                    else if (c >= 32 && c < 127) std::fputc(c, stderr);
                    else std::fprintf(stderr, "\\x%02x", c);
                }
                std::fprintf(stderr, "\n");
            }
            CHECK(has(r.out, m.seq));
        }
        // The pty must be back in cooked mode (or unreadable, which on some
        // CI sandboxes means the slave is already gone — treat as pass only
        // when we genuinely could not query).
        if (r.readable) {
            if (!r.cooked)
                std::fprintf(stderr, "route '%s': pty left in raw mode\n", how);
            CHECK(r.cooked);
        }
    }
    return 0;
}

#else  // non-POSIX

int main() { return 0; }

#endif
