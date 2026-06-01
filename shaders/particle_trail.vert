#version 460

#include "fjell_global_ubo.glsl"

FJELL_DECLARE_GLOBAL_UBO(0, 0, ubo);

#include "include/particle_common.glsl"

layout(std430, set = 1, binding = 0) readonly buffer ParticleBuffer {
    Particle particles[];
};

layout(std430, set = 1, binding = 2) readonly buffer EmitterBuffer {
    EmitterData emitter_data[];
};

layout(std430, set = 1, binding = 3) readonly buffer Counters {
    uint counters[];  // [alive, dead, trail_head] per emitter (3 uints each)
};

layout(location = 0) out vec2 frag_uv;
layout(location = 1) out vec4 frag_color;
layout(location = 2) out flat uint frag_texture_index;
layout(location = 3) out vec3 frag_world_pos;
layout(location = 4) out vec3 frag_world_normal;
layout(location = 5) out flat uint frag_is_mesh;

void main() {
    uint base_offset = gl_InstanceIndex;
    uint strip_index = gl_VertexIndex / 2;
    uint side = gl_VertexIndex % 2;

    // Read a reference particle to get emitter_index
    Particle p_ref = particles[base_offset];
    if (p_ref.emitter_index >= 256u) {
        // Safety: invalid emitter index
        gl_Position = vec4(0.0);
        frag_color = vec4(0.0);
        frag_uv = vec2(0.0);
        frag_texture_index = 0u;
        return;
    }
    EmitterData em = emitter_data[p_ref.emitter_index];

    // Read trail_head from counter buffer (3 uints per emitter, index 2 = trail_head)
    uint trail_head = counters[em.counter_offset + 2];
    uint visible_count = min(trail_head, em.max_particles);

    if (strip_index >= visible_count) {
        gl_Position = vec4(0.0);
        frag_color = vec4(0.0);
        frag_uv = vec2(0.0);
        frag_texture_index = 0u;
        return;
    }

    // Read particles in ring order: oldest first
    uint oldest_slot = (trail_head >= em.max_particles)
        ? (trail_head % em.max_particles)
        : 0u;
    uint slot = (oldest_slot + strip_index) % em.max_particles;
    Particle p = particles[base_offset + slot];

    // Width from particle size field
    float w = p.size;

    // Get next particle for segment direction
    vec3 segment_dir;
    if (strip_index + 1 < visible_count) {
        uint next_slot = (oldest_slot + strip_index + 1) % em.max_particles;
        Particle p_next = particles[base_offset + next_slot];
        vec3 delta = p_next.position - p.position;
        float delta_len = length(delta);
        segment_dir = delta_len > 0.001 ? delta / delta_len : vec3(0, 1, 0);
    } else {
        segment_dir = length(p.velocity) > 0.001 ? normalize(p.velocity) : vec3(0, 1, 0);
    }

    // Camera-facing strip
    vec3 to_cam = normalize(ubo.camera_position.xyz - p.position);
    vec3 right = normalize(cross(segment_dir, to_cam));

    float offset = (side == 0u) ? -w : w;
    vec3 world_pos = p.position + right * offset;

    frag_world_pos = world_pos;
    frag_world_normal = normalize(ubo.camera_position.xyz - world_pos);
    frag_is_mesh = 0u;

    gl_Position = ubo.projection * ubo.view * vec4(world_pos, 1.0);

    float u = (visible_count > 1u) ? float(strip_index) / float(visible_count - 1u) : 0.0;
    frag_uv = vec2(u, float(side));
    frag_color = p.color;
    frag_texture_index = p.texture_index;
}
