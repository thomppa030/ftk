#version 460

layout(location = 0) out vec4 out_color;

void main() {
    // Each fragment adds a small amount. More layers = brighter.
    out_color = vec4(0.05, 0.05, 0.05, 1.0);
}
