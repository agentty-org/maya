#pragma once
// maya::widget::timeline — Vertical event timeline (CI/pipeline style)
//
// Displays events with status icons, connectors, optional duration bars,
// and animated spinners for in-progress items.
//
//   Timeline tl;
//   tl.add({"Read auth middleware", "Read 42 lines", "1.2s", TaskStatus::Completed});
//   tl.add({"Edit auth.ts", "Replaced session middleware", "2.0s",
//           TaskStatus::InProgress, 10});
//   tl.add({"Run tests", "", "", TaskStatus::Pending});
//   auto ui = tl.build();

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "../dsl.hpp"
#include "../style/style.hpp"
#include "agentty/plan_view.hpp"  // TaskStatus

namespace maya {

struct TimelineEvent {
    std::string label;
    std::string detail    = "";
    std::string duration  = "";
    TaskStatus  status    = TaskStatus::Pending;
    int         bar_width = 0;  // 0 = no bar, 1-20 = bar width
};

class Timeline {
    std::vector<TimelineEvent> events_;
    bool show_connector_ = true;
    bool compact_        = false;
    int  frame_          = 0;
    int  track_width_    = 40;

    static constexpr Color pending_color    = Color::slot(ThemeSlot::Muted);
    static constexpr Color inprogress_color = Color::slot(ThemeSlot::Primary);
    static constexpr Color completed_color  = Color::slot(ThemeSlot::Success);

    static Color status_color(TaskStatus s) {
        switch (s) {
            case TaskStatus::Completed:  return completed_color;
            case TaskStatus::InProgress: return inprogress_color;
            case TaskStatus::Pending:    return pending_color;
        }
        return pending_color;
    }

    static const char* spinner(int frame) {
        static constexpr const char* frames[] = {
            "\xe2\xa0\x8b", "\xe2\xa0\x99", "\xe2\xa0\xb9",
            "\xe2\xa0\xb8", "\xe2\xa0\xbc", "\xe2\xa0\xb4",
            "\xe2\xa0\xa6", "\xe2\xa0\xa7", "\xe2\xa0\x87",
            "\xe2\xa0\x8f",
        };
        return frames[frame % 10];
    }

    static std::string make_dots(int count) {
        std::string s;
        for (int i = 0; i < count; ++i) s += "\xc2\xb7"; // ·
        return s;
    }

public:
    Timeline() = default;

    void set_show_connector(bool b) { show_connector_ = b; }
    void set_compact(bool b) { compact_ = b; }
    void set_frame(int f) { frame_ = f; }
    void set_track_width(int w) { track_width_ = w; }

    void add(TimelineEvent ev) { events_.push_back(std::move(ev)); }
    void clear() { events_.clear(); }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;

        auto txt   = Style{};
        auto muted = Style{}.with_dim();
        auto dim   = Style{}.with_dim();

        std::vector<Element> rows;

        for (std::size_t i = 0; i < events_.size(); ++i) {
            auto& ev = events_[i];
            Color sc = status_color(ev.status);
            bool is_last = (i + 1 == events_.size());

            std::vector<Element> parts;

            // Indent
            parts.push_back(text("  "));

            // Status icon
            if (ev.status == TaskStatus::InProgress) {
                parts.push_back(text(spinner(frame_), Style{}.with_fg(sc).with_bold()));
                parts.push_back(text("\xe2\x94\x80", Style{}.with_fg(sc))); // ─
            } else if (ev.status == TaskStatus::Completed) {
                parts.push_back(text("\xe2\x9c\x93", Style{}.with_fg(sc).with_bold())); // ✓
                parts.push_back(text(" "));
            } else {
                parts.push_back(text("\xe2\x97\x8b", Style{}.with_fg(sc))); // ○
                parts.push_back(text(" "));
            }

            // Label
            auto label_style = (ev.status == TaskStatus::Pending) ? muted : txt;
            if (ev.status == TaskStatus::Completed)
                label_style = label_style.with_dim();
            parts.push_back(text(ev.label, label_style));

            // Dot track + duration
            if (!ev.duration.empty()) {
                int label_len = static_cast<int>(ev.label.size());
                int dur_len   = static_cast<int>(ev.duration.size());
                int dots_len  = track_width_ - label_len - dur_len;
                if (dots_len < 2) dots_len = 2;

                parts.push_back(text(" " + make_dots(dots_len - 1) + " ", dim));
                parts.push_back(text(ev.duration, muted));
            }

            // Duration bar
            if (ev.bar_width > 0) {
                int bw = std::clamp(ev.bar_width, 1, 20);
                int filled = (ev.status == TaskStatus::Completed) ? bw
                           : (ev.status == TaskStatus::InProgress)
                               ? static_cast<int>(bw * 0.7)
                               : 0;
                int empty_w = bw - filled;

                parts.push_back(text("  "));

                if (filled > 0) {
                    std::string s;
                    for (int b = 0; b < filled; ++b) s += "\xe2\x96\x88"; // █
                    parts.push_back(text(std::move(s), Style{}.with_fg(sc)));
                }
                if (empty_w > 0) {
                    std::string s;
                    for (int b = 0; b < empty_w; ++b) s += "\xe2\x96\x91"; // ░
                    parts.push_back(text(std::move(s), dim));
                }
            }

            rows.push_back(h(std::move(parts)).build());

            // Connector + detail
            if (!is_last && show_connector_) {
                if (!compact_ && !ev.detail.empty()) {
                    rows.push_back(h(
                        text("  \xe2\x94\x82   ", dim), // │
                        text(ev.detail, muted)
                    ).build());
                } else if (!compact_) {
                    rows.push_back(text("  \xe2\x94\x82", dim)); // │
                }
            } else if (!is_last && !compact_ && !ev.detail.empty()) {
                rows.push_back(h(
                    text("      "),
                    text(ev.detail, muted)
                ).build());
            }
        }

        return v(std::move(rows)).build();
    }
};

} // namespace maya
