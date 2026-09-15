#ifndef FJELL_UI_QUAD_FILL_GLSL
#define FJELL_UI_QUAD_FILL_GLSL

// Shared fill logic for ui_quad.frag and ui_quad_world.frag. One copy so the
// screen-space and worldspace quad shaders can't drift the way the two
// Delegate headers once did — see fjell_ui_common.glsl's own note on set 2.
//
// Caller must declare, before including this file:
//   set 1 binding 0: sampler2D textures[] (bindless)
//   set 1 binding 1: MaterialBuffer { GpuMaterial materials[]; }
//   set 1 binding 2: CustomParamBuffer { float custom_params[]; }
//   a GlobalUbo `ubo` (for ubo.time, the caustic animation clock)
// and must have included fjell_ui_common.glsl (UiQuad, quads[]) already.
//
// Every colour on a UiQuad is the stylesheet's sRGB value. A gradient is
// interpolated in that space, the way a stylesheet author expects, and the
// result is decoded to linear light before it meets a texture sample or the
// linear target. Coverage (border and edge anti-aliasing) blends linear.

#include "fjell_ui_quad_shape.glsl"

// Returns the fill color for this fragment, or discards (never returns) when
// the fragment is fully outside the clip rect or fully transparent.
vec4 fjell_ui_quad_fill(vec2 frag_uv, uint frag_quad_index) {
    UiQuad q = quads[frag_quad_index];

    fjell_ui_quad_clip(q, frag_uv);
    FjellUiShape shape = fjell_ui_quad_shape(q, frag_uv);

    float opacity = q.params.y;
    float flags = q.params.z;
    float gradient_angle = q.params.w;

    // Shadow mode (flags == 2): gaussian falloff from SDF distance
    if (flags > 1.5 && flags < 2.5) {
        float blur = max(q.shadow_params.x, 0.001);
        // Gaussian approximation: use smoothstep over blur radius
        float shadow_alpha = 1.0 - smoothstep(-blur, blur * 0.5, shape.dist);
        vec4 shadow_color = vec4(srgb_to_linear(q.color.rgb), q.color.a * shadow_alpha * opacity);
        if (shadow_color.a < 0.001) discard;
        return shadow_color;
    }

    // Determine fill color
    vec4 fill_color;

    if (flags > 3.5) {
        // Material mode (flags == 4): look up material, sample its base_color texture
        GpuMaterial mat = materials[q.material_id];
        uint tex_idx = mat.texture_indices[0]; // base_color slot
        vec4 tex_sample = texture(textures[nonuniformEXT(tex_idx)], frag_uv);
        vec4 base_color_factor = vec4(mat.base_params[0], mat.base_params[1],
                                       mat.base_params[2], mat.base_params[3]);
        // If base_color_factor is zero (unset), treat as white
        if (base_color_factor == vec4(0.0)) base_color_factor = vec4(1.0);
        fill_color = srgb_to_linear(q.color) * tex_sample * base_color_factor;

        // Animated caustic overlay from custom params.
        // param[0] = caustic_speed, param[1] = caustic_intensity, param[2] = caustic_scale
        float TIME = ubo.time;
        if (mat.param_count > 0) {
            float c_speed     = custom_params[mat.param_offset + 0];
            float c_intensity = mat.param_count > 1 ? custom_params[mat.param_offset + 1] : 0.3;
            float c_scale     = mat.param_count > 2 ? custom_params[mat.param_offset + 2] : 1.0;
            vec3 c_tint       = mat.param_count > 5 ? vec3(custom_params[mat.param_offset + 3],
                                                           custom_params[mat.param_offset + 4],
                                                           custom_params[mat.param_offset + 5])
                                                    : vec3(1.0, 0.9, 0.5);

            // Caustic in screen-space: bar is a window into a large caustic field
            vec2 pixel_pos_norm = (q.rect.xy + frag_uv * q.rect.zw) / q.viewport_size;
            vec2 c_uv   = pixel_pos_norm * c_scale * 4.0 + vec2(TIME * c_speed * 0.3, TIME * c_speed * 0.1);
            vec2 c_n    = vec2(0.0);
            vec2 c_sum  = vec2(0.0);
            float c_freq = 10.0;
            float c_time = TIME * c_speed;
            float c_cs   = cos(1.0);
            float c_sn   = sin(1.0);

            for (int i = 0; i < 8; i++) {
                c_uv = vec2(c_uv.x * c_cs - c_uv.y * c_sn,
                            c_uv.x * c_sn + c_uv.y * c_cs);
                c_n  = vec2(c_n.x * c_cs - c_n.y * c_sn,
                            c_n.x * c_sn + c_n.y * c_cs);
                vec2 cq = c_uv * c_freq + float(i) + c_n + c_time;
                c_n   += sin(cq);
                c_sum += cos(cq) / c_freq;
                c_freq *= 1.2;
            }

            // Soft caustic — no pow highlight, just the raw pattern
            float caustic = clamp(c_sum.x + c_sum.y + 0.5, 0.0, 1.0);
            fill_color.rgb += c_tint * caustic * c_intensity;
        }
    } else if (flags > 2.5) {
        // Raw texture mode (flags == 3): sample bindless texture, multiply with fill color
        vec4 tex_sample = texture(textures[nonuniformEXT(q.texture_id)], frag_uv);
        fill_color = srgb_to_linear(q.color) * tex_sample;
    } else if (flags > 0.5) {
        // Gradient mode: interpolate between color (from) and gradient_to (to)
        float angle_rad = radians(gradient_angle);
        vec2 dir = vec2(sin(angle_rad), cos(angle_rad));
        float t = dot(frag_uv - 0.5, dir) + 0.5;
        t = clamp(t, 0.0, 1.0);
        fill_color = srgb_to_linear(mix(q.color, q.gradient_to, t));
    } else {
        fill_color = srgb_to_linear(q.color);
    }

    return fjell_ui_quad_finish(fill_color, shape, q);
}

#endif // FJELL_UI_QUAD_FILL_GLSL
