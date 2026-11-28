// maya — AI Agent Session Demo (Inline Mode)
//
// A Model/Msg/update/view/subscribe program run with Mode::Inline. A scripted
// timeline plays out on a Sub::every tick (only subscribed while the script
// or a stream is still running — a finished session is a still screen), and
// content streams in character-by-character like a real AI response.
//
// You can type into the composer at the bottom at any time: keys arrive as
// messages carrying the KeyEvent and update() does the editing.
//
//   type / ←→ / Home End / Backspace Del / Ctrl-U Ctrl-W   edit the composer
//   Enter      send the composed message
//   q          quit (when the composer is empty)
//   Esc/Ctrl-C quit
//
// Showcases every tool widget:
//   UserMessage, ThinkingBlock, ReadTool, SearchResult (Grep/Glob),
//   PlanView, EditTool, WriteTool, Permission, BashTool, DiffView,
//   AgentTool, FetchTool, AssistantMessage + StreamingMarkdown,
//   ActivityBar, ToastManager, Badge, Callout

#include <maya/host/run.hpp>
#include <maya/maya.hpp>
#include <maya/widget/activity_bar.hpp>
#include <maya/widget/agent_tool.hpp>
#include <maya/widget/badge.hpp>
#include <maya/widget/bash_tool.hpp>
#include <maya/widget/callout.hpp>
#include <maya/widget/diff_view.hpp>
#include <maya/widget/edit_tool.hpp>
#include <maya/widget/fetch_tool.hpp>
#include <maya/widget/agentty/markdown.hpp>
#include <maya/widget/message.hpp>
#include <maya/widget/agentty/permission.hpp>
#include <maya/widget/agentty/plan_view.hpp>
#include <maya/widget/read_tool.hpp>
#include <maya/widget/search_result.hpp>
#include <maya/widget/thinking.hpp>
#include <maya/widget/toast.hpp>
#include <maya/widget/write_tool.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

using namespace maya;
using namespace maya::dsl;
using namespace std::chrono_literals;

// ── Timed event ──────────────────────────────────────────────────────────────

struct TimedEvent {
    float at;   // seconds from start
    int   id;   // event id
};

// ── App state ────────────────────────────────────────────────────────────────

struct ChatApp {
    float clock = 0.0f;
    int   next_event = 0;

    std::vector<Element> frozen;      // committed conversation elements
    std::vector<TimedEvent> timeline;

    // Live widgets
    ThinkingBlock thinking;
    StreamingMarkdown streaming_md;
    ActivityBar activity;
    ToastManager toasts;

    // Streaming state
    std::string md_target;            // full markdown to stream
    int md_cursor = 0;                // chars streamed so far
    bool streaming = false;
    float stream_rate = 120.0f;       // chars per second
    float stream_accum = 0.0f;

    // Visibility flags
    bool show_thinking = false;
    bool show_permission = false;
    bool show_streaming = false;

    // Composer (the user's own text input)
    std::string composer;
    int         cursor = 0;               // byte offset into composer

    ChatApp() {
        activity.set_model("claude-opus-4");
        activity.set_tokens(0, 0);
        activity.set_context_percent(5);

        float t = 0.5f;

        // ── Turn 1 ──────────────────────────────────────────────────
        timeline.push_back({t, 1});         // user message
        t += 0.8f;
        timeline.push_back({t, 2});         // thinking start
        t += 1.5f;
        timeline.push_back({t, 3});         // read tool
        t += 0.8f;
        timeline.push_back({t, 4});         // grep tool
        t += 0.8f;
        timeline.push_back({t, 5});         // thinking done
        t += 0.3f;
        timeline.push_back({t, 6});         // plan
        t += 1.0f;
        timeline.push_back({t, 7});         // glob
        t += 0.6f;
        timeline.push_back({t, 8});         // edit tool
        t += 0.6f;
        timeline.push_back({t, 9});         // write tool
        t += 0.8f;
        timeline.push_back({t, 10});        // permission prompt
        t += 2.0f;
        timeline.push_back({t, 11});        // permission grant
        t += 0.3f;
        timeline.push_back({t, 12});        // bash tool
        t += 0.8f;
        timeline.push_back({t, 13});        // diff view
        t += 0.5f;
        timeline.push_back({t, 14});        // assistant response (streaming md)
        t += 6.0f;                          // time for streaming to finish
        timeline.push_back({t, 15});        // callout

        // ── Turn 2 ──────────────────────────────────────────────────
        t += 1.5f;
        timeline.push_back({t, 20});        // user message 2
        t += 0.8f;
        timeline.push_back({t, 21});        // thinking start
        t += 1.2f;
        timeline.push_back({t, 22});        // agent + fetch
        t += 1.5f;
        timeline.push_back({t, 23});        // thinking done
        t += 0.3f;
        timeline.push_back({t, 24});        // assistant response 2 (streaming)
        t += 5.0f;
        timeline.push_back({t, 99});        // done
    }
};

static Element gap() {
    return blank().build();
}

// ── Event handlers ───────────────────────────────────────────────────────────

static void fire_event(ChatApp& app, int id) {
    switch (id) {

    // ── Turn 1: User message ─────────────────────────────────────────────
    case 1: {
        app.frozen.push_back(gap());
        app.frozen.push_back(UserMessage::build(
            "Add dark mode support to the settings page. Make sure the "
            "toggle persists across sessions and all components respect the theme."
        ));
        app.activity.set_tokens(156, 0);
        app.activity.set_context_percent(8);
        app.toasts.push("Message sent", ToastLevel::Info);
        break;
    }

    // ── Thinking start ───────────────────────────────────────────────────
    case 2: {
        app.show_thinking = true;
        app.thinking.set_active(true);
        app.thinking.set_content(
            "The user wants dark mode for the settings page.\n"
            "I need to understand the current theme system first.\n"
            "Let me read the theme config and find color references."
        );
        app.activity.set_status("Thinking...");
        break;
    }

    // ── Read tool ────────────────────────────────────────────────────────
    case 3: {
        ReadTool read("src/theme/config.ts");
        read.set_status(ReadStatus::Success);
        read.set_elapsed(0.2f);
        read.set_expanded(true);
        read.set_max_lines(8);
        read.set_total_lines(15);
        read.set_content(
            "export interface ThemeConfig {\n"
            "  primary: string;\n"
            "  background: string;\n"
            "  surface: string;\n"
            "  text: string;\n"
            "  textSecondary: string;\n"
            "}\n"
            "\n"
            "export const lightTheme: ThemeConfig = {\n"
            "  primary: '#6366f1',\n"
            "  background: '#ffffff',\n"
            "  surface: '#f8fafc',\n"
            "  text: '#0f172a',\n"
            "  textSecondary: '#64748b',\n"
            "};"
        );
        app.frozen.push_back(gap());
        app.frozen.push_back(read.build());
        app.activity.set_tokens(156, 240);
        app.activity.set_context_percent(12);
        app.toasts.push("Read src/theme/config.ts", ToastLevel::Success);
        break;
    }

    // ── Grep tool ────────────────────────────────────────────────────────
    case 4: {
        SearchResult grep(SearchKind::Grep, "backgroundColor|surface|primary");
        grep.set_status(SearchStatus::Done);
        grep.set_elapsed(0.4f);
        grep.set_expanded(true);

        SearchFileGroup g1;
        g1.file_path = "src/components/Settings.tsx";
        g1.matches.push_back({12, "  backgroundColor: theme.background,"});
        g1.matches.push_back({28, "  color: theme.primary,"});
        g1.matches.push_back({45, "  backgroundColor: theme.surface,"});
        grep.add_group(std::move(g1));

        SearchFileGroup g2;
        g2.file_path = "src/components/Sidebar.tsx";
        g2.matches.push_back({8, "  backgroundColor: theme.surface,"});
        g2.matches.push_back({19, "  borderColor: theme.primary,"});
        grep.add_group(std::move(g2));

        SearchFileGroup g3;
        g3.file_path = "src/App.tsx";
        g3.matches.push_back({34, "  <ThemeProvider value={lightTheme}>"});
        grep.add_group(std::move(g3));

        app.frozen.push_back(gap());
        app.frozen.push_back(grep.build());
        app.activity.set_tokens(156, 380);
        app.activity.set_context_percent(15);
        break;
    }

    // ── Thinking done ────────────────────────────────────────────────────
    case 5: {
        app.thinking.set_active(false);
        app.show_thinking = false;
        app.frozen.push_back(gap());
        app.frozen.push_back(app.thinking.build());
        app.thinking = ThinkingBlock{};
        app.activity.set_status("");
        break;
    }

    // ── Plan ─────────────────────────────────────────────────────────────
    case 6: {
        PlanView plan;
        plan.add("Read current theme configuration", TaskStatus::Completed);
        plan.add("Search for color references across components", TaskStatus::Completed);
        plan.add("Create dark theme configuration", TaskStatus::InProgress);
        plan.add("Add theme toggle to Settings page", TaskStatus::Pending);
        plan.add("Persist preference in localStorage", TaskStatus::Pending);
        plan.add("Run tests to verify", TaskStatus::Pending);
        app.frozen.push_back(gap());
        app.frozen.push_back(plan.build());
        app.toasts.push("Plan created", ToastLevel::Info);
        break;
    }

    // ── Glob tool ────────────────────────────────────────────────────────
    case 7: {
        SearchResult glob(SearchKind::Glob, "src/**/*.css");
        glob.set_status(SearchStatus::Done);
        glob.set_elapsed(0.1f);
        glob.set_expanded(true);

        SearchFileGroup g;
        g.file_path = "4 files matched";
        g.matches.push_back({0, "src/styles/global.css"});
        g.matches.push_back({0, "src/styles/settings.css"});
        g.matches.push_back({0, "src/styles/components.css"});
        g.matches.push_back({0, "src/styles/animations.css"});
        glob.add_group(std::move(g));

        app.frozen.push_back(gap());
        app.frozen.push_back(glob.build());
        break;
    }

    // ── Edit tool ────────────────────────────────────────────────────────
    case 8: {
        EditTool edit("src/theme/config.ts");
        edit.set_status(EditStatus::Applied);
        edit.set_elapsed(0.1f);
        edit.set_expanded(true);
        edit.set_old_text(
            "export const lightTheme: ThemeConfig = {\n"
            "  primary: '#6366f1',\n"
            "  background: '#ffffff',\n"
            "};"
        );
        edit.set_new_text(
            "export const lightTheme: ThemeConfig = { ... };\n"
            "\n"
            "export const darkTheme: ThemeConfig = {\n"
            "  primary: '#818cf8',\n"
            "  background: '#0f172a',\n"
            "  surface: '#1e293b',\n"
            "  text: '#f1f5f9',\n"
            "  textSecondary: '#94a3b8',\n"
            "};"
        );
        app.frozen.push_back(gap());
        app.frozen.push_back(edit.build());
        app.activity.set_tokens(156, 520);
        app.activity.set_context_percent(22);
        app.toasts.push("Edited src/theme/config.ts", ToastLevel::Success);
        break;
    }

    // ── Write tool ───────────────────────────────────────────────────────
    case 9: {
        WriteTool write("src/hooks/useTheme.ts");
        write.set_status(WriteStatus::Written);
        write.set_elapsed(0.1f);
        write.set_expanded(true);
        write.set_max_preview_lines(8);
        write.set_content(
            "import { useState, useEffect } from 'react';\n"
            "import { lightTheme, darkTheme } from '../theme/config';\n"
            "\n"
            "export function useTheme() {\n"
            "  const [mode, setMode] = useState(() =>\n"
            "    localStorage.getItem('theme') || 'light'\n"
            "  );\n"
            "\n"
            "  useEffect(() => localStorage.setItem('theme', mode), [mode]);\n"
            "\n"
            "  const theme = mode === 'dark' ? darkTheme : lightTheme;\n"
            "  const toggle = () => setMode(m => m === 'dark' ? 'light' : 'dark');\n"
            "  return { theme, mode, toggle };\n"
            "}\n"
        );
        app.frozen.push_back(gap());
        app.frozen.push_back(write.build());
        app.activity.set_tokens(156, 680);
        app.activity.set_context_percent(26);
        app.toasts.push("Created src/hooks/useTheme.ts", ToastLevel::Success);
        break;
    }

    // ── Permission ───────────────────────────────────────────────────────
    case 10: {
        app.show_permission = true;
        app.activity.set_status("Awaiting permission...");
        break;
    }

    case 11: {
        app.show_permission = false;
        app.activity.set_status("");
        app.toasts.push("Permission granted", ToastLevel::Success);
        break;
    }

    // ── Bash tool ────────────────────────────────────────────────────────
    case 12: {
        BashTool bash("npm test -- --run src/hooks/useTheme.test.ts");
        bash.set_status(BashStatus::Success);
        bash.set_elapsed(3.8f);
        bash.set_expanded(true);
        bash.set_output(
            " PASS  src/hooks/useTheme.test.ts\n"
            "  useTheme\n"
            "    \xe2\x9c\x93 defaults to light theme (3ms)\n"
            "    \xe2\x9c\x93 toggles to dark theme (1ms)\n"
            "    \xe2\x9c\x93 persists preference in localStorage (2ms)\n"
            "    \xe2\x9c\x93 loads saved preference on mount (1ms)\n"
            "\n"
            "Test Suites: 1 passed, 1 total\n"
            "Tests:       4 passed, 4 total\n"
            "Time:        1.24s"
        );
        app.frozen.push_back(gap());
        app.frozen.push_back(bash.build());
        app.activity.set_tokens(156, 820);
        app.activity.set_cost(0.02f);
        app.activity.set_context_percent(30);
        break;
    }

    // ── Diff view ────────────────────────────────────────────────────────
    case 13: {
        DiffView diff("src/components/Settings.tsx",
            "@@ -1,6 +1,8 @@\n"
            " import React from 'react';\n"
            "-import { lightTheme } from '../theme/config';\n"
            "+import { useTheme } from '../hooks/useTheme';\n"
            " \n"
            " export function Settings() {\n"
            "-  const theme = lightTheme;\n"
            "+  const { theme, mode, toggle } = useTheme();\n"
            "+\n"
            "+  // Dark mode toggle persists via localStorage\n"
            "   return (\n"
        );
        app.frozen.push_back(gap());
        app.frozen.push_back(diff.build());
        break;
    }

    // ── Assistant response 1 (streaming markdown) ────────────────────────
    case 14: {
        app.streaming = true;
        app.md_cursor = 0;
        app.stream_accum = 0.0f;
        app.stream_rate = 140.0f;
        app.show_streaming = true;
        app.streaming_md.clear();
        app.md_target =
            "I've added dark mode support to the settings page. Here's what I did:\n"
            "\n"
            "## Changes\n"
            "\n"
            "1. **Created `darkTheme`** in `src/theme/config.ts` with accessible dark colors\n"
            "2. **Created `useTheme` hook** in `src/hooks/useTheme.ts` that:\n"
            "   - Persists the user's preference in `localStorage`\n"
            "   - Provides `theme`, `mode`, and `toggle` to components\n"
            "3. **Updated `Settings.tsx`** to use the new hook instead of hardcoded `lightTheme`\n"
            "\n"
            "All **4 tests pass** for the new `useTheme` hook.\n"
            "\n"
            "```tsx\n"
            "const { theme, mode, toggle } = useTheme();\n"
            "```\n"
            "\n"
            "> To add the toggle UI, create a switch component that calls `toggle()` on click.\n";
        app.activity.set_status("Generating...");
        break;
    }

    // ── Callout after turn 1 ─────────────────────────────────────────────
    case 15: {
        app.frozen.push_back(gap());
        app.frozen.push_back(
            Callout::success("All changes applied", "3 files modified, 4 tests passing").build()
        );
        app.activity.set_tokens(156, 1050);
        app.activity.set_cost(0.04f);
        app.activity.set_context_percent(38);
        break;
    }

    // ── Turn 2: User message ─────────────────────────────────────────────
    case 20: {
        app.frozen.push_back(gap());
        app.frozen.push_back(UserMessage::build(
            "Can you check the accessibility contrast ratios for the dark "
            "mode colors? I want to meet WCAG AA."
        ));
        app.activity.set_tokens(1240, 890);
        app.activity.set_context_percent(42);
        app.toasts.push("Message sent", ToastLevel::Info);
        break;
    }

    // ── Turn 2: Thinking ─────────────────────────────────────────────────
    case 21: {
        app.show_thinking = true;
        app.thinking.set_active(true);
        app.thinking.set_content(
            "User wants WCAG AA contrast verification.\n"
            "I should check the contrast ratios programmatically.\n"
            "Let me spawn a sub-agent to research the requirements."
        );
        app.activity.set_status("Thinking...");
        break;
    }

    // ── Agent with nested tools ──────────────────────────────────────────
    case 22: {
        ReadTool nested_read("docs/wcag-guidelines.md");
        nested_read.set_status(ReadStatus::Success);
        nested_read.set_elapsed(0.3f);

        FetchTool fetch("https://www.w3.org/WAI/WCAG21/Understanding/contrast-minimum");
        fetch.set_status(FetchStatus::Done);
        fetch.set_elapsed(1.2f);
        fetch.set_status_code(200);
        fetch.set_content_type("text/html");

        AgentTool agent("Researching WCAG AA contrast requirements");
        agent.set_model("claude-haiku-4-5");
        agent.set_status(AgentStatus::Completed);
        agent.set_elapsed(4.1f);
        agent.set_expanded(true);
        agent.add_tool(nested_read.build());
        agent.add_tool(fetch.build());

        app.frozen.push_back(gap());
        app.frozen.push_back(agent.build());
        app.activity.set_tokens(1240, 1320);
        app.activity.set_cost(0.06f);
        app.activity.set_context_percent(48);
        app.activity.set_status("Thinking...");
        break;
    }

    // ── Turn 2: Thinking done ────────────────────────────────────────────
    case 23: {
        app.thinking.set_active(false);
        app.show_thinking = false;
        app.frozen.push_back(gap());
        app.frozen.push_back(app.thinking.build());
        app.thinking = ThinkingBlock{};
        app.activity.set_status("");
        break;
    }

    // ── Turn 2: Assistant response (streaming) ───────────────────────────
    case 24: {
        app.streaming = true;
        app.md_cursor = 0;
        app.stream_accum = 0.0f;
        app.stream_rate = 120.0f;
        app.show_streaming = true;
        app.streaming_md.clear();
        app.md_target =
            "I checked the contrast ratios for the dark mode palette against "
            "WCAG AA (minimum 4.5:1 for normal text, 3:1 for large text):\n"
            "\n"
            "| Color Pair | Ratio | Status |\n"
            "|---|---|---|\n"
            "| `text` on `background` | **15.4:1** | Pass |\n"
            "| `textSecondary` on `background` | **7.2:1** | Pass |\n"
            "| `primary` on `background` | **5.8:1** | Pass |\n"
            "| `text` on `surface` | **11.3:1** | Pass |\n"
            "\n"
            "All color combinations **meet WCAG AA** standards. The dark "
            "theme is comfortable for extended reading.\n";
        app.activity.set_status("Generating...");
        break;
    }

    // ── Done ─────────────────────────────────────────────────────────────
    case 99: {
        app.activity.set_tokens(1240, 1580);
        app.activity.set_cost(0.08f);
        app.activity.set_context_percent(55);
        app.toasts.push("Session complete — press q to exit", ToastLevel::Success);
        break;
    }

    default: break;
    }
}

// ── Program ──────────────────────────────────────────────────────────────────

namespace {

struct Chat {
    using Model = ChatApp;

    struct Tick {};
    struct Key  { KeyEvent ev; };
    struct Quit {};
    using Msg = std::variant<Tick, Key, Quit>;

    using Cmd = jaal::Cmd<Msg>;
    using Sub = jaal::Sub<Msg>;

    static constexpr float kDt = 1.0f / 30.0f;   // tick period in seconds

    static bool animating(const Model& m) {
        return m.next_event < static_cast<int>(m.timeline.size()) || m.streaming;
    }

    static Cmd update(Model& m, Tick) {
        m.clock += kDt;
        while (m.next_event < static_cast<int>(m.timeline.size())
               && m.clock >= m.timeline[static_cast<size_t>(m.next_event)].at) {
            fire_event(m, m.timeline[static_cast<size_t>(m.next_event)].id);
            m.next_event++;
        }
        if (m.streaming && m.md_cursor < static_cast<int>(m.md_target.size())) {
            m.stream_accum += kDt * m.stream_rate;
            int n = static_cast<int>(m.stream_accum);
            if (n > 0) {
                m.stream_accum -= static_cast<float>(n);
                int end = std::min(m.md_cursor + n, static_cast<int>(m.md_target.size()));
                m.streaming_md.append(std::string_view(
                    m.md_target.data() + m.md_cursor, static_cast<size_t>(end - m.md_cursor)));
                m.md_cursor = end;
            }
        } else if (m.streaming) {
            m.streaming = false;
            m.streaming_md.finish();
            m.frozen.push_back(gap());
            m.frozen.push_back(AssistantMessage::build(m.streaming_md.build()));
            m.streaming_md.clear();
            m.show_streaming = false;
            m.activity.set_status("");
            m.toasts.push("Response complete", ToastLevel::Success);
        }
        return {};
    }

    static int prev_cp(const std::string& s, int i) {
        if (i <= 0) return 0;
        --i;
        while (i > 0 && (static_cast<unsigned char>(s[static_cast<size_t>(i)]) & 0xC0) == 0x80) --i;
        return i;
    }
    static int next_cp(const std::string& s, int i) {
        int n = static_cast<int>(s.size());
        if (i >= n) return n;
        ++i;
        while (i < n && (static_cast<unsigned char>(s[static_cast<size_t>(i)]) & 0xC0) == 0x80) ++i;
        return i;
    }

    static Cmd update(Model& m, Key k) {
        const KeyEvent& ev = k.ev;
        auto& s = m.composer;
        if (ctrl_is(ev, 'c')) return Cmd::quit(0);
        if (ctrl_is(ev, 'u')) { s.erase(0, static_cast<size_t>(m.cursor)); m.cursor = 0; return {}; }
        if (ctrl_is(ev, 'w')) {
            int p = m.cursor;
            while (p > 0 && s[static_cast<size_t>(p - 1)] == ' ') --p;
            while (p > 0 && s[static_cast<size_t>(p - 1)] != ' ') --p;
            s.erase(static_cast<size_t>(p), static_cast<size_t>(m.cursor - p));
            m.cursor = p;
            return {};
        }
        if (key_is(ev, SpecialKey::Escape)) return Cmd::quit(0);
        if (key_is(ev, SpecialKey::Enter)) {
            if (s.empty()) return {};
            m.frozen.push_back(gap());
            m.frozen.push_back(UserMessage::build(s));
            m.toasts.push("Message sent", ToastLevel::Info);
            s.clear();
            m.cursor = 0;
            return {};
        }
        if (key_is(ev, SpecialKey::Backspace)) {
            int p = prev_cp(s, m.cursor);
            s.erase(static_cast<size_t>(p), static_cast<size_t>(m.cursor - p));
            m.cursor = p;
            return {};
        }
        if (key_is(ev, SpecialKey::Delete)) {
            int p = next_cp(s, m.cursor);
            s.erase(static_cast<size_t>(m.cursor), static_cast<size_t>(p - m.cursor));
            return {};
        }
        if (key_is(ev, SpecialKey::Left))  { m.cursor = prev_cp(s, m.cursor); return {}; }
        if (key_is(ev, SpecialKey::Right)) { m.cursor = next_cp(s, m.cursor); return {}; }
        if (key_is(ev, SpecialKey::Home))  { m.cursor = 0; return {}; }
        if (key_is(ev, SpecialKey::End))   { m.cursor = static_cast<int>(s.size()); return {}; }
        if (auto* c = std::get_if<CharKey>(&ev.key); c && !ev.mods.ctrl && !ev.mods.alt) {
            char32_t cp = c->codepoint;
            if (cp == 'q' && s.empty()) return Cmd::quit(0);
            if (cp < 0x20 || cp == 0x7F) return {};
            char buf[4]; int len = 0;
            if (cp < 0x80)         { buf[0] = static_cast<char>(cp); len = 1; }
            else if (cp < 0x800)   { buf[0] = static_cast<char>(0xC0 | (cp >> 6));
                                     buf[1] = static_cast<char>(0x80 | (cp & 0x3F)); len = 2; }
            else if (cp < 0x10000) { buf[0] = static_cast<char>(0xE0 | (cp >> 12));
                                     buf[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                                     buf[2] = static_cast<char>(0x80 | (cp & 0x3F)); len = 3; }
            else                   { buf[0] = static_cast<char>(0xF0 | (cp >> 18));
                                     buf[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                                     buf[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                                     buf[3] = static_cast<char>(0x80 | (cp & 0x3F)); len = 4; }
            s.insert(static_cast<size_t>(m.cursor), buf, static_cast<size_t>(len));
            m.cursor += len;
        }
        return {};
    }

    static Cmd update(Model&, Quit) { return Cmd::quit(0); }

    static Element composer_view(const Model& m) {
        const std::string& s = m.composer;
        size_t cur = static_cast<size_t>(m.cursor);
        std::string content;
        std::vector<StyledRun> runs;
        if (cur > 0) { runs.push_back(StyledRun{0, cur, Style{}}); content = s.substr(0, cur); }
        std::string ch = " ";
        size_t after = cur;
        if (cur < s.size()) {
            size_t e = static_cast<size_t>(next_cp(s, m.cursor));
            ch = s.substr(cur, e - cur);
            after = e;
        }
        runs.push_back(StyledRun{content.size(), ch.size(), Style{}.with_inverse()});
        content += ch;
        if (after < s.size()) {
            runs.push_back(StyledRun{content.size(), s.size() - after, Style{}});
            content += s.substr(after);
        }
        Element inner = s.empty()
            ? h(Element{TextElement{.content = " ", .style = Style{}.with_inverse()}},
                text("Type a message\xe2\x80\xa6  (Enter to send)", Style{}.with_dim())).build()
            : Element{TextElement{.content = std::move(content), .style = Style{}, .runs = std::move(runs)}};
        return (v(h(text("\xe2\x9d\xaf ", Style{}.with_fg(Color::slot(ThemeSlot::Primary))),
                    std::move(inner)).build())
                | border(BorderStyle::Round) | bcolor(Color::slot(ThemeSlot::Primary))
                | padding(0, 1, 0, 1)).build();
    }

    static Element view(const Model& m) {
        std::vector<Element> rows;
        rows.push_back(Element{ElementList{m.frozen}});
        if (m.show_thinking) { rows.push_back(gap()); rows.push_back(m.thinking.build()); }
        if (m.show_permission) {
            rows.push_back(gap());
            rows.push_back(Permission("bash", "npm test -- --run src/hooks/useTheme.test.ts").build());
        }
        if (m.show_streaming) {
            rows.push_back(gap());
            rows.push_back(AssistantMessage::build(m.streaming_md.build()));
        }
        if (!m.toasts.empty()) { rows.push_back(gap()); rows.push_back(m.toasts.build()); }
        rows.push_back(gap());
        rows.push_back(composer_view(m));
        rows.push_back(m.activity.build());
        return v(std::move(rows)).build();
    }

    static std::optional<Msg> on(const Model&, const KeyEvent& k) { return Key{k}; }

    static Sub subscribe(const Model& m) {
        if (animating(m)) return Sub::every(std::chrono::milliseconds{33}, Tick{});
        return Sub::none();
    }
    static bool subs_key(const Model& m) { return animating(m); }
};

static_assert(Program<Chat>);

} // namespace

int main() {
    return run<Chat>({.title = "chat", .fps = 30, .mode = Mode::Inline});
}

