#ifndef FJELL_PAINTED_STARS_GLSL
#define FJELL_PAINTED_STARS_GLSL

// Painted stars, for a sky to draw.
//
// What made the field these replace dull was not the stars, it was that they
// were all the same star: one round soft dot at slightly different sizes, so
// the sky read as an even scatter however it was distributed. A drawn night
// sky has a cast. A few dozen stars are characters — four-pointed glints, a
// white-hot core inside a body with real colour — a larger number are small
// hard diamonds, and everything else is dust kept quiet so the first two have
// something to stand in front of.
//
// They hold still. A painted sky does not flicker, which is why this takes no
// clock.
//
// One shape does all three: a superellipse whose exponent runs from 2, a
// circle, through 1, a diamond, to a half, which is the four-pointed star. So
// there is no seam between the classes, only a star getting more pointed as it
// gets more important.

#include "sky/sky_noise.glsl"

// How the stars are drawn. A sky declares these as its uniforms and fills one
// in.
struct PaintedStars {
    // Over all of them.
    float brightness;
    // How many of the stars are the large ones. 1 is a few dozen across the
    // visible sky, which is about what a sky can hold before none of them is
    // special any more — and as many as can be chosen by a star's own roll
    // alone.
    float hero_share;
    // How large those are. 1 is as far as a star can reach and still be found
    // whole from every pixel it covers.
    float hero_size;
    // How pointed a bright star is: 0 keeps every star round, 1 draws the
    // brightest as four-pointed glints and the middle ones as diamonds.
    float glint;
    // How much colour the stars carry. 0 is the near-white of the blackbody
    // locus, which is true and which the eye barely sees; 1 is a painter's
    // orange and blue.
    float saturation;
};

float star_hash(vec3 p) {
    p = fract(p * vec3(443.897, 441.423, 437.195));
    p += dot(p, p.yzx + 19.19);
    return fract((p.x + p.y) * p.z);
}

// The colour of a star of a given temperature, coolest to hottest. The plain
// ends are the blackbody locus over the range stars occupy; the saturated
// ends are what a painter would mix for the same two stars.
vec3 star_color(float t, float vivid) {
    vec3 cool = mix(vec3(1.00, 0.63, 0.35), vec3(1.00, 0.50, 0.22), vivid);
    vec3 mid  = mix(vec3(1.00, 0.95, 0.91), vec3(1.00, 0.93, 0.80), vivid);
    vec3 hot  = mix(vec3(0.71, 0.81, 1.00), vec3(0.50, 0.70, 1.00), vivid);
    return t < 0.5 ? mix(cool, mid, t * 2.0) : mix(mid, hot, (t - 0.5) * 2.0);
}

// The stars seen along `view_dir`.
//
// `intensity` is how far the night has come out, the sky's STARS; zero draws
// nothing. `axis` and `angle` are the rotation that carried the sun from noon
// to now, undone on the view direction so the stars are fixed to the
// celestial sphere and wheel with it. `crowding` is how far inside a galaxy's
// band this direction is, 0 to 1, and the stars crowd into it: pass the
// `band` from `painted_milky_way()`, or 0.0 for a sky with no galaxy in it.
// `horizon_fade` is how much of this direction is above the horizon
// (`fjell_horizon_fade()`). Stars do not meet the ground: the air thickens
// toward it until they are gone, and this finishes them off at the edge.
vec3 painted_stars(PaintedStars look, vec3 view_dir, float intensity, vec3 axis, float angle,
                   float crowding, float horizon_fade) {
    if (intensity <= 0.0) return vec3(0.0);

    vec3 dir = sky_rotate_about(view_dir, axis, -angle);
    const float CELLS = 60.0;
    vec3 base = floor(dir * CELLS);
    vec3 sum = vec3(0.0);

    // How many cells hold a star varies across the sky instead of being one
    // constant everywhere: real skies have voids and crowds, and the band is
    // the strongest crowd of all. Interpolated noise, so a crowd thins into a
    // void gradually rather than changing count across a cell boundary.
    float clump = sky_fbm(dir * 2.4 + 137.0, 3) * 0.5 + 0.5;
    // A little over the share that should show, because stars that cannot be
    // drawn whole are turned away below and the sky should not thin for it.
    float density = (0.022 + 0.030 * clump + 0.055 * crowding * crowding) * 1.15;

    // A pixel is about a twenty-fifth of a degree, 1080 lines over a 45 degree
    // view, in the chord units everything below is measured in.
    const float PIXEL = 0.0007;

    for (int z = -1; z <= 1; ++z) {
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                vec3 cell = base + vec3(x, y, z);
                float h = star_hash(cell);
                if (h < 1.0 - density) continue;

                // Which stars are the large ones is read off the star's own
                // roll and nothing else. The density above is this *pixel's*,
                // and a star's size must not depend on which of its pixels is
                // asking, or its outline bends across a crowd's edge.
                float share = 0.012 * look.hero_share;
                float hero = smoothstep(1.0 - share, 1.0 - share * 0.13, h)
                           * step(1e-4, look.hero_share);

                // Jittered across the whole cell and a little past it, so
                // neighbours can crowd or leave a gap and no lattice shows.
                // The large ones barely move: there are too few of them to
                // show a lattice, and staying put is what leaves them room.
                vec3 jitter = vec3(star_hash(cell + 1.0),
                                   star_hash(cell + 2.0),
                                   star_hash(cell + 3.0));
                vec3 rolled = (cell + 0.5 + (jitter - 0.5) * mix(1.7, 0.2, hero)) / CELLS;

                // How large this star may be and still be drawn whole. A star
                // is rolled at a point in its cell and then moved onto the
                // sphere, and the pixels that find it are the ones whose own
                // cell touches the cell it was *rolled* in — so every pixel it
                // covers has to lie within a cell of that point, and the
                // further it had to move, the less room it has. One with no
                // room even for a dot is turned away; a large one is drawn as
                // large as it has room for, which also means no two of them
                // are the same size.
                float room = 1.0 / CELLS - abs(length(rolled) - 1.0);
                if (room < 0.0030) continue;
                vec3 star = normalize(rolled);

                // How bright this star is before the air gets to it. A power
                // of the roll skews the field the way a real sky is skewed: a
                // handful of bright stars and very many faint ones.
                float roll = (h - (1.0 - density)) / density;
                float magnitude = pow(roll, 3.5);

                // Where this pixel sits across the star, in a frame that keeps
                // the points lined up with the sky's own turning: one arm
                // along the circle the star travels, one toward the pole.
                vec3 offset = dir - star;
                vec3 east = cross(axis, star);
                east = length(east) > 1e-3 ? normalize(east) : vec3(1.0, 0.0, 0.0);
                vec3 north = cross(star, east);
                vec2 across = abs(vec2(dot(offset, east), dot(offset, north)));

                // The superellipse: exponent 2 is a circle, 1 a diamond, a
                // half the four-pointed star. Importance sharpens it.
                float pointed = max(hero, smoothstep(0.15, 0.7, magnitude) * 0.6) * look.glint;
                float exponent = mix(2.0, 0.5, pointed);
                float shape = pow(pow(across.x, exponent) + pow(across.y, exponent),
                                  1.0 / exponent);

                float reach = min(0.0085 * look.hero_size * hero * hero, room - 0.0030);
                float radius = 0.0010 + 0.0020 * magnitude + reach;

                // Small stars keep a soft edge, because a hard one two pixels
                // wide shimmers as the sky turns; large ones are cut clean.
                float soft = max(PIXEL, radius * mix(0.7, 0.12, hero));
                float body = smoothstep(radius, radius - soft, shape);

                // The white-hot core of a large star: a second flat value
                // inside the first, the same shape at a fraction of the size.
                float core_radius = radius * 0.38;
                float core = smoothstep(core_radius, core_radius - max(PIXEL, core_radius * 0.3), shape)
                           * hero;

                // Colour correlates with brightness: the bright ones spread
                // to the ends of the range so the eye finds distinct warm and
                // cool among them, while the faint majority stays near white.
                float temp = star_hash(cell + 4.0);
                float spread = (0.35 + 0.65 * magnitude) * mix(1.0, 1.3, look.saturation);
                vec3 tint = star_color(clamp(0.5 + (temp - 0.5) * spread, 0.0, 1.0), look.saturation);

                // A star arrives from nothing: the density that admits one is
                // this pixel's, so a star drawn at full strength the moment it
                // clears the bar would be cut off wherever the bar moves
                // across it.
                float arrive = smoothstep(0.0, 0.06, roll);

                vec3 star_light = tint * body * (0.10 + 0.55 * magnitude + 0.25 * hero)
                                * arrive;
                star_light = mix(star_light, vec3(0.95), core * arrive);
                sum += star_light;
            }
        }
    }

    return sum * look.brightness * intensity * 3.0 * horizon_fade;
}

#endif
