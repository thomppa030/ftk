#version 460

layout(set = 0, binding = 0) uniform sampler2D ui_texture;

layout(location = 0) in vec2 frag_uv;
layout(location = 0) out vec4 out_color;

void main() {
    out_color = texture(ui_texture, frag_uv);
}
