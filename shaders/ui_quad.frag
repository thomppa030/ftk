#version 460
#extension GL_EXT_nonuniform_qualifier : require

#include "fjell_ui_common.glsl"
#include "fjell_gpu_types.glsl"
#include "fjell_global_ubo.glsl"

layout(set = 1, binding = 0) uniform sampler2D textures[];

FJELL_DECLARE_GLOBAL_UBO(0, 0, ubo);

layout(set = 1, binding = 1) readonly buffer MaterialBuffer {
    GpuMaterial materials[];
};

layout(set = 1, binding = 2) readonly buffer CustomParamBuffer {
    float custom_params[];
};

layout(location = 0) in vec2 frag_uv;
layout(location = 1) flat in uint frag_quad_index;

layout(location = 0) out vec4 out_color;

float sdf_rounded_rect(vec2 p, vec2 half_size, vec4 radii) {
    float r;
    if (p.x < 0.0) {
        r = (p.y < 0.0) ? radii.x : radii.w;
    } else {
        r = (p.y < 0.0) ? radii.y : radii.z;
    }
    r = min(r, min(half_size.x, half_size.y));
    vec2 q_val = abs(p) - half_size + r;
    return min(max(q_val.x, q_val.y), 0.0) + length(max(q_val, 0.0)) - r;
}

void main() {
    UiQuad q = quads[frag_quad_index];

    // Clip test
    vec2 pixel_pos = q.rect.xy + frag_uv * q.rect.zw;
    if (pixel_pos.x < q.clip_rect.x || pixel_pos.x > q.clip_rect.x + q.clip_rect.z ||
        pixel_pos.y < q.clip_rect.y || pixel_pos.y > q.clip_rect.y + q.clip_rect.w) {
        discard;
    }

    vec2 half_size = q.rect.zw * 0.5;
    vec2 local_pos = (frag_uv - 0.5) * q.rect.zw;

    float border_width = q.params.x;
    float opacity = q.params.y;
    float flags = q.params.z;
    float gradient_angle = q.params.w;

    float dist = sdf_rounded_rect(local_pos, half_size, q.corners);
    float aa = fwidth(dist);
    float outer_alpha = 1.0 - smoothstep(-aa, aa, dist);

    // Shadow mode (flags == 2): gaussian falloff from SDF distance
    if (flags > 1.5 && flags < 2.5) {
        float blur = max(q.shadow_params.x, 0.001);
        // Gaussian approximation: use smoothstep over blur radius
        float shadow_alpha = 1.0 - smoothstep(-blur, blur * 0.5, dist);
        out_color = vec4(q.color.rgb, q.color.a * shadow_alpha * opacity);
        if (out_color.a < 0.001) discard;
        return;
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
        fill_color = q.color * tex_sample * base_color_factor;

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
        fill_color = q.color * tex_sample;
    } else if (flags > 0.5) {
        // Gradient mode: interpolate between color (from) and gradient_to (to)
        float angle_rad = radians(gradient_angle);
        vec2 dir = vec2(sin(angle_rad), cos(angle_rad));
        float t = dot(frag_uv - 0.5, dir) + 0.5;
        t = clamp(t, 0.0, 1.0);
        fill_color = mix(q.color, q.gradient_to, t);
    } else {
        fill_color = q.color;
    }

    // Border
    if (border_width > 0.0) {
        vec2 inner_half = half_size - border_width;
        float inner_dist = sdf_rounded_rect(local_pos, inner_half,
                                             max(q.corners - border_width, vec4(0.0)));
        float inner_alpha = 1.0 - smoothstep(-aa, aa, inner_dist);
        fill_color = mix(q.border_color, fill_color, inner_alpha);
    }

    fill_color.a *= outer_alpha * opacity;
    if (fill_color.a < 0.001) discard;

    out_color = fill_color;
}
