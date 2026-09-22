#ifndef FJELL_WATER_WAKE_GLSL
#define FJELL_WATER_WAKE_GLSL

// Wakes: what moving things leave on the water.
//
// The other half of src/renderer/resources/water_wake.cpp, evaluated with the
// same formula so a body crossing another's wake rocks on the wave the player
// can see. Change anything here and change it there, in the same shape —
// test_water_wake.cpp compiles this file and compares the two.
//
// A source (a swimmer, a boat) lays a trail of stamps as it moves, and every
// stamp is the centre of a ring spreading outward at the deep-water speed of
// its wavelength. Rings from stamps laid along a path superpose into the V
// behind anything faster than they spread, and run ahead of anything slower:
// a wader pushes ripples before it, a boat drags a wake. One formula makes
// both, and the freshest ring, leaving the body's edge as it is laid, is the
// bow wave.
//
// The wake moves the surface vertically only. It cannot fold the surface,
// adds nothing to the Jacobian the crest mask is taken from, and has no
// direction to be undefined at a ring's centre. Foam from a wake rides its
// crests, and comes from fjell_water_wake_shade() below.
//
// The records are read in place, by index, through FJELL_WATER_WAKES. A
// record is 480 bytes; copied into a local, four times a vertex, it took the
// mesh shader past what it is given to work in and lost the device. Whoever
// includes this file says where the records are —
//
//     #define FJELL_WATER_WAKES wakes.sources
//
// — naming an expression that indexes to a FjellWakeSource.

#include "fjell_water_wake_record.glsl"

#define FJELL_WAKE_PI 3.14159265358979323846

#ifndef FJELL_WATER_WAKES
#error "define FJELL_WATER_WAKES to the wake records before including fjell_water_wake.glsl"
#endif

struct FjellWakeSurface {
    float height;
    vec2  slope;   // d(height)/dx, d(height)/dz
};

// One stamp's ring at a point: height, and d(height)/d(world_xz). Matches
// ring_at().
//
// The distance is measured from the source's radius out, sqrt(r² + ρ²), so
// the ring is smooth across its own centre instead of a cone, and the crest
// leaves the body's edge rather than its middle. The amplitude decays in time
// and spreads with the circumference, 1/sqrt(r) at the crest.
//
// The ring is one crest and nothing else: a raised cosine across a band of
// the crest's half-width, flat water outside it. A crest with troughs either
// side is what a real ring has, and two dozen of them along a path add and
// cancel into a wave tank; thin crests with nothing between them stay
// separate arcs, and their envelope is what reads as the V.
//
// Returned rather than accumulated through an inout, and without swizzles,
// so that the test's C++ compiler can read this file too.
FjellWakeSurface fjell_wake_ring(uint s, uint i, vec2 world_xz, float time) {
    FjellWakeSurface r;
    r.height = 0.0;
    r.slope = vec2(0.0);
    vec4 stamp = FJELL_WATER_WAKES[s].stamps[i];
    float age = time - stamp.z;
    // Laid after this instant: motion vectors ask about a frame ago.
    if (age <= 0.0) return r;
    vec4 ring = FJELL_WATER_WAKES[s].ring;
    float radius = FJELL_WATER_WAKES[s].body.x;

    vec2 d = world_xz - vec2(stamp.x, stamp.y);
    float reg = sqrt(dot(d, d) + radius * radius);
    float u = reg - ring.y * age;
    float half_width = ring.z;
    if (abs(u) >= half_width) return r;
    float amplitude = stamp.w * exp(-age / ring.w) * inversesqrt(1.0 + ring.y * age / radius);
    float phase = FJELL_WAKE_PI * u / half_width;   // -pi..pi across the band
    float c = cos(0.5 * phase);
    r.height = amplitude * c * c;
    // d/du of cos²(phase/2), then du/d(xz) = d / reg.
    float dF = -amplitude * (FJELL_WAKE_PI / (2.0 * half_width)) * sin(phase);
    r.slope = dF * (d / reg);
    return r;
}

// Whether a point is beyond where this source's wake can reach.
bool fjell_wake_outside(uint s, vec2 world_xz) {
    vec4 b = FJELL_WATER_WAKES[s].bounds;
    return world_xz.x < b.x || world_xz.y < b.y || world_xz.x > b.z || world_xz.y > b.w;
}

// The wake's share of the surface: every source in [first, first + count)
// that moves the mesh, summed. Matches wake_surface() with `cell` 0.
//
// `cell` is the metres between the vertices asking, and fades a source out on
// a grid too coarse to draw its crests: full at four vertices across a crest,
// gone at two. That is the mesh's business, not the surface's — the CPU
// passes 0 and sums every source, and a body far enough away to stand on a
// faded level cannot be seen floating inside a ripple.
FjellWakeSurface fjell_water_wake_surface(uint first, uint count, vec2 world_xz,
                                          float time, float cell) {
    FjellWakeSurface r;
    r.height = 0.0;
    r.slope = vec2(0.0);
    for (uint s = first; s < first + count; ++s) {
        if (FJELL_WATER_WAKES[s].counts.y == 0u) continue;
        if (fjell_wake_outside(s, world_xz)) continue;
        float weight = 1.0;
        if (cell > 0.0) {
            float half_width = FJELL_WATER_WAKES[s].ring.z;
            weight = clamp(2.0 - 2.0 * cell / half_width, 0.0, 1.0);
            if (weight == 0.0) continue;
        }
        float height = 0.0;
        vec2 slope = vec2(0.0);
        uint stamps = min(FJELL_WATER_WAKES[s].counts.x, uint(FJELL_MAX_WAKE_STAMPS));
        for (uint i = 0u; i < stamps; ++i) {
            FjellWakeSurface ring = fjell_wake_ring(s, i, world_xz, time);
            height += ring.height;
            slope += ring.slope;
        }
        r.height += weight * height;
        r.slope += weight * slope;
    }
    return r;
}

struct FjellWakeShade {
    // d(height)/d(xz) of the sources too small to move the mesh, for the
    // normal. Zero for sources the mesh already carries.
    vec2  slope;
    // 0..1, how much foam the wake has put here: the strongest ring crest
    // under this point, as the share of its birth height it still has. Foam
    // rides the crests and dies with them.
    float foam;
};

// What the fragment stage adds per pixel. `footprint` is the metres one pixel
// covers, and fades a crest out before it is narrower than four pixels,
// where it would shimmer rather than show; 0 fades nothing.
FjellWakeShade fjell_water_wake_shade(uint first, uint count, vec2 world_xz,
                                      float time, float footprint) {
    FjellWakeShade r;
    r.slope = vec2(0.0);
    r.foam = 0.0;
    for (uint s = first; s < first + count; ++s) {
        if (fjell_wake_outside(s, world_xz)) continue;
        float visible = 0.0;
        if (FJELL_WATER_WAKES[s].counts.y == 0u) {
            float half_width = FJELL_WATER_WAKES[s].ring.z;
            visible = 1.0 - smoothstep(half_width * 0.25, half_width * 0.5, footprint);
        }
        vec4 churn = FJELL_WATER_WAKES[s].churn;
        vec2 slope = vec2(0.0);
        uint stamps = min(FJELL_WATER_WAKES[s].counts.x, uint(FJELL_MAX_WAKE_STAMPS));
        for (uint i = 0u; i < stamps; ++i) {
            vec4 stamp = FJELL_WATER_WAKES[s].stamps[i];
            FjellWakeSurface ring = fjell_wake_ring(s, i, world_xz, time);
            // The crest's share of its birth height, which is the ring's own
            // decay and spreading, then the foam's own faster fade on top:
            // foam is gone before the ripple is. The strongest crest, not
            // the sum: crests cross, and a sum would whiten the crossing.
            float age = time - stamp.z;
            float crest = ring.height / max(stamp.w, 1e-6);
            float share = min(stamp.w * churn.x, 1.0);
            r.foam = max(r.foam, share * crest * exp(-age / churn.y));
            if (visible > 0.0) slope += ring.slope;
        }
        if (visible > 0.0) r.slope += visible * slope;
    }
    return r;
}

#endif // FJELL_WATER_WAKE_GLSL
