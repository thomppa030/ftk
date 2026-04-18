#ifndef FJELL_INSTANCE_TRANSFORMS_GLSL
#define FJELL_INSTANCE_TRANSFORMS_GLSL

// Per-instance world matrices for InstancedMeshComponent.
//
// Binding 2 holds the raw CPU-uploaded transforms (one slot per source
// instance). It is read by the mesh shader path — meshlet_cull.comp culls
// each meshlet individually, so it directly indexes the raw slice.
//
// Binding 3 holds the compacted post-cull transforms written by
// InstanceCullPass. The vertex render path reads this one — since the
// compute pass also patches VkDrawIndexedIndirectCommand::instanceCount to
// the compacted count, `gl_InstanceIndex - gl_BaseInstance` lands exactly
// inside the compacted slice.
//
// Both bindings share the same per-draw slice offset (DrawData::first_instance)
// and are only populated when the scene contains an InstancedMeshComponent
// (PARTIALLY_BOUND_BIT on the descriptor set layout).

layout(set = 2, binding = 2) readonly buffer InstanceTransforms {
    mat4 instance_transforms[];
};

layout(set = 2, binding = 3) readonly buffer VisibleInstanceTransforms {
    mat4 visible_instance_transforms[];
};

#endif
