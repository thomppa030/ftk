#version 460

layout(location = 0) in vec2 frag_uv;
layout(location = 1) in vec4 frag_color;

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec2 out_metallic_roughness;

void main() {
    // Soft circular particle
    float dist = length(frag_uv * 2.0 - 1.0);
    float alpha = 1.0 - smoothstep(0.5, 1.0, dist);

    out_color = vec4(frag_color.rgb, frag_color.a * alpha);
    out_metallic_roughness = vec2(0.0);
}
