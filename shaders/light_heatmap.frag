#version 450

layout(set = 0, binding = 0) readonly buffer TileLightData {
    uint tile_data[];
};

layout(push_constant) uniform PushConstants {
    uint tiles_x;
    uint tiles_y;
    uint mode;
} push;

layout(location = 0) out vec4 out_color;

vec3 heatmap_color(uint count) {
    // 0 = black, 1-4 = blue, 5-16 = green, 17-64 = yellow, 65+ = red
    float c = float(count);

    vec3 black  = vec3(0.0, 0.0, 0.0);
    vec3 blue   = vec3(0.0, 0.2, 1.0);
    vec3 green  = vec3(0.0, 1.0, 0.2);
    vec3 yellow = vec3(1.0, 1.0, 0.0);
    vec3 red    = vec3(1.0, 0.0, 0.0);

    if (count == 0u) return black;

    if (count <= 4u) {
        float t = smoothstep(0.0, 4.0, c);
        return mix(black, blue, t);
    }
    if (count <= 16u) {
        float t = smoothstep(4.0, 16.0, c);
        return mix(blue, green, t);
    }
    if (count <= 64u) {
        float t = smoothstep(16.0, 64.0, c);
        return mix(green, yellow, t);
    }

    float t = smoothstep(64.0, 128.0, c);
    return mix(yellow, red, t);
}

void main() {
    uvec2 tile = uvec2(gl_FragCoord.xy) / 16u;

    // Clamp to valid tile range
    tile = min(tile, uvec2(push.tiles_x - 1u, push.tiles_y - 1u));

    uint tile_index = tile.y * push.tiles_x + tile.x;
    uint light_count = tile_data[tile_index];

    vec3 color = heatmap_color(light_count);

    if (push.mode == 1u) {
        // Grid overlay: draw 1px white lines at tile boundaries
        float fx = fract(gl_FragCoord.x / 16.0);
        float fy = fract(gl_FragCoord.y / 16.0);

        float border_threshold = 1.0 / 16.0;
        bool on_border = (fx < border_threshold) || (fy < border_threshold);

        if (on_border) {
            color = mix(color, vec3(1.0), 0.5);
        }
    }

    out_color = vec4(color, 1.0);
}
