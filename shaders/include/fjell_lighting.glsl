#ifndef FJELL_LIGHTING_GLSL
#define FJELL_LIGHTING_GLSL

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

            // Angular falloff
            vec3 spot_dir = normalize(sl.direction_inner_cos.xyz);
            float cos_theta = dot(normalize(light_pos - world_pos), -spot_dir);
            spot_atten = smoothstep(sl.outer_cos, sl.direction_inner_cos.w, cos_theta);
        }

        vec3 Lp = light_pos - world_pos;
        float dist2 = dot(Lp, Lp);
        float dist = sqrt(dist2);

        // Skip if beyond radius
        if (dist > light_radius) continue;

        vec3 Ldir = Lp / dist;
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

        vec3 rad_p = light_col * light_int;
        Lo += (diff_p + spec_p) * rad_p * NdotLp * atten;
    }

    return Lo;
}

#endif
