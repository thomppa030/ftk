#ifndef FJELL_SKY_BODIES_GLSL
#define FJELL_SKY_BODIES_GLSL

// The bodies a sky shader draws: a sun disc with a halo, a moon disc lit by
// the sun with its phase, and a star field that turns with the day. Each
// returns the light to add to SKY for one view direction, so a sky stays a
// few lines:
//
//     SKY = mix(horizon, zenith, t);
//     SKY += star_field(VIEW_DIR, STARS, CELESTIAL_AXIS, CELESTIAL_ANGLE);
//     SKY += moon_disc(VIEW_DIR, MOON_DIR, SUN_DIR, MOON_COLOR, 0.9995);
//     if (!SKY_FOR_LIGHTING) SKY += sun_disc(VIEW_DIR, SUN_DIR, SUN_COLOR, 0.9995, 3.0);

// A crisp disc with a soft halo around it. `size` is the cosine of the
// disc's angular radius (0.9995 is about the real sun); `halo` scales the
// glow beyond it.
vec3 sun_disc(vec3 view_dir, vec3 sun_dir, vec3 sun_color, float size, float halo) {
    float d = dot(view_dir, sun_dir);
    float disc = smoothstep(size, mix(size, 1.0, 0.5), d);
    float glow = pow(smoothstep(size - (1.0 - size) * 30.0, size, d), 3.0);
    return sun_color * (disc * 4.0 + glow * 0.4 * halo);
}

// A moon disc shaded as a sphere lit from the sun's direction, so the phase
// follows the two bodies rather than a slider. `size` is the cosine of the
// disc's angular radius; `moon_color` is the moon's light already scaled by
// its intensity, so a set or new moon draws nothing.
vec3 moon_disc(vec3 view_dir, vec3 moon_dir, vec3 sun_dir, vec3 moon_color, float size) {
    float d = dot(view_dir, moon_dir);
    if (d < size) return vec3(0.0);
    // The point on the disc this ray hits, as a unit sphere normal.
    vec3 right = normalize(cross(moon_dir, abs(moon_dir.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
    vec3 up = cross(right, moon_dir);
    float radius = sqrt(max(1.0 - size * size, 1e-6));
    vec2 offset = vec2(dot(view_dir, right), dot(view_dir, up)) / radius;
    float r2 = dot(offset, offset);
    if (r2 > 1.0) return vec3(0.0);
    vec3 normal = normalize(right * offset.x + up * offset.y - moon_dir * sqrt(1.0 - r2));
    // Lit where the sphere faces the sun; a sliver of earthshine keeps the
    // dark side readable against the sky.
    float lit = max(dot(normal, sun_dir), 0.0);
    float edge = smoothstep(1.0, 0.85, r2);
    return moon_color * (lit + 0.03) * edge * 12.0;
}

// Rodrigues rotation of `v` about unit `axis` by `angle`.
vec3 fjell_rotate_about(vec3 v, vec3 axis, float angle) {
    float c = cos(angle);
    float s = sin(angle);
    return v * c + cross(axis, v) * s + axis * dot(axis, v) * (1.0 - c);
}

float fjell_star_hash(vec3 p) {
    p = fract(p * vec3(443.897, 441.423, 437.195));
    p += dot(p, p.yzx + 19.19);
    return fract((p.x + p.y) * p.z);
}

// Points of light fixed to the celestial sphere: the same rotation that
// carried the sun from noon to now (the environment's axis and angle) is
// undone on the view direction, so the stars wheel with the day. Space is
// cut into cells and a few of them hold a star; the neighbouring cells are
// searched too, since a star's direction can fall just outside the cell it
// was rolled in, so every star is a whole round point. `intensity` scales
// the field; zero draws nothing.
vec3 star_field(vec3 view_dir, float intensity, vec3 axis, float angle) {
    if (intensity <= 0.0) return vec3(0.0);
    vec3 dir = fjell_rotate_about(view_dir, axis, -angle);
    const float CELLS = 60.0;
    const float LIT_CELLS = 0.035;  // share of cells holding a star
    vec3 base = floor(dir * CELLS);
    vec3 light = vec3(0.0);
    for (int z = -1; z <= 1; ++z) {
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                vec3 cell = base + vec3(x, y, z);
                float h = fjell_star_hash(cell);
                if (h < 1.0 - LIT_CELLS) continue;
                vec3 star = normalize((cell + vec3(fjell_star_hash(cell + 1.0),
                                                   fjell_star_hash(cell + 2.0),
                                                   fjell_star_hash(cell + 3.0))) / CELLS);
                // Chord distance on the unit sphere: an angular size, the
                // same on every face and at every latitude.
                float d = length(dir - star);
                // Most stars are pinpricks; a few of the brightest get a
                // little size.
                float size = 0.0012 + 0.0016 * pow(fjell_star_hash(cell + 5.0), 3.0);
                float point = smoothstep(size, size * 0.25, d);
                float brightness = (h - (1.0 - LIT_CELLS)) / LIT_CELLS;
                vec3 tint = mix(vec3(1.0, 0.92, 0.85), vec3(0.8, 0.88, 1.0), fjell_star_hash(cell + 4.0));
                light += tint * point * (0.15 + brightness * brightness);
            }
        }
    }
    return light * intensity * 3.0;
}

#endif // FJELL_SKY_BODIES_GLSL
