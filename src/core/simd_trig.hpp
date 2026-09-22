#pragma once

#include <immintrin.h>

#include <cmath>

namespace fjell::simd {

/// Eight floats at once. What every machine this engine runs on has: AVX2 is
/// required by the physics library and is enabled for the whole build on both
/// compilers.
using f32x8 = __m256;

/// Sine and cosine of eight angles at once, to within a few float ulps of
/// the standard library for any angle a wave field produces.
///
/// A polynomial rather than the standard library for two reasons. The library
/// takes one angle at a time, so a sum over hundreds of waves at thousands of
/// points spends most of its time in it. And glibc and the MSVC runtime do not
/// round the last bit alike, so a Linux player and a Windows player floating
/// on the same sea would get surfaces a bit apart; a polynomial is our own
/// arithmetic on every platform and gives the same bits on all of them.
///
/// The angle is brought to within an eighth of a turn of a multiple of a
/// quarter turn, and sine and cosine of that remainder are each a short
/// series; the quadrant then says which of the two is which and with what
/// sign. The reduction subtracts the quarter turns in three pieces of pi/2
/// with fused multiply-adds, so an angle of thousands of radians — the sum of
/// a wave's k.x and omega.t grows with distance and time — loses to the
/// reduction only what its own last bit already lost, not a bit per turn.
inline void sincos(f32x8 angle, f32x8& sin_out, f32x8& cos_out) {
    // How many quarter turns, to the nearest.
    const f32x8 two_over_pi = _mm256_set1_ps(0.6366197723675814f);
    const f32x8 quarters = _mm256_round_ps(_mm256_mul_ps(angle, two_over_pi),
                                           _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);

    // pi/2 in three parts, each with enough trailing zero bits that its
    // product with a whole number of quarter turns is exact.
    const f32x8 c1 = _mm256_set1_ps(1.5703125f);
    const f32x8 c2 = _mm256_set1_ps(4.83751296997070312500e-4f);
    const f32x8 c3 = _mm256_set1_ps(7.54978995489188216e-8f);
    f32x8 x = _mm256_fnmadd_ps(quarters, c1, angle);
    x = _mm256_fnmadd_ps(quarters, c2, x);
    x = _mm256_fnmadd_ps(quarters, c3, x);
    const f32x8 x2 = _mm256_mul_ps(x, x);

    // Both series on |x| <= pi/4.
    f32x8 s = _mm256_set1_ps(-1.9515296e-4f);
    s = _mm256_fmadd_ps(s, x2, _mm256_set1_ps(8.3321608e-3f));
    s = _mm256_fmadd_ps(s, x2, _mm256_set1_ps(-1.6666654e-1f));
    s = _mm256_mul_ps(s, x2);
    s = _mm256_fmadd_ps(s, x, x);

    f32x8 c = _mm256_set1_ps(2.4433157e-5f);
    c = _mm256_fmadd_ps(c, x2, _mm256_set1_ps(-1.3887316e-3f));
    c = _mm256_fmadd_ps(c, x2, _mm256_set1_ps(4.1666646e-2f));
    c = _mm256_mul_ps(c, x2);
    c = _mm256_fnmadd_ps(_mm256_set1_ps(0.5f), x2, _mm256_fmadd_ps(c, x2, _mm256_set1_ps(1.0f)));

    // The quadrant, as an integer: odd quadrants swap sine and cosine, and
    // quadrants 2 and 3 flip sine's sign, 1 and 2 flip cosine's.
    const __m256i q = _mm256_cvtps_epi32(quarters);
    const __m256i one = _mm256_set1_epi32(1);
    const __m256i two = _mm256_set1_epi32(2);
    const f32x8 swap = _mm256_castsi256_ps(_mm256_cmpeq_epi32(_mm256_and_si256(q, one), one));
    const f32x8 sin_flip = _mm256_castsi256_ps(_mm256_cmpeq_epi32(_mm256_and_si256(q, two), two));
    const f32x8 cos_flip = _mm256_castsi256_ps(
        _mm256_cmpeq_epi32(_mm256_and_si256(_mm256_add_epi32(q, one), two), two));

    const f32x8 sign_bit = _mm256_set1_ps(-0.0f);
    f32x8 sn = _mm256_blendv_ps(s, c, swap);
    f32x8 cs = _mm256_blendv_ps(c, s, swap);
    sn = _mm256_xor_ps(sn, _mm256_and_ps(sin_flip, sign_bit));
    cs = _mm256_xor_ps(cs, _mm256_and_ps(cos_flip, sign_bit));
    sin_out = sn;
    cos_out = cs;
}

} // namespace fjell::simd
