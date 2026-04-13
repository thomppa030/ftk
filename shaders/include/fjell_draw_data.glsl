#ifndef FJELL_DRAW_DATA_GLSL
#define FJELL_DRAW_DATA_GLSL

struct DrawData {
    mat4 model;
    mat4 prev_model;      // previous frame's world matrix (for motion vectors)
    uint material_id;
    uint meshlet_offset;
    uint meshlet_count;
    uint vertex_offset;
    uint bone_offset;     // offset into bone SSBO (0xFFFFFFFF = not skinned)
    uint bone_count;
    uint _pad0, _pad1;
};

#endif
