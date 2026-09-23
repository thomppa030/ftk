#include "ui/hierarchy_panel.hpp"
#include "core/command_history.hpp"
#include "core/commands.hpp"
#include "scene/scene.hpp"
#include "scene/camera_node.hpp"
#include "scene/light_node.hpp"
#include "ui/inspectors/inspector_registry.hpp"
#include "ui/kit/button.hpp"
#include "ui/kit/choice.hpp"
#include "ui/kit/icons.hpp"
#include "ui/kit/search.hpp"
#include "ui/panel_widget.hpp"
#include "ui/viewport_manager.hpp"
#include "renderer/camera.hpp"
#include "renderer/components/mesh_component.hpp"
#include "renderer/resources/model.hpp"

#include <imgui.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace fjell {

namespace {

// What a hierarchy drag carries: the dragged nodes' ids, in scene order.
constexpr const char* NODES_PAYLOAD = "HIERARCHY_NODE";

// The toolbar's node-kind filter, and the least the search beside it keeps.
constexpr float TYPE_FILTER_WIDTH = 96.0f;
constexpr float MIN_SEARCH_WIDTH = 60.0f;

// A node type's icon, and the category it is tinted in. A plain node (an
// empty, a mesh) has none: what it is shows in its component dots.
struct NodeVisuals {
    const char* icon;
    std::optional<theme::Category> category;
};

NodeVisuals node_visuals(const Node& obj) {
    using C = theme::Category;
    if (dynamic_cast<const DirectionalLightNode*>(&obj)) return {ui::icon::directional_light, C::Light};
    if (dynamic_cast<const PointLightNode*>(&obj)) return {ui::icon::point_light, C::Light};
    if (dynamic_cast<const SpotLightNode*>(&obj)) return {ui::icon::spot_light, C::Light};
    if (dynamic_cast<const VolumetricFogNode*>(&obj)) return {ui::icon::volumetric_fog, C::Environment};
    if (dynamic_cast<const FogVolumeNode*>(&obj)) return {ui::icon::fog_volume, C::Environment};
    if (dynamic_cast<const WorldEnvironmentNode*>(&obj)) return {ui::icon::world_environment, C::Environment};
    if (dynamic_cast<const CameraNode*>(&obj)) return {ui::icon::camera, C::Camera};
    if (!obj.preset_source.empty()) return {ui::icon::preset, C::Structure};
    if (!obj.is_spatial()) return {ui::icon::folder, C::Structure};
    if (obj.components.get<MeshComponent>() != nullptr) return {ui::icon::mesh, std::nullopt};
    return {ui::icon::transform, std::nullopt};
}

// A dot per category composed onto the node, first come first: its
// components' categories, then Logic for scripts.
std::vector<theme::Category> node_dots(const Node& obj) {
    std::vector<theme::Category> dots;
    auto add = [&](theme::Category category) {
        if (std::ranges::find(dots, category) == dots.end()) dots.push_back(category);
    };
    const auto& inspectors = component_inspectors();
    obj.components.for_each([&](ComponentTypeId id, const void*) {
        if (const auto* inspector = inspectors.find(id)) add(inspector->category);
    });
    if (!obj.scripts.empty()) add(theme::Category::Logic);
    return dots;
}

// The "add object" menu, shared by the toolbar's add button and the right
// click on empty space. Returns what was picked, or empty.
std::string draw_add_object_menu() {
    std::string picked;
    auto item = [&](const char* label, const char* type) {
        if (ImGui::MenuItem(label)) picked = type;
    };
    item("Cube", "cube");
    item("Plane", "plane");
    item("Sphere", "sphere");
    ImGui::Separator();
    item("Sprite", "sprite");
    item("Empty Node", "empty");
    item("Folder", "folder");
    ImGui::Separator();
    item("Camera", "camera");
    ImGui::Separator();
    item("Directional Light", "light:directional");
    item("Point Light", "light:point");
    item("Spot Light", "light:spot");
    ImGui::Separator();
    item("Volumetric Fog", "volumetric_fog");
    item("Fog Volume", "fog_volume");
    item("World Environment", "world_environment");
    return picked;
}

} // namespace

// ── Selection API ────────────────────────────────────────────────────────

void HierarchyPanel::select(uint32_t id) {
    selection_.clear();
    selection_.insert(id);
    *selected_id_ = id;
    last_clicked_id_ = id;
}

void HierarchyPanel::toggle_select(uint32_t id) {
    if (selection_.contains(id)) {
        selection_.erase(id);
        // Update primary selection
        if (*selected_id_ == id) {
            *selected_id_ = selection_.empty() ? 0 : *selection_.begin();
        }
    } else {
        selection_.insert(id);
        *selected_id_ = id;
    }
    last_clicked_id_ = id;
}

void HierarchyPanel::range_select(uint32_t id) {
    if (last_clicked_id_ == 0 || draw_order_.empty()) {
        select(id);
        return;
    }

    // Find positions in draw order
    auto it_anchor = std::find(draw_order_.begin(), draw_order_.end(), last_clicked_id_);
    auto it_target = std::find(draw_order_.begin(), draw_order_.end(), id);

    if (it_anchor == draw_order_.end() || it_target == draw_order_.end()) {
        select(id);
        return;
    }

    // Select everything between anchor and target (inclusive)
    if (it_anchor > it_target) std::swap(it_anchor, it_target);

    selection_.clear();
    for (auto it = it_anchor; it <= it_target; ++it) {
        selection_.insert(*it);
    }
    *selected_id_ = id;
    // Don't update last_clicked_id_ — anchor stays for further shift-clicks
}

void HierarchyPanel::add_to_selection(uint32_t id) {
    selection_.insert(id);
    *selected_id_ = id;
    last_clicked_id_ = id;
}

void HierarchyPanel::clear_selection() {
    selection_.clear();
    *selected_id_ = 0;
    last_clicked_id_ = 0;
}

// ── Filter ───────────────────────────────────────────────────────────────

bool HierarchyPanel::matches_filter(const Node& obj) const {
    // Type filter: 0=All, 1=Mesh, 2=Light, 3=Camera, 4=Folder, 5=Empty
    if (type_filter_ != 0) {
        bool type_match = false;
        switch (type_filter_) {
            case 1: type_match = obj.components.get<MeshComponent>() != nullptr; break;
            case 2: type_match = dynamic_cast<const DirectionalLightNode*>(&obj) ||
                                 dynamic_cast<const PointLightNode*>(&obj) ||
                                 dynamic_cast<const SpotLightNode*>(&obj); break;
            case 3: type_match = dynamic_cast<const CameraNode*>(&obj) != nullptr; break;
            case 4: type_match = !obj.is_spatial(); break;
            case 5: type_match = obj.is_spatial() && !obj.components.get<MeshComponent>() &&
                                 !dynamic_cast<const CameraNode*>(&obj) &&
                                 !dynamic_cast<const DirectionalLightNode*>(&obj) &&
                                 !dynamic_cast<const PointLightNode*>(&obj) &&
                                 !dynamic_cast<const SpotLightNode*>(&obj); break;
            default: break;
        }
        if (!type_match) return false;
    }

    return ui::matches(obj.name, filter_text_);
}

bool HierarchyPanel::subtree_matches_filter(const Node& obj) const {
    if (matches_filter(obj)) return true;
    auto it = children_map_.find(static_cast<int32_t>(obj.id));
    if (it != children_map_.end()) {
        for (const auto* child : it->second) {
            if (subtree_matches_filter(*child)) return true;
        }
    }
    return false;
}

// ── Init / Draw ──────────────────────────────────────────────────────────

void HierarchyPanel::init(Scene* scene, ViewportManager* viewports,
                          uint32_t* selected_id, CommandHistory* history) {
    scene_ = scene;
    viewports_ = viewports;
    selected_id_ = selected_id;
    history_ = history;
}

void HierarchyPanel::draw() {
    if (auto p = Panel(ui::icon::hierarchy, "Hierarchy")) {
        delete_pending_ = false;
        // A row drawn before the one under the cursor has already shown the
        // drag; it shows why a drop is refused a frame late.
        refusal_shown_ = std::move(refusal_now_);
        refusal_now_.clear();

        // The search box owns the keyboard while it is active, so F2 there
        // must not start a rename.
        if (ImGui::IsWindowFocused() && !ImGui::IsAnyItemActive() &&
            ImGui::IsKeyPressed(ImGuiKey_F2, false) && *selected_id_ != 0) {
            if (const auto* obj = scene_->find(*selected_id_)) rename_.start(obj->id, obj->name);
        }

        // Sync selection with external changes to selected_id_
        // (e.g. viewport click, add object, etc.)
        if (*selected_id_ != 0 && !selection_.contains(*selected_id_)) {
            selection_.clear();
            selection_.insert(*selected_id_);
            last_clicked_id_ = *selected_id_;
        } else if (*selected_id_ == 0 && !selection_.empty()) {
            selection_.clear();
            last_clicked_id_ = 0;
        }

        // Build draw-order list for shift-click range selection
        draw_order_.clear();

        // Toolbar: add, search, and which kind of node to show.
        if (ui::icon_button("##add", ui::icon::add, "Add object", ui::ButtonKind::Secondary)) {
            ImGui::OpenPopup("##HierarchyAdd");
        }
        ImGui::SameLine();
        const float type_w = TYPE_FILTER_WIDTH;
        const float search_w = std::max(ImGui::GetContentRegionAvail().x - type_w - ImGui::GetStyle().ItemSpacing.x,
                                        MIN_SEARCH_WIDTH);
        ImGui::SetNextItemWidth(search_w);
        ui::search_field("##filter", filter_text_);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        static constexpr const char* TYPE_NAMES[] = {"All", "Mesh", "Light", "Camera", "Folder", "Empty"};
        (void)ui::choice("##type", type_filter_, TYPE_NAMES);

        if (ImGui::BeginPopup("##HierarchyAdd")) {
            add_object(draw_add_object_menu());
            ImGui::EndPopup();
        }

        ImGui::Separator();

        // Build parent→children map once per frame (O(n) instead of O(n²) per node)
        children_map_.clear();
        for (auto& obj : scene_->objects()) {
            children_map_[obj->parent_id].push_back(obj.get());
        }

        // A rename outlives its row only until the node is gone.
        if (rename_.editing() && *selected_id_ == 0) rename_.cancel();

        if (auto tree = ui::Tree("##scene")) {
            auto it = children_map_.find(-1);
            if (it != children_map_.end()) {
                for (auto* obj : it->second) draw_node(*obj, 0);
            }
        }

        // Empty space = drop target for unparent to root + preset instantiation
        ImVec2 avail = ImGui::GetContentRegionAvail();
        if (avail.y > 0) {
            ImGui::Dummy(avail);
            if (ImGui::BeginDragDropTarget()) {
                // Under everything: last, at the top level.
                if (const auto* payload = ImGui::AcceptDragDropPayload(NODES_PAYLOAD)) {
                    move_nodes(payload_ids(*payload), -1, 0);
                }
                if (const auto* payload = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                    std::string path(static_cast<const char*>(payload->Data));
                    if (path.ends_with(".fjp")) {
                        on_instantiate_preset.broadcast(path, -1);
                    }
                }
                ImGui::EndDragDropTarget();
            }
        }

        // Click on empty space = deselect all
        if (ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered() &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            clear_selection();
        }

        // Right-click on empty space
        if (ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered() &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            ImGui::OpenPopup("##HierarchyContext");
        }
        if (ImGui::BeginPopup("##HierarchyContext")) {
            add_object(draw_add_object_menu());
            ImGui::EndPopup();
        }

        if (delete_pending_) on_delete_selection.broadcast();
    }
}

void HierarchyPanel::draw_node(Node& obj, int depth) {
    // Skip nodes that don't match the filter (and have no matching descendants)
    const bool filtering = is_filtering();
    if (filtering && !subtree_matches_filter(obj)) return;

    // Record draw order for shift-click range selection
    draw_order_.push_back(obj.id);

    auto child_it = children_map_.find(static_cast<int32_t>(obj.id));
    const bool has_children = child_it != children_map_.end() && !child_it->second.empty();

    const NodeVisuals visuals = node_visuals(obj);
    const std::vector<theme::Category> dots = node_dots(obj);
    const ui::RowSelection selection = obj.id == *selected_id_  ? ui::RowSelection::Primary
                                     : selection_.contains(obj.id) ? ui::RowSelection::Secondary
                                                                   : ui::RowSelection::None;
    const std::string id = std::to_string(obj.id);
    const auto row = ui::tree_row({
        .id = id.c_str(),
        .name = obj.name,
        .depth = depth,
        .has_children = has_children,
        .icon = visuals.icon,
        .category = visuals.category,
        .dots = dots,
        .selection = selection,
        .query = filter_text_,
        // A node shown only for what is under it.
        .dimmed = filtering && !matches_filter(obj),
        .force_open = filtering,
        .rename = &rename_,
        .key = obj.id,
    });
    // The row is the last item: its menu, drag and drop attach to it.
    ImGui::PushID(id.c_str());

    if (row.renamed) {
        if (history_) {
            history_->execute(std::make_unique<RenameCommand>(*scene_, obj.id, obj.name, *row.renamed));
        } else {
            obj.name = *row.renamed;
            scene_->mark_dirty();
        }
    }

    // Click handling with multi-select (Ctrl / Shift)
    if (row.clicked) {
        if (ImGui::GetIO().KeyCtrl) {
            toggle_select(obj.id);
        } else if (ImGui::GetIO().KeyShift) {
            range_select(obj.id);
        } else {
            select(obj.id);
        }
    }
    if (row.double_clicked) {
        select(obj.id);
        frame(obj);
    }

    // Right-click context menu on node
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        // If right-clicked node isn't in selection, make it the sole selection
        if (!selection_.contains(obj.id)) {
            select(obj.id);
        }
        ImGui::OpenPopup("##NodeContext");
    }
    if (ImGui::BeginPopup("##NodeContext")) {
        if (ImGui::MenuItem("Rename", "F2")) {
            rename_.start(obj.id, obj.name);
        }
        if (ImGui::MenuItem("Duplicate", "Ctrl+D")) {
            on_duplicate.broadcast();
        }
        if (ImGui::MenuItem("Delete", "Delete")) {
            delete_pending_ = true;
        }
        ImGui::EndPopup();
    }

    // The row is still the last item: nothing above added one. Dragging a
    // selected row drags the whole selection.
    if (ImGui::BeginDragDropSource()) {
        const std::vector<uint32_t> ids = dragged_ids(obj.id);
        ImGui::SetDragDropPayload(NODES_PAYLOAD, ids.data(), ids.size() * sizeof(uint32_t));
        const std::string what = ids.size() > 1 ? std::to_string(ids.size()) + " objects" : obj.name;
        ui::drag_preview(visuals.icon, visuals.category, what, refusal_shown_);
        ImGui::EndDragDropSource();
    }

    // Dropped rows go in front of this one, after it, or into it.
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* dragging = ImGui::GetDragDropPayload();
            dragging != nullptr && dragging->IsDataType(NODES_PAYLOAD)) {
            const std::vector<uint32_t> ids = payload_ids(*dragging);
            const ui::DropPlace place = ui::drop_place();
            const Target target = drop_target(obj, depth, row.open, place, ids);
            std::string refusal = refuse_drop(ids, target.parent_id);
            if (refusal.empty()) {
                ui::draw_drop(place, target.depth);
                if (const auto* payload = ImGui::AcceptDragDropPayload(
                        NODES_PAYLOAD, ImGuiDragDropFlags_AcceptBeforeDelivery
                                           | ImGuiDragDropFlags_AcceptNoDrawDefaultRect);
                    payload != nullptr && payload->IsDelivery()) {
                    move_nodes(ids, target.parent_id, target.before_id);
                }
            } else {
                refusal_now_ = std::move(refusal);
            }
        }
        if (const auto* payload = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
            std::string path(static_cast<const char*>(payload->Data));
            if (path.ends_with(".fjp")) {
                on_instantiate_preset.broadcast(path, static_cast<int32_t>(obj.id));
            }
        }
        ImGui::EndDragDropTarget();
    }

    ImGui::PopID();
    if (row.open) {
        for (auto* child : child_it->second) draw_node(*child, depth + 1);
    }
}

std::vector<uint32_t> HierarchyPanel::dragged_ids(uint32_t row_id) const {
    if (!selection_.contains(row_id)) return {row_id};
    // A node whose ancestor is also dragged goes along with it, not on its own.
    auto under_another = [&](uint32_t id) {
        const Node* node = scene_->find(id);
        for (int32_t up = node ? node->parent_id : -1; up != -1;) {
            if (selection_.contains(static_cast<uint32_t>(up))) return true;
            const Node* parent = scene_->find(static_cast<uint32_t>(up));
            up = parent ? parent->parent_id : -1;
        }
        return false;
    };
    std::vector<uint32_t> ids;
    for (const auto& obj : scene_->objects()) {
        if (selection_.contains(obj->id) && !under_another(obj->id)) ids.push_back(obj->id);
    }
    return ids;
}

std::vector<uint32_t> HierarchyPanel::payload_ids(const ImGuiPayload& payload) {
    std::vector<uint32_t> ids(static_cast<std::size_t>(payload.DataSize) / sizeof(uint32_t));
    std::memcpy(ids.data(), payload.Data, ids.size() * sizeof(uint32_t));
    return ids;
}

HierarchyPanel::Target HierarchyPanel::drop_target(const Node& row, int depth, bool open, ui::DropPlace place,
                                                   const std::vector<uint32_t>& ids) const {
    auto siblings_of = [&](int32_t parent) -> const std::vector<Node*>* {
        auto it = children_map_.find(parent);
        return it != children_map_.end() ? &it->second : nullptr;
    };
    auto dragged = [&](uint32_t id) { return std::ranges::find(ids, id) != ids.end(); };
    // The first of `parent`'s children from `from` on that isn't being moved.
    auto first_staying = [&](int32_t parent, std::size_t from) -> uint32_t {
        const auto* siblings = siblings_of(parent);
        if (siblings == nullptr) return 0;
        for (std::size_t i = from; i < siblings->size(); ++i) {
            if (!dragged((*siblings)[i]->id)) return (*siblings)[i]->id;
        }
        return 0;
    };
    auto index_among_siblings = [&]() -> std::size_t {
        const auto* siblings = siblings_of(row.parent_id);
        if (siblings == nullptr) return 0;
        auto it = std::ranges::find(*siblings, &row);
        return static_cast<std::size_t>(it - siblings->begin());
    };
    const auto self = static_cast<int32_t>(row.id);
    switch (place) {
        case ui::DropPlace::Before:
            return {row.parent_id, first_staying(row.parent_id, index_among_siblings()), depth};
        case ui::DropPlace::Into:
            return {self, 0, depth + 1};
        case ui::DropPlace::After:
            // Below an open row is its first child's place.
            if (open) return {self, first_staying(self, 0), depth + 1};
            return {row.parent_id, first_staying(row.parent_id, index_among_siblings() + 1), depth};
    }
    return {row.parent_id, 0, depth};
}

std::string HierarchyPanel::refuse_drop(const std::vector<uint32_t>& ids, int32_t parent_id) const {
    for (int32_t up = parent_id; up != -1;) {
        const Node* node = scene_->find(static_cast<uint32_t>(up));
        if (node == nullptr) break;
        if (std::ranges::find(ids, node->id) != ids.end()) {
            return up == parent_id ? node->name + " can't go inside itself"
                                   : node->name + " can't go inside something under it";
        }
        up = node->parent_id;
    }
    return {};
}

void HierarchyPanel::move_nodes(const std::vector<uint32_t>& ids, int32_t parent_id, uint32_t before_id) {
    if (ids.empty()) return;
    const Node* first = scene_->find(ids.front());
    const std::string what = ids.size() > 1 ? std::to_string(ids.size()) + " objects"
                                            : (first ? first->name : std::string("object"));
    auto command = std::make_unique<MoveNodesCommand>(*scene_, ids, parent_id, before_id, "Move " + what);
    if (history_) {
        history_->execute(std::move(command));
    } else {
        command->execute();
    }
    scene_->mark_dirty();
}

void HierarchyPanel::add_object(const std::string& type) {
    if (type.empty()) return;
    on_add_primitive.broadcast(type);
    // The new node is selected by now. An empty is only a name until it
    // holds something, so it is named first.
    if (type == "empty") {
        if (const Node* added = scene_->find(*selected_id_)) rename_.start(added->id, added->name);
    }
}

void HierarchyPanel::frame(const Node& obj) {
    const auto* spatial = obj.as_spatial();
    if (spatial == nullptr) return;
    glm::vec3 center = glm::vec3(spatial->world_matrix_[3]);
    float radius = 1.0f;
    const auto* mesh = obj.components.get<MeshComponent>();
    if (mesh && mesh->model && !mesh->model->collision_vertices().empty()) {
        float max_r2 = 0.0f;
        for (const auto& v : mesh->model->collision_vertices()) {
            auto wv = glm::vec3(spatial->world_matrix_ * glm::vec4(v, 1.0f));
            max_r2 = std::max(max_r2, glm::dot(wv - center, wv - center));
        }
        radius = std::max(std::sqrt(max_r2), 0.1f);
    }
    if (auto* cam = viewports_->active_camera()) {
        cam->focus(center, radius);
    }
}

} // namespace fjell
