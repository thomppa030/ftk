#pragma once

#include <cstdint>

namespace fjell {

static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 3;

// Shader-visible constants are mirrored into fjell_renderer_constants.glsl
// by cmake/GenerateShaderConstants.cmake — update here and reconfigure.
static constexpr uint32_t MAX_LIGHTS_PER_TILE = 256;
static constexpr uint32_t LIGHT_TILE_SIZE = 16;
static constexpr uint32_t MAX_TILES = 16384;
static constexpr uint32_t MAX_PIPELINE_BINS = 16;

// Local light shadow atlas
static constexpr uint32_t LOCAL_SHADOW_ATLAS_SIZE = 8192;
static constexpr uint32_t LOCAL_SHADOW_TILE_SIZE = 1024;
static constexpr uint32_t MAX_SHADOWED_SPOT_LIGHTS = 8;
static constexpr uint32_t MAX_SHADOWED_POINT_LIGHTS = 4;

} // namespace fjell
