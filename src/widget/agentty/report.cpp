// report.cpp — maya::report::print: a CLI report to a plain stream.
//
// Width is the terminal's (clamped 50..110 so prose stays readable) when the
// stream is a tty, 80 otherwise. Styling is kept only on a colour tty
// (NO_COLOR / TERM=dumb / a pipe get plain text). Rendered rows are padded to
// the width; that padding is trimmed so a copy or a pipe gets clean lines.

#include "maya/widget/agentty/report.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>

#include "maya/print.hpp"
#include "maya/style/theme.hpp"
#include "maya/terminal/ansi.hpp"

#ifdef _WIN32
#include <io.h>
#define MAYA_ISATTY(f) (::_isatty(::_fileno(f)) != 0)
#else
#include <unistd.h>
#define MAYA_ISATTY(f) (::isatty(::fileno(f)) == 1)
#endif

namespace maya::report {

bool painted(std::FILE* f) {
    return MAYA_ISATTY(f) && theme::detect_tier(true) != theme::ColorTier::Mono;
}

std::string render(const Element& e, std::FILE* f) {
    // Off a terminal there is no width to fit: lay out wide so a long path
    // or value stays on one line (a pipe or a log wants whole lines, the way
    // printf wrote them), then trim the padding below.
    int w = 400;
    if (MAYA_ISATTY(f))
        if (const int tw = detail::detect_terminal_width(); tw > 0) w = std::clamp(tw, 50, 110);
    const bool paint = painted(f);
    std::string s = paint ? render_to_string_ansi(e, w) : render_to_string(e, w);
    constexpr std::string_view reset = ansi::reset;
    const std::string double_reset = std::string{reset} + std::string{reset};
    std::string out;
    for (std::size_t at = 0; at < s.size();) {
        auto nl = s.find('\n', at);
        std::string line{std::string_view{s}.substr(at, nl == std::string::npos ? std::string::npos : nl - at)};
        for (;;) {
            if (line.ends_with(' ')) line.pop_back();
            else if (line.ends_with(reset) && (line.size() == reset.size() || line[line.size() - reset.size() - 1] == ' '
                                               || line.ends_with(double_reset)))
                line.resize(line.size() - reset.size());
            else break;
        }
        if (paint && line.find(reset.front()) != std::string::npos) line += reset;
        out += line;
        out += '\n';
        if (nl == std::string::npos) break;
        at = nl + 1;
    }
    while (out.ends_with("\n\n")) out.pop_back();
    return out;
}

void print(const Element& e, std::FILE* f) {
    const std::string out = render(e, f);
    std::fwrite(out.data(), 1, out.size(), f);
    std::fflush(f);
}

} // namespace maya::report
