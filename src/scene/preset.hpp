#pragma once

#include "ftk/base/result.hpp"
#include "scene/node.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace fjell {

class ResourceManager;
class Project;
class Scene;

struct PresetHeader {
    std::string name;
};

/// The nodes a `.fjp` holds, placed into a scene as a copy with fresh ids.
/// Scripts know the loaded asset as `Asset<Preset>`; this is what it holds.
///
/// Its nodes have ids of their own, and a reference between them holds those.
/// A preset refers to nothing outside itself: every placed copy would share
/// that node, and a preset belongs to no one scene.
class PresetTemplate {
public:
    [[nodiscard]] static Result<PresetTemplate> load(const std::string& path, ResourceManager& resources,
                                                     const Project* project = nullptr);
    [[nodiscard]] Result<> save(const std::string& path, const Project* project = nullptr) const;

    /// The preset `nodes` make, their ids and parents already the preset's
    /// own, the root first. A reference to a node that is not among them is
    /// dropped with a warning naming `source`: a file from before presets
    /// kept to themselves, or written by hand, would otherwise point every
    /// placed copy at whatever scene node has that id.
    [[nodiscard]] static PresetTemplate from_nodes(std::string name, std::vector<std::unique_ptr<Node>> nodes,
                                                   std::string_view source);

    /// Places a copy in `scene` under `parent_id` (-1 for none), its root moved
    /// by `offset`, and returns the root's id. Every node gets a fresh id, and
    /// references between them point at the copy's own nodes.
    uint32_t instantiate(Scene& scene, int32_t parent_id = -1, Transform offset = {}) const;

    /// A preset of the node `root_id` and everything under it, the root moved
    /// to the origin, with references between those nodes made its own. A
    /// reference to any other node is refused, naming each. The subtree's
    /// scripts are read as their entries hold them, so live scripts are saved
    /// into their entries first.
    [[nodiscard]] static Result<PresetTemplate> from_subtree(const Scene& scene, uint32_t root_id);

    /// The same for a selection, rooted at its topmost node.
    [[nodiscard]] static Result<PresetTemplate> from_selection(const Scene& scene,
                                                               const std::unordered_set<uint32_t>& ids);

    [[nodiscard]] const PresetHeader& header() const { return header_; }
    [[nodiscard]] const std::vector<std::unique_ptr<Node>>& nodes() const { return nodes_; }

private:
    PresetHeader header_;
    std::vector<std::unique_ptr<Node>> nodes_;
    uint32_t local_next_id_{1};
};

} // namespace fjell
