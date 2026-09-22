#ifndef FJELL_FOG_PUSH_GLSL
#define FJELL_FOG_PUSH_GLSL

// The push block every dispatch of the volumetric fog pass shares: the
// generated fog shader in its medium and light variants, and the temporal,
// integrate and composite steps behind them. One declaration so none of
// them can drift from each other or from FogCore::PushConstants, which
// mirrors it field for field.
//
// Each step reads the fields it needs and ignores the rest. A medium dispatch
// is one source of fog: the haze when volume_index is FOG_HAZE, else the
// placed volume at that index of the fog volume SSBO, drawn with the material
// row pushed alongside.
layout(push_constant) uniform PushConstants {
    uvec4 volume_dims;          // xyz = froxel grid, w = frame index
    float near_plane;
    float far_plane;
    float temporal_blend;
    uint viewport_width;
    uint viewport_height;
    float jitter_x;             // Halton jitter, precomputed on the CPU
    float jitter_y;
    float jitter_z;
    float origin_x;             // the source's node position: ORIGIN in density()
    float origin_y;
    float origin_z;
    uint volume_index;          // FOG_HAZE, or the volume this dispatch adds
    float motion_response;      // how far the haze answers movement
    float density_field_debug;  // paints the stirred air; 0 is off
    uint fog_material_id;       // the source's row in the material buffer
    uint source_kind;           // FOG_SOURCE_HAZE, _VOLUME or _WATER
    uint camera_below;          // water: 1 when the camera is under the surface
} pc;

const uint FOG_HAZE = 0xffffffffu;

// What a medium dispatch adds: the scene's haze, a placed volume, or the body
// of a water whose froxels are told from the surface depth.
const uint FOG_SOURCE_HAZE = 0u;
const uint FOG_SOURCE_VOLUME = 1u;
const uint FOG_SOURCE_WATER = 2u;

#endif // FJELL_FOG_PUSH_GLSL
