#ifndef FJELL_VOLUME_SHAPE_GLSL
#define FJELL_VOLUME_SHAPE_GLSL

// How much of a point sits inside a placed volume, 0 outside and 1 well
// within, falling off smoothly across the volume's edge.
//
// The shape is the unit box or the unit sphere in the volume's own space, so
// the node's transform supplies position, rotation and extent and nothing here
// needs a separate size. Shape values are fjell::ColliderShape
// (src/physics/physics_types.hpp); a shape without an analytic test here reads
// as the box.
//
// The falloff is a fraction of the extent rather than a world distance, so
// scaling a volume keeps its edge proportionally as soft.
//
// The CPU answers the same question with fjell::volume_occupancy() in
// src/scene/volume_occupancy.hpp, and tests/test_volume_occupancy.cpp compiles
// this file as C++ to hold the two together. Written without swizzles for that
// reason.

#define FJELL_VOLUME_SHAPE_BOX     0
#define FJELL_VOLUME_SHAPE_SPHERE  1

float volume_occupancy(int shape, mat4 inv_transform, vec3 world_pos, float edge_falloff) {
    vec3 local = vec3(inv_transform * vec4(world_pos, 1.0));
    float fade = clamp(edge_falloff, 0.0001, 1.0);

    if (shape == FJELL_VOLUME_SHAPE_SPHERE) {
        // Distance from the centre, 0 at the middle and 1 on the shell.
        float r = length(local);
        return 1.0 - smoothstep(1.0 - fade, 1.0, r);
    }

    // Box: fade on whichever axis is closest to its face, so corners fade once
    // rather than three times over and stay the same density as the faces.
    vec3 d = abs(local);
    float edge = max(d.x, max(d.y, d.z));
    return 1.0 - smoothstep(1.0 - fade, 1.0, edge);
}

#endif // FJELL_VOLUME_SHAPE_GLSL
