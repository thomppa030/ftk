#include "ui/track_asset_editor_context.hpp"

#include "core/project.hpp"
#include "core/project_manager.hpp"
#include "environment/fog_track.hpp"
#include "core/string_utils.hpp"
#include "renderer/vulkan_context.hpp"
#include "ui/kit/feedback.hpp"
#include "ui/kit/icons.hpp"
#include "ui/panel_widget.hpp"

#include <filesystem>

namespace fjell {

TrackAssetEditorContext::TrackAssetEditorContext(TrackAssetEditorDeps deps,
                                                 std::vector<TrackLanes::Preset> presets)
    : deps_{deps}, presets_{std::move(presets)} {
    lanes_.set_note([this](std::string_view name) -> std::string {
        if (deps_.scene == nullptr || deps_.vulkan_context == nullptr) return {};
        return fog_track::problem(name, *deps_.scene, deps_.vulkan_context->pipeline_cache());
    });
    // A fog track is listed under the fog it reaches by the uniform it
    // drives; the asset's own under their preset's group; anything else is
    // custom.
    lanes_.set_placement([this](std::string_view name) -> TrackLanes::Placement {
        if (auto fog = fog_track::parse(name)) {
            return {fog->group.empty() ? std::string("Fog") : "Fog \xc2\xb7 " + fog->group, display_name(fog->uniform)};
        }
        for (const auto& preset : presets_) {
            if (preset.name == name) return {preset.group, display_name(preset.name)};
        }
        return {"Custom", display_name(std::string(name))};
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
    lanes_.show(path);
}

void TrackAssetEditorContext::draw(float dt) {
    name_preview(viewport_manager_);
    viewport_manager_.draw(*deps_.vulkan_context, *deps_.input, dt);

    if (loaded_) {
        // The fog tracks on offer are whatever uniforms the scene's fog
        // materials declare right now, so they follow a material swap or a
        // shader edit without the editor being reopened.
        auto presets = presets_;
        if (deps_.scene != nullptr && deps_.vulkan_context != nullptr) {
            for (const auto& fog : fog_track::presets(*deps_.scene, deps_.vulkan_context->pipeline_cache())) {
                const auto target = fog_track::parse(fog.name);
                presets.push_back({fog.name,
                                   target && !target->group.empty() ? "Fog \xc2\xb7 " + target->group : "Fog",
                                   fog.color ? TrackKind::Color : TrackKind::Float,
                                   fog.name.ends_with(fog_track::DENSITY_UNIFORM) ? TrackMode::Multiply
                                                                                  : TrackMode::Override,
                                   fog.range_min, fog.range_max});
            }
        }
        lanes_.set_presets(std::move(presets));

        sync_timeline();
        Connection scrub = lanes_.on_scrub.bind([this](float t) { scrubbed(t); });
        if (lanes_.draw(ctx_title("Timeline").c_str(), tracks(), dt)) push_live();
    }

    if (auto panel = Panel(ui::icon::properties, ctx_title("Properties").c_str())) {
        if (loaded_) {
            draw_status();
            if (lanes_.draw_selection(tracks())) push_live();
            draw_properties();
        } else {
            ui::empty_state(ui::icon::warning, "The file couldn't be read", "The console says why");
        }
    }
    draw_shared_panels(dt);
}

} // namespace fjell
