#ifndef FJELL_FOG_DENSITY_FIELD_GLSL
#define FJELL_FOG_DENSITY_FIELD_GLSL

// How stirred the air is, in a box that follows the camera. Written by
// FogDensityFieldPass, read by fog.
//
// Where the motion field says something passed and leaves the decay to the
// reader, this has already done the deciding: a disturbance here has been
// drifting, curling and fraying since it was made, so what a reader gets is
// the state of the air now and not a fact about the past to interpret.
//
// Signed: negative where the air was shoved out of the way, positive where it
// piled up. The two very nearly cancel over a wake, which is what lets it
// settle back to nothing by spreading into itself.
//
// Requires, declared before this include:
//   env_ubo                FJELL_DECLARE_ENVIRONMENT_UBO(...)
//   fog_density_field_tex  sampler3D on global binding 18

/// How far into the box the disturbance fades to nothing. The box has a hard
/// edge and moves with the camera, so without this a line would travel over
/// the world. The vertical is softer because the box is shallow: a wake that
/// reached the ceiling should thin out rather than be cut off flat.
const float FOG_DENSITY_FIELD_FADE_XZ = 0.06;
const float FOG_DENSITY_FIELD_FADE_Y = 0.12;

/// The stir at a world position, 0 where nothing has happened and 0 everywhere
/// when the field is off — so a reader scaling by it needs no test of its own.
float fog_density_stir(vec3 world_pos) {
    if (env_ubo.fog_density_field_texture == 0u) return 0.0;

    vec3 uv = (env_ubo.fog_density_field_vp * vec4(world_pos, 1.0)).xyz;
    if (any(lessThan(uv, vec3(0.0))) || any(greaterThan(uv, vec3(1.0)))) return 0.0;

    vec3 to_edge = min(uv, 1.0 - uv);
    float fade = smoothstep(0.0, FOG_DENSITY_FIELD_FADE_XZ, min(to_edge.x, to_edge.z))
               * smoothstep(0.0, FOG_DENSITY_FIELD_FADE_Y, to_edge.y);

    return texture(fog_density_field_tex, uv).r * fade;
}

#endif // FJELL_FOG_DENSITY_FIELD_GLSL
