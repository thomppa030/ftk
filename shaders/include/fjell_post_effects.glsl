#ifndef FJELL_POST_EFFECTS_GLSL
#define FJELL_POST_EFFECTS_GLSL

#include "fjell_luma_weight.glsl"

// Effects a post shader calls from color(). Each reads around its pixel
// through the template's accessors, scene_at(), depth_at(), normal_at() and
// post_texel_size(), so a shader that calls none of them pays nothing for
// them. The ones that sample the scene return what COLOR is, the exposed
// scene before any tone curve; vignette() and film_grain() return a factor
// and an offset for whatever colour the shader holds.
//
// tests/test_post_effects.cpp compiles this file as C++ over a flat image and
// holds sharpen(), outline() and kuwahara() to giving it back unchanged, so it
// is written without swizzles, and a literal in vector arithmetic goes
// through a constructor.

// ── Sharpen ──────────────────────────────────────────────────────────────────

// Contrast Adaptive Sharpening (AMD FidelityFX CAS), in its cross-shaped form.
// Each pixel is pushed away from its four neighbours, by less where the
// neighbourhood already has contrast, so an edge comes back after TAA without
// ringing while flat ground and noise stay as they were. The samples are
// weighted by luma first, as TAA weighs them, so a highlight does not ring
// either. amount 0..1: 0 returns the scene untouched, 1 is CAS at full
// sharpness.
vec3 sharpen(vec2 uv, float amount) {
    vec2 t = post_texel_size();
    vec3 c = luma_weight(scene_at(uv));
    vec3 n = luma_weight(scene_at(uv + vec2(0.0, -t.y)));
    vec3 w = luma_weight(scene_at(uv + vec2(-t.x, 0.0)));
    vec3 e = luma_weight(scene_at(uv + vec2(t.x, 0.0)));
    vec3 s = luma_weight(scene_at(uv + vec2(0.0, t.y)));
    vec3 lo = min(c, min(min(n, w), min(e, s)));
    vec3 hi = max(c, max(max(n, w), max(e, s)));

    // Room left between the neighbourhood and the ends of the weighted range,
    // relative to its brightest: near 1 on flat ground, near 0 across an edge.
    vec3 room = sqrt(clamp(min(lo, vec3(1.0) - hi) / max(hi, vec3(1e-5)), vec3(0.0), vec3(1.0)));
    float a = clamp(amount, 0.0, 1.0);
    float peak = -a / mix(8.0, 5.0, a);
    vec3 k = room * peak;
    return luma_unweight(((n + w + e + s) * k + c) / (vec3(1.0) + k * vec3(4.0)));
}

// ── Outline ──────────────────────────────────────────────────────────────────

// 1 - cos of the angle between two normals, or 0 where the second is zero
// (the sky).
float post_crease(vec3 a, vec3 b) {
    return dot(b, b) > 0.25 ? 1.0 - dot(a, b) : 0.0;
}

// How strongly an ink line is drawn at uv, 0..1: where the depth jumps (an
// object's silhouette) or the surface turns (a crease).
//
// depth_edge is the jump at which a line is fully drawn, as a share of the
// distance, so near and far silhouettes read alike and far lines do not
// thicken: 0.1 draws where the far side is about a tenth farther. The test
// runs on inverse depth, which changes evenly across any flat surface on
// screen, so a floor seen at a low angle draws nothing; and only the nearer
// side of a jump draws, which keeps a silhouette one pixel wide.
//
// normal_edge is the turn at which a crease is fully drawn, as 1 - cos of the
// angle between neighbouring normals: 0.3 is about 45 degrees. The sky has no
// normal and draws no crease; where it meets the world is a depth jump.
//
// Either edge at 0 turns that half off.
float outline(vec2 uv, float depth_edge, float normal_edge) {
    vec2 t = post_texel_size();
    vec2 dx = vec2(t.x, 0.0);
    vec2 dy = vec2(0.0, t.y);
    float line = 0.0;

    if (depth_edge > 0.0) {
        float c = 1.0 / max(depth_at(uv), 1e-4);
        float l = 1.0 / max(depth_at(uv - dx), 1e-4);
        float r = 1.0 / max(depth_at(uv + dx), 1e-4);
        float u = 1.0 / max(depth_at(uv - dy), 1e-4);
        float d = 1.0 / max(depth_at(uv + dy), 1e-4);
        // How far this pixel stands in front of the line through its
        // neighbours on each axis, relative to its own inverse depth.
        float jump = (max(2.0 * c - l - r, 0.0) + max(2.0 * c - u - d, 0.0)) / c;
        line = smoothstep(0.5 * depth_edge, depth_edge, jump);
    }

    if (normal_edge > 0.0) {
        vec3 nc = normal_at(uv);
        if (dot(nc, nc) > 0.25) {
            float turn = max(max(post_crease(nc, normal_at(uv - dx)), post_crease(nc, normal_at(uv + dx))),
                             max(post_crease(nc, normal_at(uv - dy)), post_crease(nc, normal_at(uv + dy))));
            line = max(line, smoothstep(0.5 * normal_edge, normal_edge, turn));
        }
    }
    return line;
}

// ── Paint ────────────────────────────────────────────────────────────────────

// The Kuwahara filter: of the four square quarters around the pixel, each
// radius + 1 texels on a side, the mean colour of the one whose colours vary
// least. Flat areas fill with paint while edges stay where they were, which
// reads as brush strokes. The variation is measured on luma-weighted colours,
// so a highlight does not decide which quarter wins. It costs (2r + 1)²
// samples: 25 at radius 2, 49 at 3, which is the practical range; the radius
// is held to 8, and 0 returns the scene.
vec3 kuwahara(vec2 uv, int radius) {
    int r = min(max(radius, 0), 8);
    if (r == 0) return scene_at(uv);
    vec2 t = post_texel_size();

    // Per quarter, the sum of its colours and the sum and squared sum of
    // their weighted form. The quarters share the centre row and column.
    vec3 sum[4];
    vec3 wsum[4];
    vec3 wsq[4];
    for (int q = 0; q < 4; ++q) {
        sum[q] = vec3(0.0);
        wsum[q] = vec3(0.0);
        wsq[q] = vec3(0.0);
    }
    for (int j = -r; j <= r; ++j) {
        for (int i = -r; i <= r; ++i) {
            vec3 c = scene_at(uv + vec2(float(i) * t.x, float(j) * t.y));
            vec3 w = luma_weight(c);
            vec3 w2 = w * w;
            if (i <= 0 && j <= 0) { sum[0] += c; wsum[0] += w; wsq[0] += w2; }
            if (i >= 0 && j <= 0) { sum[1] += c; wsum[1] += w; wsq[1] += w2; }
            if (i <= 0 && j >= 0) { sum[2] += c; wsum[2] += w; wsq[2] += w2; }
            if (i >= 0 && j >= 0) { sum[3] += c; wsum[3] += w; wsq[3] += w2; }
        }
    }

    float count = float((r + 1) * (r + 1));
    vec3 best = sum[0] / count;
    float best_spread = 1e30;
    for (int q = 0; q < 4; ++q) {
        vec3 mean = wsum[q] / count;
        vec3 v = wsq[q] / count - mean * mean;
        float spread = v.r + v.g + v.b;
        if (spread < best_spread) {
            best_spread = spread;
            best = sum[q] / count;
        }
    }
    return best;
}

// ── Lens ─────────────────────────────────────────────────────────────────────

// The scene with red and blue pulled apart along the line from the centre, as
// a cheap lens does. amount is how far each moves at the edge of the image,
// in UV: 0.003 is subtle.
vec3 chromatic_aberration(vec2 uv, float amount) {
    float reach = 2.0 * amount;
    vec2 shift = (uv - vec2(0.5)) * reach;
    return vec3(scene_at(uv + shift).r, scene_at(uv).g, scene_at(uv - shift).b);
}

// A factor to multiply the colour by: 1 in the middle, falling to
// 1 - amount in the corners. softness is how far in from the corners the fall
// begins, as a share of the way to the centre: 1 begins at the centre, 0.3
// keeps it to the corners. Round on screen whatever the aspect.
float vignette(vec2 uv, float amount, float softness) {
    vec2 t = post_texel_size();
    vec2 aspect = vec2(t.y / t.x, 1.0);
    float corner = 0.5 * length(aspect);
    float reach = length((uv - vec2(0.5)) * aspect) / corner;
    return 1.0 - amount * smoothstep(1.0 - max(softness, 1e-3), 1.0, reach);
}

// An offset to add to a colour after the tone curve: grain of up to half of
// amount either way, new every frame. Interleaved gradient noise (Jimenez
// 2014) rather than white noise, so neighbouring pixels differ evenly and the
// grain does not crawl in clumps.
float film_grain(vec2 pixel, float time, float amount) {
    float frame = mod(floor(time * 60.0), 64.0);
    vec2 p = pixel + vec2(5.588238 * frame);
    float noise = fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
    return (noise - 0.5) * amount;
}

// ── Bands ────────────────────────────────────────────────────────────────────

// The colour cut into a few flat values by its brightness (its largest
// channel), hue kept: a picture's few decisions about light rather than a
// smooth ramp. The step between two bands keeps a narrow soft edge, as the
// sea's painted bands do, so it does not alias. For a display colour after
// the tone curve; steps is held to 2 or more.
vec3 posterize(vec3 c, float steps) {
    float count = max(steps, 2.0);
    float value = max(max(c.r, c.g), c.b);
    if (value <= 0.0) return c;
    float scaled = value * count;
    float band = floor(scaled);
    float banded = (band + smoothstep(0.35, 0.65, scaled - band)) / count;
    return c * (banded / value);
}

#endif // FJELL_POST_EFFECTS_GLSL
