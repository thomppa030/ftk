#include "ui/mesh_editor_context.hpp"
#include "core/base_path.hpp"
#include "ftk/base/log.hpp"
#include "core/project_manager.hpp"
#include "physics/collision_build.hpp"
#include "physics/collision_shape.hpp"
#include "renderer/components/mesh_component.hpp"
#include "renderer/resources/asset_shader_type.hpp"
#include "renderer/resources/fjmesh_writer.hpp"
#include "renderer/resources/model.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "renderer/vulkan_context.hpp"
#include "scene/component_meta.hpp"
#include "ui/asset_ref_widget.hpp"
#include "ui/asset_thumbnail_renderer.hpp"
#include "ui/inspectors/collision_shape_fields.hpp"
#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/choice.hpp"
#include "ftk/ui/kit/feedback.hpp"
#include "ftk/ui/kit/field.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/list_editor.hpp"
#include "ftk/ui/kit/panel.hpp"
#include "ftk/ui/kit/row.hpp"
#include "ftk/ui/kit/section.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <format>
#include <optional>

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

} // namespace

MeshEditorContext::MeshEditorContext(MeshEditorDeps deps)
    : deps_{deps} {}

ftk::EditorContext::DockPreset MeshEditorContext::dock_preset() const {
    return {.preview = {"Preview"}, .inspector = {"Materials", "Collision", "Info"}};
}

void MeshEditorContext::init() {
    viewport_manager_.init(deps_.vulkan_context->msaa_samples());
    viewport_manager_.add_viewport(ViewportSource::final_color, "Preview");
    init_shared_panels(deps_.vulkan_context);
    history_panel().init(&command_history());
}

void MeshEditorContext::draw(float dt) {
    name_preview(viewport_manager_);
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
        FTK_CORE_ERROR("Failed to load mesh: {}", result.error());
        return;
    }
    model_handle_ = *result;
    model_ = deps_.resource_manager->get(model_handle_);
    if (!model_) return;

    fjmesh_abs_path_ = abs_path;
    authored_ = model_->authored();
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

    document_opened();
    FTK_CORE_INFO("Mesh editor opened: {}", fjmesh_path);
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
    if (!model_) return;

    const auto& aabb = model_->local_aabb();
    glm::vec3 center = aabb.valid() ? (aabb.min + aabb.max) * 0.5f : glm::vec3{0.0f};
    float radius = model_->bounding_radius() > 0.0f ? model_->bounding_radius() : 1.0f;
    viewport_manager_.frame(center, radius);
}

// ── Panels ───────────────────────────────────────────────────────────────

void MeshEditorContext::draw_slots_panel() {
    auto panel = ftk::ui::Panel(ftk::ui::icon::material, ctx_title("Materials").c_str());
    if (!panel) return;
    if (!model_) {
        ftk::ui::empty_state(ftk::ui::icon::mesh, "No mesh open", "Open one from the content browser");
        return;
    }
    if (authored_.material_slots.empty()) {
        ftk::ui::empty_state(ftk::ui::icon::material, "No material slots",
                        "Reimport the mesh to record the materials its parts were made with");
        return;
    }

    InspectorContext ictx{};
    ictx.project_mgr = deps_.project_mgr;
    ictx.icons = deps_.icon_cache;
    static const std::string material_exts[] = {".fjmat"};
    bool changed = false;
    // Each slot is named by the material its part had in the source file.
    if (auto t = ftk::ui::PropertyTable("##slots")) {
        for (std::size_t i = 0; i < authored_.material_slots.size(); ++i) {
            auto& slot = authored_.material_slots[i];
            ImGui::PushID(static_cast<int>(i));
            ftk::ui::row(slot.name.c_str(), [&] {
                changed |= asset_slot({.label = slot.name.c_str(), .id = "##material", .extensions = material_exts,
                                       .ctx = &ictx, .shader_types = only_shader_type(ShaderType::Spatial)},
                                      slot.path);
            });
            ImGui::PopID();
        }
    }
    if (changed) apply_slot_materials();
}

std::string MeshEditorContext::document() const {
    if (fjmesh_abs_path_.empty()) return {};
    // The file is binary; the document is what this editor changes in it:
    // the slots, how collision is built, the shapes and the collision
    // geometry's size (rebuilt from the settings, so its size follows them).
    nlohmann::json j;
    for (const auto& slot : authored_.material_slots) {
        j["slots"].push_back({slot.name, slot.path});
    }
    j["collision_source"] = static_cast<int>(authored_.collision_source);
    j["collision_simplify_ratio"] = authored_.collision_simplify_ratio;
    j["collision_mesh"] = authored_.collision_mesh;
    j["collision_geometry"] = {collision_vertices_.size(), collision_indices_.size()};
    for (const auto& p : collision_primitives_) {
        j["shapes"].push_back({static_cast<int>(p.kind), p.position.x, p.position.y, p.position.z,
                               p.rotation.x, p.rotation.y, p.rotation.z, p.rotation.w,
                               p.extents.x, p.extents.y, p.extents.z, p.bone,
                               p.first_vertex, p.vertex_count, p.first_index, p.index_count});
    }
    return j.dump();
}

ftk::Result<> MeshEditorContext::write() {
    if (fjmesh_abs_path_.empty()) return ftk::make_error("no mesh is open");

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
    if (!written) return ftk::make_error("the mesh file couldn't be rewritten");

    collision_dirty_ = false;
    if (deps_.asset_thumbnails) deps_.asset_thumbnails->refresh(fjmesh_abs_path_);
    FTK_CORE_INFO("Mesh settings saved: {}", fjmesh_path_);
    return {};
}

ftk::Result<> MeshEditorContext::restore(const std::string& text) {
    auto j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_object()) return ftk::make_error("the mesh document isn't JSON");

    std::vector<MeshMaterialSlot> slots;
    for (const auto& slot : j.value("slots", nlohmann::json::array())) {
        slots.push_back({slot.at(0).get<std::string>(), slot.at(1).get<std::string>()});
    }
    const bool slots_changed = slots.size() != authored_.material_slots.size() ||
        !std::equal(slots.begin(), slots.end(), authored_.material_slots.begin(),
                    [](const MeshMaterialSlot& a, const MeshMaterialSlot& b) {
                        return a.name == b.name && a.path == b.path;
                    });
    authored_.material_slots = std::move(slots);

    const auto source = static_cast<FjMeshCollisionSource>(j.value("collision_source", 0));
    const float ratio = j.value("collision_simplify_ratio", 0.1F);
    const std::string proxy = j.value("collision_mesh", std::string{});
    const bool settings_changed = source != authored_.collision_source ||
                                  ratio != authored_.collision_simplify_ratio ||
                                  proxy != authored_.collision_mesh;
    authored_.collision_source = source;
    authored_.collision_simplify_ratio = ratio;
    authored_.collision_mesh = proxy;
    if (settings_changed) rebuild_collision();

    collision_primitives_.clear();
    for (const auto& s : j.value("shapes", nlohmann::json::array())) {
        CollisionPrimitive p;
        p.kind = static_cast<CollisionPrimitiveKind>(s.at(0).get<int>());
        p.position = {s.at(1).get<float>(), s.at(2).get<float>(), s.at(3).get<float>()};
        p.rotation = glm::quat{s.at(7).get<float>(), s.at(4).get<float>(), s.at(5).get<float>(), s.at(6).get<float>()};
        p.extents = {s.at(8).get<float>(), s.at(9).get<float>(), s.at(10).get<float>()};
        p.bone = s.at(11).get<int32_t>();
        p.first_vertex = s.at(12).get<uint32_t>();
        p.vertex_count = s.at(13).get<uint32_t>();
        p.first_index = s.at(14).get<uint32_t>();
        p.index_count = s.at(15).get<uint32_t>();
        collision_primitives_.push_back(p);
    }
    collision_dirty_ = true;
    if (selected_primitive_ >= static_cast<int>(collision_primitives_.size())) selected_primitive_ = -1;
    if (slots_changed) setup_preview_scene();
    return {};
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
}

void MeshEditorContext::draw_primitive_list() {
    ftk::ui::subheading("Shapes");
    if (model_ != nullptr && model_->authored_version() < FJMESH_LAYOUT_VERSION) {
        ftk::ui::status(ftk::ui::Severity::Warning,
                   "This mesh was written by an older version of the format and has no room for shapes. "
                   "Reimport it to add some.");
        return;
    }

    auto list = ftk::ui::ListEditor("##shapes", collision_primitives_.size(), {.reorder = false, .opens = true});
    if (collision_primitives_.empty()) {
        list.empty("No shapes: the mesh collides as its geometry above. Add one to collide as something "
                   "simpler, or as convex pieces so it can move");
    }
    std::optional<std::size_t> removed;
    for (std::size_t i = 0; i < collision_primitives_.size(); ++i) {
        auto& prim = collision_primitives_[i];
        const bool selected = selected_primitive_ == static_cast<int>(i);
        if (!list.begin_item(i, selected)) continue;
        // Only the selected shape shows its fields; the others one line each.
        if (selected) {
            ImGui::TextUnformatted(collision_kind_name(prim.kind));
            if (auto t = ftk::ui::PropertyTable("##shape")) {
                if (collision_shape_rows(prim, "Relative to the mesh's origin").changed) collision_dirty_ = true;
            }
        } else {
            ImGui::TextUnformatted(collision_shape_summary(prim).c_str());
        }
        if (list.end_item()) removed = i;
        if (list.clicked()) selected_primitive_ = selected ? -1 : static_cast<int>(i);
    }
    if (list.add_button("Add shape")) ImGui::OpenPopup("##add_shape");
    if (auto pick = add_shape_menu("##add_shape", "the mesh")) add_primitive(pick->kind, pick->fitted);
    if (removed) {
        collision_primitives_.erase(collision_primitives_.begin() + static_cast<std::ptrdiff_t>(*removed));
        selected_primitive_ = -1;
        collision_dirty_ = true;
    }
}

void MeshEditorContext::draw_collision_panel() {
    auto panel = ftk::ui::Panel(ftk::ui::icon::collision, ctx_title("Collision").c_str());
    if (!panel) return;
    if (!model_) {
        ftk::ui::empty_state(ftk::ui::icon::mesh, "No mesh open", "Open one from the content browser");
        return;
    }

    struct SourceHelp {
        FjMeshCollisionSource value;
        const char* help;
    };
    static constexpr ftk::ui::Choice<FjMeshCollisionSource> SOURCES[] = {
        {FjMeshCollisionSource::render_mesh, "Render mesh"},
        {FjMeshCollisionSource::convex_hull, "Convex hull"},
        {FjMeshCollisionSource::simplified, "Simplified mesh"},
        {FjMeshCollisionSource::custom_mesh, "Custom mesh"},
    };
    static constexpr SourceHelp HELP[] = {
        {FjMeshCollisionSource::render_mesh, "Collides against the visible geometry. Static bodies only"},
        {FjMeshCollisionSource::convex_hull, "The hull of the visible geometry: the only source a moving body can use"},
        {FjMeshCollisionSource::simplified, "A decimated triangle mesh. Static bodies only"},
        {FjMeshCollisionSource::custom_mesh, "Collides against a separate .fjmesh made as a proxy"},
    };

    // A mesh imported before the format kept full-detail collision has
    // nothing to rebuild from, and deriving from the stored result would
    // compound the loss it already carries.
    const bool can_rebuild = model_->has_full_collision();
    if (!can_rebuild) {
        ftk::ui::status(ftk::ui::Severity::Warning,
                   "This mesh predates the format that keeps what collision is built from. "
                   "Reimport it to change the source.");
    }

    InspectorContext ictx{};
    ictx.project_mgr = deps_.project_mgr;
    ictx.icons = deps_.icon_cache;
    static const std::string mesh_exts[] = {".fjmesh"};
    if (auto t = ftk::ui::PropertyTable("##collision")) {
        ImGui::BeginDisabled(!can_rebuild);
        ftk::ui::row("Source", [&] {
            if (ftk::ui::combo("##source", authored_.collision_source, SOURCES).changed) rebuild_collision();
        });
        ImGui::EndDisabled();
        for (const auto& h : HELP) {
            if (h.value == authored_.collision_source) ftk::ui::hint(h.help);
        }
        if (authored_.collision_source == FjMeshCollisionSource::simplified) {
            ImGui::BeginDisabled(!can_rebuild);
            // Rebuilt when the drag lets go: simplifying the helmet takes
            // about a millisecond, enough to make a drag stutter.
            ftk::ui::row("Detail", [&] {
                if (ftk::ui::slider("##detail", authored_.collision_simplify_ratio, 0.01F, 1.0F)
                        .committed) {
                    rebuild_collision();
                }
            });
            ImGui::EndDisabled();
            ftk::ui::hint("The share of the original triangles kept");
        }
        if (authored_.collision_source == FjMeshCollisionSource::custom_mesh) {
            ftk::ui::row("Proxy", [&] {
                (void)asset_slot({.label = "Proxy", .id = "##proxy", .extensions = mesh_exts, .ctx = &ictx},
                                 authored_.collision_mesh);
            });
        }
    }

    draw_primitive_list();

    ftk::ui::subheading("Current geometry");
    // A hull is a point cloud, so it reports no triangles by design.
    const bool is_hull = authored_.collision_source == FjMeshCollisionSource::convex_hull;
    if (auto t = ftk::ui::PropertyTable("##geometry")) {
        ftk::ui::row("Show in preview", [&] {
            if (ftk::ui::checkbox("##show", show_collision_).changed) {
                deps_.vulkan_context->set_show_colliders(show_collision_);
            }
        });
        ftk::ui::row("Vertices", [&] { ftk::ui::readout(group_digits(collision_vertices_.size()).c_str()); });
        ftk::ui::row("Triangles", [&] { ftk::ui::readout(group_digits(collision_indices_.size() / 3).c_str()); });
        ftk::ui::row("Moving bodies", [&] { ftk::ui::readout(is_hull ? "Yes" : "No"); });
    }
}

void MeshEditorContext::draw_info_panel() {
    auto panel = ftk::ui::Panel(ftk::ui::icon::info, ctx_title("Info").c_str());
    if (!panel) return;
    if (!model_) {
        ftk::ui::empty_state(ftk::ui::icon::mesh, "No mesh open", "Open one from the content browser");
        return;
    }

    auto readout = [](const char* label, const std::string& value) {
        ftk::ui::row(label, [&] { ftk::ui::readout(value.c_str()); });
    };
    auto vec = [](const glm::vec3& v) { return std::format("{:.3f}, {:.3f}, {:.3f} m", v.x, v.y, v.z); };

    if (auto t = ftk::ui::PropertyTable("##info")) {
        if (!authored_.source_file.empty()) readout("Imported from", authored_.source_file);
        // total_indices() covers every LOD, so triangles are per LOD below.
        readout("Parts", group_digits(model_->mega_entry_count()));
        readout("Meshlets", group_digits(model_->total_meshlet_count()));
        readout("Indices", group_digits(model_->total_indices()));
    }

    ftk::ui::subheading("Bounds");
    const auto& aabb = model_->local_aabb();
    if (auto t = ftk::ui::PropertyTable("##bounds")) {
        readout("Min", vec(aabb.min));
        readout("Max", vec(aabb.max));
        readout("Size", vec(aabb.max - aabb.min));
        readout("Radius", std::format("{:.3f} m", model_->bounding_radius()));
    }

    ftk::ui::subheading("Levels of detail");
    if (auto t = ftk::ui::PropertyTable("##lods")) {
        const auto& entries = model_->mega_entries();
        uint32_t lod_index = 0;
        for (const auto& lod : model_->lod_levels()) {
            uint32_t indices = 0;
            const uint32_t end = lod.first_mega_entry + lod.mega_entry_count;
            for (uint32_t i = lod.first_mega_entry; i < end && i < entries.size(); ++i) {
                indices += entries[i].index_count;
            }
            const std::string label = "LOD " + std::to_string(lod_index);
            readout(label.c_str(), std::format("{} triangles, {} parts, to {:.3f} of the screen",
                                               group_digits(indices / 3), lod.mega_entry_count,
                                               lod.max_screen_size));
            ++lod_index;
        }
    }

    ftk::ui::subheading("Distance field");
    if (auto t = ftk::ui::PropertyTable("##sdf")) {
        if (model_->has_sdf()) {
            const uint32_t res = model_->sdf_grid_resolution();
            readout("Grid", std::format("{} \xc3\x97 {} \xc3\x97 {}", res, res, res));
            readout("Voxels", group_digits(model_->sdf_grid_data().size()));
        } else {
            readout("Grid", "None");
        }
    }
}

void MeshEditorContext::draw_header_actions() {
    if (!model_ || authored_.source_file.empty()) return;
    // Import the source file again, keeping what was authored here.
    const auto source = find_import_source(fjmesh_abs_path_);
    ImGui::SameLine();
    ImGui::BeginDisabled(source.empty() || !deps_.import_model);
    if (ftk::ui::action(ftk::ui::icon::reload, "Reimport", ftk::ui::ButtonKind::Ghost)) deps_.import_model(source);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("%s", source.empty() ? "The source file isn't next to the mesh any more"
                                               : ("Import " + authored_.source_file + " again").c_str());
}

} // namespace fjell
