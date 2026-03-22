#include "ui/hierarchy_panel.hpp"
#include "core/command_history.hpp"
#include "core/commands.hpp"
#include "scene/scene.hpp"
#include "scene/camera_node.hpp"
#include "scene/light_node.hpp"
#include "ui/icon_cache.hpp"
#include "ui/viewport_manager.hpp"
#include "renderer/camera.hpp"
#include "renderer/mesh_component.hpp"
#include "renderer/model.hpp"

#include <imgui.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <cmath>

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
    if (!obj.is_spatial()) {
        return {{0.85f, 0.72f, 0.40f, 1.0f}, "folder"};
    }
    if (obj.components.get<MeshComponent>()) {
        return {{0.36f, 0.55f, 0.94f, 1.0f}, "mesh"};
    }
    return {{0.50f, 0.50f, 0.50f, 1.0f}, "node"};
}

} // namespace

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
    if (ImGui::Begin("Hierarchy")) {
        remove_pending_id_ = 0;

        // Add button at top
        if (ImGui::SmallButton("+")) {
            ImGui::OpenPopup("##HierarchyAdd");
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Add");

        if (ImGui::BeginPopup("##HierarchyAdd")) {
            if (ImGui::MenuItem("Cube")) on_add_primitive.broadcast("cube");
            if (ImGui::MenuItem("Plane")) on_add_primitive.broadcast("plane");
            if (ImGui::MenuItem("Sphere")) on_add_primitive.broadcast("sphere");
            ImGui::Separator();
            if (ImGui::MenuItem("Sprite")) on_add_primitive.broadcast("sprite");
            if (ImGui::MenuItem("Empty Node")) on_add_primitive.broadcast("empty");
            if (ImGui::MenuItem("Folder")) on_add_primitive.broadcast("folder");
            ImGui::Separator();
            if (ImGui::MenuItem("Camera")) on_add_primitive.broadcast("camera");
            ImGui::Separator();
            if (ImGui::MenuItem("Directional Light")) on_add_primitive.broadcast("light:directional");
            if (ImGui::MenuItem("Point Light")) on_add_primitive.broadcast("light:point");
            if (ImGui::MenuItem("Spot Light")) on_add_primitive.broadcast("light:spot");
            ImGui::Separator();
            if (ImGui::MenuItem("Volumetric Fog")) on_add_primitive.broadcast("volumetric_fog");
            if (ImGui::MenuItem("World Environment")) on_add_primitive.broadcast("world_environment");
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

        // Empty space = drop target for unparent to root
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
                ImGui::EndDragDropTarget();
            }
        }

        // Right-click on empty space
        if (ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered() &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            ImGui::OpenPopup("##HierarchyContext");
        }
        if (ImGui::BeginPopup("##HierarchyContext")) {
            if (ImGui::MenuItem("Add Cube")) on_add_primitive.broadcast("cube");
            if (ImGui::MenuItem("Add Plane")) on_add_primitive.broadcast("plane");
            if (ImGui::MenuItem("Add Sphere")) on_add_primitive.broadcast("sphere");
            ImGui::Separator();
            if (ImGui::MenuItem("Add Sprite")) on_add_primitive.broadcast("sprite");
            if (ImGui::MenuItem("Add Empty Node")) on_add_primitive.broadcast("empty");
            if (ImGui::MenuItem("Add Folder")) on_add_primitive.broadcast("folder");
            ImGui::Separator();
            if (ImGui::MenuItem("Add Camera")) on_add_primitive.broadcast("camera");
            ImGui::Separator();
            if (ImGui::MenuItem("Add Directional Light")) on_add_primitive.broadcast("light:directional");
            if (ImGui::MenuItem("Add Point Light")) on_add_primitive.broadcast("light:point");
            if (ImGui::MenuItem("Add Spot Light")) on_add_primitive.broadcast("light:spot");
            ImGui::Separator();
            if (ImGui::MenuItem("Add Volumetric Fog")) on_add_primitive.broadcast("volumetric_fog");
            if (ImGui::MenuItem("Add World Environment")) on_add_primitive.broadcast("world_environment");
            ImGui::EndPopup();
        }

        // Deferred removal
        if (remove_pending_id_ != 0) {
            on_remove_object.broadcast(remove_pending_id_);
        }
    }
    ImGui::End();
}

void HierarchyPanel::draw_node(Node& obj) {
    auto child_it = children_map_.find(static_cast<int32_t>(obj.id));
    bool has_children = child_it != children_map_.end() && !child_it->second.empty();

    ImGui::PushID(static_cast<int>(obj.id));

    bool is_selected = (obj.id == *selected_id_);

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

    // Determine visuals for this node type
    auto visuals = node_visuals(obj);

    // Row styling — equal vertical padding for centered text
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {4.0f, 6.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0.0f, 2.0f});

    if (is_selected) {
        ImGui::PushStyleColor(ImGuiCol_Header, {0.22f, 0.27f, 0.58f, 1.0f});
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, {0.26f, 0.32f, 0.65f, 1.0f});
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

    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        *selected_id_ = obj.id;
    }
    if (row_hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        *selected_id_ = obj.id;
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

    // Drag source — must follow immediately after TreeNodeEx
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
        ImGui::SetDragDropPayload("HIERARCHY_NODE", &obj.id, sizeof(obj.id));
        ImGui::Text("%s", obj.name.c_str());
        ImGui::EndDragDropSource();
    }

    // Drop target — reparent dragged object under this one
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
