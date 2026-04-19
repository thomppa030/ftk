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
    uint instance_count;  // 1 = single-instance; N = instanced (read InstanceTransforms)
    uint first_instance;  // offset into InstanceTransforms when instance_count > 1
    // Offset into SplineSegments SSBO for this draw's first instance.
    // 0xFFFFFFFF = not a spline-mesh draw (vertex stage skips deform).
    uint spline_segment_offset;
    uint _pad0;
    uint _pad1;
    uint _pad2;
};

#endif
