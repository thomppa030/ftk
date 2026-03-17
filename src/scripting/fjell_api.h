// fjell_api.h — Fjell Engine field reflection types
// Shared by the engine and game .so for inspector/serialization.

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
    uint32_t           offset;       // offsetof() from data start
    uint32_t           size;         // STRING: buffer capacity; ENUM: label count
    const char* const* enum_labels;  // ENUM only (null otherwise)
    float              min;
    float              max;
    float              speed;        // drag speed (0 = default 0.1)
} FjellFieldDesc;

#ifdef __cplusplus
}
#endif

#endif // FJELL_API_H
