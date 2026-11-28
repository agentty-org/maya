#pragma once
// maya::widget::waterfall — Timing waterfall chart (devtools style)
//
// Horizontal bars offset by start time, showing concurrent and sequential
// work like browser devtools network tab.
//
//   Waterfall wf;
//   wf.add({"Read auth.ts",   0.0f, 1.2f});
//   wf.add({"Edit auth.ts",   0.5f, 2.0f});
//   wf.add({"Run tests",      2.5f, 3.5f, {}, TaskStatus::InProgress});
//   auto ui = wf.build();

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../dsl.hpp"
#include "../style/style.hpp"
#include "agentty/plan_view.hpp"  // TaskStatus

namespace maya {

struct WaterfallEntry {
    std::string label;
    float       start    = 0.f;
    float       duration = 0.f;
    Color       color    = Color::slot(ThemeSlot::Primary);
    TaskStatus  status   = TaskStatus::Completed;
};

class Waterfall {
    std::vector<WaterfallEntry> entries_;
    int   bar_width_   = 30;
    float time_scale_  = 0.f;
    bool  show_labels_ = true;
    bool  show_times_  = true;
    int   frame_       = 0;

    static const char* spinner(int frame) {
        static constexpr const char* frames[] = {
            "\xe2\xa0\x8b", "\xe2\xa0\x99", "\xe2\xa0\xb9",
            "\xe2\xa0\xb8", "\xe2\xa0\xbc", "\xe2\xa0\xb4",
            "\xe2\xa0\xa6", "\xe2\xa0\xa7", "\xe2\xa0\x87",
            "\xe2\xa0\x8f",
        };
        return frames[frame % 10];
    }

public:
    Waterfall() = default;

    void set_bar_width(int w) { bar_width_ = std::max(4, w); }
    void set_time_scale(float s) { time_scale_ = s; }
    void set_show_labels(bool b) { show_labels_ = b; }
    void set_show_times(bool b) { show_times_ = b; }
    void set_frame(int f) { frame_ = f; }

    void add(WaterfallEntry e) { entries_.push_back(std::move(e)); }
    void clear() { entries_.clear(); }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;

        if (entries_.empty()) return text("");

        auto txt   = Style{};
        auto muted = Style{}.with_dim();
        auto dim   = Style{}.with_dim();

        int w = std::max(4, bar_width_);

        // Auto-scale
        float scale = time_scale_;
        if (scale <= 0.f) {
            for (auto& e : entries_)
                scale = std::max(scale, e.start + e.duration);
            if (scale <= 0.f) scale = 1.f;
        }

        // Max label width for column alignment
        int max_label = 0;
        if (show_labels_) {
            for (auto& e : entries_)
                max_label = std::max(max_label, static_cast<int>(e.label.size()));
        }

        std::vector<Element> rows;

        // ── Time axis header ─────────────────────────────────────────
        if (show_labels_) {
            std::vector<Element> axis;
            // Label column space
            axis.push_back(text(std::string(static_cast<size_t>(max_label + 2), ' ')));

            // Time markers
            std::string line(static_cast<size_t>(w), ' ');
            for (int q = 0; q <= 4; ++q) {
                float t = scale * q / 4.0f;
                char buf[16];
                if (t < 1.0f)
                    std::snprintf(buf, sizeof(buf), "%dms", static_cast<int>(t * 1000));
                else
                    std::snprintf(buf, sizeof(buf), "%.1fs", static_cast<double>(t));
                int pos = static_cast<int>(static_cast<float>(w) * q / 4.0f);
                std::string lbl = buf;
                int start = (q == 0) ? pos
                          : (q == 4) ? std::max(0, pos - static_cast<int>(lbl.size()))
                          : std::max(0, pos - static_cast<int>(lbl.size()) / 2);
                start = std::min(start, w - static_cast<int>(lbl.size()));
                if (start < 0) start = 0;
                for (int j = 0; j < static_cast<int>(lbl.size()) && start + j < w; ++j)
                    line[static_cast<size_t>(start + j)] = lbl[static_cast<size_t>(j)];
            }
            axis.push_back(text(std::move(line), Style{}.with_dim()));
            rows.push_back(h(std::move(axis)).build());
        }

        // ── Entry rows ───────────────────────────────────────────────
        for (auto& entry : entries_) {
            std::vector<Element> parts;

            // Label column (right-padded)
            if (show_labels_) {
                std::string lbl = entry.label;
                if (static_cast<int>(lbl.size()) < max_label)
                    lbl.append(static_cast<size_t>(max_label) - lbl.size(), ' ');
                parts.push_back(text(std::move(lbl), txt));
                parts.push_back(text("  "));
            }

            // Bar positions
            float start_frac = std::clamp(entry.start / scale, 0.f, 1.f);
            float end_frac   = std::clamp((entry.start + entry.duration) / scale, 0.f, 1.f);
            int bar_start = static_cast<int>(start_frac * w);
            int bar_end   = static_cast<int>(end_frac * w);
            if (bar_end <= bar_start && entry.duration > 0.f) bar_end = bar_start + 1;
            bar_end = std::min(bar_end, w);

            int pre_len    = bar_start;
            int filled_len = bar_end - bar_start;
            int post_len   = w - bar_end;

            // Pre-bar track
            if (pre_len > 0) {
                std::string s;
                for (int i = 0; i < pre_len; ++i) s += "\xe2\x96\x91"; // ░
                parts.push_back(text(std::move(s), dim));
            }

            // Filled bar
            if (filled_len > 0) {
                bool in_progress = (entry.status == TaskStatus::InProgress);
                int solid = (in_progress && filled_len > 1) ? filled_len - 1 : filled_len;

                if (solid > 0) {
                    std::string s;
                    for (int i = 0; i < solid; ++i) s += "\xe2\x96\x88"; // █
                    parts.push_back(text(std::move(s), Style{}.with_fg(entry.color)));
                }
                if (in_progress)
                    parts.push_back(text(spinner(frame_), Style{}.with_fg(entry.color).with_bold()));
            }

            // Post-bar track
            if (post_len > 0) {
                std::string s;
                for (int i = 0; i < post_len; ++i) s += "\xe2\x96\x91"; // ░
                parts.push_back(text(std::move(s), dim));
            }

            // Duration text
            if (show_times_) {
                double dur = static_cast<double>(entry.duration);
                char buf[16];
                if (dur < 1.0)
                    std::snprintf(buf, sizeof(buf), " %3.0fms", dur * 1000.0);
                else
                    std::snprintf(buf, sizeof(buf), " %.1fs", dur);
                parts.push_back(text(std::string(buf), muted));
            }

            rows.push_back(h(std::move(parts)).build());
        }

        return v(std::move(rows)).build();
    }
};

} // namespace maya
