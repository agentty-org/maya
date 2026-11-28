// Reproduce: code-fence right border missing inside Turn rail.
#include <maya/maya.hpp>
#include <maya/widget/agentty/markdown.hpp>
#include <maya/widget/agentty/turn.hpp>
#include <iostream>
#include <string>

int main() {
    using namespace maya;

    auto md = std::make_shared<StreamingMarkdown>();
    std::string body =
        "**Coherence type-state** in the Runtime (`app.hpp`):\n\n"
        "```cpp\n"
        "struct FullscreenSynced { Canvas front; };\n"
        "struct InlineSynced     { InlineFrameState state; };\n"
        "struct Divergent        {};   // no front buffer member at all\n"
        "```\n\n"
        "std::variant-encoded — Divergent physically lacks the front member.\n";
    md->set_content(body);
    md->finish();

    Turn::Config cfg{
        .glyph      = "\xe2\x9c\xa6",  // ✦
        .label      = "Sonnet",
        .rail_color = Color::blue(),
        .meta       = "12:34",
        .body       = {},
    };
    cfg.body.emplace_back(Turn::BodySlot{md->build()});

    Element root = Turn{cfg}.build();

    int W = 80, H = 24;
    StylePool pool;
    Canvas canvas(W, H, &pool);
    Theme theme{};
    render_tree(root, canvas, pool, theme, /*auto_height=*/false);

    // Dump canvas as text.
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            auto cp = static_cast<uint32_t>(canvas.get(x, y).character);
            if (cp == 0) cp = U' ';
            // emit utf-8
            if (cp < 0x80) std::cout << char(cp);
            else if (cp < 0x800) {
                std::cout << char(0xC0 | (cp >> 6))
                          << char(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                std::cout << char(0xE0 | (cp >> 12))
                          << char(0x80 | ((cp >> 6) & 0x3F))
                          << char(0x80 | (cp & 0x3F));
            } else {
                std::cout << char(0xF0 | (cp >> 18))
                          << char(0x80 | ((cp >> 12) & 0x3F))
                          << char(0x80 | ((cp >> 6) & 0x3F))
                          << char(0x80 | (cp & 0x3F));
            }
        }
        std::cout << '\n';
    }
    return 0;
}
