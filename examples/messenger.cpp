// examples/messenger.cpp — multi-channel terminal chat.
//
// Simulates a small group of peers chatting across four channels.
// Demonstrates:
//
//   - Pure update() / view() / subscribe() — no Signal, no mutable globals
//   - Sub::every() driving deterministic peer simulation
//   - on(KeyEvent) catch-all for free-form composer typing (UTF-8 aware)
//   - on(ResizeEvent) for terminal-aware layout
//   - Compile-time + runtime DSL pipes mixed in one expression
//   - vector<Element> children via the DslChild::ElementRange path
//   - Overlay widget for the channel jumper / help modals
//
// Controls
//
//   composer
//     type            insert character
//     left/right      move cursor
//     ctrl+a / ^E     jump to line start / end
//     ctrl+w          delete previous word
//     ctrl+u / ^K     clear to start / end of line
//     backspace       delete previous char
//     enter           send (or run slash command)
//
//   navigation
//     tab / shift+tab next / prev channel
//     up / down       scroll one message
//     PgUp / PgDn     scroll one page
//     end             jump to latest
//     ctrl+g          channel jumper
//
//   app
//     ctrl+l          clear current channel
//     ctrl+h          help
//     q / esc / ^C    quit
//
//   slash commands  (type into composer, press enter)
//     /help                       this list
//     /me <action>                emote, e.g. /me shrugs
//     /clear                      clear current channel
//     /topic <text>               retitle channel
//     /status active|away|dnd     set presence
//     /who                        list members as a system message
//     /quit                       exit

#include <maya/host/run.hpp>
#include <maya/maya.hpp>
#include <maya/widget/agentty/overlay.hpp>
#include <maya/widget/agentty/scrollbar.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

using namespace maya;
using namespace maya::dsl;
using namespace std::chrono_literals;

// ─── Palette ────────────────────────────────────────────────────────────────

namespace pal {
    constexpr Color bg          = Color::rgb( 22,  25,  32);
    constexpr Color bg_self     = Color::rgb( 26,  32,  42);
    constexpr Color panel       = Color::rgb( 28,  32,  41);
    constexpr Color panel2      = Color::rgb( 36,  40,  52);
    constexpr Color modal       = Color::rgb( 42,  48,  62);
    constexpr Color text        = Color::rgb(220, 224, 232);
    constexpr Color muted       = Color::rgb(124, 132, 148);
    constexpr Color dim         = Color::rgb( 82,  88, 102);
    constexpr Color accent      = Color::rgb(120, 200, 220);
    constexpr Color amber       = Color::rgb(230, 180, 100);
    constexpr Color green       = Color::rgb(140, 200, 130);
    constexpr Color red         = Color::rgb(230, 110, 110);
    constexpr Color pink        = Color::rgb(230, 130, 190);
    constexpr Color purple      = Color::rgb(180, 130, 230);

    // Telegram-style message-bubble palette
    constexpr Color sent_bg     = Color::rgb(122, 102, 240);   // purple
    constexpr Color sent_fg     = Color::rgb(255, 255, 255);
    constexpr Color sent_meta   = Color::rgb(210, 202, 252);
    constexpr Color sent_read   = Color::rgb(255, 255, 255);
    constexpr Color recv_bg     = Color::rgb( 55,  62,  82);   // visibly lighter slate
    constexpr Color recv_fg     = Color::rgb(235, 238, 248);
    constexpr Color recv_meta   = Color::rgb(160, 168, 188);
}

// ─── Glyphs (UTF-8) ─────────────────────────────────────────────────────────

namespace gl {
    constexpr const char* dot_on   = "\xe2\x97\x8f";   // ●
    constexpr const char* dot_off  = "\xe2\x97\x8b";   // ○
    constexpr const char* arrow    = "\xe2\x96\xb6";   // ▶
    constexpr const char* bar      = "\xe2\x96\x8c";   // ▌
    constexpr const char* ellipsis = "\xe2\x8b\xaf";   // ⋯
    constexpr const char* prompt   = "\xe2\x9d\xaf";   // ❯
    constexpr const char* enter    = "\xe2\x8f\x8e";   // ⏎
    constexpr const char* bell     = "\xe2\x9a\xa0";   // ⚠
    constexpr const char* spark    = "\xe2\x9c\xa6";   // ✦
}

// ─── Domain types ───────────────────────────────────────────────────────────

enum class Presence : std::uint8_t { Active, Away, Dnd, Offline };

struct User {
    std::string name;
    Color       color;
    Presence    presence = Presence::Active;
};

struct Reaction {
    std::string emoji;   // see emoji_glyph() for display mapping
    int         count = 1;
};

struct Message {
    int                   author_uid;          // -1 == system event
    std::string           body;
    float                 timestamp    = 0.f;  // session-relative seconds
    bool                  mentions_you = false;
    bool                  is_action    = false;  // /me messages
    std::vector<Reaction> reactions;
};

struct Channel {
    std::string          name;
    std::string          topic;
    std::vector<Message> messages;
    int                  unread       = 0;
    float                flash_until  = 0.f;   // amber flash on @mention
};

struct TypingPulse {
    int   uid;
    int   channel;
    float until;
};

struct BotEvent {
    float       typing_at;
    float       send_at;
    int         channel;
    int         uid;
    std::string body;
};

// ─── Program ────────────────────────────────────────────────────────────────

struct Messenger {

    static constexpr int kYou = 0;

    // -- Seed data ----------------------------------------------------------

    static std::vector<User> seed_users() {
        return {
            { "you",   pal::accent, Presence::Active  },
            { "alice", pal::pink,   Presence::Active  },
            { "bob",   pal::amber,  Presence::Active  },
            { "carla", pal::green,  Presence::Active  },
            { "dave",  pal::purple, Presence::Away    },
        };
    }

    static std::vector<Channel> seed_channels() {
        return {
            // Group channels
            { "#general", "the catch-all room",      {}, 0, 0.f },
            { "#dev",     "engineering chatter",     {}, 0, 0.f },
            { "#random",  "the off-topic zone",      {}, 0, 0.f },
            { "#help",    "stuck? ask here",         {}, 0, 0.f },
            // 1-1 direct messages — name matches a user in seed_users()
            { "alice",    "direct message",          {}, 0, 0.f },
            { "bob",      "direct message",          {}, 0, 0.f },
            { "carla",    "direct message",          {}, 0, 0.f },
        };
    }

    static std::vector<BotEvent> seed_script() {
        // Channel indices: 0=#general, 1=#dev, 2=#random, 3=#help,
        //                  4=alice DM, 5=bob DM, 6=carla DM
        return {
            // Group activity
            { 1.5f,  3.5f,  0, 1, "hey, anyone awake?"                                          },
            { 4.2f,  6.5f,  0, 2, "morning, just kicked off the deploy"                         },
            { 7.0f,  10.0f, 1, 3, "seeing weird latency in the diff layer — anyone touch it?"   },
            { 10.5f, 13.0f, 1, 2, "yeah I rebased onto main last night, you might be hitting f620030" },
            { 13.5f, 16.0f, 0, 1, "@you we need a UI review when you have a sec"                },
            { 16.5f, 19.0f, 2, 4, "anyone seen the new builds? the renderer demos go hard"     },
            { 19.5f, 21.0f, 1, 3, "the strip renderer is going to be wild when it lands"        },
            { 21.5f, 23.5f, 3, 2, "if anyone hits permission prompts try --allow-bash"          },
            { 24.0f, 26.5f, 0, 1, "lunch?"                                                       },
            { 27.0f, 29.5f, 0, 2, "im in. 12:30?"                                                },
            // 1-1 DMs interleaved through the session
            { 5.0f,  7.5f,  4, 1, "@you hey, got a min for a quick UI Q?"                       },
            { 8.5f,  10.5f, 4, 1, "the composer caret position drifts when I paste, expected?"  },
            { 12.0f, 14.5f, 5, 2, "btw — interested in the A3 strip renderer work? I'm picking it up next week" },
            { 18.0f, 20.0f, 6, 3, "lunch tomorrow? trying that new ramen place"                 },
            { 30.0f, 32.0f, 1, 3, "@you can you take a look at PR #4421 when you're back?"     },
            { 33.0f, 35.0f, 4, 1, "@you nvm figured it out — utf8 boundary thing"               },
        };
    }

    static const std::array<std::string_view, 24>& ambient_phrases() {
        static constexpr std::array<std::string_view, 24> p = {
            "ship it", "lgtm", "I'll take a look in a bit",
            "did anyone update the docs for that?", "merge conflict",
            "tests are green", "tests are red", "rebase needed",
            "+1", "approved", "can someone review #4421",
            "I think the cache is invalidating wrong", "saw it, will fix",
            "this is fine", "back from coffee", "afk for 20",
            "anyone seen carla's last PR?", "@you do you have a min later?",
            "OOO friday", "huh, that's a new one", "reverted it for now",
            "small nit but otherwise good", "branch was force-pushed, fyi",
            "going to grab dinner, brb"
        };
        return p;
    }

    // -- Model --------------------------------------------------------------

    struct Model {
        std::vector<User>        users;
        std::vector<Channel>     channels;
        std::vector<BotEvent>    script;
        std::vector<TypingPulse> typing;

        int           active_channel  = 0;
        std::string   composer;
        int           cursor          = 0;       // byte offset into composer

        float         clock           = 0.f;
        std::uint32_t rng_state       = 0x12345u;
        int           tick_n          = 0;
        float         next_ambient_at = 35.f;

        // Layout — driven by on(ResizeEvent)
        int term_w = 120;
        int term_h = 40;

        // Modal overlays
        bool          jumper_open = false;
        std::string   jumper_filter;
        int           jumper_index = 0;
        bool          help_open   = false;
        bool          right_panel_open = true;

        // Scroll state — `mutable` because view() takes Model by const ref
        // but the framework's writeback fills max_y/viewport_bounds at the
        // end of each render. auto_dispatch is off everywhere: we route
        // wheel events via viewport hit-test in the update() RawMouse
        // handler so wheel scrolls the panel under the cursor — not all
        // of them at once.
        mutable ScrollState msg_scroll;
        mutable ScrollState help_scroll;
        mutable ScrollState chats_scroll;
        mutable ScrollState members_scroll;
        mutable ScrollState tabs_scroll;
    };

    // -- Msg ----------------------------------------------------------------

    struct Tick {};
    struct CharIn        { char32_t cp; };
    struct Backspace     {};
    struct DeleteWord    {};
    struct DeleteToStart {};
    struct DeleteToEnd   {};
    struct CursorLeft    {};
    struct CursorRight   {};
    struct CursorHome    {};
    struct CursorEnd     {};
    struct SendComposer  {};
    struct NextChannel   {};
    struct PrevChannel   {};
    struct ClearChannel  {};
    struct ScrollUp        {};
    struct ScrollDown      {};
    struct ScrollPageUp    {};
    struct ScrollPageDn    {};
    struct ScrollLatest    {};
    struct HelpScroll      { int dy; };
    struct OpenJumper    {};
    struct CloseJumper   {};
    struct JumperChar    { char32_t cp; };
    struct JumperBack    {};
    struct JumperUp      {};
    struct JumperDown    {};
    struct JumperPick    {};
    struct OpenHelp      {};
    struct CloseHelp     {};
    struct Resize        { int w; int h; };
    struct RawMouse      { MouseEvent ev; };
    struct ToggleRightPanel {};
    struct Quit          {};

    using Msg = std::variant<
        Tick, CharIn, Backspace, DeleteWord, DeleteToStart, DeleteToEnd,
        CursorLeft, CursorRight, CursorHome, CursorEnd,
        SendComposer, NextChannel, PrevChannel, ClearChannel,
        ScrollUp, ScrollDown, ScrollPageUp, ScrollPageDn, ScrollLatest, HelpScroll,
        OpenJumper, CloseJumper, JumperChar, JumperBack, JumperUp, JumperDown, JumperPick,
        OpenHelp, CloseHelp, Resize, RawMouse, ToggleRightPanel, Quit>;

    using Cmd = jaal::Cmd<Msg>;
    using Sub = jaal::Sub<Msg>;

    // -- init ---------------------------------------------------------------

    static Cmd init(Model& m) {
        m.users    = seed_users();
        m.channels = seed_channels();
        m.script   = seed_script();
        m.msg_scroll.auto_dispatch     = false;
        m.help_scroll.auto_dispatch    = false;
        m.chats_scroll.auto_dispatch   = false;
        m.members_scroll.auto_dispatch = false;
        m.tabs_scroll.auto_dispatch    = false;
        m.msg_scroll.step_y     = 1;
        m.help_scroll.step_y    = 1;
        m.chats_scroll.step_y   = 1;
        m.members_scroll.step_y = 1;
        m.tabs_scroll.step_x    = 3;
        for (auto& ch : m.channels) {
            Message sys;
            sys.author_uid = -1;
            sys.body       = "joined " + ch.name + " · " + ch.topic;
            sys.timestamp  = -10.f;
            ch.messages.push_back(std::move(sys));
        }
        return {};
    }

    // -- Pure helpers -------------------------------------------------------

    static bool mentions_user(std::string_view body, std::string_view who) {
        for (size_t i = 0; i + who.size() <= body.size(); ++i) {
            if (body[i] != '@') continue;
            if (body.compare(i + 1, who.size(), who) == 0) return true;
        }
        return false;
    }

    static std::uint32_t rng_next(std::uint32_t& s) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return s;
    }

    static std::string fmt_age(float clock, float ts) {
        if (ts < 0) return "—";
        int age = static_cast<int>(clock - ts);
        if (age <= 1)   return "now";
        if (age < 60)   return std::to_string(age) + "s";
        if (age < 3600) return std::to_string(age / 60) + "m";
        return std::to_string(age / 3600) + "h";
    }

    static const char* presence_dot(Presence p) {
        switch (p) {
            case Presence::Active:  return gl::dot_on;
            case Presence::Away:    return gl::dot_off;
            case Presence::Dnd:     return gl::dot_on;
            case Presence::Offline: return gl::dot_off;
        }
        return gl::dot_off;
    }

    static Color presence_color(Presence p) {
        switch (p) {
            case Presence::Active:  return pal::green;
            case Presence::Away:    return pal::amber;
            case Presence::Dnd:     return pal::red;
            case Presence::Offline: return pal::dim;
        }
        return pal::dim;
    }

    static const char* presence_label(Presence p) {
        switch (p) {
            case Presence::Active:  return "active";
            case Presence::Away:    return "away";
            case Presence::Dnd:     return "dnd";
            case Presence::Offline: return "offline";
        }
        return "?";
    }

    static bool is_dm(const Channel& ch) {
        return !ch.name.empty() && ch.name[0] != '#';
    }

    static int dm_uid(const Model& m, std::string_view name) {
        for (size_t i = 0; i < m.users.size(); ++i)
            if (m.users[i].name == name) return int(i);
        return -1;
    }

    // 2-letter id for an avatar — strips a leading '#' for channels.
    static std::string two_letter_id(std::string_view name) {
        if (!name.empty() && name[0] == '#') name.remove_prefix(1);
        char a = name.size() > 0 ? char(std::toupper((unsigned char)name[0])) : '?';
        char b = name.size() > 1 ? char(std::toupper((unsigned char)name[1])) : ' ';
        return std::string{a, b};
    }

    // Deterministic palette pick for channel avatars.
    static Color channel_color(int idx) {
        static constexpr Color palette[5] = {
            pal::accent, pal::amber, pal::green, pal::pink, pal::purple
        };
        return palette[((idx % 5) + 5) % 5];
    }

    // The one place we still use background colors — for avatars, which the
    // user explicitly allowed. " XX " gives a 4-cell colored chip; the dark
    // foreground keeps the letters readable on the colored fill.
    static Element avatar(std::string_view label, Color bg_c) {
        return text(" " + two_letter_id(label) + " ",
            Style{}.with_bg(bg_c)
                   .with_fg(Color::rgb(20, 22, 28))
                   .with_bold());
    }

    // Telegram's round unread-count badges. ❶ … ❿ are single-cell
    // dingbat "negative circled digit" glyphs that read as filled
    // pills against any background — no bg color needed.
    static std::string circled_digit(int n) {
        if (n <= 0)   return "";
        if (n <= 10) {
            static const char* g[10] = {
                "\xe2\x9d\xb6", "\xe2\x9d\xb7", "\xe2\x9d\xb8",
                "\xe2\x9d\xb9", "\xe2\x9d\xba", "\xe2\x9d\xbb",
                "\xe2\x9d\xbc", "\xe2\x9d\xbd", "\xe2\x9d\xbe",
                "\xe2\x9d\xbf",
            };
            return g[n - 1];
        }
        return std::to_string(n);
    }

    static std::string truncate(std::string s, size_t max) {
        if (s.size() <= max) return s;
        std::string out = s.substr(0, max - 3);
        // Trim trailing whitespace so the wrap engine can't break on the
        // space immediately before "..." and dump the ellipsis to row 2.
        while (!out.empty() && out.back() == ' ') out.pop_back();
        return out + "...";
    }

    // Single-cell UTF-8 glyphs so reaction chips don't drift the layout
    // on terminals with sketchy wide-char width metrics.
    static std::string emoji_glyph(std::string_view name) {
        if (name == "+1")     return "+1";
        if (name == "heart")  return "\xe2\x99\xa5";  // ♥
        if (name == "fire")   return "\xe2\x98\x85";  // ★
        if (name == "eyes")   return "\xe2\x97\x89";  // ◉
        if (name == "rocket") return "\xe2\x96\xb2";  // ▲
        if (name == "tada")   return "\xe2\x9c\xa6";  // ✦
        return std::string{name};
    }

    static void encode_utf8(char32_t cp, std::string& out, int pos) {
        std::string s;
        if (cp < 0x80) {
            s.push_back(char(cp));
        } else if (cp < 0x800) {
            s.push_back(char(0xC0 | (cp >> 6)));
            s.push_back(char(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            s.push_back(char(0xE0 | (cp >> 12)));
            s.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(char(0x80 | (cp & 0x3F)));
        } else {
            s.push_back(char(0xF0 | (cp >> 18)));
            s.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
            s.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(char(0x80 | (cp & 0x3F)));
        }
        out.insert(static_cast<size_t>(pos), s);
    }

    static int utf8_prev(const std::string& s, int pos) {
        if (pos <= 0) return 0;
        int p = pos - 1;
        while (p > 0 && (((unsigned char)s[p] & 0xC0) == 0x80)) p--;
        return p;
    }

    static int utf8_next(const std::string& s, int pos) {
        int n = int(s.size());
        if (pos >= n) return n;
        int p = pos + 1;
        while (p < n && (((unsigned char)s[p] & 0xC0) == 0x80)) p++;
        return p;
    }

    static int prev_word(const std::string& s, int pos) {
        // Walk back over spaces, then back over non-spaces.
        while (pos > 0 && std::isspace((unsigned char)s[pos - 1])) --pos;
        while (pos > 0 && !std::isspace((unsigned char)s[pos - 1])) --pos;
        return pos;
    }

    // Sentinel y value that triggers "stay glued to bottom" on the next
    // paint — the renderer/scrollbar clamps to max_y.
    static constexpr int kBottomAnchor = 1 << 28;

    static void deliver(Model& m, int chan, int uid, const std::string& body,
                        bool is_action = false) {
        if (chan < 0 || chan >= int(m.channels.size())) return;

        // "Was the user reading the live tail?" — preserve their position
        // when they're scrolled up in history, follow the bottom otherwise.
        bool was_at_bottom = (chan == m.active_channel)
            && (m.msg_scroll.y >= m.msg_scroll.max_y - 1);

        Message msg;
        msg.author_uid   = uid;
        msg.body         = body;
        msg.timestamp    = m.clock;
        msg.mentions_you = !is_action && mentions_user(body, "you");
        msg.is_action    = is_action;
        m.channels[chan].messages.push_back(std::move(msg));

        m.typing.erase(
            std::remove_if(m.typing.begin(), m.typing.end(),
                [&](const TypingPulse& t) {
                    return t.uid == uid && t.channel == chan;
                }),
            m.typing.end());

        if (chan != m.active_channel) {
            m.channels[chan].unread += 1;
            if (m.channels[chan].messages.back().mentions_you)
                m.channels[chan].flash_until = m.clock + 4.f;
        } else if (was_at_bottom) {
            m.msg_scroll.y = kBottomAnchor;
        }
    }

    static void push_system(Channel& ch, std::string body, float clock) {
        Message sys;
        sys.author_uid = -1;
        sys.body       = std::move(body);
        sys.timestamp  = clock;
        ch.messages.push_back(std::move(sys));
    }

    // -- Slash commands -----------------------------------------------------

    // Returns true if the body was consumed as a command (don't deliver as msg).
    static bool run_command(Model& m, std::string_view body) {
        if (body.empty() || body[0] != '/') return false;
        body.remove_prefix(1);

        auto split = [&](std::string_view& head, std::string_view& tail) {
            auto sp = body.find(' ');
            if (sp == std::string_view::npos) { head = body; tail = {}; }
            else { head = body.substr(0, sp); tail = body.substr(sp + 1); }
        };
        std::string_view cmd, arg;
        split(cmd, arg);

        auto& ch = m.channels[m.active_channel];

        if (cmd == "help") {
            m.help_open = true;
            return true;
        }
        if (cmd == "me" && !arg.empty()) {
            deliver(m, m.active_channel, kYou, std::string{arg}, /*is_action=*/true);
            return true;
        }
        if (cmd == "clear") {
            ch.messages.clear();
            push_system(ch, "channel cleared", m.clock);
            m.msg_scroll.y = kBottomAnchor;
            return true;
        }
        if (cmd == "topic" && !arg.empty()) {
            ch.topic = std::string{arg};
            push_system(ch, "you changed the topic to: " + ch.topic, m.clock);
            return true;
        }
        if (cmd == "status" && !arg.empty()) {
            Presence p = Presence::Active;
            if      (arg == "away")     p = Presence::Away;
            else if (arg == "dnd")      p = Presence::Dnd;
            else if (arg == "active")   p = Presence::Active;
            else if (arg == "offline")  p = Presence::Offline;
            m.users[kYou].presence = p;
            push_system(ch, std::string{"you are now "} + presence_label(p),
                        m.clock);
            return true;
        }
        if (cmd == "who") {
            std::string list = "members: ";
            for (size_t i = 0; i < m.users.size(); ++i) {
                if (i) list += ", ";
                list += m.users[i].name;
                list += " (";
                list += presence_label(m.users[i].presence);
                list += ")";
            }
            push_system(ch, std::move(list), m.clock);
            return true;
        }
        if (cmd == "quit") {
            // handled by view → main; signal via flag is unnecessary, the
            // Send handler will return a Quit cmd if it detects this
            return false;  // let SendComposer catch it specially
        }
        // Unknown command — system feedback.
        push_system(ch, "unknown command: /" + std::string{cmd}
                       + "  (try /help)", m.clock);
        return true;
    }

    // -- Simulation step ----------------------------------------------------

    static void tick_bots(Model& m, float dt) {
        m.clock  += dt;
        m.tick_n += 1;

        m.typing.erase(
            std::remove_if(m.typing.begin(), m.typing.end(),
                [&](const TypingPulse& t) { return t.until <= m.clock; }),
            m.typing.end());

        for (auto it = m.script.begin(); it != m.script.end(); ) {
            const auto& ev = *it;
            if (ev.typing_at <= m.clock && ev.send_at > m.clock) {
                bool active = std::any_of(m.typing.begin(), m.typing.end(),
                    [&](const TypingPulse& t) {
                        return t.uid == ev.uid && t.channel == ev.channel;
                    });
                if (!active)
                    m.typing.push_back({ev.uid, ev.channel, ev.send_at + 0.05f});
                ++it;
            } else if (ev.send_at <= m.clock) {
                deliver(m, ev.channel, ev.uid, ev.body);
                it = m.script.erase(it);
            } else {
                ++it;
            }
        }

        if (m.script.empty() && m.clock >= m.next_ambient_at) {
            const auto& phrases = ambient_phrases();
            std::uint32_t r1 = rng_next(m.rng_state);
            std::uint32_t r2 = rng_next(m.rng_state);
            std::uint32_t r3 = rng_next(m.rng_state);
            std::uint32_t r4 = rng_next(m.rng_state);

            int uid  = 1 + int(r1 % (m.users.size() - 1));
            int chan = int(r2 % m.channels.size());
            // In a DM, only the other participant can speak — re-pin uid.
            if (is_dm(m.channels[chan])) {
                int partner = dm_uid(m, m.channels[chan].name);
                if (partner > 0) uid = partner;
            }
            const auto body = phrases[r3 % phrases.size()];

            m.script.push_back({m.clock, m.clock + 1.6f, chan, uid, std::string{body}});
            m.next_ambient_at = m.clock + 3.5f + float(r4 % 50) / 10.f;
        }

        if ((m.tick_n % 60) == 0) {
            std::uint32_t r = rng_next(m.rng_state);
            int chan = int(r % m.channels.size());
            auto& ch = m.channels[chan];
            if (ch.messages.size() >= 2) {
                std::uint32_t r2 = rng_next(m.rng_state);
                int span = int(std::min<size_t>(3, ch.messages.size()));
                int idx  = int(ch.messages.size()) - 1 - int(r2 % span);
                if (idx >= 0 && ch.messages[idx].author_uid != -1) {
                    static constexpr std::array<std::string_view, 6> em = {
                        "+1", "heart", "fire", "eyes", "rocket", "tada"
                    };
                    std::uint32_t r3 = rng_next(m.rng_state);
                    auto& rs = ch.messages[idx].reactions;
                    std::string_view pick = em[r3 % em.size()];
                    auto rit = std::find_if(rs.begin(), rs.end(),
                        [&](const Reaction& x) { return x.emoji == pick; });
                    if (rit == rs.end()) rs.push_back({std::string{pick}, 1});
                    else                 rit->count += 1;
                }
            }
        }
    }

    // -- Channel filter for jumper -----------------------------------------

    static std::vector<int> jumper_matches(const Model& m) {
        std::vector<int> out;
        std::string needle = m.jumper_filter;
        std::transform(needle.begin(), needle.end(), needle.begin(),
            [](unsigned char c) { return std::tolower(c); });
        for (size_t i = 0; i < m.channels.size(); ++i) {
            std::string hay = m.channels[i].name;
            std::transform(hay.begin(), hay.end(), hay.begin(),
                [](unsigned char c) { return std::tolower(c); });
            if (needle.empty() || hay.find(needle) != std::string::npos)
                out.push_back(int(i));
        }
        return out;
    }

    // -- update -------------------------------------------------------------

    static Cmd update(Model& m, Tick) {
                tick_bots(m, 0.05f);
                return {};
    }

    static Cmd update(Model& m, CharIn c) {
                if (m.composer.size() < 500) {
                    encode_utf8(c.cp, m.composer, m.cursor);
                    char32_t cp = c.cp;
                    int len = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
                    m.cursor += len;
                }
                return {};
    }

    static Cmd update(Model& m, Backspace) {
                if (m.cursor > 0) {
                    int p = utf8_prev(m.composer, m.cursor);
                    m.composer.erase(p, m.cursor - p);
                    m.cursor = p;
                }
                return {};
    }

    static Cmd update(Model& m, DeleteWord) {
                int p = prev_word(m.composer, m.cursor);
                m.composer.erase(p, m.cursor - p);
                m.cursor = p;
                return {};
    }

    static Cmd update(Model& m, DeleteToStart) {
                m.composer.erase(0, m.cursor);
                m.cursor = 0;
                return {};
    }

    static Cmd update(Model& m, DeleteToEnd) {
                m.composer.erase(m.cursor);
                return {};
    }

    static Cmd update(Model& m, CursorLeft) {
                m.cursor = utf8_prev(m.composer, m.cursor);
                return {};
    }

    static Cmd update(Model& m, CursorRight) {
                m.cursor = utf8_next(m.composer, m.cursor);
                return {};
    }

    static Cmd update(Model& m, CursorHome) {
                m.cursor = 0;
                return {};
    }

    static Cmd update(Model& m, CursorEnd) {
                m.cursor = int(m.composer.size());
                return {};
    }

    static Cmd update(Model& m, SendComposer) {
                auto& s = m.composer;
                while (!s.empty() && s.front() == ' ') s.erase(s.begin());
                while (!s.empty() && s.back()  == ' ') s.pop_back();
                if (s.empty()) return {};

                if (s == "/quit" || s == "/exit") {
                    return Cmd::quit(0);
                }

                if (s[0] == '/') {
                    run_command(m, s);
                } else {
                    deliver(m, m.active_channel, kYou, s);
                }
                s.clear();
                m.cursor = 0;
                m.msg_scroll.y = kBottomAnchor;
                return {};
    }

    static Cmd update(Model& m, NextChannel) {
                int n = int(m.channels.size());
                m.active_channel = (m.active_channel + 1) % n;
                m.channels[m.active_channel].unread = 0;
                m.msg_scroll.y = kBottomAnchor;
                return {};
    }

    static Cmd update(Model& m, PrevChannel) {
                int n = int(m.channels.size());
                m.active_channel = (m.active_channel - 1 + n) % n;
                m.channels[m.active_channel].unread = 0;
                m.msg_scroll.y = kBottomAnchor;
                return {};
    }

    static Cmd update(Model& m, ClearChannel) {
                auto& ch = m.channels[m.active_channel];
                ch.messages.clear();
                push_system(ch, "channel cleared", m.clock);
                m.msg_scroll.y = kBottomAnchor;
                return {};
    }

    static Cmd update(Model& m, ScrollUp) {
                m.msg_scroll.scroll_by(0, -1);
                return {};
    }

    static Cmd update(Model& m, ScrollDown) {
                m.msg_scroll.scroll_by(0, +1);
                return {};
    }

    static Cmd update(Model& m, ScrollPageUp) {
                int page = std::max(1, m.msg_scroll.viewport_bounds.h - 2);
                m.msg_scroll.scroll_by(0, -page);
                return {};
    }

    static Cmd update(Model& m, ScrollPageDn) {
                int page = std::max(1, m.msg_scroll.viewport_bounds.h - 2);
                m.msg_scroll.scroll_by(0, +page);
                return {};
    }

    static Cmd update(Model& m, ScrollLatest) {
                m.msg_scroll.y = kBottomAnchor;
                return {};
    }

    static Cmd update(Model& m, HelpScroll s) {
                m.help_scroll.scroll_by(0, s.dy);
                return {};
    }

    static Cmd update(Model& m, OpenJumper) {
                m.jumper_open   = true;
                m.jumper_filter.clear();
                m.jumper_index  = 0;
                return {};
    }

    static Cmd update(Model& m, CloseJumper) {
                m.jumper_open = false;
                return {};
    }

    static Cmd update(Model& m, JumperChar c) {
                if (m.jumper_filter.size() < 32) {
                    encode_utf8(c.cp, m.jumper_filter,
                                int(m.jumper_filter.size()));
                }
                m.jumper_index = 0;
                return {};
    }

    static Cmd update(Model& m, JumperBack) {
                if (!m.jumper_filter.empty()) {
                    int p = utf8_prev(m.jumper_filter,
                                      int(m.jumper_filter.size()));
                    m.jumper_filter.erase(p);
                }
                m.jumper_index = 0;
                return {};
    }

    static Cmd update(Model& m, JumperUp) {
                m.jumper_index = std::max(0, m.jumper_index - 1);
                return {};
    }

    static Cmd update(Model& m, JumperDown) {
                int n = int(jumper_matches(m).size());
                m.jumper_index = std::min(std::max(0, n - 1),
                                          m.jumper_index + 1);
                return {};
    }

    static Cmd update(Model& m, JumperPick) {
                auto matches = jumper_matches(m);
                if (!matches.empty()) {
                    int idx = std::clamp(m.jumper_index, 0,
                                         int(matches.size()) - 1);
                    m.active_channel = matches[size_t(idx)];
                    m.channels[m.active_channel].unread = 0;
                    m.msg_scroll.y = kBottomAnchor;
                }
                m.jumper_open = false;
                return {};
    }

    static Cmd update(Model& m, OpenHelp)  { m.help_open = true;  return {}; }
    static Cmd update(Model& m, CloseHelp) { m.help_open = false; return {}; }
    static Cmd update(Model& m, Resize r) {
                m.term_w = r.w;
                m.term_h = r.h;
                return {};
    }

    static Cmd update(Model& m, RawMouse rm) {
                const MouseEvent& me = rm.ev;
                int mx = me.x.value - 1;
                int my = me.y.value - 1;

                // ── 1) Continue an in-progress drag on the messages bar ──
                if (m.msg_scroll.is_dragging()) {
                    (void)m.msg_scroll.handle(me);
                    return {};
                }

                // ── 2) Wheel — route to the panel under the cursor ──────
                if (me.button == MouseButton::ScrollUp ||
                    me.button == MouseButton::ScrollDown) {
                    int dy = (me.button == MouseButton::ScrollUp) ? -1 : +1;
                    if (m.help_open) {
                        if (m.help_scroll.viewport_bounds.contains(mx, my))
                            m.help_scroll.scroll_by(0, dy);
                    } else if (m.tabs_scroll.viewport_bounds.contains(mx, my)) {
                        // Wheel over the tab row → scroll tabs horizontally.
                        m.tabs_scroll.scroll_by(dy * 3, 0);
                    } else if (m.chats_scroll.viewport_bounds.contains(mx, my)) {
                        m.chats_scroll.scroll_by(0, dy);
                    } else if (m.members_scroll.viewport_bounds.contains(mx, my)) {
                        m.members_scroll.scroll_by(0, dy);
                    } else if (m.msg_scroll.viewport_bounds.contains(mx, my)) {
                        m.msg_scroll.scroll_by(0, dy);
                    }
                    return {};
                }

                // ── 3) Press on a scrollbar → start drag (handled by state) ─
                if (me.kind == MouseEventKind::Press
                    && me.button == MouseButton::Left) {

                    if (m.msg_scroll.bar_v_bounds.contains(mx, my)) {
                        (void)m.msg_scroll.handle(me);
                        return {};
                    }

                    // ── 3a) Click ✕ in top-right of the right panel → close
                    if (m.right_panel_open) {
                        const auto& rvb = m.members_scroll.viewport_bounds;
                        if (!rvb.empty()
                            && my >= rvb.y && my <= rvb.y + 2
                            && mx >= rvb.x + rvb.w - 4
                            && mx <= rvb.x + rvb.w) {
                            m.right_panel_open = false;
                            return {};
                        }
                    }

                    // ── 3b) Click on the chat header → open the right panel
                    {
                        bool show_right = m.right_panel_open && m.term_w >= 110;
                        int chats_w = std::clamp(
                            m.term_w * 28 / 100, 24, 34) + 1;
                        int right_w = show_right
                            ? std::clamp(m.term_w * 28 / 100, 26, 34) + 1
                            : 0;
                        int middle_left  = chats_w;
                        int middle_right = m.term_w - right_w;
                        // Only enable click-to-open when there's room.
                        if (!show_right && m.term_w >= 110
                            && my < 2 && mx >= middle_left && mx < middle_right) {
                            m.right_panel_open = true;
                            return {};
                        }
                    }

                    // ── 4a) Click on a member → open DM with that user ──
                    const auto& mvb = m.members_scroll.viewport_bounds;
                    if (mvb.contains(mx, my)) {
                        int virt_y = (my - mvb.y) + m.members_scroll.y;
                        int y = virt_y - 2;
                        for (size_t i = 0; i < m.users.size(); ++i) {
                            if (y >= 0 && y < 3) {
                                if (int(i) != kYou) {
                                    for (size_t ci = 0; ci < m.channels.size(); ++ci) {
                                        if (is_dm(m.channels[ci])
                                            && m.channels[ci].name == m.users[i].name) {
                                            m.active_channel = int(ci);
                                            m.channels[ci].unread = 0;
                                            m.msg_scroll.y = kBottomAnchor;
                                            return {};
                                        }
                                    }
                                }
                                break;
                            }
                            y -= 3;
                        }
                    }

                    // ── 4b) Click on a chat row → switch channel ──
                    const auto& vb = m.chats_scroll.viewport_bounds;
                    if (vb.contains(mx, my)) {
                        int virt_y = (my - vb.y) + m.chats_scroll.y;

                        // Layout per build_channel_list, in vertical rows:
                        //   0-1   Search box (rounded — 1 content row,
                        //         2 with borders) + blank
                        //   4-6   Saved Messages (2 lines + blank)
                        //   7-8   " CHATS" + blank
                        //   then 3 rows per chat
                        //   then " DIRECT MESSAGES" + blank
                        //   then 3 rows per DM
                        int y = virt_y - 9;
                        for (size_t i = 0; i < m.channels.size(); ++i) {
                            if (is_dm(m.channels[i])) continue;
                            if (y >= 0 && y < 3) {
                                m.active_channel = int(i);
                                m.channels[i].unread = 0;
                                m.msg_scroll.y = kBottomAnchor;
                                return {};
                            }
                            y -= 3;
                        }
                        y -= 2;
                        for (size_t i = 0; i < m.channels.size(); ++i) {
                            if (!is_dm(m.channels[i])) continue;
                            if (y >= 0 && y < 3) {
                                m.active_channel = int(i);
                                m.channels[i].unread = 0;
                                m.msg_scroll.y = kBottomAnchor;
                                return {};
                            }
                            y -= 3;
                        }
                    }
                }

                // ── 5) Release ends drag ─────────────────────────────────
                if (me.kind == MouseEventKind::Release) {
                    (void)m.msg_scroll.handle(me);
                }
                return {};
    }

    static Cmd update(Model& m, ToggleRightPanel) {
                m.right_panel_open = !m.right_panel_open;
                return {};
    }

    static Cmd update(Model&, Quit) { return Cmd::quit(0); }

    // ─── View builders ─────────────────────────────────────────────────────

    // Live width of the messages column (after chats panel and right
    // panel are deducted). Used so bubble widths stay proportional to
    // the pane the user actually sees, not the full terminal.
    static int middle_width(const Model& m) {
        int chats_w = std::clamp(m.term_w * 28 / 100, 24, 34);
        bool show_right = m.right_panel_open && m.term_w >= 110;
        int right_w = show_right
            ? std::clamp(m.term_w * 28 / 100, 26, 34)
            : 0;
        int dividers = (right_w > 0) ? 2 : 1;
        return std::max(20, m.term_w - chats_w - right_w - dividers);
    }

    // Build a reactions string like `[+1 3]  [♥ 1]  [▲ 2]`.
    static std::string reactions_chips(const std::vector<Reaction>& rs) {
        std::string out;
        bool first = true;
        for (const auto& rx : rs) {
            if (!first) out += " ";
            first = false;
            out += "[" + emoji_glyph(rx.emoji) + " "
                 + std::to_string(rx.count) + "]";
        }
        return out;
    }

    // DEBUG: simplest possible own message — just a colored text line.
    // If this appears after sending, the dispatch path works and the
    // bubble layout is what's broken. If it doesn't appear, deliver()
    // / SendComposer isn't actually adding the message.
    static Element build_own_message_debug(const Model& m, const Message& msg) {
        return text(">>> ME: " + msg.body
                  + "  (" + fmt_age(m.clock, msg.timestamp) + ")",
            Style{}.with_fg(pal::accent).with_bold());
    }

    // Own message — Telegram-mobile sent shape but TUI:
    //   - right-aligned bubble (rounded border, no fill)
    //   - cyan accent border so it reads as "mine" at a glance
    //   - time + ✓ / ✓✓ status checks inline at bottom-right inside
    static Element build_own_message(const Model& m, const Message& msg) {
        int age = int(m.clock - msg.timestamp);
        const char* check = (age >= 2) ? "\xe2\x9c\x93\xe2\x9c\x93"
                                       : "\xe2\x9c\x93";
        Color check_c = (age >= 5) ? pal::accent
                      : (age >= 2) ? pal::muted
                      :              pal::dim;

        Style body_s = Style{}.with_fg(pal::text);
        if (msg.mentions_you) body_s = body_s.with_bold();

        int mw = middle_width(m);
        int bubble_w = std::max(16, std::min(mw - 6, mw * 55 / 100));
        Color border_c = msg.mentions_you ? pal::amber : pal::accent;

        std::vector<Element> rows;
        rows.push_back(text(msg.body, body_s));
        if (!msg.reactions.empty()) {
            rows.push_back(text(reactions_chips(msg.reactions),
                Style{}.with_fg(pal::amber).with_dim()));
        }
        rows.push_back(hstack()
            .width(Dimension::percent(100))
            .justify(Justify::End)
            (text(fmt_age(m.clock, msg.timestamp),
                  Style{}.with_fg(pal::dim)),
             text(std::string{"  "}),
             text(std::string{check},
                  Style{}.with_fg(check_c).with_bold())));

        Element bubble = vstack()
            .border(BorderStyle::Round, border_c)
            .padding(0, 1)
            .width(Dimension::fixed(bubble_w))   // FIXED, not max — for deterministic positioning
            (std::move(rows));

        // Flex-based right-align kept collapsing the bubble to zero
        // width. Bypass flex entirely with explicit space-padding:
        // pre-pad the row with the exact number of spaces needed to
        // shove the bubble against the right edge. Boring but works.
        int left_pad = std::max(0, mw - bubble_w - 6);  // 6 = messages_inner padding(1,2) × 2 + scrollbar + margin
        std::string pad(static_cast<size_t>(left_pad), ' ');
        return h(text(pad), bubble).build();
    }

    // Peer message — Telegram-mobile received shape but TUI:
    //   - left-aligned bubble (rounded border, no fill)
    //   - muted gray border (visible against terminal bg, doesn't shout)
    //   - sender name colored at the top of the bubble in group chats
    //   - time at bottom-right inside the bubble
    static Element build_peer_message(const Model& m, const Message& msg,
                                      bool compact) {
        const User& u   = m.users[msg.author_uid];
        bool group_chat = !is_dm(m.channels[m.active_channel]);

        Style body_s = Style{}.with_fg(pal::text);
        if (msg.mentions_you) body_s = body_s.with_bold();

        // Peer bubble caps at ~65% of actual middle-column width.
        int mw = middle_width(m);
        int   bubble_w = std::max(16, std::min(mw - 6, mw * 65 / 100));
        Color border_c = msg.mentions_you ? pal::amber : pal::muted;

        std::vector<Element> rows;
        if (!compact && group_chat) {
            rows.push_back(text(u.name,
                Style{}.with_fg(u.color).with_bold()));
        }
        rows.push_back(text(msg.body, body_s));
        if (!msg.reactions.empty()) {
            rows.push_back(text(reactions_chips(msg.reactions),
                Style{}.with_fg(pal::amber).with_dim()));
        }
        rows.push_back(hstack()
            .width(Dimension::percent(100))
            .justify(Justify::End)
            (text(fmt_age(m.clock, msg.timestamp),
                  Style{}.with_fg(pal::dim))));

        Element bubble = vstack()
            .border(BorderStyle::Round, border_c)
            .padding(0, 1)
            .max_width(Dimension::fixed(bubble_w))
            (std::move(rows));

        // h(bubble, spacer) — bubble on the left, spacer fills the rest.
        return h(bubble, spacer()).build();
    }

    static Element build_message(const Model& m, const Message& msg,
                                 bool compact) {
        if (msg.author_uid < 0) {
            // System lines: centered, dim, italic
            return h(
                spacer(),
                text(std::string{"\xe2\x80\xa2 "} + msg.body
                     + std::string{" \xe2\x80\xa2"},
                     Style{}.with_fg(pal::dim).with_italic()),
                spacer()
            ).build();
        }
        if (msg.is_action) {
            const User& u = m.users[msg.author_uid];
            return h(
                text(std::string{"  * "}, Style{}.with_fg(pal::muted)),
                text(u.name + " " + msg.body,
                     Style{}.with_fg(u.color).with_italic())
            ).build();
        }
        if (msg.author_uid == kYou) return build_own_message(m, msg);
        return build_peer_message(m, msg, compact);
    }

    static Element build_gap_separator(float ts_a, float ts_b) {
        int gap_s = int(ts_b - ts_a);
        std::string label;
        if (gap_s < 60)       label = std::to_string(gap_s) + "s later";
        else if (gap_s < 3600) label = std::to_string(gap_s / 60) + "m later";
        else                   label = std::to_string(gap_s / 3600) + "h later";

        return h(
            text(std::string{"\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80 "},  // ───
                 Style{}.with_fg(pal::dim)),
            text(label, Style{}.with_fg(pal::muted).with_dim()),
            text(std::string{" \xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"},  // ───
                 Style{}.with_fg(pal::dim))
        ).build();
    }

    // Renders one chat-list row (2 lines) into `rows`.
    static void append_chat_row(std::vector<Element>& rows,
                                const Model& m, int i) {
        const auto& ch = m.channels[size_t(i)];
        bool active = i == m.active_channel;
        bool flash  = ch.flash_until > m.clock;
        bool dm     = is_dm(ch);

        const Message* last = nullptr;
        for (auto it = ch.messages.rbegin(); it != ch.messages.rend(); ++it) {
            if (it->author_uid >= 0) { last = &*it; break; }
        }

        Style name_s = Style{}.with_fg(active ? pal::accent : pal::text);
        if (active || flash) name_s = name_s.with_bold();
        if (flash)           name_s = Style{}.with_fg(pal::amber).with_bold();

        // Active selection: a colored left bar that runs down BOTH rows
        // of the entry. For inactive rows, two plain spaces in its place
        // so widths stay identical between selected and unselected items.
        Element bar = active
            ? Element{text(std::string{gl::bar},
                      Style{}.with_fg(pal::accent).with_bold())}
            : Element{text(std::string{" "})};

        Element selector = active
            ? Element{text(std::string{" "} + gl::arrow,
                      Style{}.with_fg(pal::accent).with_bold())}
            : Element{text(std::string{"  "})};

        Color av_c = dm
            ? (dm_uid(m, ch.name) >= 0
                ? m.users[size_t(dm_uid(m, ch.name))].color
                : pal::muted)
            : channel_color(i);

        // Empty string when no messages — the lonely "—" placeholder
        // emphasised the empty middle space; cleaner to just omit it.
        std::string age = last ? fmt_age(m.clock, last->timestamp)
                               : std::string{};

        // For DMs, a small online dot next to the partner's name —
        // green if active, dim if otherwise. Channels just get blank.
        Element online_dot = text(std::string{""});
        if (dm) {
            int pid = dm_uid(m, ch.name);
            if (pid >= 0) {
                bool online = m.users[size_t(pid)].presence
                              == Presence::Active;
                online_dot = text(
                    std::string{" "} + gl::dot_on,
                    Style{}.with_fg(online ? pal::green : pal::dim));
            }
        }

        // Row 1: bar · selector · avatar · name (· online dot) ─── age
        rows.push_back(hstack()
            .width(Dimension::percent(100))
            .align_items(Align::Start)
            (bar,
             selector,
             text(std::string{" "}),
             avatar(ch.name, av_c),
             text(std::string{"  "}),
             text(ch.name, name_s),
             online_dot,
             spacer(),
             text(age, Style{}.with_fg(pal::dim)),
             text(std::string{" "})));

        std::string preview;
        Style       preview_s = Style{}.with_fg(pal::muted);
        if (last) {
            std::string body;
            if (last->is_action) {
                body = "* " + m.users[size_t(last->author_uid)].name
                     + " " + last->body;
            } else if (dm) {
                body = (last->author_uid == kYou ? "you: " : "")
                     + last->body;
            } else {
                body = m.users[size_t(last->author_uid)].name
                     + ": " + last->body;
            }
            // Dynamic max width: panel(28..36) minus bar(1)+pad(2)
            // +scrollbar(1)+indent(9)+badge area(5). Keeps preview
            // single-line on every terminal size.
            int panel_w = std::clamp(m.term_w * 30 / 100, 28, 36);
            int max_preview = std::max(8, panel_w - 18);
            preview = truncate(std::move(body), size_t(max_preview));
            if (last->mentions_you) preview_s = Style{}.with_fg(pal::amber);
        } else {
            preview = "no messages yet";
            preview_s = preview_s.with_italic();
        }

        Element badge;
        if (flash) {
            // Plain ASCII "!" — wide-emoji ❗ renders as 2 cells in some
            // terminals and overflows the row.
            badge = text(std::string{" !"},
                Style{}.with_fg(pal::amber).with_bold());
        } else if (ch.unread > 0) {
            badge = text(" " + circled_digit(ch.unread),
                Style{}.with_fg(pal::amber).with_bold());
        } else {
            badge = text(std::string{""});
        }

        // Row 2: bar · indent · preview ─── unread badge
        rows.push_back(hstack()
            .width(Dimension::percent(100))
            (bar,
             text(std::string{"         "}),
             text(preview, preview_s),
             spacer(),
             badge,
             text(std::string{" "})));

        rows.push_back(text(std::string{""}));
    }

    static Element build_channel_list(const Model& m) {
        std::vector<Element> rows;

        // ── Search bar — full-width rounded input box ─────────────────
        rows.push_back(hstack()
            .width(Dimension::percent(100))
            .border(BorderStyle::Round, pal::muted)
            .padding(0, 1)
            (text(std::string{"\xf0\x9f\x94\x8d  "},  // 🔍
                  Style{}.with_fg(pal::muted)),
             text(std::string{"Search"},
                  Style{}.with_fg(pal::dim).with_italic()),
             spacer()));
        rows.push_back(text(std::string{""}));

        // ── Saved Messages — pinned at the very top, Telegram-style ──
        // The 4-cell prefix (" " + "  " + " ") mirrors the bar + selector
        // + gap of regular chat rows so the SM avatar lines up vertically
        // with the GE / DE / RA channel avatars below it.
        rows.push_back(hstack()
            .width(Dimension::percent(100))
            .align_items(Align::Start)
            (text(std::string{" "}),     // empty selection bar
             text(std::string{"  "}),    // selector placeholder
             text(std::string{" "}),     // gap before avatar
             avatar("SM", pal::accent),
             text(std::string{"  "}),
             text(std::string{"Saved Messages"},
                  Style{}.with_fg(pal::text).with_bold()),
             spacer(),
             text(std::string{"\xf0\x9f\x94\x96 "},  // 🔖
                  Style{}.with_fg(pal::accent))));
        rows.push_back(hstack()
            .width(Dimension::percent(100))
            (text(std::string{" "}),
             text(std::string{"         "}),   // 9 spaces — matches chat row indent
             text(std::string{"your notes & links"},
                  Style{}.with_fg(pal::muted).with_italic())));
        rows.push_back(text(std::string{""}));

        // Section-header builder — small em-dash prefix + bold label,
        // for a sliver of visual interest without competing with chats.
        auto section = [](std::string label) {
            return h(
                text(std::string{" \xe2\x80\x94 "},   // " — "
                     Style{}.with_fg(pal::dim)),
                text(std::move(label),
                     Style{}.with_fg(pal::muted).with_bold())
            ).build();
        };

        // ── CHATS (group channels) ────────────────────────────────────
        rows.push_back(section("CHATS"));
        rows.push_back(text(std::string{""}));
        for (size_t i = 0; i < m.channels.size(); ++i) {
            if (is_dm(m.channels[i])) continue;
            append_chat_row(rows, m, int(i));
        }

        // ── DIRECT MESSAGES ───────────────────────────────────────────
        rows.push_back(section("DIRECT MESSAGES"));
        rows.push_back(text(std::string{""}));
        for (size_t i = 0; i < m.channels.size(); ++i) {
            if (!is_dm(m.channels[i])) continue;
            append_chat_row(rows, m, int(i));
        }

        // ── SHORTCUTS ─────────────────────────────────────────────────
        rows.push_back(section("SHORTCUTS"));
        rows.push_back(text(std::string{""}));
        auto hint = [](std::string kk, std::string desc) {
            return h(
                text(" " + kk, Style{}.with_fg(pal::text)),
                text(std::string{"  "}),
                spacer(),
                text(desc, Style{}.with_fg(pal::muted))
            ).build();
        };
        rows.push_back(hint("tab",  "next"));
        rows.push_back(hint("^G",   "jump"));
        rows.push_back(hint("^H",   "help"));
        rows.push_back(hint("^L",   "clear"));
        rows.push_back(hint("^P",   "panel"));
        rows.push_back(hint("q",    "quit"));
        rows.push_back(text(std::string{""}));
        rows.push_back(text(std::string{" click a chat to switch"},
            Style{}.with_fg(pal::dim).with_italic()));
        rows.push_back(text(std::string{" wheel to scroll"},
            Style{}.with_fg(pal::dim).with_italic()));

        // padding(top, right, bottom, left) = 0 top so the search box
        // hugs the panel's top edge — was wasting a row above it.
        return (v(std::move(rows)) | padding(0, 1, 1, 1)).build();
    }

    // Right panel for DMs — Telegram's "User Info" card.
    // Big avatar, name, status, contact rows, then a tab bar at the
    // bottom (Stories · Media · Files · Links · Voice) with one tab
    // active and a placeholder grid below.
    static Element build_dm_info_panel(const Model& m) {
        const auto& ch  = m.channels[m.active_channel];
        int partner_uid = dm_uid(m, ch.name);
        if (partner_uid < 0) return text(std::string{""});

        const User& u = m.users[partner_uid];

        std::vector<Element> rows;

        // Title bar with close button
        rows.push_back(hstack()
            .width(Dimension::percent(100))
            (text(std::string{" USER INFO"},
                  Style{}.with_fg(pal::muted).with_bold()),
             spacer(),
             text(std::string{"\xe2\x9c\x95  "},   // ✕
                  Style{}.with_fg(pal::muted).with_bold())));
        rows.push_back(text(std::string{""}));

        // Big avatar — bordered round frame around the colored 2-letter
        // chip so it reads as a "profile picture" rather than a tab tag.
        rows.push_back(hstack()
            .width(Dimension::percent(100))
            .justify(Justify::Center)
            (vstack()
                .border(BorderStyle::Round, u.color)
                .padding(0, 2)
                (avatar(u.name, u.color))));
        rows.push_back(text(std::string{""}));

        // Name centered, bold + slightly larger feel via padding above.
        rows.push_back(hstack()
            .width(Dimension::percent(100))
            .justify(Justify::Center)
            (text(u.name, Style{}.with_fg(pal::text).with_bold())));

        // Status row — colored presence dot + label, centered.
        rows.push_back(hstack()
            .width(Dimension::percent(100))
            .justify(Justify::Center)
            (text(std::string{presence_dot(u.presence)} + " ",
                  Style{}.with_fg(presence_color(u.presence))),
             text(std::string{presence_label(u.presence)},
                  Style{}.with_fg(presence_color(u.presence)))));
        rows.push_back(text(std::string{""}));
        rows.push_back(text(std::string{""}));

        // Contact rows: icon-like glyph + label/value stack
        auto info_row = [](std::string icon, std::string label,
                           std::string value) {
            return h(
                text(" " + icon + "  ",
                     Style{}.with_fg(pal::muted)),
                v(
                    text(value, Style{}.with_fg(pal::text)),
                    text(label, Style{}.with_fg(pal::dim))
                )
            ).build();
        };
        rows.push_back(info_row("\xe2\x98\x8e",            // ☎
                                "Phone",
                                "+1 555 0100"));
        rows.push_back(text(std::string{""}));
        rows.push_back(info_row("@",
                                "Username",
                                "@" + u.name));
        rows.push_back(text(std::string{""}));
        rows.push_back(info_row("\xe2\x93\x98",            // ⓘ (circled i)
                                "Bio",
                                "engineer · gardener · runner"));
        rows.push_back(text(std::string{""}));

        // Notifications toggle row — bell icon + label + colored toggle.
        rows.push_back(hstack()
            .width(Dimension::percent(100))
            (text(std::string{" \xe2\x9a\x91  "},          // ⚑ (flag-like bell)
                  Style{}.with_fg(pal::muted)),
             text(std::string{"Notifications"},
                  Style{}.with_fg(pal::text)),
             spacer(),
             text(std::string{"on "},
                  Style{}.with_fg(pal::green).with_bold())));
        rows.push_back(text(std::string{""}));
        rows.push_back(text(std::string{""}));

        // Tab bar — Stories | Media | Files | Links | Voice — wrapped in
        // a horizontal-scroll viewport so it stays a single row even on
        // narrow panels. Wheel-scroll horizontally, or drag the x bar.
        auto tab = [](std::string label, bool active) {
            Style s = Style{}.with_fg(active ? pal::accent : pal::muted);
            if (active) s = s.with_bold();
            return text(label, s);
        };
        auto& mut_tabs = const_cast<ScrollState&>(m.tabs_scroll);

        Element tabs_row = h(
            text(std::string{" "}),
            tab("Stories", true),
            text(std::string{"  \xc2\xb7  "},
                 Style{}.with_fg(pal::dim)),
            tab("Media", false),
            text(std::string{"  \xc2\xb7  "},
                 Style{}.with_fg(pal::dim)),
            tab("Files", false),
            text(std::string{"  \xc2\xb7  "},
                 Style{}.with_fg(pal::dim)),
            tab("Links", false),
            text(std::string{"  \xc2\xb7  "},
                 Style{}.with_fg(pal::dim)),
            tab("Voice", false),
            text(std::string{" "})
        ).build();

        // Active-tab underline lives in the SAME scroll viewport as the
        // tabs row so it tracks the active tab horizontally — when the
        // user scrolls to "Files", "Stories" + its underline both move
        // off-screen together, just like Telegram's tab strip.
        // 1-space lead + 7 heavy dashes ("Stories" is 7 chars).
        auto underline = text(
            std::string{" \xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81"},  // ━━━━━━━
            Style{}.with_fg(pal::accent).with_bold());

        int tabs_view_w = std::max(12, m.term_w * 30 / 100 - 4);
        rows.push_back(v(tabs_row, underline) | scrollx(mut_tabs, tabs_view_w));

        // Neon horizontal scrollbar sized to match the viewport.
        rows.push_back(scrollbar_x(m.tabs_scroll, tabs_view_w,
            ScrollbarStyle::neon()));
        rows.push_back(text(std::string{""}));

        // Stories grid placeholder — a small mock of 4 thumbnails as
        // bordered cells.
        for (int row = 0; row < 2; ++row) {
            std::vector<Element> cells;
            for (int col = 0; col < 2; ++col) {
                Element cell = vstack()
                    .border(BorderStyle::Round, pal::muted)
                    .padding(0, 1)
                    (text(std::string{"\xe2\x97\x86 "} + std::to_string(row * 2 + col + 1) + ":0" + (row == 0 ? "5" : "8"),
                          Style{}.with_fg(pal::dim)));
                cells.push_back(cell);
                if (col == 0) cells.push_back(text(std::string{" "}));
            }
            rows.push_back(h(std::move(cells)).build());
            rows.push_back(text(std::string{""}));
        }

        return (v(std::move(rows)) | padding(1)).build();
    }

    static Element build_member_list(const Model& m) {
        const auto& ch = m.channels[m.active_channel];
        Color av_c = channel_color(m.active_channel);

        std::vector<Element> rows;

        // ── Title bar with close button (same shape as USER INFO) ──
        rows.push_back(hstack()
            .width(Dimension::percent(100))
            (text(std::string{" \xe2\x80\x94 CHANNEL INFO"},
                  Style{}.with_fg(pal::muted).with_bold()),
             spacer(),
             text(std::string{"\xe2\x9c\x95  "},          // ✕
                  Style{}.with_fg(pal::muted).with_bold())));
        rows.push_back(text(std::string{""}));

        // ── Bordered avatar — channel "profile picture" ──
        rows.push_back(hstack()
            .width(Dimension::percent(100))
            .justify(Justify::Center)
            (vstack()
                .border(BorderStyle::Round, av_c)
                .padding(0, 2)
                (avatar(ch.name, av_c))));
        rows.push_back(text(std::string{""}));

        // ── Channel name (bold) ──
        rows.push_back(hstack()
            .width(Dimension::percent(100))
            .justify(Justify::Center)
            (text(ch.name, Style{}.with_fg(pal::text).with_bold())));

        // ── Topic (dim italic) ──
        rows.push_back(hstack()
            .width(Dimension::percent(100))
            .justify(Justify::Center)
            (text(ch.topic,
                  Style{}.with_fg(pal::muted).with_italic())));
        rows.push_back(text(std::string{""}));

        // ── N online · K total ──
        int total  = int(m.users.size());
        int online = int(std::count_if(m.users.begin(), m.users.end(),
            [](const User& u) { return u.presence == Presence::Active; }));
        rows.push_back(hstack()
            .width(Dimension::percent(100))
            .justify(Justify::Center)
            (text(std::to_string(online) + " online  \xc2\xb7  "
                  + std::to_string(total) + " total",
                  Style{}.with_fg(pal::dim).with_italic())));
        rows.push_back(text(std::string{""}));
        rows.push_back(text(std::string{""}));

        // ── — MEMBERS section ──
        rows.push_back(text(std::string{" \xe2\x80\x94 MEMBERS"},
            Style{}.with_fg(pal::muted).with_bold()));
        rows.push_back(text(std::string{""}));

        // Member rows: avatar + (name / status) + presence dot on right
        for (size_t i = 0; i < m.users.size(); ++i) {
            const auto& u = m.users[i];
            bool typing_now = std::any_of(m.typing.begin(), m.typing.end(),
                [&](const TypingPulse& t) { return t.uid == int(i); });
            bool is_self = (int(i) == kYou);

            std::string status_text = typing_now
                ? std::string{"typing\xe2\x80\xa6"}             // typing…
                : (is_self
                    ? std::string{presence_label(u.presence)} + " (you)"
                    : std::string{presence_label(u.presence)});
            Style status_style = typing_now
                ? Style{}.with_fg(pal::amber).with_italic()
                : Style{}.with_fg(presence_color(u.presence)).with_dim();

            rows.push_back(hstack()
                .width(Dimension::percent(100))
                .align_items(Align::Start)
                (text(std::string{" "}),
                 avatar(u.name, u.color),
                 text(std::string{"  "}),
                 v(
                    text(u.name, Style{}.with_fg(pal::text).with_bold()),
                    text(status_text, status_style)
                 ),
                 spacer(),
                 text(std::string{presence_dot(u.presence)},
                      Style{}.with_fg(presence_color(u.presence))),
                 text(std::string{" "})));
            rows.push_back(text(std::string{""}));
        }

        // ── — ACTIONS section ──
        rows.push_back(text(std::string{" \xe2\x80\x94 ACTIONS"},
            Style{}.with_fg(pal::muted).with_bold()));
        rows.push_back(text(std::string{""}));

        auto action_row = [](std::string icon, std::string label, Color icon_c) {
            return h(
                text("  " + icon + "  ",
                     Style{}.with_fg(icon_c).with_bold()),
                text(std::move(label),
                     Style{}.with_fg(pal::text))
            ).build();
        };
        rows.push_back(action_row("+",                  "invite someone",   pal::accent));
        rows.push_back(action_row("\xe2\x9a\x99",       "channel settings", pal::muted));   // ⚙
        rows.push_back(action_row("\xf0\x9f\x94\x8d",   "search members",   pal::muted));   // 🔍
        rows.push_back(text(std::string{""}));

        rows.push_back(text(std::string{" /status to change yours"},
            Style{}.with_fg(pal::dim).with_italic()));

        return (v(std::move(rows)) | padding(1)).build();
    }

    static Element build_typing_indicator(const Model& m) {
        std::vector<std::string> names;
        for (const auto& t : m.typing) {
            if (t.channel == m.active_channel && t.uid != kYou)
                names.push_back(m.users[t.uid].name);
        }
        if (names.empty()) return text(std::string{""});

        std::string s;
        if (names.size() == 1)      s = names[0] + " is typing";
        else if (names.size() == 2) s = names[0] + " and " + names[1] + " are typing";
        else                        s = std::to_string(names.size()) + " people are typing";

        static constexpr const char* dots[4] = { "   ", ".  ", ".. ", "..." };
        s += dots[(m.tick_n / 6) % 4];

        return h(
            text(std::string{"  "} + gl::ellipsis + "  ",
                 Style{}.with_fg(pal::amber)),
            text(s, Style{}.with_fg(pal::muted).with_italic())
        ).build();
    }

    static Element build_empty_hint(const Channel& ch) {
        std::vector<Element> rows;
        rows.push_back(text(std::string{""}));
        rows.push_back(text(std::string{""}));
        rows.push_back(text(std::string{"  "} + gl::spark + "  "
                            + ch.name + " is quiet right now",
            Style{}.with_fg(pal::muted)));
        rows.push_back(text(std::string{""}));
        rows.push_back(text(std::string{"  say hi, or try /help for commands"},
            Style{}.with_fg(pal::dim).with_italic()));
        return v(std::move(rows)).build();
    }

    // Inner content: every message in the active channel, no windowing
    // (the scroll widget clips at paint time, the scrollbar tracks position).
    static Element build_messages_inner(const Model& m) {
        const auto& ch = m.channels[m.active_channel];

        std::vector<Element> rows;

        // ── Telegram-style "── Today ──" date pill at the top ──
        rows.push_back(hstack()
            .width(Dimension::percent(100))
            .justify(Justify::Center)
            (text(std::string{" \xe2\x94\x80\xe2\x94\x80 Today \xe2\x94\x80\xe2\x94\x80 "},
                  Style{}.with_fg(pal::muted).with_bold())));
        rows.push_back(text(std::string{""}));

        bool only_system = std::all_of(ch.messages.begin(), ch.messages.end(),
            [](const Message& mm) { return mm.author_uid < 0; });
        if (only_system) {
            rows.push_back(build_empty_hint(ch));
        }

        int  prev_author = -2;
        float prev_ts    = -1000.f;
        for (size_t i = 0; i < ch.messages.size(); ++i) {
            const Message& msg = ch.messages[i];

            if (prev_ts > -100.f && msg.timestamp >= 0.f
                && (msg.timestamp - prev_ts) > 300.f) {
                rows.push_back(text(std::string{""}));
                rows.push_back(build_gap_separator(prev_ts, msg.timestamp));
                prev_author = -2;
            }

            bool compact = (prev_author == msg.author_uid)
                        && (msg.timestamp - prev_ts < 60.f)
                        && msg.author_uid >= 0
                        && !msg.is_action;

            // Always breathe between messages so reactions/status don't
            // visually run into the next body. Compact mode still skips
            // the avatar/name re-render to keep bursts grouped, but the
            // inter-message blank stays so each message reads cleanly.
            if (prev_author != -2)
                rows.push_back(text(std::string{""}));

            rows.push_back(build_message(m, msg, compact));

            prev_author = msg.is_action ? -3 : msg.author_uid;
            prev_ts     = msg.timestamp;
        }

        return vstack()
            .grow(1).shrink(1)
            .padding(1, 2)
            (std::move(rows));
    }

    static Element build_header(const Model& m) {
        const auto& ch = m.channels[m.active_channel];
        const User& self = m.users[kYou];

        int active_n = int(std::count_if(m.users.begin(), m.users.end(),
            [](const User& u) { return u.presence == Presence::Active; }));

        int pending_mentions = 0;
        int pending_unread   = 0;
        for (size_t i = 0; i < m.channels.size(); ++i) {
            if (int(i) == m.active_channel) continue;
            if (m.channels[i].flash_until > m.clock) ++pending_mentions;
            pending_unread += m.channels[i].unread;
        }

        int  sec = std::max(0, int(m.clock));
        char clock_buf[16];
        std::snprintf(clock_buf, sizeof clock_buf,
                      sec >= 3600 ? "%d:%02d:%02d" : "%d:%02d",
                      sec >= 3600 ? sec / 3600 : sec / 60,
                      sec >= 3600 ? (sec / 60) % 60 : sec % 60,
                      sec % 60);

        auto dot = []() {
            return text(std::string{"  \xc2\xb7  "},
                        Style{}.with_fg(pal::dim));
        };

        bool dm = is_dm(ch);
        int  partner_uid = dm ? dm_uid(m, ch.name) : -1;

        // Subtitle — kept compact so it doesn't wrap on narrow terminals.
        std::string subtitle;
        Style       subtitle_s = Style{}.with_fg(pal::muted);

        if (dm && partner_uid >= 0) {
            // DM: just the partner's presence label.
            subtitle = presence_label(m.users[partner_uid].presence);
            subtitle_s = Style{}.with_fg(presence_color(
                m.users[partner_uid].presence)).with_dim();
        } else {
            // Channel: "K online · topic". Drop the redundant total count.
            subtitle = std::to_string(active_n) + " online  \xc2\xb7  "
                     + ch.topic;
        }

        Color av_c = (dm && partner_uid >= 0)
            ? m.users[partner_uid].color
            : channel_color(m.active_channel);

        Element left = hstack()
            .align_items(Align::Start)
            (text(std::string{" "}),
             avatar(ch.name, av_c),
             text(std::string{"  "}),
             vstack()
                (text(ch.name,
                      Style{}.with_fg(pal::accent).with_bold()),
                 text(subtitle, subtitle_s)));

        // ── Right cluster — top-aligned to header's first row ───────
        // Compact: ❶ for mentions (single cell), then presence dot +
        // clock. Two-space separators instead of " · " for breathing
        // room without visual chatter.
        std::vector<Element> right_items;
        if (pending_mentions > 0) {
            right_items.push_back(text(
                circled_digit(pending_mentions),
                Style{}.with_fg(pal::amber).with_bold()));
            right_items.push_back(text(std::string{"  "}));
        } else if (pending_unread > 0) {
            right_items.push_back(text(
                std::to_string(pending_unread),
                Style{}.with_fg(pal::amber).with_bold()));
            right_items.push_back(text(std::string{"  "}));
        }
        right_items.push_back(text(
            std::string{presence_dot(self.presence)},
            Style{}.with_fg(presence_color(self.presence))));
        right_items.push_back(text(std::string{"  "}));
        right_items.push_back(text(
            std::string{clock_buf},
            Style{}.with_fg(pal::dim)));

        Element right = hstack().align_items(Align::Start)
            (std::move(right_items));

        // Use builder all the way so width(100%) and padding actually
        // apply — piping `| padding(...)` onto a built Element doesn't
        // reliably set it. Without explicit width(100%), the hstack
        // sizes to content and spacer() has no room to grow.
        return hstack()
            .width(Dimension::percent(100))
            .align_items(Align::Start)
            .padding(0, 1)
            (left, spacer(), right);
    }

    static Element build_composer(const Model& m) {
        bool empty       = m.composer.empty();
        bool is_command  = !empty && m.composer[0] == '/';
        bool caret_on    = ((m.tick_n / 10) % 2) == 0;

        Style body_s = Style{}.with_fg(pal::text);
        if (is_command) body_s = Style{}.with_fg(pal::accent);

        Style caret_st = Style{}.with_fg(pal::accent).with_bold();
        const char* caret = caret_on ? "\xe2\x94\x82" : " ";  // │

        std::string count = std::to_string(m.composer.size()) + "/500";
        Color count_c     = m.composer.size() > 400 ? pal::amber : pal::dim;

        // Telegram composer icons: emoji on the left, attach + mic/send
        // on the right. When the user has typed anything, the right
        // glyph flips from mic 🎤 to send ⏎ — same affordance as mobile.
        Element emoji_btn = text(std::string{"\xf0\x9f\x98\x8a  "},  // 😊
            Style{}.with_fg(pal::muted));
        Element attach_btn = text(std::string{"\xf0\x9f\x93\x8e "},   // 📎
            Style{}.with_fg(pal::muted));
        Element right_btn = empty
            ? text(std::string{"\xf0\x9f\x8e\xa4 "},                  // 🎤
                   Style{}.with_fg(pal::muted))
            : text(std::string{" \xe2\x8f\x8e "},                     // ⏎
                   Style{}.with_fg(pal::accent).with_bold());

        std::vector<Element> body_parts;
        body_parts.push_back(emoji_btn);
        if (is_command) {
            body_parts.push_back(text(std::string{gl::prompt} + " ",
                Style{}.with_fg(pal::amber).with_bold()));
        }

        if (empty) {
            body_parts.push_back(text(std::string{caret}, caret_st));
            body_parts.push_back(text(std::string{" Message"},
                Style{}.with_fg(pal::dim).with_italic()));
        } else {
            int c = std::clamp(m.cursor, 0, int(m.composer.size()));
            body_parts.push_back(text(m.composer.substr(0, c),  body_s));
            body_parts.push_back(text(std::string{caret}, caret_st));
            body_parts.push_back(text(m.composer.substr(c), body_s));
        }
        body_parts.push_back(spacer());
        body_parts.push_back(text(count, Style{}.with_fg(count_c)));
        body_parts.push_back(text(std::string{"  "}));
        body_parts.push_back(attach_btn);
        body_parts.push_back(right_btn);

        return (h(std::move(body_parts)) | padding(0, 2)).build();
    }

    // ─── Overlays ──────────────────────────────────────────────────────────

    static Element build_jumper_overlay(const Model& m) {
        auto matches = jumper_matches(m);

        std::vector<Element> rows;
        rows.push_back(h(
            text(std::string{" "} + gl::arrow + "  go to channel  ",
                 Style{}.with_fg(pal::accent).with_bold()),
            text(m.jumper_filter,
                 Style{}.with_fg(pal::text)),
            text(((m.tick_n / 10) % 2) == 0 ? "_" : " ",
                 Style{}.with_fg(pal::accent)),
            spacer(),
            text(std::string{" "} + std::to_string(matches.size())
                  + " · enter pick · esc close",
                 Style{}.with_fg(pal::dim))
        ).build());
        rows.push_back(text(std::string{""}));

        if (matches.empty()) {
            rows.push_back(text(std::string{"  no channels match \""}
                                + m.jumper_filter + "\"",
                Style{}.with_fg(pal::muted).with_italic()));
        }

        for (size_t i = 0; i < matches.size(); ++i) {
            bool sel = int(i) == m.jumper_index;
            const auto& ch = m.channels[size_t(matches[i])];

            Element marker = sel
                ? Element{text(std::string{" "} + gl::arrow + " ",
                          Style{}.with_fg(pal::accent))}
                : Element{text(std::string{"   "})};

            Style name_s = Style{}.with_fg(sel ? pal::accent : pal::text);
            if (sel) name_s = name_s.with_bold();

            rows.push_back(h(
                marker,
                text(ch.name, name_s),
                text(std::string{"   "}),
                text(ch.topic, Style{}.with_fg(pal::muted).with_dim()),
                spacer(),
                ch.unread > 0
                    ? Element{text(" " + std::to_string(ch.unread),
                        Style{}.with_fg(pal::amber).with_bold())}
                    : Element{text(std::string{""})}
            ).build());
        }

        return (v(std::move(rows))
                | padding(1, 2)
                | border(BorderStyle::Round)
                | bcolor(pal::accent)).build();
    }

    static Element build_help_overlay(const Model&) {
        std::vector<Element> rows;
        rows.push_back(text(std::string{" "} + gl::spark
                            + "  maya/chat  ·  help",
            Style{}.with_fg(pal::accent).with_bold()));
        rows.push_back(text(std::string{""}));

        auto row = [](std::string kk, std::string desc) {
            return h(
                text("  " + kk, Style{}.with_fg(pal::amber)),
                text(std::string{"   "}),
                text(desc, Style{}.with_fg(pal::text))
            ).build();
        };
        auto section = [](std::string s) {
            return text("  " + s,
                Style{}.with_fg(pal::muted).with_bold());
        };

        rows.push_back(section("composer"));
        rows.push_back(row("←/→               ", "move cursor"));
        rows.push_back(row("^A   /   ^E       ", "jump to line start / end"));
        rows.push_back(row("^W   /   ^U   ^K  ", "delete prev word / clear to start / end"));
        rows.push_back(row("backspace         ", "delete previous char"));
        rows.push_back(row("enter             ", "send (or run /command)"));
        rows.push_back(text(std::string{""}));

        rows.push_back(section("navigation"));
        rows.push_back(row("tab / shift+tab   ", "next / previous channel"));
        rows.push_back(row("↑ ↓ PgUp PgDn end ", "scroll messages"));
        rows.push_back(row("^G                ", "channel jumper"));
        rows.push_back(row("^L                ", "clear current channel"));
        rows.push_back(row("^P                ", "toggle user info panel  (also: click ✕ to close, click header to open)"));
        rows.push_back(row("q / esc / ^C      ", "quit"));
        rows.push_back(text(std::string{""}));

        rows.push_back(section("slash commands"));
        rows.push_back(row("/help             ", "this screen"));
        rows.push_back(row("/me <action>      ", "/me waves   →   * you waves"));
        rows.push_back(row("/topic <text>     ", "retitle the current channel"));
        rows.push_back(row("/status <state>   ", "active | away | dnd | offline"));
        rows.push_back(row("/who              ", "list members"));
        rows.push_back(row("/clear            ", "clear current channel"));
        rows.push_back(row("/quit             ", "exit"));
        rows.push_back(text(std::string{""}));
        rows.push_back(text(std::string{"  press any key to close"},
            Style{}.with_fg(pal::dim).with_italic()));

        return (v(std::move(rows))
                | padding(1, 2)
                | border(BorderStyle::Round)
                | bcolor(pal::accent)).build();
    }

    // ─── view ──────────────────────────────────────────────────────────────

    // Thin single-line dividers in dim color. The framework's `sep`/`vsep`
    // draw both edges of a box (top+bottom or left+right) which produces a
    // double-line look; we want just one stroke.
    static Element hdiv() {
        return Element{box()
            .border(BorderStyle::Single, pal::dim)
            .border_sides({true, false, false, false})};
    }
    static Element vdiv() {
        return Element{box()
            .border(BorderStyle::Single, pal::dim)
            .border_sides({false, false, false, true})};
    }

    static Element view(const Model& m) {
        auto& mut_msg = const_cast<ScrollState&>(m.msg_scroll);

        // ── Independently responsive: fixed header & composer, grow messages ──
        //
        // Header and composer get fixed pixel heights so they can't
        // steal from each other or be pushed off-screen. Messages box
        // has grow(1) + shrink(1) + min_height(0), so it absorbs
        // whatever vertical space is actually allocated to the middle
        // column by the renderer — which IS the live terminal height,
        // independent of any stale m.term_h.
        //
        // overflow:Hidden everywhere so content past the box clips
        // cleanly instead of pushing siblings.

        constexpr int kHeaderH    = 3;   // 1 row top breathing + 2 rows content
        constexpr int kComposerH  = 1;

        Element header_box = vstack()
            .height(Dimension::fixed(kHeaderH))
            .padding(1, 0, 0, 0)   // 1-row top breathing space
            .grow(0).shrink(0)
            .overflow(Overflow::Hidden)
            (build_header(m));

        Element composer_box = vstack()
            .height(Dimension::fixed(kComposerH))
            .grow(0).shrink(0)
            .overflow(Overflow::Hidden)
            (build_composer(m));

        // Conversation pane: pipe pattern from scrollbar.hpp docs.
        // Element satisfies the Node concept (it has its own build()
        // method that returns *this), so `Element | scrolly | grow` is
        // valid and the framework's WrappedNode applies the scroll
        // properties at build time via as_box internally.
        int sb_h = std::max(4, m.term_h - 8);

        Element messages_box = vstack()
            .grow(1)
            .shrink(1)
            .min_height(Dimension::fixed(0))
            .overflow(Overflow::Hidden)
            (hstack()
                .grow(1)
                .shrink(1)
                (build_messages_inner(m)
                    | scrolly(mut_msg, 0)
                    | grow(1),
                 scrollbar_y(m.msg_scroll, sb_h, ScrollbarStyle::neon())),
             build_typing_indicator(m));

        Element middle = vstack()
            .grow(1)
            .shrink(1)
            (header_box,
             hdiv(),
             messages_box,
             hdiv(),
             composer_box);

        // Both sidebars wrapped in scrolly + neon scrollbar so wheel and
        // drag interactions work in them too.
        auto& mut_chats   = const_cast<ScrollState&>(m.chats_scroll);
        auto& mut_members = const_cast<ScrollState&>(m.members_scroll);
        int sidebar_h = std::max(4, m.term_h);

        // Percentage-based three-column layout (30% / 40% / 30%) with
        // sensible caps so wide terminals don't get huge sidebars.
        // Middle uses grow(1) instead of an explicit 40% so when the
        // right panel is closed it absorbs the freed space (becomes 70%).
        Element chats_panel = hstack()
            .width(Dimension::percent(28))
            .max_width(Dimension::fixed(34))
            .min_width(Dimension::fixed(24))
            .shrink(0)
            (build_channel_list(m) | scrolly(mut_chats, 0) | grow(1),
             scrollbar_y(m.chats_scroll, sidebar_h, ScrollbarStyle::neon()));

        // Right panel: DM "user info" card when in a DM, else members.
        // Auto-suppress on narrow terminals so the middle column always
        // has enough room for the header + composer (otherwise the
        // header content wraps into the unread-elsewhere cluster).
        bool show_right_panel = m.right_panel_open && m.term_w >= 110;

        std::vector<Element> row_children;
        row_children.push_back(chats_panel);
        row_children.push_back(vdiv());
        // middle already has grow(1) set on its FlexStyle via the builder
        // above, so no pipe needed here.
        row_children.push_back(middle);

        if (show_right_panel) {
            Element right_inner = is_dm(m.channels[m.active_channel])
                ? build_dm_info_panel(m)
                : build_member_list(m);
            Element members_panel = hstack()
                .width(Dimension::percent(28))
                .max_width(Dimension::fixed(34))
                .min_width(Dimension::fixed(26))
                .shrink(0)
                (right_inner | scrolly(mut_members, 0) | grow(1),
                 scrollbar_y(m.members_scroll, sidebar_h, ScrollbarStyle::neon()));

            row_children.push_back(vdiv());
            row_children.push_back(members_panel);
        }

        Element base = h(std::move(row_children)).build();

        if (m.help_open) {
            int help_h = std::max(8, m.term_h - 8);
            auto& mut_help = const_cast<ScrollState&>(m.help_scroll);
            Element help_inner = h(
                build_help_overlay(m) | scrolly(mut_help, 0) | grow(1),
                scrollbar_y(m.help_scroll, help_h,
                            ScrollbarStyle::neon())
            ).build();
            return Overlay{{
                .base    = std::move(base),
                .overlay = std::move(help_inner),
                .present = true,
            }}.build();
        }
        if (m.jumper_open) {
            return Overlay{{
                .base    = std::move(base),
                .overlay = build_jumper_overlay(m),
                .present = true,
            }}.build();
        }
        return base;
    }

    // ─── input / subscribe ─────────────────────────────────────────────────

    static std::optional<Msg> on(const Model& m, const KeyEvent& k) {
        // ── Help: arrows scroll, escape/other keys close ──
        if (m.help_open) {
            if (auto* sk = std::get_if<SpecialKey>(&k.key)) {
                switch (*sk) {
                    case SpecialKey::Up:       return Msg{HelpScroll{-1}};
                    case SpecialKey::Down:     return Msg{HelpScroll{+1}};
                    case SpecialKey::PageUp:   return Msg{HelpScroll{-6}};
                    case SpecialKey::PageDown: return Msg{HelpScroll{+6}};
                    case SpecialKey::Escape:   return Msg{CloseHelp{}};
                    default: break;
                }
            }
            return Msg{CloseHelp{}};
        }

        // ── Jumper: dedicated key handling ──
        if (m.jumper_open) {
            if (auto* sk = std::get_if<SpecialKey>(&k.key)) {
                switch (*sk) {
                    case SpecialKey::Escape:    return Msg{CloseJumper{}};
                    case SpecialKey::Enter:     return Msg{JumperPick{}};
                    case SpecialKey::Up:        return Msg{JumperUp{}};
                    case SpecialKey::Down:      return Msg{JumperDown{}};
                    case SpecialKey::Tab:       return Msg{JumperDown{}};
                    case SpecialKey::BackTab:   return Msg{JumperUp{}};
                    case SpecialKey::Backspace: return Msg{JumperBack{}};
                    default: return std::nullopt;
                }
            }
            if (auto* ck = std::get_if<CharKey>(&k.key)) {
                if (k.mods.ctrl) {
                    if (ck->codepoint == 'c' || ck->codepoint == 'C'
                        || ck->codepoint == 'g' || ck->codepoint == 'G')
                        return Msg{CloseJumper{}};
                    return std::nullopt;
                }
                if (ck->codepoint >= 0x20)
                    return Msg{JumperChar{ck->codepoint}};
            }
            return std::nullopt;
        }

        // ── Normal mode ──
        if (auto* sk = std::get_if<SpecialKey>(&k.key)) {
            switch (*sk) {
                case SpecialKey::Enter:     return Msg{SendComposer{}};
                case SpecialKey::Backspace: return Msg{Backspace{}};
                case SpecialKey::Delete:    return Msg{DeleteToEnd{}};
                case SpecialKey::Tab:       return Msg{NextChannel{}};
                case SpecialKey::BackTab:   return Msg{PrevChannel{}};
                case SpecialKey::Escape:    return Msg{Quit{}};
                case SpecialKey::Up:        return Msg{ScrollUp{}};
                case SpecialKey::Down:      return Msg{ScrollDown{}};
                case SpecialKey::PageUp:    return Msg{ScrollPageUp{}};
                case SpecialKey::PageDown:  return Msg{ScrollPageDn{}};
                case SpecialKey::End:       return Msg{ScrollLatest{}};
                case SpecialKey::Home:      return Msg{CursorHome{}};
                case SpecialKey::Left:      return Msg{CursorLeft{}};
                case SpecialKey::Right:     return Msg{CursorRight{}};
                default:                    return std::nullopt;
            }
        }
        if (auto* ck = std::get_if<CharKey>(&k.key)) {
            char32_t cp = ck->codepoint;
            if (k.mods.ctrl) {
                switch (cp) {
                    case 'a': case 'A': return Msg{CursorHome{}};
                    case 'e': case 'E': return Msg{CursorEnd{}};
                    case 'w': case 'W': return Msg{DeleteWord{}};
                    case 'u': case 'U': return Msg{DeleteToStart{}};
                    case 'k': case 'K': return Msg{DeleteToEnd{}};
                    case 'b': case 'B': return Msg{CursorLeft{}};
                    case 'f': case 'F': return Msg{CursorRight{}};
                    case 'g': case 'G': return Msg{OpenJumper{}};
                    case 'h': case 'H': return Msg{OpenHelp{}};
                    case 'l': case 'L': return Msg{ClearChannel{}};
                    case 'p': case 'P': return Msg{ToggleRightPanel{}};
                    case 'c': case 'C':
                    case 'd': case 'D': return Msg{Quit{}};
                    default: return std::nullopt;
                }
            }
            if (k.mods.alt) return std::nullopt;
            if (cp >= 0x20)  return Msg{CharIn{cp}};
        }
        return std::nullopt;
    }

    static std::optional<Msg> on(const Model&, const ResizeEvent& r) {
        return Msg{Resize{r.width.value, r.height.value}};
    }

    static std::optional<Msg> on(const Model&, const MouseEvent& me) {
        return Msg{RawMouse{me}};
    }

    static Sub subscribe(const Model&) { return Sub::every(50ms, Tick{}); }
};

static_assert(Program<Messenger>);

int main() {
    return run<Messenger>({
        .title = "maya/chat",
        .mouse = true,
        .mode  = Mode::Fullscreen,
    });
}
