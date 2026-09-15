#include "ui/mesh_editor_context.hpp"
#include "ui/dockspace_layout.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"
#include "core/project_manager.hpp"
#include "renderer/components/mesh_component.hpp"
#include "physics/collision_build.hpp"
#include "physics/collision_shape.hpp"
#include "renderer/resources/fjmesh_writer.hpp"
#include "renderer/resources/model.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "renderer/vulkan_context.hpp"
#include "ui/asset_thumbnail_renderer.hpp"

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

    // Submitted every frame while this context is the one drawing. DebugDraw
    // keeps the generated lines rather than these vectors, so switching away
    // from the mesh editor simply stops the overlay.
    if (show_collision_) {
        auto& debug = deps_.vulkan_context->debug_draw();
        if (collision_primitives_.empty()) {
            // No shapes authored, so the geometry is what this mesh collides as.
            if (!collision_vertices_.empty()) {
                debug.submit_authored_collision(collision_vertices_, collision_indices_);
            }
        } else {
            CollisionMesh geometry;
            geometry.vertices = &collision_vertices_;
            geometry.indices = &collision_indices_;
            geometry.source = authored_.collision_source;
            for (int i = 0; i < static_cast<int>(collision_primitives_.size()); ++i) {
                debug.submit_collision_primitive(
                    collision_primitives_[static_cast<size_t>(i)],
                    i == selected_primitive_, geometry);
            }
        }
    }

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
    return preview_render_requests(viewport_manager_.build_render_requests());
}

void MeshEditorContext::apply_resize(VulkanContext& vk) {
    viewport_manager_.apply_resize(vk);
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

    collision_vertices_ = model_->collision_vertices();
    collision_indices_ = model_->collision_indices();
    collision_dirty_ = false;

    // The loader synthesizes one geometry-derived shape for a mesh that
    // authored none. That is not an authored shape, so it is not shown as one:
    // an empty list here means "collides as its geometry".
    collision_primitives_.clear();
    const auto& loaded = model_->collision_primitives();
    bool is_synthesized = loaded.size() == 1 && !loaded.front().is_convex() &&
                          loaded.front().first_vertex == 0 &&
                          loaded.front().first_index == 0;
    if (!is_synthesized) {
        collision_primitives_ = loaded;
    }
    selected_primitive_ = -1;

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

            // Slots hold ids; show the path they point at.
            std::string shown = "(none)";
            if (!slot.path.empty()) {
                shown = base_path::asset_display_path(
                    slot.path, deps_.project_mgr->active());
                if (shown.empty()) shown = slot.path + "  (missing)";
            }
            ImGui::Text("%zu  %s", i, slot.name.c_str());
            ImGui::TextDisabled("%s", shown.c_str());

            // Drop a .fjmat from the content browser onto the row to assign it.
            ImGui::InvisibleButton("##drop", {-1.0F, ImGui::GetFrameHeight()});
            if (ImGui::BeginDragDropTarget()) {
                if (const auto* payload = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                    std::string dropped(static_cast<const char*>(payload->Data));
                    if (std::filesystem::path(dropped).extension() == ".fjmat") {
                        // Stored as an id so renaming or moving the material
                        // doesn't orphan the slot.
                        slot.path = base_path::asset_reference(
                            dropped, deps_.project_mgr->active());
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

    // Slots written by the importer hold a path, since nothing has an id at
    // import time. Upgrade them here so they survive the material moving.
    const auto* project = deps_.project_mgr->active();
    for (auto& slot : authored_.material_slots) {
        slot.path = base_path::asset_reference(slot.path, project);
    }

    // Rewriting collision resizes a section in the middle of the file, so it
    // takes the relocating path; the authored-only path patches in place and
    // stays the cheaper option when nothing but slots changed.
    CollisionSectionData section;
    if (collision_dirty_) {
        section.vertices = collision_vertices_;
        section.indices = collision_indices_;
        section.primitives.reserve(collision_primitives_.size());
        for (const auto& prim : collision_primitives_) {
            FjMeshCollisionPrimitive out{};
            out.kind = static_cast<uint32_t>(prim.kind);
            out.position[0] = prim.position.x;
            out.position[1] = prim.position.y;
            out.position[2] = prim.position.z;
            out.rotation[0] = prim.rotation.x;
            out.rotation[1] = prim.rotation.y;
            out.rotation[2] = prim.rotation.z;
            out.rotation[3] = prim.rotation.w;
            out.extents[0] = prim.extents.x;
            out.extents[1] = prim.extents.y;
            out.extents[2] = prim.extents.z;
            out.first_vertex = prim.first_vertex;
            out.vertex_count = prim.vertex_count;
            out.first_index = prim.first_index;
            out.index_count = prim.index_count;
            section.primitives.push_back(out);
        }
    }

    bool written = collision_dirty_
        ? rewrite_collision_data(fjmesh_abs_path_, section, authored_)
        : rewrite_authored_data(fjmesh_abs_path_, authored_);
    if (!written) {
        FJELL_CORE_ERROR("Failed to save mesh settings: {}", fjmesh_path_);
        return false;
    }

    collision_dirty_ = false;
    dirty_ = false;
    if (deps_.asset_thumbnails) deps_.asset_thumbnails->refresh(fjmesh_abs_path_);
    FJELL_CORE_INFO("Mesh settings saved: {}", fjmesh_path_);
    return true;
}

void MeshEditorContext::rebuild_collision() {
    if (!model_ || !model_->has_full_collision()) return;

    CollisionBuildInput input{&model_->collision_full_vertices(),
                              &model_->collision_full_indices()};
    auto built = build_collision(input, authored_.collision_source,
                                 authored_.collision_simplify_ratio);

    collision_vertices_ = std::move(built.vertices);
    collision_indices_ = std::move(built.indices);
    collision_dirty_ = true;
    dirty_ = true;
}

void MeshEditorContext::add_primitive(CollisionPrimitiveKind kind, bool fit_to_bounds) {
    CollisionPrimitive prim;
    prim.kind = kind;

    if (fit_to_bounds && model_ != nullptr) {
        // Sizing to the mesh's own bounds is the common case by a wide margin,
        // and typing six numbers to reach it is the kind of thing that stops
        // people authoring colliders at all.
        const auto& aabb = model_->local_aabb();
        if (aabb.valid()) {
            prim.position = (aabb.min + aabb.max) * 0.5F;
            glm::vec3 half = (aabb.max - aabb.min) * 0.5F;
            switch (kind) {
                case CollisionPrimitiveKind::box:
                    prim.extents = half;
                    break;
                case CollisionPrimitiveKind::sphere:
                    // The enclosing sphere, so nothing pokes out of it.
                    prim.extents = glm::vec3{std::max({half.x, half.y, half.z})};
                    break;
                case CollisionPrimitiveKind::capsule:
                    // Upright, sized to the wider horizontal axis; y carries
                    // the full height the way the shape builder reads it.
                    prim.extents = {std::max(half.x, half.z), half.y * 2.0F, 0.0F};
                    break;
                default:
                    break;
            }
        }
    }

    collision_primitives_.push_back(prim);
    selected_primitive_ = static_cast<int>(collision_primitives_.size()) - 1;
    collision_dirty_ = true;
    dirty_ = true;
}

void MeshEditorContext::draw_primitive_list() {
    ImGui::SeparatorText("Shapes");

    if (model_ != nullptr && model_->authored_version() < FJMESH_LAYOUT_VERSION) {
        ImGui::TextWrapped(
            "This mesh was written by an older version of the format and has no "
            "room for a shape list. Reimport it to author shapes.");
        return;
    }

    if (collision_primitives_.empty()) {
        ImGui::TextWrapped(
            "No shapes authored — this mesh collides as its geometry above. "
            "Add a shape to collide as something simpler, or as several convex "
            "pieces so it can move.");
    }

    static constexpr struct { CollisionPrimitiveKind kind; const char* label; } KINDS[] = {
        {CollisionPrimitiveKind::box, "Box"},
        {CollisionPrimitiveKind::sphere, "Sphere"},
        {CollisionPrimitiveKind::capsule, "Capsule"},
    };

    if (ImGui::Button("Fit to Bounds")) ImGui::OpenPopup("fit_kind");
    if (ImGui::BeginPopup("fit_kind")) {
        for (const auto& k : KINDS) {
            if (ImGui::Selectable(k.label)) add_primitive(k.kind, true);
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Add")) ImGui::OpenPopup("add_kind");
    if (ImGui::BeginPopup("add_kind")) {
        for (const auto& k : KINDS) {
            if (ImGui::Selectable(k.label)) add_primitive(k.kind, false);
        }
        ImGui::EndPopup();
    }

    static const char* KIND_NAMES[] = {"Box", "Sphere", "Capsule", "Convex Hull",
                                        "Triangle Mesh"};

    for (int i = 0; i < static_cast<int>(collision_primitives_.size()); ++i) {
        auto& prim = collision_primitives_[static_cast<size_t>(i)];
        ImGui::PushID(i);

        bool selected = selected_primitive_ == i;
        auto label = std::string(KIND_NAMES[static_cast<int>(prim.kind)]) +
                     "##" + std::to_string(i);
        if (ImGui::Selectable(label.c_str(), selected)) {
            selected_primitive_ = selected ? -1 : i;
        }

        if (selected) {
            ImGui::Indent();
            bool changed = false;

            ImGui::SetNextItemWidth(-1);
            if (ImGui::DragFloat3("##Position", &prim.position.x, 0.01F)) changed = true;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Position");

            switch (prim.kind) {
                case CollisionPrimitiveKind::box:
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::DragFloat3("##Extents", &prim.extents.x, 0.01F, 0.001F, 1000.0F)) {
                        changed = true;
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Half extents");
                    break;
                case CollisionPrimitiveKind::sphere:
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::DragFloat("##Radius", &prim.extents.x, 0.01F, 0.001F, 1000.0F)) {
                        changed = true;
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Radius");
                    break;
                case CollisionPrimitiveKind::capsule:
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::DragFloat("##CapRadius", &prim.extents.x, 0.01F, 0.001F, 1000.0F)) {
                        changed = true;
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Radius");
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::DragFloat("##CapHeight", &prim.extents.y, 0.01F, 0.001F, 1000.0F)) {
                        changed = true;
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Height");
                    break;
                default:
                    ImGui::TextDisabled("Derived from the mesh geometry.");
                    break;
            }

            if (ImGui::SmallButton("Remove")) {
                collision_primitives_.erase(collision_primitives_.begin() + i);
                selected_primitive_ = -1;
                changed = true;
                ImGui::Unindent();
                ImGui::PopID();
                collision_dirty_ = true;
                dirty_ = true;
                break;
            }

            if (changed) {
                collision_dirty_ = true;
                dirty_ = true;
            }
            ImGui::Unindent();
        }
        ImGui::PopID();
    }
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

        // A mesh imported before the format kept full-detail collision has
        // nothing to rebuild from, and deriving from the stored result would
        // compound the loss it already carries.
        const bool can_rebuild = model_->has_full_collision();
        ImGui::BeginDisabled(!can_rebuild);

        if (ImGui::BeginCombo("Source", current)) {
            for (const auto& opt : OPTIONS) {
                bool selected = opt.value == authored_.collision_source;
                if (ImGui::Selectable(opt.label, selected) && !selected) {
                    authored_.collision_source = opt.value;
                    rebuild_collision();
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", opt.help);
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();

        for (const auto& opt : OPTIONS) {
            if (opt.value == authored_.collision_source) {
                ImGui::TextWrapped("%s", opt.help);
            }
        }

        ImGui::Separator();

        if (authored_.collision_source == FjMeshCollisionSource::simplified) {
            ImGui::BeginDisabled(!can_rebuild);
            // Rebuild on release rather than per-frame: simplifying the helmet
            // costs about a millisecond, which is enough to make a drag stutter.
            ImGui::SliderFloat("Detail", &authored_.collision_simplify_ratio,
                               0.01F, 1.0F, "%.2f");
            if (ImGui::IsItemDeactivatedAfterEdit()) rebuild_collision();
            ImGui::EndDisabled();
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

        draw_primitive_list();

        ImGui::SeparatorText("Current geometry");

        if (ImGui::Checkbox("Show in viewport", &show_collision_)) {
            deps_.vulkan_context->set_show_colliders(show_collision_);
        }

        // A hull is a point cloud, so it reports no triangles by design.
        bool is_hull = authored_.collision_source == FjMeshCollisionSource::convex_hull;
        if (ImGui::BeginTable("collision", 2, ImGuiTableFlags_SizingStretchProp)) {
            info_row("Vertices", group_digits(collision_vertices_.size()));
            info_row("Triangles", group_digits(collision_indices_.size() / 3));
            info_row("Supports dynamic", is_hull ? "yes" : "no");
            ImGui::EndTable();
        }

        if (!can_rebuild) {
            ImGui::TextWrapped(
                "This mesh predates the format that keeps the geometry collision "
                "is built from. Reimport it to change the source.");
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
