#version 460

// The fragment half of reflect.vert, with a binding of its own and a larger
// push block, for tests/test_gpu_shader.cpp.

layout(set = 0, binding = 0) uniform Camera { mat4 view_proj; } camera;
layout(set = 1, binding = 0) uniform sampler2D albedo;

layout(push_constant) uniform Push { mat4 model; vec4 colour; } push;

layout(location = 0) out vec4 out_colour;

void main() {
    out_colour = texture(albedo, vec2(0.5)) * push.colour * camera.view_proj[0][0];
}
