#ifndef FJELL_WIND_GLSL
#define FJELL_WIND_GLSL

// Wind for vertex() hooks: the standard foliage bend, a slow sway and a
// faster flutter that grow with height above the object's origin and with
// the wind's speed, damped by how stiff the thing is. The wind blows in
// world space; bring it into object space first, which for a rotation and
// uniform scale is the transpose of the model's upper 3x3:
//
//     void vertex() {
//         vec3 wind = transpose(mat3(MODEL)) * WIND_DIR;
//         VERTEX += wind_sway(VERTEX, wind, WIND_SPEED, 0.3, TIME);
//     }
//
// `stiffness` 0 is grass, 1 does not move at all.
vec3 wind_sway(vec3 vertex, vec3 wind_dir_object, float wind_speed, float stiffness, float time) {
    float height = max(vertex.y, 0.0);
    float bend = height * height * (1.0 - clamp(stiffness, 0.0, 1.0));
    // Slow sway with the gusts, phase varied across the object so a field
    // does not move as one, plus a quick flutter on top.
    float phase = dot(vertex.xz, vec2(0.7, 0.4));
    float sway = 0.5 + 0.5 * sin(time * 1.1 + phase) + 0.35 * sin(time * 2.3 + phase * 1.7);
    float flutter = 0.15 * sin(time * 6.0 + vertex.y * 3.0 + phase);
    return wind_dir_object * (sway + flutter) * wind_speed * 0.015 * bend;
}

#endif // FJELL_WIND_GLSL
