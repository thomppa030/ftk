#ifndef FJELL_SKY_BODIES_GLSL
#define FJELL_SKY_BODIES_GLSL

// The bodies a sky shader draws: a sun disc with a halo, a moon disc lit by
// the sun with its phase, and a star field that turns with the day. Each
// returns the light to add to SKY for one view direction, so a sky stays a
// few lines:
//
//     SKY = mix(horizon, zenith, t);
//     SKY += star_field(VIEW_DIR, STARS, CELESTIAL_AXIS, CELESTIAL_ANGLE, TIME);
//     SKY += moon_disc(VIEW_DIR, MOON_DIR, SUN_DIR, MOON_COLOR, FJELL_MOON_RADIUS_DEG);
//     if (!SKY_FOR_LIGHTING) SKY += sun_disc(VIEW_DIR, SUN_DIR, SUN_COLOR, FJELL_SUN_RADIUS_DEG, 1.0, 3.0);

// What the real bodies measure: both subtend about half a degree from the
// ground, so both discs are a touch over a quarter degree in radius. The
// stock skies draw them a little larger than life — a sun the true size is
// nine pixels at 1080p and reads as a speck — but only a little, because
// bloom spreads whatever clips to white further still.
#define FJELL_SUN_RADIUS_DEG 0.27
#define FJELL_MOON_RADIUS_DEG 0.26

// How bright the sun's disc draws, independent of how hard the sun lights
// the ground. Only just over the tonemapper's white point: bloom is additive
// and its threshold is 1.0, so every stop above white widens the white blob
// on screen without making the sun look any brighter. This is the dial that
// decides how big the sun *reads*, more than the radius does.
#define FJELL_SUN_DISC_NITS 1.35

// A crisp disc with a soft halo around it. `radius_deg` is the disc's
// angular radius in degrees and `halo_deg` how far past it the glow reaches;
// `halo` scales that glow's strength.
//
// `sun_color` carries the directional light's intensity, which is how bright
// the sun lights the ground — not how bright its disc should draw. The two
// are separate concerns and the disc keeps its own brightness, so turning
// the light down to tame the disc is never the move: it darkens the scene
// and the disc still clips to white, since even 1.0 is the tonemapper's
// white point and bloom takes everything above it.
vec3 sun_disc(vec3 view_dir, vec3 sun_dir, vec3 sun_color, float radius_deg, float halo_deg, float halo) {
    float d = dot(view_dir, sun_dir);
    // Work in angle rather than in the cosine: near the disc the cosine is
    // flat to four decimal places, so an edge written there is unreadable.
    float angle = degrees(acos(clamp(d, -1.0, 1.0)));
    // The limb is soft over a fixed share of the radius, so a sun of any
    // size keeps the same crispness. The disc is not flat: it falls off
    // toward the limb, so only the middle clips to white and the edge stays
    // under the bloom threshold. A plateau saturates edge to edge and the
    // eye reads the whole coin, which is what makes a small sun look big.
    float disc = 1.0 - smoothstep(radius_deg * 0.35, radius_deg, angle);
    // The glow falls away sharply at the limb and trails off thin, the way
    // air actually scatters the sun: brightest where it touches the disc,
    // an order of magnitude down a disc-width out. A smoothstep ramp
    // instead holds most of its brightness across the first half of its
    // reach, which paints a broad bright ring rather than a glow.
    float t = max(angle - radius_deg, 0.0) / max(halo_deg, 1e-4);
    float glow = 1.0 / (1.0 + 64.0 * t * t);
    glow *= 1.0 - smoothstep(0.5, 1.0, t);   // and reach zero by halo_deg
    // Normalized so the disc sits a little over the white point whatever the
    // light's intensity: its hue still follows the sun through the day, its
    // brightness does not chase the exposure.
    vec3 hue = sun_color / max(max(sun_color.r, max(sun_color.g, sun_color.b)), 1e-4);
    float lit = step(1e-4, max(sun_color.r, max(sun_color.g, sun_color.b)));
    return hue * lit * (disc * FJELL_SUN_DISC_NITS + glow * 0.15 * halo);
}

// A moon disc shaded as a sphere lit from the sun's direction, so the phase
// follows the two bodies rather than a slider. `radius_deg` is the disc's
// angular radius in degrees; `moon_color` is the moon's light already scaled
// by its intensity, so a set or new moon draws nothing.
vec3 moon_disc(vec3 view_dir, vec3 moon_dir, vec3 sun_dir, vec3 moon_color, float radius_deg) {
    float size = cos(radians(radius_deg));
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

// Gradient noise on a quintic fade, and the reason the sky does not use
// `value_noise` from noise.glsl for its galaxy.
//
// Both interpolate smoothly, so both are continuous — but smoothstep's second
// derivative is not zero at a cell boundary, so the *slope* jumps there. On
// the small, busy fields fog and particles ask for, nothing shows. Spread
// across a sky-wide gradient it draws visible creases: faint straight-edged
// facets tiling the band, which is exactly the artifact this replaces. The
// quintic 6t^5-15t^4+10t^3 has zero first and second derivative at both ends,
// so neighbouring cells meet with no crease to find.
float fjell_sky_noise(vec3 p) {
    vec3 i = floor(p);
    vec3 f = p - i;
    vec3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);

    // Hash each corner to a gradient direction and interpolate the dot
    // products, rather than the corner values: interpolating values directly
    // leaves the extrema pinned to the lattice, which reads as a grid however
    // smooth the fade.
    #define FJELL_SKY_GRAD(o) dot(normalize(fract(sin(vec3( \
            dot(i + (o), vec3(127.1, 311.7, 74.7)), \
            dot(i + (o), vec3(269.5, 183.3, 246.1)), \
            dot(i + (o), vec3(113.5, 271.9, 124.6)))) * 43758.5453) * 2.0 - 1.0), \
        f - (o))

    float d000 = FJELL_SKY_GRAD(vec3(0, 0, 0));
    float d100 = FJELL_SKY_GRAD(vec3(1, 0, 0));
    float d010 = FJELL_SKY_GRAD(vec3(0, 1, 0));
    float d110 = FJELL_SKY_GRAD(vec3(1, 1, 0));
    float d001 = FJELL_SKY_GRAD(vec3(0, 0, 1));
    float d101 = FJELL_SKY_GRAD(vec3(1, 0, 1));
    float d011 = FJELL_SKY_GRAD(vec3(0, 1, 1));
    float d111 = FJELL_SKY_GRAD(vec3(1, 1, 1));
    #undef FJELL_SKY_GRAD

    float y0 = mix(mix(d000, d100, u.x), mix(d010, d110, u.x), u.y);
    float y1 = mix(mix(d001, d101, u.x), mix(d011, d111, u.x), u.y);
    return mix(y0, y1, u.z);
}

// Octaves of the above. Returns roughly [-1, 1].
float fjell_sky_fbm(vec3 p, int octaves) {
    float sum = 0.0;
    float amplitude = 1.0;
    float total = 0.0;
    for (int i = 0; i < octaves; ++i) {
        sum += fjell_sky_noise(p) * amplitude;
        total += amplitude;
        p *= 2.02;   // not exactly 2, so octaves do not share lattice planes
        amplitude *= 0.5;
    }
    return sum / max(total, 1e-6);
}

// The colour of a star of a given temperature, as an approximation of the
// blackbody locus over the range stars actually occupy. Cool stars are
// orange-red, the sun's class is near white, hot ones blue-white. `t` walks
// that range from coolest to hottest rather than being a temperature in
// kelvin, since nothing here needs the physical unit.
vec3 fjell_star_color(float t) {
    // Three anchors along the locus, eyeballed against the usual chart:
    // ~2500 K, ~6000 K and ~12000 K, normalized so none of them dims the
    // star relative to the others — brightness is the caller's business.
    const vec3 COOL = vec3(1.00, 0.63, 0.35);
    const vec3 MID  = vec3(1.00, 0.95, 0.91);
    const vec3 HOT  = vec3(0.71, 0.81, 1.00);
    return t < 0.5 ? mix(COOL, MID, t * 2.0) : mix(MID, HOT, (t - 0.5) * 2.0);
}

// Points of light fixed to the celestial sphere: the same rotation that
// carried the sun from noon to now (the environment's axis and angle) is
// undone on the view direction, so the stars wheel with the day. Space is
// cut into cells and a few of them hold a star; the neighbouring cells are
// searched too, since a star's direction can fall just outside the cell it
// was rolled in, so every star is a whole round point. `intensity` scales
// the field; zero draws nothing.
//
// `time` drives scintillation. A star field that only rotates reads as a
// texture being dragged across the sky, because the one thing real starlight
// does that a fixed point does not is twinkle: the air it shines through is
// turbulent, and the deeper the column of air the harder it churns. So the
// wobble here is scaled by how low the star sits — fierce near the horizon,
// almost still overhead — which is what makes it read as atmosphere rather
// than as a flicker effect.
vec3 star_field(vec3 view_dir, float intensity, vec3 axis, float angle, float time) {
    if (intensity <= 0.0) return vec3(0.0);
    vec3 dir = fjell_rotate_about(view_dir, axis, -angle);
    const float CELLS = 60.0;
    vec3 base = floor(dir * CELLS);
    vec3 light = vec3(0.0);

    // The galactic plane, as a band across the celestial sphere. Stars crowd
    // into it and a faint unresolved glow fills it, which is most of what
    // makes a dark sky look like a place rather than a scatter of dots. The
    // pole is fixed in celestial space, so the band wheels with the stars.
    const vec3 GALACTIC_POLE = normalize(vec3(0.42, 0.78, -0.47));
    float band = 1.0 - smoothstep(0.0, 0.42, abs(dot(dir, GALACTIC_POLE)));

    // How many cells hold a star varies across the sky instead of being one
    // constant everywhere. A constant gives every patch of sky the same count
    // and so the same look, which is the tell that reads as procedural; real
    // skies have voids and crowds. The band is the strongest crowd of all.
    // Interpolated noise, so a crowd thins into a void gradually rather than
    // changing count across a cell boundary you can see.
    float clump = fjell_sky_fbm(dir * 2.4 + 137.0, 3) * 0.5 + 0.5;
    float density = 0.022 + 0.030 * clump + 0.055 * band * band;

    // Air mass, near enough: how much atmosphere the line of sight crosses,
    // measured on the *view* direction because that is where the observer's
    // horizon is — the celestial sphere has turned, the air has not.
    float altitude = clamp(view_dir.y, 0.0, 1.0);
    float air_mass = 1.0 - smoothstep(0.0, 0.55, altitude);

    for (int z = -1; z <= 1; ++z) {
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                vec3 cell = base + vec3(x, y, z);
                float h = fjell_star_hash(cell);
                if (h < 1.0 - density) continue;
                // Jittered across the whole cell and a little past it, so
                // neighbours can crowd or leave a gap. Confining each star to
                // the middle of its own cell is what leaves a lattice
                // visible however fine the cells are cut.
                vec3 jitter = vec3(fjell_star_hash(cell + 1.0),
                                   fjell_star_hash(cell + 2.0),
                                   fjell_star_hash(cell + 3.0));
                vec3 star = normalize((cell + 0.5 + (jitter - 0.5) * 1.7) / CELLS);
                // Chord distance on the unit sphere: an angular size, the
                // same on every face and at every latitude.
                float d = length(dir - star);

                // How bright this star is before the air gets to it. Raising
                // a uniform roll to a power skews the field the way a real
                // sky is skewed: a handful of bright stars and very many
                // faint ones, which is what gives the sky depth. A uniform
                // roll instead reads as scattered confetti, every star much
                // like its neighbour.
                float roll = (h - (1.0 - density)) / density;
                float magnitude = pow(roll, 3.5);

                // Only the brightest stars are big enough to show a disc.
                float size = 0.0012 + 0.0016 * magnitude;
                float point = smoothstep(size, size * 0.25, d);

                // Colour correlates with brightness: the bright ones spread
                // to the ends of the locus so the eye finds distinct red and
                // blue among them, while the faint majority stays near white
                // — which is also how they look to the eye, since dim light
                // lands below the threshold where colour vision works.
                float temp = fjell_star_hash(cell + 4.0);
                float spread = 0.25 + 0.75 * magnitude;
                vec3 tint = fjell_star_color(0.5 + (temp - 0.5) * spread);

                // Scintillation. Two rates well off a common multiple, each
                // with the star's own phase, so neighbours never pulse
                // together and the pattern does not repeat on a beat.
                float phase = fjell_star_hash(cell + 6.0) * 6.2831853;
                float rate = 2.3 + fjell_star_hash(cell + 7.0) * 3.1;
                float wobble = sin(time * rate + phase) * 0.6
                             + sin(time * rate * 1.73 + phase * 2.4) * 0.4;
                // Faint stars twinkle proportionally harder — they sit close
                // to the threshold of being seen at all, so the same churn
                // in the air takes them in and out of view.
                float depth = 0.14 + 0.16 * (1.0 - magnitude);
                float twinkle = 1.0 + wobble * depth * air_mass;

                light += tint * point * (0.15 + magnitude) * twinkle;
            }
        }
    }

    // The band's own glow: the stars too faint and too many to draw, which is
    // what the eye actually sees of the galaxy. Mottled, because the plane is
    // patchy with dust lanes rather than a clean airbrushed stripe, and far
    // dimmer than any drawn star — it should read as a suggestion.
    //
    // The mottling has to come from interpolated noise, not from hashing a
    // floored direction: a floored hash is constant across each cell and jumps
    // at its edges, so it draws flat blocks however fine the cells are cut.
    // Cutting them finer only makes the blocks smaller.
    float dust = fjell_sky_fbm(dir * 9.0 + 23.0, 4) * 0.5 + 0.5;
    // A second, slower field carves the dust lanes: broad dark rifts along the
    // plane rather than an even haze. Squared rather than folded with abs():
    // abs() has a corner at zero, which is exactly where the lane is darkest,
    // so folding there would crease every rift down its middle. Squaring is
    // the same V shape with a smooth floor.
    float rift = fjell_sky_fbm(dir * 3.3 + 71.0, 3);
    float lanes = 1.0 - 0.55 * (1.0 - smoothstep(0.0, 0.12, rift * rift));
    float glow = band * band * band * (0.35 + 0.65 * dust) * lanes;
    light += vec3(0.62, 0.66, 0.85) * glow * 0.085;

    return light * intensity * 3.0;
}

#endif // FJELL_SKY_BODIES_GLSL
