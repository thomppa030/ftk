#ifndef PARTICLE_COMMON_GLSL
#define PARTICLE_COMMON_GLSL

struct Particle {
    vec3  position;
    float lifetime;
    vec3  velocity;
    float max_lifetime;
    vec4  color;
    float size;
    float size_end;
    uint  emitter_index;
    float stretch;         // 0 = billboard, >0 = velocity-aligned streak (length multiplier)
    float rotation;        // radians
    float rotation_speed;  // radians/sec
    uint  texture_index;   // bindless texture index (0 = procedural)
    uint  flipbook_params; // packed: columns(8) | total_frames(8) | fps_fixed8(8) | mode(2)
};

struct EmitterData {
    vec3  emitter_position;
    float dt;
    vec3  gravity;
    uint  max_particles;
    vec4  color_start;
    vec4  color_end;
    float size_start;
    float size_end;
    float lifetime_min;
    float lifetime_max;
    vec3  velocity_min;
    uint  emit_count;
    vec3  velocity_max;
    uint  random_seed;
    uint  base_offset;
    uint  counter_offset;
    uint  dead_list_offset;
    uint  module_offset;
};

#endif
