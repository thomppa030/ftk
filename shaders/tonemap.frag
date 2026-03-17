#version 450

layout(set = 0, binding = 0) uniform sampler2D hdr_tex;

layout(location = 0) out vec4 out_color;

// AgX tone mapping (adapted from Troy Sobotka / Blender)
// Reference: https://iolite-engine.com/blog_posts/minimal_agx_implementation

vec3 agx_default_contrast_approx(vec3 x) {
    vec3 x2 = x * x;
    vec3 x4 = x2 * x2;
    return + 15.5     * x4 * x2
           - 40.14    * x4 * x
           + 31.96    * x4
           - 6.868    * x2 * x
           + 0.4298   * x2
           + 0.1191   * x
           - 0.00232;
}

void main() {
    vec2 uv = gl_FragCoord.xy / vec2(textureSize(hdr_tex, 0));
    vec3 color = max(texture(hdr_tex, uv).rgb, vec3(0.0));

    // AgX input transform (log2 encoding)
    mat3 agx_mat = mat3(
        0.842479062253094,  0.0423282422610123, 0.0423756549057051,
        0.0784335999999992, 0.878468636469772,  0.0784336,
        0.0792237451477643, 0.0791661274605434, 0.879142973793104
    );

    const float min_ev = -12.47393;
    const float max_ev = 4.026069;

    color = agx_mat * color;
    color = clamp(log2(color), min_ev, max_ev);
    color = (color - min_ev) / (max_ev - min_ev);
    color = agx_default_contrast_approx(color);

    // AgX output transform
    mat3 agx_mat_inv = mat3(
         1.19687900512017,  -0.0528968517574562, -0.0529716355144438,
        -0.0980208811401368,  1.15190312990417,  -0.0980434066481128,
        -0.0990297440797205, -0.0989611768448433,  1.15107367264116
    );
    color = agx_mat_inv * color;

    // Output linear — sRGB format applies gamma
    color = clamp(color, 0.0, 1.0);

    out_color = vec4(color, 1.0);
}
