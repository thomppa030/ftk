#include "ui/mesh_editor_context.hpp"
#include "ui/dockspace_layout.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"
#include "core/project_manager.hpp"
#include "renderer/components/mesh_component.hpp"
#include "physics/collision_shape.hpp"
#include "renderer/resources/fjmesh_writer.hpp"
#include "renderer/resources/model.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "renderer/vulkan_context.hpp"

#include <imgui.h>

#include <filesystem>

namespace fjell {

namespace {

// Thousands separators keep six- and seven-digit counts readable at a glance.
std::string group_digits(uint64_t value) {
    auto s = std::to_string(value);
    for (int pos = static_cast<int>(s.size()) - 3; pos > 0; pos -= 3) {
        s.insert(static_cast<size_t>(pos), ",");
    }
    return s;
}

void info_row(const char* label, const std::string& value) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(value.c_str());
}

} // namespace

MeshEditorContext::MeshEditorContext(MeshEditorDeps deps)
    : deps_{deps} {}

void MeshEditorContext::init() {
    viewport_manager_.init(deps_.vulkan_context->device_ref(),
                           deps_.vulkan_context->allocator(),
                           deps_.vulkan_context->msaa_samples());
    viewport_manager_.add_viewport(ViewportSource::final_color, "Mesh Preview");
    init_shared_panels(deps_.vulkan_context, deps_.project_mgr);
    history_panel().init(&command_history());
}

void MeshEditorContext::shutdown() {
    viewport_manager_.release_textures();
}

void MeshEditorContext::draw(float dt) {
    if (auto* vp = viewport_manager_.active_panel()) {
        auto t = ctx_title("Mesh Preview");
        if (vp->title != t) vp->title = std::move(t);
    }
    preview_scene_.compute_world_matrices(nullptr);
    viewport_manager_.draw(*deps_.vulkan_context, *deps_.input, dt);
    draw_slots_panel();
    draw_collision_panel();
    draw_info_panel();
    draw_shared_panels(dt);
}

std::vector<std::string> MeshEditorContext::docked_window_names() const {
    return {ctx_title("Mesh Preview"), ctx_title("Material Slots"),
            ctx_title("Collision"), ctx_title("Mesh Info"),
            ctx_title("Stats"), ctx_title("Console")};
}

void MeshEditorContext::setup_dockspace(ImGuiID main_area) {
    enum class EPanel { Preview, Slots, Collision, Info, Stats, Console };
    using N = DockNode;
    static const auto layout = N::horizontal_split(0.72f,
        N::vertical_split(0.75f,
            N::leaf({EPanel::Preview}),
            N::leaf({EPanel::Stats, EPanel::Console})),
        N::vertical_split(0.5f,
            N::leaf({EPanel::Slots}),
            N::leaf({EPanel::Collision, EPanel::Info})));
    build_dockspace(main_area, layout, docked_window_names());
}

bool MeshEditorContext::handle_shortcuts() {
    return false;
}

std::vector<ViewportRenderRequest> MeshEditorContext::build_render_requests() {
    return viewport_manager_.build_render_requests();
}

void MeshEditorContext::apply_resize(VulkanContext& vk) {
    viewport_manager_.apply_resize(vk);
}

bool MeshEditorContext::any_viewport_hovered() const {
    return viewport_manager_.any_hovered();
}

// ── Open ─────────────────────────────────────────────────────────────────

void MeshEditorContext::open_mesh(const std::string& fjmesh_path) {
    fjmesh_path_ = fjmesh_path;
    auto abs_path = base_path::resolve_asset(fjmesh_path, deps_.project_mgr->active());

    auto result = deps_.resource_manager->load_model(abs_path);
    if (!result) {
        FJELL_CORE_ERROR("Failed to load mesh: {}", result.error());
        return;
    }
    model_handle_ = *result;
    model_ = deps_.resource_manager->get(model_handle_);
    if (!model_) return;

    fjmesh_abs_path_ = abs_path;
    authored_ = model_->authored();
    dirty_ = false;
    name_ = std::filesystem::path(fjmesh_path).stem().string();

    setup_preview_scene();
    frame_camera();

    FJELL_CORE_INFO("Mesh editor opened: {}", fjmesh_path);
}

// ── Preview scene ────────────────────────────────────────────────────────

void MeshEditorContext::setup_preview_scene() {
    preview_scene_.clear();
    if (!model_handle_.valid()) return;

    // Slots that aren't assigned fall back to this, same as in a real scene.
    auto tex = deps_.resource_manager->load_texture(
        base_path::resolve("engine_assets/textures/test.png")).value_or(TextureHandle{});
    auto material = deps_.resource_manager->create_material("default", tex);

    preview_node_id_ = preview_scene_.add(
        model_handle_, material, *deps_.resource_manager,
        {}, name_, fjmesh_path_, "default");

    apply_slot_materials();
    preview_scene_.compute_world_matrices(nullptr);
}

void MeshEditorContext::apply_slot_materials() {
    auto* node = preview_scene_.find(preview_node_id_);
    if (!node) return;
    auto* mc = node->components.get<MeshComponent>();
    if (!mc) return;

    // Resolve from the in-editor assignments rather than the model's, so an
    // edit shows up before it has been saved.
    const auto* project = deps_.project_mgr->active();
    mc->slot_materials.assign(authored_.material_slots.size(), nullptr);
    for (size_t i = 0; i < authored_.material_slots.size(); ++i) {
        const auto& path = authored_.material_slots[i].path;
        if (path.empty()) continue;

        auto result = deps_.resource_manager->load_material_asset(
            base_path::resolve_asset(path, project), project);
        if (result) {
            mc->slot_materials[i] = deps_.resource_manager->get(*result);
        }
    }
}

void MeshEditorContext::frame_camera() {
    auto* cam = viewport_manager_.active_camera();
    if (!cam || !model_) return;

    const auto& aabb = model_->local_aabb();
    glm::vec3 center = aabb.valid() ? (aabb.min + aabb.max) * 0.5f : glm::vec3{0.0f};
    float radius = model_->bounding_radius() > 0.0f ? model_->bounding_radius() : 1.0f;
    cam->focus(center, radius);
}

// ── Panels ───────────────────────────────────────────────────────────────

void MeshEditorContext::draw_slots_panel() {
    if (ImGui::Begin(ctx_title("Material Slots").c_str())) {
        if (!model_) {
            ImGui::TextDisabled("No mesh loaded");
            ImGui::End();
            return;
        }

        if (authored_.material_slots.empty()) {
            ImGui::TextWrapped(
                "This mesh has no material slots. Reimport it to record the "
                "materials its submeshes were authored with.");
            ImGui::End();
            return;
        }

        ImGui::BeginDisabled(!dirty_);
        if (ImGui::Button("Save")) {
            (void)save();
        }
        ImGui::EndDisabled();
        if (dirty_) {
            ImGui::SameLine();
            ImGui::TextDisabled("unsaved changes");
        }
        ImGui::Separator();

        for (size_t i = 0; i < authored_.material_slots.size(); ++i) {
            auto& slot = authored_.material_slots[i];
            ImGui::PushID(static_cast<int>(i));

            ImGui::Text("%zu  %s", i, slot.name.c_str());
            ImGui::TextDisabled("%s", slot.path.empty() ? "(none)" : slot.path.c_str());

            // Drop a .fjmat from the content browser onto the row to assign it.
            ImGui::InvisibleButton("##drop", {-1.0F, ImGui::GetFrameHeight()});
            if (ImGui::BeginDragDropTarget()) {
                if (const auto* payload = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                    std::string dropped(static_cast<const char*>(payload->Data));
                    if (std::filesystem::path(dropped).extension() == ".fjmat") {
                        slot.path = dropped;
                        dirty_ = true;
                        apply_slot_materials();
                    }
                }
                ImGui::EndDragDropTarget();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Drop a .fjmat here to assign it to this slot");
            }

            if (!slot.path.empty()) {
                if (ImGui::SmallButton("Clear")) {
                    slot.path.clear();
                    dirty_ = true;
                    apply_slot_materials();
                }
            }

            ImGui::Separator();
            ImGui::PopID();
        }
    }
    ImGui::End();
}

bool MeshEditorContext::save() {
    if (fjmesh_abs_path_.empty()) return false;

    if (!rewrite_authored_data(fjmesh_abs_path_, authored_)) {
        FJELL_CORE_ERROR("Failed to save mesh settings: {}", fjmesh_path_);
        return false;
    }

    dirty_ = false;
    FJELL_CORE_INFO("Mesh settings saved: {}", fjmesh_path_);
    return true;
}

void MeshEditorContext::draw_collision_panel() {
    if (ImGui::Begin(ctx_title("Collision").c_str())) {
        if (!model_) {
            ImGui::TextDisabled("No mesh loaded");
            ImGui::End();
            return;
        }

        struct SourceOption {
            FjMeshCollisionSource value;
            const char* label;
            const char* help;
        };
        static constexpr SourceOption OPTIONS[] = {
            {FjMeshCollisionSource::render_mesh, "Render mesh",
             "Collide against the visible geometry. Static bodies only."},
            {FjMeshCollisionSource::convex_hull, "Convex hull",
             "Hull of the visible geometry. The only source that works on a "
             "moving body."},
            {FjMeshCollisionSource::simplified, "Simplified mesh",
             "Decimated triangle mesh. Static bodies only."},
            {FjMeshCollisionSource::custom_mesh, "Custom mesh",
             "Collide against a separate .fjmesh authored as a proxy."},
        };

        const char* current = "Render mesh";
        for (const auto& opt : OPTIONS) {
            if (opt.value == authored_.collision_source) current = opt.label;
        }

        if (ImGui::BeginCombo("Source", current)) {
            for (const auto& opt : OPTIONS) {
                bool selected = opt.value == authored_.collision_source;
                if (ImGui::Selectable(opt.label, selected)) {
                    authored_.collision_source = opt.value;
                    dirty_ = true;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", opt.help);
            }
            ImGui::EndCombo();
        }

        for (const auto& opt : OPTIONS) {
            if (opt.value == authored_.collision_source) {
                ImGui::TextWrapped("%s", opt.help);
            }
        }

        ImGui::Separator();

        if (authored_.collision_source == FjMeshCollisionSource::simplified) {
            if (ImGui::SliderFloat("Detail", &authored_.collision_simplify_ratio,
                                   0.01F, 1.0F, "%.2f")) {
                dirty_ = true;
            }
            ImGui::TextDisabled("Fraction of the original triangles to keep.");
        }

        if (authored_.collision_source == FjMeshCollisionSource::custom_mesh) {
            ImGui::Text("Proxy: %s", authored_.collision_mesh.empty()
                        ? "(none)" : authored_.collision_mesh.c_str());
            ImGui::InvisibleButton("##proxy_drop", {-1.0F, ImGui::GetFrameHeight()});
            if (ImGui::BeginDragDropTarget()) {
                if (const auto* payload = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                    std::string dropped(static_cast<const char*>(payload->Data));
                    if (std::filesystem::path(dropped).extension() == ".fjmesh") {
                        authored_.collision_mesh = dropped;
                        dirty_ = true;
                    }
                }
                ImGui::EndDragDropTarget();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Drop a .fjmesh here to use as the collision proxy");
            }
            if (!authored_.collision_mesh.empty() && ImGui::SmallButton("Clear")) {
                authored_.collision_mesh.clear();
                dirty_ = true;
            }
        }

        ImGui::SeparatorText("Current geometry");

        // What is in the file right now, which only matches the source above
        // after a reimport regenerates it.
        auto collision = collision_mesh_of(*model_);
        if (ImGui::BeginTable("collision", 2, ImGuiTableFlags_SizingStretchProp)) {
            info_row("Vertices", group_digits(model_->collision_vertices().size()));
            info_row("Triangles", group_digits(collision.triangle_count()));
            info_row("Supports dynamic", collision.supports_dynamic() ? "yes" : "no");
            ImGui::EndTable();
        }

        if (authored_.collision_source != model_->collision_source()) {
            ImGui::TextWrapped(
                "The geometry in the file was built for a different source. "
                "Save, then reimport the mesh to regenerate it.");
        }
    }
    ImGui::End();
}

void MeshEditorContext::draw_info_panel() {
    if (ImGui::Begin(ctx_title("Mesh Info").c_str())) {
        if (!model_) {
            ImGui::TextDisabled("No mesh loaded");
            ImGui::End();
            return;
        }

        ImGui::TextDisabled("%s", fjmesh_path_.c_str());

        if (!authored_.source_file.empty()) {
            ImGui::TextDisabled("Imported from %s", authored_.source_file.c_str());

            auto source = find_import_source(fjmesh_abs_path_);
            ImGui::BeginDisabled(source.empty() || !deps_.import_model);
            if (ImGui::Button("Reimport")) {
                deps_.import_model(source);
            }
            ImGui::EndDisabled();
            // The button is disabled in exactly this case, and a disabled
            // widget needs the flag to report hover at all.
            if (source.empty() &&
                ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("Source file not found next to the mesh");
            }
        }

        ImGui::Separator();

        // Geometry counts. total_indices() covers every LOD, so triangles are
        // reported per-LOD in the table below rather than as a single number.
        if (ImGui::BeginTable("geometry", 2, ImGuiTableFlags_SizingStretchProp)) {
            info_row("Submeshes", group_digits(model_->mega_entry_count()));
            info_row("Meshlets", group_digits(model_->total_meshlet_count()));
            info_row("Indices", group_digits(model_->total_indices()));
            ImGui::EndTable();
        }

        ImGui::SeparatorText("Bounds");
        const auto& aabb = model_->local_aabb();
        if (ImGui::BeginTable("bounds", 2, ImGuiTableFlags_SizingStretchProp)) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%.3f, %.3f, %.3f", aabb.min.x, aabb.min.y, aabb.min.z);
            info_row("Min", buf);
            std::snprintf(buf, sizeof(buf), "%.3f, %.3f, %.3f", aabb.max.x, aabb.max.y, aabb.max.z);
            info_row("Max", buf);
            glm::vec3 extent = aabb.max - aabb.min;
            std::snprintf(buf, sizeof(buf), "%.3f, %.3f, %.3f", extent.x, extent.y, extent.z);
            info_row("Size", buf);
            std::snprintf(buf, sizeof(buf), "%.3f", model_->bounding_radius());
            info_row("Radius", buf);
            ImGui::EndTable();
        }

        ImGui::SeparatorText("LODs");
        if (ImGui::BeginTable("lods", 4,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("LOD");
            ImGui::TableSetupColumn("Submeshes");
            ImGui::TableSetupColumn("Triangles");
            ImGui::TableSetupColumn("Screen size");
            ImGui::TableHeadersRow();

            const auto& entries = model_->mega_entries();
            uint32_t lod_index = 0;
            for (const auto& lod : model_->lod_levels()) {
                uint32_t indices = 0;
                uint32_t end = lod.first_mega_entry + lod.mega_entry_count;
                for (uint32_t i = lod.first_mega_entry; i < end && i < entries.size(); ++i) {
                    indices += entries[i].index_count;
                }

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%u", lod_index);
                ImGui::TableNextColumn();
                ImGui::Text("%u", lod.mega_entry_count);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(group_digits(indices / 3).c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%.3f", lod.max_screen_size);
                ++lod_index;
            }
            ImGui::EndTable();
        }

        ImGui::SeparatorText("SDF");
        if (model_->has_sdf()) {
            uint32_t res = model_->sdf_grid_resolution();
            ImGui::Text("%u x %u x %u", res, res, res);
            ImGui::TextDisabled("%s voxels",
                                group_digits(model_->sdf_grid_data().size()).c_str());
        } else {
            ImGui::TextDisabled("None");
        }
    }
    ImGui::End();
}

} // namespace fjell
