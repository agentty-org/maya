// examples/agent_session.cpp — an auto-piloted AI agent session, inline.
//
// A Claude-Code-style inline TUI driven by a real background "SSE" stream
// (Sub::stream → worker thread → mailbox), with a fully functional
// Composer at the bottom for multi-turn conversation.  Runs end-to-end
// without any input: launches into the first scenario after a beat,
// auto-grants permissions, and cycles indefinitely through all four
// scenarios — useful for unattended demos and recordings.  Press q,
// Esc, or Ctrl-C to exit; type any time to drive it manually instead.
//
// Permission gate:
//   No worker thread blocks waiting for the user. A stream body is
//   captureless and every argument must be Sendable, and a pointer to state
//   two threads mutate is exactly what Sendable rejects. So the decision
//   lives in the MODEL and no thread waits on another:
//     * the worker is a Sub::stream keyed "turn/<n>/<phase>", subscribed
//       only while a turn is in flight and NOT while awaiting permission;
//     * gated scenarios (auth, theme) are split at the gate: phase 1 runs
//       up to and including PermissionAsk, then returns; phase 2 starts
//       with PermissionDone and runs to MessageStop;
//     * the grant is just a message: GrantPerm sets stream_phase = 2, the
//       key changes, and subscribe() starts the phase-2 stream.
//   A turn abandoned mid-flight drops its key, so jaal stops its worker and
//   discards any late messages from it.
//
// What this demos:
//   - Sub::stream — events arrive on a worker thread, dispatched into the
//     pure Elm-style update() loop.
//   - Anthropic-shaped events: SessionStart, ThinkingDelta, ToolBegin /
//     ToolDetailDelta / ToolBodyDelta / ToolEnd, PlanCreated / PlanAdvance,
//     PermissionAsk, AssistantDelta, MessageStop. All delivered with
//     jittered, realistic timing.
//   - Tool widgets MUTATE while their inputs and outputs are still
//     arriving — bash output grows row by row, edit tool flips
//     Pending → Applying → Applied, the AgentTimeline reflects status live.
//   - Functional Composer at bottom: type, backspace, arrow-cycle starter
//     prompts, Tab to cycle, Enter to send. Border + placeholder reflect
//     the agent's State (Idle / Streaming / AwaitingPermission).
//   - Multi-turn: Enter spawns a new task; the prompt's keywords pick the
//     scenario (auth / theme / perf / general).
//   - WelcomeScreen on empty thread (brand splash + starter chips).
//   - SystemBanner appears when context exceeds 60%.
//   - Permission as a model decision + message (worker split at the gate).
//   - Status bar: PhaseChip (breathing), ContextGauge, TokenStreamSparkline
//     (EMA-smoothed live tok/s), ModelBadge.
//   - Final ChangesStrip + Callout summarizes the turn.
//
// Controls:
//   ↑/↓        cycle starter prompts (when composer empty)
//   Tab        cycle starter prompts
//   Enter      send the current message
//   space      grant pending permission (blocks during AwaitingPermission)
//   Backspace  delete last char
//   q / Esc    quit (only when not actively typing)
//   Ctrl-C     force quit anytime
//
// Usage:  ./maya_agent_session

#include <maya/core/target.hpp>
#include <maya/host/run.hpp>
#include <maya/maya.hpp>

#include <maya/widget/agentty/agent_timeline.hpp>
#include <maya/widget/agent_tool.hpp>
#include <maya/widget/bash_tool.hpp>
#include <maya/widget/callout.hpp>
#include <maya/widget/agentty/changes_strip.hpp>
#include <maya/widget/agentty/composer.hpp>
#include <maya/widget/agentty/context_gauge.hpp>
#include <maya/widget/edit_tool.hpp>
#include <maya/widget/fetch_tool.hpp>
#include <maya/widget/file_changes.hpp>
#include <maya/widget/agentty/markdown.hpp>
#include <maya/widget/message.hpp>
#include <maya/widget/agentty/model_badge.hpp>
#include <maya/widget/agentty/permission.hpp>
#include <maya/widget/agentty/phase_chip.hpp>
#include <maya/widget/agentty/plan_view.hpp>
#include <maya/widget/read_tool.hpp>
#include <maya/widget/search_result.hpp>
#include <maya/widget/system_banner.hpp>
#include <maya/widget/thinking.hpp>
#include <maya/widget/todo_list.hpp>
#include <maya/widget/agentty/token_stream_sparkline.hpp>
#include <maya/widget/agentty/turn.hpp>
#include <maya/widget/agentty/welcome_screen.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

using namespace maya;
using namespace maya::dsl;
using namespace std::chrono_literals;

// ============================================================================
// Tool kinds & live records
// ============================================================================

enum class ToolKind { Grep, Read, Edit, Write, Bash, Fetch, Agent, Todo };

static const char* tool_name(ToolKind k) {
    switch (k) {
        case ToolKind::Grep:  return "Grep";
        case ToolKind::Read:  return "Read";
        case ToolKind::Edit:  return "Edit";
        case ToolKind::Write: return "Write";
        case ToolKind::Bash:  return "Bash";
        case ToolKind::Fetch: return "Fetch";
        case ToolKind::Agent: return "Agent";
        case ToolKind::Todo:  return "Todo";
    }
    return "?";
}

static Color tool_color(ToolKind k) {
    switch (k) {
        case ToolKind::Grep:  return Color::cyan();
        case ToolKind::Read:  return Color::blue();
        case ToolKind::Edit:  return Color::magenta();
        case ToolKind::Write: return Color::bright_magenta();
        case ToolKind::Bash:  return Color::green();
        case ToolKind::Fetch: return Color::yellow();
        case ToolKind::Agent: return Color::bright_magenta();
        case ToolKind::Todo:  return Color::bright_cyan();
    }
    return Color::white();
}

struct LiveTool {
    int              id      = 0;
    ToolKind         kind    = ToolKind::Grep;
    std::string      detail;       // file path / command / pattern / url
    std::string      body;          // streamed output / content
    AgentEventStatus status  = AgentEventStatus::Pending;
    float            elapsed = 0.f;
};

// ============================================================================
// Stream events — what the worker thread dispatches into update()
// ============================================================================

namespace ev {

struct SessionStart    {};
struct ThinkingDelta   { std::string text; };
struct ThinkingStop    {};

struct ToolBegin       { int id; ToolKind kind; std::string detail_partial; };
struct ToolDetailDelta { int id; std::string chunk; };
struct ToolBodyDelta   { int id; std::string chunk; };
struct ToolEnd         { int id; bool ok; float elapsed; };

struct PlanCreated     { std::vector<std::string> tasks; };
struct PlanAdvance     { int task_index; };

// Generic "I made a todo list" event — drives the TodoListTool widget.
struct TodoCreated     { std::vector<std::string> items; };
struct TodoAdvance     { int item_index; };

struct PermissionAsk   { std::string command; };
struct PermissionDone  {};   // worker observes the user granted

struct FileChanged     { std::string path; FileChangeKind kind; int added; int removed; };
struct ContextWarning  { std::string text; };

struct AssistantDelta  { std::string text; };
// Closes the current text content block and commits it to scrollback.
// The turn remains active — more tools and more text blocks can follow,
// matching real Anthropic streams that interleave content_block_delta and
// tool_use blocks within a single message.
struct TextBlockStop   {};
struct MessageStop     {};

using Event = std::variant<
    SessionStart,
    ThinkingDelta, ThinkingStop,
    ToolBegin, ToolDetailDelta, ToolBodyDelta, ToolEnd,
    PlanCreated, PlanAdvance,
    TodoCreated, TodoAdvance,
    PermissionAsk, PermissionDone,
    FileChanged, ContextWarning,
    AssistantDelta, TextBlockStop, MessageStop>;

} // namespace ev

// ============================================================================
// Phase — drives the PhaseChip + Composer state
// ============================================================================

enum class Phase : std::uint8_t {
    Idle, Thinking, Tooling, AwaitingPermission, Streaming, Done,
};

static const char* phase_verb(Phase p) {
    switch (p) {
        case Phase::Idle:               return "Ready";
        case Phase::Thinking:           return "Thinking";
        case Phase::Tooling:            return "Working";
        case Phase::AwaitingPermission: return "Permission";
        case Phase::Streaming:          return "Streaming";
        case Phase::Done:               return "Done";
    }
    return "";
}

static Color phase_color(Phase p) {
    switch (p) {
        case Phase::Idle:               return Color::bright_black();
        case Phase::Thinking:           return Color::bright_blue();
        case Phase::Tooling:            return Color::cyan();
        case Phase::AwaitingPermission: return Color::bright_yellow();
        case Phase::Streaming:          return Color::magenta();
        case Phase::Done:               return Color::bright_green();
    }
    return Color::white();
}

static Composer::State composer_state(Phase p) {
    switch (p) {
        case Phase::Idle:
        case Phase::Done:               return Composer::State::Idle;
        case Phase::AwaitingPermission: return Composer::State::AwaitingPermission;
        case Phase::Thinking:
        case Phase::Streaming:          return Composer::State::Streaming;
        case Phase::Tooling:            return Composer::State::ExecutingTool;
    }
    return Composer::State::Idle;
}

// ============================================================================
// Worker↔UI permission gate — no shared state. The grant is a
// message (GrantPerm) that bumps Model::stream_phase; subscribe() then starts
// the post-gate half of the scenario as a new stream. See the top comment.
// ============================================================================

// ============================================================================
// Messages — what update() receives
// ============================================================================

struct Tick      {};
struct GrantPerm {};
struct Quit      {};
struct Stream    { ev::Event ev; };

// Composer input — the App owns the text buffer and dispatches edits.
struct ComposerChar      { char32_t cp; };
struct ComposerBackspace {};
struct ComposerSubmit    {};
struct ComposerCycle     { int delta; };  // ±1 to walk through starters

// Autopilot: auto-submit a starter prompt and cycle to the next when each
// turn finishes. Lets the demo run end-to-end without input — useful for
// recordings, smoke-tests, and unattended display.
struct AutoNext { int starter_idx; };

using Msg = std::variant<
    Tick, GrantPerm, Quit, Stream,
    ComposerChar, ComposerBackspace, ComposerSubmit, ComposerCycle,
    AutoNext>;

// ============================================================================
// Starter prompts — Tab/↑/↓ rotate; each picks a scenario via keyword match
// ============================================================================

static const std::vector<std::string>& starters() {
    static const std::vector<std::string> s = {
        "test_auth.cpp is flaky in CI — fails ~1 in 10 runs with a "
            "deadline_exceeded. Find the cause and fix it.",
        "Add dark mode to the settings page. Persist the preference and "
            "make every component theme-aware.",
        "API p99 latency jumped to 1.4s after the last release. "
            "Profile it and tell me what regressed.",
        "Audit src/ for unwrap()s that could panic in production paths.",
    };
    return s;
}

// ============================================================================
// Plan / Todo row helpers (defined before Model so vectors are complete)
// ============================================================================

struct PlanItem_ { std::string label; TaskStatus status; };
struct TodoItem_ { std::string label; TodoItemStatus status; };

// ============================================================================
// Model — pure state
// ============================================================================

struct Model {
    // Committed scrollback prefix — a maya-measured ScrollbackLedger
    // (Witness Chain, Trim Accounting). seal() appends a settled block
    // with a POLICY row estimate; the paint pass records each block's
    // real laid-out height back into the ledger every frame; trims mint
    // their commit count as a typed ScrollbackDebt (harvest()) that this
    // host cannot fabricate or adjust.
    ScrollbackLedger        frozen;

    // Current turn live state. The "live" buffers (tools, plan, todos,
    // changes, ctx_warning, md) are *cleared* when their content is
    // committed to scrollback, so .empty() / !active is the commit
    // signal. This lets a single turn cleanly cycle through:
    //     thinking → tools → text → tools → text → ... → MessageStop
    // mirroring real Anthropic streams that interleave content blocks.
    std::string             user_prompt;
    bool                    user_committed = false;     // user msg is one-shot

    ThinkingBlock           thinking;
    bool                    thinking_active = false;

    std::vector<LiveTool>   tools;
    std::vector<PlanItem_>  plan;
    std::vector<TodoItem_>  todos;
    TodoListStatus          todos_status = TodoListStatus::Pending;
    float                   todos_elapsed = 0.f;

    std::vector<FileChange> changes;             // accumulated this turn
    int                     turn_changes_count = 0;  // sticky for the Callout
    std::string             ctx_warning;

    StreamingMarkdown       md;
    bool                    md_active = false;

    // Assistant body slots accumulated across the turn. Each commit
    // point (thinking → frozen, tools batch → frozen, text block →
    // frozen) APPENDS to this vector instead of pushing pieces flat
    // to `frozen`. At MessageStop we wrap the whole vector in a
    // Role::Assistant Turn and push that single Element to `frozen`,
    // matching agentty's "one Anthropic message = one Turn" shape.
    std::vector<Element>    assistant_body;

    // Composer state (functional input box).
    std::string             composer_text;
    int                     composer_cursor = 0;
    int                     starter_idx     = -1;  // -1 = user-typed
    std::size_t             queued          = 0;

    // Permission gate.
    std::string             pending_perm_cmd;
    bool                    perm_open = false;

    // The in-flight turn's worker, as the model sees it: the prompt that
    // picks the scenario, and which half of it runs (1 = up to the
    // permission gate / whole scenario if ungated, 2 = after the grant,
    // 0 = no worker). subscribe() turns these into a Sub::stream.
    std::string             turn_prompt;
    int                     stream_phase = 0;

    // Status-bar telemetry.
    Phase                   phase           = Phase::Idle;
    int                     in_tokens       = 0;
    int                     out_tokens      = 0;
    int                     ctx_max         = 200000;
    int                     turn_in_tokens  = 0;       // for cumulative input
    std::vector<float>      tok_history;
    float                   tok_rate        = 0.f;
    std::chrono::steady_clock::time_point last_tok = {};
    int                     spinner_frame   = 0;
    float                   total_elapsed   = 0.f;
    int                     turn_number     = 0;
};

// ============================================================================
// Helpers
// ============================================================================

static LiveTool* find_tool(Model& m, int id) {
    for (auto& t : m.tools) if (t.id == id) return &t;
    return nullptr;
}

// One-row blank — used as an intentional separator between frozen
// scrollback elements (e.g. between the committed actions panel and the
// next assistant bubble). Wraps the DSL's `blank` node.
static Element gap()  { return blank().build(); }

// Zero-height placeholder — used for view slots that are currently
// "empty" (no live thinking, no in-flight tools panel, etc.). The DSL's
// `nothing()` returns an empty ElementList that contributes zero rows
// to the surrounding vstack. Distinct from `blank()` which is a one-row
// spacer; without `nothing` here the 8-of-10 empty slots in view()
// would stack 8 invisible 1-row blanks between scrollback and the
// live tail.
static Element none() { return nothing(); }

static bool contains_ci(std::string_view hay, std::string_view needle) {
    auto lower = [](char c) { return char(std::tolower((unsigned char)c)); };
    if (needle.empty() || needle.size() > hay.size()) return false;
    for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            if (lower(hay[i+j]) != lower(needle[j])) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

// ============================================================================
// Worker — multi-scenario "SSE" simulator
// ============================================================================

namespace sim {

// Stream text in deliberately-sloppy 1-3 byte chunks, including chunks
// that split multi-byte UTF-8 sequences mid-codepoint. This is the
// adversarial case for a TUI renderer — and the proof that maya's
// StreamingMarkdown handles it without ghosting:
//
//   - StreamSink (inside StreamingMarkdown::feed) buffers continuation
//     bytes until each codepoint completes, so the cell grid never
//     sees a partially-decoded glyph.
//   - The two-tier renderer (committed prefix + inline-only tail)
//     guarantees the element-tree height is monotonic in byte count,
//     so partial table rows / list markers / code fences can't
//     reflow the layout while streaming.
//
// We don't have to do anything special on the application side —
// chunking can be byte-level random and it still renders cleanly.
template <typename Send>
void stream_text(std::string_view src, Send&& send_chunk,
                 std::mt19937& rng, int min_ms = 0, int max_ms = 3)
{
    // Demo cadence — text snaps in at near-real-time.  Larger chunks
    // (8-18 bytes) keep dispatch overhead negligible while still
    // exercising the framework's mid-codepoint-split safety net.
    std::uniform_int_distribution<int> chunk_bytes(8, 18);
    std::uniform_int_distribution<int> delay(min_ms, max_ms);
    std::uniform_int_distribution<int> pause_chance(0, 99);
    std::uniform_int_distribution<int> short_pause(8, 22);
    std::uniform_int_distribution<int> long_pause(28, 55);

    std::size_t i = 0;
    while (i < src.size()) {
        int n = std::min<int>(chunk_bytes(rng), int(src.size() - i));
        send_chunk(std::string(src.substr(i, n)));
        i += std::size_t(n);
        std::this_thread::sleep_for(std::chrono::milliseconds(delay(rng)));

        // Natural sentence/line pauses for feel — purely cosmetic.
        if (i > 0 && i < src.size()) {
            char prev = src[i - 1];
            if ((prev == '.' || prev == '!' || prev == '?' || prev == ':')
                && pause_chance(rng) < 60) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(short_pause(rng)));
            } else if (prev == '\n' && pause_chance(rng) < 35) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(short_pause(rng)));
            } else if (pause_chance(rng) < 4) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(long_pause(rng)));
            }
        }
    }
}

using Dispatch = std::function<void(Msg)>;

// Thrown by the stream body's Dispatch when jaal has stopped the stream (the
// model dropped its key: the turn was abandoned or the program is quitting).
// Unwinds the scenario at its next send instead of letting it run on for
// seconds with every message discarded. Caught in the stream body.
struct Cancelled {};

static void send(Dispatch& d, ev::Event e) {
    d(Msg{Stream{std::move(e)}});
}

// ── Helpers shared across scenarios ────────────────────────────────────────

static void stream_bash_lines(Dispatch& d, int tool_id,
                              std::string_view src, std::mt19937& rng,
                              int min_ms = 2, int max_ms = 8)
{
    // Simulate `tail -f`: each line of test output arrives as a chunk
    // with realistic per-line cadence. Slower than text streaming so the
    // user can read each step as it lands.
    std::uniform_int_distribution<int> ld(min_ms, max_ms);
    for (std::size_t i = 0; i < src.size(); ) {
        std::size_t nl  = src.find('\n', i);
        std::size_t end = (nl == std::string_view::npos) ? src.size() : nl + 1;
        d(Msg{Stream{ev::ToolBodyDelta{tool_id,
            std::string(src.data() + i, end - i)}}});
        i = end;
        std::this_thread::sleep_for(std::chrono::milliseconds(ld(rng)));
    }
}

static void say(Dispatch& d, std::string_view text, std::mt19937& rng,
                int min_ms = 32, int max_ms = 78)
{
    stream_text(text,
        [&](std::string s){ send(d, ev::AssistantDelta{std::move(s)}); },
        rng, min_ms, max_ms);
    send(d, ev::TextBlockStop{});
}

// ── Scenario A: auth flaky-test race fix ───────────────────────────────────
// Phase 1: everything up to and including the PermissionAsk.
static void run_auth(Dispatch d, std::mt19937& rng)
{
    constexpr std::string_view THINKING =
        "The user reports a flaky auth test with timeout failures. That "
        "smells like a race between the token-refresh path and the "
        "test's stub clock. The two strongest hypotheses are: (1) a "
        "TOCTOU on the cache freshness check, where two callers race "
        "into the slow path and one of them blocks waiting for the "
        "other, blowing past the test's deadline; or (2) the stub clock "
        "advancing inside a critical section it shouldn't be in. I'll "
        "start by grepping for any existing flaky markers in the test "
        "file, then read both the test body and the implementation it "
        "exercises so I can spot the actual synchronization seam.";

    auto inter = [&]{
        std::uniform_int_distribution<int> g(15, 40);
        std::this_thread::sleep_for(std::chrono::milliseconds(g(rng)));
    };

    send(d, ev::SessionStart{});
    std::this_thread::sleep_for(68ms);

    stream_text(THINKING,
        [&](std::string s){ send(d, ev::ThinkingDelta{std::move(s)}); }, rng);
    send(d, ev::ThinkingStop{});
    inter();

    // ── Tool round 1: Grep ──────────────────────────────────────────
    send(d, ev::ToolBegin{1, ToolKind::Grep, ""});
    stream_text("\"flaky|test_auth\" in tests/",
        [&](std::string s){ send(d, ev::ToolDetailDelta{1, std::move(s)}); },
        rng, 18, 36);
    std::this_thread::sleep_for(35ms);
    send(d, ev::ToolBodyDelta{1,
        "tests/test_auth.cpp:1:// Flaky in CI — investigate.\n"
        "tests/test_auth.cpp:42:    // FIXME: timeout under load\n"
        "tests/test_auth.cpp:61:    EXPECT_NO_TIMEOUT(refresh_pair());\n"
        "tests/CMakeLists.txt:18:    test_auth\n"});
    std::this_thread::sleep_for(22ms);
    send(d, ev::ToolEnd{1, true, 0.31f});
    inter();

    // ── Block A: orient on the grep results ─────────────────────────
    say(d,
        "Four hits across two files. The two interesting ones are line 1 "
        "(an explicit \"flaky in CI\" marker someone left for posterity) "
        "and line 42 (a FIXME about timing out under load). The fact that "
        "those notes are in-tree means whoever wrote the test already had "
        "a hunch about timing — they just never tracked it down. Let me "
        "read the test body to see what concurrency shape it actually "
        "exercises, then read the implementation to look for an "
        "unsynchronized read on shared state.\n",
        rng);
    inter();

    // ── Plan ────────────────────────────────────────────────────────
    send(d, ev::PlanCreated{{
        "Locate the failing test",
        "Read the auth implementation",
        "Patch the TOCTOU race",
        "Run the suite to verify",
    }});
    std::this_thread::sleep_for(27ms);
    send(d, ev::PlanAdvance{0});
    std::this_thread::sleep_for(22ms);

    // ── Tool round 2: Read test + Read impl ─────────────────────────
    send(d, ev::ToolBegin{2, ToolKind::Read, "tests/test_auth.cpp"});
    std::this_thread::sleep_for(45ms);
    send(d, ev::ToolBodyDelta{2,
        "TEST(auth_test, concurrent_refresh_is_coalesced) {\n"
        "    StubClock clk;\n"
        "    FakeNet  net;\n"
        "    Auth a(&clk, &net);\n"
        "    clk.advance(token_lifetime + 1s);          // expire the cache\n"
        "    auto a_fut = std::async([&]{ return a.refresh_token(); });\n"
        "    auto b_fut = std::async([&]{ return a.refresh_token(); });\n"
        "    EXPECT_EQ(a_fut.get(), b_fut.get());\n"
        "    EXPECT_EQ(net.refresh_calls(), 1);          // <-- coalescing\n"
        "}\n"});
    std::this_thread::sleep_for(27ms);
    send(d, ev::ToolEnd{2, true, 0.22f});
    send(d, ev::PlanAdvance{1});
    inter();

    send(d, ev::ToolBegin{3, ToolKind::Read, "src/auth/auth.cpp"});
    std::this_thread::sleep_for(35ms);
    send(d, ev::ToolBodyDelta{3,
        "Token Auth::refresh_token() {\n"
        "    if (clock_->now() < expires_at_) return cached_;  // unsynchronized read\n"
        "    std::lock_guard lk(mu_);\n"
        "    cached_     = net_.fetch_token();\n"
        "    expires_at_ = clock_->now() + token_lifetime;\n"
        "    return cached_;\n"
        "}\n"});
    std::this_thread::sleep_for(27ms);
    send(d, ev::ToolEnd{3, true, 0.21f});
    send(d, ev::PlanAdvance{2});
    inter();

    // ── Block B: explain what's happening, propose the fix ─────────
    say(d,
        "OK — I see it now. Two things stack up here.\n"
        "\n"
        "First, the test asserts `net.refresh_calls() == 1`: when two "
        "threads race in after the cache has expired, **exactly one** of "
        "them is supposed to hit the network and the other is supposed "
        "to return the freshly cached value. That's the \"coalescing\" "
        "the test name advertises.\n"
        "\n"
        "Second, look at line 1 of `refresh_token()` — that early-return "
        "reads `expires_at_` *without holding `mu_`*. The mutex is "
        "acquired one line later, but by then the damage is done: both "
        "threads can observe `now() >= expires_at_`, both proceed past "
        "the guard, both serialize on the lock, and **both** call "
        "`net_.fetch_token()`. The assertion fails because "
        "`refresh_calls()` is 2, not 1. The reason it's only flaky "
        "(rather than always-failing) is that you also need the kernel "
        "scheduler to interleave them just right — which CI runners are "
        "great at doing.\n"
        "\n"
        "The fix is straightforward: pull the freshness check **into** "
        "the critical section so it's the same atomic check-then-act "
        "from every caller's point of view. Let me apply it.\n",
        rng);
    inter();

    // ── Tool round 3: Edit ──────────────────────────────────────────
    send(d, ev::ToolBegin{4, ToolKind::Edit, "src/auth/auth.cpp"});
    std::this_thread::sleep_for(32ms);
    send(d, ev::ToolBodyDelta{4,
        "@@ Auth::refresh_token @@\n"
        "-    if (clock_->now() < expires_at_) return cached_;\n"
        "-    std::lock_guard lk(mu_);\n"
        "-    cached_     = net_.fetch_token();\n"
        "-    expires_at_ = clock_->now() + token_lifetime;\n"
        "-    return cached_;\n"
        "+    std::lock_guard lk(mu_);\n"
        "+    if (clock_->now() < expires_at_) return cached_;\n"
        "+    cached_     = net_.fetch_token();\n"
        "+    expires_at_ = clock_->now() + token_lifetime;\n"
        "+    return cached_;\n"});
    std::this_thread::sleep_for(27ms);
    send(d, ev::ToolEnd{4, true, 0.09f});
    send(d, ev::FileChanged{"src/auth/auth.cpp", FileChangeKind::Modified, 5, 5});
    inter();

    // ── Block C: short bridge to the bash run ──────────────────────
    say(d,
        "Patch in. The diff is mechanical — same five statements, just "
        "with the freshness check moved inside the lock. Now I want to "
        "run the suite to confirm nothing else broke and that the stress "
        "iteration of `concurrent_refresh_is_coalesced` is actually "
        "stable.\n",
        rng);
    inter();

    // ── Permission gate ─────────────────────────────────────────────
    send(d, ev::PermissionAsk{"ctest -R auth_test --output-on-failure"});
    // Phase 1 ends here. The grant is a message to the MODEL; when it
    // arrives, subscribe() starts run_auth_after_gate as a new stream.
}

// Phase 2: everything after the grant. Its locals (BASH, inter, rng) are
// its own — nothing crosses the gate but the prompt.
static void run_auth_after_gate(Dispatch d, std::mt19937& rng)
{
    constexpr std::string_view BASH =
        "Running 4 tests from auth_test\n"
        "[==========] Running 4 tests from 1 test suite.\n"
        "[----------] Global test environment set-up.\n"
        "[ RUN      ] auth_test.refreshes_before_expiry\n"
        "[       OK ] auth_test.refreshes_before_expiry (12 ms)\n"
        "[ RUN      ] auth_test.expired_token_triggers_refresh\n"
        "[       OK ] auth_test.expired_token_triggers_refresh (8 ms)\n"
        "[ RUN      ] auth_test.concurrent_refresh_is_coalesced\n"
        "    note: stress-running 200 iterations under TSAN\n"
        "[       OK ] auth_test.concurrent_refresh_is_coalesced (24 ms)\n"
        "[ RUN      ] auth_test.refresh_during_grace_window\n"
        "[       OK ] auth_test.refresh_during_grace_window (15 ms)\n"
        "[----------] 4 tests from auth_test (61 ms total)\n"
        "[----------] Global test environment tear-down.\n"
        "[==========] 4 tests passed.\n";

    auto inter = [&]{
        std::uniform_int_distribution<int> g(15, 40);
        std::this_thread::sleep_for(std::chrono::milliseconds(g(rng)));
    };

    send(d, ev::PermissionDone{});
    std::this_thread::sleep_for(35ms);

    // ── Tool round 4: Bash ──────────────────────────────────────────
    send(d, ev::ToolBegin{5, ToolKind::Bash,
        "ctest -R auth_test --output-on-failure"});
    std::this_thread::sleep_for(52ms);
    stream_bash_lines(d, 5, BASH, rng);
    send(d, ev::ToolEnd{5, true, 1.42f});
    send(d, ev::PlanAdvance{3});
    inter();

    // ── Block D: final long summary ─────────────────────────────────
    stream_text(
        "All four tests pass, and the stress iteration sat through 200 "
        "runs of the concurrent case without flapping. Here's the full "
        "writeup.\n"
        "\n"
        "## Root cause\n"
        "\n"
        "`Auth::refresh_token()` had a **TOCTOU** between the freshness "
        "check and the cache update. The early-return read `expires_at_` "
        "outside the mutex; the rest of the function held it. Two "
        "callers entering after expiry could both observe the stale "
        "value, both serialize on the lock, and both perform a "
        "round-trip to the network. The test's `refresh_calls() == 1` "
        "coalescing assertion would fail, and on slow runners the "
        "second caller's wait on the lock could push past the deadline "
        "the test enforces — surfacing as a `deadline_exceeded` rather "
        "than a clean assertion failure.\n"
        "\n"
        "## Fix\n"
        "\n"
        "Move the freshness check inside the critical section. The "
        "second caller now blocks briefly on `mu_`, observes the "
        "freshly cached token, and returns it without a network call — "
        "which is the behaviour the test was always asserting:\n"
        "\n"
        "```cpp\n"
        "Token Auth::refresh_token() {\n"
        "    std::lock_guard lk(mu_);\n"
        "    if (clock_->now() < expires_at_) return cached_;\n"
        "    cached_     = net_.fetch_token();\n"
        "    expires_at_ = clock_->now() + token_lifetime;\n"
        "    return cached_;\n"
        "}\n"
        "```\n"
        "\n"
        "## Verification\n"
        "\n"
        "- All 4 tests in `auth_test` pass.\n"
        "- `concurrent_refresh_is_coalesced` ran 200 stress iterations "
        "with `--repeat until-pass:200` without a single failure.\n"
        "- Verified under TSAN: no remaining data-race warnings in the "
        "auth module.\n"
        "\n"
        "## Follow-ups worth considering\n"
        "\n"
        "1. **Convert `mu_` to `std::shared_mutex`.** The hot path is "
        "the freshness check + cache read; with a `shared_lock` it "
        "scales linearly with reader concurrency, and the slow path "
        "upgrades to a unique lock for the network fetch.\n"
        "2. **Remove the in-tree FIXMEs** in `tests/test_auth.cpp` "
        "(line 42 and the file-header note). The behaviour they warn "
        "about is now a regression the test catches reliably.\n"
        "3. **Add a metric** for `auth_token_refresh_count` so this "
        "kind of regression shows up in production telemetry, not just "
        "in CI.\n",
        [&](std::string s){ send(d, ev::AssistantDelta{std::move(s)}); }, rng);
    send(d, ev::MessageStop{});
}

// ── Scenario B: dark mode (multi-block, with TodoListTool + Write) ─────────
// Phase 1: everything up to and including the PermissionAsk.
static void run_theme(Dispatch d, std::mt19937& rng)
{
    constexpr std::string_view THINKING =
        "Dark mode is three concrete pieces: a `darkTheme` palette, a "
        "small hook to switch between palettes with localStorage "
        "persistence, and a few component updates so consumers stop "
        "importing `lightTheme` directly. The risk areas are (a) "
        "ensuring the palette meets WCAG AA contrast against typical "
        "text sizes, and (b) avoiding a hydration flash where the page "
        "renders in light mode for one frame before the hook reads from "
        "localStorage. Let me build a todo list and walk through it.";

    auto inter = [&]{
        std::uniform_int_distribution<int> g(15, 40);
        std::this_thread::sleep_for(std::chrono::milliseconds(g(rng)));
    };

    send(d, ev::SessionStart{});
    std::this_thread::sleep_for(68ms);

    stream_text(THINKING,
        [&](std::string s){ send(d, ev::ThinkingDelta{std::move(s)}); }, rng);
    send(d, ev::ThinkingStop{});
    inter();

    // ── Todo list goes up first ─────────────────────────────────────
    send(d, ev::TodoCreated{{
        "Read the current theme config",
        "Add darkTheme palette with WCAG-AA contrast",
        "Write useTheme hook with localStorage persistence",
        "Wire Settings.tsx to the hook",
        "Verify with unit tests",
    }});
    std::this_thread::sleep_for(27ms);
    send(d, ev::TodoAdvance{0});

    // ── Tool round 1: Read existing config ──────────────────────────
    send(d, ev::ToolBegin{1, ToolKind::Read, "src/theme/config.ts"});
    std::this_thread::sleep_for(35ms);
    send(d, ev::ToolBodyDelta{1,
        "export interface ThemeConfig {\n"
        "  primary:    string;\n"
        "  background: string;\n"
        "  surface:    string;\n"
        "  text:       string;\n"
        "  textMuted:  string;\n"
        "}\n"
        "\n"
        "export const lightTheme: ThemeConfig = {\n"
        "  primary:    '#6366f1',\n"
        "  background: '#ffffff',\n"
        "  surface:    '#f8fafc',\n"
        "  text:       '#0f172a',\n"
        "  textMuted:  '#64748b',\n"
        "};\n"});
    send(d, ev::ToolEnd{1, true, 0.18f});
    send(d, ev::TodoAdvance{1});
    inter();

    // ── Block A: orient on existing structure ───────────────────────
    say(d,
        "Good — `ThemeConfig` already defines five tokens, and "
        "`lightTheme` is the only consumer of the interface. That "
        "means I can add a sibling `darkTheme` constant and any caller "
        "currently importing `lightTheme` will get the same shape, just "
        "different values.\n"
        "\n"
        "I'll pick the dark palette with WCAG AA in mind: text-on-bg at "
        "≥ 4.5:1 for body copy and ≥ 3:1 for large/secondary. Slate-900 "
        "as the background, slate-100 as the body text, and a desaturated "
        "indigo for accents reads well at typical UI sizes without being "
        "hard on the eyes.\n",
        rng);
    inter();

    // ── Tool round 2: Edit (add darkTheme) ──────────────────────────
    send(d, ev::ToolBegin{2, ToolKind::Edit, "src/theme/config.ts"});
    std::this_thread::sleep_for(27ms);
    send(d, ev::ToolBodyDelta{2,
        "@@ +export const darkTheme @@\n"
        "+\n"
        "+export const darkTheme: ThemeConfig = {\n"
        "+  primary:    '#818cf8',\n"
        "+  background: '#0f172a',\n"
        "+  surface:    '#1e293b',\n"
        "+  text:       '#f1f5f9',\n"
        "+  textMuted:  '#94a3b8',\n"
        "+};\n"});
    send(d, ev::ToolEnd{2, true, 0.11f});
    send(d, ev::FileChanged{"src/theme/config.ts", FileChangeKind::Modified, 8, 0});
    send(d, ev::TodoAdvance{2});
    inter();

    // ── Block B: now the hook ───────────────────────────────────────
    say(d,
        "Palette in. Now the hook. The shape I want is `useTheme(): "
        "{ theme, mode, toggle }` so callers get the live theme object "
        "*and* the current mode label *and* a function to flip it. The "
        "hook reads from localStorage at mount and writes through on "
        "every change so the choice survives reloads.\n",
        rng);
    inter();

    // ── Tool round 3: Write the hook ────────────────────────────────
    send(d, ev::ToolBegin{3, ToolKind::Write, "src/hooks/useTheme.ts"});
    std::this_thread::sleep_for(35ms);
    send(d, ev::ToolBodyDelta{3,
        "import { useState, useEffect, useMemo } from 'react';\n"
        "import { lightTheme, darkTheme } from '../theme/config';\n"
        "\n"
        "type Mode = 'light' | 'dark';\n"
        "const KEY = 'theme';\n"
        "\n"
        "function initialMode(): Mode {\n"
        "    const saved = localStorage.getItem(KEY);\n"
        "    if (saved === 'light' || saved === 'dark') return saved;\n"
        "    return matchMedia('(prefers-color-scheme: dark)').matches\n"
        "        ? 'dark' : 'light';\n"
        "}\n"
        "\n"
        "export function useTheme() {\n"
        "    const [mode, setMode] = useState<Mode>(initialMode);\n"
        "    useEffect(() => { localStorage.setItem(KEY, mode); }, [mode]);\n"
        "    const theme = useMemo(\n"
        "        () => mode === 'dark' ? darkTheme : lightTheme,\n"
        "        [mode],\n"
        "    );\n"
        "    const toggle = () =>\n"
        "        setMode(m => m === 'dark' ? 'light' : 'dark');\n"
        "    return { theme, mode, toggle };\n"
        "}\n"});
    send(d, ev::ToolEnd{3, true, 0.14f});
    send(d, ev::FileChanged{"src/hooks/useTheme.ts", FileChangeKind::Created, 24, 0});
    send(d, ev::TodoAdvance{3});
    inter();

    // ── Block C: wire it up ─────────────────────────────────────────
    say(d,
        "Hook written. The interesting bit is `initialMode()` — it "
        "checks localStorage first, falls back to "
        "`prefers-color-scheme: dark` if there's no saved value. That "
        "way a first-time visitor on a dark-mode OS gets the dark "
        "theme by default rather than a light flash.\n"
        "\n"
        "Now I need to update `Settings.tsx` to consume the hook "
        "instead of importing `lightTheme` directly.\n",
        rng);
    inter();

    // ── Tool round 4: Edit Settings.tsx ─────────────────────────────
    send(d, ev::ToolBegin{4, ToolKind::Edit, "src/components/Settings.tsx"});
    std::this_thread::sleep_for(30ms);
    send(d, ev::ToolBodyDelta{4,
        "@@ Settings.tsx @@\n"
        "-import { lightTheme } from '../theme/config';\n"
        "+import { useTheme } from '../hooks/useTheme';\n"
        " \n"
        " export function Settings() {\n"
        "-    const theme = lightTheme;\n"
        "+    const { theme, mode, toggle } = useTheme();\n"
        " \n"
        "     return (\n"
        "         <div style={{ background: theme.background }}>\n"
        "+            <button onClick={toggle}>\n"
        "+                {mode === 'dark' ? 'Light mode' : 'Dark mode'}\n"
        "+            </button>\n"
        "             <SettingsBody theme={theme} />\n"
        "         </div>\n"
        "     );\n"
        " }\n"});
    send(d, ev::ToolEnd{4, true, 0.12f});
    send(d, ev::FileChanged{"src/components/Settings.tsx", FileChangeKind::Modified, 5, 2});
    inter();

    // ── Block D: bridge to bash ─────────────────────────────────────
    say(d,
        "Settings now pulls from the hook and the new toggle button "
        "renders inside the existing layout. Time to verify with the "
        "test suite.\n",
        rng);
    inter();

    // ── Permission gate ─────────────────────────────────────────────
    send(d, ev::PermissionAsk{"npm test useTheme"});
    // Phase 1 ends here; see run_auth.
}

// Phase 2: everything after the grant (own BASH, inter, rng).
static void run_theme_after_gate(Dispatch d, std::mt19937& rng)
{
    constexpr std::string_view BASH =
        "\n"
        "> useTheme@1.0.0 test\n"
        "> jest --runInBand src/hooks/useTheme.test.ts\n"
        "\n"
        " PASS  src/hooks/useTheme.test.ts\n"
        "  useTheme\n"
        "    \xe2\x9c\x93 defaults to light theme on first render (4 ms)\n"
        "    \xe2\x9c\x93 toggle() flips light\xe2\x86\x92" "dark (1 ms)\n"
        "    \xe2\x9c\x93 persists chosen mode to localStorage (3 ms)\n"
        "    \xe2\x9c\x93 reads persisted mode on mount (1 ms)\n"
        "    \xe2\x9c\x93 falls back to system preference if no saved value (2 ms)\n"
        "\n"
        "Test Suites: 1 passed, 1 total\n"
        "Tests:       5 passed, 5 total\n"
        "Snapshots:   0 total\n"
        "Time:        1.18 s\n";

    auto inter = [&]{
        std::uniform_int_distribution<int> g(15, 40);
        std::this_thread::sleep_for(std::chrono::milliseconds(g(rng)));
    };

    send(d, ev::PermissionDone{});
    std::this_thread::sleep_for(35ms);
    send(d, ev::TodoAdvance{4});

    // ── Tool round 5: Bash ──────────────────────────────────────────
    send(d, ev::ToolBegin{5, ToolKind::Bash, "npm test useTheme"});
    std::this_thread::sleep_for(52ms);
    stream_bash_lines(d, 5, BASH, rng);
    send(d, ev::ToolEnd{5, true, 1.18f});
    inter();

    // ── Final long block ────────────────────────────────────────────
    stream_text(
        "All five tests pass — including the system-preference fallback "
        "I added on top of what was originally requested. Here's the "
        "full picture.\n"
        "\n"
        "## What shipped\n"
        "\n"
        "1. **`darkTheme` palette** in `src/theme/config.ts` (8 new "
        "lines). Same shape as `lightTheme`, drawn from Slate + Indigo "
        "with the contrasts measured below.\n"
        "2. **`useTheme` hook** in `src/hooks/useTheme.ts` (24 lines, "
        "new file). Returns `{ theme, mode, toggle }`. Reads "
        "localStorage at mount, falls back to "
        "`prefers-color-scheme: dark`, writes through on every change.\n"
        "3. **`Settings.tsx` rewired** to consume the hook and render a "
        "toggle button. Removed the direct `lightTheme` import.\n"
        "\n"
        "## WCAG AA verification\n"
        "\n"
        "| Pair                       | Ratio   | AA bar | Verdict |\n"
        "|----------------------------|---------|--------|---------|\n"
        "| `text` on `background`     | 15.4:1  | 4.5:1  | Pass    |\n"
        "| `textMuted` on `background`|  7.2:1  | 4.5:1  | Pass    |\n"
        "| `primary` on `background`  |  5.8:1  | 4.5:1  | Pass    |\n"
        "| `text` on `surface`        | 11.3:1  | 4.5:1  | Pass    |\n"
        "\n"
        "All four pairs clear AA for body text, and three of them clear "
        "AAA (7:1) too — comfortable for extended reading.\n"
        "\n"
        "## Hydration story\n"
        "\n"
        "Because `useState`'s initializer runs synchronously on the "
        "first render, the hook reads from localStorage *before* React "
        "paints — no light-mode flash on reload. If you want to remove "
        "that one-frame uncertainty completely, set the document "
        "`color-scheme` from a tiny inline script in `index.html` "
        "before React boots.\n"
        "\n"
        "## Follow-ups\n"
        "\n"
        "- The `Sidebar` and `Modal` components also import "
        "`lightTheme` directly (3 callsites I noticed but didn't "
        "touch). Quick mechanical change — happy to do those in a "
        "follow-up turn.\n"
        "- Consider promoting `Mode` to a context so deeply-nested "
        "components don't re-call `useTheme()`. Right now the rerender "
        "shape is fine, but if you add 50 more consumers a context "
        "wrapper will save renders.\n",
        [&](std::string s){ send(d, ev::AssistantDelta{std::move(s)}); }, rng);
    send(d, ev::MessageStop{});
}

// ── Scenario C: perf investigation (sub-agent + fetch + multi-block) ──────
static void run_perf(Dispatch d, std::mt19937& rng)
{
    constexpr std::string_view THINKING =
        "A p99 latency jump from one release to the next is almost "
        "always one of: a new synchronous call on the hot path, a "
        "removed cache, a regressed serialization format, or a GC "
        "pressure change from a new allocation pattern. I should pull "
        "the metrics service to confirm the regression is real and pin "
        "the start time, then bisect the deploy window against the "
        "commit log. In parallel, a sub-agent can pull the framework's "
        "perf checklist so any recommendations I make are grounded in "
        "what the framework explicitly supports.";

    auto inter = [&]{
        std::uniform_int_distribution<int> g(15, 40);
        std::this_thread::sleep_for(std::chrono::milliseconds(g(rng)));
    };

    send(d, ev::SessionStart{});
    std::this_thread::sleep_for(68ms);

    stream_text(THINKING,
        [&](std::string s){ send(d, ev::ThinkingDelta{std::move(s)}); }, rng);
    send(d, ev::ThinkingStop{});
    inter();

    // ── Tool round 1: Fetch metrics ──────────────────────────────────
    send(d, ev::ToolBegin{1, ToolKind::Fetch,
        "https://metrics.internal/p99?handler=auth&window=14d"});
    std::this_thread::sleep_for(67ms);
    send(d, ev::ToolBodyDelta{1,
        "{\n"
        "  \"handler\":           \"auth\",\n"
        "  \"window_days\":       14,\n"
        "  \"p50_baseline_ms\":   18,\n"
        "  \"p50_current_ms\":    24,\n"
        "  \"p99_baseline_ms\":   220,\n"
        "  \"p99_current_ms\":    1410,\n"
        "  \"regression_first_seen\": \"2025-04-29T18:11Z\",\n"
        "  \"suspect_deploy\":    \"release-2025.18\",\n"
        "  \"suspect_commit\":    \"e4c3a1f\",\n"
        "  \"suspect_commit_msg\": \"auth: drop signature cache (revisit if hot)\"\n"
        "}\n"});
    send(d, ev::ToolEnd{1, true, 0.62f});
    inter();

    // ── Block A: confirm + plan next step ───────────────────────────
    say(d,
        "The metrics service confirms it cleanly. p99 went from 220ms "
        "to 1410ms — that's a **6.4×** regression — starting "
        "2025-04-29 at 18:11 UTC. The release at that timestamp is "
        "`release-2025.18`, and the metrics service has even helpfully "
        "annotated a suspect commit: `e4c3a1f`, with the commit "
        "message *\"auth: drop signature cache (revisit if hot)\"*. "
        "Whoever wrote that commit message clearly knew this was a "
        "tradeoff worth revisiting.\n"
        "\n"
        "Two parallel things to do now: spawn a sub-agent to pull the "
        "framework's perf checklist (so my recommendations cite what "
        "the framework supports), and read the actual handler code at "
        "that commit to see what the cache-drop looked like.\n",
        rng);
    inter();

    // ── Tool round 2: Sub-agent + Read (parallel-feeling) ───────────
    send(d, ev::ToolBegin{2, ToolKind::Agent,
        "research framework perf checklist for high-RPS handlers"});
    std::this_thread::sleep_for(45ms);

    // Sub-agent body fills in over time, simulating its own tool calls.
    send(d, ev::ToolBodyDelta{2,
        "  \xe2\x94\x9c\xe2\x94\x80 Fetch  https://framework.io/docs/perf       (200 OK \xc2\xb7 1.1s)\n"});
    std::this_thread::sleep_for(52ms);
    send(d, ev::ToolBodyDelta{2,
        "  \xe2\x94\x9c\xe2\x94\x80 Fetch  https://framework.io/docs/auth      (200 OK \xc2\xb7 0.7s)\n"});
    std::this_thread::sleep_for(47ms);
    send(d, ev::ToolBodyDelta{2,
        "  \xe2\x94\x9c\xe2\x94\x80 Read   docs/perf-checklist.md              (0.1s)\n"});
    std::this_thread::sleep_for(35ms);
    send(d, ev::ToolBodyDelta{2,
        "  \xe2\x94\x94\xe2\x94\x80 Note   \"cache HMAC signatures when keys rotate \xe2\x89\xa5 24h\"\n"});
    std::this_thread::sleep_for(27ms);
    send(d, ev::ToolEnd{2, true, 2.91f});
    inter();

    // ── Read the handler ─────────────────────────────────────────────
    send(d, ev::ToolBegin{3, ToolKind::Read,
        "src/server/request_handler.cpp"});
    std::this_thread::sleep_for(40ms);
    send(d, ev::ToolBodyDelta{3,
        "// e4c3a1f \xe2\x80\x94 drop signature cache (revisit if hot)\n"
        "Status RequestHandler::authenticate(const Request& req) {\n"
        "    // Compute fresh signature on every request.\n"
        "    auto sig = compute_signature(req.headers, signing_key_);\n"
        "    if (!verify(req, sig)) return Status::Unauthorized;\n"
        "    return Status::OK;\n"
        "}\n"
        "\n"
        "Bytes compute_signature(Headers h, KeyView key) {\n"
        "    Bytes scratch(2048);                       // <-- fresh alloc\n"
        "    HMAC_CTX ctx;\n"
        "    hmac_init(&ctx, key.data(), key.size());\n"
        "    for (const auto& [k, v] : h) {\n"
        "        hmac_update(&ctx, k.data(), k.size());\n"
        "        hmac_update(&ctx, v.data(), v.size());\n"
        "    }\n"
        "    hmac_final(&ctx, scratch.data());\n"
        "    return scratch;\n"
        "}\n"});
    send(d, ev::ToolEnd{3, true, 0.14f});
    inter();

    // ── Block B: connect the dots ──────────────────────────────────
    say(d,
        "Now I have the full picture. Three things stack:\n"
        "\n"
        "1. **Cache drop.** `e4c3a1f` removed the per-request "
        "signature cache. Every request now does an HMAC.\n"
        "2. **Allocation per request.** `compute_signature` allocates "
        "a 2 KB `Bytes` scratch buffer on every call. At 50 RPS sustained "
        "that's ~100 KB/s of garbage just from this function — enough "
        "to trip the GC's young-gen collection cadence.\n"
        "3. **Header iteration is O(headers).** Inside the HMAC loop, "
        "each header pair adds two `hmac_update` calls. For requests "
        "with many headers (e.g. correlation IDs, tracing context), "
        "this scales linearly with header count *per request*, not "
        "amortized.\n"
        "\n"
        "Before I write up the recommendation I want to surface the "
        "framework's documented stance — that's where the sub-agent "
        "comes in.\n",
        rng);

    // ── ContextWarning surfaces here ────────────────────────────────
    send(d, ev::ContextWarning{"Context window is 62% full"});
    inter();

    // ── Block C: long final analysis ─────────────────────────────────
    stream_text(
        "## Root cause\n"
        "\n"
        "Commit `e4c3a1f` (\"*auth: drop signature cache (revisit if "
        "hot)*\") removed an HMAC signature cache that was hiding three "
        "expensive things on the hot path:\n"
        "\n"
        "- An HMAC computation per request (~12 ms typical, more under "
        "GC pressure).\n"
        "- A 2 KB heap allocation per request from the scratch buffer "
        "in `compute_signature`.\n"
        "- A linear walk through every header pair on every call, "
        "which makes p99 sensitive to header count.\n"
        "\n"
        "The cumulative effect is what the metrics service shows: "
        "p99 sat at 220ms before the change and 1410ms after. p50 "
        "barely moved (18ms → 24ms) because the cache was previously "
        "absorbing the *long-tail* requests where header count was "
        "high — exactly the population that regressed.\n"
        "\n"
        "## Recommendation\n"
        "\n"
        "**Restore the signature cache.** The framework's perf "
        "checklist explicitly endorses caching HMAC signatures when "
        "key rotation is at least 24 hours, and our keys rotate "
        "daily — so a cache TTL of even a few minutes is well within "
        "the safety envelope. Concretely:\n"
        "\n"
        "```cpp\n"
        "Status RequestHandler::authenticate(const Request& req) {\n"
        "    static thread_local LruCache<Bytes, Bytes> sig_cache(1024);\n"
        "    auto& sig = sig_cache.get_or_compute(\n"
        "        req.signing_input(),\n"
        "        [&]{ return compute_signature(req.headers, signing_key_); }\n"
        "    );\n"
        "    return verify(req, sig) ? Status::OK : Status::Unauthorized;\n"
        "}\n"
        "```\n"
        "\n"
        "Two reasons to make it `thread_local` rather than a shared "
        "cache: (a) no lock contention on the hot path, and (b) the "
        "working set stays warm in CPU-local caches because each "
        "thread tends to handle a distinct connection slice.\n"
        "\n"
        "## Action items\n"
        "\n"
        "1. **Revert the cache removal** in `src/server/request_handler.cpp` "
        "(target: `release-2025.19`).\n"
        "2. **Add a p99 alert** at 400ms for `handler=auth` so we catch "
        "the next regression in observability rather than from a user "
        "report.\n"
        "3. **Pre-allocate** the scratch buffer in `compute_signature` "
        "as a `thread_local` for the slow-path case where the cache "
        "misses, so even the cold hits don't pay the per-call alloc.\n"
        "4. **Add a perf test** to CI that asserts `authenticate()` "
        "completes in < 500µs on a warm cache hit, against a "
        "representative request shape.\n"
        "\n"
        "I can wire any of those up if you want me to keep going.\n",
        [&](std::string s){ send(d, ev::AssistantDelta{std::move(s)}); }, rng);
    send(d, ev::MessageStop{});
}

// ── Scenario D: general unwrap audit (multi-block) ─────────────────────────
static void run_general(Dispatch d, std::mt19937& rng)
{
    constexpr std::string_view THINKING =
        "An audit for `unwrap()` in production paths is a triage "
        "exercise: most callsites are fine because the precondition is "
        "structurally guaranteed (e.g. a parser that already validated "
        "the bytes before unwrapping), but a handful are landmines that "
        "panic on input the runtime can actually see. I'll grep for all "
        "callsites, read the two or three highest-traffic ones, and "
        "rate them by exposure.";

    auto inter = [&]{
        std::uniform_int_distribution<int> g(15, 40);
        std::this_thread::sleep_for(std::chrono::milliseconds(g(rng)));
    };

    send(d, ev::SessionStart{});
    std::this_thread::sleep_for(65ms);

    stream_text(THINKING,
        [&](std::string s){ send(d, ev::ThinkingDelta{std::move(s)}); }, rng);
    send(d, ev::ThinkingStop{});
    inter();

    // ── Tool round 1: Grep ──────────────────────────────────────────
    send(d, ev::ToolBegin{1, ToolKind::Grep, "\\.unwrap\\(\\) in src/"});
    std::this_thread::sleep_for(47ms);
    send(d, ev::ToolBodyDelta{1,
        "src/io/reader.rs:42:        let buf = file.read().unwrap();\n"
        "src/io/writer.rs:88:        socket.write_all(&buf).unwrap();\n"
        "src/util/parse.rs:17:    let n: i32 = s.parse().unwrap();\n"
        "src/util/cache.rs:104:   let row = rows.first().unwrap();\n"
        "src/server/route.rs:201: let route = routes.get(path).unwrap();\n"
        "src/proto/decode.rs:88:  let header = bytes.get(0..16).unwrap();\n"});
    send(d, ev::ToolEnd{1, true, 0.26f});
    inter();

    // ── Block A: triage findings ────────────────────────────────────
    say(d,
        "Six callsites total. At first glance the two that scare me "
        "most are `src/util/parse.rs:17` (parsing an integer from a "
        "string with no validation upstream — that's a classic source "
        "of input-driven panics) and `src/server/route.rs:201` "
        "(routing decision based on a hashmap lookup that *should* be "
        "infallible but isn't structurally guaranteed). Let me read "
        "those two and confirm whether the inputs are validated "
        "anywhere upstream.\n",
        rng);
    inter();

    // ── Tool round 2: Read parse.rs ─────────────────────────────────
    send(d, ev::ToolBegin{2, ToolKind::Read, "src/util/parse.rs"});
    std::this_thread::sleep_for(27ms);
    send(d, ev::ToolBodyDelta{2,
        "pub fn parse_age(s: &str) -> i32 {\n"
        "    let n: i32 = s.parse().unwrap();      // <-- panics on bad input\n"
        "    n.clamp(0, 150)\n"
        "}\n"
        "\n"
        "// callers:\n"
        "//   handlers/profile.rs:88  parse_age(&form.age)\n"
        "//   handlers/admin.rs:204   parse_age(&row.age_text)\n"});
    send(d, ev::ToolEnd{2, true, 0.13f});
    inter();

    // ── Tool round 3: Read route.rs ─────────────────────────────────
    send(d, ev::ToolBegin{3, ToolKind::Read, "src/server/route.rs"});
    std::this_thread::sleep_for(27ms);
    send(d, ev::ToolBodyDelta{3,
        "fn dispatch(req: &Request, routes: &RouteMap) -> Response {\n"
        "    let path = req.path();\n"
        "    let route = routes.get(path).unwrap();   // <-- panics on unknown path\n"
        "    route.handler(req)\n"
        "}\n"});
    send(d, ev::ToolEnd{3, true, 0.11f});
    inter();

    // ── Final long block ────────────────────────────────────────────
    stream_text(
        "Confirmed — both of those are real panics on adversarial "
        "input.\n"
        "\n"
        "## Severity ranking\n"
        "\n"
        "| File:line                   | Risk     | Why |\n"
        "|-----------------------------|----------|-----|\n"
        "| `src/util/parse.rs:17`      | **High** | Called from a form handler. Any non-numeric `age` from the client crashes the worker. |\n"
        "| `src/server/route.rs:201`   | **High** | Any path not in the route table panics the server thread. |\n"
        "| `src/proto/decode.rs:88`    | Medium   | Reading a 16-byte header from arbitrary bytes; short messages crash. |\n"
        "| `src/io/reader.rs:42`       | Low      | I/O error → panic, but `file.read()` only fails on already-broken fd. |\n"
        "| `src/io/writer.rs:88`       | Low      | Same shape — write errors are usually fatal anyway in this codepath. |\n"
        "| `src/util/cache.rs:104`     | Low      | `rows.first()` is preceded by `if !rows.is_empty()` two lines up. |\n"
        "\n"
        "## Recommended fixes\n"
        "\n"
        "**`parse_age`** — return `Result<i32, AgeParseError>` and let "
        "the handler return a 400 on failure. The clamping behaviour is "
        "already a code smell that's hiding the real input contract.\n"
        "\n"
        "**`dispatch`** — replace `unwrap()` with a fallthrough to a "
        "404 handler. The route table is built at startup; a missing "
        "path means the request is novel, not that the program is "
        "broken.\n"
        "\n"
        "**`decode.rs`** — return a typed `DecodeError::Truncated` "
        "instead of panicking. Any production protocol decoder will "
        "see truncated frames eventually.\n"
        "\n"
        "Want me to open a follow-up turn and make these changes? I "
        "can do them in one PR or split them by severity.\n",
        [&](std::string s){ send(d, ev::AssistantDelta{std::move(s)}); }, rng);
    send(d, ev::MessageStop{});
}

// ── Dispatcher: pick scenario from prompt ──────────────────────────────────
// `phase` 1 runs a scenario from the start (to its permission gate, or to
// the end if it has none); 2 runs a gated scenario's post-grant half. The
// phase-2 rng is seeded afresh — the jitter it drives is cosmetic.
inline void run(const std::string& prompt, int phase, Dispatch dispatch)
{
    std::mt19937 rng{
        std::random_device{}() ^ std::uint32_t(prompt.size()) ^ 0xC0DEC0DE
            ^ std::uint32_t(phase)
    };

    if (contains_ci(prompt, "auth") || contains_ci(prompt, "flak") ||
        contains_ci(prompt, "deadlin") || contains_ci(prompt, "test_auth"))
    {
        if (phase == 1) run_auth(std::move(dispatch), rng);
        else            run_auth_after_gate(std::move(dispatch), rng);
    }
    else if (contains_ci(prompt, "dark") || contains_ci(prompt, "theme") ||
             contains_ci(prompt, "wcag"))
    {
        if (phase == 1) run_theme(std::move(dispatch), rng);
        else            run_theme_after_gate(std::move(dispatch), rng);
    }
    else if (contains_ci(prompt, "p99") || contains_ci(prompt, "perf") ||
             contains_ci(prompt, "slow") || contains_ci(prompt, "latency") ||
             contains_ci(prompt, "profil"))
    {
        if (phase == 1) run_perf(std::move(dispatch), rng);   // ungated
    }
    else {
        if (phase == 1) run_general(std::move(dispatch), rng); // ungated
    }
}

} // namespace sim

// ============================================================================
// Build the AgentTimeline live panel from Model::tools
// ============================================================================

static Element actions_panel(const Model& m, bool live) {
    if (m.tools.empty()) return none();

    int done = 0, mutate = 0, inspect = 0;
    for (const auto& t : m.tools) {
        if (t.status == AgentEventStatus::Done ||
            t.status == AgentEventStatus::Failed) ++done;
        if (t.kind == ToolKind::Edit || t.kind == ToolKind::Bash) ++mutate;
        else                                                       ++inspect;
    }

    // Cross-tool semantics: scan completed Greps once up-front and build
    // a `path → {line numbers}` index.  Subsequent Read tools that open
    // any of those paths inherit the grep hits as `highlight_lines`, so
    // the body anchors the user's eye on lines the assistant flagged
    // earlier in the same turn instead of forcing a re-scan.
    std::unordered_map<std::string, std::set<int>> grep_hits;
    for (const auto& t : m.tools) {
        if (t.kind != ToolKind::Grep || t.body.empty()) continue;
        std::size_t pos = 0;
        while (pos < t.body.size()) {
            const auto nl  = t.body.find('\n', pos);
            const auto end = (nl == std::string::npos) ? t.body.size() : nl;
            const std::string_view ln(t.body.data() + pos, end - pos);
            const auto p1 = ln.find(':');
            const auto p2 = (p1 == std::string_view::npos)
                          ? std::string_view::npos : ln.find(':', p1 + 1);
            bool digits = (p1 != std::string_view::npos)
                       && (p2 != std::string_view::npos)
                       && (p2 > p1 + 1);
            for (std::size_t j = p1 + 1; j < p2 && digits; ++j)
                if (ln[j] < '0' || ln[j] > '9') digits = false;
            if (digits) {
                std::string path{ln.substr(0, p1)};
                int linum = 0;
                for (std::size_t j = p1 + 1; j < p2; ++j)
                    linum = linum * 10 + (ln[j] - '0');
                if (linum > 0) grep_hits[std::move(path)].insert(linum);
            }
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
    }

    std::vector<AgentTimelineEvent> events;
    events.reserve(m.tools.size());
    for (const auto& t : m.tools) {
        AgentTimelineEvent e;
        e.name            = tool_name(t.kind);
        e.detail          = t.detail;
        e.elapsed_seconds = t.elapsed;
        e.category_color  = tool_color(t.kind);
        e.status          = t.status;
        // Map each ToolKind to the body-preview rendering that fits its
        // shape: a Read shows line numbers, an Edit shows diff coloring, a
        // Grep parses path:line:text columns, etc. The fallback CodeBlock
        // covers Agent (free-text trace) and any unknown kinds. We render
        // the body slot whenever we have content OR the tool is in flight
        // — the "awaiting…" skeleton lets the user tell a stuck tool from
        // a slow one before the first byte arrives.
        const bool body_streaming = t.body.empty() &&
            t.status == AgentEventStatus::Running;
        if (!t.body.empty() || body_streaming) {
            e.body.text         = t.body;
            e.body.failed       = (t.status == AgentEventStatus::Failed);
            e.body.is_streaming = body_streaming;

            switch (t.kind) {
                case ToolKind::Edit:
                    e.body.kind = ToolBodyPreview::Kind::GitDiff;
                    break;
                case ToolKind::Bash:
                    e.body.kind = ToolBodyPreview::Kind::BashOutput;
                    break;
                case ToolKind::Read: {
                    e.body.kind = ToolBodyPreview::Kind::FileRead;
                    // Inherit grep hits on this same path, if any.  The
                    // FileRead body will summarise them in a header and
                    // brighten any visible matching line.
                    auto it = grep_hits.find(t.detail);
                    if (it != grep_hits.end())
                        e.body.highlight_lines = it->second;
                    break;
                }
                case ToolKind::Write:
                    e.body.kind = ToolBodyPreview::Kind::FileWrite;
                    break;
                case ToolKind::Fetch:
                    // Json self-sniffs and falls back to CodeBlock when the
                    // body isn't JSON, so it's safe as the unconditional
                    // pick for fetch responses.
                    e.body.kind = ToolBodyPreview::Kind::Json;
                    break;
                case ToolKind::Grep:
                    e.body.kind = ToolBodyPreview::Kind::GrepMatches;
                    break;
                case ToolKind::Agent:
                case ToolKind::Todo:
                default:
                    e.body.kind = ToolBodyPreview::Kind::CodeBlock;
                    break;
            }
        }
        events.push_back(std::move(e));
    }

    AgentTimeline::Config cfg;
    cfg.title = std::string(" ACTIONS  \xc2\xb7  ") + std::to_string(done) +
                "/" + std::to_string(int(m.tools.size())) +
                (live ? "  \xc2\xb7  in flight " : " ");
    cfg.border_color = live ? Color::cyan() : Color::bright_black();
    cfg.frame        = m.spinner_frame;
    cfg.stats        = {
        {"INSPECT", inspect, Color::blue()},
        {"MUTATE",  mutate,  Color::magenta()},
    };
    cfg.events       = std::move(events);
    if (!live) {
        AgentTimelineFooter f;
        f.glyph   = "\xe2\x9c\x93";
        f.text    = "settled";
        f.color   = Color::bright_green();
        f.summary = std::to_string(int(m.tools.size())) + " actions";
        cfg.footer = std::move(f);
    }
    return AgentTimeline{std::move(cfg)}.build();
}

// ── PlanView ───────────────────────────────────────────────────────────────
static Element plan_card(const Model& m) {
    if (m.plan.empty()) return none();
    PlanView p;
    for (const auto& it : m.plan) p.add(it.label, it.status);
    return p.build();
}

// ── TodoList ───────────────────────────────────────────────────────────────
static Element todos_card(const Model& m) {
    if (m.todos.empty()) return none();
    TodoListTool t;
    std::vector<TodoListItem> items;
    items.reserve(m.todos.size());
    for (const auto& it : m.todos)
        items.push_back({it.label, it.status});  // {content, status}
    t.set_items(std::move(items));
    t.set_status(m.todos_status);
    t.set_elapsed(m.todos_elapsed);
    t.set_expanded(true);
    return t.build();
}

// ── ChangesStrip ───────────────────────────────────────────────────────────
static Element changes_card(const Model& m) {
    if (m.changes.empty()) return none();
    ChangesStrip::Config c;
    c.changes = m.changes;
    return ChangesStrip{std::move(c)}.build();
}

// ── SystemBanner ───────────────────────────────────────────────────────────
static Element ctx_banner(const Model& m) {
    if (m.ctx_warning.empty()) return none();
    SystemBanner b(m.ctx_warning, BannerLevel::Warning);
    return b.build();
}

// ============================================================================
// Turn-wrapped views — agentty-style chrome.
//
// agentty wraps every chat message in maya::Turn{glyph + speaker label +
// meta + rail + body}. To make this example a faithful reproduction of
// agentty's rendering pipeline we do the same here: the user message
// goes through a Role::User Turn, the assistant content (thinking,
// actions panel, plan, todos, markdown) goes through a single
// Role::Assistant Turn whose body slots are pushed in agent-session
// order.
// ============================================================================

static std::string turn_meta_now(int turn_number) {
    // Compact "HH:MM · turn N" — same shape format_turn_meta() builds in
    // agentty (sans elapsed since this demo doesn't track per-turn start
    // wall-clock). The leading and inter-segment middle-dots are the
    // same encoding agentty uses for the header meta strip.
    std::time_t now = std::time(nullptr);
    std::tm     tm{};
#if MAYA_OS_WINDOWS
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    std::string meta = buf;
    meta += "  \xc2\xb7  turn ";
    meta += std::to_string(turn_number);
    return meta;
}

static Element user_turn_element(const Model& m, std::string_view prompt) {
    Turn::Config cfg;
    cfg.glyph      = "\xe2\x9d\xaf";              // ❯
    cfg.label      = "You";
    cfg.rail_color = Color::magenta();
    cfg.meta       = turn_meta_now(m.turn_number);
    cfg.body.emplace_back(
        Turn::PlainText{std::string{prompt}, Color::bright_white()});
    return Turn{std::move(cfg)}.build();
}

// Build a settled assistant Turn from the accumulated body slots in
// `m.assistant_body`. Used by the freeze path at MessageStop / commit
// boundaries — one Turn per Anthropic-style assistant Message, header
// + rail + every body slot the turn produced.
static Element settled_assistant_turn_element(const Model& m) {
    Turn::Config cfg;
    cfg.glyph      = "\xe2\x9c\xa6";              // ✦
    cfg.label      = "Opus 4.7";
    cfg.rail_color = Color::bright_magenta();
    cfg.meta       = turn_meta_now(m.turn_number);
    for (const auto& e : m.assistant_body)
        cfg.body.emplace_back(e);   // BodySlot accepts Element directly
    return Turn{std::move(cfg)}.build();
}

// Build a live assistant Turn whose body interleaves accumulated body
// slots (already-completed text blocks / tool batches inside the same
// Anthropic Message) PLUS the in-flight live state (thinking, actions,
// plan, todos, markdown). Returns none() if there's nothing to show
// — caller skips the gap+turn in that frame.
static Element assistant_turn_element(const Model& m) {
    const bool nothing =
        m.assistant_body.empty()
        && !m.thinking_active && m.tools.empty() && m.plan.empty()
        && m.todos.empty() && !m.md_active && m.ctx_warning.empty();
    if (nothing) return none();

    Turn::Config cfg;
    cfg.glyph      = "\xe2\x9c\xa6";              // ✦
    cfg.label      = "Opus 4.7";
    cfg.rail_color = Color::bright_magenta();
    cfg.meta       = turn_meta_now(m.turn_number);

    // Already-rolled-up slots (every text block / tool batch that's
    // closed during this in-flight turn — see TextBlockStop /
    // AssistantDelta first-time handlers).
    for (const auto& e : m.assistant_body)
        cfg.body.emplace_back(e);

    // In-flight live state.
    if (m.thinking_active)
        cfg.body.emplace_back(m.thinking.build());
    if (!m.tools.empty())
        cfg.body.emplace_back(actions_panel(m, /*live=*/true));
    if (!m.plan.empty())
        cfg.body.emplace_back(plan_card(m));
    if (!m.todos.empty())
        cfg.body.emplace_back(todos_card(m));
    if (!m.ctx_warning.empty())
        cfg.body.emplace_back(ctx_banner(m));
    if (m.md_active)
        cfg.body.emplace_back(AssistantMessage::build(m.md.build()));

    return Turn{std::move(cfg)}.build();
}

// ── Composer (functional) ──────────────────────────────────────────────────
static Element composer_view(const Model& m) {
    Composer::Config c;
    c.text         = m.composer_text;
    c.cursor       = m.composer_cursor;
    c.state        = composer_state(m.phase);
    c.queued       = m.queued;
    c.profile      = {"Sonnet 4.7", Color::magenta()};
    c.expanded     = false;
    return Composer{std::move(c)}.build();
}

// ── Status bar ─────────────────────────────────────────────────────────────
static Element status_bar(const Model& m) {
    PhaseChip::Config phase_cfg;
    phase_cfg.glyph        = (m.phase == Phase::Done) ? "\xe2\x9c\x93"
                                                       : "\xe2\x97\x8f";
    phase_cfg.verb         = phase_verb(m.phase);
    phase_cfg.color        = phase_color(m.phase);
    phase_cfg.breathing    = (m.phase == Phase::Idle);
    phase_cfg.frame        = m.spinner_frame;
    phase_cfg.elapsed_secs = m.total_elapsed;
    phase_cfg.verb_width   = 11;

    TokenStreamSparkline::Config tok_cfg;
    tok_cfg.rate    = m.tok_rate;
    tok_cfg.total   = m.in_tokens + m.out_tokens;
    tok_cfg.history = m.tok_history;
    tok_cfg.live    = (m.phase == Phase::Streaming || m.phase == Phase::Thinking);
    tok_cfg.color   = phase_color(m.phase);

    ContextGauge::Config ctx_cfg;
    ctx_cfg.used  = m.in_tokens + m.out_tokens;
    ctx_cfg.max   = m.ctx_max;
    ctx_cfg.cells = 12;

    return v(
        h(
            PhaseChip{phase_cfg},
            t<"  ">,
            ContextGauge{ctx_cfg},
            t<"  ">,
            TokenStreamSparkline{tok_cfg},
            t<"  ">,
            ModelBadge({.label = "Opus 4.7"}).build()
        ) | pad<0, 1>,
        // Bottom hint row.
        h(
            t<"\xe2\x86\xb5 send"> | Dim,
            t<"  ">,
            t<"\xe2\x86\x91/\xe2\x86\x93 starters"> | Dim,
            t<"  ">,
            t<"space allow"> | Dim,
            t<"  ">,
            t<"q quit"> | Dim
        ) | pad<0, 1>
    ).build();
}

// ── Welcome screen (empty thread) ──────────────────────────────────────────
static Element welcome_view() {
    WelcomeScreen::Config c;
    c.sigil_color    = Color::magenta();
    c.tagline        = "Real SSE-streamed agent simulation, all local.";
    c.model_badge    = ModelBadge({.label = "Opus 4.7"}).build();
    c.profile_label  = "demo";
    c.starters_title = "Try one of these";
    c.starters       = starters();
    c.hint_intro     = "type to begin";
    c.hints          = {
        {"\xe2\x86\xb5",     " send",       Color::cyan()},
        {"\xe2\x86\x91\xe2\x86\x93", " starters", Color::cyan()},
        {"Tab",  " cycle",      Color::cyan()},
        {"^C",   " quit",       Color::bright_black()},
    };
    return WelcomeScreen{std::move(c)}.build();
}

// ============================================================================
// view() — pure rendering of Model
// ============================================================================

static Element view(const Model& m) {
    bool empty = m.frozen.empty() && !m.user_committed && !m.thinking_active &&
                 m.tools.empty() && !m.md_active && m.composer_text.empty();

    return v(
        // Welcome OR frozen scrollback
        dyn([&]() -> Element {
            if (empty) return welcome_view();
            // Zero-copy fragment via the DSL, tagged with the ledger:
            // the renderer reads the sealed blocks through the pointer
            // instead of deep-copying them every frame, AND the paint
            // pass records each block's laid-out height back into the
            // ledger — the measurements harvest() mints trim debt from.
            return ledger_ref(m.frozen);
        }),

        // In-flight user message — wrapped in a Role::User Turn to mirror
        // agentty's chrome (glyph + "You" + meta + rail).
        dyn([&]() -> Element {
            if (m.user_committed || m.user_prompt.empty()) return none();
            return v(blank(), user_turn_element(m, m.user_prompt)).build();
        }),

        // Live assistant content — ONE Role::Assistant Turn whose body
        // interleaves thinking, actions, plan, todos, markdown in the
        // order they accumulate during the turn. This is the agentty
        // pattern: every assistant Message gets wrapped in a Turn with
        // a header that can shrink to 0 rows when the body overflows.
        dyn([&]() -> Element {
            Element e = assistant_turn_element(m);
            if (std::holds_alternative<ElementList>(e.inner)) {
                auto& l = std::get<ElementList>(e.inner);
                if (l.items.empty()) return e;
            }
            return v(blank(), std::move(e)).build();
        }),

        // Permission prompt
        dyn([&]() -> Element {
            if (!m.perm_open) return none();
            Permission p("bash", m.pending_perm_cmd);
            return v(
                blank(),
                p,
                t<"  press [space] to allow, [n] to deny"> | Dim
            ).build();
        }),

        // Closing summary callout — appears once per Done turn.
        dyn([&]() -> Element {
            if (m.phase != Phase::Done) return none();
            std::string detail;
            if (m.turn_changes_count > 0) {
                detail = std::to_string(m.turn_changes_count) +
                         " files modified  \xc2\xb7  ";
            }
            detail += "turn ";
            detail += std::to_string(m.turn_number);
            detail += " complete";
            return v(blank(),
                Callout::success("Done", detail)).build();
        }),

        // ── Composer & status bar (always pinned at the bottom) ────────
        blank(),
        dyn([&]{ return composer_view(m); }),
        dyn([&]{ return status_bar(m); })
    ).build();
}

// ============================================================================
// update() — pure transitions
// ============================================================================

static void track_tokens(Model& m, int new_out) {
    auto now = std::chrono::steady_clock::now();
    if (m.last_tok.time_since_epoch().count() != 0) {
        auto dt = std::chrono::duration<float>(now - m.last_tok).count();
        if (dt > 0.f) {
            float inst = float(new_out) / dt;
            m.tok_rate = 0.7f * m.tok_rate + 0.3f * inst;
            m.tok_history.push_back(m.tok_rate);
            if (m.tok_history.size() > 24)
                m.tok_history.erase(m.tok_history.begin());
        }
    }
    m.last_tok = now;
    m.out_tokens += new_out;
}

// Reset the "in-flight" portion of Model so a new turn can start clean.
static void reset_turn(Model& m) {
    m.user_prompt.clear();
    m.user_committed     = false;
    m.thinking           = ThinkingBlock{};
    m.thinking_active    = false;
    m.tools.clear();
    m.plan.clear();
    m.todos.clear();
    m.todos_status       = TodoListStatus::Pending;
    m.todos_elapsed      = 0.f;
    m.changes.clear();
    m.turn_changes_count = 0;
    m.ctx_warning.clear();
    m.md                 = StreamingMarkdown{};
    m.md_active          = false;
    m.assistant_body.clear();
    m.perm_open          = false;
    m.pending_perm_cmd.clear();
}

// The turn's worker: a jaal stream body. Captureless, and its arguments
// (the prompt, the phase) are owned values — nothing here is shared with
// the UI thread. sim's scenarios talk through a Dispatch; this builds one
// locally that posts into the Sink, and unwinds the scenario (Cancelled)
// at its next send once jaal has stopped the stream or the loop is gone.
static void turn_worker(jaal::Sink<Msg> out, std::stop_token st,
                        std::string prompt, int phase)
{
    sim::Dispatch d = [&out, &st](Msg msg) {
        if (st.stop_requested() || !out.send(std::move(msg)))
            throw sim::Cancelled{};
    };
    try {
        sim::run(prompt, phase, std::move(d));
    } catch (const sim::Cancelled&) {
        // Turn abandoned / program quitting: just end.
    }
}

// ============================================================================
// Program — wire it together. jaal dispatches P::update(Model&, Case) by
// overload resolution, so each Msg case is its own static member.
// ============================================================================

struct App {
    using Model = ::Model;
    using Msg   = ::Msg;
    using Cmd   = jaal::Cmd<Msg, commit_scrollback>;
    using Sub   = jaal::Sub<Msg>;

    static Cmd init(Model& m) {
        m.phase = Phase::Idle;
        // Autopilot: kick off the first scenario after a short beat so
        // the WelcomeScreen flashes briefly, then the demo starts on
        // its own. Pressing q / Esc / Ctrl-C still quits anytime.
        return Cmd::after(450ms, AutoNext{0});
    }

    static Cmd update(Model&, Quit) {
        return Cmd::quit(0);
    }

    static Cmd update(Model& m, Tick) {
        m.spinner_frame = (m.spinner_frame + 1) & 0xFFFF;
        // thinking's spinner is clock-driven — nothing to advance.
        if (m.phase != Phase::Idle && m.phase != Phase::Done)
            m.total_elapsed += 0.05f;
        return {};
    }

    // ── Composer input ────────────────────────────────────────────

    static Cmd update(Model& m, ComposerChar c) {
        if (m.phase != Phase::Idle && m.phase != Phase::Done)
            return {};
        if (c.cp == '\r' || c.cp == '\n')
            return {};
        // Inline UTF-8 encode (cp < 0x80 covers ASCII; anything else
        // arrives as multi-byte from CharKey already serialized).
        if (c.cp < 0x80) {
            m.composer_text += char(c.cp);
        } else if (c.cp < 0x800) {
            m.composer_text += char(0xC0 | (c.cp >> 6));
            m.composer_text += char(0x80 | (c.cp & 0x3F));
        } else if (c.cp < 0x10000) {
            m.composer_text += char(0xE0 | (c.cp >> 12));
            m.composer_text += char(0x80 | ((c.cp >> 6) & 0x3F));
            m.composer_text += char(0x80 | (c.cp & 0x3F));
        }
        m.composer_cursor = int(m.composer_text.size());
        m.starter_idx = -1;
        return {};
    }

    static Cmd update(Model& m, ComposerBackspace) {
        if (m.phase != Phase::Idle && m.phase != Phase::Done)
            return {};
        if (!m.composer_text.empty()) {
            // Trim trailing UTF-8 continuation bytes too.
            while (!m.composer_text.empty() &&
                   (static_cast<unsigned char>(m.composer_text.back()) & 0xC0) == 0x80) {
                m.composer_text.pop_back();
            }
            if (!m.composer_text.empty()) m.composer_text.pop_back();
            m.composer_cursor = int(m.composer_text.size());
        }
        return {};
    }

    static Cmd update(Model& m, ComposerCycle e) {
        if (m.phase != Phase::Idle && m.phase != Phase::Done)
            return {};
        const auto& list = starters();
        int n = int(list.size());
        int idx = m.starter_idx;
        idx = (idx < 0 ? 0 : idx + e.delta);
        idx = ((idx % n) + n) % n;
        m.starter_idx = idx;
        m.composer_text   = list[std::size_t(idx)];
        m.composer_cursor = int(m.composer_text.size());
        return {};
    }

    static Cmd update(Model& m, ComposerSubmit) {
        if (m.phase != Phase::Idle && m.phase != Phase::Done)
            return {};
        if (m.composer_text.empty())
            return {};

        std::string prompt = std::move(m.composer_text);
        m.composer_text.clear();
        m.composer_cursor = 0;
        m.starter_idx     = -1;

        reset_turn(m);
        m.user_prompt = prompt;
        m.turn_number += 1;
        m.turn_in_tokens = int(prompt.size() / 4);
        m.in_tokens     += m.turn_in_tokens;
        m.total_elapsed  = 0.f;
        m.phase          = Phase::Thinking;
        // The turn's worker is a Sub::stream keyed on (turn, phase):
        // setting these is what starts it (see subscribe()).
        m.turn_prompt    = std::move(prompt);
        m.stream_phase   = 1;
        return {};
    }

    static Cmd update(Model& m, GrantPerm) {
        // The grant is a MODEL change, not a flag a worker thread polls.
        // Phase 1's stream already ended at the ask; moving to phase 2
        // changes the stream key, so subscribe() starts the post-gate half
        // of the scenario as a fresh stream. No thread waits on another.
        if (m.perm_open && m.stream_phase == 1) {
            m.perm_open    = false;
            m.phase        = Phase::Tooling;
            m.stream_phase = 2;
        }
        return {};
    }

    static Cmd update(Model& m, AutoNext e) {
        // Don't interrupt an in-flight turn; if the user happened to
        // hit a key, defer until next idle.  In practice this branch
        // never fires because we only schedule AutoNext on Done /
        // init, but the guard keeps the autopilot from racing user
        // input.
        if (m.phase != Phase::Idle && m.phase != Phase::Done)
            return {};

        const auto& list = starters();
        int n = int(list.size());
        int idx = ((e.starter_idx % n) + n) % n;
        std::string prompt = list[std::size_t(idx)];

        m.composer_text.clear();
        m.composer_cursor = 0;
        m.starter_idx     = -1;

        reset_turn(m);
        m.user_prompt    = prompt;
        m.turn_number   += 1;
        m.turn_in_tokens = int(prompt.size() / 4);
        m.in_tokens     += m.turn_in_tokens;
        m.total_elapsed  = 0.f;
        m.phase          = Phase::Thinking;
        // The turn's worker is a Sub::stream keyed on (turn, phase):
        // setting these is what starts it (see subscribe()).
        m.turn_prompt    = std::move(prompt);
        m.stream_phase   = 1;
        return {};
    }

    static Cmd update(Model& m, Stream s) {
        // followup is a Cmd we may schedule from inside the visit
        // (auto-grant on PermissionAsk, auto-cycle on MessageStop).
        Cmd followup = Cmd{};
        std::visit(overload{
            [&](ev::SessionStart&) {
                m.phase = Phase::Thinking;
                // Commit the user message NOW (agentty pattern):
                // every settled turn pair is one user Turn followed
                // by one assistant Turn. The assistant body slots
                // accumulate in m.assistant_body across the turn
                // and get wrapped in a single Role::Assistant Turn
                // at MessageStop.
                if (!m.user_committed && !m.user_prompt.empty()) {
                    m.frozen.seal(gap(), 1, /*separator=*/true);
                    m.frozen.seal_measured(
                        user_turn_element(m, m.user_prompt),
                        /*fallback_est_rows=*/3);
                    m.user_committed = true;
                }
                m.assistant_body.clear();
            },
            [&](ev::ThinkingDelta& e) {
                if (!m.thinking_active) {
                    m.thinking.set_active(true);
                    m.thinking_active = true;
                }
                m.thinking.append(e.text);
                track_tokens(m, int(e.text.size() / 4) + 1);
                m.phase = Phase::Thinking;
            },
            [&](ev::ThinkingStop&) {
                m.thinking.set_active(false);
                m.thinking_active = false;
                // Append to assistant body — committed inside the
                // wrapping assistant Turn at MessageStop.
                m.assistant_body.push_back(m.thinking.build());
                m.thinking = ThinkingBlock{};
                m.phase = Phase::Tooling;
            },
            [&](ev::ToolBegin& e) {
                LiveTool t;
                t.id     = e.id;
                t.kind   = e.kind;
                t.detail = std::move(e.detail_partial);
                t.status = AgentEventStatus::Pending;
                m.tools.push_back(std::move(t));
                m.phase = Phase::Tooling;
            },
            [&](ev::ToolDetailDelta& e) {
                if (auto* t = find_tool(m, e.id))
                    t->detail += e.chunk;
            },
            [&](ev::ToolBodyDelta& e) {
                if (auto* t = find_tool(m, e.id)) {
                    if (t->status == AgentEventStatus::Pending)
                        t->status = AgentEventStatus::Running;
                    t->body += e.chunk;
                    track_tokens(m, int(e.chunk.size() / 6) + 1);
                }
            },
            [&](ev::ToolEnd& e) {
                if (auto* t = find_tool(m, e.id)) {
                    t->status = e.ok ? AgentEventStatus::Done
                                     : AgentEventStatus::Failed;
                    t->elapsed = e.elapsed;
                }
            },
            [&](ev::PlanCreated& e) {
                m.plan.clear();
                for (auto& s_ : e.tasks)
                    m.plan.push_back({std::move(s_), TaskStatus::Pending});
            },
            [&](ev::PlanAdvance& e) {
                if (e.task_index >= 0 && e.task_index < int(m.plan.size())) {
                    for (int i = 0; i < e.task_index; ++i)
                        m.plan[std::size_t(i)].status = TaskStatus::Completed;
                    m.plan[std::size_t(e.task_index)].status = TaskStatus::InProgress;
                }
            },
            [&](ev::TodoCreated& e) {
                m.todos.clear();
                for (auto& s_ : e.items)
                    m.todos.push_back({std::move(s_), TodoItemStatus::Pending});
                m.todos_status = TodoListStatus::Running;
            },
            [&](ev::TodoAdvance& e) {
                if (e.item_index >= 0 && e.item_index < int(m.todos.size())) {
                    for (int i = 0; i < e.item_index; ++i)
                        m.todos[std::size_t(i)].status = TodoItemStatus::Completed;
                    m.todos[std::size_t(e.item_index)].status = TodoItemStatus::InProgress;
                }
            },
            [&](ev::PermissionAsk& e) {
                m.pending_perm_cmd = std::move(e.command);
                m.perm_open       = true;
                m.phase           = Phase::AwaitingPermission;
                // Autopilot: grant after a short beat so the demo
                // runs end-to-end unattended. (Press space yourself
                // sooner if you want to skip the wait.)
                followup = Cmd::after(160ms, GrantPerm{});
            },
            [&](ev::PermissionDone&) {
                m.perm_open = false;
                m.phase     = Phase::Tooling;
            },
            [&](ev::FileChanged& e) {
                FileChange fc;
                fc.path          = std::move(e.path);
                fc.kind          = e.kind;
                fc.lines_added   = e.added;
                fc.lines_removed = e.removed;
                m.changes.push_back(std::move(fc));
                m.turn_changes_count += 1;
            },
            [&](ev::ContextWarning& e) {
                m.ctx_warning = std::move(e.text);
                // Bump used tokens to make the gauge match.
                m.in_tokens = std::max(m.in_tokens,
                                       int(m.ctx_max * 0.62));
            },
            [&](ev::AssistantDelta& e) {
                if (!m.md_active) {
                    // Roll up in-flight live panels into the
                    // assistant body BEFORE the prose block starts
                    // — same interleave-within-one-message shape
                    // agentty produces. These become body slots of
                    // the single assistant Turn we push at
                    // MessageStop.
                    if (!m.tools.empty()) {
                        m.assistant_body.push_back(
                            actions_panel(m, /*live=*/false));
                        m.tools.clear();
                    }
                    if (!m.plan.empty()) {
                        for (auto& it : m.plan)
                            it.status = TaskStatus::Completed;
                        m.assistant_body.push_back(plan_card(m));
                        m.plan.clear();
                    }
                    if (!m.todos.empty()) {
                        for (auto& it : m.todos)
                            it.status = TodoItemStatus::Completed;
                        m.todos_status = TodoListStatus::Done;
                        m.assistant_body.push_back(todos_card(m));
                        m.todos.clear();
                    }
                    if (!m.ctx_warning.empty()) {
                        m.assistant_body.push_back(ctx_banner(m));
                        m.ctx_warning.clear();
                    }
                    m.md = StreamingMarkdown{};
                    m.md_active = true;
                }
                m.md.append(e.text);
                track_tokens(m, int(e.text.size() / 4) + 1);
                m.phase = Phase::Streaming;
            },
            [&](ev::TextBlockStop&) {
                // Append the just-closed text block to the assistant
                // body; more tools / text blocks may follow inside
                // the same Turn.
                if (m.md_active) {
                    m.md.finish();
                    m.assistant_body.push_back(
                        AssistantMessage::build(m.md.build()));
                    m.md = StreamingMarkdown{};
                    m.md_active = false;
                }
                m.phase = Phase::Tooling;
            },
            [&](ev::MessageStop&) {
                // Final commit. Roll any remaining live state into
                // the assistant body, then wrap the entire body in
                // ONE Role::Assistant Turn and push it to frozen.
                // Mirrors agentty's one-Message-equals-one-Turn
                // shape exactly.
                if (m.md_active) {
                    m.md.finish();
                    m.assistant_body.push_back(
                        AssistantMessage::build(m.md.build()));
                    m.md = StreamingMarkdown{};
                    m.md_active = false;
                }
                if (!m.tools.empty()) {
                    m.assistant_body.push_back(
                        actions_panel(m, /*live=*/false));
                    m.tools.clear();
                }

                if (!m.assistant_body.empty()) {
                    m.frozen.seal(gap(), 1, /*separator=*/true);
                    m.frozen.seal_measured(
                        settled_assistant_turn_element(m),
                        /*fallback_est_rows=*/12);
                    m.assistant_body.clear();
                }

                if (!m.changes.empty()) {
                    m.frozen.seal(gap(), 1, /*separator=*/true);
                    m.frozen.seal_measured(changes_card(m),
                                           /*fallback_est_rows=*/4);
                    m.changes.clear();
                }
                m.phase = Phase::Done;

                // ── Bounded scrollback ─────────────────────────────
                // Each turn seals ~2-6 blocks into m.frozen, and
                // the inline renderer's prev_cells buffer mirrors
                // every row ever drawn so the row diff has something
                // to compare against.  Without bounding, a multi-hour
                // autopilot loop accumulates hundreds of MB resident,
                // which macOS jetsam will kill with SIGKILL.
                //
                // When frozen exceeds the soft cap, drop the oldest
                // chunk and tell the renderer those rows are now
                // committed to terminal scrollback.  The commit count
                // is NOT guessed: ledger.harvest() mints a typed
                // ScrollbackDebt from the heights maya's own paint
                // pass recorded for the dropped blocks — exact by
                // construction (Witness Chain, Trim Accounting).
                // The dropped content is still visible in the user's
                // terminal scrollback — we're only shrinking the
                // *renderer's* mirror, not what the user can see.
                Cmd trim_cmd = Cmd{};
                constexpr int FROZEN_MAX  = 240;
                constexpr int FROZEN_TRIM = 80;
                if (int(m.frozen.size()) > FROZEN_MAX) {
                    int n = std::min<int>(FROZEN_TRIM,
                        int(m.frozen.size()) - FROZEN_MAX / 2);
                    m.frozen.drop_front(std::size_t(n));
                    // The debt, not a row count: only the ledger can mint
                    // one, from rows maya's paint pass recorded, so this
                    // can't commit a number that drifts from the wire.
                    trim_cmd = commit_from<Cmd>(m.frozen.harvest());
                }

                int next_idx = m.turn_number % int(starters().size());
                Cmd cycle = Cmd::after(700ms,
                    AutoNext{next_idx});
                followup = trim_cmd.is_none()
                    ? std::move(cycle)
                    : Cmd::batch(std::move(trim_cmd),
                                      std::move(cycle));
            },
        }, s.ev);
        return followup;
    }

    static Element view(const Model& m) { return ::view(m); }

    static std::optional<Msg> on(const Model&, const KeyEvent& k) {
        // Hard-quit chord
        if (ctrl_is(k, 'c')) return Msg{Quit{}};

        // Specials
        if (key_is(k, SpecialKey::Escape))    return Msg{Quit{}};
        if (key_is(k, SpecialKey::Enter))     return Msg{ComposerSubmit{}};
        if (key_is(k, SpecialKey::Tab))       return Msg{ComposerCycle{+1}};
        if (key_is(k, SpecialKey::BackTab))   return Msg{ComposerCycle{-1}};
        if (key_is(k, SpecialKey::Up))        return Msg{ComposerCycle{-1}};
        if (key_is(k, SpecialKey::Down))      return Msg{ComposerCycle{+1}};
        if (key_is(k, SpecialKey::Backspace)) return Msg{ComposerBackspace{}};

        // Space — context-sensitive: if a permission card is open it grants;
        // otherwise it's just a literal space character into the composer.
        if (key_is(k, ' ')) return Msg{GrantPerm{}};

        // Plain character — type into the composer.
        if (auto* ch = std::get_if<CharKey>(&k.key)) {
            if (!k.mods.ctrl && !k.mods.alt && !k.mods.super_) {
                return Msg{ComposerChar{ch->codepoint}};
            }
        }
        return std::nullopt;
    }

    // The animation tick, plus the turn's WORKER.
    //
    // The worker is a Sub::stream keyed on (turn, phase). It runs exactly as
    // long as the model says a turn is in flight and not blocked on the
    // user: while the permission card is open there is NO worker at all
    // (phase 1 has finished at the ask, phase 2 hasn't started). The grant
    // bumps stream_phase, the key changes, and jaal starts phase 2. A turn
    // abandoned for a new prompt changes the key too, so jaal stops the old
    // worker and drops anything it still sends.
    static Sub subscribe(const Model& m) {
        auto tick = Sub::every(50ms, Tick{});

        const bool working = m.stream_phase > 0 && !m.perm_open
                          && m.phase != Phase::Idle && m.phase != Phase::Done;
        if (!working) return tick;

        auto worker = Sub::stream(
            "turn/" + std::to_string(m.turn_number) + "/" + std::to_string(m.stream_phase),
            turn_worker, m.turn_prompt, m.stream_phase);
        return Sub::batch(std::move(tick), std::move(worker));
    }
};

static_assert(Program<App>);

int main() {
    return run<App>({
        .title = "agent session",
        .mode  = Mode::Inline,
    });
}
