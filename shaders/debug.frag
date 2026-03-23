#version 460

layout(location = 0) in vec4 frag_color;
layout(location = 0) out vec4 out_color;
layout(location = 1) out vec2 out_metallic_roughness;

void main() {
    out_color = frag_color;
    out_metallic_roughness = vec2(0.0);
}
