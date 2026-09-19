#ifndef FJELL_WEATHER_SURFACE_GLSL
#define FJELL_WEATHER_SURFACE_GLSL

// What the weather does to a lit surface, applied after fragment() unless the
// shader declares `render_mode no_weather`: rain wets it, snow covers it. A
// surface takes weather in proportion to how open it is to the sky
// (sky_exposure, one where nothing shelters it) and how much it faces up, so
// walls get damp while floors get puddled and roofs whiten first.
//
// Wetting follows the usual porous-surface model: the albedo darkens, the
// roughness drops toward a water film, and while it rains the film ripples.
// Snow is a white, rough layer whose edge is broken by noise so it never
// reads as a flat cutoff.

// How smooth a water film leaves a surface it fully covers. Ground, not
// glass: wet earth and stone sit well short of a mirror, and a value low
// enough to reflect cleanly makes every surface without a roughness map show
// its base colour as specular.
const float WET_ROUGHNESS = 0.35;

// How many flat steps the wetting is cut into, and how hard the step is.
//
// A continuous darkening is a photograph of wet ground: every surface takes
// the same even wash and the only thing that varies is how much. Water does
// not sit like that — it runs off what is steep and pools in what is flat, so
// ground reads as a few distinct states rather than a gradient. Cutting the
// wetting into steps is the same move the clouds make, and for the same
// reason: flat values with edges between them read as painted, a smooth ramp
// reads as a render.
//
// 0 bands keeps the wash.
const float WET_BANDS = 3.0;
const float WET_BAND_HARDNESS = 0.85;

// A value cut into genuinely flat levels; hardness 1 is an exact cut, 0 an
// identity. Flattening toward each level's own centre rather than reshaping
// the fraction inside it, which is a contrast curve and never repeats a value.
float fjell_weather_band(float x, float bands, float hardness) {
    if (bands < 2.0) return x;
    float scaled = x * bands;
    float index = floor(scaled);
    float within = scaled - index;
    float keep = 1.0 - hardness;
    float shaped = keep <= 1e-4 ? step(0.5, within)
                                : clamp((within - 0.5) / keep + 0.5, 0.0, 1.0);
    shaped = shaped * shaped * (3.0 - 2.0 * shaped);
    return (index + mix(within, shaped, hardness)) / bands;
}

float fjell_weather_hash(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float fjell_weather_noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(fjell_weather_hash(i), fjell_weather_hash(i + vec2(1.0, 0.0)), u.x),
               mix(fjell_weather_hash(i + vec2(0.0, 1.0)), fjell_weather_hash(i + vec2(1.0, 1.0)), u.x), u.y);
}

// Rain rings on a water film: expanding circles that fade out, a few per
// square metre, as a normal tilt in the ground plane.
vec2 fjell_rain_ripples(vec2 xz, float time) {
    vec2 tilt = vec2(0.0);
    for (int i = 0; i < 2; ++i) {
        vec2 p = xz * 1.6 + float(i) * 37.1;
        vec2 cell = floor(p);
        vec2 local = fract(p) - 0.5;
        vec2 centre = vec2(fjell_weather_hash(cell + 0.31), fjell_weather_hash(cell + 0.71)) - 0.5;
        vec2 d = local - centre * 0.6;
        float r = length(d);
        float age = fract(time * 0.8 + fjell_weather_hash(cell + 0.13));
        float radius = age * 0.45;
        float ring = sin((r - radius) * 60.0) * exp(-abs(r - radius) * 30.0);
        float fade = (1.0 - age) * smoothstep(0.5, 0.1, r);
        tilt += normalize(d + 1e-4) * ring * fade;
    }
    return tilt;
}

void fjell_apply_weather(inout vec3 albedo, inout float roughness, inout float metallic,
                         inout vec3 normal, vec3 geometric_normal, vec3 world_pos,
                         float wetness, float snow, float precipitation,
                         float sky_exposure, float time) {
    float up = smoothstep(0.15, 0.6, geometric_normal.y);
    float exposed = clamp(sky_exposure, 0.0, 1.0);

    // ── Wet ──
    float wet = clamp(wetness, 0.0, 1.0) * exposed;
    float wet_here = wet * mix(0.35, 1.0, up);

    // Where the water gathers. Ground is not evenly wet: it runs off what is
    // steep and stands in what is flat and low, so the same rain leaves one
    // patch dark and its neighbour barely damp. Breaking the wetting up by
    // the ground's own shape is what stops it reading as a filter laid over
    // everything.
    float pool = fjell_weather_noise(world_pos.xz * 0.6)
               * 0.6 + fjell_weather_noise(world_pos.xz * 2.3) * 0.4;
    wet_here *= mix(0.55, 1.25, pool);
    // Then cut into flat steps, so a surface is dry, damp or wet rather than
    // somewhere on a ramp between them.
    wet_here = fjell_weather_band(clamp(wet_here, 0.0, 1.0), WET_BANDS, WET_BAND_HARDNESS);
    albedo *= mix(1.0, 0.55, wet_here * (1.0 - metallic));
    // Glossy only once really wet: a damp surface stays matte. Wet ground is
    // not a mirror — a water film smooths the microsurface but the ground
    // under it still scatters, and a target low enough to reflect cleanly
    // turns whatever is painted into the base colour into visible specular,
    // since a surface with no roughness map has nothing else to break the
    // highlight up.
    //
    // Water can only smooth a surface, never roughen it, so the film is a
    // floor and not a destination: a surface already glossier than the film
    // keeps its own roughness rather than being dulled by the rain.
    roughness = mix(roughness, min(roughness, WET_ROUGHNESS), wet_here * wet_here);
    if (precipitation > 0.0 && wet_here > 0.3) {
        vec2 tilt = fjell_rain_ripples(world_pos.xz, time) * precipitation * (wet_here - 0.3) * 0.5;
        normal = normalize(normal + vec3(tilt.x, 0.0, tilt.y));
    }

    // ── Snow ──
    float snow_here = clamp(snow, 0.0, 1.0) * exposed;
    if (snow_here > 0.0) {
        float n = fjell_weather_noise(world_pos.xz * 1.7 + world_pos.y * 0.4);
        float lie = up * (0.7 + 0.6 * n);
        float cover = smoothstep(1.0 - snow_here, min(1.0 - snow_here + 0.3, 1.0), lie);
        albedo = mix(albedo, vec3(0.92, 0.94, 0.97), cover);
        roughness = mix(roughness, 0.85, cover);
        metallic *= 1.0 - cover;
        normal = normalize(mix(normal, geometric_normal, cover * 0.6));
    }
}

#endif // FJELL_WEATHER_SURFACE_GLSL
