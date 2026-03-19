#version 460

struct UiQuad {
    vec4 rect;          // x, y, w, h (pixels)
    vec4 color;         // background RGBA (or gradient start)
    vec4 border_color;  // border RGBA
    vec4 corners;       // border-radius (tl, tr, br, bl)
    vec4 params;        // border_width, opacity, flags, gradient_angle
    vec4 clip_rect;     // overflow:hidden scissor
    vec4 gradient_to;   // gradient end color RGBA
    vec2 viewport_size; // for px->NDC
    vec2 shadow_params; // blur_radius, spread
};

layout(std430, set = 2, binding = 0) readonly buffer UiQuadBuffer {
    UiQuad quads[];
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
    if (flags > 1.5) {
        float blur = max(q.shadow_params.x, 0.001);
        // Gaussian approximation: use smoothstep over blur radius
        float shadow_alpha = 1.0 - smoothstep(-blur, blur * 0.5, dist);
        out_color = vec4(q.color.rgb, q.color.a * shadow_alpha * opacity);
        if (out_color.a < 0.001) discard;
        return;
    }

    // Determine fill color
    vec4 fill_color;
    if (flags > 0.5) {
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
