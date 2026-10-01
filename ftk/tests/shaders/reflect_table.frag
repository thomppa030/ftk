#version 460

// The fragment half of reflect_table.vert, reading the table at index 0 only,
// for tests/test_gpu_shader.cpp.

layout(set = 1, binding = 0) uniform sampler2D textures[];

layout(location = 0) out vec4 out_colour;

void main() {
    out_colour = texture(textures[0], vec2(0.5));
}
