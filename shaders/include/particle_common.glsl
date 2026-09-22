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

// One record per emitter slot, 96 bytes, rewritten every dispatch. Matches
// GpuEmitterData in particle_system.hpp field for field; std430 rounds the
// array stride up to the vec3s' 16-byte alignment, so the size is kept a
// multiple of 16 by the trailing word.
struct EmitterData {
    vec3  emitter_position;
    float dt;                 // this emitter's step: zero while paused or not the dispatching scene's
    vec3  emitter_velocity;
    uint  max_particles;
    uint  emit_count;
    uint  random_seed;
    uint  base_offset;
    uint  counter_offset;
    uint  module_offset;      // into the module buffer, in uints
    uint  flags;              // bit 0 trail, 1 lit, 2 mesh, 3 has sub-emitter, 4-5 its trigger
    float trail_width;
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
    uint  _pad;
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
