#version 460

// Reads an unsized texture table at a constant index only, as reflect_table.frag
// does at another, for tests/test_gpu_shader.cpp: glslang sizes each stage's
// table by the highest index it uses.

layout(set = 1, binding = 0) uniform sampler2D textures[];

void main() {
    gl_Position = textureLod(textures[2], vec2(0.5), 0.0);
}
