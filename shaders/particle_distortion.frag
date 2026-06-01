#version 460
#extension GL_EXT_nonuniform_qualifier : require

#include "fjell_global_ubo.glsl"

FJELL_DECLARE_GLOBAL_UBO(0, 0, ubo);

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
