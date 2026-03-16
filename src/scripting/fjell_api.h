// fjell_api.h — Fjell Engine Component API
// Pure C header for game script shared libraries.
// Include this in your .cpp component files.

#ifndef FJELL_API_H
#define FJELL_API_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── Field types for inspector / serialization reflection ────────────────

typedef enum FjellFieldType {
    FJELL_FIELD_FLOAT = 0,
    FJELL_FIELD_INT,
    FJELL_FIELD_BOOL,
    FJELL_FIELD_VEC2,
    FJELL_FIELD_VEC3,
    FJELL_FIELD_VEC4,
    FJELL_FIELD_COLOR3,
    FJELL_FIELD_COLOR4,
    FJELL_FIELD_STRING,
    FJELL_FIELD_ENUM
} FjellFieldType;

typedef struct FjellFieldDesc {
    const char*        name;
    FjellFieldType     type;
    uint32_t           offset;       // offsetof() from component data start
    uint32_t           size;         // STRING: buffer capacity; ENUM: label count
    const char* const* enum_labels;  // ENUM only (null otherwise)
    float              min;
    float              max;
    float              speed;        // drag speed (0 = default 0.1)
} FjellFieldDesc;

// ── JSON bridge (opaque handles, engine-side implementation) ────────────

typedef struct FjellJsonWriter FjellJsonWriter;
typedef struct FjellJsonReader FjellJsonReader;

extern void        fjell_json_write_float(FjellJsonWriter* w, const char* key, float v);
extern void        fjell_json_write_int(FjellJsonWriter* w, const char* key, int32_t v);
extern void        fjell_json_write_bool(FjellJsonWriter* w, const char* key, uint8_t v);
extern void        fjell_json_write_vec2(FjellJsonWriter* w, const char* key, const float* xy);
extern void        fjell_json_write_vec3(FjellJsonWriter* w, const char* key, const float* xyz);
extern void        fjell_json_write_vec4(FjellJsonWriter* w, const char* key, const float* xyzw);
extern void        fjell_json_write_string(FjellJsonWriter* w, const char* key, const char* v);

extern float       fjell_json_read_float(const FjellJsonReader* r, const char* key, float fb);
extern int32_t     fjell_json_read_int(const FjellJsonReader* r, const char* key, int32_t fb);
extern uint8_t     fjell_json_read_bool(const FjellJsonReader* r, const char* key, uint8_t fb);
extern void        fjell_json_read_vec2(const FjellJsonReader* r, const char* key,
                                        float* out, const float* fb);
extern void        fjell_json_read_vec3(const FjellJsonReader* r, const char* key,
                                        float* out, const float* fb);
extern void        fjell_json_read_vec4(const FjellJsonReader* r, const char* key,
                                        float* out, const float* fb);
extern const char* fjell_json_read_string(const FjellJsonReader* r, const char* key,
                                          const char* fb);

// ── Engine API (passed to component callbacks) ──────────────────────────

typedef struct FjellEngineApi {
    uint32_t object_id;

    // Transform
    void (*get_position)(uint32_t id, float* out_xyz);
    void (*set_position)(uint32_t id, const float* xyz);
    void (*get_rotation)(uint32_t id, float* out_xyz);
    void (*set_rotation)(uint32_t id, const float* xyz);
    void (*get_scale)(uint32_t id, float* out_xyz);
    void (*set_scale)(uint32_t id, const float* xyz);

    // Input
    uint8_t (*is_key_held)(int key);
    uint8_t (*is_key_pressed)(int key);
    uint8_t (*is_key_released)(int key);
    uint8_t (*is_mouse_button_held)(int button);
    uint8_t (*is_mouse_button_pressed)(int button);
    void    (*get_mouse_delta)(float* out_xy);

    // Time
    float delta_time;
    float elapsed_time;

    // Logging
    void (*log_info)(const char* msg);
    void (*log_warn)(const char* msg);
    void (*log_error)(const char* msg);

    // Physics
    void (*apply_force)(uint32_t id, const float* xyz);
    void (*apply_impulse)(uint32_t id, const float* xyz);
    void (*set_linear_velocity)(uint32_t id, const float* xyz);
    void (*get_linear_velocity)(uint32_t id, float* out_xyz);
    void (*set_angular_velocity)(uint32_t id, const float* xyz);
    void (*get_angular_velocity)(uint32_t id, float* out_xyz);
    uint8_t (*raycast)(const float* origin, const float* direction,
                       float max_distance, float* out_hit_pos,
                       float* out_hit_normal, float* out_distance,
                       uint32_t* out_object_id);
} FjellEngineApi;

// ── Component callback signatures ───────────────────────────────────────

typedef void (*FjellCreateFn)(void* data, const FjellEngineApi* api);
typedef void (*FjellDestroyFn)(void* data);
typedef void (*FjellUpdateFn)(void* data, const FjellEngineApi* api, float dt);
typedef void (*FjellInspectorFn)(void* data);
typedef void (*FjellSerializeFn)(const void* data, FjellJsonWriter* writer);
typedef void (*FjellDeserializeFn)(void* data, const FjellJsonReader* reader);

// ── Component type descriptor ───────────────────────────────────────────

typedef struct FjellComponentType {
    const char*           name;
    uint32_t              instance_size;
    uint32_t              version;

    FjellCreateFn         create;       // nullable
    FjellDestroyFn        destroy;      // nullable
    FjellUpdateFn         update;       // nullable
    FjellInspectorFn      inspector;    // nullable (null = auto from fields)
    FjellSerializeFn      serialize;    // nullable (null = auto from fields)
    FjellDeserializeFn    deserialize;  // nullable (null = auto from fields)

    const FjellFieldDesc* fields;
    uint32_t              field_count;
} FjellComponentType;

// ── Registration (game .so must export this) ────────────────────────────

typedef const FjellComponentType* const* (*FjellRegisterFn)(void);

// ── Helper macros (C++ only) ────────────────────────────────────────────

#ifdef __cplusplus

#define FJELL_FIELD(struct_type, member, field_type) \
    FjellFieldDesc { \
        #member, field_type, \
        static_cast<uint32_t>(offsetof(struct_type, member)), \
        static_cast<uint32_t>(sizeof(((struct_type*)nullptr)->member)), \
        nullptr, 0.0f, 0.0f, 0.0f \
    }

#define FJELL_FIELD_RANGE(struct_type, member, field_type, lo, hi, spd) \
    FjellFieldDesc { \
        #member, field_type, \
        static_cast<uint32_t>(offsetof(struct_type, member)), \
        static_cast<uint32_t>(sizeof(((struct_type*)nullptr)->member)), \
        nullptr, static_cast<float>(lo), static_cast<float>(hi), static_cast<float>(spd) \
    }

#define FJELL_EXPORT_COMPONENTS(...) \
    extern "C" const FjellComponentType* const* fjell_register_components(void) { \
        static const FjellComponentType* types[] = { __VA_ARGS__, nullptr }; \
        return types; \
    }

#endif

#ifdef __cplusplus
}
#endif

#endif // FJELL_API_H
