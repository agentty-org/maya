// examples/agent.cpp — a simulated Claude-Code-style agent session, inline.
//
// Demonstrates all agent UX widgets in a realistic conversation flow:
//   User prompt → thinking → tool calls → streaming response → summary
//
// Controls:
//   space      advance to next phase / speed up streaming
//   enter      send the pre-filled user prompt
//   tab        toggle expanded/collapsed tool output
//   r          restart simulation
//   q/Esc      quit

#include <maya/host/run.hpp>
#include <maya/maya.hpp>

// Agent UX widgets
#include <maya/widget/activity_bar.hpp>
#include <maya/widget/api_usage.hpp>
#include <maya/widget/bash_tool.hpp>
#include <maya/widget/context_window.hpp>
#include <maya/widget/cost_tracker.hpp>
#include <maya/widget/edit_tool.hpp>
#include <maya/widget/error_block.hpp>
#include <maya/widget/file_changes.hpp>
#include <maya/widget/git_status.hpp>
#include <maya/widget/agentty/markdown.hpp>
#include <maya/widget/message.hpp>
#include <maya/widget/agentty/model_badge.hpp>
#include <maya/widget/agentty/permission.hpp>
#include <maya/widget/read_tool.hpp>
#include <maya/widget/streaming_cursor.hpp>
#include <maya/widget/system_banner.hpp>
#include <maya/widget/thinking.hpp>
#include <maya/widget/token_stream.hpp>
#include <maya/widget/tool_call.hpp>
#include <maya/widget/turn_divider.hpp>
#include <maya/widget/write_tool.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

using namespace maya;
using namespace maya::dsl;

// ── Simulation phases ───────────────────────────────────────────────────────

enum class Phase : int {
    Idle,             // waiting for user input
    UserSent,         // user message displayed
    Thinking,         // thinking block expanding
    ReadFile,         // reading auth.ts
    ReadDone,         // read complete
    PermissionAsk,    // ask permission to edit
    PermissionGrant,  // permission granted
    EditFile,         // editing auth.ts
    EditDone,         // edit applied
    RunTests,         // running test suite
    TestsDone,        // tests passed
    WriteFile,        // writing new test file
    WriteDone,        // write complete
    Streaming,        // streaming response
    Complete,         // done — show summary
};

static Phase next_phase(Phase p) {
    int n = static_cast<int>(p) + 1;
    if (n > static_cast<int>(Phase::Complete)) return Phase::Complete;
    return static_cast<Phase>(n);
}

// ── Program ─────────────────────────────────────────────────────────────────

struct Agent {
    struct Model {
        Phase phase         = Phase::Idle;
        float phase_time    = 0.f;   // time in current phase
        float total_time    = 0.f;
        int   frame         = 0;
        int   stream_chars  = 0;     // chars revealed in streaming
        bool  tool_expanded = true;
        int   context_used  = 34200;

        // Thinking content builds up
        std::string thinking_text;

        // Streaming response text
        std::string response_text;

        // Cost tracking
        int input_tokens  = 0;
        int output_tokens = 0;
        int cache_read    = 0;
        float cost_usd    = 0.f;
    };

    struct Tick {};
    struct Advance {};    // space — advance phase
    struct Send {};       // enter — send user message
    struct ToggleTool {}; // tab — expand/collapse
    struct Restart {};
    struct Quit {};
    using Msg = std::variant<Tick, Advance, Send, ToggleTool, Restart, Quit>;

    using Cmd = jaal::Cmd<Msg>;
    using Sub = jaal::Sub<Msg>;

    // ── The full response text the agent "types out" ─────────────────────

    static constexpr const char* full_response =
        "I found and fixed the authentication bug. Here's what was wrong:\n"
        "\n"
        "The `validateToken()` function in `src/auth.ts` was comparing the token "
        "expiry timestamp using `<` instead of `<=`, which caused tokens to be "
        "rejected exactly at their expiry second. This is a classic off-by-one "
        "error in time comparisons.\n"
        "\n"
        "**Changes made:**\n"
        "- Fixed the comparison operator in `validateToken()` from `<` to `<=`\n"
        "- Added a 5-second grace period for clock skew between services\n"
        "- Wrote a regression test covering the boundary condition\n"
        "\n"
        "All 47 tests pass, including the new one. The fix is minimal and "
        "backwards-compatible.";

    static constexpr const char* thinking_full =
        "The user wants me to fix an authentication bug. Let me start by reading "
        "the auth module to understand the current implementation...\n\n"
        "I should look at the token validation logic specifically — off-by-one "
        "errors in expiry checks are a common source of auth bugs. Let me check "
        "the comparison operators and boundary conditions.";

    // ── update ───────────────────────────────────────────────────────────

    static Cmd update(Model& m, Tick) {
            {
                float dt = 0.05f;
                m.frame++;
                m.phase_time += dt;
                m.total_time += dt;

                // Phase-specific tick logic
                switch (m.phase) {
                case Phase::UserSent:
                    if (m.phase_time > 0.6f) {
                        m.phase = Phase::Thinking;
                        m.phase_time = 0.f;
                    }
                    break;
                case Phase::Thinking: {
                    // Gradually reveal thinking text
                    size_t target = static_cast<size_t>(m.phase_time * 80.f);
                    size_t full_len = std::strlen(thinking_full);
                    if (target > full_len) target = full_len;
                    m.thinking_text = std::string(thinking_full, target);
                    m.input_tokens += 12;
                    m.context_used += 8;

                    if (m.phase_time > 3.5f) {
                        m.phase = Phase::ReadFile;
                        m.phase_time = 0.f;
                    }
                    break;
                }
                case Phase::ReadFile:
                    m.input_tokens += 45;
                    m.context_used += 42;
                    if (m.phase_time > 1.2f) {
                        m.phase = Phase::ReadDone;
                        m.phase_time = 0.f;
                        m.cache_read += 840;
                    }
                    break;
                case Phase::ReadDone:
                    if (m.phase_time > 0.5f) {
                        m.phase = Phase::PermissionAsk;
                        m.phase_time = 0.f;
                    }
                    break;
                case Phase::PermissionAsk:
                    // Wait for user to advance
                    break;
                case Phase::PermissionGrant:
                    if (m.phase_time > 0.4f) {
                        m.phase = Phase::EditFile;
                        m.phase_time = 0.f;
                    }
                    break;
                case Phase::EditFile:
                    m.output_tokens += 8;
                    if (m.phase_time > 1.8f) {
                        m.phase = Phase::EditDone;
                        m.phase_time = 0.f;
                        m.cost_usd += 0.003f;
                    }
                    break;
                case Phase::EditDone:
                    if (m.phase_time > 0.3f) {
                        m.phase = Phase::RunTests;
                        m.phase_time = 0.f;
                    }
                    break;
                case Phase::RunTests:
                    if (m.phase_time > 2.5f) {
                        m.phase = Phase::TestsDone;
                        m.phase_time = 0.f;
                    }
                    break;
                case Phase::TestsDone:
                    if (m.phase_time > 0.3f) {
                        m.phase = Phase::WriteFile;
                        m.phase_time = 0.f;
                    }
                    break;
                case Phase::WriteFile:
                    m.output_tokens += 15;
                    if (m.phase_time > 1.5f) {
                        m.phase = Phase::WriteDone;
                        m.phase_time = 0.f;
                        m.cost_usd += 0.002f;
                    }
                    break;
                case Phase::WriteDone:
                    if (m.phase_time > 0.3f) {
                        m.phase = Phase::Streaming;
                        m.phase_time = 0.f;
                    }
                    break;
                case Phase::Streaming: {
                    // Type out response at ~80 chars/tick
                    int target = static_cast<int>(m.phase_time * 80.f);
                    int full_len = static_cast<int>(std::strlen(full_response));
                    m.stream_chars = std::min(target, full_len);
                    m.response_text = std::string(full_response,
                        static_cast<size_t>(m.stream_chars));
                    m.output_tokens += 6;
                    m.cost_usd += 0.0001f;

                    if (m.stream_chars >= full_len) {
                        m.phase = Phase::Complete;
                        m.phase_time = 0.f;
                        m.cost_usd = 0.042f; // final cost
                    }
                    break;
                }
                default:
                    break;
                }

                return {};
            }
    }

    static Cmd update(Model& m, Advance) {
                if (m.phase == Phase::PermissionAsk) {
                    m.phase = Phase::PermissionGrant;
                    m.phase_time = 0.f;
                } else if (m.phase == Phase::Complete || m.phase == Phase::Idle) {
                    // no-op
                } else {
                    // Speed through current phase
                    m.phase_time += 5.f;
                }
                return {};
    }

    static Cmd update(Model& m, Send) {
                if (m.phase == Phase::Idle) {
                    m.phase = Phase::UserSent;
                    m.phase_time = 0.f;
                    m.input_tokens = 1240;
                    m.cache_read = 12800;
                    m.context_used += 1240;
                    // Auto-advance to thinking after brief pause
                }
                return {};
    }

    static Cmd update(Model& m, ToggleTool) {
                m.tool_expanded = !m.tool_expanded;
                return {};
    }

    static Cmd update(Model& m, Restart) { m = Model{}; return {}; }

    static Cmd update(Model&, Quit) {
        return Cmd::quit(0);
    }

    // ── Colors ───────────────────────────────────────────────────────────

    static constexpr Color purple  = Color::rgb(198, 120, 221);
    static constexpr Color blue    = Color::rgb(97, 175, 239);
    static constexpr Color green   = Color::rgb(152, 195, 121);
    static constexpr Color amber   = Color::rgb(229, 192, 123);
    static constexpr Color red     = Color::rgb(224, 108, 117);
    static constexpr Color cyan    = Color::rgb(86, 182, 194);
    static constexpr Color fg      = Color::rgb(200, 204, 212);
    static constexpr Color muted   = Color::rgb(127, 132, 142);
    static constexpr Color dim_col = Color::rgb(62, 68, 81);
    static constexpr Color bg_dark = Color::rgb(33, 37, 43);

    // ── View helpers ─────────────────────────────────────────────────────

    static Element build_header(const Model& m) {
        ModelBadge badge({.label = "Opus 4.6"});

        GitStatusWidget git;
        git.set_branch("fix/auth-bug");
        git.set_ahead(0);
        git.set_dirty(1, 0, 0);

        auto phase_label = [&]() -> const char* {
            switch (m.phase) {
                case Phase::Idle:         return "waiting";
                case Phase::UserSent:     return "received";
                case Phase::Thinking:     return "thinking";
                case Phase::ReadFile:
                case Phase::ReadDone:     return "reading";
                case Phase::PermissionAsk:
                case Phase::PermissionGrant: return "permission";
                case Phase::EditFile:
                case Phase::EditDone:     return "editing";
                case Phase::RunTests:
                case Phase::TestsDone:    return "testing";
                case Phase::WriteFile:
                case Phase::WriteDone:    return "writing";
                case Phase::Streaming:    return "responding";
                case Phase::Complete:     return "complete";
            }
            return "";
        };

        Color phase_color = m.phase == Phase::Complete ? green
                          : m.phase == Phase::Idle ? muted : purple;

        return h(
            badge.build(),
            text("   "),
            git.build(),
            text("   "),
            text(phase_label(), Style{}.with_fg(phase_color).with_bold()),
            text("  "),
            text(std::format("{:.1f}s", static_cast<double>(m.total_time)),
                Style{}.with_fg(muted))
        ).build();
    }

    static Element build_banner(const Model& m) {
        int pct = (m.context_used * 100) / 200000;
        if (pct < 40) return text("");

        BannerLevel level = pct > 85 ? BannerLevel::Error
                          : pct > 70 ? BannerLevel::Warning
                          : BannerLevel::Info;
        SystemBanner banner(
            "Context window " + std::to_string(pct) + "% used",
            level);
        return banner.build();
    }

    static Element build_user_turn() {
        return v(
            TurnDivider(TurnRole::User, 1).build(),
            text(""),
            UserMessage::build("Fix the authentication bug in src/auth.ts — "
                "users are getting logged out randomly after exactly 1 hour"),
            text("")
        ).build();
    }

    static Element build_thinking(const Model& m) {
        ThinkingBlock tb;
        tb.set_active(m.phase == Phase::Thinking);
        tb.set_expanded(true);
        tb.set_content(m.thinking_text);
        return tb.build();
    }

    static Element build_read_tool(const Model& m) {
        ReadTool rt("src/auth.ts");
        rt.set_expanded(m.tool_expanded);
        rt.set_max_lines(8);
        rt.set_total_lines(42);

        bool done = static_cast<int>(m.phase) > static_cast<int>(Phase::ReadFile);
        if (done) {
            rt.set_status(ReadStatus::Success);
            rt.set_elapsed(1.2f);
            rt.set_content(
                "import { verify } from 'jsonwebtoken';\n"
                "\n"
                "export function validateToken(token: string): boolean {\n"
                "  const decoded = verify(token, SECRET);\n"
                "  const now = Math.floor(Date.now() / 1000);\n"
                "  if (decoded.exp < now) {   // BUG: should be <=\n"
                "    throw new TokenExpiredError('Token expired');\n"
                "  }\n"
                "  return true;\n"
                "}\n");
        } else {
            rt.set_status(ReadStatus::Reading);
            rt.set_elapsed(m.phase_time);
        }
        return rt.build();
    }

    static Element build_permission() {
        Permission perm("Edit", "Apply changes to src/auth.ts");
        perm.set_result(PermissionResult::Pending);
        return perm.build();
    }

    static Element build_edit_tool(const Model& m) {
        EditTool et("src/auth.ts");
        et.set_expanded(m.tool_expanded);
        et.set_old_text("  if (decoded.exp < now) {");
        et.set_new_text("  if (decoded.exp <= now + 5) {  // fix: <= with 5s grace");

        bool done = static_cast<int>(m.phase) > static_cast<int>(Phase::EditFile);
        if (done) {
            et.set_status(EditStatus::Applied);
            et.set_elapsed(1.8f);
        } else {
            et.set_status(EditStatus::Applying);
            et.set_elapsed(m.phase_time);
        }
        return et.build();
    }

    static Element build_bash_tool(const Model& m) {
        BashTool bt("npm test");
        bt.set_expanded(m.tool_expanded);
        bt.set_max_output_lines(6);

        bool done = static_cast<int>(m.phase) > static_cast<int>(Phase::RunTests);
        if (done) {
            bt.set_status(BashStatus::Success);
            bt.set_exit_code(0);
            bt.set_elapsed(2.4f);
            bt.set_output(
                "> auth-service@2.1.0 test\n"
                "> jest --coverage\n"
                "\n"
                " PASS  src/__tests__/auth.test.ts\n"
                " PASS  src/__tests__/middleware.test.ts\n"
                " PASS  src/__tests__/token-expiry.test.ts\n"
                "\n"
                "Test Suites: 3 passed, 3 total\n"
                "Tests:       47 passed, 47 total\n"
                "Time:        2.341s");
        } else {
            bt.set_status(BashStatus::Running);
            bt.set_elapsed(m.phase_time);
            int lines = static_cast<int>(m.phase_time * 2.f);
            std::string partial = "> auth-service@2.1.0 test\n> jest --coverage\n";
            if (lines > 2) partial += "\n PASS  src/__tests__/auth.test.ts\n";
            if (lines > 3) partial += " PASS  src/__tests__/middleware.test.ts\n";
            if (lines > 4) partial += " PASS  src/__tests__/token-expiry.test.ts\n";
            bt.set_output(partial);
        }
        return bt.build();
    }

    static Element build_write_tool(const Model& m) {
        WriteTool wt("src/__tests__/token-expiry.test.ts");
        wt.set_expanded(m.tool_expanded);
        wt.set_max_preview_lines(6);

        bool done = static_cast<int>(m.phase) > static_cast<int>(Phase::WriteFile);
        if (done) {
            wt.set_status(WriteStatus::Written);
            wt.set_elapsed(1.4f);
            wt.set_content(
                "import { validateToken } from '../auth';\n"
                "\n"
                "describe('token expiry boundary', () => {\n"
                "  it('should accept token at exact expiry second', () => {\n"
                "    const token = createToken({ exp: now() });\n"
                "    expect(() => validateToken(token)).not.toThrow();\n"
                "  });\n"
                "});\n");
        } else {
            wt.set_status(WriteStatus::Writing);
            wt.set_elapsed(m.phase_time);
        }
        return wt.build();
    }

    static Element build_response(const Model& m) {
        if (m.phase == Phase::Streaming) {
            StreamingCursor cursor;
            cursor.set_style(CursorStyle::Dots);
            cursor.set_label("Generating...");
            // Advance cursor based on frame
            for (int i = 0; i < m.frame; ++i) cursor.tick();

            return v(
                markdown(m.response_text),
                text(""),
                cursor.build()
            ).build();
        }
        // Complete
        return markdown(m.response_text);
    }

    static Element build_file_changes() {
        FileChanges fc;
        fc.add("src/auth.ts", FileChangeKind::Modified, 2, 1);
        fc.add("src/__tests__/token-expiry.test.ts", FileChangeKind::Created, 8, 0);
        return fc.build();
    }

    static Element build_cost_summary(const Model& m) {
        CostTracker cost;
        cost.add_turn({
            .input_tokens  = m.input_tokens,
            .output_tokens = m.output_tokens,
            .cache_read    = m.cache_read,
            .cache_write   = 0,
            .cost_usd      = m.cost_usd,
            .latency_ms    = m.total_time * 1000.f,
        });
        return cost.build();
    }

    static Element build_context_bar(const Model& m) {
        ContextWindow ctx(200000);
        ctx.set_width(52);
        ctx.add_segment("System",   12400, Color::rgb(97, 175, 239));
        ctx.add_segment("History",  static_cast<int>(m.cache_read),
                        Color::rgb(198, 120, 221));
        ctx.add_segment("Tools",    m.input_tokens,
                        Color::rgb(229, 192, 123));
        ctx.add_segment("Response", m.output_tokens,
                        Color::rgb(152, 195, 121));
        return ctx.build();
    }

    static Element build_activity_bar(const Model& m) {
        ActivityBar bar;
        bar.set_model("claude-opus-4-6");
        bar.set_tokens(m.input_tokens, m.output_tokens);
        bar.set_cost(m.cost_usd);
        bar.set_context_percent((m.context_used * 100) / 200000);
        bar.set_status(
            m.phase == Phase::Complete ? "done"
          : m.phase == Phase::Idle ? "ready"
          : "working");
        return bar.build();
    }

    static Element build_api_usage(const Model& m) {
        APIUsage api;
        api.set_compact(true);
        api.set_requests(1, 60);
        api.set_tokens(m.input_tokens + m.output_tokens, 200000);
        api.set_latency_ms(static_cast<int>(m.total_time * 1000.f));
        return api.build();
    }

    static Element build_prompt() {
        return h(
            text("\xe2\x9d\xaf ", Style{}.with_fg(purple).with_bold()),
            text("Fix the authentication bug in src/auth.ts",
                Style{}.with_fg(fg)),
            text("  (enter to send)", Style{}.with_fg(muted).with_dim())
        ).build();
    }

    // ── view ─────────────────────────────────────────────────────────────

    static Element view(const Model& m) {
        std::vector<Element> rows;

        // Header bar
        rows.push_back(build_header(m));
        rows.push_back(text(""));

        // System banner (only if context is getting high)
        auto banner = build_banner(m);
        rows.push_back(std::move(banner));

        // ── Idle: show prompt ────────────────────────────────────────
        if (m.phase == Phase::Idle) {
            rows.push_back(text(""));
            rows.push_back(build_prompt());
            rows.push_back(text(""));
            rows.push_back(text("Press enter to send, q to quit",
                Style{}.with_fg(muted).with_dim()));

        } else {
            // ── User message ─────────────────────────────────────────
            rows.push_back(build_user_turn());

            // ── Assistant turn divider ───────────────────────────────
            if (static_cast<int>(m.phase) >= static_cast<int>(Phase::Thinking)) {
                rows.push_back(TurnDivider(TurnRole::Assistant, 1).build());
                rows.push_back(text(""));
            }

            // ── Thinking ─────────────────────────────────────────────
            if (static_cast<int>(m.phase) >= static_cast<int>(Phase::Thinking)) {
                rows.push_back(build_thinking(m));
                rows.push_back(text(""));
            }

            // ── Read tool ────────────────────────────────────────────
            if (static_cast<int>(m.phase) >= static_cast<int>(Phase::ReadFile)) {
                rows.push_back(build_read_tool(m));
                rows.push_back(text(""));
            }

            // ── Permission prompt ────────────────────────────────────
            if (m.phase == Phase::PermissionAsk) {
                rows.push_back(build_permission());
                rows.push_back(text(""));
                rows.push_back(text("Press space to allow",
                    Style{}.with_fg(amber).with_dim()));
            }

            // ── Edit tool ────────────────────────────────────────────
            if (static_cast<int>(m.phase) >= static_cast<int>(Phase::EditFile)) {
                rows.push_back(build_edit_tool(m));
                rows.push_back(text(""));
            }

            // ── Bash tool (tests) ────────────────────────────────────
            if (static_cast<int>(m.phase) >= static_cast<int>(Phase::RunTests)) {
                rows.push_back(build_bash_tool(m));
                rows.push_back(text(""));
            }

            // ── Write tool ───────────────────────────────────────────
            if (static_cast<int>(m.phase) >= static_cast<int>(Phase::WriteFile)) {
                rows.push_back(build_write_tool(m));
                rows.push_back(text(""));
            }

            // ── Streaming response ───────────────────────────────────
            if (static_cast<int>(m.phase) >= static_cast<int>(Phase::Streaming)) {
                rows.push_back(
                    (v(build_response(m)) | padding(0, 1)).build()
                );
                rows.push_back(text(""));
            }

            // ── Completion summary ───────────────────────────────────
            if (m.phase == Phase::Complete) {
                // File changes + cost in a bordered summary card
                auto summary = v(
                    build_file_changes(),
                    text(""),
                    build_cost_summary(m),
                    text(""),
                    build_context_bar(m)
                ) | border(BorderStyle::Round)
                  | bcolor(Color::rgb(50, 54, 62))
                  | btext(" Summary ")
                  | padding(0, 1);

                rows.push_back(std::move(summary));
                rows.push_back(text(""));
            }
        }

        // ── Bottom bar ───────────────────────────────────────────────
        rows.push_back(build_activity_bar(m));

        // Help line
        std::string help_str = m.phase == Phase::Idle
            ? "enter send  q quit"
            : m.phase == Phase::PermissionAsk
            ? "space allow  tab toggle  r restart  q quit"
            : "space skip  tab toggle  r restart  q quit";
        rows.push_back(text(help_str, Style{}.with_fg(dim_col)));

        return (v(std::move(rows)) | pad<1>).build();
    }

    // ── input ────────────────────────────────────────────────────────────

    static std::optional<Msg> on(const Model&, const KeyEvent& k) {
        static const keymap<Msg> keys{
            {'q',                Quit{}},
            {SpecialKey::Escape, Quit{}},
            {' ',                Advance{}},
            {SpecialKey::Tab,    ToggleTool{}},
            {'r',                Restart{}},
            {SpecialKey::Enter,  Send{}},
        };
        return keys(k);
    }

    // ── subscribe ────────────────────────────────────────────────────────

    static Sub subscribe(const Model& m) {
        bool needs_tick =
            m.phase != Phase::Idle && m.phase != Phase::Complete
            && m.phase != Phase::PermissionAsk;

        // Auto-advance from UserSent to Thinking
        bool auto_advance = (m.phase == Phase::UserSent && m.phase_time > 0.5f);

        if (needs_tick || auto_advance)
            return Sub::every(std::chrono::milliseconds(50), Tick{});
        return Sub::none();
    }
};

static_assert(Program<Agent>);

int main() {
    return run<Agent>({.title = "agent", .fps = 20, .mode = Mode::Inline});
}
