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
