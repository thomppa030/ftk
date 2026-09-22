#version 460
#extension GL_EXT_nonuniform_qualifier : require

// FJSL water template. The water mesh shader generates the surface from the
// node's .fjwater wave field; this fragment stage shades it.
//
// One hook. fragment() sees the surface the waves made — WORLD_POS,
// WORLD_NORMAL, and the four the wave field alone can answer: CREST, how
// pinched the surface is here, which is where whitecaps belong, WAVE_HEIGHT,
// how far this point was lifted above the flat sea, WAVE_DIR, which way
// the sea travels, which is what tells a leading face from a trailing one,
// and WAKE, the foam on the crests of whatever has moved through it. It
// writes ALBEDO, ALPHA and the rest exactly as a spatial shader does. light()
// works as for spatial. There is no vertex(): a sea is shaped by its .fjwater, not
// by its material, and saying otherwise is a parse error rather than code
// that never runs.
//
// BELOW is 1 when the camera is under this water and the eye sees the
// underside of this pixel of its surface. Then
// WORLD_NORMAL points down toward the eye, and a material shades the
// water/air boundary from inside: a mirror past Snell's window, the sky
// refracted through it. The surface is drawn without culling, so a material
// that ignores BELOW simply shows its topside from underneath.
//
// SCREEN_UV_TOWARD(dir) is where a direction from the eye lands on the screen,
// for sampling what lies that way — the sky through a refracted ray — from
// the screen copy: xy the uv, z 1 when it is on screen and 0 when the
// direction leaves the frame and only sky_cubemap can answer.
//
// Two depths, and they are not interchangeable. WATER_DEPTH is the path the
// light took through the water, which is what absorption reads. WATER_DEPTH_
// VERTICAL is how deep the water is straight down, which is what a shore
// reads. The first changes when the camera moves and the second does not.
// From below, WATER_DEPTH is the water between the eye and the surface, and
// WATER_DEPTH_VERTICAL is how far the surface stands above the eye: the two
// things absorption and darkness-with-depth want from that side.
//
// SDF_DISTANCE is how far the surface is from solid geometry, for foam that
// gathers against rock. Static geometry only, and large where there is none.

#include "fjell_common.glsl"
#include "fjell_global_ubo.glsl"
#include "fjell_gpu_types.glsl"
#include "fjell_renderer_constants.glsl"

FJELL_DECLARE_GLOBAL_UBO(0, 0, ubo);

layout(set = 0, binding = 4) uniform sampler2D depth_texture;
layout(set = 0, binding = 8) uniform sampler2D screen_color_texture;
layout(set = 0, binding = 9) uniform sampler3D scene_sdf;

layout(set = 1, binding = 0) uniform sampler2D textures[];

layout(set = 1, binding = 1) readonly buffer MaterialBuffer {
    GpuMaterial materials[];
};

layout(set = 1, binding = 2) readonly buffer ParamsBuffer {
    float custom_params[];
};

// Binding 0 is the wave field, read by the mesh stage only. The wakes are
// read here as well: the ones too narrow for the mesh to draw tilt the normal
// per pixel, and the foam on every wake's crests is what WAKE reports.
#include "fjell_water_wake_record.glsl"
layout(std430, set = 2, binding = 5) readonly buffer WakeBuffer {
    FjellWakeSource sources[];
} wakes;
#define FJELL_WATER_WAKES wakes.sources
#include "fjell_water_wake.glsl"

// Mirrors WaterPushConstants in water_pass.hpp and the block in water.task
// and water.mesh.
layout(push_constant) uniform PushConstants {
    vec2  world_origin;
    float world_size;
    float surface_height;
    uint  patches_per_side;
    uint  patch_quads;
    uint  water_material_id;
    float delta_time;
    vec2  wave_direction;
    uint  infinite;
    float simulated_patch_size;
    vec3  sdf_volume_min;
    uint  wake_first;
    vec3  sdf_volume_max;
    uint  wake_count;
    uint  camera_below;
    uint  line_active;
    uint  _pad_b;
    uint  _pad_c;
} push;

layout(location = 0) in vec3 frag_color;
layout(location = 1) in vec2 frag_tex_coord;
layout(location = 2) in vec3 frag_world_pos;
layout(location = 3) in vec3 frag_world_normal;
layout(location = 4) in vec4 frag_clip_pos;
layout(location = 5) in vec4 frag_prev_clip_pos;
layout(location = 6) in float frag_wave_lift;
layout(location = 7) in float frag_crest;

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec2 out_metallic_roughness;
layout(location = 2) out vec2 out_normal;
layout(location = 3) out vec2 out_velocity;

#include "fjell_surface_lighting.glsl"

// Linear view-space distance from a depth buffer sample.
float linearize_depth(float d) {
    float n = ubo.projection[3][2] / ubo.projection[2][2];
    float f = ubo.projection[3][2] / (ubo.projection[2][2] + 1.0);
    return n * f / (f - d * (f - n));
}

vec3 reconstruct_world_pos(vec2 screen_uv, float raw_depth) {
    vec4 ndc = vec4(screen_uv * 2.0 - 1.0, raw_depth, 1.0);
    vec4 world = ubo.inv_view_proj * ndc;
    return world.xyz / world.w;
}

// Where a direction from the eye lands on the screen: the uv, and whether it
// is on the screen at all. Taken from the eye rather than the surface point
// because what a material wants that way is the sky, which is at infinity
// and cares only about the direction.
vec3 fjell_screen_uv_toward(vec3 dir) {
    vec4 clip = ubo.unjittered_vp * vec4(ubo.camera_position.xyz + dir * 1000.0, 1.0);
    if (clip.w <= 0.0) return vec3(0.0);
    vec2 uv = clip.xy / clip.w * 0.5 + 0.5;
    float inside = all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0))) ? 1.0 : 0.0;
    return vec3(uv, inside);
}
#define SCREEN_UV_TOWARD(dir) fjell_screen_uv_toward(dir)

// ── Custom texture + param defines (generated by FJSL parser) ──
/*FJSL_CUSTOM_TEXTURES*/

// ── User-authored file-scope helpers / constants (generated by FJSL parser) ──
/*FJSL_USER_GLOBALS*/

// ── light() hook ── see fjell_spatial_template.glsl for the contract.
void fjell_light(
    vec3 LIGHT_DIR, vec3 LIGHT_COLOR, float LIGHT_INTENSITY, float NdotL,
    vec3 NORMAL, vec3 VIEW_DIR, vec3 ALBEDO, float METALLIC, float ROUGHNESS, vec3 F0,
    inout vec3 DIFFUSE_LIGHT, inout vec3 SPECULAR_LIGHT)
{
/*FJSL_LIGHT_USER_CODE*/
}

void main() {
    GpuMaterial mat = materials[push.water_material_id];

    // ── Built-in inputs ──
    vec3 WORLD_POS = frag_world_pos;
    vec3 WORLD_NORMAL = normalize(frag_world_normal);

    // The foam a wake has put here, 0..1: the strongest ring crest under
    // this point as the share of its birth height it has left. It rides the
    // V arms and the bow arcs and dies with the rings; what foam that is
    // stays the material's reading of it.
    //
    // The rings of anything too narrow for the mesh to draw — a swimmer's —
    // tilt the normal here instead, faded by how many metres a pixel covers
    // so a ripple narrower than a few pixels does not shimmer.
    float WAKE = 0.0;
    if (push.wake_count > 0u) {
        float footprint = length(fwidth(WORLD_POS.xz));
        FjellWakeShade wake = fjell_water_wake_shade(push.wake_first, push.wake_count,
                                                     WORLD_POS.xz, ubo.time, footprint);
        WORLD_NORMAL = normalize(vec3(WORLD_NORMAL.x - wake.slope.x * WORLD_NORMAL.y,
                                      WORLD_NORMAL.y,
                                      WORLD_NORMAL.z - wake.slope.y * WORLD_NORMAL.y));
        WAKE = wake.foam;
    }
    vec2 UV = frag_world_pos.xz;             // metres, so tiling is authored in metres
    // 0..1 across a placed body of water. An infinite sea has no edges to
    // measure from, so there it is world metres and a shader keying off it
    // has to be told which kind of water it is drawing.
    vec2 WATER_UV = frag_tex_coord;
    vec3 VIEW_DIR = normalize(ubo.camera_position.xyz - frag_world_pos);
    float TIME = ubo.time;

    // Which side of the surface the eye is on. Only asked when the camera is
    // under this water at all, because a folded crest turns its own normal
    // over and would read as an underside from the beach; under the water it
    // is decided per pixel, since an eye just under a crest sees the next
    // trough from above. On the underside the normal the eye sees points at
    // it, so the material's Fresnel and the lighting face the right way.
    float BELOW = push.camera_below != 0u && dot(VIEW_DIR, WORLD_NORMAL) < 0.0 ? 1.0 : 0.0;
    if (BELOW > 0.5) WORLD_NORMAL = -WORLD_NORMAL;
    #define SCREEN_TEXTURE screen_color_texture
    vec2 SCREEN_UV = gl_FragCoord.xy / vec2(textureSize(screen_color_texture, 0));
    #define DEPTH_TEXTURE depth_texture
    #define SDF_TEXTURE scene_sdf
    // The sun, as the spatial template offers it. Water reaches for it more
    // than most surfaces do: its specular is a sun glint, and the light that
    // carries through the body of a wave is strongest looking into it.
    vec3 LIGHT_DIR = normalize(ubo.light_direction.xyz);
    vec3 LIGHT_COLOR = ubo.light_color.xyz;
    float LIGHT_INTENSITY = ubo.light_direction.w;

    // How pinched the surface is here: 0 on flat water, rising toward 1 where
    // the trains have bunched to a point. Whitecaps live at the top of this.
    //
    // Interpolated from the vertices. The grid is eleven vertices to a patch,
    // so a crest sharper than the spacing arrives as a linear ramp between the
    // vertices that bracket it — foam drawn from it carries a little of the
    // triangulation with it. Solving the field again per pixel removes that
    // and was measured at 14% of the water pass, which is not a trade a
    // stylized sea needs: hard-edged foam hides the difference.
    float CREST = frag_crest;

    // How far the waves lifted this point above the flat sea. Negative in a
    // trough.
    float WAVE_HEIGHT = frag_wave_lift;

    // Which way this sea travels, world XZ, amplitude-weighted across its
    // trains. Constant over the surface, so it arrives in the push block
    // rather than as a varying.
    //
    // What it is for: foam gathers on the face of a wave that is advancing,
    // not evenly around a crest. Compare it against the surface's own
    // horizontal tilt — dot(normalize(NORMAL.xz), WAVE_DIR) — and the sign
    // separates the leading face from the one sliding away behind it.
    //
    // Zero when the trains cancel, which is a sea with no direction of travel
    // rather than an error. Anything reading it should stay sensible there,
    // which is what WAVE_DIR_STRENGTH is for.
    vec2 WAVE_DIR = push.wave_direction;
    // 0 when the sea has no net direction, 1 when its trains agree. Fade any
    // directional foam term out by this and an opposed sea gets the even
    // treatment that is correct for it.
    float WAVE_DIR_STRENGTH = length(push.wave_direction);

    // Tangent frame, U along world +X and V along world +Z, so a normal map
    // on water lines up with the metres UV is measured in.
    vec3 T = normalize(vec3(1.0, 0.0, 0.0) - WORLD_NORMAL * WORLD_NORMAL.x);
    vec3 B = cross(T, WORLD_NORMAL);
    mat3 TBN = mat3(T, B, WORLD_NORMAL);

    // ── Depth of the water under this pixel ──
    //
    // Two of them, because "how deep is the water" has two different answers
    // and using one where the other belongs is not a subtle error.
    float raw_depth = texture(depth_texture, SCREEN_UV).r;
    float scene_depth = linearize_depth(raw_depth);
    float surface_depth = linearize_depth(gl_FragCoord.z);

    // Where the bed under this pixel is, for anything projected onto it.
    vec3 FLOOR_POS = reconstruct_world_pos(SCREEN_UV, raw_depth);

    // How far light travels through the water to reach the eye: the length of
    // the view ray between this surface and whatever is behind it. This is
    // what Beer's law wants — absorption is a property of the path, so a
    // shallow glance through a puddle tints more than a look straight down.
    //
    // Corrected for the wave, because the raw measurement is dominated by the
    // surface's own height once waves are metres tall: a crest stands higher,
    // so its column is longer, so it tints toward the deep colour while the
    // trough beside it stays clear — dark bands rolling through the sea,
    // which is backwards. A crest is the thin part of the water.
    //
    // The lift is vertical and the column is measured along the view ray, so
    // it converts before the two can meet. At a shallow angle a metre of lift
    // is many metres of ray; straight down it is one. Getting this wrong does
    // not look like an error, it looks like nothing happening at all.
    //
    // From below the water is in front of the surface, not behind it: the
    // path is the eye's own distance to the surface, and what the depth
    // buffer holds behind the surface is air.
    float ray_descent = max(abs(VIEW_DIR.y), 0.15);
    float WATER_DEPTH = BELOW > 0.5
        ? surface_depth
        : max(scene_depth - surface_depth - WAVE_HEIGHT / ray_descent, 0.0);

    // How deep the water is here, measured straight down. A property of the
    // place rather than of where the camera is, which is what anything asking
    // "how close is land" needs: shore foam, a shallows colour ramp, a wave
    // that should die as it runs up a beach.
    //
    // WATER_DEPTH cannot answer that. It is a ray length, so it grows as the
    // camera lowers — the same spot reads 0.30 m from overhead and saturates
    // its clamp at a standing eye height, which put the shore foam band
    // everywhere when looked down at and nowhere when looked across.
    //
    // Large, not zero, where there is nothing behind the water: with no bed
    // to hit, the depth buffer reads the far plane and FLOOR_POS lands
    // hundreds of metres down the view ray. That is the right sign — open
    // water is deep water — and a shore term keying off it is asking for a
    // small depth, so it simply never triggers out at sea. Do not read it as
    // a real distance to a seabed that is not there.
    //
    // From below there is no bed behind the surface to measure to, and the
    // question changes: how far the surface stands above the eye is what
    // decides how much of the sky's light reaches down here.
    float WATER_DEPTH_VERTICAL = BELOW > 0.5
        ? max(WORLD_POS.y - ubo.camera_position.y, 0.0)
        : max(WORLD_POS.y - FLOOR_POS.y, 0.0);

    // How far this point is from solid geometry, in metres, from the scene
    // SDF. What a foam collar reads: water gathers where it meets rock, and
    // the distance to the rock is the mask.
    //
    // The SDF is baked and sees static geometry only, so this answers for a
    // cliff and not for a boat. It is also clamped a few metres out by the
    // bake, so treat a large value as "nothing near" rather than as a real
    // measurement — which is what SHORE_DISTANCE below is shaped for.
    //
    // Far away when there is no SDF at all: an empty volume has min == max,
    // and dividing by that extent would be a divide by zero, so the degenerate
    // case is answered before the sample rather than after it.
    float SDF_DISTANCE = 1000.0;
    {
        vec3 extent = push.sdf_volume_max - push.sdf_volume_min;
        if (all(greaterThan(extent, vec3(1e-4)))) {
            vec3 uvw = (WORLD_POS - push.sdf_volume_min) / extent;
            if (all(greaterThanEqual(uvw, vec3(0.0))) && all(lessThanEqual(uvw, vec3(1.0)))) {
                SDF_DISTANCE = texture(scene_sdf, uvw).r;
            }
        }
    }

    // ── Surface defaults ──
    // Water is not a PBR surface until fragment() makes it one. These are a
    // still, clear, dielectric sheet: a shader that writes nothing gets flat
    // water rather than a black hole.
    vec3 ALBEDO = vec3(0.02, 0.05, 0.08);
    vec3 NORMAL = WORLD_NORMAL;
    float METALLIC = 0.0;
    float ROUGHNESS = 0.05;
    vec3 EMISSION = vec3(0.0);
    float AO = 1.0;
    float ALPHA = 1.0;

    // ── Custom param reads (generated by FJSL parser) ──
/*FJSL_CUSTOM_PARAMS*/

    // ── User fragment code ──
    {
/*FJSL_USER_CODE*/
    }

    // Specular antialiasing: widen roughness where normals vary across the
    // pixel. Water needs this more than most surfaces — a sea of sharp
    // crests under a low sun is the worst case for specular aliasing.
    ROUGHNESS = specular_aa_roughness(NORMAL, WORLD_POS, max(ROUGHNESS, 0.089));

    // ── PBR lighting ──
    fjell_shade_surface(NORMAL, VIEW_DIR, WORLD_POS, frag_world_normal,
                        ALBEDO, ALPHA, METALLIC, ROUGHNESS, EMISSION, AO,
                        frag_clip_pos, frag_prev_clip_pos);
    /*FJSL_LIGHTING_END*/

    // ── Debug: where the simulated sea's texture repeats ──
    // A checker per tile and a bright line along every tile edge, so a seam
    // on the water can be seen against where the texture wraps. Drawn from
    // the world position rather than the rest position the texture is sampled
    // at, so the lines sit up to the horizontal displacement off the true
    // edge: near enough to tell whether a seam runs along it.
    if (ubo.debug_light_channel == DEBUG_LIGHT_CHANNEL_WATER_TILES) {
        float tile_size = push.simulated_patch_size;
        if (tile_size <= 0.0) {
            out_color = vec4(0.4, 0.0, 0.4, 1.0);   // no simulated sea on this water
        } else {
            vec2 tile = WORLD_POS.xz / tile_size;
            vec2 in_tile = fract(tile);
            // Metres to the nearest tile edge, and a line about two pixels
            // wide across it whatever the distance.
            float to_edge = min(min(in_tile.x, 1.0 - in_tile.x), min(in_tile.y, 1.0 - in_tile.y)) * tile_size;
            float line_width = 2.0 * fwidth(to_edge);
            float edge = 1.0 - smoothstep(0.0, line_width, to_edge);
            vec2 which = floor(tile);
            float checker = mod(which.x + which.y, 2.0);
            vec3 tint = mix(vec3(0.05, 0.25, 0.45), vec3(0.10, 0.45, 0.35), checker);
            // Lit by the geometry alone, so the surface's shape still reads.
            float shade = 0.5 + 0.5 * max(NORMAL.y, 0.0);
            out_color = vec4(mix(tint * shade, vec3(1.0, 0.2, 0.9), edge), 1.0);
        }
    }
}
