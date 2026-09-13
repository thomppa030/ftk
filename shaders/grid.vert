#version 460

// Fullscreen triangle. The grid plane is intersected per pixel in the
// fragment shader, so the geometry here carries no world position — a
// camera-following quad would clip against the near plane and lose
// precision towards the horizon.
layout(location = 0) out vec2 ndc;

void main() {
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    ndc = uv * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
}
