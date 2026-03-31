#include "ui/material_editor_context.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"
#include "core/project_manager.hpp"
#include "renderer/bindless.hpp"
#include "renderer/fjsl_parser.hpp"
#include "renderer/material.hpp"
#include "renderer/mesh_component.hpp"
#include "renderer/pipeline_cache.hpp"
#include "renderer/resource_manager.hpp"
#include "renderer/vulkan_context.hpp"
#include "ui/icon_cache.hpp"
#include "ui/material_params_widget.hpp"
#include "ui/code_editor.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <cstdio>
#include <filesystem>

namespace fjell {

MaterialEditorContext::MaterialEditorContext(MaterialEditorDeps deps)
    : deps_{deps} {}

void MaterialEditorContext::init() {
    viewport_manager_.init(deps_.vulkan_context->device_ref(),
                           deps_.vulkan_context->msaa_samples());
    viewport_manager_.add_viewport(ViewportSource::final_color, "Material Preview");
}

void MaterialEditorContext::shutdown() {
    viewport_manager_.release_textures();
}

void MaterialEditorContext::draw(float dt) {
    preview_scene_.compute_world_matrices(nullptr);
    viewport_manager_.draw(*deps_.vulkan_context, *deps_.input, dt);
    draw_params_panel();
    draw_texture_panel();
}

std::vector<std::string> MaterialEditorContext::docked_window_names() const {
    int ci = context_index();
    char stats[32], console[32], history[32], code[32];
    std::snprintf(stats, sizeof(stats), "Stats##ctx%d", ci);
    std::snprintf(console, sizeof(console), "Console##ctx%d", ci);
    std::snprintf(history, sizeof(history), "History##ctx%d", ci);
    std::snprintf(code, sizeof(code), "Code Editor##ctx%d", ci);
    return {code, "Material Preview", "Material Params", "Texture Slots",
            history, stats, console};
}

void MaterialEditorContext::setup_dockspace(ImGuiID main_area) {
    ImGuiID center = main_area;

    // Right panel: Params (top) + Textures (bottom) — 28%
    ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.28f, nullptr, &center);
    ImGuiID right_bottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.50f, nullptr, &right);

    // Bottom: stats/console — 25%
    ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.25f, nullptr, &center);

    int ci = context_index();
    char stats[32], console[32], history[32], code[32];
    std::snprintf(stats, sizeof(stats), "Stats##ctx%d", ci);
    std::snprintf(console, sizeof(console), "Console##ctx%d", ci);
    std::snprintf(history, sizeof(history), "History##ctx%d", ci);
    std::snprintf(code, sizeof(code), "Code Editor##ctx%d", ci);

    ImGui::DockBuilderDockWindow(code, center);
    ImGui::DockBuilderDockWindow("Material Preview", center);
    ImGui::DockBuilderDockWindow("Material Params", right);
    ImGui::DockBuilderDockWindow("Texture Slots", right_bottom);
    ImGui::DockBuilderDockWindow(history, right_bottom);
    ImGui::DockBuilderDockWindow(stats, bottom);
    ImGui::DockBuilderDockWindow(console, bottom);
}

bool MaterialEditorContext::handle_shortcuts() {
    return false;
}

std::vector<ViewportRenderRequest> MaterialEditorContext::build_render_requests() {
    return viewport_manager_.build_render_requests();
}

void MaterialEditorContext::apply_resize(VulkanContext& vk) {
    viewport_manager_.apply_resize(vk);
}

bool MaterialEditorContext::any_viewport_hovered() const {
    return viewport_manager_.any_hovered();
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

    if (auto* cam = viewport_manager_.active_camera()) {
        cam->focus({0.0f, 0.0f, 0.0f}, 1.5f);
    }

    // Open the .fjsl source in the code editor
    if (deps_.code_editor && !material_->shader_path().empty()) {
        auto shader_abs = base_path::resolve(material_->shader_path());
        deps_.code_editor->open(shader_abs);
    }

    FJELL_CORE_INFO("Material editor opened: {}", fjmat_path);
}

void MaterialEditorContext::save_material() {
    if (fjmat_abs_path_.empty() || !material_) return;

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
            asset_.params[u.name] = vals;
        }
    }

    save_material_asset(fjmat_abs_path_, asset_);
    FJELL_CORE_INFO("Material saved: {}", fjmat_abs_path_);
}

bool MaterialEditorContext::save() {
    if (fjmat_path_.empty()) return false;
    save_material();
    return true;
}

// ── Preview scene ────────────────────────────────────────────────────────

void MaterialEditorContext::setup_preview_scene() {
    preview_scene_.clear();

    auto sphere = deps_.resource_manager->create_primitive("sphere");
    if (!sphere) return;

    preview_node_id_ = preview_scene_.add(
        *sphere, material_handle_, *deps_.resource_manager,
        {}, "Preview Sphere", "primitive:sphere", fjmat_path_);

    preview_scene_.compute_world_matrices(nullptr);
}

// ── Panels ───────────────────────────────────────────────────────────────

void MaterialEditorContext::draw_params_panel() {
    if (ImGui::Begin("Material Params")) {
        if (!material_) {
            ImGui::TextDisabled("No material loaded");
            ImGui::End();
            return;
        }

        auto* cache = deps_.vulkan_context->pipeline_cache();
        auto& bindless = deps_.vulkan_context->bindless_textures();
        uint32_t pid = material_->pipeline_id();

        ImGui::TextDisabled("Shader: %s", material_->shader_path().c_str());
        ImGui::TextDisabled("Material: %s", fjmat_path_.c_str());
        ImGui::Separator();

        if (pid < cache->shader_count()) {
            const auto& meta = cache->metadata(pid);
            (void)draw_material_params(meta, bindless, *material_);
        }
    }
    ImGui::End();
}

void MaterialEditorContext::draw_texture_panel() {
    if (ImGui::Begin("Texture Slots")) {
        if (!material_) {
            ImGui::TextDisabled("No material loaded");
            ImGui::End();
            return;
        }

        // Fixed PBR slots
        draw_texture_slot("base_color");
        draw_texture_slot("normal_map");
        draw_texture_slot("metallic_roughness");
        draw_texture_slot("emissive");

        // Custom texture slots from shader metadata
        auto* cache = deps_.vulkan_context->pipeline_cache();
        uint32_t pid = material_->pipeline_id();
        if (pid < cache->shader_count()) {
            const auto& meta = cache->metadata(pid);
            for (const auto& u : meta.uniforms) {
                if (u.type != UniformType::Sampler2D) continue;
                draw_texture_slot(u.name.c_str());
            }
        }
    }
    ImGui::End();
}

void MaterialEditorContext::draw_texture_slot(const char* slot_name) {
    ImGui::PushID(slot_name);

    auto it = asset_.textures.find(slot_name);
    bool has_texture = it != asset_.textures.end() && !it->second.empty();
    std::string display_name = has_texture
        ? std::filesystem::path(it->second).filename().string()
        : "(none)";

    // Thumbnail
    float thumb_size = 48.0f;
    if (has_texture && deps_.icon_cache) {
        auto resolved = base_path::resolve_asset(it->second, deps_.project_mgr->active());
        auto desc = deps_.icon_cache->thumbnail(resolved);
        if (desc) {
            ImGui::Image(desc, {thumb_size, thumb_size});
        } else {
            ImGui::Dummy({thumb_size, thumb_size});
        }
    } else {
        ImGui::Dummy({thumb_size, thumb_size});
    }

    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::Text("%s", slot_name);
    ImGui::TextDisabled("%s", display_name.c_str());

    if (has_texture) {
        if (ImGui::SmallButton("Clear")) {
            asset_.textures.erase(slot_name);
            // TODO: reload material to reflect cleared texture
        }
    }
    ImGui::EndGroup();

    // Drag-drop target for texture assets from content browser
    if (ImGui::BeginDragDropTarget()) {
        if (const auto* payload = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
            std::string path(static_cast<const char*>(payload->Data));
            auto ext = std::filesystem::path(path).extension().string();
            if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga") {
                asset_.textures[slot_name] = path;
                // Save and reload to apply texture change
                save_material();
                auto result = deps_.resource_manager->load_material_asset(
                    fjmat_abs_path_, deps_.project_mgr->active());
                if (result) {
                    material_handle_ = *result;
                    material_ = deps_.resource_manager->get(material_handle_);
                    setup_preview_scene();
                }
            }
        }
        ImGui::EndDragDropTarget();
    }

    ImGui::Separator();
    ImGui::PopID();
}

} // namespace fjell
