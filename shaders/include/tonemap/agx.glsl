#ifndef FJELL_TONEMAP_AGX_GLSL
#define FJELL_TONEMAP_AGX_GLSL

// AgX tone mapping (Troy Sobotka / Blender / Filament)
// Reference: https://github.com/dmnsgn/glsl-tone-map/blob/main/agx.glsl

const mat3 FJ_AGX_LINEAR_SRGB_TO_LINEAR_REC2020 = mat3(
    0.6274, 0.0691, 0.0164,
    0.3293, 0.9195, 0.0880,
    0.0433, 0.0113, 0.8956
);

const mat3 FJ_AGX_LINEAR_REC2020_TO_LINEAR_SRGB = mat3(
    1.6605, -0.1246, -0.0182,
   -0.5876,  1.1329, -0.1006,
   -0.0728, -0.0083,  1.1187
);

const mat3 FJ_AGX_INSET = mat3(
    0.856627153315983,  0.137318972929847,  0.11189821299995,
    0.0951212405381588, 0.761241990602591,  0.0767994186031903,
    0.0482516061458583, 0.101439036467562,  0.811302368396859
);

const mat3 FJ_AGX_OUTSET = mat3(
     1.1271005818144368,  -0.1413297634984383,  -0.14132976349843826,
    -0.11060664309660323,  1.157823702216272,   -0.11060664309660294,
    -0.016493938717834573,-0.016493938717834257, 1.2519364065950405
);

const float FJ_AGX_MIN_EV = -12.47393;
const float FJ_AGX_MAX_EV =   4.026069;

vec3 fj_agx_contrast(vec3 x) {
    vec3 x2 = x * x;
    vec3 x4 = x2 * x2;
    return + 15.5   * x4 * x2
           - 40.14  * x4 * x
           + 31.96  * x4
           -  6.868 * x2 * x
           +  0.4298 * x2
           +  0.1191 * x
           -  0.00232;
}

vec3 tonemap_agx(vec3 color) {
    color = FJ_AGX_LINEAR_SRGB_TO_LINEAR_REC2020 * color;
    color = FJ_AGX_INSET * color;
    color = max(color, 1e-10);
    color = clamp(log2(color), FJ_AGX_MIN_EV, FJ_AGX_MAX_EV);
    color = (color - FJ_AGX_MIN_EV) / (FJ_AGX_MAX_EV - FJ_AGX_MIN_EV);
    color = clamp(color, 0.0, 1.0);
    color = fj_agx_contrast(color);
    color = FJ_AGX_OUTSET * color;
    // Outset works in gamma space, convert back to linear
    color = pow(max(vec3(0.0), color), vec3(2.2));
    color = FJ_AGX_LINEAR_REC2020_TO_LINEAR_SRGB * color;
    return clamp(color, 0.0, 1.0);
}

#endif
