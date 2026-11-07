#pragma once
// maya::widget::reasoning — ReasoningStream: a first-class, streaming
// reasoning / chain-of-thought block.
//
// This is the widget every host should use to show a model "thinking". It
// OWNS a StreamingMarkdown internally, so reasoning text streams exactly like
// normal answer text — incremental parse, a rate-smoothed reveal cursor, and
// the renderer's component cache — instead of re-parsing a static blob every
// frame. The chrome around it is tuned for a calm, legible "watch it think"
// experience:
//
//   ┃ ✦ Thinking ⠋                       ← animated header (live)
//   ┃ The user wants a uniform reasoning
//   ┃ display. First I'll look at how the
//   ┃ answer body streams so I can reuse …
//
//   ┃ ✦ Reasoned · ~420 tokens           ← settled header (no spinner)
//   ┃ …full reasoning stays here, never folds…
//
// With meter_below the same line moves under the body and reads as a footer,
// flush with the body's first column. meter_sweep rides a highlight band
// across it while live:
//
//   ┃ …so I can reuse the reveal machinery.
//   ┃
//   ┃ ✦ Thinking ⠋  ·  158 tok  ·  6.2s  ·  26 tok/s
//       └──── a brighter band travels left→right across this row
//
// Design decisions (all deliberate, all UX):
//   • A soft indigo LEFT RAIL (a real box border, not per-line prefixes) so
//     the block reads as one distinct, quiet aside and the markdown body can
//     be a real rendered tree (lists, code, emphasis) rather than raw lines.
//   • The body is DIMMED so the answer below always wins the eye; reasoning
//     is context, not the headline.
//   • LIVE shows an animated ✦ + word + spinner and a reveal caret on the
//     streaming edge. SETTLED keeps the FULL text expanded (never collapses
//     to a one-line summary — reasoning you can't re-read is reasoning you
//     can't trust) and swaps the spinner for a token estimate.
//   • Great streaming defaults are applied INTERNALLY (reveal fx, adaptive
//     pacing) so a host gets smooth streaming for free by just feeding bytes.
//
// Usage:
//   ReasoningStream r;
//   r.set_live(true);
//   r.feed(delta);            // or r.set_content(full) each frame
//   ... on end of reasoning ...
//   r.finish();
//   r.set_live(false);
//   Element e = r.build();    // or `Element e = r;`

#include <cstddef>
#include <cmath>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../core/anim_clock.hpp"
#include "../core/animation.hpp"   // anim::lerp(Color,Color,t)
#include "../core/motion.hpp"      // anim::wave / anim::frame_index (self-pacing)
#include "../element/box.hpp"
#include "../layout/yoga.hpp"      // layout::compute — measure the tail window
#include "../render/renderer.hpp"  // build_layout_tree
#include "../style/border.hpp"
#include "../style/style.hpp"
#include "markdown.hpp"
#include "spinner.hpp"
#include "gradient.hpp"       // detail::lerp_color — theme-safe per-char blend

namespace maya {

class ReasoningStream {
public:
    // The rail / accent hue. A soft indigo reads as "meta / thought" without
    // competing with status colors (blue=info, magenta=agent). Hosts may
    // override to match a theme.
    struct Config {
        Color accent      = Color::slot(ThemeSlot::Accent); // muted indigo rail + sigil
        Color header_word = Color::slot(ThemeSlot::Secondary); // dim lavender header word
        bool  dim_body    = true;                         // reasoning recedes
        // The reasoning body text COLOR. maya deliberately suppresses the SGR
        // "dim" attribute (it made text vanish on some themes), so recession is
        // carried by COLOR, not the dim flag — we recolor every run of the
        // rendered markdown to this muted gray so reasoning clearly reads as a
        // quiet aside beneath the answer.
        Color body_fg     = Color::slot(ThemeSlot::Muted); // muted gray
        // "Stream of consciousness" gradient: while LIVE, fade the body
        // vertically from `body_fg` at the top (older thoughts, receded) to
        // `body_fg_bright` at the bottom (the newest lines, glowing) so the
        // eye follows the live edge as the model thinks. Settled reasoning
        // renders flat in `body_fg` (a frozen, uniform aside). Off by
        // default — hosts opt in per block.
        bool  gradient_body = false;
        Color body_fg_bright = Color::slot(ThemeSlot::Text); // bright lavender (newest)
        // Breathe the newest lines + the rail with the animation clock while
        // live, so the block reads as actively thinking (needs gradient_body).
        bool  pulse = false;
        // Show only the last N ROWS of the reasoning (a scrolling "thought
        // ticker") so a long chain-of-thought stays compact and doesn't
        // shove the composer around. 0 = show everything.
        //
        // The HOST decides when this applies, and it is deliberately not
        // tied to set_live(): "is the model thinking right now" (the header)
        // and "can more reasoning still arrive" (the window) are different
        // questions, and a widget that answers them with one flag changes
        // the block's shape every time the stream pauses or a tool runs.
        // A host that wants the full body passes 0.
        //
        // ROWS, not line-nodes: the block's height is min(content, N), so it
        // grows to the cap and then holds. A node window could not promise
        // that — a node wraps to a variable number of rows, so the height
        // dipped whenever a shorter paragraph became the newest node, and a
        // mid-stream shrink yanks the host's composer up.
        int   live_tail_rows = 0;
        // STRUCTURE the reasoning into beats: paragraph-leading decision
        // markers ("Let me…", "First", "Actually", "So," …) and markdown
        // emphasis/headers render as bright `waypoint_fg` waypoints while the
        // connective prose recedes — so the block reads as phases, not a gray
        // river. Applies live AND settled. Off by default.
        bool  structured = false;
        Color waypoint_fg = Color::slot(ThemeSlot::Text); // bright lavender-white beat
        // When the host hands build_with_body() a body that already carries
        // its own per-row colors (the faded_tail), skip the chrome's flat
        // recolor so the fade survives.
        bool  body_prestyled = false;
        // Put the "✦ Thinking · 158 tokens · 6.2s" line UNDER the body
        // instead of over it. Two reasons, one of them structural:
        //
        //   • it reads as a footer for what you just watched, and while the
        //     model is live the spinner then sits closest to the live edge;
        //   • the meter MUTATES every frame (tokens, elapsed, spinner). At
        //     the top of a growing block that row scrolls toward the
        //     viewport edge while still changing, which is a row the host
        //     may already have committed to the terminal's scrollback.
        //     Below the body it is always the newest row.
        bool  meter_below = false;
        // Paint the meter row as a filled CHIP — a band behind the line —
        // instead of bare text on the block's background. A fill reads as a
        // LABEL for the thing beside it rather than another line of it,
        // which is what this row is.
        //
        // The host supplies the finished Style because only it can pair ink
        // with a band under the terminal's own palette: agentty's
        // ui::chip_style tints an RGB theme and hands the question to the
        // terminal with SGR 7 otherwise. A widget guessing at that is how
        // you get normal foreground on a bright band.
        bool  meter_chip = false;
        Style meter_chip_style{};
        // Ride a highlight band left→right across the meter row while live.
        // The footer is the one row that says "still working", and motion
        // says it without another word or glyph competing for the eye.
        //
        // Theme-safe by construction: each character blends from its own ink
        // toward meter_sweep_fg through lerp_color, which SNAPS instead of
        // projecting when either end has no channels. So an RGB theme gets a
        // smooth crest and theme::native gets a hard-edged band in the user's
        // own palette — never maya's guess at it. Off under reduce_motion.
        bool  meter_sweep = false;
        Color meter_sweep_fg = Color::slot(ThemeSlot::Text);
        int   meter_sweep_ms = 2600;   // one pass, edge to edge
        // Add throughput to the meter ("· 26 tok/s"). Needs elapsed_ms, and
        // stays hidden until there is enough of a sample to be honest.
        bool  meter_rate = false;
        // Wrap the block in a bordered box (the left ┃ rail). A bordered box
        // is an ATOMIC layout unit — maya can't scroll it off row-by-row, so a
        // reasoning taller than the viewport gets CLIPPED (rows vanish) once
        // its top passes the viewport edge. Set false to render a plain
        // column (header + body) that flows and commits to scrollback exactly
        // like prose — distinguished by the ✦ header + body tint instead of a
        // rail. Hosts with unbounded reasoning should use false.
        bool  boxed = true;
        std::string live_word    = "Thinking";
        std::string settled_word = "Reasoned";
    };

    ReasoningStream() : ReasoningStream(Config{}) {}
    explicit ReasoningStream(Config cfg) : cfg_(std::move(cfg)) {
        md_ = std::make_shared<StreamingMarkdown>();
        // Smooth streaming, out of the box. These are the same knobs the
        // answer-body path tunes; a host gets them for free.
        md_->set_reveal_fx(true);
        md_->set_reveal_pacing(/*floor_cps=*/45.0, /*drain_secs=*/0.40);
        md_->set_reveal_adaptive(true, /*floor_min=*/25.0, /*floor_max=*/180.0);
        md_->set_live(true);
    }

    // ── Streaming input ──────────────────────────────────────────────────
    /// Append a reasoning delta (the canonical streaming entry point).
    void feed(std::string_view bytes) { md_->feed(bytes); }
    /// Replace the whole reasoning body (for hosts that resend the full
    /// string each frame — e.g. a growing msg.reasoning field).
    void set_content(std::string_view full) { md_->set_content(full); }
    /// Same as set_content, but offloads a large divergent reparse to a
    /// worker thread (loading history / snapshotting) so the render thread
    /// never stalls. Pure-append growth stays on the fast synchronous path.
    void set_content_async(std::string_view full) { md_->set_content_async(full); }
    /// Commit any pending tail at end-of-stream so the settled height is
    /// stable (call once when the reasoning channel closes).
    void finish() { md_->finish(); }

    // ── State ────────────────────────────────────────────────────────────
    /// Live = still receiving reasoning (animated header + reveal caret).
    /// Settled = done (full text stays; spinner → token estimate).
    void set_live(bool live) noexcept {
        live_ = live;
        md_->set_live(live);
    }
    [[nodiscard]] bool is_live() const noexcept { return live_; }

    /// Tell the widget how many reasoning CHARACTERS were produced, so the
    /// settled header shows an accurate token estimate. Hosts whose body
    /// lives outside the widget (build_with_body) MUST call this — otherwise
    /// the estimate falls back to the widget's own StreamingMarkdown, which
    /// is empty on that path ("~0 tokens").
    void set_char_hint(std::size_t chars) noexcept { char_hint_ = chars; }

    /// Wall-clock reasoning duration in milliseconds, shown in the header next
    /// to the token meter ("· 3.2s"). While live, pass the RUNNING elapsed for
    /// a ticking timer; when settled, pass the final duration. 0 hides it.
    void set_elapsed_ms(std::int64_t ms) noexcept { elapsed_ms_ = ms; }

    /// True while ANYTHING is still animating (reveal glide, live caret,
    /// spinner, in-flight async parse). Hosts drive request_animation_frame
    /// off this so the block stays smooth without a wasteful always-on tick.
    [[nodiscard]] bool is_animating() const noexcept {
        return live_ || md_->is_animating();
    }

    /// The reasoning text so far (codepoint-clean).
    [[nodiscard]] const std::string& source() const noexcept {
        return md_->source();
    }
    [[nodiscard]] bool empty() const noexcept { return md_->source().empty(); }

    [[nodiscard]] const Config& config() const noexcept { return cfg_; }

    // ── Render ───────────────────────────────────────────────────────────
    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        return assemble(build_body());
    }

    /// Render the chrome (header + rail) around a body Element the HOST
    /// already built. This is the integration path for hosts whose
    /// markdown state lives OUTSIDE the widget (e.g. a cross-frame render
    /// cache): they own a persistent StreamingMarkdown, hand its build()
    /// output here, and still get the widget's polished, uniform chrome.
    /// `set_content`/`feed` on this widget's own StreamingMarkdown are then
    /// unused, but is_live()/set_live() still drive the header state.
    [[nodiscard]] Element build_with_body(Element body) const {
        // A pre-styled body (e.g. the faded fixed-height tail) owns its own
        // per-row colors — don't flatten it with the chrome recolor.
        if (cfg_.body_prestyled) return assemble(std::move(body));
        return assemble(dim_wrap(std::move(body)));
    }

    // Build a reasoning tail with DYNAMIC height up to `cap` rows: wrap to
    // the render width, and
    //   * while the wrapped content fits in `cap`, show ALL of it at full
    //     brightness (the block simply GROWS with the reasoning — no padding,
    //     no fade, no big empty container);
    //   * once it exceeds `cap`, pin to `cap` rows showing the LAST lines
    //     (newest at the bottom) and fade the top rows smoothly toward
    //     `faded` (dissolving into the background) so older thought scrolls
    //     away in high resolution.
    // Lazy (wraps at layout width); hand to build_with_body() with
    // Config::body_prestyled = true so the chrome doesn't re-recolor it.
    [[nodiscard]] static Element faded_tail(std::string source, int cap,
                                            LitColor faded, LitColor full) {
        ComponentElement comp;
        comp.render = [source = std::move(source), cap, faded, full]
                      (int w, int) -> Element {
            const int width = w > 6 ? w : 48;
            std::vector<std::string> lines = wrap_text(source, width);
            const int maxh = cap > 0 ? cap : 1;
            const int H = std::min<int>(static_cast<int>(lines.size()), maxh);
            const int start = static_cast<int>(lines.size()) - H;
            const bool capped = start > 0;   // content hidden above → fade top
            std::vector<Element> rows;
            rows.reserve(static_cast<std::size_t>(H));
            for (int i = 0; i < H; ++i) {
                LitColor c = full;
                if (capped) {
                    // Concentrate the dissolve in the TOP few rows; the rest
                    // stay full so most of the window is readable.
                    const double t = H <= 1 ? 1.0
                        : static_cast<double>(i) / static_cast<double>(H - 1);
                    const double a = t * t * (3.0 - 2.0 * t);   // smoothstep
                    c = maya::anim::lerp(faded, full, a);
                }
                TextElement te;
                te.content = lines[static_cast<std::size_t>(start + i)];
                te.style   = Style{}.with_fg(c);
                te.wrap    = TextWrap::NoWrap;   // already wrapped to width
                rows.push_back(Element{std::move(te)});
            }
            BoxElement box;
            box.layout.direction = FlexDirection::Column;
            box.children = std::move(rows);
            return Element{std::move(box)};
        };
        return Element{std::move(comp)};
    }

private:
    // Greedy word-wrap `src` to `width` columns, splitting on newlines
    // (blank lines preserved as empty rows). Long words are hard-split.
    // Byte-oriented — fine for the ASCII-dominant reasoning text; a wide
    // rune counts as its bytes, which at worst wraps a hair early.
    static std::vector<std::string> wrap_text(std::string_view src, int width) {
        if (width < 1) width = 1;
        std::vector<std::string> out;
        auto flush_para = [&](std::string_view para) {
            if (para.empty()) { out.emplace_back(); return; }
            std::string line;
            std::size_t p = 0;
            while (p < para.size()) {
                while (p < para.size() && para[p] == ' ') ++p;
                std::size_t we = p;
                while (we < para.size() && para[we] != ' ') ++we;
                std::string_view word = para.substr(p, we - p);
                p = we;
                if (word.empty()) break;
                auto hard_split = [&](std::string_view& w2) {
                    while (static_cast<int>(w2.size()) > width) {
                        out.emplace_back(w2.substr(0, static_cast<std::size_t>(width)));
                        w2.remove_prefix(static_cast<std::size_t>(width));
                    }
                };
                if (line.empty()) {
                    hard_split(word);
                    line.assign(word);
                } else if (static_cast<int>(line.size() + 1 + word.size()) <= width) {
                    line += ' ';
                    line.append(word);
                } else {
                    out.push_back(std::move(line));
                    line.clear();
                    hard_split(word);
                    line.assign(word);
                }
            }
            if (!line.empty()) out.push_back(std::move(line));
            else if (out.empty() || !out.back().empty()) { /* nothing */ }
        };
        std::size_t i = 0;
        while (true) {
            std::size_t nl = src.find('\n', i);
            if (nl == std::string_view::npos) { flush_para(src.substr(i)); break; }
            flush_para(src.substr(i, nl - i));
            i = nl + 1;
        }
        return out;
    }

    [[nodiscard]] Element assemble(Element body) const {
        std::vector<Element> rows;
        rows.reserve(3);
        if (cfg_.meter_below) {
            rows.push_back(std::move(body));
            // One empty row between the reasoning and its meter, so the
            // chip sits apart from the text instead of looking like the
            // next line of it.
            rows.push_back(Element{TextElement{}});
            rows.push_back(build_header());
        } else {
            rows.push_back(build_header());
            rows.push_back(std::move(body));
        }

        BoxElement box;
        box.layout.direction = FlexDirection::Column;
        if (cfg_.boxed) {
            // Bordered rail box (atomic — see Config::boxed). Left padding gives
            // the rail a one-column gutter from the content.
            box.layout.padding = Edges<int>{0, 0, 0, 1};
            box.border = BorderConfig{
                .style  = BorderStyle::Bold,                // ┃ heavier = a "rail"
                .sides  = BorderSides{false, false, false, true}, // left only
                .colors = BorderColors{
                    .left = live_ ? rail_color()
                                  : dim(theme::live().resolve(cfg_.accent)),
                },
            };
        }
        // Unboxed: a plain header+body column. maya splits it row-by-row like
        // prose, so a tall reasoning scrolls off the top / commits to
        // scrollback instead of being clipped at the viewport edge.
        box.children = std::move(rows);
        return Element{std::move(box)};
    }

    Config cfg_;
    std::shared_ptr<StreamingMarkdown> md_;
    bool live_ = true;
    std::size_t char_hint_ = 0; // host-supplied reasoning length (0 = use md_)
    std::int64_t elapsed_ms_ = 0; // host-supplied reasoning duration (0 = hide)

    // A dimmer variant of a color for the settled rail (recede once done).
    static LitColor dim(LitColor c) noexcept { return c.darken(0.45f); }

    // Rail hue while live: breathes between dim and full accent in lockstep
    // with the body pulse (same pulse01 phase) so the whole block feels alive
    // while thinking. Flat accent when pulse is off.
    [[nodiscard]] LitColor rail_color() const {
        const LitColor accent = theme::live().resolve(cfg_.accent);
        if (!(cfg_.pulse && live_)) return accent;
        return maya::anim::lerp(dim(accent), accent,
                                0.45 + 0.55 * pulse01());
    }

    [[nodiscard]] std::size_t reasoning_chars() const noexcept {
        return char_hint_ ? char_hint_ : md_->source().size();
    }

    // Humanized token estimate: "4" → "820" → "1.2k" → "18k" (~4 chars/token).
    [[nodiscard]] std::string token_estimate() const {
        const std::size_t toks = (reasoning_chars() + 3) / 4;
        if (toks < 1000) return std::to_string(toks);
        if (toks < 10000)
            return std::to_string(toks / 1000) + "." +
                   std::to_string((toks % 1000) / 100) + "k";
        return std::to_string((toks + 500) / 1000) + "k";
    }

    // Reasoning throughput, "26 tok/s". Empty until the sample is long
    // enough to mean something: at 200 ms a burst reads 400 tok/s and then
    // halves twice while you watch it, which is noise wearing a number.
    [[nodiscard]] std::string rate_label() const {
        if (elapsed_ms_ < 1500) return {};
        const double toks = static_cast<double>((reasoning_chars() + 3) / 4);
        const double secs = static_cast<double>(elapsed_ms_) / 1000.0;
        if (toks < 1.0 || secs <= 0.0) return {};
        const long rate = static_cast<long>(toks / secs + 0.5);
        if (rate <= 0) return {};
        return std::to_string(rate) + " tok/s";
    }

    // Humanized reasoning duration: "0.8s" → "3.2s" → "42s" → "1m03s".
    // Sub-10s keeps one decimal (a short think reads as "3.2s", not "3s");
    // ≥10s rounds to whole seconds; ≥60s switches to m s.
    [[nodiscard]] std::string duration_label() const {
        const std::int64_t ms = elapsed_ms_;
        if (ms >= 60000) {
            const std::int64_t s = ms / 1000;
            const std::int64_t mm = s / 60, ss = s % 60;
            return std::to_string(mm) + "m" + (ss < 10 ? "0" : "") +
                   std::to_string(ss) + "s";
        }
        if (ms >= 10000)
            return std::to_string((ms + 500) / 1000) + "s";
        const std::int64_t tenths = (ms + 50) / 100;   // round to 0.1s
        return std::to_string(tenths / 10) + "." +
               std::to_string(tenths % 10) + "s";
    }

    [[nodiscard]] Element build_header() const {
        std::string content;
        std::vector<StyledRun> runs;
        // On a chip every run shares the band's ink: a label is text on a
        // tint, and ink that changes mid-chip is a second thing for the eye
        // to resolve. Emphasis (bold/italic) still comes through.
        const bool chip = cfg_.meter_chip;
        auto push = [&](std::string_view part, Style st) {
            if (chip) {
                Style s = cfg_.meter_chip_style;
                if (st.bold)   s = s.with_bold();
                if (st.italic) s = s.with_italic();
                st = s;
            }
            const std::size_t s = content.size();
            content.append(part);
            runs.push_back(StyledRun{s, content.size() - s, st});
        };

        // The chip's own padding, filled like the rest of the band.
        if (chip) push(" ", Style{});

        // ✦ sigil in the accent hue.
        {
            const LitColor accent = theme::live().resolve(cfg_.accent);
            push("\xe2\x9c\xa6 ", Style{}.with_fg(live_ ? accent : dim(accent)));
        }

        // The header WORD ("Thinking" / "Reasoned") renders identically in
        // both states — same color, same weight — so the block reads as one
        // element that simply swapped its label, not two differently-styled
        // things. The meta (token estimate + duration) shares one muted style.
        const Style word_style = Style{}.with_fg(cfg_.header_word).with_bold();
        const Style meta_style = Style{}.with_fg(cfg_.header_word).with_italic();

        if (live_) {
            push(cfg_.live_word, word_style);
            // Spinner frame off the shared clock (deterministic in tests) so
            // it breathes even without a host advance() call.
            push(" ", Style{});
            push(spinner_frame(), Style{}.with_fg(cfg_.accent).with_dim());
            // Live thinking METER: the token estimate ticks up as reasoning
            // streams, so the block conveys effort/progress, not just motion.
            // Reflects the FULL source even when the body is tail-windowed.
            if (reasoning_chars() > 0)
                push("  \xc2\xb7  " + token_estimate() + " tok", meta_style); // · N tok
            if (elapsed_ms_ > 0)
                push("  \xc2\xb7  " + duration_label(), meta_style);          // · 3.2s
            if (cfg_.meter_rate)
                if (const std::string r = rate_label(); !r.empty())
                    push("  \xc2\xb7  " + r, meta_style);                     // · 26 tok/s
        } else {
            push(cfg_.settled_word, word_style);
            // Suppress the token meta at ~0 so a stray empty block never
            // reads "0 tokens".
            if (reasoning_chars() > 0)
                push("  \xc2\xb7  " + token_estimate() + " tokens", meta_style); // · N tokens
            if (elapsed_ms_ > 0)
                push("  \xc2\xb7  " + duration_label(), meta_style);            // · 3.2s
            if (cfg_.meter_rate)
                if (const std::string r = rate_label(); !r.empty())
                    push("  \xc2\xb7  " + r, meta_style);                       // · 26 tok/s
        }

        if (chip) push(" ", Style{});

        apply_sweep(content, runs);

        return Element{TextElement{
            .content = std::move(content),
            .style   = {},
            .wrap    = TextWrap::TruncateEnd, // one row, never wraps
            .runs    = std::move(runs),
        }};
    }

    // Is the travelling band running this frame? Live only — a settled
    // footer is a record, and a record that shimmers reads as still working.
    [[nodiscard]] bool sweep_active() const noexcept {
        return cfg_.meter_sweep && live_ && !maya::anim::reduce_motion();
    }

    // Recolor the finished meter into per-character runs, each blended from
    // its own ink toward meter_sweep_fg by a gaussian crest that travels
    // left→right. Runs with no fg (the plain spaces) are left alone, so the
    // band brightens ink and never paints over an inherited default.
    //
    // The crest starts and ends OFF the row (-band → n+band) so each pass
    // arrives and leaves instead of popping back to the first column.
    void apply_sweep(const std::string& content,
                     std::vector<StyledRun>& runs) const {
        if (!sweep_active() || content.empty() || runs.empty()) return;

        std::vector<std::pair<std::size_t, std::size_t>> cells;
        for (std::size_t pos = 0; pos < content.size();) {
            const std::size_t start = pos;
            (void)decode_utf8(content, pos);
            if (pos <= start) break;              // malformed: don't spin
            cells.push_back({start, pos - start});
        }
        const int n = static_cast<int>(cells.size());
        if (n < 2) return;

        // Byte offset -> the style the run pass already decided for it.
        auto style_at = [&runs](std::size_t off) {
            for (const StyledRun& r : runs)
                if (off >= r.byte_offset && off < r.byte_offset + r.byte_length)
                    return r.style;
            return Style{};
        };

        const double band = std::max(4.0, static_cast<double>(n) * 0.22);
        const double head = maya::anim::loop_phase(
            static_cast<double>(cfg_.meter_sweep_ms));
        const double crest = -band + head * (static_cast<double>(n) + 2.0 * band);

        std::vector<StyledRun> swept;
        swept.reserve(cells.size());
        for (int i = 0; i < n; ++i) {
            Style st = style_at(cells[static_cast<std::size_t>(i)].first);
            const double d = (static_cast<double>(i) - crest) / band;
            const double w = std::exp(-d * d * 2.4);
            if (w > 0.02 && st.fg)
                st = st.with_fg(detail::lerp_color(*st.fg, cfg_.meter_sweep_fg,
                                                   static_cast<float>(w)));
            swept.push_back(StyledRun{cells[static_cast<std::size_t>(i)].first,
                                      cells[static_cast<std::size_t>(i)].second,
                                      st});
        }
        runs = std::move(swept);
    }

    [[nodiscard]] Element build_body() const {
        return dim_wrap(md_->build());
    }

    [[nodiscard]] Element dim_wrap(Element body) const {
        if (!cfg_.dim_body) return body;
        // Recede the whole reasoning tree by COLOR (the maya dim SGR is
        // suppressed, so a with_dim() box does nothing visible). The body is a
        // lazy StreamingMarkdown ComponentElement, so we can't recolor it
        // statically — wrap it in a ComponentElement that renders the inner
        // tree at layout time and then overrides every text run's fg.
        //
        // WHILE LIVE (and gradient_body), fade vertically from body_fg at the
        // top (older thoughts) to body_fg_bright at the bottom (newest) so the
        // eye rides the live edge — a "stream of consciousness". Settled, or
        // gradient off, it's a flat muted recolor (a frozen, uniform aside).
        const bool grad = cfg_.gradient_body && live_;
        const bool pulse = cfg_.pulse && live_;
        const int  cap   = cfg_.live_tail_rows;   // host-decided, see Config
        const bool structured = cfg_.structured;
        // Resolved up front: everything below BLENDS these (lerp, darken), and
        // blending needs channels. cfg_ holds Color so a host may configure a
        // theme slot; this is the boundary where it becomes a painted colour.
        const Theme& th_    = theme::live();
        const LitColor fg     = th_.resolve(cfg_.body_fg);
        const LitColor bright = th_.resolve(cfg_.body_fg_bright);
        const LitColor waypoint = th_.resolve(cfg_.waypoint_fg);
        ComponentElement comp;
        comp.render = [body = std::move(body), fg, bright, waypoint,
                       grad, pulse, cap, structured]
                      (int w, int h) -> Element {
            Element rendered = body; // copy the (cheap) wrapper node
            int total = materialize_and_count(rendered, w, h);
            // Coarse prune first: a visible row needs a node and every node
            // is at least one row tall, so nodes older than the last `cap`
            // can never land inside a cap-row window. Dropping them keeps
            // the per-frame work bounded by the window, not by how long the
            // model has been thinking.
            if (cap > 0 && total > cap) {
                int pidx = 0;
                prune_to_tail(rendered, total - cap, pidx);
                total = cap;
            }
            // Gradient endpoint (breathing when pulsing); flat = fg→fg.
            LitColor to = fg;
            if (grad) {
                to = bright;
                if (pulse) {
                    const double k = 0.62 + 0.38 * pulse01();
                    to = maya::anim::lerp(fg, bright, k);
                }
            }
            int idx = 0;
            apply_gradient(rendered, fg, to, idx, total, w, h,
                           structured, waypoint);
            // A provider's delta usually ends on a paragraph break, which
            // leaves a trailing gap under the newest line. In a row window
            // that gap costs budget -- the oldest row gets cropped to pay
            // for a row with nothing in it -- and while the wire is quiet it
            // is just dead space at the bottom of the block. Trim it: the
            // last row is always the newest text, and the next delta brings
            // its own spacing back.
            drop_trailing_blanks(rendered, w, h);
            return cap > 0 ? window_to_rows(std::move(rendered), cap, w, h)
                           : rendered;
        };
        return Element{std::move(comp)};
    }

    // Does this subtree paint nothing but whitespace? A trailing gap node,
    // not a row the reader needs.
    static bool blank_subtree(const Element& e, int w, int h) {
        bool blank = true;
        std::visit([&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, TextElement>) {
                blank = node.content.find_first_not_of(" \t\r\n") == std::string::npos;
            } else if constexpr (std::is_same_v<T, BoxElement>) {
                // Borders and padding paint, so such a box is not blank.
                if (node.has_border() || node.layout.padding.vertical() > 0) {
                    blank = false;
                    return;
                }
                for (const auto& c : node.children)
                    if (!blank_subtree(c, w, h)) { blank = false; return; }
            } else if constexpr (std::is_same_v<T, ElementList>) {
                for (const auto& c : node.items)
                    if (!blank_subtree(c, w, h)) { blank = false; return; }
            } else if constexpr (std::is_same_v<T, ComponentElement>) {
                blank = node.render ? blank_subtree(node.render(w, h), w, h) : true;
            } else {
                blank = false;   // anything else paints something
            }
        }, e.inner);
        return blank;
    }

    // Drop the trailing whitespace-only nodes of a vertical stack, and the
    // gap rows they brought with them.
    static void drop_trailing_blanks(Element& e, int w, int h) {
        std::visit([&](auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, BoxElement>) {
                while (!node.children.empty()
                       && blank_subtree(node.children.back(), w, h))
                    node.children.pop_back();
                if (!node.children.empty())
                    drop_trailing_blanks(node.children.back(), w, h);
            } else if constexpr (std::is_same_v<T, ElementList>) {
                while (!node.items.empty()
                       && blank_subtree(node.items.back(), w, h))
                    node.items.pop_back();
                if (!node.items.empty())
                    drop_trailing_blanks(node.items.back(), w, h);
            } else if constexpr (std::is_same_v<T, ComponentElement>) {
                if (node.render) {
                    Element inner = node.render(w, h);
                    drop_trailing_blanks(inner, w, h);
                    node.render = [inner = std::move(inner)](int, int) {
                        return inner;
                    };
                }
            }
            // ElementListRef is borrowed, app-owned data — never mutated.
        }, e.inner);
    }

    // Rows the (materialized) tree occupies at `w`, straight from the layout
    // engine — no second wrap implementation to drift from the painter.
    static int measure_rows(const Element& e, int w) {
        std::vector<layout::LayoutNode> nodes;
        const std::size_t root = render_detail::build_layout_tree(e, nodes, Theme{});
        layout::compute(nodes, root, w);
        return nodes[root].computed.size.height.value;
    }

    // Keep the newest `cap` rows of the body, cut at ROW granularity.
    //
    // The cut splits a paragraph's wrapped lines instead of dropping the
    // paragraph, so the window holds its height while markdown structure,
    // styled runs and the reveal caret all survive — and the caret is at the
    // bottom, which is the edge the window is anchored to.
    static Element window_to_rows(Element body, int cap, int w, int h) {
        if (w <= 0 || cap <= 0) return body;
        flatten_components(body, w, h);
        const int rows = measure_rows(body, w);
        if (rows <= cap) return body;

        // One row of the window goes to a `⋯ N lines above` marker, so a
        // capped block says there is more instead of silently starting
        // mid-thought. Height stays exactly `cap`.
        const bool marker = cap >= 3;
        const int want = rows - cap + (marker ? 1 : 0);
        const int cut  = drop_top_rows(body, want, w);
        BoxElement stack;
        stack.layout.direction = FlexDirection::Column;
        if (marker) {
            TextElement m;
            m.content = "\xe2\x8b\xaf " + std::to_string(cut)
                      + (cut == 1 ? " line above" : " lines above");
            m.style   = Style{}.with_fg(Color::slot(ThemeSlot::Muted)).with_italic();
            m.wrap    = TextWrap::TruncateEnd;
            stack.children.push_back(Element{std::move(m)});
        }
        // A container's gap row cannot outlive the child above it, so a cut
        // that lands on one eats a row more than asked. Hand it back as a
        // blank row so the height stays exactly `cap`.
        if (cut > want) {
            BoxElement pad;
            pad.layout.height = Dimension::fixed(cut - want);
            stack.children.push_back(Element{std::move(pad)});
        }
        if (!marker && cut <= want) return body;
        stack.children.push_back(std::move(body));
        return Element{std::move(stack)};
    }

    // Replace every (materialized) component with the subtree it renders, so
    // the measure + crop below walk a plain Box/List/Text tree. Measuring a
    // live ComponentElement would go through the renderer's pointer-keyed
    // component cache, and these nodes are temporaries.
    static void flatten_components(Element& e, int w, int h) {
        if (auto* comp = std::get_if<ComponentElement>(&e.inner)) {
            if (comp->render) {
                Element inner = comp->render(w, h);
                flatten_components(inner, w, h);
                e = std::move(inner);
                return;
            }
        }
        std::visit([&](auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, BoxElement>) {
                for (auto& c : node.children) flatten_components(c, w, h);
            } else if constexpr (std::is_same_v<T, ElementList>) {
                for (auto& c : node.items) flatten_components(c, w, h);
            }
        }, e.inner);
    }

    // Drop the first `k` rows of a vertical block subtree, in place. Returns
    // the rows actually dropped: `k`, or one container gap row more when the
    // cut lands on a gap (window_to_rows pays that back).
    static int drop_top_rows(Element& e, int k, int w) {
        if (k <= 0) return 0;
        int done = 0;
        std::visit([&](auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, TextElement>) {
                done = crop_text_top(node, k, w);
            } else if constexpr (std::is_same_v<T, BoxElement>) {
                // The box's own top chrome goes first: the window cut it off.
                if (node.has_border() && node.border.sides.top) {
                    node.border.sides.top = false;
                    ++done;
                }
                const int p = std::min(k - done, node.layout.padding.top);
                if (p > 0) { node.layout.padding.top -= p; done += p; }
                done += drop_leading(node.children, node.layout.gap,
                                     k - done,
                                     std::max(0, w - node.inner_horizontal()));
            } else if constexpr (std::is_same_v<T, ElementList>) {
                done = drop_leading(node.items, 0, k, w);
            }
            // ElementListRef is borrowed, app-owned data — never mutated.
        }, e.inner);
        return done;
    }

    template <class Vec>
    static int drop_leading(Vec& kids, int gap, int k, int w) {
        if (k <= 0) return 0;
        int done = 0;
        std::size_t drop = 0;
        while (drop < kids.size() && done < k) {
            const int r = measure_rows(kids[drop], w);
            if (done + r <= k) {
                // Whole child, and the gap row that followed it goes too.
                done += r + (drop + 1 < kids.size() ? gap : 0);
                ++drop;
                continue;
            }
            done += drop_top_rows(kids[drop], k - done, w);
            break;
        }
        kids.erase(kids.begin(), kids.begin() + static_cast<std::ptrdiff_t>(drop));
        return done;
    }

    // Drop the first `k` wrapped rows of a text node (0 < k < its rows).
    // The wrap cache keys on content.size(), so the shorter content
    // invalidates it without a setter.
    static int crop_text_top(TextElement& t, int k, int w) {
        const auto& lines = t.format(w);
        if (k <= 0 || static_cast<std::size_t>(k) >= lines.size()) return 0;
        const std::size_t off = lines[static_cast<std::size_t>(k)].byte_offset;
        if (off == 0 || off > t.content.size()) return 0;
        t.content.erase(0, off);
        std::vector<StyledRun> kept;
        kept.reserve(t.runs.size());
        for (auto r : t.runs) {
            const std::size_t end = r.byte_offset + r.byte_length;
            if (end <= off) continue;            // entirely above the cut
            if (r.byte_offset < off) {           // straddles it
                r.byte_length = end - off;
                r.byte_offset = 0;
            } else {
                r.byte_offset -= off;
            }
            kept.push_back(r);
        }
        t.runs = std::move(kept);
        return k;
    }

    // Pulse phase in [0,1] from the shared animation clock (~1.4 s period),
    // so the body glow and the rail breathe in lockstep. anim::wave() is
    // phase-continuous on the shared clock AND self-pacing: calling it
    // declares "I'm animating" to the run loop, so a live reasoning block
    // keeps painting without the host having to poll is_animating().
    [[nodiscard]] static double pulse01() {
        return anim::wave(1400.0);
    }

    // Recursively override the foreground color of every text run in a tree.
    // ComponentElement children are rendered (with the given w/h) so their
    // produced subtree is recolored too — this is what reaches the streaming
    // markdown's lazily-built paragraphs.
    static void recolor_fg(Element& e, Color fg, int w, int h) {
        std::visit([&](auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, TextElement>) {
                node.style = node.style.with_fg(fg);
                for (auto& r : node.runs) r.style = r.style.with_fg(fg);
            } else if constexpr (std::is_same_v<T, BoxElement>) {
                // Leave a background (e.g. code block) intact; only steer fg.
                for (auto& c : node.children) recolor_fg(c, fg, w, h);
            } else if constexpr (std::is_same_v<T, ElementList>) {
                for (auto& c : node.items) recolor_fg(c, fg, w, h);
            } else if constexpr (std::is_same_v<T, ComponentElement>) {
                if (node.render) {
                    Element inner = node.render(w, h);
                    recolor_fg(inner, fg, w, h);
                    // Replace the lazy node with the recolored, materialized
                    // subtree so the renderer paints the tinted version.
                    node.render = [inner = std::move(inner)](int, int) {
                        return inner;
                    };
                }
            }
            // ElementListRef: borrowed app-owned data — don't mutate.
        }, e.inner);
    }

    // Materialize every lazy ComponentElement into a concrete subtree AND
    // count the TextElement nodes, in traversal (top-to-bottom) order. The
    // count is the number of gradient steps; materializing here means the
    // second pass (apply_gradient) walks the exact same node set/order.
    static int materialize_and_count(Element& e, int w, int h) {
        int n = 0;
        std::visit([&](auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, TextElement>) {
                n = 1;
            } else if constexpr (std::is_same_v<T, BoxElement>) {
                for (auto& c : node.children) n += materialize_and_count(c, w, h);
            } else if constexpr (std::is_same_v<T, ElementList>) {
                for (auto& c : node.items) n += materialize_and_count(c, w, h);
            } else if constexpr (std::is_same_v<T, ComponentElement>) {
                if (node.render) {
                    Element inner = node.render(w, h);
                    n += materialize_and_count(inner, w, h);
                    node.render = [inner = std::move(inner)](int, int) {
                        return inner;
                    };
                }
            }
        }, e.inner);
        return n;
    }

    // Does this paragraph START a reasoning beat? True for lines that open
    // with a decision/phase marker — the connective tissue of chain-of-
    // thought. Case-insensitive on the first token, after trimming leading
    // whitespace and markdown list/quote glyphs.
    static bool is_beat(std::string_view s) {
        std::size_t i = 0;
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '>'
                || s[i] == '-' || s[i] == '*' || s[i] == '#' || s[i] == '.'
                || (s[i] >= '0' && s[i] <= '9') || s[i] == ')'))
            ++i;
        s.remove_prefix(i);
        if (s.empty()) return false;
        char buf[20];
        const std::size_t n = std::min<std::size_t>(s.size(), sizeof(buf) - 1);
        for (std::size_t k = 0; k < n; ++k) {
            char c = s[k];
            buf[k] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
        }
        std::string_view p{buf, n};
        static constexpr std::string_view kMarkers[] = {
            "let me", "let's", "lets ", "first", "second", "third", "next",
            "then,", "then ", "finally", "now,", "now ", "actually", "wait",
            "hmm", "so,", "so ", "okay", "ok,", "ok ", "looking at", "i need",
            "i'll", "i should", "i want", "i can", "the key", "the issue",
            "the problem", "the plan", "the goal", "step ", "note:", "important",
            "however", "but ", "instead", "alternatively", "to summar",
            "in summary", "therefore", "because", "given ", "considering",
            "overall", "hold on", "on second", "checking", "i'm going",
        };
        for (auto m : kMarkers) if (p.starts_with(m)) return true;
        return false;
    }

    // Recolor each TextElement to lerp(from,to, idx/(total-1)) in traversal
    // order — idx 0 (top / oldest) = from, idx total-1 (bottom / newest) = to.
    // When `structured`, paragraphs that OPEN a beat (is_beat) and any bold
    // run (markdown emphasis / headers) get `waypoint` + bold instead, so the
    // reasoning reads as phases. Assumes the tree is materialized.
    static void apply_gradient(Element& e, LitColor from, LitColor to,
                               int& idx, int total, int w, int h,
                               bool structured = false,
                               Color waypoint = {}) {
        std::visit([&](auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, TextElement>) {
                const double t = total <= 1 ? 1.0
                    : static_cast<double>(idx) / static_cast<double>(total - 1);
                const LitColor base = maya::anim::lerp(from, to, t);
                const bool beat = structured && is_beat(node.content);
                if (beat) {
                    node.style = node.style.with_fg(waypoint).with_bold();
                    for (auto& r : node.runs)
                        r.style = r.style.with_fg(waypoint).with_bold();
                } else {
                    node.style = node.style.with_fg(base);
                    for (auto& r : node.runs)
                        r.style = (structured && r.style.bold)
                            ? r.style.with_fg(waypoint)   // inline emphasis = mini-beat
                            : r.style.with_fg(base);
                }
                ++idx;
            } else if constexpr (std::is_same_v<T, BoxElement>) {
                for (auto& c : node.children)
                    apply_gradient(c, from, to, idx, total, w, h, structured, waypoint);
            } else if constexpr (std::is_same_v<T, ElementList>) {
                for (auto& c : node.items)
                    apply_gradient(c, from, to, idx, total, w, h, structured, waypoint);
            } else if constexpr (std::is_same_v<T, ComponentElement>) {
                if (node.render) {
                    Element inner = node.render(w, h);
                    apply_gradient(inner, from, to, idx, total, w, h, structured, waypoint);
                    node.render = [inner = std::move(inner)](int, int) {
                        return inner;
                    };
                }
            }
        }, e.inner);
    }

    // Tail window: keep only line-nodes whose global index >= `cutoff` (the
    // last N), pruning earlier text and any container left empty, so a long
    // live reasoning chain collapses to its most-recent lines. `idx` threads
    // the running text-node counter in the SAME order as
    // materialize_and_count. Returns whether `e` should stay in its parent.
    static bool prune_to_tail(Element& e, int cutoff, int& idx) {
        bool keep = true;
        std::visit([&](auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, TextElement>) {
                keep = (idx >= cutoff);
                ++idx;
            } else if constexpr (std::is_same_v<T, BoxElement>) {
                std::vector<Element> kept;
                kept.reserve(node.children.size());
                for (auto& c : node.children)
                    if (prune_to_tail(c, cutoff, idx)) kept.push_back(std::move(c));
                node.children = std::move(kept);
                keep = !node.children.empty();
            } else if constexpr (std::is_same_v<T, ElementList>) {
                std::vector<Element> kept;
                kept.reserve(node.items.size());
                for (auto& c : node.items)
                    if (prune_to_tail(c, cutoff, idx)) kept.push_back(std::move(c));
                node.items = std::move(kept);
                keep = !node.items.empty();
            } else if constexpr (std::is_same_v<T, ComponentElement>) {
                if (node.render) {
                    Element inner = node.render(0, 0);  // materialized → args ignored
                    keep = prune_to_tail(inner, cutoff, idx);
                    node.render = [inner = std::move(inner)](int, int) {
                        return inner;
                    };
                }
            }
        }, e.inner);
        return keep;
    }

    // Header spinner glyph derived from the shared animation clock so the
    // block animates in lockstep with the rest of the live chrome. Frames +
    // cadence come from the one Spinner SSOT (widget/spinner.hpp); build()
    // re-arms via the spinner's self-pacing frame read.
    [[nodiscard]] std::string_view spinner_frame() const {
        constexpr auto f = detail::get_spinner_frames<SpinnerStyle::Dots>();
        const auto idx = anim::frame_index(
            static_cast<std::size_t>(f.count),
            static_cast<double>(f.interval) * 1000.0);
        return f.frames[idx];
    }
};

} // namespace maya
