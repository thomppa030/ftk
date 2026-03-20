#version 460

layout(location = 0) out vec2 frag_uv;

void main() {
    // Fullscreen triangle: 3 vertices cover the entire screen
    vec2 pos = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    frag_uv = pos;
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
