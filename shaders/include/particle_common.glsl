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
    vec3  emitter_velocity;
    float _pad0;
    uint  flags;              // bit 0 trail, 1 lit, 2 mesh, 3 has sub-emitter, 4-5 its trigger
    uint  trail_head;         // ring buffer write index
    float trail_width;        // base width
    float trail_width_over_life; // 0=taper, 1=constant
    // Sub-emitter fields
    uint  sub_event_offset;   // offset into sub-emitter event buffer
    uint  sub_event_source;   // parent emitter index (0xFFFFFFFF = not a child)
    uint  sub_burst_count;    // particles per event (child only)
    float sub_vel_inherit;    // velocity inheritance factor (child only)
    // Mesh particle draw range (MegaBuffer), for GPU-side indirect command
    // construction — mesh entries never change per-emitter after resolve.
    uint  mesh_vertex_offset;
    uint  mesh_first_index;
    uint  mesh_index_count;
    // std430 rounds this array's per-element stride up to 16-byte alignment
    // (192 bytes) regardless of the struct's own packed size (188) — this
    // trailing word makes that explicit rather than relying on implicit
    // compiler padding. Matches GpuEmitterData's C++ layout exactly.
    uint  _pad1;
};

// Sub-emitter event (64 bytes, written by sim, consumed by emit)
struct SubEmitterEvent {
    vec3  position;       // 12
    float speed;          // 4
    vec3  velocity;       // 12
    float size;           // 4
    vec4  color;          // 16
    uint  parent_emitter; // 4
    uint  event_type;     // 0=death, 1=collision
    uint  _pad0;          // 4
    uint  _pad1;          // 4
};

#define MAX_SUB_EVENTS_PER_EMITTER 256

#endif
