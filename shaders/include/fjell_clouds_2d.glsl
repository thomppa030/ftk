#ifndef FJELL_CLOUDS_2D_GLSL
#define FJELL_CLOUDS_2D_GLSL

// A cloud layer as a lit sheet at a fixed height, the same noise for the sky
// that draws it and the shadow map that projects it onto the ground, so the
// shadow under a cloud is the cloud's. The layer is placed in world space: a
// view ray meets it where it crosses the cloud height, so moving the camera
// slides the clouds the way real ones slide, and the wind carries the whole
// sheet along over time.
//
// The weather front shapes the coverage: `front_dir` is the way the front
// travels over the ground and `front_edge` where its edge stands across the
// sky, from -1 at the windward horizon to 1 at the leeward one. Clouds exist
// on the windward side of the edge, so as the edge sweeps across during a
// weather's approach the sky closes from upwind, and the tail flips the
// direction and opens it from the same side.
//
// Usage in a sky:
//     vec4 cloud = cloud_layer(CAMERA_POS, VIEW_DIR, CLOUD_COVERAGE, CLOUD_DENSITY,
//                              WIND_DRIFT, FRONT_DIR, FRONT_EDGE, FRONT_SOFTNESS,
//                              SUN_DIR, SUN_COLOR, MOON_COLOR, ambient);
//     SKY = mix(SKY, cloud.rgb, cloud.a);

// How high the sheet sits and how big one cloud is on it. Globals rather
// than constants so a sky can expose them: the two only mean anything as a
// ratio, and that ratio is what decides how the sky reads.
//
// Looking straight up you are as close to the sheet as you will ever be, so a
// cloud there subtends `2*atan(size / 2 / height)` — the widest it can look.
// At 1400 m across a sheet 1500 m up that is fifty degrees, barely one cloud
// on screen, which is the "under a microscope" effect. Real cumulus run
// 500-1000 m across with their base at 1000-2000 m, and these defaults sit in
// that range: about sixteen degrees overhead, four clouds across a 60 degree
// view.
float fjell_cloud_height = 2200.0;        // metres above the world origin
float fjell_cloud_size = 600.0;           // metres across one cloud feature
// The sheet is a shell at cloud height around a planet this big, not a plane.
//
// A plane's distance is height/sin(elevation), which runs to infinity at the
// horizon: the sheet foreshortens without limit, so cloud detail compresses
// into a band of stipple and then a smear where nothing can be resolved. A
// shell terminates — at this radius the far edge is about 138 km away — and
// that finite limit is exactly why real clouds converge to a horizon line
// instead of dissolving into noise.
//
// Earth's radius. Not because the world is round here, but because the number
// decides where the sheet ends, and this is the value that puts it where the
// eye expects the horizon to be.
const float FJELL_CLOUD_PLANET_RADIUS = 6371000.0;

// Roughly how much sky one pixel covers, in radians. Not read from the
// projection because the sheet is drawn by a fullscreen sky pass that has no
// per-pixel derivative worth trusting at the horizon, where the footprint
// grows without bound; a fixed estimate at a common resolution and field of
// view is what the detail budget needs, and being off by a factor of two
// costs one octave either way.
const float FJELL_CLOUD_PIXEL_ANGLE = 0.001;   // ~1080p at a 60 degree fov
// Noise units per metre, derived so one noise blob — about half a unit — is
// `fjell_cloud_size` metres across. Derived rather than authored, so the size
// above is the only place the scale is decided.
#define FJELL_CLOUD_SCALE (0.5 / max(fjell_cloud_size, 1.0))
const float FJELL_CLOUD_DRIFT = 4.0;          // the sheet moves this many times faster than the ground wind
const float FJELL_CLOUD_PERIOD = 4096.0;      // the noise repeats after this many units, keeping it precise far out

// What the detail budget measures its octaves against: the largest feature
// the field carries, which is one cloud.
#define FJELL_CLOUD_FEATURE_METRES (max(fjell_cloud_size, 1.0))

float fjell_cloud_hash(vec2 p) {
    // Wrap so the same cell hashes the same anywhere and the arithmetic
    // stays small; the mixing below is free of the correlated streaks the
    // cheaper multiply hashes show.
    p = mod(p, FJELL_CLOUD_PERIOD);
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float fjell_cloud_noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    // Quintic rather than smoothstep: smoothstep's second derivative is not
    // zero at a cell boundary, so the slope jumps there. On a small busy
    // field nothing shows, but a broad soft overcast sheet is exactly the
    // case that puts a gentle gradient across a whole cell and draws the
    // crease as a faint straight-edged facet.
    vec2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    return mix(mix(fjell_cloud_hash(i), fjell_cloud_hash(i + vec2(1.0, 0.0)), u.x),
               mix(fjell_cloud_hash(i + vec2(0.0, 1.0)), fjell_cloud_hash(i + vec2(1.0, 1.0)), u.x), u.y);
}

// How many octaves of detail survive at this pixel, as a continuous count.
// Set once per fragment before sampling the field; 99 asks for all of them.
//
// This is mipmapping, done by hand because the field is evaluated rather than
// sampled. An octave finer than two pixels cannot be resolved: what it adds
// is not detail but noise, and noise that changes as the camera moves is the
// flicker you see along the horizon. Dropping it is not a loss of detail —
// the detail was never visible, only its aliasing was.
float fjell_cloud_lod = 99.0;

float fjell_cloud_fbm(vec2 p) {
    float v = 0.0;
    float a = 0.5;
    mat2 rot = mat2(0.8, 0.6, -0.6, 0.8);
    for (int i = 0; i < 5; ++i) {
        // Fade the octave out across its last step rather than dropping it on
        // one frame: a hard cut moves as the camera does and shows up as a
        // band sliding through the sky.
        float keep = clamp(fjell_cloud_lod - float(i), 0.0, 1.0);
        if (keep <= 0.0) break;
        v += a * fjell_cloud_noise(p) * keep;
        // The octaves that survive are renormalised, so dropping detail
        // thins the field rather than darkening it.
        v += a * 0.5 * (1.0 - keep);
        p = rot * p * 2.03 + 11.3;
        a *= 0.5;
    }
    return v;
}

// The same field with its coordinates pushed about by a coarser copy of
// itself. Plain fbm gives round blobs, because every octave is isotropic
// about the same origin; warping the domain first stretches and curls those
// blobs along the warp field, which is what turns a lumpy sheet into
// something with the billowing, sheared structure clouds actually have.
float fjell_cloud_warped_fbm(vec2 p) {
    vec2 warp = vec2(fjell_cloud_fbm(p * 0.5 + 17.7),
                     fjell_cloud_fbm(p * 0.5 + 43.1)) - 0.25;
    return fjell_cloud_fbm(p + warp * 1.6);
}

// How far along a ray the cloud shell is, or -1 when the ray never meets it.
//
// The shell is centred a planet-radius below the viewer, so locally it is the
// same sheet at `height` overhead — it differs only in curving away, which is
// what gives the horizon a finite distance. `origin_y` is the viewer's height
// above the world origin.
float fjell_cloud_shell_distance(float dir_y, float origin_y, float height) {
    // Looking down, or so close to level that the shell is effectively behind
    // the viewer: no crossing.
    if (dir_y <= 0.0) return -1.0;
    const float R = FJELL_CLOUD_PLANET_RADIUS;
    float r = R + max(origin_y, 0.0);        // viewer's distance from centre
    float shell = R + height;                // the shell's radius
    // Ray from (0, r) in direction (cos, dir_y) against a circle of radius
    // `shell`: the quadratic's discriminant is always positive when the
    // viewer is inside the shell, so only the forward root matters.
    float b = r * dir_y;
    float c = (shell * shell) - (r * r);
    float disc = (b * b) + c;
    if (disc <= 0.0) return -1.0;
    return -b + sqrt(disc);
}

// Where the sheet is over a ground position, after the wind has moved it.
// `wind_drift` is how far the wind has carried things so far, in metres:
// the environment integrates it, so the sheet rides through gusts smoothly.
vec2 fjell_cloud_sheet_pos(vec2 world_xz, vec2 wind_drift) {
    return (world_xz - wind_drift * FJELL_CLOUD_DRIFT) * FJELL_CLOUD_SCALE;
}

// How much of the front's coverage applies for a view direction: full on
// the windward side of the edge, none past it, soft across it.
float fjell_cloud_front_mask(vec3 dir, vec2 front_dir, float front_edge, float front_softness) {
    vec2 flat_dir = dir.xz;
    float len = length(flat_dir);
    if (len < 1e-4) return 1.0;
    float t = -dot(flat_dir / len, front_dir);   // 1 toward the windward horizon
    return smoothstep(front_edge - front_softness, front_edge + front_softness, t);
}

// The range the cloud field occupies, and the value it centres on. Every
// place that cuts, shapes or renormalises the field measures against these,
// so a change to the field's construction cannot silently rewrite what a
// coverage value means.
const float FJELL_CLOUD_FIELD_LO = 0.18;
const float FJELL_CLOUD_FIELD_HI = 1.02;
const float FJELL_CLOUD_FIELD_MID = 0.60;

// ── Shape ───────────────────────────────────────────────────────────────────
// How the sheet is cut into clouds. The look does not come from posterising
// the field or inking its levels — both were tried and read as a contour map
// or as a photograph with the contrast pushed, because a flat value is the
// symptom and the silhouette is the cause. It comes from how the field is
// built: folded octaves for the creased, torn edge, and a multiplicative
// contrast term that drives most of the sky toward nothing and leaves the
// cloud that survives standing with a steep edge.
//
// These are globals a sky assigns in its sky() body before calling
// cloud_layer(), not #defines: this include is expanded into the template
// above the point where a material's uniforms exist, so a compile-time
// constant here could not be driven per-material and the dial would not be
// one. Assigning them from uniforms is what lets a .fjmat carry the look.
//
//     fjell_cloud_contrast = cloud_contrast;   // in sky()
//     vec4 cloud = cloud_layer(...);

// How hard the silhouette is cut. Higher clears more of the sky and leaves
// the cloud that survives with a steeper edge.
float fjell_cloud_contrast = 1.0;
// How much the folded, creased octaves show through. This is the wispiness:
// at 0 the shapes are round, at 1 they are torn and filamented.
float fjell_cloud_wisp = 1.0;
// How hard a cloud's outline is: how far the sky takes to become cloud. The
// silhouette itself does not move.
float fjell_cloud_edge = 1.0;

// Folded octaves: `abs()` creases the noise at every zero crossing, which is
// what puts the torn, filamented edges on a drawn cloud where plain fbm
// gives a rounded blob. Returns roughly 0..1.
float fjell_cloud_turbulence(vec2 p) {
    float sum = 0.0;
    float amplitude = 0.8;
    float total = 0.0;
    mat2 rot = mat2(1.6, 1.2, -1.2, 1.6);
    for (int i = 0; i < 5; ++i) {
        sum += abs(fjell_cloud_noise(p) * 2.0 - 1.0) * amplitude;
        total += amplitude;
        p = rot * p;
        amplitude *= 0.7;
    }
    return sum / max(total, 1e-4);
}

// The cloud field: roughly FIELD_LO..FIELD_HI, and the one place the sheet's
// shape is decided. The opacity, the colour's thickness and the shadow the
// sheet throws on the ground all read it, so a cloud is the same shape in
// all three rather than the three being rolled separately.
float fjell_cloud_field(vec2 sheet_pos) {
    float smooth_field = fjell_cloud_warped_fbm(sheet_pos)
                       + fjell_cloud_fbm(sheet_pos * 3.7 + 5.1) * 0.35;

    float turb = fjell_cloud_turbulence(sheet_pos * 1.7);
    // Centred on the field's own middle, so the multiply sharpens about the
    // value the coverage threshold is measured against rather than about zero.
    float base = smooth_field - FJELL_CLOUD_FIELD_MID;
    // Multiplicative, not additive: multiplying the field by the turbulence
    // plus its own magnitude is what empties most of the sky and leaves
    // isolated forms. An additive blend only ever produces another even wash.
    float drawn = base * (turb * mix(0.6, 1.9, fjell_cloud_wisp) + abs(base) * 2.0)
                * mix(1.0, 3.0, fjell_cloud_contrast);
    // Back into the range the cut expects. A field that drifts out of it
    // silently rewrites what every coverage value means — the sheet inverts,
    // and the sky fills in where it should be clearing.
    return clamp(FJELL_CLOUD_FIELD_MID + drawn,
                 FJELL_CLOUD_FIELD_LO, FJELL_CLOUD_FIELD_HI);
}

// Cloud amount 0..1 at a point of the sheet for the weather's coverage and
// density: coverage decides how much of the sheet has cloud, density how
// thick that cloud is.
//
// A covered sky is genuinely opaque, so this approaches 1 nearly everywhere
// at full coverage — and it should. What keeps an overcast sky from looking
// blank is not holes in it but the shading, which reads the field directly
// in cloud_layer() and so keeps its structure when the opacity has none left
// to give.
float fjell_cloud_amount(vec2 sheet_pos, float coverage, float density) {
    if (coverage <= 0.0) return 0.0;
    float f = fjell_cloud_field(sheet_pos);

    // The cut descends through the field as the sky fills, but stops short of
    // the floor: at full coverage it sits just above FIELD_LO, so the thinnest
    // scraps still ramp rather than clamping. Letting it reach the floor is
    // what turns the sky into one flat sheet.
    float cut = mix(FJELL_CLOUD_FIELD_HI, FJELL_CLOUD_FIELD_LO + 0.02, coverage);
    // How far the sky takes to become cloud. The field only spans
    // FIELD_HI - FIELD_LO, so a width of 0.3 would be a third of everything
    // there is and every silhouette would come out soft; a drawn cloud ends
    // where it ends.
    float width = mix(0.075, 0.055, coverage) * mix(1.0, 0.45, fjell_cloud_edge);
    float shape = clamp(smoothstep(cut, cut + width, f), 0.0, 1.0);

    return shape * mix(0.35, 1.0, density);
}

// The lit cloud for a view ray from `origin`: rgb its colour, a how much of
// the sky it covers along that ray. Zero below the horizon.
vec4 cloud_layer(vec3 origin, vec3 dir, float coverage, float density,
                 vec2 wind_drift,
                 vec2 front_dir, float front_edge, float front_softness,
                 vec3 sun_dir, vec3 sun_color, vec3 moon_color, vec3 ambient) {
    if (dir.y <= 0.0 || coverage <= 0.0) return vec4(0.0);
    // Where the ray crosses the sheet. The shell's far edge is a finite
    // distance away, so the sheet stops instead of foreshortening forever.
    float t = fjell_cloud_shell_distance(dir.y, origin.y, fjell_cloud_height);
    if (t <= 0.0) return vec4(0.0);
    vec3 hit = origin + dir * t;
    vec2 sheet = fjell_cloud_sheet_pos(hit.xz, wind_drift);

    // How much sheet falls inside one pixel, and so how much of the field is
    // worth evaluating here. Two things stretch that footprint: the distance
    // to the sheet, and the angle the ray meets it at — a grazing ray covers
    // ground far faster than a steep one, which is why the far sheet packs so
    // much field into so little screen.
    //
    // Dimming the cloud instead does not help: a half-transparent pixel that
    // flips between cloud and sky still flips. The detail has to go, not its
    // contrast.
    float footprint = t * FJELL_CLOUD_PIXEL_ANGLE / max(dir.y, 0.004);
    // Octaves that fit in fewer than two pixels are past Nyquist. log2 of how
    // many times the base feature can halve before reaching that limit is how
    // many are left worth having.
    float feature = FJELL_CLOUD_FEATURE_METRES;
    fjell_cloud_lod = clamp(log2(max(feature / max(footprint * 2.0, 1e-3), 1.0)),
                            0.0, 99.0);

    // What is left of the sheet once even the base feature is smaller than a
    // pixel. Past that point the budget has nothing further to drop — the LOD
    // is already at zero — and what remains is one unresolvable blob per
    // pixel, which is the last of the flicker. Keyed to the budget itself
    // rather than to an angle, so it starts exactly where detail runs out
    // instead of at a number that has to be rediscovered whenever the feature
    // size or the sheet's height changes.
    float distance_fade = 1.0 - smoothstep(0.0, 1.6, fjell_cloud_lod);

    float amount = fjell_cloud_amount(sheet, coverage, density)
                 * fjell_cloud_front_mask(dir, front_dir, front_edge, front_softness);
    // What is left of the sheet at the far edge: not zero, because cloud that
    // goes perfectly clear at distance reads as a wall of empty sky where the
    // horizon should be.
    amount *= mix(1.0, 0.55, distance_fade);
    if (amount <= 0.001) return vec4(0.0);

    // Lit from above by the sun and moon, forward scatter brightening the
    // side toward the sun, bottoms darker the thicker the cloud.
    //
    // Thickness comes from the *field*, not from `amount`. Once the sky is
    // overcast `amount` is one everywhere — it is opacity, and a covered sky
    // really is opaque — so a colour driven by it goes flat exactly when the
    // weather is most dramatic. The field keeps its structure at every
    // coverage, so shading with it is what leaves a storm sky churning
    // instead of blank.
    float field = fjell_cloud_field(sheet);
    // The field arrives banded when stylized, but a smoothstep across it
    // would blur those steps back out, so the ramp is re-cut on the way to
    // the colour. This is what puts the flat values in the paint rather than
    // only in the shape.
    // How deep into the cloud this pixel is, measured against the field's own
    // range so the two cannot drift apart. The window sits just below the
    // field's middle and runs a little past its top: low enough that cloud
    // shades rather than staying uniformly pale, high enough that most of a
    // lit cloud is still its bright value. Centred on the middle instead,
    // half of every cloud lands at the dark end and the sky reads as one grey
    // mass with holes in it; pushed much higher, cloud goes white and the
    // overcast case is a flat pale sheet again.
    //
    // The window follows the style because the two fields do not share a
    // distribution: the contrast multiply pushes the styled field's mass
    // upward, so a window that balances the soft sheet leaves the drawn one
    // uniformly dark, and one that balances the drawn sheet leaves the soft
    // one pale — which is the flat-white overcast this layer exists to avoid.
    float relief = smoothstep(FJELL_CLOUD_FIELD_MID - 0.15,
                              FJELL_CLOUD_FIELD_HI + 0.13, field);
    // Heavier weather is thicker throughout, so the whole sheet darkens as
    // it closes rather than only its densest parts.
    float thickness = mix(0.25, 1.0, relief) * mix(0.45, 1.0, density)
                    * mix(0.65, 1.0, coverage);

    float sun_up = max(sun_dir.y, 0.0);
    // A smooth falloff toward the sun is the most photoreal thing a sky has
    // left, the way a GGX lobe is in sea.fjsl. Drawn, it is one flat
    // brightening where the sun is and nothing outside it.
    float forward = smoothstep(0.55, 0.75, max(dot(dir, sun_dir), 0.0));

    // The lit top and the shaded base, as two colours the thickness picks
    // between. A thin scrap is bright and takes the sun's own hue, so it
    // warms through sunrise and sunset with the light; a thick mass settles
    // toward a cool slate that no amount of sun turns golden, which is why
    // a storm reads blue-grey while fair cloud reads warm white.
    vec3 sun_hue = sun_color / max(max(sun_color.r, max(sun_color.g, sun_color.b)), 1e-4);
    vec3 bright = mix(vec3(1.0), sun_hue, 0.55 * sun_up + 0.35 * forward);
    vec3 heavy = mix(vec3(0.34, 0.36, 0.42), vec3(0.20, 0.22, 0.29), coverage);

    vec3 body = mix(bright, heavy, thickness);
    vec3 lit = body * (ambient * (0.9 - 0.35 * thickness)
                     + sun_color * (0.40 * sun_up + 0.45 * forward) * (1.0 - 0.55 * thickness)
                     + moon_color * 3.0 * (1.0 - 0.5 * thickness));
    // Far cloud loses its contrast to the air in front of it, settling toward
    // the ambient the horizon sits in. Without this the distant sheet keeps
    // full contrast and reads as a hard band pasted along the skyline, which
    // is the other half of what a flat sheet got wrong there.
    vec3 haze = ambient * 0.9 + sun_color * 0.12 * max(sun_dir.y, 0.0);
    lit = mix(lit, haze, distance_fade * 0.7);

    return vec4(lit, clamp(amount, 0.0, 1.0));
}

#endif // FJELL_CLOUDS_2D_GLSL
