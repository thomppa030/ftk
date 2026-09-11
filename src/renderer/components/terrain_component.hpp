#pragma once

#include "renderer/resources/resource_handle.hpp"
#include "scene/node_component.hpp"

#include <string>

namespace fjell {

class TerrainAsset;

// Heightfield terrain. Geometry is generated in the mesh shader from the
// asset's height texture rather than stored as vertices, so the component
// carries only the reference and the values the shader needs as constants.
struct TerrainComponent : NodeComponent {
    TerrainAssetHandle terrain_handle;
    TerrainAsset* terrain{nullptr};

    // Project-relative path to the .fjterrain, kept for serialization and for
    // re-resolving the handle after a scene load or asset reimport.
    std::string terrain_path;

    // Drawn as wireframe over the surface. Editor-only, never serialized into
    // the asset itself.
    bool show_wireframe{false};

    [[nodiscard]] bool has_asset() const { return terrain != nullptr; }
};

} // namespace fjell
