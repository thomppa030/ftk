#include "renderer/components/spline_mesh_component.hpp"
#include "ftk/base/log.hpp"
#include "renderer/indirect_batch.hpp"
#include "renderer/instance_provider.hpp"
#include "renderer/render_queue.hpp"
#include "renderer/resources/model.hpp"
#include "scene/component_registry.hpp"
#include "scene/node.hpp"
#include "scene/spline_editable.hpp"
#include "core/base_path.hpp"
#include "core/project_manager.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "renderer/resources/material.hpp"

#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>

namespace fjell {


void load_spline_mesh_model(SplineMeshComponent& sm, std::string& source,
                            ResourceManager& resources, const Project* project) {
    if (source.empty()) source = "primitive:cube";
    if (source.starts_with("primitive:")) {
        if (auto result = resources.create_primitive(source.substr(10))) {
            sm.model_handle = *result;
            sm.model = resources.get(sm.model_handle);
            sm.dirty = true;
        }
        return;
    }
    if (auto result = resources.load_model(base_path::resolve_asset(source, project))) {
        sm.model_handle = *result;
        sm.model = resources.get(sm.model_handle);
        sm.dirty = true;
    }
}

void load_spline_mesh_material(SplineMeshComponent& sm, ResourceManager& resources,
                               const Project* project) {
    if (sm.material_name.empty()) sm.material_name = "default";
    if (sm.material_name != "default") {
        if (auto result = resources.load_material_asset(
                base_path::resolve_asset(sm.material_name, project), project)) {
            sm.material_handle = *result;
            sm.material = resources.get(sm.material_handle);
        }
        return;
    }
    sm.material_handle = resources.create_material("default", resources.default_white_handle());
    sm.material = resources.get(sm.material_handle);
}

static const bool registered = [] {
    ComponentRegistry::instance().register_component<SplineMeshComponent>({
        .display_name = "Spline Mesh",
        .json_key = "spline_mesh",

        .init = [](Node3D& node, const InspectorContext& ctx) {
            auto& sm = node.components.add<SplineMeshComponent>();
            sm.spline.add_point(0.0F, glm::vec3(0.0F));
            sm.spline.add_point(1.0F, glm::vec3(2.0F, 0.0F, 0.0F));
            sm.spline.auto_set_tangents();
            sm.spline.build_reparam_table(sm.reparam_samples);

            if (ctx.resource_manager != nullptr) {
                const Project* project = ctx.project_mgr != nullptr ? ctx.project_mgr->active() : nullptr;
                std::string source = node.source;
                load_spline_mesh_model(sm, source, *ctx.resource_manager, project);
                node.source = source;
                load_spline_mesh_material(sm, *ctx.resource_manager, project);
            }
        },

        .serialize = [](const void* ptr, const Node& /*node*/, nlohmann::json& out,
                        const Project* proj) {
            auto* sm = static_cast<const SplineMeshComponent*>(ptr);
            out["material"]       = base_path::asset_reference(sm->material_name, proj);
            out["forward"]        = static_cast<int>(sm->forward);
            out["up"]             = {sm->up_dir.x, sm->up_dir.y, sm->up_dir.z};
            out["closed"]         = sm->spline.closed;
            out["reparam_samples"] = sm->reparam_samples;

            nlohmann::json arr = nlohmann::json::array();
            for (const auto& p : sm->spline.points) {
                nlohmann::json row;
                row["key"]    = p.input_key;
                row["value"]  = {p.value.x, p.value.y, p.value.z};
                row["arrive"] = {p.arrive_tangent.x, p.arrive_tangent.y, p.arrive_tangent.z};
                row["leave"]  = {p.leave_tangent.x,  p.leave_tangent.y,  p.leave_tangent.z};
                row["mode"]   = static_cast<int>(p.interp_mode);
                arr.push_back(std::move(row));
            }
            out["points"] = std::move(arr);
        },

        .deserialize = [](Node& node, const nlohmann::json& j, ResourceManager& res, const Project* proj) {
            if (!node.components.has<SplineMeshComponent>()) {
                node.components.add<SplineMeshComponent>();
            }
            auto* sm = node.components.get<SplineMeshComponent>();
            if (!sm) return;

            sm->material_name   = j.value("material", "default");
            sm->forward         = static_cast<SplineMeshForward>(j.value("forward", 0));
            sm->reparam_samples = j.value("reparam_samples", 256U);
            sm->spline.closed   = j.value("closed", false);

            if (j.contains("up") && j["up"].is_array() && j["up"].size() == 3) {
                sm->up_dir = {j["up"][0].get<float>(),
                              j["up"][1].get<float>(),
                              j["up"][2].get<float>()};
            }

            if (!node.source.empty()) {
                if (node.source.starts_with("primitive:")) {
                    auto result = res.create_primitive(node.source.substr(10));
                    if (result) {
                        sm->model_handle = *result;
                        sm->model = res.get(sm->model_handle);
                    }
                } else {
                    auto resolved = base_path::resolve_asset(node.source, proj);
                    auto result = res.load_model(resolved);
                    if (result) {
                        sm->model_handle = *result;
                        sm->model = res.get(sm->model_handle);
                    }
                }
            }

            if (sm->material_name != "default" &&
                base_path::references_asset(sm->material_name, ".fjmat")) {
                auto resolved = base_path::resolve_asset(sm->material_name, proj);
                auto result = res.load_material_asset(resolved, proj);
                if (result) {
                    sm->material_handle = *result;
                    sm->material = res.get(sm->material_handle);
                }
            } else {
                auto tex = res.default_white_handle();
                sm->material_handle = res.create_material("default", tex);
                sm->material = res.get(sm->material_handle);
            }

            sm->spline.points.clear();
            if (j.contains("points") && j["points"].is_array()) {
                for (const auto& row : j["points"]) {
                    SplinePoint<glm::vec3> p;
                    p.input_key = row.value("key", 0.0F);
                    if (row.contains("value")) {
                        const auto& v = row["value"];
                        p.value = {v[0].get<float>(), v[1].get<float>(), v[2].get<float>()};
                    }
                    if (row.contains("arrive")) {
                        const auto& v = row["arrive"];
                        p.arrive_tangent = {v[0].get<float>(), v[1].get<float>(), v[2].get<float>()};
                    }
                    if (row.contains("leave")) {
                        const auto& v = row["leave"];
                        p.leave_tangent = {v[0].get<float>(), v[1].get<float>(), v[2].get<float>()};
                    }
                    p.interp_mode = static_cast<SplineInterp>(row.value("mode", 1));
                    sm->spline.points.push_back(p);
                }
            }
            sm->spline.auto_set_tangents();
            sm->spline.build_reparam_table(sm->reparam_samples);
            sm->dirty = true;
        },

        .script_type_name = {},
    });

    register_spline_editable<SplineMeshComponent>(
        [](SplineMeshComponent& c) -> Spline<glm::vec3>* { return &c.spline; },
        [](SplineMeshComponent& c) -> int* { return &c.editor_selected_point; },
        [](SplineMeshComponent& c) {
            c.spline.auto_set_tangents();
            c.spline.build_reparam_table(c.reparam_samples);
            c.dirty = true;
        });

    // Rebuild cached segments + instance transforms + local bounds when dirty.
    // Runs under the scene gatherer probe once per frame. Segments live in
    // node-local space; the node's world matrix (on DrawCommand) transforms
    // them into world space through dd.model in the vertex shader.
    auto rebuild_spline_mesh = [](SplineMeshComponent& sm) {
        const auto& pts = sm.spline.points;
        const size_t n = pts.size();
        const bool   closed = sm.spline.closed && n >= 2;
        const size_t seg_count = n < 2 ? 0 : (n - 1 + (closed ? 1 : 0));

        sm.segments.clear();
        sm.instance_transforms.clear();
        if (seg_count == 0 || !sm.model) {
            sm.local_bounds = {};
            sm.dirty = false;
            return;
        }

        const AABB&  local = sm.model->local_aabb();
        const uint32_t fwd = static_cast<uint32_t>(sm.forward);
        // Half-span of the source mesh along the forward axis in object space.
        float half_span = 0.5F;
        switch (sm.forward) {
            case SplineMeshForward::X: half_span = 0.5F * (local.max.x - local.min.x); break;
            case SplineMeshForward::Y: half_span = 0.5F * (local.max.y - local.min.y); break;
            case SplineMeshForward::Z: half_span = 0.5F * (local.max.z - local.min.z); break;
        }
        if (half_span < 1e-4F) half_span = 1e-4F;

        sm.segments.reserve(seg_count);
        sm.instance_transforms.assign(seg_count, glm::mat4(1.0F));

        AABB bounds{};
        bool first = true;
        auto expand = [&](const glm::vec3& p) {
            if (first) { bounds.min = bounds.max = p; first = false; }
            else       { bounds.min = glm::min(bounds.min, p); bounds.max = glm::max(bounds.max, p); }
        };

        // Parallel-transport a (right, up, forward) basis along the curve.
        // Each segment's end frame is the rotation-minimizing continuation of
        // its start frame; each segment's start frame equals the previous
        // segment's end frame. The result is continuous across segment
        // boundaries — no flips at sharp bends or inflection points.
        auto basis_to_quat = [](const glm::vec3& r, const glm::vec3& u,
                                const glm::vec3& f) -> glm::vec4 {
            glm::mat3 m(r, u, f);
            glm::quat q = glm::quat_cast(m);
            return {q.x, q.y, q.z, q.w};
        };

        auto rotate_min = [](const glm::vec3& from, const glm::vec3& to) -> glm::mat3 {
            const glm::vec3 f_n = glm::normalize(from);
            const glm::vec3 t_n = glm::normalize(to);
            const float     d   = glm::dot(f_n, t_n);
            if (d > 0.9999F) return glm::mat3(1.0F);
            if (d < -0.9999F) {
                // 180° rotation about any axis perpendicular to `from`.
                glm::vec3 axis = glm::abs(f_n.x) < 0.9F
                    ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
                axis = glm::normalize(glm::cross(f_n, axis));
                return glm::mat3(glm::angleAxis(glm::pi<float>(), axis));
            }
            const glm::vec3 axis = glm::normalize(glm::cross(f_n, t_n));
            const float     ang  = std::acos(d);
            return glm::mat3(glm::angleAxis(ang, axis));
        };

        // Pick an initial up by projecting up_dir perpendicular to the first
        // tangent. Falls back to a world axis when they're parallel.
        auto make_initial_frame = [&](const glm::vec3& tangent)
            -> std::tuple<glm::vec3, glm::vec3, glm::vec3> {
            const glm::vec3 f = glm::normalize(tangent);
            glm::vec3 up_ref = sm.up_dir;
            if (std::abs(glm::dot(glm::normalize(up_ref), f)) > 0.999F) {
                up_ref = std::abs(f.x) < 0.9F ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
            }
            const glm::vec3 r = glm::normalize(glm::cross(up_ref, f));
            const glm::vec3 u = glm::cross(f, r);
            return {r, u, f};
        };

        // Seed with the first segment's starting tangent.
        const SplinePoint<glm::vec3>& p0 = pts[0];
        const SplinePoint<glm::vec3>& p1 = pts[1];
        const float dx0 = p1.input_key - p0.input_key;
        const glm::vec3 t0 = p0.leave_tangent * dx0;
        auto [r_cur, u_cur, f_cur] = make_initial_frame(t0);

        for (size_t i = 0; i < seg_count; ++i) {
            const SplinePoint<glm::vec3>& a = pts[i];
            const SplinePoint<glm::vec3>& b = pts[(i + 1) % n];
            const float dx = (i == n - 1 && closed)
                ? sm.spline.closing_span()
                : (b.input_key - a.input_key);
            const glm::vec3 start_t = a.leave_tangent  * dx;
            const glm::vec3 end_t   = b.arrive_tangent * dx;

            // Approximate arc length with 8 chord samples. Used to drive UV
            // re-parameterization in the shader so textures tile by world
            // distance rather than stretching with the deformed geometry.
            constexpr int kArcSamples = 8;
            float arc_len = 0.0F;
            auto hermite = [&](float t) {
                const float t2 = t * t;
                const float t3 = t2 * t;
                const float h00 =  2.0F * t3 - 3.0F * t2 + 1.0F;
                const float h10 =         t3 - 2.0F * t2 + t;
                const float h01 = -2.0F * t3 + 3.0F * t2;
                const float h11 =         t3 -        t2;
                return h00 * a.value + h10 * start_t
                     + h01 * b.value + h11 * end_t;
            };
            glm::vec3 prev_p = a.value;
            for (int s = 1; s <= kArcSamples; ++s) {
                const float t_s = static_cast<float>(s) / static_cast<float>(kArcSamples);
                const glm::vec3 p_s = hermite(t_s);
                arc_len += glm::length(p_s - prev_p);
                prev_p = p_s;
            }

            // Align the carried frame to the segment's actual start tangent
            // (handles CurveBreak corners where leave != arrive).
            glm::mat3 rot_start = rotate_min(f_cur, start_t);
            glm::vec3 r_s = rot_start * r_cur;
            glm::vec3 u_s = rot_start * u_cur;
            glm::vec3 f_s = glm::normalize(start_t);

            // Parallel-transport to the end tangent.
            glm::mat3 rot_end = rotate_min(f_s, end_t);
            glm::vec3 r_e = rot_end * r_s;
            glm::vec3 u_e = rot_end * u_s;
            glm::vec3 f_e = glm::normalize(end_t);

            GpuSplineSegment seg{};
            seg.start_pos  = a.value;
            seg.end_pos    = b.value;
            seg.start_tan  = start_t;
            seg.end_tan    = end_t;
            seg.start_quat = basis_to_quat(r_s, u_s, f_s);
            seg.end_quat   = basis_to_quat(r_e, u_e, f_e);
            seg.forward    = fwd;
            seg.half_span  = half_span;
            seg.arc_length = arc_len;
            sm.segments.push_back(seg);

            expand(a.value);
            expand(b.value);

            // Carry the end frame forward into the next segment.
            r_cur = r_e;
            u_cur = u_e;
            f_cur = f_e;
        }

        // Inflate by the cross-section extent so the deformed mesh stays
        // inside the AABB even where it bulges widest.
        glm::vec3 cross_extent{0.0F};
        if (sm.forward != SplineMeshForward::X) cross_extent.x = 0.5F * (local.max.x - local.min.x);
        if (sm.forward != SplineMeshForward::Y) cross_extent.y = 0.5F * (local.max.y - local.min.y);
        if (sm.forward != SplineMeshForward::Z) cross_extent.z = 0.5F * (local.max.z - local.min.z);
        const float cross_radius = glm::length(cross_extent);
        bounds.min -= glm::vec3(cross_radius);
        bounds.max += glm::vec3(cross_radius);

        sm.local_bounds = bounds;
        sm.dirty = false;
    };

    InstanceProviderRegistry::instance().add(
        component_type_id<SplineMeshComponent>(),
        [rebuild_spline_mesh](Node& node, const Node3D& spatial) -> std::optional<DrawCommand> {
            auto* sm = node.components.get<SplineMeshComponent>();
            if (!sm || !sm->model) return std::nullopt;
            if (sm->dirty) rebuild_spline_mesh(*sm);
            if (sm->segments.empty()) return std::nullopt;

            DrawCommand dc{};
            dc.world_matrix      = spatial.world_matrix_;
            dc.prev_world_matrix = spatial.prev_world_matrix_;
            dc.push.model        = spatial.world_matrix_;
            dc.model             = sm->model;
            dc.node_id           = node.id;
            dc.instance_transforms = &sm->instance_transforms;
            dc.spline_segments     = &sm->segments;

            if (sm->material) {
                dc.push.material_id = sm->material->material_id();
                dc.pipeline_id      = sm->material->pipeline_id();
                dc.sort_key = RenderQueue::make_sort_key(
                    dc.pipeline_id, sm->material->material_id(), node.id);
                dc.transparent = sm->material->force_transparent()
                              || (sm->material->params().base_color_factor.w < 1.0F);
            }

            dc.world_aabb = sm->local_bounds.transform(spatial.world_matrix_);
            return dc;
        });

    return true;
}();

} // namespace fjell
