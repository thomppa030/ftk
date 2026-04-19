#ifndef FJELL_SPLINE_DEFORM_GLSL
#define FJELL_SPLINE_DEFORM_GLSL

// ─────────────────────────────────────────────────────────────────────────
// Spline-mesh vertex deform (Unreal-style SplineMeshComponent).
//
// Deforms a single mesh between two endpoints of a Hermite spline segment.
// Called from the vertex / mesh shader stage before the model transform.
//
// The deform picks an axis of the source mesh (the "forward axis") and
// reinterprets positions along that axis as a parameter t in [0, 1] over
// the spline segment. The mesh is sliced along forward; each slice is
// translated onto the spline, and the in-plane axes (the other two) are
// rotated by the spline's frame at that t. Tangents/normals are rotated by
// the same frame so lighting stays correct.
//
// Geometry stored per segment (one entry per SplineMesh instance):
//   start_pos  — world-space start point
//   start_tan  — world-space tangent *scaled for this segment* (length
//                controls how much the curve bulges — think Hermite handle)
//   end_pos, end_tan — same at the segment end
//   up_dir     — reference up used to build the frame (z column)
//   forward    — which source-mesh axis runs along the spline (0=X,1=Y,2=Z)
//   half_span  — half the source-mesh extent along `forward` in object space
//
// Everything is authored once per segment and lives in an SSBO on the
// render side; here we just declare the struct by value for callers.
// ─────────────────────────────────────────────────────────────────────────

struct SplineSegment {
    vec3  start_pos;
    float half_span;   // mesh extent along forward axis, in object space
    vec3  start_tan;
    uint  forward;     // 0 = X, 1 = Y, 2 = Z
    vec3  end_pos;
    float _pad0;
    vec3  end_tan;
    float arc_length;  // approximate segment length for UV re-parameterization
    // Rotation-minimizing frame quaternions (xyzw) at the segment endpoints.
    // Each encodes the basis whose Z axis points along the curve tangent and
    // whose X/Y axes are the right/up for that slice.
    vec4  start_quat;
    vec4  end_quat;
};

// ── Hermite helpers ───────────────────────────────────────────────────────

vec3 fjell_spline_hermite(SplineSegment s, float t) {
    float t2 = t * t;
    float t3 = t2 * t;
    float h00 =  2.0 * t3 - 3.0 * t2 + 1.0;
    float h10 =        t3 - 2.0 * t2 + t;
    float h01 = -2.0 * t3 + 3.0 * t2;
    float h11 =        t3 -       t2;
    return h00 * s.start_pos + h10 * s.start_tan
         + h01 * s.end_pos   + h11 * s.end_tan;
}

vec3 fjell_spline_hermite_tangent(SplineSegment s, float t) {
    float t2 = t * t;
    float dh00 =  6.0 * t2 -  6.0 * t;
    float dh10 =  3.0 * t2 -  4.0 * t + 1.0;
    float dh01 = -6.0 * t2 +  6.0 * t;
    float dh11 =  3.0 * t2 -  2.0 * t;
    return dh00 * s.start_pos + dh10 * s.start_tan
         + dh01 * s.end_pos   + dh11 * s.end_tan;
}

// ── Per-slice frame ───────────────────────────────────────────────────────

// Quaternion → 3x3 rotation matrix. Columns are the rotated X, Y, Z basis.
mat3 fjell_quat_to_mat3(vec4 q) {
    float x = q.x, y = q.y, z = q.z, w = q.w;
    float xx = x*x, yy = y*y, zz = z*z;
    float xy = x*y, xz = x*z, yz = y*z;
    float wx = w*x, wy = w*y, wz = w*z;
    return mat3(
        vec3(1.0 - 2.0*(yy + zz),       2.0*(xy + wz),       2.0*(xz - wy)),
        vec3(      2.0*(xy - wz), 1.0 - 2.0*(xx + zz),       2.0*(yz + wx)),
        vec3(      2.0*(xz + wy),       2.0*(yz - wx), 1.0 - 2.0*(xx + yy))
    );
}

// Shortest-arc slerp between two unit quaternions. Flips sign when the dot
// product is negative so the interpolation always takes the short way.
vec4 fjell_quat_slerp(vec4 a, vec4 b, float t) {
    float d = dot(a, b);
    if (d < 0.0) { b = -b; d = -d; }
    // Near-parallel quaternions fall back to linear lerp to avoid div-by-zero
    // in sin(theta). The result is renormalized below.
    if (d > 0.9995) return normalize(mix(a, b, t));
    float theta_0 = acos(clamp(d, -1.0, 1.0));
    float theta   = theta_0 * t;
    float sin_t0  = sin(theta_0);
    float s0 = sin(theta_0 - theta) / sin_t0;
    float s1 = sin(theta)           / sin_t0;
    return s0 * a + s1 * b;
}

// Rotation-minimizing frame at parameter t. Columns: right, up, forward.
// The quaternions were computed CPU-side using parallel transport along the
// whole spline, so the frame is continuous across segment boundaries and
// stable through inflection points / sharp bends.
mat3 fjell_spline_frame(SplineSegment s, float t) {
    vec4 q = fjell_quat_slerp(s.start_quat, s.end_quat, t);
    return fjell_quat_to_mat3(q);
}

// ── Deform entry points ───────────────────────────────────────────────────

// Extract the 2D in-plane position (the mesh slice in the cross-section
// perpendicular to `forward`). Returns (right_coord, up_coord, along_coord).
vec3 fjell_spline_split_axes(vec3 p, uint forward) {
    if (forward == 0u) return vec3(p.y, p.z, p.x); // along = X
    if (forward == 1u) return vec3(p.z, p.x, p.y); // along = Y
    return                  vec3(p.x, p.y, p.z);   // along = Z (default)
}

// Map along-axis object-space coord into [0, 1] segment parameter, centered
// around the mesh origin using half_span.
float fjell_spline_along_to_t(float along, float half_span) {
    // (along + half_span) ∈ [0, 2*half_span] → t ∈ [0, 1]
    float denom = max(2.0 * half_span, 1e-6);
    return clamp((along + half_span) / denom, 0.0, 1.0);
}

// Deform a position. Call this on VERTEX before the model transform is
// applied, i.e. while VERTEX is still object-space.
vec3 fjell_spline_deform_position(vec3 v, SplineSegment s) {
    vec3 split = fjell_spline_split_axes(v, s.forward);
    float right_c = split.x;
    float up_c    = split.y;
    float along_c = split.z;

    float t = fjell_spline_along_to_t(along_c, s.half_span);
    mat3 frame = fjell_spline_frame(s, t);
    vec3 center = fjell_spline_hermite(s, t);

    // right = frame[0], up = frame[1]
    return center + right_c * frame[0] + up_c * frame[1];
}

// Running arc length from t=0 to t_target, approximated by summing 8 chord
// samples along the hermite. Fixes the t → distance nonlinearity — without
// this a constant UV scale leaves visible compression/stretching where the
// curve tangent magnitude varies (starts/ends of segments with strong
// tangents, or where a segment accelerates through its middle).
float fjell_spline_running_length(SplineSegment s, float t_target) {
    const int N = 8;
    vec3 prev = s.start_pos;
    float accum = 0.0;
    for (int i = 1; i <= N; ++i) {
        float t = t_target * (float(i) / float(N));
        vec3 p = fjell_spline_hermite(s, t);
        accum += length(p - prev);
        prev = p;
    }
    return accum;
}

// Remap UV so the along-forward U axis tiles by world-space distance.
// Convention: UV.u runs along the mesh's forward axis in [0, 1] over the
// source span (2*half_span). We replace the along-forward fraction of u
// with `running_arc_length / (2*half_span)` — tiling rate matches the flat
// case but distributes evenly along the actual curve.
vec2 fjell_spline_remap_uv(vec2 uv, vec3 v_object, SplineSegment s) {
    vec3 split = fjell_spline_split_axes(v_object, s.forward);
    float t = fjell_spline_along_to_t(split.z, s.half_span);
    float running = fjell_spline_running_length(s, t);
    float span = max(2.0 * s.half_span, 1e-6);
    float u_along_fraction = (split.z + s.half_span) / span;
    float u_transverse = uv.x - u_along_fraction;
    return vec2(u_transverse + running / span, uv.y);
}

// Deform a direction (normal or tangent). Same frame but no translation.
vec3 fjell_spline_deform_direction(vec3 n, vec3 v_for_t, SplineSegment s) {
    // Use the original object-space position to resolve *where on the curve*
    // this vertex lives. Normals at the same t rotate by the same frame.
    vec3 split = fjell_spline_split_axes(v_for_t, s.forward);
    float t = fjell_spline_along_to_t(split.z, s.half_span);
    mat3 frame = fjell_spline_frame(s, t);

    vec3 split_n = fjell_spline_split_axes(n, s.forward);
    // Rotate the (right, up, forward) components by the frame.
    return split_n.x * frame[0] + split_n.y * frame[1] + split_n.z * frame[2];
}

#endif
