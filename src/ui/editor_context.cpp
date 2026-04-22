#include "ui/editor_context.hpp"
#include "ui/console.hpp"
#include "core/log.hpp"

namespace fjell {

void EditorContext::draw_shared_panels(float dt) {
    auto stats_title = ctx_title("Stats");
    auto history_title = ctx_title("History");
    auto console_title = ctx_title("Console");
    auto settings_title = ctx_title("Project Settings");

    stats_panel_.draw(dt, stats_title.c_str());
    history_panel_.draw(history_title.c_str());
    project_settings_panel_.draw(settings_title.c_str());
    if (auto& sink = log::console_sink(); sink) {
        sink->draw(console_title.c_str());
    }
}

} // namespace fjell
