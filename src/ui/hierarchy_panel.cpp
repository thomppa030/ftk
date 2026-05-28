#include "ui/hierarchy_panel.hpp"
#include "core/command_history.hpp"
#include "core/commands.hpp"
#include "scene/scene.hpp"
#include "scene/camera_node.hpp"
#include "scene/light_node.hpp"
#include "ui/icon_cache.hpp"
#include "ui/icons_lc.hpp"
#include "ui/panel_widget.hpp"
#include "ui/viewport_manager.hpp"
#include "renderer/camera.hpp"
#include "renderer/components/mesh_component.hpp"
#include "renderer/resources/model.hpp"

#include <imgui.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace fjell {

namespace {

struct NodeVisuals {
    ImVec4 border_color;
    const char* icon_name;
};

NodeVisuals node_visuals(const Node& obj) {
    if (dynamic_cast<const DirectionalLightNode*>(&obj) ||
        dynamic_cast<const PointLightNode*>(&obj) ||
        dynamic_cast<const SpotLightNode*>(&obj)) {
        const char* icon = "point_light";
        if (dynamic_cast<const DirectionalLightNode*>(&obj)) icon = "dir_light";
        return {{0.96f, 0.78f, 0.26f, 1.0f}, icon};
    }
    if (dynamic_cast<const VolumetricFogNode*>(&obj)) {
        return {{0.55f, 0.70f, 0.85f, 1.0f}, "node"};
    }
    if (dynamic_cast<const WorldEnvironmentNode*>(&obj)) {
        return {{0.4f, 0.6f, 0.9f, 1.0f}, "node"};
    }
    if (dynamic_cast<const CameraNode*>(&obj)) {
        return {{0.31f, 0.79f, 0.63f, 1.0f}, "camera"};
    }
    if (!obj.preset_source.empty()) {
        return {{0.3f, 0.7f, 0.65f, 1.0f}, "node"};
    }
    if (!obj.is_spatial()) {
        return {{0.85f, 0.72f, 0.40f, 1.0f}, "folder"};
    }
    if (obj.components.get<MeshComponent>()) {
        return {{0.36f, 0.55f, 0.94f, 1.0f}, "mesh"};
    }
    return {{0.50f, 0.50f, 0.50f, 1.0f}, "node"};
}

// Draw the shared "add object" menu items used by both toolbar + and context menu
void draw_add_object_menu(Delegate<void(const std::string&)>& on_add) {
    if (ImGui::MenuItem("Cube")) on_add.broadcast("cube");
    if (ImGui::MenuItem("Plane")) on_add.broadcast("plane");
    if (ImGui::MenuItem("Sphere")) on_add.broadcast("sphere");
    ImGui::Separator();
    if (ImGui::MenuItem("Sprite")) on_add.broadcast("sprite");
    if (ImGui::MenuItem("Empty Node")) on_add.broadcast("empty");
    if (ImGui::MenuItem("Folder")) on_add.broadcast("folder");
    ImGui::Separator();
    if (ImGui::MenuItem("Camera")) on_add.broadcast("camera");
    ImGui::Separator();
    if (ImGui::MenuItem("Directional Light")) on_add.broadcast("light:directional");
    if (ImGui::MenuItem("Point Light")) on_add.broadcast("light:point");
    if (ImGui::MenuItem("Spot Light")) on_add.broadcast("light:spot");
    ImGui::Separator();
    if (ImGui::MenuItem("Volumetric Fog")) on_add.broadcast("volumetric_fog");
    if (ImGui::MenuItem("World Environment")) on_add.broadcast("world_environment");
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

    // Text filter (case-insensitive substring)
    if (filter_text_[0] == '\0') return true;

    // Simple case-insensitive search
    auto to_lower = [](const std::string& s) {
        std::string out = s;
        for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    };
    std::string name_lower = to_lower(obj.name);
    std::string filter_lower = to_lower(filter_text_);
    return name_lower.find(filter_lower) != std::string::npos;
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
                          uint32_t* selected_id, CommandHistory* history,
                          IconCache* icons) {
    scene_ = scene;
    viewports_ = viewports;
    selected_id_ = selected_id;
    history_ = history;
    icons_ = icons;
}

void HierarchyPanel::draw() {
    if (auto p = Panel(ICON_LC_LIST_TREE, "Hierarchy")) {
        remove_pending_id_ = 0;

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

        // ── Toolbar row ──────────────────────────────────────────────
        float h = ImGui::GetFrameHeight();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 4.0f));
        if (ImGui::Button(ICON_LC_PLUS "##add", {h, h})) {
            ImGui::OpenPopup("##HierarchyAdd");
        }
        ImGui::PopStyleVar();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Add object");
        }
        ImGui::SameLine();

        // Type filter combo (fixed width, right-aligned)
        float combo_w = 70.0f;
        float avail_w = ImGui::GetContentRegionAvail().x;
        float search_w = avail_w - combo_w - ImGui::GetStyle().ItemSpacing.x;
        if (search_w < 60.0f) search_w = 60.0f;

        ImGui::SetNextItemWidth(search_w);
        ImGui::InputTextWithHint("##filter", "Search...", filter_text_, sizeof(filter_text_));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        const char* type_labels[] = {"All", "Mesh", "Light", "Camera", "Folder", "Empty"};
        ImGui::Combo("##type", &type_filter_, type_labels, 6);

        if (ImGui::BeginPopup("##HierarchyAdd")) {
            draw_add_object_menu(on_add_primitive);
            ImGui::EndPopup();
        }

        ImGui::Separator();

        // Build parent→children map once per frame (O(n) instead of O(n²) per node)
        children_map_.clear();
        for (auto& obj : scene_->objects()) {
            children_map_[obj->parent_id].push_back(obj.get());
        }

        // Draw root objects
        auto it = children_map_.find(-1);
        if (it != children_map_.end()) {
            for (auto* obj : it->second) {
                draw_node(*obj);
            }
        }

        // Empty space = drop target for unparent to root + preset instantiation
        ImVec2 avail = ImGui::GetContentRegionAvail();
        if (avail.y > 0) {
            ImGui::Dummy(avail);
            if (ImGui::BeginDragDropTarget()) {
                if (const auto* payload = ImGui::AcceptDragDropPayload("HIERARCHY_NODE")) {
                    uint32_t dragged_id = *static_cast<const uint32_t*>(payload->Data);
                    if (history_) {
                        auto* child = scene_->find(dragged_id);
                        auto name = child ? child->name : "";
                        history_->execute(std::make_unique<ReparentCommand>(
                            *scene_, dragged_id, child ? child->parent_id : -1,
                            -1, name));
                    } else {
                        scene_->reparent(dragged_id, -1);
                    }
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
            draw_add_object_menu(on_add_primitive);
            ImGui::EndPopup();
        }

        // Deferred removal
        if (remove_pending_id_ != 0) {
            on_remove_object.broadcast(remove_pending_id_);
            selection_.erase(remove_pending_id_);
        }
    }
}

void HierarchyPanel::draw_node(Node& obj) {
    // Skip nodes that don't match the filter (and have no matching descendants)
    bool filtering = is_filtering();
    if (filtering && !subtree_matches_filter(obj)) return;

    // Record draw order for shift-click range selection
    draw_order_.push_back(obj.id);

    auto child_it = children_map_.find(static_cast<int32_t>(obj.id));
    bool has_children = child_it != children_map_.end() && !child_it->second.empty();

    ImGui::PushID(static_cast<int>(obj.id));

    bool is_selected = selection_.contains(obj.id);
    bool is_primary = (obj.id == *selected_id_);

    ImGuiTreeNodeFlags flags =
        ImGuiTreeNodeFlags_OpenOnArrow |
        ImGuiTreeNodeFlags_SpanAvailWidth |
        ImGuiTreeNodeFlags_FramePadding;

    if (!has_children) {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    }
    if (is_selected) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }

    // Auto-expand when filtering so matches are visible
    if (filtering && has_children) {
        ImGui::SetNextItemOpen(true);
    }

    // Determine visuals for this node type
    auto visuals = node_visuals(obj);

    // Row styling — equal vertical padding for centered text
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {4.0f, 6.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0.0f, 2.0f});

    // Primary selection = brighter, secondary = dimmer
    if (is_selected) {
        if (is_primary) {
            ImGui::PushStyleColor(ImGuiCol_Header, {0.22f, 0.27f, 0.58f, 1.0f});
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, {0.26f, 0.32f, 0.65f, 1.0f});
        } else {
            ImGui::PushStyleColor(ImGuiCol_Header, {0.18f, 0.22f, 0.45f, 1.0f});
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, {0.22f, 0.27f, 0.52f, 1.0f});
        }
        ImGui::PushStyleColor(ImGuiCol_Text, {0.95f, 0.95f, 0.95f, 1.0f});
    }

    std::string padded_label = "          " + obj.name;
    float tree_indent = ImGui::GetTreeNodeToLabelSpacing();

    bool node_open = ImGui::TreeNodeEx(padded_label.c_str(), flags);

    if (is_selected) {
        ImGui::PopStyleColor(3);
    }

    ImGui::PopStyleVar(2);

    // Capture row rect and hover state
    ImVec2 row_min = ImGui::GetItemRectMin();
    ImVec2 row_max = ImGui::GetItemRectMax();
    float row_h = row_max.y - row_min.y;
    bool row_hovered = ImGui::IsItemHovered();

    // Icon size based on actual row height
    float icon_size = row_h * 0.8f;

    // Draw left border stripe
    {
        ImU32 border_col = ImGui::ColorConvertFloat4ToU32(visuals.border_color);
        ImGui::GetWindowDrawList()->AddRectFilled(
            {row_min.x, row_min.y},
            {row_min.x + 3.0f, row_max.y},
            border_col);
    }

    // Draw icon over the padded space
    if (icons_) {
        auto icon_desc = icons_->icon(visuals.icon_name);
        if (icon_desc) {
            ImVec2 icon_pos = {row_min.x + tree_indent + 2.0f,
                               row_min.y + (row_h - icon_size) * 0.5f};
            ImGui::GetWindowDrawList()->AddImage(
                icon_desc, icon_pos,
                {icon_pos.x + icon_size, icon_pos.y + icon_size});
        }
    }

    // Delete "button" — drawn manually, click detected via mouse pos
    if (row_hovered || is_selected) {
        float btn_size = 14.0f;
        float margin = 8.0f;
        ImVec2 btn_min = {row_max.x - btn_size - margin,
                          row_min.y + (row_max.y - row_min.y - btn_size) * 0.5f};
        ImVec2 btn_max = {btn_min.x + btn_size, btn_min.y + btn_size};

        ImVec2 mouse = ImGui::GetMousePos();
        bool btn_hovered = mouse.x >= btn_min.x && mouse.x <= btn_max.x &&
                           mouse.y >= btn_min.y && mouse.y <= btn_max.y;

        ImU32 bg_col = btn_hovered
            ? ImGui::ColorConvertFloat4ToU32({0.8f, 0.2f, 0.2f, 0.6f})
            : IM_COL32(0, 0, 0, 0);
        ImU32 text_col = ImGui::ColorConvertFloat4ToU32({0.7f, 0.7f, 0.7f, 1.0f});

        auto* draw_list = ImGui::GetWindowDrawList();
        if (btn_hovered) {
            draw_list->AddRectFilled(btn_min, btn_max, bg_col, 2.0f);
        }
        // Center the "X" text in the button rect
        ImVec2 text_size = ImGui::CalcTextSize("X");
        ImVec2 text_pos = {btn_min.x + (btn_size - text_size.x) * 0.5f,
                           btn_min.y + (btn_size - text_size.y) * 0.5f};
        draw_list->AddText(text_pos, text_col, "X");

        if (btn_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            remove_pending_id_ = obj.id;
        }
    }

    // Click handling with multi-select (Ctrl / Shift)
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        if (ImGui::GetIO().KeyCtrl) {
            toggle_select(obj.id);
        } else if (ImGui::GetIO().KeyShift) {
            range_select(obj.id);
        } else {
            select(obj.id);
        }
    }

    // Double-click to focus camera
    if (row_hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        select(obj.id);
        if (auto* spatial = obj.as_spatial()) {
            glm::vec3 center = glm::vec3(spatial->world_matrix_[3]);
            float radius = 1.0f;
            auto* mesh = obj.components.get<MeshComponent>();
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
        if (ImGui::MenuItem("Duplicate", "Ctrl+D")) {
            on_duplicate.broadcast();
        }
        if (ImGui::MenuItem("Delete", "Delete")) {
            remove_pending_id_ = obj.id;
        }
        ImGui::EndPopup();
    }

    // Drag source — must follow immediately after TreeNodeEx
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
        ImGui::SetDragDropPayload("HIERARCHY_NODE", &obj.id, sizeof(obj.id));
        if (selection_.size() > 1 && selection_.contains(obj.id)) {
            ImGui::Text("%zu objects", selection_.size());
        } else {
            ImGui::Text("%s", obj.name.c_str());
        }
        ImGui::EndDragDropSource();
    }

    // Drop target — reparent dragged object under this one, or instantiate preset
    if (ImGui::BeginDragDropTarget()) {
        if (const auto* payload = ImGui::AcceptDragDropPayload("HIERARCHY_NODE")) {
            uint32_t dragged_id = *static_cast<const uint32_t*>(payload->Data);
            if (dragged_id != obj.id) {
                bool is_ancestor = false;
                int32_t check = static_cast<int32_t>(obj.id);
                while (check != -1) {
                    if (static_cast<uint32_t>(check) == dragged_id) {
                        is_ancestor = true;
                        break;
                    }
                    const auto* p = scene_->find(static_cast<uint32_t>(check));
                    check = p ? p->parent_id : -1;
                }
                if (!is_ancestor) {
                    if (history_) {
                        auto* child = scene_->find(dragged_id);
                        auto old_parent = child ? child->parent_id : -1;
                        auto name = child ? child->name : "";
                        history_->execute(std::make_unique<ReparentCommand>(
                            *scene_, dragged_id, old_parent,
                            static_cast<int32_t>(obj.id), name));
                    } else {
                        scene_->reparent(dragged_id, static_cast<int32_t>(obj.id));
                    }
                }
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

    // Recurse children
    if (has_children && node_open) {
        for (auto* child : child_it->second) {
            draw_node(*child);
        }
        ImGui::TreePop();
    }

    ImGui::PopID();
}

} // namespace fjell
