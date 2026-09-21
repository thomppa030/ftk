#ifndef FJELL_AIR_MASS_GLSL
#define FJELL_AIR_MASS_GLSL

// How much atmosphere a view ray looks through, relative to straight up.
//
// Anything that glows *through* the air rather than sitting on top of it
// wants this: a ray toward the horizon crosses far more haze than one toward
// the zenith, and every metre of it scatters. Without it a glow around the
// sun is a function of the angle to the sun alone, which draws a circle —
// and a real one is stretched and brightened downward, the lower half of a
// low sun's halo brighter than its upper.
//
// Kasten & Young (1989), "Revised optical air mass tables and approximation
// formula". The textbook 1/sin(elevation) divides by zero at the horizon and
// passes ten thousand just above it, which paints a white bar along the
// skyline; this levels off near 38, which is what the real atmosphere does
// once its curvature matters.

// Air mass looking straight up. Not exactly 1: the fit does not quite return
// one at the zenith, and normalising by the wrong constant tilts everything
// the whole sky over.
#define FJELL_ZENITH_AIR_MASS 0.999494

/// Relative air mass along `dir`, 1 at the zenith and about 36.5 at the
/// horizon. Below the horizon it holds at the horizon value rather than
/// growing without bound.
float fjell_air_mass(vec3 dir) {
    float elev_deg = degrees(asin(clamp(dir.y, -1.0, 1.0)));
    elev_deg = max(elev_deg, 0.0);
    return 1.0 / (sin(radians(elev_deg)) + 0.15 * pow(elev_deg + 3.885, -1.253));
}

// How far the true horizon sits below level, in degrees, seen from `height`
// metres up. The ground curves away, so from anywhere above it you see a
// little past level — a tenth of a degree from ten metres, a degree from a
// kilometre. Small, but it is the difference between a sun that sets when it
// touches level and one that sets when it touches the ground you can see.
//
// The same planet the cloud sheet is a shell around, spelled here because a
// sky shader reaches for this without the clouds.
#define FJELL_PLANET_RADIUS 6371000.0

float fjell_horizon_dip_deg(float height) {
    float h = max(height, 0.0);
    return degrees(acos(FJELL_PLANET_RADIUS / (FJELL_PLANET_RADIUS + h)));
}

/// How far `dir` sits above the true horizon, in degrees, from `height` metres
/// up. Negative below it. This is the elevation a body sets by: measured
/// against level alone, a body on a mountain would vanish while still plainly
/// in the sky.
float fjell_elevation_over_horizon(vec3 dir, float height) {
    return degrees(asin(clamp(dir.y, -1.0, 1.0))) + fjell_horizon_dip_deg(height);
}

/// How much of a celestial body survives at `dir`, seen from `height` metres
/// up: 1 well above the horizon, 0 below it, easing across a band `softness`
/// degrees wide either side.
///
/// A body does not switch off when it touches the horizon — it sinks. The
/// band is what makes it sink rather than blink, and it is also where the
/// ground would be cutting the disc if the sky drew ground.
float fjell_horizon_fade(vec3 dir, float height, float softness) {
    float elev = fjell_elevation_over_horizon(dir, height);
    return smoothstep(-softness, softness, elev);
}

/// The air mass along `dir` as a brightness scale, 1 at the zenith.
///
/// `strength` is how hard the horizon is favoured, as an exponent: the air
/// mass itself spans 1 to 38, and scaling a glow by that paints the horizon
/// white. A quarter leaves about two and a half times the zenith at the
/// skyline — an asymmetry the eye reads as depth rather than as a gradient
/// laid over the sky. 0 is an exact identity, so this is safe to apply
/// unconditionally and dial off.
float fjell_air_mass_gain(vec3 dir, float strength) {
    if (strength <= 0.0) return 1.0;
    return pow(fjell_air_mass(dir) / FJELL_ZENITH_AIR_MASS, strength);
}

#endif // FJELL_AIR_MASS_GLSL
