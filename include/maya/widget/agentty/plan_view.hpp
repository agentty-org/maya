#pragma once
// maya::widget::plan_view — Task/plan display with status icons
//
// Displays a list of tasks with status indicators.
//
// Usage:
//   PlanView plan;
//   plan.add("Analyze codebase", TaskStatus::InProgress);
//   plan.add("Read configuration files", TaskStatus::Completed);
//   plan.add("Write implementation");
//   auto ui = plan.build();

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "../../dsl.hpp"
#include "../../style/style.hpp"

namespace maya {

enum class TaskStatus : uint8_t { Pending, InProgress, Completed };

struct PlanView {
    struct Task {
        std::string label;
        TaskStatus status = TaskStatus::Pending;
    };

    std::vector<Task> tasks;

    void add(std::string label, TaskStatus status = TaskStatus::Pending) {
        tasks.push_back({std::move(label), status});
    }

    void set_status(size_t index, TaskStatus status) {
        if (index < tasks.size()) tasks[index].status = status;
    }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;

        constexpr auto pending_color    = Color::slot(ThemeSlot::Muted);
        constexpr auto inprogress_color = Color::slot(ThemeSlot::Primary);
        constexpr auto completed_color  = Color::slot(ThemeSlot::Success);

        std::vector<Element> rows;
        rows.reserve(tasks.size());

        for (auto const& task : tasks) {
            const char* icon;
            Style icon_style;
            Style text_style;

            switch (task.status) {
            case TaskStatus::Pending:
                icon = "\xe2\x97\x8b"; // ○
                icon_style = Style{}.with_fg(pending_color);
                text_style = Style{}.with_fg(pending_color);
                break;
            case TaskStatus::InProgress:
                icon = "\xe2\x97\x8f"; // ●
                icon_style = Style{}.with_fg(inprogress_color).with_bold();
                text_style = Style{}.with_fg(inprogress_color);
                break;
            case TaskStatus::Completed:
                icon = "\xe2\x9c\x93"; // ✓
                icon_style = Style{}.with_fg(completed_color);
                text_style = Style{}.with_fg(completed_color).with_dim().with_strikethrough();
                break;
            }

            rows.push_back(h(
                text("  "),
                text(icon, icon_style),
                text(" "),
                text(task.label, text_style)
            ).build());
        }

        return v(std::move(rows)).build();
    }
};

} // namespace maya
