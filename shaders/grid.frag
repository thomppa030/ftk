#version 460

layout(push_constant) uniform GridPush {
    float cell_size;
    float thickness;
    float fade_distance;
    float opacity;
} grid;

layout(set = 0, binding = 0) uniform GlobalUbo {
    mat4 view;
    mat4 projection;
    vec4 light_direction;
    vec4 light_color;
    vec4 camera_position;
    vec4 ambient_color;
    mat4 cascade_vp[3];
    vec4 cascade_splits;
    uvec4 tile_info;
    mat4 inv_projection;
} ubo;

layout(location = 0) in vec3 world_pos;
layout(location = 0) out vec4 out_color;
layout(location = 1) out vec2 out_metallic_roughness;
layout(location = 2) out vec2 out_normal;

// Anti-aliased line mask for one grid level.
// Returns coverage in [0, 1]: 1 on the line, 0 between lines.
float grid_line(vec2 p, float scale, vec2 fw, float thickness) {
    vec2 cell = p / scale;
    vec2 d = 0.5 - abs(fract(cell) - 0.5);          // distance to nearest line (in cell units)
    vec2 aa = fw / scale;                            // 1 pixel in cell units
    vec2 hw = aa * thickness * 0.5;                  // line half-width
    vec2 line = 1.0 - smoothstep(hw, hw + aa, d);
    return max(line.x, line.y);
}

void main() {
    vec2 p = world_pos.xz;
    float dist = length(ubo.camera_position.xyz - world_pos);
    vec2 fw = fwidth(p);
    float pixel_size = max(fw.x, fw.y);

    // Pick a base scale so the smallest visible cell is at least ~6 pixels wide.
    // Each LOD step is ×10; we blend the next step in as cells get larger on screen.
    float MIN_PIXELS = 6.0;
    float lod = max(0.0, log(max(pixel_size, 1e-6) * MIN_PIXELS / grid.cell_size) / log(10.0));
    float lod_floor = floor(lod);
    float lod_blend = fract(lod);

    float s0 = grid.cell_size * pow(10.0, lod_floor);          // current finest visible level
    float s1 = s0 * 10.0;                                       // next coarser
    float s2 = s0 * 100.0;                                      // next-next coarser

    // Slightly thicker mid/coarse lines so every 10th and 100th cell read as emphasized.
    float l0 = grid_line(p, s0, fw, grid.thickness);
    float l1 = grid_line(p, s1, fw, grid.thickness * 1.3);
    float l2 = grid_line(p, s2, fw, grid.thickness * 1.6);

    // l0 fades out as we transition to the next LOD; l1 is always on; l2 is emphasized.
    float fine = l0 * (1.0 - lod_blend);
    float mid = l1;
    float coarse = l2;

    // Axis lines — use per-axis fwidth so each line's AA matches its own screen-space derivative.
    float ax = 1.0 - smoothstep(fw.x * grid.thickness * 0.5, fw.x * (grid.thickness * 0.5 + 1.0), abs(p.x));
    float az = 1.0 - smoothstep(fw.y * grid.thickness * 0.5, fw.y * (grid.thickness * 0.5 + 1.0), abs(p.y));

    float any_line = max(max(fine, mid), max(coarse, max(ax, az)));
    if (any_line < 0.01) {
        out_color = vec4(0.0);
        out_metallic_roughness = vec2(0.0);
        out_normal = vec2(0.0);
        return;
    }

    // Color: emphasized lines (every 10th, every 100th) are brighter.
    vec3 fine_color = vec3(0.08, 0.08, 0.09);
    vec3 mid_color = vec3(0.22, 0.22, 0.24);
    vec3 coarse_color = vec3(0.40, 0.40, 0.42);
    vec3 color = fine_color;
    color = mix(color, mid_color, mid);
    color = mix(color, coarse_color, coarse);

    color = mix(color, vec3(0.15, 0.25, 0.8), ax);
    color = mix(color, vec3(0.8, 0.2, 0.15), az);

    // Distance fade — soft falloff inside fade_distance.
    float fade = 1.0 - smoothstep(grid.fade_distance * 0.3, grid.fade_distance, dist);
    // Emphasized lines stay more opaque than fine lines.
    float line_strength = max(max(fine * 0.6, mid * 0.85), max(coarse, max(ax, az)));
    float alpha = line_strength * fade * grid.opacity;

    out_color = vec4(color, alpha);
    out_metallic_roughness = vec2(0.0);
    out_normal = vec2(0.0);
}
