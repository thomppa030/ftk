#ifndef FJELL_RENDERER_CONSTANTS_GLSL
#define FJELL_RENDERER_CONSTANTS_GLSL

// Generated from C++ headers by cmake/GenerateShaderConstants.cmake.
// Do not edit by hand — update the source .hpp and rerun cmake.

const uint MAX_LIGHTS_PER_TILE = 256;
const uint LIGHT_TILE_SIZE = 16;
const uint MAX_TILES = 16384;
const uint MAX_PIPELINE_BINS = 16;
const uint LOCAL_SHADOW_ATLAS_SIZE = 8192;
const uint LOCAL_SHADOW_TILE_SIZE = 1024;
const uint MAX_SHADOWED_SPOT_LIGHTS = 8;
const uint MAX_SHADOWED_POINT_LIGHTS = 4;
const uint SHADOW_MAP_SIZE = 4096;

const float LOCAL_SHADOW_ATLAS_SIZE_F = float(LOCAL_SHADOW_ATLAS_SIZE);
const float SHADOW_MAP_SIZE_F = float(SHADOW_MAP_SIZE);
const float OVERHEAD_SLAB_METRES = 40.0 + 40.0;

#endif // FJELL_RENDERER_CONSTANTS_GLSL
