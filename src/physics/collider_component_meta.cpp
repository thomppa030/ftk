#include "scene/component_registry.hpp"
#include "physics/physics_types.hpp"
#include "renderer/components/mesh_component.hpp"
#include "scene/node.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

namespace fjell {

// Channel picker plus the per-channel response table. Laid out the way people
// think about it — one row per channel, three states — while the filter stores
// it as two masks.
static bool draw_collision_filter(CollisionFilter& filter) {
    bool changed = false;

    ImGui::SeparatorText("Collision");

    ImGui::Text("Channel"); ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##Channel", collision_channel_name(filter.channel))) {
        for (uint32_t i = 0; i < COLLISION_CHANNEL_COUNT; ++i) {
            auto channel = static_cast<CollisionChannel>(i);
            bool selected = filter.channel == channel;
            if (ImGui::Selectable(collision_channel_name(channel), selected) && !selected) {
                // Switching channel takes that channel's defaults: the previous
                // table was written against a different meaning.
                filter = default_filter_for(channel);
                changed = true;
            }
        }
        ImGui::EndCombo();
    }

    if (!ImGui::TreeNodeEx("Responses", ImGuiTreeNodeFlags_DefaultOpen)) return changed;

    // One column per response, one row per channel. Exactly one box is ticked
    // per row — a radio group drawn as checkboxes, which is how a collision
    // matrix reads at a glance. Ignore gets its own box rather than being the
    // absence of the others: "nothing ticked" reads as unset, not as a choice.
    static constexpr struct { CollisionResponse value; const char* label; } COLUMNS[] = {
        {CollisionResponse::ignore, "Ignore"},
        {CollisionResponse::overlap, "Overlap"},
        {CollisionResponse::block, "Block"},
    };

    if (ImGui::BeginTable("responses", 4, ImGuiTableFlags_SizingStretchProp |
                                          ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Channel");
        for (const auto& column : COLUMNS) {
            ImGui::TableSetupColumn(column.label, ImGuiTableColumnFlags_WidthFixed,
                                    ImGui::CalcTextSize(column.label).x +
                                        (ImGui::GetFrameHeight() * 1.2F));
        }
        ImGui::TableHeadersRow();

        for (uint32_t i = 0; i < COLLISION_CHANNEL_COUNT; ++i) {
            auto channel = static_cast<CollisionChannel>(i);
            auto current = filter.response_to(channel);

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(collision_channel_name(channel));

            ImGui::PushID(static_cast<int>(i));
            for (const auto& column : COLUMNS) {
                ImGui::TableNextColumn();
                ImGui::PushID(static_cast<int>(column.value));
                bool ticked = current == column.value;
                // The header names the column, so the box itself is unlabelled.
                // Re-ticking the active box would otherwise clear the row and
                // leave it with no response at all.
                if (ImGui::Checkbox("##r", &ticked) && ticked) {
                    filter.set_response(channel, column.value);
                    changed = true;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s %s", column.label,
                                      collision_channel_name(channel));
                }
                ImGui::PopID();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::TreePop();
    return changed;
}

static const bool registered = [] {
    ComponentRegistry::instance().register_component<ColliderComponent>({
        .display_name = "Collider",
        .json_key = "collider",
        .color = {0.85f, 0.45f, 0.25f, 1.0f},

        .draw = [](Node3D& node, void* ptr, const InspectorContext& /*ctx*/) -> bool {
            auto* col = static_cast<ColliderComponent*>(ptr);
            bool changed = false;

            bool has_mesh = node.components.has<MeshComponent>();
            ImGui::BeginDisabled(!has_mesh);
            if (ImGui::Checkbox("Inherit From Mesh", &col->inherit_from_mesh)) changed = true;
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip(has_mesh
                    ? "Use the collision authored on the mesh asset"
                    : "Needs a Mesh component");
            }
            if (col->inherit_from_mesh && has_mesh) {
                ImGui::TextDisabled("Collision comes from the mesh asset.");
                if (ImGui::Checkbox("Is Trigger", &col->is_trigger)) changed = true;
                if (draw_collision_filter(col->filter)) changed = true;
                return changed;
            }

            static const char* shape_labels[] = {"Box", "Sphere", "Capsule", "Mesh"};
            int sh = static_cast<int>(col->shape);
            ImGui::Text("Shape"); ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##Shape", &sh, shape_labels, 4)) { col->shape = static_cast<ColliderShape>(sh); changed = true; }
            switch (col->shape) {
                case ColliderShape::Box:
                    ImGui::Text("Half Extents"); ImGui::SetNextItemWidth(-1);
                    if (ImGui::DragFloat3("##HalfExtents", &col->half_extents.x, 0.05f, 0.01f, 100.0f)) changed = true;
                    break;
                case ColliderShape::Sphere:
                    ImGui::Text("Radius"); ImGui::SetNextItemWidth(-1);
                    if (ImGui::DragFloat("##Radius", &col->radius, 0.05f, 0.01f, 100.0f)) changed = true;
                    break;
                case ColliderShape::Capsule:
                    ImGui::Text("Radius"); ImGui::SetNextItemWidth(-1);
                    if (ImGui::DragFloat("##RadiusCap", &col->radius, 0.05f, 0.01f, 100.0f)) changed = true;
                    ImGui::Text("Height"); ImGui::SetNextItemWidth(-1);
                    if (ImGui::DragFloat("##Height", &col->height, 0.05f, 0.01f, 100.0f)) changed = true;
                    break;
                case ColliderShape::Mesh:
                    ImGui::TextDisabled("Uses model collision mesh");
                    break;
            }
            ImGui::Text("Offset"); ImGui::SetNextItemWidth(-1);
            if (ImGui::DragFloat3("##Offset", &col->offset.x, 0.05f)) changed = true;
            if (ImGui::Checkbox("Is Trigger", &col->is_trigger)) changed = true;

            if (draw_collision_filter(col->filter)) changed = true;
            return changed;
        },

        .init = [](Node3D& node, const InspectorContext& /*ctx*/) {
            auto& col = node.components.add<ColliderComponent>();
            // A collider added to a primitive should already match it; anything
            // else gets a unit box to be dragged into shape.
            if (!fit_collider_to_primitive(col, node.source)) {
                col.shape = ColliderShape::Box;
            }
        },

        .serialize = [](const void* ptr, const Node& /*node*/, nlohmann::json& out,
                        const Project* /*proj*/) {
            auto* col = static_cast<const ColliderComponent*>(ptr);
            out["shape"] = static_cast<int>(col->shape);
            out["inherit_from_mesh"] = col->inherit_from_mesh;
            out["half_extents"] = {col->half_extents.x, col->half_extents.y, col->half_extents.z};
            out["radius"] = col->radius;
            out["height"] = col->height;
            out["offset"] = {col->offset.x, col->offset.y, col->offset.z};
            out["is_trigger"] = col->is_trigger;
            out["channel"] = collision_channel_name(col->filter.channel);
            out["block_mask"] = col->filter.block_mask;
            out["overlap_mask"] = col->filter.overlap_mask;
        },

        .deserialize = [](Node& node, const nlohmann::json& j, ResourceManager& /*res*/, const Project* /*proj*/) {
            auto& col = node.components.add<ColliderComponent>();
            col.shape = static_cast<ColliderShape>(j.value("shape", 0));
            col.inherit_from_mesh = j.value("inherit_from_mesh", false);
            if (j.contains("half_extents")) { auto& he = j["half_extents"]; col.half_extents = {he[0].get<float>(), he[1].get<float>(), he[2].get<float>()}; }
            col.radius = j.value("radius", 0.5f);
            col.height = j.value("height", 1.0f);
            if (j.contains("offset")) { auto& o = j["offset"]; col.offset = {o[0].get<float>(), o[1].get<float>(), o[2].get<float>()}; }
            col.is_trigger = j.value("is_trigger", false);

            // Stored by name, so adding or reordering channels cannot silently
            // repoint every collider in every scene.
            auto channel_name = j.value("channel", std::string{});
            col.filter.channel = CollisionChannel::world_static;
            for (uint32_t i = 0; i < COLLISION_CHANNEL_COUNT; ++i) {
                auto candidate = static_cast<CollisionChannel>(i);
                if (channel_name == collision_channel_name(candidate)) {
                    col.filter.channel = candidate;
                    break;
                }
            }
            // A scene saved before channels existed gets the defaults for its
            // channel rather than an all-zero table that collides with nothing.
            auto defaults = default_filter_for(col.filter.channel);
            col.filter.block_mask = j.value("block_mask", defaults.block_mask);
            col.filter.overlap_mask = j.value("overlap_mask", defaults.overlap_mask);
        },

        .script_type_name = "ColliderComponent",
        .script_get = [](Node& node) -> void* { return node.components.get<ColliderComponent>(); },
        .script_add = [](Node& node) -> void* { return &node.components.add<ColliderComponent>(); },
    });
    return true;
}();

} // namespace fjell
