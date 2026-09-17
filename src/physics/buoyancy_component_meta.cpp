#include "scene/component_registry.hpp"
#include "physics/buoyancy.hpp"
#include "physics/physics_types.hpp"
#include "scene/node.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>

namespace fjell {

static const bool registered = [] {
    ComponentRegistry::instance().register_component<BuoyancyComponent>({
        .display_name = "Buoyancy",
        .json_key = "buoyancy",
        .color = {0.25f, 0.6f, 0.85f, 1.0f},

        .draw = [](Node3D& node, void* ptr, const InspectorContext& /*ctx*/) -> bool {
            auto* b = static_cast<BuoyancyComponent*>(ptr);
            bool changed = false;

            // Buoyancy is a force on a body, so without one there is nothing
            // to push. Said here rather than left to be discovered by a crate
            // that refuses to float.
            const auto* rb = node.components.get<RigidbodyComponent>();
            if (rb == nullptr) {
                ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.25f, 1.0f),
                                   "Needs a Rigidbody to float.");
            } else if (rb->body_type != BodyType::Dynamic) {
                ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.25f, 1.0f),
                                   "Only a Dynamic rigidbody floats.");
            }
            if (node.components.get<ColliderComponent>() == nullptr) {
                ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.25f, 1.0f),
                                   "Needs a Collider for its size.");
            }

            ImGui::Text("Density"); ImGui::SetNextItemWidth(-1);
            if (ImGui::DragFloat("##Density", &b->density, 0.01f, 0.0f, 5.0f, "%.2f")) {
                changed = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("How heavy this is for its size, against water. "
                                  "Below 1 floats, 1 hangs, above sinks. Oak is "
                                  "about 0.75, a sealed drum nearer 0.2.");
            }

            // The rigidbody's Mass field is driven by this while buoyancy is
            // on, so show what it comes to. Two numbers for one property is
            // what made a cube at density 0.1 sink: the dial said a tenth of
            // water and the solver used the authored mass of 1 kg.
            if (const auto* col = node.components.get<ColliderComponent>()) {
                const BuoyancySamples s = buoyancy_samples(*b, col);
                if (s.volume > 0.0f) {
                    ImGui::TextDisabled("%.3g m\xc2\xb3 at this density weighs %.3g kg",
                                        static_cast<double>(s.volume),
                                        static_cast<double>(s.volume * b->density
                                                            * WATER_DENSITY));
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("The Rigidbody's Mass is driven from this "
                                          "while Buoyancy is on, so the two cannot "
                                          "disagree.");
                    }
                    ImGui::TextDisabled("Floats with %.0f%% submerged",
                                        static_cast<double>(
                                            std::min(b->density, 1.0f) * 100.0f));
                }
            }

            ImGui::Text("Drag"); ImGui::SetNextItemWidth(-1);
            if (ImGui::DragFloat("##Drag", &b->drag, 0.05f, 0.0f, 20.0f, "%.2f")) {
                changed = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("How hard the water resists it moving. This is "
                                  "what settles a float instead of letting it "
                                  "bob forever.");
            }

            ImGui::Text("Angular drag"); ImGui::SetNextItemWidth(-1);
            if (ImGui::DragFloat("##AngularDrag", &b->angular_drag, 0.05f, 0.0f, 20.0f,
                                 "%.2f")) {
                changed = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("How hard the water resists it turning. Separate "
                                  "from Drag so a hull can glide forward and still "
                                  "resist being spun.");
            }

            if (ImGui::Checkbox("Follow the current", &b->follow_current)) changed = true;
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Carried along by the water it sits in, so a raft "
                                  "drifts with a swell instead of staying put "
                                  "under it.");
            }

            ImGui::Separator();
            if (b->points.empty()) {
                ImGui::TextDisabled("Floating on the collider's shape");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("A box floats on its corners, a round shape on "
                                      "its axes. Add points only for a hull whose "
                                      "float points are not its shape.");
                }
                if (ImGui::SmallButton("Author points")) {
                    b->points.push_back(glm::vec3(0.0f));
                    changed = true;
                }
            } else {
                ImGui::TextDisabled("Floating on %d authored point%s",
                                    static_cast<int>(b->points.size()),
                                    b->points.size() == 1 ? "" : "s");
                for (std::size_t i = 0; i < b->points.size(); ++i) {
                    ImGui::PushID(static_cast<int>(i));
                    ImGui::SetNextItemWidth(-60.0f);
                    if (ImGui::DragFloat3("##Point", &b->points[i].x, 0.05f)) changed = true;
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Remove")) {
                        b->points.erase(b->points.begin() + static_cast<long>(i));
                        ImGui::PopID();
                        changed = true;
                        break;
                    }
                    ImGui::PopID();
                }
                if (ImGui::SmallButton("Add point")) {
                    b->points.push_back(glm::vec3(0.0f));
                    changed = true;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Back to the collider")) {
                    b->points.clear();
                    changed = true;
                }
            }
            return changed;
        },

        .init = [](Node3D& node, const InspectorContext& /*ctx*/) {
            node.components.add<BuoyancyComponent>();
        },

        .serialize = [](const void* ptr, const Node& /*node*/, nlohmann::json& out,
                        const Project* /*proj*/) {
            auto* b = static_cast<const BuoyancyComponent*>(ptr);
            out["density"] = b->density;
            out["drag"] = b->drag;
            out["angular_drag"] = b->angular_drag;
            out["follow_current"] = b->follow_current;
            // Only written when authored, so a body floating on its collider
            // does not carry an empty list in every scene file.
            if (!b->points.empty()) {
                auto& points = out["points"] = nlohmann::json::array();
                for (const auto& p : b->points) points.push_back({p.x, p.y, p.z});
            }
        },

        .deserialize = [](Node& node, const nlohmann::json& j, ResourceManager& /*res*/,
                          const Project* /*proj*/) {
            auto& b = node.components.add<BuoyancyComponent>();
            b.density = j.value("density", 0.6f);
            b.drag = j.value("drag", 1.2f);
            b.angular_drag = j.value("angular_drag", 0.8f);
            b.follow_current = j.value("follow_current", true);
            if (j.contains("points") && j["points"].is_array()) {
                for (const auto& p : j["points"]) {
                    if (p.is_array() && p.size() == 3) {
                        b.points.emplace_back(p[0].get<float>(), p[1].get<float>(),
                                              p[2].get<float>());
                    }
                }
            }
        },

        .script_type_name = "BuoyancyComponent",
        .script_get = [](Node& node) -> void* { return node.components.get<BuoyancyComponent>(); },
        .script_add = [](Node& node) -> void* { return &node.components.add<BuoyancyComponent>(); },
    });
    return true;
}();

} // namespace fjell
