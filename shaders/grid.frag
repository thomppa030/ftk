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

void main() {
    float x = world_pos.x;
    float z = world_pos.z;
    float dist = length(ubo.camera_position.xyz - world_pos);

    // Compute how close we are to the nearest grid line (0..0.5 range)
    vec2 cell = vec2(x, z) / grid.cell_size;
    vec2 coarse_cell = vec2(x, z) / (grid.cell_size * 10.0);
    // Distance from nearest grid line (edge, not center)
    // fract gives 0..1 within a cell; nearest edge is at 0 or 1
    vec2 d1 = 0.5 - abs(fract(cell) - 0.5);              // fine grid
    vec2 d2 = 0.5 - abs(fract(coarse_cell) - 0.5);       // coarse grid (every 10)

    // Screen-space pixel size in world units at this fragment
    vec2 fw = fwidth(vec2(x, z));

    // Line half-width in cell-fraction space for each grid level
    vec2 hw1 = (fw / grid.cell_size) * grid.thickness * 0.5;
    vec2 hw2 = (fw / (grid.cell_size * 10.0)) * grid.thickness * 2.0;

    // Anti-aliased line: 1.0 on line, 0.0 away from line
    // aa band = 1 pixel feather outside the line edge
    vec2 aa1 = fw / grid.cell_size;
    vec2 aa2 = fw / (grid.cell_size * 10.0);

    vec2 line1 = 1.0 - smoothstep(hw1 - aa1, hw1 + aa1, d1);
    float fine = max(line1.x, line1.y);

    vec2 line2 = 1.0 - smoothstep(hw2 - aa2, hw2 + aa2, d2);
    float coarse = max(line2.x, line2.y);

    // Axis lines (world-space distance from axis)
    float axis_hw = max(fw.x, fw.y) * grid.thickness;
    float aa_axis = max(fw.x, fw.y) * 1.0;
    float ax = 1.0 - smoothstep(axis_hw - aa_axis, axis_hw + aa_axis, abs(x));
    float az = 1.0 - smoothstep(axis_hw - aa_axis, axis_hw + aa_axis, abs(z));

    // Any line visible at all?
    float any_line = max(max(fine, coarse), max(ax, az));
    if (any_line < 0.01) {
        out_color = vec4(0.0);
        out_metallic_roughness = vec2(0.0);
    out_normal = vec2(0.0);
        return;
    }

    // Compose — solid black lines, colored axes
    vec3 color = vec3(0.08, 0.08, 0.09);

    color = mix(color, vec3(0.15, 0.25, 0.8), ax);
    color = mix(color, vec3(0.8, 0.2, 0.15), az);

    // Distance fade — only affects alpha
    float fade = 1.0 - smoothstep(grid.fade_distance * 0.3, grid.fade_distance, dist);
    float alpha = any_line * 0.8 * fade * grid.opacity;

    out_color = vec4(color, alpha);
    out_metallic_roughness = vec2(0.0);
    out_normal = vec2(0.0);
}
