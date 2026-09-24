#include "ui/material_editor_context.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"
#include "core/project_manager.hpp"
#include "core/string_utils.hpp"
#include "platform/platform.hpp"
#include "renderer/bindless.hpp"
#include "renderer/components/mesh_component.hpp"
#include "renderer/pipeline_cache.hpp"
#include "renderer/resources/fjsl_parser.hpp"
#include "renderer/resources/material.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "renderer/vulkan_context.hpp"
#include "scene/component_meta.hpp"
#include "ui/asset_ref_widget.hpp"
#include "ui/asset_thumbnail_renderer.hpp"
#include "ui/icon_cache.hpp"
#include "ui/kit/button.hpp"
#include "ui/kit/feedback.hpp"
#include "ui/kit/icons.hpp"
#include "ui/kit/row.hpp"
#include "ui/material_params_widget.hpp"
#include "ui/panel_widget.hpp"
#include "ui/shader_error_callout.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <cstdio>
#include <filesystem>

namespace fjell {

MaterialEditorContext::MaterialEditorContext(MaterialEditorDeps deps)
    : deps_{deps} {}

EditorContext::DockPreset MaterialEditorContext::dock_preset() const {
    return {.preview = {"Preview"}, .inspector = {"Parameters", "Textures"}};
}

void MaterialEditorContext::init() {
    viewport_manager_.init(deps_.vulkan_context->device_ref(),
                           deps_.vulkan_context->allocator(),
                           deps_.vulkan_context->msaa_samples());
    viewport_manager_.add_viewport(ViewportSource::final_color, "Preview");
    init_shared_panels(deps_.vulkan_context);
    history_panel().init(&command_history());
}

void MaterialEditorContext::shutdown() {
    viewport_manager_.release_textures();
}

void MaterialEditorContext::draw(float dt) {
    // Update viewport title with context suffix
    name_preview(viewport_manager_);
    preview_scene_.compute_world_matrices(nullptr);
    viewport_manager_.draw(*deps_.vulkan_context, *deps_.input, dt);
    draw_params_panel();
    draw_texture_panel();
    draw_shared_panels(dt);
}

bool MaterialEditorContext::handle_shortcuts() {
    return false;
}

std::vector<ViewportRenderRequest> MaterialEditorContext::build_render_requests() {
    return preview_render_requests(viewport_manager_.build_render_requests());
}

void MaterialEditorContext::apply_resize(VulkanContext& vk) {
    viewport_manager_.apply_resize(vk);
}

// ── Open / Save ──────────────────────────────────────────────────────────

void MaterialEditorContext::open_material(const std::string& fjmat_path) {
    fjmat_path_ = fjmat_path;
    fjmat_abs_path_ = base_path::resolve_asset(fjmat_path, deps_.project_mgr->active());

    // Load the .fjmat asset
    asset_ = load_material_asset(fjmat_abs_path_);

    auto result = deps_.resource_manager->load_material_asset(
        fjmat_abs_path_, deps_.project_mgr->active());
    if (!result) {
        FJELL_CORE_ERROR("Failed to load material: {}", result.error());
        return;
    }
    material_handle_ = *result;
    material_ = deps_.resource_manager->get(material_handle_);
    if (!material_) return;

    // Set tab name to filename
    name_ = std::filesystem::path(fjmat_path).stem().string();

    setup_preview_scene();

    viewport_manager_.frame({0.0f, 0.0f, 0.0f}, 1.5f);

    document_opened();
    FJELL_CORE_INFO("Material editor opened: {}", fjmat_path);
}

MaterialAsset MaterialEditorContext::current_asset() const {
    MaterialAsset asset = asset_;
    if (!material_) return asset;

    auto* cache = deps_.vulkan_context->pipeline_cache();
    auto& bindless = deps_.vulkan_context->bindless_textures();
    uint32_t pid = material_->pipeline_id();

    if (pid < cache->shader_count()) {
        const auto& meta = cache->metadata(pid);
        const auto& gpu = bindless.material_at(material_->material_id());

        for (const auto& u : meta.uniforms) {
            if (u.type == UniformType::Sampler2D) continue;
            uint32_t fcount = uniform_float_count(u.type);
            std::vector<float> vals(fcount);
            for (uint32_t i = 0; i < fcount; ++i) {
                vals[i] = bindless.read_param(gpu.param_offset + u.slot + i);
            }
            decode_uniform_from_gpu(u, vals);
            asset.params[u.name] = vals;
        }
        write_base_params(asset, {
            .base_color_factor = material_->params().base_color_factor,
            .metallic_factor = gpu.base_params[4],
            .roughness_factor = gpu.base_params[5],
            .uv_scale = {gpu.base_params[7], gpu.base_params[8]},
        });
    }
    return asset;
}

std::string MaterialEditorContext::document() const {
    if (fjmat_path_.empty()) return {};
    return material_asset_text(current_asset());
}

Result<> MaterialEditorContext::write() {
    if (fjmat_abs_path_.empty() || !material_) return make_error("no material is open");
    asset_ = current_asset();
    try {
        save_material_asset(fjmat_abs_path_, asset_);
    } catch (const std::exception& e) {
        return make_error(e.what());
    }
    if (deps_.asset_thumbnails) deps_.asset_thumbnails->refresh(fjmat_abs_path_);
    FJELL_CORE_INFO("Material saved: {}", fjmat_abs_path_);
    return {};
}

Result<> MaterialEditorContext::restore(const std::string& text) {
    MaterialAsset asset;
    try {
        asset = material_asset_from_text(text);
    } catch (const std::exception& e) {
        return make_error(e.what());
    }
    if (!material_) return make_error("no material is open");

    if (asset.shader != asset_.shader || asset.textures != asset_.textures) {
        asset_ = std::move(asset);
        save_and_reload();
        return {};
    }
    asset_ = asset;
    auto* cache = deps_.vulkan_context->pipeline_cache();
    const uint32_t pid = material_->pipeline_id();
    if (pid < cache->shader_count()) {
        apply_material_values(cache->metadata(pid), deps_.vulkan_context->bindless_textures(), *material_, asset);
    }
    return {};
}

void MaterialEditorContext::save_and_reload() {
    if (auto saved = save(); !saved) {
        FJELL_CORE_ERROR("Couldn't save {}: {}", fjmat_path_, saved.error());
        return;
    }
    auto result = deps_.resource_manager->load_material_asset(fjmat_abs_path_, deps_.project_mgr->active());
    if (result) {
        material_handle_ = *result;
        material_ = deps_.resource_manager->get(material_handle_);
        setup_preview_scene();
    }
}

// ── Preview scene ────────────────────────────────────────────────────────

void MaterialEditorContext::setup_preview_scene() {
    preview_scene_.clear();

    // Only a spatial material has a surface pipeline to put on a sphere. The
    // rest are drawn by a pass of their own, from geometry a sphere does not
    // have; draw_params_panel says so and points at where each is previewed.
    if (material_ && material_->shader_type() != ShaderType::Spatial) return;

    auto sphere = deps_.resource_manager->create_primitive("sphere");
    if (!sphere) return;

    preview_node_id_ = preview_scene_.add(
        *sphere, material_handle_, *deps_.resource_manager,
        {}, "Preview Sphere", "primitive:sphere", fjmat_path_);

    preview_scene_.compute_world_matrices(nullptr);
}

// ── Panels ───────────────────────────────────────────────────────────────

void MaterialEditorContext::draw_params_panel() {
    auto panel = Panel(ui::icon::properties, ctx_title("Parameters").c_str());
    if (!panel) return;
    if (!material_) {
        ui::empty_state(ui::icon::material, "No material open", "Open one from the content browser");
        return;
    }

    auto* cache = deps_.vulkan_context->pipeline_cache();
    const uint32_t pid = material_->pipeline_id();
    const Project* project = deps_.project_mgr != nullptr ? deps_.project_mgr->active() : nullptr;
    shader_error_callout(*cache, pid, project);

    // Only a spatial material goes on the preview sphere; the rest are drawn
    // by a pass of their own from geometry the sphere does not have. Say
    // where to look instead: a blank preview reads as a broken material.
    const char* elsewhere = nullptr;
    switch (material_->shader_type()) {
        case ShaderType::Spatial: break;
        case ShaderType::Ui:
            elsewhere = "A UI shader doesn't draw on the preview. Put the material on a node and "
                        "open that widget in the UI editor.";
            break;
        case ShaderType::Terrain:
            elsewhere = "A terrain shader doesn't draw on the preview. Put the material on a "
                        "Terrain component and look at the scene.";
            break;
        case ShaderType::Water:
            elsewhere = "A water shader doesn't draw on the preview: its surface comes from the "
                        "node's .fjwater. Put the material on a Water component and look at the scene.";
            break;
        case ShaderType::Sky:
            elsewhere = "A sky shader doesn't draw on the preview. Set it as the scene's sky and "
                        "look at the scene.";
            break;
        case ShaderType::Fog:
            elsewhere = "A fog shader doesn't draw on the preview. Put the material on the "
                        "scene's Volumetric Fog node and look at the scene.";
            break;
    }
    if (elsewhere != nullptr) ui::status(ui::Severity::Warning, elsewhere);

    if (pid < cache->shader_count()) {
        (void)draw_material_params(cache->metadata(pid), deps_.vulkan_context->bindless_textures(), *material_);
    }
}

void MaterialEditorContext::draw_texture_panel() {
    auto panel = Panel(ui::icon::texture, ctx_title("Textures").c_str());
    if (!panel) return;
    if (!material_) {
        ui::empty_state(ui::icon::material, "No material open", "Open one from the content browser");
        return;
    }

    // The fixed PBR slots on a surface material, then the textures the
    // shader declares; ui and terrain shaders sample only their own.
    std::vector<std::string> slots;
    if (material_->shader_type() == ShaderType::Spatial) {
        slots = {"base_color", "normal_map", "metallic_roughness", "emissive"};
    }
    auto* cache = deps_.vulkan_context->pipeline_cache();
    const uint32_t pid = material_->pipeline_id();
    if (pid < cache->shader_count()) {
        for (const auto& u : cache->metadata(pid).uniforms) {
            if (u.type == UniformType::Sampler2D) slots.push_back(u.name);
        }
    }

    InspectorContext ictx{};
    ictx.project_mgr = deps_.project_mgr;
    ictx.icons = deps_.icon_cache;
    static const std::string texture_exts[] = {".png", ".jpg", ".jpeg", ".tga"};
    bool changed = false;
    if (auto t = ui::PropertyTable("##textures")) {
        for (const auto& slot : slots) {
            ImGui::PushID(slot.c_str());
            const std::string label = display_name(slot);
            ui::row(label.c_str(), [&] {
                auto it = asset_.textures.find(slot);
                std::string path = it != asset_.textures.end() ? it->second : std::string{};
                if (asset_slot({.label = label.c_str(), .id = "##texture", .extensions = texture_exts, .ctx = &ictx},
                               path)) {
                    if (path.empty()) {
                        asset_.textures.erase(slot);
                    } else {
                        asset_.textures[slot] = path;
                    }
                    changed = true;
                }
            });
            ImGui::PopID();
        }
    }
    // A texture reaches the GPU material only through the file.
    if (changed) save_and_reload();
}

void MaterialEditorContext::draw_header_actions() {
    if (!material_) return;
    // The shader this material is made from, opened in the external editor.
    const std::filesystem::path shader = material_->shader_path();
    ImGui::SameLine();
    if (ui::action(ui::icon::shader, shader.filename().string().c_str(), ui::ButtonKind::Ghost)) {
        const Project* project = deps_.project_mgr != nullptr ? deps_.project_mgr->active() : nullptr;
        if (auto opened = platform::open_path(base_path::resolve_shared(shader.string(), project)); !opened) {
            FJELL_CORE_ERROR("Couldn't open {}: {}", shader.string(), opened.error());
        }
    }
    ImGui::SetItemTooltip("Open %s", shader.string().c_str());
}

} // namespace fjell
