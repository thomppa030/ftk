#version 460

layout(set = 0, binding = 0) uniform sampler2D scene_tex;
layout(set = 0, binding = 1) uniform sampler2D bloom_tex;

layout(push_constant) uniform PushConstants {
    float bloom_intensity;
    float padding1;
    float padding2;
    float padding3;
} push;

layout(location = 0) out vec4 out_color;

void main() {
    vec2 uv = gl_FragCoord.xy / vec2(textureSize(scene_tex, 0));
    vec3 scene = texture(scene_tex, uv).rgb;

    // bloom_tex is mip[0] which is half res — sample it at half scale
    vec2 bloom_uv = gl_FragCoord.xy / (vec2(textureSize(bloom_tex, 0)) * 2.0);
    vec3 bloom = texture(bloom_tex, bloom_uv).rgb;

    out_color = vec4(scene + bloom * push.bloom_intensity, 1.0);
}
