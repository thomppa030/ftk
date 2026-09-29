#ifndef FJELL_LIGHTING_GLSL
#define FJELL_LIGHTING_GLSL

#include "fjell_renderer_constants.glsl"

// ── Local light shadow sampling ────────────────────────────────────────
// Requires: local_shadow_atlas (sampler2DShadow, binding 11)
//           POISSON_DISK[] and random_rotation() from fjell_shadow.glsl

const float POINT_SHADOW_NEAR = 0.1;          // must match LocalShadowPass

float sample_spot_shadow(vec3 world_pos, GpuSpotLight light) {
    if (light.shadow_tile_size == 0) return 1.0;

    vec4 light_clip = light.shadow_vp * vec4(world_pos, 1.0);
    vec3 proj = light_clip.xyz / light_clip.w;

    if (proj.z < 0.0 || proj.z > 1.0) return 1.0;

    vec2 suv = proj.xy * 0.5 + 0.5;
    if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) return 1.0;

    float tile_size = float(light.shadow_tile_size);
    float bias = 0.003;
    float compare = proj.z - bias;

    // PCF: 9-tap rotated Poisson disk
    float texel = 1.0 / LOCAL_SHADOW_ATLAS_SIZE_F;
    float spread = 2.5 * texel * tile_size / 512.0; // scale filter with tile resolution
    mat2 rot = random_rotation(gl_FragCoord.xy);

    float shadow = 0.0;
    for (int i = 0; i < 9; ++i) {
        vec2 offset = rot * POISSON_DISK[i] * spread;
        vec2 sample_uv = suv + offset / tile_size * LOCAL_SHADOW_ATLAS_SIZE_F;
        // Clamp to tile bounds to avoid bleeding
        sample_uv = clamp(sample_uv, vec2(0.001), vec2(0.999));
        vec2 atlas_uv = (vec2(float(light.shadow_atlas_x), float(light.shadow_atlas_y))
                        + sample_uv * tile_size) / LOCAL_SHADOW_ATLAS_SIZE_F;
        shadow += texture(local_shadow_atlas, vec3(atlas_uv, compare));
    }
    return shadow / 9.0;
}

// Point light: compute face + atlas UV from a direction vector
void point_shadow_face(vec3 dir, GpuPointLight light,
                       out int face, out vec2 face_uv) {
    vec3 abs_dir = abs(dir);
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
}

float sample_point_shadow(vec3 world_pos, GpuPointLight light) {
    if (light.shadow_info.w == 0) return 1.0;

    vec3 to_frag = world_pos - light.position_radius.xyz;
    float dist2 = dot(to_frag, to_frag);
    float inv_dist = inversesqrt(max(dist2, 1e-8));
    float dist = dist2 * inv_dist;
    vec3 dir = to_frag * inv_dist;

    int face;
    vec2 face_uv;
    point_shadow_face(dir, light, face, face_uv);

    int tile_size = light.shadow_info.z;
    int face_col = face % 2;
    int face_row = face / 2;
    vec2 tile_origin = vec2(float(light.shadow_info.x + face_col * tile_size),
                            float(light.shadow_info.y + face_row * tile_size));

    // Convert linear distance to perspective depth (Vulkan ZO)
    float far = light.position_radius.w;
    float z_buf = far * (dist - POINT_SHADOW_NEAR) / (dist * (far - POINT_SHADOW_NEAR));
    float bias = 0.005;
    float compare = z_buf - bias;

    // PCF: 9-tap rotated Poisson disk in face UV space
    float texel = 1.0 / float(tile_size);
    float spread = 2.5 * texel;
    mat2 rot = random_rotation(gl_FragCoord.xy);

    float shadow = 0.0;
    for (int i = 0; i < 9; ++i) {
        vec2 offset = rot * POISSON_DISK[i] * spread;
        vec2 uv = clamp(face_uv + offset, vec2(0.001), vec2(0.999));
        vec2 atlas_uv = (tile_origin + uv * float(tile_size)) / LOCAL_SHADOW_ATLAS_SIZE_F;
        shadow += texture(local_shadow_atlas, vec3(atlas_uv, compare));
    }
    return shadow / 9.0;
}

// Forward+ tile-based point/spot light accumulation.
// Requires: ubo, point_lights[], spot_lights[], tile_data[],
//           MAX_LIGHTS_PER_TILE, distribution_GGX, geometry_smith, fresnel_schlick, PI
vec3 accumulate_tile_lights(vec3 world_pos, vec3 N, vec3 V, float NdotV,
                            vec3 albedo, float metallic, float roughness, vec3 F0) {
    uint tiles_x = ubo.tile_info.x;
    uint total_tiles = ubo.tile_info.x * ubo.tile_info.y;
    uvec2 tile_coord = uvec2(gl_FragCoord.xy) / 16;
    uint tile_index = tile_coord.y * tiles_x + tile_coord.x;

    uint light_count = tile_data[tile_index];
    vec3 Lo = vec3(0.0);

    for (uint i = 0; i < light_count; i++) {
        uint light_idx = tile_data[total_tiles + tile_index * MAX_LIGHTS_PER_TILE + i];

        vec3 light_pos;
        float light_radius;
        vec3 light_col;
        float light_int;
        float spot_atten = 1.0;

        if (light_idx < 1024) {
            // Point light
            GpuPointLight pl = point_lights[light_idx];
            light_pos = pl.position_radius.xyz;
            light_radius = pl.position_radius.w;
            light_col = pl.color_intensity.xyz;
            light_int = pl.color_intensity.w;
        } else {
            // Spot light
            uint si = light_idx - 1024;
            GpuSpotLight sl = spot_lights[si];
            light_pos = sl.position_radius.xyz;
            light_radius = sl.position_radius.w;
            light_col = sl.color_intensity.xyz;
            light_int = sl.color_intensity.w;

            // Angular falloff (sl.direction_inner_cos.xyz is pre-normalized CPU-side)
            vec3 spot_dir = sl.direction_inner_cos.xyz;
            float cos_theta = dot(normalize(light_pos - world_pos), -spot_dir);
            spot_atten = smoothstep(sl.outer_cos, sl.direction_inner_cos.w, cos_theta);
        }

        vec3 Lp = light_pos - world_pos;
        float dist2 = dot(Lp, Lp);

        // Skip if beyond radius (compare squared, no sqrt)
        if (dist2 > light_radius * light_radius) continue;

        float inv_dist = inversesqrt(max(dist2, 1e-8));
        vec3 Ldir = Lp * inv_dist;
        vec3 Hp = normalize(V + Ldir);

        float NdotLp = max(dot(N, Ldir), 0.0);
        float NdotHp = max(dot(N, Hp), 0.0);
        float HdotVp = max(dot(Hp, V), 0.0);

        float Dp = distribution_GGX(NdotHp, roughness);
        float Gp = geometry_smith(NdotV, NdotLp, roughness);
        vec3  Fp = fresnel_schlick(HdotVp, F0);

        vec3 spec_p = (Dp * Gp * Fp) / (4.0 * NdotV * NdotLp + 0.0001);
        vec3 kDp = (vec3(1.0) - Fp) * (1.0 - metallic);
        vec3 diff_p = kDp * albedo / PI;

        // Inverse-square attenuation with radius cutoff
        float atten = 1.0 / (dist2 + 0.0001) * spot_atten;

        // Shadow for local lights
        float local_shadow = 1.0;
        if (light_idx < 1024) {
            local_shadow = sample_point_shadow(world_pos, point_lights[light_idx]);
        } else {
            uint si = light_idx - 1024;
            local_shadow = sample_spot_shadow(world_pos, spot_lights[si]);
        }

        vec3 rad_p = light_col * light_int;
        Lo += (diff_p + spec_p) * rad_p * NdotLp * atten * local_shadow;
    }

    return Lo;
}

// ── Second directional body ────────────────────────────────────────────
// The environment's other distant light (the moon by day, the setting sun
// by night), shaded like the primary but without cascaded shadows: the
// caller applies whatever occlusion it has. Zero intensity costs nothing.
// Requires fjell_pbr.glsl.
vec3 fjell_secondary_directional(vec3 N, vec3 V, float NdotV,
                                 vec3 albedo, float metallic, float roughness, vec3 F0,
                                 vec4 dir_intensity, vec4 color) {
    if (dir_intensity.w <= 0.0) return vec3(0.0);
    vec3 L = normalize(dir_intensity.xyz);
    float NdotL = max(dot(N, L), 0.0);
    if (NdotL <= 0.0) return vec3(0.0);
    vec3 H = normalize(V + L);
    float NdotH = max(dot(N, H), 0.0);
    float HdotV = max(dot(H, V), 0.0);
    float D = distribution_GGX(NdotH, roughness);
    float G = geometry_smith(NdotV, NdotL, roughness);
    vec3  F = fresnel_schlick(HdotV, F0);
    vec3 specular = (D * G * F) / (4.0 * NdotV * NdotL + 0.0001);
    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);
    vec3 diffuse = kD * albedo / PI;
    return (diffuse + specular) * color.rgb * dir_intensity.w * NdotL;
}

// ── Ambient diffuse ────────────────────────────────────────────────────
// The diffuse light reaching a surface from all around it, sky and bounce
// together, before the surface's albedo: DDGI's probes where their grid
// covers the pixel, the sky's unshadowed irradiance where it does not.
// `gi` is gi_indirect at the pixel times ubo.ddgi_weight: the probes'
// irradiance scaled by how much of the pixel their grid covers, with that
// cover in alpha. `sky_irradiance` is the sky cubemap's diffuse irradiance
// around the normal, and `sky_intensity` how brightly the sky lights the
// world (ubo.ambient_color.w), at which the probes already saw it.
vec3 fjell_ambient_irradiance(vec4 gi, vec3 sky_irradiance, float sky_intensity) {
    return gi.rgb + sky_irradiance * (sky_intensity * (1.0 - gi.a));
}

#endif // FJELL_LIGHTING_GLSL
