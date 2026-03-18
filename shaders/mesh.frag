#version 460
#extension GL_EXT_nonuniform_qualifier : require

const float PI = 3.14159265359;

struct GpuMaterial {
    uint base_color_idx;
    uint normal_idx;
    uint metallic_roughness_idx;
    uint emissive_idx;
    vec4 base_color_factor;
    float metallic_factor;
    float roughness_factor;
    float has_normal_map;
    float padding;
};

layout(set = 0, binding = 0) uniform GlobalUbo {
    mat4 view;
    mat4 projection;
    vec4 light_direction;    // xyz = dir toward light, w = intensity
    vec4 light_color;        // xyz = color
    vec4 camera_position;    // xyz = eye pos
    vec4 ambient_color;      // xyz = color, w = intensity
    mat4 cascade_vp[3];      // light-space matrices per cascade
    vec4 cascade_splits;     // xyz = split distances
    uvec4 tile_info;         // x = tiles_x, y = tiles_y, z = point_count, w = spot_count
    mat4 inv_projection;
} ubo;

layout(set = 0, binding = 1) uniform sampler2DArrayShadow shadow_map;

struct GpuPointLight {
    vec4 position_radius;   // xyz = world pos, w = radius
    vec4 color_intensity;   // xyz = color, w = intensity
};

struct GpuSpotLight {
    vec4 position_radius;       // xyz = world pos, w = radius
    vec4 direction_inner_cos;   // xyz = dir, w = cos(inner_angle)
    vec4 color_intensity;       // xyz = color, w = intensity
    float outer_cos;
    float pad0;
    float pad1;
    float pad2;
};

layout(set = 0, binding = 2) readonly buffer LightBuffer {
    GpuPointLight point_lights[1024];
    GpuSpotLight spot_lights[1024];
};

const uint MAX_LIGHTS_PER_TILE = 256;

layout(set = 0, binding = 3) readonly buffer TileLightIndexBuffer {
    uint tile_data[]; // [tile_count counts][tile_count * MAX_LIGHTS_PER_TILE indices]
};

layout(set = 1, binding = 0) uniform sampler2D textures[];

layout(set = 1, binding = 1) readonly buffer MaterialBuffer {
    GpuMaterial materials[];
};

struct DrawData {
    mat4 model;
    uint material_id;
    uint meshlet_offset;
    uint meshlet_count;
    uint vertex_offset;
};

layout(set = 2, binding = 0) readonly buffer DrawDataBuffer {
    DrawData draws[];
};

layout(location = 0) in vec3 frag_color;
layout(location = 1) in vec2 frag_tex_coord;
layout(location = 2) in vec3 frag_world_pos;
layout(location = 3) in vec3 frag_world_normal;
layout(location = 4) in mat3 frag_TBN;
layout(location = 7) flat in uint frag_draw_index;

layout(location = 0) out vec4 out_color;

// GGX/Trowbridge-Reitz normal distribution
float distribution_GGX(float NdotH, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * denom * denom);
}

// Schlick-GGX geometry function
float geometry_schlick_GGX(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

// Smith's method combining both view and light directions
float geometry_smith(float NdotV, float NdotL, float roughness) {
    return geometry_schlick_GGX(NdotV, roughness) *
           geometry_schlick_GGX(NdotL, roughness);
}

// Fresnel-Schlick approximation
vec3 fresnel_schlick(float cos_theta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cos_theta, 0.0, 1.0), 5.0);
}

// Cascaded shadow map lookup
float shadow_factor(vec3 world_pos, float NdotL_for_bias) {
    // Find which cascade this fragment belongs to
    float view_depth = -(ubo.view * vec4(world_pos, 1.0)).z;
    int cascade = 2;
    for (int i = 0; i < 3; ++i) {
        if (view_depth < ubo.cascade_splits[i]) {
            cascade = i;
            break;
        }
    }

    // Project into light space
    vec4 light_pos = ubo.cascade_vp[cascade] * vec4(world_pos, 1.0);
    vec3 proj = light_pos.xyz / light_pos.w;

    // Remap XY from [-1,1] to [0,1]
    vec2 uv = proj.xy * 0.5 + 0.5;

    // Out of shadow map bounds = lit
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 ||
        proj.z < 0.0 || proj.z > 1.0) {
        return 1.0;
    }

    // Slope-scaled bias: steeper surfaces get more bias to avoid acne
    // while keeping contact shadows tight
    float bias = max(0.001 * (1.0 - NdotL_for_bias), 0.0002);
    float compare_depth = proj.z - bias;

    // PCF 3x3: sample shadow map with offsets for softer edges
    float shadow = 0.0;
    vec2 texel_size = vec2(1.0) / vec2(2048.0);
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            vec2 offset = vec2(float(x), float(y)) * texel_size;
            shadow += texture(shadow_map, vec4(uv + offset, float(cascade), compare_depth));
        }
    }
    shadow /= 9.0;
    return shadow;
}

void main() {
    uint material_id = draws[frag_draw_index].material_id;
    GpuMaterial mat = materials[material_id];

    // Base color
    vec4 base_color = texture(textures[nonuniformEXT(mat.base_color_idx)], frag_tex_coord) *
                      mat.base_color_factor *
                      vec4(frag_color, 1.0);
    vec3 albedo = base_color.rgb;

    // Metallic / roughness
    vec4 mr_sample = texture(textures[nonuniformEXT(mat.metallic_roughness_idx)], frag_tex_coord);
    float metallic = mr_sample.b * mat.metallic_factor;
    float roughness = mr_sample.g * mat.roughness_factor;
    roughness = clamp(roughness, 0.04, 1.0);

    // Normal
    float normal_len = length(frag_world_normal);
    if (normal_len < 0.001) {
        // No normals — render unlit (backward compatibility)
        out_color = base_color;
        return;
    }

    vec3 N;
    if (mat.has_normal_map > 0.5) {
        vec3 tangent_normal = texture(textures[nonuniformEXT(mat.normal_idx)], frag_tex_coord).rgb * 2.0 - 1.0;
        N = normalize(frag_TBN * tangent_normal);
    } else {
        N = normalize(frag_world_normal);
    }

    // Emissive
    vec3 emissive = texture(textures[nonuniformEXT(mat.emissive_idx)], frag_tex_coord).rgb;

    // Lighting vectors
    vec3 V = normalize(ubo.camera_position.xyz - frag_world_pos);
    vec3 L = normalize(ubo.light_direction.xyz);
    vec3 H = normalize(V + L);
    float light_intensity = ubo.light_direction.w;

    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.001);
    float NdotH = max(dot(N, H), 0.0);
    float HdotV = max(dot(H, V), 0.0);

    // F0: reflectance at normal incidence
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    // Cook-Torrance BRDF
    float D = distribution_GGX(NdotH, roughness);
    float G = geometry_smith(NdotV, NdotL, roughness);
    vec3  F = fresnel_schlick(HdotV, F0);

    vec3 specular = (D * G * F) / (4.0 * NdotV * NdotL + 0.0001);

    // Diffuse (energy conserving)
    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);
    vec3 diffuse = kD * albedo / PI;

    float shadow = shadow_factor(frag_world_pos, NdotL);

    // Direct lighting (directional)
    vec3 radiance = ubo.light_color.xyz * light_intensity;
    vec3 Lo = (diffuse + specular) * radiance * NdotL * shadow;

    // ── Forward+ tile-based point/spot light accumulation ────────────
    uint tiles_x = ubo.tile_info.x;
    uint total_tiles = ubo.tile_info.x * ubo.tile_info.y;
    uvec2 tile_coord = uvec2(gl_FragCoord.xy) / 16;
    uint tile_index = tile_coord.y * tiles_x + tile_coord.x;

    uint light_count = tile_data[tile_index];

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
            float cos_theta = dot(normalize(light_pos - frag_world_pos), -spot_dir);
            spot_atten = smoothstep(sl.outer_cos, sl.direction_inner_cos.w, cos_theta);
        }

        vec3 Lp = light_pos - frag_world_pos;
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

    // Ambient
    vec3 ambient = ubo.ambient_color.xyz * ubo.ambient_color.w * albedo;

    vec3 color = ambient + Lo + emissive;

    // Output linear HDR — tone mapping happens in a separate pass
    out_color = vec4(color, base_color.a);
}
