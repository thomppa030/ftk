#version 450 core

// Dear ImGui fragment stage for an sRGB swapchain. ImGui authors every
// colour as sRGB — style, swatches, picker gradients, draw-list literals —
// while the attachment encodes whatever is written, so the vertex colour is
// decoded here and lands on screen as the colour that was picked. Textures
// already arrive linear: sRGB-format images decode on sample and the
// engine's render targets hold linear light, so they pass straight through.

layout(location = 0) out vec4 fColor;
layout(set = 0, binding = 0) uniform sampler2D sTexture;
layout(location = 0) in struct { vec4 Color; vec2 UV; } In;

vec3 srgb_to_linear(vec3 c) {
    return mix(c / 12.92,
               pow((c + 0.055) / 1.055, vec3(2.4)),
               greaterThan(c, vec3(0.04045)));
}

void main() {
    vec4 color = vec4(srgb_to_linear(In.Color.rgb), In.Color.a);
    fColor = color * texture(sTexture, In.UV.st);
}
