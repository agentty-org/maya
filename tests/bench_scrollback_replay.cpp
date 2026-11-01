// bench_scrollback_replay — where does a cold replay spend its time?
// Builds N Turn-shaped blocks (header row, wrapped prose, a bordered code
// block), then times render_history cold and warm at two widths.
#include "maya/dsl.hpp"
#include "maya/render/scrollback_replay.hpp"
#include "maya/style/theme.hpp"
#include "maya/widget/markdown.hpp"

#include <chrono>
#include <cstdlib>
#include <print>
#include <string>
#include <vector>

using namespace maya;
using clk = std::chrono::steady_clock;

static std::vector<ReplayBlock> make_blocks(int n) {
    std::vector<ReplayBlock> out;
    out.reserve(n);
    std::string code = "```python\n";
    for (int k = 0; k < 12; ++k)
        code += "def fn_" + std::to_string(k) + "(x):\n    return x * " + std::to_string(k) + "\n";
    code += "```\n";
    for (int i = 0; i < n; ++i) {
        std::string md = "ANSWER-" + std::to_string(i) + ". ";
        for (int w = 0; w < 60; ++w) md += "word" + std::to_string(w) + " ";
        md += "\n\n- point one\n- point **two** with `code`\n- three\n\n" + code + "\nClosing ";
        for (int w = 0; w < 25; ++w) md += "tail" + std::to_string(w) + " ";
        using namespace dsl;
        Element e = v(text("  \xe2\x9c\xa6 model                                  turn " + std::to_string(i)),
                      markdown(md)).build();
        out.push_back(make_replay_block(static_cast<std::uint64_t>(0x9000 + i), std::move(e)));
    }
    return out;
}

static double ms(clk::time_point t0) {
    return std::chrono::duration<double, std::milli>(clk::now() - t0).count();
}

int main(int argc, char** argv) {
    const int n = argc > 1 ? std::atoi(argv[1]) : 1200;
    auto blocks = make_blocks(n);
    ScrollbackReplay src;
    src.blocks = [&] { return blocks; };
    for (int width : {120, 90}) {
        replay::clear_cache();
        auto t0 = clk::now();
        auto r = replay::render_history(src, width, theme::native);
        const double cold = ms(t0);
        t0 = clk::now();
        auto r2 = replay::render_history(src, width, theme::native);
        const double warm = ms(t0);
        std::println("width={} blocks={} rows={} bytes={} cold={:.1f}ms (render {:.1f}) warm={:.1f}ms",
                     width, r.blocks, r.rows, r.bytes.size(), cold, r.render_ms, warm);
        (void)r2;
    }
}
