#ifndef FJELL_INSTANCE_TRANSFORMS_GLSL
#define FJELL_INSTANCE_TRANSFORMS_GLSL

// Shared binding for InstancedMeshComponent per-instance world matrices.
// Set 2 binding 2 in DrawDataDescriptor.
// Only read when DrawData::instance_count > 1; otherwise use DrawData::model.
layout(set = 2, binding = 2) readonly buffer InstanceTransforms {
    mat4 instance_transforms[];
};

mat4 fjell_resolve_model(mat4 base_model, uint instance_count,
                         uint first_instance, uint instance_id) {
    return (instance_count > 1u)
        ? instance_transforms[first_instance + instance_id]
        : base_model;
}

#endif
