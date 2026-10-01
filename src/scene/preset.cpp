#include "scene/preset.hpp"
#include "scene/scene.hpp"
#include "ftk/base/log.hpp"

#include <algorithm>
#include <unordered_map>

namespace fjell {

namespace {

/// "DoorController `lever` on 'Door'", for a report.
std::string describe(const NodeReferenceSite& site, const Node& holder) {
    return std::string(site.holder) + " `" + std::string(site.field) + "` on '" + holder.name + "'";
}

} // namespace

// ── From nodes ───────────────────────────────────────────────────────────

PresetTemplate PresetTemplate::from_nodes(std::string name, std::vector<std::unique_ptr<Node>> nodes,
                                          std::string_view source) {
    PresetTemplate preset;
    preset.header_.name = std::move(name);
    preset.nodes_ = std::move(nodes);

    std::unordered_map<uint32_t, const Node*> own;
    for (const auto& node : preset.nodes_) {
        own[node->id] = node.get();
        preset.local_next_id_ = std::max(preset.local_next_id_, node->id + 1);
    }
    for (const auto& node : preset.nodes_) {
        node->each_node_reference([&](const NodeReferenceSite& site, uint32_t& id) {
            if (own.contains(id)) return;
            FJELL_CORE_WARN("{}: {} refers to node {}, which is not in the preset; the reference is dropped",
                            source, describe(site, *node), id);
            id = 0;
        });
    }
    return preset;
}

// ── Instantiate ──────────────────────────────────────────────────────────

uint32_t PresetTemplate::instantiate(Scene& scene, int32_t parent_id, Transform offset) const {
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

        // Before it joins the scene, whose scripts read their references as
        // they are made. A preset holds none to anything outside it.
        clone->each_node_reference([&](const NodeReferenceSite&, uint32_t& id) {
            const auto it = remap.find(id);
            id = it != remap.end() ? it->second : 0;
        });

        scene.add_node(std::move(clone));
    }

    return root_id;
}

// ── From subtree ─────────────────────────────────────────────────────────

Result<PresetTemplate> PresetTemplate::from_subtree(const Scene& scene, uint32_t root_id) {
    auto subtree = scene.collect_subtree(root_id);
    if (subtree.empty()) return PresetTemplate{};

    PresetTemplate preset;
    preset.header_.name = subtree[0]->name;

    // Build ID remap: scene ID → preset-local ID
    std::unordered_map<uint32_t, uint32_t> remap;
    uint32_t next_local = 1;
    for (const auto* node : subtree) {
        remap[node->id] = next_local++;
    }
    preset.local_next_id_ = next_local;

    std::vector<std::string> outside;
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

        clone->each_node_reference([&](const NodeReferenceSite& site, uint32_t& id) {
            if (const auto it = remap.find(id); it != remap.end()) {
                id = it->second;
                return;
            }
            const Node* target = scene.find(id);
            outside.push_back(describe(site, *node) + " refers to " +
                              (target != nullptr ? "'" + target->name + "'" : "a node that is gone"));
        });

        preset.nodes_.push_back(std::move(clone));
    }

    if (!outside.empty()) {
        std::string message = "'" + preset.header_.name +
                              "' cannot be made a preset, which may not refer to nodes outside it: ";
        for (std::size_t i = 0; i < outside.size(); ++i) {
            if (i > 0) message += "; ";
            message += outside[i];
        }
        return make_error(std::move(message));
    }
    return preset;
}

// ── From selection ───────────────────────────────────────────────────────

Result<PresetTemplate> PresetTemplate::from_selection(const Scene& scene, const std::unordered_set<uint32_t>& ids) {
    if (ids.empty()) return PresetTemplate{};

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
