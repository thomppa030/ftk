#version 450

void main() {
    vec2 pos = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    // z = 1.0 so the sky sits at the far plane
    gl_Position = vec4(pos * 2.0 - 1.0, 1.0, 1.0);
}
