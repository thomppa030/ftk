#include "ui/inspectors/spline_inspectors.hpp"

#include "renderer/components/instanced_mesh_component.hpp"
#include "renderer/components/spline_component.hpp"
#include "renderer/components/spline_mesh_component.hpp"
#include "scene/node.hpp"
#include "ui/asset_ref_widget.hpp"
#include "ui/inspectors/asset_rows.hpp"
#include "ui/kit/button.hpp"
#include "ui/kit/choice.hpp"
#include "ui/kit/field.hpp"
#include "ui/kit/icons.hpp"
#include "ui/kit/list_editor.hpp"
#include "ui/kit/row.hpp"
#include "ui/kit/section.hpp"
#include "ui/theme.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>

namespace fjell {

namespace {

constexpr ui::Choice<SplineInterp> MODES[] = {
    {SplineInterp::Linear, "Linear"},
    {SplineInterp::CurveAuto, "Curve (auto)"},
    {SplineInterp::CurveUser, "Curve (user)"},
    {SplineInterp::CurveBreak, "Curve (break)"},
    {SplineInterp::Constant, "Constant"},
};

constexpr ui::Choice<SplineMeshForward> FORWARDS[] = {
    {SplineMeshForward::X, "X"},
    {SplineMeshForward::Y, "Y"},
    {SplineMeshForward::Z, "Z"},
};

// A long list of points scrolls after this many cards.
constexpr int MAX_CARDS = 8;

const char* mode_name(SplineInterp mode) {
    for (const auto& [value, name] : MODES) {
        if (value == mode) return name;
    }
    return "";
}

// A button for an action, led by its icon.
bool action(const char* icon, const char* label, ui::ButtonKind kind = ui::ButtonKind::Secondary) {
    const std::string text = std::string(icon) + "  " + label;
    return ui::button(text.c_str(), kind);
}

// One line standing for a card that is not open: `first` in the text
// colour, then `rest` in the secondary colour.
void summary(const char* first, const char* rest) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(first);
    ImGui::SameLine(0.0f, theme::GAP_M);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::text_secondary());
    ImGui::TextUnformatted(rest);
    ImGui::PopStyleColor();
}

// Rebuilds what a spline derives from its points after they change.
template <typename SplineHolder>
void refresh(SplineHolder& holder) {
    holder.dirty = true;
    holder.spline.auto_set_tangents();
    holder.spline.build_reparam_table(holder.reparam_samples);
}

// The rows above the points shared by Spline and Spline Mesh: whether the
// spline closes, and how long it is.
ui::Edit spline_rows(Spline<glm::vec3>& spline) {
    ui::Edit edit;
    ui::row("Closed", "Join the last point back to the first.", [&] {
        bool closed = spline.closed;
        const ui::Edit e = ui::checkbox("##closed", closed);
        if (e.changed) spline.closed = closed;
        edit |= e;
    });
    ui::row("Length", [&] {
        char text[32];
        std::snprintf(text, sizeof(text), "%.2f m", spline.total_length());
        ui::readout(text);
    });
    return edit;
}

// A spline's points as a list: one line each, and the selected point open
// with its key, mode, position and, for a user curve, its tangents. The
// selected point is the one the viewport's gizmo moves. Points stay in key
// order, so the list is not reordered by dragging.
ui::Edit spline_points(Spline<glm::vec3>& spline, int& selected) {
    ui::Edit edit;
    auto& points = spline.points;
    std::optional<std::size_t> remove;
    std::optional<int> select;
    ui::subheading("Points");
    {
        ui::ListEditor list("##points", points.size(), {.reorder = false, .max_cards = MAX_CARDS});
        for (std::size_t i = 0; i < points.size(); ++i) {
            const bool open = selected == static_cast<int>(i);
            if (!list.begin_item(i, open)) continue;
            auto& p = points[i];
            if (open) {
                if (auto t = ui::PropertyTable("##point")) {
                    ui::row("Key", [&] { edit |= ui::drag("##key", p.input_key, {.speed = 0.05f}); });
                    ui::row("Mode", [&] { edit |= ui::choice("##mode", p.interp_mode, MODES); });
                    ui::row("Position", [&] {
                        edit |= ui::vec3("##position", p.value, {.speed = 0.05f, .unit = ui::Unit::Metres});
                    });
                    const bool user = p.interp_mode == SplineInterp::CurveUser;
                    if (user || p.interp_mode == SplineInterp::CurveBreak) {
                        ui::row("Tangent in", [&] {
                            edit |= ui::vec3("##arrive", p.arrive_tangent, {.speed = 0.05f});
                        });
                        ui::row("Tangent out", [&] {
                            edit |= ui::vec3("##leave", p.leave_tangent, {.speed = 0.05f});
                        });
                        // A user curve is symmetric: the leaving tangent
                        // follows the arriving one whichever is edited.
                        if (user) p.leave_tangent = p.arrive_tangent;
                    }
                }
            } else {
                char key[16];
                std::snprintf(key, sizeof(key), "%.2f", p.input_key);
                char rest[96];
                std::snprintf(rest, sizeof(rest), "%.1f, %.1f, %.1f   %s", p.value.x, p.value.y, p.value.z,
                              mode_name(p.interp_mode));
                summary(key, rest);
            }
            if (list.end_item()) remove = i;
            if (list.clicked()) select = open ? -1 : static_cast<int>(i);
        }
        if (points.empty()) list.empty("No points yet");
        if (list.add_button("Add point")) {
            const float key = points.empty() ? 0.0f : points.back().input_key + 1.0f;
            const glm::vec3 position = points.empty() ? glm::vec3(0.0f)
                                                      : points.back().value + glm::vec3(1.0f, 0.0f, 0.0f);
            selected = static_cast<int>(spline.add_point(key, position));
            edit = {true, true};
        }
    }
    if (remove) {
        spline.remove_point(*remove);
        if (selected == static_cast<int>(*remove)) selected = -1;
        else if (selected > static_cast<int>(*remove)) --selected;
        edit = {true, true};
    } else if (select) {
        selected = *select;
    }
    return edit;
}

// The scatter tool's settings, per node: editor state, not part of the
// component, kept by node id so an undo that swaps the component for a copy
// keeps them.
struct Scatter {
    int count{100};
    glm::vec3 area_min{-10.0f, 0.0f, -10.0f};
    glm::vec3 area_max{10.0f, 0.0f, 10.0f};
    float scale_min{0.9f};
    float scale_max{1.1f};
    float rotation_jitter{180.0f};  // degrees around Y
    bool on_ground{true};
    int seed{1337};
};

Scatter& scatter_for(uint32_t node_id) {
    static std::unordered_map<uint32_t, Scatter> settings;
    return settings[node_id];
}

// Places `s.count` instances at random in the area. More are seeded by
// how many there are already, so scattering more doesn't repeat the first
// batch.
void scatter(InstancedMeshComponent& inst, const Scatter& s, bool more) {
    if (!more) inst.instances.clear();
    std::mt19937 rng(static_cast<uint32_t>(s.seed) + static_cast<uint32_t>(inst.instances.size()));
    std::uniform_real_distribution<float> dx(s.area_min.x, s.area_max.x);
    std::uniform_real_distribution<float> dy(s.area_min.y, s.area_max.y);
    std::uniform_real_distribution<float> dz(s.area_min.z, s.area_max.z);
    std::uniform_real_distribution<float> ds(s.scale_min, s.scale_max);
    const float jitter = glm::radians(s.rotation_jitter);
    std::uniform_real_distribution<float> dr(-jitter, jitter);
    inst.instances.reserve(inst.instances.size() + static_cast<std::size_t>(s.count));
    for (int i = 0; i < s.count; ++i) {
        const glm::vec3 position{dx(rng), s.on_ground ? 0.0f : dy(rng), dz(rng)};
        const float angle = dr(rng);
        const float size = ds(rng);
        inst.instances.push_back(glm::translate(glm::mat4(1.0f), position)
                                 * glm::rotate(glm::mat4(1.0f), angle, glm::vec3(0.0f, 1.0f, 0.0f))
                                 * glm::scale(glm::mat4(1.0f), glm::vec3(size)));
    }
    inst.dirty = true;
}

// An instance's matrix as the position, turn about Y and scale the editor
// shows. Only a turn about Y is recovered, which is all the scatter makes.
struct Placement {
    glm::vec3 position;
    float yaw;
    glm::vec3 scale;

    explicit Placement(const glm::mat4& m)
        : position{m[3]}
        , scale{glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2]))} {
        const glm::vec3 x_axis = glm::vec3(m[0]) / std::max(scale.x, 1e-6f);
        yaw = glm::degrees(std::atan2(x_axis.z, x_axis.x));
    }

    [[nodiscard]] glm::mat4 matrix() const {
        return glm::translate(glm::mat4(1.0f), position)
             * glm::rotate(glm::mat4(1.0f), glm::radians(yaw), glm::vec3(0.0f, 1.0f, 0.0f))
             * glm::scale(glm::mat4(1.0f), scale);
    }
};

} // namespace

ui::Edit draw_spline(Node3D& /*node*/, void* component, const InspectorContext& /*ctx*/) {
    auto& sp = *static_cast<SplineComponent*>(component);
    ui::Edit edit;
    if (auto t = ui::PropertyTable("##spline")) {
        edit |= spline_rows(sp.spline);
    }
    edit |= spline_points(sp.spline, sp.editor_selected_point);
    if (edit.changed) refresh(sp);
    return edit;
}

ui::Edit draw_spline_mesh(Node3D& node, void* component, const InspectorContext& ctx) {
    auto& sm = *static_cast<SplineMeshComponent*>(component);
    static const std::string model_exts[] = {".fjmesh"};
    ui::Edit edit;
    if (auto t = ui::PropertyTable("##spline_mesh")) {
        ui::row("Model", "Laid along the spline, one copy per segment. Empty uses a cube.", [&] {
            // A primitive isn't an asset: shown as an empty slot.
            std::string model = node.source.starts_with("primitive:") ? std::string{} : node.source;
            if (asset_slot({.label = "Model", .id = "##model", .extensions = model_exts, .ctx = &ctx}, model)) {
                if (ctx.resource_manager != nullptr) {
                    load_spline_mesh_model(sm, model, *ctx.resource_manager, project_of(ctx));
                }
                node.source = model;
                edit = {true, true};
            }
        });
        if (material_row("##material", sm.material_name, ctx)) {
            if (ctx.resource_manager != nullptr) {
                load_spline_mesh_material(sm, *ctx.resource_manager, project_of(ctx));
            }
            edit = {true, true};
        }
        ui::row("Forward axis", "Which of the model's axes runs along the spline.",
                [&] { edit |= ui::choice("##forward", sm.forward, FORWARDS); });
        ui::row("Up", "The direction the model keeps up as the spline turns, in the node's space.", [&] {
            edit |= ui::vec3("##up", sm.up_dir, {.speed = 0.05f});
        });
        edit |= spline_rows(sm.spline);
    }
    edit |= spline_points(sm.spline, sm.editor_selected_point);
    if (edit.changed) refresh(sm);
    return edit;
}

ui::Edit draw_instanced_mesh(Node3D& node, void* component, const InspectorContext& ctx) {
    auto& inst = *static_cast<InstancedMeshComponent*>(component);
    static const std::string model_exts[] = {".fjmesh"};
    ui::Edit edit;
    if (auto t = ui::PropertyTable("##instanced_mesh")) {
        ui::row("Model", [&] {
            std::string model = node.source;
            if (asset_slot({.label = "Model", .id = "##model", .extensions = model_exts, .ctx = &ctx}, model)) {
                if (!model.empty() && ctx.resource_manager != nullptr) {
                    if (auto result = ctx.resource_manager->load_model(
                            base_path::resolve_asset(model, project_of(ctx)))) {
                        inst.model_handle = *result;
                        inst.model = ctx.resource_manager->get(inst.model_handle);
                    }
                }
                node.source = model;
                inst.dirty = true;
                edit = {true, true};
            }
        });
        if (material_row("##material", inst.material_name, ctx)) {
            load_material(inst.material_name, inst.material_handle, inst.material, ctx);
            edit = {true, true};
        }
    }

    Scatter& s = scatter_for(node.id);
    ui::subheading("Scatter");
    if (auto t = ui::PropertyTable("##scatter")) {
        ui::row("Count", [&] { (void)ui::drag_int("##count", s.count, 1.0f, 1, 100000); });
        ui::row("Area from", [&] {
            (void)ui::vec3("##area_min", s.area_min, {.speed = 0.1f, .unit = ui::Unit::Metres});
        });
        ui::row("Area to", [&] {
            (void)ui::vec3("##area_max", s.area_max, {.speed = 0.1f, .unit = ui::Unit::Metres});
        });
        ui::row("Scale from", [&] {
            (void)ui::drag("##scale_min", s.scale_min, {.speed = 0.01f, .min = 0.01f, .max = 100.0f});
        });
        ui::row("Scale to", [&] {
            (void)ui::drag("##scale_max", s.scale_max, {.speed = 0.01f, .min = 0.01f, .max = 100.0f});
        });
        ui::row("Turn", "How far each instance may be turned about its up axis, either way.", [&] {
            (void)ui::slider("##jitter", s.rotation_jitter, 0.0f, 180.0f, ui::Unit::Degrees, "%.0f");
        });
        ui::row("On the ground", "Place every instance at height 0 instead of anywhere in the area's height.",
                [&] { (void)ui::checkbox("##ground", s.on_ground); });
        ui::row("Seed", "The same seed scatters the same way.", [&] { (void)ui::drag_int("##seed", s.seed, 1.0f); });
        ui::row("", [&] {
            if (action(ui::icon::vfx, "Scatter")) {
                scatter(inst, s, false);
                edit = {true, true};
            }
            ImGui::SetItemTooltip("Replace every instance with a new scatter");
            ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
            if (action(ui::icon::add, "Scatter more")) {
                scatter(inst, s, true);
                edit = {true, true};
            }
            ImGui::SetItemTooltip("Add a scatter to the instances already there");
        });
    }

    char heading[48];
    std::snprintf(heading, sizeof(heading), "Instances · %zu", inst.instances.size());
    ui::subheading(heading);
    // Which instance is open: inspector state, one per list.
    int& open_instance = *ImGui::GetStateStorage()->GetIntRef(ImGui::GetID("##open_instance"), -1);
    std::optional<std::size_t> remove;
    std::optional<int> select;
    {
        ui::ListEditor list("##instances", inst.instances.size(), {.reorder = false, .max_cards = MAX_CARDS});
        for (std::size_t i = 0; i < inst.instances.size(); ++i) {
            const bool open = open_instance == static_cast<int>(i);
            if (!list.begin_item(i, open)) continue;
            Placement p(inst.instances[i]);
            if (open) {
                ui::Edit e;
                if (auto t = ui::PropertyTable("##instance")) {
                    ui::row("Position", [&] {
                        e |= ui::vec3("##position", p.position, {.speed = 0.05f, .unit = ui::Unit::Metres});
                    });
                    ui::row("Turn", [&] {
                        e |= ui::drag("##yaw", p.yaw, {.speed = 0.5f, .min = -360.0f, .max = 360.0f,
                                                       .unit = ui::Unit::Degrees, .format = "%.1f"});
                    });
                    ui::row("Scale", [&] {
                        e |= ui::vec3("##scale", p.scale, {.speed = 0.01f, .min = 0.001f, .max = 1000.0f});
                    });
                }
                if (e.changed) {
                    inst.instances[i] = p.matrix();
                    inst.dirty = true;
                }
                edit |= e;
            } else {
                char position[64];
                std::snprintf(position, sizeof(position), "%.1f, %.1f, %.1f", p.position.x, p.position.y,
                              p.position.z);
                char rest[48];
                std::snprintf(rest, sizeof(rest), "%.0f\xc2\xb0   %.2f\xc3\x97", p.yaw, p.scale.x);
                summary(position, rest);
            }
            if (list.end_item()) remove = i;
            if (list.clicked()) select = open ? -1 : static_cast<int>(i);
        }
        if (inst.instances.empty()) list.empty("No instances. Scatter some, or add one at the node's origin");
        if (list.add_button("Add instance")) {
            inst.instances.emplace_back(1.0f);
            inst.dirty = true;
            open_instance = static_cast<int>(inst.instances.size()) - 1;
            edit = {true, true};
        }
        if (!inst.instances.empty()) {
            ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
            if (action(ui::icon::remove, "Clear instances", ui::ButtonKind::GhostDanger)) {
                inst.instances.clear();
                inst.dirty = true;
                open_instance = -1;
                edit = {true, true};
            }
        }
    }
    if (remove) {
        inst.instances.erase(inst.instances.begin() + static_cast<std::ptrdiff_t>(*remove));
        inst.dirty = true;
        if (open_instance == static_cast<int>(*remove)) open_instance = -1;
        else if (open_instance > static_cast<int>(*remove)) --open_instance;
        edit = {true, true};
    } else if (select) {
        open_instance = *select;
    }
    return edit;
}

} // namespace fjell
