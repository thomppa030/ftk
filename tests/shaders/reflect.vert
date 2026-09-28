#version 460

// A vertex stage sharing its camera block with reflect.frag, for
// tests/test_gpu_shader.cpp.

layout(set = 0, binding = 0) uniform Camera { mat4 view_proj; } camera;

layout(push_constant) uniform Push { mat4 model; } push;

void main() {
    gl_Position = camera.view_proj * push.model * vec4(0.0, 0.0, 0.0, 1.0);
}
