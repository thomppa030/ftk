#version 460

#include "fjell_global_ubo.glsl"

layout(push_constant) uniform GridPush {
    float cell_size;
    float thickness;
    float fade_distance;
    float opacity;
} grid;

FJELL_DECLARE_GLOBAL_UBO(0, 0, ubo);

layout(location = 0) in vec2 ndc;
layout(location = 0) out vec4 out_color;

// How far the fade radius reaches relative to the camera's height above the
// plane. Without this the grid collapses to a small disc under the camera
// when flying up.
const float HEIGHT_FADE_SCALE = 4.0;

// Below this |ray.y| the plane is edge-on and every LOD collapses into a
// solid band along the horizon.
const float GRAZING_EPSILON = 0.02;

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

// World-space ray through this pixel, from the inverse of the same jittered
// view-projection the geometry was rendered with, so the depth written below
// is directly comparable to the depth buffer.
vec3 ray_direction() {
    vec4 near_h = ubo.inv_view_proj * vec4(ndc, 0.0, 1.0);
    vec4 far_h  = ubo.inv_view_proj * vec4(ndc, 1.0, 1.0);
    return normalize(far_h.xyz / far_h.w - near_h.xyz / near_h.w);
}

void main() {
    vec3 origin = ubo.camera_position.xyz;
    vec3 dir = ray_direction();

    // Intersect y = 0. Rays parallel to the plane, and those pointing away
    // from it, never hit. Works from below the plane as well as above.
    if (abs(dir.y) < 1e-6) { discard; }
    float t = -origin.y / dir.y;
    if (t <= 0.0) { discard; }

    vec3 hit = origin + dir * t;
    vec2 p = hit.xz;

    // Depth of the hit point, so the hardware depth test occludes the grid
    // per sample against real geometry instead of against a quad.
    vec4 clip = ubo.projection * ubo.view * vec4(hit, 1.0);
    gl_FragDepth = clip.z / clip.w;

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
    if (any_line < 0.01) { discard; }

    // Color: emphasized lines (every 10th, every 100th) are brighter.
    vec3 fine_color = vec3(0.08, 0.08, 0.09);
    vec3 mid_color = vec3(0.22, 0.22, 0.24);
    vec3 coarse_color = vec3(0.40, 0.40, 0.42);
    vec3 color = fine_color;
    color = mix(color, mid_color, mid);
    color = mix(color, coarse_color, coarse);

    color = mix(color, vec3(0.15, 0.25, 0.8), ax);
    color = mix(color, vec3(0.8, 0.2, 0.15), az);

    // Distance fade over the ray, widened by camera height so climbing keeps
    // a usable grid instead of shrinking it to a disc.
    float radius = max(grid.fade_distance, abs(origin.y) * HEIGHT_FADE_SCALE);
    float fade = 1.0 - smoothstep(radius * 0.3, radius, t);

    // Grazing fade — kills the solid band where the plane goes edge-on.
    fade *= smoothstep(0.0, GRAZING_EPSILON, abs(dir.y));

    // Coarsest-level fade — once one pixel spans the widest cell there is no
    // structure left to draw, only aliasing.
    fade *= 1.0 - smoothstep(s2 * 0.5, s2, pixel_size);

    // Emphasized lines stay more opaque than fine lines.
    float line_strength = max(max(fine * 0.6, mid * 0.85), max(coarse, max(ax, az)));
    float alpha = line_strength * fade * grid.opacity;
    if (alpha < 0.001) { discard; }

    out_color = vec4(color, alpha);
}
