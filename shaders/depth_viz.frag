#version 460

layout(set = 0, binding = 0) uniform sampler2D depth_tex;

layout(push_constant) uniform PushConstants {
    float near;
    float far;
} push;

layout(location = 0) out vec4 out_color;

void main() {
    vec2 uv = gl_FragCoord.xy / vec2(textureSize(depth_tex, 0));
    float depth = texture(depth_tex, uv).r;

    // Depth = 1.0 means nothing was rendered (clear value)
    if (depth >= 1.0) {
        out_color = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    // Linearize: reverse the perspective projection
    // For Vulkan [0,1] depth range with perspective projection
    float linear_z = push.near * push.far / (push.far - depth * (push.far - push.near));

    // Normalize to visible range (clamp far to something useful)
    float viz_far = min(push.far, 100.0);
    float viz = clamp(linear_z / viz_far, 0.0, 1.0);

    out_color = vec4(vec3(viz), 1.0);
}
