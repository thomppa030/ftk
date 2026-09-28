// A preset's `.fjp` file: its nodes as the scene file writes them.
#include "scene/preset.hpp"
#include "scene/node_serializer.hpp"
#include "core/log.hpp"

#include <nlohmann/json.hpp>

#include <fstream>

namespace fjell {

// ── Save ─────────────────────────────────────────────────────────────────

Result<> PresetTemplate::save(const std::string& path, const Project* project) const {
    nlohmann::json j;
    j["preset"] = {{"name", header_.name}};
    j["next_id"] = local_next_id_;
    j["objects"] = nlohmann::json::array();

    for (const auto& node : nodes_) {
        j["objects"].push_back(serialize_node(*node, project));
    }

    std::ofstream file(path);
    if (!file.is_open()) {
        return make_error("Failed to save preset: " + path);
    }
    file << j.dump(2);
    FJELL_CORE_INFO("Preset saved: {} ({} nodes)", path, nodes_.size());
    return {};
}

// ── Load ─────────────────────────────────────────────────────────────────

Result<PresetTemplate> PresetTemplate::load(const std::string& path, ResourceManager& resources,
                                            const Project* project) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return make_error("Preset file not found: " + path);
    }

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(file);
    } catch (const nlohmann::json::parse_error& e) {
        return make_error(std::string("Failed to parse preset: ") + e.what());
    }

    std::string name;
    if (j.contains("preset")) {
        name = j["preset"].value("name", "");
    }

    std::vector<std::unique_ptr<Node>> nodes;
    for (const auto& jobj : j["objects"]) {
        auto node = deserialize_node(jobj, resources, project);
        if (node) {
            nodes.push_back(std::move(node));
        }
    }

    FJELL_CORE_INFO("Preset loaded: {} ({} nodes)", path, nodes.size());
    return from_nodes(std::move(name), std::move(nodes), path);
}

} // namespace fjell
