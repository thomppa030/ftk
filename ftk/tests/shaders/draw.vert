#version 460

// A triangle covering the target, for the GPU library's link test
// (tests/library_link/gpu.cpp) to draw through a render encoder.

void main() {
    const vec2 corner = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(corner * 2.0 - 1.0, 0.5, 1.0);
}
