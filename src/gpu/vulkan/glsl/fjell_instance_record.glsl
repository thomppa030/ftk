// One instance record a top-level acceleration structure is built from, as
// the Vulkan backend lays it out (VkAccelerationStructureInstanceKHR, 64
// bytes), for a shader that writes records: what Device::write_instances()
// writes on the CPU.
#ifndef FJELL_INSTANCE_RECORD_GLSL
#define FJELL_INSTANCE_RECORD_GLSL

struct InstanceRecord {
    // Object to world: the three rows of a 3 x 4 matrix, whose last column
    // is the translation.
    float transform[12];
    // Custom index (24 bits) and mask (8 bits).
    uint custom_index_and_mask;
    // Binding table offset (24 bits, unused) and flags (8 bits).
    uint offset_and_flags;
    // What Device::instance_reference() named the bottom level by.
    uvec2 reference;
};

// Both sides of the triangles face every ray
// (VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR).
const uint INSTANCE_RECORD_BOTH_SIDES = 0x01u;

// An instance of the bottom level `reference` names, placed by
// `transform`, which rays whose cull mask shares a bit with `mask` see, and
// of which a hit reads back `custom_index`. Both sides of its triangles are
// hit.
InstanceRecord instance_record(float transform[12], uint custom_index, uint mask, uvec2 reference) {
    InstanceRecord record;
    record.transform = transform;
    record.custom_index_and_mask = (custom_index & 0xFFFFFFu) | ((mask & 0xFFu) << 24);
    record.offset_and_flags = INSTANCE_RECORD_BOTH_SIDES << 24;
    record.reference = reference;
    return record;
}

#endif
