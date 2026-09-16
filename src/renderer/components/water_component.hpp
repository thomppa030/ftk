#pragma once

#include "renderer/resources/fjwater.hpp"
#include "renderer/resources/resource_handle.hpp"
#include "scene/node_component.hpp"

#include <string>

namespace fjell {

class Material;

/// A body of water.
///
/// The surface is generated in a mesh shader from the node's `.fjwater` wave
/// field rather than stored as vertices, so the component carries the
/// reference and the extent to generate over, not geometry.
///
/// Where the water sits is the node's transform: its Y is the surface at
/// rest, and the waves move about it. Rotation does not turn the sea —
/// a wave field travels in world directions, and a rotated body of water is
/// the same water.
struct WaterComponent : NodeComponent {
    /// How the surface is extended.
    enum class Extent : std::uint8_t {
        /// A square centred on the node, `size` metres a side. A lake, a
        /// harbour, a pool.
        bounded,
        /// Follows the camera to the horizon. Not yet implemented — the
        /// clipmap that draws it is its own piece of work — and a node set to
        /// it draws as bounded meanwhile.
        infinite,
    };

    Extent extent{Extent::bounded};

    /// Side length in metres for `bounded`.
    float size{200.0f};

    /// How finely the surface is generated, as patches per side. Each patch
    /// is a fixed 11 quads, so the spacing is `size / (patches * 11)` metres.
    ///
    /// This is what decides whether the shortest waves in the field can be
    /// drawn at all: under about four vertices per wavelength a train reads
    /// as noise rather than chop. The editor shows what the field needs.
    std::uint32_t patches_per_side{24};

    /// Project-relative path to the `.fjwater`, kept for serialization and
    /// for reloading after an edit.
    std::string water_path;

    /// The wave field itself, loaded from that path.
    ///
    /// Held by value rather than through the resource pools: a `.fjwater` is
    /// a list of numbers with nothing on the GPU to share and nothing to
    /// unload, and two nodes naming the same file want their own copy the
    /// moment one of them is driven from a storm. An empty list is flat
    /// water, which is a sea a scene can legitimately want.
    FjWaterData water;
    bool water_loaded{false};

    /// The surface shader: a `.fjmat` whose shader is `shader_type water`.
    /// Empty draws with the engine's water.fjsl at its defaults.
    MaterialHandle material_handle;
    Material* material{nullptr};
    std::string material_path;

    /// Metres between vertices at the current settings, for the inspector to
    /// show against what the wave field asks for.
    [[nodiscard]] float vertex_spacing() const {
        std::uint32_t patches = patches_per_side > 0 ? patches_per_side : 1;
        return size / static_cast<float>(patches * 11);
    }
};

} // namespace fjell
