// fjell_api.h — Fjell Engine field reflection types
// Shared by the engine and game .so for inspector/serialization.

#ifndef FJELL_SCRIPTING_API_H
#define FJELL_SCRIPTING_API_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── API version ─────────────────────────────────────────────────────────
// Bump this when changing any SDK struct layout, vtable shape, or ABI contract.
// Engine checks this at .so load time and rejects mismatches.
#define FJELL_API_VERSION 3

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
    FJELL_FIELD_ENUM,
    FJELL_FIELD_WEAK_REF,
    FJELL_FIELD_REF,
    FJELL_FIELD_SHARED_REF
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
    const char*        ref_type_name; // REF/WEAK_REF/SHARED_REF: target type name (null otherwise)
} FjellFieldDesc;

// ── Key codes (match GLFW values) ────────────────────────────────────

#define FJELL_KEY_SPACE          32
#define FJELL_KEY_APOSTROPHE     39
#define FJELL_KEY_COMMA          44
#define FJELL_KEY_MINUS          45
#define FJELL_KEY_PERIOD         46
#define FJELL_KEY_SLASH          47
#define FJELL_KEY_0              48
#define FJELL_KEY_1              49
#define FJELL_KEY_2              50
#define FJELL_KEY_3              51
#define FJELL_KEY_4              52
#define FJELL_KEY_5              53
#define FJELL_KEY_6              54
#define FJELL_KEY_7              55
#define FJELL_KEY_8              56
#define FJELL_KEY_9              57
#define FJELL_KEY_A              65
#define FJELL_KEY_B              66
#define FJELL_KEY_C              67
#define FJELL_KEY_D              68
#define FJELL_KEY_E              69
#define FJELL_KEY_F              70
#define FJELL_KEY_G              71
#define FJELL_KEY_H              72
#define FJELL_KEY_I              73
#define FJELL_KEY_J              74
#define FJELL_KEY_K              75
#define FJELL_KEY_L              76
#define FJELL_KEY_M              77
#define FJELL_KEY_N              78
#define FJELL_KEY_O              79
#define FJELL_KEY_P              80
#define FJELL_KEY_Q              81
#define FJELL_KEY_R              82
#define FJELL_KEY_S              83
#define FJELL_KEY_T              84
#define FJELL_KEY_U              85
#define FJELL_KEY_V              86
#define FJELL_KEY_W              87
#define FJELL_KEY_X              88
#define FJELL_KEY_Y              89
#define FJELL_KEY_Z              90
#define FJELL_KEY_ESCAPE         256
#define FJELL_KEY_ENTER          257
#define FJELL_KEY_TAB            258
#define FJELL_KEY_BACKSPACE      259
#define FJELL_KEY_RIGHT          262
#define FJELL_KEY_LEFT           263
#define FJELL_KEY_DOWN           264
#define FJELL_KEY_UP             265
#define FJELL_KEY_LEFT_SHIFT     340
#define FJELL_KEY_LEFT_CONTROL   341
#define FJELL_KEY_LEFT_ALT       342
#define FJELL_KEY_RIGHT_SHIFT    344
#define FJELL_KEY_RIGHT_CONTROL  345
#define FJELL_KEY_RIGHT_ALT      346

#define FJELL_MOUSE_LEFT         0
#define FJELL_MOUSE_RIGHT        1
#define FJELL_MOUSE_MIDDLE       2

#ifdef __cplusplus
}
#endif

#endif // FJELL_SCRIPTING_API_H
