#version 460

layout(location = 0) in vec3 frag_color;
layout(location = 1) in vec3 frag_world_normal;

layout(location = 0) out vec4 out_color;

void main() {
    vec3 N = normalize(frag_world_normal);
    vec3 L = normalize(vec3(0.5, 1.0, 0.3));
    float NdotL = abs(dot(N, L)); // abs for double-sided lighting
    float lighting = 0.3 + 0.7 * NdotL;

    out_color = vec4(frag_color * lighting, 1.0);
}
