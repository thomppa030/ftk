#version 460

// Wireframe overlay for the terrain patch grid. Deliberately flat and unlit:
// this is an editor aid for reading patch boundaries and tessellation density,
// so it has to stay legible against whatever the surface underneath is doing.

layout(location = 0) in vec3 frag_color;
layout(location = 1) in vec2 frag_tex_coord;
layout(location = 2) in vec3 frag_world_pos;
layout(location = 3) in vec3 frag_world_normal;
layout(location = 4) in vec4 frag_clip_pos;
layout(location = 5) in vec4 frag_prev_clip_pos;

// Only colour is written. The other G-buffer targets have their write masks
// cleared on this pipeline, because GTAO, SSR and TAA read them and lines
// drawn over the surface must not disturb what the fill pass established.
layout(location = 0) out vec4 out_color;

const vec3 WIRE_COLOR = vec3(0.1, 1.0, 0.4);

void main() {
    out_color = vec4(WIRE_COLOR, 1.0);
}
