// A particle's fragment, unlit: its colour from the particle and its texture.
// particle.frag draws blended particles, which fade where they meet the scene
// and read its depth for that; particle_opaque.frag defines
// FJELL_PARTICLE_OPAQUE for opaque ones, which blend nothing and write that
// depth themselves, so they neither fade nor read it.
#extension GL_EXT_nonuniform_qualifier : require

#include "fjell_global_ubo.glsl"

FJELL_DECLARE_GLOBAL_UBO(0, 0, ubo);

#ifndef FJELL_PARTICLE_OPAQUE
layout(set = 1, binding = 1) uniform sampler2D depth_tex;
#endif
layout(set = 2, binding = 0) uniform sampler2D textures[];

layout(location = 0) in vec2 frag_uv;
layout(location = 1) in vec4 frag_color;
layout(location = 2) in flat uint frag_texture_index;

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec2 out_metallic_roughness;
layout(location = 2) out vec2 out_normal;

#ifndef FJELL_PARTICLE_OPAQUE
// Linearize depth from NDC [0,1] to positive view-space distance.
// For perspectiveRH_ZO: z_view = proj[3][2] / (-d - proj[2][2])
// Result is negative (RH convention), abs() gives distance from camera.
float linearize_depth(float d) {
    return abs(ubo.projection[3][2] / (-d - ubo.projection[2][2]));
}
#endif

void main() {
    float alpha;
    vec3 color;

    if (frag_texture_index > 0u) {
        // Textured particle
        vec4 tex = texture(textures[nonuniformEXT(frag_texture_index)], frag_uv);
        color = frag_color.rgb * tex.rgb;
        alpha = frag_color.a * tex.a;
    } else {
        // Procedural fade (no texture assigned)
        vec2 centered = frag_uv * 2.0 - 1.0;
        float v_fade = 1.0 - centered.y * centered.y;
        float h_fade = 1.0 - abs(centered.x);
        h_fade = h_fade * h_fade * h_fade;
        color = frag_color.rgb;
        alpha = frag_color.a * v_fade * h_fade;
    }

#ifndef FJELL_PARTICLE_OPAQUE
    // Soft particle depth fade
    vec2 screen_uv = gl_FragCoord.xy / vec2(textureSize(depth_tex, 0));
    float scene_dist = linearize_depth(texture(depth_tex, screen_uv).r);
    float particle_dist = linearize_depth(gl_FragCoord.z);
    float soft_fade = clamp((scene_dist - particle_dist) / 0.5, 0.0, 1.0);
    alpha *= soft_fade;
#endif

    out_color = vec4(color, alpha);
    out_metallic_roughness = vec2(0.0);
    out_normal = vec2(0.0);
}
