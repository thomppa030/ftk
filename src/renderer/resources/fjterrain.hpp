#pragma once

// .fjterrain binary format — Fjell engine internal terrain format.
// No engine dependencies — standard library and <glm/glm.hpp> only.
//
// A terrain is a square grid of height samples plus per-sample material layer
// weights. Geometry is generated on the GPU from the height data, so no
// vertices or indices are stored here.

#include <cstdint>
#include <string>
#include <vector>

namespace fjell {

static constexpr uint32_t FJTERRAIN_MAGIC   = 0x5254464A; // "JFTR" little-endian
// Version 2 added the name table the layer table indexes into. A version 1
// file is accepted as long as it carries no layers, which none ever did.
static constexpr uint32_t FJTERRAIN_VERSION = 2;

// Layer tiling when a layer table does not say otherwise: one texture repeat
// every four metres reads well for ground materials at character scale.
static constexpr float FJTERRAIN_DEFAULT_LAYER_UV_SCALE = 4.0F;

// Layer weights pack one layer per channel of an RGBA texture, so four layers
// blend without a shader branch.
static constexpr uint32_t FJTERRAIN_MAX_LAYERS = 4;

// Heights are stored normalized to [0,1] and scaled by `height_scale` at load,
// which keeps the on-disk representation independent of world units.
enum class FjTerrainHeightFormat : uint32_t {
    unorm16 = 0, // 2 bytes/sample
    float32 = 1, // 4 bytes/sample
};

struct FjTerrainHeader {
    uint32_t magic;
    uint32_t version;

    // Height samples per side. One greater than the quad count, so adjacent
    // patches share an edge row rather than leaving a seam.
    uint32_t sample_count;
    FjTerrainHeightFormat height_format;

    // World-space extent of the terrain on X and Z, in metres.
    float world_size;
    // World-space height of a normalized sample value of 1.0, in metres.
    float height_scale;
    // Lowest world-space height, corresponding to a sample value of 0.0.
    float height_offset;

    // Quads per patch side. Bounds the mesh shader's per-workgroup output:
    // an N-quad patch emits (N+1)^2 vertices and 2N^2 primitives, and both
    // must fit the device's mesh shader limits.
    uint32_t patch_quads;

    uint32_t layer_count;   // <= FJTERRAIN_MAX_LAYERS
    uint32_t weight_count;  // weight samples per side; 0 when no layers are painted

    uint32_t reserved[6];
};

// A material layer. `material_path` indexes the name table and points at a
// .fjmat relative to the project root; an empty name is an unassigned layer.
struct FjTerrainLayer {
    uint32_t material_path;
    float    uv_scale;      // world metres per texture repeat
};

// Parsed contents of a .fjterrain file.
struct FjTerrainData {
    FjTerrainHeader header{};

    // sample_count^2 entries, row-major, normalized to [0,1].
    std::vector<float> heights;

    // weight_count^2 RGBA entries when painted, one byte per layer. Empty
    // until the first paint stroke: the renderer then treats the whole field
    // as layer 0, which is what a freshly generated terrain wants.
    std::vector<uint8_t> weights;

    std::vector<FjTerrainLayer> layers;
    // The strings the layer table indexes. Written after the layer table.
    std::vector<std::string> names;

    [[nodiscard]] uint32_t sample_count() const { return header.sample_count; }
    [[nodiscard]] uint32_t patch_quads() const { return header.patch_quads; }

    // Patches tile the quad grid, so the sample grid must divide evenly into
    // patches after accounting for the shared edge row.
    [[nodiscard]] uint32_t patches_per_side() const {
        const uint32_t quads = header.sample_count > 0 ? header.sample_count - 1 : 0;
        return (header.patch_quads > 0) ? quads / header.patch_quads : 0;
    }

    [[nodiscard]] float world_height(float normalized) const {
        return header.height_offset + (normalized * header.height_scale);
    }
};

} // namespace fjell
