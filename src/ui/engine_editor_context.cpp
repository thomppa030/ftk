#include "ui/engine_editor_context.hpp"
#include "ui/viewport_manager.hpp"

namespace fjell {

std::vector<std::string> EngineEditorContext::bottom_tabs() const {
    return {"Console", "Stats", "History"};
}

void EngineEditorContext::draw_shared_panels(float dt) {
    const auto stats_title = ctx_title("Stats");
    stats_panel_.draw(dt, stats_title.c_str());
    ftk::EditorContext::draw_shared_panels(dt);
}

void EngineEditorContext::name_preview(ViewportManager& viewports) const {
    if (auto* panel = viewports.active_panel()) {
        auto title = ctx_title("Preview");
        if (panel->title != title) panel->title = std::move(title);
    }
}

} // namespace fjell
