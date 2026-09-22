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
} pc;

const uint FOG_HAZE = 0xffffffffu;

#endif // FJELL_FOG_PUSH_GLSL
