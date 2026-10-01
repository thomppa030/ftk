#version 460

// Declares reflect.vert's set 0 binding 0 as a texture instead of its camera
// block, which a pipeline of the two cannot have. For tests/test_gpu_shader.cpp.

layout(set = 0, binding = 0) uniform sampler2D camera;

layout(location = 0) out vec4 out_colour;

void main() {
    out_colour = texture(camera, vec2(0.5));
}
