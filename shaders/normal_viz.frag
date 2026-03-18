#version 460

layout(location = 0) in vec3 frag_world_normal;
layout(location = 0) out vec4 out_color;

void main() {
    // Map normal [-1,1] to [0,1] for visualization
    // Store raw normal in RGB for shader resource use
    vec3 n = normalize(frag_world_normal);
    out_color = vec4(n * 0.5 + 0.5, 1.0);
}
