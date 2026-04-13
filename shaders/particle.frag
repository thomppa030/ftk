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

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec2 out_metallic_roughness;
layout(location = 2) out vec2 out_normal;

// Linearize depth from NDC [0,1] to positive view-space distance.
// For perspectiveRH_ZO: z_view = proj[3][2] / (-d - proj[2][2])
// Result is negative (RH convention), abs() gives distance from camera.
float linearize_depth(float d) {
    return abs(ubo.projection[3][2] / (-d - ubo.projection[2][2]));
}

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

    // Soft particle depth fade
    vec2 screen_uv = gl_FragCoord.xy / vec2(textureSize(depth_tex, 0));
    float scene_dist = linearize_depth(texture(depth_tex, screen_uv).r);
    float particle_dist = linearize_depth(gl_FragCoord.z);
    float soft_fade = clamp((scene_dist - particle_dist) / 0.5, 0.0, 1.0);
    alpha *= soft_fade;

    out_color = vec4(color, alpha);
    out_metallic_roughness = vec2(0.0);
    out_normal = vec2(0.0);
}
