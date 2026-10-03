#version 460

// The UI canvas into the window: its texels are sRGB values, blended as such;
// the window's sRGB target encodes what it is given, so each is decoded here
// and comes out as it went in.

layout(set = 0, binding = 0) uniform sampler2D canvas;

layout(location = 0) out vec4 out_colour;

vec3 decode_srgb(vec3 encoded) {
    return mix(encoded / 12.92, pow((encoded + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), encoded));
}

void main() {
    const vec4 texel = texelFetch(canvas, ivec2(gl_FragCoord.xy), 0);
    out_colour = vec4(decode_srgb(texel.rgb), 1.0);
}
