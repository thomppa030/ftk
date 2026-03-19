#include "ui/hierarchy_panel.hpp"
#include "core/command_history.hpp"
#include "core/commands.hpp"
#include "scene/scene.hpp"
#include "ui/viewport_manager.hpp"
#include "renderer/camera.hpp"
#include "renderer/model.hpp"

#include <imgui.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <cmath>

namespace fjell {

void HierarchyPanel::init(Scene* scene, ViewportManager* viewports,
                          uint32_t* selected_id, CommandHistory* history) {
    scene_ = scene;
    viewports_ = viewports;
    selected_id_ = selected_id;
    history_ = history;
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
            if (ImGui::MenuItem("Cube") && add_primitive_cb_) add_primitive_cb_("cube");
            if (ImGui::MenuItem("Plane") && add_primitive_cb_) add_primitive_cb_("plane");
            if (ImGui::MenuItem("Sphere") && add_primitive_cb_) add_primitive_cb_("sphere");
            ImGui::Separator();
            if (ImGui::MenuItem("Sprite") && add_primitive_cb_) add_primitive_cb_("sprite");
            ImGui::EndPopup();
        }

        ImGui::Separator();

        // Draw root objects
        for (auto& obj : scene_->objects()) {
            if (obj.parent_id == -1) {
                draw_node(obj);
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
            if (ImGui::MenuItem("Add Cube") && add_primitive_cb_) add_primitive_cb_("cube");
            if (ImGui::MenuItem("Add Plane") && add_primitive_cb_) add_primitive_cb_("plane");
            if (ImGui::MenuItem("Add Sphere") && add_primitive_cb_) add_primitive_cb_("sphere");
            ImGui::Separator();
            if (ImGui::MenuItem("Add Sprite") && add_primitive_cb_) add_primitive_cb_("sprite");
            ImGui::EndPopup();
        }

        // Deferred removal
        if (remove_pending_id_ != 0 && remove_object_cb_) {
            remove_object_cb_(remove_pending_id_);
        }
    }
    ImGui::End();
}

void HierarchyPanel::draw_node(SceneNode& obj) {
    auto& objects = scene_->objects();

    bool has_children = false;
    for (const auto& other : objects) {
        if (other.parent_id == static_cast<int32_t>(obj.id)) {
            has_children = true;
            break;
        }
    }

    ImGui::PushID(static_cast<int>(obj.id));

    bool is_selected = (obj.id == *selected_id_);

    ImGuiTreeNodeFlags flags =
        ImGuiTreeNodeFlags_OpenOnArrow |
        ImGuiTreeNodeFlags_SpanAvailWidth |
        ImGuiTreeNodeFlags_DefaultOpen |
        ImGuiTreeNodeFlags_FramePadding;

    if (!has_children) {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    }
    if (is_selected) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }

    // Row styling — equal vertical padding for centered text
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {4.0f, 4.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0.0f, 1.0f});

    if (is_selected) {
        ImGui::PushStyleColor(ImGuiCol_Header, {0.22f, 0.27f, 0.58f, 1.0f});
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, {0.26f, 0.32f, 0.65f, 1.0f});
    }

    bool node_open = ImGui::TreeNodeEx(obj.name.c_str(), flags);

    if (is_selected) {
        ImGui::PopStyleColor(2);
    }

    ImGui::PopStyleVar(2);

    bool row_hovered = ImGui::IsItemHovered();

    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        *selected_id_ = obj.id;
    }
    if (row_hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        *selected_id_ = obj.id;
        glm::vec3 center = glm::vec3(obj.world_matrix_[3]);
        float radius = 1.0f;
        if (obj.mesh && obj.mesh->model && !obj.mesh->model->collision_vertices().empty()) {
            float max_r2 = 0.0f;
            for (const auto& v : obj.mesh->model->collision_vertices()) {
                auto wv = glm::vec3(obj.world_matrix_ * glm::vec4(v, 1.0f));
                max_r2 = std::max(max_r2, glm::dot(wv - center, wv - center));
            }
            radius = std::max(std::sqrt(max_r2), 0.1f);
        }
        if (auto* cam = viewports_->active_camera()) {
            cam->focus(center, radius);
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

    // Delete button — always present to avoid layout shift, hidden when not relevant
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 20);
    if (row_hovered || is_selected) {
        ImGui::PushStyleColor(ImGuiCol_Button, {0.0f, 0.0f, 0.0f, 0.0f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.8f, 0.2f, 0.2f, 0.6f});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, {0.9f, 0.1f, 0.1f, 0.8f});
        ImGui::PushStyleColor(ImGuiCol_Text, {0.7f, 0.7f, 0.7f, 1.0f});
        if (ImGui::SmallButton("X")) {
            remove_pending_id_ = obj.id;
        }
        ImGui::PopStyleColor(4);
    } else {
        ImGui::Dummy({20.0f, 0.0f});
    }

    // Recurse children
    if (has_children && node_open) {
        for (auto& child : objects) {
            if (child.parent_id == static_cast<int32_t>(obj.id)) {
                draw_node(child);
            }
        }
        ImGui::TreePop();
    }

    ImGui::PopID();
}

} // namespace fjell
