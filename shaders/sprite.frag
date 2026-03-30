#version 460
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 1, binding = 0) uniform sampler2D textures[];

layout(location = 0) in vec2 frag_uv;
layout(location = 1) in flat uint frag_tex_index;
layout(location = 2) in float frag_opacity;
layout(location = 3) in vec4 frag_tint;

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec2 out_metallic_roughness;

void main() {
    vec4 tex_color = texture(textures[nonuniformEXT(frag_tex_index)], frag_uv);
    if (tex_color.a < 0.01) discard;
    out_color = tex_color * frag_tint;
    out_color.a *= frag_opacity;
    out_metallic_roughness = vec2(0.0);
}
