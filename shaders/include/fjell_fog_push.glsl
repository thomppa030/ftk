#ifndef FJELL_FOG_PUSH_GLSL
#define FJELL_FOG_PUSH_GLSL

// The push block every dispatch of the volumetric fog pass shares: the
// generated fog medium shader and the temporal, integrate and composite
// steps behind it. One declaration so the four cannot drift from each other
// or from FogCore::PushConstants, which mirrors it field for field.
//
// Each step reads the fields it needs and ignores the rest; the medium shader
// is the only reader of the volumes, the motion response and its material row.
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
    float origin_x;             // the fog node's world position: ORIGIN in density()
    float origin_y;
    float origin_z;
    uint volume_count;          // entries of the fog volume SSBO to read
    float motion_response;      // how far the height fog answers movement
    float density_field_debug;  // paints the stirred air; 0 is off
    uint fog_material_id;       // the fog node's row in the material buffer
} pc;

#endif // FJELL_FOG_PUSH_GLSL
