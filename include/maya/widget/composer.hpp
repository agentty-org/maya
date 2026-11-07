#pragma once
// maya::widget::Composer — bordered input box reflecting agent state.
//
// State-driven input box: border / prompt / placeholder change with
// activity (idle, awaiting permission, streaming, executing tool); the
// hint row carries Send / newline / expand keys plus right-side ambient
// indicators (queued count, word/token counters, profile chip). Wraps,
// caps height, pins height during activity to prevent jitter, and adds
// a bottom-right "N lines" caption when multi-line.
//
//   maya::Composer{{
//       .text        = m.ui.composer.text,
//       .cursor      = m.ui.composer.cursor,
//       .state       = compute_state(m),
//       .active_color = phase_color(m.s.phase),
//       .queued      = m.ui.composer.queued.size(),
//       .profile     = {.label = profile_label(m.d.profile),
//                       .color = profile_color(m.d.profile)},
//       .expanded    = m.ui.composer.expanded,
//   }}.build();

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../device.hpp"        // request_animation_frame
#include "../dsl.hpp"
#include "../element/element.hpp"
#include "../style/border.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

#include "divider.hpp"
#include "../style/theme.hpp"

namespace maya {

class Composer {
public:
    // Drives border color, prompt boldness, placeholder text, and the
    // height-pin behaviour that keeps the box from bobbing during
    // streaming / tool execution.
    enum class State : std::uint8_t {
        Idle,                  // nothing happening — may or may not have text
        AwaitingPermission,    // user must respond to a permission prompt above
        Streaming,             // model is generating
        ExecutingTool,         // a tool is running
    };

    struct ProfileChip {
        std::string label;     // raw — widget small-caps's it
        Color       color = Color::slot(ThemeSlot::Accent);
    };

    struct Config {
        std::string text;
        int         cursor = 0;

        State state         = State::Idle;
        // The color used for the border / prompt while the agent is
        // active (Streaming / ExecutingTool). Caller picks based on the
        // current phase so the composer matches the status bar.
        Color active_color  = Color::slot(ThemeSlot::Info);

        // Brand palette
        Color text_color      = Color::slot(ThemeSlot::Text);
        Color accent_color    = Color::slot(ThemeSlot::Accent);   // "primed" border, idle + text
        Color warn_color      = Color::slot(ThemeSlot::Warning);    // awaiting-permission border
        Color highlight_color = Color::slot(ThemeSlot::Info);      // queue-depth chip

        // Right-side ambient indicators
        std::size_t queued = 0;

        // LOOP mode: the caller re-sends one armed message after every
        // completed turn until the user toggles it off. When `loop` is set
        // the composer paints a ⟳ chip (with the completed-iteration count
        // once it has fired at least once) in the right cluster and tints
        // the border with `loop_color`, so an app that is auto-sending on
        // your behalf can never look like an idle one.
        bool  loop = false;
        int   loop_iterations = 0;
        Color loop_color = Color::slot(ThemeSlot::Info);
        // Seconds until the loop's next auto-send, when it is waiting out a
        // backoff after a failed turn. >0 renders the chip as a countdown
        // (⟳ RETRY 24s) instead of a plain LOOP badge, so a paused loop is
        // visibly WAITING rather than mistaken for a wedged one.
        int   loop_wait_secs = 0;

        // Render the body text as a READ-ONLY readout: dimmed, no caret.
        // Callers set this when the buffer is showing something the user
        // cannot currently edit (agentty: the prompt a ^B loop is re-sending)
        // so "you can't type here right now" is legible from the text itself
        // rather than only discoverable by pressing a key and seeing nothing.
        bool read_only = false;
        ProfileChip profile;

        // Left slot of the hint row. When non-empty this REPLACES the
        // built-in key hints ("↵ send · ⇧↵ newline") — the caller owns
        // that real estate and paints whatever is more useful there
        // (agentty puts the active model · provider). It is measured
        // like every other fragment and sheds whole if the profile chip
        // can't otherwise fit.
        Element status;   // default-empty = built-in key hints

        // Hide the built-in "↵ send" / "⇧↵ newline" key hints. Only
        // meaningful when `status` is empty (a non-empty status already
        // displaces them); set it to leave the left slot blank.
        bool show_key_hints = true;

        // Ambient-counter overrides. The composer `text` the widget
        // sees is the CHIP-RENDERED display string — a long paste or
        // @file is a short caption like "[Pasted text · 412 lines ·
        // 14 KB]", NOT its expanded body. Counting words / tokens /
        // lines off that caption undercounts wildly the moment any
        // attachment exists (a 400-line paste reads as "1 line, ~10
        // tok"). When the caller knows the real figures — it can
        // expand attachment bodies — it sets these; the widget uses
        // them verbatim instead of deriving from `text`. -1 (default)
        // ⇒ derive from the visible text (legacy, correct when there
        // are no attachments).
        int token_estimate = -1;
        int word_estimate  = -1;
        int line_estimate  = -1;

        // Layout
        bool expanded = false;

        // Content-stable cross-frame cache identity. When non-empty,
        // build() wraps its result in a ComponentElement carrying this
        // hash_id, so the renderer's cross-frame component cache blits
        // the composer's laid-out cells instead of re-running
        // layout::compute over the whole box (border + divider +
        // width-adaptive hint component) every frame.
        //
        // WHY: during streaming the host re-runs view() on every delta,
        // rebuilding the composer Element fresh each frame. Without a
        // stable key the renderer pointer-keys on a brand-new
        // ComponentElement address that never matches the prior frame,
        // so it re-lays-out the whole subtree — and the width-adaptive
        // hint row's 1-cell layout drift reads as flicker. The caller
        // derives this id from exactly the fields that change the
        // rendered pixels (text, cursor, state, colors, counts, width),
        // so it stays constant across the many streaming frames where
        // none of those move — turning per-frame relayout into an O(1)
        // cache blit. Leave empty to keep legacy per-frame rebuild.
        //
        // NOTE: the blink phase is deliberately NOT folded into the id.
        // While active (streaming / executing) the cursor holds steady
        // (no blink), so the composer is genuinely frame-invariant and
        // the cache is always fresh. While idle the widget requests its
        // own animation frames for the blink; the id excludes blink so
        // the caller doesn't have to thread wall-clock state through —
        // idle repaints are cheap and rare (~4 Hz) either way.
        CacheId cache_id{};

        // Minimum body-row count. The composer pads its inner column
        // with blank rows up to this floor so transient height
        // changes (empty→one char, last char→empty, 1-line→2-line
        // wrap) don't reshape the box and reflow the diff against
        // every row above. Default 1 = legacy behavior. Set to 2 (or
        // higher) when the caller wants stable composer height to
        // suppress streaming-time flicker. Caps at the natural body
        // height — never SHRINKS a composer that already has more
        // content than the floor.
        int min_body_rows = 1;

        // Wall-clock (anim::default_clock().now_ms()) of the user's last
        // composer edit. The idle blink STOPS 15 s after this, mirroring
        // kitty's own cursor_stop_blinking_after: once the user stops
        // typing, the painted block cursor goes solid-visible and the
        // composer cell stops changing. This is load-bearing for idle CPU
        // on GPU terminals with an aggressive repaint_delay (kitty defaults
        // repaint at up to 100 fps whenever ANY cell is dirty) — a forever-
        // blinking painted cursor keeps the compositor awake indefinitely,
        // where kitty's OWN hardware cursor would have stopped after 15 s.
        // agentty hides the hardware cursor and paints its own, so it must
        // reproduce that stop itself. 0 (default) ⇒ never times out (legacy
        // always-blink) so callers that don't plumb activity are unchanged.
        std::int64_t last_edit_ms = 0;

        // ── Hardware caret ─────────────────────────────────────
        // Use the terminal's REAL cursor as the composer caret instead
        // of the painted blinking block. The widget still inserts the
        // caret glyph bytes (wrap geometry stays byte-identical to the
        // painted mode — no reflow on toggle) but styles the cell
        // conceal + caret_anchor: it paints NOTHING, and the inline
        // serializer's frame epilogue moves the hardware cursor onto
        // that cell and shows it (DECTCEM).
        //
        // What this buys over the painted caret:
        //   • native blink at the terminal's own cadence — and ZERO
        //     animation-frame wake-ups from maya while idle (the whole
        //     blink/RAF/idle-timeout machinery is bypassed; the
        //     last_edit_ms plumbing above becomes irrelevant);
        //   • IME candidate windows (CJK, dead keys) anchor at the
        //     true caret cell instead of wherever the hidden cursor
        //     happened to rest;
        //   • screen readers and terminal accessibility tools track
        //     the real cursor position;
        //   • the user's own cursor shape/color/blink settings apply.
        //
        // The blink-phase cache subtleties disappear too: with no
        // phase in the cells, the composer is frame-invariant while
        // idle, so the cross-frame component cache could serve it in
        // ANY state (build() still gates on active-state for painted
        // mode compatibility).
        bool hardware_caret = false;
    };

    explicit Composer(Config c)
        : cfg_sp_(std::make_shared<const Config>(std::move(c))) {}

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        // Cross-frame cache wrap. Engage ONLY when a stable id was
        // supplied AND the composer is in an active (streaming /
        // executing) state. Those are exactly the frames where (a) the
        // host re-runs view() on every delta — so an uncached composer
        // pays a full relayout per frame, the flicker source — and (b)
        // the cursor holds steady (no blink), so the composer is
        // genuinely frame-invariant and a cached blit is correct. While
        // idle the caret blinks (cells change ~every 530 ms) so caching
        // would freeze the blink; fall through to a live build there.
        const bool active = (cfg_sp_->state == State::Streaming
                          || cfg_sp_->state == State::ExecutingTool);
        if (!cfg_sp_->cache_id.empty() && active) {
            ComponentElement comp;
            comp.hash_id = cfg_sp_->cache_id;
            // Capture the shared config by ref-bump. The old code copied
            // the whole Config (text string included) into the closure on
            // EVERY build() call — a per-frame deep copy paid even on a
            // renderer cache HIT, where the closure is constructed but
            // never invoked. Same failure mode as the agent-timeline
            // per-event capture fix: closures built per frame must be
            // O(1) to construct; the O(content) work belongs inside the
            // render callback, which only runs on a cache miss.
            comp.render = [cfg = cfg_sp_](int, int) -> Element {
                return Composer{cfg}.build_impl();
            };
            return Element{std::move(comp)};
        }
        return build_impl();
    }

    [[nodiscard]] Element build_impl() const {
        using namespace dsl;
        // Local alias so the body reads through the shared config with
        // the historical `cfg_.` syntax (member is cfg_sp_ below).
        const Config& cfg_ = *cfg_sp_;

        const Color muted = Color::slot(ThemeSlot::Muted);
        const bool  has_text     = !cfg_.text.empty();
        const bool  is_awaiting  = (cfg_.state == State::AwaitingPermission);
        const bool  is_streaming = (cfg_.state == State::Streaming);
        const bool  is_executing = (cfg_.state == State::ExecutingTool);
        const bool  active       = is_streaming || is_executing;

        // ── State-driven box / prompt color.
        // LOOP outranks the idle colors but NOT the live-phase ones: while a
        // turn is actually streaming/executing the phase color is the more
        // urgent fact, and the ⟳ chip already says the loop is armed. When
        // idle-but-armed the border carries the loop hue, so the gap between
        // turns never looks like "nothing is going to happen".
        Color box_color =
            is_awaiting ? cfg_.warn_color :
            active      ? cfg_.active_color :
            cfg_.loop   ? cfg_.loop_color :
            has_text    ? cfg_.accent_color :
                          muted;

        // ── Cursor injection — blinking block.
        //
        // We can't drive the real terminal hardware cursor from inside a
        // widget, so the cursor is a glyph painted into the text. We
        // ALWAYS emit U+2588 FULL BLOCK ('█') and toggle its visibility
        // by swapping the STYLE between visible (fg=text) and invisible
        // (fg=bg, i.e. transparent against the box bg). Using one stable
        // byte sequence is critical: any phase-dependent byte length
        // would reflow downstream wrap caches (width vs bytes) and
        // cause a one-cell jitter on every blink — the symptom users
        // perceive as composer flicker. Style-only toggles never
        // reflow.
        //
        // Blink is suppressed while the agent is streaming or running
        // a tool: in those states the user can still type (input
        // queues), and a steady cursor reads as "yes, your keystrokes
        // are landing somewhere" rather than competing with the
        // spinner for the eye.
        //
        // RAF is requested ONLY when the blink phase actually toggles
        // a visible cell — i.e., the composer is idle (blink active)
        // AND a cursor will be painted (always true; placeholder path
        // also paints one). Calling RAF unconditionally would pin the
        // app to 2 Hz repaints even when nothing on screen changes.
        // Cursor blink driven by the maya animation framework. A 530 ms
        // square wave: visible for the first half of each period, hidden for
        // the second. anim::blink() owns the wall-clock read AND the wake
        // scheduling — it asks the loop for exactly ONE wake at the next
        // half-period boundary (~4 Hz), never a 60 fps re-arm, so an idle
        // composer costs one repaint per visible toggle. The phase is only
        // consulted (and frames only requested) while idle; a streaming/
        // executing composer holds a steady cursor.
        constexpr double kBlinkPeriodMs = 530.0;
        bool blink_off = false;
        if (!active && !cfg_.hardware_caret) {
            // (Hardware-caret mode skips ALL of this: the terminal owns
            // the blink, maya schedules nothing. The painted-mode
            // machinery below is untouched.)
            const std::int64_t now_ms = anim::default_clock().now_ms();
            // Mirror kitty's cursor_stop_blinking_after: once the user has
            // been idle (no edit) for 15 s, hold the cursor SOLID-visible
            // and stop requesting frames. The composer cell then stops
            // changing, so a GPU terminal with an aggressive repaint_delay
            // stops compositing — the idle-CPU fix. A default last_edit_ms
            // of 0 disables the timeout (legacy always-blink).
            constexpr std::int64_t kStopBlinkAfterMs = 15'000;
            const bool blink_expired =
                cfg_.last_edit_ms != 0
                && (now_ms - cfg_.last_edit_ms) >= kStopBlinkAfterMs;
            if (blink_expired) {
                // Solid cursor, NO frame request — the cell is now static
                // and nothing wakes the loop until real input arrives.
                blink_off = false;
            } else {
                blink_off = !anim::blink(kBlinkPeriodMs);
            }
        }

        std::string with_cursor = cfg_.text;
        // Clamp DOWN into range first (negative or past-the-end cursors
        // from host-side races must not throw std::out_of_range in
        // std::string::insert), then snap BACK to the nearest UTF-8
        // sequence boundary at or below the clamp. A byte offset that
        // lands inside a multi-byte sequence (host counts columns, or a
        // stale cursor survives an edit) would otherwise split the
        // sequence — the two halves each render as U+FFFD and the line
        // gains a phantom cell, shifting every glyph right of the caret.
        int cur = std::clamp<int>(cfg_.cursor, 0,
                                  static_cast<int>(cfg_.text.size()));
        while (cur > 0
               && (static_cast<unsigned char>(cfg_.text[static_cast<std::size_t>(cur)])
                   & 0xC0) == 0x80) {
            --cur;   // continuation byte → back up to the lead byte
        }
        // Always insert the FULL BLOCK byte sequence; visibility is a
        // style decision below. Same bytes every frame ⇒ same wrap.
        with_cursor.insert(static_cast<std::size_t>(cur),
                           "\xe2\x96\x88");

        // ── Prompt chip + body rows.
        Style prompt_style = (active || has_text || is_awaiting)
            ? Style{}.with_fg(box_color).with_bold()
            : Style{}.with_fg(box_color).with_dim();
        auto prompt_chip  = text("\xe2\x9d\xaf ", prompt_style);              // ❯
        auto continuation = text("\xe2\x94\x8a ", fg_dim_(muted));            // ┊
        auto blank_pre    = text("  ");

        std::vector<Element> body_rows;
        if (!has_text) {
            // When queued items exist and the composer is empty, lead
            // with the recall affordance — host's ↑ keybinding pulls
            // them back into the buffer for editing. Mirrors Claude
            // Code's "Press up to edit queued messages" hint (binary
            // offset 84591379). Suffix the contextual phase verb so
            // the user keeps the "type to add another" affordance
            // visible too.
            std::string placeholder;
            if (cfg_.queued > 0) {
                placeholder = cfg_.queued == 1
                    ? std::string{"press \xe2\x86\x91 to edit queued"}            // ↑
                    : std::string{"\xe2\x86\x91 drain queue • \xe2\x8c\xa5\xe2\x86\x91/\xe2\x8c\xa5\xe2\x86\x93 cycle items"};  // ↑ / ⌥↑ ⌥↓
                if (is_awaiting)       placeholder += " \xe2\x80\x94 awaiting permission above\xe2\x80\xa6";
                else if (is_executing) placeholder += " \xe2\x80\x94 type to queue another\xe2\x80\xa6";
                else if (is_streaming) placeholder += " \xe2\x80\x94 type to queue another\xe2\x80\xa6";
                else                   placeholder += " \xe2\x80\x94 or type a new message\xe2\x80\xa6";
            } else {
                placeholder =
                    is_awaiting  ? "awaiting permission \xe2\x80\x94 respond above\xe2\x80\xa6" :
                    is_executing ? "running tool \xe2\x80\x94 type to queue\xe2\x80\xa6"        :
                    is_streaming ? "streaming \xe2\x80\x94 type to queue\xe2\x80\xa6"           :
                                   "type a message\xe2\x80\xa6";
            }
            body_rows.push_back(h(
                prompt_chip,
                // Stable bytes: always emit █, toggle visibility via
                // style. Same rationale as the with-text path —
                // changing byte length on blink reflows wrap caches.
                // Hardware-caret mode: same bytes again (identical
                // geometry), but conceal + caret_anchor — paint
                // nothing, let the serializer put the REAL cursor here.
                // Shape/color state channel: see the with-text path
                // (idle = 0 = the USER'S configured cursor, untouched).
                text("\xe2\x96\x88",
                     cfg_.hardware_caret
                         ? [&] {
                               // Block caret, always (see the with-text
                               // path): blinking when idle, steady while
                               // the agent is busy / awaiting a decision.
                               Style s = Style{}
                                             .with_conceal()
                                             .with_caret_anchor()
                                             .with_caret_shape(
                                                 (is_awaiting || active)
                                                     ? uint8_t{2}
                                                     : uint8_t{1})
                                             .with_fg(box_color);
                               return s;
                           }()
                         : blink_off
                             ? Style{}.with_fg(muted).with_dim()
                             : Style{}.with_fg(muted)),
                text(placeholder, Style{}.with_fg(muted).with_italic())
            ).build());
        } else {
            // Style-driven cursor blink: split each line on the cursor
            // placeholder (which we just inserted as the FULL BLOCK
            // byte sequence) and emit the cursor as its own TextElement
            // whose foreground toggles between visible and the box
            // background. Byte layout is stable across the blink phase
            // — only the SGR color attribute changes — so word-wrap
            // never reflows.
            constexpr std::string_view kBlock = "\xe2\x96\x88";
            // Read-only: dim the body so the buffer reads as a readout the
            // user is watching, not a draft they are holding, and park the
            // caret out of sight (there is nothing to type into).
            const Style text_style    = cfg_.read_only
                ? Style{}.with_fg(cfg_.text_color).with_dim()
                : Style{}.with_fg(cfg_.text_color);
            const Style cursor_visible = cfg_.read_only
                ? Style{}.with_fg(box_color).with_dim()
                : Style{}.with_fg(cfg_.text_color);
            // "Invisible" cursor: dim foreground in the box color so
            // the cell stays the same width but reads as empty. Using
            // box bg directly would require knowing the parent's
            // resolved bg, which the widget doesn't have access to;
            // dimming to the box border color hides the glyph against
            // the box chrome reliably across themes.
            const Style cursor_hidden = Style{}.with_fg(box_color).with_dim();
            // Hardware-caret mode: the caret cell paints NOTHING
            // (conceal) and carries the anchor meta-bit; the inline
            // serializer moves + shows the REAL cursor there. Same
            // bytes as painted mode ⇒ identical wrap geometry.
            // The DECSCUSR shape is a STATE channel on the exact pixel
            // the user watches. A text input's caret reads as a BLOCK —
            // unconditionally, in every terminal and under tmux, and
            // regardless of the terminal's own configured cursor style
            // (an editor owns its caret's shape the way vim/emacs do).
            // Only the BLINK/steadiness carries state:
            //   idle            → 1 blinking block (ready for input)
            //   streaming/tool  → 2 steady block   (keystrokes queue)
            //   awaiting perm   → 2 steady block + warn color (decide)
            // fg = box_color: the caret picks up the phase color via
            // OSC 12 (concealed glyph, so fg is free to carry it).
            const uint8_t hw_shape = (is_awaiting || active) ? 2 : 1;
            Style cursor_hw = Style{}
                                  .with_conceal()
                                  .with_caret_anchor()
                                  .with_caret_shape(hw_shape)
                                  .with_fg(box_color);
            auto lines = split_lines(with_cursor);
            for (std::size_t i = 0; i < lines.size(); ++i) {
                Element prefix = (i == 0) ? prompt_chip
                                          : (lines.size() > 1 ? continuation : blank_pre);
                std::string_view line = lines[i];
                // Locate the cursor by its KNOWN byte offset, not by
                // searching for the block glyph. The user's own text can
                // legitimately contain █ (pasted progress bars, block
                // art); a find() would style the first such glyph as the
                // cursor — dimming user content to invisible on every
                // blink phase while the real caret rendered as plain
                // text. split_lines returns views into with_cursor, so
                // the line's offset is a pointer subtraction and the
                // cursor lands in this line iff `cur` falls inside its
                // byte range (the 3-byte block never spans lines — it
                // was inserted whole).
                const std::size_t line_off = static_cast<std::size_t>(
                    line.data() - with_cursor.data());
                const std::size_t cur_u = static_cast<std::size_t>(cur);
                const bool cursor_here =
                    cur_u >= line_off && cur_u < line_off + line.size();
                const std::size_t cur_pos =
                    cursor_here ? cur_u - line_off : std::string_view::npos;
                if (!cursor_here) {
                    TextElement te;
                    te.content = std::string{line};
                    te.style   = text_style;
                    te.wrap    = TextWrap::Wrap;
                    te.runs    = token_runs(line, 0, line.size(), line_off, text_style);
                    body_rows.push_back(h(prefix, Element{std::move(te)}).build());
                } else {
                    TextElement te;
                    te.content = std::string{line};
                    te.style   = text_style;
                    te.wrap    = TextWrap::Wrap;
                    te.runs = token_runs(line, 0, cur_pos, line_off, text_style);
                    te.runs.push_back(StyledRun{
                        .byte_offset = cur_pos,
                        .byte_length = kBlock.size(),
                        .style       = cfg_.hardware_caret
                                           ? cursor_hw
                                           : blink_off ? cursor_hidden
                                                       : cursor_visible});
                    for (auto& r : token_runs(line, cur_pos + kBlock.size(),
                                              line.size(), line_off, text_style))
                        te.runs.push_back(r);
                    body_rows.push_back(h(prefix, Element{std::move(te)}).build());
                }
            }
        }

        // ── Sizing: let the inner column size itself from its rendered
        // rows so soft-wrapped long lines actually push the composer's
        // top edge up. Earlier we pinned `height(rows)` from
        // `body_rows.size()` for jitter-stability, but that only counted
        // explicit '\n' breaks — a single long word-wrapped line clipped
        // to 1 row and overflowed visibly. The status bar pins the bottom
        // of the screen, so growing upward is the correct direction and
        // doesn't bob the user's eye.
        // Pad body to the configured floor with blank rows. Stable
        // composer height across short transient text changes
        // (empty placeholder → first char → empty again) means the
        // diff doesn't see a height delta on every keystroke, which
        // would otherwise push every row above by one canvas-Y and
        // force a full repaint via the per-row diff.
        while (static_cast<int>(body_rows.size()) < cfg_.min_body_rows) {
            body_rows.push_back(text(""));
        }
        auto inner = (v(std::move(body_rows)) | padding(0, 1)).build();

        // ── Hint row: width-adaptive left, ambient right.
        auto kbd = [tc = cfg_.text_color](const char* k) {
            return text(k, Style{}.with_fg(tc).with_bold());
        };
        auto lbl = [muted](const char* l) { return text(l, fg_dim_(muted)); };
        // Separator between hint-row clusters. Single-spaced: the row is a
        // dense ambient strip, and "  ·  " spent 5 columns on every join in
        // a row that sheds fragments under width pressure.
        auto dot = [muted]() { return text(" \xc2\xb7 ", fg_dim_(muted)); };

        // ── Width-adaptive hint clusters — MEASURED.
        //
        // Both sides progressively shed segments as available width
        // shrinks. The profile chip on the right is the only must-keep
        // anchor — everything else (newline on the left; queued /
        // words / tokens on the right) sheds in priority order once
        // the combined natural width can't fit.
        //
        // Every decision below measures the REAL styled fragments via
        // measure_element — there is no glyph-tally table to keep in
        // sync with the builders, so a relabeled hint or a wider glyph
        // re-decides by itself. Computing required-width vs avail every
        // frame keeps the row coherent across resizes, profile swaps,
        // and ticking word counters.
        auto hint_left_builder = [kbd, lbl, dot](int density) {
            std::vector<Element> out;
            out.push_back(kbd("\xe2\x86\xb5"));           // ↵
            out.push_back(lbl(" send"));
            if (density >= 1) {
                out.push_back(dot());
                out.push_back(kbd("\xe2\x87\xa7\xe2\x86\xb5 / \xe2\x8c\xa5\xe2\x86\xb5"));
                out.push_back(lbl(" newline"));
            }
            return out;
        };

        // Right-cluster ingredients passed into the lambda by value so
        // each frame's relayout can rebuild the cluster from a snapshot
        // of the current counts. Width-driven sheds happen INSIDE the
        // lambda where `w` is known.
        struct RightInputs {
            bool has_text;
            int queued;
            int words;
            int toks;
            bool loop;
            int loop_iterations;
            int loop_wait_secs;
            Color loop_color;
            Color highlight_color;
            Color muted_color;
            Color profile_color;
            std::string profile_label;
        };
        RightInputs ri{
            has_text,
            static_cast<int>(cfg_.queued),
            has_text ? (cfg_.word_estimate  >= 0 ? cfg_.word_estimate
                                                 : word_count(cfg_.text)) : 0,
            has_text ? (cfg_.token_estimate >= 0 ? cfg_.token_estimate
                                                 : approx_tokens(cfg_.text)) : 0,
            cfg_.loop,
            cfg_.loop_iterations,
            cfg_.loop_wait_secs,
            cfg_.loop_color,
            cfg_.highlight_color,
            muted,
            cfg_.profile.color,
            std::string{cfg_.profile.label},
        };

        Element hint_element = component(
            [hint_left_builder, ri, kbd, lbl, dot,
             status = cfg_.status, show_hints = cfg_.show_key_hints]
            (int w, int /*h*/) -> Element {
                using namespace dsl;

                // 2-col indent + trailing space + padding(0,1) on both
                // sides eat 6 cols of chrome — subtract before deciding
                // what each cluster can afford.
                constexpr int kChromeCols = 6;
                const int avail = std::max(0, w - kChromeCols);

                auto width_of = [w](const Element& el) {
                    return measure_element(el, w > 0 ? w : 1).width.value;
                };

                // The profile chip — the must-keep right anchor.
                std::vector<Element> chip_parts;
                chip_parts.push_back(text("\xe2\x96\x8e",
                    Style{}.with_fg(ri.profile_color)));
                chip_parts.push_back(text(" "));
                chip_parts.push_back(text(
                    small_caps_(ri.profile_label),
                    Style{}.with_fg(ri.profile_color).with_bold()));
                Element chip = h(std::move(chip_parts)).build();
                const int chip_cols = width_of(chip);

                // Left cluster. A caller-supplied `status` owns the slot
                // outright when present (it is the more informative thing
                // to say than a key the user already knows) and sheds
                // whole if the profile chip would not otherwise fit.
                // Otherwise fall back to the built-in key hints at the
                // richest density that still leaves room for the chip.
                std::vector<Element> left;
                const int status_cols = width_of(status);
                if (status_cols > 0) {
                    if (status_cols + chip_cols <= avail)
                        left.push_back(status);
                } else if (show_hints) {
                    for (int density = 1; density >= 0; --density) {
                        auto cand = hint_left_builder(density);
                        Element probe = h(cand).build();
                        if (density == 0
                            || width_of(probe) + chip_cols <= avail) {
                            left = std::move(cand);
                            break;
                        }
                    }
                }
                const int left_cols =
                    left.empty() ? 0 : width_of(h(left).build());
                const int budget = std::max(0, avail - left_cols);

                // Optional right segments, measured as the real fragments.
                //
                // Counts are NOT tabular-padded. Right-aligning a number to a
                // fixed column keeps it from twitching as it grows, which is
                // worth it for a value that changes every frame — but these
                // sit in a right-aligned cluster whose whole width already
                // shifts with the text, so the padding bought no stability
                // and cost four leading spaces at typical counts ("    1
                // words"). Separators are single-spaced for the same reason:
                // this is an ambient readout, not a table.
                Element queued_seg = h(
                    text("\xe2\x9d\x9a ", Style{}.with_fg(ri.highlight_color)),
                    text(std::to_string(ri.queued) + " queued",
                         Style{}.with_fg(ri.highlight_color).with_bold()),
                    dot()).build();
                // LOOP chip: ⟳ LOOP (×N once it has re-fired). Highest keep
                // priority of the optional segments — an app auto-sending on
                // the user's behalf must never render as an idle one, so this
                // sheds only after counters AND queued are already gone.
                Element loop_seg = h(
                    text("\xe2\x9f\xb3 ", Style{}.with_fg(ri.loop_color).with_bold()),
                    text(ri.loop_wait_secs > 0
                             ? "RETRY " + std::to_string(ri.loop_wait_secs) + "s"
                         : ri.loop_iterations > 0
                             ? "LOOP \xc3\x97" + std::to_string(ri.loop_iterations)
                             : std::string{"LOOP"},
                         Style{}.with_fg(ri.loop_color).with_bold()),
                    dot()).build();
                Element counters_seg = h(
                    text(std::to_string(ri.words) + "w",
                         fg_dim_(ri.muted_color)),
                    text(" \xc2\xb7 ", fg_dim_(ri.muted_color)),
                    text("~" + std::to_string(ri.toks) + " tok",
                         fg_dim_(ri.muted_color)),
                    dot()).build();

                // Counters need the most room so they shed first, then
                // queued; the chip never sheds — if even the chip can't
                // fit the row is degenerate at that width. LOOP outranks
                // both optional segments: it is the only one reporting that
                // the app will act on its own.
                const bool show_loop = ri.loop
                    && width_of(loop_seg) + chip_cols <= budget;
                const int loop_cols = show_loop ? width_of(loop_seg) : 0;
                const bool show_queued   = ri.queued > 0
                    && loop_cols + width_of(queued_seg) + chip_cols <= budget;
                const bool show_counters = ri.has_text
                    && loop_cols
                        + (show_queued ? width_of(queued_seg) : 0)
                        + width_of(counters_seg) + chip_cols <= budget;

                std::vector<Element> hint_right;
                if (show_loop)     hint_right.push_back(std::move(loop_seg));
                if (show_queued)   hint_right.push_back(std::move(queued_seg));
                if (show_counters) hint_right.push_back(std::move(counters_seg));
                hint_right.push_back(std::move(chip));

                // 2-col indent so the hint row lines up with body text:
                // `inner` has padding(0,1) (→1 col left), and body row 0
                // starts with the "❯ " prompt (→2 cols). Match that exactly
                // by adding padding(0,1) here and prepending 2 spaces so
                // ↵ sits directly under the first body character.
                return (h(
                    text("  "),
                    h(left),
                    spacer(),
                    h(hint_right),
                    text(" ")
                ) | padding(0, 1)).build();
            });

        // ── Box composition with optional bottom-right line-count caption.
        int line_count = cfg_.line_estimate >= 0
            ? cfg_.line_estimate
            : static_cast<int>(split_lines(cfg_.text).size());

        // ── Divider between body and hint row — a VERY THIN hairline:
        // the Dashed border's light triple-dash glyph (┄) reads far
        // airier than a solid rule, and .with_dim() drops it into the
        // chrome so the input area splits from the key hints / counters
        // / profile chip without a heavy bar cutting the box in two.
        // Left padding = 3 to clear `inner`'s padding(0,1) plus the
        // 2-col "❯ " prompt, so the rule's left end sits exactly under
        // the first body character. Right padding = 1 matches `inner`.
        auto rule = (Divider{DividerConfig{
            .line       = BorderStyle::Dashed,
            .line_style = Style{}.with_fg(box_color).with_dim(),
        }}.build() | padding(0, 1, 0, 3)).build();

        auto box = v(inner, std::move(rule), std::move(hint_element))
                   | border(BorderStyle::Round)
                   | bcolor(box_color)
                   // The box sits ON the canvas, so it carries the canvas
                   // colour. Without this it states a border and a border
                   // colour but no background, so every cell the box owns —
                   // the frame glyphs, the padding ring, the gap around the
                   // rule — falls back to the TERMINAL's colour. On a themed
                   // canvas that is the composer reading as a rectangular
                   // hole with the chrome above and below it painted.
                   //
                   // Under `native` this resolves to default_color() and
                   // nothing is stated, which is exactly what it replaces.
                   | bgc(theme::live().background);

        if (line_count > 1) {
            box = std::move(box) | btext(
                " " + std::to_string(line_count) + " lines ",
                BorderTextPos::Bottom, BorderTextAlign::End);
        }
        // No `| grow(1.0f)` here: the composer is a natural-height
        // sibling of the Thread inside AppLayout's column. The Thread
        // is the ONLY element that should grow vertically; giving the
        // composer a non-zero grow factor makes it compete with the
        // Thread for slack space, and during streaming — when the
        // Thread's natural height oscillates with every delta — the
        // composer's allocated rows oscillate too (visible as flicker)
        // or get squeezed to zero (composer disappears entirely until
        // a terminal resize forces a relayout). Width-fill stays
        // intact because AppLayout's column applies the parent's
        // default cross-axis Stretch.
        return box.build();
    }

private:
    // Runs for bytes [a, b) of one composer line: `@path` mentions in the
    // link colour, and a `/command` at the very start of the message in
    // bold accent. Colour only — no bytes change, so wrap and caret hold.
    // `line_off` is the line's offset in the whole buffer (a /command only
    // counts on the first line).
    std::vector<StyledRun> token_runs(std::string_view line, std::size_t a,
                                      std::size_t b, std::size_t line_off,
                                      Style base) const {
        std::vector<StyledRun> out;
        if (b <= a) return out;
        const Style mention_st = Style{}.with_fg(Color::slot(ThemeSlot::Link));
        const Style cmd_st     = Style{}.with_fg(cfg_sp_->accent_color).with_bold();
        auto ws = [](char c) { return c == ' ' || c == '\t'; };
        std::size_t at = a, i = a;
        auto flush = [&](std::size_t to) {
            if (to > at) out.push_back({at, to - at, base});
            at = to;
        };
        while (i < b) {
            const bool word_start = i == 0 || ws(line[i - 1]);
            const bool cmd  = word_start && i == 0 && line_off == 0 && line[i] == '/';
            const bool ment = word_start && line[i] == '@';
            if ((cmd || ment) && i + 1 < line.size() && !ws(line[i + 1])) {
                std::size_t e = i + 1;
                while (e < line.size() && !ws(line[e])) ++e;
                // The caret block can sit inside a token; split around it.
                e = std::min(e, b);
                flush(i);
                out.push_back({i, e - i, cmd ? cmd_st : mention_st});
                at = i = e;
                continue;
            }
            ++i;
        }
        flush(b);
        return out;
    }

    std::shared_ptr<const Config> cfg_sp_;

    // Rebuild-from-shared constructor used by the cached render closure —
    // ref-bump, no Config copy.
    explicit Composer(std::shared_ptr<const Config> c) : cfg_sp_(std::move(c)) {}

    // ── Helpers ───────────────────────────────────────────────────────────

    static std::vector<std::string_view> split_lines(std::string_view s) {
        std::vector<std::string_view> out;
        std::size_t start = 0;
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\n') {
                out.emplace_back(s.data() + start, i - start);
                start = i + 1;
            }
        }
        out.emplace_back(s.data() + start, s.size() - start);
        return out;
    }

    static int word_count(std::string_view s) {
        int n = 0;
        bool in_word = false;
        for (char c : s) {
            bool ws = (c == ' ' || c == '\t' || c == '\n' || c == '\r');
            if (!ws && !in_word) { ++n; in_word = true; }
            else if (ws)         { in_word = false; }
        }
        return n;
    }

    // ~4 bytes/token Claude heuristic — close enough for live counter UI.
    static int approx_tokens(std::string_view s) {
        return static_cast<int>((s.size() + 3) / 4);
    }

    // Right-aligned fixed-width int — keeps surrounding chips pinned as
    // counters tick.
    // Letter-spaced uppercase ("D O N E") for short labels.
    static std::string small_caps_(std::string_view s) {
        std::string out;
        out.reserve(s.size() * 2);
        for (std::size_t i = 0; i < s.size(); ++i) {
            char c = s[i];
            out.push_back(static_cast<char>(
                (c >= 'a' && c <= 'z') ? (c - 32) : c));
            // Only letter-space at UTF-8 sequence boundaries — a space
            // after every BYTE shreds multi-byte profile labels into
            // mojibake. Continuation bytes match (b & 0xC0) == 0x80.
            if (i + 1 < s.size()
                && (static_cast<unsigned char>(s[i + 1]) & 0xC0) != 0x80)
                out.push_back(' ');
        }
        return out;
    }

    // bright_black is already the "subdued secondary" tone — stacking
    // SGR `dim` on top can collapse below readability on some themes,
    // so suppress dim when the color is already bright_black.
    static Style fg_dim_(Color c) {
        const bool is_already_muted = theme::is_muted(c);
        return is_already_muted
            ? Style{}.with_fg(c)
            : Style{}.with_fg(c).with_dim();
    }
};

} // namespace maya
