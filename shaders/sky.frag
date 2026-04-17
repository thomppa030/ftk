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
    mat4 unjittered_vp;
    mat4 prev_unjittered_vp;
    float time;
} ubo;

layout(set = 0, binding = 6) uniform samplerCube sky_cubemap;

layout(push_constant) uniform PushConstants {
    vec4 top_color;
    vec4 bottom_color;
    float viewport_width;
    float viewport_height;
    float use_hdri;     // > 0.5 = sample cubemap instead of gradient
    float padding2;
} push;

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec2 out_metallic_roughness;
layout(location = 2) out vec2 out_normal;

void main() {
    // Reconstruct world-space view direction from screen position.
    vec2 ndc = vec2(
        (gl_FragCoord.x / push.viewport_width) * 2.0 - 1.0,
        (gl_FragCoord.y / push.viewport_height) * 2.0 - 1.0
    );
    vec4 world_near = ubo.inv_view_proj * vec4(ndc, 0.0, 1.0);
    vec4 world_far  = ubo.inv_view_proj * vec4(ndc, 1.0, 1.0);
    vec3 view_dir = normalize(world_far.xyz / world_far.w - world_near.xyz / world_near.w);

    vec3 color;

    if (push.use_hdri > 0.5) {
        // Sample HDRI cubemap at lowest mip for sharp sky
        color = textureLod(sky_cubemap, view_dir, 0.0).rgb;
    } else {
        // Procedural gradient
        float t = gl_FragCoord.y / push.viewport_height;
        color = mix(push.top_color.rgb, push.bottom_color.rgb, t);
    }

    // Sun — light_direction.xyz points TOWARD the light
    vec3 sun_dir = normalize(ubo.light_direction.xyz);
    float sun_dot = dot(view_dir, sun_dir);

    // Sun disc: smaller core, wide soft edge, falloff toward the rim.
    float disc = smoothstep(0.9985, 0.99985, sun_dot);
    float disc_falloff = pow(disc, 2.0);

    // Glow halo that explicitly fades to zero at a defined outer radius
    // so the sun integrates cleanly into the sky instead of bleeding a
    // soft cloud outward with no clear boundary.
    float glow = smoothstep(0.985, 0.9985, sun_dot);
    glow = pow(glow, 3.0);  // concentrate brightness near the disc edge

    vec3 sun_color = ubo.light_color.rgb * ubo.light_direction.w;
    color += sun_color * disc_falloff * 3.0;
    color += sun_color * glow * 0.5;

    out_color = vec4(color, 1.0);
    out_metallic_roughness = vec2(0.0);
    out_normal = vec2(0.0);
}
