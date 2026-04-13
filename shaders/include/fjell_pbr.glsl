#ifndef FJELL_PBR_GLSL
#define FJELL_PBR_GLSL

// GGX/Trowbridge-Reitz normal distribution
float distribution_GGX(float NdotH, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * denom * denom);
}

// Schlick-GGX geometry function
float geometry_schlick_GGX(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

// Smith's method combining both view and light directions
float geometry_smith(float NdotV, float NdotL, float roughness) {
    return geometry_schlick_GGX(NdotV, roughness) *
           geometry_schlick_GGX(NdotL, roughness);
}

// Specular antialiasing: widen roughness based on normal curvature across the pixel.
// Prevents sub-pixel specular spikes from producing extreme HDR values.
// Combines Kaplanyan 2016 (normal filtering) with pixel footprint scaling
// so distant surfaces get wider roughness (sub-pixel highlight stabilization).
float specular_aa_roughness(vec3 N, vec3 world_pos, float roughness) {
    // Normal-based variance (catches normal map detail)
    vec3 dNdx = dFdx(N);
    vec3 dNdy = dFdy(N);
    float normal_variance = dot(dNdx, dNdx) + dot(dNdy, dNdy);

    // Pixel footprint: world-space pixel size increases with distance
    vec3 dPdx = dFdx(world_pos);
    vec3 dPdy = dFdy(world_pos);
    float pixel_size = sqrt(dot(dPdx, dPdx) + dot(dPdy, dPdy));

    // Scale pixel footprint contribution — tuned so a 1m pixel ≈ 0.1 roughness boost
    float footprint_variance = pixel_size * 0.1;

    float kernel_roughness = min(normal_variance * 2.0 + footprint_variance, 0.25);
    return clamp(sqrt(roughness * roughness + kernel_roughness), 0.0, 1.0);
}

// Fresnel-Schlick approximation
vec3 fresnel_schlick(float cos_theta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cos_theta, 0.0, 1.0), 5.0);
}

// Fresnel-Schlick with roughness for IBL ambient specular
vec3 fresnel_schlick_roughness(float cos_theta, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cos_theta, 0.0, 1.0), 5.0);
}

#endif
