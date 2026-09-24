#include "ui/material_editor_context.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"
#include "core/project_manager.hpp"
#include "renderer/bindless.hpp"
#include "renderer/resources/fjsl_parser.hpp"
#include "renderer/resources/material.hpp"
#include "renderer/components/mesh_component.hpp"
#include "renderer/pipeline_cache.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "renderer/vulkan_context.hpp"
#include "ui/asset_thumbnail_renderer.hpp"
#include "ui/icon_cache.hpp"
#include "ui/material_params_widget.hpp"

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

    if (auto* cam = viewport_manager_.active_camera()) {
        cam->focus({0.0f, 0.0f, 0.0f}, 1.5f);
    }

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
    if (ImGui::Begin(ctx_title("Parameters").c_str())) {
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
        // Only a spatial material goes on the preview sphere; the rest are
        // drawn by a pass of their own from geometry the sphere does not
        // have. Say which, and where to look instead — the preview is simply
        // blank otherwise, which reads as a broken material.
        switch (material_->shader_type()) {
            case ShaderType::Spatial:
                break;
            case ShaderType::Ui:
                ImGui::TextWrapped("UI shader: the preview sphere cannot draw it. Assign the "
                                   "material to a node and open that widget in the UI editor.");
                break;
            case ShaderType::Terrain:
                ImGui::TextWrapped("Terrain shader: the preview sphere cannot draw it. Assign "
                                   "the material to a Terrain component and look at the scene.");
                break;
            case ShaderType::Water:
                ImGui::TextWrapped("Water shader: the preview sphere cannot draw it — the "
                                   "surface comes from the node's .fjwater wave field. Assign "
                                   "the material to a Water component and look at the scene.");
                break;
            case ShaderType::Sky:
                ImGui::TextWrapped("Sky shader: the preview sphere cannot draw it. Set it as "
                                   "the scene's sky and look at the scene.");
                break;
            case ShaderType::Fog:
                ImGui::TextWrapped("Fog shader: the preview sphere cannot draw it. Assign the "
                                   "material to the scene's Volumetric Fog node and look at "
                                   "the scene.");
                break;
        }
        ImGui::Separator();

        if (pid < cache->shader_count()) {
            const auto& meta = cache->metadata(pid);
            (void)draw_material_params(meta, bindless, *material_);
        }
    }
    ImGui::End();
}

void MaterialEditorContext::draw_texture_panel() {
    if (ImGui::Begin(ctx_title("Textures").c_str())) {
        if (!material_) {
            ImGui::TextDisabled("No material loaded");
            ImGui::End();
            return;
        }

        // Fixed PBR slots; ui and terrain shaders sample only the textures
        // they declare
        if (material_->shader_type() == ShaderType::Spatial) {
            draw_texture_slot("base_color");
            draw_texture_slot("normal_map");
            draw_texture_slot("metallic_roughness");
            draw_texture_slot("emissive");
        }

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
            save_and_reload();
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
                save_and_reload();
            }
        }
        ImGui::EndDragDropTarget();
    }

    ImGui::Separator();
    ImGui::PopID();
}

} // namespace fjell
