#ifndef FJELL_VOLUME_GLSL
#define FJELL_VOLUME_GLSL

#include "noise.glsl"
#include "fjell_volume_shape.glsl"

// What fog does with a placed volume beyond where it is: `volume_occupancy()`
// and the shape constants are fjell_volume_shape.glsl's, the region every
// volume shares.

// How much a volume's noise field leaves at this point, 0..1, to multiply
// whatever the volume contributes. Sampled in **world space** so the field
// does not swim when the volume is moved or scaled, and so two volumes at the
// same scale read as one continuous field rather than two tiled ones.
//
// `contrast` is how broken up the fog looks, and it deliberately does not
// change how much there is: the field is normalised back to an average of one,
// so an artist can dial wisps in and out without re-tuning density.
float volume_noise(vec3 world_pos, vec3 drift, float scale, float contrast, int octaves) {
    if (contrast <= 0.0) return 1.0;

    float inv_scale = 1.0 / max(scale, 0.01);
    float n = fbm_noise(world_pos * inv_scale + drift * inv_scale, max(octaves, 1));

    // [-1,1] -> [1-contrast, 1+contrast], so the mean stays 1.
    return max(1.0 + n * clamp(contrast, 0.0, 1.0), 0.0);
}

#endif // FJELL_VOLUME_GLSL
