#ifndef FJELL_PAINTED_MILKY_WAY_GLSL
#define FJELL_PAINTED_MILKY_WAY_GLSL

// A painted Milky Way, for a sky to draw.
//
// By the same hand as the clouds, the sun and the moon: one field, cut hard,
// a few flat values. What was wrong with the band it replaces was its shape
// before anything else — a stripe of one width ruled right round the sky,
// which is a belt and not a galaxy. Seen from inside, a galaxy is lopsided:
// a wide bright bulge toward its centre, thinning to a ragged ribbon on the
// far side, with dust lying along the middle of it in torn dark lanes.
//
// So the band has a centre, its width follows how near the centre a direction
// is, and everything else is the one rule the sun uses: each value is solid
// toward the plane and gives way outward, and where it gives way it tears.
// The haze tears into the sky, the body into the haze, the core into the
// body — and the dust is the same rule cutting the other way, out of them.
//
// Everything is measured on the sphere itself. There is no longitude in here,
// only dot products, so there is no seam where an angle wraps round.

#include "sky/sky_noise.glsl"

// How the galaxy is drawn. A sky declares these as its uniforms and fills one
// in.
struct PaintedMilkyWay {
    // How wide the band is on the far side from the centre and at the bulge,
    // as the sine of the angle it reaches off the plane. The gap between the
    // two is what makes it a galaxy rather than a belt.
    float arm_width;
    float bulge_width;
    // Where round the plane the centre sits, in degrees. The plane is fixed —
    // the stars crowd into it — so this slides the bulge along it to wherever
    // it frames best in the hours a scene is played in.
    float centre_turn;
    // How big the forms are, in features across a radian of sky.
    float form_scale;
    // Which galaxy this is: slides the field under the band.
    float pattern;
    // How much dust lies along the plane. 0 leaves the band whole.
    float dust;

    // Over all of it. The band is faint by nature, but hard shapes drawn too
    // faintly stop reading as shapes and start reading as noise.
    float brightness;
    // The values, outermost to innermost, and the dust that cuts across them.
    vec3 haze_color;
    vec3 body_color;
    vec3 core_color;
    vec3 dust_color;
};

// Doubles the frequency, near enough, and turns it about an axis that is none
// of the lattice's own, so no two octaves share a grid.
vec3 galaxy_next_octave(vec3 p) {
    return mat3(vec3(0.0, -1.616, -1.212),
                vec3(1.616, 0.7272, -0.9696),
                vec3(1.212, -0.9696, 1.2928)) * p + 3.1;
}

// The clouds' field, on the sphere: a stack of noise folded through abs(),
// which creases it into ridges, multiplied into a plain stack of the same
// noise, which drives most of the field to nothing and leaves isolated forms
// with torn edges. Warped first, so the forms are not laid out on the noise's
// own grid. Roughly -0.2 where it is emptiest, 0 at its median, past 0.8 only
// at the heart of a form.
float galaxy_field(vec3 p, int octaves) {
    float q = 0.0;
    float amplitude = 0.5;
    vec3 w = p * 0.5;
    for (int i = 0; i < 2; ++i) {
        q += sky_noise(w) * amplitude;
        w = galaxy_next_octave(w) - 3.1;
        amplitude *= 0.5;
    }
    p += q * 0.6;

    float r = 0.0;
    float f = 0.0;
    float ridge_weight = 0.8;
    float plain_weight = 0.7;
    for (int i = 0; i < octaves; ++i) {
        float n = sky_noise(p) * 1.8;
        r += abs(ridge_weight * n);
        f += plain_weight * n;
        p = galaxy_next_octave(p);
        ridge_weight *= 0.7;
        plain_weight *= 0.6;
    }
    return f * (r + f);
}

// The one rule. `across` is how far out a point is over the stretch a value
// gives way in: at or under 0 the value is solid, at 1 it is gone, and
// between them it is whatever of the field clears a cut climbing from `low`
// to `high`. `width` is the edge, about a pixel of the field.
float galaxy_torn(float field, float across, float low, float high, float width) {
    if (across >= 1.0) return 0.0;
    if (across <= 0.0) return 1.0;
    float level = mix(low, high, across);
    return smoothstep(level - width, level + width, field);
}

// The galaxy seen along `view_dir`.
//
// `intensity` is how far the night has come out, the sky's STARS; zero draws
// nothing. `axis` and `angle` are the rotation that carried the sun from noon
// to now, undone on the view direction so the galaxy is fixed to the
// celestial sphere and wheels with it. `horizon_fade` is how much of this
// direction is above the horizon, 1 well up and 0 below
// (`fjell_horizon_fade()`), and is the caller's because this function does
// not know where its horizon is.
//
// `band` is how far inside the galaxy this direction is, 0 outside it and 1
// on the plane, and is set even where nothing is drawn. Smooth on purpose,
// and without the dust: `painted_stars()` reads it to decide how many stars
// to draw per pixel, and a hard edge there cuts stars in half.
vec3 painted_milky_way(PaintedMilkyWay look, vec3 view_dir, float intensity, vec3 axis, float angle,
                       float horizon_fade, out float band) {
    band = 0.0;

    vec3 dir = sky_rotate_about(view_dir, axis, -angle);

    // The plane, and where along it the centre is. The pole is the one the
    // stars were always crowded about, so a sky that swaps galaxies keeps its
    // constellations where they were.
    const vec3 GALACTIC_POLE = normalize(vec3(0.42, 0.78, -0.47));
    vec3 ahead = normalize(cross(GALACTIC_POLE, vec3(0.0, 1.0, 0.0)));
    vec3 centre = sky_rotate_about(ahead, GALACTIC_POLE, radians(look.centre_turn));

    // How far off the plane, and how nearly toward the centre. Both are dot
    // products: nothing here is an angle that wraps.
    float off_plane = dot(dir, GALACTIC_POLE);
    vec3 in_plane = dir - GALACTIC_POLE * off_plane;
    float toward = dot(in_plane / max(length(in_plane), 1e-5), centre);
    toward = smoothstep(-0.3, 1.0, toward);
    toward *= toward;

    // 0 on the plane, 1 at the band's edge, wherever along it this is.
    float height = abs(off_plane) / mix(look.arm_width, look.bulge_width, toward);
    band = 1.0 - smoothstep(0.0, 1.0, height);
    if (intensity <= 0.0 || height >= 1.0) return vec3(0.0);

    // A pixel is about a twenty-fifth of a degree — 1080 lines over a 45
    // degree view — which is what the edge of a cut is as wide as.
    float width = clamp(0.0007 * look.form_scale * 1.5, 0.004, 0.05);
    float field = galaxy_field(dir * look.form_scale + look.pattern, 4);

    float haze = galaxy_torn(field, (height - 0.30) / 0.70, -0.25, 0.9, width);
    float body = galaxy_torn(field, (height - 0.05) / 0.60, -0.25, 0.7, width) * haze;
    // The core is the bulge's own: it has nowhere to be on the far side.
    float core = galaxy_torn(field, height / max(0.55 * toward, 1e-3), -0.2, 0.8, width) * body;

    // Dust lies along the plane, so its field is stretched along it: reading
    // the noise faster across the plane than along it makes forms that are
    // long the way the band runs. Thickest through the bulge, where there is
    // the most galaxy to be in front of.
    float lanes = 0.0;
    if (look.dust > 0.0) {
        vec3 along = dir * look.form_scale * 1.7
                   + GALACTIC_POLE * off_plane * look.form_scale * 4.0 + look.pattern + 36.0;
        float dust_field = galaxy_field(along, 3);
        lanes = galaxy_torn(dust_field, height / (0.25 + 0.35 * toward),
                            mix(0.7, -0.1, look.dust), 0.9, width) * body;
    }

    vec3 painted = look.haze_color * haze;
    painted = mix(painted, look.body_color, body);
    painted = mix(painted, look.core_color, core);
    painted = mix(painted, look.dust_color, lanes);

    return painted * look.brightness * intensity * 3.0 * horizon_fade;
}

#endif
