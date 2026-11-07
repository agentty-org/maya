#pragma once
// maya::widget::permission — Tool permission prompt card
//
// Zed's bordered card presentation + Claude Code's key-hint UX.
//
//   ╭─ ⚠ Permission Required ─────────────╮
//   │ bash wants to execute:               │
//   │ rm -rf node_modules && npm install   │
//   │                                      │
//   │ [y] allow  [n] deny  [a] always      │
//   ╰──────────────────────────────────────╯

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../dsl.hpp"
#include "../style/border.hpp"
#include "../style/style.hpp"
#include "../style/theme.hpp"
#include "markdown.hpp"   // syntax_line_runs

#include <optional>

namespace maya {

enum class PermissionResult : uint8_t { Pending, Allow, AllowAlways, Deny };

class Permission {
public:
    struct Config {
        Config() = default;
        std::string tool_name;
        std::string description;
        bool show_always_allow = false;
        // Optional: verb after the name ("wants to run"); picked from the
        // tool name when empty. `lang` syntax-colours the description
        // ("sh" for a command). `accent` tints the chip; warning if unset.
        std::string verb;
        std::string lang;
        std::optional<Color> accent;
    };

    explicit Permission(Config cfg) : cfg_(std::move(cfg)) {}

    Permission(std::string tool_name, std::string description) {
        cfg_.tool_name = std::move(tool_name);
        cfg_.description = std::move(description);
    }

    [[nodiscard]] PermissionResult result() const { return result_; }
    void set_result(PermissionResult r) { result_ = r; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::vector<Element> rows;
        const Color accent = cfg_.accent.value_or(Color::slot(ThemeSlot::Warning));

        std::string verb = cfg_.verb;
        if (verb.empty()) {
            verb = "wants to execute";
            if (cfg_.tool_name == "read" || cfg_.tool_name == "Read")
                verb = "wants to read";
            else if (cfg_.tool_name == "edit" || cfg_.tool_name == "Edit"
                  || cfg_.tool_name == "write" || cfg_.tool_name == "Write")
                verb = "wants to edit";
        }

        // ` Bash `  wants to run
        {
            std::string content = " " + cfg_.tool_name + " ";
            std::vector<StyledRun> runs;
            runs.push_back(StyledRun{0, content.size(), chip(accent)});
            const std::size_t v0 = content.size();
            content += "  " + verb;
            runs.push_back(StyledRun{v0, content.size() - v0,
                Style{}.with_fg(Color::slot(ThemeSlot::Text))});
            rows.push_back(Element{TextElement{
                .content = std::move(content), .style = {}, .runs = std::move(runs),
            }});
        }

        // What will happen: the command / path, syntax-coloured when we know
        // the language, each line behind a muted `$` or `│` gutter.
        if (!cfg_.description.empty()) {
            rows.push_back(Element{TextElement{.content = ""}});
            const bool shell = cfg_.lang == "sh";
            const auto syn = cfg_.lang.empty()
                ? std::vector<std::vector<StyledRun>>{}
                : syntax_line_runs(cfg_.description, cfg_.lang);
            std::size_t pos = 0, li = 0;
            const std::string& d = cfg_.description;
            while (pos <= d.size()) {
                const auto nl = d.find('\n', pos);
                const auto line = std::string_view{d}.substr(
                    pos, nl == std::string::npos ? std::string::npos : nl - pos);
                const std::string gutter = li == 0 && shell ? "$ " : "  ";
                std::string content = gutter + std::string{line};
                std::vector<StyledRun> runs{
                    {0, gutter.size(), Style{}.with_fg(Color::slot(ThemeSlot::Muted))}};
                const Style base = Style{}.with_fg(Color::slot(ThemeSlot::Text)).with_bold();
                if (li < syn.size() && !syn[li].empty()) {
                    std::size_t at = 0;
                    for (const auto& r : syn[li]) {
                        if (r.byte_offset > at)
                            runs.push_back({gutter.size() + at, r.byte_offset - at, base});
                        Style s = base;
                        if (r.style.fg) s.fg = r.style.fg;
                        s.italic = r.style.italic;
                        runs.push_back({gutter.size() + r.byte_offset, r.byte_length, s});
                        at = r.byte_offset + r.byte_length;
                    }
                    if (at < line.size())
                        runs.push_back({gutter.size() + at, line.size() - at, base});
                } else if (!line.empty()) {
                    runs.push_back({gutter.size(), line.size(), base});
                }
                rows.push_back(Element{TextElement{
                    .content = std::move(content), .style = {}, .runs = std::move(runs),
                }});
                if (nl == std::string::npos) break;
                pos = nl + 1;
                ++li;
            }
        }

        // Spacer
        rows.push_back(Element{TextElement{.content = ""}});

        // Key hints as keycaps:  y  allow    n  deny    a  always allow
        {
            std::string content;
            std::vector<StyledRun> runs;
            const Style label_st = Style{}.with_fg(Color::slot(ThemeSlot::Text));
            auto add_hint = [&](const char* key, const char* label, Color c) {
                std::size_t off = content.size();
                content += std::string{" "} + key + " ";
                runs.push_back(StyledRun{off, content.size() - off, chip(c)});
                off = content.size();
                std::string rest = std::string{" "} + label + "    ";
                content += rest;
                runs.push_back(StyledRun{off, rest.size(), label_st});
            };

            add_hint("y", "allow", Color::slot(ThemeSlot::Success));
            add_hint("n", "deny", Color::slot(ThemeSlot::Error));
            if (cfg_.show_always_allow)
                add_hint("a", "always allow", Color::slot(ThemeSlot::Info));

            rows.push_back(Element{TextElement{
                .content = std::move(content), .style = {},
                .wrap = TextWrap::NoWrap, .runs = std::move(runs),
            }});
        }

        return (dsl::v(std::move(rows))
            | dsl::border(BorderStyle::Round)
            | dsl::bcolor(accent)
            | dsl::btext(" \xe2\x9a\xa0 needs your ok ",
                         BorderTextPos::Top, BorderTextAlign::Start)
            | dsl::padding(0, 1, 0, 1)).build();
    }

private:
    // Solid chip in `c`: explicit fill on a named scheme, SGR 7 on native
    // where the palette colour can't be measured.
    static Style chip(Color c) {
        if (theme::live().resolve(c).has_channels())
            return Style{}.with_bg(c)
                          .with_fg(Color::slot(ThemeSlot::InverseText)).with_bold();
        return Style{}.with_fg(c).with_inverse().with_bold();
    }

    Config cfg_;
    PermissionResult result_ = PermissionResult::Pending;
};

} // namespace maya
