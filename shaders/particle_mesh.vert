#version 460

#include "fjell_global_ubo.glsl"

FJELL_DECLARE_GLOBAL_UBO(0, 0, ubo);

#include "include/particle_common.glsl"

// Particle SSBO (same as billboard shader)
layout(std430, set = 1, binding = 0) readonly buffer ParticleBuffer {
    Particle particles[];
};

// Standard MegaBuffer vertex attributes
layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_color;
layout(location = 2) in vec2 in_tex_coord;
layout(location = 3) in vec3 in_normal;
layout(location = 4) in vec4 in_tangent;

layout(location = 0) out vec2 frag_uv;
layout(location = 1) out vec4 frag_color;
layout(location = 2) out flat uint frag_texture_index;
layout(location = 3) out vec3 frag_world_pos;
layout(location = 4) out vec3 frag_world_normal;
layout(location = 5) out flat uint frag_is_mesh;

void main() {
    // gl_InstanceIndex = base_offset + particle_local_index
    // (set via firstInstance in VkDrawIndexedIndirectCommand)
    Particle p = particles[gl_InstanceIndex];

    // Dead particle — move off-screen (behind far plane)
    if (p.lifetime <= 0.0) {
        gl_Position = vec4(0.0, 0.0, -2.0, 1.0); // behind clip volume
        frag_color = vec4(0.0);
        frag_uv = vec2(0.0);
        frag_texture_index = 0u;
        frag_world_pos = vec3(0.0);
        frag_world_normal = vec3(0.0, 1.0, 0.0);
        return;
    }

    // Build model matrix: position + Y-axis rotation + uniform scale
    float c = cos(p.rotation);
    float s = sin(p.rotation);
    float sz = p.size;

    mat4 model = mat4(
        vec4(c * sz, 0, s * sz, 0),
        vec4(0, sz, 0, 0),
        vec4(-s * sz, 0, c * sz, 0),
        vec4(p.position, 1.0)
    );

    vec4 world_pos = model * vec4(in_position, 1.0);
    frag_world_pos = world_pos.xyz;
    frag_world_normal = normalize(mat3(model) * in_normal);
    frag_uv = in_tex_coord;
    frag_color = p.color;
    frag_texture_index = p.texture_index;
    frag_is_mesh = 1u;

    gl_Position = ubo.projection * ubo.view * world_pos;
}
