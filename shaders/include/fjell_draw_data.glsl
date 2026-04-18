#ifndef FJELL_DRAW_DATA_GLSL
#define FJELL_DRAW_DATA_GLSL

// Per-draw metadata, reached via VkDrawIndexedIndirectCommand::firstInstance
// = draw_index (see IndirectBatch::build). Layout must match GpuDrawData in
// indirect_batch.hpp byte-for-byte (std430, 192 bytes).

struct DrawData {
    mat4 model;
    mat4 prev_model;
    uint material_id;
    uint meshlet_offset;
    uint meshlet_count;
    uint vertex_offset;
    uint bone_offset;
    uint bone_count;
    uint instance_count;     // 1 = single instance; N = instanced
    uint first_instance;     // offset into the shared InstanceTransforms SSBO (legacy path)

    // Buffer device addresses (valid when instance_count > 1). Unused by the
    // currently-bound shaders — they still read from the set 2 binding 2/3
    // SSBOs via first_instance. Step-by-step migration will switch shaders to BDA access.
    uvec2 instances_addr_raw;
    uvec2 visible_addr_raw;
    uvec2 prev_instances_addr_raw;
    uvec2 _pad_end_raw;
};

#endif
