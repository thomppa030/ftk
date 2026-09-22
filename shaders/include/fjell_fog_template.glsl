#version 460
#extension GL_EXT_nonuniform_qualifier : require

// FJSL fog template: the volumetric fog pass's medium and light dispatches,
// with the look handed to a `shader_type fog;` .fjsl. Compiled twice from
// one generated source:
//
//   as is                   the medium variant, one dispatch per source of
//                           fog. The haze covers the froxel grid and writes
//                           what its density() decides; each placed volume
//                           then adds its own over the froxels its bounds
//                           reach. The image holds, per froxel, the sum of
//                           colour × scattering × density and the sum of
//                           density.
//   with FJELL_FOG_LIGHT    the light variant, once over the grid from the
//                           scene's fog node material: the stir, the distance
//                           fade, then every light through light(), stored
//                           as in-scatter and extinction for the temporal and
//                           integrate steps behind it.
//
// The user writes density(), which says what the medium is at WORLD_POS —
// DENSITY, COLOR and SCATTERING — and may write light(), which says how that
// medium scatters one light toward the eye. Everything else here is the
// engine's. A fog shader is named for what it writes, a medium at a point,
// and not for the froxel grid this pass happens to sample it on.

#include "fjell_common.glsl"
#include "fjell_global_ubo.glsl"
#include "fjell_gpu_types.glsl"
#include "fjell_renderer_constants.glsl"
#include "fjell_volume.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

FJELL_DECLARE_GLOBAL_UBO(0, 0, ubo);
#include "fjell_environment.glsl"
FJELL_DECLARE_ENVIRONMENT_UBO(0, FJELL_ENVIRONMENT_UBO_BINDING, env_ubo);
layout(set = 0, binding = 15) uniform sampler2D cloud_shadow_tex;
#include "fjell_cloud_shadow.glsl"
layout(set = 0, binding = 16) uniform sampler2D motion_field_tex;
#include "fjell_motion_field.glsl"
layout(set = 0, binding = 18) uniform sampler3D fog_density_field_tex;
#include "fjell_fog_density_field.glsl"

#ifdef FJELL_FOG_LIGHT
layout(set = 0, binding = 1) uniform sampler2DArrayShadow shadow_map;

layout(set = 0, binding = 2) readonly buffer LightBuffer {
    GpuPointLight point_lights[1024];
    GpuSpotLight spot_lights[1024];
};

layout(set = 0, binding = 6) uniform samplerCube sky_cubemap;

layout(set = 0, binding = 11) uniform sampler2DShadow local_shadow_atlas;
#else
layout(set = 0, binding = 17) readonly buffer FogVolumeBuffer {
    GpuFogVolume fog_volumes[];
};
#endif

// The material set: the fog shader's own textures and parameters live in the
// same bindless tables every other material reads.
layout(set = 1, binding = 0) uniform sampler2D textures[];

layout(set = 1, binding = 1) readonly buffer MaterialBuffer {
    GpuMaterial materials[];
};

layout(set = 1, binding = 2) readonly buffer ParamsBuffer {
    float custom_params[];
};

// The medium accumulated so far: rgb = Σ colour × scattering × density,
// a = Σ density. The haze writes it, each volume adds to it, the light
// variant reads it.
#ifdef FJELL_FOG_LIGHT
layout(set = 2, binding = 0, rgba16f) uniform readonly image3D fog_medium;
layout(set = 2, binding = 1, rgba16f) uniform writeonly image3D scatter_volume;
#else
layout(set = 2, binding = 0, rgba16f) uniform image3D fog_medium;
#endif

#include "fjell_fog_push.glsl"

// Exponential depth distribution
float slice_to_depth(float t) {
    return pc.near_plane * pow(pc.far_plane / pc.near_plane, t);
}

// Light arriving equally from every direction scatters toward the eye by
// 1/4π, whatever the medium's asymmetry — the phase function integrates to
// one over the sphere, so an even distribution leaves only the average.
const float ISOTROPIC_PHASE = 0.0795774715;

// Which light a fog shader's light() is being asked about.
const int FOG_LIGHT_SUN = 0;
const int FOG_LIGHT_POINT = 1;
const int FOG_LIGHT_SPOT = 2;
const int FOG_LIGHT_AMBIENT = 3;

// How far above what came through the fog still answers it: a walker stirs
// the fog around their legs and leaves what is over their head alone.
const float MOTION_FADE_METRES = 1.0;

// What the height fog gives up along a path, and how much of it piles along
// the flanks instead. Far more than a volume's MOTION_THIN, because with no
// wisps to drag the thinning is the whole of what shows rather than a hint on
// top of it. The heap is what keeps the trough from reading as fog deleted
// rather than moved.
//
// Expect little from these in an open scene, and do not reach for them when
// it does not show: how bright fog looks along a ray is set by the whole
// column, and what a walker disturbs is a few metres of it. Taking density
// out of those metres loses a little in-scatter and gains back nearly the
// same amount of light from the fog behind, because fog thin enough to see
// through is also thin enough to let that light past. Against a haze reaching
// hundreds of metres the two cancel to within a fraction of a percent, and
// emptying the wake completely is no more visible than thinning it.
//
// A fog volume is the other case: the bank is a large enough share of the fog
// along the ray for the same thinning to read plainly, which is why the volume
// path leans on it. Height fog wants something that changes what the fog looks
// like rather than how much of it there is.
const float GLOBAL_MOTION_THIN = 0.5;
const float GLOBAL_MOTION_HEAP = 0.35;

// What a stirred texel is worth to the fog around it. A full-strength wake
// takes the air to somewhat over half and a full-strength heap to somewhat
// under half again, which is a plain change in a bank of fog without either
// end reading as a hole punched through it or a wall built beside it.
const float FOG_DENSITY_STRENGTH = 0.8;
const float FOG_DENSITY_FLOOR = 0.2;
const float FOG_DENSITY_CEIL = 1.8;

// How bright the debug colour comes out once the integrator's own scaling has
// been undone. A froxel's slice is a fraction of a metre near the camera, so
// the colour is spread over many of them before it reaches the eye.
const float DEBUG_EMIT_GAIN = 0.25;

// Henyey-Greenstein phase function, for a light() body to shape its lobe
// with. The engine's default clamps the result at 0.1: the isotropic baseline
// is about 0.08, so that keeps the forward peak gentle and stops the sun
// direction smearing a bright blob across the fog.
float henyey_greenstein(float g, float cos_theta) {
    float g2 = g * g;
    float denom = 1.0 + g2 - 2.0 * g * cos_theta;
    return (1.0 - g2) / (4.0 * 3.14159265 * denom * sqrt(denom));
}

#ifdef FJELL_FOG_LIGHT
// ── Local light shadow sampling for volumetric ─────────────────────
// 4-tap PCF with per-froxel jitter to reduce blockiness across frames.

const float POINT_SHADOW_NEAR = 0.1;

// 4-tap offsets for cheap volumetric PCF
const vec2 VOL_PCF[4] = vec2[](
    vec2(-0.5, -0.5), vec2( 0.5, -0.5),
    vec2(-0.5,  0.5), vec2( 0.5,  0.5)
);

// Per-froxel pseudo-random rotation (breaks up grid alignment between froxels)
mat2 vol_random_rotation(ivec3 coord, uint frame) {
    float angle = fract(sin(float(coord.x * 73 + coord.y * 157 + coord.z * 311 + frame * 37)) * 43758.5453) * 6.2831853;
    float s = sin(angle);
    float c = cos(angle);
    return mat2(c, s, -s, c);
}

float sample_spot_shadow_vol(vec3 world_pos, GpuSpotLight light, mat2 rot) {
    if (light.shadow_tile_size == 0) return 1.0;

    vec4 light_clip = light.shadow_vp * vec4(world_pos, 1.0);
    vec3 proj = light_clip.xyz / light_clip.w;
    // Outside the shadow frustum = fully shadowed. For spot lights the
    // shadow VP covers the cone's extent; anything past the near/far or
    // outside the projection can't receive light.
    if (proj.z < 0.0 || proj.z > 1.0) return 0.0;

    vec2 suv = proj.xy * 0.5 + 0.5;
    if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) return 0.0;

    float tile_size = float(light.shadow_tile_size);
    float compare = proj.z - 0.003;
    float texel = 1.0 / LOCAL_SHADOW_ATLAS_SIZE_F;
    float spread = 2.0 * texel * tile_size / 1024.0;

    float shadow = 0.0;
    for (int i = 0; i < 4; ++i) {
        vec2 offset = rot * VOL_PCF[i] * spread;
        vec2 sample_suv = clamp(suv + offset / tile_size * LOCAL_SHADOW_ATLAS_SIZE_F, vec2(0.001), vec2(0.999));
        vec2 atlas_uv = (vec2(float(light.shadow_atlas_x), float(light.shadow_atlas_y))
                        + sample_suv * tile_size) / LOCAL_SHADOW_ATLAS_SIZE_F;
        shadow += texture(local_shadow_atlas, vec3(atlas_uv, compare));
    }
    return shadow / 4.0;
}

float sample_point_shadow_vol(vec3 world_pos, GpuPointLight light, mat2 rot) {
    if (light.shadow_info.w == 0) return 1.0;

    vec3 to_frag = world_pos - light.position_radius.xyz;
    float dist2 = dot(to_frag, to_frag);
    float inv_dist = inversesqrt(max(dist2, 1e-8));
    float dist = dist2 * inv_dist;
    vec3 dir = to_frag * inv_dist;

    vec3 abs_dir = abs(dir);
    int face;
    vec2 face_uv;
    if (abs_dir.x >= abs_dir.y && abs_dir.x >= abs_dir.z) {
        face = dir.x > 0.0 ? 0 : 1;
        face_uv = dir.x > 0.0 ? vec2(-dir.z, -dir.y) : vec2(dir.z, -dir.y);
        face_uv /= abs_dir.x;
    } else if (abs_dir.y >= abs_dir.z) {
        face = dir.y > 0.0 ? 2 : 3;
        face_uv = dir.y > 0.0 ? vec2(dir.x, dir.z) : vec2(dir.x, -dir.z);
        face_uv /= abs_dir.y;
    } else {
        face = dir.z > 0.0 ? 4 : 5;
        face_uv = dir.z > 0.0 ? vec2(dir.x, -dir.y) : vec2(-dir.x, -dir.y);
        face_uv /= abs_dir.z;
    }
    face_uv = face_uv * 0.5 + 0.5;

    int tile_size = light.shadow_info.z;
    int face_col = face % 2;
    int face_row = face / 2;
    vec2 tile_origin = vec2(float(light.shadow_info.x + face_col * tile_size),
                            float(light.shadow_info.y + face_row * tile_size));

    float far = light.position_radius.w;
    float z_buf = far * (dist - POINT_SHADOW_NEAR) / (dist * (far - POINT_SHADOW_NEAR));
    float compare = z_buf - 0.005;

    float texel = 1.0 / float(tile_size);
    float spread = 2.0 * texel;

    float shadow = 0.0;
    for (int i = 0; i < 4; ++i) {
        vec2 offset = rot * VOL_PCF[i] * spread;
        vec2 uv = clamp(face_uv + offset, vec2(0.001), vec2(0.999));
        vec2 atlas_uv = (tile_origin + uv * float(tile_size)) / LOCAL_SHADOW_ATLAS_SIZE_F;
        shadow += texture(local_shadow_atlas, vec3(atlas_uv, compare));
    }
    return shadow / 4.0;
}

// Simple single-tap shadow for volumetric (no PCF needed at froxel resolution)
float sample_shadow(vec3 world_pos) {
    float view_depth = -(ubo.view * vec4(world_pos, 1.0)).z;

    int cascade = 2;
    for (int i = 0; i < 3; ++i) {
        if (view_depth < ubo.cascade_splits[i]) {
            cascade = i;
            break;
        }
    }

    vec4 lp = ubo.cascade_vp[cascade] * vec4(world_pos, 1.0);
    vec3 proj = lp.xyz / lp.w;
    vec2 suv = proj.xy * 0.5 + 0.5;

    // Outside the furthest cascade = no reliable shadow info. Treat as
    // shadowed so distant/sky froxels don't receive full sun in-scatter
    // and smear a phase-peaked blob across the fog in the sun direction.
    if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0 ||
        proj.z < 0.0 || proj.z > 1.0) {
        return 0.0;
    }

    float bias = 0.005 * (1.0 + float(cascade));
    return texture(shadow_map, vec4(suv, float(cascade), proj.z - bias)) * cloud_shadow_factor(world_pos);
}
#endif // FJELL_FOG_LIGHT

// Windowed attenuation for volumetric lights.
// The near-source floor scales with light radius so that a froxel landing
// close to the light can't deposit unbounded radiance (which used to smear
// across the sky through trilinear sampling of the last froxel slice).
float vol_attenuation(float dist, float radius) {
    float t = dist / radius;
    float window = clamp(1.0 - t * t, 0.0, 1.0);
    window *= window;
    float min_dist_sq = radius * radius * 0.01;
    float inv_sq = 1.0 / max(dist * dist, min_dist_sq);
    return inv_sq * window;
}

// ── Custom texture + param defines (generated by FJSL parser) ──
/*FJSL_CUSTOM_TEXTURES*/

// ── User-authored file-scope helpers / constants (generated by FJSL parser) ──
/*FJSL_USER_GLOBALS*/

// The fog shader's density() with its built-ins in scope. The outputs are
// parameters rather than a returned struct so a body may `return;` early,
// and so a uniform named `density` cannot collide with a member.
//
// For the haze OCCUPANCY is one, LOCAL_POS is the offset from the node and
// DRIFT is zero. For a placed volume LOCAL_POS is the point in the volume's
// own unit space, OCCUPANCY how far inside its shape the point sits, and
// DRIFT how far the node has carried its field so far.
void fjell_fog_density(vec3 WORLD_POS, vec3 VIEW_DIR, float VIEW_DIST, vec3 LOCAL_POS,
                       float OCCUPANCY, vec3 DRIFT, float MOTION_RESPONSE,
                       MotionSample MOTION, MotionGradient MOTION_FALLOFF, float STIR,
                       inout float DENSITY, inout vec3 COLOR, inout float SCATTERING) {
    // ── Where ──
    vec3 ORIGIN = vec3(pc.origin_x, pc.origin_y, pc.origin_z);
    vec3 CAMERA_POS = ubo.camera_position.xyz;

    // ── Bodies and time ──
    vec3 SUN_DIR = normalize(env_ubo.sun_dir_intensity.xyz);
    float TIME = ubo.time;
    float TIME_OF_DAY = env_ubo.weather1.w;

    // ── Weather ──
    vec3 WIND_DIR = env_ubo.wind_dir_speed_gust.xyz;
    float WIND_SPEED = env_ubo.wind_dir_speed_gust.w;
    vec2 WIND_DRIFT = env_ubo.wind_drift.xy;
    float CLOUD_COVERAGE = env_ubo.weather0.x;
    float PRECIPITATION = env_ubo.weather0.z;

    // ── User density code ──
    {
/*FJSL_USER_CODE*/
    }
}

// The fog shader's light(), once per light that reaches this froxel. What it
// adds to SCATTERED is multiplied by the medium's colour, scattering and
// density afterwards, so a body stays about the phase and the shadow.
void fjell_fog_light(int LIGHT_KIND, vec3 LIGHT_DIR, vec3 LIGHT_COLOR, float COS_THETA,
                     float SHADOW, vec3 WORLD_POS, vec3 VIEW_DIR, float DENSITY, vec3 COLOR,
                     inout vec3 SCATTERED) {
    // ── User light code ──
    {
/*FJSL_LIGHT_USER_CODE*/
    }
}

// Where this froxel is in the world, and which way the eye looks through it.
struct FjellFroxel {
    vec3 world_pos;
    vec3 ray_dir;
    float linear_depth;   // view-space depth of the jittered sample
    vec3 froxel_uv;
};

FjellFroxel fjell_froxel(ivec3 coord) {
    FjellFroxel f;
    // Froxel UV with precomputed Halton jitter for temporal stability
    f.froxel_uv = (vec3(coord) + 0.5 + vec3(pc.jitter_x, pc.jitter_y, pc.jitter_z))
                  / vec3(pc.volume_dims.xyz);

    // Reconstruct world position from froxel coordinates
    f.linear_depth = slice_to_depth(f.froxel_uv.z);
    vec2 ndc_xy = f.froxel_uv.xy * 2.0 - 1.0;

    // Get world-space ray from camera through this froxel
    vec4 near_world = ubo.inv_view_proj * vec4(ndc_xy, 0.0, 1.0);
    near_world /= near_world.w;
    vec4 far_world = ubo.inv_view_proj * vec4(ndc_xy, 1.0, 1.0);
    far_world /= far_world.w;
    f.ray_dir = normalize(far_world.xyz - near_world.xyz);

    // Walk from this froxel column's own near-plane point to the slice's
    // view-space depth. Starting at the near point instead of the eye keeps
    // the reconstruction valid for orthographic cameras, where each column
    // has a distinct origin; for perspective the two coincide.
    vec3 view_forward = -vec3(ubo.view[0][2], ubo.view[1][2], ubo.view[2][2]);
    float cos_angle = dot(f.ray_dir, view_forward);
    float near_dist = -(ubo.view * near_world).z;
    float ray_dist = (f.linear_depth - near_dist) / max(cos_angle, 1e-6);
    f.world_pos = near_world.xyz + f.ray_dir * ray_dist;
    return f;
}

#ifndef FJELL_FOG_LIGHT

// ── Medium variant: one source of fog into the accumulated fjell_medium ──

void main() {
    ivec3 coord = ivec3(gl_GlobalInvocationID.xyz);
    if (any(greaterThanEqual(coord, ivec3(pc.volume_dims.xyz)))) return;

    FjellFroxel f = fjell_froxel(coord);
    vec3 world_pos = f.world_pos;

    const bool fjell_haze = pc.volume_index == FOG_HAZE;

    // Where the point sits for this source. A volume that does not reach
    // the froxel adds nothing and leaves the image as the sources before it
    // left it; the haze reaches everywhere and is the first to write.
    vec3 fjell_local_pos = world_pos - vec3(pc.origin_x, pc.origin_y, pc.origin_z);
    float fjell_occupancy = 1.0;
    vec3 fjell_drift = vec3(0.0);
    float fjell_motion_response = pc.motion_response;
    if (!fjell_haze) {
        GpuFogVolume vol = fog_volumes[pc.volume_index];
        fjell_occupancy = volume_occupancy(int(vol.shape_falloff.x), vol.inv_transform,
                                     world_pos, vol.shape_falloff.y);
        if (fjell_occupancy <= 0.0) return;
        fjell_local_pos = (vol.inv_transform * vec4(world_pos, 1.0)).xyz;
        fjell_drift = vol.drift.xyz;
        fjell_motion_response = vol.shape_falloff.z;
    }

    // What has come through here lately, for everything that answers it.
    MotionSample fjell_motion = motion_at(world_pos);
    // Which way the wake weakens, for telling its two sides apart.
    MotionGradient fjell_motion_falloff = fjell_motion.recency > 0.0
        ? motion_gradient(world_pos, MOTION_FIELD_TEXEL_M)
        : MotionGradient(vec2(0.0), 0.0);
    // What the air here has actually been through, as it stands now, for a
    // shader that shapes its fjell_medium by it. The engine scales the whole
    // fjell_medium by it in the light variant.
    float fjell_stir = fog_density_stir(world_pos);

    float fjell_density = 0.0;
    vec3 fjell_color = vec3(1.0);
    float fjell_scattering = 1.0;
    fjell_fog_density(world_pos, f.ray_dir, distance(world_pos, ubo.camera_position.xyz),
                      fjell_local_pos, fjell_occupancy, fjell_drift, fjell_motion_response,
                      fjell_motion, fjell_motion_falloff, fjell_stir,
                      fjell_density, fjell_color, fjell_scattering);
    fjell_density = max(fjell_density, 0.0);

    // The height fog is a smooth function of height with no wisps in it, so
    // there is nothing in it to displace — what it can do is give way. The air
    // along the path is briefly thinner and what was shoved aside piles a
    // little along the flanks, which very nearly cancel over the wake: a
    // walker crossing a misty field parts it rather than draining it.
    //
    // The wake's falloff runs across the heading along its flanks and along
    // the heading through its middle, so one cross product separates the
    // trough from the heaps beside it. A volume's own shader answers movement
    // itself, with wisps to drag, and is left alone here.
    if (fjell_haze && pc.motion_response > 0.0 && fjell_motion.recency > 0.0) {
        float reach = 1.0 - smoothstep(fjell_motion.height, fjell_motion.height + MOTION_FADE_METRES,
                                       world_pos.y);
        float fjell_stir_here = pc.motion_response * fjell_motion.recency * reach;
        vec2 heading = length(fjell_motion.velocity) > 1e-4
            ? normalize(fjell_motion.velocity) : vec2(0.0);
        float flank = abs(heading.x * fjell_motion_falloff.direction.y
                        - heading.y * fjell_motion_falloff.direction.x);
        fjell_density *= max(1.0 - fjell_stir_here * (GLOBAL_MOTION_THIN - GLOBAL_MOTION_HEAP * flank),
                       0.0);
    }

    // Sources add: overlaps are thicker, and the order they arrive in cannot
    // matter. Colour and fjell_scattering are carried premultiplied by fjell_density,
    // so a warm bank reads warm where it is thick and fades to the fjell_haze's
    // colour as it thins.
    vec4 fjell_contribution = vec4(fjell_color * fjell_scattering * fjell_density, fjell_density);
    if (fjell_haze) {
        imageStore(fog_medium, coord, fjell_contribution);
    } else {
        imageStore(fog_medium, coord, imageLoad(fog_medium, coord) + fjell_contribution);
    }
}

#else

// ── Light variant: the accumulated fjell_medium, lit ──

void main() {
    ivec3 coord = ivec3(gl_GlobalInvocationID.xyz);
    if (any(greaterThanEqual(coord, ivec3(pc.volume_dims.xyz)))) return;

    FjellFroxel f = fjell_froxel(coord);
    vec3 world_pos = f.world_pos;
    vec3 ray_dir = f.ray_dir;

    vec4 fjell_medium = imageLoad(fog_medium, coord);
    float fjell_density_sum = fjell_medium.a;
    // Σ colour × fjell_scattering × fjell_density: what the fjell_medium here scatters per
    // unit of light, before the lights are known.
    vec3 fjell_scatter_sum = fjell_medium.rgb;

    // What the air here has actually been through, applied to everything
    // that makes fog: the field does not know which fog it is stirring, and
    // the fjell_haze is as much fog as a placed bank is.
    //
    // A share of what is here rather than an amount of its own, so a wake is
    // as strong as the fog it is carved in and cannot light up clear air above
    // a bank. Scaled well below one so that the air thins where it was pushed
    // aside and thickens where it piled up without either reaching a hole or a
    // wall — displaced air, not removed air.
    float fjell_stir = fog_density_stir(world_pos);
    if (fjell_stir != 0.0) {
        float scale = clamp(1.0 + fjell_stir * FOG_DENSITY_STRENGTH, FOG_DENSITY_FLOOR,
                            FOG_DENSITY_CEIL);
        fjell_density_sum *= scale;
        fjell_scatter_sum *= scale;
    }

    // Colours what the fjell_stir is doing to the fog that is here, red where the air
    // was shoved aside and blue where it piled up. It paints and adds nothing:
    // where there is no fog there is nothing to see, which is the point — this
    // shows the effect rather than the field behind it.
    //
    // Emitted rather than lit, so the colour reads the same at any hour and in
    // any weather instead of being one more thing the lighting scales.
    vec3 fjell_debug_emit = vec3(0.0);
    if (pc.density_field_debug > 0.0 && fjell_stir != 0.0) {
        float show = clamp(abs(fjell_stir) * 6.0 * pc.density_field_debug, 0.0, 1.0);
        fjell_debug_emit = (fjell_stir < 0.0 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 0.4, 1.0)) * show;
    }

    // Fade fjell_density to zero near max distance to prevent uniform grey wall.
    // Use view-space depth (not ray_dist) so the fjell_fade is uniform across the
    // screen — otherwise edge froxels fjell_fade earlier than center ones and the
    // volume's frustum silhouette becomes visible against the sky. Volumes
    // fjell_fade with it: one sitting at the far plane would otherwise punch a
    // sharp edge through the fjell_fade.
    float dist_ratio = f.linear_depth / pc.far_plane;
    float fjell_fade = 1.0 - smoothstep(0.7, 1.0, dist_ratio);
    float fjell_fog_density = fjell_density_sum * fjell_fade;

    // Early out for negligible fjell_density
    if (fjell_fog_density < 1e-6) {
        imageStore(scatter_volume, coord, vec4(0.0));
        return;
    }

    // Extinction controls fog opacity (how much light is blocked).
    // Scattering controls in-fjell_scattered light brightness independently,
    // so god rays can be bright without making the fog opaque.
    float fjell_extinction = fjell_fog_density * 0.1;
    // What light() sees as the fjell_medium's colour: the colour × fjell_scattering the
    // sources here average to, weighted by how much each contributed.
    vec3 fjell_fog_color = fjell_scatter_sum / max(fjell_density_sum, 1e-6);

    // Every light that reaches this froxel, fjell_scattered toward the eye by the
    // fog shader's light(). The sun first.
    vec3 sun_dir = ubo.light_direction.xyz;
    vec3 fjell_scattered = vec3(0.0);
    fjell_fog_light(FOG_LIGHT_SUN, sun_dir, ubo.light_color.xyz * ubo.light_direction.w,
                    dot(ray_dir, sun_dir), sample_shadow(world_pos),
                    world_pos, ray_dir, fjell_fog_density, fjell_fog_color, fjell_scattered);

    // Per-froxel rotation for PCF jitter (varies per froxel + frame for temporal smoothing)
    mat2 pcf_rot = vol_random_rotation(coord, pc.volume_dims.w);

    // Point light fjell_scattering
    uint point_count = ubo.tile_info.z;
    for (uint i = 0; i < point_count; ++i) {
        GpuPointLight pl = point_lights[i];
        vec3 to_light = pl.position_radius.xyz - world_pos;
        float dist2 = dot(to_light, to_light);
        float radius = pl.position_radius.w;
        if (dist2 > radius * radius) continue;

        float inv_dist = inversesqrt(max(dist2, 1e-8));
        float dist = dist2 * inv_dist;
        vec3 L = to_light * inv_dist;
        float atten = vol_attenuation(dist, radius);
        float pl_shadow = sample_point_shadow_vol(world_pos, pl, pcf_rot);

        fjell_fog_light(FOG_LIGHT_POINT, L,
                        pl.color_intensity.xyz * pl.color_intensity.w * pl.vol_params.x * atten,
                        dot(ray_dir, L), pl_shadow,
                        world_pos, ray_dir, fjell_fog_density, fjell_fog_color, fjell_scattered);
    }

    // Spot light fjell_scattering
    uint spot_count = ubo.tile_info.w;
    for (uint i = 0; i < spot_count; ++i) {
        GpuSpotLight sl = spot_lights[i];
        vec3 to_light = sl.position_radius.xyz - world_pos;
        float dist2 = dot(to_light, to_light);
        float radius = sl.position_radius.w;
        if (dist2 > radius * radius) continue;

        float inv_dist = inversesqrt(max(dist2, 1e-8));
        float dist = dist2 * inv_dist;
        vec3 L = to_light * inv_dist;
        // sl.direction_inner_cos.xyz is pre-normalized CPU-side
        vec3 spot_dir = sl.direction_inner_cos.xyz;
        float cos_theta = dot(-L, spot_dir);
        float spot_atten = smoothstep(sl.outer_cos, sl.direction_inner_cos.w, cos_theta);
        if (spot_atten <= 0.0) continue;

        float atten = vol_attenuation(dist, radius) * spot_atten;
        float sl_shadow = sample_spot_shadow_vol(world_pos, sl, pcf_rot);

        fjell_fog_light(FOG_LIGHT_SPOT, L,
                        sl.color_intensity.xyz * sl.color_intensity.w * sl.vol_params.x * atten,
                        dot(ray_dir, L), sl_shadow,
                        world_pos, ray_dir, fjell_fog_density, fjell_fog_color, fjell_scattered);
    }

    // Ambient in-scatter: the sky, arriving from everywhere at once. It reads
    // the same irradiance mip of the cubemap that lit surfaces sample, so
    // ambient_intensity means one thing across the engine and fog takes its
    // colour from the sky rather than from a flat value nothing else uses.
    // It reaches light() with no direction and no shadow; the default body
    // scatters it by the isotropic phase, which keeps it in proportion to the
    // directional terms.
    vec3 sky_ambient = textureLod(sky_cubemap, ray_dir, 5.0).rgb;
    fjell_fog_light(FOG_LIGHT_AMBIENT, vec3(0.0), sky_ambient * ubo.ambient_color.w, 0.0, 1.0,
                    world_pos, ray_dir, fjell_fog_density, fjell_fog_color, fjell_scattered);

    // Σ colour × fjell_scattering × fjell_density, faded with the fjell_density, times what
    // the lights scatter: each source's share of the light is its own colour
    // and brightness, and the sum is what the fjell_medium here sends to the eye.
    vec3 in_scattered = fjell_scatter_sum * fjell_fade * fjell_scattered;

    // The integrator divides what it is handed by fjell_extinction and scales it by
    // how much of the slice the fog absorbs — right for light fjell_scattered out of
    // a fjell_medium, wrong for a colour that is only here to be looked at. Undoing
    // exactly that leaves the debug colour at the strength it was written,
    // whether the fog it sits in is thick or barely there.
    if (fjell_debug_emit != vec3(0.0)) {
        float slice_far = slice_to_depth(min(f.froxel_uv.z + 1.0 / float(pc.volume_dims.z), 1.0));
        float thickness = max(slice_far - f.linear_depth, 1e-4);
        float absorbed = 1.0 - exp(-fjell_extinction * thickness);
        in_scattered += fjell_debug_emit * fjell_extinction / max(absorbed, 1e-7) * DEBUG_EMIT_GAIN;
    }

    imageStore(scatter_volume, coord, vec4(in_scattered, fjell_extinction));
}

#endif // FJELL_FOG_LIGHT
