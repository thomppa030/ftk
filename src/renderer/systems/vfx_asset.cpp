#include "renderer/systems/vfx_asset.hpp"
#include "renderer/systems/vfx_emitter.hpp"
#include "renderer/systems/vfx_expression.hpp"
#include "renderer/systems/vfx_module.hpp"
#include "renderer/resources/material.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <stdexcept>
#include <unordered_map>

namespace fjell {

// ── VfxEffect v2 load/save ────────────────────────────────────────────

static VfxEmitter load_emitter_v2(const nlohmann::json& je) {
    VfxEmitter emitter;
    emitter.name = je.value("name", "emitter");
    emitter.max_particles = je.value("max_particles", 10000u);
    emitter.blend_mode = static_cast<ParticleBlendMode>(je.value("blend_mode", 0));

    auto load_modules = [](const nlohmann::json& arr, std::vector<std::unique_ptr<VfxModule>>& out) {
        for (const auto& jm : arr) {
            auto type_name = jm.value("type", "");
            auto mod = create_module(type_name);
            if (mod) {
                mod->from_json(jm);
                out.push_back(std::move(mod));
            }
        }
    };

    if (je.contains("spawn")) load_modules(je["spawn"], emitter.spawn_modules);
    if (je.contains("update")) load_modules(je["update"], emitter.update_modules);
    if (je.contains("render")) load_modules(je["render"], emitter.render_modules);

    // Expression graph (optional)
    if (je.contains("expressions")) {
        auto& jex = je["expressions"];
        auto& graph = emitter.expression_graph;
        if (jex.contains("nodes")) {
            for (const auto& jn : jex["nodes"]) {
                auto type_str = jn.value("type", "");
                // Map type name to enum
                static const std::unordered_map<std::string, ExprNodeType> type_map = {
                    {"constant_float", ExprNodeType::constant_float},
                    {"constant_vec3",  ExprNodeType::constant_vec3},
                    {"constant_vec4",  ExprNodeType::constant_vec4},
                    {"time",           ExprNodeType::time},
                    {"delta_time",     ExprNodeType::delta_time},
                    {"random",         ExprNodeType::random},
                    {"add",            ExprNodeType::add},
                    {"subtract",       ExprNodeType::subtract},
                    {"multiply",       ExprNodeType::multiply},
                    {"divide",         ExprNodeType::divide},
                    {"negate",         ExprNodeType::negate},
                    {"abs",            ExprNodeType::abs_node},
                    {"sin",            ExprNodeType::sin_node},
                    {"cos",            ExprNodeType::cos_node},
                    {"lerp",           ExprNodeType::lerp},
                    {"remap",          ExprNodeType::remap},
                    {"clamp",          ExprNodeType::clamp},
                    {"split_vec3",     ExprNodeType::split_vec3},
                    {"combine_vec3",   ExprNodeType::combine_vec3},
                    {"split_vec4",     ExprNodeType::split_vec4},
                    {"combine_vec4",   ExprNodeType::combine_vec4},
                };
                auto it = type_map.find(type_str);
                if (it == type_map.end()) continue;

                auto node = std::make_unique<ExpressionNode>();
                node->type = it->second;
                node->node_id = jn.value("id", graph.next_node_id++);

                // Load constant value
                if (jn.contains("value")) {
                    auto& jv = jn["value"];
                    if (jv.is_number()) {
                        node->constant_value = jv.get<float>();
                    } else if (jv.is_array() && jv.size() == 3) {
                        node->constant_value = glm::vec3(jv[0], jv[1], jv[2]);
                    } else if (jv.is_array() && jv.size() == 4) {
                        node->constant_value = glm::vec4(jv[0], jv[1], jv[2], jv[3]);
                    }
                }

                // Track max ID for future allocations
                if (node->node_id >= graph.next_node_id) {
                    graph.next_node_id = node->node_id + 1;
                }
                graph.nodes.push_back(std::move(node));
            }
        }
        if (jex.contains("links")) {
            for (const auto& jl : jex["links"]) {
                ExpressionLink link;
                link.link_id = jl.value("id", graph.next_link_id++);
                link.source_pin_id = jl.value("source", uint64_t(0));
                link.target_pin_id = jl.value("target", uint64_t(0));
                if (link.link_id >= graph.next_link_id) {
                    graph.next_link_id = link.link_id + 1;
                }
                graph.links.push_back(link);
            }
        }
    }

    return emitter;
}

static nlohmann::json save_emitter_v2(const VfxEmitter& emitter) {
    nlohmann::json je;
    je["name"] = emitter.name;
    je["max_particles"] = emitter.max_particles;
    je["blend_mode"] = static_cast<int>(emitter.blend_mode);

    auto save_modules = [](const std::vector<std::unique_ptr<VfxModule>>& mods) {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& m : mods) {
            if (!m->enabled) continue;
            nlohmann::json jm;
            m->to_json(jm);
            arr.push_back(jm);
        }
        return arr;
    };

    je["spawn"] = save_modules(emitter.spawn_modules);
    je["update"] = save_modules(emitter.update_modules);
    je["render"] = save_modules(emitter.render_modules);

    // Expression graph
    if (!emitter.expression_graph.empty()) {
        static const char* expr_type_names[] = {
            "constant_float", "constant_vec3", "constant_vec4",
            "time", "delta_time", "random",
            "add", "subtract", "multiply", "divide",
            "negate", "abs", "sin", "cos",
            "lerp", "remap", "clamp",
            "split_vec3", "combine_vec3", "split_vec4", "combine_vec4",
        };

        nlohmann::json jex;
        nlohmann::json nodes_arr = nlohmann::json::array();
        for (const auto& node : emitter.expression_graph.nodes) {
            nlohmann::json jn;
            jn["id"] = node->node_id;
            jn["type"] = expr_type_names[static_cast<int>(node->type)];

            // Save constant value
            if (node->type == ExprNodeType::constant_float) {
                jn["value"] = std::get<float>(node->constant_value);
            } else if (node->type == ExprNodeType::constant_vec3) {
                auto v = std::get<glm::vec3>(node->constant_value);
                jn["value"] = {v.x, v.y, v.z};
            } else if (node->type == ExprNodeType::constant_vec4) {
                auto v = std::get<glm::vec4>(node->constant_value);
                jn["value"] = {v.x, v.y, v.z, v.w};
            }
            nodes_arr.push_back(jn);
        }
        jex["nodes"] = nodes_arr;

        nlohmann::json links_arr = nlohmann::json::array();
        for (const auto& link : emitter.expression_graph.links) {
            nlohmann::json jl;
            jl["id"] = link.link_id;
            jl["source"] = link.source_pin_id;
            jl["target"] = link.target_pin_id;
            links_arr.push_back(jl);
        }
        jex["links"] = links_arr;

        je["expressions"] = jex;
    }

    return je;
}

VfxEffect load_vfx_effect_from_json(const nlohmann::json& j) {
    VfxEffect effect;
    effect.name = j.value("name", "effect");
    if (j.contains("emitters")) {
        for (const auto& je : j["emitters"]) {
            effect.emitters.push_back(load_emitter_v2(je));
        }
    }
    return effect;
}

VfxEffect load_vfx_effect(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        throw std::runtime_error("Failed to open .fjvfx file: " + path);
    }

    nlohmann::json j;
    f >> j;
    return load_vfx_effect_from_json(j);
}

void save_vfx_effect(const std::string& path, const VfxEffect& effect) {
    nlohmann::json j;
    j["version"] = 2;
    j["name"] = effect.name;

    nlohmann::json emitters_arr = nlohmann::json::array();
    for (const auto& emitter : effect.emitters) {
        emitters_arr.push_back(save_emitter_v2(emitter));
    }
    j["emitters"] = emitters_arr;

    std::ofstream f(path);
    if (!f.is_open()) {
        throw std::runtime_error("Failed to write .fjvfx file: " + path);
    }
    f << j.dump(4) << "\n";
}

void resolve_vfx_textures(VfxEffect& effect, ResourceManager& resources,
                          const Project* project) {
    for (auto& emitter : effect.emitters) {
        for (auto& mod : emitter.spawn_modules) {
            if (mod->type() != ModuleType::initial_texture) continue;
            auto* tex_mod = static_cast<InitialTextureModule*>(mod.get());
            if (tex_mod->texture_path.empty() || tex_mod->bindless_index != 0) continue;

            auto resolved = base_path::resolve_asset(tex_mod->texture_path, project);
            auto result = resources.load_texture(resolved);
            if (result) {
                tex_mod->bindless_index = resources.bindless_index(*result);
            }
        }

        // Resolve mesh particle materials to bindless material IDs
        for (auto& mod : emitter.render_modules) {
            if (mod->type() != ModuleType::mesh_particle) continue;
            auto* mm = static_cast<MeshParticleModule*>(mod.get());
            if (mm->material_path.empty() || mm->resolved_material_id != 0) continue;

            auto resolved = base_path::resolve_asset(mm->material_path, project);
            auto mat_result = resources.load_material_asset(resolved, project);
            if (mat_result) {
                auto* mat = resources.get(*mat_result);
                if (mat) mm->resolved_material_id = mat->material_id();
            }
        }
    }
}

} // namespace fjell
