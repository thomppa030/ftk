#pragma once

#include "core/result.hpp"
#include "scene/node.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace fjell {

class ResourceManager;
class Project;
class Scene;

struct PresetHeader {
    std::string name;
};

class Preset {
public:
    [[nodiscard]] static Result<Preset> load(const std::string& path,
                                             ResourceManager& resources,
                                             const Project* project = nullptr);
    [[nodiscard]] Result<> save(const std::string& path, const Project* project = nullptr) const;

    /// Instantiate into scene. Returns root node ID.
    /// All preset-local IDs are remapped to fresh scene IDs.
    uint32_t instantiate(Scene& scene, int32_t parent_id = -1,
                         Transform offset = {}) const;

    /// Build a preset from a single node and all its descendants.
    static Preset from_subtree(const Scene& scene, uint32_t root_id);

    /// Build a preset from a selection of nodes (uses the first selected
    /// node as root and collects its subtree).
    static Preset from_selection(const Scene& scene,
                                 const std::unordered_set<uint32_t>& ids);

    [[nodiscard]] const PresetHeader& header() const { return header_; }
    [[nodiscard]] const std::vector<std::unique_ptr<Node>>& nodes() const { return nodes_; }

private:
    PresetHeader header_;
    std::vector<std::unique_ptr<Node>> nodes_;
    uint32_t local_next_id_{1};
};

} // namespace fjell
