// Single-scattering atmospheric model for procedural sky rendering.
//
// Approach follows Nishita 1993 / Hillaire 2020 "A Scalable and Production
// Ready Sky and Atmosphere Rendering Technique" in a per-pixel form — no
// precomputed transmittance tables. Good enough for 2K at interactive
// framerates on modern GPUs; complexity is O(primary_steps × secondary_steps).
//
// Coordinate conventions:
//   - Atmosphere is a spherical shell around a planet.
//   - Camera height is a parameter (defaulted to slightly above ground).
//   - view_dir points away from the camera.
//   - sun_dir points TOWARD the sun (matches ubo.light_direction convention).
//
// Usage:
//   AtmosphereParams params = make_default_atmosphere_params();
//   params.sun_color = vec3(1.0);
//   params.sun_intensity = 22.0;
//   vec3 sky = atmosphere(view_dir, sun_dir, params);

#ifndef FJELL_ATMOSPHERE_GLSL
#define FJELL_ATMOSPHERE_GLSL

struct AtmosphereParams {
    float planet_radius;       // meters (Earth: 6_360_000)
    float atmosphere_radius;   // meters (Earth: 6_460_000, ~100km above surface)
    float camera_height;       // meters above ground
    vec3  rayleigh_coeff;      // per-wavelength scatter coefficient (Earth: (5.8e-6, 13.5e-6, 33.1e-6))
    float rayleigh_scale;      // scale height in meters (Earth: 8_500)
    float mie_coeff;           // scalar scatter coefficient (Earth haze: 21e-6)
    float mie_scale;           // scale height in meters (Earth: 1_200)
    float mie_g;               // Henyey-Greenstein asymmetry (Earth haze: 0.76)
    vec3  sun_color;           // linear RGB
    float sun_intensity;       // multiplier on sun_color
    float ground_albedo;       // flat fallback below horizon
    int   primary_steps;       // samples along view ray (16-32 is good)
    int   secondary_steps;     // samples toward sun for optical depth (8 is fine)
};

AtmosphereParams make_default_atmosphere_params() {
    AtmosphereParams p;
    p.planet_radius      = 6360000.0;
    p.atmosphere_radius  = 6460000.0;
    p.camera_height      = 1.0;
    p.rayleigh_coeff     = vec3(5.8e-6, 13.5e-6, 33.1e-6);
    p.rayleigh_scale     = 8500.0;
    p.mie_coeff          = 21e-6;
    p.mie_scale          = 1200.0;
    p.mie_g              = 0.76;
    p.sun_color          = vec3(1.0);
    p.sun_intensity      = 22.0;
    p.ground_albedo      = 0.1;
    p.primary_steps      = 16;
    p.secondary_steps    = 8;
    return p;
}

// Ray-sphere intersection with a sphere centered at origin. Returns t0 and t1
// where the ray enters/exits; both may be negative if the ray is behind the
// origin. Returns (-1, -1) if the ray misses.
vec2 ray_sphere(vec3 ro, vec3 rd, float radius) {
    float b = dot(ro, rd);
    float c = dot(ro, ro) - radius * radius;
    float disc = b * b - c;
    if (disc < 0.0) return vec2(-1.0);
    float s = sqrt(disc);
    return vec2(-b - s, -b + s);
}

float rayleigh_phase(float cos_theta) {
    return (3.0 / (16.0 * 3.14159265)) * (1.0 + cos_theta * cos_theta);
}

float mie_phase_hg(float cos_theta, float g) {
    float g2 = g * g;
    float denom = 1.0 + g2 - 2.0 * g * cos_theta;
    return (1.0 - g2) / (4.0 * 3.14159265 * denom * sqrt(max(denom, 1e-6)));
}

// Main entry. view_dir is the ray from the camera outward; sun_dir points
// TOWARD the sun. Returns linear RGB radiance (HDR, feeds into tone mapping).
vec3 atmosphere(vec3 view_dir, vec3 sun_dir, AtmosphereParams p) {
    // Place the camera at (0, planet_radius + camera_height, 0). Ray-trace
    // the view ray against the atmosphere shell; the integration interval is
    // from the camera to where the view ray exits the atmosphere (or hits
    // the ground). Sun scattering is sampled per primary step along the ray
    // to the sun's exit from the atmosphere.
    vec3 ro = vec3(0.0, p.planet_radius + p.camera_height, 0.0);

    vec2 atmos_hit = ray_sphere(ro, view_dir, p.atmosphere_radius);
    if (atmos_hit.y < 0.0) return vec3(0.0);  // looking away from atmosphere
    float t_start = max(atmos_hit.x, 0.0);
    float t_end = atmos_hit.y;

    // Note: we deliberately do NOT clip t_end at the planet intersection.
    // Doing so would collapse the integration interval to near-zero for
    // below-horizon rays (especially when looking toward the sun, where the
    // Mie forward-scatter peak would otherwise dominate), producing a hard
    // horizon cutoff. Instead we clamp sample altitudes to zero below the
    // surface, which keeps Rayleigh/Mie contributions smooth across the
    // horizon at the cost of some physical fidelity inside the planet.

    float step_len = (t_end - t_start) / float(p.primary_steps);

    vec3 accum_rayleigh = vec3(0.0);
    vec3 accum_mie = vec3(0.0);
    float optical_depth_r = 0.0;
    float optical_depth_m = 0.0;

    float cos_theta = dot(view_dir, sun_dir);
    float phase_r = rayleigh_phase(cos_theta);
    float phase_m = mie_phase_hg(cos_theta, p.mie_g);

    for (int i = 0; i < p.primary_steps; ++i) {
        float t = t_start + step_len * (float(i) + 0.5);
        vec3 sample_pos = ro + view_dir * t;
        // Clamp altitude to >= 0 so below-horizon samples behave as if they
        // were sitting on the surface rather than producing huge exp() values.
        float sample_alt = max(length(sample_pos) - p.planet_radius, 0.0);

        float density_r = exp(-sample_alt / p.rayleigh_scale) * step_len;
        float density_m = exp(-sample_alt / p.mie_scale) * step_len;

        optical_depth_r += density_r;
        optical_depth_m += density_m;

        // Secondary ray toward the sun: accumulate optical depth to the
        // atmosphere exit in the sun direction. Lift the sample to the
        // surface if it's below ground so below-horizon view-ray samples
        // still see the sun through the atmosphere — otherwise every
        // sub-horizon sun ray would be rejected and the tint would snap.
        vec3 sun_origin = sample_pos;
        float sun_origin_len = length(sun_origin);
        if (sun_origin_len < p.planet_radius) {
            sun_origin = sun_origin * (p.planet_radius / max(sun_origin_len, 1e-3));
        }
        vec2 sun_hit = ray_sphere(sun_origin, sun_dir, p.atmosphere_radius);
        if (sun_hit.y > 0.0) {
            // Soft occlusion by the planet body (sun below local horizon):
            // attenuate instead of hard-rejecting, so the horizon transition
            // is a smooth fade rather than a cliff.
            vec2 sun_ground = ray_sphere(sun_origin, sun_dir, p.planet_radius);
            float sun_visibility = 1.0;
            if (sun_ground.x > 0.0 && sun_ground.x < sun_hit.y) {
                float sun_elev = dot(normalize(sun_origin), sun_dir);
                sun_visibility = smoothstep(-0.1, 0.05, sun_elev);
            }

            if (sun_visibility > 0.0) {
                float sun_step = sun_hit.y / float(p.secondary_steps);
                float sun_depth_r = 0.0;
                float sun_depth_m = 0.0;

                for (int j = 0; j < p.secondary_steps; ++j) {
                    float tj = sun_step * (float(j) + 0.5);
                    vec3 sun_pos = sun_origin + sun_dir * tj;
                    float sun_alt = max(length(sun_pos) - p.planet_radius, 0.0);
                    sun_depth_r += exp(-sun_alt / p.rayleigh_scale) * sun_step;
                    sun_depth_m += exp(-sun_alt / p.mie_scale) * sun_step;
                }

                vec3 tau =
                    p.rayleigh_coeff * (optical_depth_r + sun_depth_r) +
                    1.1 * p.mie_coeff * (optical_depth_m + sun_depth_m);
                vec3 attenuation = exp(-tau) * sun_visibility;

                accum_rayleigh += density_r * attenuation;
                accum_mie += density_m * attenuation;
            }
        }
    }

    vec3 scattered =
        (accum_rayleigh * p.rayleigh_coeff * phase_r +
         accum_mie * p.mie_coeff * phase_m) *
        p.sun_color * p.sun_intensity;

    return scattered;
}

#endif // FJELL_ATMOSPHERE_GLSL
