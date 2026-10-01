#ifndef FJELL_SKY_NOISE_GLSL
#define FJELL_SKY_NOISE_GLSL

// Gradient noise on the sky's sphere, shared by the painted night sky: the
// galaxy and the stars read the same field.

// Rodrigues rotation of `v` about unit `axis` by `angle`.
vec3 sky_rotate_about(vec3 v, vec3 axis, float angle) {
    float c = cos(angle);
    float s = sin(angle);
    return v * c + cross(axis, v) * s + axis * dot(axis, v) * (1.0 - c);
}

// One lattice corner's share of the noise below: its gradient, hashed from
// where it is, against the offset to it.
float sky_noise_corner(vec3 i, vec3 f, vec3 o) {
    return dot(normalize(fract(sin(vec3(
                   dot(i + o, vec3(127.1, 311.7, 74.7)),
                   dot(i + o, vec3(269.5, 183.3, 246.1)),
                   dot(i + o, vec3(113.5, 271.9, 124.6)))) * 43758.5453) * 2.0 - 1.0),
               f - o);
}

// Gradient noise on a quintic fade.
//
// Quintic and not smoothstep because smoothstep's second derivative is not
// zero at a cell boundary, so the slope jumps there, and spread across a
// sky-wide gradient that draws faint straight-edged facets. Gradients and not
// values because interpolated values leave the extrema pinned to the lattice,
// which reads as a grid however smooth the fade.
float sky_noise(vec3 p) {
    vec3 i = floor(p);
    vec3 f = p - i;
    vec3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);

    float d000 = sky_noise_corner(i, f, vec3(0, 0, 0));
    float d100 = sky_noise_corner(i, f, vec3(1, 0, 0));
    float d010 = sky_noise_corner(i, f, vec3(0, 1, 0));
    float d110 = sky_noise_corner(i, f, vec3(1, 1, 0));
    float d001 = sky_noise_corner(i, f, vec3(0, 0, 1));
    float d101 = sky_noise_corner(i, f, vec3(1, 0, 1));
    float d011 = sky_noise_corner(i, f, vec3(0, 1, 1));
    float d111 = sky_noise_corner(i, f, vec3(1, 1, 1));

    float y0 = mix(mix(d000, d100, u.x), mix(d010, d110, u.x), u.y);
    float y1 = mix(mix(d001, d101, u.x), mix(d011, d111, u.x), u.y);
    return mix(y0, y1, u.z);
}

// Octaves of the above. Returns roughly [-1, 1].
float sky_fbm(vec3 p, int octaves) {
    float sum = 0.0;
    float amplitude = 1.0;
    float total = 0.0;
    for (int i = 0; i < octaves; ++i) {
        sum += sky_noise(p) * amplitude;
        total += amplitude;
        p *= 2.02;   // not exactly 2, so octaves do not share lattice planes
        amplitude *= 0.5;
    }
    return sum / max(total, 1e-6);
}

#endif
