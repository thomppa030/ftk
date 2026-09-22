#pragma once

#include "renderer/resources/fjwater.hpp"
#include "renderer/resources/water_derivation.hpp"
#include "renderer/resources/water_clipmap.hpp"
#include "renderer/resources/resource_handle.hpp"
#include "scene/component_storage.hpp"
#include "scene/node_component.hpp"

#include <memory>
#include <string>

namespace fjell {

class Material;
class MaterialInstance;

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
    WaterComponent();
    ~WaterComponent();
    WaterComponent(WaterComponent&&) noexcept;
    WaterComponent& operator=(WaterComponent&&) noexcept;

    /// How the surface is extended.
    enum class Extent : std::uint8_t {
        /// A square centred on the node, `size` metres a side. A lake, a
        /// harbour, a pool.
        bounded,
        /// Follows the camera to the horizon, drawn from nested clipmap
        /// levels. `size` and `patches_per_side` do not apply: the levels are
        /// fixed, and only the node's Y is read.
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

    /// The field as the weather has made it: `water` after the wind response,
    /// recomputed at the start of each frame.
    ///
    /// Everything that asks where this water is reads this and not `water` —
    /// the renderer, and buoyancy when it arrives — because a boat has to
    /// float on the sea the storm is blowing, not on the calm one on disk.
    /// `water` stays exactly what was authored so that saving the asset does
    /// not bake in whatever the weather happened to be doing.
    ///
    /// Equal to `water` when the wind response is off, which is the default
    /// and the whole of the editor's world.
    FjWaterData effective_water;

    /// What derives `effective_water` from `water` each frame. Its own per
    /// body of water: it keeps the sea it last drew and any being shaped.
    water::SeaDerivation derivation;

    /// The trails of whatever has moved through this water lately, laid each
    /// frame from the scene's motion sources and handed to `effective_water`
    /// as its wakes. Its own per body of water: whether a source is in this
    /// water is this water's question.
    water::WakeTrails wakes;

    /// The surface shader: a `.fjmat` whose shader is `shader_type water`.
    /// Empty draws with the engine's water.fjsl at its defaults.
    MaterialHandle material_handle;
    Material* material{nullptr};
    std::string material_path;

    /// Per-node override of the surface material. Created on the first
    /// `set_param` from a script and dropped by `reset`, so a sea whose
    /// shader a script drives — rain from the weather, a colour from the
    /// time of day — does not drag every other body of water using the same
    /// `.fjmat` along with it. The pass reads `resolved_material_id()`.
    std::unique_ptr<MaterialInstance> instance;

    /// The SSBO row the surface draws with: the override's when a script has
    /// made one, the shared material's otherwise, 0 when there is no
    /// material and the pass falls back to the engine's own water shader.
    [[nodiscard]] std::uint32_t resolved_material_id() const;

    /// Metres between vertices at the current settings, for the inspector to
    /// show against what the wave field asks for.
    ///
    /// For an infinite surface this is the spacing at the camera, on the
    /// finest clipmap level. That is the one the shortest train has to be
    /// measured against: the coarser levels are further away, where a train
    /// too short to draw is also too small to see.
    [[nodiscard]] float vertex_spacing() const {
        if (extent == Extent::infinite) {
            return WaterClipmap::finest_spacing();
        }
        std::uint32_t patches = patches_per_side > 0 ? patches_per_side : 1;
        return size / static_cast<float>(patches * WATER_PATCH_QUADS);
    }
};

// A duplicate is the same water with the same material, but starts without
// the per-node MaterialInstance: that is runtime override state belonging to
// the one node a script set it on.
void clone_component(const ComponentStorage& from, const WaterComponent& src, ComponentStorage& to);

} // namespace fjell
