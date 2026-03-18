#version 460

layout(location = 0) in vec2 frag_uv;
layout(location = 0) out vec4 out_color;

void main() {
    // Checkerboard: alternates black/white every 0.125 UV units (8x8 grid)
    vec2 grid = floor(frag_uv * 8.0);
    float checker = mod(grid.x + grid.y, 2.0);
    vec3 bg = mix(vec3(0.15), vec3(0.85), checker);

    // Overlay UV coordinates as red/green color on top
    vec2 uv_fract = fract(frag_uv);
    vec3 uv_color = vec3(uv_fract, 0.0);

    out_color = vec4(mix(bg, uv_color, 0.5), 1.0);
}
