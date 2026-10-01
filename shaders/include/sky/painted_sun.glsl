#ifndef FJELL_PAINTED_SUN_GLSL
#define FJELL_PAINTED_SUN_GLSL

// A painted sun and the glow around it, for a sky to draw.
//
// Takes where it is looking and where the sun is rather than reading a sky's
// built-ins, so the same function draws a sun wherever the caller can answer
// those — a sky, a reflection, a preview.
//
// Painted the way the clouds and the sea's shore foam are: one field, cut
// hard. The field is the clouds' own construction, so the sun is visibly by
// the same hand as the sky it sits in. `stylize` at 0 is a clean circle and
// every term below is an identity there; at 1 it is fully painted.

// How the sun is drawn. A sky declares these as its uniforms and fills one in.
struct PaintedSun {
    // Angular radius in degrees. The real sun is about a quarter of a degree;
    // a game sun is usually larger, because a quarter degree reads as a
    // pinprick.
    float size;
    // How far the glow reaches past the limb, in degrees, and how bright it
    // is. Wide, because this is the air around the sun rather than a ring on
    // the disc: it is what a hazy sky does for tens of degrees, and drawn here
    // it is occluded by the clouds and the terrain in front of it, which a
    // screen-space glow added afterwards can never be.
    float halo_size;
    float halo;
    // Where the tight lobe is half-bright, in degrees. The aureole hugging the
    // disc, as distinct from the haze reaching out past it.
    float halo_core;
    // How much of the glow is the broad lobe rather than the tight one. 0 is a
    // close aureole and open sky beyond it; up toward 1 the haze carries tens
    // of degrees, which is what a hazy or dusty sky looks like.
    float halo_spread;
    // How hard the halo stretches toward the horizon, where the ray crosses
    // more air. 0 leaves it a circle.
    float halo_stretch;

    // The master dial. 0 is a clean disc with a smooth falloff; 1 is the
    // painted sun. Every term is an identity at 0, so the style is turned up
    // against a working sun rather than replacing it.
    //
    // The sun is three flat values and one rule, applied three times. Each
    // value is solid toward the centre and gives way outward, and where it
    // gives way it tears: the field is cut at a level that climbs with
    // distance, so less and less of it clears the cut and a solid shape breaks
    // into tongues, then scraps, then nothing. The silhouette tears into the
    // sky, the body tears into the rim, the hot forms tear into the body.
    //
    // A quiet sun is a round disc, one darker band drawn round its edge by
    // hand, a few short licks of flame and no hot forms. Turning `tear`,
    // `corona` and `hot` up is the same sun drawn wilder, not a different one.
    float stylize;
    // How big the forms are, in features across the radius. The sun is under
    // a hundred pixels wide, so this is a small number: the shapes have to be
    // large enough to be shapes. Past about 3 they stop resolving and the sun
    // turns to grit, which no other dial will fix.
    float form_scale;
    // How far inside the authored radius the silhouette starts to tear, as a
    // share of it. 0 keeps a perfect circle under the flames; higher lets the
    // sky bite into the disc.
    float tear;
    // How far past the radius the last scrap of flame reaches, in radii.
    float corona;
    // Where the body starts giving way to the rim, as a share of the radius.
    // This is the limb darkening a real sun has, as a painter would put it
    // down: one darker value round the edge, with a torn boundary rather than
    // a gradient.
    float core;
    // How much of the face the hottest value takes. 0 leaves it out.
    float hot;
    // How fast the forms boil. The octaves drift against each other, as the
    // clouds' do, so shapes change in place instead of sliding as a sheet.
    float boil;

    // The three values, outermost to innermost.
    //
    // These land on the bloom curve, which is not free: a pixel under about
    // 0.5 never blooms, one over 1.0 always does. So the rim stays under it
    // and keeps the silhouette crisp, the body sits at it, and the hot forms
    // are what bleed.
    vec3 rim_color;
    vec3 body_color;
    vec3 hot_color;
};

// What is left of the glow once the sun is below the horizon. The disc has
// gone by then; this is the light it still throws up into the air from under
// the edge of the world.
const float PAINTED_SUN_AFTERGLOW = 0.15;

vec2 sun_hash(vec2 p) {
    p = vec2(dot(p, vec2(127.1, 311.7)), dot(p, vec2(269.5, 183.3)));
    return -1.0 + 2.0 * fract(sin(p) * 43758.5453123);
}

// Simplex noise, which the clouds' field below is built on.
float sun_noise(vec2 p) {
    const float K1 = 0.366025404;
    const float K2 = 0.211324865;
    vec2 i = floor(p + (p.x + p.y) * K1);
    vec2 a = p - i + (i.x + i.y) * K2;
    vec2 o = (a.x > a.y) ? vec2(1.0, 0.0) : vec2(0.0, 1.0);
    vec2 b = a - o + K2;
    vec2 c = a - 1.0 + 2.0 * K2;
    vec3 h = max(0.5 - vec3(dot(a, a), dot(b, b), dot(c, c)), 0.0);
    vec3 n = h * h * h * h * vec3(dot(a, sun_hash(i + 0.0)),
                                  dot(b, sun_hash(i + o)),
                                  dot(c, sun_hash(i + 1.0)));
    return dot(n, vec3(70.0));
}

// Doubles the frequency and turns it, so no two octaves share a grid.
vec2 sun_next_octave(vec2 p) {
    return mat2(vec2(1.6, 1.2), vec2(-1.2, 1.6)) * p;
}

// A stack of noise folded through abs(), which creases it into ridges,
// multiplied into a plain stack of the same noise, which drives most of the
// field to nothing and leaves isolated forms with torn edges. Roughly -0.2
// where it is emptiest, 0 at its median, past 0.8 only at the heart of a form.
//
// `octaves` is how many this pixel can resolve. Anything finer is not detail,
// it is what makes a small sun shimmer.
float sun_field(vec2 uv, float drift, float octaves) {
    float q = 0.0;
    float amplitude = 0.1;
    vec2 quv = uv * 0.5;
    for (int i = 0; i < 3; ++i) {
        q += sun_noise(quv) * amplitude * clamp(octaves - float(i), 0.0, 1.0);
        quv = sun_next_octave(quv);
        amplitude *= 0.4;
    }

    float r = 0.0;
    vec2 tuv = uv + q - drift;
    float weight = 0.8;
    for (int i = 0; i < 4; ++i) {
        r += abs(weight * sun_noise(tuv)) * clamp(octaves - float(i), 0.0, 1.0);
        tuv = sun_next_octave(tuv) + drift;
        weight *= 0.7;
    }

    float f = 0.0;
    tuv = uv + q - drift;
    weight = 0.7;
    for (int i = 0; i < 4; ++i) {
        f += weight * sun_noise(tuv) * clamp(octaves - float(i), 0.0, 1.0);
        tuv = sun_next_octave(tuv) + drift;
        weight *= 0.6;
    }

    return f * (r + f);
}

// The one rule. `band` is how far out a point is across the stretch a value
// gives way over: at or under 0 the value is solid, at 1 it is gone, and
// between them it is whatever of the field clears a cut climbing from `low`
// to `high`. `width` is the edge, about a pixel of the field.
float sun_cut(float field, float band, float low, float high, float width) {
    if (band >= 1.0) return 0.0;
    if (band <= 0.0) return 1.0;
    float level = mix(low, high, band);
    return smoothstep(level - width, level + width, field);
}

// The sun and its glow, seen along `view_dir`.
//
// `air_gain` is how much air the view ray looks through, relative to straight
// up, as a brightness scale (`fjell_air_mass_gain()` in fjell_air_mass.glsl);
// 1.0 is a sky with no air in it. `horizon_fade` is how much of the body is
// above the horizon, 1 well up and 0 below (`fjell_horizon_fade()`). Both are
// the caller's, because this function does not know how much air the caller
// has, or where its horizon is.
vec3 painted_sun(PaintedSun look, vec3 view_dir, vec3 sun_dir, vec3 sun_color, float time,
                 float air_gain, float horizon_fade) {
    // Work in angle rather than in the cosine: near the disc the cosine is
    // flat to four decimal places, so an edge written there is unreadable.
    float angle = degrees(acos(clamp(dot(view_dir, sun_dir), -1.0, 1.0)));

    // The disc's own frame. `face` is where this pixel sits across the sun in
    // units of the radius, so the limb is at 1 whatever angular size the sun
    // was authored at and the same dials draw the same sun large or small.
    vec3 axis = abs(sun_dir.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 right = normalize(cross(sun_dir, axis));
    vec3 up = cross(right, sun_dir);
    vec2 face = vec2(dot(view_dir, right), dot(view_dir, up)) / max(radians(look.size), 1e-6);
    float radial = length(face);
    // The frame is a projection, so the far side of the sky lands on the disc
    // too. Only the half of it facing the sun is the sun.
    float facing = step(0.0, dot(view_dir, sun_dir));

    // ── The plain disc ──────────────────────────────────────────────────────
    float silhouette = (1.0 - smoothstep(0.7, 1.0, radial)) * facing;
    float body = 1.0;
    float heart = 0.0;

    // ── The painted one ─────────────────────────────────────────────────────
    // Only where there can be any of it: the field is the expensive part and
    // nearly all of the sky is nowhere near the sun.
    float reach = 1.0 + look.corona;
    if (look.stylize > 0.0 && facing > 0.0 && radial < reach) {
        // How much of the face one pixel covers. This function is not told
        // the resolution, so it takes a pixel to be a twenty-fifth of a
        // degree — 1080 lines over a 45 degree view. Wrong by the ratio for
        // another display, which costs or gains an octave at the margin and
        // nothing else.
        float pixel = 0.04 / max(look.size, 1e-3) * look.form_scale;
        // Three octaves at most, however large the sun: the fourth is the
        // fine fray on every edge, and a calm sun does not want it.
        float octaves = clamp(log2(1.0 / max(pixel * 2.0, 1e-4)), 1.0, 3.0);
        float width = clamp(pixel * 1.5, 0.004, 0.15);

        // Offset off the noise's origin, where simplex is at its most regular.
        float field = sun_field(face * look.form_scale + vec2(17.3, 5.1), time * look.boil, octaves);

        float painted = sun_cut(field, (radial - (1.0 - look.tear)) / max(look.corona + look.tear, 1e-3),
                                -0.25, 0.9, width);
        silhouette = mix(silhouette, painted, look.stylize);
        body = sun_cut(field, (radial - look.core) / max(1.0 - look.core, 1e-3), -0.25, 0.6, width);
        // The hot forms thin out from the centre all the way to where the
        // body ends, and the dial lowers the whole climb: more of the face
        // clears it.
        heart = sun_cut(field, (radial + 0.35) / (look.core + 0.6), -0.25,
                        mix(1.6, 0.4, look.hot), width)
              * step(1e-3, look.hot) * body * step(radial, 1.0);
    }

    // ── The glow past the limb ──────────────────────────────────────────────
    // The air around the sun, not a ring on the disc. Two lobes, because that
    // is what the air does: a tight aureole from forward scatter close to the
    // disc, and a broad one from haze that carries tens of degrees out. One
    // lobe cannot be both — tuned tight it is gone by five degrees, tuned wide
    // it has no core and the sun sits in a flat wash.
    //
    // Anchored in degrees, so widening `halo_size` lengthens the tail instead
    // of stretching the whole curve. Measured from the authored radius and
    // not from the torn edge: the air does not follow a flame.
    float out_deg = max(angle - look.size, 0.0);
    float near = out_deg / max(look.halo_core, 1e-4);
    float wide = out_deg / max(look.halo_size * 0.4, 1e-4);
    float falloff = mix(1.0 / (1.0 + near * near),
                        1.0 / (1.0 + wide * wide),
                        look.halo_spread);
    // Faded to nothing by the reach, so the halo ends somewhere definite.
    falloff *= 1.0 - smoothstep(0.35, 1.0, out_deg / max(look.halo_size, 1e-4));
    // More air toward the horizon means more of the sun scattered toward the
    // eye, so the halo stretches down rather than staying a circle. Drawn
    // here, it is behind the clouds and the terrain: this is the sky, so
    // anything in front of it covers it, which a glow composited afterwards
    // can never be.
    falloff *= mix(1.0, air_gain, look.halo_stretch);

    // ── Colour ──────────────────────────────────────────────────────────────
    // Normalized so the disc sits where it was authored whatever the sun's
    // intensity: its hue still follows the sun through the day, its brightness
    // does not chase the exposure.
    float peak = max(sun_color.r, max(sun_color.g, sun_color.b));
    vec3 hue = sun_color / max(peak, 1e-4);
    float lit = step(1e-4, peak);

    // Just over the white point: bright enough to bloom, not so bright that
    // the disc blows out to a white blob at high exposure.
    const float disc_nits = 1.35;

    // The three values carry their own colours, so the sun's survive rather
    // than being flattened to the hue of the day; the day still tints them,
    // which is what warms the disc through sunrise and sunset.
    vec3 painted_color = mix(look.rim_color, look.body_color, body);
    painted_color = mix(painted_color, look.hot_color, heart);
    painted_color *= mix(vec3(1.0), hue, 0.5);
    vec3 surface = mix(hue * disc_nits, painted_color, look.stylize);

    // Below the horizon the disc is gone but the air above it is not: a sun
    // just set still lights the sky from underneath, which is the whole of
    // twilight. So the disc takes the fade whole and the glow keeps a trace
    // of itself past it.
    float glow_fade = mix(PAINTED_SUN_AFTERGLOW, 1.0, horizon_fade);

    return lit * (surface * silhouette * horizon_fade
                  + hue * falloff * 0.15 * look.halo * glow_fade);
}

#endif
