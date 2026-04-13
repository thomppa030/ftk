#version 460
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 0) uniform GlobalUbo {
    mat4 view;
    mat4 projection;
    vec4 light_direction;
    vec4 light_color;
    vec4 camera_position;
    vec4 ambient_color;
    mat4 cascade_vp[3];
    vec4 cascade_splits;
    uvec4 tile_info;
    mat4 inv_projection;
    mat4 inv_view_proj;
    mat4 prev_view_proj;
    mat4 unjittered_vp;
    mat4 prev_unjittered_vp;
    float time;
} ubo;

layout(set = 1, binding = 1) uniform sampler2D depth_tex;
layout(set = 2, binding = 0) uniform sampler2D textures[];

layout(location = 0) in vec2 frag_uv;
layout(location = 1) in vec4 frag_color;
layout(location = 2) in flat uint frag_texture_index;

layout(location = 0) out vec2 out_distortion;

layout(push_constant) uniform PushConstants {
    float strength;
};

float linearize_depth(float d) {
    return abs(ubo.projection[3][2] / (-d - ubo.projection[2][2]));
}

void main() {
    vec2 normal_xy;

    if (frag_texture_index > 0u) {
        // Sample normal map (RG = tangent-space XY)
        vec3 n = texture(textures[nonuniformEXT(frag_texture_index)], frag_uv).rgb;
        normal_xy = n.xy * 2.0 - 1.0;
    } else {
        // Procedural heat haze: smooth radial push, fading at edges
        vec2 centered = frag_uv * 2.0 - 1.0;
        float dist = length(centered);
        float falloff = 1.0 - smoothstep(0.0, 1.0, dist);
        falloff *= falloff; // quadratic for softer edges
        normal_xy = centered * falloff;
    }

    // Soft particle fade
    vec2 screen_uv = gl_FragCoord.xy / vec2(textureSize(depth_tex, 0));
    float scene_dist = linearize_depth(texture(depth_tex, screen_uv).r);
    float particle_dist = linearize_depth(gl_FragCoord.z);
    float soft_fade = clamp((scene_dist - particle_dist) / 0.5, 0.0, 1.0);

    float alpha = frag_color.a * soft_fade;
    out_distortion = normal_xy * strength * alpha;
}
