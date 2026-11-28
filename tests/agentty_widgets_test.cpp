// agentty_widgets_test — the widgets agentty hands data to instead of drawing.
//
// These pin what the host USED to spell itself, so moving it into maya can't
// quietly change what the user sees:
//
//   1. RunTranscript: off a colour tty it emits no escape at all (pipes and
//      NO_COLOR stay clean), on one it emits exactly the old frames; the
//      pinned row needs >= 3 rows.
//   2. report::Table: columns line up by DISPLAY width (an em dash is three
//      bytes, one column), and pinned widths line up rows printed apart.
//   3. turn_lines: each line is ONE row at any width.

#include "doctest/doctest.h"

#include <maya/print.hpp>
#include <maya/widget/agentty/report.hpp>
#include <maya/widget/agentty/run_transcript.hpp>
#include <maya/widget/agentty/turn_lines.hpp>

#include <string>
#include <vector>

using namespace maya;

namespace {

bool has_escape(const std::string& s) { return s.find('\x1b') != std::string::npos; }

std::vector<std::string> lines_of(const std::string& s) {
    std::vector<std::string> out;
    std::size_t at = 0;
    while (at <= s.size()) {
        auto nl = s.find('\n', at);
        if (nl == std::string::npos) nl = s.size();
        std::string l = s.substr(at, nl - at);
        while (!l.empty() && l.back() == ' ') l.pop_back();
        out.push_back(l);
        at = nl + 1;
    }
    while (!out.empty() && out.back().empty()) out.pop_back();
    return out;
}

} // namespace

TEST_CASE("RunTranscript: plain words only off a colour tty") {
    const RunTranscript t{{.styled = false, .rows = 40}};
    CHECK_FALSE(t.pinned());
    CHECK(t.clear().empty());
    CHECK(t.begin_status().empty());
    CHECK(t.tick(3, "npm", 0).empty());
    CHECK(t.end_status().empty());
    CHECK(t.clear_line().empty());
    for (const auto& s : {t.header("npm test"), t.captured_header("npm test"),
                          t.footer(RunTranscript::Outcome::Done, 0, 2), t.continue_prompt()})
        CHECK_FALSE(has_escape(s));
    CHECK(t.header("npm test").find("$ npm test\n") != std::string::npos);
    CHECK(t.footer(RunTranscript::Outcome::Failed, 2, 7)
          == "\n\xe2\x95\xb0\xe2\x94\x80 \xe2\x9c\x97 failed  exit 2  \xc2\xb7  7s\n");
}

TEST_CASE("RunTranscript: the pinned row and its teardown") {
    const RunTranscript t{{.styled = true, .rows = 30}};
    CHECK(t.pinned());
    CHECK(t.begin_status() == "\x1b[1;29r\x1b[29;1H");
    const std::string beat = t.tick(12, "npm", 1);
    CHECK(beat.find("\x1b]2;\xe2\x97\x8f 12s \xe2\x80\x94 npm \xe2\x80\x94 agentty\x07") == 0);
    CHECK(beat.find("\x1b[s\x1b[30;1H\x1b[2K") != std::string::npos);
    CHECK(beat.find("running\xe2\x80\xa6 12s \xc2\xb7 npm \xc2\xb7 Ctrl-C to stop") != std::string::npos);
    CHECK(beat.ends_with("\x1b[u"));
    CHECK(t.end_status() == "\x1b[r\x1b[30;1H\x1b[2K\x1b[29;1H\x1b]2;\x07");

    // The header rule: "╭─ running ─ Ctrl-C to stop " then 50 more ─.
    std::string rule = "\x1b[2m\xe2\x95\xad\xe2\x94\x80 running \xe2\x94\x80 Ctrl-C to stop ";
    for (int i = 0; i < 50; ++i) rule += "\xe2\x94\x80";
    CHECK(t.header("ls") == rule + "\x1b[0m\n\x1b[36m$ \x1b[0m\x1b[1mls\x1b[0m\n");

    const RunTranscript tiny{{.styled = true, .rows = 2}};
    CHECK_FALSE(tiny.pinned());
    CHECK(tiny.begin_status().empty());
    CHECK(tiny.tick(1, "x", 0).find("\x1b[s") == std::string::npos);   // title only
}

TEST_CASE("report::Table aligns by display width") {
    report::Table t;
    t.indent = 0;
    t.rows.push_back({{"concern"}, {"reads"}, {"writes"}});
    t.rows.push_back({{"mcp"}, {"\xe2\x80\x94"}, {"/a"}});      // em dash: 3 bytes, 1 col
    t.rows.push_back({{"credentials"}, {"2"}, {"/b"}});
    const auto ls = lines_of(render_to_string(t.build(), 80));
    REQUIRE(ls.size() == 3);
    // Every last column starts at the same display column.
    auto col_of = [](const std::string& l, const std::string& needle) {
        const auto at = l.find(needle);
        return string_width(l.substr(0, at));
    };
    CHECK(col_of(ls[0], "writes") == col_of(ls[1], "/a"));
    CHECK(col_of(ls[1], "/a") == col_of(ls[2], "/b"));
}

TEST_CASE("report::Table widths line up rows printed one at a time") {
    auto one = [](std::string a, std::string b) {
        report::Table t;
        t.indent = 0;
        t.widths = {10};
        t.rows.push_back({{std::move(a)}, {std::move(b)}});
        return lines_of(render_to_string(t.build(), 80)).at(0);
    };
    CHECK(one("a", "x").find('x') == one("abcdefgh", "x").find('x'));
}

TEST_CASE("turn_lines are one row at any width") {
    const RouteLine r{.model = "claude-sonnet-4-5-with-a-long-name", .effort = "high",
                      .complexity = "complex", .note = "a long note that would wrap a narrow row"};
    RetrievalMeta m;
    m.confidence = 0.46;
    m.kinds = {{"docs", 2}, {"memory", 1}};
    m.passages = 3;
    for (int w : {20, 40, 120}) {
        CHECK(lines_of(render_to_string(r.build(), w)).size() == 1);
        CHECK(lines_of(render_to_string(m.build(), w)).size() == 1);
        CHECK(lines_of(render_to_string(ForkNote{.path = "/very/long/path/to/a/thread/transcript.md"}.build(), w)).size() == 1);
    }
    const auto wide = lines_of(render_to_string(m.build(), 120)).at(0);
    CHECK(wide.find("46%\xc2\xb7moderate") != std::string::npos);
    CHECK(wide.find("2 doc \xc2\xb7 1 mem  (3)") != std::string::npos);
}
