#include "ui/track_asset_editor_context.hpp"

#include "core/project.hpp"
#include "core/project_manager.hpp"
#include "environment/fog_track.hpp"
#include "renderer/vulkan_context.hpp"
#include "ui/kit/feedback.hpp"
#include "ui/kit/icons.hpp"
#include "ui/panel_widget.hpp"

#include <filesystem>

namespace fjell {

TrackAssetEditorContext::TrackAssetEditorContext(TrackAssetEditorDeps deps,
                                                 std::vector<TrackTimelineEditor::Preset> presets)
    : deps_{deps}, presets_{std::move(presets)} {
    tracks_.set_note([this](std::string_view name) -> std::string {
        if (deps_.scene == nullptr || deps_.vulkan_context == nullptr) return {};
        return fog_track::problem(name, *deps_.scene, deps_.vulkan_context->pipeline_cache());
    });
}

EditorContext::DockPreset TrackAssetEditorContext::dock_preset() const {
    return {.preview = {"Preview"}, .under_preview = {"Timeline"}, .inspector = {"Properties"}};
}

void TrackAssetEditorContext::init() {
    viewport_manager_.init(deps_.vulkan_context->device_ref(),
                           deps_.vulkan_context->allocator(),
                           deps_.vulkan_context->msaa_samples());
    viewport_manager_.add_viewport(ViewportSource::final_color, "Preview");
    init_shared_panels(deps_.vulkan_context);
    history_panel().init(&command_history());
    timeline_.set_title(ctx_title("Timeline"));
}

void TrackAssetEditorContext::shutdown() {
    viewport_manager_.release_textures();
}

std::vector<ViewportRenderRequest> TrackAssetEditorContext::build_render_requests() {
    // The scene's own sky, not the studio backdrop: the sky is part of what
    // these assets shape.
    return viewport_manager_.build_render_requests();
}

void TrackAssetEditorContext::apply_resize(VulkanContext& vk) {
    viewport_manager_.apply_resize(vk);
}

void TrackAssetEditorContext::open(const std::string& path) {
    path_ = path;
    abs_path_ = deps_.project_mgr != nullptr && deps_.project_mgr->active() != nullptr
        ? deps_.project_mgr->active()->resolve(path)
        : path;
    name_ = std::filesystem::path(path).stem().string();
    loaded_ = load();
    if (loaded_) document_opened();
}

void TrackAssetEditorContext::draw(float dt) {
    name_preview(viewport_manager_);
    viewport_manager_.draw(*deps_.vulkan_context, *deps_.input, dt);

    if (loaded_) sync_timeline();
    Connection scrub = timeline_.on_scrub.bind([this](float t) { scrubbed(t); });
    timeline_.draw(dt);

    if (auto panel = Panel(ui::icon::properties, ctx_title("Properties").c_str())) {
        if (loaded_) {
            draw_properties();
        } else {
            ui::empty_state(ui::icon::warning, "The file couldn't be read", "The console says why");
        }
    }
    draw_shared_panels(dt);
}

void TrackAssetEditorContext::draw_tracks(TrackSet& tracks, float cursor,
                                          const std::function<std::string(float)>& format_time) {
    auto presets = presets_;
    if (deps_.scene != nullptr && deps_.vulkan_context != nullptr) {
        for (const auto& fog : fog_track::presets(*deps_.scene, deps_.vulkan_context->pipeline_cache())) {
            presets.push_back({fog.name, fog.color ? TrackKind::Color : TrackKind::Float,
                               fog.name.ends_with(fog_track::DENSITY_UNIFORM) ? TrackMode::Multiply
                                                                              : TrackMode::Override,
                               fog.range_min, fog.range_max});
        }
    }
    tracks_.set_presets(std::move(presets));
    if (tracks_.draw("##tracks", tracks, cursor, format_time)) push_live();
}

} // namespace fjell
