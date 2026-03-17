#version 450

const float PI = 3.14159265359;

layout(set = 0, binding = 0) uniform GlobalUbo {
    mat4 view;
    mat4 projection;
    vec4 light_direction;    // xyz = dir toward light, w = intensity
    vec4 light_color;        // xyz = color
    vec4 camera_position;    // xyz = eye pos
    vec4 ambient_color;      // xyz = color, w = intensity
    mat4 cascade_vp[3];      // light-space matrices per cascade
    vec4 cascade_splits;     // xyz = split distances
} ubo;

layout(set = 0, binding = 1) uniform sampler2DArrayShadow shadow_map;

layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 base_color_factor;
    float metallic_factor;
    float roughness_factor;
    float has_normal_map;
    float padding;
} push;

layout(set = 1, binding = 0) uniform sampler2D base_color_map;
layout(set = 1, binding = 1) uniform sampler2D normal_map;
layout(set = 1, binding = 2) uniform sampler2D metallic_roughness_map;
layout(set = 1, binding = 3) uniform sampler2D emissive_map;

layout(location = 0) in vec3 frag_color;
layout(location = 1) in vec2 frag_tex_coord;
layout(location = 2) in vec3 frag_world_pos;
layout(location = 3) in vec3 frag_world_normal;
layout(location = 4) in mat3 frag_TBN;

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
    // Base color
    vec4 base_color = texture(base_color_map, frag_tex_coord) *
                      push.base_color_factor *
                      vec4(frag_color, 1.0);
    vec3 albedo = base_color.rgb;

    // Metallic / roughness
    vec4 mr_sample = texture(metallic_roughness_map, frag_tex_coord);
    float metallic = mr_sample.b * push.metallic_factor;
    float roughness = mr_sample.g * push.roughness_factor;
    roughness = clamp(roughness, 0.04, 1.0);

    // Normal
    float normal_len = length(frag_world_normal);
    if (normal_len < 0.001) {
        // No normals — render unlit (backward compatibility)
        out_color = base_color;
        return;
    }

    vec3 N;
    if (push.has_normal_map > 0.5) {
        vec3 tangent_normal = texture(normal_map, frag_tex_coord).rgb * 2.0 - 1.0;
        N = normalize(frag_TBN * tangent_normal);
    } else {
        N = normalize(frag_world_normal);
    }

    // Emissive
    vec3 emissive = texture(emissive_map, frag_tex_coord).rgb;

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

    // Direct lighting
    vec3 radiance = ubo.light_color.xyz * light_intensity;
    vec3 Lo = (diffuse + specular) * radiance * NdotL * shadow;

    // Ambient
    vec3 ambient = ubo.ambient_color.xyz * ubo.ambient_color.w * albedo;

    vec3 color = ambient + Lo + emissive;

    // Output linear HDR — tone mapping happens in a separate pass
    out_color = vec4(color, base_color.a);
}
