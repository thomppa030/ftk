#pragma once

#include <cstdint>

namespace fjell {

static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 3;

// Must match GLSL constants in mesh.frag, light_cull.comp, light_heatmap.frag
static constexpr uint32_t MAX_LIGHTS_PER_TILE = 256;
static constexpr uint32_t LIGHT_TILE_SIZE = 16;
static constexpr uint32_t MAX_TILES = 16384;
static constexpr uint32_t SHADOW_MAP_SIZE = 2048;
static constexpr uint32_t MAX_PIPELINE_BINS = 16;

// Local light shadow atlas
static constexpr uint32_t LOCAL_SHADOW_ATLAS_SIZE = 8192;
static constexpr uint32_t LOCAL_SHADOW_TILE_SIZE = 1024;
static constexpr uint32_t MAX_SHADOWED_SPOT_LIGHTS = 8;
static constexpr uint32_t MAX_SHADOWED_POINT_LIGHTS = 4;

} // namespace fjell
