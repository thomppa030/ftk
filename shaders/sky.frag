#version 460

#include "fjell_atmosphere.glsl"
#include "fjell_global_ubo.glsl"

FJELL_DECLARE_GLOBAL_UBO(0, 0, ubo);

layout(set = 0, binding = 6) uniform samplerCube sky_cubemap;

// Sky source selector (push.sky_mode):
//   0 = gradient (top_color / bottom_color)
//   1 = HDRI cubemap
//   2 = procedural atmospheric scattering
layout(push_constant) uniform PushConstants {
    vec4 top_color;
    vec4 bottom_color;
    float viewport_width;
    float viewport_height;
    float sky_mode;
    float atmosphere_rayleigh_scale;
    float atmosphere_mie_scale;
    float atmosphere_mie_g;
    float atmosphere_sun_intensity;
    float atmosphere_mie_coeff;
    float atmosphere_hdri_floor;
    float atmosphere_has_hdri;  // 1.0 if HDRI cubemap should modulate atmosphere, 0.0 for gradient
} push;

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec2 out_metallic_roughness;
layout(location = 2) out vec2 out_normal;

void main() {
    // Reconstruct world-space view direction from screen position.
    vec2 ndc = vec2(
        (gl_FragCoord.x / push.viewport_width) * 2.0 - 1.0,
        (gl_FragCoord.y / push.viewport_height) * 2.0 - 1.0
    );
    vec4 world_near = ubo.inv_view_proj * vec4(ndc, 0.0, 1.0);
    vec4 world_far  = ubo.inv_view_proj * vec4(ndc, 1.0, 1.0);
    vec3 view_dir = normalize(world_far.xyz / world_far.w - world_near.xyz / world_near.w);

    vec3 color;
    vec3 sun_dir = normalize(ubo.light_direction.xyz);
    float sun_dot = dot(view_dir, sun_dir);
    vec3 sun_color = ubo.light_color.rgb * ubo.light_direction.w;

    if (push.sky_mode > 1.5) {
        // Atmospheric scattering mode. The atmosphere is applied as a color
        // tint driven by sun direction; the backdrop it multiplies depends
        // on whether the scene has an HDRI (cubemap) or not (gradient).
        AtmosphereParams ap = make_default_atmosphere_params();
        ap.rayleigh_scale = push.atmosphere_rayleigh_scale;
        ap.mie_scale      = push.atmosphere_mie_scale;
        ap.mie_g          = push.atmosphere_mie_g;
        ap.mie_coeff      = push.atmosphere_mie_coeff;
        ap.sun_color      = ubo.light_color.rgb;
        ap.sun_intensity  = push.atmosphere_sun_intensity;
        vec3 tint = atmosphere(view_dir, sun_dir, ap);
        // Floor the tint so the backdrop remains at least partly visible
        // even when the atmospheric contribution drops to near zero.
        vec3 tint_floored = max(tint, vec3(push.atmosphere_hdri_floor));

        vec3 backdrop;
        if (push.atmosphere_has_hdri > 0.5) {
            backdrop = textureLod(sky_cubemap, view_dir, 0.0).rgb;
        } else {
            float t = gl_FragCoord.y / push.viewport_height;
            backdrop = mix(push.top_color.rgb, push.bottom_color.rgb, t);
        }
        color = backdrop * tint_floored;
    } else if (push.sky_mode > 0.5) {
        // Sample HDRI cubemap at lowest mip for sharp sky
        color = textureLod(sky_cubemap, view_dir, 0.0).rgb;
    } else {
        // Procedural gradient
        float t = gl_FragCoord.y / push.viewport_height;
        color = mix(push.top_color.rgb, push.bottom_color.rgb, t);
    }

    // Sun disc + glow are drawn on top for gradient/HDRI. For atmospheric
    // scattering the sun emerges naturally from the forward-scatter Mie
    // peak, so we only add a small crisp disc to give it a defined center.
    if (push.sky_mode > 1.5) {
        float disc = smoothstep(0.9995, 0.99995, sun_dot);
        color += sun_color * disc * 4.0;
    } else {
        float disc = smoothstep(0.9985, 0.99985, sun_dot);
        float disc_falloff = pow(disc, 2.0);
        float glow = smoothstep(0.985, 0.9985, sun_dot);
        glow = pow(glow, 3.0);
        color += sun_color * disc_falloff * 3.0;
        color += sun_color * glow * 0.5;
    }

    out_color = vec4(color, 1.0);
    out_metallic_roughness = vec2(0.0);
    out_normal = vec2(0.0);
}
