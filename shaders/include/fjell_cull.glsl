#ifndef FJELL_CULL_GLSL
#define FJELL_CULL_GLSL

// Shared frustum / cone / Hi-Z culling math. Used by every cull pass that
// rejects geometry before it reaches the rasterizer.

// Returns a single normalized world-space frustum plane (xyz = unit normal,
// w = signed distance from origin) for plane index 0..5 (left, right, bottom,
// top, near, far). Caller writes into its own storage (shared or local), so
// this works in compute shaders that use shared memory and in those that
// keep planes in a local array.
vec4 cull_frustum_plane(mat4 vp, int idx) {
    vec4 p;
    if (idx == 0) p = vec4(vp[0][3] + vp[0][0], vp[1][3] + vp[1][0], vp[2][3] + vp[2][0], vp[3][3] + vp[3][0]);
    else if (idx == 1) p = vec4(vp[0][3] - vp[0][0], vp[1][3] - vp[1][0], vp[2][3] - vp[2][0], vp[3][3] - vp[3][0]);
    else if (idx == 2) p = vec4(vp[0][3] + vp[0][1], vp[1][3] + vp[1][1], vp[2][3] + vp[2][1], vp[3][3] + vp[3][1]);
    else if (idx == 3) p = vec4(vp[0][3] - vp[0][1], vp[1][3] - vp[1][1], vp[2][3] - vp[2][1], vp[3][3] - vp[3][1]);
    else if (idx == 4) p = vec4(vp[0][3] + vp[0][2], vp[1][3] + vp[1][2], vp[2][3] + vp[2][2], vp[3][3] + vp[3][2]);
    else               p = vec4(vp[0][3] - vp[0][2], vp[1][3] - vp[1][2], vp[2][3] - vp[2][2], vp[3][3] - vp[3][2]);
    return p / length(p.xyz);
}

// Returns true when the sphere (world space) is at least partially inside
// the frustum defined by `planes`.
bool cull_sphere_frustum(vec4 planes[6], vec3 center, float radius) {
    for (int i = 0; i < 6; ++i) {
        if (dot(planes[i].xyz, center) + planes[i].w < -radius) {
            return false;
        }
    }
    return true;
}

// Returns true when the axis-aligned bounding box (world space) is at least
// partially inside the frustum. Uses the positive-vertex / p-vertex test.
bool cull_aabb_frustum(vec4 planes[6], vec3 aabb_min, vec3 aabb_max) {
    for (int i = 0; i < 6; ++i) {
        vec4 plane = planes[i];
        vec3 p;
        p.x = (plane.x >= 0.0) ? aabb_max.x : aabb_min.x;
        p.y = (plane.y >= 0.0) ? aabb_max.y : aabb_min.y;
        p.z = (plane.z >= 0.0) ? aabb_max.z : aabb_min.z;
        if (dot(plane.xyz, p) + plane.w < 0.0) {
            return false;
        }
    }
    return true;
}

// AABB frustum test for an OBB given by a local-space AABB plus a world
// transform. The transform-then-test path is used by per-instance cull.
bool cull_obb_frustum(vec4 planes[6], vec3 local_min, vec3 local_max, mat4 model) {
    vec3 center = (local_min + local_max) * 0.5;
    vec3 extent = (local_max - local_min) * 0.5;
    vec3 world_center = (model * vec4(center, 1.0)).xyz;
    // Conservative world-space half-extents along each axis.
    mat3 abs_M = mat3(abs(model[0].xyz), abs(model[1].xyz), abs(model[2].xyz));
    vec3 world_extent = abs_M * extent;
    return cull_aabb_frustum(planes, world_center - world_extent, world_center + world_extent);
}

// Cone-backface test using meshoptimizer's perspective formula. Returns
// true when the meshlet faces away from the camera and can be skipped.
// apex_local / axis_local come from MeshletBounds (object space); `model`
// puts them in world space.
bool cull_cone(vec3 apex_local, vec3 axis_local, float cutoff,
               mat4 model, vec3 camera_pos) {
    if (cutoff >= 1.0) return false; // degenerate cone — never cull
    vec3 apex = (model * vec4(apex_local, 1.0)).xyz;
    vec3 axis = normalize(mat3(model) * axis_local);
    return dot(normalize(apex - camera_pos), axis) >= cutoff;
}

// Hi-Z occlusion test. Conservative: projects the sphere's nearest point
// (toward the camera) and compares against the max depth in the screen-space
// AABB covered by the sphere's projection. Returns true when the sphere is
// fully behind the depth pyramid at that footprint.
//
// `view`, `projection` are the matrices used to draw the depth pyramid.
// `camera_pos` is the world-space camera position (pass ubo.camera_position.xyz).
// `viewport` is (width, height) in pixels.
// `hiz_mip_count` is the number of Hi-Z mips; 0 disables the test.
// `hiz_pyramid` samples NDC-space depth at the requested mip.
bool cull_hiz_sphere(vec3 center, float radius,
                     mat4 view, mat4 projection, vec3 camera_pos,
                     vec2 viewport, int hiz_mip_count,
                     sampler2D hiz_pyramid)
{
    if (hiz_mip_count == 0) return false;

    vec4 clip_center = projection * view * vec4(center, 1.0);
    if (clip_center.w <= 0.0) return false;       // sphere straddles or is behind the near plane

    // Conservative near point: nudge the world-space center toward the camera
    // by `radius` along the world-space (camera -> center) direction. Works
    // for any projection.
    vec3 to_cam = camera_pos - center;
    float d = length(to_cam);
    if (d <= radius) return false;                // camera is inside the sphere

    vec3 near_point = center + to_cam * (radius / d);
    vec4 clip_near = projection * view * vec4(near_point, 1.0);
    if (clip_near.w <= 0.0) return false;
    float sphere_near_z = clip_near.z / clip_near.w;

    vec3 ndc_center = clip_center.xyz / clip_center.w;

    // Screen-space radius in pixels via vertical projection scale.
    float screen_radius = (radius * abs(projection[1][1]) / clip_center.w) * viewport.y * 0.5;

    vec2 uv_center = ndc_center.xy * 0.5 + 0.5;
    vec2 uv_extent = vec2(screen_radius / viewport.x, screen_radius / viewport.y);
    vec2 uv_min = uv_center - uv_extent;
    vec2 uv_max = uv_center + uv_extent;

    // Sphere partially off-screen — bail (conservative).
    if (uv_max.x < 0.0 || uv_min.x > 1.0 || uv_max.y < 0.0 || uv_min.y > 1.0) {
        return false;
    }
    uv_min = clamp(uv_min, vec2(0.0), vec2(1.0));
    uv_max = clamp(uv_max, vec2(0.0), vec2(1.0));

    // Pick a mip where the sphere covers ~1 texel. Hi-Z mip 0 is half-res,
    // so we shift by 1.
    float diameter_px = max(screen_radius * 2.0, 1.0);
    float mip = clamp(ceil(log2(diameter_px)) - 1.0, 0.0, float(hiz_mip_count - 1));

    float d0 = textureLod(hiz_pyramid, uv_min, mip).r;
    float d1 = textureLod(hiz_pyramid, vec2(uv_max.x, uv_min.y), mip).r;
    float d2 = textureLod(hiz_pyramid, vec2(uv_min.x, uv_max.y), mip).r;
    float d3 = textureLod(hiz_pyramid, uv_max, mip).r;
    float max_depth = max(max(d0, d1), max(d2, d3));

    return sphere_near_z > max_depth;
}

#endif // FJELL_CULL_GLSL
