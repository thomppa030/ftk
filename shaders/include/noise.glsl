#ifndef NOISE_GLSL
#define NOISE_GLSL

// 3D value noise (hash-based, no LUT)
vec3 noise_hash(vec3 p) {
    p = vec3(dot(p, vec3(127.1, 311.7, 74.7)),
             dot(p, vec3(269.5, 183.3, 246.1)),
             dot(p, vec3(113.5, 271.9, 124.6)));
    return fract(sin(p) * 43758.5453123) * 2.0 - 1.0;
}

// Smooth 3D noise [-1, 1]
float value_noise(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    vec3 u = f * f * (3.0 - 2.0 * f);

    return mix(mix(mix(dot(noise_hash(i + vec3(0,0,0)), f - vec3(0,0,0)),
                       dot(noise_hash(i + vec3(1,0,0)), f - vec3(1,0,0)), u.x),
                   mix(dot(noise_hash(i + vec3(0,1,0)), f - vec3(0,1,0)),
                       dot(noise_hash(i + vec3(1,1,0)), f - vec3(1,1,0)), u.x), u.y),
               mix(mix(dot(noise_hash(i + vec3(0,0,1)), f - vec3(0,0,1)),
                       dot(noise_hash(i + vec3(1,0,1)), f - vec3(1,0,1)), u.x),
                   mix(dot(noise_hash(i + vec3(0,1,1)), f - vec3(0,1,1)),
                       dot(noise_hash(i + vec3(1,1,1)), f - vec3(1,1,1)), u.x), u.y), u.z);
}

// Curl noise — divergence-free 3D vector field from scalar noise derivatives
vec3 curl_noise(vec3 p) {
    float e = 0.01;
    // Partial derivatives via central differences of 3 independent noise fields
    float n1 = value_noise(p + vec3(0, e, 0)) - value_noise(p - vec3(0, e, 0));
    float n2 = value_noise(p + vec3(0, 0, e)) - value_noise(p - vec3(0, 0, e));
    float n3 = value_noise(p + vec3(e, 0, 0)) - value_noise(p - vec3(e, 0, 0));

    // Use offset noise fields for the other components
    vec3 q = p + vec3(31.341, 57.129, 13.37);
    float m1 = value_noise(q + vec3(0, e, 0)) - value_noise(q - vec3(0, e, 0));
    float m2 = value_noise(q + vec3(0, 0, e)) - value_noise(q - vec3(0, 0, e));
    float m3 = value_noise(q + vec3(e, 0, 0)) - value_noise(q - vec3(e, 0, 0));

    float inv_e2 = 1.0 / (2.0 * e);
    return vec3(n2 - m1, n3 - m2, m3 - n1) * inv_e2;
}

// Fractal noise: octaves of value_noise at doubling frequency and halving
// amplitude. Returns roughly [-1, 1] — the weights are normalised by the sum
// they could reach, so octave count changes how detailed the field is without
// changing how strong it is.
float fbm_noise(vec3 p, int octaves) {
    float sum = 0.0;
    float amplitude = 1.0;
    float total = 0.0;

    for (int i = 0; i < octaves; ++i) {
        sum += value_noise(p) * amplitude;
        total += amplitude;
        p *= 2.0;
        amplitude *= 0.5;
    }

    return sum / max(total, 1e-6);
}

#endif
