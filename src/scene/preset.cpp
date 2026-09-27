#include "scene/preset.hpp"
#include "scene/node_serializer.hpp"
#include "scene/scene.hpp"
#include "core/log.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <unordered_map>

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

Result<PresetTemplate> PresetTemplate::load(const std::string& path,
                            ResourceManager& resources,
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

    PresetTemplate preset;

    if (j.contains("preset")) {
        preset.header_.name = j["preset"].value("name", "");
    }
    preset.local_next_id_ = j.value("next_id", 1u);

    for (const auto& jobj : j["objects"]) {
        auto node = deserialize_node(jobj, resources, project);
        if (node) {
            preset.nodes_.push_back(std::move(node));
        }
    }

    FJELL_CORE_INFO("Preset loaded: {} ({} nodes)", path, preset.nodes_.size());
    return preset;
}

// ── Instantiate ──────────────────────────────────────────────────────────

uint32_t PresetTemplate::instantiate(Scene& scene, int32_t parent_id,
                             Transform offset) const {
    if (nodes_.empty()) return 0;

    // Build ID remap: preset-local ID → new scene ID
    std::unordered_map<uint32_t, uint32_t> remap;
    for (const auto& node : nodes_) {
        uint32_t new_id = scene.next_id();
        remap[node->id] = new_id;
        scene.set_next_id(new_id + 1);
    }

    uint32_t root_id = 0;

    // Clone and add nodes in order (parents before children)
    for (const auto& node : nodes_) {
        auto clone = node->clone();
        clone->id = remap[node->id];

        // Remap parent_id
        if (node->parent_id < 0) {
            // Root node of preset — parent to the caller-provided parent
            clone->parent_id = parent_id;
            if (root_id == 0) {
                root_id = clone->id;
                // Apply transform offset to root
                if (auto* spatial = clone->as_spatial()) {
                    spatial->transform.position += offset.position;
                }
            }
        } else {
            auto it = remap.find(static_cast<uint32_t>(node->parent_id));
            if (it != remap.end()) {
                clone->parent_id = static_cast<int32_t>(it->second);
            }
        }

        scene.add_node(std::move(clone));
    }

    return root_id;
}

// ── From subtree ─────────────────────────────────────────────────────────

PresetTemplate PresetTemplate::from_subtree(const Scene& scene, uint32_t root_id) {
    auto subtree = scene.collect_subtree(root_id);
    if (subtree.empty()) return {};

    PresetTemplate preset;
    preset.header_.name = subtree[0]->name;

    // Build ID remap: scene ID → preset-local ID
    std::unordered_map<uint32_t, uint32_t> remap;
    uint32_t next_local = 1;
    for (const auto* node : subtree) {
        remap[node->id] = next_local++;
    }
    preset.local_next_id_ = next_local;

    for (const auto* node : subtree) {
        auto clone = node->clone();
        clone->id = remap[node->id];

        // Remap parent_id
        if (node->id == root_id) {
            clone->parent_id = -1;
            // Zero out root transform so preset is origin-relative
            if (auto* spatial = clone->as_spatial()) {
                spatial->transform.position = glm::vec3(0.0f);
            }
        } else {
            auto it = remap.find(static_cast<uint32_t>(node->parent_id));
            if (it != remap.end()) {
                clone->parent_id = static_cast<int32_t>(it->second);
            } else {
                clone->parent_id = -1;
            }
        }

        preset.nodes_.push_back(std::move(clone));
    }

    return preset;
}

// ── From selection ───────────────────────────────────────────────────────

PresetTemplate PresetTemplate::from_selection(const Scene& scene,
                              const std::unordered_set<uint32_t>& ids) {
    if (ids.empty()) return {};

    // Find the "topmost" selected node — the one whose parent is not in the
    // selection. If multiple, just pick the first one.
    uint32_t root_id = *ids.begin();
    for (uint32_t id : ids) {
        const auto* node = scene.find(id);
        if (node && ids.find(static_cast<uint32_t>(node->parent_id)) == ids.end()) {
            root_id = id;
            break;
        }
    }

    return from_subtree(scene, root_id);
}

} // namespace fjell
