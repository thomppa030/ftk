#ifndef FJELL_PAINTED_MOON_GLSL
#define FJELL_PAINTED_MOON_GLSL

// A painted moon, for a sky to draw. By the same hand as `painted_sun()` and
// the clouds: one field, cut hard, a few flat values.
//
// What the field draws here is not fire. The construction that tears a cloud
// out of the sky leaves isolated forms with ragged edges, and on a pale disc
// isolated ragged forms are seas: the maria, the darker plains that make a
// moon read as the moon rather than as a white coin. They hold still — this
// is rock — and the only edge that moves is the terminator, which follows the
// sun as it always did.
//
// Takes where it is looking and where the two bodies are rather than reading
// a sky's built-ins, so the same function draws a moon wherever the caller
// can answer those. `stylize` at 0 is the smooth shaded sphere `moon_disc()`
// draws and every term below is an identity there.

// How the moon is drawn. A sky declares these as its uniforms and fills one
// in.
struct PaintedMoon {
    // Angular radius in degrees. The real moon is about a quarter of a degree,
    // which is a dozen pixels; a painted one usually wants to be several times
    // that, and it is the size that decides how much of the face can be drawn.
    float size;
    // The glow in the air around it: how bright, and how far it reaches in
    // degrees. It follows the phase, so a thin crescent barely lights the air
    // and a full moon sits in a pool of it.
    float halo;
    float halo_size;

    // The master dial. 0 is the smooth shaded sphere; 1 is the painted moon.
    float stylize;
    // How big the seas are, in features across the radius. A moon is small on
    // screen, so this is a small number: past about 2.5 the seas turn to
    // speckle.
    float form_scale;
    // How much of the face the seas take. The real near side is about a
    // third.
    float maria;
    // Which face this moon has. The seas are wherever the field put them; this
    // slides the field under the disc until they sit somewhere worth looking
    // at.
    float pattern;
    // How far the line between day and night strays from a clean curve. A
    // drawn terminator catches on the ground it crosses; a ruled one reads as
    // a mask.
    float terminator_tear;

    // The three values, as shares of the moon's light: the bright highlands,
    // the seas, and the night side, which is earthshine and only just there.
    vec3 highland_color;
    vec3 maria_color;
    vec3 night_color;
};

vec2 moon_hash(vec2 p) {
    p = vec2(dot(p, vec2(127.1, 311.7)), dot(p, vec2(269.5, 183.3)));
    return -1.0 + 2.0 * fract(sin(p) * 43758.5453123);
}

// Simplex noise, which the clouds' field below is built on.
float moon_noise(vec2 p) {
    const float K1 = 0.366025404;
    const float K2 = 0.211324865;
    vec2 i = floor(p + (p.x + p.y) * K1);
    vec2 a = p - i + (i.x + i.y) * K2;
    vec2 o = (a.x > a.y) ? vec2(1.0, 0.0) : vec2(0.0, 1.0);
    vec2 b = a - o + K2;
    vec2 c = a - 1.0 + 2.0 * K2;
    vec3 h = max(0.5 - vec3(dot(a, a), dot(b, b), dot(c, c)), 0.0);
    vec3 n = h * h * h * h * vec3(dot(a, moon_hash(i + 0.0)),
                                  dot(b, moon_hash(i + o)),
                                  dot(c, moon_hash(i + 1.0)));
    return dot(n, vec3(70.0));
}

// Doubles the frequency and turns it, so no two octaves share a grid.
vec2 moon_next_octave(vec2 p) {
    return mat2(vec2(1.6, 1.2), vec2(-1.2, 1.6)) * p;
}

// A stack of noise folded through abs(), which creases it into ridges,
// multiplied into a plain stack of the same noise, which drives most of the
// field to nothing and leaves isolated forms with torn edges. Roughly -0.2
// where it is emptiest, 0 at its median, past 0.8 only at the heart of a form.
//
// `octaves` is how many this pixel can resolve. Three at most: the seas are
// shapes, and a fourth octave is fray on their shores that a body this small
// on screen turns into shimmer.
float moon_field(vec2 uv, float octaves) {
    float q = 0.0;
    float amplitude = 0.1;
    vec2 quv = uv * 0.5;
    for (int i = 0; i < 3; ++i) {
        q += moon_noise(quv) * amplitude * clamp(octaves - float(i), 0.0, 1.0);
        quv = moon_next_octave(quv);
        amplitude *= 0.4;
    }

    float r = 0.0;
    vec2 tuv = uv + q;
    float weight = 0.8;
    for (int i = 0; i < 3; ++i) {
        r += abs(weight * moon_noise(tuv)) * clamp(octaves - float(i), 0.0, 1.0);
        tuv = moon_next_octave(tuv);
        weight *= 0.7;
    }

    float f = 0.0;
    tuv = uv + q;
    weight = 0.7;
    for (int i = 0; i < 3; ++i) {
        f += weight * moon_noise(tuv) * clamp(octaves - float(i), 0.0, 1.0);
        tuv = moon_next_octave(tuv);
        weight *= 0.6;
    }

    return f * (r + f);
}

// The moon and its glow, seen along `view_dir`.
//
// The phase is geometry: the disc is shaded as a sphere lit from `sun_dir`,
// so the crescent follows the two bodies rather than a dial. `moon_color` is
// the moon's light already scaled by its intensity, so a set or a new moon
// draws nothing. `horizon_fade` is how much of the body is above the horizon,
// 1 well up and 0 below (`fjell_horizon_fade()`), and is the caller's because
// this function does not know where its horizon is. Unlike the sun the moon
// leaves nothing behind once it is down, glow included.
vec3 painted_moon(PaintedMoon look, vec3 view_dir, vec3 moon_dir, vec3 sun_dir, vec3 moon_color,
                  float horizon_fade) {
    // The disc's own frame. `face` is where this pixel sits across the moon
    // in units of the radius, so the limb is at 1 whatever angular size it
    // was authored at. The frame keeps the zenith up, so the face stays
    // upright as the moon crosses the sky rather than turning with it.
    vec3 axis = abs(moon_dir.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 right = normalize(cross(moon_dir, axis));
    vec3 up = cross(right, moon_dir);
    vec2 face = vec2(dot(view_dir, right), dot(view_dir, up)) / max(sin(radians(look.size)), 1e-6);
    float r2 = dot(face, face);
    // The frame is a projection, so the far side of the sky lands on the disc
    // too. Only the half of it facing the moon is the moon.
    float facing = step(0.0, dot(view_dir, moon_dir));

    // How much of the face one pixel covers. This function is not told the
    // resolution, so it takes a pixel to be a twenty-fifth of a degree — 1080
    // lines over a 45 degree view. Wrong by the ratio for another display,
    // which softens or sharpens an edge by that much.
    float pixel = 0.04 / max(look.size, 1e-3);

    vec3 surface = vec3(0.0);
    float disc = 0.0;
    if (facing > 0.0 && r2 < 1.0 + pixel * 4.0) {
        // The point on the sphere this ray hits, and how squarely it faces
        // the sun. This is the whole of the phase.
        vec3 normal = normalize(right * face.x + up * face.y
                                - moon_dir * sqrt(max(1.0 - r2, 0.0)));
        float sunward = dot(normal, sun_dir);

        // The smooth sphere: shaded by the sun, a sliver of earthshine so the
        // night side stays readable, a soft limb.
        vec3 smooth_surface = vec3(max(sunward, 0.0) + 0.03);
        float smooth_disc = smoothstep(1.0, 0.85, r2);

        // The painted one: a hard limb, a hard terminator, and the seas.
        float radial = sqrt(r2);
        float painted_disc = 1.0 - smoothstep(1.0 - pixel, 1.0 + pixel, radial);

        float field_pixel = pixel * look.form_scale;
        float octaves = clamp(log2(1.0 / max(field_pixel * 2.0, 1e-4)), 1.0, 3.0);
        float width = clamp(field_pixel * 1.5, 0.004, 0.15);
        float field = moon_field(face * look.form_scale + vec2(look.pattern, look.pattern * 0.262),
                                 octaves);

        // The cut that leaves about a third of the field standing at the
        // default, and none or nearly all of it at the ends of the dial.
        float sea_level = mix(0.6, -0.1, look.maria);
        float sea = smoothstep(sea_level - width, sea_level + width, field)
                  * step(1e-3, look.maria);

        // The terminator is the same kind of edge: where the sun's angle
        // crosses zero, pushed about by the field so it catches on the
        // ground it crosses.
        float day = smoothstep(-pixel, pixel, sunward + field * look.terminator_tear);

        vec3 painted_surface = mix(look.night_color,
                                   mix(look.highland_color, look.maria_color, sea), day);

        surface = mix(smooth_surface, painted_surface, look.stylize);
        disc = mix(smooth_disc * step(r2, 1.0), painted_disc, look.stylize);
    }

    // ── The glow in the air around it ───────────────────────────────────────
    // Brightest where it touches the disc and thin a disc-width out, the way
    // air scatters any small bright body. Scaled by how much of the face is
    // lit: the sun stands opposite a full moon and beside a new one.
    float angle = degrees(acos(clamp(dot(view_dir, moon_dir), -1.0, 1.0)));
    float t = max(angle - look.size, 0.0) / max(look.halo_size, 1e-4);
    float glow = 1.0 / (1.0 + 16.0 * t * t);
    glow *= 1.0 - smoothstep(0.5, 1.0, t);
    float lit_share = 0.5 - 0.5 * dot(moon_dir, sun_dir);

    // Twelve is what `moon_disc()` draws a full moon at, against a light
    // whose intensity is a moon's and not a sun's. Kept, so a scene's night
    // is as bright under this moon as it was under that one.
    return moon_color * 12.0 * horizon_fade
         * (surface * disc + vec3(glow * lit_share * 0.05 * look.halo));
}

#endif
