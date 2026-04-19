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
    float _pad1;
    vec3  up_dir;
    float _pad2;
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

// Orthonormal right/up/forward frame at parameter t, using s.up_dir as the
// reference. Forward = normalized curve tangent. Right = cross(forward, up).
// Up = cross(right, forward) so it stays orthogonal even when up_dir isn't.
mat3 fjell_spline_frame(SplineSegment s, float t) {
    vec3 f = normalize(fjell_spline_hermite_tangent(s, t));
    vec3 up_ref = s.up_dir;
    // Degenerate-case guard: if up is parallel to forward, rotate it.
    if (abs(dot(up_ref, f)) > 0.999) {
        up_ref = (abs(f.x) < 0.9) ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
    }
    vec3 r = normalize(cross(f, up_ref));
    vec3 u = cross(r, f);
    // Columns: right, up, forward — a right-handed basis.
    return mat3(r, u, f);
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
