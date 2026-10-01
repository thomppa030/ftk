#version 450 core

// Dear ImGui fragment stage for an sRGB swapchain. ImGui authors every
// colour as sRGB — style, swatches, picker gradients, draw-list literals —
// while the attachment encodes whatever is written, so the vertex colour is
// decoded here and lands on screen as the colour that was picked. Textures
// already arrive linear: sRGB-format images decode on sample and the
// engine's render targets hold linear light, so they pass straight through.

#include "ftk_color_space.glsl"

layout(location = 0) out vec4 fColor;
layout(set = 0, binding = 0) uniform sampler2D sTexture;
layout(location = 0) in struct { vec4 Color; vec2 UV; } In;

void main() {
    fColor = srgb_to_linear(In.Color) * texture(sTexture, In.UV.st);
}
