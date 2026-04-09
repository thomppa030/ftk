#version 460

layout(set = 0, binding = 0) uniform GlobalUbo {
    mat4 view;
    mat4 projection;
    vec4 light_direction;    // xyz = dir toward light, w = intensity
    vec4 light_color;        // xyz = color
    vec4 camera_position;    // xyz = eye pos
    vec4 ambient_color;
    mat4 cascade_vp[3];
    vec4 cascade_splits;
    uvec4 tile_info;
    mat4 inv_projection;
    mat4 inv_view_proj;
    mat4 prev_view_proj;
    float time;
} ubo;

layout(push_constant) uniform PushConstants {
    vec4 top_color;
    vec4 bottom_color;
    float viewport_width;
    float viewport_height;
    float padding1;
    float padding2;
} push;

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec2 out_metallic_roughness;
layout(location = 2) out vec2 out_normal;

void main() {
    float t = gl_FragCoord.y / push.viewport_height;

    // Sky gradient
    vec3 color = mix(push.top_color.rgb, push.bottom_color.rgb, t);

    // Reconstruct world-space view direction from screen position.
    // Vulkan NDC: X [-1,+1] left to right, Y [-1,+1] top to bottom (after Y-flip in projection).
    vec2 ndc = vec2(
        (gl_FragCoord.x / push.viewport_width) * 2.0 - 1.0,
        (gl_FragCoord.y / push.viewport_height) * 2.0 - 1.0
    );
    vec4 world_near = ubo.inv_view_proj * vec4(ndc, 0.0, 1.0);
    vec4 world_far  = ubo.inv_view_proj * vec4(ndc, 1.0, 1.0);
    vec3 view_dir = normalize(world_far.xyz / world_far.w - world_near.xyz / world_near.w);

    // Sun — light_direction.xyz points TOWARD the light
    vec3 sun_dir = normalize(ubo.light_direction.xyz);
    float sun_dot = dot(view_dir, sun_dir);

    // Sun disc with soft edge
    float sun = smoothstep(0.9975, 0.999, sun_dot);

    // Sun glow halo
    float glow = pow(max(sun_dot, 0.0), 128.0) * 0.3;
    glow += pow(max(sun_dot, 0.0), 32.0) * 0.15;

    vec3 sun_color = ubo.light_color.rgb * ubo.light_direction.w;
    color += sun_color * sun * 4.0;
    color += sun_color * glow;

    out_color = vec4(color, 1.0);
    out_metallic_roughness = vec2(0.0);
    out_normal = vec2(0.0);
}
